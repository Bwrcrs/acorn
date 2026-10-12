#include "table.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "error.hpp"

Table::Table(Pager& pager, TableSchema schema, uint32_t firstPage)
    : pager_(&pager), schema_(std::move(schema)), firstPage_(firstPage) {}

namespace {

// 列数和类型校验。insert 和 updateRow 共用这一份。
//
// 【为什么必须共用】
// 这是"表里每一行都符合表结构"这条不变式的全部内容。
// 抄成两份的话，早晚会有一份忘了改——比如以后加 NOT NULL 约束，
// 只改了 insert 那份，于是 UPDATE 就能把 NULL 塞进非空列。
// 不变式的实现只能有一处，这是和"lowercase 只能有一个实现"
// 同一个道理，只是后果更严重。
void checkRow(Row& row, const TableSchema& schema) {
    // ---- 先查列数 ----
    // 列数对不上时，报错要带上具体数字。
    // 只说"列数不对"，用户得自己数建表语句里有几列、
    // VALUES 里有几个值——那是一定会数错的。
    if (row.size() != schema.columns.size()) {
        throw AcornError("表 `" + schema.name + "` 有 " +
                         std::to_string(schema.columns.size()) +
                         " 列，但给了 " + std::to_string(row.size()) +
                         " 个值");
    }

    // ---- 再逐列查类型 ----
    // 注意这里是**边查边改**：fitToType 可能把整数 3 换成浮点数 3.0
    //（列是 REAL 时）。所以必须把结果写回 row[i]，
    // 不能只检查不接收——那样检查通过了，存进去的还是那个整数。
    for (size_t i = 0; i < row.size(); i++) {
        row[i] = fitToType(row[i], schema.columns[i].type,
                           schema.columns[i].name);
    }
}

}  // namespace

// ------------------------------------------------------------
//  找链尾
// ------------------------------------------------------------

uint32_t Table::lastPage() const {
    if (lastPage_ != INVALID_PAGE) return lastPage_;

    // 沿着链走到最后一个数据页。
    //
    // 【为什么要走一遍，不能直接算出来】
    // 因为页在文件里是散着的（见 table.hpp 开头那张图）。
    // "第 n 页的数据在哪"没有公式，只能从第一个页开始跟着 next 走。
    // 阶段 4 的 B+ 树会给"想找某个值在哪"修一条近路，
    // 但"最后一个页在哪"这件事，到最后也还是得另外记一个指针——
    // 这是链表这个结构本身的性质，赖不掉。
    //
    // 走一遍的结果会缓存在 lastPage_ 里，所以一张表一辈子只走这一次
    //（除非它被清空重建）。
    uint32_t pageNo = firstPage_;
    while (pageNo != INVALID_PAGE) {
        lastPage_ = pageNo;
        Pager::Handle page = pager_->get(pageNo);
        const uint32_t next = pageNext(page.data());
        if (next == INVALID_PAGE) break;
        pageNo = next;
    }
    return lastPage_;
}

// ------------------------------------------------------------
//  插入
// ------------------------------------------------------------

RowId Table::placeAnywhere(const std::string& cell) {
    // 先在最常见的去处试一下：最后一个数据页。
    //
    // 【为什么"只试最后一个"是对的，以及它的代价】
    // 对一个只往里塞数据的表（最常见的情况），最后一个页就是
    // 唯一可能还有空位的页——前面的页早就满了。
    // 所以"试最后一个"不是偷懒，它命中率极高。
    //
    // 代价是：**删过行的页里的空洞不会被回填**。
    // 比如说插 100 行（铺了 3 页），删掉中间 50 行，
    // 那 3 页里各有一半是墓碑；接着再插 50 行，
    // 会往第 4、5 页写，而不是回到前 3 页的空位里。
    // 文件因此比"理论上需要的大小"大。
    //
    // 要不要修？真数据库会修（SQLite 的 freelist、Postgres 的 FSM）。
    // 但那需要**另外记一份"哪些页还有多少空间"**——又是一份
    // 可能过期的信息。阶段 3 先不做，因为它不影响正确性，
    // 只影响文件大小；而多一份可能过期的元数据是实打实的风险。
    // 页内的空洞是回收的（heapCompact），这一条不能省——
    // 不然"删一行再插一行"会把页撑爆。
    const uint32_t candidate = lastPage();

    {
        // 【getForWrite 而不是 get】因为下面可能真的会改这一页。
        // 代价是"试了一下发现装不下"时也白记了一页日志——
        // 但那最多每插入一行多 4KB 的日志，而且要等到这一页
        // 第一次被写才记（同一页只记一次），换掉的是
        // "改了一页却没记日志"这种**回滚时才知道**的错。
        Pager::Handle page = pager_->getForWrite(candidate);
        uint16_t slot = 0;
        if (heapInsert(page.data(), cell.data(), cell.size(), slot)) {
            page.markDirty();
            return RowId{candidate, slot};
        }
    }

    // 最后一页也满了（或者这一行根本放不下）。
    if (cell.size() > MAX_CELL_SIZE) {
        throw AcornError(
            "这一行有 " + std::to_string(cell.size()) +
            " 字节，超过了一页能装下的上限（" +
            std::to_string(MAX_CELL_SIZE) +
            " 字节）。一页就是 " + std::to_string(PAGE_SIZE) +
            " 字节，行不能跨页存。把这一行的文本列改短一些");
    }

    // 开一个新页，挂到链尾。
    Pager::Handle fresh = pager_->allocatePage();
    initHeapPage(fresh.data(), INVALID_PAGE);

    // 把老链尾的 next 指向新页。
    //
    // 【两页要同时拿着】老链尾得改"我的下一个是谁"，新页得写内容。
    // 这就是缓存必须支持同时借出多页的原因——一次只能拿一页的话，
    // 这里就得"拿A改A放下、再拿B改B"，中间那一步如果A被踢出缓存
    // 并且写回了，接着又改B……听着也行，但只要有一个环节忘了
    // 标脏，改动就静悄悄地丢了。
    // 同时拿着，改完一起放手，中间没有缝。
    const uint32_t newPageNo = fresh.pageNo();
    {
        Pager::Handle tail = pager_->getForWrite(candidate);
        setPageNext(tail.data(), newPageNo);
        tail.markDirty();
    }

    uint16_t slot = 0;
    if (!heapInsert(fresh.data(), cell.data(), cell.size(), slot)) {
        // 全新的一页都放不下，说明上面那个长度检查漏了什么。
        // 真走到这儿就是内部错误了。
        throw AcornError("内部错误：一行 " + std::to_string(cell.size()) +
                         " 字节，连一个空页都装不下");
    }
    fresh.markDirty();

    lastPage_ = newPageNo;  // 缓存里更新一下，下次插入直接就找它
    return RowId{newPageNo, slot};
}

RowId Table::insert(Row& row) {
    checkRow(row, schema_);

    // 编码要在校验**之后**。反过来的话，`INSERT INTO t VALUES ('abc')`
    // 插进整数列会先被编成字节，再在校验里抛异常——
    // 那串字节哪儿也没去，倒也无害，但顺序摆正了读起来更清楚：
    // 先确认这一行是对的，再把它变成字节。
    const std::string cell = encodeRow(row);
    return placeAnywhere(cell);
}

// ------------------------------------------------------------
//  扫描
// ------------------------------------------------------------

void Table::scan(const std::function<void(RowId, const Row&)>& visit) const {
    uint32_t pageNo = firstPage_;

    while (pageNo != INVALID_PAGE) {
        // 【一次只借一页】Handle 在这一轮结束时就还回去了。
        // 要是把整条链的 Handle 都攒在一个 vector 里，一张表就能
        // 把 64 格的缓存全占满，然后 pickVictim 找不到可淘汰的页，
        // 直接抛"缓存全被占住了"。这不是理论上的担心——
        // 一张表超过 64 页就会发生。
        Pager::Handle page = pager_->get(pageNo);

        const uint16_t slotCount = heapSlotCount(page.data());
        for (uint16_t slot = 0; slot < slotCount; slot++) {
            size_t length = 0;
            const char* cell = heapCell(page.data(), slot, length);
            if (cell == nullptr) continue;  // 墓碑：这一行删过了

            // decodeRow 解出来的 Row 是个临时对象，它活到 visit
            // 返回为止——正好够用，而且调用方拿不到一个会过期的引用。
            // 这也是 scan 的回调签名收 `const Row&` 却安全的原因。
            visit(RowId{pageNo, slot}, decodeRow(cell, length));
        }

        pageNo = pageNext(page.data());
        // 这里 page 析构，这一页还回缓存。下一轮再借下一页。
    }
}

// ------------------------------------------------------------
//  按行号读一行（回表）
// ------------------------------------------------------------

Row Table::row(RowId id) const {
    if (id.pageNo == INVALID_PAGE || id.pageNo >= pager_->pageCount()) {
        throw AcornError("索引里的一个行号指向" + pageName(id.pageNo) +
                         "，但文件里没有这一页。索引和表对不上了");
    }

    Pager::Handle page = pager_->get(id.pageNo);
    size_t length = 0;
    const char* cell = heapCell(page.data(), id.slot, length);
    if (cell == nullptr) {
        // 三种可能：槽位越界、槽位是墓碑（那一行删了）、
        // 行的字节已经不在页内了。三种都说明索引里存着一个
        // 指向空气的行号——这是索引维护漏了一处的表现。
        throw AcornError(
            "索引里的一个行号指向" + pageName(id.pageNo) + " 的第 " +
            std::to_string(id.slot) +
            " 个槽位，但那里没有行（那一行删过了，或者根本没占过这个槽位）。"
            "索引和表对不上了");
    }
    return decodeRow(cell, length);
}

// ------------------------------------------------------------
//  更新
// ------------------------------------------------------------

RowId Table::updateRow(RowId id, Row& row) {
    // 【先校验，再编码，最后才动页】
    // 顺序反了的话，`UPDATE t SET id = 'abc'` 会先把页改坏，
    // 再在 fitToType 里抛异常——那一行就永久脏了。
    checkRow(row, schema_);
    const std::string cell = encodeRow(row);

    // ---- 第一步：尽量在原来那一页里解决 ----
    {
        Pager::Handle page = pager_->getForWrite(id.pageNo);
        if (heapUpdate(page.data(), id.slot, cell.data(), cell.size())) {
            page.markDirty();
            return id;  // 原地改的，槽位没动，行号还是它
        }
    }

    // ---- 第二步：这一页实在放不下，搬到别的页去 ----
    //
    // 走到这里，老位置已经还原成原样了（heapUpdate 保证的），
    // 所以现在动它不会丢数据。
    //
    // ★ 这一步会让传进来的 RowId 失效 ★
    // 老槽位变成墓碑，行搬到了新页的新槽位。**调用方必须用返回值**，
    // 拿着老行号继续用的话，它会指向一个空位。
    // （阶段 4 之前这里是 `(void)moved;`——那时候调用方改完就
    //   不再看这些行号，靠约定撑着。现在索引里也存着行号，
    //   所以改成返回出来，让调用方绕不过去。）
    const RowId moved = placeAnywhere(cell);

    Pager::Handle oldPage = pager_->getForWrite(id.pageNo);
    heapDelete(oldPage.data(), id.slot);
    oldPage.markDirty();

    return moved;
}

// ------------------------------------------------------------
//  删除
// ------------------------------------------------------------

void Table::deleteRows(const std::vector<RowId>& ids) {
    // 【阶段 2 那条"必须升序"的要求没了】
    // 阶段 2 是从后往前 erase，非要升序不可——那是 vector 的实现
    // 细节漏到了接口上。现在删一行只是把它的槽位长度改成 0，
    // 删第 3 行和第 5 行之间没有任何相互影响，顺序随便。
    //
    // 所以这里**故意不加**升序检查：留一条已经没有理由的约束，
    // 比没有约束更糟——后来的人会以为它有什么深意，然后照着编规矩。
    for (const RowId& id : ids) {
        Pager::Handle page = pager_->getForWrite(id.pageNo);
        heapDelete(page.data(), id.slot);
        page.markDirty();
    }
}

// ------------------------------------------------------------
//  释放整张表
// ------------------------------------------------------------

void Table::releaseAllPages() {
    // ---- 先收集，再释放 ----
    // 【为什么不能边走边释放】释放一页会把它的页头改写成
    // "下一个空闲页是谁"，而链表的 next 就在页头第 4 个字节。
    // 等于一边沿路走一边把路标擦掉，走两步就迷路了。
    //
    // （这和 DELETE 不能边扫边删是**不同**的原因：
    //   那边是迭代器失效（语言层面的规则），
    //   这边是数据本身被改掉（逻辑层面的）。区分这两者有用——
    //   前者只能靠"先收集"，后者其实可以靠"先把 next 读出来再改"。）
    std::vector<uint32_t> pages;
    uint32_t pageNo = firstPage_;
    while (pageNo != INVALID_PAGE) {
        pages.push_back(pageNo);
        Pager::Handle page = pager_->get(pageNo);
        pageNo = pageNext(page.data());
    }

    for (uint32_t target : pages) {
        pager_->freePage(target);
    }

    firstPage_ = INVALID_PAGE;
    lastPage_ = INVALID_PAGE;
}

#include "btree.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "error.hpp"

// ============================================================
//  条目的字节格式
//
//  叶子：  [键的编码][行所在页 4][行槽位 2]              长度 = 键长 + 6
//  内部：  [键的编码][行所在页 4][行槽位 2][子页号 4]     长度 = 键长 + 10
//  内部第 0 个：[0x05][子页号 4]                        长度 = 5
//
//  【键的编码为什么直接借 page.hpp 的 encodeValue】
//  因为索引里的键和行里的值是**同一个值**，必须用同一套字节表示。
//  各写一份的话，哪天编码改了（比如浮点换个写法），索引里的键
//  就再也匹配不上行里的值——而且**不报错**，只是查不到。
//  见 page.hpp 里 encodeValue 那段。
//
//  【叶子条目和"键部分"的关系】
//  叶子条目**就是**键部分（键 + 行号）——叶子本来就要存行号。
//  内部条目 = 键部分 + 4 字节子页号。所以"把叶子条目变成内部条目"
//  就是在尾巴上接 4 个字节，两处共用一个函数。
// ============================================================

namespace {

void appendU16(std::string& out, uint16_t value) {
    char buffer[2];
    writeU16(buffer, value);
    out.append(buffer, 2);
}

void appendU32(std::string& out, uint32_t value) {
    char buffer[4];
    writeU32(buffer, value);
    out.append(buffer, 4);
}

// 键部分的字节：键的编码 + 行号。叶子条目和内部条目的前半截都是它。
std::string makeKeyBytes(const Value& key, const RowId& id) {
    std::string out = encodeValue(key);
    appendU32(out, id.pageNo);
    appendU16(out, id.slot);
    return out;
}

std::string makeLeafEntry(const Value& key, const RowId& id) {
    return makeKeyBytes(key, id);  // 叶子条目 == 键部分，见上面的说明
}

// 把一个"键部分"补成一个内部条目。
std::string makeInternalEntry(const std::string& keyBytes, uint32_t child) {
    std::string out = keyBytes;
    appendU32(out, child);
    return out;
}

// 内部节点第 0 个条目：没有键，只有一个孩子。
std::string makeNoKeyEntry(uint32_t child) {
    std::string out;
    out.push_back(static_cast<char>(BTREE_NO_KEY));
    appendU32(out, child);
    return out;
}

// 从条目末尾取出子页号。内部条目才有——调之前先确认类型。
uint32_t childOf(const std::string& entry) {
    return readU32(entry.data() + entry.size() - 4);
}

// 解出一个条目的"键部分"。
//
// hasKeyOut = false 表示这是内部节点的第 0 个条目（没有键）。
// keyBytesOut 是键部分占的字节数——有了它，调用方才知道后面
// 那 4 个字节是不是子页号。
//
// 【为什么先看第一个字节，而不是先试着 decodeValue】
// 因为 0x05 不是合法的值标签，decodeValue 见到它会直接报
// "文件坏了"。那是**对**的行为（这个标签不该出现在别处），
// 但在这里它是个正常情况，所以得先认出来再决定。
void decodeKeyBytes(const char* data, size_t length, bool& hasKeyOut,
                    Value& keyOut, RowId& rowOut, size_t& keyBytesOut) {
    if (length >= 1 &&
        static_cast<unsigned char>(data[0]) == BTREE_NO_KEY) {
        hasKeyOut = false;
        keyOut = Value::makeNull();
        rowOut = RowId{};
        keyBytesOut = 1;
        return;
    }

    size_t used = 0;
    keyOut = decodeValue(data, length, used);
    if (used + 6 > length) {
        throw AcornError("数据文件坏了：索引的一个条目里放不下行号");
    }
    rowOut.pageNo = readU32(data + used);
    rowOut.slot = readU16(data + used + 4);
    keyBytesOut = used + 6;
    hasKeyOut = true;
}

// 条目里的键和 (key, id) 比大小。没有键的条目（内部节点第 0 个）
// 当成负无穷——它本来就是"比所有键都小"的意思。
//
// 【为什么要连行号一起比】见 btree.hpp 里"键其实是两截"那段。
// 只比值的话，一列上有一百个 5 的时候，找"哪个 5"就无从谈起。
int compareEntry(const char* data, size_t length, const Value& key,
                 const RowId& id) {
    bool hasKey = false;
    Value entryKey;
    RowId entryRow;
    size_t keyBytes = 0;
    decodeKeyBytes(data, length, hasKey, entryKey, entryRow, keyBytes);

    if (!hasKey) return -1;  // 负无穷

    const int byValue = compareForSort(entryKey, key);
    if (byValue != 0) return byValue;

    if (entryRow.pageNo != id.pageNo) {
        return entryRow.pageNo < id.pageNo ? -1 : 1;
    }
    if (entryRow.slot != id.slot) {
        return entryRow.slot < id.slot ? -1 : 1;
    }
    return 0;
}

// 这么多个条目装得进一页吗。
//
// 【算式必须和 heapFreeBytes 一致】那边是
//     cellTop - (12 + 槽位数 * 4)
// 而 cellTop 从 PAGE_SIZE 往下走，每放一个 cell 减它的长度。
// 所以"全部装得下"就是 sum(长度 + 4) <= PAGE_SIZE - 12。
// 两处算式要是各写各的，就会出现"我说装得下、heapInsertAt 说
// 装不下"，然后 rebuild 里那个 throw 就开始乱喷。
bool fitsInPage(const std::vector<std::string>& entries) {
    size_t needed = HEAP_HEADER_SIZE;
    for (const std::string& entry : entries) {
        needed += entry.size() + SLOT_SIZE;
    }
    return needed <= PAGE_SIZE;
}

}  // namespace

Btree::Btree(Pager& pager, uint32_t rootPage, Value::Type keyType)
    : pager_(&pager), rootPage_(rootPage), keyType_(keyType) {}

// ------------------------------------------------------------
//  页内的查找
// ------------------------------------------------------------

uint16_t Btree::insertPosition(const char* page, const Value& key,
                               const RowId& id) const {
    const uint16_t count = heapSlotCount(page);
    uint16_t live = 0;  // 到目前数到几个活着的条目
    for (uint16_t slot = 0; slot < count; slot++) {
        size_t length = 0;
        const char* cell = heapCell(page, slot, length);
        if (cell == nullptr) continue;  // 墓碑：不占位置
        if (compareEntry(cell, length, key, id) >= 0) return live;
        ++live;
    }
    return live;  // 活着的都比它小，那就该追加到末尾
}

uint16_t Btree::chooseChild(const char* page, const Value& key,
                            const RowId& id) const {
    // 找"第一个比 (key,id) 大的条目"，它的**前一个**就是该走的孩子。
    // 也就是"最后一个分隔键 <= (key,id) 的条目"。规则的理由见
    // btree.hpp 里"删除为什么不合并节点"那段。
    const uint16_t count = heapSlotCount(page);
    uint16_t slot = 0;
    for (; slot < count; slot++) {
        size_t length = 0;
        const char* cell = heapCell(page, slot, length);
        if (cell == nullptr) continue;
        if (compareEntry(cell, length, key, id) > 0) break;
    }

    if (slot == 0) {
        // 第 0 个条目没有键，比什么都小，所以循环至少会走到 slot = 1。
        // 停在 0 说明这一页的第 0 个条目带着键——那不是内部节点该有的样子。
        throw AcornError("数据文件坏了：索引的内部节点第一个条目带着键");
    }
    if (count == 0) {
        throw AcornError("数据文件坏了：索引的内部节点一个条目都没有");
    }
    return static_cast<uint16_t>(slot - 1);
}

std::vector<std::string> Btree::readEntries(const char* page) const {
    std::vector<std::string> entries;
    const uint16_t count = heapSlotCount(page);
    entries.reserve(count);
    for (uint16_t slot = 0; slot < count; slot++) {
        size_t length = 0;
        const char* cell = heapCell(page, slot, length);
        if (cell == nullptr) continue;  // 墓碑（只在叶子里会出现）
        entries.emplace_back(cell, length);
    }
    return entries;
}

// ------------------------------------------------------------
//  下行
// ------------------------------------------------------------

uint32_t Btree::descendToLeaf(const Value& key, const RowId& id) const {
    uint32_t pageNo = rootPage_;
    for (int depth = 0; depth <= MAX_DEPTH; depth++) {
        Pager::Handle handle = pager_->get(pageNo);
        const PageType type = pageType(handle.data());
        if (type == PageType::BTREE_LEAF) return pageNo;
        if (type != PageType::BTREE_INTERNAL) {
            throw AcornError("数据文件坏了：" + pageName(pageNo) +
                             "本来应该是索引的节点，实际不是");
        }
        const uint16_t index = chooseChild(handle.data(), key, id);
        size_t length = 0;
        const char* cell = heapCell(handle.data(), index, length);
        if (cell == nullptr || length < 4) {
            throw AcornError("数据文件坏了：索引的内部节点里有个空条目");
        }
        pageNo = readU32(cell + length - 4);
    }
    throw AcornError("索引的层数超过了 " + std::to_string(MAX_DEPTH) +
                     " 层。这个文件多半被改坏了");
}

uint32_t Btree::leftmostLeaf() const {
    uint32_t pageNo = rootPage_;
    for (int depth = 0; depth <= MAX_DEPTH; depth++) {
        Pager::Handle handle = pager_->get(pageNo);
        const PageType type = pageType(handle.data());
        if (type == PageType::BTREE_LEAF) return pageNo;
        if (type != PageType::BTREE_INTERNAL) {
            throw AcornError("数据文件坏了：" + pageName(pageNo) +
                             "本来应该是索引的节点，实际不是");
        }
        // 一直走第 0 个孩子。见 chooseChild 里那段——第 0 个条目
        // 是"比所有键都小"的那一支，所以它就是最左边。
        if (heapSlotCount(handle.data()) == 0) {
            throw AcornError("数据文件坏了：索引的内部节点一个条目都没有");
        }
        size_t length = 0;
        const char* cell = heapCell(handle.data(), 0, length);
        if (cell == nullptr || length < 4) {
            throw AcornError("数据文件坏了：索引的内部节点第 0 个条目是空的");
        }
        pageNo = readU32(cell + length - 4);
    }
    throw AcornError("索引的层数超过了 " + std::to_string(MAX_DEPTH) +
                     " 层。这个文件多半被改坏了");
}

// ------------------------------------------------------------
//  重建和分裂
// ------------------------------------------------------------

void Btree::rebuild(char* page, PageType type, uint32_t next,
                    const std::vector<std::string>& entries) const {
    // 整页重来：页头清干净，然后一条一条按顺序插进去。
    //
    // 【为什么敢整页重来】因为这棵树里**没有人记得某个条目的位置**。
    // 外面只认页号，进了页之后一切重新找。
    // （对比 heapCompact 那段：表里的槽位下标是 RowId 的一部分，
    //   动不得，所以那边只能"先把活着的都拷出来再摆回去"，
    //   而这里连"摆回原来的下标"都不用管。）
    initBtreePage(page, type, next);

    for (const std::string& entry : entries) {
        uint16_t slot = 0;
        if (!heapInsertAt(page, heapSlotCount(page), entry.data(),
                          entry.size(), slot)) {
            // 调用方在分裂之前已经算过装得下。真走到这儿说明
            // fitsInPage 和 heapInsertAt 的算式对不上了——那是代码的错，
            // 不是数据的错，所以用"内部错误"。
            throw AcornError(
                "内部错误：重建 B+ 树节点时放不下了（" +
                std::to_string(entries.size()) + " 个条目）");
        }
    }
}

Btree::Split Btree::splitNode(uint32_t pageNo, PageType type,
                              const std::vector<std::string>& entries,
                              uint32_t next) {
    // ---- 一、挑从哪切开 ----
    //
    // 目标有两个，第二个才是重点：
    //   1. 两半都装得下（这是硬条件）
    //   2. 两半的字节数尽量平均
    //
    // 第 2 条不是"顺手优化一下"：B+ 树的均摊代价靠的就是
    // "节点分裂成两个半满的"，两边一起慢慢填满。
    // 要是每次都切个 9:1，那个 1 那边一会儿又满了，
    // 又要分裂——树会往一边歪，长高得比该有的快。
    std::vector<size_t> prefix(entries.size() + 1, 0);
    for (size_t i = 0; i < entries.size(); i++) {
        prefix[i + 1] = prefix[i] + entries[i].size() + SLOT_SIZE;
    }

    const size_t total = prefix.back();
    size_t bestCut = 0;
    size_t bestDiff = 0;
    bool found = false;

    // cut 是"上半个从第几个条目开始"。两边都得至少留一个条目，
    // 所以 cut 从 1 到 entries.size()-1。
    for (size_t cut = 1; cut < entries.size(); cut++) {
        const size_t leftBytes = prefix[cut];
        const size_t rightBytes = total - prefix[cut];
        if (leftBytes > PAGE_SIZE - HEAP_HEADER_SIZE) continue;
        if (rightBytes > PAGE_SIZE - HEAP_HEADER_SIZE) continue;

        const size_t diff =
            leftBytes > rightBytes ? leftBytes - rightBytes : rightBytes - leftBytes;
        if (!found || diff < bestDiff) {
            found = true;
            bestCut = cut;
            bestDiff = diff;
        }
    }

    if (!found) {
        // 走不到这里：能进这个函数说明"这一页装不下"了，
        // 而装不下的前一页是装得下的，所以总长度不超过两页。
        // 不过既然是算出来的结论，卡一道比相信它便宜。
        throw AcornError("内部错误：索引节点分裂时找不到一个两半都装得下的切点");
    }

    std::vector<std::string> lower(entries.begin(),
                                   entries.begin() + static_cast<long>(bestCut));
    std::vector<std::string> upper(entries.begin() + static_cast<long>(bestCut),
                                   entries.end());

    // ---- 二、算分隔键 ----
    //
    // 【叶子和内部节点在这里**不一样**，这是 B+ 树最容易搞混的一处】
    //
    //  叶子：「复制上去」。上半个的第一个条目**原样留在叶子里**，
    //        同时拷一份到父节点当路标。所以叶子里的键在树里出现两次。
    //        为什么能拷：叶子里装的是数据，父节点那份只是个路标，
    //        两份不需要是"同一个东西"。
    //
    //  内部：「推上去」。上半个的第一个条目要把**键交给父节点**，
    //        自己只留下孩子。因为内部条目的键是"这一支的下界"，
    //        现在这一支被劈成两半了，那个下界变成"新那一半的下界"，
    //        而老这一半的下界由更早的条目管着。
    //
    //  两种做法都保证同一件事：**父节点那个路标 = 新子树里最小的键**。
    //  这是路标规则成立的前提。
    std::string separator;
    if (type == PageType::BTREE_LEAF) {
        separator = upper.front();
    } else {
        const std::string& first = upper.front();
        separator = first.substr(0, first.size() - 4);
        // 新节点的第 0 个条目：只留孩子，不留键。
        upper.front() = makeNoKeyEntry(childOf(first));
    }

    // ---- 三、写两个页 ----
    //
    // 【先写新页，再改老页的指针】顺序反过来的话，中间那一瞬间
    // 老页指着新页、新页里还是空的——现在没有崩溃恢复，这个顺序
    // 不影响正确性，但阶段 5 加日志的时候，"先建好再挂上去"
    // 会省掉一堆麻烦。现在就这么写，到时候不用改。
    const uint32_t newPageNo = [&]() {
        Pager::Handle fresh = pager_->allocatePage();
        // 叶子的 next 是一条链（范围扫描顺着它走），所以新叶子
        // 接在老叶子后面：新叶子的 next = 老叶子的 next。
        // 内部节点不串链，next 永远是"没有"。
        rebuild(fresh.data(), type,
                type == PageType::BTREE_LEAF ? next : INVALID_PAGE, upper);
        fresh.markDirty();
        return fresh.pageNo();
    }();

    {
        Pager::Handle oldPage = pager_->getForWrite(pageNo);
        rebuild(oldPage.data(), type,
                type == PageType::BTREE_LEAF ? newPageNo : next, lower);
        oldPage.markDirty();
    }

    Split result;
    result.happened = true;
    result.newPage = newPageNo;
    result.sepKey = std::move(separator);
    return result;
}

// ------------------------------------------------------------
//  插入
// ------------------------------------------------------------

Btree::Split Btree::insertInto(uint32_t pageNo, const std::string& entry,
                               const Value& key, const RowId& id, int depth) {
    if (depth > MAX_DEPTH) {
        throw AcornError("索引的层数超过了 " + std::to_string(MAX_DEPTH) +
                         " 层。这个文件多半被改坏了");
    }

    // 【这个 handle 会一直握到函数结束，包括递归进去的那段时间】
    // 递归里取别的页可能触发淘汰，但这一页被 pin 着，淘汰挑不到它。
    // 一层握一个，树高 3~4 层就是 3~4 个 pin——远不到 64 的上限。
    //
    // 【为什么是 getForWrite】因为下面两种情况下这一页都会被改：
    // 插得下（重建这一页）和孩子分裂了（把孩子的新分隔键记进来）。
    // 所以这一页**有可能会改**，那就得按"要改"来拿。
    //
    // 代价：一次只往下走的插入（没分裂）会把路上每一页都记一遍日志，
    // 哪怕最后一页都没改。日志大小因此和**树高**成正比而不是
    // 和"改了几页"成正比——树高是个位数，这个偏差可以接受。
    // 反过来的写法（先 get、真要改了再补记日志）做不到：
    // 到那时候这一页已经被 rebuild 过了，"原来的样子"已经没了。
    Pager::Handle handle = pager_->getForWrite(pageNo);
    const PageType type = pageType(handle.data());
    if (type != PageType::BTREE_LEAF && type != PageType::BTREE_INTERNAL) {
        throw AcornError("数据文件坏了：" + pageName(pageNo) +
                         "本来应该是索引的节点，实际不是");
    }

    // 先把整页的条目读成拷贝。后面不管递归到哪一页去，
    // 手上这一份都不会失效（见 readEntries 的说明）。
    std::vector<std::string> entries = readEntries(handle.data());
    const uint32_t next = pageNext(handle.data());

    if (type == PageType::BTREE_LEAF) {
        // ---- 插到"第一个不比它小的条目"前面 ----
        // 注意比的是 (键, 行号)：键相同的那些按行号排队。
        // 这个顺序是确定的——不确定的话，同一个查询两次跑出来
        // 的行序可能不一样，测试根本没法写。
        //
        // insertPosition 返回的**就是** entries 里的下标（它也跳过墓碑），
        // 所以这里直接拿来用，不用再换算。
        const uint16_t at = insertPosition(handle.data(), key, id);
        entries.insert(entries.begin() + static_cast<long>(at), entry);

        if (fitsInPage(entries)) {
            rebuild(handle.data(), type, next, entries);
            handle.markDirty();
            return Split{};
        }
        return splitNode(pageNo, type, entries, next);
    }

    // ---- 内部节点：先决定去哪个孩子 ----
    const uint16_t index = chooseChild(handle.data(), key, id);
    size_t childLength = 0;
    const char* childCell = heapCell(handle.data(), index, childLength);
    if (childCell == nullptr || childLength < 4) {
        throw AcornError("数据文件坏了：索引的内部节点里有个空条目");
    }
    const uint32_t child = readU32(childCell + childLength - 4);

    const Split childSplit = insertInto(child, entry, key, id, depth + 1);
    if (!childSplit.happened) return Split{};

    // ---- 孩子分裂了：把新的分隔键插在孩子右边 ----
    entries.insert(entries.begin() + static_cast<long>(index) + 1,
                   makeInternalEntry(childSplit.sepKey, childSplit.newPage));

    if (fitsInPage(entries)) {
        rebuild(handle.data(), type, next, entries);
        handle.markDirty();
        return Split{};
    }
    return splitNode(pageNo, type, entries, next);
}

void Btree::insert(const Value& key, RowId id) {
    // 顺手检查一下键的类型。这一步**不该**触发——Database 在把值
    // 塞进来之前已经按列类型调过 fitToType 了。写在这里是因为它
    // 一旦真的触发，说明有一条插索引的路径漏掉了类型转换；
    // 那种情况下如果放行，索引里的键会和行里的值长得不一样，
    // 表现是"索引明明有，就是查不到"——**不报错**的那种坏。
    //
    // 整数插进浮点索引是允许的：比较的时候两者会统一成 double，
    // 而且 Database 那边其实已经转成浮点了，这里只是多留一道口子。
    if (!key.isNull() && key.type != keyType_ &&
        !(keyType_ == Value::Type::REAL && key.type == Value::Type::INTEGER)) {
        throw AcornError("内部错误：往" + std::string(sqlTypeName(keyType_)) +
                         "索引里插了一个" + key.typeName() + "键");
    }

    const std::string entry = makeLeafEntry(key, id);

    // 【为什么在进门就查长度，而不是等分裂失败了再说】
    // 一个键要是连一整页都放不下，那它**永远**插不进去——
    // 换页、分裂、重建都救不了它。这种事必须当场说清楚。
    // 更要紧的是：有了这一条，"分裂一定成功"就有了保证
    //（两半各自至少装得下一个条目），于是分裂那条路上
    // 不用再写任何回滚。
    if (entry.size() > MAX_CELL_SIZE || entry.size() > MAX_KEY_BYTES + 6) {
        throw AcornError("这个索引键太长了（编码之后 " +
                         std::to_string(entry.size()) + " 字节，上限 " +
                         std::to_string(MAX_KEY_BYTES + 6) +
                         " 字节）。索引建在短一点的列上，或者把这一列的值改短");
    }

    const Split split = insertInto(rootPage_, entry, key, id, 0);
    if (!split.happened) return;

    // ---- 根也分裂了：树长高一层 ----
    //
    // 新根只有两个条目：
    //   第 0 个 = [没有键][老根]   ← 老根那一支整个搬到左边
    //   第 1 个 = [分隔键][新页]   ← 右边新开的那一支
    //
    // 【这里要小心 handle 的排列】先取到新页的 handle，再在它上面写。
    // 写完把 rootPage_ 换成新页号——这一步调用方要负责存回目录记录
    //（见 Database::insertRow），不然下次打开库，索引就只剩
    // 分裂出来的那一半了。
    Pager::Handle fresh = pager_->allocatePage();
    initBtreePage(fresh.data(), PageType::BTREE_INTERNAL, INVALID_PAGE);

    const std::string left = makeNoKeyEntry(rootPage_);
    const std::string right = makeInternalEntry(split.sepKey, split.newPage);

    uint16_t slot = 0;
    if (!heapInsertAt(fresh.data(), 0, left.data(), left.size(), slot) ||
        !heapInsertAt(fresh.data(), 1, right.data(), right.size(), slot)) {
        throw AcornError("内部错误：新建 B+ 树根节点时放不下两个条目");
    }
    fresh.markDirty();

    rootPage_ = fresh.pageNo();
}

// ------------------------------------------------------------
//  删除
// ------------------------------------------------------------

bool Btree::remove(const Value& key, RowId id) {
    // 【这个函数不合并节点】理由见 btree.hpp 开头那段：
    // 路标过期了也依然是对的，所以省掉合并不影响正确性，
    // 只影响空间。代价是删空的叶子页留在链上。
    const uint32_t leaf = descendToLeaf(key, id);

    // 要改这一页（打墓碑），所以按"要改"来拿。
    // 找不到那个条目时会白记一页日志——但"找不到"本身就是异常路径
    //（下面 return false 之后 Database 会抛"索引和表对不上了"），
    // 那点浪费无所谓。
    Pager::Handle handle = pager_->getForWrite(leaf);
    const uint16_t count = heapSlotCount(handle.data());
    for (uint16_t slot = 0; slot < count; slot++) {
        size_t length = 0;
        const char* cell = heapCell(handle.data(), slot, length);
        if (cell == nullptr) continue;
        if (compareEntry(cell, length, key, id) != 0) continue;

        // 只把槽位长度改成 0（墓碑）。字节留在原地，等下次
        // 这一页重建（插入会重建）或者压实的时候收回去。
        heapDelete(handle.data(), slot);
        handle.markDirty();
        return true;
    }
    return false;
}

// ------------------------------------------------------------
//  查找
// ------------------------------------------------------------

void Btree::scanFrom(uint32_t startLeaf, bool hasLower, const Value& lower,
                     bool lowerInclusive, bool hasUpper, const Value& upper,
                     bool upperInclusive,
                     const std::function<void(RowId)>& visit) const {
    uint32_t pageNo = startLeaf;

    // 【为什么要数着走】叶子链是**文件里的 next 指针**串起来的。
    // 文件被改坏时（某个页的 next 指回了前面），这就变成一个环，
    // 扫描会永远跑下去——用户看到的是"卡住了"，而不是"文件坏了"。
    // 页数是个天然的步数上限：一条链不可能比文件里的总页数还长。
    const uint32_t limit = pager_->pageCount() + 1;

    for (uint32_t steps = 0; pageNo != INVALID_PAGE; steps++) {
        if (steps > limit) {
            throw AcornError("数据文件坏了：索引的叶子链走不到头（可能绕成了环）");
        }

        Pager::Handle handle = pager_->get(pageNo);
        if (pageType(handle.data()) != PageType::BTREE_LEAF) {
            throw AcornError("数据文件坏了：" + pageName(pageNo) +
                             "在叶子链上，但它不是叶子");
        }

        const uint16_t count = heapSlotCount(handle.data());
        const uint32_t next = pageNext(handle.data());

        for (uint16_t slot = 0; slot < count; slot++) {
            size_t length = 0;
            const char* cell = heapCell(handle.data(), slot, length);
            if (cell == nullptr) continue;

            bool hasKey = false;
            Value entryKey;
            RowId entryRow;
            size_t keyBytes = 0;
            decodeKeyBytes(cell, length, hasKey, entryKey, entryRow, keyBytes);
            if (!hasKey) {
                // 叶子里不该出现没有键的条目（那是内部节点的用法）。
                throw AcornError("数据文件坏了：索引的叶子里有个没有键的条目");
            }

            if (hasLower) {
                const int c = compareForSort(entryKey, lower);
                if (c < 0) continue;
                if (c == 0 && !lowerInclusive) continue;
            }
            if (hasUpper) {
                const int c = compareForSort(entryKey, upper);
                // 叶子里的键是排好序的，所以一旦越过上界，
                // 后面（以及后面那些叶子）只会更大。直接收工。
                if (c > 0) return;
                if (c == 0 && !upperInclusive) return;
            }

            visit(entryRow);
        }

        pageNo = next;
    }
}

void Btree::searchEqual(const Value& key,
                        const std::function<void(RowId)>& visit) const {
    // 从"这一列值等于 key 的那些行里最小的那个行号"开始往下找。
    //
    // 【为什么起点用 RowId{0, 0}】因为它是**任何真实行号的下界**：
    // 第 0 页是文件头，不可能存行，所以真实的行号一定 >= (1, 0)
    // > (0, 0)。用它当起点，下降会落在"第一个键 >= key 的条目"
    // 所在的那个叶子上——正好是第一个 5 待的地方，一个都不会漏。
    //
    // （这就是把行号拼进键里的第二个好处：能明确地说出
    //   "从这一组重复键的**第一个**开始"。）
    const RowId lowest{0, 0};
    scanFrom(descendToLeaf(key, lowest), true, key, true, true, key, true, visit);
}

void Btree::searchRange(bool hasLower, const Value& lower, bool lowerInclusive,
                        bool hasUpper, const Value& upper, bool upperInclusive,
                        const std::function<void(RowId)>& visit) const {
    // 起点：有下界就从下界那儿下去，没下界就从最左边的叶子开始。
    const uint32_t start = hasLower ? descendToLeaf(lower, RowId{0, 0})
                                    : leftmostLeaf();
    scanFrom(start, hasLower, lower, lowerInclusive, hasUpper, upper,
             upperInclusive, visit);
}

void Btree::scanAll(const std::function<void(RowId)>& visit) const {
    scanFrom(leftmostLeaf(), false, Value::makeNull(), true, false,
             Value::makeNull(), true, visit);
}

// ------------------------------------------------------------
//  收集所有页
// ------------------------------------------------------------

void Btree::collectPages(uint32_t pageNo, std::vector<uint32_t>& out,
                         int depth) const {
    if (depth > MAX_DEPTH) {
        throw AcornError("索引的层数超过了 " + std::to_string(MAX_DEPTH) +
                         " 层。这个文件多半被改坏了");
    }

    Pager::Handle handle = pager_->get(pageNo);
    const PageType type = pageType(handle.data());

    // 先记下自己，再往下走——前序。顺序对释放页没影响，
    // 但输出一份"从根往下"的列表，出了问题一眼能看出是哪一层。
    out.push_back(pageNo);

    if (type == PageType::BTREE_LEAF) return;
    if (type != PageType::BTREE_INTERNAL) {
        throw AcornError("数据文件坏了：" + pageName(pageNo) +
                         "本来应该是索引的节点，实际不是");
    }

    // 【为什么顺着孩子的指针走，而不是顺着叶子链走】
    // 叶子链只覆盖叶子，而且它是一条链——从它出发会漏掉内部节点，
    // 那些页不还回去就永远回不来了。递归走子树则一个不漏。
    // （而且叶子的 next 是单向的，就算只想收集叶子也不该用它。）
    const uint16_t count = heapSlotCount(handle.data());
    std::vector<uint32_t> children;
    children.reserve(count);
    for (uint16_t slot = 0; slot < count; slot++) {
        size_t length = 0;
        const char* cell = heapCell(handle.data(), slot, length);
        if (cell == nullptr || length < 4) continue;
        children.push_back(readU32(cell + length - 4));
    }

    // 先把孩子收齐再递归：递归会取别的页，而这里不能再碰 handle
    // 指向的东西（虽然 pin 着不会失效，但把"用到的都先拿出来"
    // 当成习惯，以后加并发锁的时候这段不用重看）。
    for (uint32_t child : children) {
        collectPages(child, out, depth + 1);
    }
}

std::vector<uint32_t> Btree::allPages() const {
    std::vector<uint32_t> pages;
    collectPages(rootPage_, pages, 0);
    return pages;
}

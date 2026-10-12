#include "database.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "error.hpp"
#include "page.hpp"
#include "token.hpp"  // lowercase

// ------------------------------------------------------------
//  打开
// ------------------------------------------------------------

Database::Database(const std::string& path, CrashPoint crash, int lockWaitMs)
    : pager_(new Pager(path, crash, lockWaitMs)) {
    loadCatalog();

    // ---- 建库时的初始化，提交掉 ----
    //
    // 新文件的头几页（文件头、第一个目录页）是在 loadCatalog 里
    // 分配出来的，而分配会顺手开一个隐式事务（见 Pager::getForWrite）。
    // 不在这儿提交的话，这个事务会一直开到用户的**第一条语句**，
    // 然后按"那条语句自己开的事务"被提交或者回滚——
    // 要是第一条语句是 SELECT（最常见），它就一直没人管，
    // 最后在析构时被回滚，留下一个"文件在、文件头是空的"库。
    //
    // 【顺手得到的好处】初始化本身也变成原子的了：要是建到一半
    // 掉电，下次打开会照日志整个退回去，而不是留下半个文件头。
    if (pager_->inTransaction()) {
        pager_->commitTransaction();
    }
}

void Database::loadCatalog() {
    // ---- 全新的文件（或者内存库）----
    if (pager_->pageCount() == 0) {
        // 第一个分配出来的页必然是第 0 页（文件本来是空的）。
        Pager::Handle header = pager_->allocatePage();
        initFileHeader(header.data());
        header.markDirty();
        return;
    }

    // ---- 已经有内容的文件 ----
    {
        Pager::Handle header = pager_->get(0);
        std::string why;
        if (!checkFileHeader(header.data(), why)) {
            // 【绝不"猜"】文件不是我们的格式就直接拒绝，
            // 一个字都不往里写。设想用户敲错了文件名：
            //     acorn -d 论文.docx -e "INSERT INTO t VALUES (1);"
            // 要是这里"宽容"地当成新库初始化，那个文件就被
            // 4096 字节的第 0 页盖掉了，而且**用户不会看到任何提示**。
            // 报错是这个场景下唯一可接受的行为。
            throw AcornError("这不是一个 Acorn 数据库文件：" + pager_->path() +
                             "（" + why + "）。换个文件名，或者先把它挪走");
        }

        // ---- 走一遍目录链，把表结构读回来 ----
        //
        // 【为什么表结构要存下来，而不是让用户每次重新 CREATE】
        // 因为数据在磁盘上是一堆**裸字节**。哪几个字节是整数、
        // 哪几个是文本的长度、文本从哪开始——全靠表结构来解释。
        // 没有表结构，那 4KB 就只是一堆数，永远变不成行。
        // 「数据」和「数据的形状」必须一起存，这是所有持久化格式
        // 都要面对的第一件事。
        uint32_t pageNo = fileCatalogFirst(header.data());
        // 索引目录链的第一页。**必须在这里读出来**——出了这个作用域
        // header 这张借条就还回去了，再问就得重新 get 一次。
        const uint32_t indexFirst = fileIndexCatalogFirst(header.data());
        // 【这里**不用** markDirty】借条还回去靠的是 Handle 的析构，
        // 和标不标脏毫无关系。以前这句是白标的——它会让每次打开库
        // 都凭空多写 4KB 回去，而内容一个字节都没变。
        // 更要紧的是阶段 5 之后它变成了一句**谎话**：
        // markDirty 的意思是"我改过这一页"，而这里根本没改。
        // （真按"改过"来算，标了脏又不记日志，正好违反不变式。）

        while (pageNo != INVALID_PAGE) {
            Pager::Handle page = pager_->get(pageNo);
            const uint32_t next = pageNext(page.data());

            catalogScan(page.data(), [&](const char* record, size_t length,
                                         uint16_t offset) {
                // 第 0 个字节是标志：0 = 活着，1 = 已删（DROP TABLE 过）
                if (record[0] != 0) return;

                TableSchema schema;
                uint32_t firstPage = INVALID_PAGE;
                if (!decodeCatalogRecord(record, length, schema, firstPage)) {
                    throw AcornError("目录页里的记录读不出来（" +
                                     pageName(pageNo) + "，第 " +
                                     std::to_string(offset) +
                                     " 字节）。这个文件可能坏了");
                }

                const std::string key = schema.key;
                if (tables_.find(key) != tables_.end()) {
                    throw AcornError("目录里有两张叫 `" + schema.name +
                                     "` 的表。这个文件坏了");
                }

                const std::string name = schema.name;
                order_.push_back(name);
                catalogRefs_[key] = CatalogRef{pageNo, offset};
                tables_.emplace(key, Table(*pager_, std::move(schema), firstPage));
            });

            catalogLast_ = pageNo;
            pageNo = next;
        }

        // ---- 再走一遍索引目录链 ----
        //
        // 【为什么必须放在表的循环**后面】因为索引记录里没有列类型，
        // 要拿表结构现补（见下面那段）。顺序反过来的话，
        // tables_ 还是空的，每条索引记录都会找不到它的表。
        pageNo = indexFirst;
        while (pageNo != INVALID_PAGE) {
            Pager::Handle page = pager_->get(pageNo);
            const uint32_t next = pageNext(page.data());

            catalogScan(page.data(), [&](const char* record, size_t length,
                                         uint16_t offset) {
                if (record[0] != 0) return;  // DROP INDEX 过

                IndexSchema schema;
                uint32_t rootPage = INVALID_PAGE;
                if (!decodeIndexRecord(record, length, schema, rootPage)) {
                    throw AcornError("索引目录里的记录读不出来（" +
                                     pageName(pageNo) + "，第 " +
                                     std::to_string(offset) +
                                     " 字节）。这个文件可能坏了");
                }

                if (indexes_.find(schema.key) != indexes_.end()) {
                    throw AcornError("索引目录里有两个叫 `" + schema.name +
                                     "` 的索引。这个文件坏了");
                }

                const auto tableIt = tables_.find(schema.tableKey);
                if (tableIt == tables_.end()) {
                    // 表没了索引还在。这只可能是 DROP TABLE 漏了
                    // 连带删索引那一步——那时候索引里的行号指向的是
                    // 已经还给空闲链的页，也就是**别人的数据**。
                    // 与其带着它跑，不如当场说清楚。
                    throw AcornError(
                        "索引 `" + schema.name + "` 建在表 `" +
                        schema.tableName + "` 上，但目录里没有这张表。" +
                        "这个文件坏了（或者是一次没做完的 DROP TABLE）");
                }

                const TableSchema& tableSchema = tableIt->second.schema();
                if (schema.column < 0 ||
                    static_cast<size_t>(schema.column) >=
                        tableSchema.columns.size()) {
                    throw AcornError(
                        "索引 `" + schema.name + "` 说是建在表 `" +
                        tableSchema.name + "` 的第 " +
                        std::to_string(schema.column) +
                        " 列上，但这张表只有 " +
                        std::to_string(tableSchema.columns.size()) +
                        " 列。这个文件坏了");
                }

                // 列类型**现查**，不存在索引记录里——存两份的话，
                // 将来加 ALTER TABLE 就会出现"索引记录说是 INT、
                // 表里说是 TEXT"这种两个真相打架的局面。
                schema.columnType =
                    tableSchema.columns[static_cast<size_t>(schema.column)].type;
                // 表名也用表结构里的原样写法。索引记录里那份是
                // 建索引的时候抄的，表要是改过名（以后的事），
                // 它就是个过期的副本。
                schema.tableName = tableSchema.name;

                const std::string indexKey = schema.key;
                const std::string tableKey = schema.tableKey;
                const std::string indexName = schema.name;
                indexOrder_.push_back(indexName);
                indexesByTable_[tableKey].push_back(indexKey);
                indexes_.emplace(indexKey,
                                 IndexInfo{std::move(schema), rootPage,
                                           CatalogRef{pageNo, offset}});
            });

            indexCatalogLast_ = pageNo;
            pageNo = next;
        }
    }
}

Database::~Database() = default;

void Database::flush() {
    pager_->flush();
}

// ------------------------------------------------------------
//  事务（阶段 5）
// ------------------------------------------------------------

void Database::begin() {
    // 【已经在事务里的话，谁报错】执行器报，不在这里报。
    // 因为这件事只有执行器知道怎么说人话（"BEGIN 不能嵌套，
    // 要结束先敲 COMMIT 或 ROLLBACK"），而这一层的读者是程序。
    // 这里拦的是"程序自己写错了"——那种情况下报错越直白越好。
    if (pager_->inTransaction()) {
        throw AcornError("内部错误：已经在事务里了，不能再开一个");
    }
    // 用户敲的 BEGIN 是**显式**事务：它不会被自动提交。
    pager_->beginTransaction(/*implicit=*/false);
}

void Database::commit() {
    pager_->commitTransaction();
}

void Database::rollback() {
    // ---- 1. 页退回磁盘上原来的样子 ----
    pager_->rollbackTransaction();

    // ---- 2. 内存里那份映射也跟着退回去 ----
    //
    // ★ 这一步不能省 ★
    // 页退回去了，但 tables_ 里可能还留着"事务里建的那张表"，
    // 或者少着"事务里删掉的那张表"。那之后每一次查询都会
    // 拿着一份和磁盘对不上的地图走。
    //
    // 办法是重新读一遍目录——唯一的真相来源是磁盘。
    // （为什么不是"把改动反向撤销一遍"，见 database.hpp 里那段。）
    clearCatalog();
    loadCatalog();
}

void Database::clearCatalog() {
    tables_.clear();
    order_.clear();
    catalogRefs_.clear();
    catalogLast_ = INVALID_PAGE;

    indexes_.clear();
    indexOrder_.clear();
    indexesByTable_.clear();
    indexCatalogLast_ = INVALID_PAGE;

    // 【注意这里没有清 pager_】它才是"库本身"，回滚不该把它扔掉。
    // 重建一个 Pager 也能达到目的，但那会把缓存、文件句柄、
    // 事务状态全丢掉——而回滚只是数据退回一步，库还是同一个库。
}

// ------------------------------------------------------------
//  找表
// ------------------------------------------------------------

std::string Database::noSuchTable(const std::string& tableName) const {
    std::string message = "没有叫 `" + tableName + "` 的表";

    if (order_.empty()) {
        message += "（现在一张表都没有，先用 CREATE TABLE 建一张）";
        return message;
    }

    message += "（现在有：";
    for (size_t i = 0; i < order_.size(); i++) {
        if (i > 0) message += ", ";
        message += order_[i];
    }
    message += "）";
    return message;
}

bool Database::exists(const std::string& tableName) const {
    return tables_.find(lowercase(tableName)) != tables_.end();
}

Table& Database::table(const std::string& tableName) {
    auto it = tables_.find(lowercase(tableName));
    if (it == tables_.end()) {
        throw AcornError(noSuchTable(tableName));
    }
    return it->second;
}

const Table& Database::table(const std::string& tableName) const {
    // 和上面那个非 const 版本逻辑一模一样。
    //
    // C++ 里有个省掉这份重复的惯用写法：让非 const 版本去调 const 版本，
    // 再把结果 const_cast 掉。但那个写法要解释为什么 const_cast 在这里
    // 是安全的，读起来比这两份重复还费劲。
    // 四行重复换一个不用解释，值。
    auto it = tables_.find(lowercase(tableName));
    if (it == tables_.end()) {
        throw AcornError(noSuchTable(tableName));
    }
    return it->second;
}

const TableSchema& Database::schemaOf(const std::string& tableName) const {
    return table(tableName).schema();
}

// ------------------------------------------------------------
//  目录
// ------------------------------------------------------------

namespace {

// 两条目录链各自"第一页在哪"写在文件头的不同偏移上
//（表在 20，索引在 28）。传函数指针而不是传一个枚举再 if，
// 是因为这两个函数本来就存在，而且签名一模一样。
using SetFirstFn = void (*)(char*, uint32_t);

}  // namespace

Database::CatalogRef Database::appendToChain(const std::string& record,
                                            uint32_t& last,
                                            SetFirstFn setFirst) {
    // ---- 还没有目录页，先建第一个 ----
    if (last == INVALID_PAGE) {
        Pager::Handle fresh = pager_->allocatePage();
        initCatalogPage(fresh.data(), INVALID_PAGE);
        fresh.markDirty();
        last = fresh.pageNo();

        // 文件头里的"链第一页"要指过来。
        //
        // 【注意这里同时按着两页】fresh（目录页）和 header（第 0 页）。
        // 缓存支持同时借出多页，就是为了这种地方——"两张纸要一起改"
        // 在数据库里到处都是（这里是目录链表头，别处是数据页链表尾）。
        Pager::Handle header = pager_->getForWrite(0);
        setFirst(header.data(), last);
        header.markDirty();
    }

    // ---- 先试最后一页 ----
    {
        // 和 Table::placeAnywhere 里那句一样：这儿也可能是"试了才发现
        // 装不下"，但日志多记一页远比"改了没记"便宜（见那边的说明）。
        Pager::Handle page = pager_->getForWrite(last);
        uint16_t offset = 0;
        if (catalogAppend(page.data(), record.data(), record.size(), offset)) {
            page.markDirty();
            return CatalogRef{last, offset};
        }
    }

    // ---- 装不下了，挂一个新目录页 ----
    const uint32_t previous = last;

    Pager::Handle fresh = pager_->allocatePage();
    initCatalogPage(fresh.data(), INVALID_PAGE);
    const uint32_t newPageNo = fresh.pageNo();

    uint16_t offset = 0;
    if (!catalogAppend(fresh.data(), record.data(), record.size(), offset)) {
        // 一条记录连一整页空页都装不下。表结构上千列才可能走到这里，
        // 所以现实中说这句话是给写代码的人看的。
        throw AcornError("内部错误：一条目录记录有 " +
                         std::to_string(record.size()) +
                         " 字节，连一整页目录页都装不下");
    }
    fresh.markDirty();

    {
        Pager::Handle tail = pager_->getForWrite(previous);
        setPageNext(tail.data(), newPageNo);
        tail.markDirty();
    }

    last = newPageNo;
    return CatalogRef{newPageNo, offset};
}

Database::CatalogRef Database::appendToCatalog(const std::string& record) {
    return appendToChain(record, catalogLast_, setFileCatalogFirst);
}

Database::CatalogRef Database::appendToIndexCatalog(const std::string& record) {
    return appendToChain(record, indexCatalogLast_, setFileIndexCatalogFirst);
}

// ------------------------------------------------------------
//  建表
// ------------------------------------------------------------

void Database::createTable(TableSchema schema) {
    // 重名检查用 key（小写），所以 `CREATE TABLE Users` 之后
    // 再 `CREATE TABLE users` 会被拦住——这在 SQL 里确实该拦。
    if (tables_.find(schema.key) != tables_.end()) {
        throw AcornError("已经有一张叫 `" + schema.name + "` 的表了");
    }

    // 下面要 move 走 schema，所以这几样必须**先**取出来。
    // 顺序写反了不会报错，只会得到一个空字符串的表名——
    // 然后在某个谁也想不到的地方炸掉。
    const std::string key = schema.key;
    const std::string name = schema.name;

    // 先把目录记录编出来。这一步会抛（列太多、名字太长），
    // 那就还没分配任何页，干干净净地失败。
    const std::string record = encodeCatalogRecord(schema, INVALID_PAGE);
    if (record.size() > MAX_CATALOG_RECORD) {
        throw AcornError("这张表的定义有 " + std::to_string(record.size()) +
                         " 字节，超过一页能装下的上限（" +
                         std::to_string(MAX_CATALOG_RECORD) + " 字节）。少几列试试");
    }

    // ---- 给这张表开第一个数据页 ----
    // 【为什么建表就分配，哪怕一行都还没插】
    // 因为"这张表的数据从哪一页开始"这个信息必须**当场**写进目录，
    // 不然目录记录里就得留个空位，等第一次插入再回来改它——
    // 那又是一份"待补的信息"。空表占 4KB 换掉这个麻烦，值。
    Pager::Handle first = pager_->allocatePage();
    initHeapPage(first.data(), INVALID_PAGE);
    first.markDirty();
    const uint32_t firstPage = first.pageNo();

    // ---- 写目录 ----
    // 这里要用带真实 firstPage 的记录重新编一份。
    // （上面那份 firstPage 是占位的，只为了量长度。
    //   长度不受这个数影响，因为页号是定长 4 字节。）
    try {
        const CatalogRef ref = appendToCatalog(
            encodeCatalogRecord(schema, firstPage));

        order_.push_back(name);
        catalogRefs_[key] = ref;
        tables_.emplace(key, Table(*pager_, std::move(schema), firstPage));
    } catch (...) {
        // 目录写失败就把刚分配的数据页还回去，别留一个谁也不知道的孤儿页。
        // 【为什么值得写这个 catch】一个"分配了但没人引用"的页，
        // 后果是文件里凭空多 4KB，而且**永远找不回来**——
        // 空闲链上没它，目录里也没它。这种泄漏攒久了文件就废了。
        pager_->freePage(firstPage);
        throw;
    }
}

// ------------------------------------------------------------
//  删表
// ------------------------------------------------------------

void Database::dropTable(const std::string& tableName) {
    const std::string key = lowercase(tableName);
    auto it = tables_.find(key);
    if (it == tables_.end()) {
        throw AcornError(noSuchTable(tableName));
    }

    // ---- 先把这张表的索引全删掉 ----
    //
    // 【为什么必须先做，而且必须做】
    // 索引里存的是行号（页号 + 槽位）。表一删，那些页就还给
    // 空闲链了，随时会被别的表拿去用——索引里那些行号于是指向
    // **别人的数据**。留在那儿的话，一个"WHERE 索引列 = x"的查询
    // 会去读别人的页，把别人的行当自己的答案返回。不报错。
    //
    // 【为什么先复制一份名字】
    // dropIndexByKey 会改 indexesByTable_，而下面这个循环正拿着
    // 它的内部 vector 在走。一边遍历一边改同一个容器，
    // 是自找的。
    const auto byTable = indexesByTable_.find(key);
    if (byTable != indexesByTable_.end()) {
        const std::vector<std::string> indexKeys = byTable->second;
        for (const std::string& indexKey : indexKeys) {
            dropIndexByKey(indexKey);
        }
    }

    // ---- 再把数据页还给空闲链 ----
    // 【顺序要紧：先还页，再从内存里删表】
    // 反过来的话，releaseAllPages 就要在一个已经不在 map 里的
    // Table 上调用——那种代码能写，但"用到一半的东西被拿走了"
    // 是最容易出事的一类写法。
    //
    // （上面删索引放在这一步**之前**，理由不是顺序依赖，
    //   而是上面那段说的：索引项必须在页被还回去之前失效。）
    it->second.releaseAllPages();

    // ---- 再把目录记录标成已删 ----
    const auto refIt = catalogRefs_.find(key);
    if (refIt != catalogRefs_.end()) {
        Pager::Handle page = pager_->getForWrite(refIt->second.pageNo);
        catalogMarkDeleted(page.data(), refIt->second.offset);
        page.markDirty();
        catalogRefs_.erase(refIt);
    }

    // ---- 最后从内存里去掉 ----
    const std::string name = it->second.schema().name;
    tables_.erase(it);

    order_.erase(std::remove(order_.begin(), order_.end(), name), order_.end());
}

// ------------------------------------------------------------
//  索引维护（写路径）
// ------------------------------------------------------------

void Database::checkIndexKey(const Value& key, const IndexSchema& schema) {
    // NULL 也能进索引：`WHERE x IS NULL` 是要走索引的，而索引里
    // 把 NULL 排在最小（见 compareForSort）。所以这里不拦。
    if (key.isNull()) return;

    // 类型要对得上。整数放进浮点索引是允许的——fitToType 会把它
    // 补成浮点，而两者比较时本来就会统一成 double。
    const bool compatible =
        key.type == schema.columnType ||
        (schema.columnType == Value::Type::REAL && key.type == Value::Type::INTEGER);
    if (!compatible) {
        throw AcornError("索引 `" + schema.name + "` 建在" +
                         std::string(sqlTypeName(schema.columnType)) + " 列上，装不下" +
                         key.typeName() + " " + key.toSqlLiteral() + " 这个键");
    }

    const size_t bytes = encodeValue(key).size();
    if (bytes > MAX_KEY_BYTES) {
        throw AcornError("索引 `" + schema.name + "` 装不下这个键：编码之后 " +
                         std::to_string(bytes) + " 字节，上限 " +
                         std::to_string(MAX_KEY_BYTES) +
                         " 字节（文本列的话，一个值最长 " +
                         std::to_string(MAX_INDEX_TEXT) + " 字节）。"
                         "把这一列的值改短，或者别在这个列上建索引");
    }
}

void Database::insertIndexEntry(IndexInfo& info, const Value& key, RowId id) {
    Btree tree(*pager_, info.rootPage, info.schema.columnType);
    tree.insert(key, id);

    if (tree.rootPage() == info.rootPage) return;

    // ---- 树长高了一层，根页换人了 ----
    // 目录记录里存的就是根页号。不写回的话，下次打开库，
    // 索引就只剩新根下面那半棵树——而那是**一半的行**查不到，
    // 且不报错。
    info.rootPage = tree.rootPage();

    Pager::Handle page = pager_->getForWrite(info.ref.pageNo);
    catalogSetRecordPage(page.data(), info.ref.offset, info.rootPage);
    page.markDirty();
}

void Database::removeIndexEntry(IndexInfo& info, const Value& key, RowId id) {
    Btree tree(*pager_, info.rootPage, info.schema.columnType);
    if (tree.remove(key, id)) return;

    // 【找不到就报错，不要装作没事】
    // 这条索引项应该存在（它是当初插行时一起插进去的）。找不到
    // 说明某一条写路径漏了索引维护——那时候继续跑下去，
    // 索引和表就永久性地对不上了。
    //
    // 注意这里**不需要**处理"根页变了"：本项目的删除不合并节点，
    // 所以根页永远不会因为删除而换（见 btree.hpp 开头那段）。
    throw AcornError("内部错误：索引 `" + info.schema.name +
                     "` 里找不到该删的那一条（键 " + key.toSqlLiteral() +
                     "，行号 " + std::to_string(id.pageNo) + ":" +
                     std::to_string(id.slot) + "）。索引和表已经对不上了");
}

void Database::insertRow(const std::string& tableName, Row row) {
    Table& target = table(tableName);
    const auto byTable = indexesByTable_.find(target.schema().key);
    if (byTable == indexesByTable_.end()) {
        target.insert(row);
        return;
    }

    // ---- 一、先把所有索引键量一遍 ----
    // 键太长是这条路上唯一**可以预见**的失败。放在最前面，
    // 就保证"插表"和"插索引"两步都不会半路失败。
    // （长度检查不受"整数补成浮点"影响——两种编码都是 9 个字节。）
    for (const std::string& indexKey : byTable->second) {
        const IndexInfo& info = indexes_.find(indexKey)->second;
        checkIndexKey(row[static_cast<size_t>(info.schema.column)], info.schema);
    }

    // ---- 二、插表 ----
    // 【为什么插表在前】因为 insert 会就地补型（列是 REAL 时
    // 把整数补成浮点），插完之后 row 里就是"真正存进去的样子"，
    // 索引该存的是它。反过来先取键的话，索引里会存着补型之前的
    // 那个整数——两者比较时相等，所以查得到，但"索引里存的和
    // 行里存的不一样"本身就是个隐患。
    const RowId id = target.insert(row);

    // ---- 三、插索引 ----
    // 走到这里不会再失败了（键都量过了），所以不存在
    // "行进去了、索引没进去"的中间状态。
    for (const std::string& indexKey : byTable->second) {
        IndexInfo& info = indexes_.find(indexKey)->second;
        insertIndexEntry(info, row[static_cast<size_t>(info.schema.column)], id);
    }
}

void Database::updateRow(const std::string& tableName, RowId id, Row row) {
    Table& target = table(tableName);
    const auto byTable = indexesByTable_.find(target.schema().key);
    if (byTable == indexesByTable_.end()) {
        target.updateRow(id, row);
        return;
    }

    // ---- 老的值要先读出来 ----
    // 索引里那条老索引项是按**老值**存的。不先读到它，就没法
    // 把它删掉——那样索引里会永远留着一条"键是老值、行号是这一行"
    // 的项，于是 `WHERE 老值` 会查出一行内容对不上的数据。
    //
    // 而且必须在 updateRow **之前**读：它会就地改 row，
    // 改完老值就没了。
    const Row before = target.row(id);

    std::vector<Value> oldKeys;
    oldKeys.reserve(byTable->second.size());
    for (const std::string& indexKey : byTable->second) {
        const IndexInfo& info = indexes_.find(indexKey)->second;
        const size_t column = static_cast<size_t>(info.schema.column);
        // 新键先量一遍，理由同 insertRow。
        checkIndexKey(row[column], info.schema);
        oldKeys.push_back(before[column]);
    }

    // updateRow 返回的是**现在**的行号。改这一列之外的值通常
    // 不会让行搬家，行号就还是原来那个；整页装不下时它会搬到
    // 别的页去，那时行号变了，索引里的新项要按新行号存。
    const RowId now = target.updateRow(id, row);

    for (size_t i = 0; i < byTable->second.size(); i++) {
        IndexInfo& info = indexes_.find(byTable->second[i])->second;
        const size_t column = static_cast<size_t>(info.schema.column);

        // 【先删老的，再插新的——顺序不能反】
        // 反过来的话，`UPDATE t SET id = id`（新键 == 老键，
        // 非常常见）会先插一条一模一样的，再按 (键, 行号) 去删——
        // 两条完全一样，删掉的是哪一条就无法保证了，
        // 结果是索引里留下一条、丢掉一条。
        removeIndexEntry(info, oldKeys[i], id);
        insertIndexEntry(info, row[column], now);
    }
}

void Database::deleteRows(const std::string& tableName,
                          const std::vector<RowId>& ids) {
    Table& target = table(tableName);
    const auto byTable = indexesByTable_.find(target.schema().key);
    if (byTable == indexesByTable_.end()) {
        target.deleteRows(ids);
        return;
    }

    // 【索引项要按行里存着的值删，所以得先把行读出来】
    // 删完那些槽位就成了墓碑，行再也读不出来了——顺序不能反。
    //
    // 代价是每删一行要回表读一次（执行器那边其实刚读过）。
    // 想省掉这一趟的话，得让调用方把行一起传进来。
    // 现在不省：多读一次是慢，读错了是坏。
    for (const RowId& id : ids) {
        const Row before = target.row(id);
        for (const std::string& indexKey : byTable->second) {
            IndexInfo& info = indexes_.find(indexKey)->second;
            removeIndexEntry(info,
                             before[static_cast<size_t>(info.schema.column)], id);
        }
    }

    target.deleteRows(ids);
}

// ------------------------------------------------------------
//  索引：建和删
// ------------------------------------------------------------

void Database::createIndex(IndexSchema schema) {
    // 索引名在**整个库**里唯一，不是"每张表内唯一"——
    // 因为 DROP INDEX 只写索引名，不写表名。
    if (indexes_.find(schema.key) != indexes_.end()) {
        throw AcornError("已经有一个叫 `" + schema.name + "` 的索引了");
    }

    Table& target = table(schema.tableName);  // 表不存在会在这儿抛
    const TableSchema& tableSchema = target.schema();

    if (schema.column < 0 ||
        static_cast<size_t>(schema.column) >= tableSchema.columns.size()) {
        // 执行器那边本来会先查一遍列名，走到这儿说明它漏了。
        throw AcornError("内部错误：建索引时给的列号 " +
                         std::to_string(schema.column) + " 在表 `" +
                         tableSchema.name + "` 上越界了");
    }

    const size_t column = static_cast<size_t>(schema.column);
    // 表名和列类型都**以表结构为准**，不信调用方填的。
    // （索引记录里存的是列号，列类型现查——存两份的话，
    //   将来加 ALTER TABLE 就会出现两个真相打架。）
    schema.tableKey = tableSchema.key;
    schema.tableName = tableSchema.name;
    schema.columnType = tableSchema.columns[column].type;

    // ---- 一、先把目录记录编出来量长度 ----
    // 这一步会抛（名字太长），那就还什么都没分配，干净地失败。
    // 页号是定长 4 字节，所以用占位页号量出来的长度是真的。
    const std::string probe = encodeIndexRecord(schema, INVALID_PAGE);
    if (probe.size() > MAX_INDEX_RECORD) {
        throw AcornError("这条索引的定义有 " + std::to_string(probe.size()) +
                         " 字节，超过一页能装下的上限（" +
                         std::to_string(MAX_INDEX_RECORD) + " 字节）");
    }

    // ---- 二、扫一遍全表，把要进索引的键收齐、并且**全部量一遍长度** ----
    //
    // 【为什么先量长度】一个个插进去才发现"第 500 行的这一列太长"，
    // 那时候树已经建了一半：要么回滚（得把树占的页一个不落地
    // 找回来还掉），要么留一个半成品索引。两个都不好。
    // 先量一遍，建树那条路上就没有任何**可以预见**的失败了。
    //
    // 【为什么也要先收集】因为 scan 正在借着一个数据页，
    // 而建树要借一堆别的页。收集完再建，"扫"和"写"两件事
    // 分得干干净净，不用去想"扫描过程中能不能写"。
    std::vector<std::pair<Value, RowId>> entries;
    target.scan([&](RowId id, const Row& row) {
        const Value& key = row[column];
        checkIndexKey(key, schema);
        entries.emplace_back(key, id);
    });

    // ---- 三、建树 ----
    uint32_t rootPage = INVALID_PAGE;
    {
        Pager::Handle root = pager_->allocatePage();
        initBtreePage(root.data(), PageType::BTREE_LEAF, INVALID_PAGE);
        root.markDirty();
        rootPage = root.pageNo();
    }   // 借条还回去，接下来建树自己去借页
    {
        Btree tree(*pager_, rootPage, schema.columnType);
        for (const std::pair<Value, RowId>& entry : entries) {
            tree.insert(entry.first, entry.second);
        }
        // 树长高了根就换了，目录里要记的是**最后**那个根。
        rootPage = tree.rootPage();
    }

    // ---- 四、写目录记录 ----
    const CatalogRef ref = appendToIndexCatalog(encodeIndexRecord(schema, rootPage));

    // ---- 五、登记到内存里 ----
    const std::string indexKey = schema.key;
    const std::string tableKey = schema.tableKey;
    indexOrder_.push_back(schema.name);
    indexesByTable_[tableKey].push_back(indexKey);
    indexes_.emplace(indexKey, IndexInfo{std::move(schema), rootPage, ref});
}

void Database::dropIndexByKey(const std::string& indexKey) {
    const auto it = indexes_.find(indexKey);
    if (it == indexes_.end()) return;

    // ---- 先把树占的页还给空闲链 ----
    // 和 dropTable 一样**先收集、再释放**。allPages 走的是节点里的
    // 孩子指针而不是页头里的 next，所以本来也不会迷路；
    // 但"先收集再释放"这个习惯值得保持——改一处实现的人
    // 不会回头重看这段。
    {
        Btree tree(*pager_, it->second.rootPage, it->second.schema.columnType);
        const std::vector<uint32_t> pages = tree.allPages();
        for (uint32_t pageNo : pages) {
            pager_->freePage(pageNo);
        }
    }

    // ---- 目录记录标成已删 ----
    {
        Pager::Handle page = pager_->getForWrite(it->second.ref.pageNo);
        catalogMarkDeleted(page.data(), it->second.ref.offset);
        page.markDirty();
    }

    // ---- 最后从内存里去掉 ----
    const std::string name = it->second.schema.name;
    const std::string tableKey = it->second.schema.tableKey;
    indexes_.erase(it);

    indexOrder_.erase(std::remove(indexOrder_.begin(), indexOrder_.end(), name),
                      indexOrder_.end());

    const auto byTable = indexesByTable_.find(tableKey);
    if (byTable != indexesByTable_.end()) {
        std::vector<std::string>& list = byTable->second;
        list.erase(std::remove(list.begin(), list.end(), indexKey), list.end());
        if (list.empty()) indexesByTable_.erase(byTable);
    }
}

void Database::dropIndex(const std::string& indexName) {
    const std::string key = lowercase(indexName);
    if (indexes_.find(key) == indexes_.end()) {
        std::string message = "没有叫 `" + indexName + "` 的索引";
        if (indexOrder_.empty()) {
            message += "（现在一个索引都没有，先用 CREATE INDEX 建一个）";
        } else {
            message += "（现在有：";
            for (size_t i = 0; i < indexOrder_.size(); i++) {
                if (i > 0) message += ", ";
                message += indexOrder_[i];
            }
            message += "）";
        }
        throw AcornError(message);
    }

    dropIndexByKey(key);
}

// ------------------------------------------------------------
//  索引：查询和查看
// ------------------------------------------------------------

std::vector<std::string> Database::indexNames() const {
    return indexOrder_;
}

const IndexSchema* Database::findIndex(const std::string& indexName) const {
    const auto it = indexes_.find(lowercase(indexName));
    if (it == indexes_.end()) return nullptr;
    return &it->second.schema;
}

std::vector<IndexSchema> Database::indexesOf(const std::string& tableName) const {
    const TableSchema& schema = schemaOf(tableName);  // 表不存在会抛
    std::vector<IndexSchema> out;

    const auto it = indexesByTable_.find(schema.key);
    if (it == indexesByTable_.end()) return out;

    for (const std::string& indexKey : it->second) {
        const auto found = indexes_.find(indexKey);
        if (found != indexes_.end()) out.push_back(found->second.schema);
    }
    return out;
}

void Database::searchIndex(const std::string& indexName, bool hasLower,
                           const Value& lower, bool lowerInclusive,
                           bool hasUpper, const Value& upper,
                           bool upperInclusive,
                           const std::function<void(RowId)>& visit) const {
    const auto it = indexes_.find(lowercase(indexName));
    if (it == indexes_.end()) {
        // 规划器是拿着 findIndex 的结果来的，走到这儿说明
        // 中间有人把它删了（同一条语句里不可能，所以是内部错误）。
        throw AcornError("内部错误：要用的索引 `" + indexName + "` 不见了");
    }

    Btree tree(*pager_, it->second.rootPage, it->second.schema.columnType);
    tree.searchRange(hasLower, lower, lowerInclusive, hasUpper, upper,
                     upperInclusive, visit);
}

// ------------------------------------------------------------
//  列出所有表
// ------------------------------------------------------------

std::vector<std::string> Database::tableNames() const {
    return order_;
}

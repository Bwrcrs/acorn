#pragma once

// ============================================================
//  Database —— 一个数据库实例：一个文件 + 一堆表
//
//  职责两件事：
//    1. 管好"表名 -> 表"这个映射，**所有按名字找表的地方都要经过这里**
//    2. 管好**表结构存在哪**（目录）
//
//  第 1 件事从阶段 1 就在做，理由没变：大小写抹平、重名检查、
//  找不到时的报错，这三件事必须在同一个地方做。散在各个语句的
//  执行里的话，早晚会出现"CREATE 的时候判重名用了小写、SELECT
//  的时候查表忘了转小写"这种不对等，表现出来就是"建了表却说没有"。
//
//  第 2 件事是阶段 3 新加的。数据能落盘之后，**表结构也得落盘**——
//  不然重开一个库，数据都在，却不知道每张表有几列、叫什么名字，
//  那些字节就永远解不出来了。这就是"目录"（catalog）的由来。
//
//  ------------------------------------------------------------
//  目录长什么样
//  ------------------------------------------------------------
//
//   第 0 页（文件头）
//        | 记着目录链的第一页
//        v
//   +----------+    +----------+
//   | 目录页 1  | -> | 目录页 2  | -> 没有了
//   +----------+    +----------+
//     一条条记录      一条条记录
//     （表名、列、第一个数据页）
//
//  和表的数据页一样是链表。目录通常很小——一张表才占几十字节，
//  一页 4KB 能装下几十张表——所以绝大多数库只有一页目录。
//  但格式上必须允许它增长，不然"表太多了"会变成一个没法处理的情况。
//
//  【DROP TABLE 只把记录打个"已删"标记，不真删】
//  和行删除一样：删中间一条，后面记录的下标全变，而且还得处理
//  "这一页空了怎么办"。打个标记、打开时跳过，代价是几十字节。
//  这条欠条很划算——现在不用为"目录页回收"写任何代码。
//
//  ------------------------------------------------------------
//  阶段 5：事务
//  ------------------------------------------------------------
//
//  事务本身在 Pager 那一层（回滚日志管的是"页"）。这一层多出来的
//  只有一件事，但它非做不可：
//
//  ★ 回滚之后，内存里那份"有哪些表、有哪些索引"必须重新读一遍 ★
//
//  因为它是**磁盘状态的副本**：tables_ / indexes_ / catalogRefs_ 这些
//  映射记的都是"目录页里有什么"。回滚把目录页退回去了，这些映射
//  却还停在回滚之前——于是数据库会以为自己有一张磁盘上并不存在的表。
//  接下来每一次查询都会读到"没有数据页的表"，或者更糟，
//  读到页号指向已经失效的地方。
//
//  【为什么用"重新读一遍目录"而不是"把改动反向撤销一遍"】
//  因为反向撤销要给每一种语句都写一份逆操作（建表 <-> 删表、
//  改列名 <-> 改回来……），每一份都可能写错，而且加了新语句
//  就得记着再加一份。重新读目录只有一句"以磁盘为准"，
//  它**不可能和磁盘不一致**——因为它是从磁盘读出来的。
//  （代价是回滚慢一点：要把目录链扫一遍。目录通常只有一页。）
//
//  这也是整个项目里反复用的那一招：**能算出来的就别存，
//  对不上的时候以谁为准要事先说好。** 这里的答案是磁盘。
//
//  ------------------------------------------------------------
//  阶段 4：第二条目录链（索引）
//  ------------------------------------------------------------
//
//  索引也需要落盘，而且**必须**落盘：B+ 树在文件里，根页号
//  要是没记下来，下次打开就是一棵谁也找不到的树。
//
//   第 0 页（文件头）
//        | 偏移 20：目录链第一页        | 偏移 28：索引目录链第一页
//        v                              v
//   表目录页 -> ...                索引目录页 -> ...
//   （表名、列、第一个数据页）      （索引名、表名、列号、根页）
//
//  【为什么是两条链，不是一条】
//  两条链里的记录形状完全不同，而记录格式里**没有"这条是什么"
//  的判别字节**。挤在一条链上的话，要么给每条记录加一个判别字节
//  （等于改已有记录的格式），要么让读的人靠猜。
//  分开之后，各自的记录格式就是自己的事。
//  （代价是打开库要扫两遍。目录通常只有一页，这个代价是零。）
// ============================================================

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "btree.hpp"
#include "pager.hpp"
#include "schema.hpp"
#include "table.hpp"

class Database {
public:
    // path 为空 = 内存库，进程一退就没了。
    // 文件不存在就新建；存在就当 Acorn 数据库打开，
    // 不是的话抛 AcornError（**绝不会**覆盖一个不认识的文件）。
    //
    // crash 是测试用的崩溃开关，正常跑永远用默认值。见 pager.hpp。
    //
    // lockWaitMs 是抢不到锁时愿意等多久，0 = 立刻报错（阶段 6）。
    explicit Database(const std::string& path,
                      CrashPoint crash = CrashPoint::NONE, int lockWaitMs = 0);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool isPersistent() const { return pager_->pageCount() > 0 && !pager_->inMemory(); }
    bool inMemory() const { return pager_->inMemory(); }
    const std::string& path() const { return pager_->path(); }

    // 这个进程现在拿着这个库的什么锁（阶段 6）。给 .info 用。
    std::string lockState() const { return pager_->lockState(); }

    // 把改动写到磁盘上。
    //
    // 【阶段 3 里它是什么语义】调用方（main）在**每条语句成功之后**
    // 调一次，所以效果是"自动提交"——每条语句一执行完就落盘。
    // 这是 SQLite 的默认行为，也意味着阶段 3 里**不存在
    // "一半的改动"**：语句要么整条跑完了才落盘，要么抛异常根本没落。
    //
    // 但它**不是事务**：一条 UPDATE 改到第 3 行报错时，前 2 行已经
    // 改在内存里了，而且下一次 flush 会把它们写出去。
    // 回滚是阶段 5 的事——那时候会先写日志再改页，出事就照日志退回。
    void flush();

    // ------------------------------------------------------------
    //  事务（阶段 5）
    // ------------------------------------------------------------
    //
    // 一个事务是这样的：
    //
    //     BEGIN;                       <- begin()
    //     INSERT ...; UPDATE ...;      <- 这段里的改动都还没定
    //     COMMIT;                      <- commit()：一次性生效
    //     或者 ROLLBACK;               <- rollback()：全部撤销
    //
    // 【没写 BEGIN 的时候呢】每条语句自己就是一个事务——执行完自动提交，
    // 中途出错自动回滚。这叫自动提交（autocommit），是 SQL 的默认行为，
    // 也是阶段 3 那句"语句要么整条生效要么整个没生效"真正兑现的地方
    //（阶段 3 其实只做到了"抛异常的那条不提交"，改到一半的行会留下）。
    // 这件事由 main 里的语句循环负责，见 main.cpp。
    //
    // 【为什么不需要嵌套计数】`BEGIN` 在事务里再敲一次是会报错的
    //（标准 SQL 也没有嵌套事务）。所以事务状态就是一个 bool，
    // 不是深度。少一个"层数"就少一类"层数没配平"的 bug。
    bool inTransaction() const { return pager_->inTransaction(); }

    // 这个事务是不是"某条语句要改数据、于是自己开的"（自动提交）。
    // main 靠它决定语句跑完之后要不要替用户提交。见 pager.hpp。
    bool transactionIsImplicit() const { return pager_->transactionIsImplicit(); }

    void begin();
    void commit();

    // 撤销这个事务里所有的改动。
    //
    // 【它做两件事】页退回磁盘上原来的样子（Pager 管），
    // 内存里那份表/索引的映射重新从目录读一遍（这一层管）。
    // 两件都做完，才算真的回到事务开始前——只做前一件的话，
    // 数据库会以为有些表存在，而磁盘上它们已经没了。
    void rollback();

    bool exists(const std::string& tableName) const;

    // 下面三个找不到表时都抛 AcornError
    const TableSchema& schemaOf(const std::string& tableName) const;
    Table& table(const std::string& tableName);
    const Table& table(const std::string& tableName) const;

    // 建表。已经有同名的表就抛 AcornError。
    // 会立刻分配第一个数据页并往目录里写一条记录——
    // 也就是说**建完表就落盘**，不是等到插入第一行才落。
    void createTable(TableSchema schema);

    // 删表。把表占的所有数据页还给空闲链，目录记录标成已删。
    // 表不存在就抛 AcornError。
    //
    // 【它连带删掉这张表上的所有索引】
    // 索引的存在依赖表：表没了，索引里那些行号指向的就是
    // 已经还给空闲链的页——那些页随时会被别的表拿去用，
    // 索引里的行号会指向**别人的数据**。所以必须一起删。
    // （真数据库里 DROP TABLE 一定要显式写 CASCADE 才删依赖对象，
    //   是因为还有视图、外键这些东西；索引不算"独立对象"，
    //   它是表的一部分，所以各家都是直接一起删。）
    void dropTable(const std::string& tableName);

    // 所有表名，**按建表顺序**。
    std::vector<std::string> tableNames() const;

    // ------------------------------------------------------------
    //  索引（阶段 4）
    // ------------------------------------------------------------

    // 建索引。同名索引已存在、表不存在、列号越界都抛 AcornError。
    //
    // 【它会扫描整张表，把现有的行全灌进去】
    // 不灌的话，索引只对"建完之后新插的行"有效——那时候
    // `WHERE id = 1` 走索引查不到已经存在的那一行，**结果少了行**，
    // 而且不报错。真数据库建索引慢，就是因为这一趟全表扫。
    //
    // schema.columnType 由这里从表结构里补上（调用方不用填）。
    void createIndex(IndexSchema schema);

    // 删索引。把 B+ 树占的所有页还给空闲链，目录记录标成已删。
    // 不存在就抛 AcornError。
    void dropIndex(const std::string& indexName);

    // 所有索引名，**按建索引顺序**。
    std::vector<std::string> indexNames() const;

    // 这张表上的索引，按建索引顺序。表不存在就抛 AcornError。
    std::vector<IndexSchema> indexesOf(const std::string& tableName) const;

    // 找到某个索引的定义；没有就返回 nullptr。
    // （和 table() 不一样，这里**不抛异常**：查"有没有可用索引"
    //   是规划器天天要做的事，找不到是常态，不是错误。）
    const IndexSchema* findIndex(const std::string& indexName) const;

    // 按索引查一批行号。
    //
    // 【这是执行器能对索引做的唯一一件事】
    // 它拿不到 Btree 对象本身，也就不可能绕过"表 + 索引一起改"
    // 这条规矩去动树。边界划在这里，比划在"谁能拿到 Btree"上
    // 结实得多——后者靠自觉，前者靠编译器。
    //
    // 语义和 Btree::searchRange 一样：两头都可以缺，
    // 闭区间 / 开区间由 inclusive 决定。等值查找就是
    // "两头都是同一个值、两头都闭"。
    //
    // 【回调收到的是行号，不是行】回表是执行器的事。
    // 索引不该知道"表那一行长什么样"，那是 Table 的事。
    void searchIndex(const std::string& indexName, bool hasLower,
                     const Value& lower, bool lowerInclusive, bool hasUpper,
                     const Value& upper, bool upperInclusive,
                     const std::function<void(RowId)>& visit) const;

    // ------------------------------------------------------------
    //  写入口（表 + 索引，一次做完）
    // ------------------------------------------------------------
    //
    // ★ 执行器只该走这三个，不要自己去碰 Table ★
    //
    // 【为什么】因为一次写要同时落在**两个地方**：表的数据页，
    // 和这张表上所有索引的 B+ 树。分开做的话，任何一条路径
    // 漏掉索引维护，表现都是"索引和表对不上"——
    // 而那种坏是**不报错**的坏：查询静悄悄地少返回几行。
    //
    // 所以"维护索引"这件事被放在拥有这两样东西的人手里
    //（Database 同时管着表和索引），就像"校验行的类型"被放在
    // Table 里一样（见 table.hpp）。**不变式由拥有它的人来守。**
    void insertRow(const std::string& tableName, Row row);

    // 改一行。老的索引项要按**老值**删掉，新的按新值插进去——
    // 这也是 UPDATE 改索引列之后还能查得到的原因。
    void updateRow(const std::string& tableName, RowId id, Row row);

    // 删一批行。每一条的索引项都要跟着删。
    void deleteRows(const std::string& tableName, const std::vector<RowId>& ids);

    // 调试用：页缓存现在什么状况。
    std::string cacheStats() const { return pager_->cacheStats(); }

private:
    // 走一遍两条目录链，把"有哪些表、有哪些索引"读进内存。
    //
    // 【构造和回滚共用这一个函数】构造的时候是"第一次读"，
    // 回滚之后是"重新读一遍，把内存里那份过期的换掉"。
    // 两件事一模一样，写成两份的话，将来改目录格式就得记得改两处——
    // 而漏掉的那一处会在某一次回滚之后才发作。
    void loadCatalog();

    // 把内存里那份映射清空。回滚前调，调完再 loadCatalog()。
    void clearCatalog();

    // 拼"没有这张表"的报错。
    // 顺带把现有的表列出来——用户打错一个字，一眼就能看出来，
    // 不用自己去回忆建过哪些表。
    std::string noSuchTable(const std::string& tableName) const;

    // 一条目录记录在哪：哪一页、页内第几个字节。
    //
    // 【为什么要把这个记在内存里】因为 DROP TABLE 要回头把那条
    // 记录的第一个字节改成"已删"。不知道它在哪就只能重新扫一遍
    // 整条目录链去找——能work，但每删一张表就把整个目录读一遍，
    // 而这个信息在我们自己写它的时候**本来就知道**。
    // 顺手记下来，比事后找便宜。
    struct CatalogRef {
        uint32_t pageNo = INVALID_PAGE;
        uint16_t offset = 0;
    };

    CatalogRef appendToCatalog(const std::string& record);

    // 往一条目录链上追加记录。表目录和索引目录共用这一份——
    // "往一条只增不减的页链上追加"这件事，和链里装的是什么毫无关系。
    // setFirst 负责把"链的第一页"写进文件头（两条链写的偏移不同）。
    CatalogRef appendToChain(const std::string& record, uint32_t& last,
                             void (*setFirst)(char*, uint32_t));

    // 索引目录那条链上的同一件事。分开写是因为那两条链的
    // "链尾在哪"要各记一个（见文件头关于两条链的说明）。
    CatalogRef appendToIndexCatalog(const std::string& record);

    // 一个索引在内存里的全部状态。
    struct IndexInfo {
        IndexSchema schema;

        // B+ 树的根页。**它会变**——树长高一层时根就换了。
        // 换了必须写回目录记录，不然下次打开只剩半棵树。
        uint32_t rootPage = INVALID_PAGE;

        // 这条索引的目录记录在哪（根页变了要回去改它）。
        CatalogRef ref;
    };

    // 追加一条索引项，并在根页变了的时候把新根页写回目录。
    //
    // 【为什么不把 Btree 对象存成成员】
    // 因为 Btree 里只有 (pager, 根页号, 键类型) 三样东西，
    // 它是个轻量的句柄，真正的状态在页里（也就是在缓存里）。
    // 存成成员的话，"根页变了"就要同时改两处（Btree 里那个，
    // 和目录记录），于是有了一份可能对不上的状态。
    // 每次现造一个，状态就只有一份。
    void insertIndexEntry(IndexInfo& info, const Value& key, RowId id);
    void removeIndexEntry(IndexInfo& info, const Value& key, RowId id);

    // 真正干活的 dropIndex：调用方保证 indexKey 存在。
    void dropIndexByKey(const std::string& indexKey);

    // 索引里能不能放下这个键（编码之后的长度）。
    // createIndex 用它**先把所有行量一遍**，见那边的说明。
    static void checkIndexKey(const Value& key, const IndexSchema& schema);

    std::unique_ptr<Pager> pager_;

    // key 是**小写**表名。见 schema.hpp 里 name / key 两份的说明。
    std::unordered_map<std::string, Table> tables_;

    // 建表顺序，单独记一份。
    //
    // unordered_map 的遍历顺序是**未指定**的，换个编译器、加一张表、
    // 甚至同一份代码跑两次都可能不一样。要是 .tables 命令直接遍历
    // 那个 map，输出顺序就是随机的，测试根本没法写。
    // 顺序在用户眼里是有意义的（"我刚才建的那张表呢"），
    // 所以老老实实再存一个 vector。
    std::vector<std::string> order_;

    std::unordered_map<std::string, CatalogRef> catalogRefs_;

    // 目录链的最后一页，追加记录时直接往它上面写。
    // 打开库的时候走一遍链算出来。
    uint32_t catalogLast_ = INVALID_PAGE;

    // ---- 索引（阶段 4）----
    // key 一律是**小写索引名**，和 tables_ 用表名的小写是同一个规矩。
    std::unordered_map<std::string, IndexInfo> indexes_;

    // 建索引顺序。理由和 order_ 一模一样：unordered_map 的遍历
    // 顺序是未指定的，直接遍历它打印，输出顺序就是随机的。
    std::vector<std::string> indexOrder_;

    // 表名（小写）-> 这张表上的索引名（小写），按建索引顺序。
    //
    // 【为什么另记这一份】因为"这张表上有哪些索引"是每次写行
    // 都要问一次的。不记的话就得把 indexes_ 整个遍历一遍——
    // 索引不多的时候没什么，但它是**每次插入**都要做的事。
    // 这是"用空间换掉一个每次都要付的代价"，不是缓存正确性。
    std::unordered_map<std::string, std::vector<std::string>> indexesByTable_;

    uint32_t indexCatalogLast_ = INVALID_PAGE;
};

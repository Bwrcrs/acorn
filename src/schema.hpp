#pragma once

// ============================================================
//  Schema —— 表结构（有哪些列、每列什么类型）
//
//  数据库和"一堆 JSON 文件"最本质的区别，就在这个文件里：
//  数据的形状是**先声明好**的，往里写东西要过检查。
//
//  JSON 随便塞，`{"age": "三十"}` 也能存进去，等你真读到那一行
//  才发现不对——那时候已经离出错的地方很远了。
//  而建表时声明了 INT，之后每次插入都被挡一道，错误当场就爆。
//  这就是"schema"这个词的全部份量。
//
//  【name 和 key 为什么要存两份】
//  SQL 里表名、列名大小写不敏感，`Users` 和 `users` 是同一张表。
//  但显示的时候要显示用户当初写的样子（`Users`）。
//  于是每个名字存两份：
//    · name —— 用户写的原样，打印用
//    · key  —— 转小写，比较用
//
//  查表、判重名一律用 key，打印一律用 name。
//  听着啰嗦，但这是唯一能同时把两件事都做对的办法——
//  只存一份的话，要么显示变成全小写（用户会以为自己的名字被改了），
//  要么比较变成大小写敏感（`SELECT * FROM Users` 找不到 `users` 表）。
// ============================================================

#include <string>
#include <vector>

#include "value.hpp"

struct Column {
    std::string name;  // 用户写的原样：Users
    std::string key;   // 转小写：users。比较只用它
    Value::Type type;
};

// 造一个 Column，key 自动算好。
//
// 提供这个函数而不是让调用方自己填 key，是因为"忘了填 key"
// 是个**不会报错**的 bug：key 是空串，然后所有按名查列都查不到，
// 表现是"明明建了这张表却说没有"。这种错最难查，
// 所以从接口上让它不可能发生。
Column makeColumn(std::string name, Value::Type type);

struct TableSchema {
    std::string name;
    std::string key;

    // 列的顺序就是建表时写的顺序，SELECT * 按这个顺序输出。
    // 用 vector 而不是 unordered_map，就是因为**顺序有意义**——
    // 哈希表会把顺序打乱，每次查询列的顺序都不一样，没法用。
    std::vector<Column> columns;

    // 按列名找下标，找不到返回 -1。
    //
    // 返回 -1 而不是抛异常，是因为"没找到"和"找到了"是两件事，
    // 让调用方自己决定怎么处理——查 WHERE 里的列名要报错，
    // 但别的地方可能只是想知道有没有。
    // 列名的大小写在这里抹平。
    int indexOf(const std::string& columnName) const;

    // "id, name, score" —— 报错和显示表结构时用。
    //
    // 存在的理由全在报错上：用户把 `name` 打成 `nmae`，
    // 你回一句"没有这一列"，他还得翻回建表语句去对；
    // 你把现有列列出来，他一眼就看见自己少打了一个字母。
    // 这种小事决定了工具好不好用。
    std::string columnList() const;
};

// 造一个 TableSchema，name 和 key 都填好。理由同 makeColumn。
TableSchema makeTableSchema(std::string name, std::vector<Column> columns);

// ------------------------------------------------------------
//  IndexSchema —— 一个索引长什么样
// ------------------------------------------------------------
//
// 阶段 4 的索引很简单：**一张表的一列**。就叫它"单列索引"。
//
// 【为什么列号是 int 而不是列名】
// 见 page.hpp 里索引记录那段：列号是这张表内部的说法，
// 改列名不该动到索引。而且列号定长 2 字节，列名不是。
//
// 【表名为什么也要存一份（明明有 tableKey 就够查了）】
// 报错和显示的时候要用**用户当初写的样子**（`CREATE INDEX i ON Users`
// 里的 `Users`），而 tableKey 是全小写的。这和 TableSchema 存
// name / key 两份是同一个理由。
struct IndexSchema {
    std::string name;       // 索引名，用户写的原样
    std::string key;        // 小写，比较用
    std::string tableName;  // 建在哪张表上（原样）
    std::string tableKey;   // 小写
    int column = -1;        // 在这张表的第几列上（0 开始）

    // 索引的键是什么类型——就是那一列的类型。
    // B+ 树要它来判断"这个键和那个键能不能比"。
    // 这里存的是建索引**当时**那一列的类型。
    Value::Type columnType = Value::Type::NULL_VALUE;
};

IndexSchema makeIndexSchema(std::string name, std::string tableName,
                            std::string tableKey, int column,
                            Value::Type columnType);

// "索引 名字 建在 表名(列名)" —— 报错和 .indexes 显示用。
std::string describeIndex(const IndexSchema& schema,
                          const TableSchema& table);

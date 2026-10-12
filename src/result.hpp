#pragma once

// ============================================================
//  ResultSet —— 一条语句执行完之后交给用户看的东西
//
//  为什么非要有这么个结构，直接打印不行吗？
//  因为"执行"和"显示"是两件事，混在一起以后会很麻烦：
//    · 测试要能拿到结果去比对，不能只能看屏幕
//    · 阶段 2 加 ORDER BY / LIMIT 时，要对结果再做一轮加工
//    · 以后做 JOIN，多条语句的结果可能要拼在一起
//  分开之后，执行器只管把数据摆好，怎么显示交给 main。
//
//  所以 SELECT 和 INSERT 返回的是同一种东西，只是 isQuery 不同：
//  前者有一张表要画，后者只有一句"插了 3 行"。
// ============================================================

#include <string>
#include <vector>

#include "value.hpp"

struct ResultSet {
    // 是不是一条查询（SELECT）。
    // CREATE / INSERT 这些没有结果表，只有一句反馈话术。
    bool isQuery = false;

    // 表头。SELECT * 的时候，这几列的名字来自表结构。
    std::vector<std::string> columnNames;

    // 数据。每一行的长度必然和 columnNames 一样——
    // 这个不变式由执行器保证，画表格的地方可以放心直接用。
    std::vector<std::vector<Value>> rows;

    // 非查询语句的反馈，例如 "已创建表 users（3 列）"。
    std::string message;
};

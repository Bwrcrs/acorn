#pragma once

// ============================================================
//  Value —— Acorn 里的一个"值"
//
//  一行数据就是若干 Value 排在一起。SQL 的值系统比 Nova 复杂，
//  有两个东西是 Nova 完全不需要操心的：
//
//  【一】整数和浮点数必须分开。
//  Nova 里所有数字底层都是 double，图的是省事。数据库不行：
//    · 建表时写了 INT，这一列就不该存得下 1.5
//    · 后面做页式存储，INT 占 4 字节、REAL 占 8 字节，
//      一条记录占多少空间必须是算得出来的
//    · 排序时整数和浮点数的比较规则不同
//  所以这里 long long 和 double 是两个独立的分支。
//
//  【二】有 NULL。
//  NULL 不是"空字符串"、也不是 0，它的意思是"不知道"。
//  因为"不知道"，`NULL = NULL` 的结果也是 NULL 而不是 true ——
//  这是 SQL 最反直觉的一条规则，也是新手写 WHERE 最常翻车的地方。
//  细节见 value.cpp 里的三值逻辑那段。
//
//  【设计取舍】和 Nova 一样用"标签 + 字段"，不用 std::variant。
//  理由也一模一样：std::visit 漏写一个分支会喷四十行模板内部报错，
//  而这里漏写一种类型，编译器的 -Wswitch 会直接点名是哪个。
// ============================================================

#include <cstdint>
#include <string>

struct Value {
    // 注意这里没有叫 NULL 的枚举值。
    // NULL 在 C 标准库里是个宏（<cstddef> 里 #define NULL ...），
    // 宏是纯文本替换，会把 `Type::NULL` 悄悄变成 `Type::0`，编译不过。
    // Nova 的 TRUE / FALSE 就栽在这上面（见 nova/src/token.hpp），
    // 这里提前避开，叫 NULL_VALUE。
    enum class Type {
        NULL_VALUE,  // SQL 的 NULL —— "不知道"
        INTEGER,     // 整数，底层是 long long
        REAL,        // 浮点数，底层是 double
        TEXT,        // 字符串
        BOOLEAN,     // true / false
    };

    // 当前装的是哪种。默认 NULL——这也是 SQL 里"没写"的默认值。
    Type type = Type::NULL_VALUE;

    // 下面几个字段"谁用谁填"，具体哪个有效完全由 type 决定。
    long long integer = 0;
    double real = 0.0;
    std::string text;
    bool boolean = false;

    // ---- 构造 ----
    // 用静态函数而不是重载构造函数，调用点一眼能看懂造的是什么：
    // `Value(1)` 看不出是整数还是浮点，`Value::makeInt(1)` 不会歧义。
    static Value makeNull();
    static Value makeInt(long long value);
    static Value makeReal(double value);
    static Value makeText(std::string value);
    static Value makeBool(bool value);

    bool isNull() const { return type == Type::NULL_VALUE; }

    // 这个值是否"数值型"（整数或浮点数）。
    // 比较和算术要先问这个：整数和浮点数是可以互相比的，
    // 但数值和文本不行。
    bool isNumeric() const { return type == Type::INTEGER || type == Type::REAL; }

    // 把值转成 double。只有数值型调用它才有意义。
    // 单独抽出来，是因为"整数和浮点数怎么比"这件事
    // 在好几个地方都要做（比较、排序、算术），逻辑只能有一份。
    double toDouble() const;

    // 打印成用户看到的样子。SELECT 的输出用它。
    std::string toString() const;

    // 打印成**能再读回去**的 SQL 字面量。
    //
    // 和 toString() 的区别就一处：文本值带不带引号。
    //
    //     文本 abc      文本 it's
    //     toString()     -> abc          it's
    //     toSqlLiteral() -> 'abc'        'it''s'
    //
    // 为什么要两个：给人看的输出（SELECT 的结果表）不该出现引号，
    // 那只是噪音；但语法树打印、以及以后做导出的时候，
    // 得让人一眼分出"值 123"和"文本 \"123\""——
    // 不加引号这两个长得一模一样。
    //
    // 里面那个 `it's` 变成 `it''s` 就是 SQL 的转义规则：
    // 字符串里的单引号连写两个。读回去还是 it's。
    std::string toSqlLiteral() const;

    // 类型的中文名，只给报错用："整数" "文本" ...
    std::string typeName() const;
};

// Value::Type 对应的 SQL 类型名：INTEGER -> "INT"。
//
// 值的类型和列的类型本来就是同一套枚举（见 ast.hpp 的 ColumnDef），
// 所以这个映射单独抽出来一份，打印建表语句、显示表结构都要用。
// 写两份的话，以后加一种类型必然漏掉一份。
const char* sqlTypeName(Value::Type type);

// 三路比较：a < b 返回负数，a == b 返回 0，a > b 返回正数。
//
// 【前提】两个都不是 NULL。
// "和 NULL 比较"的结果本身是个 NULL，那属于三值逻辑，
// 必须由调用方先判断——不能混进这个函数里，否则返回值就没法
// 既表示"小于"又表示"不知道"了。这个函数只管
// "两个都确定知道的值怎么比"。
//
// 类型不兼容（比如拿整数和文本比）时抛 AcornError，
// 不做隐式转换。这条和 Nova 里 `1 + "a"` 直接报错是同一个立场：
// 静悄悄地转过去，错误会跑到离现场很远的地方才发作。
int compareValues(const Value& a, const Value& b);

// 带 NULL 的三路比较，给**排序**和**索引**用。
//
// 和 compareValues 的区别只有一处：它管 NULL。
// 规则是 **NULL 最小**（跟着 SQLite 走；Postgres 默认反过来）。
//
// 【为什么这个函数必须只有一份实现】
// 因为"谁在前谁在后"这件事一旦有两套说法，就会出现
// "ORDER BY 走索引时顺序和全表扫时不一样"这种鬼故事——
// 而且它只在数据量大到真用上索引时才出现。
// 同一个规则有两处实现，它们早晚会分叉；分叉在排序上
// 就是"同一个查询两次跑出不同的顺序"。
//
// 【前提】除了 NULL 之外，两个值必须能比（类型不兼容会抛）。
// "和 NULL 比较"的三值逻辑（`= NULL` 恒为 UNKNOWN）是**另一回事**，
// 不在这里处理——那是 WHERE 判断真假的事，不是排序的事。
int compareForSort(const Value& a, const Value& b);

// 把一个值调整成"能装进某一列"的样子，装不进就抛 AcornError。
//
// 规则（只有一条允许转换，其余一律拒绝）：
//
//   值的类型      列是 INT   列是 REAL   列是 TEXT   列是 BOOL
//   ----------   ---------   ---------   ---------   ---------
//   NULL          可以        可以        可以        可以
//   INT           可以        可以（加宽） 不行        不行
//   REAL          不行        可以        不行        不行
//   TEXT          不行        不行        可以        不行
//   BOOL          不行        不行        不行        可以
//
// 唯一那条允许的转换是 INT -> REAL，因为它是**无损**的：
// 3 变成 3.0 之后还能变回来，信息一点没丢。
// 反方向的 REAL -> INT 就丢信息了（3.7 该变成 3 还是 4？），
// 所以直接报错，让用户自己写清楚。
//
// 这和 Nova 里 `1 + "a"` 直接报错是同一个立场：不许隐式转换。
// 区别在于 Nova 是"一律不许"，这里是"只许无损的那一种"——
// 数据库要是连 INT 插进 REAL 列都报错，用起来会让人想砸键盘。
//
// columnName 只用来拼报错信息，让人知道是哪一列出的问题。
Value fitToType(const Value& value, Value::Type columnType,
                const std::string& columnName);

// 把浮点数打印成用户想看到的样子：3.0 打成 "3" 而不是 "3.000000"。
// （std::to_string(double) 是固定 6 位小数，直接用会得到 "3.000000"）
std::string formatNumber(double value);

#pragma once

// ============================================================
//  Token —— SQL 的一个词法单元
//
//  和 Nova 的 Token 是同一套思路：词法分析器把 SQL 文本切成一串
//  Token，语法分析器只认 Token、不认原始字符。
//
//  但 SQL 有两个自己的特点，写词法分析器时要专门处理：
//
//  【一】`=` 是"相等"，不是"赋值"。
//  C / Nova 里 `=` 是赋值、`==` 才是判断。SQL 反过来：
//  `WHERE id = 1` 里的 `=` 就是判断相等，SQL 里没有赋值这个动作
//  （改数据要用 UPDATE ... SET）。所以这里只有一个 EQUAL，
//  不需要像 Nova 那样分 EQUAL 和 EQUAL_EQUAL 两个。
//
//  【二】`!=` 和 `<>` 是完全同义的。
//  写哪个都行，历史遗留。标准里推荐 `<>`，但几乎所有人都写 `!=`。
//  两者都扫成同一个 TokenKind::NOT_EQUAL，后面的阶段就不必再管这件事。
//
//  另外：SQL 的关键字是**大小写不敏感**的，`select` 和 `SELECT`
//  是一回事。这件事在词法分析器里就处理掉（统一转成大写再查表），
//  不要留给后面的阶段，否则每一层都要记得做一次。
// ============================================================

#include <string>

// Token 的种类。
// 用 enum class 而不是普通 enum：普通 enum 的名字会泄漏到外层作用域，
// 有个 STAR 之后别处就不能再叫 STAR 了。enum class 强制写
// TokenKind::STAR，不会撞名。
enum class TokenKind {
    // ---- 标点 ----
    LEFT_PAREN,   // (
    RIGHT_PAREN,  // )
    COMMA,        // ,
    SEMICOLON,    // ;
    STAR,         // *      —— SELECT * 的通配符，或乘号
    DOT,          // .      —— 表名.列名，阶段 2 做 JOIN 时用

    // ---- 算术运算符 ----
    // 注意 MINUS 同时是"减号"和"负号"，PLUS 同时是"加号"和"正号"。
    // 词法分析器不管这个区别——单个字符就是单个 Token。
    // 是二元还是二元，取决于它出现在哪儿，那是语法分析器的事：
    //   `a - b` 里它在两个表达式中间，是减法
    //   `-b`   里它在表达式开头，是取负
    PLUS,         // +
    MINUS,        // -
    SLASH,        // /
    PERCENT,      // %

    // ---- 比较运算符 ----
    EQUAL,        // =      —— SQL 里这是"相等"，不是赋值
    NOT_EQUAL,    // != 或 <>，两者同义，扫成同一个
    LESS,         // <
    LESS_EQUAL,   // <=
    GREATER,      // >
    GREATER_EQUAL,// >=

    // ---- 字面量的载体 ----
    IDENTIFIER,   // 表名、列名
    INTEGER,      // 42
    REAL,         // 3.14
    STRING,       // 'abc'   —— 注意 SQL 用单引号，不是双引号

    // ---- 关键字 ----
    // 关键字就是"被语言预定了的标识符"。扫描器先按标识符扫出来，
    // 再查表看是不是关键字。
    //
    // 类型关键字叫 INT_TYPE / TEXT_TYPE 而不是 INT / TEXT，
    // 是为了让读代码的人一眼分清"这是 Token 种类"还是"这是某个变量"。
    // （Nova 里 TRUE / FALSE 撞上了 <windows.h> 的宏，被迫改名 TRUE_KW，
    //  这里主动加上后缀，同样的问题不会再犯。）
    CREATE, TABLE, INSERT, INTO, VALUES, SELECT, FROM, WHERE,
    AND, OR, NOT,
    NULL_KW, TRUE_KW, FALSE_KW,
    INT_TYPE, REAL_TYPE, TEXT_TYPE, BOOL_TYPE,

    // ---- 阶段 2 加的 ----
    UPDATE, SET, DELETE, ORDER, BY, ASC, DESC, LIMIT, OFFSET,
    IS, AS,

    // ---- 阶段 3 加的 ----
    // DROP 变成关键字了，代价和 ORDER / AS 一样：叫 drop 的表
    // 从此建不出来。这笔账每加一个关键字都要重新算一遍。
    DROP,

    // ---- 阶段 4 加的 ----
    // CREATE INDEX 名 ON 表 (列);  /  DROP INDEX 名;
    // 又是两个保留字，代价同样记账：叫 index 或 on 的表/列
    // 从此建不出来。理由见 tokenizer.cpp 里那张表下面的说明。
    INDEX, ON,

    // ---- 阶段 5 加的 ----
    // BEGIN / COMMIT / ROLLBACK。三个都是整个语句，后面直接跟分号，
    // 不像前几个那样只是一个短语的开头。
    //
    // 【这三个的保留字代价比前几个小】因为它们都只在语句**开头**
    // 才有意义：`SELECT begin FROM t` 里那个 begin 完全可以当列名用，
    // 可惜我们的语法分析器没那么聪明（它看的是 Token 的种类，
    // 不是位置）。真数据库里 SQLite 也把 BEGIN 当保留字。
    // 这笔账照记：叫 begin / commit / rollback 的表和列从此建不出来。
    BEGIN, COMMIT, ROLLBACK,

    // ---- 聚合函数名【故意不在这儿】----
    // COUNT / SUM / AVG / MIN / MAX 不是关键字，是**普通标识符**。
    // 语法上它们的识别方式是"标识符后面紧跟一个左括号"。
    // 这么做是有意为之：如果把它们设成关键字，
    // 那你就再也不能有一个叫 `count` 的列了——而 `count` 是个
    // 再正常不过的列名（订单数量、访问次数）。
    // 真数据库也是这么处理的。
    //
    // 代价是写错函数名时，报错的人得是执行器（它才知道有哪些函数），
    // 而不是语法分析器。这个代价可以接受。

    END_OF_FILE  // 文件结束。有了它，语法分析器不用额外判断"还有没有内容"
};

struct Token {
    TokenKind kind;
    std::string lexeme;  // 原文。"select" / "'abc'" / "42"
                         // 注意保留原文的大小写，报错时要原样显示给用户看
    int line;            // 第几行，从 1 开始
    int column;          // 第几列，从 1 开始，按**字符数**不按字节数

    // 字面量携带的值，只有对应的那一种 Token 会填。
    // 这里和 Nova 的 Token 有一个不同：整数和浮点数分两个字段。
    // Nova 里所有数字都是 double，一个 number 字段就够了；
    // 数据库必须知道 `42` 是整数、`3.14` 是浮点数，
    // 因为往 INT 列里插 3.5 是要报错的。
    long long integerValue = 0;
    double realValue = 0.0;
    std::string textValue;  // STRING 用：去掉引号、转义已处理后的内容
};

// 把 SQL 文本统一成小写。
//
// SQL 里关键字、表名、列名**全部大小写不敏感**：`SELECT` 和 `select`
// 是一回事，`Users` 和 `users` 是同一张表。
//
// 所以"要比名字了"的地方，两边都先过一遍这个函数再比。
// 这件事必须只有一个实现——散在好几处的话，早晚会有一处漏掉，
// 表现出来就是"明明建了表却说表不存在"，而且只在特定大小写下才犯，
// 极难查。
//
// 只动 ASCII 的 A-Z。中文没有大小写，字节原样保留。
std::string lowercase(const std::string& text);

// 把种类转成可读字符串，给 --tokens 打印用。
const char* tokenKindName(TokenKind kind);

// 把一个 Token 打印成一行，给 --tokens 用。
std::string tokenToString(const Token& token);

// 这个 TokenKind 是不是一个类型名（INT / REAL / TEXT / BOOL）。
// 建表解析列定义时要用，单独问一句比在那边写一长串 || 清楚。
bool isTypeKeyword(TokenKind kind);

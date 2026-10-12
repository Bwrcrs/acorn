#pragma once

// ============================================================
//  Parser —— 语法分析器（递归下降）
//
//  输入 Token 流，输出语法树。和 Nova 一样：语法规则有几层，
//  就写几个函数，一层调一层，层数越深优先级越高。
//  **优先级完全由调用层次表达，一个数字都不用比较。**
//
//  语法（EBNF 记法，{} 表示重复，[] 表示可选）：
//
//    program     := statement { statement } EOF
//
//    statement   := createStmt | insertStmt | selectStmt
//                 | updateStmt | deleteStmt | dropStmt
//                 | beginStmt | commitStmt | rollbackStmt
//
//    beginStmt   := "BEGIN" ";"
//    commitStmt  := "COMMIT" ";"
//    rollbackStmt:= "ROLLBACK" ";"
//
//    createStmt  := "CREATE" "TABLE" name "(" columnDef {"," columnDef} ")" ";"
//    columnDef   := name type
//    type        := "INT" | "REAL" | "TEXT" | "BOOL"
//
//    insertStmt  := "INSERT" "INTO" name "VALUES" row {"," row} ";"
//    row         := "(" expr {"," expr} ")"
//
//    selectStmt  := "SELECT" ( "*" | expr {"," expr} ) "FROM" name
//                   [ "WHERE" expr ]
//                   [ "ORDER" "BY" orderItem {"," orderItem} ]
//                   [ "LIMIT" integer [ "OFFSET" integer ] ] ";"
//    orderItem   := expr [ "ASC" | "DESC" ]
//
//    updateStmt  := "UPDATE" name "SET" assignment {"," assignment}
//                   [ "WHERE" expr ] ";"
//    assignment  := name "=" expr
//
//    deleteStmt  := "DELETE" "FROM" name [ "WHERE" expr ] ";"
//
//  表达式按优先级从低到高，一层一个函数。**优先级完全由调用层次
//  表达，一个数字都不用比较**——这是递归下降最漂亮的地方，
//  也是它比"打一张优先级表然后按数字调度"更好读的原因：
//  你只要顺着函数调用往上看，就知道谁先算。
//
//    expression  := orExpr
//    orExpr      := andExpr { "OR" andExpr }
//    andExpr     := notExpr { "AND" notExpr }
//    notExpr     := "NOT" notExpr | comparison
//    comparison  := additive [ compOp additive ] [ "IS" ["NOT"] "NULL" ]
//    additive    := multiplicative { ("+" | "-") multiplicative }
//    multiplicative := unary { ("*" | "/" | "%") unary }
//    unary       := "-" unary | primary
//    primary     := literal | name | name "(" [ "*" | expression ] ")"
//                 | "(" expression ")"
//    compOp      := "=" | "!=" | "<>" | "<" | "<=" | ">" | ">="
//
//  几个值得留意的点：
//
//  · NOT 的层次比比较**低**。所以 `NOT x = 1` 解析成 `NOT (x = 1)`，
//    而不是 `(NOT x) = 1`——后者在 SQL 里讲不通。
//    这就是为什么 notExpr 在 comparison 外面一层。
//
//  · comparison 只允许比一次，不允许连写。
//    `a = b = c` 在 SQL 里是非法的，不像 C 那样有结合性。
//    这里主动检测并报错，比让它悄悄算出一个怪结果强。
//
//  · 算术的优先级比比较**高**。所以 `score + 10 > 80` 是
//    `(score + 10) > 80`，符合数学直觉，也符合所有数据库的做法。
//
//  · 一元负号从 primary 挪到了 unary 这一层（阶段 2 改的）。
//    阶段 1 它待在 primary 里，那样 `-x * 2` 会解析成 `-(x * 2)`——
//    因为负号先吃掉了整个 primary，而 primary 里没有乘法，
//    整个表达式会被拆错。挪出来之后，`-x * 2` 正确地解析成 `(-x) * 2`。
//    "加新运算符时，旧运算符该不该换层"是改语法时最容易漏掉的事。
//
//  · `*` 有两种身份，靠**位置**区分，不靠词法：
//      SELECT *        —— primary 期待一个值的开头，`*` 只能是通配符
//      COUNT(*)        —— 同理，括号里期待一个表达式
//      a * b           —— 已经在 multiplicative 里，是乘号
//    三种情况的上下文互斥，所以不需要任何回溯。
// ============================================================

#include <string>
#include <vector>

#include "ast.hpp"
#include "token.hpp"

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    // 一路解析到 END_OF_FILE，返回所有语句。
    std::vector<StmtPtr> parseProgram();

private:
    // ---- 游标操作 ----
    const Token& peek() const;
    const Token& advance();
    bool check(TokenKind kind) const;
    bool match(TokenKind kind);  // 是这种就吃掉并返回 true
    const Token& consume(TokenKind kind, const std::string& message);

    // 要一个"名字"（表名 / 列名）。
    // 单独抽出来是因为这里有个 SQL 特有的麻烦：关键字不能直接当名字用。
    // 用 consume(IDENTIFIER) 的话，用户写 `CREATE TABLE order (...)` 会
    // 收到一句干巴巴的"这里应该是标识符"，完全不知道问题出在哪。
    const Token& consumeName(const std::string& what);

    // ---- 各条语法规则 ----
    StmtPtr statement();
    StmtPtr createStatement();
    StmtPtr insertStatement();
    StmtPtr selectStatement();
    StmtPtr updateStatement();
    StmtPtr deleteStatement();
    StmtPtr dropStatement();

    // BEGIN / COMMIT / ROLLBACK。
    //
    // 【为什么合成一个函数】它们唯一的差别是开头那个关键字，
    // 后面都是"跟一个分号"。写三个几乎一样的函数，
    // 读的人还得逐一确认"这三个真的只差一个词吗"。
    // 用传进来的关键字决定造哪个节点，一眼就看完。
    StmtPtr transactionStatement();

    // CREATE 和 DROP 各自分出来的第二个分支。
    // 单独放两个函数而不是塞进上面那两行里，是因为它们各有
    // 十几行——挤在一起的话，createStatement 里最重要的一件事
    //（表名和列定义怎么解析）会被淹掉。
    StmtPtr createIndexStatement();
    StmtPtr dropIndexStatement();

    ColumnDef columnDef();
    Value::Type typeName();

    // ORDER BY 的一项
    OrderItem orderItem();

    // LIMIT / OFFSET 后面那个数。
    // 单独抽出来是因为这里有个必须拦住的坑：`LIMIT -1`。
    // 语法上 MINUS 是个合法 Token，直接读 integerValue 会得到
    // 一个看起来正常的数，但语义上"限制 -1 行"毫无意义。
    // 在语法分析阶段就报错，比让执行器去处理负数清楚。
    long long limitCount(const char* what);

    // ---- 表达式，按优先级从低到高 ----
    ExprPtr expression();
    ExprPtr orExpression();
    ExprPtr andExpression();
    ExprPtr notExpression();
    ExprPtr comparison();
    ExprPtr additive();
    ExprPtr multiplicative();
    ExprPtr unary();
    ExprPtr primary();

    // 函数调用。进到这里时，名字已经吃掉、下一个 Token 一定是左括号。
    ExprPtr callExpression(const Token& nameToken);

    // comparison 要用：这个 Token 是比较运算符吗？是的话返回对应的 BinaryOp。
    // 返回 bool 而不是指针，是为了避免在比较运算符的枚举里再加一个
    // "不是比较运算符"的假值——那种假值早晚会被当成真的用出去。
    static bool asComparisonOp(TokenKind kind, BinaryOp& out);

    std::vector<Token> tokens_;
    size_t current_ = 0;  // 下一个待处理的 Token 下标
};

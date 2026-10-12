#pragma once

// ============================================================
//  AstPrinter —— 把语法树打成 s-表达式
//
//  这是**访问者模式的第二个使用者**，也是"那层 accept 抽象没白加"
//  最直接的证据：它和执行器毫无关系，却完整复用了同一棵语法树，
//  而且在 ast.hpp / ast.cpp 里一个字都不用改。
//
//  如果当初把 eval() 直接写在节点上，今天要加这个打印器就得
//  把所有节点类改一遍。
//
//  【一个必须说明的别扭之处】
//  ExprVisitor 的返回值被钉死成了 Value（因为执行器要算值），
//  但打印器只想要个字符串，只好用 Value::makeText() 把字符串包起来。
//
//  这是 C++ 虚函数没法"泛化返回类型"留下的疤。真正的解法是
//  模板访问者或者 std::variant，但那两样都会把代码难度抬高一截，
//  不该在第一阶段引入。这里选择**忍受一个看得见的别扭**，
//  而不是引入一个看不懂的机制。
//
//  输出长这样：
//
//      SELECT name FROM users WHERE score > 90;
//        -> (select (name) from users where (> score 90))
//
//      SELECT * FROM users;
//        -> (select * from users)
//
//      CREATE TABLE users (id INT, name TEXT);
//        -> (create-table users (id INT) (name TEXT))
//
//      INSERT INTO users VALUES (1, 'alice');
//        -> (insert users (1 'alice'))
//
//  括号在树里是看不见的，但它的作用留在了结构里——
//  `(> score 90)` 这个括号就是优先级留下的痕迹。
// ============================================================

#include <string>

#include "ast.hpp"

class AstPrinter : public ExprVisitor, public StmtVisitor {
public:
    // ---- ExprVisitor ----
    // 返回的是 Value::makeText(打印出来的字符串)
    Value visitLiteral(const LiteralExpr& expr) override;
    Value visitColumn(const ColumnExpr& expr) override;
    Value visitBinary(const BinaryExpr& expr) override;
    Value visitUnary(const UnaryExpr& expr) override;
    Value visitCall(const CallExpr& expr) override;

    // ---- StmtVisitor ----
    // 打印结果放在 ResultSet::message 里
    ResultSet visitCreateTable(const CreateTableStmt& stmt) override;
    ResultSet visitInsert(const InsertStmt& stmt) override;
    ResultSet visitSelect(const SelectStmt& stmt) override;
    ResultSet visitUpdate(const UpdateStmt& stmt) override;
    ResultSet visitDelete(const DeleteStmt& stmt) override;
    ResultSet visitDropTable(const DropTableStmt& stmt) override;
    ResultSet visitCreateIndex(const CreateIndexStmt& stmt) override;
    ResultSet visitDropIndex(const DropIndexStmt& stmt) override;
    ResultSet visitBegin(const BeginStmt& stmt) override;
    ResultSet visitCommit(const CommitStmt& stmt) override;
    ResultSet visitRollback(const RollbackStmt& stmt) override;

    // 外部入口：打印一条语句
    std::string printStmt(const Stmt& stmt);

    // 外部入口：打印一个表达式。
    // 执行器会用它给计算出来的列起名字——`SELECT id + 1` 的结果那一列
    // 没有天然的名字，用表达式的文本当名字最直观。
    std::string printExpr(const Expr& expr);

private:
    // 把访问者方法返回的 Value 拆回字符串。
    // 每个方法都要写一遍 `v.text`，抽出来省得写错。
    static std::string unwrap(const Value& value);
};

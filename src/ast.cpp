#include "ast.hpp"

// ============================================================
//  accept —— 访问者模式里最关键的三行样板
//
//  每个节点类都长这样：
//      Value XxxExpr::accept(ExprVisitor& visitor) const {
//          return visitor.visitXxx(*this);
//      }
//
//  看着像废话，"我调你、你调我"绕了一圈。但就是这一圈，
//  让"加一种新操作"从"改所有节点类"变成"加一个新访问者"。
//
//  代价是每个节点多这三行。节点有 4 种，一共 12 行——
//  换来的是一张执行器表、一张打印器表，将来还有类型检查器。
//  这笔账很划算，而且节点种类是稳定的、操作种类是会涨的。
//
//  【对比】如果直接在节点上写 `virtual Value eval()`，
//  返回类型就钉死成 Value 了，想再写个返回字符串的打印器
//  就得把所有节点类改一遍。ast_printer.cpp 的存在就是
//  "这层抽象没白加"的证据。
// ============================================================

// ------------------------------------------------------------
//  表达式
// ------------------------------------------------------------

Value LiteralExpr::accept(ExprVisitor& visitor) const {
    return visitor.visitLiteral(*this);
}

Value ColumnExpr::accept(ExprVisitor& visitor) const {
    return visitor.visitColumn(*this);
}

Value BinaryExpr::accept(ExprVisitor& visitor) const {
    return visitor.visitBinary(*this);
}

Value UnaryExpr::accept(ExprVisitor& visitor) const {
    return visitor.visitUnary(*this);
}

Value CallExpr::accept(ExprVisitor& visitor) const {
    return visitor.visitCall(*this);
}

// ------------------------------------------------------------
//  语句
// ------------------------------------------------------------

ResultSet CreateTableStmt::accept(StmtVisitor& visitor) const {
    return visitor.visitCreateTable(*this);
}

ResultSet InsertStmt::accept(StmtVisitor& visitor) const {
    return visitor.visitInsert(*this);
}

ResultSet SelectStmt::accept(StmtVisitor& visitor) const {
    return visitor.visitSelect(*this);
}

ResultSet UpdateStmt::accept(StmtVisitor& visitor) const {
    return visitor.visitUpdate(*this);
}

ResultSet DeleteStmt::accept(StmtVisitor& visitor) const {
    return visitor.visitDelete(*this);
}

ResultSet DropTableStmt::accept(StmtVisitor& visitor) const {
    return visitor.visitDropTable(*this);
}

ResultSet CreateIndexStmt::accept(StmtVisitor& visitor) const {
    return visitor.visitCreateIndex(*this);
}

ResultSet DropIndexStmt::accept(StmtVisitor& visitor) const {
    return visitor.visitDropIndex(*this);
}

ResultSet BeginStmt::accept(StmtVisitor& visitor) const {
    return visitor.visitBegin(*this);
}

ResultSet CommitStmt::accept(StmtVisitor& visitor) const {
    return visitor.visitCommit(*this);
}

ResultSet RollbackStmt::accept(StmtVisitor& visitor) const {
    return visitor.visitRollback(*this);
}

// ------------------------------------------------------------
//  运算符的名字
// ------------------------------------------------------------

const char* binaryOpName(BinaryOp op) {
    switch (op) {
        case BinaryOp::EQUAL:         return "=";
        case BinaryOp::NOT_EQUAL:     return "!=";
        case BinaryOp::LESS:          return "<";
        case BinaryOp::LESS_EQUAL:    return "<=";
        case BinaryOp::GREATER:       return ">";
        case BinaryOp::GREATER_EQUAL: return ">=";
        case BinaryOp::AND:           return "AND";
        case BinaryOp::OR:            return "OR";
        case BinaryOp::ADD:           return "+";
        case BinaryOp::SUBTRACT:      return "-";
        case BinaryOp::MULTIPLY:      return "*";
        case BinaryOp::DIVIDE:        return "/";
        case BinaryOp::MODULO:        return "%";
    }
    // 不写 default，漏一种 -Wswitch 就会在编译期喊出来
    return "?";
}

const char* unaryOpName(UnaryOp op) {
    switch (op) {
        case UnaryOp::NOT:         return "NOT";
        case UnaryOp::NEGATE:      return "-";
        case UnaryOp::IS_NULL:     return "IS NULL";
        case UnaryOp::IS_NOT_NULL: return "IS NOT NULL";
    }
    return "?";
}

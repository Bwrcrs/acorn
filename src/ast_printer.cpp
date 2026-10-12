#include "ast_printer.hpp"

#include "token.hpp"  // lowercase

#include <string>
#include <utility>

std::string AstPrinter::unwrap(const Value& value) {
    // 访问者方法的返回值约定就是"一个装着字符串的 TEXT 值"。
    // 万一不是，说明别处有 bug——直接取 .text 会得到一个空串，
    // 让人以为是打印逻辑坏了。所以这里主动断一下。
    return value.text;
}

// ------------------------------------------------------------
//  表达式
// ------------------------------------------------------------

Value AstPrinter::visitLiteral(const LiteralExpr& expr) {
    // 用 toSqlLiteral 而不是 toString：文本要带引号。
    // 不然 `INSERT INTO t VALUES ('123')` 打印出来是 (insert t (123))，
    // 和真的插了个数字 123 长得一模一样，看不出区别。
    return Value::makeText(expr.value.toSqlLiteral());
}

Value AstPrinter::visitColumn(const ColumnExpr& expr) {
    return Value::makeText(expr.name);
}

Value AstPrinter::visitBinary(const BinaryExpr& expr) {
    // 先递归打两边，再拼起来。
    // 深度优先遍历，和 Nova 里求值器的走法完全一样。
    std::string left = unwrap(expr.left->accept(*this));
    std::string right = unwrap(expr.right->accept(*this));

    return Value::makeText("(" + std::string(binaryOpName(expr.op)) + " " +
                           left + " " + right + ")");
}

Value AstPrinter::visitUnary(const UnaryExpr& expr) {
    std::string operand = unwrap(expr.operand->accept(*this));
    return Value::makeText("(" + std::string(unaryOpName(expr.op)) + " " +
                           operand + ")");
}

Value AstPrinter::visitCall(const CallExpr& expr) {
    // 函数名统一成小写再打印。
    // 打印器的作用是让人看清"解析出来的结构"，而 `COUNT(*)`
    // 和 `count(*)` 解析出来是同一个东西，打出来也该是同一个样子。
    // （注意这和列名不一样——列名保留用户写的大小写，
    //   因为那是在提醒他表结构里叫什么。）
    const std::string name = lowercase(expr.name);

    if (expr.star) {
        return Value::makeText("(" + name + " *)");
    }
    return Value::makeText("(" + name + " " +
                           unwrap(expr.argument->accept(*this)) + ")");
}

// ------------------------------------------------------------
//  语句
// ------------------------------------------------------------

ResultSet AstPrinter::visitCreateTable(const CreateTableStmt& stmt) {
    std::string out = "(create-table " + stmt.tableName;
    for (const ColumnDef& column : stmt.columns) {
        out += " (" + column.name + " " + sqlTypeName(column.type) + ")";
    }
    out += ")";

    ResultSet result;
    result.message = std::move(out);
    return result;
}

ResultSet AstPrinter::visitInsert(const InsertStmt& stmt) {
    std::string out = "(insert " + stmt.tableName;
    for (const std::vector<ExprPtr>& row : stmt.rows) {
        out += " (";
        for (size_t i = 0; i < row.size(); i++) {
            if (i > 0) out += " ";
            out += unwrap(row[i]->accept(*this));
        }
        out += ")";
    }
    out += ")";

    ResultSet result;
    result.message = std::move(out);
    return result;
}

ResultSet AstPrinter::visitSelect(const SelectStmt& stmt) {
    std::string out = "(select ";

    if (stmt.selectAll) {
        out += "*";
    } else {
        out += "(";
        for (size_t i = 0; i < stmt.columns.size(); i++) {
            if (i > 0) out += " ";
            const SelectItem& item = stmt.columns[i];
            // 有别名就打成 `(as 表达式 别名)`，没有就打表达式本身。
            // 别名包一层而不是平铺，是为了让"别名属于哪一项"没有歧义——
            // 平铺成 `(name (as (* score 2) 双倍))` 的话，
            // 一眼看不出 `双倍` 是修饰哪一项的。
            if (item.alias.empty()) {
                out += unwrap(item.expr->accept(*this));
            } else {
                out += "(as " + unwrap(item.expr->accept(*this)) + " " +
                       item.alias + ")";
            }
        }
        out += ")";
    }

    out += " from " + stmt.tableName;

    // where 是 nullptr 就表示没有 WHERE。这里必须判空——
    // 这正是"用空指针表达不存在"比"用恒真表达式表达"好的地方：
    // 忘了判空会直接崩，而用恒真表达式假装存在的话，
    // 忘了判空只是多走一遍没用的逻辑，bug 藏起来了。
    if (stmt.where) {
        out += " where " + unwrap(stmt.where->accept(*this));
    }

    // ORDER BY 和 LIMIT 没写的时候不打——
    // `(order-by)` 这种空壳只会让输出更难读。
    if (!stmt.orderBy.empty()) {
        out += " (order-by";
        for (const OrderItem& item : stmt.orderBy) {
            out += " (" + std::string(item.ascending ? "asc" : "desc") + " " +
                   unwrap(item.key->accept(*this)) + ")";
        }
        out += ")";
    }

    if (stmt.limit >= 0) {
        out += " (limit " + std::to_string(stmt.limit);
        if (stmt.offset > 0) {
            out += " offset " + std::to_string(stmt.offset);
        }
        out += ")";
    }

    out += ")";

    ResultSet result;
    result.message = std::move(out);
    return result;
}

ResultSet AstPrinter::visitUpdate(const UpdateStmt& stmt) {
    std::string out = "(update " + stmt.tableName + " (set";
    for (const Assignment& assignment : stmt.assignments) {
        out += " (= " + assignment.column + " " +
               unwrap(assignment.value->accept(*this)) + ")";
    }
    out += ")";

    if (stmt.where) {
        out += " where " + unwrap(stmt.where->accept(*this));
    }
    out += ")";

    ResultSet result;
    result.message = std::move(out);
    return result;
}

ResultSet AstPrinter::visitDropTable(const DropTableStmt& stmt) {
    ResultSet result;
    result.message = "(drop-table " + stmt.tableName + ")";
    return result;
}

ResultSet AstPrinter::visitDelete(const DeleteStmt& stmt) {
    std::string out = "(delete " + stmt.tableName;

    if (stmt.where) {
        out += " where " + unwrap(stmt.where->accept(*this));
    }
    out += ")";

    ResultSet result;
    result.message = std::move(out);
    return result;
}

ResultSet AstPrinter::visitCreateIndex(const CreateIndexStmt& stmt) {
    ResultSet result;
    result.message = "(create-index " + stmt.indexName + " on " +
                     stmt.tableName + " (" + stmt.columnName + "))";
    return result;
}

ResultSet AstPrinter::visitDropIndex(const DropIndexStmt& stmt) {
    ResultSet result;
    result.message = "(drop-index " + stmt.indexName + ")";
    return result;
}

// 三个事务语句没有参数，所以打出来就是光秃秃一个词。
// 括号还是留着——它是"这是个节点"的标志，和别的节点长得一致。
ResultSet AstPrinter::visitBegin(const BeginStmt& stmt) {
    ResultSet result;
    result.message = "(begin)";
    return result;
}

ResultSet AstPrinter::visitCommit(const CommitStmt& stmt) {
    ResultSet result;
    result.message = "(commit)";
    return result;
}

ResultSet AstPrinter::visitRollback(const RollbackStmt& stmt) {
    ResultSet result;
    result.message = "(rollback)";
    return result;
}

// ------------------------------------------------------------
//  外部入口
// ------------------------------------------------------------

std::string AstPrinter::printStmt(const Stmt& stmt) {
    return stmt.accept(*this).message;
}

std::string AstPrinter::printExpr(const Expr& expr) {
    return unwrap(expr.accept(*this));
}

#include "parser.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "error.hpp"

namespace {

// 报错时描述"读到了什么"。
// 单独抽出来是因为几乎所有报错信息都要带这一句，
// 而"读到文件末尾"和"读到某个 Token"的说法完全不一样——
// 前者说"语句已经结束了"比硬打一个空字符串清楚得多。
std::string describe(const Token& token) {
    if (token.kind == TokenKind::END_OF_FILE) return "语句已经结束了";
    return "读到的是 `" + token.lexeme + "`";
}

}  // namespace

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

// ------------------------------------------------------------
//  游标操作
// ------------------------------------------------------------

const Token& Parser::peek() const {
    // 不需要越界检查：Token 流末尾一定有一个 END_OF_FILE，
    // 而下面的 advance() 保证不会走过它。
    return tokens_[current_];
}

const Token& Parser::advance() {
    const Token& token = tokens_[current_];
    // 停在 END_OF_FILE 上不再往前。这样 peek() 永远不会越界，
    // 而且"已经读到头了"这件事会一直保持住。
    if (token.kind != TokenKind::END_OF_FILE) {
        current_++;
    }
    return token;
}

bool Parser::check(TokenKind kind) const {
    return peek().kind == kind;
}

bool Parser::match(TokenKind kind) {
    if (!check(kind)) return false;
    advance();
    return true;
}

const Token& Parser::consume(TokenKind kind, const std::string& message) {
    if (check(kind)) return advance();
    const Token& found = peek();
    throw SyntaxError(message + "，" + describe(found),
                      found.line, found.column);
}

const Token& Parser::consumeName(const std::string& what) {
    if (check(TokenKind::IDENTIFIER)) return advance();

    // ---- SQL 特有的麻烦：关键字不能直接当名字用 ----
    // `CREATE TABLE order (...)` 里的 order 很可能是用户想要的名字，
    // 但它已经被 SQL 预定了。如果这里只报一句"应该写标识符"，
    // 用户会盯着 `order` 这三个字母看半天，想不通哪里不像标识符。
    // 直接点破，他就知道要改个名字了。
    const Token& found = peek();
    if (found.kind != TokenKind::END_OF_FILE) {
        throw SyntaxError("这里要写" + what + "，但 `" + found.lexeme +
                              "` 是 SQL 关键字，不能当名字用，换一个吧",
                          found.line, found.column);
    }
    throw SyntaxError("这里要写" + what + "，" + describe(found),
                      found.line, found.column);
}

// ------------------------------------------------------------
//  程序
// ------------------------------------------------------------

std::vector<StmtPtr> Parser::parseProgram() {
    std::vector<StmtPtr> statements;

    while (!check(TokenKind::END_OF_FILE)) {
        // 空语句（多打的分号）直接跳过，不当错误。
        // 在 REPL 里连打两个分号是常事，为这个报错很烦人；
        // 而且语句末尾的分号本来就已经被各自的规则吃掉了，
        // 这里剩下来的分号只是多余，不是错。
        if (match(TokenKind::SEMICOLON)) continue;

        statements.push_back(statement());
    }
    return statements;
}

StmtPtr Parser::statement() {
    if (check(TokenKind::CREATE)) return createStatement();
    if (check(TokenKind::INSERT)) return insertStatement();
    if (check(TokenKind::SELECT)) return selectStatement();
    if (check(TokenKind::UPDATE)) return updateStatement();
    if (check(TokenKind::DELETE)) return deleteStatement();
    if (check(TokenKind::DROP)) return dropStatement();
    if (check(TokenKind::BEGIN) || check(TokenKind::COMMIT) ||
        check(TokenKind::ROLLBACK)) {
        return transactionStatement();
    }

    const Token& found = peek();
    throw SyntaxError(
        "语句要以 CREATE / INSERT / SELECT / UPDATE / DELETE / DROP / "
        "BEGIN / COMMIT / ROLLBACK 开头，" +
            describe(found),
        found.line, found.column);
}

// ------------------------------------------------------------
//  BEGIN / COMMIT / ROLLBACK
// ------------------------------------------------------------

StmtPtr Parser::transactionStatement() {
    const Token& keyword = advance();

    consume(TokenKind::SEMICOLON, "语句末尾要有分号");

    switch (keyword.kind) {
        case TokenKind::BEGIN:    return std::make_unique<BeginStmt>();
        case TokenKind::COMMIT:   return std::make_unique<CommitStmt>();
        case TokenKind::ROLLBACK: return std::make_unique<RollbackStmt>();
        default: break;
    }

    // 走到这儿说明调用方把别的 Token 喂进来了——那是程序写错了，
    // 不是用户写错了。
    throw AcornError("内部错误：transactionStatement 收到了 `" +
                     keyword.lexeme + "`");
}

// ------------------------------------------------------------
//  CREATE TABLE
// ------------------------------------------------------------

StmtPtr Parser::createStatement() {
    const Token& createToken = advance();  // CREATE，留着报错定位用

    // ---- 阶段 4：CREATE 后面现在有两种东西 ----
    if (check(TokenKind::INDEX)) return createIndexStatement();

    consume(TokenKind::TABLE, "CREATE 后面要跟 TABLE 或 INDEX");
    const Token& nameToken = consumeName("表名");
    std::string tableName = nameToken.lexeme;

    consume(TokenKind::LEFT_PAREN, "表名后面要跟一对括号，里面写列的定义");

    std::vector<ColumnDef> columns;
    if (!check(TokenKind::RIGHT_PAREN)) {
        do {
            columns.push_back(columnDef());
        } while (match(TokenKind::COMMA));
    }
    consume(TokenKind::RIGHT_PAREN, "列定义结束后要有右括号");
    consume(TokenKind::SEMICOLON, "语句末尾要有分号");

    if (columns.empty()) {
        throw SyntaxError("表至少要有一列，`CREATE TABLE " + tableName +
                              " ()` 建不出东西来",
                          createToken.line, createToken.column);
    }

    // ---- 列名不能重复 ----
    // 这件事必须在建表时就拦住。要是放过去，后面 `SELECT name` 到底
    // 取的是哪一列就没法说了——两列都叫 name，查出来是哪个？
    //
    // 比较之前先转小写：SQL 里列名大小写不敏感，
    // 所以 `Name` 和 `name` 是重复的。
    // 这个检查是 O(n²)，但列数撑死几十个，不值得为此建哈希表。
    for (size_t i = 0; i < columns.size(); i++) {
        for (size_t j = i + 1; j < columns.size(); j++) {
            if (lowercase(columns[i].name) == lowercase(columns[j].name)) {
                // 箭头戳在**后出现的那个**重复列名上。
                // 指第一个没有意义——第一个是用户想要的那列，
                // 该改的是后面这个。
                throw SyntaxError("列名 `" + columns[j].name + "` 重复了",
                                  columns[j].line, columns[j].column);
            }
        }
    }

    return std::make_unique<CreateTableStmt>(std::move(tableName),
                                             std::move(columns));
}

ColumnDef Parser::columnDef() {
    const Token& nameToken = consumeName("列名");

    ColumnDef def;
    def.name = nameToken.lexeme;
    def.line = nameToken.line;
    def.column = nameToken.column;
    def.type = typeName();
    return def;
}

Value::Type Parser::typeName() {
    if (match(TokenKind::INT_TYPE))  return Value::Type::INTEGER;
    if (match(TokenKind::REAL_TYPE)) return Value::Type::REAL;
    if (match(TokenKind::TEXT_TYPE)) return Value::Type::TEXT;
    if (match(TokenKind::BOOL_TYPE)) return Value::Type::BOOLEAN;

    const Token& found = peek();
    throw SyntaxError("这里要写列的类型（INT / REAL / TEXT / BOOL），" +
                          describe(found),
                      found.line, found.column);
}

// ------------------------------------------------------------
//  INSERT
// ------------------------------------------------------------

StmtPtr Parser::insertStatement() {
    advance();  // INSERT
    consume(TokenKind::INTO, "INSERT 后面要跟 INTO");

    const Token& nameToken = consumeName("表名");
    std::string tableName = nameToken.lexeme;

    consume(TokenKind::VALUES, "表名后面要跟 VALUES");

    std::vector<std::vector<ExprPtr>> rows;
    do {
        consume(TokenKind::LEFT_PAREN, "每一行数据要用一对括号包起来");

        std::vector<ExprPtr> row;
        if (!check(TokenKind::RIGHT_PAREN)) {
            do {
                row.push_back(expression());
            } while (match(TokenKind::COMMA));
        }
        consume(TokenKind::RIGHT_PAREN, "这一行的括号没有闭合");

        rows.push_back(std::move(row));
    } while (match(TokenKind::COMMA));  // 逗号后面还有一行

    consume(TokenKind::SEMICOLON, "语句末尾要有分号");

    // 行数为 0 是不可能的：上面的 do-while 至少转一圈。
    // 但"空行"是可能的——`INSERT INTO t VALUES ();`
    // 这个不在这里拦，交给执行器去说"列数对不上"，因为
    // 只有执行器知道表里有几列，能报出更有用的数字。
    return std::make_unique<InsertStmt>(std::move(tableName), std::move(rows));
}

// ------------------------------------------------------------
//  SELECT
// ------------------------------------------------------------

StmtPtr Parser::selectStatement() {
    advance();  // SELECT

    bool selectAll = false;
    std::vector<SelectItem> columns;

    if (match(TokenKind::STAR)) {
        selectAll = true;

        // ---- `SELECT *, COUNT(*)` 要单独给一句人话 ----
        // 不拦的话，错误会跑到下面那行 consume(FROM) 上，
        // 报成"SELECT 的列写完之后要跟 FROM，读到的是 `,`"——
        // 技术上没错，但用户看到的和自己犯的错对不上号，
        // 得自己反推半天。这里直接把真正的问题说出来。
        //
        // `*` 已经代表"所有列"了，再往后面缀东西是自相矛盾的：
        // 要么用 `*`，要么把想要的列一个个列出来。
        if (check(TokenKind::COMMA)) {
            throw SyntaxError("`SELECT *` 后面不能再跟别的列了——"
                              "`*` 已经是「所有列」的意思。"
                              "想要额外的列或表达式，就把要的列一个个列出来",
                              peek().line, peek().column);
        }
    } else {
        do {
            SelectItem item;
            // 先拿到表达式，再看后面跟不跟别名。
            // 顺序不能反——`AS` 左边可能是个很复杂的表达式，
            // 只有解析完它才知道别名从哪儿开始。
            item.expr = expression();

            // ---- 别名只认显式的 AS ----
            // 很多数据库也允许省略 AS：`SELECT score s FROM t`。
            // 这里**故意不支持**，原因是它会让一个常见笔误静悄悄地合法：
            //     SELECT name score FROM t     -- 少打了一个逗号
            // 用户想选两列，实际得到一列叫 score 的 name。
            // 没有 AS 的话这一行会报语法错误，正好提醒他。
            // 「宁可不支持，也不要让笔误有个看起来合理的解释」
            // 和 WHERE 后面必须写布尔值是同一条原则。
            if (match(TokenKind::AS)) {
                item.alias = consumeName("别名").lexeme;
            }

            columns.push_back(std::move(item));
        } while (match(TokenKind::COMMA));
    }

    consume(TokenKind::FROM, "SELECT 的列写完之后要跟 FROM");
    const Token& nameToken = consumeName("表名");
    std::string tableName = nameToken.lexeme;

    ExprPtr where;
    if (match(TokenKind::WHERE)) {
        // WHERE 后面必须真有条件。`SELECT * FROM t WHERE;` 要在这里拦住，
        // 不然 where 就是个空指针，执行器那边得再判一次。
        where = expression();
    }

    std::vector<OrderItem> orderBy;
    if (match(TokenKind::ORDER)) {
        consume(TokenKind::BY, "ORDER 后面要跟 BY");
        do {
            orderBy.push_back(orderItem());
        } while (match(TokenKind::COMMA));
    }

    long long limit = -1;
    long long offset = -1;
    if (match(TokenKind::LIMIT)) {
        limit = limitCount("LIMIT");
        if (match(TokenKind::OFFSET)) {
            offset = limitCount("OFFSET");
        }
    }

    consume(TokenKind::SEMICOLON, "语句末尾要有分号");

    auto stmt = std::make_unique<SelectStmt>(
        selectAll, std::move(columns), std::move(tableName), std::move(where));
    // 这两项在构造之后再填。原因是顺序：ORDER BY 和 LIMIT 出现在
    // FROM/WHERE 后面，而构造函数得在最开始就调。硬要塞进构造函数，
    // 参数列表会长到没法看（而且以后每加一个子句就再长一截）。
    stmt->orderBy = std::move(orderBy);
    stmt->limit = limit;
    stmt->offset = offset;
    return stmt;
}

OrderItem Parser::orderItem() {
    OrderItem item;
    item.key = expression();
    // 没写就是升序，这是 SQL 的规定。所以默认值在 AST 里就设成 true，
    // 这里只在用户明确写了的时候才改。
    if (match(TokenKind::ASC)) {
        item.ascending = true;
    } else if (match(TokenKind::DESC)) {
        item.ascending = false;
    }
    return item;
}

long long Parser::limitCount(const char* what) {
    const Token& found = peek();

    if (!check(TokenKind::INTEGER)) {
        // 这条报错顺手挡掉了 `LIMIT -1`：那个 MINUS 会让 check 失败，
        // 于是用户看到的是"要跟一个整数"，而不是一个悄悄生效的负数限制。
        throw SyntaxError(std::string(what) +
                              " 后面要跟一个非负整数，" + describe(found),
                          found.line, found.column);
    }

    const long long value = found.integerValue;  // 先取值，再 advance
    advance();
    return value;
}

// ------------------------------------------------------------
//  UPDATE / DELETE
// ------------------------------------------------------------

StmtPtr Parser::updateStatement() {
    advance();  // UPDATE

    const Token& nameToken = consumeName("表名");
    std::string tableName = nameToken.lexeme;

    consume(TokenKind::SET, "表名后面要跟 SET，然后写要改的列");

    std::vector<Assignment> assignments;
    do {
        const Token& colToken = consumeName("列名");

        Assignment assignment;
        assignment.column = colToken.lexeme;
        assignment.line = colToken.line;
        assignment.column_pos = colToken.column;

        consume(TokenKind::EQUAL, "列名后面要跟 = 和新的值");
        assignment.value = expression();

        assignments.push_back(std::move(assignment));
    } while (match(TokenKind::COMMA));

    ExprPtr where;
    if (match(TokenKind::WHERE)) {
        where = expression();
    }

    consume(TokenKind::SEMICOLON, "语句末尾要有分号");

    return std::make_unique<UpdateStmt>(std::move(tableName),
                                        std::move(assignments),
                                        std::move(where));
}

StmtPtr Parser::deleteStatement() {
    advance();  // DELETE

    consume(TokenKind::FROM, "DELETE 后面要跟 FROM");

    const Token& nameToken = consumeName("表名");
    std::string tableName = nameToken.lexeme;

    ExprPtr where;
    if (match(TokenKind::WHERE)) {
        where = expression();
    }

    consume(TokenKind::SEMICOLON, "语句末尾要有分号");

    // `DELETE FROM t;` 没有 WHERE 是**合法**的，意思是清空整张表。
    // 这里不拦——标准就是这么定的。危险是危险，但拦下来会让
    // "清空表"这件事没有别的写法。执行器的返回消息里会报删除的行数，
    // 用户看到"删除了 10000 行"自然会吓一跳，这就是提醒。
    return std::make_unique<DeleteStmt>(std::move(tableName),
                                        std::move(where));
}

// ------------------------------------------------------------
//  CREATE INDEX
// ------------------------------------------------------------

// CREATE INDEX 名 ON 表 (列);
//
// 【为什么列名后面那条 COMMA 要专门拦一下】
// 不拦的话，`(a, b)` 会走到"列名后面要有右括号"那句错上，
// 用户看到的是自己明明写对了括号的一个抱怨。多写四行，
// 换来一句"多列索引还没有"，值。
StmtPtr Parser::createIndexStatement() {
    advance();  // INDEX

    const Token& nameToken = consumeName("索引名");
    std::string indexName = nameToken.lexeme;

    consume(TokenKind::ON, "索引名后面要跟 ON，再说建在哪张表上");
    const Token& tableToken = consumeName("表名");
    std::string tableName = tableToken.lexeme;

    consume(TokenKind::LEFT_PAREN, "表名后面要跟一对括号，里面写建在哪一列上");

    const Token& columnToken = consumeName("列名");

    CreateIndexStmt stmt(std::move(indexName), std::move(tableName),
                         columnToken.lexeme);
    // 存下列名的位置。表不存在、列不存在这些错都是在执行器里报的，
    // 那时手里只剩这个名字——不在这儿记下来，箭头就只能戳回语句开头。
    stmt.columnLine = columnToken.line;
    stmt.columnPos = columnToken.column;

    if (check(TokenKind::COMMA)) {
        const Token& found = peek();
        throw SyntaxError(
            "现在只支持单列索引，`(a, b)` 这种多列索引还没有，" +
                describe(found),
            found.line, found.column);
    }
    consume(TokenKind::RIGHT_PAREN, "列名后面要有右括号");
    consume(TokenKind::SEMICOLON, "语句末尾要有分号");

    return std::make_unique<CreateIndexStmt>(std::move(stmt));
}

// ------------------------------------------------------------
//  DROP TABLE / DROP INDEX
// ------------------------------------------------------------

StmtPtr Parser::dropStatement() {
    advance();  // DROP

    // ---- 阶段 4：DROP 后面也有两种了 ----
    if (check(TokenKind::INDEX)) return dropIndexStatement();

    // 【只认 DROP TABLE，别的 DROP 一律拒绝】
    // 标准 SQL 里 DROP 还能删视图、删库……现在都没有。
    // 与其让 `DROP VIEW x` 掉进一句通用的"DROP 后面要跟 TABLE"
    // 让人以为是自己写错了，不如直接说清楚"这里只支持这两样"。
    if (!check(TokenKind::TABLE)) {
        const Token& found = peek();
        throw SyntaxError(
            "DROP 现在只支持 `DROP TABLE 表名` 和 `DROP INDEX 索引名`，" +
                describe(found) + "。删列、删视图、删库这些还没有",
            found.line, found.column);
    }
    advance();  // TABLE

    const Token& nameToken = consumeName("表名");
    std::string tableName = nameToken.lexeme;

    consume(TokenKind::SEMICOLON, "语句末尾要有分号");

    // 没写 IF EXISTS（标准 SQL 有）。表不存在时直接报错，
    // 报错里会列出所有现有的表——打错一个字时一眼就能看出来。
    return std::make_unique<DropTableStmt>(std::move(tableName));
}

// DROP INDEX 索引名;
StmtPtr Parser::dropIndexStatement() {
    advance();  // INDEX

    const Token& nameToken = consumeName("索引名");
    std::string indexName = nameToken.lexeme;

    consume(TokenKind::SEMICOLON, "语句末尾要有分号");

    // 同样没有 IF EXISTS：索引不存在时报错里会列出所有现有的索引。
    return std::make_unique<DropIndexStmt>(std::move(indexName));
}

// ------------------------------------------------------------
//  表达式：一层一个函数，层数越深优先级越高
// ------------------------------------------------------------

ExprPtr Parser::expression() {
    return orExpression();
}

ExprPtr Parser::orExpression() {
    ExprPtr expr = andExpression();

    // 用 while 而不是 if：这样 `a OR b OR c` 会自然地被解析成
    // 左结合的 `((a OR b) OR c)`，不用为"多个"写额外代码。
    while (match(TokenKind::OR)) {
        ExprPtr right = andExpression();
        expr = std::make_unique<BinaryExpr>(BinaryOp::OR, std::move(expr),
                                            std::move(right));
    }
    return expr;
}

ExprPtr Parser::andExpression() {
    ExprPtr expr = notExpression();

    while (match(TokenKind::AND)) {
        ExprPtr right = notExpression();
        expr = std::make_unique<BinaryExpr>(BinaryOp::AND, std::move(expr),
                                            std::move(right));
    }
    return expr;
}

ExprPtr Parser::notExpression() {
    if (match(TokenKind::NOT)) {
        // 递归调自己，所以 `NOT NOT x` 也成立（虽然没人会那么写）。
        // NOT 的层次比比较低，所以 `NOT x = 1` 会先解析出 `x = 1`，
        // 再整体取反——这正是 SQL 要的意思。
        ExprPtr operand = notExpression();
        return std::make_unique<UnaryExpr>(UnaryOp::NOT, std::move(operand));
    }
    return comparison();
}

ExprPtr Parser::comparison() {
    // 注意这里是 additive()，不是 primary()——比较的两边都是
    // **算术表达式**。这就是"算术比比较紧"的全部实现：
    // 比较这一层根本看不见加号，加号在它下面一层已经被吃干净了。
    ExprPtr left = additive();

    BinaryOp op;
    if (asComparisonOp(peek().kind, op)) {
        advance();  // 吃掉运算符

        ExprPtr right = additive();

        // ---- 比较不能连写 ----
        // C 里 `a == b == c` 勉强讲得通（左结合，拿布尔的 0/1 再比），
        // SQL 里则是明确的语法错误。这里主动检测，比让它算出一个
        // 谁也看不懂的结果强——用户真正想写的多半是 `a = b AND b = c`。
        BinaryOp ignored;
        if (asComparisonOp(peek().kind, ignored)) {
            const Token& found = peek();
            throw SyntaxError(
                "比较不能连写，`a = b = c` 在 SQL 里没有意义。"
                "想表达两个条件同时成立，中间要写 AND",
                found.line, found.column);
        }

        left = std::make_unique<BinaryExpr>(op, std::move(left),
                                            std::move(right));
    }

    // ---- IS [NOT] NULL ----
    // 用 while 而不是 if：`x IS NULL IS NULL` 虽然没人写，但
    // 用 while 的规则更简单一致——和 orExpr 那边同一个理由。
    //
    // 它放在 comparison 这一层，紧跟在比较后面，所以
    // `a = b IS NULL` 解析成 `(a = b) IS NULL`——这正是 SQL 的意思：
    // 先算出比较的结果（可能是 NULL），再问"这个结果是不是 NULL"。
    while (match(TokenKind::IS)) {
        const bool negated = match(TokenKind::NOT);

        if (!check(TokenKind::NULL_KW)) {
            const Token& found = peek();
            throw SyntaxError(
                std::string("IS 后面只能是 NULL") +
                    (negated ? "（写 `IS NOT NULL`）" : "（或 `IS NOT NULL`）") +
                    "，判断是不是 NULL 不能拿别的值来比。" + describe(found),
                found.line, found.column);
        }
        advance();  // NULL

        left = std::make_unique<UnaryExpr>(
            negated ? UnaryOp::IS_NOT_NULL : UnaryOp::IS_NULL, std::move(left));
    }

    return left;
}

// ------------------------------------------------------------
//  算术：+ - 比 * / % 松
// ------------------------------------------------------------

ExprPtr Parser::additive() {
    ExprPtr expr = multiplicative();

    // while 而不是 if，让 `a + b + c` 自然左结合成 `((a + b) + c)`。
    // 和 orExpr / andExpr 是同一个写法——加运算符的时候照着抄就行，
    // 这三层的结构完全一样，只有 Token 和 BinaryOp 不同。
    while (true) {
        BinaryOp op;
        if (match(TokenKind::PLUS))         op = BinaryOp::ADD;
        else if (match(TokenKind::MINUS))   op = BinaryOp::SUBTRACT;
        else break;

        expr = std::make_unique<BinaryExpr>(op, std::move(expr),
                                            multiplicative());
    }
    return expr;
}

ExprPtr Parser::multiplicative() {
    ExprPtr expr = unary();

    while (true) {
        BinaryOp op;
        if (match(TokenKind::STAR))          op = BinaryOp::MULTIPLY;
        else if (match(TokenKind::SLASH))    op = BinaryOp::DIVIDE;
        else if (match(TokenKind::PERCENT))  op = BinaryOp::MODULO;
        else break;

        expr = std::make_unique<BinaryExpr>(op, std::move(expr), unary());
    }
    return expr;
}

ExprPtr Parser::unary() {
    if (match(TokenKind::MINUS)) {
        // 递归调 unary 而不是 primary：这样 `- -5` 也成立
        //（虽然没人会那么写），而且规则简单一致。
        //
        // 注意 `--5` **不会**走到这里——`--` 在词法分析阶段
        // 就被当成行注释吃掉了。所以写 `--5` 得到的是一个空语句，
        // 不是负负得正。这是 SQL 的规矩，不是 bug。
        return std::make_unique<UnaryExpr>(UnaryOp::NEGATE, unary());
    }

    if (match(TokenKind::PLUS)) {
        // 一元正号：`+5`。SQL 里合法，语义上什么都不做。
        // 直接返回操作数，**连节点都不建**——`+x` 和 `x` 在语义上
        // 完全一样，为它建个节点只是给执行器多一个永远走不到的分支。
        // 能少一种节点就少一种。
        return unary();
    }

    return primary();
}

ExprPtr Parser::primary() {
    const Token& token = peek();

    switch (token.kind) {
        case TokenKind::INTEGER:
            advance();
            return std::make_unique<LiteralExpr>(
                Value::makeInt(token.integerValue));

        case TokenKind::REAL:
            advance();
            return std::make_unique<LiteralExpr>(
                Value::makeReal(token.realValue));

        case TokenKind::STRING:
            advance();
            return std::make_unique<LiteralExpr>(
                Value::makeText(token.textValue));

        case TokenKind::TRUE_KW:
            advance();
            return std::make_unique<LiteralExpr>(Value::makeBool(true));

        case TokenKind::FALSE_KW:
            advance();
            return std::make_unique<LiteralExpr>(Value::makeBool(false));

        case TokenKind::NULL_KW:
            advance();
            return std::make_unique<LiteralExpr>(Value::makeNull());

        case TokenKind::IDENTIFIER:
            advance();

            // ---- 标识符后面紧跟左括号 = 函数调用 ----
            // 这是整个语法分析器里**唯一**需要往后看一个 Token 才能
            // 决定怎么解析的地方。但它不构成回溯：看一眼就够了，
            // 而且两种走法要吃掉的东西完全不重叠。
            //
            // 不往后看的话，`COUNT(*)` 会被解析成"列 COUNT"，
            // 然后卡在 `(` 上报一句莫名其妙的错。
            if (check(TokenKind::LEFT_PAREN)) {
                return callExpression(token);
            }

            // 列引用。是不是真有这么一列，要等执行器拿到表结构才知道——
            // 语法分析阶段不知道有哪些表，所以这里不检查。
            return std::make_unique<ColumnExpr>(token.lexeme);

        case TokenKind::LEFT_PAREN: {
            advance();
            ExprPtr inner = expression();
            consume(TokenKind::RIGHT_PAREN, "括号没有闭合");
            // 括号本身不进语法树——它不产生任何计算，
            // 它的作用已经体现在树的结构里了。
            // （Nova 里也是这么处理的，用 --ast 能看到括号消失但结构还在）
            return inner;
        }

        default:
            break;
    }

    throw SyntaxError(
        "这里要写一个值（数字 / 字符串 / 列名 / 括号表达式），" +
            describe(token),
        token.line, token.column);
}

// ------------------------------------------------------------
//  函数调用
// ------------------------------------------------------------

ExprPtr Parser::callExpression(const Token& nameToken) {
    consume(TokenKind::LEFT_PAREN, "");  // 调用方已经确认过是左括号

    // ---- 白名单 ----
    // 故意的：不认识的函数名在这里就报错，而且报错位置精确到
    // 那个名字上。如果放过去让执行器去报"未知函数"，位置信息就丢了
    // （执行器手里只有语法树，没有列号），用户得自己去找是哪个词打错了。
    //
    // 而且这些函数都是**关键字以外**的普通标识符（见 token.hpp 的说明），
    // 所以校验它们的责任天然就落在这里。
    static const char* KNOWN_AGGREGATES[] = {"count", "sum", "avg", "min", "max"};
    const std::string lower = lowercase(nameToken.lexeme);

    bool known = false;
    for (const char* name : KNOWN_AGGREGATES) {
        if (lower == name) { known = true; break; }
    }
    if (!known) {
        throw SyntaxError(
            "没有叫 `" + nameToken.lexeme +
                "` 的函数。现在支持的只有聚合函数 COUNT / SUM / AVG / MIN / MAX",
            nameToken.line, nameToken.column);
    }

    bool star = false;
    ExprPtr argument;

    if (match(TokenKind::STAR)) {
        star = true;
        // `SUM(*)` 讲不通：一行有好几列，求和要加哪一列？
        // 只有 COUNT(*) 有意义——它数的是"有几行"，不是"某一列的和"。
        if (lower != "count") {
            throw SyntaxError(
                nameToken.lexeme + "(*) 没有意义。只有 COUNT 能写 COUNT(*)，"
                "它数的是行数；别的聚合函数要写清楚统计哪一列",
                nameToken.line, nameToken.column);
        }
    } else if (!check(TokenKind::RIGHT_PAREN)) {
        argument = expression();
    }

    consume(TokenKind::RIGHT_PAREN, "函数调用的括号没有闭合");

    if (!star && !argument) {
        throw SyntaxError("`" + nameToken.lexeme + "()` 的括号是空的，"
                          "里面要写一列，比如 " + nameToken.lexeme + "(score)",
                          nameToken.line, nameToken.column);
    }

    return std::make_unique<CallExpr>(nameToken.lexeme, std::move(argument),
                                      star);
}

bool Parser::asComparisonOp(TokenKind kind, BinaryOp& out) {
    switch (kind) {
        case TokenKind::EQUAL:         out = BinaryOp::EQUAL;         return true;
        case TokenKind::NOT_EQUAL:     out = BinaryOp::NOT_EQUAL;     return true;
        case TokenKind::LESS:          out = BinaryOp::LESS;          return true;
        case TokenKind::LESS_EQUAL:    out = BinaryOp::LESS_EQUAL;    return true;
        case TokenKind::GREATER:       out = BinaryOp::GREATER;       return true;
        case TokenKind::GREATER_EQUAL: out = BinaryOp::GREATER_EQUAL; return true;
        default:                       return false;
    }
}

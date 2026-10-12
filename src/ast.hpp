#pragma once

// ============================================================
//  AST —— 语法树
//
//  语法分析器把 Token 流变成这棵树，执行器遍历这棵树干活。
//  树本身只是"数据的形状"，不含任何行为。
//
//  【和 Nova 最大的区别：这里用 unique_ptr，不是 shared_ptr】
//
//  Nova 里 Environment（作用域）必须用 shared_ptr，因为一个闭包
//  返回之后，它那次调用的作用域还得活着，而且是"多个闭包可能
//  指着同一个作用域"——所有权不唯一，只能靠引用计数。
//  那是 Nova 整个项目里性价比最高的一个前瞻决定。
//
//  但 SQL 的语法树是**纯树**：每个节点有且只有一个父节点，
//  没有共享、没有环。所有权唯一的时候，unique_ptr 才是对的——
//  它表达的信息更强（"我独占这个孩子"），开销更小（不维护引用计数），
//  而且天然不可拷贝，编译器会帮你挡住"手滑复制了一棵子树"。
//
//  一句话：**先问所有权唯不唯一，再决定用哪个智能指针。**
//  不是"shared_ptr 更安全所以到处都用它"——那是把安全和含糊混为一谈。
//
//  【访问者模式】
//  和 Nova 一样，节点上不写虚函数，而是绕一层 accept(Visitor&)。
//  同一棵树要支持两套操作：执行（executor）和打印（ast_printer）。
//  ast_printer.cpp 就是这个设计最直接的证据——它和执行器毫无关系，
//  却完整复用了同一棵树。
//
//  代价也照实说：这个访问者接口的返回值被钉死成了 Value。
//  执行器要的正是 Value，但打印器只想要个字符串，只好把字符串
//  包成 Value::makeText() 返回。这是 C++ 虚函数没法做"返回类型泛化"
//  留下的疤。（真正的解法是模板访问者，或者 std::variant，
//  但那两样都会把代码搞得难读很多，不该在第一阶段引入。）
// ============================================================

#include <memory>
#include <string>
#include <vector>

#include "result.hpp"
#include "value.hpp"

// ------------------------------------------------------------
//  表达式
// ------------------------------------------------------------

// 二元运算符。
// AND / OR 和比较运算符放在同一个枚举里，是因为它们在语法上
// 处于同一套优先级体系里，解析时按层次分先后就行。
// （区分它们的是"怎么算"，那是执行器的事，不是语法的事。）
enum class BinaryOp {
    EQUAL,          // =
    NOT_EQUAL,      // != 或 <>
    LESS,           // <
    LESS_EQUAL,     // <=
    GREATER,        // >
    GREATER_EQUAL,  // >=
    AND,            // AND
    OR,             // OR

    // ---- 算术 ----
    // 和上面那些分开写一块，因为它们有一个本质区别：
    // 比较和 AND/OR 的结果是布尔（或 NULL），算术的结果是**数**。
    // 执行器里这两类走的是完全不同的两条路，混在一起写容易出错。
    ADD,            // +
    SUBTRACT,       // -
    MULTIPLY,       // *
    DIVIDE,         // /
    MODULO,         // %
};

enum class UnaryOp {
    NOT,          // NOT x
    NEGATE,       // -x
    IS_NULL,      // x IS NULL
    IS_NOT_NULL,  // x IS NOT NULL
};

struct Expr;
struct LiteralExpr;
struct ColumnExpr;
struct BinaryExpr;
struct UnaryExpr;
struct CallExpr;

// unique_ptr 要求类型完整，但表达式节点之间互相引用，
// 所以只能先声明再定义——这就是上面那几行的用处。
using ExprPtr = std::unique_ptr<Expr>;

class ExprVisitor {
public:
    virtual ~ExprVisitor() = default;

    // 返回 Value 是为了执行器（它要算出值来）。
    // 打印器不需要值，只能把字符串包成 Value 返回——见文件开头的说明。
    virtual Value visitLiteral(const LiteralExpr& expr) = 0;
    virtual Value visitColumn(const ColumnExpr& expr) = 0;
    virtual Value visitBinary(const BinaryExpr& expr) = 0;
    virtual Value visitUnary(const UnaryExpr& expr) = 0;
    virtual Value visitCall(const CallExpr& expr) = 0;
};

struct Expr {
    virtual ~Expr() = default;
    virtual Value accept(ExprVisitor& visitor) const = 0;
};

// 字面量：42 / 3.14 / 'abc' / TRUE / NULL
struct LiteralExpr : Expr {
    Value value;

    explicit LiteralExpr(Value value_) : value(std::move(value_)) {}

    Value accept(ExprVisitor& visitor) const override;
};

// 列引用：WHERE 里的 score，指"当前这一行的 score 列"。
// 阶段 2 做 JOIN 时会在前面加上表名。
struct ColumnExpr : Expr {
    std::string name;

    explicit ColumnExpr(std::string name_) : name(std::move(name_)) {}

    Value accept(ExprVisitor& visitor) const override;
};

struct BinaryExpr : Expr {
    BinaryOp op;
    ExprPtr left;
    ExprPtr right;

    BinaryExpr(BinaryOp op_, ExprPtr left_, ExprPtr right_)
        : op(op_), left(std::move(left_)), right(std::move(right_)) {}

    Value accept(ExprVisitor& visitor) const override;
};

struct UnaryExpr : Expr {
    UnaryOp op;
    ExprPtr operand;

    UnaryExpr(UnaryOp op_, ExprPtr operand_)
        : op(op_), operand(std::move(operand_)) {}

    Value accept(ExprVisitor& visitor) const override;
};

// 函数调用：COUNT(*) / SUM(score) / AVG(score)。
//
// 【为什么只有一个参数】
// 阶段 2 只做聚合函数，而聚合函数全都只吃一个参数
//（COUNT(*) 的 `*` 不算参数，见下面的 star）。
// 等以后要做 `SUBSTR(s, 1, 3)` 这类普通函数时，把 argument 换成
// vector<ExprPtr> 就行——但**现在不换**：为一个还没影的需求
// 把接口撑成 vector，结果就是每个用到的地方都得写 `.front()`
// 或者处理"万一是空的"，白白多出一堆分支。
//
// （这和前面几处"提前留缝"不矛盾。留缝的标准是**需求已经看得见**：
//   SELECT 的列存表达式，是因为阶段 2 的 `score * 2` 就在眼前；
//   参数列表撑成 vector，是因为多参数函数连要长什么样都还没定。
//   分界线是"已经确定要加"和"以后也许会加"。）
struct CallExpr : Expr {
    std::string name;  // 原样保留用户写的大小写，报错时用
    ExprPtr argument;  // 参数。COUNT(*) 时是 nullptr

    // COUNT(*) 里那个 `*`。
    // 用一个 bool 而不是塞一个"通配符表达式"进 argument——
    // 因为 `*` 在这里不产生任何值，它的意思是"整行都算进来"。
    // 这和 SELECT * 的处理方式是同一个思路。
    bool star = false;

    CallExpr(std::string name_, ExprPtr argument_, bool star_)
        : name(std::move(name_)), argument(std::move(argument_)), star(star_) {}

    Value accept(ExprVisitor& visitor) const override;
};

// ------------------------------------------------------------
//  语句
// ------------------------------------------------------------

// 建表时一列的定义。阶段 1 只有名字和类型，
// 以后可能有 NOT NULL / DEFAULT / PRIMARY KEY。
struct ColumnDef {
    std::string name;
    Value::Type type;  // 直接复用 Value 的类型枚举——
                       // 列的类型和值的类型本来就是一套东西，
                       // 没必要再定义一份长得一样的枚举

    // 列名在源码里的位置。存着它只为一件事：报"列名重复"的错时，
    // 能把箭头戳在**重复的那一列**上，而不是笼统地指回语句开头的
    // CREATE。`CREATE TABLE t (id INT, id TEXT)` 这种错，
    // 指在行首等于没指。
    int line = 1;
    int column = 1;
};

struct CreateTableStmt;
struct InsertStmt;
struct SelectStmt;
struct UpdateStmt;
struct DeleteStmt;
struct DropTableStmt;
struct CreateIndexStmt;
struct DropIndexStmt;
struct BeginStmt;
struct CommitStmt;
struct RollbackStmt;

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

class StmtVisitor {
public:
    virtual ~StmtVisitor() = default;

    virtual ResultSet visitCreateTable(const CreateTableStmt& stmt) = 0;
    virtual ResultSet visitInsert(const InsertStmt& stmt) = 0;
    virtual ResultSet visitSelect(const SelectStmt& stmt) = 0;
    virtual ResultSet visitUpdate(const UpdateStmt& stmt) = 0;
    virtual ResultSet visitDelete(const DeleteStmt& stmt) = 0;
    virtual ResultSet visitDropTable(const DropTableStmt& stmt) = 0;
    virtual ResultSet visitCreateIndex(const CreateIndexStmt& stmt) = 0;
    virtual ResultSet visitDropIndex(const DropIndexStmt& stmt) = 0;
    virtual ResultSet visitBegin(const BeginStmt& stmt) = 0;
    virtual ResultSet visitCommit(const CommitStmt& stmt) = 0;
    virtual ResultSet visitRollback(const RollbackStmt& stmt) = 0;
};

struct Stmt {
    virtual ~Stmt() = default;
    virtual ResultSet accept(StmtVisitor& visitor) const = 0;
};

// CREATE TABLE users (id INT, name TEXT);
struct CreateTableStmt : Stmt {
    std::string tableName;
    std::vector<ColumnDef> columns;

    CreateTableStmt(std::string tableName_, std::vector<ColumnDef> columns_)
        : tableName(std::move(tableName_)), columns(std::move(columns_)) {}

    ResultSet accept(StmtVisitor& visitor) const override;
};

// INSERT INTO users VALUES (1, 'alice'), (2, 'bob');
// 一次可以插多行，所以是"行的列表"而不是"一行"。
struct InsertStmt : Stmt {
    std::string tableName;
    std::vector<std::vector<ExprPtr>> rows;

    InsertStmt(std::string tableName_, std::vector<std::vector<ExprPtr>> rows_)
        : tableName(std::move(tableName_)), rows(std::move(rows_)) {}

    ResultSet accept(StmtVisitor& visitor) const override;
};

// ORDER BY 的一项：按哪个表达式排，是升序还是降序。
struct OrderItem {
    ExprPtr key;
    bool ascending = true;  // 没写 ASC/DESC 时默认升序（SQL 的规定）
};

// 选择列表里的一项：一个表达式，外加可选的别名。
//
//     SELECT score * 2 AS 双倍分, name FROM users
//            ^^^^^^^^^^^^  ^^^^^^^
//              expr         alias
struct SelectItem {
    ExprPtr expr;

    // 空串表示没写 AS。**不能用"缺省别名"来占位**——
    // 别名本来就可能是任何合法标识符，没有一个值能当"没有"的哨兵。
    // 空串在这里是安全的，因为 SQL 里标识符不能是空串。
    std::string alias;
};

// SELECT * FROM users WHERE score > 90;
struct SelectStmt : Stmt {
    // 是不是 `SELECT *`。
    // 单独用一个 bool，而不是在 columns 里塞个"通配符表达式"——
    // 因为它在语义上真的不一样：通配符不产生一个值，
    // 它是"把表里所有列都取出来"，列数取决于表结构。
    bool selectAll = false;

    // 选中的列。
    //
    // 【前瞻设计，兑现了，然后又往前走了一小步】
    // 这里存的是**表达式**而不是列名。
    // 阶段 1 只有 `SELECT name, score`，存列名就够了。
    // 但阶段 2 要支持 `SELECT score * 2`、`SELECT COUNT(*)`。
    //
    // 当初就按表达式存，所以阶段 2 加这些功能时，
    // **这个字段一个字都没改**——只是 parser 往里塞的东西变复杂了。
    // 这就是那层前瞻的回报：改动集中在一处，而不是散到
    // ast / parser / executor / printer 四个文件里。
    // （Nova 的 Environment 从第一天就用 shared_ptr，是同一个道理。）
    //
    // 阶段 2 收尾加 `AS` 别名时，这个字段从 `vector<ExprPtr>`
    // 变成了 `vector<SelectItem>`——**这就是那层前瞻的真实边界**：
    // 它让"往表达式里加东西"变便宜，但"往列表项上加东西"还是得动结构。
    // 好在这个改动只有 4 处（parser 填、executor 读、printer 读），
    // 而不是全项目铺开。前瞻降低改动的成本，不是消除它。
    std::vector<SelectItem> columns;

    std::string tableName;

    // WHERE 条件。没有 WHERE 时是 nullptr——
    // 这就是"可能不存在"最直白的表达，比塞一个恒真表达式清楚。
    ExprPtr where;

    // ORDER BY 的排序键。空 vector 表示没写 ORDER BY。
    //
    // 这里用"空"而不是 nullptr 表达"没有"，和 where 的做法**不一样**，
    // 是有意的：where 是单个对象，"有没有"只能靠指针；
    // 排序键本来就是一组，而空的一组天然就是"没有"。
    // 能用空集合表达的地方就别引入指针——少一种可能出错的状态。
    std::vector<OrderItem> orderBy;

    // LIMIT / OFFSET。
    // -1 表示"没写"。用 -1 而不是 0 当哨兵，因为 **0 是个合法的
    // LIMIT**（`LIMIT 0` 就是"一行都不要"，虽然没人这么写），
    // 用 0 当"没写"会让这两件事分不开。
    //
    // 这里没用 std::optional：能用 -1 说清楚的事，不值得为它
    // 引入一个额外类型。行数不可能是负数，这个哨兵是安全的。
    long long limit = -1;
    long long offset = -1;

    SelectStmt(bool selectAll_, std::vector<SelectItem> columns_,
               std::string tableName_, ExprPtr where_)
        : selectAll(selectAll_),
          columns(std::move(columns_)),
          tableName(std::move(tableName_)),
          where(std::move(where_)) {}

    ResultSet accept(StmtVisitor& visitor) const override;
};

// UPDATE users SET score = score + 1 WHERE name = 'alice';
//
// assignments 用 vector 而不是 map：**一个语句里 `SET a = 1, a = 2`
// 是合法的**（后写的赢），而 map 会悄悄把前一个吃掉。
// 顺序在 SQL 里是有意义的，别用无序的容器装它。
struct Assignment {
    std::string column;
    ExprPtr value;
    int line = 1;    // 列名的位置，报错时箭头要戳准
    int column_pos = 1;
};

struct UpdateStmt : Stmt {
    std::string tableName;
    std::vector<Assignment> assignments;
    ExprPtr where;  // 可为 nullptr：`UPDATE t SET x = 1;` 改全表

    UpdateStmt(std::string tableName_, std::vector<Assignment> assignments_,
               ExprPtr where_)
        : tableName(std::move(tableName_)),
          assignments(std::move(assignments_)),
          where(std::move(where_)) {}

    ResultSet accept(StmtVisitor& visitor) const override;
};

// DELETE FROM users WHERE score < 60;
struct DeleteStmt : Stmt {
    std::string tableName;
    ExprPtr where;  // 可为 nullptr：`DELETE FROM t;` 清空整张表

    DeleteStmt(std::string tableName_, ExprPtr where_)
        : tableName(std::move(tableName_)), where(std::move(where_)) {}

    ResultSet accept(StmtVisitor& visitor) const override;
};

// DROP TABLE users;
//
// 【为什么删表比删行"重"】删行只是把槽位标成墓碑，页还在；
// 删表是把整条数据页链还给空闲链，那些 4KB 真的能被别人用了。
// 所以这个语句是阶段 3"空闲页管理"唯一的用户——没有它，
// 空闲链就是一堆永远走不到的代码。
struct DropTableStmt : Stmt {
    std::string tableName;

    explicit DropTableStmt(std::string tableName_)
        : tableName(std::move(tableName_)) {}

    ResultSet accept(StmtVisitor& visitor) const override;
};

// CREATE INDEX idx_score ON users (score);
//
// 【索引名和列名为什么各存了 line / column】
// 只为一件事：报错时箭头戳准。`CREATE INDEX i ON users (scoer)`
// 里那个拼错的列名，指回语句开头的 CREATE 等于没指。
// （和 ColumnDef 存位置是同一个理由。）
struct CreateIndexStmt : Stmt {
    std::string indexName;
    std::string tableName;
    std::string columnName;

    int columnLine = 1;
    int columnPos = 1;

    CreateIndexStmt(std::string indexName_, std::string tableName_,
                    std::string columnName_)
        : indexName(std::move(indexName_)),
          tableName(std::move(tableName_)),
          columnName(std::move(columnName_)) {}

    ResultSet accept(StmtVisitor& visitor) const override;
};

// DROP INDEX idx_score;
struct DropIndexStmt : Stmt {
    std::string indexName;

    explicit DropIndexStmt(std::string indexName_)
        : indexName(std::move(indexName_)) {}

    ResultSet accept(StmtVisitor& visitor) const override;
};

// ------------------------------------------------------------
//  事务（阶段 5）
// ------------------------------------------------------------
//
// 三个节点，一点内容都没有。
//
// 【为什么这样是对的，而不是"设计得不够"】
// 因为 BEGIN / COMMIT / ROLLBACK 本来就**没有参数**。
// 要给它们加字段只能是硬凑（比如"隔离级别"——那是阶段 6 以后的事）。
//
// 空结构体在 C++ 里大小是 1，三个节点一共 3 个字节。
// 硬把它们塞进一个枚举字段的通用语句节点里也能省下这点空间，
// 但代价是每处用到的地方都要写 `if (stmt.kind == BEGIN)`，
// 而访问者模式的好处正是"不用判断种类"——恢复成判断就把好处扔了。
//
// 这三行 accept 也是 ast.cpp 里那三行样板；
// 真正的逻辑在执行器里（而且很短，因为事务的实现在 Pager）。
struct BeginStmt : Stmt {
    ResultSet accept(StmtVisitor& visitor) const override;
};

struct CommitStmt : Stmt {
    ResultSet accept(StmtVisitor& visitor) const override;
};

struct RollbackStmt : Stmt {
    ResultSet accept(StmtVisitor& visitor) const override;
};

// 运算符的名字，打印语法树和报错时用。
const char* binaryOpName(BinaryOp op);
const char* unaryOpName(UnaryOp op);
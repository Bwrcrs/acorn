#pragma once

// ============================================================
//  Executor —— 执行器
//
//  同时实现 ExprVisitor 和 StmtVisitor：语句节点进来，
//  在 executor.cpp 里分派到具体做法；语句里的表达式再通过
//  ExprVisitor 求值。
//
//  【状态挂在成员上，是接口逼出来的】
//  求值一个列名，必须知道"现在读的是哪一行、这一行是哪张表的"。
//  但 visitColumn 的签名是定死的（只收一个 ColumnExpr），
//  没地方再塞两个参数进来。
//
//  所以只能把"当前行"挂成成员变量。这就带来一个必须小心的问题：
//  用完得记得清掉。忘了清的话，下一次 INSERT 求值时会看见
//  上一轮遗留的行——**而且不会报错**，只是结果莫名其妙。
//
//  所以不手写"置空"，用下面的 RowScope 靠析构函数管。
//  这和 Nova 里 EnvironmentGuard 是同一个套路，理由都一样：
//  执行过程中会抛异常，手写的那行"恢复现场"就可能执行不到。
// ============================================================

#include <vector>

#include "ast.hpp"
#include "database.hpp"
#include "result.hpp"
#include "schema.hpp"
#include "table.hpp"
#include "value.hpp"

class Executor : public ExprVisitor, public StmtVisitor {
public:
    explicit Executor(Database& db);

    // 执行一条语句
    ResultSet execute(const Stmt& stmt);

    // ---- StmtVisitor ----
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

    // ---- ExprVisitor ----
    Value visitLiteral(const LiteralExpr& expr) override;
    Value visitColumn(const ColumnExpr& expr) override;
    Value visitBinary(const BinaryExpr& expr) override;
    Value visitUnary(const UnaryExpr& expr) override;
    Value visitCall(const CallExpr& expr) override;

private:
    // RAII 守卫：换上"当前行 / 当前表"，出作用域自动换回来。
    // 定义在 executor.cpp 里——它只是个实现细节，
    // 外面的人不需要知道有这个东西。
    class RowScope;

    // 一个聚合函数的累加器。
    //
    // 【为什么定义在头文件里】
    // 因为下面有个 `std::vector<AggregateState>` 成员，而 vector 的
    // 实例化要求元素类型是**完整类型**。只有前向声明的话编译不过。
    // （RowScope 能藏进 .cpp，是因为它只以引用的形式出现在函数里。）
    struct AggregateState {
        long long count = 0;        // COUNT 用：数了多少行/多少个非 NULL
        bool sawNonNull = false;    // SUM/AVG/MIN/MAX 用：空集的答案是 NULL

        // SUM 要同时记整数和浮点两条路。
        // `SUM(id)` 的答案应该是整数 6，不是浮点数 6——
        // 全程用 double 累加的话，超过 2^53 就开始丢精度，
        // 而且 SUM 一列整数却打出 "6" 还是 "6" 看不出来，
        // 但打出 1e16 和 10000000000000000 就看出区别了。
        bool sumIsReal = false;
        long long intSum = 0;
        double realSum = 0.0;

        Value best;  // MIN/MAX 用：目前见过的最小/最大值
    };

    // 把一个值喂给第 index 个聚合器。COUNT(*) 那一路不走这里。
    //
    // 【为什么收的是下标，而不是 AggregateState&】
    // 因为"这是 MIN 还是 MAX"这个信息不在累加器里，而在调用节点上
    //（aggregateNodes_[index]->name）。累加器只管攒数据，
    // 不管自己是什么身份——这样它就只是个纯粹的数据袋子，
    // 不需要跟着聚合种类一起增删字段。
    void accumulateValue(size_t index, const Value& value);

    Database& db_;

    // 求值列名时的上下文。没有正在读的行时是 nullptr，
    // 这时候出现列名就是用户的错（比如写在了 INSERT 的 VALUES 里），
    // visitColumn 会报一句能看懂的话。
    const TableSchema* currentSchema_ = nullptr;
    const Row* currentRow_ = nullptr;

    // ---- 聚合函数的累加器 ----
    //
    // 【为什么聚合不能用普通的逐行求值】
    // 普通表达式是"给我一行，我算出一个值"。聚合不是：
    // COUNT(*) 要看到**所有**行才能给出答案，单独一行算不出"总数"。
    //
    // 所以聚合走的是另一条路：visitSelect 先扫一遍全表把结果累加
    // 到 aggregateStates_ 里，**扫完之后**才去求值选择列表。
    // 那时候 visitCall 不再是"计算"，而是"取已经算好的结果"。
    //
    // 两个 vector 一一对应：aggregateNodes_[i] 这个调用节点，
    // 它的累加器是 aggregateStates_[i]。用指针做对应关系而不是
    // 下标，是因为调用节点藏在表达式的任意深度（`COUNT(*) + 1`
    // 里的 COUNT 就不是选择列表的第一项），没法用位置去猜。
    //
    // 【这两个成员是每次查询的临时状态】
    // 每次 visitSelect 开头都会清空重填。它们挂在成员上是不得已——
    // visitCall 的签名只有 `const CallExpr&`，没地方再传一个
    // "累加器列表"进去。这和 currentRow_ 挂在成员上是同一个原因。
    std::vector<const CallExpr*> aggregateNodes_;
    std::vector<AggregateState> aggregateStates_;
};

#include "executor.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ast_printer.hpp"
#include "token.hpp"  // lowercase
#include "error.hpp"

// ============================================================
//  三值逻辑
// ============================================================

namespace {

// SQL 的真假不是两种，是**三种**。
//
// 这是 SQL 里最反直觉的一条，也是新手写 WHERE 最容易翻车的地方。
// 看这条：
//
//     SELECT * FROM users WHERE age = NULL;
//
// 它**永远返回空**，一行都不会有。因为 NULL 的意思是"不知道"，
// 而"不知道他几岁"和"他到底几岁"之间没法判断相等——
// 结果是"不知道"，不是"是"，也不是"否"。
// 想查年龄为空的行要写 `WHERE age IS NULL`（阶段 2 才有）。
//
// 多出来的这第三种，让 AND / OR 也不能用普通的真值表，
// 得用 Kleene 三值逻辑。实现在 visitBinary 里。
enum class Truth {
    IS_TRUE,
    IS_FALSE,
    IS_NULL,  // "不知道"
};

Truth truthOf(const Value& value) {
    if (value.isNull()) return Truth::IS_NULL;

    if (value.type != Value::Type::BOOLEAN) {
        // ---- 这里和 Nova 的选择正好相反，值得对照着看 ----
        // Nova 规定：只有 false 和 nil 是假，0 和 "" 都算真。
        // 那是个"图省事"的选择，好处是不用记一堆特例。
        //
        // SQL 这边选择报错。因为 WHERE 后面该写的是**条件**，不是值：
        // `WHERE age` 在 C 里意思是"age 非 0"，可用户真正想说的
        // 多半是 `WHERE age IS NOT NULL`，或者 `WHERE age > 0`。
        // 放它过去，用户就会以为自己写对了，然后拿着错结果去干别的。
        throw AcornError(
            "条件的位置要写布尔值（TRUE / FALSE），但拿到的是" +
            value.typeName() + " " + value.toSqlLiteral() +
            "。SQL 不像 C 那样把 0 当假、非 0 当真，条件要写清楚，"
            "比如 `age > 0`");
    }

    return value.boolean ? Truth::IS_TRUE : Truth::IS_FALSE;
}

// ------------------------------------------------------------
//  收集表达式里用到的列名
// ------------------------------------------------------------

// 【这里为什么**不**用访问者】
//
// ExprVisitor 的方法必须返回 Value（因为执行器要算值），
// 而"把列名收集到一个 vector 里"这件事根本没有值可返回，
// 硬用的话每个方法都得 `return Value::makeNull();` 凑数。
//
// 那就不是一个好抽象了——这是上一节说的那道疤在这里露出来。
// 访问者模式擅长的是"对每个节点做一件**产生值**的事"，
// 不擅长"遍历时顺手收集点东西"。
//
// 所以这里老老实实写递归 + dynamic_cast。二十行，一眼看得懂，
// 比硬套一个会返回一堆空值的访问者强。
//
// 顺带一提：这件事在真正的数据库里叫 **binder**（绑定器）——
// 把"用户写的列名"绑定到"表结构里的第几列"。那是查询规划的第一步，
// 阶段 2 会把它正式做出来（Nova 的 README 末尾也提到过类似的东西，
// 叫 Resolver，是同一个思路：在真正执行之前，先把名字对一遍）。
void collectColumns(const Expr& expr, std::vector<std::string>& out) {
    if (const auto* column = dynamic_cast<const ColumnExpr*>(&expr)) {
        out.push_back(column->name);
        return;
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
        collectColumns(*binary->left, out);
        collectColumns(*binary->right, out);
        return;
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        collectColumns(*unary->operand, out);
        return;
    }
    if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
        // 聚合函数的参数里的列**也要验存在**：
        // `SELECT SUM(scor) FROM t` 里的 scor 打错了，一样要报错。
        // 只差一点就写成"聚合函数整个跳过"，那样打错的列名
        // 会一路走到执行时才炸，而且报的是一句看不懂的话。
        //
        // COUNT(*) 没有参数（argument 是 nullptr），所以要判一下。
        if (call->argument) collectColumns(*call->argument, out);
        return;
    }
    // LiteralExpr 里不可能有列名，不用管。
    // 加新表达式类型时这里要补一支——编译器不会提醒你，
    // 所以这段函数短一点比较好。
}

// ------------------------------------------------------------
//  聚合函数的识别
// ------------------------------------------------------------

// 表达式里有没有聚合函数调用。
//
// 注意 CallExpr 直接返回 true 就够了，**不需要**再递归进它的参数：
// 因为聚合套聚合是非法写法，本来就会在别处被拦下。
bool hasAggregate(const Expr& expr) {
    if (dynamic_cast<const CallExpr*>(&expr) != nullptr) return true;
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
        return hasAggregate(*binary->left) || hasAggregate(*binary->right);
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        return hasAggregate(*unary->operand);
    }
    return false;
}

// 表达式里有没有"**不在聚合函数里面**的列引用"。
//
// 这是判断一条 SELECT 合不合法的那把尺子：
// 一旦查询里有聚合函数，就要求每一项都必须是"聚合"或"常量"。
// `SELECT id, COUNT(*) FROM t` 之所以非法，是因为 id 想问
// "哪一行的 id？"——全体行塌缩成一行之后，这个问题没有答案。
// （标准 SQL 里这要配 GROUP BY 才有意义，而 GROUP BY 是阶段 3 的事。）
//
// 注意 CallExpr 那一支直接返回 false：聚合函数**里面**的列
// 是合法的，`COUNT(*)` 和 `SUM(score)` 都正是这么写的。
// 递归进去查会把它们误判成裸列引用。
bool hasBareColumn(const Expr& expr) {
    if (dynamic_cast<const ColumnExpr*>(&expr) != nullptr) return true;
    if (dynamic_cast<const CallExpr*>(&expr) != nullptr) return false;
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
        return hasBareColumn(*binary->left) || hasBareColumn(*binary->right);
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        return hasBareColumn(*unary->operand);
    }
    return false;
}

// 按确定的顺序把表达式里所有聚合调用收集出来。
//
// 顺序必须是**确定的**：调用方会拿这个列表的下标去建对应的累加器数组，
// 两次遍历顺序不一样的话，累加器就和调用节点对不上了。
// 这里固定用"先左后右、先操作数后参数"，每次跑到结果都一样。
void collectAggregates(const Expr& expr, std::vector<const CallExpr*>& out) {
    if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
        out.push_back(call);
        // 聚合里套聚合（`SUM(COUNT(*))`）在 SQL 里非法。
        // 这里不拦，交给调用方检查——因为"为什么非法"这句话
        // 更适合放在能看见整条语句的地方说。
        return;
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
        collectAggregates(*binary->left, out);
        collectAggregates(*binary->right, out);
        return;
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        collectAggregates(*unary->operand, out);
        return;
    }
}

// 聚合调用里有没有直接套着另一个聚合。
bool hasNestedAggregate(const CallExpr& call) {
    return call.argument && hasAggregate(*call.argument);
}

// 确认表达式里提到的列都真的存在，不存在就报错。
//
// 【为什么必须在扫描之前做】
// 因为表可能是空的。空表一行都不会扫到，那么"求值这个表达式"
// 这件事永远不会发生，写错的列名也就永远暴露不出来——
//     SELECT * FROM 空表 WHERE nmae = 1
// 会安安静静地返回一张空表，用户还以为"确实没数据"。
// 这种错比报错难查一百倍。
void requireColumnsExist(const Expr& expr, const TableSchema& schema) {
    std::vector<std::string> names;
    collectColumns(expr, names);

    for (const std::string& name : names) {
        if (schema.indexOf(name) < 0) {
            throw AcornError("表 `" + schema.name + "` 里没有叫 `" + name +
                             "` 的列（有：" + schema.columnList() + "）");
        }
    }
}

// ------------------------------------------------------------
//  规划：这条 WHERE 能不能走索引（阶段 4）
// ------------------------------------------------------------
//
// 【什么是查询规划】
// 同一条查询往往有好几种做法，规划就是"挑一种划算的"。
// 这里挑的只有一件事：WHERE 里有没有一个条件能靠某个索引
// 把要读的行从"整张表"缩小到"几条"，而不是老老实实从头扫到尾。
//
// ★ 规划器不许改变查询的结果，只能改变查询的快慢 ★
//
// 这条规矩比"能不能用上索引"重要得多。本项目里它具体体现为
// 下面三处"宁可不优化"：
//   · 常量当场算不出来（`id = 1/0`）→ 退回全表扫
//   · 类型比不了（`name > 5`）     → 退回全表扫
//   · 是 OR 而不是 AND            → 退回全表扫
// 每一处的注释里都写了为什么。它们全都是同一个理由：
// **全表扫时这些错误"该发生的时候才发生"，规划器抢先一步
//   就会把它提前**，于是空表上的行为跟着变了。
struct IndexPlan {
    bool usable = false;  // false = 老老实实全表扫，其余字段都没意义

    std::string indexName;

    // 区间。两头都可以缺（`id > 5` 只有下界）。
    // 等值查找就是"两头都是同一个值、两头都闭"。
    bool hasLower = false;
    Value lower;
    bool lowerInclusive = true;
    bool hasUpper = false;
    Value upper;
    bool upperInclusive = true;
};

// 这个表达式里有没有列引用。
bool mentionsColumn(const Expr& expr) {
    std::vector<std::string> names;
    collectColumns(expr, names);
    return !names.empty();
}

// 这个值和这一列的类型比得了吗。
// 和执行器里比较运算的规则保持一致（整数和浮点可以互比）。
bool comparableWith(const Value& value, Value::Type columnType) {
    if (columnType == Value::Type::REAL) return value.isNumeric();
    return value.type == columnType;
}

// 试着一个条件能不能变成"对某个索引做一次范围查找"。
//
// 认的形状只有一种：`列 op 常量`（常量写在左边也认）。
// 那一列上得真有索引，运算符还得能说成一段区间。
bool planCondition(const Expr& expr, const TableSchema& schema,
                   const Database& db,
                   const std::function<Value(const Expr&)>& evalConstant,
                   IndexPlan& out) {
    const auto* binary = dynamic_cast<const BinaryExpr*>(&expr);
    if (binary == nullptr) {
        // IS NULL / IS NOT NULL 走的是 UnaryExpr，这里先不管。
        // 【它们是能用索引的】`x IS NULL` 就是"键 = NULL"的一次
        // 等值查找（索引里 NULL 排在最前）。只是要多写一支，
        // 而少写这一支只是慢，不会错。先不做。
        return false;
    }

    // ---- 一、运算符要能说成一段区间 ----
    bool hasLower = false;
    bool lowerInclusive = true;
    bool hasUpper = false;
    bool upperInclusive = true;

    switch (binary->op) {
        case BinaryOp::EQUAL:
            // 等值：两头都是它。
            hasLower = true;
            hasUpper = true;
            break;
        case BinaryOp::GREATER:
            hasLower = true;
            lowerInclusive = false;
            break;
        case BinaryOp::GREATER_EQUAL:
            hasLower = true;
            lowerInclusive = true;
            break;
        case BinaryOp::LESS:
            hasUpper = true;
            upperInclusive = false;
            break;
        case BinaryOp::LESS_EQUAL:
            hasUpper = true;
            upperInclusive = true;
            break;
        default:
            // `!=` 用不了：它不是一段区间，是"整条数轴挖掉一个点"。
            // 硬用就得查两段再拼起来，而"拼"意味着还要去重。
            // 去重的漏做和多做**都不报错**，一个少行一个重复行。
            return false;
    }

    // ---- 二、哪边是列，哪边是常量 ----
    // 列那边必须是**光秃秃的一个列名**。`id + 1 = 5` 用不了——
    // 想拿它反推出 id 的范围，得把表达式求逆，那是另一件事。
    //
    // 常量那边的判定是"里面没有列引用"，而不是"它长得像个字面量"：
    // `id = 1 + 2` 里右边是个表达式，但它是常量，
    // 而且**没有列引用的表达式求值时不需要当前行**——
    // 所以在这一行都还没读的时候就能算出来。
    const auto* leftColumn = dynamic_cast<const ColumnExpr*>(binary->left.get());
    const auto* rightColumn = dynamic_cast<const ColumnExpr*>(binary->right.get());

    const ColumnExpr* column = nullptr;
    const Expr* constant = nullptr;
    bool flipped = false;  // 常量写在左边，比如 `5 > id`
    if (leftColumn != nullptr && !mentionsColumn(*binary->right)) {
        column = leftColumn;
        constant = binary->right.get();
    } else if (rightColumn != nullptr && !mentionsColumn(*binary->left)) {
        column = rightColumn;
        constant = binary->left.get();
        flipped = true;
    } else {
        // 两边都是列（`a = b`）、或者两边都是常量：用不了索引。
        return false;
    }

    // ---- 三、这一列上有没有索引 ----
    // 列名要是写错了，indexOf 返回 -1，这里直接放弃——
    // 报错留给别处（requireColumnsExist 会说一句能看懂的话）。
    const int columnIndex = schema.indexOf(column->name);
    if (columnIndex < 0) return false;

    const std::vector<IndexSchema> indexes = db.indexesOf(schema.name);
    const IndexSchema* chosen = nullptr;
    for (const IndexSchema& index : indexes) {
        if (index.column == columnIndex) {
            chosen = &index;
            break;
        }
    }
    if (chosen == nullptr) return false;  // 这一列上没索引，退回全表扫

    // ---- 四、常量要能当场算出来 ----
    Value bound;
    try {
        bound = evalConstant(*constant);
    } catch (...) {
        // 【为什么这里把异常吞掉】
        // 走到这一步说明这个表达式里没有列引用，所以它是常量，
        // 而全表扫的时候**每一行都会算它一遍**——它要报错的话，
        // 全表扫在表非空时同样会报。
        // 但表是空的时候，全表扫一行都不扫，根本不会去算它。
        // 规划器在这儿抢先算，就会把"空表返回空"变成"报错"。
        // 规划器不许改变结果 → 退回全表扫，让错误在原来的时候出现。
        return false;
    }

    // ---- 五、NULL 用不了索引 ----
    // `x = NULL` 永远不为真（三值逻辑），`x > NULL` 也一样。
    // 拿 NULL 去索引里查，查出来的行会被 WHERE 再判一遍全部丢掉——
    // 结果是**对的**，只是白干。所以这不是正确性问题，是白干问题。
    if (bound.isNull()) return false;

    // ---- 六、类型得比得了 ----
    // 比不了就退回全表扫。理由和上面吞异常那段一模一样：
    // "不同类型不能比"这句错是**逐行判 WHERE 时**报的，
    // 表是空的话它根本不会出现。规划器不能把它提前。
    const Value::Type columnType =
        schema.columns[static_cast<size_t>(columnIndex)].type;
    if (!comparableWith(bound, columnType)) return false;

    // ---- 七、填计划 ----
    out.usable = true;
    out.indexName = chosen->name;

    if (flipped) {
        // ★ `5 > id` 和 `id < 5` 是一回事，边界要**换个边** ★
        // 忘了这件事的话，`5 > id` 会被当成 `id > 5` 去查——
        // 那是一个安静的、完全相反的答案。
        std::swap(hasLower, hasUpper);
        std::swap(lowerInclusive, upperInclusive);
    }

    out.hasLower = hasLower;
    out.lowerInclusive = lowerInclusive;
    out.hasUpper = hasUpper;
    out.upperInclusive = upperInclusive;
    if (hasLower) out.lower = bound;
    if (hasUpper) out.upper = bound;
    return true;
}

// 在 WHERE 里找一个能用索引的条件。
//
// 【为什么只在顶层 AND 里找，绝不碰 OR】
// `a = 1 OR b = 2`：要是只挑 `a = 1` 去走索引，另一个分支的行
// 会被整个漏掉——**结果少行，而且不报错**。
// 要用索引就得把两个分支的结果并起来再去重（真数据库里叫
// index merge），那是另一套机制。
//
// `a = 1 AND b = 2` 就安全：挑哪一个都行。因为两个条件都要满足，
// 用 `a` 的索引筛出来的行**一定包含**所有满足整个条件的行，
// 剩下的条件在逐行判 WHERE 的时候还会再算一遍。
//
// 挑一个就够了，不去找第二个：索引的收益是"把几千页变成几页"，
// 第二个索引带来的收益远小于它带来的复杂度。
bool planIndex(const Expr* where, const TableSchema& schema,
               const Database& db,
               const std::function<Value(const Expr&)>& evalConstant,
               IndexPlan& out) {
    if (where == nullptr) return false;

    // 把顶层的 AND 拆平：`a AND b AND c` 要能一个个试。
    const auto* binary = dynamic_cast<const BinaryExpr*>(where);
    if (binary != nullptr && binary->op == BinaryOp::AND) {
        // 短路：左边能用就不试右边了（out 已经被填好）。
        return planIndex(binary->left.get(), schema, db, evalConstant, out) ||
               planIndex(binary->right.get(), schema, db, evalConstant, out);
    }

    return planCondition(*where, schema, db, evalConstant, out);
}

// 按计划扫一遍表，把每一行连着行号交给 visit。
//
// 【这是"走索引"和"全表扫"唯一的分岔口】
// SELECT / UPDATE / DELETE 都从这儿拿行，所以"索引怎么用"
// 只有一份实现。分成三份的话，早晚会有一个语句忘了走索引
//（那只是慢），或者更糟——在其中一份里写错，而**只有那个语句**
// 出错。
void scanWithPlan(Database& db, const Table& table, const IndexPlan& plan,
                  const std::function<void(RowId, const Row&)>& visit) {
    if (!plan.usable) {
        table.scan(visit);
        return;
    }

    // ---- 先把行号收齐，再一行行回表 ----
    // 因为 searchIndex 的回调跑在 B+ 树扫描的过程中，那时候
    // 树的一串页正借着。在回调里回表会去借表的数据页，
    // "正在扫树"和"正在读表"两件事就混在一起了。收齐再读，
    // 两条路分得干干净净。
    std::vector<RowId> ids;
    db.searchIndex(plan.indexName, plan.hasLower, plan.lower,
                   plan.lowerInclusive, plan.hasUpper, plan.upper,
                   plan.upperInclusive, [&](RowId id) { ids.push_back(id); });

    // ---- 把行号排回"全表扫的顺序" ----
    //
    // 索引里出来的行号是按**键**排的，全表扫是按**页号和槽位号**
    // 排的。不排的话，同一条查询"建索引前"和"建索引后"返回的
    // 行序就不一样了。
    //
    // 【为什么要较这个真】SQL 确实不保证没有 ORDER BY 时的行序，
    // 但"加一个索引会改变查询输出"在用户眼里就是"加索引把数据
    // 搞乱了"。我们要的性质是：**加索引只让它变快，结果一个字不变**。
    // 这样"加了索引之后结果不对"就永远只需要怀疑索引维护，
    // 不需要怀疑行序。
    std::sort(ids.begin(), ids.end(), [](const RowId& a, const RowId& b) {
        if (a.pageNo != b.pageNo) return a.pageNo < b.pageNo;
        return a.slot < b.slot;
    });

    for (const RowId& id : ids) {
        // row() 返回的是个临时 Row，它活到 visit 返回为止——
        // 和 Table::scan 的回调收 `const Row&` 是同一个契约。
        visit(id, table.row(id));
    }
}

// ------------------------------------------------------------
//  算术
// ------------------------------------------------------------

// 整数溢出的报错。抽出来是因为四则运算每一处都要用，文案得一致。
std::string overflowMessage(const char* op) {
    return std::string("整数溢出了：两个数做 `") + op +
           "` 的结果超出了 64 位整数能表示的范围。"
           "整数溢出在 C++ 里是未定义行为（不是回绕），"
           "与其给你一个莫名其妙的负数，不如在这里停下";
}

// 算术运算。和比较运算最大的不同：
// **比较的结果是布尔，算术的结果是数**，而且是 INT 还是 REAL 要看两边。
Value evalArithmetic(BinaryOp op, const Value& left, const Value& right) {
    // NULL 传染：任何一边是 NULL，结果就是 NULL。
    // 这条和比较运算一样，理由也一样——"不知道"参与任何计算
    // 得到的都还是"不知道"。
    if (left.isNull() || right.isNull()) return Value::makeNull();

    if (!left.isNumeric() || !right.isNumeric()) {
        // 和 Nova 里 `1 + "a"` 不许隐式转换是同一条原则。
        // 静悄悄给个答案，错误会跑到离现场很远的地方才发作。
        throw AcornError(std::string("`") + binaryOpName(op) +
                         "` 的两边都得是数字，但拿到的是" +
                         left.typeName() + "和" + right.typeName());
    }

    const char* opName = binaryOpName(op);

    // ---- 有一边是浮点，就整体按浮点算 ----
    // 这是唯一的"自动升级"：整数 2 和浮点 3.5 相加，
    // 结果当然是 5.5，没有第二种讲法。这也符合 C 的隐式转换规则。
    if (left.type == Value::Type::REAL || right.type == Value::Type::REAL) {
        const double a = left.toDouble();
        const double b = right.toDouble();

        switch (op) {
            case BinaryOp::ADD:      return Value::makeReal(a + b);
            case BinaryOp::SUBTRACT: return Value::makeReal(a - b);
            case BinaryOp::MULTIPLY: return Value::makeReal(a * b);
            case BinaryOp::DIVIDE:
                if (b == 0.0) throw AcornError("除数不能是 0");
                return Value::makeReal(a / b);
            case BinaryOp::MODULO:
                if (b == 0.0) throw AcornError("取余的除数不能是 0");
                // 用 fmod 而不是 % —— % 只能用在整数上。
                return Value::makeReal(std::fmod(a, b));
            default: break;
        }
    }

    // ---- 两边都是整数，结果就是整数 ----
    const long long a = left.integer;
    const long long b = right.integer;
    long long out = 0;

    // 【溢出必须查】
    // C++ 里 signed 整数溢出是**未定义行为**——标准说的是"未定义"，
    // 不是"回绕"。GCC 在 -O2 下真的会利用这一点做优化，
    // 所以 `a + b` 溢出之后，你连"得到一个负数"都不一定能指望。
    //
    // __builtin_add_overflow 这些是 GCC/Clang 的内建函数，
    // 算完顺便告诉你有没有溢出，比手写一圈边界判断清楚得多。
    //（本机是 TDM-GCC，能用。别的编译器上要换成手写判断。）
    switch (op) {
        case BinaryOp::ADD:
            if (__builtin_add_overflow(a, b, &out)) {
                throw AcornError(overflowMessage(opName));
            }
            return Value::makeInt(out);

        case BinaryOp::SUBTRACT:
            if (__builtin_sub_overflow(a, b, &out)) {
                throw AcornError(overflowMessage(opName));
            }
            return Value::makeInt(out);

        case BinaryOp::MULTIPLY:
            if (__builtin_mul_overflow(a, b, &out)) {
                throw AcornError(overflowMessage(opName));
            }
            return Value::makeInt(out);

        case BinaryOp::DIVIDE:
            if (b == 0) throw AcornError("除数不能是 0");
            // 单独这一格：INT64_MIN / -1 的正解是 2^63，刚好超出范围。
            // 不特判的话它就是未定义行为，而且**测不出来**——
            // 得刚好凑到那个数才会犯。
            if (a == LLONG_MIN && b == -1) {
                throw AcornError(overflowMessage(opName));
            }
            // C++ 的整数除法向零截断，SQL 标准也是这么定的。
            // 所以 7 / 2 = 3，-7 / 2 = -3。见 README 里的说明。
            return Value::makeInt(a / b);

        case BinaryOp::MODULO:
            if (b == 0) throw AcornError("取余的除数不能是 0");
            if (a == LLONG_MIN && b == -1) return Value::makeInt(0);
            return Value::makeInt(a % b);

        default:
            break;
    }

    throw AcornError("内部错误：算术运算走到了不该到的分支");
}

// ------------------------------------------------------------
//  排序用的比较
// ------------------------------------------------------------

// 【2026-09-24：这个函数搬去 value.cpp 了，变成了 compareForSort】
// 阶段 4 的 B+ 树也要按同一套规则排键。留在这里的话，
// 索引里的顺序和 ORDER BY 的顺序就是两份实现——它们一旦分叉，
// 表现是"走索引的查询和全表扫的查询给出不同的行序"，
// 而且只在数据量大到真用上索引时才出现。
//
// 规则本身没变：**NULL 最小**（跟 SQLite 走，Postgres 默认反过来）。
// 对着一列数据，你**想**注意到哪些是空的，所以升序让它冒头。

}  // namespace

// ============================================================
//  RowScope —— 用析构函数管"恢复现场"
// ============================================================

// 进这个作用域时，把执行器的"当前行"换成给定的这一行；
// 出去时（**不管是怎么出去的**）自动换回来。
//
// 关键在于析构函数一定会被调用：
//   · 正常执行完，离开作用域 → 调
//   · 中途抛异常，栈展开 → 也调
//
// 手写版本是这样的：
//     auto saved = currentRow_;
//     currentRow_ = &row;
//     ... 求值 ...
//     currentRow_ = saved;      // ← 上面抛异常的话，这行永远执行不到
//
// 一旦执行不到，执行器的"当前行"就永久停在那一行上，
// 后面所有求值全乱套。这和 Nova 里 EnvironmentGuard 解决的问题
// 一模一样——**作用域的本质就是对象的生命周期**，
// 这句话在这里是字面意思。
class Executor::RowScope {
public:
    RowScope(Executor& executor, const TableSchema& schema, const Row& row)
        : executor_(executor),
          savedSchema_(executor.currentSchema_),
          savedRow_(executor.currentRow_) {
        executor_.currentSchema_ = &schema;
        executor_.currentRow_ = &row;
    }

    ~RowScope() {
        executor_.currentSchema_ = savedSchema_;
        executor_.currentRow_ = savedRow_;
    }

    // 禁止拷贝。这个对象代表"一段作用域"，复制它没有任何意义，
    // 而且复制出来的那份析构时会多做一次恢复，把现场搞乱。
    RowScope(const RowScope&) = delete;
    RowScope& operator=(const RowScope&) = delete;

private:
    Executor& executor_;
    const TableSchema* savedSchema_;
    const Row* savedRow_;
};

// ============================================================
//  Executor
// ============================================================

Executor::Executor(Database& db) : db_(db) {}

ResultSet Executor::execute(const Stmt& stmt) {
    return stmt.accept(*this);
}

// ------------------------------------------------------------
//  CREATE TABLE
// ------------------------------------------------------------

ResultSet Executor::visitCreateTable(const CreateTableStmt& stmt) {
    std::vector<Column> columns;
    columns.reserve(stmt.columns.size());
    for (const ColumnDef& def : stmt.columns) {
        // 用 makeColumn 而不是手填，是为了自动算好 key（小写名）。
        // 见 schema.hpp 里的说明。
        columns.push_back(makeColumn(def.name, def.type));
    }

    db_.createTable(makeTableSchema(stmt.tableName, std::move(columns)));

    ResultSet result;
    result.message = "已创建表 `" + stmt.tableName + "`（" +
                     std::to_string(stmt.columns.size()) + " 列）";
    return result;
}

// ------------------------------------------------------------
//  INSERT
// ------------------------------------------------------------

ResultSet Executor::visitInsert(const InsertStmt& stmt) {
    size_t inserted = 0;
    for (const std::vector<ExprPtr>& exprRow : stmt.rows) {
        Row row;
        row.reserve(exprRow.size());

        for (const ExprPtr& expr : exprRow) {
            // 这里没有 RowScope，所以 currentRow_ 是 nullptr。
            // 用户要是在 VALUES 里写了列名（`VALUES (name)`），
            // visitColumn 会给出"这里没有正在读取的行"的报错——
            // 这正是我们想要的行为，不用额外写检查。
            row.push_back(expr->accept(*this));
        }

        // 列数和类型的检查在 Table::insert 里，见 table.hpp 的说明。
        // 校验不过会抛异常，那时 inserted 还没加，
        // 整条语句的效果就是"一行都没插进去"。
        //
        // 【走 db_ 而不是 table】因为这一行还要同时进这张表的
        // 每一个索引。见 database.hpp 里 "★ 执行器只该走这三个 ★"。
        db_.insertRow(stmt.tableName, std::move(row));
        inserted++;
    }

    ResultSet result;
    result.message = "插入了 " + std::to_string(inserted) + " 行";
    return result;
}

// ------------------------------------------------------------
//  SELECT
// ------------------------------------------------------------

ResultSet Executor::visitSelect(const SelectStmt& stmt) {
    const Table& table = db_.table(stmt.tableName);
    const TableSchema& schema = table.schema();

    ResultSet result;
    result.isQuery = true;

    // ---- 第一步：搞清楚这是不是一条聚合查询 ----
    bool aggregateQuery = false;
    for (const SelectItem& item : stmt.columns) {
        if (hasAggregate(*item.expr)) aggregateQuery = true;
    }

    // 聚合函数写在 WHERE 里讲不通：WHERE 是**逐行**判断的，
    // 那时候"一共有多少行"这个数还不存在，拿什么去比。
    // 不拦的话，走到 visitCall 会报一句"没有对应的累加器"——
    // 那是给写代码的人看的话，用户看不懂。
    if (stmt.where && hasAggregate(*stmt.where)) {
        throw AcornError(
            "聚合函数不能写在 WHERE 里。WHERE 是逐行判断的，"
            "那时候还没有一个总数可以拿来比。顺序是先筛选、再统计");
    }

    // ---- 第二步：准备聚合累加器 ----
    aggregateNodes_.clear();
    aggregateStates_.clear();

    if (aggregateQuery) {
        for (const SelectItem& item : stmt.columns) {
            collectAggregates(*item.expr, aggregateNodes_);

            // 每一项都必须是"聚合"或者"不含列的常量"。
            // `SELECT id, COUNT(*) FROM t` 里那个 id 想问
            // "哪一行的 id"——全体塌缩成一行之后没有答案。
            // （标准 SQL 里这要配 GROUP BY，而那是阶段 3 的事。）
            if (hasBareColumn(*item.expr)) {
                AstPrinter printer;
                throw AcornError(
                    "`" + printer.printExpr(*item.expr) +
                    "` 里既有聚合函数又有普通的列。一旦用了 COUNT/SUM "
                    "这类聚合函数，结果就从很多行塌缩成了一行，"
                    "那一行里的普通列指的到底是哪一行就没法回答了。"
                    "要么去掉聚合函数，要么把这一列也套上聚合");
            }
        }

        // 聚合套聚合是非法写法，在这里拦。
        for (const CallExpr* call : aggregateNodes_) {
            if (hasNestedAggregate(*call)) {
                throw AcornError("聚合函数不能套着写，`" + call->name +
                                 "(...)` 里面不能再放聚合函数");
            }
        }

        aggregateStates_.assign(aggregateNodes_.size(),
                                AggregateState{});
    }

    // ---- 第三步：决定表头 ----
    // 必须在扫描之前做完，而且**列名检查也要在这里做**。
    // 原因见上面 requireColumnsExist 的说明：空表一行都不扫，
    // 写错的列名就没机会被发现。
    if (stmt.selectAll) {
        for (const Column& column : schema.columns) {
            result.columnNames.push_back(column.name);
        }
    } else {
        AstPrinter printer;
        for (const SelectItem& item : stmt.columns) {
            requireColumnsExist(*item.expr, schema);

            // ---- 别名优先 ----
            // 写了 AS 就用别名，别的什么都不看。
            // 这是 `SELECT score * 2 AS 双倍` 存在的全部理由：
            // 不给聚合和表达式列起个能叫出口的名字，
            // `(count *)` 这种表头在别处就没法引用了。
            if (!item.alias.empty()) {
                result.columnNames.push_back(item.alias);
                continue;
            }

            const auto* columnExpr =
                dynamic_cast<const ColumnExpr*>(item.expr.get());
            if (columnExpr != nullptr) {
                // 是直接的列引用，表头就用**表结构里原本的列名**，
                // 而不是用户这次敲的样子。这样 `SELECT NAME FROM users`
                // 输出的表头仍然是 `name`——和表结构一致，
                // 也让同一个脚本每次跑出来的表头都一样。
                const int index = schema.indexOf(columnExpr->name);
                result.columnNames.push_back(
                    schema.columns[static_cast<size_t>(index)].name);
            } else {
                // 不是单纯的列引用（`score * 2` 这种）。
                // 没有天然的名字，就拿表达式本身的文本当表头，
                // 比如 `(* score 2)`。这里复用 AstPrinter——
                // 访问者模式的又一次白捡：打印器是用执行器完全无关的
                // 理由写出来的，这里却直接拿来用了。
                result.columnNames.push_back(printer.printExpr(*item.expr));
            }
        }
    }

    // ---- 第四步：WHERE 和 ORDER BY 里用到的列也要先验 ----
    // 它们的求值发生在扫描过程中，空表的话同样轮不到它们执行。
    if (stmt.where) {
        requireColumnsExist(*stmt.where, schema);
    }

    // ---- 第四步半：规划一下这次怎么扫 ----
    // 排在列名检查**后面**，是因为规划器会按列名去表结构里找下标，
    // 列名写错时它只能默默放弃——先让上面那句报出能看懂的话。
    //
    // evalConstant 传的是执行器自己：常量那边没有列引用，
    // 求值不需要"当前行"，所以在这里就能算。
    IndexPlan plan;
    planIndex(stmt.where.get(), schema, db_,
              [&](const Expr& expr) { return expr.accept(*this); }, plan);

    // ---- 别名的可见范围：只在 ORDER BY 里 ----
    //
    // `SELECT score * 2 AS 双倍 FROM t ORDER BY 双倍` 必须能用——
    // 给列起名字最实际的用处就是拿这个名字去排序。
    //
    // 【为什么不干脆让别名到处可见】
    // 标准 SQL 规定别名**只在 ORDER BY 里可见**，WHERE 里看不到。
    // 这不是随便定的：WHERE 在 SELECT 之前执行，
    // `SELECT score * 2 AS s FROM t WHERE s > 100` 里的 `s`
    // 在筛选的时候还不存在。
    // 所以这里只给 ORDER BY 开这个口子，WHERE 那边照旧报"没有叫 s 的列"。
    //
    // 【一个真的会咬人的歧义】
    // 如果别名和真实列名撞了（`SELECT score AS id, id FROM t ORDER BY id`），
    // **别名赢**——和标准 SQL 一致。用户想按真的 id 排就得写 `ORDER BY t.id`，
    // 而那个现在还不支持。所以这种写法在本项目里是"能跑但会出乎意料"，
    // 测在 alias.sql 里，让它的行为至少是**确定的**。
    std::map<std::string, const Expr*> aliasExprs;
    if (!stmt.selectAll) {
        for (const SelectItem& item : stmt.columns) {
            if (!item.alias.empty()) {
                // 键统一小写，因为标识符大小写不敏感：
                // `AS 双倍` 之后写 `ORDER BY 双倍` 和 `ORDER BY 双倍` 都该认。
                aliasExprs[lowercase(item.alias)] = item.expr.get();
            }
        }
    }

    // 把 ORDER BY 的键里"其实是个别名"的裸列名换成它背后的表达式。
    // 返回 nullptr 表示这不是别名，按普通列名处理。
    const auto resolveAlias = [&](const Expr& key) -> const Expr* {
        const auto* columnExpr = dynamic_cast<const ColumnExpr*>(&key);
        if (columnExpr == nullptr) return nullptr;
        const auto it = aliasExprs.find(lowercase(columnExpr->name));
        return it == aliasExprs.end() ? nullptr : it->second;
    };

    for (const OrderItem& item : stmt.orderBy) {
        // 别名指向的那个表达式在第三步已经验过了（它就是某个选择项），
        // 这里再验一遍反而会报"没有叫 s 的列"。
        if (resolveAlias(*item.key) != nullptr) continue;
        requireColumnsExist(*item.key, schema);
    }

    // ---- 第五步：扫一遍 ----
    // 注意这里是回调，不是"先取一个 vector 再遍历"。
    // 理由见 table.hpp 开头——那是为阶段 3 的磁盘分页留的接缝。
    //
    // 【阶段 4 之后这一行有了第二种走法】
    // scanWithPlan 看 plan：能走索引就走索引（拿到行号再回表），
    // 不能就还是全表扫。下面这段 lambda 两种走法**共用**——
    // 它只管"拿到一行"之后干什么，不管这一行是怎么来的。

    // 攒下来的结果行 + 每行的排序键。
    // 排序键必须在**扫描的时候**算出来：那时候当前行还在，
    // 扫描一结束 currentRow_ 就没了，再想求值 `ORDER BY score * 2`
    // 就没上下文了。这是个容易踩的坑——
    // 先收集行、排完序再算键，代码看起来一样，但就是跑不起来。
    struct Pending {
        Row cells;
        std::vector<Value> sortKeys;
    };
    std::vector<Pending> pending;

    scanWithPlan(db_, table, plan, [&](RowId, const Row& row) {
        // 换上"当前行"。出了这个 lambda 会自动换回来，
        // 哪怕下面抛异常也一样。见 RowScope 的说明。
        RowScope scope(*this, schema, row);

        if (stmt.where) {
            const Value condition = stmt.where->accept(*this);
            // 只有**确定是真**才留下。
            // 假和"不知道"（NULL）都不留——这就是
            // `WHERE age = NULL` 永远返回空表的原因。
            //
            // 【走索引的时候这一步更不能省】
            // 索引只负责"缩小候选集"，不负责给出最终答案：
            // 挑出来的那个条件只是 WHERE 的一**部分**，
            // 别的部分还没算。少了这一句，
            // `WHERE id = 1 AND name = 'x'` 会把所有 id = 1 的行
            // 全返回。而且不只是少一层过滤——多出来的行是真的
            // 多出来了，不报错。
            if (truthOf(condition) != Truth::IS_TRUE) {
                return;
            }
        }

        if (aggregateQuery) {
            // 聚合路径：不求值，只往累加器里塞。
            for (size_t i = 0; i < aggregateNodes_.size(); i++) {
                const CallExpr& call = *aggregateNodes_[i];

                if (call.star) {
                    // COUNT(*) —— 数的是"有几行"，不看任何一列的值。
                    // 所以哪怕整行全是 NULL 也照样算一行。
                    // 这正是 COUNT(*) 和 COUNT(某列) 的区别所在。
                    aggregateStates_[i].count++;
                    continue;
                }
                accumulateValue(i, call.argument->accept(*this));
            }
            return;
        }

        Pending item;

        if (stmt.selectAll) {
            item.cells = row;
        } else {
            item.cells.reserve(stmt.columns.size());
            for (const SelectItem& selectItem : stmt.columns) {
                item.cells.push_back(selectItem.expr->accept(*this));
            }
        }

        item.sortKeys.reserve(stmt.orderBy.size());
        for (const OrderItem& orderItem : stmt.orderBy) {
            // 是别名就换成它背后的表达式再求值。
            // `ORDER BY 双倍` 实际算的是 `score * 2`——
            // 复用同一个表达式意味着**排序键和显示出来的值必然一致**，
            // 不会出现"显示的是一回事、排序用的是另一回事"。
            const Expr* key = resolveAlias(*orderItem.key);
            if (key == nullptr) key = orderItem.key.get();
            item.sortKeys.push_back(key->accept(*this));
        }

        pending.push_back(std::move(item));
    });

    // ---- 第六步：聚合查询：把累加器算成一行 ----
    if (aggregateQuery) {
        // 注意这里**没有** RowScope：聚合结果不依赖任何一行。
        // 选择列表里剩下的部分只能是常量和聚合调用
        //（上面 hasBareColumn 已经验过了），所以不需要当前行。
        Row out;
        out.reserve(stmt.columns.size());
        for (const SelectItem& selectItem : stmt.columns) {
            out.push_back(selectItem.expr->accept(*this));
        }

        // 聚合查询**永远恰好返回一行**，哪怕表是空的。
        // `SELECT COUNT(*) FROM 空表` 的答案是 0，不是"没有结果"——
        // 这是个很容易搞错的地方，但它是对的：
        // "数一数有几行"这个问题，哪怕一行都没有，也有确定的答案。
        result.rows.push_back(std::move(out));
        return result;
    }

    // ---- 第七步：排序 ----
    if (!stmt.orderBy.empty()) {
        // 用 stable_sort 而不是 sort：排序键相同的行保持它们原本的
        // 相对顺序（也就是插入顺序）。sort 不保证这一点，
        // 于是同一个脚本跑两次可能得到不同的行序——
        // 对测试是灾难，对用户也是。
        //
        // 代价是 stable_sort 慢一点、费一点内存。这里完全值得：
        // 结果的**可复现**比那点性能重要得多。真到了要抠性能的时候，
        // 该换的是索引，不是这个。
        std::stable_sort(
            pending.begin(), pending.end(),
            [&](const Pending& a, const Pending& b) {
                for (size_t i = 0; i < stmt.orderBy.size(); i++) {
                    const int cmp = compareForSort(a.sortKeys[i],
                                                   b.sortKeys[i]);
                    if (cmp == 0) continue;  // 这一项分不出胜负，看下一项
                    return stmt.orderBy[i].ascending ? cmp < 0 : cmp > 0;
                }
                // 所有排序键都一样 → 谁也不比谁小。
                // 返回 false 让 stable_sort 保持原顺序。
                return false;
            });
    }

    // ---- 第八步：LIMIT / OFFSET ----
    // 顺序很重要：必须是**先排序再截断**。
    // 反过来就成了"先随便取几行、再把这几个排个序"，
    // 那不是 LIMIT 的意思（那是"取前 N 名"，前提是已经排好了）。
    //
    // 标准 SQL 里 OFFSET 只能跟着 LIMIT 出现，所以
    // limit < 0 时 offset 一定是没写的，不用单独处理。
    const long long total = static_cast<long long>(pending.size());
    long long begin = 0;
    long long end = total;

    if (stmt.limit >= 0) {
        begin = (stmt.offset > 0) ? stmt.offset : 0;
        // 用"减法"防溢出：`begin + limit` 在 limit 是 INT64_MAX 时
        // 会溢出成负数，然后 end < begin，一行都不返回——
        // 而且不报错。写成这样就没有加法。
        end = (stmt.limit > total - begin) ? total : begin + stmt.limit;

        // OFFSET 超出总行数时，begin > total，同样一行都不返回。
        // 这不是错误：`LIMIT 10 OFFSET 1000` 在一张 5 行的表上，
        // 正确答案就是"没有行"。
        if (begin > total) begin = total;
        if (end < begin) end = begin;
    }

    result.rows.reserve(static_cast<size_t>(end - begin));
    for (long long i = begin; i < end; i++) {
        result.rows.push_back(std::move(pending[static_cast<size_t>(i)].cells));
    }

    return result;
}

// ------------------------------------------------------------
//  聚合函数
// ------------------------------------------------------------

void Executor::accumulateValue(size_t index, const Value& value) {
    const std::string kind = lowercase(aggregateNodes_[index]->name);
    AggregateState& state = aggregateStates_[index];

    // ---- NULL 怎么处理 ----
    // 标准 SQL 规定：**所有聚合函数都忽略 NULL**，只有 COUNT(*) 例外
    //（它走另一条路，压根不到这里）。
    //
    // 这条最反直觉的后果是 AVG：它不是"和 / 行数"，
    // 而是"和 / **非 NULL 的个数**"。
    //   分数 90, NULL, 80  →  AVG 是 (90+80)/2 = 85，不是 170/3。
    // 这是对的："不知道他多少分"不该把平均分往下拉。
    //
    // 所以 count 在这里的含义是"非 NULL 的个数"，
    // 和 COUNT(*) 那个"行数"不是一回事。两者不会搞混——
    // COUNT(*) 从来不走这条路径。
    if (value.isNull()) return;

    state.sawNonNull = true;
    state.count++;

    // ---- COUNT ----
    // COUNT 到这儿就结束了：它要的只是"非 NULL 的个数"，
    // 上面那行 count++ 已经办完了，值本身是什么类型无所谓。
    //
    // 【这个提前返回是必须的，不是优化】
    // 没有它的话，COUNT 会一路掉到下面 SUM/AVG 的数字类型检查上，
    // 于是 `COUNT(name)` 会报"只能用在数字列上"——**这是错的**。
    // COUNT 对文本、布尔一样有意义。
    //（COUNT(数字列) 恰好不报错，因为它掉进了整数分支白白加了一通，
    //  加出来的 intSum 根本没人看。那种"能用"是撞上的，不是设计出来的。）
    if (kind == "count") return;

    // ---- MIN / MAX ----
    // 用 compareValues 比，所以数字、文本、布尔都能用，
    // 而且沿用了它的类型规则（不同类型之间不许比，直接报错）。
    if (kind == "min" || kind == "max") {
        if (state.best.isNull()) {
            // 第一个值直接收下。此时 best 是 NULL，
            // 没法拿它去 compareValues（会抛"不能比较"）。
            state.best = value;
            return;
        }
        const int cmp = compareValues(value, state.best);
        if ((kind == "min" && cmp < 0) || (kind == "max" && cmp > 0)) {
            state.best = value;
        }
        return;
    }

    // ---- SUM / AVG 共用的加法 ----
    // 整数一路加整数，见过浮点才整体转成浮点。
    // 这样 `SUM(id)` 是一列整数加起来得一列整数，
    // 而不是浮点——浮点超过 2^53 就开始丢精度，
    // 一列整数求和却算错，是最让人不能接受的那种 bug。
    if (value.type == Value::Type::REAL) {
        if (!state.sumIsReal) {
            // 从整数切换到浮点：把之前攒的整数和搬过去。
            // 漏了这一步的话，前面几行的整数就白加了。
            state.realSum = static_cast<double>(state.intSum);
            state.sumIsReal = true;
        }
        state.realSum += value.real;
        return;
    }

    if (value.type == Value::Type::INTEGER) {
        if (state.sumIsReal) {
            state.realSum += static_cast<double>(value.integer);
        } else {
            // ---- 整数求和要查溢出 ----
            // 和 evalArithmetic 里用的是同一个内建函数，理由也一样：
            // signed 溢出是未定义行为，不是"回绕成负数"。
            //
            // 【这里的报错和四则运算那条有什么不同】
            // 四则运算能指出是哪两个数加爆的，这里指不出来——
            // 攒到第几行爆的是个累积过程，没有"那两个数"。
            // 但**当前累计值和正在加的这个值**是有意义的：
            // 它告诉你表里哪一行的值大到能把它顶出去，
            // 顺着这个数去表里找，比"结果不对"好查得多
            //（说实话，通常就是某个值本身接近 2^63）。
            long long sum = 0;
            if (__builtin_add_overflow(state.intSum, value.integer, &sum)) {
                throw AcornError(
                    "`" + aggregateNodes_[index]->name +
                    "` 在整数范围内加爆了：累加到 " +
                    std::to_string(state.intSum) + " 时又加上了 " +
                    std::to_string(value.integer) +
                    "，超出了 64 位整数能表示的范围。"
                    "可以先把这一列转成浮点再求和（会损失精度），"
                    "或者分批统计");
            }
            state.intSum = sum;
        }
        return;
    }

    // 剩下的是 TEXT / BOOLEAN。SUM 和 AVG 对它们没意义。
    // MIN/MAX 在上面已经返回了，所以能走到这里的只有 SUM/AVG。
    throw AcornError("`" + aggregateNodes_[index]->name +
                     "` 只能用在数字列上，但这一列是" + value.typeName());
}

// ------------------------------------------------------------
//  表达式求值
// ------------------------------------------------------------

Value Executor::visitLiteral(const LiteralExpr& expr) {
    // 字面量不需要任何上下文，直接就是它自己。
    return expr.value;
}

Value Executor::visitColumn(const ColumnExpr& expr) {
    if (currentSchema_ == nullptr || currentRow_ == nullptr) {
        // 走到这里说明用户把列名写在了不该写的地方，最常见的是
        // `INSERT INTO users VALUES (name)` —— 这里没有"某一行"
        // 可以取 name，只有一串待插入的值，里面写列名讲不通。
        throw AcornError("`" + expr.name +
                         "` 是个列名，但这里没有正在读取的行，"
                         "不知道它指什么。VALUES 里只能写具体的值"
                         "（数字 / 字符串 / TRUE / FALSE / NULL）");
    }

    // 名字到下标的绑定。表名和列名的大小写都在
    // TableSchema::indexOf 里抹平了。
    const int index = currentSchema_->indexOf(expr.name);
    if (index < 0) {
        // 正常情况下走不到这里——visitSelect 已经用
        // requireColumnsExist 提前验过了。留着是防御性的：
        // 万一以后有别的路径直接求值列名，至少报的是一句人话。
        throw AcornError("表 `" + currentSchema_->name + "` 里没有叫 `" +
                         expr.name + "` 的列（有：" +
                         currentSchema_->columnList() + "）");
    }

    return (*currentRow_)[static_cast<size_t>(index)];
}

Value Executor::visitUnary(const UnaryExpr& expr) {
    // ---- IS NULL / IS NOT NULL ----
    // 【这两个是全 SQL 里唯一能"看" NULL 的运算符。】
    //
    // 别的运算碰到 NULL 都会被传染成 NULL：
    // `NULL = NULL` 是 NULL，`NULL + 1` 是 NULL。
    // 所以想判断一个值是不是 NULL，**没法用 `=`**——
    // `WHERE age = NULL` 永远返回空表，一行都不会有。
    //
    // 必须用 `IS NULL`。它直接检查"这个值是不是 NULL"，
    // 给出 TRUE 或 FALSE，而不是 NULL。
    //
    // 所以这个分支要放在 NOT 的前面，而且**绝不能**在求值
    // operand 之后先做 NULL 传染——那正是它要绕开的东西。
    if (expr.op == UnaryOp::IS_NULL || expr.op == UnaryOp::IS_NOT_NULL) {
        const Value operand = expr.operand->accept(*this);
        const bool isNull = operand.isNull();
        return Value::makeBool(expr.op == UnaryOp::IS_NULL ? isNull
                                                          : !isNull);
    }

    if (expr.op == UnaryOp::NOT) {
        const Truth truth = truthOf(expr.operand->accept(*this));
        if (truth == Truth::IS_NULL) {
            // NOT 不知道 = 还是不知道。
            // "不知道他是不是成年"取反之后，仍然是"不知道"。
            return Value::makeNull();
        }
        return Value::makeBool(truth == Truth::IS_FALSE);
    }

    // 负号
    const Value operand = expr.operand->accept(*this);
    if (operand.isNull()) return operand;  // -NULL 还是 NULL
    if (operand.type == Value::Type::INTEGER) {
        return Value::makeInt(-operand.integer);
    }
    if (operand.type == Value::Type::REAL) {
        return Value::makeReal(-operand.real);
    }
    throw AcornError("负号只能用在数字前面，不能用在" + operand.typeName() +
                     "上");
}

Value Executor::visitBinary(const BinaryExpr& expr) {
    // ---- 逻辑运算：三值逻辑（Kleene 逻辑）----
    //
    // 真值表（T = 真，F = 假，N = 不知道）：
    //
    //   AND  │ T  F  N          OR   │ T  F  N
    //   ─────┼─────────         ─────┼─────────
    //    T   │ T  F  N           T   │ T  T  T
    //    F   │ F  F  F           F   │ T  F  N
    //    N   │ N  F  N           N   │ T  N  N
    //
    // 看 AND 那一列：只有两边都确定是真才为真；
    // 只要有一边确定是假，整条就是假——**哪怕另一边不知道**。
    // 这一格（N AND F = F）是最能说明问题的地方：
    // "我不知道他几岁" AND "他不是人" 是假，因为后者已经定死了结论。
    //
    // 下面用提前 return 实现短路，和 Nova 里短路求值的道理一样：
    // 左边一旦定死结论，右边就不用算了。
    if (expr.op == BinaryOp::AND) {
        const Truth left = truthOf(expr.left->accept(*this));
        if (left == Truth::IS_FALSE) return Value::makeBool(false);  // 短路

        const Truth right = truthOf(expr.right->accept(*this));
        if (right == Truth::IS_FALSE) return Value::makeBool(false);
        if (left == Truth::IS_NULL || right == Truth::IS_NULL) {
            return Value::makeNull();
        }
        return Value::makeBool(true);
    }

    if (expr.op == BinaryOp::OR) {
        const Truth left = truthOf(expr.left->accept(*this));
        if (left == Truth::IS_TRUE) return Value::makeBool(true);  // 短路

        const Truth right = truthOf(expr.right->accept(*this));
        if (right == Truth::IS_TRUE) return Value::makeBool(true);
        if (left == Truth::IS_NULL || right == Truth::IS_NULL) {
            return Value::makeNull();
        }
        return Value::makeBool(false);
    }

    // ---- 算术运算 ----
    // 放在比较之前：算术和比较是两类运算，走两条完全不同的路
    //（一条产出数、一条产出布尔），中间没有任何交集，
    // 所以顺序不影响结果，只要别漏。
    switch (expr.op) {
        case BinaryOp::ADD:
        case BinaryOp::SUBTRACT:
        case BinaryOp::MULTIPLY:
        case BinaryOp::DIVIDE:
        case BinaryOp::MODULO:
            return evalArithmetic(expr.op, expr.left->accept(*this),
                                  expr.right->accept(*this));
        default:
            break;
    }

    // ---- 比较运算 ----
    const Value left = expr.left->accept(*this);
    const Value right = expr.right->accept(*this);

    // 任何一边是 NULL，结果就是 NULL。
    // 注意是 NULL，不是 FALSE——这个区别在 `NOT (a = b)` 里看得出来：
    // 要是这里给 FALSE，NOT 之后就成了 TRUE，一行会被错误地选中。
    // 给 NULL，NOT NULL 还是 NULL，行为才对。
    if (left.isNull() || right.isNull()) {
        return Value::makeNull();
    }

    const int cmp = compareValues(left, right);

    switch (expr.op) {
        case BinaryOp::EQUAL:         return Value::makeBool(cmp == 0);
        case BinaryOp::NOT_EQUAL:     return Value::makeBool(cmp != 0);
        case BinaryOp::LESS:          return Value::makeBool(cmp < 0);
        case BinaryOp::LESS_EQUAL:    return Value::makeBool(cmp <= 0);
        case BinaryOp::GREATER:       return Value::makeBool(cmp > 0);
        case BinaryOp::GREATER_EQUAL: return Value::makeBool(cmp >= 0);

        case BinaryOp::AND:
        case BinaryOp::OR:
        case BinaryOp::ADD:
        case BinaryOp::SUBTRACT:
        case BinaryOp::MULTIPLY:
        case BinaryOp::DIVIDE:
        case BinaryOp::MODULO:
            // 上面都已经提前返回了，走不到这里。
            // 保留这些 case 是为了让 -Wswitch 满意：
            // 以后加了新运算符却忘了在上面处理时，编译器会直接点名，
            // 而不是悄悄走到最后那句 return NULL 上去。
            break;
    }

    return Value::makeNull();
}

// ------------------------------------------------------------
//  聚合函数（求值）
// ------------------------------------------------------------

Value Executor::visitCall(const CallExpr& expr) {
    // 找到这个调用节点对应的累加器。
    //
    // 线性查找。看着笨，但聚合函数的个数就是选择列表的长度，
    // 撑死几十个，为它建一张哈希表不值得——而且节点是指针，
    // 哈希表还得自己写 hash，代码多出来一大截。
    // （和建表时查重复列名用 O(n²) 是同一个判断：
    //   规模小到一定程度，简单直接的写法就是最好的写法。）
    for (size_t i = 0; i < aggregateNodes_.size(); i++) {
        if (aggregateNodes_[i] != &expr) continue;

        const AggregateState& state = aggregateStates_[i];
        const std::string kind = lowercase(expr.name);

        if (kind == "count") {
            // COUNT 有个特例：**空集也返回 0，不是 NULL**。
            // "数一数有几个"哪怕一个都没有也是 0，这是确定的答案。
            //（COUNT(*) 用 star 那一路，count 在扫描时已经在加了。）
            return Value::makeInt(state.count);
        }

        // 剩下的 SUM / AVG / MIN / MAX 在空集上都返回 NULL——
        // "一堆空值的和是多少"这个问题没有答案，不是 0。
        // 这个区别很重要：如果 SUM 空集给 0，那
        // `SELECT SUM(x) FROM t WHERE 1=0` 和"真的和为 0"就分不开了。
        if (!state.sawNonNull) return Value::makeNull();

        if (kind == "sum") {
            return state.sumIsReal ? Value::makeReal(state.realSum)
                                   : Value::makeInt(state.intSum);
        }

        if (kind == "avg") {
            // AVG **一律返回浮点**，哪怕输入全是整数。
            // 整数除法会截断，`AVG` 一个 1 和一个 2 得到 1 就错了
            //（正确答案是 1.5）。这是 SUM 和 AVG 在类型上的分水岭：
            // SUM 要保持整数的精确，AVG 要保持除法的精确，两者不可兼得
            // 时各自选了自己更需要的那个。
            const double sum = state.sumIsReal
                                   ? state.realSum
                                   : static_cast<double>(state.intSum);
            // state.count 在这里是"非 NULL 的个数"（见 accumulateValue）。
            // 能走到这里说明 count > 0，不会除零。
            return Value::makeReal(sum / static_cast<double>(state.count));
        }

        if (kind == "min" || kind == "max") {
            return state.best;
        }

        // 语法分析阶段已经用白名单拦过了，正常走不到这里。
        throw AcornError("内部错误：`" + expr.name + "` 没有对应的聚合实现");
    }

    // 走到这里说明这个聚合调用没有配累加器。最可能的原因是
    // 它出现在了 SELECT 的结果列表以外的地方（比如 ORDER BY 或
    // UPDATE 的 SET 里），而那些地方没有扫描全表的过程。
    throw AcornError(
        "聚合函数 `" + expr.name +
        "` 只能出现在 SELECT 的结果列表里。它会先把整张表扫一遍，"
        "所以在 WHERE / ORDER BY / UPDATE 这些逐行处理的地方用不了");
}

// ------------------------------------------------------------
//  UPDATE
// ------------------------------------------------------------

ResultSet Executor::visitUpdate(const UpdateStmt& stmt) {
    Table& table = db_.table(stmt.tableName);
    const TableSchema& schema = table.schema();

    // ---- 先校验，再动手 ----
    // 和 SELECT 同样的道理，而且这里更要紧：UPDATE 是**写操作**。
    // 校验发现得晚，可能已经把前面几行改坏了。
    for (const Assignment& assignment : stmt.assignments) {
        if (schema.indexOf(assignment.column) < 0) {
            throw AcornError("表 `" + schema.name + "` 里没有叫 `" +
                             assignment.column + "` 的列（有：" +
                             schema.columnList() + "）");
        }
        requireColumnsExist(*assignment.value, schema);
    }
    if (stmt.where) {
        requireColumnsExist(*stmt.where, schema);
    }

    // 规划（和 SELECT 那边同一套）。理由见 planIndex 的说明。
    IndexPlan plan;
    planIndex(stmt.where.get(), schema, db_,
              [&](const Expr& expr) { return expr.accept(*this); }, plan);

    // ---- 第一步：扫一遍，算出每一行改成什么样 ----
    // 【为什么先全收集、再统一应用，而不是边扫边改】
    // 这是标准 SQL 的规定，而且它解决了一个很实际的问题：
    //
    //     UPDATE t SET a = b, b = a;      -- 交换两列
    //
    // 边扫边改的话，第一行改完 a 变成 b 的值，第二行再算 b = a
    // 就会拿**已经改过的** a 去算，结果两列都变成原来 b 的值。
    // 先全收集就不一样：所有赋值表达式求值时读的都是**原始行**，
    // 所以交换能正确工作。
    //
    // 注意 RowScope 绑的还是 row（原始行），不是 updated——
    // 这一行代码就是上面那段话的全部实现。
    std::vector<RowId> ids;
    std::vector<Row> updated;
    std::vector<size_t> targetIndex;  // 每个赋值目标列的下标，省得反复查

    targetIndex.reserve(stmt.assignments.size());
    for (const Assignment& assignment : stmt.assignments) {
        targetIndex.push_back(
            static_cast<size_t>(schema.indexOf(assignment.column)));
    }

    scanWithPlan(db_, table, plan, [&](RowId id, const Row& row) {
        RowScope scope(*this, schema, row);

        if (stmt.where) {
            if (truthOf(stmt.where->accept(*this)) != Truth::IS_TRUE) return;
        }

        Row next = row;  // 从原始行出发
        for (size_t i = 0; i < stmt.assignments.size(); i++) {
            // 同一个列被赋值两次（`SET a = 1, a = 2`）时后写的赢——
            // 因为这里是按顺序写的，后面的自然覆盖前面的。
            next[targetIndex[i]] = stmt.assignments[i].value->accept(*this);
        }

        ids.push_back(id);
        updated.push_back(std::move(next));
    });

    // ---- 第二步：统一应用 ----
    // 类型校验在 Table::updateRow 里，和 insert 走同一条路。
    // 中途有一行不合规就会抛异常，那时前面几行已经改完了——
    // **没有事务，这是阶段 5 要解决的问题**，README 里记着。
    //
    // 【走 db_ 而不是 table】因为一次改动要同时落在两处：
    // 表的数据页，和这张表上所有索引。交给 Database 去做，
    // 是因为它同时管着这两样东西——见 database.hpp 里
    // "★ 执行器只该走这三个 ★"那段。
    for (size_t i = 0; i < ids.size(); i++) {
        db_.updateRow(stmt.tableName, ids[i], std::move(updated[i]));
    }

    ResultSet result;
    result.message = "更新了 " + std::to_string(ids.size()) + " 行";
    return result;
}

// ------------------------------------------------------------
//  DELETE
// ------------------------------------------------------------

ResultSet Executor::visitDelete(const DeleteStmt& stmt) {
    Table& table = db_.table(stmt.tableName);
    const TableSchema& schema = table.schema();

    if (stmt.where) {
        requireColumnsExist(*stmt.where, schema);
    }

    IndexPlan plan;
    planIndex(stmt.where.get(), schema, db_,
              [&](const Expr& expr) { return expr.accept(*this); }, plan);

    // 先把要删的行号收集齐，再一次性删。
    //
    // 【为什么不能边扫边删】
    // 两个理由，第二个是硬性的：
    //   1. 扫描过程中修改容器，迭代器就失效了——这是规则层面不允许
    //   2. Table::scan 的回调收的是 `const Row&`，本来就改不了
    //
    // 收集行号还有个好处：删除的**数量**是确定的。
    // 边扫边删的话，"删了几行"得另外数一个计数器。
    std::vector<RowId> ids;

    scanWithPlan(db_, table, plan, [&](RowId id, const Row& row) {
        RowScope scope(*this, schema, row);

        if (stmt.where) {
            if (truthOf(stmt.where->accept(*this)) != Truth::IS_TRUE) return;
        }
        // 走索引时 scanWithPlan 已经把行号排过一遍了，所以这里
        // 和全表扫一样是从前往后的顺序。（阶段 2 的实现要求
        // 升序，那个要求后来消失了——见 table.hpp 里 deleteRows
        // 的说明。这里不再依赖顺序，但顺序仍然是确定的。）
        ids.push_back(id);
    });

    // 走 db_ 而不是 table：每一条的索引项都要跟着删，
    // 而"表 + 索引一起改"这件事由 Database 负责。
    db_.deleteRows(stmt.tableName, ids);

    ResultSet result;
    result.message = "删除了 " + std::to_string(ids.size()) + " 行";
    return result;
}

// ------------------------------------------------------------
//  DROP TABLE
// ------------------------------------------------------------

ResultSet Executor::visitDropTable(const DropTableStmt& stmt) {
    // 表在不在的检查、数据页怎么还、目录记录怎么改，全在 Database 里。
    // 【为什么执行器这边薄得只有一行】
    // 因为"删一张表"这件事和 SQL 没有一点关系——它是一个纯粹的
    // 存储层操作：把一串页还给空闲链，改一个字节的标志位。
    // 执行器该管的只有"哪张表"（那要查名字，属于 SQL 的语义），
    // 剩下的越权去管，就变成把存储细节抄进执行器里了。
    //
    // 这个分界在阶段 1 就定下来了（`Table::insert` 自己校验类型，
    // 而不是执行器校验），这里是同一个原则的又一次应用。
    db_.dropTable(stmt.tableName);

    ResultSet result;
    result.message = "删掉了表 `" + stmt.tableName + "`";
    return result;
}

// ------------------------------------------------------------
//  CREATE INDEX / DROP INDEX
// ------------------------------------------------------------

ResultSet Executor::visitCreateIndex(const CreateIndexStmt& stmt) {
    // 表得先存在——不然下面查列的时候报的是"没有这张表"，
    // 但用户看到的语句是建索引，得先告诉他问题出在表上。
    const TableSchema& schema = db_.schemaOf(stmt.tableName);  // 不存在会抛

    const int columnIndex = schema.indexOf(stmt.columnName);
    if (columnIndex < 0) {
        throw AcornError("表 `" + schema.name + "` 里没有叫 `" +
                         stmt.columnName + "` 的列（有：" +
                         schema.columnList() + "）");
    }

    // columnType 不用填：Database::createIndex 会从表结构里补，
    // 免得"索引记的列类型"和"表记的列类型"两个真相。
    IndexSchema index = makeIndexSchema(
        stmt.indexName, schema.name, schema.key, columnIndex,
        schema.columns[static_cast<size_t>(columnIndex)].type);

    db_.createIndex(std::move(index));

    ResultSet result;
    result.message = "已在表 `" + schema.name + "` 的列 `" +
                     schema.columns[static_cast<size_t>(columnIndex)].name +
                     "` 上建好索引 `" + stmt.indexName + "`";
    return result;
}

ResultSet Executor::visitDropIndex(const DropIndexStmt& stmt) {
    // 索引在不在、树怎么还、目录记录怎么改，全在 Database 里。
    // 和 visitDropTable 一样，执行器这边只负责"用户说的是哪个名字"。
    db_.dropIndex(stmt.indexName);

    ResultSet result;
    result.message = "删掉了索引 `" + stmt.indexName + "`";
    return result;
}

// ------------------------------------------------------------
//  BEGIN / COMMIT / ROLLBACK
// ------------------------------------------------------------
//
// 【为什么这三个这么短】因为事务是"页"那个层次的事，实现全在 Pager
//（回滚日志、提交顺序、缓存作废）。执行器这边只剩下**跟人说话**：
// 判断现在该不该开事务，以及把结果说成一句中文。
//
// 这不是偷懒，是分层正确：要是把事务逻辑写在这里，那"一条 UPDATE
// 改到一半失败"就没法回滚了——因为执行器根本不知道那次 UPDATE
// 改过哪些页。**回滚的能力必须长在"知道改了哪些页"的那一层。**
//
// 三个状态的检查都在执行器而不是 Database，理由只有一个：
// 报错要报人话。"已经在事务里了"和"没有事务可以提交"是一对
// 常见的糊涂账（尤其是刚学事务的人），值得各说一句。

ResultSet Executor::visitBegin(const BeginStmt& stmt) {
    if (db_.inTransaction()) {
        throw AcornError(
            "已经在一个事务里了，不能再来一个 BEGIN（事务不能嵌套）。"
            "要结束它，敲 COMMIT 提交、或者 ROLLBACK 撤销");
    }

    db_.begin();

    ResultSet result;
    result.message = "开始了事务（接下来的改动要 COMMIT 才算数）";
    return result;
}

ResultSet Executor::visitCommit(const CommitStmt& stmt) {
    if (!db_.inTransaction()) {
        throw AcornError("现在没有事务可以提交（要开事务得先敲 BEGIN）");
    }

    db_.commit();

    ResultSet result;
    result.message = "提交了事务，改动已经生效";
    return result;
}

ResultSet Executor::visitRollback(const RollbackStmt& stmt) {
    if (!db_.inTransaction()) {
        throw AcornError("现在没有事务可以回滚（要开事务得先敲 BEGIN）");
    }

    db_.rollback();

    ResultSet result;
    result.message = "回滚了事务，改动都退回去了";
    return result;
}

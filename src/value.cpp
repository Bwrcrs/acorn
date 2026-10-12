#include "value.hpp"

#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

#include "error.hpp"

// ------------------------------------------------------------
//  构造
// ------------------------------------------------------------

Value Value::makeNull() {
    return Value{};  // 默认就是 NULL_VALUE
}

Value Value::makeInt(long long value) {
    Value v;
    v.type = Type::INTEGER;
    v.integer = value;
    return v;
}

Value Value::makeReal(double value) {
    Value v;
    v.type = Type::REAL;
    v.real = value;
    return v;
}

Value Value::makeText(std::string value) {
    Value v;
    v.type = Type::TEXT;
    v.text = std::move(value);
    return v;
}

Value Value::makeBool(bool value) {
    Value v;
    v.type = Type::BOOLEAN;
    v.boolean = value;
    return v;
}

// ------------------------------------------------------------
//  转换
// ------------------------------------------------------------

double Value::toDouble() const {
    // 只有整数和浮点数两种能转。别的类型调用是调用方的错，
    // 说明在上层少了类型检查——所以这里直接抛，不返回个 0 蒙混过去。
    if (type == Type::INTEGER) return static_cast<double>(integer);
    if (type == Type::REAL) return real;
    throw AcornError("内部错误：toDouble() 被用在了" + typeName() + "上");
}

// ------------------------------------------------------------
//  打印
// ------------------------------------------------------------

std::string formatNumber(double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value > 0 ? "Inf" : "-Inf";

    // ---- 先判范围，再强转，顺序不能反 ----
    // 把一个超出 long long 范围的 double 强转成 long long 是
    // 未定义行为——标准说的是"未定义"，不是"得到某个数"。
    // 所以必须先确认在范围内。Nova 的 formatNumber 里踩过这个坑。
    const double LIMIT = 9.0e18;
    if (value >= -LIMIT && value <= LIMIT) {
        long long asInt = static_cast<long long>(value);
        if (static_cast<double>(asInt) == value) {
            return std::to_string(asInt);  // 3.0 -> "3"
        }
    }

    // std::to_string(double) 是固定 6 位小数，3.14 会变成 "3.140000"，
    // 所以自己用 ostringstream 控制精度。
    std::ostringstream out;
    out << std::setprecision(15) << value;
    return out.str();
}

std::string Value::toString() const {
    switch (type) {
        case Type::NULL_VALUE:
            // 大写 NULL，和 SQL 里的写法一致。这么写还有个好处：
            // 一眼能和字符串值 "NULL" 区分开——后者打出来不带引号，
            // 但在一列文本里出现时上下文是清楚的。
            return "NULL";
        case Type::INTEGER:
            return std::to_string(integer);
        case Type::REAL:
            return formatNumber(real);
        case Type::TEXT:
            return text;
        case Type::BOOLEAN:
            return boolean ? "true" : "false";
    }
    // 走到这里说明加了一种新类型但忘了在上面处理。
    // 故意不写 default，就是为了让 -Wswitch 在编译期把这件事喊出来
    //（这就是不用 std::variant 换来的好处之一）。
    return "?";
}

std::string Value::toSqlLiteral() const {
    if (type != Type::TEXT) {
        // 数字、布尔、NULL 的字面量写法和显示写法是一样的，直接复用。
        return toString();
    }

    // 文本：两边加单引号，里面的单引号写成两个。
    // 这就是 SQL 的转义规则——不是反斜杠。所以这里不需要处理 `\`，
    // 反斜杠在 SQL 字符串里就是个普通字符。
    std::string out = "'";
    for (char ch : text) {
        if (ch == '\'') out += "''";
        else out.push_back(ch);
    }
    out += "'";
    return out;
}

std::string Value::typeName() const {
    switch (type) {
        case Type::NULL_VALUE: return "NULL";
        case Type::INTEGER:    return "整数";
        case Type::REAL:       return "浮点数";
        case Type::TEXT:       return "文本";
        case Type::BOOLEAN:    return "布尔";
    }
    return "?";
}

// 注意下面 case 后面必须写全 Value::Type::——这是个自由函数，
// 不在 Value 类里面。写成 Type::INTEGER 编译器会说不认识 Type。
// （类里面的成员函数才能简写成 Type::。同一个枚举，
//   能不能简写取决于你在哪儿用，这是 C++ 作用域查找的规矩。）
const char* sqlTypeName(Value::Type type) {
    switch (type) {
        case Value::Type::NULL_VALUE: return "NULL";  // 列不可能是 NULL 类型，这里只为穷尽
        case Value::Type::INTEGER:    return "INT";
        case Value::Type::REAL:       return "REAL";
        case Value::Type::TEXT:       return "TEXT";
        case Value::Type::BOOLEAN:    return "BOOL";
    }
    return "?";
}

// ------------------------------------------------------------
//  类型检查
// ------------------------------------------------------------

Value fitToType(const Value& value, Value::Type columnType,
                const std::string& columnName) {
    // NULL 可以进任何列。
    // 因为 NULL 的意思是"不知道"，而"不知道"对任何类型都成立——
    // "我不知道他的年龄"和"年龄必须是整数"之间没有冲突。
    // （等以后加了 NOT NULL 约束，这一条就要多一个判断）
    if (value.isNull()) return value;

    if (value.type == columnType) return value;

    // 唯一允许的转换：整数放进浮点列。无损，所以放行。
    if (columnType == Value::Type::REAL && value.type == Value::Type::INTEGER) {
        return Value::makeReal(static_cast<double>(value.integer));
    }

    // 其余一律拒绝。报错信息里把三个关键信息都带上：
    // 哪一列、那列是什么类型、塞进去的是什么。
    // 只说"类型不匹配"的话，用户还得回去翻建表语句。
    throw AcornError("列 `" + columnName + "` 声明的是 " +
                     sqlTypeName(columnType) + "，装不下" + value.typeName() +
                     " " + value.toSqlLiteral());
}

// ------------------------------------------------------------
//  比较
// ------------------------------------------------------------

int compareValues(const Value& a, const Value& b) {
    // ---- 数值之间：先统一成 double 再比 ----
    // 整数和浮点数是可以互相比的，这是 SQL 的规矩，也是唯一
    // 允许的一处"隐式转换"。注意这里先判 isNumeric 再转，
    // 不能先转再判——文本调 toDouble() 会直接抛。
    if (a.isNumeric() && b.isNumeric()) {
        double x = a.toDouble();
        double y = b.toDouble();
        if (x < y) return -1;
        if (x > y) return 1;
        return 0;
    }

    // ---- 同类型之间 ----
    if (a.type == Value::Type::TEXT && b.type == Value::Type::TEXT) {
        // std::string 的 < 是按字节比的。对纯 ASCII 就是字典序；
        // 对中文是按 UTF-8 的字节序，和拼音顺序不一样。
        // 排序规则（collation）是个大话题，这里先用最简单的。
        if (a.text < b.text) return -1;
        if (a.text > b.text) return 1;
        return 0;
    }

    if (a.type == Value::Type::BOOLEAN && b.type == Value::Type::BOOLEAN) {
        int x = a.boolean ? 1 : 0;
        int y = b.boolean ? 1 : 0;
        return x - y;  // false(0) < true(1)
    }

    // ---- 剩下的都是不许比的 ----
    // 别家的数据库在这里往往会定一套"类型优先级"（比如 SQLite 规定
    // 数字永远小于文本），然后给你一个结果。我们选择报错。
    // 理由和 Nova 里 `1 + "a"` 不许隐式转换一样：静悄悄地给个答案，
    // 错误就会跑到离现场很远的地方才发作，到时候根本查不出来。
    throw AcornError("不能比较" + a.typeName() + "和" + b.typeName() +
                     "，SQL 里不同类型之间不做隐式转换");
}

int compareForSort(const Value& a, const Value& b) {
    // 【这是"NULL 排哪"的唯一一份实现】
    // ORDER BY 的全表扫路径用它，阶段 4 的 B+ 树也用它。
    // 详情见 value.hpp。
    const bool aNull = a.isNull();
    const bool bNull = b.isNull();
    if (aNull && bNull) return 0;
    if (aNull) return -1;  // NULL 最小
    if (bNull) return 1;
    return compareValues(a, b);
}

#pragma once

// ============================================================
//  报错
//
//  Acorn 里的错误分成两类，这个划分不是走形式，它决定了
//  "错误信息能不能带上位置"：
//
//  · SyntaxError —— 词法/语法分析阶段发现的问题。这时候手里
//    一定有 Token，所以**必然**知道是第几行第几列。
//    比如：少了个右括号、CREATE TABLE 后面没写表名。
//
//  · AcornError —— 执行阶段发现的问题。SQL 本身写得没毛病，
//    是执行不下去。这时候已经和原始文本脱钩了，硬要报位置
//    只能瞎猜，所以只给一句话。
//    比如：表不存在、往整数列里插字符串、拿整数和文本比大小。
//
//  为什么非要分开？因为"报错报得很准"是数据库好不好用的
//  分水岭之一。用户敲错一个字符，你告诉他"第 1 行第 24 列"，
//  他两秒钟就改完了；你只说"语法错误"，他得自己数。
// ============================================================

#include <stdexcept>
#include <string>

class AcornError : public std::runtime_error {
public:
    explicit AcornError(const std::string& message)
        : std::runtime_error(message) {}
};

// 「库现在被别的进程占着，这次没轮上」——阶段 6 加的。
//
// 【为什么值得单独一个类型，而不是一句普通的 AcornError】
// 因为它对应**另一个退出码**。
//
// 命令行上，退出码是脚本唯一看得见的东西（人看的是那句话，
// 脚本看的是那个数）。acorn 的约定是：
//   1 = SQL 写得不对          -> 重试一万次也是白搭
//   2 = 命令行/环境有问题      -> 等一会儿重试可能就好了
// "库被占着"属于第 2 种，可是它**被发现的位置**有两种：
//   · 开库的时候撞上（别人正独占着）—— 这时还没跑到 SQL，
//     自然落在"打开失败"那一类里，是 2
//   · 已经开着库、敲到 BEGIN 要升级成独占时才撞上 —— 这时
//     错误是从语句执行里冒出来的，默认会归到第 1 类
// 同一个问题、两个退出码，调用方就没法据此判断该不该重试了。
//
// 【为什么不靠比对错误文案】那等于把提示措辞当成 API：
// 以后有人把"写不了它"改成"不能写"，调用方就在没人改它的情况下坏了。
// 【为什么不用 AcornError 加个 bool 字段】那种字段总有人忘了设，
// 而忘了设的后果是静悄悄地报错成"你的 SQL 有问题"——正是最难查的那种。
// 一个独立的类型，忘不了：压根构造不出一个"看起来普通"的它。
class BusyError : public AcornError {
public:
    explicit BusyError(const std::string& message)
        : AcornError(message) {}
};

class SyntaxError : public AcornError {
public:
    SyntaxError(const std::string& message, int line, int column)
        : AcornError(message), line_(line), column_(column) {}

    int line() const { return line_; }
    int column() const { return column_; }

private:
    int line_;
    int column_;
};

// 「这段还没写完」——SyntaxError 的一个特例，专门给 REPL 用。
//
// 【为什么需要单独一个类型】
// REPL 是**一行一行**攒语句的，它必须回答一个问题：
// 用户敲到这儿，是写完了还是还在写？
//
// 光看"词法分析报没报错"答不出来：
//     SELECT * FROM t WHERE name = 'abc     ← 没写完，报警是错的
//     SELECT @ FROM t                       ← 写错了，就该现在报
// 两者都是 SyntaxError，混在一起要么误报、要么把真错误吞掉。
//
// 所以把"再往下敲几个字符就好了"的那几种情况单独标出来：
// 字符串没收尾、块注释没收尾。只有这几种在 REPL 里表示"继续攒"。
//
// 它继承 SyntaxError，所以所有 `catch (const SyntaxError&)`
// 的地方（脚本模式、-e 模式）行为完全不变——
// 在那些地方输入已经全部到手，没写完就是错了。
class IncompleteError : public SyntaxError {
public:
    IncompleteError(const std::string& message, int line, int column)
        : SyntaxError(message, line, column) {}
};

// 把语法错误渲染成人看的样子：一句话 + 出错那行的原文 + 指到列的箭头。
//
//    语法错误（第 1 行第 27 列）：少了一个右括号
//      SELECT * FROM users WHERE (id = 1;
//                                ^
//
// 需要整份源码是因为要把它那一行原样打出来——只给行列号，
// 用户还是得自己去数第几列。
std::string formatSyntaxError(const std::string& source,
                              const SyntaxError& error);

#pragma once

// ============================================================
//  Tokenizer —— 词法分析器
//
//  把一段 SQL 文本切成一串 Token。一个字符一个字符往前挪，
//  看当前字符是什么再决定下一步，这就是最简单的状态机。
//
//  和 Nova 的扫描器比，有三处是 SQL 特有的：
//
//  【一】关键字大小写不敏感。
//  `select` / `Select` / `SELECT` 是同一个东西。处理位置就在这里，
//  不能往后推——要是留给语法分析器，那每一层都得记得做一次，
//  漏掉一处就是个诡异的 bug。（顺带一提，表名和列名在 SQL 里的
//  大小写敏感性各家实现不一样，SQLite 是不敏感、PostgreSQL 是不敏感
//  除非加了引号，MySQL 看操作系统。这里统一按不敏感处理。）
//
//  【二】字符串用**单引号**，不是双引号。
//  而且字符串里的单引号靠**连写两个**来表示，不用反斜杠：
//      'it''s'      表示  it's
//  这跟 C / Nova 的 `\"` 是两套完全不同的习惯。
//  （双引号在标准 SQL 里是给标识符用的，比如列名里带空格时。
//   本项目不支持，见到双引号直接报错，免得用户以为能用。）
//
//  【三】列号按"字符"数，不按"字节"数。
//  中文在 UTF-8 里占 3 个字节。如果按字节数列号，报错时箭头
//  会指到隔壁去。这里的做法是：只在遇到字符的**首字节**时列号加一，
//  续字节（10xxxxxx）不算——见 advance()。
// ============================================================

#include <string>
#include <vector>

#include "token.hpp"

class Tokenizer {
public:
    explicit Tokenizer(std::string source);

    // 一路扫到底，返回全部 Token（末尾一定有一个 END_OF_FILE）。
    // 出错就抛 SyntaxError，里面带着行列号。
    std::vector<Token> scanTokens();

private:
    // ---- 字符级的工具 ----
    bool isAtEnd() const;
    char peek() const;          // 看当前字符，不移动
    char peekNext() const;      // 多看一眼下一个，用来分辨 >= 和 >
    char advance();             // 吃掉当前字符并移动
    bool match(char expected);  // 下一个正好是 expected 就吃掉，返回是否吃到了

    // ---- 每种东西怎么扫 ----
    void scanToken();
    void scanNumber();
    void scanIdentifier();
    void scanString();
    void skipLineComment();    // -- 到行尾
    void skipBlockComment();   // /* ... */

    // 把 [tokenStart_, current_) 这段原文包成一个 Token 收进去
    void addToken(TokenKind kind);
    void error(const std::string& message) const;  // 在 token 起点报错
    // 同上，但抛的是 IncompleteError：表示"再敲几个字符就好了"。
    // 只有字符串没收尾和块注释没收尾该用它，见 error.hpp。
    void incomplete(const std::string& message) const;

    std::string source_;
    std::vector<Token> tokens_;

    size_t current_ = 0;   // 下一个待处理字符的下标

    // 当前字符所在的行列（都是 1 起始）
    int line_ = 1;
    int column_ = 1;

    // 当前这个 Token 是从哪儿开始的。
    // 扫描一个 Token 的过程中游标会一路往前跑，跑到头的时候
    // 早就不知道它从哪开始了——所以必须在开始扫之前先记下来。
    size_t tokenStart_ = 0;
    int tokenLine_ = 1;
    int tokenColumn_ = 1;
};

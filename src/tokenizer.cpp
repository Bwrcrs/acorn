#include "tokenizer.hpp"

#include <cctype>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "error.hpp"

namespace {

// SQL 关键字表。
//
// 传进来的名字必须是**已经转成小写**的（调用方用 lowercase()，
// 见 token.hpp）——这就是"关键字大小写不敏感"的全部实现。
//
// 用 static 是为了只建一次表。函数内的 static 局部变量在 C++11 之后
// 是线程安全初始化的，不用担心。
//
// 用 unordered_map 而不是一长串 if：关键字会越加越多，
// 查表 O(1)，而且加新关键字只要在表里添一行。
//
// 顺便：一个词可以对应多个关键字，比如 INTEGER / INT 都是整数类型。
// 别名是 SQL 的历史包袱，但用户两种都可能写，得认。
TokenKind keywordKind(const std::string& name) {
    static const std::unordered_map<std::string, TokenKind> KEYWORDS = {
        {"create", TokenKind::CREATE},
        {"table",  TokenKind::TABLE},
        {"insert", TokenKind::INSERT},
        {"into",   TokenKind::INTO},
        {"values", TokenKind::VALUES},
        {"select", TokenKind::SELECT},
        {"from",   TokenKind::FROM},
        {"where",  TokenKind::WHERE},
        {"and",    TokenKind::AND},
        {"or",     TokenKind::OR},
        {"not",    TokenKind::NOT},
        {"null",   TokenKind::NULL_KW},
        {"true",   TokenKind::TRUE_KW},
        {"false",  TokenKind::FALSE_KW},
        {"int",     TokenKind::INT_TYPE},
        {"integer", TokenKind::INT_TYPE},
        {"real",    TokenKind::REAL_TYPE},
        {"float",   TokenKind::REAL_TYPE},
        {"double",  TokenKind::REAL_TYPE},
        {"text",    TokenKind::TEXT_TYPE},
        {"varchar", TokenKind::TEXT_TYPE},
        {"bool",    TokenKind::BOOL_TYPE},
        {"boolean", TokenKind::BOOL_TYPE},

        // ---- 阶段 2 加的 ----
        {"update", TokenKind::UPDATE},
        {"set",    TokenKind::SET},
        {"delete", TokenKind::DELETE},
        // ORDER 变成关键字了。这不是随手加的——阶段 1 的 README 里
        // 就把这件事记成了一颗雷：在那之前 `order` 能当表名用。
        // 而现在 `SELECT * FROM t ORDER BY x` 里的 ORDER 必须被认成
        // 关键字，否则会被解析成"从表 t、表 order、表 by 里查"，
        // 而且**不报错**，给你一个莫名其妙的结果。
        // 代价是叫 `order` 的表名现在建不了了——这正是"保留字"的含义。
        {"order",  TokenKind::ORDER},
        {"by",     TokenKind::BY},
        {"as",     TokenKind::AS},
        {"asc",    TokenKind::ASC},
        {"desc",   TokenKind::DESC},
        {"limit",  TokenKind::LIMIT},
        {"offset", TokenKind::OFFSET},
        {"is",     TokenKind::IS},

        // ---- 阶段 3 加的 ----
        {"drop",   TokenKind::DROP},

        // ---- 阶段 4 加的 ----
        // 又是两个保留字，代价和 ORDER / DROP 一样：叫 index 或 on
        // 的表再建不出来了。这笔账值得算一遍——
        //
        // 【INDEX】如果它不算关键字，`CREATE INDEX i ON t (c)` 里的
        // INDEX 会被当成表名，于是语法分析器会试着解析
        // "CREATE 表名叫 INDEX，然后..."，报出来的错会非常莫名其妙。
        // 而且 DROP INDEX 同理。必须加。
        //
        // 【ON】是 INDEX 那一句里的连接词，同一个道理。
        // 但它比 INDEX 更冤：`on` 在别的 SQL 里是个挺常见的列名
        //（开关状态），而且它只在 `CREATE INDEX` 那一个位置上有用。
        // 真数据库（SQLite）也是把 ON 设成保留字的，所以这个代价
        // 大家一样付。备选方案是"在 CREATE INDEX 后面允许用引号
        // 括起来的标识符"——那要引入一套新的词法规则，
        // 为一个词名付这个价钱不划算。
        {"index",  TokenKind::INDEX},
        {"on",     TokenKind::ON},

        // ---- 阶段 5 加的 ----
        // 三个事务语句。都是整句，后面直接跟分号。
        //
        // 【START TRANSACTION 这个别名没加】标准 SQL 里它是 BEGIN 的同义词，
        // 但它要 START 和 TRANSACTION 两个词一起才认得出，也就是
        // 一口气要多吃两个保留字。为一个"另一种写法"付这个价钱不值——
        // 想加的话，等真有人抱怨 BEGIN 不好写再说。
        {"begin",    TokenKind::BEGIN},
        {"commit",   TokenKind::COMMIT},
        {"rollback", TokenKind::ROLLBACK},
    };

    auto it = KEYWORDS.find(name);
    if (it == KEYWORDS.end()) return TokenKind::IDENTIFIER;
    return it->second;
}

// 能当标识符的开头吗？
// 注意这里收的是 unsigned char——调用方必须先转好再传进来。
bool isIdentifierStart(unsigned char byte) {
    // byte >= 0x80 是 UTF-8 里的多字节字符，也就是中文之类。
    // 允许它，表名和列名就能用中文。
    //
    // 【必须转 unsigned char】char 在本机是有符号的，0xE6 会被
    // 当成负数，`byte >= 0x80` 永远不成立——表现就是"中文表名认不出来"。
    // Nova 的扫描器踩过这个坑，这里提前避开。
    return std::isalpha(byte) != 0 || byte == '_' || byte >= 0x80;
}

bool isIdentifierPart(unsigned char byte) {
    return std::isalnum(byte) != 0 || byte == '_' || byte >= 0x80;
}

}  // namespace

Tokenizer::Tokenizer(std::string source) : source_(std::move(source)) {}

std::vector<Token> Tokenizer::scanTokens() {
    while (!isAtEnd()) {
        // 每个 Token 开始之前，先把它的起点记下来
        tokenStart_ = current_;
        tokenLine_ = line_;
        tokenColumn_ = column_;
        scanToken();
    }

    // 收尾。补一个 END_OF_FILE，语法分析器就不用老问"还有没有内容"了
    tokenStart_ = current_;
    tokenLine_ = line_;
    tokenColumn_ = column_;
    addToken(TokenKind::END_OF_FILE);

    return tokens_;
}

// ------------------------------------------------------------
//  字符级工具
// ------------------------------------------------------------

bool Tokenizer::isAtEnd() const {
    return current_ >= source_.size();
}

char Tokenizer::peek() const {
    if (isAtEnd()) return '\0';
    return source_[current_];
}

char Tokenizer::peekNext() const {
    if (current_ + 1 >= source_.size()) return '\0';
    return source_[current_ + 1];
}

char Tokenizer::advance() {
    char c = source_[current_++];

    if (c == '\n') {
        // Windows 上换行是 "\r\n" 两个字符。这里 '\r' 被当成普通空白
        // 跳过了（见 scanToken），真正涨行号的是 '\n'。
        line_++;
        column_ = 1;
        return c;
    }

    // ---- 列号只在"字符的首字节"上加一 ----
    // UTF-8 的续字节长这样：10xxxxxx，也就是 (b & 0xC0) == 0x80。
    // 它属于上一个字符，不能单独算一列。这样写的好处是不用管
    // 这个字符到底是 1 字节还是 3 字节，规则自动就对了。
    unsigned char byte = static_cast<unsigned char>(c);
    if ((byte & 0xC0) != 0x80) {
        column_++;
    }
    return c;
}

bool Tokenizer::match(char expected) {
    if (isAtEnd()) return false;
    if (source_[current_] != expected) return false;
    advance();
    return true;
}

void Tokenizer::addToken(TokenKind kind) {
    Token token;
    token.kind = kind;
    token.lexeme = source_.substr(tokenStart_, current_ - tokenStart_);
    token.line = tokenLine_;
    token.column = tokenColumn_;
    tokens_.push_back(std::move(token));
}

void Tokenizer::error(const std::string& message) const {
    // 用 token 的起点报错，不是用当前游标——用户关心的是
    // "这个词从哪开始写错的"，不是"扫到第几个字符才发现不对"。
    throw SyntaxError(message, tokenLine_, tokenColumn_);
}

// 和 error() 一样，只是换成"这段还没写完"那个类型。
// 只有字符串和块注释没收尾这两种情况该用——见 error.hpp 的说明。
void Tokenizer::incomplete(const std::string& message) const {
    throw IncompleteError(message, tokenLine_, tokenColumn_);
}

// ------------------------------------------------------------
//  主循环
// ------------------------------------------------------------

void Tokenizer::scanToken() {
    char c = advance();

    switch (c) {
        // ---- 标点 ----
        case '(': addToken(TokenKind::LEFT_PAREN); return;
        case ')': addToken(TokenKind::RIGHT_PAREN); return;
        case ',': addToken(TokenKind::COMMA); return;
        case ';': addToken(TokenKind::SEMICOLON); return;
        case '.': addToken(TokenKind::DOT); return;

        // `*` 有两种身份：SELECT * 里的通配符，和乘号。
        // 词法分析器不分——两种情况都是这一个 Token，
        // 到底哪个意思由语法分析器看上下文决定。
        case '*': addToken(TokenKind::STAR); return;

        case '+': addToken(TokenKind::PLUS); return;
        case '%': addToken(TokenKind::PERCENT); return;

        case '-':
            // `--` 是行注释。先看一眼后面是不是还有一个减号，
            // 是的话整行都不要了。
            if (peek() == '-') {
                skipLineComment();
                return;
            }
            addToken(TokenKind::MINUS);
            return;

        case '/':
            // `/` 也有两种身份，但这里的歧义是**真的**歧义：
            // `/*` 是注释的开头，`/` 单独一个是除号。
            // 和 `*` 不一样，`*` 那两种身份词法上长得一模一样，
            // 这一个必须在这里看下一个字符才能定。
            if (peek() == '*') {
                advance();  // 吃掉 '*'
                skipBlockComment();
                return;
            }
            addToken(TokenKind::SLASH);
            return;

        // ---- 比较运算符 ----
        case '=':
            // SQL 里 `=` 就是"相等"，没有"赋值"。所以不需要像 Nova
            // 那样再分辨一次后面跟不跟 `=`。
            addToken(TokenKind::EQUAL);
            return;

        case '!':
            // 标准 SQL 里 `!=` 其实不是标准写法（标准是 `<>`），
            // 但所有人都写 `!=`，两种都收。
            if (match('=')) {
                addToken(TokenKind::NOT_EQUAL);
                return;
            }
            error("单独的 `!` 没有意义，判断不等请写 `!=` 或 `<>`");
            return;

        case '<':
            // `<>` 和 `!=` 同义，扫成同一个 Token，后面就不用管了
            if (match('=')) { addToken(TokenKind::LESS_EQUAL); return; }
            if (match('>')) { addToken(TokenKind::NOT_EQUAL); return; }
            addToken(TokenKind::LESS);
            return;

        case '>':
            if (match('=')) { addToken(TokenKind::GREATER_EQUAL); return; }
            addToken(TokenKind::GREATER);
            return;

        // ---- 字面量 ----
        case '\'':
            scanString();
            return;

        // ---- 空白 ----
        // '\r' 在这里被吃掉：Windows 的换行是 "\r\n"，
        // 行号由 advance() 里的 '\n' 负责涨。
        case ' ':
        case '\t':
        case '\r':
        case '\n':
            return;

        default:
            break;
    }

    if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
        scanNumber();
        return;
    }

    if (isIdentifierStart(static_cast<unsigned char>(c))) {
        scanIdentifier();
        return;
    }

    if (c == '"') {
        // 标准 SQL 里双引号是给标识符用的（列名带空格时才需要）。
        // 本项目不支持，但必须给一句人话——不然用户会以为
        // "双引号和单引号都能当字符串用"，然后莫名其妙地报错。
        error("SQL 里字符串要用单引号，不是双引号。应该写成 'abc' 而不是 \"abc\"");
        return;
    }

    error(std::string("认不出来的字符 `") + c + "`");
}

// ------------------------------------------------------------
//  数字
// ------------------------------------------------------------

void Tokenizer::scanNumber() {
    while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
        advance();
    }

    bool isReal = false;

    // ---- 小数点要往后看一眼 ----
    // `t.col` 里的点是"表名.列名"的分隔符，不是小数点。
    // 靠"点后面跟的是不是数字"就能分辨：
    //   3.14  -> 点后面是数字，是小数点
    //   t.col -> 点后面是字母，是分隔符
    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peekNext())) != 0) {
        isReal = true;
        advance();  // 吃掉 '.'
        while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
            advance();
        }
    }

    std::string text = source_.substr(tokenStart_, current_ - tokenStart_);

    if (isReal) {
        double value = 0.0;
        try {
            value = std::stod(text);
        } catch (const std::out_of_range&) {
            // std::stod 碰到超出 double 范围的数字会抛 std::out_of_range。
            // 它是标准库异常，不是 AcornError——不接住的话会直接穿透
            // 最外层的 catch，用户看到的就是一句 "terminate called..."，
            // 而不是"这个数字太大了"。（Nova 踩过同一个坑）
            error("这个数字超出了能表示的范围：" + text);
        } catch (const std::invalid_argument&) {
            error("这个数字不合法：" + text);
        }
        addToken(TokenKind::REAL);
        tokens_.back().realValue = value;
        return;
    }

    long long value = 0;
    try {
        value = std::stoll(text);
    } catch (const std::out_of_range&) {
        error("这个整数太大了，超出 64 位整数范围：" + text);
    } catch (const std::invalid_argument&) {
        error("这个整数不合法：" + text);
    }
    addToken(TokenKind::INTEGER);
    tokens_.back().integerValue = value;
}

// ------------------------------------------------------------
//  标识符 / 关键字
// ------------------------------------------------------------

void Tokenizer::scanIdentifier() {
    while (isIdentifierPart(static_cast<unsigned char>(peek()))) {
        advance();
    }

    std::string text = source_.substr(tokenStart_, current_ - tokenStart_);

    // ---- 大小写不敏感就实现在这一行 ----
    // 转成小写拿去查关键字表。但 Token 里存下来的 lexeme 仍然是
    // 用户原本写的样子——报错时显示的是用户真正敲的东西，
    // 而不是被规范化过的版本。用户写 `Select` 打错了，
    // 你回他一句 `SELECT` 会让他以为自己没打错。
    //
    // addToken 存下来的 lexeme 就是原文，查表、查列都用它，
    // 不需要再往 textValue 里存一份——同一个东西存两处，
    // 早晚会有一处忘了更新。
    addToken(keywordKind(lowercase(text)));
}

// ------------------------------------------------------------
//  字符串
// ------------------------------------------------------------

void Tokenizer::scanString() {
    // 进来的时候，开头的那个单引号已经被 advance() 吃掉了
    std::string value;

    while (true) {
        if (isAtEnd()) {
            incomplete("字符串没有收尾的单引号");
        }

        char c = advance();

        if (c == '\'') {
            // ---- 连写两个单引号 = 一个字面单引号 ----
            //   'it''s'   表示  it's
            // 这是 SQL 标准的规定，和 C / Nova 用反斜杠的 `\'` 是
            // 两套完全不同的习惯。所以 SQL 的字符串里，
            // 反斜杠就是个普通字符，不需要转义。
            if (peek() == '\'') {
                advance();  // 吃掉第二个引号
                value.push_back('\'');
                continue;
            }
            break;  // 单个引号，字符串到这儿结束
        }

        value.push_back(c);
    }

    addToken(TokenKind::STRING);
    tokens_.back().textValue = value;
}

// ------------------------------------------------------------
//  注释
// ------------------------------------------------------------

void Tokenizer::skipLineComment() {
    // 一路吃着走到行尾。'\n' 不吃——留给主循环去涨行号。
    while (!isAtEnd() && peek() != '\n') {
        advance();
    }
}

void Tokenizer::skipBlockComment() {
    while (true) {
        if (isAtEnd()) {
            incomplete("块注释 /* 没有收尾的 */");
        }
        if (peek() == '*' && peekNext() == '/') {
            advance();  // '*'
            advance();  // '/'
            return;
        }
        advance();
    }
}

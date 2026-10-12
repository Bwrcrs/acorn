#include "token.hpp"

#include <cctype>
#include <string>

#include "value.hpp"  // formatNumber

std::string lowercase(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        // 必须先转 unsigned char 再交给 std::tolower。
        // char 在本机是**有符号**的，中文的字节（如 0xE6）会变成负数，
        // 而 std::tolower 的参数要求是"能表示成 unsigned char 的值或 EOF"，
        // 传负数进去是未定义行为。Nova 的扫描器踩过同一个坑。
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

bool isTypeKeyword(TokenKind kind) {
    return kind == TokenKind::INT_TYPE || kind == TokenKind::REAL_TYPE ||
           kind == TokenKind::TEXT_TYPE || kind == TokenKind::BOOL_TYPE;
}

const char* tokenKindName(TokenKind kind) {
    switch (kind) {
        case TokenKind::LEFT_PAREN:    return "LEFT_PAREN";
        case TokenKind::RIGHT_PAREN:   return "RIGHT_PAREN";
        case TokenKind::COMMA:         return "COMMA";
        case TokenKind::SEMICOLON:     return "SEMICOLON";
        case TokenKind::STAR:          return "STAR";
        case TokenKind::DOT:           return "DOT";
        case TokenKind::PLUS:          return "PLUS";
        case TokenKind::MINUS:         return "MINUS";
        case TokenKind::SLASH:         return "SLASH";
        case TokenKind::PERCENT:       return "PERCENT";

        case TokenKind::EQUAL:         return "EQUAL";
        case TokenKind::NOT_EQUAL:     return "NOT_EQUAL";
        case TokenKind::LESS:          return "LESS";
        case TokenKind::LESS_EQUAL:    return "LESS_EQUAL";
        case TokenKind::GREATER:       return "GREATER";
        case TokenKind::GREATER_EQUAL: return "GREATER_EQUAL";

        case TokenKind::IDENTIFIER:    return "IDENTIFIER";
        case TokenKind::INTEGER:       return "INTEGER";
        case TokenKind::REAL:          return "REAL";
        case TokenKind::STRING:        return "STRING";

        // 关键字就返回它本来的写法，比返回 "SELECT_KW" 更好认
        case TokenKind::CREATE:        return "CREATE";
        case TokenKind::TABLE:         return "TABLE";
        case TokenKind::INSERT:        return "INSERT";
        case TokenKind::INTO:          return "INTO";
        case TokenKind::VALUES:        return "VALUES";
        case TokenKind::SELECT:        return "SELECT";
        case TokenKind::FROM:          return "FROM";
        case TokenKind::WHERE:         return "WHERE";
        case TokenKind::AND:           return "AND";
        case TokenKind::OR:            return "OR";
        case TokenKind::NOT:           return "NOT";
        case TokenKind::NULL_KW:       return "NULL";
        case TokenKind::TRUE_KW:       return "TRUE";
        case TokenKind::FALSE_KW:      return "FALSE";
        case TokenKind::INT_TYPE:      return "INT";
        case TokenKind::REAL_TYPE:     return "REAL";
        case TokenKind::TEXT_TYPE:     return "TEXT";
        case TokenKind::BOOL_TYPE:     return "BOOL";
        case TokenKind::UPDATE:        return "UPDATE";
        case TokenKind::SET:           return "SET";
        case TokenKind::DELETE:        return "DELETE";
        case TokenKind::DROP:          return "DROP";
        case TokenKind::INDEX:         return "INDEX";
        case TokenKind::ON:            return "ON";
        case TokenKind::BEGIN:         return "BEGIN";
        case TokenKind::COMMIT:        return "COMMIT";
        case TokenKind::ROLLBACK:      return "ROLLBACK";
        case TokenKind::ORDER:         return "ORDER";
        case TokenKind::BY:            return "BY";
        case TokenKind::AS:            return "AS";
        case TokenKind::ASC:           return "ASC";
        case TokenKind::DESC:          return "DESC";
        case TokenKind::LIMIT:         return "LIMIT";
        case TokenKind::OFFSET:        return "OFFSET";
        case TokenKind::IS:            return "IS";

        case TokenKind::END_OF_FILE:   return "END_OF_FILE";
    }
    // 故意不写 default：这样加了新 Token 种类却忘了在这里处理时，
    // -Wswitch 会在编译期直接点名，而不是运行时打出一个 "?"。
    return "?";
}

std::string tokenToString(const Token& token) {
    std::string out = "<";
    out += tokenKindName(token.kind);

    // 字面量把"解析出来的值"也带上，这样一眼能看出词法分析器
    // 有没有把它认对：`42` 应该既是 INTEGER 又带着 42。
    switch (token.kind) {
        case TokenKind::INTEGER:
            out += " " + std::to_string(token.integerValue);
            break;
        case TokenKind::REAL:
            out += " " + formatNumber(token.realValue);
            break;
        case TokenKind::STRING:
            out += " \"" + token.textValue + "\"";
            break;
        case TokenKind::IDENTIFIER:
            out += " " + token.lexeme;
            break;
        default:
            break;
    }

    out += ">  @ " + std::to_string(token.line) + ":" +
           std::to_string(token.column);
    return out;
}

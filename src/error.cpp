#include "error.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include "text.hpp"

namespace {

// 把整份源码按行切开。
// 顺手干掉行尾的 '\r'：Windows 上换行是 "\r\n" 两个字符，
// 不剥掉的话原样打出来光标会被顶回行首，后面的箭头就全错位了。
std::vector<std::string> splitLines(const std::string& source) {
    std::vector<std::string> lines;
    std::string current;

    for (char ch : source) {
        if (ch == '\n') {
            if (!current.empty() && current.back() == '\r') current.pop_back();
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    // 最后一行可能没有换行符收尾，别漏了
    if (!current.empty()) {
        if (current.back() == '\r') current.pop_back();
        lines.push_back(current);
    }
    return lines;
}

}  // namespace

std::string formatSyntaxError(const std::string& source,
                              const SyntaxError& error) {
    std::string out = "语法错误（第 " + std::to_string(error.line()) +
                      " 行第 " + std::to_string(error.column()) +
                      " 列）：" + error.what();

    std::vector<std::string> lines = splitLines(source);

    // 位置越界就别硬渲染了，光给一句话也比打出一堆乱码强。
    if (error.line() < 1 || error.line() > static_cast<int>(lines.size())) {
        return out;
    }

    const std::string& line = lines[static_cast<size_t>(error.line() - 1)];

    // 缩进两格，让箭头行和源码行的左边距一致
    out += "\n  " + line + "\n  ";

    // 箭头要往右挪的格数 = 出错位置**之前**那段的显示宽度。
    // 注意不是 column - 1 ——那是字符数，碰上中文会比实际格数少，
    // 箭头就会指在字符中间。见 text.hpp 开头那段说明。
    const int charsBefore = error.column() - 1;
    const int indent =
        charsBefore > 0 ? displayWidth(line, static_cast<size_t>(charsBefore)) : 0;
    out.append(static_cast<size_t>(indent), ' ');
    out += "^";

    return out;
}

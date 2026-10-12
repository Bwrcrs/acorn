#include "text.hpp"

#include <cstddef>
#include <string>

int displayWidth(const std::string& text, size_t charCount) {
    int width = 0;
    size_t chars = 0;
    size_t i = 0;

    while (i < text.size() && chars < charCount) {
        // 必须转成 unsigned char 再比大小。
        // char 在本机是**有符号**的，中文的 0xE6 会被当成负数，
        // `byte >= 0xE0` 永远不成立——结果就是中文被当成单字节，
        // 宽度算成 1，表格全部错位。
        // （Nova 的扫描器在这个坑里栽过一次）
        const unsigned char byte = static_cast<unsigned char>(text[i]);

        // UTF-8 的首字节自带长度信息：
        //   0xxxxxxx                            1 字节
        //   110xxxxx 10xxxxxx                   2 字节
        //   1110xxxx 10xxxxxx 10xxxxxx          3 字节  ← 中文在这个区间
        //   11110xxx 10xxxxxx 10xxxxxx 10xxxxxx 4 字节
        size_t len = 1;
        if (byte >= 0xF0)      len = 4;
        else if (byte >= 0xE0) len = 3;
        else if (byte >= 0xC0) len = 2;

        width += (len >= 3) ? 2 : 1;
        i += len;
        ++chars;
    }
    return width;
}

int displayWidth(const std::string& text) {
    // 上界给 text.size()：字节数一定 >= 字符数，所以肯定够数，
    // 循环会在走到字符串末尾时自然停下。
    return displayWidth(text, text.size());
}

std::string padRight(const std::string& text, int width) {
    const int current = displayWidth(text);
    if (current >= width) return text;
    return text + std::string(static_cast<size_t>(width - current), ' ');
}

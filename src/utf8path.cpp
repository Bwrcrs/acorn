#include "utf8path.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

std::FILE* openPath(const std::string& path, const char* mode) {
#ifdef _WIN32
    // mode 本身永远是纯 ASCII（"rb"、"w+b" 这种），所以直接
    // 一个字符一个字符地转成宽字符就够了——不劳 utf8ToWide。
    std::wstring widePath = utf8ToWide(path);
    if (widePath.empty()) return nullptr;

    std::wstring wideMode;
    for (const char* p = mode; *p != '\0'; ++p) {
        wideMode.push_back(static_cast<wchar_t>(*p));
    }

    return _wfopen(widePath.c_str(), wideMode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

int removePath(const std::string& path) {
#ifdef _WIN32
    const std::wstring widePath = utf8ToWide(path);
    if (widePath.empty()) return -1;
    return _wremove(widePath.c_str());
#else
    return std::remove(path.c_str());
#endif
}

#ifdef _WIN32
std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();

    // 先问长度、再转换。两次调用是 CP_UTF8 这个组合的标准写法：
    // 第一次传 nullptr 只问"要多少个宽字符"，第二次才真写。
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) return std::wstring();

    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                        static_cast<int>(text.size()), &out[0], size);
    return out;
}
#endif

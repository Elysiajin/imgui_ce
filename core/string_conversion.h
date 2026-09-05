#pragma once

#include <string>
#include <windows.h>

// 宽字符字符串 -> UTF-8
inline std::string wstring_to_utf8(const wchar_t* wstr)
{
    if (!wstr)
        return {};

    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1,
                                  nullptr, 0, nullptr, nullptr);
    std::string out;
    if (len > 0) {
        out.resize(len);
        WideCharToMultiByte(CP_UTF8, 0, wstr, -1,
                            out.data(), len, nullptr, nullptr);
        out.resize(len - 1);   // 去掉末尾 null
    }
    return out;
}

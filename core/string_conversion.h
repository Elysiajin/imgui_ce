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

// UTF-8 -> 宽字符（文件路径传给 Win32 / pugixml 宽字符重载用）
inline std::wstring utf8_to_wstring(const std::string& str)
{
    if (str.empty())
        return {};

    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring out;
    if (len > 0) {
        out.resize(len);
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, out.data(), len);
        out.resize(len - 1);   // 去掉末尾 null
    }
    return out;
}

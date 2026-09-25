#include "ct/address_parser.h"

#include <algorithm>
#include <cctype>

namespace {

std::string trim_copy(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string to_lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// CE 习惯：裸地址/偏移默认十六进制，允许 0x 前缀；支持前导负号。
bool parse_hex_int(const std::string& s, uint64_t& out)
{
    std::string t = trim_copy(s);
    if (t.empty())
        return false;
    bool neg = false;
    if (t[0] == '-') { neg = true; t = t.substr(1); }
    else if (t[0] == '+') { t = t.substr(1); }
    if (t.empty())
        return false;
    if (t.size() > 1 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))
        t = t.substr(2);
    uint64_t v = 0;
    for (char c : t) {
        if (!std::isxdigit((unsigned char)c))
            return false;
        v = v * 16 + (uint64_t)(std::isdigit((unsigned char)c)
                                    ? c - '0'
                                    : std::tolower((unsigned char)c) - 'a' + 10);
    }
    out = neg ? (uint64_t)(0 - (int64_t)v) : v;
    return true;
}

// 找模块表项（大小写不敏感）；找不到返回 nullptr。
const module_info* find_module(const std::vector<module_info>& modules,
                               const std::string& name)
{
    const std::string low = to_lower(trim_copy(name));
    for (const auto& m : modules)
        if (to_lower(m.name) == low)
            return &m;
    return nullptr;
}

} // namespace

parsed_address parse_interpretable_address(
    const std::string& text,
    const std::vector<module_info>& modules,
    const std::unordered_map<std::string, uint64_t>& symbols)
{
    parsed_address out;
    std::string t = trim_copy(text);

    // 剥掉引号：CE 的 Description 整体包裹（"xxx"）与模块名引用
    // （"Module Name"+0x10）两种形式——引号不属于地址语法本身
    t.erase(std::remove(t.begin(), t.end(), '"'), t.end());
    t = trim_copy(t);

    if (t.empty())
        return out;

    // ---- 模块±偏移：从右往左找 '+/-' 分隔（模块名可能含 '-'，
    //      如 "Tutorial-i386.exe+29D8D"；偏移本身只出现一次）----
    const size_t sep_rplus = t.rfind('+'), sep_rminus = t.rfind('-');
    size_t sep = std::string::npos;
    if (sep_rplus != std::string::npos && sep_rminus != std::string::npos)
        sep = std::max(sep_rplus, sep_rminus);
    else if (sep_rplus != std::string::npos) sep = sep_rplus;
    else sep = sep_rminus;
    if (sep != std::string::npos && sep > 0) {
        const std::string mod_name = t.substr(0, sep);
        uint64_t offset = 0;
        if (parse_hex_int(t.substr(sep), offset) &&
            find_module(modules, mod_name)) {
            const module_info* m = find_module(modules, mod_name);
            out.address = m->base + offset;
            out.ok = true;
            out.is_module_relative = true;
            out.module_name = mod_name;
            out.module_offset = offset;
            return out;
        }
        // 不是模块形式 → 掉到符号/数值分支继续尝试
    }

    // ---- 纯数值 ----
    uint64_t v = 0;
    if (parse_hex_int(t, v)) {
        out.address = v;
        out.ok = true;
        return out;
    }

    // ---- 符号 ----
    auto it = symbols.find(to_lower(t));
    if (it != symbols.end()) {
        out.address = it->second;
        out.ok = true;
        out.is_module_relative = true;   // 符号地址同样随进程变化
        return out;
    }

    return out;
}

#pragma once

#include "type/value_type.h"
#include "type/scan_data_stream_define.h"
#include "core/process_manager.h"
#include "scan/encoding_formatter.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// 地址列表(下方 watch 列表)与内存值之间的类型映射 + 读取/写回。
// 集中在这里，避免 UI 面板里散落 memcpy/strtod，让"加入地址栏"能带上正确的类型。

inline bool value_type_is_float(value_type t) {
    return t == value_type::float32 || t == value_type::float64;
}

inline scan_data_type value_type_to_scan_data_type(value_type t) {
    switch (t) {
    case value_type::one_byte:    return scan_data_type::int8;
    case value_type::two_bytes:   return scan_data_type::int16;
    case value_type::four_bytes:  return scan_data_type::int32;
    case value_type::eight_bytes: return scan_data_type::int64;
    case value_type::float32:     return scan_data_type::float32;
    case value_type::float64:     return scan_data_type::float64;
    case value_type::text:        return scan_data_type::ascii_string;
    case value_type::byte_array:  return scan_data_type::byte_array;
    }
    return scan_data_type::int32;
}

inline value_type scan_data_type_to_value_type(scan_data_type t) {
    switch (t) {
    case scan_data_type::int8:    return value_type::one_byte;
    case scan_data_type::int16:   return value_type::two_bytes;
    case scan_data_type::int32:   return value_type::four_bytes;
    case scan_data_type::int64:   return value_type::eight_bytes;
    case scan_data_type::float32: return value_type::float32;
    case scan_data_type::float64: return value_type::float64;
    case scan_data_type::byte_array: return value_type::byte_array;
    default:                      return value_type::text;
    }
}

// 读内存并格式化为显示字符串；失败/unattached 返回 "---"。
// radix 只对整数类型生效（Hex/Dec/Oct），浮点/字符串/AOB 忽略。
inline std::string read_address_value(uint64_t addr, value_type t,
                                      value_radix radix = value_radix::decimal) {
    auto* mem = process_manager::instance().memory();
    if (!mem) return "---";
    const auto dt  = value_type_to_scan_data_type(t);
    const size_t sz = scan_data_type_size(dt);
    if (sz == 0) {                 // text / AOB 字符串
        if (t == value_type::byte_array) {
            std::vector<uint8_t> buf(32);
            if (!mem->read(addr, buf.data(), buf.size())) return "---";
            return encoding_formatter::format_byte_array(buf.data(), buf.size());
        }
        std::vector<uint8_t> buf(64);
        if (!mem->read(addr, buf.data(), buf.size())) return "---";
        size_t len = 0;
        while (len < buf.size() && buf[len] != 0) ++len;
        return encoding_formatter::format_string(std::string(reinterpret_cast<const char*>(buf.data()), len), dt);
    }
    uint64_t raw = 0;
    if (!mem->read(addr, &raw, sz)) return "---";
    return encoding_formatter::format_address_value(raw, dt, radix);
}

// 解析文本并按类型写回内存；成功返回 true。
// radix 用于整数类型的非 0x/0 前缀输入（Hex/Oct/Dec 回退解析）。
inline bool write_address_value(uint64_t addr, value_type t, const std::string& text,
                                value_radix radix = value_radix::decimal) {
    auto* mem = process_manager::instance().memory();
    if (!mem) return false;
    const auto dt = value_type_to_scan_data_type(t);
    const size_t sz = scan_data_type_size(dt);
    if (sz == 0) {                 // text / AOB：写原始字节 + NUL 结尾
        std::vector<uint8_t> b;
        if (t == value_type::byte_array) {
            // 解析形如 "3E ?? 1A" 的字节数组（空格/逗号分隔）
            const char* p = text.c_str();
            while (*p) {
                while (*p == ' ' || *p == ',') ++p;
                if (!*p) break;
                char tok[8] = {0};
                int k = 0;
                while (*p && *p != ' ' && *p != ',' && k < 7) tok[k++] = *p++;
                std::string s(tok);
                std::transform(s.begin(), s.end(), s.begin(), ::toupper);
                if (s == "??") { b.push_back(0); continue; }
                unsigned v = 0;
                if (sscanf(s.c_str(), "%2x", &v) != 1) return false;
                b.push_back(static_cast<uint8_t>(v));
            }
            if (b.empty()) return false;
        } else {
            b.assign(text.begin(), text.end());
            b.push_back(0);
        }
        return mem->write(addr, b.data(), b.size());
    }
    uint64_t bits = 0;
    if (value_type_is_float(t)) {
        char* end = nullptr;
        double d = std::strtod(text.c_str(), &end);
        if (end == text.c_str()) return false;
        if (dt == scan_data_type::float32) { float f = static_cast<float>(d); std::memcpy(&bits, &f, sizeof(f)); }
        else                                 { std::memcpy(&bits, &d, sizeof(d)); }
    } else {
        char* end = nullptr;
        // 根据 radix 选择默认进制，但保留 0x/0 前缀的自动识别（base=0 在 radix 非 10 时仍优先字面量）
        int base = 0;
        if (radix == value_radix::hex) base = 16;
        else if (radix == value_radix::octal) base = 8;
        else base = 0;
        long long sv = std::strtoll(text.c_str(), &end, base);
        if (end == text.c_str()) return false;
        bits = static_cast<uint64_t>(sv);
    }
    return mem->write(addr, &bits, sz);
}

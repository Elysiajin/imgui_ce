#pragma once

#include "type/value_type.h"
#include "type/scan_data_stream_define.h"
#include "core/process_manager.h"
#include "scan/encoding_formatter.h"

#include <algorithm>
#include <cstdint>
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
    default:                      return value_type::text;
    }
}

// 读内存并格式化为显示字符串；失败/unattached 返回 "---"
inline std::string read_address_value(uint64_t addr, value_type t) {
    auto* mem = process_manager::instance().memory();
    if (!mem) return "---";
    const auto dt  = value_type_to_scan_data_type(t);
    const size_t sz = scan_data_type_size(dt);
    if (sz == 0) {                 // text / 字符串
        std::vector<uint8_t> buf(64);
        if (!mem->read(addr, buf.data(), buf.size())) return "---";
        size_t len = 0;
        while (len < buf.size() && buf[len] != 0) ++len;
        return encoding_formatter::format_string(std::string(reinterpret_cast<const char*>(buf.data()), len), dt);
    }
    uint64_t raw = 0;
    if (!mem->read(addr, &raw, sz)) return "---";
    return encoding_formatter::format_value(raw, dt, false);
}

// 解析文本并按类型写回内存；成功返回 true
inline bool write_address_value(uint64_t addr, value_type t, const std::string& text) {
    auto* mem = process_manager::instance().memory();
    if (!mem) return false;
    const auto dt = value_type_to_scan_data_type(t);
    const size_t sz = scan_data_type_size(dt);
    if (sz == 0) {                 // text：写字符串 + NUL 结尾
        std::vector<uint8_t> b(text.begin(), text.end());
        b.push_back(0);
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
        long long sv = std::strtoll(text.c_str(), &end, 0);
        if (end == text.c_str()) return false;
        bits = static_cast<uint64_t>(sv);
    }
    return mem->write(addr, &bits, sz);
}

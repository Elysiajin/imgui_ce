#pragma once
#include <string>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <cstdint>
#include "type/scan_data_stream_define.h"
#include "type/value_type.h"

class encoding_formatter
{
public:
    encoding_formatter() = default;
    ~encoding_formatter() = default;

    // 按进制格式化无符号整数（radix: 10/16/8），供地址栏/结果区统一使用。
    inline static std::string format_uint(uint64_t raw, unsigned radix) {
        char buf[64] = {};
        switch (radix) {
        case 16: snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)raw); break;
        case 8:  snprintf(buf, sizeof(buf), "0%llo", (unsigned long long)raw); break;
        case 10:
        default: snprintf(buf, sizeof(buf), "%llu", (unsigned long long)raw); break;
        }
        return buf;
    }

    inline static std::string format_value(uint64_t raw, scan_data_type type, bool hex_display = false) {
        char buf[64] = {};
        if (hex_display && !is_floating_point(type)) {
            switch (type) {
            case scan_data_type::int8:  snprintf(buf, sizeof(buf), "0x%x", static_cast<unsigned>(static_cast<uint8_t>(raw))); break;
            case scan_data_type::int16: snprintf(buf, sizeof(buf), "0x%x", static_cast<unsigned>(static_cast<uint16_t>(raw))); break;
            case scan_data_type::int32: snprintf(buf, sizeof(buf), "0x%x", static_cast<unsigned>(static_cast<uint32_t>(raw))); break;
            case scan_data_type::int64: snprintf(buf, sizeof(buf), "0x%llx", raw); break;
            case scan_data_type::bit:   snprintf(buf, sizeof(buf), "0x%x", static_cast<unsigned>(raw & 1)); break;
            default: return std::to_string(raw);
            }
            return buf;
        }
        switch (type) {
        case scan_data_type::int8:  snprintf(buf, sizeof(buf), "%d", static_cast<int8_t>(raw)); break;
        case scan_data_type::int16: snprintf(buf, sizeof(buf), "%d", static_cast<int16_t>(raw)); break;
        case scan_data_type::int32: snprintf(buf, sizeof(buf), "%d", static_cast<int32_t>(raw)); break;
        case scan_data_type::int64: snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(raw)); break;
        case scan_data_type::float32: {
            float f; std::memcpy(&f, &raw, sizeof(f));
            snprintf(buf, sizeof(buf), "%g", f); break;
        }
        case scan_data_type::float64: {
            double d; std::memcpy(&d, &raw, sizeof(d));
            snprintf(buf, sizeof(buf), "%g", d); break;
        }
        default: return std::to_string(raw);
        }
        return buf;
    }

    // 地址栏整数类型按指定进制（hex/octal）格式化；浮点/字符串仍走原路径。
    inline static std::string format_address_value(uint64_t raw, scan_data_type type, value_radix radix) {
        if (!is_floating_point(type) && type != scan_data_type::ascii_string &&
            type != scan_data_type::utf8_string && type != scan_data_type::utf16_string &&
            type != scan_data_type::byte_array) {
            switch (radix) {
            case value_radix::hex:   return format_value(raw, type, true);
            case value_radix::octal: return format_uint(raw, 8);
            case value_radix::decimal:
            default:                 return format_value(raw, type, false);
            }
        }
        return format_value(raw, type, false);
    }

    inline static std::string format_string(const std::string& str, scan_data_type) {
        return str;
    }

    inline static std::string format_utf16_string(const uint16_t* data, size_t length) {
        std::string utf8;
        for (size_t i = 0; i < length; ++i) {
            uint16_t c = data[i];
            if (c == 0) break;
            if (c < 0x80) {
                utf8 += static_cast<char>(c);
            }
            else if (c < 0x800) {
                utf8 += static_cast<char>(0xC0 | (c >> 6));
                utf8 += static_cast<char>(0x80 | (c & 0x3F));
            }
            else {
                utf8 += static_cast<char>(0xE0 | (c >> 12));
                utf8 += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                utf8 += static_cast<char>(0x80 | (c & 0x3F));
            }
        }
        return utf8;
    }

    inline static std::string format_byte_array(const uint8_t* data, size_t length) {
        std::string hex;
        char tmp[4];
        for (size_t i = 0; i < length; ++i) {
            snprintf(tmp, sizeof(tmp), "%02X ", data[i]);
            hex += tmp;
        }
        return hex;
    }
};

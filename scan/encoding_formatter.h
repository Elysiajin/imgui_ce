#pragma once
#include <string>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <cstdint>
#include "type/scan_data_stream_define.h"

class encoding_formatter
{
public:
    encoding_formatter() = default;
    ~encoding_formatter() = default;

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

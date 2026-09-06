#pragma once

#include "type/scan_data_stream_define.h"

#include <cstdlib>
#include <cstring>
#include <string>

// 把用户输入文本解析为扫描用的位模式（由 uint64_t 承载）。
// 与引擎侧的 memcpy 位还原严格配套：
//  - 浮点类型（float32/float64）：文本解析为数值后，以 IEEE 位模式写入 out。
//    Hex 勾选时按 CE 行为把文本当十六进制数值（"1000" → 4096.0f）。
//  - 整数类型：Hex 勾选按 16 进制，否则按 10 进制。
inline bool parse_scan_value(const std::string& text, scan_data_type dt, bool hex, uint64_t& out) {
    out = 0;
    if (text.empty()) return false;

    if (is_floating_point(dt)) {
        char* end = nullptr;
        double d;
        if (hex) {
            unsigned long long h = strtoull(text.c_str(), &end, 16);
            if (end == text.c_str()) return false;
            d = static_cast<double>(h);
        } else {
            d = strtod(text.c_str(), &end);
            if (end == text.c_str()) return false;
        }
        if (dt == scan_data_type::float32) {
            float f = static_cast<float>(d);
            std::memcpy(&out, &f, sizeof(f));
        } else {
            std::memcpy(&out, &d, sizeof(d));
        }
        return true;
    }

    char* end = nullptr;
    if (hex) {
        out = strtoull(text.c_str(), &end, 16);
        return end != text.c_str();
    }
    long long sv = strtoll(text.c_str(), &end, 10);
    if (end == text.c_str()) return false;
    out = static_cast<uint64_t>(sv);
    return true;
}

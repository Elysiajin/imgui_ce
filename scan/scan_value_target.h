#pragma once

#include "type/scan_data_stream_define.h"

#include <cstring>
#include <type_traits>
#include <variant>

// 首次扫描目标值构造（从 scan_request 的 value_params 还原出 T 类型的 v1/v2）。
// 与 scan_engine::task_first_scan 的匹配语义保持一致，抽成头文件以便单测覆盖。
// 返回 is_float_approx：浮点 + 勾选近似值 + exact_value 时，v1/v2 是区间 [min,max]。
template<typename T>
inline bool build_first_scan_targets(const scan_request& req, T& v1, T& v2) {
    v1 = T{};
    v2 = T{};
    const bool is_float_approx = std::is_floating_point_v<T>
        && req.contain_approximate_value
        && req.first_type == scan_type::exact_value;

    if (auto* p = std::get_if<value_params>(&req.params)) {
        if constexpr (std::is_floating_point_v<T>) {
            // value1/value2 已经是浮点位模式（见 scan_value_parser.h），直接按位还原
            T target;
            std::memcpy(&target, &p->value1, sizeof(T));
            if (is_float_approx) {
                // 勾选"包含近似值" → ±1% 相对容差，转成 [v1,v2] 区间
                constexpr T relative_epsilon = static_cast<T>(0.01);
                T lo = target * (static_cast<T>(1.0) - relative_epsilon);
                T hi = target * (static_cast<T>(1.0) + relative_epsilon);
                T abs_min = static_cast<T>(0.0001);
                if (target >= static_cast<T>(0)) {
                    if (lo < -abs_min) lo = static_cast<T>(0);
                }
                if (hi - lo < abs_min) { lo = target - abs_min; hi = target + abs_min; }
                std::memcpy(&v1, &lo, sizeof(T));
                std::memcpy(&v2, &hi, sizeof(T));
            } else {
                std::memcpy(&v1, &target, sizeof(T));
                if (req.first_type == scan_type::between) {
                    T tmp;
                    std::memcpy(&tmp, &p->value2, sizeof(T));
                    std::memcpy(&v2, &tmp, sizeof(T));
                }
            }
        } else {
            std::memcpy(&v1, &p->value1, sizeof(T));
            if (req.first_type == scan_type::between)
                std::memcpy(&v2, &p->value2, sizeof(T));
        }
    }
    return is_float_approx;
}

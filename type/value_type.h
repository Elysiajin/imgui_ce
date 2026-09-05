#pragma once

#include <cstdint>

// 地址列表 / 手动添加地址所使用的数据类型（对应 CE 的 TMemoryRecord 取值类型）。
// 顺序与 address_list_panel.cpp 中 type_names 数组保持一一对应。
enum class value_type : uint8_t {
    one_byte,
    two_bytes,
    four_bytes,
    eight_bytes,
    float32,
    float64,
    text,
};

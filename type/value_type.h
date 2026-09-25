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
    byte_array,
    binary,           // CE 的 Binary（按位显示/编辑）
    auto_assembler,   // CE 的 Auto Assembler Script 条目（值列显示 <脚本>）
};

// 地址栏数值显示的进制（对应 CE 内存记录 / hexview 的 showashex + 本项目增强的八进制）。
enum class value_radix : uint8_t {
    decimal = 0,   // 有符号十进制
    hex     = 1,   // 十六进制
    octal   = 2,   // 八进制
};

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "type/module_info.h"

// CE 的"可解释地址"（interpretable address）文本解析：
//   "004CEF18" / "0x403000"            → 直接数值（十六进制）
//   "Sword2.exe+13740C" / "game.exe"   → 模块基址(+偏移)，偏移按十六进制（CE 习惯）
//   "module-10"                        → 模块基址 - 偏移
//   "demage" 等符号                    → 查符号表（CT 的 UserdefinedSymbols /
//                                        脚本 registersymbol 的结果）
struct parsed_address {
    uint64_t    address = 0;          // 解析结果；ok == false 时无意义
    bool        ok = false;
    bool        is_module_relative = false;   // 模块+偏移形式（基址随加载重定位）
    std::string module_name;          // is_module_relative 时有效（原文大小写）
    uint64_t    module_offset = 0;
};

// modules: 附加进程的模块表（process_manager::module_snapshot()，也接受空表——
//          此时模块形式解析失败但纯数值仍可用）。
// symbols: 符号名(小写) → 地址；找不到符号则解析失败。
parsed_address parse_interpretable_address(
    const std::string& text,
    const std::vector<module_info>& modules,
    const std::unordered_map<std::string, uint64_t>& symbols = {});

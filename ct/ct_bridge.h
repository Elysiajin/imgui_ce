#pragma once

#include <vector>

#include "ct/cheat_table.h"
#include "type/module_info.h"

class address_list_panel;

// CT 表 ↔ 地址列表 的双向桥接。
//
// 地址列表内部是"先序扁平数组"（parent_id + depth 表达树），
// CT 是递归嵌套的 XML 树；这里负责两种形态的互转。
namespace ct_bridge {

// ct 树 → 地址列表（先序展开）。会 clear 原列表。
// modules 用于解析可解释地址（模块±偏移）；进程未附加时传空表，
// 条目保留地址原文，附加后由 update_values() 重试解析。
void apply_ct_table(const cheat_table& ct, address_list_panel& list,
                    const std::vector<module_info>& modules);

// 地址列表 → ct 树（按 depth 组树）。user_symbols 一并带出。
cheat_table build_ct_table(const address_list_panel& list);

} // namespace ct_bridge

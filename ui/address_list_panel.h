#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "type/value_type.h"

// 一条地址记录（对应 CE 的 TMemoryRecord）
// 用 id 作为跨帧稳定标识，避免依赖数组下标
struct address_record {
    uint64_t     id = 0;                    // 稳定行键
    std::string  description;               // 描述
    std::string  address;                   // 显示的地址字符串
    uint64_t     real_address = 0;          // 解析后的真实地址
    bool         valid = true;

    value_type   type = value_type::four_bytes;
    std::string  value;                     // 当前值（字符串展示）
    std::string  previous_value;            // 上一次的值（用于变色）

    bool    frozen    = false;              // 是否锁定（激活）
    bool    show_hex  = false;              // 该行十六进制显示
    bool    writable  = false;

    bool    changed   = false;              // 值是否刚变动过（上色用）
    uint32_t color    = 0;                  // 行颜色（不用 ImU32，保持头文件无 imgui 依赖）
};

class address_list_panel {
public:
    void render();

    // 供外部（结果区/手动添加）填充数据
    void add_record(const address_record& rec);
    void clear();
    std::vector<address_record>& records() { return records_; }

private:
    address_record* find_record(uint64_t id);
    void remove_record(uint64_t id);

    std::vector<address_record> records_;
    uint64_t next_id_ = 1;   // 自增 id

    // 右键/编辑状态
    uint64_t selected_row_id_ = 0;      // 当前被右键/选中的行
    uint64_t edit_id_ = 0;              // 正在编辑的行（0 表示无）
    std::string edit_buf_;              // 编辑缓冲
};

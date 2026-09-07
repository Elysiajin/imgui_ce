#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "type/value_type.h"

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

    // 实时刷新所有行内存值 + 高亮（约 200ms 节流）
    void update_values();
    // 每帧把 frozen 行的基准值持续写回内存（数据冻结）
    void apply_freeze();
    std::vector<address_record>& records() { return records_; }

private:
    address_record* find_record(uint64_t id);
    void remove_record(uint64_t id);
    void begin_edit(uint64_t id, std::string initial);
    void commit_edit(address_record* rec);

    std::vector<address_record> records_;
    uint64_t next_id_ = 1;   // 自增 id

    // 编辑状态（edit_col_：1=描述, 3=类型, 4=值）
    uint64_t selected_row_id_ = 0;      // 最近被右键选中的行
    uint64_t pending_delete_id_ = 0;    // 待删除行（延迟到行循环之外执行，避免迭代中失效）
    uint64_t edit_id_ = 0;              // 正在编辑的行（0 表示无）
    int      edit_col_ = 1;
    char     edit_buf_[256] = {};       // 固定大小编辑缓冲，避免空字段无法输入
};

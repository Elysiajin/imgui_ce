#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "type/value_type.h"

struct aa_session;   // ct/aa_script.h（脚本 alloc 的跨激活状态）

// CE 风格的地址条目（对应 CT 的 <CheatEntry> → TMemoryRecord）。
//
// 树状结构：records_ 按"先序"扁平存放，parent_id + depth 表达父子关系
// （渲染折叠跳过、CT 加载展开/保存组树都基于这个约定）。
// id 作为跨帧稳定标识，避免依赖数组下标。
struct address_record {
    uint64_t     id = 0;                    // 稳定行键
    std::string  description;               // 描述
    std::string  address;                   // 地址原文（CT 的 <Address>；手工添加为空）
    uint64_t     real_address = 0;          // 解析后的真实地址（0 = 尚未解析成功）
    bool         valid = true;

    value_type   type = value_type::four_bytes;
    std::string  value;                     // 当前值（字符串展示）
    std::string  previous_value;            // 上一次的值（用于变色）

    bool    frozen    = false;              // 激活（数值行=冻结；脚本行=已执行 ENABLE）
    bool    show_hex  = false;              // 该行十六进制显示（兼容旧字段，实际以 radix 为准）
    value_radix radix = value_radix::decimal; // 数值显示进制（Hex/Dec/Oct）
    bool    writable  = false;

    bool    changed   = false;              // 值是否刚变动过（上色用）
    uint32_t color    = 0;                  // 行颜色
    bool    has_color = false;              // color 有效（CT <Color> 往返）

    // ---- 树状结构 ----
    uint64_t parent_id = 0;                 // 0 = 根
    int      depth = 0;                     // 树深度（渲染缩进；先序插入时维护）
    bool     expanded = true;               // 有子节点时的展开状态

    // ---- CE 条目种类 ----
    bool is_group = false;                  // <GroupHeader>（纯分组行）
    std::vector<uint64_t> offsets;          // 非空即指针链（CE：<Offsets>）
    uint64_t pointer_final = 0;             // 指针链解出的最终地址
    bool     pointer_ok = false;            // 本次解引用是否成功

    // AA 脚本条目
    std::string script;                     // <AssemblerScript> 全文
    std::string script_status;              // 最近一次执行结果（错误/成功提示）
    std::shared_ptr<aa_session> session;    // 激活会话（alloc 的地址，disable 释放）

    // Binary 位域
    int bit_start = 0;
    int bit_length = 0;

    // Array of byte 长度（0 = 默认 32 字节）
    int byte_length = 0;

    // String / 类型往返字段（保存 CT 用，不影响显示）
    bool show_signed = false;
    bool show_signed_present = false;
    bool unicode_string = false;            // "Unicode String" 标志
    int  string_length = 0;
    int  codepage = 0;
    bool zero_terminate = true;
    std::string custom_type_name;           // <CustomType> 类型名
    std::string ct_raw_type;                // 未识别 <VariableType> 原文
};

class address_list_panel {
public:
    void render();

    // 供外部（结果区/手动添加）填充数据
    void add_record(const address_record& rec);               // 顶层追加
    void add_child_record(uint64_t parent_id,
                          const address_record& rec);         // 追加到父的子树末尾
    void clear();

    // 实时刷新所有行内存值 + 高亮（约 200ms 节流）；含指针链解引用与
    // 未解析地址（模块+偏移）的重试解析
    void update_values();
    // 每帧把 frozen 行的基准值持续写回内存（数据冻结）
    void apply_freeze();
    std::vector<address_record>& records() { return records_; }
    const std::vector<address_record>& records() const { return records_; }

    // CT 的 <UserdefinedSymbols>（name → 可解释地址原文），供地址解析/脚本用
    std::vector<std::pair<std::string, std::string>> user_symbols;

private:
    address_record* find_record(uint64_t id);
    void remove_record_with_children(uint64_t id);
    void begin_edit(uint64_t id, std::string initial);
    void commit_edit(address_record* rec);
    bool has_children(const address_record& r) const;

    // 读当前值（脚本行返回 <脚本>；指针行先解引用；二进制行按位串）
    std::string read_current_value(address_record& r);

    std::vector<address_record> records_;
    uint64_t next_id_ = 1;   // 自增 id

    // 编辑状态（edit_col_：1=描述, 3=类型, 4=值）
    uint64_t selected_row_id_ = 0;      // 最近被右键选中的行
    uint64_t pending_delete_id_ = 0;    // 待删除行（延迟到行循环之外执行，避免迭代中失效）
    uint64_t edit_id_ = 0;              // 正在编辑的行（0 表示无）
    int      edit_col_ = 1;
    char     edit_buf_[256] = {};       // 固定大小编辑缓冲，避免空字段无法输入

    // 脚本查看/编辑弹窗
    uint64_t script_edit_id_ = 0;
    char     script_buf_[0x4000] = {};
};

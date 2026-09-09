#ifndef MEMORY_WINDOW_H
#define MEMORY_WINDOW_H

#include "ui_state.h"
#include "imgui.h"

#include "ui/assembler_window.h"
#include "ui/hex_view.h"
#include "ui/inject_window.h"
#include "ui/zydis_disassembler.h"
#include "type/process_arch.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

class memory_window
{
public:
    explicit memory_window(ui_state& ui);

    void render();

private:
    // 反汇编视图：跨帧缓存 + 架构/地址变化时重建。
    void render_disassembly_view();
    void render_disasm_toolbar();
    // 反汇编行注释列：分支目标 / 模块+偏移
    std::string disasm_comment_for(const disasm_line& ln) const;
    // 附加进程后自动定位：主模块入口点（反汇编）+ 主模块基址（hex dump）
    void auto_navigate_on_attach();
    // 反汇编显式跳转（goto/Follow/箭头点击/自动定位）：重置解码窗口与滚动
    void disasm_jump_to(uint64_t addr);
    // 反汇编跳转回退：弹出 back 栈上一地址（CE GoBack 语义）
    void disasm_go_back();
    bool disasm_has_back() const { return !disasm_back_stack_.empty(); }
    // 跳转箭头层（x64dbg 风格 gutter，层级排布 + 悬停/点击交互）
    void draw_jump_arrows(float gutter_x0, const std::vector<float>& row_tops,
                          float text_h);
    // 在指令列单元格内绘制着色 token 序列；处理 H 模式点击与指令行双击跟随
    void render_instr_tokens(uint64_t addr, const std::string& text,
                             bool is_branch, uint64_t branch_target);
    // 注释列：显示用户注释（优先）或自动标签；双击进入编辑
    void render_comment_cell(uint64_t addr, const disasm_line& ln);

    // 用户注释：内存态（addr -> 文本），仅本进程生命周期
    std::map<uint64_t, std::string> disasm_comments_;
    uint64_t  comment_edit_addr_ = 0;      // 非 0 = 正在编辑该地址的注释
    bool      comment_need_focus_ = false; // 编辑框下一帧自动聚焦
    char      comment_edit_buf_[512] = {};

    // ---- H 高亮模式：按 H 进入/退出；点击指令整条加背景色块，点击寄存器词单独高亮 ----
    bool       disasm_hl_mode_ = false;
    struct hl_word_key {
        uint64_t addr;
        uint32_t word_idx;
        bool operator<(const hl_word_key& o) const {
            return addr != o.addr ? addr < o.addr : word_idx < o.word_idx;
        }
    };
    std::set<uint64_t>          hl_instructions_;   // 整条指令高亮（起始地址）
    std::set<hl_word_key>       hl_reg_words_;      // 寄存器词高亮

    ui_state& state_;
    assembler_window assembler_window_;   // 自动汇编窗口（成员名加下划线，避免与类型同名）
    inject_window    inject_window_;      // 注入窗口（Tools -> Inject 打开）
    // ---- 反汇编视图状态 ----
    disassembler               disasm_;
    std::vector<disasm_line>   disasm_lines_;
    // 地址列符号标签（与 disasm_lines_ 平行；空串 = 无符号，显示十六进制地址）
    std::vector<std::string>   disasm_addr_symbols_;
    uint64_t                   disasm_base_ = 0;      // 解码窗口首行地址（滑动窗口随滚动重锚）
    process_arch               disasm_cached_arch_ = process_arch::unknown;
    uint64_t                   disasm_cached_base_ = 0;
    int                        disasm_cached_count_ = 0;
    bool                       disasm_scroll_top_ = false;
    uint64_t                   disasm_selected_ = 0;
    bool                       disasm_show_symbols_ = false;
    bool                       disasm_show_jcc_ = true;    // 箭头开关：条件跳转
    bool                       disasm_show_jmp_ = true;    // 箭头开关：无条件跳转
    bool                       disasm_show_call_ = true;   // 箭头开关：函数调用
    char                       disasm_goto_buf_[64] = {};
    std::vector<uint64_t>      disasm_base_history_;  // 窗口前进历史（向上滚动回退用）
    // 跳转回退栈（CE 的 disassemblerview.backlist）：Follow/goto/箭头点击等显式
    // 跳转前把旧地址压栈，"Back" 按钮弹出回退。与分页前进历史分离。
    std::vector<uint64_t>      disasm_back_stack_;
    bool                       disasm_going_back_ = false;
    uint32_t                   navigated_pid_ = 0;    // 已做过自动定位的 pid

    // ---- hex dump 视图 ----
    hex_view hex_view_;
};

#endif // MEMORY_WINDOW_H

#ifndef MEMORY_WINDOW_H
#define MEMORY_WINDOW_H

#include "ui_state.h"
#include "imgui.h"

#include "ui/assembler_window.h"
#include "ui/hex_view.h"
#include "ui/zydis_disassembler.h"
#include "type/process_arch.h"

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
    // 跳转箭头层（x64dbg 风格 gutter，层级排布 + 悬停/点击交互）
    void draw_jump_arrows(float gutter_x0, const std::vector<float>& row_tops,
                          float text_h);

    ui_state& state_;
    assembler_window assembler_window_;   // 自动汇编窗口（成员名加下划线，避免与类型同名）
    // ---- 反汇编视图状态 ----
    disassembler               disasm_;
    std::vector<disasm_line>   disasm_lines_;
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
    uint32_t                   navigated_pid_ = 0;    // 已做过自动定位的 pid

    // ---- hex dump 视图 ----
    hex_view hex_view_;
};

#endif // MEMORY_WINDOW_H

#pragma once

#include "ui_state.h"
#include "type/value_type.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// 十六进制内存视图（对应 CE 的 THexView / x64dbg 的 dump 窗口）。
//
//  - 内存按 4096 字节页缓存：整页读取失败（跨区边界）时按 64 字节块降级重试，
//    不可读块显示 "??"，避免逐字节 ReadProcessMemory 的开销（仿 CE 的 TPageinfo）。
//  - 定时（约 250ms）重读已缓存页并 diff，变化字节带时间戳，渲染时渐变高亮
//    （仿 CE 的 TChangelist + fadetimer）。
//  - 显示类型可切换（Byte/Word/Dword/Qword/Float/Double），地址列宽随目标进程架构变化。
//  - 就地编辑：hex 字节 / 数值 / ASCII 三种编辑路径，写回目标进程
//    （Win32MemoryAccessor::write 自带 VirtualProtectEx 回退）。
class hex_view {
public:
    explicit hex_view(ui_state& ui) : state_(ui) {}

    void render();

    // 主动失效全部缓存（pid 变化时也会自动清）
    void invalidate();

private:
    static constexpr size_t k_page_size     = 4096;
    static constexpr size_t k_chunk_size    = 64;    // 降级读取粒度（4096/64 = 64 块，恰好一个 uint64 掩码）
    static constexpr size_t k_max_pages     = 64;    // 页缓存上限（64 页 = 256KB）
    static constexpr int    k_bytes_per_row = 16;
    static constexpr int    k_fade_ms       = 1500;  // 变化高亮渐变时长
    static constexpr int    k_refresh_ms    = 250;   // 实时刷新间隔

    enum edit_col_kind { ec_hex = 0, ec_value = 1, ec_ascii = 2 };

    struct page {
        uint64_t             base = 0;
        uint64_t             readable_mask = 0;   // 每 k_chunk_size 字节一块的位图
        std::vector<uint8_t> data;                // k_page_size 字节
    };

    struct display_type_info {
        const char* name;
        int         size;      // 字节数
        bool        is_int;    // 整数（支持 hex 显示/输入）
        value_type  vtype;     // 加入地址列表时使用的类型
    };

    static int                             type_count();
    static const display_type_info&        type_info(int idx);

    page*    get_page(uint64_t page_base);
    uint8_t  byte_at(uint64_t addr, bool& valid);
    void     refresh_pages();
    void     handle_pid_change();

    void     render_toolbar();
    void     render_table();
    void     render_row_byte_mode(uint64_t row_addr);
    void     render_row_value_mode(uint64_t row_addr, int vsize);
    void     render_ascii_cell(uint64_t row_addr);
    void     draw_data_cell(uint64_t addr, const char* text, bool valid,
                            int size, int kind, bool hex_input);
    void     render_cell_menu(uint64_t addr);

    void     open_editor(uint64_t addr, int kind, int size, int type_idx,
                         bool hex_input, const char* initial);
    void     open_ascii_editor(uint64_t addr);
    bool     commit_edit();
    bool     commit_ascii_edit();
    bool     write_bytes(uint64_t addr, const void* data, size_t n);
    void     goto_address(uint64_t addr);

    ui_state& state_;

    // ---- 页缓存与变化标记 ----
    std::unordered_map<uint64_t, page>    pages_;
    std::unordered_map<uint64_t, int64_t> change_ticks_;   // addr -> 变化时刻(ms)
    uint32_t                              cached_pid_ = 0;
    int64_t                               last_refresh_ms_ = 0;

    // ---- 显示状态 ----
    uint64_t view_base_     = 0;    // 滑动窗口第一行地址（16 对齐，随滚动动态重锚）
    uint64_t last_target_   = 0;    // 检测 dump_view_address 变化
    int      display_type_  = 0;
    bool     hex_display_   = true;
    bool     want_scroll_top_ = false;  // 跳转后把子窗口滚动归零
    uint64_t selected_addr_ = 0;
    bool     has_selection_ = false;

    // ---- 编辑状态 ----
    bool     edit_open_  = false;
    bool     edit_focus_ = false;
    uint64_t edit_addr_  = 0;
    int      edit_kind_  = ec_hex;
    int      edit_size_  = 1;
    int      edit_type_  = 0;
    bool     edit_hex_input_ = false;
    char     edit_buf_[256] = {};

    char     goto_buf_[64] = {};
};

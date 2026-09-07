#ifndef UI_STATE_H
#define UI_STATE_H

#include <cstdint>
#include <string>
#include <vector>

#include "type/scan_data_stream_define.h"   // scan_data_type / scan_type / next_scan_type
#include "type/module_info.h"

// 内存浏览器当前激活的视图（上侧反汇编 / 下侧十六进制 dump）。
// 与 x64dbg 的 CPU 窗口一致：顶部反汇编 + 底部十六进制 TAB。
enum class memory_viewer_mode {
    disassembly = 0,
    hexdump     = 1,
};

struct ui_state {
    bool show_about_window  = false;
    bool show_debug_window  = false;
    bool show_memory_window = false;
    bool show_assembler_window = false;

    // 设置页面
    bool        show_settings_window = false;
    int         theme     = 0;                    // 0 = Dark, 1 = Light
    std::string cache_dir;                        // 缓存目录（空 = 默认，用 %TEMP%/MyScanApp_Data/<pid>）

    scan_data_type data_type_ = scan_data_type::int32;   // 数值/字符串/字节数组/All/结构体
    char         scan_value[0x400] = "100";          // 搜索值输入框缓冲
    char         scan_value2[0x400] = "";            // Between 第二个输入框
    bool         hex          = false;               // Hex 勾选
    bool         case_sensitive = true;              // 字符串区分大小写

    scan_type     first_scan_type_  = scan_type::exact_value;   // 首次扫描条件
    next_scan_type next_scan_type_   = next_scan_type::equal;    // 再次扫描条件

    bool first_scan_done = false;   // 是否已完成过首次扫描（决定条件列表内容）

    // 内存扫描选项
    bool       writable      = false;
    bool       executable    = false;
    bool       copy_on_write = false;
    bool       fast_scan     = true;
    bool       not_match     = false;
    bool       include_code  = true;

    // 扫描模式选择：首次扫描(First)/再次扫描(Next)的 UI 状态
    scan_mode   scan_mode = scan_mode::first;

    // 显示进程列表
    bool       show_process_window = false;

    // 显示进程详情窗口（模块 / 内存区域 TAB 页）
    bool       show_process_detail = false;

    // 内存浏览器：激活视图 + 两个视图各自的跳转地址（x64dbg 风格：上反汇编 / 下 dump）。
    memory_viewer_mode memory_view_mode = memory_viewer_mode::hexdump;
    uint64_t           disasm_view_address = 0;
    uint64_t           dump_view_address   = 0;

    // ===== 附加进程后的模块下拉缓存（跨帧保留，避免每帧重新枚举）=====
    uint32_t                attached_pid   = 0;
    bool                    modules_loaded = false;
    int                     module_selected = -1;  // 0 = 全部模块
    std::vector<std::string> module_names;
    std::vector<module_info> module_infos;         // 与 module_names 对齐（索引 -1）

    // 扫描输入错误提示（空 = 无错误）
    std::string scan_error;
};

#endif // UI_STATE_H

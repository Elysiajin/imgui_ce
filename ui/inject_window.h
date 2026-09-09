#ifndef INJECT_WINDOW_H
#define INJECT_WINDOW_H

#include <string>
#include <vector>
#include <windows.h>

#include "ui/file_browser.h"
#include "libs/S_inject/include/app/Injector.hpp"

// 注入窗口（S-inject XInject::Injector 后端）。
//
// 归属：作为 memory_window 的成员，由内存窗口菜单栏 Tools -> Inject 打开，
// 自身持有可见性状态，渲染自包含的独立 ImGui 窗口。
// 功能：
//   - 列出可注入的 64 位进程并选择
//   - 文件浏览器选择要注入的 DLL（默认过滤 *.dll）
//   - 选择注入方式（远程线程 / 反射 / APC / 上下文）
//   - 注入 / 卸载 DLL，附带状态日志
class inject_window
{
public:
    inject_window();

    void open()  { open_ = true; }   // 由 memory_window 的 Inject 菜单项调用
    void close() { open_ = false; }
    bool is_open() const { return open_; }

    void render();                   // 若 open_ 则渲染自己的注入窗口

private:
    void refresh_processes();
    void do_inject();
    void do_unload();
    void log(const std::string& line);

    bool open_ = false;              // 窗口可见性（自包含，不依赖 ui_state）
    DWORD attached_pid_ = 0;         // 已附加进程 PID（仅可注入该进程；0=未附加）

    file_browser browser_;           // DLL 选择器
    std::vector<ProcessInfo> procs_; // 全局 typedef（见 Injector.hpp）
    int selected_proc_ = -1;

    std::vector<std::string> method_names_;
    int method_ = 0;                 // 选中注入方式下标

    char unload_name_[128]{};        // 待卸载 DLL 基名
    bool show_hex_ = false;

    std::string log_text_;           // 状态日志
};

#endif // INJECT_WINDOW_H
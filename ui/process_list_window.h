#ifndef PROCESS_LIST_WINDOW_H
#define PROCESS_LIST_WINDOW_H

#include "ui/ui_state.h"
#include "type/process_info.h"

#include <chrono>
#include <vector>

class process_list_window
{
public:
    explicit process_list_window(ui_state& ui) : state_(ui), show_hex(false) {}

    void render();

private:
    ui_state& state_;
    bool show_hex;                              // 进程id以16进制显示
    std::vector<process_info> process_list_;    // 缓存的进程列表
    int selected_pid_ = -1;
    std::chrono::steady_clock::time_point last_refresh_{};  // 上次刷新时刻
};

#endif // PROCESS_LIST_WINDOW_H

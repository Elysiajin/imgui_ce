#ifndef PROCESS_DETAIL_WINDOW_H
#define PROCESS_DETAIL_WINDOW_H

#include "ui/ui_state.h"
#include "type/memory_region.h"
#include "type/module_info.h"

#include <vector>

class process_detail_window
{
public:
    explicit process_detail_window(ui_state& ui) : state_(ui) {}

    void render();

private:
    void render_modules_tab();
    void render_regions_tab();

    ui_state& state_;

    std::vector<module_info>  modules_;   // 缓存的模块列表
    std::vector<memory_region> regions_;  // 缓存的内存区域列表
};

#endif // PROCESS_DETAIL_WINDOW_H

#ifndef PROCESS_DETAIL_WINDOW_H
#define PROCESS_DETAIL_WINDOW_H

#include "ui/ui_state.h"
#include "type/memory_region.h"
#include "type/module_info.h"

#include <cstdint>
#include <string>
#include <vector>

class process_detail_window
{
public:
    explicit process_detail_window(ui_state& ui) : state_(ui) {}

    void render();

private:
    void render_modules_tab();
    void render_regions_tab();

    // 模块符号懒加载：展开/滚动到某模块时才解析其导出表，结果缓存在 symbol_table
    void ensure_module_symbols(int mod_idx);

    ui_state& state_;

    std::vector<module_info>  modules_;   // 缓存的模块列表
    std::vector<memory_region> regions_;  // 缓存的内存区域列表

    // 符号表格行缓存：模块索引 -> 该模块符号表对应的行数据（与 symbol_table 同步重建）
    struct sym_row {
        uint64_t address;
        const char* name;
    };
    int                     sym_module_ = -1;     // 行缓存属于哪个模块（-1 = 无）
    uint32_t                sym_pid_    = 0;      // 行缓存属于哪个进程
    std::vector<sym_row>    sym_rows_;
    char                    sym_filter_[64] = {}; // 符号名过滤（大小写不敏感）
};

#endif // PROCESS_DETAIL_WINDOW_H

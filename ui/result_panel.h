#pragma once

#include "type/scan_data_stream_define.h"
#include "type/value_type.h"

#include <string>

class application_context;

// Result panel reads scan results directly from scan_service, and writes
// selected rows into the shared address list via application_context.
class result_panel {
public:
    explicit result_panel(application_context& ctx) : ctx_(ctx) {}

    void render();

private:
    application_context& ctx_;

    // ---- 手动添加地址对话框 ----
    bool show_add_dialog_ = false;
    char add_desc_buf_[128] = {};
    char add_addr_buf_[128] = {};       // 支持模块+偏移 / hex
    int  add_type_index_ = 2;           // value_type 下标，默认 4 字节
    std::string add_error_;
};

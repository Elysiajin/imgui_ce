#pragma once

#include "type/scan_data_stream_define.h"

class application_context;

// Result panel reads scan results directly from scan_service, and writes
// selected rows into the shared address list via application_context.
class result_panel {
public:
    explicit result_panel(application_context& ctx) : ctx_(ctx) {}

    void render();

private:
    application_context& ctx_;
};

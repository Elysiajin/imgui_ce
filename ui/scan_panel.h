#ifndef SCAN_PANEL_H
#define SCAN_PANEL_H

#include "ui_state.h"

class application_context;

class scan_panel
{
public:
    explicit scan_panel(ui_state& ui, application_context& ctx);

    void render(); // 绘制界面
private:
    ui_state& state_;
    application_context& ctx_;
    bool subscribed_ = false;
};

#endif // SCAN_PANEL_H

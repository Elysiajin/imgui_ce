#pragma once

#include "ui_state.h"

// 设置窗口：配置缓存目录、清理缓存、切换界面风格。
class settings_window
{
public:
    explicit settings_window(ui_state& ui) : state_(ui) {}

    void render();

private:
    ui_state& state_;
};

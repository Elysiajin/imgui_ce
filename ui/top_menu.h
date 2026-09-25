#ifndef TOP_MENU_H
#define TOP_MENU_H

#include "ui/ui_state.h"
#include "ui/file_browser.h"

class top_menu
{
public:
    explicit top_menu(ui_state& ui) : state_(ui) {}

    // 经典布局入口：窗口菜单栏 + CT 弹窗
    void render();

    // 仅渲染 CT 打开/保存弹窗（菜单风外壳由 menu_shell 每帧调用，
    // 弹窗入口按钮在其"工具"页通过 open_ct_dialog 触发）
    void render_ct_dialogs();

    // 打开 CT 弹窗：mode 1 = 加载，2 = 保存
    void open_ct_dialog(int mode);

private:
    void render_menu_bar();

    ui_state& state_;

    // CT 打开/保存弹窗（复用项目自绘 file_browser，即时渲染，无阻塞对话框）
    file_browser browser_;
    int          ct_dialog_ = 0;          // 0=无, 1=打开, 2=保存
    char         ct_save_name_[128] = {}; // 保存模式下的文件名输入
    bool         browser_init_ = false;   // 初次打开时设置过滤器/初始路径
};

#endif // TOP_MENU_H

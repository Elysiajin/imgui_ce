#ifndef UI_MENU_SHELL_H
#define UI_MENU_SHELL_H

#include "ui/ui_state.h"

#include "imgui.h"

#include <cstdint>
#include <string>

struct ImDrawList;
class application_context;
class scan_panel;
class result_panel;
class top_menu;

// ── 菜单风主界面外壳（imgui_menu_example / Clean-imgui-menu 风格移植）──
//
// 复刻例项目的绘制序列：半透明深色圆角主面板 + 左侧图标侧边栏
// （渐变 Logo / TabButton 列表 / 底部进程块）+ 右侧内容区 +
// tab 切换滑动动画。视觉全部 ImDrawList 手绘，不依赖样式表；
// 窗口本身以 NoBackground 模式交由本类画出面板底。
//
// 与经典布局并存：ui_state::layout_mode 由设置窗口切换，
// main.cpp 据此选择调用 menu_shell::render() 或旧布局。
class menu_shell
{
public:
    menu_shell(ui_state& ui, application_context& ctx,
               scan_panel& scan, result_panel& result, top_menu& menu);

    // 在 Begin("主界面") 内调用（窗口须带 NoBackground 标志）
    void render();

private:
    void draw_sidebar(ImDrawList* dl, const ImVec2& p, const ImVec2& ws);        // Logo + tabs + 进程块
    void draw_logo(ImDrawList* dl, const ImVec2& p);                             // 渐变 Logo + 下划线
    void draw_user_block(ImDrawList* dl, const ImVec2& p, const ImVec2& ws);     // 底部进程块
    void render_content();                                                       // 右侧内容区（随动画滑动）

    void render_tab_scan();      // 双列：左扫描 / 右结果
    void render_tab_address();   // 地址列表独占
    void render_tab_tools();     // CT/窗口入口按钮组
    void render_tab_settings();  // 布局 + 主题 + 打开设置窗口
    void render_tab_about();     // 关于 + 图标字形总览

    ui_state&            state_;
    application_context& ctx_;
    scan_panel&          scan_panel_;
    result_panel&        result_panel_;
    top_menu&            top_menu_;

    // tab 状态与切换滑动动画（例项目 size_child 逻辑：
    // 先滑出旧页 0→max，到顶后真正切 tab，再滑入新页 max→0）
    int   tab_current_ = 0;
    int   tab_next_    = 0;
    bool  anim_active_ = false;
    float anim_slide_  = 0.0f;

    // 底部进程块缓存（进程全量枚举较重，仅在 pid 变化时枚举一次）
    uint32_t    user_pid_  = 0;
    std::string user_name_;
    ImTextureID user_icon_ = 0;   // 进程图标纹理（未加载成功为 0）
};

// 扫描状态辉光条（经典布局与菜单风外壳共用；实现自 main.cpp 提取）
void render_scan_status_bar();

#endif // UI_MENU_SHELL_H

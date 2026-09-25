#ifndef UI_MENU_WIDGETS_H
#define UI_MENU_WIDGETS_H

#include "imgui.h"

// ── "菜单风"外壳自定义控件（imgui_menu_example 风格移植）──────
//
// 例项目（Clean-imgui-menu）在其改版 imgui.h 中声明了 TabButton /
// OptButton 但实现源码缺失（只留有预编译 obj），这里按声明语义重写。
// 视觉全部 ImDrawList 手绘；强调色取 ImGuiCol_CheckMark（各主题的
// 主色调），保证跟随主题切换。
namespace menu_widgets
{
    // 侧边栏 tab：左侧图标（fonts::icon）+ 标签；active 项绘制强调色
    // 底色与左侧高亮条。返回 true = 本帧被点击。
    bool TabButton(const char* ico, const char* label, const ImVec2& size_arg, bool active);

    // 圆形图标小按钮（例项目右上角 L/B）。rotation=true 时每次点击图标
    // 旋转 90° 并带回弹动画。str_id 缺省取 ico；同字形多个按钮时须显式传。
    bool OptButton(const char* ico, const ImVec2& size_arg, bool rotation = false,
                   const char* str_id = nullptr);

    // 水平渐变文字：AddText 后对顶点区间做线性 RGB 渐变（保留各自 alpha）。
    // grad_width 为渐变跨度；col_left/col_right 的 alpha 决定文字透明度。
    void GradientText(ImDrawList* draw_list, const ImVec2& pos, ImFont* font, float font_size,
                      const char* text, ImU32 col_left, ImU32 col_right, float grad_width);

    // 调试辅助：在当前窗口渲染图标字体 A~Z 字形总览（挑选 tab 图标用，
    // 菜单风外壳"关于"页可展开查看）。
    void IconFontOverview();
}

#endif // UI_MENU_WIDGETS_H

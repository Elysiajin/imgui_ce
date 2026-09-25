#ifndef THEME_H
#define THEME_H

#include "imgui.h"

// ── 可扩展主题注册表 ──────────────────────────────────────
//
// 每个主题 = 一个名称 + 一个应用函数。新增主题只需在 theme_registry[]
// 追加一项并实现 apply 函数，settings_window 会通过遍历注册表自动列出、
// main.cpp 会在启动时应用当前主题。无需改动任何 UI 逻辑。
//
// 注意：本工程使用 ImGui 1.92，样式枚举为 ImGuiCol_*（如 ChildBg、
// ModalWindowDimBg），不再存在旧版 CloseButton/Column/ComboBg 等。

enum class theme_id {
    Dark      = 0,
    Light     = 1,
    Cyan      = 2,
    Midnight  = 3,
    LightBlue = 4,
    Evicted   = 5,   // 菜单风（imgui_menu_example 风格，配合 layout_mode=1 使用）

    Count
};

namespace theme
{
    // 各主题应用函数（前向声明，供注册表引用）
    inline void apply_cyan();
    inline void apply_midnight();
    inline void apply_lightblue();
    inline void apply_evicted();

    // 主题项：名称（用于设置窗口下拉）+ 应用回调（无参）
    struct item {
        const char* name;
        theme_id    id;
        void        (*apply)();
    };

    // Dark/Light 以无参闭包包装（StyleColorsDark 本身接收 ImGuiStyle* 默认参数）
    inline void apply_dark()  { ImGui::StyleColorsDark(); }
    inline void apply_light() { ImGui::StyleColorsLight(); }

    // 全局可用主题注册表（顺序即设置窗口中的展示顺序）
    inline const item registry[] = {
        { "深色",       theme_id::Dark,       apply_dark },
        { "浅色",      theme_id::Light,      apply_light },
        { "青色",       theme_id::Cyan,       apply_cyan },
        { "午夜",   theme_id::Midnight,   apply_midnight },
        { "浅蓝", theme_id::LightBlue,  apply_lightblue },
        { "菜单风",   theme_id::Evicted,    apply_evicted },
    };

    // 注册表条目数（自动推导，便于遍历）
    constexpr int count = (int)(sizeof(registry) / sizeof(registry[0]));

    // 按 id 应用主题；未找到则保持现状
    inline void apply(theme_id id) {
        for (const auto& t : registry) {
            if (t.id == id && t.apply) { t.apply(); return; }
        }
    }

    // Cyan 主题实现（青色风格，黑底青字）
    inline void apply_cyan()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style.Alpha = 1.0f;
        style.WindowRounding = 3.0f;
        style.ChildRounding = 3.0f;
        style.GrabRounding = 1.0f;
        style.GrabMinSize = 20.0f;
        style.FrameRounding = 3.0f;

        ImVec4* col = style.Colors;
        col[ImGuiCol_Text]                 = ImVec4(0.00f, 1.00f, 1.00f, 1.00f);
        col[ImGuiCol_TextDisabled]         = ImVec4(0.00f, 0.40f, 0.41f, 1.00f);
        col[ImGuiCol_WindowBg]             = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
        col[ImGuiCol_ChildBg]              = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        col[ImGuiCol_Border]               = ImVec4(0.00f, 1.00f, 1.00f, 0.65f);
        col[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        col[ImGuiCol_FrameBg]              = ImVec4(0.44f, 0.80f, 0.80f, 0.18f);
        col[ImGuiCol_FrameBgHovered]       = ImVec4(0.44f, 0.80f, 0.80f, 0.27f);
        col[ImGuiCol_FrameBgActive]        = ImVec4(0.44f, 0.81f, 0.86f, 0.66f);
        col[ImGuiCol_TitleBg]              = ImVec4(0.14f, 0.18f, 0.21f, 0.73f);
        col[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.00f, 0.00f, 0.00f, 0.54f);
        col[ImGuiCol_TitleBgActive]        = ImVec4(0.00f, 0.35f, 0.36f, 1.00f);
        col[ImGuiCol_MenuBarBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.20f);
        col[ImGuiCol_ScrollbarBg]          = ImVec4(0.22f, 0.29f, 0.30f, 0.71f);
        col[ImGuiCol_ScrollbarGrab]        = ImVec4(0.00f, 1.00f, 1.00f, 0.44f);
        col[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.00f, 1.00f, 1.00f, 0.74f);
        col[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.00f, 1.00f, 1.00f, 1.00f);
        col[ImGuiCol_CheckMark]            = ImVec4(0.00f, 1.00f, 1.00f, 0.68f);
        col[ImGuiCol_SliderGrab]           = ImVec4(0.00f, 1.00f, 1.00f, 0.36f);
        col[ImGuiCol_SliderGrabActive]     = ImVec4(0.00f, 1.00f, 1.00f, 0.76f);
        col[ImGuiCol_Button]               = ImVec4(0.00f, 0.65f, 0.65f, 0.46f);
        col[ImGuiCol_ButtonHovered]        = ImVec4(0.01f, 1.00f, 1.00f, 0.43f);
        col[ImGuiCol_ButtonActive]         = ImVec4(0.00f, 1.00f, 1.00f, 0.62f);
        col[ImGuiCol_Header]               = ImVec4(0.00f, 1.00f, 1.00f, 0.33f);
        col[ImGuiCol_HeaderHovered]        = ImVec4(0.00f, 1.00f, 1.00f, 0.42f);
        col[ImGuiCol_HeaderActive]         = ImVec4(0.00f, 1.00f, 1.00f, 0.54f);
        col[ImGuiCol_ResizeGrip]           = ImVec4(0.00f, 1.00f, 1.00f, 0.54f);
        col[ImGuiCol_ResizeGripHovered]    = ImVec4(0.00f, 1.00f, 1.00f, 0.74f);
        col[ImGuiCol_ResizeGripActive]     = ImVec4(0.00f, 1.00f, 1.00f, 1.00f);
        col[ImGuiCol_PlotLines]            = ImVec4(0.00f, 1.00f, 1.00f, 1.00f);
        col[ImGuiCol_PlotLinesHovered]     = ImVec4(0.00f, 1.00f, 1.00f, 1.00f);
        col[ImGuiCol_PlotHistogram]        = ImVec4(0.00f, 1.00f, 1.00f, 1.00f);
        col[ImGuiCol_PlotHistogramHovered] = ImVec4(0.00f, 1.00f, 1.00f, 1.00f);
        col[ImGuiCol_TextSelectedBg]       = ImVec4(0.00f, 1.00f, 1.00f, 0.22f);

        col[ImGuiCol_PopupBg]              = ImVec4(0.16f, 0.24f, 0.22f, 0.90f);
        col[ImGuiCol_TableHeaderBg]        = ImVec4(0.00f, 1.00f, 1.00f, 0.33f);
        col[ImGuiCol_TableBorderStrong]    = ImVec4(0.00f, 1.00f, 1.00f, 0.65f);
        col[ImGuiCol_TableBorderLight]     = ImVec4(0.00f, 0.50f, 0.50f, 0.40f);
        col[ImGuiCol_TableRowBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        col[ImGuiCol_TableRowBgAlt]        = ImVec4(0.00f, 1.00f, 1.00f, 0.05f);
        col[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.04f, 0.10f, 0.09f, 0.51f);
    }

    // Midnight 主题（暗灰蓝底 + 红色强调色）
    inline void apply_midnight()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowMinSize     = ImVec2(160, 20);
        style.FramePadding      = ImVec2(4, 2);
        style.ItemSpacing       = ImVec2(6, 2);
        style.ItemInnerSpacing  = ImVec2(2, 4);
        style.Alpha             = 0.95f;
        style.WindowRounding    = 4.0f;
        style.FrameRounding     = 2.0f;
        style.IndentSpacing     = 6.0f;
        style.GrabMinSize       = 14.0f;
        style.GrabRounding      = 16.0f;
        style.ScrollbarSize     = 12.0f;
        style.ScrollbarRounding = 16.0f;

        ImVec4* col = style.Colors;
        col[ImGuiCol_Text]                  = ImVec4(0.86f, 0.93f, 0.89f, 0.78f);
        col[ImGuiCol_TextDisabled]          = ImVec4(0.86f, 0.93f, 0.89f, 0.28f);
        col[ImGuiCol_WindowBg]              = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
        col[ImGuiCol_ChildBg]               = ImVec4(0.13f, 0.14f, 0.17f, 0.00f);
        col[ImGuiCol_Border]                = ImVec4(0.31f, 0.31f, 1.00f, 0.00f);
        col[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        col[ImGuiCol_FrameBg]               = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
        col[ImGuiCol_FrameBgHovered]        = ImVec4(0.92f, 0.18f, 0.29f, 0.78f);
        col[ImGuiCol_FrameBgActive]         = ImVec4(0.92f, 0.18f, 0.29f, 1.00f);
        col[ImGuiCol_TitleBg]               = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
        col[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.20f, 0.22f, 0.27f, 0.75f);
        col[ImGuiCol_TitleBgActive]         = ImVec4(0.92f, 0.18f, 0.29f, 1.00f);
        col[ImGuiCol_MenuBarBg]             = ImVec4(0.20f, 0.22f, 0.27f, 0.47f);
        col[ImGuiCol_ScrollbarBg]           = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
        col[ImGuiCol_ScrollbarGrab]         = ImVec4(0.09f, 0.15f, 0.16f, 1.00f);
        col[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.92f, 0.18f, 0.29f, 0.78f);
        col[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.92f, 0.18f, 0.29f, 1.00f);
        col[ImGuiCol_CheckMark]             = ImVec4(0.71f, 0.22f, 0.27f, 1.00f);
        col[ImGuiCol_SliderGrab]            = ImVec4(0.47f, 0.77f, 0.83f, 0.14f);
        col[ImGuiCol_SliderGrabActive]      = ImVec4(0.92f, 0.18f, 0.29f, 1.00f);
        col[ImGuiCol_Button]                = ImVec4(0.47f, 0.77f, 0.83f, 0.14f);
        col[ImGuiCol_ButtonHovered]         = ImVec4(0.92f, 0.18f, 0.29f, 0.86f);
        col[ImGuiCol_ButtonActive]          = ImVec4(0.92f, 0.18f, 0.29f, 1.00f);
        col[ImGuiCol_Header]                = ImVec4(0.92f, 0.18f, 0.29f, 0.76f);
        col[ImGuiCol_HeaderHovered]         = ImVec4(0.92f, 0.18f, 0.29f, 0.86f);
        col[ImGuiCol_HeaderActive]          = ImVec4(0.92f, 0.18f, 0.29f, 1.00f);
        col[ImGuiCol_Separator]             = ImVec4(0.14f, 0.16f, 0.19f, 1.00f);
        col[ImGuiCol_SeparatorHovered]      = ImVec4(0.92f, 0.18f, 0.29f, 0.78f);
        col[ImGuiCol_SeparatorActive]       = ImVec4(0.92f, 0.18f, 0.29f, 1.00f);
        col[ImGuiCol_ResizeGrip]            = ImVec4(0.47f, 0.77f, 0.83f, 0.04f);
        col[ImGuiCol_ResizeGripHovered]     = ImVec4(0.92f, 0.18f, 0.29f, 0.78f);
        col[ImGuiCol_ResizeGripActive]      = ImVec4(0.92f, 0.18f, 0.29f, 1.00f);
        col[ImGuiCol_PlotLines]             = ImVec4(0.86f, 0.93f, 0.89f, 0.63f);
        col[ImGuiCol_PlotLinesHovered]      = ImVec4(0.92f, 0.18f, 0.29f, 1.00f);
        col[ImGuiCol_PlotHistogram]         = ImVec4(0.86f, 0.93f, 0.89f, 0.63f);
        col[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.92f, 0.18f, 0.29f, 1.00f);
        col[ImGuiCol_TextSelectedBg]        = ImVec4(0.92f, 0.18f, 0.29f, 0.43f);
        col[ImGuiCol_PopupBg]               = ImVec4(0.20f, 0.22f, 0.27f, 0.90f);
        col[ImGuiCol_TableHeaderBg]         = ImVec4(0.92f, 0.18f, 0.29f, 0.76f);
        col[ImGuiCol_TableBorderStrong]     = ImVec4(0.31f, 0.31f, 1.00f, 0.65f);
        col[ImGuiCol_TableBorderLight]      = ImVec4(0.31f, 0.31f, 1.00f, 0.35f);
        col[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        col[ImGuiCol_TableRowBgAlt]         = ImVec4(0.86f, 0.93f, 0.89f, 0.06f);
        col[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.20f, 0.22f, 0.27f, 0.73f);
    }

    // Light Blue 主题（Pacôme Danhiez 浅色风格，蓝色强调色）
    inline void apply_lightblue()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style.Alpha = 1.0f;
        style.FrameRounding = 3.0f;

        ImVec4* col = style.Colors;
        col[ImGuiCol_Text]                  = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
        col[ImGuiCol_TextDisabled]          = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
        col[ImGuiCol_WindowBg]              = ImVec4(0.94f, 0.94f, 0.94f, 0.94f);
        col[ImGuiCol_ChildBg]               = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        col[ImGuiCol_PopupBg]               = ImVec4(1.00f, 1.00f, 1.00f, 0.94f);
        col[ImGuiCol_Border]                = ImVec4(0.00f, 0.00f, 0.00f, 0.39f);
        col[ImGuiCol_BorderShadow]          = ImVec4(1.00f, 1.00f, 1.00f, 0.10f);
        col[ImGuiCol_FrameBg]               = ImVec4(1.00f, 1.00f, 1.00f, 0.94f);
        col[ImGuiCol_FrameBgHovered]        = ImVec4(0.26f, 0.59f, 0.98f, 0.40f);
        col[ImGuiCol_FrameBgActive]         = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
        col[ImGuiCol_TitleBg]               = ImVec4(0.96f, 0.96f, 0.96f, 1.00f);
        col[ImGuiCol_TitleBgCollapsed]      = ImVec4(1.00f, 1.00f, 1.00f, 0.51f);
        col[ImGuiCol_TitleBgActive]         = ImVec4(0.82f, 0.82f, 0.82f, 1.00f);
        col[ImGuiCol_MenuBarBg]             = ImVec4(0.86f, 0.86f, 0.86f, 1.00f);
        col[ImGuiCol_ScrollbarBg]           = ImVec4(0.98f, 0.98f, 0.98f, 0.53f);
        col[ImGuiCol_ScrollbarGrab]         = ImVec4(0.69f, 0.69f, 0.69f, 1.00f);
        col[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.59f, 0.59f, 0.59f, 1.00f);
        col[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.49f, 0.49f, 0.49f, 1.00f);
        col[ImGuiCol_CheckMark]             = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
        col[ImGuiCol_SliderGrab]            = ImVec4(0.24f, 0.52f, 0.88f, 1.00f);
        col[ImGuiCol_SliderGrabActive]      = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
        col[ImGuiCol_Button]                = ImVec4(0.26f, 0.59f, 0.98f, 0.40f);
        col[ImGuiCol_ButtonHovered]         = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
        col[ImGuiCol_ButtonActive]          = ImVec4(0.06f, 0.53f, 0.98f, 1.00f);
        col[ImGuiCol_Header]                = ImVec4(0.26f, 0.59f, 0.98f, 0.31f);
        col[ImGuiCol_HeaderHovered]         = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
        col[ImGuiCol_HeaderActive]          = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
        col[ImGuiCol_Separator]             = ImVec4(0.39f, 0.39f, 0.39f, 1.00f);
        col[ImGuiCol_SeparatorHovered]      = ImVec4(0.26f, 0.59f, 0.98f, 0.78f);
        col[ImGuiCol_SeparatorActive]       = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
        col[ImGuiCol_ResizeGrip]            = ImVec4(1.00f, 1.00f, 1.00f, 0.50f);
        col[ImGuiCol_ResizeGripHovered]     = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
        col[ImGuiCol_ResizeGripActive]      = ImVec4(0.26f, 0.59f, 0.98f, 0.95f);
        col[ImGuiCol_PlotLines]             = ImVec4(0.39f, 0.39f, 0.39f, 1.00f);
        col[ImGuiCol_PlotLinesHovered]      = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
        col[ImGuiCol_PlotHistogram]         = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
        col[ImGuiCol_PlotHistogramHovered]  = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
        col[ImGuiCol_TextSelectedBg]        = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
        col[ImGuiCol_TableHeaderBg]         = ImVec4(0.26f, 0.59f, 0.98f, 0.31f);
        col[ImGuiCol_TableBorderStrong]     = ImVec4(0.39f, 0.39f, 0.39f, 1.00f);
        col[ImGuiCol_TableBorderLight]      = ImVec4(0.39f, 0.39f, 0.39f, 0.50f);
        col[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        col[ImGuiCol_TableRowBgAlt]         = ImVec4(0.00f, 0.00f, 0.00f, 0.05f);
        col[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.20f, 0.20f, 0.20f, 0.35f);
    }

    // Evicted 主题（菜单风，imgui_menu_example 风格移植；命名取自例项目 Logo）
    //
    // 色值出处为例项目 main.cpp 的硬编码绘制色：主面板 ImColor(9,9,9,180)、
    // 侧边栏/分割线 (25,25,25,180)、强调色为默认 Widget Color RGB(190,38,38)。
    // 例项目透出的是模糊背景图，本项目按约定透出深色纯底，故 WindowBg 取
    // 近似不透明的深灰黑（保留少量透明度以维持玻璃质感）。
    inline void apply_evicted()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding    = 8.0f;
        style.ChildRounding     = 8.0f;
        style.PopupRounding     = 8.0f;
        style.FrameRounding     = 4.0f;
        style.GrabRounding      = 4.0f;
        style.ScrollbarRounding = 8.0f;
        style.TabRounding       = 6.0f;
        style.WindowPadding     = ImVec2(10.0f, 10.0f);
        style.FramePadding      = ImVec2(6.0f, 4.0f);
        style.ItemSpacing       = ImVec2(8.0f, 6.0f);
        style.ScrollbarSize     = 13.0f;

        const ImVec4 accent    (0.745f, 0.149f, 0.149f, 1.00f);   // RGB(190,38,38)
        const ImVec4 accent_hi (0.880f, 0.260f, 0.260f, 1.00f);   // hover 提亮
        const ImVec4 accent_lo (0.580f, 0.110f, 0.110f, 1.00f);   // active 压暗

        ImVec4* col = style.Colors;
        col[ImGuiCol_Text]                  = ImVec4(0.92f, 0.92f, 0.94f, 1.00f);
        col[ImGuiCol_TextDisabled]          = ImVec4(0.52f, 0.52f, 0.55f, 1.00f);
        col[ImGuiCol_WindowBg]              = ImVec4(0.035f, 0.035f, 0.039f, 0.94f);  // (9,9,9)
        col[ImGuiCol_ChildBg]               = ImVec4(0.098f, 0.098f, 0.102f, 0.62f);  // (25,25,25)
        col[ImGuiCol_PopupBg]               = ImVec4(0.070f, 0.070f, 0.075f, 0.97f);
        col[ImGuiCol_Border]                = ImVec4(0.24f, 0.24f, 0.25f, 0.55f);
        col[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

        col[ImGuiCol_FrameBg]               = ImVec4(0.130f, 0.130f, 0.140f, 1.00f);
        col[ImGuiCol_FrameBgHovered]        = ImVec4(0.170f, 0.170f, 0.180f, 1.00f);
        col[ImGuiCol_FrameBgActive]         = ImVec4(accent.x * 0.45f, accent.y * 0.45f, accent.z * 0.45f, 1.00f);

        col[ImGuiCol_TitleBg]               = ImVec4(0.045f, 0.045f, 0.050f, 1.00f);
        col[ImGuiCol_TitleBgActive]         = ImVec4(0.060f, 0.060f, 0.065f, 1.00f);
        col[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.045f, 0.045f, 0.050f, 0.75f);
        col[ImGuiCol_MenuBarBg]             = ImVec4(0.070f, 0.070f, 0.075f, 1.00f);

        col[ImGuiCol_ScrollbarBg]           = ImVec4(0.060f, 0.060f, 0.065f, 1.00f);
        col[ImGuiCol_ScrollbarGrab]         = ImVec4(0.220f, 0.220f, 0.230f, 1.00f);
        col[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.320f, 0.320f, 0.330f, 1.00f);
        col[ImGuiCol_ScrollbarGrabActive]   = accent;

        col[ImGuiCol_CheckMark]             = accent;
        col[ImGuiCol_SliderGrab]            = accent;
        col[ImGuiCol_SliderGrabActive]      = accent_hi;

        col[ImGuiCol_Button]                = ImVec4(0.150f, 0.150f, 0.160f, 1.00f);
        col[ImGuiCol_ButtonHovered]         = accent;
        col[ImGuiCol_ButtonActive]          = accent_lo;

        col[ImGuiCol_Header]                = ImVec4(accent.x, accent.y, accent.z, 0.55f);
        col[ImGuiCol_HeaderHovered]         = ImVec4(accent_hi.x, accent_hi.y, accent_hi.z, 0.75f);
        col[ImGuiCol_HeaderActive]          = accent;

        col[ImGuiCol_Separator]             = ImVec4(0.24f, 0.24f, 0.25f, 0.55f);
        col[ImGuiCol_SeparatorHovered]      = accent;
        col[ImGuiCol_SeparatorActive]       = accent_hi;

        col[ImGuiCol_ResizeGrip]            = ImVec4(0.24f, 0.24f, 0.25f, 0.30f);
        col[ImGuiCol_ResizeGripHovered]     = accent;
        col[ImGuiCol_ResizeGripActive]      = accent_hi;

        col[ImGuiCol_Tab]                   = ImVec4(0.100f, 0.100f, 0.105f, 1.00f);
        col[ImGuiCol_TabHovered]            = ImVec4(accent_hi.x, accent_hi.y, accent_hi.z, 0.80f);
        col[ImGuiCol_TabSelected]           = ImVec4(accent.x, accent.y, accent.z, 0.75f);
        col[ImGuiCol_TabDimmed]             = ImVec4(0.080f, 0.080f, 0.085f, 1.00f);
        col[ImGuiCol_TabDimmedSelected]     = ImVec4(0.120f, 0.100f, 0.100f, 1.00f);
        col[ImGuiCol_DockingPreview]        = ImVec4(accent.x, accent.y, accent.z, 0.50f);
        col[ImGuiCol_DockingEmptyBg]        = ImVec4(0.045f, 0.045f, 0.050f, 1.00f);

        col[ImGuiCol_PlotLines]             = ImVec4(0.75f, 0.75f, 0.78f, 1.00f);
        col[ImGuiCol_PlotLinesHovered]      = accent_hi;
        col[ImGuiCol_PlotHistogram]         = accent;
        col[ImGuiCol_PlotHistogramHovered]  = accent_hi;

        col[ImGuiCol_TextSelectedBg]        = ImVec4(accent.x, accent.y, accent.z, 0.35f);
        col[ImGuiCol_DragDropTarget]        = accent_hi;
        col[ImGuiCol_NavCursor]             = ImVec4(accent.x, accent.y, accent.z, 0.80f);

        col[ImGuiCol_TableHeaderBg]         = ImVec4(0.120f, 0.120f, 0.130f, 1.00f);
        col[ImGuiCol_TableBorderStrong]     = ImVec4(0.280f, 0.280f, 0.290f, 1.00f);
        col[ImGuiCol_TableBorderLight]      = ImVec4(0.190f, 0.190f, 0.200f, 1.00f);
        col[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        col[ImGuiCol_TableRowBgAlt]         = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

        col[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.00f, 0.00f, 0.00f, 0.60f);
    }
}

#endif // THEME_H
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

    Count
};

namespace theme
{
    // 各主题应用函数（前向声明，供注册表引用）
    inline void apply_cyan();
    inline void apply_midnight();
    inline void apply_lightblue();

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
}

#endif // THEME_H
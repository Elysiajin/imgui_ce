#include "ui/fonts.h"

#include "imgui.h"

// 图标字体 TTF 数据（来自 imgui_menu_example 的 ico_font.h，数组名 icon[]）。
// 仅在本文件展开，避免全局符号多份定义；::icon 显式取全局作用域，
// 与 fonts::icon 区分。
#include "ico_font.h"

void fonts::load_icon()
{
    ImGuiIO& io = ImGui::GetIO();
    icon = io.Fonts->AddFontFromMemoryTTF(::icon, (int)sizeof(::icon), 18.0f, nullptr,
                                          io.Fonts->GetGlyphRangesDefault());
}

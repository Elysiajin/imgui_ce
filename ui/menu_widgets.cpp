#include "menu_widgets.h"

#include "fonts.h"
#include "imgui_internal.h"   // ShadeVertsLinearColorGradientKeepAlpha

#include <cmath>
#include <map>

namespace {

// 按帧率无关的指数趋近做动画插值
float approach(float cur, float target, float dt, float speed = 14.0f)
{
    const float k = 1.0f - std::exp(-speed * dt);
    return cur + (target - cur) * k;
}

} // namespace

bool menu_widgets::TabButton(const char* ico, const char* label, const ImVec2& size_arg, bool active)
{
    const ImVec2 size = ImGui::CalcItemSize(size_arg, ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
    const ImVec2 pos  = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, size);
    const bool clicked = ImGui::IsItemClicked();
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 accent   = ImGui::GetColorU32(ImGuiCol_CheckMark);
    const ImU32 text_col = ImGui::GetColorU32(active ? ImGuiCol_Text : ImGuiCol_TextDisabled);

    // 激活项：半透明强调底 + 左侧高亮条；悬停项：淡底
    if (active) {
        dl->AddRectFilled(pos, pos + size, ImGui::GetColorU32(ImGuiCol_Header, 0.30f), 6.0f);
        dl->AddRectFilled(pos + ImVec2(0.0f, size.y * 0.22f),
                          pos + ImVec2(3.0f, size.y * 0.78f), accent, 1.5f);
    } else if (hovered) {
        dl->AddRectFilled(pos, pos + size, ImGui::GetColorU32(ImGuiCol_Header, 0.14f), 6.0f);
    }

    // 图标（fonts::icon 单独绘制）+ 标签；缺字体时整行退化为纯文本居中
    float text_x = pos.x + size.x * 0.5f;
    if (fonts::icon) {
        const float       icon_size = size.y * 0.46f;
        const ImVec2      isz = fonts::icon->CalcTextSizeA(icon_size, FLT_MAX, 0.0f, ico);
        const ImU32       ico_col = active ? accent : ImGui::GetColorU32(ImGuiCol_Text, 0.85f);
        dl->AddText(fonts::icon, icon_size,
                    pos + ImVec2(size.x * 0.22f - isz.x * 0.5f, (size.y - isz.y) * 0.5f),
                    ico_col, ico);
        text_x = pos.x + size.x * 0.58f;
    }
    const ImVec2 tsz = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(text_x - tsz.x * 0.5f, pos.y + (size.y - tsz.y) * 0.5f), text_col, label);

    return clicked;
}

bool menu_widgets::OptButton(const char* ico, const ImVec2& size_arg, bool rotation, const char* str_id)
{
    const char* id_str = str_id ? str_id : ico;
    const ImVec2 size = ImGui::CalcItemSize(size_arg, 30.0f, 30.0f);
    const ImVec2 pos  = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id_str, size);
    const bool clicked = ImGui::IsItemClicked();
    const bool hovered = ImGui::IsItemHovered();
    const bool held    = ImGui::IsItemActive();

    ImDrawList*    dl     = ImGui::GetWindowDrawList();
    const ImGuiID  id     = ImGui::GetID(id_str);
    const ImVec2   center = pos + size * 0.5f;
    const float    radius = size.x * 0.5f;
    const float    dt     = ImGui::GetIO().DeltaTime;

    // 旋转动画状态：rotation=true 的按钮每次点击目标角 +90°，当前角指数趋近
    static std::map<ImGuiID, float> s_angle;
    static std::map<ImGuiID, float> s_target;
    float angle = 0.0f;
    if (rotation) {
        float& target = s_target[id];
        float& cur    = s_angle[id];
        if (clicked)
            target += 1.5707963f;
        cur   = approach(cur, target, dt);
        angle = cur;
    }

    // 圆底 + 描边（悬停描边用强调色，按压轻微收缩）
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
    ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg, hovered ? 0.9f : 0.55f);
    if (held)
        bg = ImGui::GetColorU32(ImGuiCol_Header, 0.9f);
    dl->AddCircleFilled(center, radius * (held ? 0.93f : 1.0f), bg);
    dl->AddCircle(center, radius, hovered ? accent : ImGui::GetColorU32(ImGuiCol_Border), 0, 1.5f);

    // 图标；带旋转时 AddText 后手动旋转顶点区间（AddText 不支持角度）
    if (fonts::icon) {
        const float  icon_size = size.y * 0.5f;
        const ImVec2 isz = fonts::icon->CalcTextSizeA(icon_size, FLT_MAX, 0.0f, ico);
        const ImVec2 ipos = center - isz * 0.5f;
        const int    v0 = dl->VtxBuffer.Size;
        dl->AddText(fonts::icon, icon_size, ipos, ImGui::GetColorU32(ImGuiCol_Text), ico);
        if (angle != 0.0f) {
            const int   v1 = dl->VtxBuffer.Size;
            const float s  = std::sin(angle);
            const float c  = std::cos(angle);
            for (int i = v0; i < v1; ++i) {
                ImVec2&       vp = dl->VtxBuffer[i].pos;
                const ImVec2  d  = vp - center;
                vp = center + ImVec2(d.x * c - d.y * s, d.x * s + d.y * c);
            }
        }
    }
    return clicked;
}

void menu_widgets::GradientText(ImDrawList* draw_list, const ImVec2& pos, ImFont* font, float font_size,
                                const char* text, ImU32 col_left, ImU32 col_right, float grad_width)
{
    if (!draw_list || !font || !text || !*text)
        return;
    const int v0 = draw_list->VtxBuffer.Size;
    draw_list->AddText(font, font_size, pos, col_left, text);
    const int v1 = draw_list->VtxBuffer.Size;
    if (v1 == v0)
        return;
    // 顶点颜色已写入 col_left，这里沿水平方向在两色间渐变（保留顶点 alpha）
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw_list, v0, v1, pos,
                                                  pos + ImVec2(grad_width, 0.0f),
                                                  col_left, col_right);
}

void menu_widgets::IconFontOverview()
{
    if (!fonts::icon) {
        ImGui::TextDisabled("图标字体未加载");
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::TextDisabled("图标字体字形总览（fonts::icon，A~Z）");
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 13; ++col) {
            if (col)
                ImGui::SameLine(0.0f, 14.0f);
            const char      ch  = (char)('A' + row * 13 + col);
            const char      buf[2] = { ch, '\0' };
            ImGui::Text("%c", ch);
            ImGui::SameLine(0.0f, 2.0f);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddText(fonts::icon, 22.0f, p, IM_COL32_WHITE, buf);
            ImGui::Dummy(ImVec2(26.0f, 28.0f));
        }
    }
}

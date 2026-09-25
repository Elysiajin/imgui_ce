#include "menu_shell.h"

#include "app_context.h"
#include "fonts.h"
#include "menu_widgets.h"
#include "process_icon_cache.h"
#include "result_panel.h"
#include "scan_panel.h"
#include "theme.h"
#include "top_menu.h"

#include "core/process_manager.h"
#include "scan/scan_service.h"

#include "imgui.h"

#include <processthreadsapi.h>

#include <cmath>
#include <cstdio>

namespace {

// ── 布局常量（例项目 main.cpp 的坐标体系：面板 905x624、侧边栏 190）──
constexpr float k_sidebar_w   = 190.0f;   // 侧边栏宽
constexpr float k_header_h    = 75.0f;    // Logo 区高度（1px 分割线所在 y）
constexpr float k_tab_h       = 40.0f;    // TabButton 高
constexpr float k_content_gap = 13.0f;    // 内容区与侧边栏的间距
constexpr float k_rounding    = 10.0f;    // 主面板圆角
constexpr float k_slide_max   = 10.0f;    // tab 切换滑动幅度（px）
constexpr float k_slide_speed = 60.0f;    // 滑动速度 px/s（例项目 1/Framerate*60/帧）

// tab 定义。ico 为图标字体的 ASCII 字形索引，可在"关于"页展开
// IconFontOverview 挑选后调整。
struct tab_def { const char* ico; const char* label; };
constexpr tab_def k_tabs[] = {
    { "N", "扫描" },
    { "I", "地址列表" },
    { "T", "工具" },
    { "O", "设置" },
    { "S", "关于" },
};
constexpr int k_tab_count = (int)(sizeof(k_tabs) / sizeof(k_tabs[0]));

} // namespace

menu_shell::menu_shell(ui_state& ui, application_context& ctx,
                       scan_panel& scan, result_panel& result, top_menu& menu)
    : state_(ui), ctx_(ctx), scan_panel_(scan), result_panel_(result), top_menu_(menu)
{
}

void menu_shell::render()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p  = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();

    // tab 滑动动画推进：滑出阶段 0→max（旧页上移），到顶切 tab，
    // 滑入阶段 max→0（新页落位）。速度按例项目折算 60px/s。
    const float dt = ImGui::GetIO().DeltaTime;
    if (anim_active_) {
        anim_slide_ += dt * k_slide_speed;
        if (anim_slide_ >= k_slide_max) {
            anim_slide_  = k_slide_max;
            tab_current_ = tab_next_;
            anim_active_ = false;
        }
    } else if (anim_slide_ > 0.0f) {
        anim_slide_ -= dt * k_slide_speed;
        if (anim_slide_ < 0.0f)
            anim_slide_ = 0.0f;
    }

    // 1) 主面板底：例项目 ImColor(9,9,9,180)，窗口 NoBackground 由这里手画
    dl->AddRectFilled(p, p + ws, IM_COL32(9, 9, 9, 180), k_rounding);

    // 2) Logo 区下的 1px 分割线（例项目 ImColor(25,25,25,180)）
    dl->AddRectFilled(p + ImVec2(k_sidebar_w, k_header_h),
                      p + ImVec2(ws.x - 2.0f, k_header_h + 1.0f),
                      IM_COL32(25, 25, 25, 180));

    // 3) 右上角圆形按钮：'L' 打开进程选择，'B'(带旋转) 退出程序
    ImGui::SetCursorPos(ImVec2(ws.x - 88.0f - ImGui::GetStyle().WindowPadding.x, 22.0f));
    if (menu_widgets::OptButton("L", ImVec2(30, 30), false))
        state_.show_process_window = true;
    ImGui::SameLine(0.0f, 8.0f);
    if (menu_widgets::OptButton("B", ImVec2(30, 30), true))
        ExitProcess(0);

    // 4) 侧边栏底：ChildBg 色，仅左侧圆角（例项目做法）
    dl->AddRectFilled(p, p + ImVec2(k_sidebar_w, ws.y),
                      ImGui::GetColorU32(ImGuiCol_ChildBg), k_rounding,
                      ImDrawFlags_RoundCornersLeft);

    // 5) 侧边栏内容（Logo / tabs / 进程块）+ 6) 内容区
    draw_sidebar(dl, p, ws);
    render_content();

    // 7) CT 打开/保存弹窗（"工具"页按钮经 open_ct_dialog 触发）
    top_menu_.render_ct_dialogs();
}

void menu_shell::draw_sidebar(ImDrawList* dl, const ImVec2& p, const ImVec2& ws)
{
    draw_logo(dl, p);

    // tab 列表（例项目 190x40 TabButton）
    const float pad_y = ImGui::GetStyle().WindowPadding.y;
    ImGui::SetCursorPos(ImVec2(0.0f, k_header_h + 14.0f - pad_y));
    for (int i = 0; i < k_tab_count; ++i) {
        if (menu_widgets::TabButton(k_tabs[i].ico, k_tabs[i].label,
                                    ImVec2(k_sidebar_w, k_tab_h), i == tab_current_)) {
            // 动画进行中忽略新点击（例项目行为：滑出完成后才真正切换）
            if (i != tab_current_ && !anim_active_) {
                tab_next_    = i;
                anim_active_ = true;
                anim_slide_  = 0.0f;
            }
        }
    }

    draw_user_block(dl, p, ws);
}

void menu_shell::draw_logo(ImDrawList* dl, const ImVec2& p)
{
    // 例项目：图标字形 + 标题文字（粗体 Segoe UI）做水平灰阶渐变。
    // 本项目未内嵌 Segoe UI，标题用主字体大字号，图标字体绘制左侧字形。
    const float ico_size = 30.0f;
    const float txt_size = 24.0f;
    const ImU32 col_l = IM_COL32(90, 90, 95, 200);     // 渐变左端（暗灰）
    const ImU32 col_r = IM_COL32(175, 175, 182, 255);  // 渐变右端（亮灰）

    if (fonts::icon)
        menu_widgets::GradientText(dl, p + ImVec2(18.0f, 22.0f), fonts::icon, ico_size,
                                   "U", col_l, col_r, k_sidebar_w);
    menu_widgets::GradientText(dl, p + ImVec2(58.0f, 24.0f), fonts::regular, txt_size,
                               "内存修改器", col_l, col_r, 150.0f);

    // Logo 下 1px 下划线（例项目为整条横贯侧边栏的细线）
    dl->AddRectFilled(p + ImVec2(20.0f, k_header_h - 6.0f),
                      p + ImVec2(k_sidebar_w - 20.0f, k_header_h - 5.0f),
                      IM_COL32(25, 25, 25, 180));
}

void menu_shell::draw_user_block(ImDrawList* dl, const ImVec2& p, const ImVec2& ws)
{
    // 例项目侧边栏底部的用户信息块（头像圆 + 名字两行）→
    // 本项目改为"附加进程块"：附加时显示进程头像与 PID/名称，
    // 未附加显示 '?'。点击打开进程选择窗口。
    // 进程名与图标只在 pid 变化时枚举一次（进程全量枚举较重，禁止每帧执行）。
    auto& pm = process_manager::instance();
    if (pm.is_attached()) {
        const uint32_t pid = pm.attached_pid();
        if (user_pid_ != pid) {
            user_pid_   = pid;
            user_name_.clear();
            user_icon_  = 0;
            for (const auto& pi : pm.processes().enumerate()) {
                if (pi.pid == pid) {
                    user_name_ = pi.name;
                    user_icon_ = process_icon_cache::instance().icon_for(pi);
                    break;
                }
            }
        }
    } else {
        user_pid_  = 0;
        user_name_.clear();
        user_icon_ = 0;
    }

    const ImVec2 center = p + ImVec2(40.0f, ws.y - 56.0f);
    const float  r      = 22.0f;

    // 头像：附加进程用真实图标（圆形裁剪），否则 '?' 字形占位；
    // 外圈描边跟随强调色（例项目为固定色圆环）
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
    dl->AddCircleFilled(center, r, IM_COL32(10, 9, 10, 255));
    bool drew_icon = false;
    if (user_icon_) {
        dl->AddImageRounded(user_icon_, center - ImVec2(r, r), center + ImVec2(r, r),
                            ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, r);
        drew_icon = true;
    }
    if (!drew_icon && fonts::icon) {
        const ImVec2 isz = fonts::icon->CalcTextSizeA(24.0f, FLT_MAX, 0.0f, "?");
        dl->AddText(fonts::icon, 24.0f, center - isz * 0.5f,
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), "?");
    }
    dl->AddCircle(center, r + 2.0f, accent, 0, 3.0f);

    // 名字两行：第一行进程名（超宽截断），第二行 PID / 未附加
    char line2[48];
    if (user_pid_ != 0)
        snprintf(line2, sizeof(line2), "PID %u", (unsigned)user_pid_);
    else
        snprintf(line2, sizeof(line2), "未附加");
    std::string name = user_name_;
    if (name.empty())
        name = user_pid_ != 0 ? "未知进程" : "点击选择进程";
    const float max_w = k_sidebar_w - 76.0f;
    while (!name.empty() && ImGui::CalcTextSize(name.c_str()).x > max_w)
        name.pop_back();
    if (name.size() != user_name_.size() && !name.empty())
        name.replace(name.size() - 3, 3, "...");

    dl->AddText(p + ImVec2(72.0f, ws.y - 68.0f), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
    dl->AddText(p + ImVec2(72.0f, ws.y - 50.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled), line2);

    // 整块点击区域
    ImGui::SetCursorPos(ImVec2(8.0f, ImGui::GetWindowSize().y - 80.0f));
    if (ImGui::InvisibleButton("##user_block", ImVec2(k_sidebar_w - 16.0f, 66.0f)))
        state_.show_process_window = true;
}

void menu_shell::render_content()
{
    const ImGuiStyle& st  = ImGui::GetStyle();
    const ImVec2      pad = st.WindowPadding;
    const ImVec2      ws  = ImGui::GetWindowSize();

    // 内容区几何：x 从侧边栏右缘 + 间距起，y 从 Logo 分割线下起；
    // 底部预留状态条高度（与经典布局同款辉光条），仅留 8px 收边让
    // 状态条贴住面板底缘（过大留白会显得没铺满）。
    const float status_h = ImGui::GetTextLineHeight() + st.FramePadding.y * 2.0f + 6.0f;
    const float x  = k_sidebar_w + k_content_gap - pad.x;
    const float y0 = k_header_h + 14.0f - pad.y;
    const float w  = ws.x - x - k_content_gap;
    const float h  = ws.y - y0 - status_h - st.ItemSpacing.y - 8.0f;
    if (w <= 0.0f || h <= 0.0f)
        return;

    // 内容随动画整体垂直滑动（例项目 SetCursorPos(203, 88 - size_child)）
    ImGui::SetCursorPos(ImVec2(x, y0 - anim_slide_));
    ImGui::BeginChild("##evicted_content", ImVec2(w, h), ImGuiChildFlags_Borders);
    switch (tab_current_) {
    case 0: render_tab_scan();     break;
    case 1: render_tab_address();  break;
    case 2: render_tab_tools();    break;
    case 3: render_tab_settings(); break;
    case 4: render_tab_about();    break;
    }
    ImGui::EndChild();

    // 状态条：常驻内容区下方（附加状态 + 扫描进度）
    ImGui::SetCursorPos(ImVec2(x, y0 + h + st.ItemSpacing.y));
    render_scan_status_bar();
}

void menu_shell::render_tab_scan()
{
    // 双列网格（例项目内容区布局）：左列扫描、右列结果
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float avail_h = ImGui::GetContentRegionAvail().y;
    const float left_w  = avail_w * 0.42f;

    ImGui::BeginChild("##scan_col", ImVec2(left_w, avail_h), ImGuiChildFlags_Borders);
    scan_panel_.render();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##result_col", ImVec2(0, avail_h), ImGuiChildFlags_Borders);
    result_panel_.render();
    ImGui::EndChild();
}

void menu_shell::render_tab_address()
{
    ctx_.address_list.render();
}

void menu_shell::render_tab_tools()
{
    ImGui::TextDisabled("Cheat Table");
    if (ImGui::Button("加载 CT", ImVec2(120, 0)))
        top_menu_.open_ct_dialog(1);
    ImGui::SameLine();
    if (ImGui::Button("保存 CT", ImVec2(120, 0)))
        top_menu_.open_ct_dialog(2);
    if (!state_.current_ct_path.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", state_.current_ct_path.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("窗口");

    if (ImGui::Button("打开进程", ImVec2(120, 0)))
        state_.show_process_window = true;
    ImGui::SameLine();
    if (ImGui::Button("进程详情", ImVec2(120, 0)))
        state_.show_process_detail = true;
    if (ImGui::Button("内存查看器", ImVec2(120, 0)))
        state_.show_memory_window = true;
    ImGui::SameLine();
    if (ImGui::Button("调试面板", ImVec2(120, 0)))
        state_.show_debug_window = true;
}

void menu_shell::render_tab_settings()
{
    // 布局切换（经典 / 菜单风）
    ImGui::Text("布局");
    static const char* k_layouts[] = { "经典", "菜单风格" };
    int lm = state_.layout_mode;
    if (lm < 0 || lm > 1) lm = 0;
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("##layout", k_layouts[lm])) {
        for (int i = 0; i < 2; ++i) {
            const bool sel = (i == lm);
            if (ImGui::Selectable(k_layouts[i], sel))
                state_.layout_mode = i;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // 主题下拉（与设置窗口同款：遍历 theme 注册表）
    ImGui::Text("主题");
    int cur_idx = -1;
    for (int i = 0; i < theme::count; ++i)
        if ((int)theme::registry[i].id == state_.theme) { cur_idx = i; break; }
    const char* preview = (cur_idx >= 0) ? theme::registry[cur_idx].name : "选择主题...";
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("##shell_theme", preview)) {
        for (int i = 0; i < theme::count; ++i) {
            const bool sel = (i == cur_idx);
            if (ImGui::Selectable(theme::registry[i].name, sel)) {
                state_.theme = (int)theme::registry[i].id;
                theme::apply(theme::registry[i].id);
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();
    if (ImGui::Button("打开设置窗口"))
        state_.show_settings_window = true;
    ImGui::TextDisabled("（缓存目录等其余设置在设置窗口中）");
}

void menu_shell::render_tab_about()
{
    ImGui::Text("作者:Jin");
    ImGui::Text("QQ:3264688446");
    ImGui::Spacing();
    if (ImGui::Button("打开调试面板"))
        state_.show_debug_window = true;

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("图标字形总览"))
        menu_widgets::IconFontOverview();
}

// ── 常驻状态进度条 ──────────────────────────────────────────
// 左侧：附加状态（"未附加" / "PID xxxx · 进程名"）；右侧：扫描状态（"未扫描" / 百分比）。
// 视觉：直角条 + 多层描边辉光；扫描中辉光随时间脉动。
// 颜色：取当前主题的强调色（ImGuiCol_CheckMark，每套主题都定义为各自的主色调），
//       再按背景明暗归一化饱和度/亮度 —— 色相跟随主题，保证 Dark / Light / Cyan /
//       Midnight / Light Blue 下既醒目又不与整体风格割裂。
// （经典布局与菜单风外壳共用；原位于 main.cpp，随布局双模式提取至此）
void render_scan_status_bar()
{
    auto& svc = scan_service::instance();
    auto& pm  = process_manager::instance();

    const bool  scanning = svc.is_scanning();
    const bool  attached = pm.is_attached();
    const float progress = scanning ? svc.progress() : 0.0f;

    // 附加进程名缓存：仅在 pid 变化时枚举一次系统进程（避免每帧快照开销）
    static uint32_t    s_pid = 0;
    static std::string s_name;
    if (attached) {
        if (s_pid != pm.attached_pid()) {
            s_pid = pm.attached_pid();
            s_name.clear();
            for (const auto& p : pm.processes().enumerate())
                if (p.pid == s_pid) { s_name = p.name; break; }
        }
    } else {
        s_pid  = 0;
        s_name.clear();
    }

    const ImGuiStyle& st = ImGui::GetStyle();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float bar_h = ImGui::GetTextLineHeight() + st.FramePadding.y * 2.0f + 6.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float  bar_w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p1 = ImVec2(p0.x + bar_w, p0.y + bar_h);
    ImGui::Dummy(ImVec2(bar_w, bar_h));   // 常驻占位：无扫描时也保持布局稳定

    const ImVec4 wbg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const float  lum = wbg.x * 0.299f + wbg.y * 0.587f + wbg.z * 0.114f;
    const bool   is_light = lum > 0.5f;

    // 强调色 = 主题强调色的色相 + 按明暗归一化的饱和度/亮度
    ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    float ah, as, av;
    ImGui::ColorConvertRGBtoHSV(accent.x, accent.y, accent.z, ah, as, av);
    if (as < 0.05f) { as = 0.65f; ah = 0.55f; }   // 主题强调色接近灰色时兜底为蓝色系
    if (is_light) {
        as = as < 0.65f ? 0.65f : as;             // 浅色背景：压暗到可读范围
        av = 0.55f;
    } else {
        as = as < 0.85f ? 0.85f : as;             // 深色背景：提亮到高饱和鲜亮
        av = av < 0.95f ? 0.95f : av;
    }
    ImGui::ColorConvertHSVtoRGB(ah, as, av, accent.x, accent.y, accent.z);
    accent.w = 1.0f;

    const float rounding = 0.0f;

    // 辉光：由外向内叠画多层圆角矩形，透明度递增；扫描中随时间脉动
    const float t = (float)ImGui::GetTime();
    const float pulse = scanning ? 0.70f + 0.30f * sinf(t * 5.0f) : 0.45f;
    for (int i = 4; i >= 1; --i) {
        const float expand = (float)i * 2.5f;
        const float alpha  = pulse * (is_light ? 0.16f : 0.30f) * (1.0f - (float)(i - 1) / 4.0f);
        dl->AddRectFilled(ImVec2(p0.x - expand, p0.y - expand),
                          ImVec2(p1.x + expand, p1.y + expand),
                          ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, alpha)),
                          rounding);
    }

    // 条底与描边（描边用强调色低透明度，替代主题 Border，保证形态可辨）
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
    dl->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, 0.55f)), rounding);

    // 进度填充 + 顶部高光
    float frac = progress;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const float fill_w = bar_w * frac;
    if (fill_w >= 2.0f) {
        const ImVec2 fp1(p0.x + fill_w, p1.y);
        dl->AddRectFilled(p0, fp1, ImGui::GetColorU32(accent), rounding);
        const ImVec4 hi(accent.x + (1.0f - accent.x) * 0.30f,
                        accent.y + (1.0f - accent.y) * 0.30f,
                        accent.z + (1.0f - accent.z) * 0.30f, 0.45f);
        dl->AddRectFilled(ImVec2(p0.x + 1.0f, p0.y + 1.0f),
                          ImVec2(fp1.x - 1.0f, p0.y + (bar_h - 2.0f) * 0.45f),
                          ImGui::GetColorU32(hi), rounding * 0.8f,
                          ImDrawFlags_RoundCornersTop);
    }

    // 左侧附加状态 / 右侧扫描状态（分居两端，避免拥挤）
    char left_buf[300];
    if (attached) {
        if (!s_name.empty())
            snprintf(left_buf, sizeof(left_buf), "PID %u   ·   %s", (unsigned)s_pid, s_name.c_str());
        else
            snprintf(left_buf, sizeof(left_buf), "PID %u", (unsigned)s_pid);
    } else {
        snprintf(left_buf, sizeof(left_buf), "未附加");
    }

    char right_buf[32];
    const char* right_text;
    if (scanning) {
        snprintf(right_buf, sizeof(right_buf), "%.1f%%", frac * 100.0f);
        right_text = right_buf;
    } else {
        right_text = "未扫描";
    }

    const ImU32 text_col = ImGui::GetColorU32(accent);
    const ImVec2 lsz = ImGui::CalcTextSize(left_buf);
    dl->AddText(ImVec2(p0.x + st.FramePadding.x + 3.0f, p0.y + (bar_h - lsz.y) * 0.5f), text_col, left_buf);
    const ImVec2 rsz = ImGui::CalcTextSize(right_text);
    dl->AddText(ImVec2(p1.x - rsz.x - st.FramePadding.x - 3.0f, p0.y + (bar_h - rsz.y) * 0.5f), text_col, right_text);
}

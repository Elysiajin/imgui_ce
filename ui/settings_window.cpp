#include "settings_window.h"
#include "imgui.h"
#include "theme.h"
#include "scan/temp_path_manager.h"

#include <cstring>
#include <string>

void settings_window::render() {
    if (!state_.show_settings_window)
        return;

    if (ImGui::Begin("设置", &state_.show_settings_window, ImGuiWindowFlags_NoCollapse)) {

        ImGui::Text("缓存目录 (留空 = 默认 %%TEMP%%/MyScanApp_Data/<pid>)");
        static char dir_buf[0x400] = "";
        // 首次进入时用当前缓存根目录填充输入框
        static bool inited = false;
        if (!inited) {
            std::string cur = temp_path_manager::get_base_dir();
            if (cur.empty()) cur = (std::filesystem::temp_directory_path() / "MyScanApp_Data").string();
            std::strncpy(dir_buf, cur.c_str(), sizeof(dir_buf) - 1);
            inited = true;
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##cache_dir", dir_buf, IM_ARRAYSIZE(dir_buf))) {
            state_.cache_dir = dir_buf;
        }

        if (ImGui::Button("应用缓存路径")) {
            temp_path_manager::set_base_dir(state_.cache_dir);
        }
        ImGui::SameLine();
        if (ImGui::Button("清理缓存")) {
            temp_path_manager::cleanup();
        }
        ImGui::SameLine();

        ImGui::Separator();

        // ---- 主界面布局：经典（菜单栏 + 上下分栏）/ 菜单风（侧边栏外壳）----
        ImGui::Text("布局");
        static const char* k_layouts[] = { "经典", "菜单风格" };
        int lm = state_.layout_mode;
        if (lm < 0 || lm > 1) lm = 0;
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo("##layout", k_layouts[lm])) {
            for (int i = 0; i < 2; ++i) {
                const bool selected = (i == lm);
                if (ImGui::Selectable(k_layouts[i], selected))
                    state_.layout_mode = i;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // ---- 界面风格：下拉遍历主题注册表，便于后续扩展多种样式 ----
        ImGui::Text("主题");
        int cur_idx = -1;
        for (int i = 0; i < theme::count; ++i)
            if ((int)theme::registry[i].id == state_.theme) { cur_idx = i; break; }
        const char* preview = (cur_idx >= 0) ? theme::registry[cur_idx].name : "选择主题...";
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo("##theme", preview)) {
            for (int i = 0; i < theme::count; ++i) {
                bool selected = (i == cur_idx);
                if (ImGui::Selectable(theme::registry[i].name, selected)) {
                    state_.theme = (int)theme::registry[i].id;
                    theme::apply(theme::registry[i].id);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::End();
}

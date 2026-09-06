#include "settings_window.h"
#include "imgui.h"
#include "scan/temp_path_manager.h"

#include <cstring>
#include <string>

void settings_window::render() {
    if (!state_.show_settings_window)
        return;

    if (ImGui::Begin("Settings", &state_.show_settings_window, ImGuiWindowFlags_NoCollapse)) {

        ImGui::Text("Cache directory (empty = default %%TEMP%%/MyScanApp_Data/<pid>)");
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

        if (ImGui::Button("Apply cache path")) {
            temp_path_manager::set_base_dir(state_.cache_dir);
        }
        ImGui::SameLine();
        if (ImGui::Button("Clean cache")) {
            temp_path_manager::cleanup();
        }
        ImGui::SameLine();

        ImGui::Separator();

        // ---- 界面风格 ----
        ImGui::Text("Theme");
        if (ImGui::RadioButton("Dark", state_.theme == 0)) {
            state_.theme = 0;
            ImGui::StyleColorsDark();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Light", state_.theme == 1)) {
            state_.theme = 1;
            ImGui::StyleColorsLight();
        }
    }
    ImGui::End();
}

#include "process_list_window.h"
#include "imgui.h"
#include "ui/process_icon_cache.h"
#include "core/process_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

void process_list_window::render() {
    auto& pm = process_manager::instance();

    // 节流刷新：窗口显示时枚举一次，之后距上次超过 ~1000ms 自动重枚举。
    const auto now = std::chrono::steady_clock::now();
    const auto refresh_threshold = std::chrono::milliseconds(1000);
    if (now - last_refresh_ >= refresh_threshold) {
        process_list_ = pm.processes().enumerate();
        std::sort(process_list_.begin(), process_list_.end(),
                  [](const process_info& a, const process_info& b) {
                      return a.pid < b.pid;
                  });
        last_refresh_ = now;
    }

    if (ImGui::Begin("Process Window", &state_.show_process_window, ImGuiWindowFlags_NoCollapse)) {
        ImGui::InputText("Filter", filter_buf_, IM_ARRAYSIZE(filter_buf_));

        ImGui::Separator();

        if (ImGui::BeginTable("Process Tab", 4,
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {

            ImGui::TableSetupColumn("Pid");
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Ppid", ImGuiTableColumnFlags_WidthFixed, 80.f);
            ImGui::TableSetupColumn("Threads", ImGuiTableColumnFlags_WidthFixed, 80.f);

            ImGui::TableHeadersRow();

            auto& icon_cache = process_icon_cache::instance();

            std::string filter_str = filter_buf_;
            std::transform(filter_str.begin(), filter_str.end(), filter_str.begin(), ::tolower);
            bool has_filter = !filter_str.empty();

            for (const auto& p : process_list_) {
                if(has_filter){
                    // 过滤
                    std::string name_lower = p.name;
                    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
                    if(name_lower.find(filter_str) == std::string::npos){
                        continue;
                    }
                }

                ImGui::PushID(static_cast<int>(p.pid));

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                char pid_buf[32];
                snprintf(pid_buf, IM_ARRAYSIZE(pid_buf), "%u [%X]", p.pid, p.pid);

                bool clicked = ImGui::Selectable(pid_buf, selected_pid_ == static_cast<int>(p.pid),
                                                 ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
                if (clicked) {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        pm.attach(p.pid);
                        state_.modules_loaded = false;
                        state_.show_process_window = false;
                    } else {
                        selected_pid_ = static_cast<int>(p.pid);
                    }
                }

                if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && ImGui::IsItemHovered())
                    selected_pid_ = static_cast<int>(p.pid);

                if (ImGui::BeginPopupContextItem("ProcessContextMenu")) {
                    if (ImGui::MenuItem("Attach")) {
                        pm.attach(p.pid);
                        state_.modules_loaded = false;
                        state_.show_process_window = false;
                    }
                    if (ImGui::MenuItem("Look for detail")) {
                        pm.attach(p.pid);
                        state_.modules_loaded = false;
                        state_.show_process_detail = true;
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Terminate Process")) {
                        if(!process_manager::terminate_process(p.pid)){
                            // ImGui::OpenPopup("Terminater Error");
                            error_window_ = true;
                        }
                    }

                    ImGui::EndPopup();
                }

                ImGui::TableSetColumnIndex(1);
                ImTextureID icon = icon_cache.icon_for(p);
                if (icon) {
                    ImGui::Image((ImTextureRef)icon, ImVec2(16, 16));
                    ImGui::SameLine();
                }
                ImGui::TextUnformatted(p.name.c_str());

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u", p.ppid);

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%u", p.thread_count);

                ImGui::PopID();
            }

            ImGui::EndTable();
        }

        if(error_window_){
            ImGui::OpenPopup("Terminater Error");
        }

        if(ImGui::BeginPopupModal("Terminater Error", &error_window_, ImGuiWindowFlags_AlwaysAutoResize)){
            ImGui::TextUnformatted("Terminate Process Failed!");
            if(ImGui::Button("Close", ImVec2(120, 0))){
                ImGui::CloseCurrentPopup();
                error_window_ = false;
            }

            ImGui::EndPopup();
        }
        // if(error_window_){

        // }
    }
    ImGui::End();
}

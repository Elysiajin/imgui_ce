#include "top_menu.h"
#include "imgui.h"

#include "app_context.h"
#include "core/process_manager.h"
#include "ct/cheat_table.h"
#include "ct/ct_bridge.h"

#include <processthreadsapi.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

// CT 文件 → 地址列表（树状填充）
bool load_ct_into_list(const std::string& path, std::string& error)
{
    const ct_load_result res = load_ct_file(path);
    if (!res.ok) {
        error = res.error;
        return false;
    }

    auto& app = application_context::instance();
    auto& list = app.address_list;

    // 附加进程时用真实模块表解析 "模块+偏移"；未附加则保留原文，
    // 附加后由 update_values() 重试解析
    const std::vector<module_info> modules =
        process_manager::instance().module_snapshot();
    ct_bridge::apply_ct_table(res.table, list, modules);

    // 附加进程时给条目填初始值（CE 行为：加载即读当前值）
    list.update_values();
    return true;
}

} // namespace

void top_menu::render() {
    render_menu_bar();
    render_ct_dialogs();
}

void top_menu::open_ct_dialog(int mode) {
    ct_dialog_ = mode;
    browser_init_ = false;
    if (mode == 2) {
        // 保存模式：预填上次文件名
        std::snprintf(ct_save_name_, sizeof(ct_save_name_), "%s",
                      state_.current_ct_path.empty()
                          ? "table.CT"
                          : state_.current_ct_path.c_str());
    }
}

void top_menu::render_menu_bar() {
    if(ImGui::BeginMenuBar()){
        if(ImGui::BeginMenu("文件")){
            if(ImGui::MenuItem("保存")){
                open_ct_dialog(2);
            }

            if(ImGui::MenuItem("加载")){
                open_ct_dialog(1);
            }

            if(ImGui::MenuItem("打开进程", "O")){
                state_.show_process_window = true;
            }

            if(ImGui::MenuItem("退出", "Alt+F4")) ExitProcess(0);

            ImGui::EndMenu();
        }

        if(ImGui::BeginMenu("编辑")){
            if(ImGui::MenuItem("设置")){
                state_.show_settings_window = true;
            }
            ImGui::EndMenu();
        }

        if(ImGui::BeginMenu("关于")){
            if(ImGui::MenuItem("关于本程序")){
                state_.show_about_window = true;
            }
            if(ImGui::MenuItem("调试面板")){
                state_.show_about_window = false;
                state_.show_debug_window = true;
            }
            ImGui::EndMenu();
        }


        ImGui::EndMenuBar();
    }
}

void top_menu::render_ct_dialogs() {
    // ---- CT 打开/保存弹窗（项目自绘 file_browser，即时渲染非阻塞）----
    if (ct_dialog_ != 0) {
        ImGui::SetNextWindowSize(ImVec2(720, 460), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal(ct_dialog_ == 1 ? "加载 Cheat Table" : "保存 Cheat Table",
                                   nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            if (!browser_init_) {
                browser_.set_filter("ct");
                if (!state_.current_ct_path.empty())
                    browser_.set_initial_path(state_.current_ct_path);
                browser_init_ = true;
            }
            browser_.render();

            if (ct_dialog_ == 2) {
                // 保存模式：输入目标文件名
                ImGui::SetNextItemWidth(300);
                ImGui::InputTextWithHint("##ct_save_name", "文件名（如 table.CT）",
                                         ct_save_name_, sizeof(ct_save_name_));
            }

            const bool have = browser_.has_selection();
            ImGui::BeginDisabled(ct_dialog_ == 1 && !have);
            if (ImGui::Button(ct_dialog_ == 1 ? "加载" : "保存", ImVec2(90, 0))) {
                std::string target;
                if (ct_dialog_ == 1) {
                    target = browser_.selected_path().string();
                } else {
                    // 保存：目录用浏览器当前目录 + 输入的文件名
                    std::filesystem::path p = browser_.current_path();
                    std::string name = ct_save_name_;
                    if (!name.empty()) {
                        // 自动补 .CT 扩展名
                        if (name.size() < 3 || _stricmp(name.c_str() + name.size() - 3, ".CT") != 0)
                            name += ".CT";
                        p /= name;
                    }
                    target = p.string();
                }
                if (!target.empty()) {
                    if (ct_dialog_ == 1) {
                        std::string err;
                        if (load_ct_into_list(target, err)) {
                            state_.current_ct_path = target;
                            state_.ct_error.clear();
                            ct_dialog_ = 0;
                            ImGui::CloseCurrentPopup();
                        } else {
                            state_.ct_error = err;
                        }
                    } else {
                        cheat_table ct = ct_bridge::build_ct_table(
                            application_context::instance().address_list);
                        std::string err;
                        if (save_ct_file(ct, target, err)) {
                            state_.current_ct_path = target;
                            state_.ct_error.clear();
                            ct_dialog_ = 0;
                            ImGui::CloseCurrentPopup();
                        } else {
                            state_.ct_error = err;
                        }
                    }
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("取消", ImVec2(90, 0))) {
                ct_dialog_ = 0;
                ImGui::CloseCurrentPopup();
            }
            if (!state_.ct_error.empty())
                ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s",
                                   state_.ct_error.c_str());
            ImGui::EndPopup();
        } else if (ct_dialog_ != 0) {
            // 首帧 OpenPopup
            ImGui::OpenPopup(ct_dialog_ == 1 ? "加载 Cheat Table" : "保存 Cheat Table");
        }
    }
}

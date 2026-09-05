#include "process_detail_window.h"
#include "imgui.h"
#include "core/process_manager.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>

static const char* region_state_name(uint32_t state) {
    if (state & MEM_COMMIT)  return "Commit";
    if (state & MEM_RESERVE) return "Reserve";
    if (state & MEM_FREE)    return "Free";
    return "?";
}

static const char* region_type_name(uint32_t type) {
    if (type & MEM_PRIVATE) return "Private";
    if (type & MEM_IMAGE)   return "Image";
    if (type & MEM_MAPPED)  return "Mapped";
    return "?";
}

void process_detail_window::render_modules_tab() {
    auto& pm = process_manager::instance();

    // 懒加载模块（pid 变化时重载）
    if (modules_.empty() || state_.attached_pid != pm.attached_pid()) {
        modules_ = pm.modules().enumerate(pm.attached_pid());
        state_.attached_pid = pm.attached_pid();

        state_.module_names.clear();
        state_.module_names.push_back("<All Mods>");
        for (const auto& m : modules_)
            state_.module_names.push_back(m.name);
        if (state_.module_selected < 0 || state_.module_selected >= (int)state_.module_names.size())
            state_.module_selected = 0;
    }

    if (ImGui::BeginTable("##module_table", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Base", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size");
        ImGui::TableHeadersRow();

        for (const auto& m : modules_) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(m.name.c_str());

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%016llX", (unsigned long long)m.base);

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("0x%llX", (unsigned long long)m.size);
        }
        ImGui::EndTable();
    }
}

void process_detail_window::render_regions_tab() {
    auto& pm = process_manager::instance();

    if (regions_.empty() || state_.attached_pid != pm.attached_pid())
        regions_ = pm.regions().enumerate();

    if (ImGui::BeginTable("##region_table", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Base", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("Type");
        ImGui::TableHeadersRow();

        for (const auto& r : regions_) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("0x%llX", (unsigned long long)r.base);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("0x%llX", (unsigned long long)r.size);

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(region_state_name(r.state));

            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(region_type_name(r.type));
        }
        ImGui::EndTable();
    }
}

void process_detail_window::render() {
    if (!state_.show_process_detail)
        return;

    auto& pm = process_manager::instance();

    if (!pm.is_attached()) {
        state_.show_process_detail = false;
        return;
    }

    char title[64];
    snprintf(title, sizeof(title), "Process Detail - PID %u###process_detail", pm.attached_pid());

    if (ImGui::Begin(title, &state_.show_process_detail)) {
        if (ImGui::BeginTabBar("##detail_tabs")) {
            if (ImGui::BeginTabItem("Modules")) {
                render_modules_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Memory regions")) {
                render_regions_tab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

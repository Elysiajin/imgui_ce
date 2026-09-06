#include "result_panel.h"
#include "app_context.h"
#include "ui/address_list_panel.h"
#include "imgui.h"
#include "scan/scan_service.h"
#include "scan/scan_data_provider.h"
#include "core/process_manager.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
// Smallest matching type for an All-mode result token (matches CE behaviour)
scan_data_type primary_type_for_mask(uint16_t type_mask) {
    static constexpr int k_small_first[] = { 0, 1, 2, 4, 3, 5 };
    static constexpr scan_data_type k_type_map[] = {
        scan_data_type::int8, scan_data_type::int16, scan_data_type::int32,
        scan_data_type::int64, scan_data_type::float32, scan_data_type::float64
    };
    if (type_mask == 0) return scan_data_type::int32;
    for (int ti : k_small_first)
        if (type_mask & (1 << ti)) return k_type_map[ti];
    return scan_data_type::int32;
}
} // namespace

void result_panel::render() {
    auto& svc = scan_service::instance();
    scan_result_repository* repo = svc.get_repository();
    scan_data_provider* provider = svc.get_data_provider();
    if (!repo || !provider) return;

    const size_t total = repo->get_result_count();

    // ---- Render-limit dropdown: cap how many rows we draw (0 = all) ----
    static int limit = 0;
    constexpr const char* k_limit_items[] = { "All", "100", "1,000", "5,000", "10,000", "50,000" };
    constexpr int         k_limit_vals[]  = { 0,     100,   1000,    5000,    10000,    50000 };
    int limit_idx = 0;
    for (int i = 0; i < IM_ARRAYSIZE(k_limit_vals); ++i)
        if (k_limit_vals[i] == limit) { limit_idx = i; break; }
    ImGui::SetNextItemWidth(110);
    if (ImGui::Combo("Render", &limit_idx, k_limit_items, IM_ARRAYSIZE(k_limit_items)))
        limit = k_limit_vals[limit_idx];

    ImGui::SameLine();
    const size_t shown = (limit > 0 && (size_t)limit < total) ? (size_t)limit : total;
    if (svc.is_scanning())
        ImGui::Text("Scanning...");
    ImGui::Text("Found: %llu     Displaying: %llu", (unsigned long long)total, (unsigned long long)shown);

    const scan_data_type dtype = provider->get_display_type();

    // result_panel 是跨帧持久对象（由 application_context 持有），
    // 选中行存于 ctx_.selected_result_index，避免文件级 static。
    int& selected_row = ctx_.selected_result_index;
    const int row_total = (int)shown;
    if (selected_row >= row_total) selected_row = row_total - 1;

    // 双击结果行 / 右键菜单 共用的"加入地址列表"动作
    auto add_address_to_list = [&](uint64_t addr) {
        address_record rec;
        rec.real_address = addr;
        char addr_buf[32];
        snprintf(addr_buf, sizeof(addr_buf), "%016llX", (unsigned long long)addr);
        rec.address = addr_buf;
        rec.description = "result";
        rec.valid = true;
        ctx_.address_list.add_record(rec);
    };

    float avail = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginChild("result_table", ImVec2(0, avail - 40), ImGuiChildFlags_Borders)) {
        if (ImGui::BeginTable("##result", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Address");
            ImGui::TableSetupColumn("Current");
            ImGui::TableSetupColumn("Previous");
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(row_total);
            while (clipper.Step()) {
                // Read the whole visible batch once (fewer allocations than per-row).
                std::vector<scan_result> batch = repo->read_pool_chunk(
                    (size_t)clipper.DisplayStart,
                    (size_t)(clipper.DisplayEnd - clipper.DisplayStart));
                if (batch.empty()) continue;

                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const scan_result& r = batch[(size_t)(row - clipper.DisplayStart)];

                    ImGui::PushID(row);
                    ImGui::TableNextRow();

                    // Address
                    ImGui::TableSetColumnIndex(0);
                    char buf[32];
                    snprintf(buf, sizeof(buf), "0x%08llX", (unsigned long long)r.address);
                    if (ImGui::Selectable(buf, selected_row == row, ImGuiSelectableFlags_SpanAllColumns)) {
                        selected_row = row;
                    }
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        // 双击：把该行地址加入下方地址列表
                        selected_row = row;
                        add_address_to_list(r.address);
                    }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        selected_row = row;
                        ImGui::OpenPopup("##result_row_menu");
                    }

                    if(ImGui::BeginPopupContextItem("Result Menu")){
                        if(ImGui::BeginMenu("Debugger")){
                            if(ImGui::MenuItem("Find Access")){

                            }

                            if(ImGui::MenuItem("Find Write")){

                            }

                            ImGui::EndMenu();
                        }

                        ImGui::EndPopup();
                    }

                    // Current / Previous values
                    scan_data_type dt = (dtype == scan_data_type::all) ? primary_type_for_mask(r.type_mask) : dtype;
                    std::string cur  = provider->get_current_value(r.address, dt);
                    std::string prev = provider->get_previous_value(r.address, dt);

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(cur.c_str());

                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(prev.c_str());

                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    if (ImGui::Button("View Memory")) {}
    ImGui::SameLine();
    if (ImGui::Button("Manual Add Address")) {}

    if (ImGui::BeginPopup("##result_row_menu")) {
        if (selected_row >= 0 && selected_row < row_total) {
            if (ImGui::MenuItem("Add to address list")) {
                add_address_to_list(repo->get_address_at_index((size_t)selected_row));
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Copy address")) {
                char addr[32];
                snprintf(addr, sizeof(addr), "%016llX",
                         (unsigned long long)repo->get_address_at_index(selected_row));
                ImGui::SetClipboardText(addr);
            }
        }
        ImGui::EndPopup();
    }
}

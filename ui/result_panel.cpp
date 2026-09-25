#include "result_panel.h"
#include "app_context.h"
#include "ui/address_list_panel.h"
#include "ui/address_value.h"
#include "imgui.h"
#include "scan/scan_service.h"
#include "scan/scan_data_provider.h"
#include "core/process_manager.h"
#include "ct/address_parser.h"

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
    constexpr const char* k_limit_items[] = { "全部", "100", "1,000", "5,000", "10,000", "50,000" };
    constexpr int         k_limit_vals[]  = { 0,     100,   1000,    5000,    10000,    50000 };
    int limit_idx = 0;
    for (int i = 0; i < IM_ARRAYSIZE(k_limit_vals); ++i)
        if (k_limit_vals[i] == limit) { limit_idx = i; break; }
    ImGui::SetNextItemWidth(110);
    if (ImGui::Combo("渲染", &limit_idx, k_limit_items, IM_ARRAYSIZE(k_limit_items)))
        limit = k_limit_vals[limit_idx];

    ImGui::SameLine();
    const size_t shown = (limit > 0 && (size_t)limit < total) ? (size_t)limit : total;
    ImGui::Text("找到: %llu     显示: %llu", (unsigned long long)total, (unsigned long long)shown);

    const scan_data_type dtype = provider->get_display_type();

    // result_panel 是跨帧持久对象（由 application_context 持有），
    // 选中行存于 ctx_.selected_result_index，避免文件级 static。
    int& selected_row = ctx_.selected_result_index;
    const int row_total = (int)shown;
    if (selected_row >= row_total) selected_row = row_total - 1;

    // 双击结果行 / 右键菜单 共用的"加入地址列表"动作
    // 携带当前扫描类型（float/double → 8 字节等），否则地址栏类型固定为 4 字节。
    const scan_data_type scan_dt = dtype;
    auto add_address_to_list = [&](uint64_t addr) {
        address_record rec;
        rec.real_address = addr;
        char addr_buf[32];
        snprintf(addr_buf, sizeof(addr_buf), "%016llX", (unsigned long long)addr);
        rec.address = addr_buf;
        rec.description = "扫描结果";
        rec.valid = true;
        rec.type = scan_data_type_to_value_type(scan_dt);   // ← 关键：用扫描类型
        // 基准值 = 扫描类型对应的当前值（previous_value 作为改动基准）
        rec.previous_value = read_address_value(addr, rec.type);
        rec.value = rec.previous_value;
        ctx_.address_list.add_record(rec);
    };

    float avail = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginChild("result_table", ImVec2(0, avail - 40), ImGuiChildFlags_Borders)) {
        if (ImGui::BeginTable("##result", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("地址");
            ImGui::TableSetupColumn("当前值");
            ImGui::TableSetupColumn("前值");
            ImGui::TableHeadersRow();

            // 行高显式告知 clipper（行都是单行 Selectable）＝Selectable 高
            // （FontSize + 2*FramePadding.y）+ 单元格上下 CellPadding。
            // 后台扫描线程会在 UI 渲染中途换掉整个结果池（scan_service 工作线程
            // 执行 replace_all_results_from_pool），read_pool_chunk 按新池 clamp
            // 后返回的批次可能比请求区间短甚至为空：
            //   - 按请求区间索引 batch 会越界（崩溃）；
            //   - 空批次使 clipper 行高推断失败（触发 imgui.cpp 断言）。
            // 显式行高跳过推断，循环按 batch 实际大小绘制，两者都杜绝。
            const ImGuiStyle& rst = ImGui::GetStyle();
            const float row_h = ImGui::GetTextLineHeight() + rst.FramePadding.y * 2.0f
                              + rst.CellPadding.y * 2.0f;
            ImGuiListClipper clipper;
            clipper.Begin(row_total, row_h);
            while (clipper.Step()) {
                const int first = clipper.DisplayStart;
                std::vector<scan_result> batch = repo->read_pool_chunk(
                    (size_t)first,
                    (size_t)(clipper.DisplayEnd - clipper.DisplayStart));
                const int batch_n = (int)batch.size();

                for (int bi = 0; bi < batch_n; ++bi) {
                    const int row = first + bi;
                    const scan_result& r = batch[(size_t)bi];

                    ImGui::PushID(row);
                    ImGui::TableNextRow();

                    // Address（仿 CE：命中模块则显示"模块+偏移"并绿色着色；未命中模块的原始 hex 用默认色）
                    ImGui::TableSetColumnIndex(0);
                    auto& pm = process_manager::instance();
                    std::string disp;
                    bool is_base = false;
                    bool in_module = pm.is_attached() && pm.resolve_address(r.address, disp, is_base);
                    if (!in_module) {
                        char raw[32];
                        snprintf(raw, sizeof(raw), "0x%08llX", (unsigned long long)r.address);
                        disp = raw;
                    }
                    if (in_module) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.24f, 0.86f, 0.35f, 1.f)); // green
                    if (ImGui::Selectable(disp.c_str(), selected_row == row, ImGuiSelectableFlags_SpanAllColumns)) {
                        selected_row = row;
                    }
                    if (in_module) ImGui::PopStyleColor();
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
                        if(ImGui::BeginMenu("调试器")){
                            if(ImGui::MenuItem("查找访问")){

                            }

                            if(ImGui::MenuItem("查找写入")){

                            }

                            ImGui::EndMenu();
                        }

                        ImGui::EndPopup();
                    }

                    // Current / Previous values
                    scan_data_type dt = (dtype == scan_data_type::all) ? primary_type_for_mask(r.type_mask) : dtype;
                    std::string cur  = provider->get_current_value(r.address, dt);
                    std::string prev = provider->get_previous_value(r.address, dt);

                    // 值变动红色高亮（仿 CE found list 的 ChangedValueColor = clRed）：
                    // 当前值与上一快照值不同，且两者均可读（非 "---"）时标红。
                    const bool changed = !cur.empty() && !prev.empty() &&
                                         cur != "---" && prev != "---" &&
                                         cur != prev;

                    ImGui::TableSetColumnIndex(1);
                    if (changed)
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.35f, 0.35f, 1.f));
                    ImGui::TextUnformatted(cur.c_str());
                    if (changed)
                        ImGui::PopStyleColor();

                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(prev.c_str());

                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    if (ImGui::Button("查看内存")) {
        // 扫描结果本质是数据地址：跳到内存浏览器的十六进制 dump 视图。
        uint64_t addr = 0;
        if (selected_row >= 0 && selected_row < row_total)
            addr = repo->get_address_at_index((size_t)selected_row);
        ctx_.open_memory_viewer.emit(memory_viewer_mode::hexdump, addr);
    }
    ImGui::SameLine();
    if (ImGui::Button("手动添加地址"))
        show_add_dialog_ = true;

    // ---- 手动添加地址对话框（CE 行为：描述/地址/类型，支持模块+偏移）----
    if (show_add_dialog_)
        ImGui::OpenPopup("##add_address");
    if (ImGui::BeginPopupModal("##add_address", &show_add_dialog_,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        static const char* k_types[] = {
            "字节", "2 字节", "4 字节", "8 字节", "单精度浮点数",
            "双精度浮点数", "文本", "字节数组", "位域(二进制)"
        };
        ImGui::SetNextItemWidth(260);
        ImGui::InputTextWithHint("描述", "如：血量", add_desc_buf_, sizeof(add_desc_buf_));
        ImGui::SetNextItemWidth(260);
        ImGui::InputTextWithHint("地址", "hex 或 模块+偏移（如 game.exe+2A3B）",
                                 add_addr_buf_, sizeof(add_addr_buf_));
        ImGui::SetNextItemWidth(260);
        ImGui::Combo("类型", &add_type_index_, k_types, IM_ARRAYSIZE(k_types));
        if (!add_error_.empty())
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", add_error_.c_str());
        if (ImGui::Button("确定", ImVec2(80, 0))) {
            const parsed_address pa = parse_interpretable_address(
                add_addr_buf_,
                process_manager::instance().module_snapshot());
            if (!pa.ok) {
                add_error_ = "无法解析地址（模块未枚举？先附加进程）";
            } else {
                address_record rec;
                rec.real_address = pa.address;
                rec.address = add_addr_buf_;   // 保留用户原文（可解释地址）
                rec.description = add_desc_buf_[0] ? add_desc_buf_ : "手动添加";
                rec.type = static_cast<value_type>(
                    add_type_index_ >= 0 && add_type_index_ < 9 ? add_type_index_ : 2);
                rec.valid = true;
                rec.previous_value = read_address_value(rec.real_address, rec.type);
                rec.value = rec.previous_value;
                ctx_.address_list.add_record(rec);
                add_error_.clear();
                show_add_dialog_ = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(80, 0))) {
            add_error_.clear();
            show_add_dialog_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##result_row_menu")) {
        if (selected_row >= 0 && selected_row < row_total) {
            if (ImGui::MenuItem("添加到地址列表")) {
                add_address_to_list(repo->get_address_at_index((size_t)selected_row));
            }
            ImGui::Separator();
            if (ImGui::MenuItem("复制地址")) {
                char addr[32];
                snprintf(addr, sizeof(addr), "%016llX",
                         (unsigned long long)repo->get_address_at_index(selected_row));
                ImGui::SetClipboardText(addr);
            }
        }
        ImGui::EndPopup();
    }
}

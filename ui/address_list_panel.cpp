#include "address_list_panel.h"
#include "address_value.h"
#include "imgui.h"

#include <algorithm>
#include <chrono>

address_record* address_list_panel::find_record(uint64_t id) {
    auto it = std::find_if(records_.begin(), records_.end(),
                           [id](const address_record& r) { return r.id == id; });
    return it != records_.end() ? &*it : nullptr;
}

void address_list_panel::remove_record(uint64_t id) {
    auto it = std::remove_if(records_.begin(), records_.end(),
                             [id](const address_record& r) { return r.id == id; });
    records_.erase(it, records_.end());
}

void address_list_panel::add_record(const address_record& rec) {
    address_record r = rec;
    r.id = next_id_++;
    records_.push_back(std::move(r));
}

void address_list_panel::clear() {
    records_.clear();
    selected_row_id_ = 0;
    edit_id_ = 0;
}

// 实时刷新所有行：读取当前内存值，与基准值比对并高亮（约 200ms 节流）。
void address_list_panel::update_values() {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(200)) return;
    last = now;

    // 未附加进程：所有行显示 "---"，不改变已记录的基准值
    if (!process_manager::instance().is_attached()) return;

    for (auto& r : records_) {
        std::string cur = read_address_value(r.real_address, r.type);
        r.value = cur;
        // previous_value 是"加入/扫描时的基准值"，不动；
        // 实时值 != 基准值 → 红色高亮
        r.changed = (cur != r.previous_value && !r.previous_value.empty());
    }
}

bool address_list_panel::begin_edit(uint64_t id, std::string initial) {
    auto* rec = find_record(id);
    if (!rec) return false;
    edit_id_ = id;
    edit_buf_ = std::move(initial);
    return true;
}

void address_list_panel::commit_edit(address_record* rec) {
    if (!rec) { edit_id_ = 0; return; }
    // edit_col_: 1=描述（当前列），4=值（当前列）。commit 由 render 按 edit_col_ 分派。
    // 值为字符串描述编辑：直接写回记录；值为数值列：写回目标内存。
    if (edit_col_ == 1) {
        rec->description = edit_buf_;
    } else { // 值列
        if (write_address_value(rec->real_address, rec->type, edit_buf_)) {
            rec->value = edit_buf_;
            // 修改值后基准值更新为本值（高亮清零）
            rec->previous_value = edit_buf_;
            rec->changed = false;
        }
    }
    edit_id_ = 0;
}

void address_list_panel::render() {
    // 实时刷新值 + 高亮
    update_values();

    // 让表格吃满可用高度，表格自身可滚动
    float avail = ImGui::GetContentRegionAvail().y;

    if (ImGui::BeginChild("addr_list", ImVec2(0, avail), ImGuiChildFlags_Borders)) {
        if (ImGui::BeginTable("##addr_table", 5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Active");
            ImGui::TableSetupColumn("Description");
            ImGui::TableSetupColumn("Address");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Value");
            ImGui::TableHeadersRow();

            for (auto& r : records_) {
                ImGui::PushID((int)r.id);

                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::Checkbox("##frozen", &r.frozen);

                // ---- 第1列：描述（双击可编辑/写回）----
                ImGui::TableSetColumnIndex(1);
                if (edit_id_ == r.id && edit_col_ == 1) {
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    if (ImGui::InputText("##desc_edit", edit_buf_.data(), edit_buf_.size() + 1,
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                        commit_edit(&r);
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) edit_id_ = 0;
                } else {
                    if (ImGui::Selectable(r.description.c_str())) {
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            edit_col_ = 1;
                            begin_edit(r.id, r.description);
                        }
                    }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        selected_row_id_ = r.id;
                        ImGui::OpenPopup("##row_menu");
                    }
                }

                // ---- 第2列：地址 ----
                ImGui::TableSetColumnIndex(2);
                char buf[32];
                snprintf(buf, sizeof(buf), "%016llX", r.real_address);
                if (ImGui::Selectable(buf)) {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        // TODO: 打开内存浏览器跳到此地址
                    }
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    selected_row_id_ = r.id;
                    ImGui::OpenPopup("##row_menu");
                }

                // ---- 第3列：类型 ----
                ImGui::TableSetColumnIndex(3);
                static const char* type_names[] = {"Byte","2 Bytes","4 Bytes","8 Bytes","Float","Double","Text"};
                int idx = static_cast<int>(r.type);
                if (idx < 0 || idx >= IM_ARRAYSIZE(type_names)) idx = 0;
                ImGui::TextUnformatted(type_names[idx]);

                // ---- 第4列：数值（双击可编辑并写回内存；变动时变色）----
                ImGui::TableSetColumnIndex(4);
                if (edit_id_ == r.id && edit_col_ == 4) {
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    if (ImGui::InputText("##val_edit", edit_buf_.data(), edit_buf_.size() + 1,
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                        commit_edit(&r);
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) edit_id_ = 0;
                } else {
                    ImVec4 col = (r.changed) ? ImVec4(1.f, 0.4f, 0.4f, 1.f)
                                             : ImGui::GetStyleColorVec4(ImGuiCol_Text);
                    ImGui::PushStyleColor(ImGuiCol_Text, col);
                    if (ImGui::Selectable(r.value.c_str())) {
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            edit_col_ = 4;
                            begin_edit(r.id, r.value);
                        }
                    }
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        selected_row_id_ = r.id;
                        ImGui::OpenPopup("##row_menu");
                    }
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    // ---- 数据行右键菜单 ----
    if (ImGui::BeginPopup("##row_menu")) {
        address_record* rec = find_record(selected_row_id_);
        if (rec) {
            if (ImGui::MenuItem("Freeze", nullptr, &rec->frozen)) {
                // TODO: 真正执行写冻结值
            }
            if (ImGui::MenuItem("Show in hex", nullptr, &rec->show_hex)) {}
            if (ImGui::Separator(), ImGui::MenuItem("Modify")) {
                edit_col_ = 4;
                begin_edit(rec->id, rec->value);
            }
            if (ImGui::MenuItem("Edit Description")) {
                edit_col_ = 1;
                begin_edit(rec->id, rec->description);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete")) {
                remove_record(rec->id);
            }
        }
        ImGui::EndPopup();
    }
}

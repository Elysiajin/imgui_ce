#include "address_list_panel.h"
#include "imgui.h"

#include <algorithm>

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

void address_list_panel::render() {
    // 让表格吃满可用高度，表格自身可滚动
    float avail = ImGui::GetContentRegionAvail().y;

    if (ImGui::BeginChild("addr_list", ImVec2(0, avail), ImGuiChildFlags_Borders)) {
        if (ImGui::BeginTable("##addr_table", 5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_RowBg)) {
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

                ImGui::TableSetColumnIndex(1);
                if (edit_id_ == r.id) {
                    // 正在编辑：用一个窄输入框
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    if (ImGui::InputText("##desc_edit", edit_buf_.data(), edit_buf_.size() + 1,
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                        r.description = edit_buf_;
                        edit_id_ = 0;
                    }
                    // Esc 取消
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) edit_id_ = 0;
                } else {
                    if (ImGui::Selectable(r.description.c_str())) {
                        // 单击选中，双击进入编辑
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            edit_id_ = r.id;
                            edit_buf_ = r.description;
                        }
                    }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        selected_row_id_ = r.id;
                        ImGui::OpenPopup("##row_menu");
                    }
                }

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

                ImGui::TableSetColumnIndex(3);
                static const char* type_names[] = {"Byte","2 Bytes","4 Bytes","8 Bytes","Float","Double","Text"};
                int idx = static_cast<int>(r.type);
                if (idx < 0 || idx >= IM_ARRAYSIZE(type_names)) idx = 0;
                ImGui::TextUnformatted(type_names[idx]);

                ImGui::TableSetColumnIndex(4);
                ImVec4 col = (r.changed) ? ImVec4(1.f, 0.4f, 0.4f, 1.f)
                                         : ImGui::GetStyleColorVec4(ImGuiCol_Text);
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextUnformatted(r.value.c_str());
                ImGui::PopStyleColor();

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
                edit_buf_ = rec->value;
                edit_id_ = rec->id;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete")) {
                remove_record(rec->id);
            }
        }
        ImGui::EndPopup();
    }
}

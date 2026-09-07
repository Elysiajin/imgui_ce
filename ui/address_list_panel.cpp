#include "address_list_panel.h"
#include "address_value.h"
#include "app_context.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

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

// 每帧把 frozen 行的基准值写回内存，实现"数据冻结"。
// 冻结目标值 = previous_value（加入/扫描时的基准值，用户改值后同步更新）。
void address_list_panel::apply_freeze() {
    if (!process_manager::instance().is_attached()) return;
    for (auto& r : records_) {
        if (!r.frozen || r.previous_value.empty()) continue;
        write_address_value(r.real_address, r.type, r.previous_value);
    }
}

void address_list_panel::begin_edit(uint64_t id, std::string initial) {
    auto* rec = find_record(id);
    if (!rec) return;
    edit_id_ = id;
    std::snprintf(edit_buf_, sizeof(edit_buf_), "%s", initial.c_str());
}

void address_list_panel::commit_edit(address_record* rec) {
    if (!rec) { edit_id_ = 0; return; }
    // edit_col_: 1=描述, 3=类型, 4=值。
    switch (edit_col_) {
    case 1: rec->description = edit_buf_; break;
    case 3: {
        // 类型下拉：由下拉控件直接改 rec->type，此处无需处理
        break;
    }
    case 4: {
        if (write_address_value(rec->real_address, rec->type, edit_buf_)) {
            rec->value = edit_buf_;
            rec->previous_value = edit_buf_;
            rec->changed = false;
        }
        break;
    }
    }
    edit_id_ = 0;
}

// 地址列表类型名（与 value_type 枚举序一致）
static const char* k_type_names[] = {
    "Byte", "2 Bytes", "4 Bytes", "8 Bytes", "Float", "Double", "Text"
};
static_assert(IM_ARRAYSIZE(k_type_names) == 7);

void address_list_panel::render() {
    // 冻结：先把冻结行的基准值写回，再刷新显示
    apply_freeze();
    // 实时刷新值 + 高亮
    update_values();

    // 让表格吃满可用高度，表格自身可滚动
    float avail = ImGui::GetContentRegionAvail().y;

    // 右键菜单的固定 ID：在窗口 ID 栈上取一次，OpenPopupEx/BeginPopupEx 共用，
    // 保证右键打开与 BeginPopup 命中同一个弹出层。
    const ImGuiID row_menu_id = ImGui::GetID("##row_menu");

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

                // ---- 第1列：描述（双击可编辑）----
                ImGui::TableSetColumnIndex(1);
                if (edit_id_ == r.id && edit_col_ == 1) {
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    if (ImGui::InputText("##desc_edit", edit_buf_, sizeof(edit_buf_),
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                        commit_edit(&r);
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) edit_id_ = 0;
                } else {
                    if (ImGui::Selectable(r.description.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            edit_col_ = 1;
                            begin_edit(r.id, r.description);
                        }
                    }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        selected_row_id_ = r.id;
                        ImGui::OpenPopupEx(row_menu_id, ImGuiPopupFlags_MouseButtonRight);
                    }
                }

                // ---- 第2列：地址 ----
                ImGui::TableSetColumnIndex(2);
                char buf[32];
                snprintf(buf, sizeof(buf), "%016llX", r.real_address);
                if (ImGui::Selectable(buf, false, ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        // 双击地址：跳到内存浏览器十六进制 dump 视图
                        application_context::instance().open_memory_viewer.emit(
                            memory_viewer_mode::hexdump, r.real_address);
                    }
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    selected_row_id_ = r.id;
                    ImGui::OpenPopupEx(row_menu_id, ImGuiPopupFlags_MouseButtonRight);
                }

                // ---- 第3列：类型（下拉框）----
                ImGui::TableSetColumnIndex(3);
                {
                    int idx = static_cast<int>(r.type);
                    if (idx < 0 || idx >= (int)IM_ARRAYSIZE(k_type_names)) idx = 0;
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    if (ImGui::BeginCombo("##type_combo", k_type_names[idx])) {
                        for (int i = 0; i < (int)IM_ARRAYSIZE(k_type_names); ++i) {
                            const bool selected = (i == idx);
                            if (ImGui::Selectable(k_type_names[i], selected)) {
                                r.type = static_cast<value_type>(i);
                                // 类型改变后重读当前值并重置基准
                                r.previous_value.clear();
                                r.value = read_address_value(r.real_address, r.type);
                                r.previous_value = r.value;
                                r.changed = false;
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                }

                // ---- 第4列：数值（双击可编辑并写回内存；变动时变色）----
                ImGui::TableSetColumnIndex(4);
                if (edit_id_ == r.id && edit_col_ == 4) {
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    if (ImGui::InputText("##val_edit", edit_buf_, sizeof(edit_buf_),
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                        commit_edit(&r);
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) edit_id_ = 0;
                } else {
                    ImVec4 col = (r.changed) ? ImVec4(1.f, 0.4f, 0.4f, 1.f)
                                             : ImGui::GetStyleColorVec4(ImGuiCol_Text);
                    ImGui::PushStyleColor(ImGuiCol_Text, col);
                    if (ImGui::Selectable(r.value.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            edit_col_ = 4;
                            begin_edit(r.id, r.value);
                        }
                    }
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        selected_row_id_ = r.id;
                        ImGui::OpenPopupEx(row_menu_id, ImGuiPopupFlags_MouseButtonRight);
                    }
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    // ---- 数据行右键菜单 ----
    if (ImGui::BeginPopupEx(row_menu_id, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) {
        if (address_record* rec = find_record(selected_row_id_)) {
            if (ImGui::MenuItem("Freeze", nullptr, &rec->frozen)) {
                // TODO: 真正执行写冻结值
            }
            if (ImGui::MenuItem("Show in hex")) {  }
            ImGui::Separator();
            if (ImGui::MenuItem("View in dump")) {
                application_context::instance().open_memory_viewer.emit(
                    memory_viewer_mode::hexdump, rec->real_address);
            }
            if (ImGui::MenuItem("View in disassembly")) {
                application_context::instance().open_memory_viewer.emit(
                    memory_viewer_mode::disassembly, rec->real_address);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Modify")) {
                edit_col_ = 4;
                begin_edit(rec->id, rec->value);
            }
            if (ImGui::MenuItem("Edit Description")) {
                edit_col_ = 1;
                begin_edit(rec->id, rec->description);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete")) {
                pending_delete_id_ = rec->id;   // 行循环内不直接删除，见循环结束后的清理
            }
        }
        ImGui::EndPopup();
    }

    // 行循环已结束，此处移除菜单请求删除的行是安全的。
    if (pending_delete_id_ != 0) {
        remove_record(pending_delete_id_);
        pending_delete_id_ = 0;
        edit_id_ = 0;
    }
}

#include "address_list_panel.h"
#include "address_value.h"
#include "app_context.h"
#include "imgui.h"
#include "imgui_internal.h"

#include "ct/aa_script.h"
#include "ct/address_parser.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <unordered_map>

address_record* address_list_panel::find_record(uint64_t id) {
    auto it = std::find_if(records_.begin(), records_.end(),
                           [id](const address_record& r) { return r.id == id; });
    return it != records_.end() ? &*it : nullptr;
}

// 找 id 的下标；找不到返回 size()
static size_t index_of(const std::vector<address_record>& records, uint64_t id) {
    for (size_t i = 0; i < records.size(); ++i)
        if (records[i].id == id)
            return i;
    return records.size();
}

void address_list_panel::remove_record_with_children(uint64_t id) {
    const size_t i = index_of(records_, id);
    if (i >= records_.size()) return;
    // 先序数组：子树是 [i, 第一个 depth <= 自身 的行)
    size_t j = i + 1;
    while (j < records_.size() && records_[j].depth > records_[i].depth) ++j;
    records_.erase(records_.begin() + (long)i, records_.begin() + (long)j);
}

void address_list_panel::add_record(const address_record& rec) {
    address_record r = rec;
    r.id = next_id_++;
    records_.push_back(std::move(r));
}

void address_list_panel::add_child_record(uint64_t parent_id,
                                          const address_record& rec) {
    const size_t pi = index_of(records_, parent_id);
    if (pi >= records_.size()) {
        add_record(rec);
        return;
    }
    // 插到父的子树末尾（先序数组中父的后代结束处）
    size_t at = pi + 1;
    while (at < records_.size() && records_[at].depth > records_[pi].depth) ++at;
    address_record r = rec;
    r.id = next_id_++;
    r.parent_id = parent_id;
    r.depth = records_[pi].depth + 1;
    records_.insert(records_.begin() + (long)at, std::move(r));
}

void address_list_panel::clear() {
    records_.clear();
    user_symbols.clear();
    selected_row_id_ = 0;
    edit_id_ = 0;
    script_edit_id_ = 0;
}

static std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

// CT 的 UserdefinedSymbols（可解释地址原文）→ 名字到地址的查询表
static std::unordered_map<std::string, uint64_t> build_symbol_map(
    const std::vector<std::pair<std::string, std::string>>& user_symbols,
    const std::vector<module_info>& modules) {
    std::unordered_map<std::string, uint64_t> out;
    for (const auto& s : user_symbols) {
        const parsed_address pa = parse_interpretable_address(s.second, modules);
        if (pa.ok) {
            std::string low = s.first;
            std::transform(low.begin(), low.end(), low.begin(), ::tolower);
            out[low] = pa.address;
        }
    }
    return out;
}

// 指针链解引用：base +off0 → 读 → +off1 → 读 …（末级偏移后不再读）
static bool deref_pointer(address_record& r) {
    auto* mem = process_manager::instance().memory();
    if (!mem) { r.pointer_ok = false; return false; }
    uint64_t p = r.real_address;
    bool ok = true;
    for (size_t i = 0; i < r.offsets.size(); ++i) {
        p += r.offsets[i];
        if (i + 1 < r.offsets.size()) {
            if (!mem->read(p, &p, sizeof(p))) { ok = false; break; }
        }
    }
    r.pointer_final = p;
    r.pointer_ok = ok;
    return ok;
}

// 读当前值（组行空；脚本行 <脚本>；指针行先解引用；AOB/Binary 带长度参数）
std::string address_list_panel::read_current_value(address_record& r) {
    if (r.is_group) return {};
    if (r.type == value_type::auto_assembler) return "<脚本>";
    uint64_t addr = r.real_address;
    if (r.type != value_type::auto_assembler && !r.offsets.empty()) {
        if (!deref_pointer(r))
            return "---";
        addr = r.pointer_final;
    }
    if (addr == 0) return "??";
    return read_address_value(addr, r.type, r.radix, r.byte_length, r.bit_length);
}

// 实时刷新所有行：读取当前内存值，与基准值比对并高亮（约 200ms 节流）。
void address_list_panel::update_values() {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(200)) return;
    last = now;

    auto& pm = process_manager::instance();
    if (!pm.is_attached()) return;

    // 模块表快照（带锁拷贝，无系统调用）：未解析地址的重试解析用
    const auto modules = pm.module_snapshot();

    for (auto& r : records_) {
        if (r.is_group || r.type == value_type::auto_assembler) continue;

        // CT 加载时未解析成功的地址（模块未枚举/未附加），附加后重试
        if (r.real_address == 0 && !r.address.empty()) {
            const parsed_address pa = parse_interpretable_address(r.address, modules);
            if (pa.ok) r.real_address = pa.address;
        }
        if (r.real_address == 0) continue;

        std::string cur = read_current_value(r);
        if (cur == "---") continue;
        r.value = cur;
        // previous_value 是"加入/扫描时的基准值"，不动；
        // 实时值 != 基准值 → 红色高亮
        r.changed = (cur != r.previous_value && !r.previous_value.empty());
    }
}

// 每帧把 frozen 行的基准值写回内存，实现"数据冻结"。
// 冻结目标值 = previous_value（加入/扫描时的基准值，用户改值后同步更新）。
// 脚本/分组行不走数值冻结。
void address_list_panel::apply_freeze() {
    auto& pm = process_manager::instance();
    if (!pm.is_attached()) return;
    auto* mem = pm.memory();
    if (!mem) return;
    for (auto& r : records_) {
        if (!r.frozen || r.is_group || r.type == value_type::auto_assembler) continue;
        if (r.previous_value.empty() || r.real_address == 0) continue;
        write_address_value(r.real_address, r.type, r.previous_value, r.radix,
                            r.bit_length);
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
        if (write_address_value(rec->real_address, rec->type, edit_buf_, rec->radix,
                                rec->bit_length)) {
            rec->value = edit_buf_;
            rec->previous_value = edit_buf_;
            rec->changed = false;
        }
        break;
    }
    }
    edit_id_ = 0;
}

bool address_list_panel::has_children(const address_record& r) const {
    // 先序数组：下一行深度 +1 即第一个孩子
    const size_t i = index_of(records_, r.id);
    return i + 1 < records_.size() && records_[i + 1].depth == r.depth + 1;
}

// ---- 脚本条目激活 / 取消（AA 两遍汇编 + 全局符号落库）----
// symbols：解析视图（会随 registersymbol/unregistersymbol 就地更新），
// enable 时 registered 并入 user_symbols（全局符号，其他条目的 Address 可引用），
// disable 时 unregistered 移除。
static void run_script(address_record& r, bool enable,
                       const std::vector<module_info>& modules,
                       std::unordered_map<std::string, uint64_t>& symbols,
                       std::vector<std::pair<std::string, std::string>>& user_symbols) {
    auto& pm = process_manager::instance();
    if (!pm.is_attached()) {
        r.script_status = "未附加进程";
        return;
    }
    if (!r.session)
        r.session = std::make_shared<aa_session>();
    const aa_result res = aa_run_block(
        r.script, enable, modules, symbols, *r.session,
        pm.memory(),
        pm.memory() ? pm.memory()->architecture() : process_arch::unknown,
        pm.regions().enumerate());

    if (enable) {
        for (const auto& [name, addr] : res.registered) {
            char hex[24];
            std::snprintf(hex, sizeof(hex), "%llX", (unsigned long long)addr);
            bool found = false;
            for (auto& s : user_symbols) {
                if (_stricmp(s.first.c_str(), name.c_str()) == 0) {
                    s.second = hex;
                    found = true;
                    break;
                }
            }
            if (!found)
                user_symbols.emplace_back(name, hex);
            symbols[to_lower_copy(name)] = addr;
        }
    } else {
        for (const auto& name : res.unregistered) {
            for (auto it = user_symbols.begin(); it != user_symbols.end(); ++it) {
                if (_stricmp(it->first.c_str(), name.c_str()) == 0) {
                    user_symbols.erase(it);
                    break;
                }
            }
            symbols.erase(to_lower_copy(name));
        }
    }

    r.script_status.clear();
    for (const auto& w : res.warnings)
        r.script_status += w + "\n";
    if (!res.ok) {
        for (const auto& e : res.errors)
            r.script_status += e + "\n";
    }
}

// 地址列表类型名（与 value_type 枚举序一致）
static const char* k_type_names[] = {
    "字节", "2 字节", "4 字节", "8 字节", "单精度浮点数", "双精度浮点数",
    "文本", "字节数组", "位域(二进制)", "汇编脚本"
};
static_assert(IM_ARRAYSIZE(k_type_names) == 10);

// 进制显示名（与 value_radix 枚举序一致）
static const char* k_radix_names[] = { "十进制", "十六进制", "八进制" };
static_assert(IM_ARRAYSIZE(k_radix_names) == 3);

namespace {

// 树形折叠三角（自绘，无字体依赖；Progg Clean 无 ▸ 字形）
bool small_triangle(bool expanded) {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##tri", ImVec2(h * 0.7f, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cx = p.x + h * 0.28f;
    const float cy = p.y + h * 0.5f;
    const float r = h * 0.28f;
    if (expanded)
        dl->AddTriangleFilled(ImVec2(cx - r * 0.6f, cy - r),
                              ImVec2(cx - r * 0.6f, cy + r),
                              ImVec2(cx + r, cy), ImGui::GetColorU32(ImGuiCol_Text));
    else
        dl->AddTriangleFilled(ImVec2(cx - r * 0.7f, cy - r),
                              ImVec2(cx + r * 0.7f, cy),
                              ImVec2(cx - r * 0.7f, cy + r),
                              ImGui::GetColorU32(ImGuiCol_Text));
    return clicked;
}

} // namespace

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

    if (ImGui::BeginChild("addr_list", ImVec2(0, avail), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY)) {
        if (ImGui::BeginTable("##addr_table", 5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("激活");
            ImGui::TableSetupColumn("描述");
            ImGui::TableSetupColumn("地址");
            ImGui::TableSetupColumn("类型");
            ImGui::TableSetupColumn("数值");
            ImGui::TableHeadersRow();

            // 折叠游标：skip_depth 之下的后代全部隐藏，depth <= 时脱离折叠
            int skip_depth = INT_MAX;

            for (size_t idx = 0; idx < records_.size(); ++idx) {
                address_record& r = records_[idx];

                if (r.depth <= skip_depth) skip_depth = INT_MAX;
                if (r.depth > skip_depth) continue;   // 被折叠祖先隐藏

                ImGui::PushID((int)r.id);
                ImGui::TableNextRow();

                // ---- 第0列：激活（数值=冻结；脚本=执行；分组=联动子树）----
                ImGui::TableSetColumnIndex(0);
                if (r.is_group) {
                    // 分组勾选联动全部后代（含被折叠隐藏的）
                    if (ImGui::Checkbox("##group_active", &r.frozen)) {
                        const auto mods = process_manager::instance().module_snapshot();
                        auto syms = build_symbol_map(user_symbols, mods);
                        size_t j = idx + 1;
                        while (j < records_.size() &&
                               records_[j].depth > r.depth) {
                            address_record& c = records_[j];
                            if (c.type == value_type::auto_assembler) {
                                if (c.frozen != r.frozen)
                                    run_script(c, r.frozen, mods, syms, user_symbols);
                                c.frozen = r.frozen;
                            } else if (!c.is_group) {
                                c.frozen = r.frozen;
                            }
                            ++j;
                        }
                    }
                } else if (r.type == value_type::auto_assembler) {
                    if (ImGui::Checkbox("##script_active", &r.frozen)) {
                        const auto mods = process_manager::instance().module_snapshot();
                        auto syms = build_symbol_map(user_symbols, mods);
                        run_script(r, r.frozen, mods, syms, user_symbols);
                        if (!r.script_status.empty())
                            r.frozen = false;   // 执行失败回滚
                    }
                } else {
                    ImGui::Checkbox("##frozen", &r.frozen);
                }

                // ---- 第1列：描述（树缩进 + 折叠三角 + 可编辑）----
                ImGui::TableSetColumnIndex(1);
                if (r.depth > 0)
                    ImGui::Dummy(ImVec2((float)r.depth * 14.f, 0.f));
                ImGui::SameLine(0.f, 0.f);
                const bool kids = has_children(r);
                if (kids) {
                    if (small_triangle(r.expanded))
                        r.expanded = !r.expanded;
                    ImGui::SameLine(0.f, 2.f);
                }
                if (edit_id_ == r.id && edit_col_ == 1) {
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    if (ImGui::InputText("##desc_edit", edit_buf_, sizeof(edit_buf_),
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                        commit_edit(&r);
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) edit_id_ = 0;
                } else {
                    const ImVec4 text_col = r.has_color
                        ? ImGui::ColorConvertU32ToFloat4(r.color)
                        : ImGui::GetStyleColorVec4(ImGuiCol_Text);
                    ImGui::PushStyleColor(ImGuiCol_Text, text_col);
                    if (ImGui::Selectable(r.description.c_str(), false,
                                          ImGuiSelectableFlags_AllowDoubleClick |
                                          ImGuiSelectableFlags_SpanAllColumns)) {
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            if (r.type == value_type::auto_assembler) {
                                // 脚本行双击 → 编辑脚本
                                script_edit_id_ = r.id;
                                std::snprintf(script_buf_, sizeof(script_buf_), "%s",
                                              r.script.c_str());
                                ImGui::OpenPopup("##script_edit");
                            } else {
                                edit_col_ = 1;
                                begin_edit(r.id, r.description);
                            }
                        }
                    }
                    ImGui::PopStyleColor();
                    if (!r.script_status.empty() && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", r.script_status.c_str());
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        selected_row_id_ = r.id;
                        ImGui::OpenPopupEx(row_menu_id, ImGuiPopupFlags_MouseButtonRight);
                    }
                }

                // ---- 第2列：地址（指针行 P->…；未解析显示原文）----
                ImGui::TableSetColumnIndex(2);
                {
                    char buf[64];
                    if (r.is_group) {
                        buf[0] = 0;
                    } else if (!r.offsets.empty()) {
                        snprintf(buf, sizeof(buf), "P->%llX",
                                 (unsigned long long)r.pointer_final);
                    } else if (r.real_address != 0) {
                        snprintf(buf, sizeof(buf), "%016llX",
                                 (unsigned long long)r.real_address);
                    } else {
                        snprintf(buf, sizeof(buf), "%s",
                                 r.address.empty() ? "????????" : r.address.c_str());
                    }
                    if (ImGui::Selectable(buf, false, ImGuiSelectableFlags_AllowDoubleClick)) {
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                            r.real_address != 0) {
                            // 双击地址：跳到内存浏览器十六进制 dump 视图
                            application_context::instance().open_memory_viewer.emit(
                                memory_viewer_mode::hexdump,
                                !r.offsets.empty() ? r.pointer_final : r.real_address);
                        }
                    }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        selected_row_id_ = r.id;
                        ImGui::OpenPopupEx(row_menu_id, ImGuiPopupFlags_MouseButtonRight);
                    }
                }

                // ---- 第3列：类型（脚本/分组行固定显示）----
                ImGui::TableSetColumnIndex(3);
                if (r.is_group) {
                    ImGui::TextDisabled("");
                } else if (r.type == value_type::auto_assembler) {
                    ImGui::TextDisabled("<脚本>");
                } else {
                    int tidx = static_cast<int>(r.type);
                    if (tidx < 0 || tidx >= (int)IM_ARRAYSIZE(k_type_names)) tidx = 0;
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    if (ImGui::BeginCombo("##type_combo", k_type_names[tidx])) {
                        for (int i = 0; i < (int)IM_ARRAYSIZE(k_type_names); ++i) {
                            if (i == (int)value_type::auto_assembler)
                                continue;   // 脚本类型不可在此选（需脚本编辑）
                            const bool selected = (i == tidx);
                            if (ImGui::Selectable(k_type_names[i], selected)) {
                                r.type = static_cast<value_type>(i);
                                // 类型改变后重读当前值并重置基准
                                r.previous_value.clear();
                                r.value = read_current_value(r);
                                r.previous_value = r.value;
                                r.changed = false;
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                }

                // ---- 第4列：数值（变动变色；脚本行 <脚本>；双击编辑/查看）----
                ImGui::TableSetColumnIndex(4);
                if (!r.is_group) {
                    if (edit_id_ == r.id && edit_col_ == 4) {
                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                        if (ImGui::InputText("##val_edit", edit_buf_, sizeof(edit_buf_),
                                             ImGuiInputTextFlags_EnterReturnsTrue)) {
                            commit_edit(&r);
                        }
                        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) edit_id_ = 0;
                    } else {
                        const bool script_row =
                            (r.type == value_type::auto_assembler);
                        ImVec4 col = (r.changed) ? ImVec4(1.f, 0.4f, 0.4f, 1.f)
                                                 : ImGui::GetStyleColorVec4(ImGuiCol_Text);
                        if (script_row && r.frozen)
                            col = ImVec4(0.4f, 1.f, 0.5f, 1.f);
                        ImGui::PushStyleColor(ImGuiCol_Text, col);
                        const std::string shown =
                            script_row ? std::string("<脚本>") : r.value;
                        if (ImGui::Selectable(shown.c_str(), false,
                                              ImGuiSelectableFlags_AllowDoubleClick)) {
                            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                if (script_row) {
                                    script_edit_id_ = r.id;
                                    std::snprintf(script_buf_, sizeof(script_buf_), "%s",
                                                  r.script.c_str());
                                    ImGui::OpenPopup("##script_edit");
                                } else if (r.real_address != 0) {
                                    edit_col_ = 4;
                                    begin_edit(r.id, r.value);
                                }
                            }
                        }
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                            selected_row_id_ = r.id;
                            ImGui::OpenPopupEx(row_menu_id, ImGuiPopupFlags_MouseButtonRight);
                        }
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
            if (rec->type != value_type::auto_assembler && !rec->is_group) {
                if (ImGui::MenuItem("冻结", nullptr, &rec->frozen)) {
                    // TODO: 真正执行写冻结值
                }
                // 进制显示子菜单：十进制 / 十六进制 / 八进制
                if (ImGui::BeginMenu("显示为")) {
                    int r_idx = static_cast<int>(rec->radix);
                    if (r_idx < 0 || r_idx >= (int)IM_ARRAYSIZE(k_radix_names)) r_idx = 0;
                    for (int i = 0; i < (int)IM_ARRAYSIZE(k_radix_names); ++i) {
                        if (ImGui::MenuItem(k_radix_names[i], nullptr, i == r_idx)) {
                            rec->radix = static_cast<value_radix>(i);
                            rec->show_hex = (i == (int)value_radix::hex);
                            rec->value = read_current_value(*rec);
                            rec->previous_value = rec->value;
                            rec->changed = false;
                        }
                    }
                    ImGui::EndMenu();
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("在转储中查看") && rec->real_address != 0) {
                application_context::instance().open_memory_viewer.emit(
                    memory_viewer_mode::hexdump,
                    !rec->offsets.empty() ? rec->pointer_final : rec->real_address);
            }
            if (ImGui::MenuItem("在反汇编中查看") && rec->real_address != 0) {
                application_context::instance().open_memory_viewer.emit(
                    memory_viewer_mode::disassembly,
                    !rec->offsets.empty() ? rec->pointer_final : rec->real_address);
            }
            if (rec->type == value_type::auto_assembler && ImGui::MenuItem("编辑脚本")) {
                script_edit_id_ = rec->id;
                std::snprintf(script_buf_, sizeof(script_buf_), "%s", rec->script.c_str());
                ImGui::OpenPopup("##script_edit");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("修改") && !rec->is_group &&
                rec->type != value_type::auto_assembler) {
                edit_col_ = 4;
                begin_edit(rec->id, rec->value);
            }
            if (ImGui::MenuItem("编辑描述")) {
                edit_col_ = 1;
                begin_edit(rec->id, rec->description);
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("添加")) {
                if (ImGui::MenuItem("添加子条目")) {
                    address_record n;
                    n.description = "新条目";
                    add_child_record(rec->id, n);
                }
                if (ImGui::MenuItem("添加子分组")) {
                    address_record n;
                    n.description = "新分组";
                    n.is_group = true;
                    add_child_record(rec->id, n);
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("删除")) {
                pending_delete_id_ = rec->id;   // 行循环外删除（含子树）
            }
        }
        ImGui::EndPopup();
    }

    // ---- 脚本查看/编辑弹窗 ----
    if (ImGui::BeginPopup("##script_edit")) {
        ImGui::Text("脚本编辑");
        ImGui::InputTextMultiline("##script_text", script_buf_, sizeof(script_buf_),
                                  ImVec2(640, 380),
                                  ImGuiInputTextFlags_AllowTabInput);
        if (ImGui::Button("保存")) {
            if (address_record* rec = find_record(script_edit_id_)) {
                rec->script = script_buf_;
                if (rec->frozen) {
                    // 脚本变更后旧激活状态不可靠：先还原再要求手动重新激活
                    const auto mods = process_manager::instance().module_snapshot();
                    auto syms = build_symbol_map(user_symbols, mods);
                    run_script(*rec, false, mods, syms, user_symbols);
                    rec->frozen = false;
                    rec->script_status += "脚本已修改，请重新激活。\n";
                }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("关闭"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // 行循环已结束，此处移除菜单请求删除的行（含子树）是安全的。
    if (pending_delete_id_ != 0) {
        remove_record_with_children(pending_delete_id_);
        pending_delete_id_ = 0;
        edit_id_ = 0;
    }
}

#include "process_detail_window.h"
#include "imgui.h"
#include "ui/symbol_table.h"
#include "core/process_manager.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

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

// ── 模块符号行缓存 ──────────────────────────────────────────
// sym_rows_ 持有 symbol_table 内部存储的名字指针，因此：
//   - 仅在"切换模块/切换进程"时重建一次（导出表解析本身也是惰性的）
//   - 每帧渲染只做遍历 + ListClipper 裁剪，几千个符号也毫无压力
void process_detail_window::ensure_module_symbols(int mod_idx) {
    const auto& pm = process_manager::instance();
    const uint32_t pid = pm.attached_pid();

    if (sym_module_ == mod_idx && sym_pid_ == pid)
        return;

    sym_rows_.clear();
    sym_module_ = -1;
    sym_pid_    = pid;

    auto& st = symbol_table::instance();
    st.update_target(pid);
    if (mod_idx < 0 || mod_idx >= (int)modules_.size())
        return;

    if (!st.ensure_loaded(modules_[mod_idx].base))
        return;

    const module_symbols* ms = st.find_module(modules_[mod_idx].base);
    if (!ms || !ms->has_exports)
        return;

    sym_rows_.reserve(ms->symbols.size());
    for (const auto& s : ms->symbols)
        sym_rows_.push_back({ s.address, ms->symbol_name(s) });
    sym_module_ = mod_idx;
}

void process_detail_window::render_modules_tab() {
    auto& pm = process_manager::instance();

    // 懒加载模块（pid 变化时重载）
    if (modules_.empty() || state_.attached_pid != pm.attached_pid()) {
        modules_ = pm.modules().enumerate(pm.attached_pid());
        state_.attached_pid = pm.attached_pid();

        state_.module_names.clear();
        state_.module_names.push_back("<全部模块>");
        for (const auto& m : modules_)
            state_.module_names.push_back(m.name);
        if (state_.module_selected < 0 || state_.module_selected >= (int)state_.module_names.size())
            state_.module_selected = 0;

        sym_module_ = -1;  // 模块列表变化，行缓存失效
    }

    symbol_table::instance().update_target(pm.attached_pid());

    for (int mi = 0; mi < (int)modules_.size(); ++mi) {
        const auto& mod = modules_[mi];
        ImGui::PushID((int)(mod.base & 0xffffffff));

        if (ImGui::CollapsingHeader(mod.name.c_str())) {
            ImGui::Text("%s | 0x%llX | 0x%llX", mod.name.c_str(),
                        (unsigned long long)mod.base, (unsigned long long)mod.size);

            ImGui::TextUnformatted("详细符号:");
            ImGui::Spacing();

            ensure_module_symbols(mi);

            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
            ImGui::InputTextWithHint("##sym_filter", "过滤符号", sym_filter_, sizeof sym_filter_);

            if (sym_module_ != mi || sym_rows_.empty()) {
                ImGui::TextDisabled("(无导出)");
            } else {
                const float table_h = std::min(360.0f, ImGui::GetTextLineHeight() * 18.0f);
                if (ImGui::BeginTable("##syms", 2,
                                      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                                      ImVec2(0.f, table_h))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("地址", ImGuiTableColumnFlags_WidthFixed,
                                            ImGui::CalcTextSize("FFFFFFFFFFFF").x + 14.f);
                    ImGui::TableSetupColumn("符号", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();

                    // 过滤：小写包含匹配（空过滤串 = 全量）。
                    // 注意：ListClipper 的行数必须与实际绘制的行数一致 ——
                    // 若在 Step() 的显示区间内 continue 跳过不匹配的行，首个
                    // 区间可能一行都不画，clipper 第二次 Step() 推断行高时
                    // 游标未移动，触发 imgui.cpp "table->RowPosY1 ==
                    // clipper->StartPosY" 断言（输入过滤串即崩）。因此先构建
                    // 过滤后的索引，再交给 clipper 裁剪。
                    char filter[64];
                    snprintf(filter, sizeof filter, "%s", sym_filter_);
                    for (char& c : filter) c = (char)std::tolower((unsigned char)c);
                    const bool has_filter = filter[0] != 0;

                    auto contains_ci = [](const char* name, const char* f) {
                        for (const char* p = name; *p; ++p) {
                            size_t k = 0;
                            while (f[k] &&
                                   std::tolower((unsigned char)p[k]) == (unsigned char)f[k])
                                ++k;
                            if (!f[k]) return true;
                        }
                        return false;
                    };
                    if (has_filter) {
                        sym_filtered_.clear();
                        for (int i = 0; i < (int)sym_rows_.size(); ++i)
                            if (contains_ci(sym_rows_[i].name, filter))
                                sym_filtered_.push_back(i);
                    }
                    const int row_count = has_filter ? (int)sym_filtered_.size()
                                                     : (int)sym_rows_.size();

                    ImGuiListClipper clip;
                    clip.Begin(row_count);
                    while (clip.Step()) {
                        for (int ri = clip.DisplayStart; ri < clip.DisplayEnd; ++ri) {
                            const int      i = has_filter ? sym_filtered_[ri] : ri;
                            const sym_row& r = sym_rows_[i];
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::Text("%llX", (unsigned long long)r.address);
                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextUnformatted(r.name);
                        }
                    }
                    ImGui::EndTable();
                }
            }
        }
        ImGui::PopID();
        ImGui::Dummy(ImVec2(-1, 10));
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
            if (ImGui::BeginTabItem("模块")) {
                render_modules_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("内存区域")) {
                render_regions_tab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

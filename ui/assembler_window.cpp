#include "assembler_window.h"

#include "asm/asm_highlight.h"

#include <map>

void assembler_window::load_language() {
    editor_.SetLanguageDefinition(asm_language::get());
}

void assembler_window::log(int level, const std::string& text) {
    log_.push_back({level, text});
}

void assembler_window::reparse(const std::string& text) {
    parsed_text_ = text;
    diags_       = asm_parse::parse_source(text);

    TextEditor::ErrorMarkers markers;
    for (const auto& d : diags_) {
        if (!d.ok && markers.find(d.line) == markers.end())
            markers.emplace(d.line, d.message);
    }
    editor_.SetErrorMarkers(markers);
}

void assembler_window::render() {
    if (!state_.show_assembler_window)
        return;

    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("汇编窗口", &state_.show_assembler_window,
                      ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("文件")) {
            if (ImGui::MenuItem("保存")) {
            }
            if (ImGui::MenuItem("打开")) {
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("视图")) {
            if (ImGui::MenuItem("语法高亮", nullptr, &syntax_highlight_)) {
                editor_.SetColorizerEnable(syntax_highlight_);
            }
            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }

    // 文本变化时增量重解析（对比快照，避免每帧空转）
    if (editor_.IsTextChanged() || parsed_text_.empty()) {
        std::string text = editor_.GetText();
        if (text != parsed_text_)
            reparse(text);
    }

    // 工具条：状态统计 + 手动解析（写入调试输出）
    int error_count = 0;
    for (const auto& d : diags_)
        if (!d.ok) ++error_count;

    if (ImGui::Button("汇编")) {
        if (error_count == 0) {
            log(0, "[汇编] OK: " + std::to_string(diags_.size()) +
                       " 条指令解析成功, 0 错误");
        } else {
            log(2, "[汇编] 失败: " + std::to_string(error_count) +
                       " 个错误 / 共 " + std::to_string(diags_.size()) + " 行");
            for (const auto& d : diags_) {
                if (!d.ok)
                    log(2, "  第 " + std::to_string(d.line) + " 行: " + d.message);
            }
        }
    }
    ImGui::SameLine();
    if (error_count == 0)
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f),
                           "%d 行解析成功", (int)diags_.size());
    else
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f),
                           "%d 错误 / %d 行", error_count, (int)diags_.size());

    // 上侧：代码编辑区；下侧：调试输出（可拖动分割）
    const float log_height = 190.0f;
    float editor_height = ImGui::GetContentRegionAvail().y - log_height;
    if (editor_height < 100.0f) editor_height = 100.0f;

    ImGui::Separator();
    editor_.Render("汇编脚本", ImVec2(0, editor_height));

    render_log_panel();

    // begin里面压栈了，要弹出
    ImGui::End();
}

void assembler_window::render_log_panel() {
    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    if (!ImGui::CollapsingHeader("调试输出"))
        return;

    if (ImGui::BeginTabBar("##asm_output_tabs")) {
        // ---- 解析日志 ----
        if (ImGui::BeginTabItem("日志")) {
            if (ImGui::Button("清除"))
                log_.clear();
            ImGui::SameLine();
            ImGui::Checkbox("自动滚动", &auto_scroll_);

            ImGui::BeginChild("##asm_log_child", ImVec2(0, 0), false,
                              ImGuiWindowFlags_HorizontalScrollbar);
            for (const auto& e : log_) {
                ImVec4 color(0.70f, 0.75f, 0.80f, 1.0f);          // info
                if (e.level == 1) color = ImVec4(0.95f, 0.80f, 0.30f, 1.0f); // warn
                if (e.level == 2) color = ImVec4(0.95f, 0.35f, 0.35f, 1.0f); // error
                ImGui::TextColored(color, "%s", e.text.c_str());
            }
            if (auto_scroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // ---- 解析结果（结构化）----
        if (ImGui::BeginTabItem("解析结果")) {
            ImGui::BeginChild("##asm_parsed_child", ImVec2(0, 0), false,
                              ImGuiWindowFlags_HorizontalScrollbar);
            if (ImGui::BeginTable("##asm_parsed_table", 4,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("行号", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                ImGui::TableSetupColumn("标签", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                ImGui::TableSetupColumn("助记符", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("操作数");
                ImGui::TableHeadersRow();

                for (const auto& d : diags_) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (!d.ok)
                        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "%d", d.line);
                    else
                        ImGui::Text("%d", d.line);

                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(d.label.c_str());

                    ImGui::TableNextColumn();
                    if (!d.ok)
                        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "%s", d.message.c_str());
                    else
                        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "%s", d.mnemonic.c_str());

                    ImGui::TableNextColumn();
                    for (size_t i = 0; i < d.operands.size(); ++i) {
                        if (i) ImGui::SameLine();
                        const auto& op = d.operands[i];
                        ImGui::TextUnformatted(op.text.c_str());
                        if (ImGui::IsItemHovered() && !op.text.empty()) {
                            // 悬停提示：展示解析出的结构化分量
                            std::string tip;
                            switch (op.kind) {
                            case asm_parse::operand_kind::reg:
                                tip = "寄存器 " + op.reg_name +
                                      " (" + std::to_string(op.reg_size) + " 位)";
                                break;
                            case asm_parse::operand_kind::imm:
                                tip = "立即数 " + std::to_string(op.value);
                                break;
                            case asm_parse::operand_kind::memory: {
                                tip = "内存 [";
                                if (!op.seg_reg.empty()) tip += op.seg_reg + ":";
                                tip += op.base_reg;
                                if (!op.index_reg.empty())
                                    tip += " + " + op.index_reg + "*" + std::to_string(op.scale);
                                if (op.has_disp)
                                    tip += (op.disp >= 0 ? " + 0x" : " - 0x") +
                                           [&] {
                                               char buf[32];
                                               unsigned long long v =
                                                   op.disp < 0 ? (unsigned long long)(-op.disp)
                                                               : (unsigned long long)op.disp;
                                               snprintf(buf, sizeof(buf), "%llX", v);
                                               return std::string(buf);
                                           }();
                                for (const auto& s : op.symbols) tip += " + " + s;
                                tip += "]";
                                break;
                            }
                            case asm_parse::operand_kind::label:
                                tip = "符号 '" + op.symbol + "'";
                                break;
                            case asm_parse::operand_kind::far_label:
                                tip = "远符号 " + op.seg_reg + ":" + op.symbol;
                                break;
                            default:
                                break;
                            }
                            if (!tip.empty())
                                ImGui::SetTooltip("%s", tip.c_str());
                        }
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

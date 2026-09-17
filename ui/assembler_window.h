#ifndef ASSEMBLER_WINDOW_H
#define ASSEMBLER_WINDOW_H

#include "ui/ui_state.h"
#include "Imgui/imgui.h"
#include "Imgui/TextEditor.h"
#include "asm/asm_parser.h"

#include <string>
#include <vector>

// 自动汇编窗口：上侧 TextEditor 代码编辑（x86 汇编高亮 + 行内错误标记），
// 下侧调试输出（解析日志 / 解析结果两个 TAB）。
class assembler_window
{
public:
    explicit assembler_window(ui_state& ui) : state_(ui) { load_language(); }

    void render();
    void load_language();
private:
    // 文本变化时重新解析，更新错误标记与状态行
    void reparse(const std::string& text);

    // 调试输出区
    void log(int level, const std::string& text);
    void render_log_panel();

    ui_state& state_;
    TextEditor editor_;

    bool syntax_highlight_ = true;

    std::string parsed_text_;                 // 已解析的文本快照
    std::vector<asm_parse::asm_diag> diags_;  // 最近一次解析结果

    struct log_entry {
        int         level = 0;   // 0 = info, 1 = warn, 2 = error
        std::string text;
    };
    std::vector<log_entry> log_;
    bool auto_scroll_ = true;
};

#endif // ASSEMBLER_WINDOW_H

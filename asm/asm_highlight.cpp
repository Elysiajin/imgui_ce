#include "asm_highlight.h"

#include "asm/asm_parser.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_set>

namespace {

bool is_word_start(char c) {
    return std::isalpha((unsigned char)c) || c == '_' || c == '$' ||
           c == '?' || c == '@' || c == '.';
}

bool is_word_char(char c) {
    return is_word_start(c) || std::isdigit((unsigned char)c);
}

bool is_hex_digit(char c) {
    return std::isxdigit((unsigned char)c);
}

std::string to_upper(const std::string& s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return (char)std::toupper(c); });
    return r;
}

// TextEditor 的 mTokenize 回调：返回 false 时该字符按默认色跳过。
bool tokenize_asm(const char* in_begin, const char* in_end,
                  const char*& out_begin, const char*& out_end,
                  TextEditor::PaletteIndex& palette_index) {
    char c = *in_begin;

    if (std::isspace((unsigned char)c))
        return false;

    // ';' 注释到行尾
    if (c == ';') {
        out_begin = in_begin;
        out_end   = in_end;
        palette_index = TextEditor::PaletteIndex::Comment;
        return true;
    }

    // "//" 注释到行尾
    if (c == '/' && in_begin + 1 < in_end && in_begin[1] == '/') {
        out_begin = in_begin;
        out_end   = in_end;
        palette_index = TextEditor::PaletteIndex::Comment;
        return true;
    }

    // 字符字面量 'A'
    if (c == '\'') {
        const char* p = in_begin + 1;
        while (p < in_end && *p != '\'') ++p;
        out_begin = in_begin;
        out_end   = (p < in_end) ? p + 1 : in_end;
        palette_index = TextEditor::PaletteIndex::CharLiteral;
        return true;
    }

    // 数字：0x1A / 0b1010 / 1234 / 1234h / 0ABCh（贪心吞十六进制字符与后缀）
    if (std::isdigit((unsigned char)c)) {
        const char* p = in_begin + 1;
        while (p < in_end && (is_hex_digit(*p) || *p == 'x' || *p == 'X' ||
                              *p == 'h' || *p == 'H' || *p == 'b' || *p == 'B'))
            ++p;
        out_begin = in_begin;
        out_end   = p;
        palette_index = TextEditor::PaletteIndex::Number;
        return true;
    }

    // 标识符：具体颜色（关键字 / 寄存器 / 伪指令）由 Colorize 阶段查表决定
    if (is_word_start(c)) {
        const char* p = in_begin + 1;
        while (p < in_end && is_word_char(*p)) ++p;
        out_begin = in_begin;
        out_end   = p;
        palette_index = TextEditor::PaletteIndex::Identifier;
        return true;
    }

    // 标点：, [ ] ( ) + - * / :
    if (std::strchr(",[]()+-*/:", c)) {
        out_begin = in_begin;
        out_end   = in_begin + 1;
        palette_index = TextEditor::PaletteIndex::Punctuation;
        return true;
    }

    return false;
}

} // namespace

const TextEditor::LanguageDefinition& asm_language::get() {
    static const TextEditor::LanguageDefinition k_def = [] {
        TextEditor::LanguageDefinition def;
        def.mCaseSensitive      = false;   // 查表前统一转大写
        def.mSingleLineComment  = ";";
        def.mPreprocChar        = '\0';    // 汇编无预处理
        def.mAutoIndentation    = false;
        def.mTokenize           = &tokenize_asm;

        // 助记符 + 指令前缀 -> 关键字
        for (const auto& m : asm_parse::mnemonic_list())
            def.mKeywords.insert(to_upper(m));

        // 寄存器 -> 已知标识符
        for (const auto& r : asm_parse::register_list())
            def.mIdentifiers.emplace(to_upper(r), TextEditor::Identifier());

        // 段寄存器 / 尺寸说明符 / ptr -> 预处理标识符（第三种颜色）
        static const char* k_preproc[] = {
            "BYTE", "WORD", "DWORD", "QWORD", "TBYTE", "OWORD",
            "XMMWORD", "YMMWORD", "PTR",
        };
        for (const char* p : k_preproc)
            def.mPreprocIdentifiers.emplace(p, TextEditor::Identifier());

        return def;
    }();
    return k_def;
}

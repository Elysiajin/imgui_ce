#ifndef ASM_HIGHLIGHT_H
#define ASM_HIGHLIGHT_H

#include "Imgui/TextEditor.h"

// 基于解析器词法表构建 x86/x64 汇编的 TextEditor 语言定义。
//
// 高亮方案（利用 TextEditor 现有的着色管线，无需改库）：
//   - 助记符 / 指令前缀 / CE 伪指令  -> mKeywords      (Keyword 色)
//   - 寄存器（全位宽）               -> mIdentifiers   (KnownIdentifier 色)
//   - 段覆盖 / 尺寸说明符 / ptr      -> mPreprocIdentifiers
//   - 数字（0x/十进制/0b/字符字面量）-> mTokenize 回调  (Number 色)
//   - 注释 ; 与 //                   -> mSingleLineComment
//   - 未命中上述规则的标识符走默认 Identifier 色
class asm_language {
public:
    // 返回缓存的共享定义（内部首次调用时构建，随后直接复用）。
    static const TextEditor::LanguageDefinition& get();
};

#endif // ASM_HIGHLIGHT_H

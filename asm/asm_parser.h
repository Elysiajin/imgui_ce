#pragma once

// Intel 语法 x86/x64 汇编指令文本解析器。
//
// 只做"解析"（词法 + 语法检查），不做机器码编码；每行产出一个 asm_diag，
// 无错误即表示该行可被后续的编码阶段（Zydis Encoder）直接消费。
//
// 支持的语法子集：
//   - 指令：助记符 [操作数[, 操作数]]，支持 rep/lock 等前缀
//   - 标号：foo: / foo: mov eax, 1
//   - 注释：';' 与 "//" 到行尾
//   - CE 段标记：[enable] / [disable]（跳过）
//   - 操作数：寄存器、立即数（0x/十进制/二进制/字符字面量，可带符号）、
//     内存操作数 [base + index*scale + disp + symbol]、
//     段覆盖（fs:[0x30] / ds:symbol）、尺寸说明符（dword ptr [..] 等）
//
// 返回值：每行一条 asm_diag（行号 1-based）。

#include <cstdint>
#include <string>
#include <vector>

namespace asm_parse {

// 操作数种类
enum class operand_kind {
    none = 0,
    reg,        // 寄存器
    imm,        // 立即数（value 有效）
    memory,     // 内存操作数（base/index/disp/symbols 有效）
    label,      // 标号 / 符号引用（symbol 有效）
    far_label,  // seg:symbol 形式（seg + symbol 有效）
};

struct asm_operand {
    operand_kind kind = operand_kind::none;
    std::string  text;               // 原文（压缩空白，小写）
    std::string  reg_name;           // kind == reg 时为规范小写名，如 "rax"
    int          reg_size = 0;       // 寄存器位宽：8/16/32/64/...
    int64_t      value    = 0;       // kind == imm 时为数值
    std::string  symbol;             // kind == label/far_label 时为符号名
    // 内存操作数分量
    std::string seg_reg;             // 段覆盖（fs/ds/...，可空）
    std::string base_reg;            // 基址寄存器（规范小写名，可空）
    std::string index_reg;           // 变址寄存器（可空）
    int         scale     = 0;       // 变址倍率 1/2/4/8，0 表示无
    int64_t     disp      = 0;       // 位移/绝对地址累加值
    bool        has_base  = false;
    bool        has_index = false;
    bool        has_disp  = false;
    std::vector<std::string> symbols; // [ ] 内的符号引用（如 [module+0x10]）
};

// 单行解析结果
struct asm_diag {
    int         line = 0;            // 1-based 行号
    bool        ok   = true;
    std::string message;             // 错误描述（ok == false 时）
    std::string label;               // 行首标号（可空）
    std::string mnemonic;            // 规范小写助记符（如 "rep movsb"）
    std::vector<asm_operand> operands;
};

// 解析一段汇编源码（按行拆分，逐行解析），返回非空行的诊断。
std::vector<asm_diag> parse_source(const std::string& source);

// 解析单行（不含换行）。line 为 1-based 行号，仅用于诊断。
asm_diag parse_line(const std::string& text, int line);

// ---- 词法表（供语法高亮 / 补全复用，名字均为小写）----
const std::vector<std::string>& mnemonic_list();
const std::vector<std::string>& register_list();

bool is_register(const std::string& name);   // 大小写不敏感
int  register_size(const std::string& name); // 位宽，未知返回 0

} // namespace asm_parse

#include "asm/asm_parser.h"

#include <cstdio>

static const char* kind_name(asm_parse::operand_kind k) {
    switch (k) {
    case asm_parse::operand_kind::reg:       return "reg ";
    case asm_parse::operand_kind::imm:       return "imm ";
    case asm_parse::operand_kind::memory:    return "mem ";
    case asm_parse::operand_kind::label:     return "lbl ";
    case asm_parse::operand_kind::far_label: return "flbl";
    default:                                 return "none";
    }
}

int main() {
    const char* src =
        "; comment line\n"
        "mov rax, rbx\n"
        "mov EAX, [Rbx + RCx*4 - 0x10]\n"
        "lea rax, [rip+0x1234]\n"
        "push 0x10\n"
        "push -1\n"
        "movzx eax, byte ptr [rdi]\n"
        "mov rax, qword ptr fs:[0x30]\n"
        "mylabel:\n"
        "mylabel: nop\n"
        "rep movsb\n"
        "mov rax, [module+0x123]\n"
        "mov eax, 'A'\n"
        "add [rax], 5\n"
        "db 0x90, 90, 0b1010, 0ABCh\n"
        "call sub_1234\n"
        "movss xmm0, [rax+r8*2]\n"
        "vzeroupper\n"
        "mov rax,\n"
        "xyz eax, ebx\n"
        "mov rax, [rbx+rcx*3]\n"
        "mov rax rbx\n"
        "mov rax, [rbx+rcx+rdx]\n"
        "mov [rax\n"
        "mov rax, [rax+rbx*8+rcx]\n";

    auto diags = asm_parse::parse_source(src);
    for (const auto& d : diags) {
        if (!d.ok) {
            printf("L%02d  ERROR   %s\n", d.line, d.message.c_str());
            continue;
        }
        printf("L%02d  %-10s %-8s", d.line,
               (!d.label.empty() && d.mnemonic.empty() ? "[label]" : d.mnemonic.c_str()),
               d.label.c_str());
        for (const auto& op : d.operands) {
            printf(" | %s:%s", kind_name(op.kind), op.text.c_str());
            if (op.kind == asm_parse::operand_kind::memory) {
                printf("(b=%s,i=%s*%d,d=%lld",
                       op.base_reg.c_str(), op.index_reg.c_str(),
                       op.scale, (long long)op.disp);
                for (const auto& s : op.symbols) printf(",sym=%s", s.c_str());
                printf(")");
            }
            if (op.kind == asm_parse::operand_kind::imm)
                printf("(%lld)", (long long)op.value);
        }
        printf("\n");
    }
    return 0;
}

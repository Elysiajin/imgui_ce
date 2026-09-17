#include "asm_parser.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <unordered_map>
#include <unordered_set>

namespace asm_parse {
namespace {

const std::vector<std::string>& reg_names() {
    static const std::vector<std::string> k_regs = [] {
        std::vector<std::string> v = {
            // 64 位通用
            "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "rip",
            "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
            // 32 位
            "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "eip",
            "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d",
            // 16 位
            "ax",  "bx",  "cx",  "dx",  "si",  "di",  "bp",  "sp",
            "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w",
            // 8 位
            "al",  "ah",  "bl",  "bh",  "cl",  "ch",  "dl",  "dh",
            "sil", "dil", "bpl", "spl",
            "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b",
            // 段寄存器
            "cs", "ds", "es", "fs", "gs", "ss",
        };
        for (int i = 0; i < 8;  ++i) v.push_back("st"  + std::to_string(i));
        for (int i = 0; i < 8;  ++i) v.push_back("mm"  + std::to_string(i));
        for (int i = 0; i < 8;  ++i) v.push_back("k"   + std::to_string(i));
        for (int i = 0; i < 16; ++i) v.push_back("cr"  + std::to_string(i));
        for (int i = 0; i < 16; ++i) v.push_back("dr"  + std::to_string(i));
        for (int i = 0; i < 32; ++i) v.push_back("xmm" + std::to_string(i));
        for (int i = 0; i < 32; ++i) v.push_back("ymm" + std::to_string(i));
        for (int i = 0; i < 32; ++i) v.push_back("zmm" + std::to_string(i));
        return v;
    }();
    return k_regs;
}

int reg_width(const std::string& r) {
    static const std::unordered_set<std::string> k8 = {
        "al", "ah", "bl", "bh", "cl", "ch", "dl", "dh",
    };
    static const std::unordered_set<std::string> k16 = {
        "ax", "bx", "cx", "dx", "si", "di", "bp", "sp",
    };
    if (r[0] == 'z') return 512;
    if (r[0] == 'y') return 256;
    if (r[0] == 'x') return 128;
    if (r[0] == 'k') return 64;
    if (r[0] == 'm') return 64;
    if (r[0] == 'c') return 64;
    if (r[0] == 'd' && r != "ds" && r != "di" && r != "dl" && r != "dh") return 64;
    if (r[0] == 's' && r.compare(0, 2, "st") == 0) return 80;
    if (r[0] == 'r') {
        if (r.back() == 'd') return 32;
        if (r.back() == 'w') return 16;
        if (r.back() == 'b') return 8;
        return 64;
    }
    if (k8.count(r)) return 8;
    if (r == "cs" || r == "ds" || r == "es" || r == "fs" || r == "gs" || r == "ss")
        return 16;
    if (k16.count(r)) return 16;
    return 32;
}

const std::unordered_map<std::string, int>& reg_sizes() {
    static const std::unordered_map<std::string, int> k_sizes = [] {
        std::unordered_map<std::string, int> m;
        for (const auto& r : reg_names()) m.emplace(r, reg_width(r));
        return m;
    }();
    return k_sizes;
}

const std::unordered_set<std::string>& mnemonics() {
    static const std::unordered_set<std::string> k_set = {
        "aaa", "aad", "aam", "aas", "adc", "add", "addpd", "addps", "addsd",
        "addss", "addsubpd", "addsubps", "and", "andnpd", "andnps", "andpd",
        "andps", "arpl", "blendpd", "blendps", "blendvpd", "blendvps", "bound",
        "bsf", "bsr", "bswap", "bt", "btc", "btr", "bts", "call", "cbw", "cwde",
        "cdqe", "cdq", "cqo", "clac", "clc", "cld", "cli", "clts", "cmc",
        "cmova", "cmovae", "cmovb", "cmovbe", "cmovc", "cmove", "cmovg",
        "cmovge", "cmovl", "cmovle", "cmovna", "cmovnae", "cmovnb", "cmovnbe",
        "cmovnc", "cmovne", "cmovng", "cmovnge", "cmovnl", "cmovnle", "cmovno",
        "cmovnp", "cmovns", "cmovnz", "cmovo", "cmovp", "cmovpe", "cmovpo",
        "cmovs", "cmovz", "cmp", "cmppd", "cmpps", "cmpsd", "cmpss", "cmpsb",
        "cmpsw", "cmpsq", "cmpxchg", "cmpxchg8b", "cmpxchg16b", "comisd",
        "comiss", "cpuid", "cvtdq2pd", "cvtdq2ps", "cvtpd2dq", "cvtpd2pi",
        "cvtpd2ps", "cvtpi2pd", "cvtpi2ps", "cvtps2dq", "cvtps2pd", "cvtps2pi",
        "cvtsd2si", "cvtsd2ss", "cvtsi2sd", "cvtsi2ss", "cvtss2sd", "cvtss2si",
        "cvttpd2dq", "cvttpd2pi", "cvttps2dq", "cvttps2pi", "cvttsd2si",
        "cvttss2si", "cwd", "daa", "das", "dec", "div", "divpd", "divps",
        "divsd", "divss", "dppd", "dpps", "emms", "enter", "extractps",
        "f2xm1", "fabs", "fadd", "faddp", "fbld", "fbstp", "fchs", "fclex",
        "fcom", "fcomp", "fcompp", "fcos", "fdecstp", "fdiv", "fdivp", "fdivr",
        "fdivrp", "femms", "ffree", "fiadd", "ficom", "ficomp", "fidiv",
        "fidivr", "fild", "fimul", "fincstp", "finit", "fist", "fistp",
        "fisttp", "fisub", "fisubr", "fld", "fld1", "fldcw", "fldenv", "fldl2e",
        "fldl2t", "fldlg2", "fldln2", "fldpi", "fldz", "fmul", "fmulp", "fnclex",
        "fninit", "fnop", "fnsave", "fnstcw", "fnstenv", "fnstsw", "fpatan",
        "fprem", "fprem1", "fptan", "frndint", "frstor", "fsave", "fscale",
        "fsin", "fsincos", "fsqrt", "fst", "fstcw", "fstenv", "fstp", "fstsw",
        "fsub", "fsubp", "fsubr", "fsubrp", "ftst", "fucom", "fucomi",
        "fucomip", "fucomp", "fucompp", "fxam", "fxch", "fxrstor", "fxsave",
        "fyl2x", "fyl2xp1", "getsec", "hlt", "idiv", "imul", "in", "inc",
        "insb", "insw", "insd", "int", "int3", "into", "invd", "invlpg", "iret",
        "iretd", "iretq", "ja", "jae", "jb", "jbe", "jc", "je", "jg", "jge",
        "jl", "jle", "jmp", "jna", "jnae", "jnb", "jnbe", "jnc", "jne", "jng",
        "jnge", "jnl", "jnle", "jno", "jnp", "jns", "jnz", "jo", "jp", "jpe",
        "jpo", "js", "jz", "lahf", "ldmxcsr", "lds", "lea", "leave", "les",
        "lfence", "lfs", "lgdt", "lgs", "lidt", "lldt", "lmsw", "lock", "lods",
        "lodsb", "lodsw", "lodsd", "lodsq", "loop", "loope", "loopz", "loopne",
        "loopnz", "lsl", "lss", "ltr", "maskmovdqu", "maskmovq", "maxpd",
        "maxps", "maxsd", "maxss", "mfence", "minpd", "minps", "minsd", "minss",
        "monitor", "mov", "movapd", "movaps", "movbe", "movd", "movddup",
        "movdq2q", "movdqa", "movdqu", "movhlps", "movhpd", "movhps", "movlhps",
        "movlpd", "movlps", "movmskpd", "movmskps", "movntdq", "movnti",
        "movntpd", "movntps", "movntq", "movq", "movq2dq", "movs", "movsb",
        "movsw", "movsd", "movss", "movsq", "movsx", "movsxd", "movupd", "movups",
        "movzx", "mul", "mulpd", "mulps", "mulsd", "mulss", "mwait", "neg",
        "nop", "not", "or", "orpd", "orps", "out", "outsb", "outsw", "outsd",
        "packssdw", "packsswb", "packuswb", "paddb", "paddd", "paddq", "paddw",
        "pand", "pandn", "pause", "pavgb", "pavgw", "pblendw", "pcmpeqb",
        "pcmpeqd", "pcmpeqq", "pcmpeqw", "pcmpgtb", "pcmpgtd", "pcmpgtq",
        "pcmpgtw", "pextrb", "pextrd", "pextrq", "pextrw", "pinsrb", "pinsrd",
        "pinsrq", "pinsw", "pmaddwd", "pmaxsw", "pmaxub", "pminsw", "pminub",
        "pmovmskb", "pmulhuw", "pmulhw", "pmullw", "pmuludq", "pop", "popa",
        "popad", "popcnt", "popf", "popfd", "popfq", "por", "prefetch0",
        "prefetch1", "prefetch2", "prefetchnta", "psadbw", "pshufb", "pshufd",
        "pshufhw", "pshuflw", "pshufw", "psignb", "psignd", "psignw", "pslld",
        "psllq", "psllw", "psrad", "psraw", "psrld", "psrlq", "psrlw", "psubb",
        "psubd", "psubq", "psubw", "psubsb", "psubsw", "psubusb", "psubusw",
        "ptest", "punpckhbw", "punpckhdq", "punpckhqdq", "punpckhwd",
        "punpcklbw", "punpckldq", "punpcklqdq", "punpcklwd", "push", "pusha",
        "pushad", "pushf", "pushfd", "pushfq", "pxor", "rcl", "rcpps", "rcpss",
        "rcr", "rdfsbase", "rdgsbase", "rdmsr", "rdpmc", "rdrand", "rdseed",
        "rdtsc", "rdtscp", "rep", "repe", "repne", "repz", "repnz", "ret",
        "retf", "rol", "ror", "rorx", "roundpd", "roundps", "roundsd",
        "roundss", "rsqrtps", "rsqrtss", "sahf", "sal", "sar", "sbb", "scas",
        "scasb", "scasw", "scasd", "scasq", "seta", "setae", "setb", "setbe",
        "setc", "sete", "setg", "setge", "setl", "setle", "setna", "setnae",
        "setnb", "setnbe", "setnc", "setne", "setng", "setnge", "setnl",
        "setnle", "setno", "setnp", "setns", "setnz", "seto", "setp", "setpe",
        "setpo", "sets", "setz", "sfence", "sgdt", "shl", "shld", "shr", "shrd",
        "shufpd", "shufps", "sidt", "sldt", "smsw", "sqrtpd", "sqrtps",
        "sqrtsd", "sqrtss", "stac", "stc", "std", "sti", "stmxcsr", "str",
        "sub", "subpd", "subps", "subsd", "subss", "swapgs", "syscall",
        "sysenter", "sysexit", "sysret", "test", "tzcnt", "ucomisd", "ucomiss",
        "ud2", "unpckhpd", "unpckhps", "unpcklpd", "unpcklps", "verr", "verw",
        "wait", "wbinvd", "wrfsbase", "wrgsbase", "wrmsr", "xadd", "xchg",
        "xgetbv", "xlat", "xlatb", "xor", "xrstor", "xsave", "xsaveopt",
        "xsetbv",
        // 常见 AVX/AVX2
        "vaddpd", "vaddps", "vaddsd", "vaddss", "vandpd", "vandps",
        "vbroadcastf128", "vbroadcastsd", "vbroadcastss", "vcmppd", "vcmpps",
        "vcomisd", "vcomiss", "vcvtsd2ss", "vcvtsi2sd", "vcvtsi2ss",
        "vcvtss2sd", "vdivpd", "vdivps", "vdivsd", "vdivss", "vdppd", "vdpps",
        "vextractf128", "vextractps", "vfmaddpd", "vfmaddps", "vfmaddsd",
        "vfmaddss", "vfmsubpd", "vfmsubps", "vgatherdpd", "vgatherdps",
        "vgatherqpd", "vgatherqps", "vinsertf128", "vlddqu", "vmaskmovpd",
        "vmaskmovps", "vmaxpd", "vmaxps", "vminpd", "vminps", "vmovapd",
        "vmovaps", "vmovd", "vmovdqa", "vmovdqu", "vmovmskpd", "vmovmskps",
        "vmovq", "vmovsd", "vmovss", "vmovupd", "vmovups", "vmulpd", "vmulps",
        "vmulsd", "vmulss", "vorpd", "vorps", "vpaddb", "vpaddd", "vpaddq",
        "vpaddw", "vpand", "vpandn", "vpblendd", "vpblendvb", "vpblendw",
        "vpbroadcastb", "vpbroadcastd", "vpbroadcastq", "vpbroadcastw",
        "vpgatherdd", "vpgatherdq", "vpgatherqd", "vpgatherqq", "vpmaddwd",
        "vpmulld", "vpmullw", "vpor", "vpshufb", "vpshufd", "vpslld", "vpsllq",
        "vpsllvd", "vpsllw", "vpsrad", "vpsraw", "vpsrld", "vpsrlq", "vpsrlvd",
        "vpsrlw", "vpsubb", "vpsubd", "vpsubq", "vpsubw", "vpunpckhbw",
        "vpunpckhdq", "vpunpcklqdq", "vpunpcklwd", "vpxor", "vrcpps",
        "vroundpd", "vroundps", "vshufpd", "vshufps", "vsqrtpd", "vsqrtps",
        "vsqrtsd", "vsqrtss", "vsubpd", "vsubps", "vsubsd", "vsubss",
        "vtestpd", "vtestps", "vucomisd", "vucomiss", "vunpckhpd", "vunpckhps",
        "vunpcklpd", "vunpcklps", "vxorpd", "vxorps", "vzeroall", "vzeroupper",
        // CE / MASM 风格伪指令与运算符（按助记符位置处理，不做操作数校验）
        "db", "dw", "dd", "dq", "rb", "rw", "rd", "rq", "align", "org",
        "alloc", "dealloc", "label", "registersymbol", "unregistersymbol",
        "aobscan", "aobscanmodule", "aobscanregion", "define", "createthread",
        "createthreadandwait", "loadlibrary", "assert", "luacall", "asmcall",
        "fullaccess", "globalalloc", "structure", "ends", "offsets",
    };
    return k_set;
}

const std::unordered_set<std::string>& instruction_prefixes() {
    static const std::unordered_set<std::string> k_set = {
        "rep", "repe", "repne", "repz", "repnz", "lock", "wait", "notrack",
    };
    return k_set;
}

const std::unordered_set<std::string>& segment_names() {
    static const std::unordered_set<std::string> k_set = {
        "cs", "ds", "es", "fs", "gs", "ss",
    };
    return k_set;
}

bool is_section_marker(const std::string& s) {
    std::string t;
    t.reserve(s.size());
    for (char c : s) t.push_back((char)std::tolower((unsigned char)c));
    return t == "[enable]" || t == "[disable]";
}

std::string to_lower(const std::string& s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return r;
}

bool is_ident_start(char c) {
    return std::isalpha((unsigned char)c) || c == '_' || c == '$' ||
           c == '?' || c == '@' || c == '.';
}

bool is_ident_char(char c) {
    return is_ident_start(c) || std::isdigit((unsigned char)c);
}

// 整数字面量：0x1A / 0b1010 / 1234 / 1234h / 0ABCh / 'A'
bool parse_int(const std::string& s, int64_t& out) {
    if (s.empty()) return false;
    if (s.size() >= 3 && s.front() == '\'' && s.back() == '\'') {
        out = (int64_t)(unsigned char)s[1];
        return true;
    }
    std::string t = to_lower(s);
    int base = 10;
    size_t start = 0;
    if (t.compare(0, 2, "0x") == 0) {
        base = 16;
        start = 2;
    } else if (t.compare(0, 2, "0b") == 0) {
        base = 2;
        start = 2;
    } else if (t.back() == 'h' && std::isdigit((unsigned char)t[0])) {
        base = 16;
        t.pop_back();
    }
    if (start >= t.size()) return false;
    const char* first = t.data() + start;
    const char* last  = t.data() + t.size();
    auto r = std::from_chars(first, last, out, base);
    return r.ec == std::errc() && r.ptr == last;
}

// 将 [begin, end) 内的空白压缩掉并转小写，生成操作数归一化文本
std::string normalize_text(const char* begin, const char* end) {
    std::string r;
    r.reserve(end - begin);
    for (const char* p = begin; p < end; ++p) {
        if (std::isspace((unsigned char)*p)) continue;
        r.push_back((char)std::tolower((unsigned char)*p));
    }
    return r;
}

class line_parser {
public:
    line_parser(const std::string& text, int line)
        : text_(text), line_(line) {
        cur_ = text_.c_str();
        end_ = cur_ + text_.size();
    }

    asm_diag run() {
        asm_diag d;
        d.line = line_;

        skip_spaces();
        if (cur_ >= end_) return d;

        // 行首标号（可多个：foo: bar: mov ...）
        std::string label;
        while (true) {
            const char* save = cur_;
            std::string name;
            if (!read_ident(name)) break;
            skip_spaces();
            if (peek() != ':') { cur_ = save; break; }
            advance();
            label = label.empty() ? to_lower(name) : label + ":" + to_lower(name);
            skip_spaces();
            if (cur_ >= end_) break;
        }

        if (cur_ >= end_) {
            d.label = label;    // 纯标号行
            return d;
        }

        parse_instruction(d);
        d.label = label;
        return d;
    }

private:
    char peek() const { return cur_ < end_ ? *cur_ : '\0'; }
    void advance() { if (cur_ < end_) ++cur_; }

    void skip_spaces() {
        while (cur_ < end_ && std::isspace((unsigned char)*cur_)) ++cur_;
    }

    bool read_ident(std::string& out) {
        skip_spaces();
        if (cur_ >= end_ || !is_ident_start(*cur_)) return false;
        const char* start = cur_;
        while (cur_ < end_ && is_ident_char(*cur_)) ++cur_;
        out.assign(start, cur_);
        return true;
    }

    bool read_number_text(std::string& out) {
        skip_spaces();
        if (cur_ >= end_ || !std::isdigit((unsigned char)*cur_)) return false;
        const char* start = cur_;
        while (cur_ < end_ && (std::isxdigit((unsigned char)*cur_) ||
               *cur_ == 'x' || *cur_ == 'X' || *cur_ == 'h' ||
               *cur_ == 'b' || *cur_ == 'B'))
            ++cur_;
        out.assign(start, cur_);
        return true;
    }

    bool read_int_literal(int64_t& out) {
        skip_spaces();
        if (cur_ < end_ && *cur_ == '\'') {
            const char* start = cur_;
            ++cur_;
            if (cur_ < end_) ++cur_;
            if (cur_ < end_ && *cur_ == '\'') {
                ++cur_;
                return parse_int(std::string(start, cur_), out);
            }
            cur_ = start;
            return false;
        }
        std::string s;
        if (!read_number_text(s)) return false;
        return parse_int(s, out);
    }

    void parse_instruction(asm_diag& d) {
        std::string first;
        if (!read_ident(first)) {
            d.ok = false;
            d.message = "无法识别的指令: '" +
                        normalize_text(cur_, end_) + "'";
            return;
        }
        skip_spaces();

        // 指令前缀（rep/lock 等），支持 "rep movsb" 连写形式
        std::string mnemonic = first;
        if (instruction_prefixes().count(to_lower(first)) && cur_ < end_) {
            const char* save = cur_;
            std::string second;
            if (read_ident(second)) {
                mnemonic = first + " " + second;
                skip_spaces();
            } else {
                cur_ = save;
            }
        }

        // "rep movsb" 拆前缀后查表
        std::string low = to_lower(mnemonic);
        std::string base_low = low;
        size_t sp = base_low.find(' ');
        if (sp != std::string::npos) base_low = base_low.substr(sp + 1);
        if (!mnemonics().count(base_low)) {
            d.ok = false;
            d.message = "未知助记符 '" + mnemonic + "'";
            return;
        }
        d.mnemonic = low;

        if (cur_ >= end_) return;

        // 操作数列表（逗号分隔）
        while (cur_ < end_) {
            asm_operand op;
            const char* op_begin = cur_;
            if (!parse_operand(op)) {
                d.ok = false;
                d.message = "无法解析操作数: '" +
                            normalize_text(cur_, end_) + "'";
                d.operands.clear();
                return;
            }
            op.text = normalize_text(op_begin, cur_);
            d.operands.push_back(std::move(op));

            skip_spaces();
            if (peek() == ',') {
                advance();
                skip_spaces();
                if (cur_ >= end_) {
                    d.ok = false;
                    d.message = "逗号后缺少操作数";
                    return;
                }
            } else if (cur_ < end_) {
                d.ok = false;
                d.message = "预期 ','，但得到 '" + std::string(cur_, cur_ + 1) + "'";
                return;
            }
        }
    }

    bool parse_operand(asm_operand& op) {
        skip_spaces();

        // 尺寸说明符 byte/word/dword/qword/... [ptr]
        {
            const char* save = cur_;
            std::string w;
            if (read_ident(w)) {
                std::string wl = to_lower(w);
                static const std::unordered_map<std::string, int> k_sizes = {
                    {"byte", 8}, {"word", 16}, {"dword", 32}, {"qword", 64},
                    {"tbyte", 80}, {"oword", 128}, {"xmmword", 128},
                    {"ymmword", 256},
                };
                auto it = k_sizes.find(wl);
                if (it != k_sizes.end()) {
                    op.reg_size = it->second;   // 借用字段记录期望位宽
                    skip_spaces();
                    const char* save2 = cur_;
                    std::string p;
                    if (read_ident(p) && to_lower(p) == "ptr") {
                        skip_spaces();
                    } else {
                        cur_ = save2;
                    }
                } else {
                    cur_ = save;
                }
            }
        }

        // 段覆盖 seg:...
        std::string seg;
        {
            const char* save = cur_;
            std::string w;
            if (read_ident(w) && peek() == ':' && segment_names().count(to_lower(w))) {
                seg = to_lower(w);
                advance();
                skip_spaces();
            } else {
                cur_ = save;
            }
        }

        if (peek() == '[') {
            op.kind = operand_kind::memory;
            if (!seg.empty()) {
                op.seg_reg = seg;
            }
            if (!parse_memory(op)) return false;
            skip_spaces();
            return true;
        }

        std::string w;
        {
            const char* save = cur_;
            if (read_ident(w)) {
                std::string wl = to_lower(w);
                if (seg.empty() && is_register(wl)) {
                    op.kind     = operand_kind::reg;
                    op.reg_name = wl;
                    op.reg_size = register_size(wl);
                    return true;
                }
                op.kind   = seg.empty() ? operand_kind::label
                                        : operand_kind::far_label;
                op.symbol = wl;
                if (!seg.empty()) {
                    op.seg_reg = seg;
                }
                return true;
            }
            cur_ = save;
        }

        // 带符号立即数
        int sign = 1;
        {
            const char* save = cur_;
            skip_spaces();
            if (peek() == '-' || peek() == '+') {
                sign = (peek() == '-') ? -1 : 1;
                advance();
            } else {
                cur_ = save;
            }
        }
        int64_t v = 0;
        if (read_int_literal(v)) {
            op.kind  = operand_kind::imm;
            op.value = sign * v;
            return true;
        }
        return false;
    }

    bool parse_memory(asm_operand& op) {
        advance();               // '['
        skip_spaces();

        // 找配对的 ']'
        const char* expr_end = end_;
        const char* p = cur_;
        int depth = 0;
        while (p < end_) {
            if (*p == '[') {
                ++depth;
            } else if (*p == ']') {
                if (depth == 0) { expr_end = p; break; }
                --depth;
            }
            ++p;
        }
        if (p >= end_) return false;    // 未闭合

        int sign = 1;
        bool first_term = true;
        bool any_term = false;

        while (cur_ < expr_end) {
            skip_spaces();
            if (cur_ >= expr_end) break;
            char c = *cur_;

            if (c == '+' || c == '-') {
                sign = first_term ? (c == '-' ? -1 : 1)
                                  : (c == '-' ? -sign : sign);
                advance();
                skip_spaces();
                if (cur_ >= expr_end) return false;
                c = *cur_;
            }
            if (c == '*' || c == ',' || c == '[') return false;

            std::string ident;
            int64_t num = 0;
            if (read_ident(ident)) {
                std::string il = to_lower(ident);
                if (is_register(il)) {
                    int scale = 1;
                    if (skip_blank_and_star()) {
                        int64_t sc = 0;
                        if (!read_int_literal(sc)) return false;
                        if (sc != 1 && sc != 2 && sc != 4 && sc != 8) return false;
                        scale = (int)sc;
                    }
                    if (scale != 1 && op.has_index) return false;
                    if (scale == 1 && !op.has_base) {
                        op.base_reg = il;
                        op.has_base = true;
                    } else if (!op.has_index) {
                        op.index_reg = il;
                        op.scale     = scale;
                        op.has_index = true;
                    } else {
                        return false;    // 寄存器过多
                    }
                } else {
                    // 符号/标号引用（如 [module+0x10]），编码阶段再解析地址
                    op.symbols.push_back(il);
                    op.has_disp = true;
                }
            } else if (read_int_literal(num)) {
                op.disp += sign * num;
                op.has_disp = true;
            } else {
                return false;
            }

            any_term = true;
            first_term = false;
            sign = 1;
        }

        if (!any_term) return false;    // []
        advance();               // 消费 ']'
        return true;
    }

    bool skip_blank_and_star() {
        skip_spaces();
        if (peek() != '*') return false;
        advance();
        skip_spaces();
        return true;
    }

    const std::string& text_;
    int         line_;
    const char* cur_ = nullptr;
    const char* end_ = nullptr;
};

} // namespace

bool is_register(const std::string& name) {
    return reg_sizes().count(to_lower(name)) != 0;
}

int register_size(const std::string& name) {
    auto it = reg_sizes().find(to_lower(name));
    return it != reg_sizes().end() ? it->second : 0;
}

const std::vector<std::string>& mnemonic_list() {
    static std::vector<std::string> v = [] {
        std::vector<std::string> out(mnemonics().begin(), mnemonics().end());
        std::sort(out.begin(), out.end());
        return out;
    }();
    return v;
}

const std::vector<std::string>& register_list() {
    return reg_names();
}

asm_diag parse_line(const std::string& text, int line) {
    line_parser p(text, line);
    return p.run();
}

std::vector<asm_diag> parse_source(const std::string& source) {
    std::vector<asm_diag> out;
    int line_no = 0;
    size_t pos = 0;
    while (pos <= source.size()) {
        size_t eol = source.find('\n', pos);
        std::string raw = source.substr(
            pos, (eol == std::string::npos ? source.size() : eol) - pos);
        ++line_no;

        // 去注释：';' 或 "//" 到行尾
        size_t cut = raw.find(';');
        size_t cut2 = raw.find("//");
        if (cut2 != std::string::npos && (cut == std::string::npos || cut2 < cut))
            cut = cut2;
        if (cut != std::string::npos) raw.resize(cut);

        size_t b = raw.find_first_not_of(" \t\r");
        if (b != std::string::npos) {
            size_t e = raw.find_last_not_of(" \t\r");
            std::string line = raw.substr(b, e - b + 1);
            if (!is_section_marker(line)) {
                asm_diag d = parse_line(line, line_no);
                if (!d.label.empty() || !d.mnemonic.empty() || !d.ok)
                    out.push_back(std::move(d));
            }
        }

        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return out;
}

} // namespace asm_parse

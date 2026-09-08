#include "memory_window.h"

#include "core/process_manager.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

// 内存浏览器（x64dbg CPU 窗口风格）：
//   - 上侧：反汇编视图（disasm_view_address）
//   - 下侧：十六进制 dump TAB（dump_view_address）
// 视图/跳转地址由 main.cpp 通过 application_context::open_memory_viewer 信号更新，
// 见 ui_state::memory_view_mode / disasm_view_address / dump_view_address。

memory_window::memory_window(ui_state& ui)
    : state_(ui), assembler_window_(ui), hex_view_(ui) {}

namespace {

// ---- 跳转箭头 gutter 布局参数（宽度随字体缩放的部分在绘制时计算）----
constexpr float k_gutter_w  = 50.f;   // 箭头 gutter 列宽
constexpr float k_lane_w    = 6.f;    // 每层箭头的横向间距
constexpr int   k_max_slots = 5;      // 最多层级数（超出丢弃，防遮挡）
constexpr float k_head_w    = 5.f;    // 箭头三角形宽度

// 解析地址输入：默认按十六进制（CE 习惯），0x 前缀亦可。
bool parse_address_text(const char* s, uint64_t& out)
{
    if (!s || !*s)
        return false;
    char* end = nullptr;
    const bool has_0x = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'));
    const unsigned long long v = strtoull(s, &end, has_0x ? 0 : 16);
    if (end == s)
        return false;
    out = (uint64_t)v;
    return true;
}

// 依据主题背景亮度选择箭头配色，保证深/浅色主题下都清晰可辨
void arrow_palette(ImVec4& jcc, ImVec4& jmp, ImVec4& call)
{
    const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const bool light =
        (bg.x * 0.299f + bg.y * 0.587f + bg.z * 0.114f) > 0.5f;
    if (light) {
        jcc  = ImVec4(0.28f, 0.34f, 0.44f, 0.85f);   // 条件跳转：暗灰蓝（虚线）
        jmp  = ImVec4(0.80f, 0.42f, 0.02f, 0.90f);   // 无条件跳转：橙（实线）
        call = ImVec4(0.05f, 0.38f, 0.78f, 0.90f);   // 函数调用：蓝（实线）
    } else {
        jcc  = ImVec4(0.60f, 0.64f, 0.70f, 0.80f);
        jmp  = ImVec4(1.00f, 0.72f, 0.25f, 0.90f);
        call = ImVec4(0.35f, 0.65f, 1.00f, 0.90f);
    }
}

// 画一条线段（dashed=true 时手动分段模拟虚线，ImDrawList 无原生虚线）
void draw_seg(ImDrawList* dl, const ImVec2& a, const ImVec2& b,
              ImU32 col, float th, bool dashed)
{
    if (!dashed) {
        dl->AddLine(a, b, col, th);
        return;
    }
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.f)
        return;
    const float ux = dx / len, uy = dy / len;
    constexpr float k_dash = 4.f, k_gap = 3.f;
    for (float t = 0.f; t < len; t += k_dash + k_gap) {
        const float t2 = t + k_dash < len ? t + k_dash : len;
        dl->AddLine(ImVec2(a.x + ux * t, a.y + uy * t),
                    ImVec2(a.x + ux * t2, a.y + uy * t2), col, th);
        if (t2 >= len)
            break;
    }
}

// 工具栏里的图例色块 + 类型开关
void arrow_toggle(const char* label, const ImVec4& col, bool* v, bool is_sameline = true)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(p.x + 2.f, p.y + h * 0.15f),
        ImVec2(p.x + 11.f, p.y + h * 0.85f),
        ImGui::ColorConvertFloat4ToU32(col));
    ImGui::Dummy(ImVec2(13.f, 0.f));
    ImGui::SameLine(0.f, 1.f);
    ImGui::Checkbox(label, v);
    if(is_sameline) ImGui::SameLine(0.f, 4.f);
}

// 远程读 PE 头，解析主模块的 AddressOfEntryPoint（失败返回 0，调用方回退模块基址）。
uint64_t query_entry_point(IMemoryAccessor* mem, uint64_t base)
{
    if (!mem)
        return 0;
    uint8_t hdr[0x400];
    if (!mem->read(base, hdr, sizeof(hdr)))
        return 0;
    if (hdr[0] != 'M' || hdr[1] != 'Z')
        return 0;
    uint32_t e_lfanew = 0;
    std::memcpy(&e_lfanew, hdr + 0x3c, 4);
    if (e_lfanew < 0x40 || e_lfanew + 44 > sizeof(hdr))
        return 0;
    uint32_t sig = 0;
    std::memcpy(&sig, hdr + e_lfanew, 4);
    if (sig != 0x00004550)   // 'PE\0\0'
        return 0;
    // OptionalHeader 位于 e_lfanew+24（4 字节签名 + 20 字节 FileHeader），
    // AddressOfEntryPoint 是 OptionalHeader 的第 16 字节处（PE32/PE32+ 相同）。
    uint32_t aep = 0;
    std::memcpy(&aep, hdr + e_lfanew + 24 + 16, 4);
    return aep ? base + aep : 0;
}

// ============================================================================
// 反汇编指令行的"词法切分 + 着色"（类 x64dbg）：
//   不做 Zydis formatter token 集成，直接在已有指令文本上按标点切词并分类，
//   成本足够低（每帧仅对 ≤150 行做），行为可预期。
// ============================================================================

enum class wkind { w_mnem, w_reg, w_num, w_sym, w_text, w_punct, w_blank };

struct asm_word {
    std::string s;
    wkind       kind = wkind::w_text;
};

static bool is_asm_punct(char c)
{
    return c == '[' || c == ']' || c == '(' || c == ')' ||
           c == ',' || c == '+' || c == '-' || c == '*' || c == ':';
}

// x86 前缀指令词（lock/rep...）不是助记符，切词后据此跳过
static bool is_mnem_prefix(const std::string& w)
{
    static const std::set<std::string> pre = {
        "lock", "rep", "repe", "repz", "repne", "repnz"
    };
    return pre.count(w) != 0;
}

// 常用寄存器（Zydis 输出小写；含 64/32/16/8 位 GPR、段、标志、向量寄存器）
static const std::set<std::string>& reg_name_set()
{
    static const std::set<std::string> s = [] {
        std::set<std::string> r;
        const char* base[] = {
            // 8 位
            "al","ah","bl","bh","cl","ch","dl","dh",
            "spl","bpl","sil","dil",
            // 16 位
            "ax","bx","cx","dx","si","di","bp","sp","ip",
            // 32 位
            "eax","ebx","ecx","edx","esi","edi","ebp","esp","eip",
            // 64 位
            "rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp","rip",
            // 段 / 标志
            "es","cs","ss","ds","fs","gs","eflags","rflags",
        };
        for (const char* n : base) r.insert(n);
        for (int i = 0; i < 16; ++i) {
            char b[8];
            std::snprintf(b, sizeof(b), "r%db", i);  r.insert(b);
            std::snprintf(b, sizeof(b), "r%dw", i);  r.insert(b);
            std::snprintf(b, sizeof(b), "r%dd", i);  r.insert(b);
            std::snprintf(b, sizeof(b), "r%d",  i);  r.insert(b);
            std::snprintf(b, sizeof(b), "xmm%d", i); r.insert(b);
            std::snprintf(b, sizeof(b), "ymm%d", i); r.insert(b);
            std::snprintf(b, sizeof(b), "zmm%d", i); r.insert(b);
        }
        for (int i = 16; i < 32; ++i) {
            char b[8];
            std::snprintf(b, sizeof(b), "r%db", i);  r.insert(b);
            std::snprintf(b, sizeof(b), "r%dw", i);  r.insert(b);
            std::snprintf(b, sizeof(b), "r%dd", i);  r.insert(b);
            std::snprintf(b, sizeof(b), "r%d",  i);  r.insert(b);
            std::snprintf(b, sizeof(b), "xmm%d", i); r.insert(b);
            std::snprintf(b, sizeof(b), "ymm%d", i); r.insert(b);
            std::snprintf(b, sizeof(b), "zmm%d", i); r.insert(b);
        }
        return r;
    }();
    return s;
}

static std::vector<asm_word> split_asm_words(const std::string& t)
{
    std::vector<asm_word> out;
    if (t.empty())
        return out;
    const std::string low = [&] {
        std::string s = t;
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }();

    // 按原文切分：标点独立成词、**连续空白保留为独立 blank 词**（渲染只推进宽度，
    // 不画字），这样重排后的间距与原文逐字符一致（", " 不会挤成 ",,"）。
    // 分类用转小写后的文本。
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) {
            asm_word w;
            w.s = cur;
            std::string c = cur;
            std::transform(c.begin(), c.end(), c.begin(),
                           [](unsigned char ch) { return (char)std::tolower(ch); });
            if (reg_name_set().count(c))
                w.kind = wkind::w_reg;
            else if (std::isdigit((unsigned char)c[0]) ||
                     (c.size() > 1 && c[0] == '0' && (c[1] == 'x' || c[1] == 'X')) ||
                     (c.size() > 1 && c.back() == 'h' &&
                      std::isxdigit((unsigned char)c[c.size() - 2])))
                w.kind = wkind::w_num;
            else if (c == "ptr" || c == "byte" || c == "word" ||
                     c == "dword" || c == "qword" || c == "xmmword" ||
                     c == "ymmword" || c == "zmmword" || c == "short" ||
                     c == "near" || c == "far")
                w.kind = wkind::w_sym;
            out.push_back(std::move(w));
            cur.clear();
        }
    };

    for (size_t i = 0; i < t.size();) {
        const char ch = t[i];
        if (is_asm_punct(ch)) {
            flush();
            out.push_back(asm_word{ std::string(1, ch), wkind::w_punct });
            ++i;
        } else if (std::isspace((unsigned char)ch)) {
            flush();
            size_t j = i;
            while (j < t.size() && std::isspace((unsigned char)t[j]))
                ++j;
            out.push_back(asm_word{ t.substr(i, j - i), wkind::w_blank });
            i = j;
        } else {
            cur += ch;
            ++i;
        }
    }
    flush();
    return out;
}

// 助记符分类（决定整条指令/助记符颜色）
enum class mne_class { m_data, m_arith, m_jmp, m_jcc, m_call, m_ret, m_stack, m_other };

static mne_class classify_mnemonic(const std::string& m)
{
    static const std::set<std::string> data = {
        "mov","movzx","movsx","movsxd","movsb","movsw","movsd","movsq",
        "movaps","movapd","movups","movupd","movdqa","movdqu",
        "lea","xchg","bswap","movbe","cmov","cmove","cmovne","cmovg",
        "cmovge","cmovl","cmovle","cmova","cmovae","cmovb","cmovbe","cmovs","cmovns",
    };
    static const std::set<std::string> arith = {
        "add","sub","adc","sbb","inc","dec","neg","mul","imul","div","idiv",
        "and","or","xor","not","shl","shr","sar","rol","ror","rcl","rcr",
        "test","cmp","cmpsb","scasb",
        "sete","setne","setg","setge","setl","setle","seta","setae","setb","setbe","sets","setns",
        "bt","bts","btr","btc","bsf","bsr","popcnt","lzcnt","tzcnt",
        "addss","addsd","subss","subsd","mulss","mulsd","divss","divsd",
        "sqrtss","sqrtsd","xorps","andps","orps","maxss","minsd","roundsd",
        "ucomiss","ucomisd","comiss","comisd","cvtsi2ss","cvtsi2sd","cvttss2si","cvttsd2si",
    };
    static const std::set<std::string> jcc_all = {
        "jo","jno","jb","jnae","jc","jnb","jae","jnc","jz","je","jnz","jne",
        "jbe","jna","jnbe","ja","js","jns","jp","jpe","jnp","jpo","jl","jnge",
        "jge","jnl","jle","jng","jg","jnle","jcxz","jecxz","jrcxz",
    };
    static const std::set<std::string> stack = {
        "push","pop","pusha","popa","pushad","popad","pushf","popf","pushfd","popfd",
        "pushfq","popfq","enter","leave","call",
    };
    // 前缀后的真实助记符在 m 里（m 可能形如 "lock add"）
    std::string real = m;
    const size_t sp = real.find(' ');
    if (sp != std::string::npos)
        real = real.substr(sp + 1);
    if (real == "call")  return mne_class::m_call;
    if (real == "ret" || real == "retn" || real == "iret" || real == "iretq")
        return mne_class::m_ret;
    if (real == "jmp" || real == "ljmp") return mne_class::m_jmp;
    if (jcc_all.count(real)) return mne_class::m_jcc;
    if (stack.count(real))   return mne_class::m_stack;
    if (arith.count(real))   return mne_class::m_arith;
    if (data.count(real))    return mne_class::m_data;
    return mne_class::m_other;
}

// 依据背景亮度选择深/浅两套指令配色（延续 arrow_palette 的做法）
static bool dark_ui()
{
    const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    return (bg.x * 0.299f + bg.y * 0.587f + bg.z * 0.114f) <= 0.5f;
}

static ImVec4 text_color_for(wkind kind, mne_class mc, bool dark)
{
    switch (kind) {
    case wkind::w_reg:
        return dark ? ImVec4(0.53f, 0.85f, 0.56f, 1.f)
                    : ImVec4(0.05f, 0.55f, 0.25f, 1.f);   // 寄存器：绿
    case wkind::w_num:
        return dark ? ImVec4(0.92f, 0.75f, 0.42f, 1.f)
                    : ImVec4(0.70f, 0.45f, 0.00f, 1.f);   // 立即数/位移：黄
    case wkind::w_punct:
        return dark ? ImVec4(0.48f, 0.48f, 0.52f, 1.f)
                    : ImVec4(0.45f, 0.45f, 0.45f, 1.f);   // 分隔符：灰
    case wkind::w_sym:
        return dark ? ImVec4(0.55f, 0.58f, 0.66f, 1.f)
                    : ImVec4(0.40f, 0.42f, 0.50f, 1.f);   // ptr/qword：暗灰蓝
    case wkind::w_text:
        return dark ? ImVec4(0.62f, 0.65f, 0.72f, 1.f)
                    : ImVec4(0.30f, 0.32f, 0.38f, 1.f);   // 其它词
    case wkind::w_mnem:
        switch (mc) {
        case mne_class::m_data:  return dark ? ImVec4(0.40f, 0.62f, 0.95f, 1.f) : ImVec4(0.08f, 0.35f, 0.80f, 1.f); // mov 类：蓝
        case mne_class::m_arith: return dark ? ImVec4(0.78f, 0.48f, 0.90f, 1.f) : ImVec4(0.55f, 0.15f, 0.72f, 1.f); // 算术逻辑：紫
        case mne_class::m_jmp:   return dark ? ImVec4(1.00f, 0.72f, 0.25f, 1.f) : ImVec4(0.80f, 0.42f, 0.02f, 1.f); // jmp：橙
        case mne_class::m_jcc:   return dark ? ImVec4(0.85f, 0.62f, 0.35f, 1.f) : ImVec4(0.68f, 0.36f, 0.05f, 1.f); // 条件跳转：暗橙
        case mne_class::m_call:  return dark ? ImVec4(0.35f, 0.68f, 1.00f, 1.f) : ImVec4(0.05f, 0.40f, 0.85f, 1.f); // call：青蓝
        case mne_class::m_ret:   return dark ? ImVec4(1.00f, 0.47f, 0.47f, 1.f) : ImVec4(0.80f, 0.10f, 0.10f, 1.f); // ret：红
        case mne_class::m_stack: return dark ? ImVec4(0.62f, 0.76f, 0.82f, 1.f) : ImVec4(0.15f, 0.45f, 0.55f, 1.f); // push/pop：青灰
        default:                 return dark ? ImVec4(0.90f, 0.90f, 0.90f, 1.f) : ImVec4(0.15f, 0.15f, 0.15f, 1.f);
        }
    case wkind::w_blank:         // 空白词不绘制文字，不设色（防御）
        return ImVec4(1.f, 1.f, 1.f, 1.f);
    }
    return ImVec4(1.f, 1.f, 1.f, 1.f);
}

} // namespace

// 附加进程后自动定位（CE 行为：打开 Memory Viewer 直接看到目标代码）：
//   反汇编视图 → 主模块入口点；hex dump → 主模块基址。
// pid 变化时触发一次（含换进程）；脱离后重新附加会再次定位。
void memory_window::auto_navigate_on_attach()
{
    auto& pm = process_manager::instance();
    const uint32_t pid = pm.attached_pid();
    if (pid == navigated_pid_)
        return;
    navigated_pid_ = pid;
    if (pid == 0)
        return;

    // 主模块 = 模块列表第一项（Toolhelp32 快照顺序，exe 在最前）
    uint64_t base = 0;
    const auto mods = pm.modules().enumerate(pid);
    if (!mods.empty())
        base = mods.front().base;
    if (!base)
        return;

    const uint64_t entry = query_entry_point(pm.memory(), base);
    disasm_jump_to(entry ? entry : base);
    state_.dump_view_address = base;
}

std::string memory_window::disasm_comment_for(const disasm_line& ln) const
{
    // 分支行：CE/x64dbg 风格的 sub_ 函数标签
    if (ln.is_branch && ln.branch_target) {
        char sub[40];
        snprintf(sub, sizeof(sub), "sub_%llX",
                 (unsigned long long)ln.branch_target);
        return sub;
    }
    // 普通行（可选）：显示自身 模块+偏移
    if (disasm_show_symbols_) {
        std::string disp;
        bool is_base = false;
        if (process_manager::instance().resolve_address(ln.address, disp, is_base))
            return disp;
    }
    return {};
}

void memory_window::render_disasm_toolbar()
{
    ImGui::Checkbox("Symbols", &disasm_show_symbols_);

    ImGui::SameLine();
    // 跳转回退（CE 的 Back 菜单项）：仅当有历史时可用。
    // 注意：必须先用局部变量固定 has_back，再无条件配对 Begin/EndDisabled，
    // 否则点击 Back 后 has_back 在同一帧内翻转（1→0），会导致 EndDisabled 多调一次。
    const bool has_back = disasm_has_back();
    ImGui::BeginDisabled(!has_back);
    if (ImGui::Button("Back")) {
        disasm_go_back();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(190);
    if (ImGui::InputTextWithHint("##disasm_goto", "Goto address (hex), Enter",
                                 disasm_goto_buf_, sizeof(disasm_goto_buf_),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        uint64_t a = 0;
        if (parse_address_text(disasm_goto_buf_, a))
            disasm_jump_to(a);
    }

    // 跳转箭头类型开关（带颜色图例）
    ImVec4 c_jcc, c_jmp, c_call;
    arrow_palette(c_jcc, c_jmp, c_call);
    arrow_toggle("Jcc", c_jcc, &disasm_show_jcc_);
    arrow_toggle("Jmp", c_jmp, &disasm_show_jmp_);
    arrow_toggle("Call", c_call, &disasm_show_call_, false);
}

// 把分支指令文本中的裸目标地址（Zydis 输出的 0x+十六进制，兼容大小写/补零）
// 替换为解析出的标签，如 <Trae CN.exe+0x41cf480>；找不到保持原样。
static void replace_branch_label(std::string& text, uint64_t target,
                                 const std::string& label)
{
    char raw[32];
    const unsigned long long t = (unsigned long long)target;
    snprintf(raw, sizeof(raw), "0x%016llX", t);          // 大写补零（默认）
    size_t pos = text.find(raw);
    if (pos == std::string::npos) {
        snprintf(raw, sizeof(raw), "0x%016llx", t);      // 小写补零
        pos = text.find(raw);
    }
    if (pos == std::string::npos) {
        snprintf(raw, sizeof(raw), "0x%llX", t);         // 大写不补零
        pos = text.find(raw);
    }
    if (pos == std::string::npos) {
        snprintf(raw, sizeof(raw), "0x%llx", t);         // 小写不补零
        pos = text.find(raw);
    }
    if (pos != std::string::npos)
        text.replace(pos, strlen(raw), label);
}

// 向后定位：返回 base 之前第 lines 条指令的起始地址。
// x86 变长指令无法精确反解，启发式：向前多读一段顺序解码，
// 取严格位于 base 之前的倒数第 lines 个指令起点；失败返回 base（不扩展）。
static uint64_t backward_top_address(disassembler& d, uint64_t base, int lines)
{
    auto* mem = process_manager::instance().memory();
    if (!mem || lines <= 0)
        return base;
    const int back_bytes = lines * 15 + 16;
    const uint64_t start = base - (uint64_t)back_bytes;
    if (start >= base)
        return base;   // 下溢保护
    const std::vector<uint64_t> starts = d.decode_starts(start, lines + 8, back_bytes);
    std::vector<uint64_t> before;
    for (uint64_t a : starts) {
        if (a >= base)
            break;
        before.push_back(a);
    }
    if ((int)before.size() >= lines)
        return before[before.size() - lines];
    return base;
}

void memory_window::disasm_jump_to(uint64_t addr)
{
    // 显式跳转前把当前地址压入回退栈（CE 的 backlist.Push）。
    // going_back 或地址未变时不压栈，避免回退操作本身再次入栈 / 重复入栈。
    if (!disasm_going_back_ &&
        state_.disasm_view_address != 0 &&
        state_.disasm_view_address != addr) {
        disasm_back_stack_.push_back(state_.disasm_view_address);
        if (disasm_back_stack_.size() > 512)
            disasm_back_stack_.erase(disasm_back_stack_.begin());
    }

    state_.disasm_view_address = addr;
    disasm_base_ = addr;
    disasm_selected_ = addr;
    disasm_base_history_.clear();
    disasm_scroll_top_ = true;
}

void memory_window::disasm_go_back()
{
    if (disasm_back_stack_.empty())
        return;
    const uint64_t addr = disasm_back_stack_.back();
    disasm_back_stack_.pop_back();
    disasm_going_back_ = true;
    disasm_jump_to(addr);
    disasm_going_back_ = false;
}

void memory_window::render_disassembly_view() {
    auto& pm = process_manager::instance();
    const process_arch arch = pm.is_attached() ? pm.memory()->architecture()
                                               : process_arch::unknown;

    // 架构变化时（32↔64 位切换 / 未附加）重建 decoder。
    disasm_.set_arch(arch);

    ImGui::BeginChild("##disasm_view", ImVec2(0, 500.f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);

    render_disasm_toolbar();

    // H 键切换"高亮模式"（点击指令/寄存器打色块，再点取消）。
    // 有文本输入框被激活时（goto 框等）不抢 H 按键。
    if (ImGui::IsKeyPressed(ImGuiKey_H) && !ImGui::GetIO().WantTextInput &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        disasm_hl_mode_ = !disasm_hl_mode_;
    }
    // 模式状态提示（色块图例样式与下方一致）
    {
        const ImVec2 hp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(hp.x + 2.f, hp.y + 2.f),
            ImVec2(hp.x + 11.f, hp.y + ImGui::GetTextLineHeight() - 1.f),
            disasm_hl_mode_
                ? ImGui::ColorConvertFloat4ToU32(ImVec4(1.f, 0.72f, 0.25f, 0.9f))
                : ImGui::ColorConvertFloat4ToU32(ImVec4(0.45f, 0.45f, 0.45f, 0.6f)));
        ImGui::Dummy(ImVec2(13.f, 0.f));
        ImGui::SameLine(0.f, 1.f);
        ImGui::TextUnformatted(disasm_hl_mode_ ? "H: click instr/reg to highlight  (Esc off)"
                                               : "H: highlight mode  (H to enter)");
        if (disasm_hl_mode_ && ImGui::IsKeyPressed(ImGuiKey_Escape))
            disasm_hl_mode_ = false;
        ImGui::SameLine(0.f, 24.f);
        if (ImGui::Button("Clear HL")) {
            hl_instructions_.clear();
            hl_reg_words_.clear();
        }
    }

    // 行高与可视行数（滑动窗口 = 可视行数的 3 倍，滚动到边缘整体重锚）
    const float text_h = ImGui::GetTextLineHeight();
    const float row_h = text_h + ImGui::GetStyle().CellPadding.y * 2.f;
    float avail_h = ImGui::GetContentRegionAvail().y;
    if (avail_h < row_h * 6.f)
        avail_h = row_h * 6.f;
    const int visible_rows = (int)(avail_h / row_h) < 6
                                 ? 6 : (int)(avail_h / row_h);
    int page_lines = visible_rows * 3;
    if (page_lines < 60) page_lines = 60;
    if (page_lines > 150) page_lines = 150;

    // 仅在解码基址/架构/行数变化时才重新反汇编，避免每帧解码整屏。
    if (arch != process_arch::unknown &&
        (disasm_base_ != disasm_cached_base_ ||
         arch != disasm_cached_arch_ ||
         page_lines != disasm_cached_count_)) {
        disasm_lines_ = disasm_.disassemble(disasm_base_, page_lines);
        // 把指令中的绝对地址格式化为 <模块+偏移>：
        //   1) 分支指令（jcc/jmp/call）的目标地址——由 disassembler 填入 branch_target
        //   2) 一般指令的纯绝对寻址 disp（如 mov rdx, [imm64]）——由 disassembler
        //      填入 mem_abs_addrs，仅 base/index=NONE 的 memory 操作数参与
        // 地址不在任何模块范围内时 resolve_address 返回 false，保持 Zydis 输出的
        // 原始绝对地址文本不变。
        for (auto& ln : disasm_lines_) {
            if (ln.is_branch && ln.branch_target) {
                std::string disp;
                bool is_base = false;
                if (process_manager::instance().resolve_address(
                        ln.branch_target, disp, is_base))
                    replace_branch_label(ln.text, ln.branch_target,
                                         "<" + disp + ">");
            }
            for (uint64_t addr : ln.mem_abs_addrs) {
                std::string disp;
                bool is_base = false;
                if (process_manager::instance().resolve_address(
                        addr, disp, is_base))
                    replace_branch_label(ln.text, addr, "<" + disp + ">");
            }
        }
        disasm_cached_base_ = disasm_base_;
        disasm_cached_arch_ = arch;
        disasm_cached_count_ = page_lines;
    }

    if (ImGui::BeginTable("##Disassembly", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable,
                          ImVec2(0.f, avail_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        // 第 0 列：跳转箭头 gutter（固定宽、不可调整）
        ImGui::TableSetupColumn("##gutter", ImGuiTableColumnFlags_WidthFixed |
                                                ImGuiTableColumnFlags_NoResize,
                                k_gutter_w);
        // 比例字体下按最宽内容样例定宽，保证地址/字节列完整显示；
        // 指令与注释列平分剩余宽度
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("FFFFFFFFFFFFFFFF").x + 12.f);
        ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("00 00 00 00 00 00 00 00 00 00 00 00 00 00 00").x + 10.f);
        ImGui::TableSetupColumn("Instruction", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();

        if (arch == process_arch::unknown) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("No process attached.");
        } else if (disasm_lines_.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("Unable to disassemble @ %016llX",
                                (unsigned long long)state_.disasm_view_address);
        } else {
            std::vector<float> row_tops;
            row_tops.reserve(disasm_lines_.size());
            float gutter_x0 = 0.f;

            for (size_t li = 0; li < disasm_lines_.size(); ++li) {
                const disasm_line& ln = disasm_lines_[li];
                ImGui::TableNextRow();

                // ---- 第 0 列：箭头 gutter（记录每行屏幕坐标，行循环后统一绘制）----
                ImGui::TableSetColumnIndex(0);
                const ImVec2 gp = ImGui::GetCursorScreenPos();
                gutter_x0 = gp.x;
                row_tops.push_back(gp.y);

                // ---- 地址列：行选择 + 右键菜单（双击跟随/编辑注释在各自列内处理）----
                ImGui::TableSetColumnIndex(1);
                char abuf[24];
                snprintf(abuf, sizeof(abuf), "%016llX",
                         (unsigned long long)ln.address);
                // 右键菜单等控件必须按行限定 ID 作用域，否则可视行共用同一个
                // "##disasm_line" + "Follow"/"Copy address" 标签，触发 ImGui 的
                // "visible items with conflicting ID" 冲突告警。用行地址低位作唯一 ID。
                ImGui::PushID((int)(ln.address & 0xffffffff));
                if (ImGui::Selectable(abuf, disasm_selected_ == ln.address,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    disasm_selected_ = ln.address;
                }
                if (ImGui::BeginPopupContextItem("##disasm_line")) {
                    if (ln.is_branch && ln.branch_target &&
                        ImGui::MenuItem("Follow")) {
                        disasm_jump_to(ln.branch_target);
                    }
                    if (ImGui::MenuItem("Follow in dump")) {
                        state_.dump_view_address = ln.address;
                    }
                    if (ImGui::MenuItem("Copy address")) {
                        ImGui::SetClipboardText(abuf);
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();

                // ---- 字节列 ----
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(ln.bytes.c_str());

                // ---- 指令列：token 级着色（x64dbg 风格）+ H 高亮点击 + 双击跟随 ----
                ImGui::TableSetColumnIndex(3);
                render_instr_tokens(ln.address, ln.text,
                                    ln.is_branch, ln.branch_target);

                // ---- 注释列：用户注释优先，否则自动标签；双击进编辑 ----
                ImGui::TableSetColumnIndex(4);
                render_comment_cell(ln.address, ln);
            }

            // 行布局完成后统一绘制箭头层（覆盖在 gutter 之上）
            draw_jump_arrows(gutter_x0, row_tops, text_h);
        }

        // ---- 滑动窗口重锚：到底前进一页，到顶（滚轮向上）回退一页。
        // 必须在 EndTable 之前调用——此时 Get/SetScrollY 作用于表格内部滚动区。
        const float sy = ImGui::GetScrollY();
        const float smax = ImGui::GetScrollMaxY();
        const float page_h = (float)visible_rows * row_h;
        const float wheel = ImGui::GetIO().MouseWheel;

        if (disasm_scroll_top_) {
            ImGui::SetScrollY(0.f);
            disasm_scroll_top_ = false;
        } else if (smax > 0.f && sy >= smax - 0.5f &&
                   (int)disasm_lines_.size() > visible_rows) {
            // 到底：旧窗口基址入历史，基址前进一页
            disasm_base_history_.push_back(disasm_base_);
            if (disasm_base_history_.size() > 512)
                disasm_base_history_.erase(disasm_base_history_.begin());
            disasm_base_ = disasm_lines_[visible_rows].address;
            ImGui::SetScrollY(sy - page_h);
        } else if (smax > 0.f && sy <= 0.5f &&
                   ImGui::IsWindowHovered() && wheel > 0.f) {
            // 到顶：优先回退前进历史，跳转后无历史则向后解码定位
            uint64_t new_base = 0;
            if (!disasm_base_history_.empty() &&
                disasm_base_history_.back() < disasm_base_) {
                new_base = disasm_base_history_.back();
                disasm_base_history_.pop_back();
            }
            if (!new_base)
                new_base = backward_top_address(disasm_, disasm_base_, visible_rows);
            if (new_base < disasm_base_) {
                disasm_base_ = new_base;
                ImGui::SetScrollY(page_h);
            }
        }

        // ---- 空格键：选中行智能跟随（正在输入文本/编辑注释时忽略）----
        if (ImGui::IsKeyPressed(ImGuiKey_Space) &&
            !ImGui::GetIO().WantTextInput && comment_edit_addr_ == 0) {
            for (const auto& ln : disasm_lines_) {
                if (ln.address == disasm_selected_) {
                    if (ln.is_branch && ln.branch_target)
                        disasm_jump_to(ln.branch_target);
                    else
                        state_.dump_view_address = ln.address;   // 普通行：hex dump 定位
                    break;
                }
            }
        }

        ImGui::EndTable();
    }

    ImGui::EndChild();
}

// ---- 跳转箭头层（x64dbg 风格 gutter）----
// 数据来自 disasm_lines_（解码时已带 branch 目标），布局来自 row_tops（每行屏幕 y）。
// 层级分配：按跨度升序贪心占槽——短箭头贴近代码列，被跨越的长箭头（外层循环）
// 依次向左排开；仅在两端都可见时画完整箭头，目标不可见时画方向短箭头。
void memory_window::draw_jump_arrows(float gutter_x0,
                                     const std::vector<float>& row_tops,
                                     float text_h)
{
    if (disasm_lines_.empty() || row_tops.size() != disasm_lines_.size())
        return;
    if (!disasm_show_jcc_ && !disasm_show_jmp_ && !disasm_show_call_)
        return;

    const int n = (int)disasm_lines_.size();

    // 地址 -> 行索引（行地址严格递增，二分查找）
    auto find_idx = [&](uint64_t addr) -> int {
        int lo = 0, hi = n - 1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            if (disasm_lines_[mid].address == addr) return mid;
            if (disasm_lines_[mid].address < addr) lo = mid + 1;
            else hi = mid - 1;
        }
        return -1;
    };

    // ---- 收集箭头 ----
    struct jarrow {
        int      src = -1, dst = -1;
        int      slot = 0;
        bool     cond = false, call = false;
        bool     forward = true;   // 目标在下方
        bool     stub = false;     // 目标不在视图内 → 只画方向短箭头
        bool     assigned = false;
        bool     skip = false;     // 层级用尽，丢弃
        uint64_t from = 0, to = 0;
    };
    std::vector<jarrow> arrows;
    arrows.reserve(16);

    for (int i = 0; i < n; ++i) {
        const disasm_line& ln = disasm_lines_[i];
        if (!ln.is_branch || ln.is_ret || !ln.branch_target ||
            ln.branch_target == ln.address)
            continue;

        jarrow a;
        a.src = i;
        a.from = ln.address;
        a.to = ln.branch_target;
        a.cond = ln.is_cond;
        a.call = ln.is_call;

        // 类型开关过滤
        if (a.call) { if (!disasm_show_call_) continue; }
        else if (a.cond) { if (!disasm_show_jcc_) continue; }
        else { if (!disasm_show_jmp_) continue; }

        a.dst = find_idx(a.to);
        a.stub = (a.dst < 0);
        a.forward = a.stub ? (a.to > a.from) : (a.dst > a.src);
        arrows.push_back(a);
    }
    if (arrows.empty())
        return;

    // ---- 层级分配：区间图贪心着色，重叠（含共享端点）的箭头占不同槽位 ----
    std::vector<int> order;
    order.reserve(arrows.size());
    for (int i = 0; i < (int)arrows.size(); ++i)
        if (!arrows[i].stub)
            order.push_back(i);
    std::sort(order.begin(), order.end(), [&](int x, int y) {
        const int sx = arrows[x].dst > arrows[x].src
                           ? arrows[x].dst - arrows[x].src
                           : arrows[x].src - arrows[x].dst;
        const int sy = arrows[y].dst > arrows[y].src
                           ? arrows[y].dst - arrows[y].src
                           : arrows[y].src - arrows[y].dst;
        return sx != sy ? sx < sy : x < y;
    });

    auto overlaps = [](const jarrow& a, const jarrow& b) {
        const int a1 = ImMin(a.src, a.dst), a2 = ImMax(a.src, a.dst);
        const int b1 = ImMin(b.src, b.dst), b2 = ImMax(b.src, b.dst);
        return a1 <= b2 && b1 <= a2;
    };

    for (int idx : order) {
        bool used[k_max_slots] = {};
        for (const jarrow& o : arrows)
            if (o.assigned && overlaps(arrows[idx], o))
                used[o.slot] = true;
        int s = 0;
        while (s < k_max_slots && used[s])
            ++s;
        if (s >= k_max_slots)
            arrows[idx].skip = true;   // 层级用尽，丢弃防遮挡
        else {
            arrows[idx].slot = s;
            arrows[idx].assigned = true;
        }
    }

    // ---- 绘制 ----
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 c_jcc, c_jmp, c_call;
    arrow_palette(c_jcc, c_jmp, c_call);

    const float x_tip  = gutter_x0 + k_gutter_w - 8.f;   // 箭头尖端（贴近地址列）
    const float x_base = x_tip + k_head_w;               // 三角底边
    const ImVec2 mouse = ImGui::GetMousePos();

    auto near_h = [&](float x1, float x2, float y) {
        return mouse.y > y - 3.f && mouse.y < y + 3.f &&
               mouse.x > ImMin(x1, x2) - 2.f && mouse.x < ImMax(x1, x2) + 2.f;
    };
    auto near_v = [&](float x, float y1, float y2) {
        return mouse.x > x - 3.f && mouse.x < x + 3.f &&
               mouse.y > ImMin(y1, y2) - 2.f && mouse.y < ImMax(y1, y2) + 2.f;
    };

    int hovered = -1;

    for (int ai = 0; ai < (int)arrows.size(); ++ai) {
        const jarrow& a = arrows[ai];
        if (a.skip || a.src >= (int)row_tops.size())
            continue;
        if (!a.stub && (a.dst < 0 || a.dst >= (int)row_tops.size()))
            continue;

        const float ys = row_tops[a.src] + text_h * 0.5f;
        const float lane_x = x_tip - 4.f - (float)(a.slot + 1) * k_lane_w;

        // 颜色深浅：跳转距离越远越淡；选中行相关箭头加重加粗
        const ImVec4* base = a.call ? &c_call : (a.cond ? &c_jcc : &c_jmp);
        float alpha = base->w;
        if (!a.stub) {
            const float span = (float)(a.dst > a.src ? a.dst - a.src
                                                     : a.src - a.dst);
            alpha *= 1.0f - 0.45f * ImMin(span / (float)n, 1.0f);
        }
        const bool related =
            (disasm_lines_[a.src].address == disasm_selected_) ||
            (!a.stub && disasm_lines_[a.dst].address == disasm_selected_);
        float th = related ? 2.0f : 1.0f;

        // 悬停命中测试
        bool hov = near_h(lane_x, x_base, ys);
        if (!hov && !a.stub) {
            const float yd = row_tops[a.dst] + text_h * 0.5f;
            hov = near_v(lane_x, ys, yd) || near_h(lane_x, x_base, yd);
        } else if (!hov && a.stub) {
            const float yend = ys + (a.forward ? 8.f : -8.f);
            hov = near_v(lane_x, ys, yend);
        }
        if (hov) {
            alpha = 1.0f;
            th = 2.0f;
            hovered = ai;
        }

        const ImU32 col = ImGui::ColorConvertFloat4ToU32(
            ImVec4(base->x, base->y, base->z, alpha));
        const bool dashed = a.cond && !a.call;   // 条件跳转虚线，其余实线

        if (!a.stub) {
            const float yd = row_tops[a.dst] + text_h * 0.5f;
            draw_seg(dl, ImVec2(lane_x, ys), ImVec2(x_base, ys), col, th, dashed);
            if (fabsf(yd - ys) > 0.5f)
                draw_seg(dl, ImVec2(lane_x, ys), ImVec2(lane_x, yd), col, th, dashed);
            draw_seg(dl, ImVec2(lane_x, yd), ImVec2(x_base, yd), col, th, dashed);
            // 目标端箭头（指向代码列）
            dl->AddTriangleFilled(ImVec2(x_base, yd), ImVec2(x_tip, yd - 3.2f),
                                  ImVec2(x_tip, yd + 3.2f), col);
        } else {
            // 目标不在视图内：源横线 + 短方向箭头
            const float yend = ys + (a.forward ? 8.f : -8.f);
            draw_seg(dl, ImVec2(lane_x, ys), ImVec2(x_base, ys), col, th, dashed);
            draw_seg(dl, ImVec2(lane_x, ys), ImVec2(lane_x, yend), col, th, dashed);
            const float tip_y = yend + (a.forward ? 3.f : -3.f);
            dl->AddTriangleFilled(ImVec2(lane_x, tip_y),
                                  ImVec2(lane_x - 3.f, yend),
                                  ImVec2(lane_x + 3.f, yend), col);
        }
    }

    // ---- 悬停提示 + 点击跳转 ----
    if (hovered >= 0) {
        const jarrow& a = arrows[hovered];
        const char* tname = a.call ? "CALL (函数调用)"
                          : a.cond ? "JCC (条件跳转)"
                                   : "JMP (无条件跳转)";
        char tip[192];
        snprintf(tip, sizeof(tip),
                 "%s\n%016llX  ->  %016llX\n距离 %lld 字节\n点击箭头跳转到目标",
                 tname, (unsigned long long)a.from, (unsigned long long)a.to,
                 (long long)(a.to - a.from));
        ImGui::SetTooltip("%s", tip);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            disasm_jump_to(a.to);
    }
}

void memory_window::render() {
    // 附加后自动定位到主模块入口点 / 基址（CE 行为）
    auto_navigate_on_attach();

    if (state_.show_assembler_window)
        assembler_window_.render();

    if (!ImGui::Begin("Memory Viewer", &state_.show_memory_window, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Tools")) {
            if (ImGui::MenuItem("Inject")) {
                // TODO
            }
            if (ImGui::MenuItem("Alloc Memory")) {
                // TODO
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Auto assembly")) {
                state_.show_assembler_window = true;
            }
            ImGui::EndMenu();
        }

        if(ImGui::BeginMenu("View Map")){
            if(ImGui::MenuItem("Module View")){

            }

            if(ImGui::MenuItem("PE Analyzer")){

            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }



    // 上侧：反汇编视图
    render_disassembly_view();

    // 下侧：十六进制 dump TAB（多个 TAB 共享同一个视图状态，对应 x64dbg 的多 dump 页）
    if (ImGui::BeginTabBar("##dump_tabs")) {
        for (int i = 1; i <= 5; ++i) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Hex Dump %d", i);
            if (ImGui::BeginTabItem(buf)) {
                hex_view_.render();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

// ============================================================================
// 指令列：token 级着色绘制 + H 高亮模式点击 + 指令行双击跟随
// ============================================================================
void memory_window::render_instr_tokens(uint64_t addr, const std::string& text,
                                        bool is_branch, uint64_t branch_target)
{
    if (text.empty())
        return;
    const std::vector<asm_word> words = split_asm_words(text);
    if (words.empty())
        return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const float line_h = ImGui::GetTextLineHeight();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool dark = dark_ui();

    // 助记符 = 第一个非前缀/非空白/非标点词
    mne_class mc = mne_class::m_other;
    size_t mn_idx = words.size();   // 助记符所在词下标
    for (size_t i = 0; i < words.size(); ++i) {
        const asm_word& w = words[i];
        if (w.kind == wkind::w_punct || w.kind == wkind::w_blank)
            continue;
        if (is_mnem_prefix(w.s)) {
            continue;
        }
        // 行首字母词即助记符（无前缀时即首个词）
        mc = classify_mnemonic(w.s);
        mn_idx = i;
        break;
    }

    // 整条指令 H 高亮：助记符起画背景块，覆盖到最后一个词
    if (hl_instructions_.count(addr)) {
        ImVec2 mins(FLT_MAX, FLT_MAX), maxs(-FLT_MAX, -FLT_MAX);
        float x = p0.x;
        for (const auto& w : words) {
            const ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, w.s.c_str());
            mins.x = (std::min)(mins.x, x);
            mins.y = p0.y;
            maxs.x = (std::max)(maxs.x, x + sz.x);
            maxs.y = p0.y + line_h;
            x += sz.x;
        }
        const ImU32 hl_col = dark ? IM_COL32(30, 80, 160, 90)
                                  : IM_COL32(40, 100, 220, 60);
        dl->AddRectFilled(mins, maxs, hl_col, 2.f);
    }

    // 逐词绘制（drawlist 文本，不占用 item 高度；行高由地址列 Selectable 提供）
    float x = p0.x;
    const float y = p0.y;
    bool hovered_any = false;
    bool click_any = false;
    bool dbl_click = false;
    const ImVec2 mouse = ImGui::GetMousePos();

    for (size_t i = 0; i < words.size(); ++i) {
        const asm_word& w = words[i];
        const ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, w.s.c_str());
        const ImRect r(x, y, x + sz.x, y + line_h);

        // 空白词：只推进 x（间距与原文一致），不绘制、不参与任何交互
        if (w.kind == wkind::w_blank) {
            x += sz.x;
            continue;
        }

        // 词类型（首词为助记符时使用分类色）
        const wkind wk = (i == mn_idx) ? wkind::w_mnem : w.kind;

        // ---- H 高亮模式点击：寄存器词单独高亮；助记符词 = 整条指令高亮 ----
        if (disasm_hl_mode_ && r.Contains(mouse) && !ImGui::GetIO().WantTextInput) {
            hovered_any = true;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                click_any = true;
                if (w.kind == wkind::w_reg) {
                    hl_word_key key{ addr, (uint32_t)i };
                    auto it = hl_reg_words_.find(key);
                    if (it != hl_reg_words_.end()) hl_reg_words_.erase(it);
                    else                          hl_reg_words_.insert(key);
                } else if (i == mn_idx) {
                    auto it = hl_instructions_.find(addr);
                    if (it != hl_instructions_.end()) hl_instructions_.erase(it);
                    else                              hl_instructions_.insert(addr);
                }
            }
        }

        // ---- 双击跟随（H 模式下不抢双击；分支指令在指令列双击 = 跳到目标）----
        if (!disasm_hl_mode_ && r.Contains(mouse) && !ImGui::GetIO().WantTextInput &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            dbl_click = true;
            if (is_branch && branch_target)
                disasm_jump_to(branch_target);
        }

        // ---- 寄存器词 H 高亮背景 ----
        const bool reg_hl = hl_reg_words_.count(hl_word_key{ addr, (uint32_t)i }) != 0;
        if (reg_hl) {
            const ImU32 bg = dark ? IM_COL32(45, 120, 60, 110)
                                  : IM_COL32(60, 160, 90, 70);
            dl->AddRectFilled(r.Min, r.Max, bg, 1.5f);
        }

        // 文字颜色
        ImU32 col;
        if (reg_hl) {
            col = dark ? IM_COL32(0x8f, 0xff, 0xa8, 255)
                       : IM_COL32(0x00, 0x60, 0x2a, 255);
        } else {
            const ImVec4 c = text_color_for(wk, mc, dark);
            col = ImGui::ColorConvertFloat4ToU32(c);
        }
        dl->AddText(font, fs, ImVec2(x, y), col, w.s.c_str());

        x += sz.x;
    }
    (void)hovered_any; (void)click_any; (void)dbl_click;
}

// ============================================================================
// 注释列：用户注释优先显示（亮黄色），否则自动标签；双击单元格进入编辑
// ============================================================================
void memory_window::render_comment_cell(uint64_t addr, const disasm_line& ln)
{
    const bool dark = dark_ui();

    // ---- 正在编辑本行注释：输入框 ----
    if (comment_edit_addr_ == addr) {
        if (comment_need_focus_) {
            ImGui::SetKeyboardFocusHere();
            comment_need_focus_ = false;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool enter = ImGui::InputText(
            "##comment_edit", comment_edit_buf_, sizeof(comment_edit_buf_),
            ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter) {                                   // Enter：保存
            disasm_comments_[addr] = comment_edit_buf_;
            comment_edit_addr_ = 0;
        }
        if (comment_edit_addr_ == addr &&
            ImGui::IsKeyPressed(ImGuiKey_Escape)) {    // Esc：取消
            comment_edit_addr_ = 0;
        }
        if (comment_edit_addr_ == addr &&
            ImGui::IsItemDeactivatedAfterEdit()) {     // 点击别处失焦：保存
            disasm_comments_[addr] = comment_edit_buf_;
            comment_edit_addr_ = 0;
        }
        return;
    }

    // ---- 显示文本：用户注释优先 ----
    const std::string comment = [&] {
        auto it = disasm_comments_.find(addr);
        if (it != disasm_comments_.end() && !it->second.empty())
            return it->second;
        return disasm_comment_for(ln);
    }();
    if (comment.empty())
        return;

    const ImVec2 p   = ImGui::GetCursorScreenPos();
    const float  lh  = ImGui::GetTextLineHeight();
    const bool   user_c = disasm_comments_.count(addr) != 0;

    ImVec4 col = user_c ? (dark ? ImVec4(0.95f, 0.80f, 0.42f, 1.f)
                                : ImVec4(0.60f, 0.42f, 0.00f, 1.f))
                        : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(comment.c_str());
    ImGui::PopStyleColor();

    // 双击进入编辑（命中单元格文本区）
    const ImVec2 sz = ImGui::CalcTextSize(comment.c_str());
    const ImRect cell(p.x, p.y, p.x + sz.x, p.y + lh);
    if (comment_edit_addr_ == 0 && !ImGui::GetIO().WantTextInput &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
        cell.Contains(ImGui::GetMousePos())) {
        comment_edit_addr_ = addr;
        comment_need_focus_ = true;
        std::memset(comment_edit_buf_, 0, sizeof(comment_edit_buf_));
        const auto it = disasm_comments_.find(addr);
        if (it != disasm_comments_.end())
            std::snprintf(comment_edit_buf_, sizeof(comment_edit_buf_), "%s",
                          it->second.c_str());
    }
}

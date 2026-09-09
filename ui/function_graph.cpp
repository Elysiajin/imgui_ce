#include "function_graph.h"

#include "core/process_manager.h"
#include "ui/symbol_table.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace {

// 构建上限（防御异常大"函数"把 UI 卡死）
constexpr int      kMaxInsn   = 8000;
constexpr int      kMaxBlocks = 1200;
constexpr uint64_t kRange     = 0x8000;   // 跳转目标仍视为函数内的地址窗口

// 布局常量
constexpr float kColGap = 84.f;   // 列间距（含前向边水平走廊）
constexpr float kRowGap = 56.f;   // 行间距
constexpr float kPadX   = 8.f;    // 块内边距
constexpr float kPadY   = 5.f;
constexpr float kTitlePadY = 4.f;
constexpr float kLaneH  = 11.f;   // 前向边水平走廊道间距
constexpr float kLaneW  = 13.f;   // 回边纵向通道间距
constexpr float kAnchorStep = 12.f;  // 同一边上多条出/入边的锚点间距

// 主题相关
static bool fg_dark_ui() {
    const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const float lum = bg.x * 0.299f + bg.y * 0.587f + bg.z * 0.114f;
    return lum < 0.5f;
}

} // namespace

// ════════════════════════════ 构建 ════════════════════════════

bool function_graph_builder::decode_next(uint64_t addr, disasm_line& out)
{
    // 顺序命中缓存则直接消费（一次 ReadProcessMemory 解码一整段）
    if (!cache_.empty() && addr == cache_next_ && cache_idx_ < cache_.size()) {
        out = cache_[cache_idx_++];
        cache_next_ += out.length;
        return true;
    }
    cache_ = dis_.disassemble(addr, 64);
    cache_idx_ = 0;
    if (cache_.empty())
        return false;
    out = cache_[cache_idx_++];
    cache_next_ = addr + out.length;
    return true;
}

int function_graph_builder::ensure_block(uint64_t start)
{
    const auto it = block_of_.find(start);
    if (it != block_of_.end())
        return it->second;
    const int idx = (int)f_->blocks.size();
    fg_block b;
    b.start = start;
    b.end   = start;
    f_->blocks.push_back(std::move(b));
    block_of_.emplace(start, idx);
    queue_.push_back(start);
    return idx;
}

int function_graph_builder::find_block_containing(uint64_t addr) const
{
    for (int i = 0; i < (int)f_->blocks.size(); ++i) {
        const fg_block& b = f_->blocks[i];
        if (addr >= b.start && addr < b.end && !b.insns.empty())
            return i;
    }
    return -1;
}

// 目标地址 → 块索引：已有块直接返回；块中部则拆分；否则新建 shell 并入队
int function_graph_builder::resolve_target(uint64_t target)
{
    const auto it = block_of_.find(target);
    if (it != block_of_.end())
        return it->second;
    const int owner = find_block_containing(target);
    if (owner >= 0) {
        split_block(owner, target);
        return block_of_[target];
    }
    return ensure_block(target);
}

// 在 bi 块的 at 地址处切开：前半保留原 start（承接所有指向原起点的边），
// 后半成为新块并接手全部出边；前半以 fall 边连到后半。
void function_graph_builder::split_block(int bi, uint64_t at)
{
    fg_block& b = f_->blocks[bi];
    size_t k = 0;
    while (k < b.insns.size() && b.insns[k].address != at)
        ++k;
    if (k == 0 || k >= b.insns.size())
        return;

    fg_block nb;
    nb.start  = at;
    nb.end    = b.end;
    nb.insns.assign(std::make_move_iterator(b.insns.begin() + k),
                    std::make_move_iterator(b.insns.end()));
    nb.filled        = b.filled;
    nb.edge_fall     = b.edge_fall;   nb.fall_addr  = b.fall_addr;
    nb.fall_external = b.fall_external;
    nb.edge_taken    = b.edge_taken;  nb.taken_addr = b.taken_addr;
    nb.taken_external= b.taken_external;
    nb.edge_jmp      = b.edge_jmp;    nb.jmp_addr   = b.jmp_addr;
    nb.jmp_external  = b.jmp_external;
    nb.is_ret        = b.is_ret;
    nb.truncated     = b.truncated;

    b.insns.resize(k);
    b.end = at;
    b.edge_fall = b.edge_taken = b.edge_jmp = -1;
    b.fall_addr = b.taken_addr = b.jmp_addr = 0;
    b.fall_external = b.taken_external = b.jmp_external = false;
    b.is_ret = false;
    b.truncated = false;
    b.filled = true;

    const int ni = (int)f_->blocks.size();
    f_->blocks.push_back(std::move(nb));       // 注意：此后 b 引用失效
    block_of_.emplace(at, ni);
    f_->blocks[bi].edge_fall = ni;             // 前半顺序流入后半
    f_->blocks[bi].fall_addr = at;
}

uint64_t function_graph_builder::find_function_entry(uint64_t addr)
{
    auto& pm  = process_manager::instance();
    auto* mem = pm.memory();
    if (!pm.is_attached() || !mem)
        return 0;

    // 1) 导出符号精确命中
    {
        const module_symbols* mod = nullptr;
        const symbol_entry*   sym = nullptr;
        uint64_t off = 0;
        if (symbol_table::instance().find_symbol(addr, &mod, &sym, &off) && off == 0)
            return addr;
    }

    // 2) 紧前 int3/nop 填充（≥2 字节）→ addr 即函数头
    {
        const size_t back = (size_t)std::min<uint64_t>(addr, 0x400);
        std::vector<uint8_t> buf(back);
        if (back >= 2 && mem->read(addr - back, buf.data(), buf.size())) {
            size_t i = buf.size();
            while (i > 0 && (buf[i - 1] == 0xCC || buf[i - 1] == 0x90))
                --i;
            if (buf.size() - i >= 2)
                return addr;
        }
    }

    // 3) 位于导出符号起点之后 ≤0x2000 → 符号即函数入口
    {
        symbol_table::instance().ensure_loaded_for_address(addr);
        const module_symbols* mod = nullptr;
        const symbol_entry*   sym = nullptr;
        uint64_t off = 0;
        if (symbol_table::instance().find_symbol(addr, &mod, &sym, &off) &&
            off != 0 && off <= 0x2000)
            return sym->address;
    }

    // 4) 兜底：地址本身
    return addr;
}

bool function_graph_builder::build(uint64_t addr, fg_function& out, std::string& err)
{
    auto& pm  = process_manager::instance();
    auto* mem = pm.memory();
    if (!pm.is_attached() || !mem) {
        err = "no process attached";
        return false;
    }

    dis_.set_arch(mem->architecture());
    cache_.clear();
    cache_idx_ = 0;
    cache_next_ = 0;
    insn_count_ = 0;

    const uint64_t entry = find_function_entry(addr);
    if (!entry) {
        err = "cannot locate function entry";
        return false;
    }

    out = fg_function{};
    out.entry = entry;
    f_ = &out;
    block_of_.clear();
    queue_.clear();

    // 函数名（导出符号优先，回退十六进制地址）
    std::string nm = symbol_table::instance().format_symbol(entry, false);
    if (nm.empty()) {
        char hb[24];
        snprintf(hb, sizeof hb, "%llX", (unsigned long long)entry);
        nm = hb;
    }
    out.name = nm;

    ensure_block(entry);
    out.blocks[0].is_entry = true;

    // 递归下降（CE parseFunction 语义的工作队列式实现）
    for (size_t qi = 0; qi < queue_.size() && !out.truncated; ++qi) {
        const uint64_t start = queue_[qi];
        const int bi = block_of_[start];
        if (f_->blocks[bi].filled)
            continue;
        f_->blocks[bi].filled = true;

        uint64_t cur = start;
        for (;;) {
            if (insn_count_ >= kMaxInsn || (int)f_->blocks.size() >= kMaxBlocks) {
                f_->blocks[bi].truncated = true;
                out.truncated = true;
                break;
            }

            disasm_line ln;
            if (!decode_next(cur, ln)) {
                f_->blocks[bi].truncated = true;   // 不可读：路径终止
                break;
            }
            f_->blocks[bi].insns.push_back(ln);
            ++insn_count_;
            cur += ln.length;
            f_->blocks[bi].end = cur;

            if (ln.is_ret) {                       // ret：块终止
                f_->blocks[bi].is_ret = true;
                break;
            }
            if (ln.is_branch && ln.is_call)
                continue;                          // call 不影响 CFG 顺序流

            if (ln.is_branch && !ln.is_cond) {     // 无条件 jmp
                const uint64_t t = ln.branch_target;
                if (t == 0 || t == cur)
                    break;                         // 间接跳转/自跳：无法静态跟随
                if (t >= out.entry - kRange && t <= out.entry + kRange) {
                    f_->blocks[bi].edge_jmp = resolve_target(t);
                    f_->blocks[bi].jmp_addr = t;
                } else {
                    f_->blocks[bi].jmp_external = true;
                    f_->blocks[bi].jmp_addr = t;
                }
                break;
            }

            if (ln.is_cond) {                      // 条件跳转：taken 一侧成块
                const uint64_t t = ln.branch_target;
                if (t != 0 && t >= out.entry - kRange && t <= out.entry + kRange) {
                    f_->blocks[bi].edge_taken = resolve_target(t);
                    f_->blocks[bi].taken_addr = t;
                } else {
                    f_->blocks[bi].taken_external = true;
                    f_->blocks[bi].taken_addr = t;
                }
                // fall-through 顺序继续
            }

            // 顺序后继：命中其它块的起点 → 连边收敛；命中块中部 → 拆分
            const auto it = block_of_.find(cur);
            if (it != block_of_.end()) {
                f_->blocks[bi].edge_fall = it->second;
                f_->blocks[bi].fall_addr = cur;
                break;
            }
            const int owner = find_block_containing(cur);
            if (owner >= 0) {
                split_block(owner, cur);
                f_->blocks[bi].edge_fall = block_of_[cur];
                f_->blocks[bi].fall_addr = cur;
                break;
            }
        }
    }

    // 展平出边
    for (int i = 0; i < (int)out.blocks.size(); ++i) {
        const fg_block& b = out.blocks[i];
        if (b.edge_fall  >= 0) out.edges.push_back({i, b.edge_fall,  fg_edge_kind::fall});
        if (b.edge_taken >= 0) out.edges.push_back({i, b.edge_taken, fg_edge_kind::taken});
        if (b.edge_jmp   >= 0) out.edges.push_back({i, b.edge_jmp,   fg_edge_kind::uncond});
    }
    return !out.blocks.empty();
}

// ════════════════════════════ 布局 ════════════════════════════

// 块标题：就近符号标签（含相对偏移），无符号回退十六进制地址。
// 测量与渲染必须共用同一文本，否则标题会溢出块边界。
static std::string block_title(const fg_block& b, const fg_function& g)
{
    (void)g;
    char t[160];
    std::string sym = symbol_table::instance().format_symbol(b.start, false);
    if (!sym.empty())
        return sym;
    snprintf(t, sizeof t, "%llX", (unsigned long long)b.start);
    return t;
}

// 把指令文本中的原始跳转目标地址替换为 <模块.符号+偏移>（与反汇编视图一致）。
// 无符号目标保留原始地址文本。
static void decorate_block_insns(fg_block& b)
{
    auto& st = symbol_table::instance();
    for (auto& in : b.insns) {
        if (!in.is_branch || in.branch_target == 0) continue;
        st.ensure_loaded_for_address(in.branch_target);
        std::string label = st.format_symbol(in.branch_target);
        if (label.empty()) continue;

        char raw[32];
        snprintf(raw, sizeof raw, "0x%016llX", (unsigned long long)in.branch_target);
        size_t pos = in.text.find(raw);
        if (pos == std::string::npos) {
            snprintf(raw, sizeof raw, "0x%llX", (unsigned long long)in.branch_target);
            pos = in.text.find(raw);
            if (pos == std::string::npos) continue;
        }
        in.text.replace(pos, std::strlen(raw), "<" + label + ">");
    }
}

// 测量块尺寸（需 ImGui 上下文：用当前字体量文本宽度）
static void measure_blocks(fg_function& g)
{
    const float line_h  = ImGui::GetTextLineHeight();
    const float title_h = line_h + kTitlePadY * 2.f;
    for (auto& b : g.blocks) {
        float w = ImGui::CalcTextSize(block_title(b, g).c_str()).x;
        for (const auto& in : b.insns)
            w = std::max(w, ImGui::CalcTextSize(in.text.c_str()).x);
        b.w = std::max(w + kPadX * 2.f + 4.f, 70.f);
        b.h = title_h + (float)b.insns.size() * line_h + kPadY * 2.f;
    }
}

// 分层 + 排序：回边标记（DFS 三色）→ 最长路径分层 → DFS 初始行序 → 重心降交叉
static void assign_grid(fg_function& g)
{
    const int n = (int)g.blocks.size();
    if (!n) return;

    // 1) 回边标记
    {
        std::vector<std::vector<int>> adj(n);
        for (int ei = 0; ei < (int)g.edges.size(); ++ei)
            adj[g.edges[ei].from].push_back(ei);
        std::vector<uint8_t> color(n, 0);       // 0 白 1 灰 2 黑
        std::vector<std::pair<int, size_t>> st;
        for (int root = 0; root < n; ++root) {
            if (color[root]) continue;
            color[root] = 1;
            st.push_back({root, 0});
            while (!st.empty()) {
                const int u = st.back().first;
                if (st.back().second < adj[u].size()) {
                    const int ei = adj[u][st.back().second++];
                    const int v = g.edges[ei].to;
                    if (color[v] == 1) g.edges[ei].back = true;   // 指向栈内节点
                    else if (color[v] == 0) {
                        color[v] = 1;
                        st.push_back({v, 0});
                    }
                } else {
                    color[u] = 2;
                    st.pop_back();
                }
            }
        }
    }

    // 2) 最长路径分层（仅前向边，迭代到不动点）
    std::vector<int> col(n, 0);
    for (int pass = 0; pass < n; ++pass) {
        bool changed = false;
        for (const auto& e : g.edges) {
            if (e.back) continue;
            if (col[e.to] < col[e.from] + 1) {
                col[e.to] = col[e.from] + 1;
                changed = true;
            }
        }
        if (!changed) break;
    }
    int maxc = 0;
    for (int i = 0; i < n; ++i) {
        g.blocks[i].col = col[i];
        maxc = std::max(maxc, col[i]);
    }

    // 3) 行序：DFS 前序（fall→taken→jmp）给初始序，再 4 轮重心降交叉
    std::vector<int> row(n, -1);
    {
        std::vector<std::vector<int>> adj(n);
        for (int ei = 0; ei < (int)g.edges.size(); ++ei)
            if (!g.edges[ei].back)
                adj[g.edges[ei].from].push_back(ei);
        for (auto& a : adj)
            std::sort(a.begin(), a.end(), [&](int x, int y) {
                return (int)g.edges[x].kind < (int)g.edges[y].kind;
            });

        std::vector<int> col_cnt(maxc + 1, 0);
        std::vector<int> stk{0};
        while (!stk.empty()) {
            const int u = stk.back();
            stk.pop_back();
            if (row[u] >= 0) continue;
            row[u] = col_cnt[col[u]]++;
            for (int ei : adj[u])
                if (row[g.edges[ei].to] < 0)
                    stk.push_back(g.edges[ei].to);
        }
        for (int i = 0; i < n; ++i)
            if (row[i] < 0) row[i] = col_cnt[col[i]]++;
    }

    // 双向邻接（重心用，含回边）
    std::vector<std::vector<int>> succ(n), pred(n);
    for (const auto& e : g.edges) {
        succ[e.from].push_back(e.to);
        pred[e.to].push_back(e.from);
    }

    std::vector<std::vector<int>> by_col(maxc + 1);
    for (int i = 0; i < n; ++i) {
        g.blocks[i].row = row[i];
        by_col[col[i]].push_back(i);
    }
    for (int pass = 0; pass < 4; ++pass) {
        const bool fwd = (pass % 2) == 0;
        for (int ci = 0; ci <= maxc; ++ci) {
            const int c = fwd ? ci : (maxc - ci);
            if (by_col[c].size() < 2) continue;
            std::vector<float> key;
            for (int u : by_col[c]) {
                float sum = 0;
                int   cnt = 0;
                for (int v : succ[u]) { sum += (float)g.blocks[v].row; ++cnt; }
                for (int v : pred[u]) { sum += (float)g.blocks[v].row; ++cnt; }
                key.push_back(cnt ? sum / cnt : (float)g.blocks[u].row);
            }
            std::vector<size_t> idx(by_col[c].size());
            for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
            std::sort(idx.begin(), idx.end(),
                      [&](size_t a, size_t b) { return key[a] < key[b]; });
            const std::vector<int> sorted = by_col[c];
            for (size_t i = 0; i < idx.size(); ++i) {
                by_col[c][i] = sorted[idx[i]];
                g.blocks[sorted[idx[i]]].row = (int)i;
            }
        }
    }
}

static void position_blocks(fg_function& g)
{
    int maxc = 0, maxr = 0;
    for (const auto& b : g.blocks) {
        maxc = std::max(maxc, b.col);
        maxr = std::max(maxr, b.row);
    }
    std::vector<float> col_w(maxc + 1, 0), row_h(maxr + 1, 0);
    for (const auto& b : g.blocks) {
        col_w[b.col] = std::max(col_w[b.col], b.w);
        row_h[b.row] = std::max(row_h[b.row], b.h);
    }
    std::vector<float> col_x(maxc + 1, 0), row_y(maxr + 1, 0);
    float x = 0;
    for (int c = 0; c <= maxc; ++c) { col_x[c] = x; x += col_w[c] + kColGap; }
    float y = 0;
    for (int r = 0; r <= maxr; ++r) { row_y[r] = y; y += row_h[r] + kRowGap; }
    for (auto& b : g.blocks) {
        b.x = col_x[b.col] + (col_w[b.col] - b.w) * 0.5f;
        b.y = row_y[b.row];
    }
}

static void route_edges(fg_function& g)
{
    std::unordered_map<int, int> out_cnt, in_cnt, out_i, in_i;
    for (const auto& e : g.edges) {
        if (e.back) continue;
        ++out_cnt[e.from];
        ++in_cnt[e.to];
    }

    // lane 分配：前向边按（源列,目标列）分组；回边按目标列
    std::map<std::pair<int, int>, int> lane;

    for (auto& e : g.edges) {
        const fg_block& u = g.blocks[e.from];
        const fg_block& v = g.blocks[e.to];

        if (e.from == e.to) {   // 自环：右侧小回环
            const float cy = u.y + u.h * 0.5f;
            const float r  = u.x + u.w;
            e.pts = {{r, cy}, {r + 16.f, cy}, {r + 16.f, u.y - 10.f},
                     {u.x + u.w * 0.5f, u.y - 10.f}, {u.x + u.w * 0.5f, u.y}};
            continue;
        }

        if (!e.back) {
            const int oi = out_i[e.from]++;
            float sx = u.x + u.w * 0.5f +
                       ((float)oi - (float)(out_cnt[e.from] - 1) * 0.5f) * kAnchorStep;
            sx = std::clamp(sx, u.x + 6.f, u.x + u.w - 6.f);

            const int ii = in_i[e.to]++;
            float dx = v.x + v.w * 0.5f +
                       ((float)ii - (float)(in_cnt[e.to] - 1) * 0.5f) * kAnchorStep;
            dx = std::clamp(dx, v.x + 6.f, v.x + v.w - 6.f);

            const int ln = lane[{u.col, v.col}]++;
            const float ymid = std::max(u.y + u.h, v.y) + 12.f + kLaneH * (float)ln;
            e.pts = {{sx, u.y + u.h}, {sx, ymid}, {dx, ymid}, {dx, v.y}};
        } else {
            const int ln = lane[{std::min(u.col, v.col), -1}]++;
            const float xl = std::min(u.x, v.x) - 24.f - kLaneW * (float)ln;
            const float sy = u.y + u.h * 0.5f;
            const float dy = v.y + v.h * 0.5f;
            e.pts = {{u.x, sy}, {xl, sy}, {xl, dy}, {v.x, dy}};
        }
    }
}

// ════════════════════════════ 窗口 ════════════════════════════

void function_graph_window::open_at(uint64_t addr)
{
    pending_addr_ = addr;
    need_rebuild_ = true;
    open_ = true;
}

void function_graph_window::rebuild(uint64_t addr)
{
    built_ = false;
    graph_ = fg_function{};

    function_graph_builder bld;
    std::string err;
    if (!bld.build(addr, graph_, err)) {
        build_err_ = err;
        return;
    }

    // 先装饰指令文本（跳转目标 → 符号标签），保证测量与渲染一致
    for (auto& b : graph_.blocks)
        decorate_block_insns(b);

    measure_blocks(graph_);
    assign_grid(graph_);
    position_blocks(graph_);
    route_edges(graph_);
    built_ = true;
    selected_ = -1;
    fit_view();
}

void function_graph_window::fit_view()
{
    if (graph_.blocks.empty() || canvas_sz_.x <= 0 || canvas_sz_.y <= 0)
        return;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const auto& b : graph_.blocks) {
        x0 = std::min(x0, b.x);         y0 = std::min(y0, b.y);
        x1 = std::max(x1, b.x + b.w);   y1 = std::max(y1, b.y + b.h);
    }
    const float bw = x1 - x0, bh = y1 - y0;
    zoom_ = std::clamp(std::min(canvas_sz_.x / bw, canvas_sz_.y / bh) * 0.92f, 0.08f, 2.0f);
    scroll_.x = x0 - (canvas_sz_.x / zoom_ - bw) * 0.5f;
    scroll_.y = y0 - (canvas_sz_.y / zoom_ - bh) * 0.5f;
}

int function_graph_window::hit_block(ImVec2 mp, ImVec2 p0) const
{
    for (int i = (int)graph_.blocks.size() - 1; i >= 0; --i) {
        const auto& b = graph_.blocks[i];
        const ImVec2 sp(p0.x + (b.x - scroll_.x) * zoom_,
                        p0.y + (b.y - scroll_.y) * zoom_);
        const ImVec2 ep(sp.x + b.w * zoom_, sp.y + b.h * zoom_);
        if (mp.x >= sp.x && mp.x <= ep.x && mp.y >= sp.y && mp.y <= ep.y)
            return i;
    }
    return -1;
}

void function_graph_window::render(const std::function<void(uint64_t)>& jump_to_disasm)
{
    if (!open_)
        return;
    ImGui::SetNextWindowSize(ImVec2(940, 660), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Function Graph###function_graph", &open_)) {
        ImGui::End();
        return;
    }

    // 先量出画布尺寸，重建后 fit_view 才有正确的视口
    const ImVec2 canvas = ImGui::GetContentRegionAvail();
    canvas_sz_ = canvas;

    if (need_rebuild_) {
        rebuild(pending_addr_);
        need_rebuild_ = false;
    }

    if (!built_) {
        ImGui::TextDisabled("Cannot build function graph: %s", build_err_.c_str());
        ImGui::End();
        return;
    }

    if (ImGui::Button("Fit"))
        fit_view();
    ImGui::SameLine();
    ImGui::TextDisabled("%s  |  %d blocks, %d edges%s", graph_.name.c_str(),
                        (int)graph_.blocks.size(), (int)graph_.edges.size(),
                        graph_.truncated ? "  (truncated)" : "");

    ImGui::BeginChild("##fgcanvas", canvas, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoMove);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

    // ---- 滚轮缩放（鼠标为锚点）----
    if (hovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f) {
            const float old = zoom_;
            zoom_ = std::clamp(zoom_ * (wheel > 0 ? 1.15f : 1.f / 1.15f), 0.08f, 4.0f);
            const float kx = (mp.x - p0.x) / old, ky = (mp.y - p0.y) / old;
            scroll_.x += kx - (mp.x - p0.x) / zoom_;
            scroll_.y += ky - (mp.y - p0.y) / zoom_;
        }
    }

    // ---- 命中 / 选择 / 拖动 ----
    const int hit = hovered ? hit_block(mp, p0) : -1;
    if (hovered) {
        if (ImGui::IsMouseClicked(0)) {
            selected_ = hit;
            drag_block_ = hit;
            panning_ = (hit < 0);
            last_mouse_ = mp;
        }
        if (hit >= 0 && ImGui::IsMouseDoubleClicked(0) && jump_to_disasm)
            jump_to_disasm(graph_.blocks[hit].start);
    }
    if (ImGui::IsMouseReleased(0)) {
        drag_block_ = -1;
        panning_ = false;
    }
    if (ImGui::IsMouseDown(0) && (drag_block_ >= 0 || panning_)) {
        const ImVec2 d(mp.x - last_mouse_.x, mp.y - last_mouse_.y);
        if (drag_block_ >= 0) {
            graph_.blocks[drag_block_].x += d.x / zoom_;
            graph_.blocks[drag_block_].y += d.y / zoom_;
        } else {
            scroll_.x -= d.x / zoom_;
            scroll_.y -= d.y / zoom_;
        }
    }
    last_mouse_ = mp;

    const auto w2s = [&](const ImVec2& w) {
        return ImVec2(p0.x + (w.x - scroll_.x) * zoom_,
                      p0.y + (w.y - scroll_.y) * zoom_);
    };
    const ImVec2 view_min(scroll_.x - 60.f, scroll_.y - 60.f);
    const ImVec2 view_max(scroll_.x + canvas.x / zoom_ + 60.f,
                          scroll_.y + canvas.y / zoom_ + 60.f);

    const bool dark = fg_dark_ui();
    const ImU32 col_fall  = ImGui::ColorConvertFloat4ToU32({0.90f, 0.32f, 0.30f, 1.f});
    const ImU32 col_taken = ImGui::ColorConvertFloat4ToU32({0.35f, 0.62f, 1.00f, 1.f});
    const ImU32 col_jmp   = ImGui::ColorConvertFloat4ToU32({0.80f, 0.45f, 0.95f, 1.f});
    const ImU32 col_border = ImGui::ColorConvertFloat4ToU32(
        dark ? ImVec4{0.55f, 0.55f, 0.58f, 1.f} : ImVec4{0.35f, 0.35f, 0.40f, 1.f});
    const ImU32 col_sel = ImGui::ColorConvertFloat4ToU32({1.0f, 0.72f, 0.25f, 1.f});
    const ImU32 col_text = ImGui::GetColorU32(ImGuiCol_Text);

    const float line_h  = ImGui::GetTextLineHeight();
    const float title_h = line_h + kTitlePadY * 2.f;
    const float thickness = std::clamp(1.4f * zoom_, 1.0f, 3.0f);

    // ---- 边（先画，块盖在上面）----
    for (const auto& e : graph_.edges) {
        if (e.pts.size() < 2) continue;
        float ex0 = 1e9f, ey0 = 1e9f, ex1 = -1e9f, ey1 = -1e9f;
        for (const auto& p : e.pts) {
            ex0 = std::min(ex0, p.x); ey0 = std::min(ey0, p.y);
            ex1 = std::max(ex1, p.x); ey1 = std::max(ey1, p.y);
        }
        if (ex1 < view_min.x || ex0 > view_max.x || ey1 < view_min.y || ey0 > view_max.y)
            continue;

        const ImU32 col = e.kind == fg_edge_kind::fall  ? col_fall
                          : e.kind == fg_edge_kind::taken ? col_taken
                                                          : col_jmp;
        std::vector<ImVec2> sp;
        sp.reserve(e.pts.size());
        for (const auto& p : e.pts) sp.push_back(w2s(p));
        dl->AddPolyline(sp.data(), (int)sp.size(), col, 0, thickness);

        // 箭头（最后一段方向）
        const ImVec2& a = sp[sp.size() - 2];
        const ImVec2& b = sp[sp.size() - 1];
        float vx = b.x - a.x, vy = b.y - a.y;
        const float len = std::sqrt(vx * vx + vy * vy);
        if (len > 0.001f) {
            vx /= len; vy /= len;
            const float as = 5.5f * std::clamp(zoom_, 0.6f, 1.6f);
            const ImVec2 t1(b.x - vx * as * 2.f - vy * as,
                            b.y - vy * as * 2.f + vx * as);
            const ImVec2 t2(b.x - vx * as * 2.f + vy * as,
                            b.y - vy * as * 2.f - vx * as);
            dl->AddTriangleFilled(b, t1, t2, col);
        }
    }

    // ---- 块 ----
    const ImU32 col_bg   = ImGui::GetColorU32(ImGuiCol_WindowBg);
    const ImU32 col_hdr  = ImGui::GetColorU32(ImGuiCol_TableHeaderBg);
    const ImU32 col_hdr_entry = ImGui::ColorConvertFloat4ToU32({0.20f, 0.55f, 0.25f, 0.85f});
    const ImU32 col_hdr_ret   = ImGui::ColorConvertFloat4ToU32({0.70f, 0.22f, 0.22f, 0.85f});

    for (int i = 0; i < (int)graph_.blocks.size(); ++i) {
        const auto& b = graph_.blocks[i];
        if (b.x + b.w < view_min.x || b.x > view_max.x ||
            b.y + b.h < view_min.y || b.y > view_max.y)
            continue;

        const ImVec2 sp = w2s({b.x, b.y});
        const ImVec2 ep(sp.x + b.w * zoom_, sp.y + b.h * zoom_);
        const ImVec2 hdr_ep(ep.x, sp.y + title_h * zoom_);

        dl->AddRectFilled(sp, ep, col_bg, 3.f);
        const ImU32 hdr = b.is_entry ? col_hdr_entry
                          : (b.is_ret ? col_hdr_ret : col_hdr);
        dl->AddRectFilled(sp, hdr_ep, hdr, 3.f, ImDrawFlags_RoundCornersTop);
        dl->AddRect(sp, ep, i == selected_ ? col_sel : col_border, 3.f, 0,
                    i == selected_ ? 2.0f : 1.0f);

        if (zoom_ < 0.35f)
            continue;

        // 标题：就近符号标签（与测量宽度一致），无符号回退十六进制地址
        const std::string title = block_title(b, graph_);
        ImFont* font = ImGui::GetFont();
        const float fs = ImGui::GetFontSize() * zoom_;
        dl->AddText(font, fs, ImVec2(sp.x + kPadX * zoom_, sp.y + kTitlePadY * zoom_),
                    col_text, title.c_str());

        if (zoom_ < 0.75f)
            continue;

        float ty = sp.y + title_h * zoom_ + kPadY * zoom_;
        for (const auto& in : b.insns) {
            dl->AddText(font, fs, ImVec2(sp.x + kPadX * zoom_, ty),
                        in.is_branch ? col_taken : col_text, in.text.c_str());
            ty += line_h * zoom_;
        }
    }

    ImGui::EndChild();
    ImGui::End();
}

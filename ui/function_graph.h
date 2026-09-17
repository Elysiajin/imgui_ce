#ifndef FUNCTION_GRAPH_H
#define FUNCTION_GRAPH_H

#include "ui/zydis_disassembler.h"

#include "imgui.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ── 函数控制流图（对标 Ghidra Function Graph 的精简实现）─────────────
// 数据流：入口地址 -> CFG 构建（递归下降 + 基本块切分，CE parseFunction 语义）
//       -> 垂直分层布局（流程自上而下；最长路径分层 + 重心排序降交叉，
//          Ghidra flowchart 同思路）
//       -> 正交边路由（折线 + 回边走左侧通道）-> ImGui 画布渲染。

// 出边语义：fall = 顺序执行/条件未命中；taken = 条件命中；uncond = 无条件 jmp
enum class fg_edge_kind { fall, taken, uncond };

struct fg_edge {
    int from = -1;
    int to = -1;
    fg_edge_kind kind = fg_edge_kind::fall;
    bool back = false;                 // 回边（DFS 判定），路由绕左侧通道
    std::vector<ImVec2> pts;           // 世界坐标折线（末点 = 箭头位置）
};

struct fg_block {
    uint64_t start = 0, end = 0;       // [start, end)
    std::vector<disasm_line> insns;
    bool filled = false;               // 已完成顺序解码

    // 构建期出边（最多三类）
    int      edge_fall = -1;  uint64_t fall_addr = 0;  bool fall_external = false;
    int      edge_taken = -1; uint64_t taken_addr = 0; bool taken_external = false;
    int      edge_jmp = -1;   uint64_t jmp_addr = 0;   bool jmp_external = false;

    // 布局结果（世界坐标，由分层布局一次性计算，之后不可改动）
    int col = 0, row = 0;
    float x = 0, y = 0, w = 0, h = 0;

    bool is_entry = false;
    bool is_ret = false;               // 以 ret 结束
    bool truncated = false;            // 解码中断（读取失败/超上限）
};

struct fg_function {
    uint64_t entry = 0;
    std::string name;
    std::vector<fg_block> blocks;
    std::vector<fg_edge>  edges;
    bool truncated = false;
};

// CFG 构建器
// 函数边界识别（融合 CE/Ghidra 思路，按优先级）：
//   1) 地址恰好命中导出符号（symbol_table，导出即函数起点）→ 直接作入口；
//   2) 地址紧前方存在 ≥2 字节 int3/nop 填充（CC/90）→ 地址即函数头
//      （编译器在函数间填充，覆盖无导出的本地函数）；
//   3) 地址位于某导出符号起点之后 ≤0x2000 → 该符号即入口（导出函数中段）；
//   4) 兜底：地址本身（CE 在无任何信息时同样从选中地址开始解析）。
// 函数体识别：从入口递归下降（工作队列），在 ret / 无条件 jmp / 不可读处收束；
// 条件跳转把 taken/not-taken 两个目标都作为新块起点；跳到已解析块则连边收敛；
// 跳入已解析块内部则拆分该块（保证块不重叠）。目标超出范围窗口视为块外跳转。
class function_graph_builder {
public:
    bool build(uint64_t addr, fg_function& out, std::string& err);

    static uint64_t find_function_entry(uint64_t addr);

private:
    bool decode_next(uint64_t addr, disasm_line& out);
    int  ensure_block(uint64_t start);
    int  resolve_target(uint64_t target);
    int  find_block_containing(uint64_t addr) const;
    void split_block(int bi, uint64_t at);

    disassembler dis_;                 // 独立 decoder（架构随目标进程）
    std::vector<disasm_line> cache_;   // 顺序解码缓存（减少 ReadProcessMemory）
    size_t   cache_idx_ = 0;
    uint64_t cache_next_ = 0;

    fg_function* f_ = nullptr;
    std::unordered_map<uint64_t, int> block_of_;   // 块起始地址 -> 块索引
    std::vector<uint64_t> queue_;                  // 待填充块起始地址
    int insn_count_ = 0;
};

// 函数图窗口（独立 ImGui 窗口；交互：拖拽平移/滚轮缩放/单击选中/双击跳转反汇编）
class function_graph_window {
public:
    void open_at(uint64_t addr);
    void render(const std::function<void(uint64_t)>& jump_to_disasm);
    bool is_open() const { return open_; }
    void close() { open_ = false; }

private:
    void rebuild(uint64_t addr);       // 构建 + 测量 + 布局（需 ImGui 上下文）
    void fit_view();
    int  hit_block(ImVec2 mouse, ImVec2 origin) const;

    bool open_ = false;
    bool need_rebuild_ = false;
    uint64_t pending_addr_ = 0;

    fg_function graph_;
    bool built_ = false;
    std::string build_err_;

    // 视口：世界坐标 scroll + 缩放
    ImVec2 scroll_{0, 0};
    float  zoom_ = 1.0f;
    ImVec2 canvas_sz_{0, 0};

    // 交互状态
    int    selected_ = -1;
    bool   panning_ = false;
    ImVec2 last_mouse_{0, 0};
};

#endif // FUNCTION_GRAPH_H

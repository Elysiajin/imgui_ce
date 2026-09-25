// AA 两遍汇编引擎单测（mock 内存：本地缓冲区，不依赖目标进程）。
//
// 验证 CE教程.CT 里真实出现的脚本模式：
//   alloc/label/registersymbol + 前向/后向标号引用 + jmp/nop/db 数据
// 以及 aobscan 的通配匹配。

#include <windows.h>

#include "ct/aa_script.h"
#include "ct/address_parser.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            std::printf("  [FAIL] %s\n", msg);              \
            ++g_fail;                                       \
        }                                                   \
    } while (0)

// 本地内存 mock：一整块 RW 缓冲充当目标进程内存
struct mock_memory : IMemoryAccessor {
    static constexpr uint64_t k_base = 0x00400000;
    std::vector<uint8_t> mem;      // k_base 起的线性空间
    std::vector<uint64_t> alloc_addrs;

    mock_memory(size_t size = 1 << 20) : mem(size, 0xCC) {}

    bool attach(uint32_t) override { return true; }
    void detach() override {}
    bool read(uint64_t a, void* b, size_t n) override {
        if (a < k_base || a - k_base + n > mem.size()) return false;
        std::memcpy(b, mem.data() + (a - k_base), n);
        return true;
    }
    bool write(uint64_t a, const void* b, size_t n) override {
        if (a < k_base || a - k_base + n > mem.size()) return false;
        std::memcpy(mem.data() + (a - k_base), b, n);
        return true;
    }
    bool is_process_alive() const override { return true; }
    std::string name() const override { return "mock"; }
    uint64_t alloc(size_t n) override {
        // 简化：从高位段按 64KB 对齐切
        const uint64_t a = k_base + mem.size() - (alloc_addrs.size() + 1) * 0x10000;
        alloc_addrs.push_back(a);
        return a;
    }
    bool free_mem(uint64_t a) override {
        for (size_t i = 0; i < alloc_addrs.size(); ++i)
            if (alloc_addrs[i] == a) { alloc_addrs.erase(alloc_addrs.begin() + (long)i); return true; }
        return false;
    }
    process_arch architecture() const override { return process_arch::x86_64; }
};

// 定位 mock 里源码标号 → 写入位置的关系靠脚本自身；这里的检查用直接字节断言
static bool bytes_at(const mock_memory& m, uint64_t addr,
                     std::initializer_list<uint8_t> expect) {
    if (addr < mock_memory::k_base) return false;
    const uint64_t off = addr - mock_memory::k_base;
    size_t i = 0;
    for (uint8_t b : expect) {
        if (off + i >= m.mem.size() || m.mem[(size_t)(off + i)] != b) return false;
        ++i;
    }
    return true;
}

int main()
{
    // ---------------------------------------------------------------
    // 用例 1：alloc + label + registersymbol + 前向/后向跳转引用
    // （CE教程.CT "修改伤害" 脚本的等价结构）
    {
        std::printf("== alloc/label/registersymbol/jmp 前向引用\n");
        mock_memory mem;
        // "game.exe"+29D8D: 在 mock 里映射：地址解析需要模块表——
        // 直接用绝对地址 0x401000 作为补丁点
        const std::string script = R"([ENABLE]
alloc(newmem,2048)
label(returnhere)
label(originalcode)

newmem:
mov [ebx+04],eax
jmp returnhere

originalcode:
nop
nop

"Tutorial-i386.exe"+29D8D:
jmp newmem
returnhere:

registersymbol(newmem)

[DISABLE]
dealloc(newmem)
unregistersymbol(newmem)
"Tutorial-i386.exe"+29D8D:
db 89 43 04
)";

        // 模块表：Tutorial-i386.exe @ 0x400000（mock 基址一致）
        std::vector<module_info> mods = { {"Tutorial-i386.exe", 0x400000, 0x100000, "C:/t.exe"} };
        aa_session session;
        std::unordered_map<std::string, uint64_t> symbols;
        const aa_result r = aa_run_block(script, true, mods, symbols, session,
                                         &mem, process_arch::x86_64, {});
        CHECK(r.ok, "enable ok");
        for (auto& e : r.errors) std::printf("    err: %s\n", e.c_str());
        CHECK(session.allocs.count("newmem"), "alloc 登记 newmem");
        CHECK(!r.registered.empty() && r.registered[0].first == "newmem",
              "registersymbol(newmem) 返回");

        const uint64_t patch = 0x400000 + 0x29D8D;
        // 补丁点应是 jmp rel32（E9，目标 newmem alloc 地址）
        CHECK(mem.mem[(size_t)(patch - mock_memory::k_base)] == 0xE9,
              "补丁点写入 jmp rel32");

        // 激活后再取消：dealloc 释放 + db 恢复 89 43 04
        symbols["newmem"] = session.allocs["newmem"];
        const aa_result rd = aa_run_block(script, false, mods, symbols, session,
                                          &mem, process_arch::x86_64, {});
        CHECK(rd.ok, "disable ok");
        for (auto& e : rd.errors) std::printf("    err: %s\n", e.c_str());
        CHECK(session.allocs.empty(), "dealloc 后会话清空");
        CHECK(bytes_at(mem, patch, {0x89, 0x43, 0x04}), "disable 恢复 db 89 43 04");
    }

    // ---------------------------------------------------------------
    // 用例 2：短跳转（rel8）+ 后向引用 + 迭代定长
    {
        std::printf("== 短跳转迭代定长\n");
        mock_memory mem;
        const std::string script = R"(00401000:
nop
nop
back:
jmp back
db 90
)";
        std::vector<module_info> mods;
        aa_session session;
        std::unordered_map<std::string, uint64_t> symbols;
        const aa_result r = aa_run_block(script, true, mods, symbols, session,
                                         &mem, process_arch::x86_64, {});
        CHECK(r.ok, "ok");
        for (auto& e : r.errors) std::printf("    err: %s\n", e.c_str());
        // 0x401000 nop, 0x401001 nop, 0x401002 back: jmp short(2B), 0x401004 db 90
        CHECK(bytes_at(mem, 0x401004, {0x90}), "short jmp 布局收敛（db 落点正确）");
    }

    // ---------------------------------------------------------------
    // 用例 3：db/dw/dd 数据写入
    {
        std::printf("== db/dw/dd 数据\n");
        mock_memory mem;
        const std::string script = R"(00402000:
db 11 22 33
dw 0x1234
dd 0xdeadbeef
)";
        std::vector<module_info> mods;
        aa_session s;
        std::unordered_map<std::string, uint64_t> sy;
        const aa_result r = aa_run_block(script, true, mods, sy, s, &mem,
                                         process_arch::x86_64, {});
        CHECK(r.ok, "ok");
        for (auto& e : r.errors) std::printf("    err: %s\n", e.c_str());
        CHECK(bytes_at(mem, 0x402000, {0x11, 0x22, 0x33, 0x34, 0x12}),
              "db/dw 字节序与落点");
        CHECK(bytes_at(mem, 0x402005, {0xef, 0xbe, 0xad, 0xde}),
              "dd 小端落点");
    }

    // ---------------------------------------------------------------
    // 用例 4：aobscanmodule 通配扫描
    {
        std::printf("== aobscanmodule\n");
        mock_memory mem;
        // 在 mock 偏移 0x1000 放特征 8B 03 89 02 ?? 45
        const uint8_t pat[] = {0x8B, 0x03, 0x89, 0x02, 0x77, 0x45};
        std::memcpy(mem.mem.data() + 0x1000, pat, sizeof(pat));

        const std::string script = R"([ENABLE]
aobscanmodule(INJECT,Tutorial-i386.exe,8B 03 89 02 ?? 45)
INJECT:
nop
nop
)";
        std::vector<module_info> mods = { {"Tutorial-i386.exe", 0x400000, 0x100000, "C:/t.exe"} };
        aa_session s;
        std::unordered_map<std::string, uint64_t> sy;
        std::vector<memory_region> regions = {
            {mock_memory::k_base, mem.mem.size(), PAGE_READWRITE, 0x1000, 0}};
        const aa_result r = aa_run_block(script, true, mods, sy, s, &mem,
                                         process_arch::x86_64, regions);
        CHECK(r.ok, "ok");
        for (auto& e : r.errors) std::printf("    err: %s\n", e.c_str());
        CHECK(bytes_at(mem, mock_memory::k_base + 0x1000, {0x90, 0x90}),
              "aobscan 命中处写入 nop nop");
    }

    // ---------------------------------------------------------------
    // 用例 5：{$LUA} 拒绝执行
    {
        std::printf("== {$LUA} 拒绝\n");
        mock_memory mem;
        const std::string script = R"([ENABLE]
{$LUA}
writeBytes("x", 1, 2, 3)
)";
        std::vector<module_info> mods;
        aa_session s;
        std::unordered_map<std::string, uint64_t> sy;
        const aa_result r = aa_run_block(script, true, mods, sy, s, &mem,
                                         process_arch::x86_64, {});
        CHECK(!r.ok, "{$LUA} 明确报错不执行");
        CHECK(!r.errors.empty(), "有错误信息");
    }

    std::printf(g_fail == 0 ? "\nALL PASSED\n" : "\n%d FAILURES\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}

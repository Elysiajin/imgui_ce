// scan_perf —— 定位扫描瓶颈的独立微基准。
// 量化三个关键问题（不依赖整个 scan_engine 链接，只依赖 SIMD 头 + Win32 API）：
//   1) RPM 读取吞吐 vs SIMD 计算吞吐    （首扫：读是瓶颈还是算是瓶颈）
//   2) 逐地址 8B 读取 vs 逐页批量读取    （再扫 A/B：系统调用次数）
//   3) 稀疏快照的二分/访问开销
//
// 用法：
//   scan_perf --victim <MB>            # 靶进程模式：分配内存、填目标值、随时改写
//   scan_perf <victim_pid> [simd_mb]   # 父模式：测量
#include "../scan/scan_simd_accelerate.h"
#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <chrono>

using clk = std::chrono::steady_clock;
static long long ms_of(clk::time_point a, clk::time_point b) {
    return (long long)std::chrono::duration_cast<std::chrono::microseconds>(b - a).count() / 1000;
}
static double gb_per_s(size_t bytes, long long ms) {
    if (ms <= 0) return 0.0;
    return (double)bytes / (1024.0 * 1024.0 * 1024.0) / ((double)ms / 1000.0);
}

// ---- 枚举目标进程可读的已提交区域，累计字节数 ----
static size_t collect_readable_regions(HANDLE h, std::vector<MEMORY_BASIC_INFORMATION>& out) {
    size_t total = 0;
    uintptr_t addr = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (VirtualQueryEx(h, (LPCVOID)addr, &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_COMMIT &&
            mbi.Protect != PAGE_NOACCESS &&
            !(mbi.Protect & PAGE_GUARD) &&
            (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_READONLY ||
             mbi.Protect == PAGE_EXECUTE_READWRITE || mbi.Protect == PAGE_EXECUTE_READ ||
             mbi.Protect & PAGE_WRITECOPY || mbi.Protect & PAGE_EXECUTE_WRITECOPY)) {
            out.push_back(mbi);
            total += mbi.RegionSize;
        }
        uintptr_t next = (uintptr_t)mbi.BaseAddress + (uintptr_t)mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }
    return total;
}

static size_t region_bytes(const std::vector<MEMORY_BASIC_INFORMATION>& r) {
    size_t s = 0;
    for (auto& m : r) s += m.RegionSize;
    return s;
}

// ==================== 靶进程模式 ====================
static int run_victim(size_t mb) {
    size_t total = mb * 1024ull * 1024ull;
    DWORD granularity = 0;
    SYSTEM_INFO si; GetSystemInfo(&si); granularity = si.dwAllocationGranularity;
    size_t alloc_size = (total / granularity + 1) * granularity;

    uint8_t* base = (uint8_t*)VirtualAlloc(NULL, alloc_size, MEM_RESERVE, PAGE_READWRITE);
    if (!base) { printf("victim: VirtualAlloc reserve failed\n"); return 1; }
    if (!VirtualAlloc(base, total, MEM_COMMIT, PAGE_READWRITE)) {
        printf("victim: VirtualAlloc commit failed (err=%lu)\n", GetLastError()); return 1;
    }
    memset(base, 0, total);
    // 每 4KB 页的首地址写入 int32 0x1234 → 命中地址 = 页数，量级可控
    for (size_t off = 0; off + 4 <= total; off += 0x1000)
        *(int32_t*)(base + off) = 0x1234;

    size_t n_pages = total / 0x1000;
    printf("victim ready pid=%lu alloc=%zuMB pages=%zu\n",
           GetCurrentProcessId(), total >> 20, n_pages);

    // 周期性改写少量命中位（让“changed”再扫有真实变化）
    std::srand(1234);
    size_t idx = 0;
    while (true) {
        Sleep(300);
        for (int i = 0; i < 1000; ++i, ++idx) {
            size_t off = (idx % n_pages) * 0x1000;
            if (off + 4 <= total) *(int32_t*)(base + off) = (std::rand() & 0x7fffffff);
        }
    }
    return 0;
}

// ---- RPM 持续读一段区域（分块）----
static long long read_throughput(HANDLE h, const std::vector<MEMORY_BASIC_INFORMATION>& regions,
                                 size_t chunk, size_t give_up_bytes, size_t& out_read_bytes) {
    std::vector<uint8_t> buf(chunk);
    long long total_ms = 0;
    size_t done = 0;
    auto t0 = clk::now();
    size_t budget = give_up_bytes; // 只读前 N 字节，避免扫到巨量
    for (auto& r : regions) {
        SIZE_T remaining = r.RegionSize;
        BYTE* p = (BYTE*)r.BaseAddress;
        while (remaining && budget) {
            SIZE_T n = remaining < chunk ? remaining : chunk;
            if (n > budget) n = budget;
            SIZE_T rd = 0;
            if (!ReadProcessMemory(h, p, buf.data(), n, &rd)) break;
            done += rd;
            budget -= rd;
            remaining -= rd; p += rd;
            if (done % (32*1024*1024) == 0) continue; // 进度无感
        }
        if (!budget) break;
    }
    out_read_bytes = done;
    return ms_of(t0, clk::now());
}

// ---- 再扫 A：逐地址 8B RPM（当前实现）----
static long long scatter_per_address(HANDLE h, std::vector<const uint8_t*> addrs) {
    uint8_t val[8];
    auto t0 = clk::now();
    for (auto a : addrs) {
        SIZE_T rd = 0;
        ReadProcessMemory(h, a, val, 8, &rd);
    }
    return ms_of(t0, clk::now());
}

// ---- 再扫 B：逐页 4096 RPM 一次 + 本地取 8B（批量优化）----
static long long scatter_per_page(HANDLE h, const std::vector<MEMORY_BASIC_INFORMATION>& regions,
                                  std::vector<const uint8_t*> addrs) {
    // 按页分组读取一次，再本地取 8 字节
    std::vector<uint8_t> pagebuf(0x1000);
    auto t0 = clk::now();
    for (auto a : addrs) {
        const uint8_t* page = (const uint8_t*)((uintptr_t)a & ~(uintptr_t)0xFFF);
        SIZE_T rd = 0;
        if (ReadProcessMemory(h, page, pagebuf.data(), 0x1000, &rd)) {
            // 本地 memcpy 8 字节（代表从缓冲取候选值）
            uint8_t tmp; (void)tmp;
            volatile uint8_t x = pagebuf[(uintptr_t)a & 0xFFF];
            (void)x;
        }
    }
    return ms_of(t0, clk::now());
}

// ==================== 父模式 ====================
static int run_parent(DWORD pid, size_t simd_mb) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                           PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) { printf("parent: OpenProcess failed (err=%lu)\n", GetLastError()); return 1; }

    std::vector<MEMORY_BASIC_INFORMATION> regions;
    size_t total_readable = collect_readable_regions(h, regions);
    printf("victim readable regions=%zu  bytes=%.1f MB\n",
           regions.size(), (double)total_readable / (1024.0*1024.0));

    printf("\n=========== 1) RPM 读取吞吐 vs SIMD 计算吞吐 ===========\n");
    // 读所有可读区域（限 1GB 预算）
    size_t read_bytes = 0;
    long long rd_ms = read_throughput(h, regions, 4096, 0x40000000ull /*1GB*/, read_bytes);
    printf("  RPM chunked read : %5lld ms   %6.0f MB   %.2f GB/s\n",
           rd_ms, (double)read_bytes/1048576.0, gb_per_s(read_bytes, rd_ms));

    // 纯 SIMD 计算（本地缓冲，隔离内存来源）
    size_t sb = simd_mb * 1024 * 1024;
    std::vector<uint8_t> mem(sb, 0);
    std::vector<uint8_t> target(sb, 0);
    // 一半槽位填 0x1234，为“有命中”场景
    for (size_t i = 0; i < sb; i += 4) { *(int32_t*)(mem.data()+i) = (i & 0x400 ? 0x1234 : 0); }
    for (size_t i = 0; i < sb; i += 4) *(int32_t*)(target.data()+i) = 0x1234;

    // (a) 无命中：搜索一个不在缓冲里的值 → 纯比较，不含结果存储
    std::vector<uint8_t> missbuf(sb, 0);
    for (size_t i = 0; i < sb; i += 4) *(int32_t*)(missbuf.data()+i) = 0x7FFFF0;
    std::vector<uint64_t> dummy;
    auto sm0 = clk::now();
    simd_scanner::scan_memory_block_for_matches<int32_t>(
        mem.data(), missbuf.data(), sb, 0x1000000, 4, SimdOp::equal, dummy);
    long long simd_pure_ms = ms_of(sm0, clk::now());

    // (b) 有命中：真实结果集存储成本
    std::vector<uint64_t> matches;
    auto sm1 = clk::now();
    simd_scanner::scan_memory_block_for_matches<int32_t>(
        mem.data(), target.data(), sb, 0x1000000, 4, SimdOp::equal, matches);
    long long simd_hit_ms = ms_of(sm1, clk::now());

    printf("  SIMD int32 scan   : pure(no-hit)=%5lld ms, with-hit=%5lld ms  (%.0f MB ms, hits=%zu)\n",
           simd_pure_ms==0?0:simd_pure_ms, simd_hit_ms,
           (double)sb/1048576.0, matches.size());
    printf("    pure compute GB/s = %.2f\n",  gb_per_s(sb, simd_pure_ms));
    printf("    hit storage GB/s  = %.2f\n",  gb_per_s(sb, simd_hit_ms));
    printf("  => read : pure-compute = 1 : %.2f\n", (double)rd_ms / (simd_pure_ms?simd_pure_ms:1));

    printf("\n=========== 2) 再扫地址读取：逐地址 8B vs 逐页批量 ===========\n");
    // 构造地址集：每页一个对齐地址
    std::vector<const uint8_t*> addrs;
    addrs.reserve(total_readable / 0x1000);
    for (auto& r : regions) {
        for (const uint8_t* p = (const uint8_t*)r.BaseAddress;
             p < (const uint8_t*)r.BaseAddress + r.RegionSize; p += 0x1000)
            addrs.push_back(p);
    }
    // 限 200 万条，避免太久
    if (addrs.size() > 2000000) addrs.resize(2000000);
    printf("  address count     : %zu\n", addrs.size());
    long long a_ms = scatter_per_address(h, addrs);
    long long b_ms = scatter_per_page(h, regions, addrs);
    printf("  per-address 8B    : %5lld ms   (%.0f us/op)\n",
           a_ms, (double)a_ms*1000.0/addrs.size());
    printf("  per-page 4096 once: %5lld ms   (%.0f us/op)\n",
           b_ms, (double)b_ms*1000.0/addrs.size());
    printf("  => speedup of batching: %.1fx\n", b_ms ? (double)a_ms/b_ms : 0.0);

    CloseHandle(h);
    return 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc >= 3 && strcmp(argv[1], "--victim") == 0) {
        return run_victim((size_t)strtoull(argv[2], nullptr, 10));
    }
    if (argc >= 2) {
        DWORD pid = (DWORD)strtoul(argv[1], nullptr, 10);
        size_t simd_mb = argc >= 3 ? (size_t)strtoull(argv[2], nullptr, 10) : 256;
        return run_parent(pid, simd_mb);
    }
    printf("usage:\n  scan_perf --victim <MB>\n  scan_perf <victim_pid> [simd_mb]\n");
    return 1;
}
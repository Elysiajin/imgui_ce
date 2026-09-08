// scan_bench：对真实目标进程跑真实扫描流水线，分段计时，定位耗时大头。
// 用法: scan_bench <pid> [value]
#include "core/process_manager.h"
#include "scan/scan_engine.h"
#include "scan/process_memory_snapshot_manager.h"

#include <cstdio>
#include <cstdlib>
#include <chrono>

using clk = std::chrono::steady_clock;
static long long ms_of(clk::time_point a, clk::time_point b) {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: scan_bench <pid> [value]\n"); return 1; }
    const uint32_t pid = (uint32_t)strtoul(argv[1], nullptr, 10);
    const uint64_t value = (argc >= 3) ? strtoull(argv[2], nullptr, 0) : 0x1234ull;

    auto& pm = process_manager::instance();
    printf("attach pid=%lu ... ", (unsigned long)pid);
    fflush(stdout);
    if (!pm.attach(pid)) { printf("FAILED\n"); return 1; }
    printf("ok\n");

    scan_request req;
    req.mode       = scan_mode::first;
    req.data_type  = scan_data_type::int32;
    req.alignment  = 4;                       // 对应 UI 默认勾选 Fast Scan
    req.first_type = scan_type::exact_value;
    req.params     = value_params{ value, 0 };

    process_memory_snapshot_manager mgr;
    scan_engine eng(&mgr);

    // ---- 1) 首次扫描（现在走实时读 + 边读边匹配，不落全量快照文件）----
    auto t0 = clk::now();
    auto rep = eng.execute(req, {});
    auto t1 = clk::now();
    printf("[1] first scan (stream): %lld ms   (results=%zu)\n",
           ms_of(t0, t1), rep.results->total_size());

    // ---- 取回首轮结果 ----
    std::vector<scan_result> prev;
    {
        const size_t n = rep.results->total_size();
        prev.reserve(n);
        for (size_t off = 0; off < n; off += 65536) {
            auto page = rep.results->read_chunk(off, 65536);
            prev.insert(prev.end(), page.begin(), page.end());
        }
    }
    printf("    collected %zu results\n", prev.size());

    // ---- 2) 再次扫描（changed）----
    scan_request nreq = req;
    nreq.mode      = scan_mode::next;
    nreq.next_type = next_scan_type::changed;
    t0 = clk::now();
    auto rep2 = eng.execute(nreq, prev);
    t1 = clk::now();
    printf("[2] next scan (changed): %lld ms   (results=%zu)\n",
           ms_of(t0, t1), rep2.results->total_size());

    pm.detach();
    return 0;
}

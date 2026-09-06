// scan_result_store 压缩算法单测：往返一致性、随机访问、真实压缩率
// 编译：g++ -O2 -std=c++20 -I.. tests/scan_result_store_test.cpp -o store_test.exe
#include "../scan/scan_result_store.h"

#include <cassert>
#include <cstdio>
#include <random>
#include <set>
#include <vector>

static int g_failed = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++g_failed; \
    } \
} while (0)

// 生成升序地址序列
template<typename Gen>
static std::vector<scan_result> gen_results(size_t n, Gen next_addr, bool with_mask, std::mt19937& rng) {
    std::vector<scan_result> v;
    v.reserve(n);
    std::uniform_int_distribution<uint16_t> mask_gen(0, 0x3F);
    uint64_t addr = 0;
    for (size_t i = 0; i < n; ++i) {
        addr = next_addr(addr);
        scan_result r;
        r.address = addr;
        r.type_mask = with_mask ? mask_gen(rng) : 0;
        v.push_back(r);
    }
    return v;
}

// 通用校验：往返一致 + 随机访问一致
static void verify(const std::vector<scan_result>& input, const char* name, bool expect_gain) {
    scan_result_store store;
    store.build(input);

    // 1) 整表往返
    std::vector<scan_result> decoded;
    store.decode_all(decoded);
    CHECK(decoded.size() == input.size());
    bool all_equal = true;
    for (size_t i = 0; i < input.size(); ++i) {
        if (decoded[i].address != input[i].address || decoded[i].type_mask != input[i].type_mask) {
            all_equal = false;
            break;
        }
    }
    CHECK(all_equal);

    // 2) 随机访问（抽样 1000 个下标 + 全量首尾）
    std::mt19937 rng(7);
    std::uniform_int_distribution<size_t> pick(0, input.size() ? input.size() - 1 : 0);
    for (int t = 0; t < 1000 && !input.empty(); ++t) {
        size_t idx = pick(rng);
        scan_result r = store.at(idx);
        if (r.address != input[idx].address || r.type_mask != input[idx].type_mask) {
            std::printf("  [FAIL] random access mismatch at %zu\n", idx);
            ++g_failed;
            break;
        }
    }
    if (!input.empty()) {
        scan_result first = store.at(0), last = store.at(input.size() - 1);
        CHECK(first.address == input.front().address);
        CHECK(last.address == input.back().address);
    }

    // 3) 压缩率
    const double ratio = store.compression_ratio();
    const size_t raw = scan_result_store::raw_size_of(input.size());
    std::printf("  %-28s n=%-8zu raw=%-10zu enc=%-10zu ratio=%.2f mask=%d\n",
                name, input.size(), raw, store.encoded_size(), ratio,
                (int)store.uses_mask_arrays());
    if (expect_gain)
        CHECK(ratio < 1.0);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::mt19937 rng(42);

    // 边界：空
    {
        std::vector<scan_result> empty;
        scan_result_store s;
        s.build(empty);
        CHECK(s.size() == 0);
        CHECK(s.encoded_size() == 0);
        std::vector<scan_result> out;
        s.decode_all(out);
        CHECK(out.empty());
        std::printf("  empty                        OK\n");
    }

    // 边界：单条
    {
        std::vector<scan_result> one = {{0x7FF600001234ull, 0}};
        verify(one, "single entry", true);
    }

    // 密集：堆数组式，步长 4，同一批 64KB 页里大量命中（精确值扫描的典型分布）
    {
        uint64_t addr = 0x4C1C00100000ull;
        auto res = gen_results(200000, [&](uint64_t) { addr += 4; return addr; }, false, rng);
        verify(res, "dense stride-4 (no mask)", true);
        // 密集无 mask：每条应压到约 2 字节，断言明显收益
        scan_result_store s; s.build(res);
        CHECK(s.compression_ratio() < 0.30);
    }

    // 密集 + 带 mask（All 扫描典型分布）
    {
        uint64_t addr = 0x4C1C00200000ull;
        auto res = gen_results(200000, [&](uint64_t) { addr += 4; return addr; }, true, rng);
        verify(res, "dense stride-4 (mask)", true);
        scan_result_store s; s.build(res);
        CHECK(s.compression_ratio() < 0.50);
    }

    // 稀疏：随机 48 位地址（跨模块零散命中，多数块会退到 4 字节偏移）
    {
        std::uniform_int_distribution<uint64_t> gap(1, 0x100000); // 平均 ~0.5MB 间隔
        uint64_t addr = 0x10000000ull;
        auto res = gen_results(200000, [&](uint64_t) { addr += gap(rng); return addr; }, false, rng);
        verify(res, "sparse random (no mask)", true);
        scan_result_store s; s.build(res);
        CHECK(s.compression_ratio() < 0.70); // 6B/10B = 0.6 上限
    }

    // 跨 4GB 大间隔（切断成多个块，偏移不回绕）
    {
        uint64_t addr = 0x1000ull;
        auto res = gen_results(5000, [&](uint64_t) { addr += 0x180000000ull; return addr; }, false, rng);
        verify(res, ">4GB gaps", true);
        // 校验地址解码无回绕：抽全量
        scan_result_store s; s.build(res);
        std::vector<scan_result> dec; s.decode_all(dec);
        bool ok = true;
        for (size_t i = 0; i < res.size(); ++i)
            if (dec[i].address != res[i].address) { ok = false; break; }
        CHECK(ok);
    }

    // 混合：密集簇 + 稀疏跳变（最接近真实扫描）
    {
        std::uniform_int_distribution<uint64_t> cluster_gap(4, 64);
        std::uniform_int_distribution<uint64_t> cluster_jump(0x10000, 0x1000000);
        uint64_t addr = 0x7FF000000000ull;
        auto res = gen_results(150000, [&](uint64_t) {
            addr += (rng() % 8 == 0) ? cluster_jump(rng) : cluster_gap(rng);
            return addr;
        }, true, rng);
        verify(res, "mixed clusters (mask)", true);
    }

    // 块边界：恰好 4096 / 4097 条
    for (size_t n : { (size_t)4096, (size_t)4097 }) {
        uint64_t addr = 0x100000ull;
        auto res = gen_results(n, [&](uint64_t) { addr += 4; return addr; }, true, rng);
        scan_result_store s; s.build(res);
        std::vector<scan_result> dec; s.decode_all(dec);
        bool ok = dec.size() == n;
        for (size_t i = 0; ok && i < n; ++i)
            if (dec[i].address != res[i].address || dec[i].type_mask != res[i].type_mask) ok = false;
        CHECK(ok);
        // 随机访问边界：块首/块尾
        scan_result r0 = s.at(0), rlast = s.at(n - 1);
        CHECK(r0.address == res[0].address);
        CHECK(rlast.address == res[n - 1].address);
        scan_result r4095 = s.at(4095), r4096 = s.at(4096 % n);
        CHECK(r4095.address == res[4095].address);
        CHECK(r4096.address == res[4096 % n].address);
        std::printf("  block boundary n=%-8zu  OK (ratio=%.3f)\n", n, s.compression_ratio());
    }

    if (g_failed == 0) std::printf("ALL STORE TESTS PASSED\n");
    else std::printf("%d CHECK(S) FAILED\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}

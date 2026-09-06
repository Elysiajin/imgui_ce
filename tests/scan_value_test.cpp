// float/double 扫描值管线单测：输入解析 → 目标位还原 → SIMD 匹配
// 编译：g++ -mavx2 -std=c++20 -I.. tests/scan_value_test.cpp -o scan_value_test.exe
#include "../scan/scan_simd_accelerate.h"
#include "../scan/scan_value_parser.h"
#include "../scan/scan_value_target.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <set>

static int g_failed = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++g_failed; \
    } \
} while (0)

// 构造一个 scan_request（value_params，exact_value）
static scan_request make_exact_request(scan_data_type dt, uint64_t bits) {
    scan_request req;
    req.mode = scan_mode::first;
    req.data_type = dt;
    req.first_type = scan_type::exact_value;
    req.contain_approximate_value = false;
    req.not_match = false;
    value_params vp;
    vp.value1 = bits;
    vp.value2 = 0;
    req.params = vp;
    return req;
}

// ───────────────────────── 1. 输入解析 ─────────────────────────
static void test_parse_float() {
    std::printf("[test] parse float/double input\n");
    uint64_t out = 0;

    // 十进制 "1000"（Hex 不勾选）→ float 1000.0f 位模式 0x447A0000
    CHECK(parse_scan_value("1000", scan_data_type::float32, false, out));
    {
        float f; std::memcpy(&f, &out, sizeof(f));
        CHECK(f == 1000.0f);
        CHECK(out == 0x447A0000ull);
    }
    // double "1000" → double 1000.0 位模式 0x408F400000000000
    CHECK(parse_scan_value("1000", scan_data_type::float64, false, out));
    {
        double d; std::memcpy(&d, &out, sizeof(d));
        CHECK(d == 1000.0);
        CHECK(out == 0x408F400000000000ull);
    }
    // 小数 / 负数 / 科学计数
    CHECK(parse_scan_value("-3.14", scan_data_type::float32, false, out));
    { float f; std::memcpy(&f, &out, sizeof(f)); CHECK(std::fabs(f + 3.14f) < 1e-6f); }
    CHECK(parse_scan_value("2.5e3", scan_data_type::float64, false, out));
    { double d; std::memcpy(&d, &out, sizeof(d)); CHECK(d == 2500.0); }
    // 非法输入
    CHECK(!parse_scan_value("abc", scan_data_type::float32, false, out));
    CHECK(!parse_scan_value("", scan_data_type::float32, false, out));
    // Hex 勾选（CE 兼容）：按十六进制数值解析，1000h = 4096.0f
    CHECK(parse_scan_value("1000", scan_data_type::float32, true, out));
    { float f; std::memcpy(&f, &out, sizeof(f)); CHECK(f == 4096.0f); }
    // 整数路径不受影响
    CHECK(parse_scan_value("1000", scan_data_type::int32, false, out));
    CHECK(out == 1000);
    CHECK(parse_scan_value("1000", scan_data_type::int32, true, out));
    CHECK(out == 0x1000);
}

// ───────────────── 2. 首次扫描目标位还原 ─────────────────
static void test_first_targets() {
    std::printf("[test] build_first_scan_targets float/double\n");
    uint64_t bits = 0;
    CHECK(parse_scan_value("1000", scan_data_type::float32, false, bits));
    scan_request req = make_exact_request(scan_data_type::float32, bits);

    float v1 = 0, v2 = 0;
    bool approx = build_first_scan_targets<float>(req, v1, v2);
    CHECK(!approx);
    CHECK(v1 == 1000.0f);   // ← 核心断言：目标必须是浮点 1000.0f，而不是把 0x447A0000 当整数

    // double
    CHECK(parse_scan_value("1000", scan_data_type::float64, false, bits));
    scan_request reqd = make_exact_request(scan_data_type::float64, bits);
    double d1 = 0, d2 = 0;
    build_first_scan_targets<double>(reqd, d1, d2);
    CHECK(d1 == 1000.0);

    // Between：v1/v2 为区间
    uint64_t b1 = 0, b2 = 0;
    parse_scan_value("10", scan_data_type::float32, false, b1);
    parse_scan_value("20", scan_data_type::float32, false, b2);
    scan_request reqb = make_exact_request(scan_data_type::float32, b1);
    reqb.first_type = scan_type::between;
    { value_params vp; std::get<value_params>(reqb.params).value2 = b2; (void)vp; }
    std::get<value_params>(reqb.params).value2 = b2;
    build_first_scan_targets<float>(reqb, v1, v2);
    CHECK(v1 == 10.0f && v2 == 20.0f);

    // 近似值：1000 ±1% → 约 [990, 1010]（浮点舍入，用宽松断言）
    uint64_t fbits = 0;
    CHECK(parse_scan_value("1000", scan_data_type::float32, false, fbits));
    scan_request reqa = make_exact_request(scan_data_type::float32, fbits);
    reqa.contain_approximate_value = true;
    build_first_scan_targets<float>(reqa, v1, v2);
    CHECK(v1 < 999.0f && v2 > 1001.0f);
}

// ───────────────── 3. SIMD float/double 匹配内核 ─────────────────
static void test_simd_float_match() {
    std::printf("[test] simd float exact match\n");
    // 64KB 缓冲，按 4 字节填充已知 float 序列
    const size_t N = 64 * 1024 / sizeof(float);
    std::vector<float> buf(N);
    std::set<uint64_t> expect;
    for (size_t i = 0; i < N; ++i) {
        switch (i % 8) {
        case 0: buf[i] = 1000.0f; expect.insert(0x10000000ull + i * 4); break; // 命中
        case 1: buf[i] = 999.0f;  break;
        case 2: buf[i] = 0.0f;    break;
        case 3: buf[i] = -1000.0f; break;
        case 4: buf[i] = 1000.5f; break;
        case 5: buf[i] = std::nanf(""); break;
        case 6: buf[i] = 1e-42f;  break;   // denormal
        case 7: buf[i] = 1e38f;   break;
        }
    }
    const uint8_t* mem = reinterpret_cast<const uint8_t*>(buf.data());
    std::vector<uint8_t> target(64 * 1024);
    { float t = 1000.0f; for (size_t i = 0; i < target.size(); i += 4) std::memcpy(target.data() + i, &t, 4); }

    std::vector<uint64_t> matched;
    std::printf("  calling simd float scan (64KB)...\n");
    simd_scanner::scan_memory_block_for_matches<float>(mem, target.data(), 64 * 1024,
        0x10000000ull, 4, SimdOp::equal, matched);
    std::printf("  matched=%zu expect=%zu\n", matched.size(), expect.size());

    CHECK(matched.size() == expect.size());
    for (auto a : matched) CHECK(expect.count(a) == 1);

    // double 同样验证
    std::printf("[test] simd double exact match\n");
    const size_t M = 64 * 1024 / sizeof(double);
    std::vector<double> dbuf(M);
    std::set<uint64_t> dexpect;
    for (size_t i = 0; i < M; ++i) {
        switch (i % 4) {
        case 0: dbuf[i] = 1000.0; dexpect.insert(0x20000000ull + i * 8); break;
        case 1: dbuf[i] = 999.0; break;
        case 2: dbuf[i] = 0.0; break;
        case 3: dbuf[i] = std::nan(""); break;
        }
    }
    std::vector<uint8_t> dtarget(64 * 1024);
    { double t = 1000.0; for (size_t i = 0; i < dtarget.size(); i += 8) std::memcpy(dtarget.data() + i, &t, 8); }
    std::vector<uint64_t> dmatched;
    simd_scanner::scan_memory_block_for_matches<double>(reinterpret_cast<const uint8_t*>(dbuf.data()),
        dtarget.data(), 64 * 1024, 0x20000000ull, 8, SimdOp::equal, dmatched);
    CHECK(dmatched.size() == dexpect.size());
    for (auto a : dmatched) CHECK(dexpect.count(a) == 1);

    // 1 字节对齐（Fast Scan 关闭）：每个 1000.0f 的 4 个字节偏移都会命中
    std::printf("[test] simd float alignment=1\n");
    std::vector<uint64_t> amatched;
    simd_scanner::scan_memory_block_for_matches<float>(mem, target.data(), 64 * 1024,
        0x10000000ull, 1, SimdOp::equal, amatched);
    CHECK(amatched.size() >= expect.size());
    for (auto a : expect) {
        bool found = false;
        for (auto m : amatched) if (m >= a && m < a + 4) { found = true; break; }
        CHECK(found); // 原命中地址附近（+0..3）必有记录
    }
}

// ───────────────── 4. 端到端：解析 → 目标 → 匹配 ─────────────────
static void test_end_to_end_float_scan() {
    std::printf("[test] end-to-end: parse '1000' as Float, match against memory\n");
    uint64_t bits = 0;
    CHECK(parse_scan_value("1000", scan_data_type::float32, false, bits));
    scan_request req = make_exact_request(scan_data_type::float32, bits);
    float v1 = 0, v2 = 0;
    build_first_scan_targets<float>(req, v1, v2);

    // 模拟目标进程内存：一个含 float health=1000 的结构数组
    std::vector<float> mem = { 0.0f, 1000.0f, 999.5f, 1000.0f, 1001.0f, 1000.0f };
    std::vector<uint8_t> tbuf(mem.size() * 4);
    for (size_t i = 0; i < tbuf.size(); i += 4) std::memcpy(tbuf.data() + i, &v1, 4);

    std::vector<uint64_t> matched;
    simd_scanner::scan_memory_block_for_matches<float>(reinterpret_cast<const uint8_t*>(mem.data()),
        tbuf.data(), tbuf.size(), 0x400000ull, 4, SimdOp::equal, matched);
    CHECK(matched.size() == 3);
    if (matched.size() == 3) {
        CHECK(matched[0] == 0x400004ull);
        CHECK(matched[1] == 0x40000Cull);
        CHECK(matched[2] == 0x400014ull);
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_parse_float();
    test_first_targets();
    test_simd_float_match();
    test_end_to_end_float_scan();
    if (g_failed == 0) std::printf("ALL TESTS PASSED\n");
    else std::printf("%d CHECK(S) FAILED\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}

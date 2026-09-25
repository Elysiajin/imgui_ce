#include "scan/process_memory_snapshot_manager.h"
#include "scan/win32_process_memory_snapshot.h"
#include "scan/sparse_memory_snapshot.h"
#include "scan/live_process_memory_snapshot.h"
#include "scan/thread_pool.h"
#include "core/process_manager.h"
#include "scan/temp_path_manager.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

// 尽力读：整块读失败时二分降级到 4KB，能救回多少算多少，读不到的部分填 0。
// （旧实现是一失败就把整块 64KB 全部写 0，会把同一块里的有效字节一起丢掉。）
size_t read_best_effort(IMemoryAccessor* acc, uint64_t addr, uint8_t* dst, size_t size)
{
    constexpr size_t k_min_block = 4096;
    if (acc->read(addr, dst, size))
        return size;
    if (size <= k_min_block) {
        std::memset(dst, 0, size);
        return size;
    }
    size_t half = (size / 2) & ~(k_min_block - 1);
    if (half < k_min_block)
        half = k_min_block;
    const size_t a = read_best_effort(acc, addr, dst, half);
    const size_t b = read_best_effort(acc, addr + half, dst + half, size - half);
    return a + b;
}

std::string make_unique_snapshot_path()
{
    static std::atomic<uint32_t> seq{0};
    const auto dir = std::filesystem::path(temp_path_manager::get_work_dir());
    return (dir / ("snapshot_" + std::to_string(GetCurrentProcessId()) + "_" +
                   std::to_string(seq.fetch_add(1)) + ".bin")).string();
}

} // namespace

std::shared_ptr<i_process_memory_snapshot>
process_memory_snapshot_manager::create_snapshot(const std::vector<memory_region>& regions,
                                                 const progress_fn& on_progress)
{
    // ── 1. 先算好每个 region 在文件中的偏移（连续排放），并得到总大小 ──
    std::map<uint64_t, size_t> index;
    uint64_t total_bytes = 0;
    for (const auto& reg : regions) {
        if (reg.size == 0) continue;
        index[reg.base] = static_cast<size_t>(total_bytes);
        total_bytes += reg.size;
    }

    const std::string path = make_unique_snapshot_path();

    // ── 2. 临时映射文件：不落盘 + 关闭即删除 ──
    //   FILE_ATTRIBUTE_TEMPORARY   → 系统尽量把页面留在系统缓存，不写回磁盘
    //   FILE_FLAG_DELETE_ON_CLOSE  → 最后一个句柄关闭时自动删除（崩溃也不残留）
    //   FILE_FLAG_SEQUENTIAL_SCAN  → 提示顺序访问，加大预读
    HANDLE h_file = CreateFileA(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (h_file == INVALID_HANDLE_VALUE)
        return nullptr;

    auto* accessor = process_manager::instance().memory();
    if (!accessor) {
        CloseHandle(h_file);
        return nullptr;
    }

    // 预扩到最终大小：后面各线程按自己的偏移写入，互不覆盖；
    // 没写到的空洞读出来是 0，正好等价于"读不到的内存按 0 处理"。
    LARGE_INTEGER end;
    end.QuadPart = static_cast<LONGLONG>(total_bytes);
    if (total_bytes > 0 &&
        (!SetFilePointerEx(h_file, end, nullptr, FILE_BEGIN) || !SetEndOfFile(h_file))) {
        CloseHandle(h_file);
        return nullptr;
    }

    // ── 3. 并行抓内存（ReadProcessMemory 是耗时大头），写盘串行 ──
    // 每个 worker 自带 1MB 缓冲；写时用互斥锁 + 显式定位，保证顺序写入不互相踩。
    constexpr size_t k_read_chunk = 1024 * 1024;

    std::mutex write_mtx;
    std::atomic<uint64_t> reported_bytes{0};
    std::vector<std::future<void>> futures;
    futures.reserve(regions.size());

    for (const auto& reg : regions) {
        if (reg.size == 0) continue;
        futures.push_back(global_thread_pool::instance().enqueue(
            [this, &reg, &index, h_file, accessor, &write_mtx, &reported_bytes, &on_progress] {
                // ★ 缓冲按"本区域实际大小"分配，且用 new[] 不做零初始化：
                //   旧写法对每条区域都固定申请并 memset 1MB，几百条区域就是几百 MB
                //   白白清零，纯属浪费。
                const size_t buf_cap = static_cast<size_t>(
                    reg.size < k_read_chunk ? reg.size : k_read_chunk);
                std::unique_ptr<uint8_t[]> buf(new uint8_t[buf_cap]);
                uint64_t file_pos   = index.at(reg.base);
                uint64_t addr       = reg.base;
                uint64_t remaining  = reg.size;

                while (remaining > 0) {
                    const size_t want = static_cast<size_t>(
                        remaining < buf_cap ? remaining : buf_cap);

                    read_best_effort(accessor, addr, buf.get(), want);

                    {
                        std::lock_guard<std::mutex> lock(write_mtx);
                        LARGE_INTEGER pos;
                        pos.QuadPart = static_cast<LONGLONG>(file_pos);
                        SetFilePointerEx(h_file, pos, nullptr, FILE_BEGIN);
                        DWORD written = 0;
                        WriteFile(h_file, buf.get(), static_cast<DWORD>(want), &written, nullptr);
                    }

                    addr      += want;
                    file_pos  += want;
                    remaining -= want;

                    // 每抓满一个进度单位上报一次
                    if (on_progress) {
                        const uint64_t before = reported_bytes.fetch_add(want);
                        const uint64_t after  = before + want;
                        if (after / k_progress_unit_bytes > before / k_progress_unit_bytes)
                            on_progress(static_cast<int>(after / k_progress_unit_bytes -
                                                         before / k_progress_unit_bytes));
                    }
                }
            }));
    }

    for (auto& f : futures) {
        if (f.valid()) f.get();
    }

    return std::make_shared<win32_process_memory_snapshot>(h_file, path,
                                                           std::move(index), total_bytes);
}

std::shared_ptr<i_process_memory_snapshot>
process_memory_snapshot_manager::create_live_snapshot()
{
    return std::make_shared<live_process_memory_snapshot>();
}

std::shared_ptr<i_process_memory_snapshot>
process_memory_snapshot_manager::create_sparse_snapshot(
    const std::vector<uint64_t>& addresses,
    size_t value_size,
    const std::shared_ptr<i_process_memory_snapshot>& src)
{
    if (addresses.empty() || !src || value_size == 0 || value_size > 8)
        return std::make_shared<sparse_memory_snapshot>(
            std::vector<scan_result>{}, std::vector<uint8_t>{}, value_size);

    // 1. 排序去重（scan_result_store 的 PE 表编码依赖升序）
    std::vector<uint64_t> sorted = addresses;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

    // 2. 分片并行读取每个地址的 value_size 字节值；读不到的地址丢弃。
    //    地址与值分别产出，按分片顺序合并，保证 index 对齐。
    constexpr size_t k_slice = 8192;
    const size_t slices = (sorted.size() + k_slice - 1) / k_slice;
    std::vector<std::vector<scan_result>> partial_res(slices);
    std::vector<std::vector<uint8_t>>     partial_val(slices);
    std::vector<std::future<void>> futures;
    futures.reserve(slices);

    for (size_t s = 0; s < slices; ++s) {
        const size_t begin = s * k_slice;
        const size_t end   = (std::min)(begin + k_slice, sorted.size());
        futures.push_back(global_thread_pool::instance().enqueue(
            [&sorted, src, value_size, begin, end, &partial_res, &partial_val, s] {
                auto& rv = partial_res[s];
                auto& vv = partial_val[s];
                rv.reserve(end - begin);
                vv.reserve((end - begin) * value_size);
                uint8_t tmp[8] = {};
                for (size_t i = begin; i < end; ++i) {
                    if (!src->read_data(sorted[i], tmp, value_size))
                        continue;                       // 读不到的跳过
                    rv.push_back({ sorted[i], 0 });
                    vv.insert(vv.end(), tmp, tmp + value_size);
                }
            }));
    }
    for (auto& f : futures)
        if (f.valid()) f.get();

    std::vector<scan_result> results;
    std::vector<uint8_t> values;
    size_t total = 0;
    for (const auto& rv : partial_res) total += rv.size();
    results.reserve(total);
    values.reserve(total * value_size);
    for (size_t s = 0; s < slices; ++s) {
        results.insert(results.end(), partial_res[s].begin(), partial_res[s].end());
        values.insert(values.end(), partial_val[s].begin(), partial_val[s].end());
    }

    return std::make_shared<sparse_memory_snapshot>(
        std::move(results), std::move(values), value_size);
}

void process_memory_snapshot_manager::clear() {
    // 快照文件带 DELETE_ON_CLOSE，最后一个 shared_ptr 释放时句柄关闭即自动删除，
    // 这里不再手动 remove（手动删对"仍打开"的文件也只会失败）。
    std::lock_guard<std::mutex> lock(m_snap_mtx);
    m_first.reset();
    m_prev.reset();
}

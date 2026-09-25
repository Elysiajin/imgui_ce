#pragma once
#include "scan/iprocess_memory_snapshot.h"
#include "type/memory_region.h"

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class process_memory_snapshot_manager {
public:
    // 快照构建过程中每完成 k_progress_unit_bytes 字节回调一次（用于驱动进度条），
    // 传 nullptr 表示不需要进度上报。
    using progress_fn = std::function<void(int units)>;

    // 进度上报粒度：每抓完 8MB 目标进程内存报一次
    static constexpr uint64_t k_progress_unit_bytes = 8ull * 1024 * 1024;

    explicit process_memory_snapshot_manager() = default;

    std::shared_ptr<i_process_memory_snapshot> create_snapshot(
        const std::vector<memory_region>& regions,
        const progress_fn& on_progress = nullptr);

    // 只保存 addresses 上当前值的稀疏快照（用于下一轮"上次值"比较）。
    // value_size 是每个地址要保存的字节数，必须与实际扫描读取的宽度一致。
    std::shared_ptr<i_process_memory_snapshot> create_sparse_snapshot(
        const std::vector<uint64_t>& addresses,
        size_t value_size,
        const std::shared_ptr<i_process_memory_snapshot>& src);

    // 再次扫描且已有结果集时，不需要整份内存快照
    std::shared_ptr<i_process_memory_snapshot> create_live_snapshot();

    // m_first / m_prev 由扫描工作线程写入（scan_engine::execute），UI 线程在
    // 渲染结果表时逐行 get_*_value 拷贝读取 —— shared_ptr 的并发读写不是
    // 原子操作（引用计数竞争会堆损坏崩溃），必须经 m_snap_mtx 保护。
    void set_first_snapshot(std::shared_ptr<i_process_memory_snapshot> snapshot) {
        std::lock_guard<std::mutex> lock(m_snap_mtx);
        m_first = snapshot;
    }
    void set_previous_snapshot(std::shared_ptr<i_process_memory_snapshot> snapshot) {
        std::lock_guard<std::mutex> lock(m_snap_mtx);
        m_prev = snapshot;
    }

    std::shared_ptr<i_process_memory_snapshot> get_first_process_memory_snapshot() const {
        std::lock_guard<std::mutex> lock(m_snap_mtx);
        return m_first;
    }
    std::shared_ptr<i_process_memory_snapshot> get_previous_process_memory_snapshot() const {
        std::lock_guard<std::mutex> lock(m_snap_mtx);
        return m_prev;
    }

    void clear();

private:
    std::shared_ptr<i_process_memory_snapshot> m_first = nullptr;
    std::shared_ptr<i_process_memory_snapshot> m_prev  = nullptr;
    mutable std::mutex m_snap_mtx;   // 保护 m_first / m_prev（工作线程写，UI 线程读）
};

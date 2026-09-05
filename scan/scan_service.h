#pragma once
#include "type/scan_data_stream_define.h"
#include "scan/scan_data_provider.h"
#include "scan/scan_result_repository.h"
#include "scan/process_memory_snapshot_manager.h"
#include "scan/scan_engine.h"

#include <atomic>
#include <memory>
#include <thread>
#include <mutex>

// 扫描服务
//   - 后台线程执行扫描（engine 内部再用线程池分块）
//   - 持有 SnapshotManager / Engine / Repository / DataProvider
//   - 每次扫描前把当前结果快照为"上一次扫描"，支持撤销
//   - UI 通过轮询 is_scanning()/pending_scan_done() 感知完成
class scan_service {
public:
    static scan_service& instance();

    scan_service(const scan_service&) = delete;
    scan_service& operator=(const scan_service&) = delete;

    void start_scan(const scan_request& request);
    void cancel();

    bool is_scanning() const { return m_scanning.load(std::memory_order_acquire); }
    bool has_results() const;
    int  total_results() const;
    int  potential_address_count() const { return m_engine ? m_engine->potential_address_count() : 0; }
    // 扫描进度（委托给引擎），0..1
    float progress() const;
    int   total_items() const { return m_engine ? m_engine->total_items() : 0; }
    int   progress_items() const { return m_engine ? m_engine->progress() : 0; }
    bool is_unknown_initial_mode() const { return m_expect_empty_results.load(); }

    // 已完成的扫描是否已被消费（UI 读取后调用 consume_scan_done）
    bool scan_finished() { return m_scan_finished.load(std::memory_order_acquire); }
    void consume_scan_done() { m_scan_finished.store(false, std::memory_order_release); }

    scan_data_provider* get_data_provider() const { return m_data_provider.get(); }
    scan_result_repository* get_repository() const { return m_repository.get(); }

    bool has_previous_results() const;
    bool restore_previous_results();

    void clear();
    void reset();

private:
    scan_service();
    ~scan_service();

    void on_scan_finished();

    std::shared_ptr<process_memory_snapshot_manager> m_process_snapshot_manager;
    std::unique_ptr<scan_data_provider>     m_data_provider;
    std::unique_ptr<scan_engine>           m_engine;
    std::unique_ptr<scan_result_repository> m_repository;

    std::thread m_worker;
    std::atomic<bool> m_scanning{ false };
    std::atomic<bool> m_scan_finished{ false };
    std::atomic<bool> m_expect_empty_results{ false };
    std::mutex m_restore_mutex;
};

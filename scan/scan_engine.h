#pragma once
#include "type/scan_data_stream_define.h"
#include "scan/adaptive_cache.h"
#include "scan/process_memory_snapshot_manager.h"
#include "scan/scan_simd_accelerate.h"
#include "scan/thread_pool.h"
#include "core/process_manager.h"

#include <atomic>
#include <memory>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cwctype>

class scan_engine {
public:
    struct scan_report {
        std::shared_ptr<adaptive_cache_pool<scan_result>> results;
        scan_data_type data_type;
    };

    scan_engine() = delete;
    scan_engine(process_memory_snapshot_manager* process_snapshot_manager);
    ~scan_engine() = default;

    scan_report execute(const scan_request& request, const std::vector<scan_result>& prev_results);

    void cancel() { m_cancel.store(true, std::memory_order_release); }

    void clear()
    {
        cancel();
        m_progress.store(0);
        m_total_items.store(0);
        m_potential_address.store(0);
    }

    bool is_cancelled() const { return m_cancel.load(std::memory_order_acquire); }
    int progress() const { return m_progress.load(std::memory_order_relaxed); }
    int total_items() const { return m_total_items.load(); }
    int potential_address_count() const { return m_potential_address.load(std::memory_order_relaxed); }

private:
    inline std::vector<std::pair<uint64_t, size_t>> get_readable_pages() {
        std::vector<std::pair<uint64_t, size_t>> pages;
        auto regions = process_manager::instance().get_memory_regions();
        const size_t page_size = 0x1000;

        for (const auto& reg : regions) {
            for (uint64_t addr = reg.base; addr < reg.base + reg.size; addr += page_size) {
                size_t size = std::min(page_size, reg.size - (addr - reg.base));
                pages.emplace_back(addr, size);
            }
        }
        return pages;
    }

    inline bool compare_byte_insensitive(uint8_t a, uint8_t b) {
        return std::tolower(static_cast<int>(a)) == std::tolower(static_cast<int>(b));
    }

    inline bool compare_utf16_insensitive(uint16_t a, uint16_t b) {
        return std::towlower(a) == std::towlower(b);
    }

    template <typename T>
    void dispatch_scan(const scan_request& req, const std::vector<scan_result>& prev_results,
        std::shared_ptr<adaptive_cache_pool<scan_result>> out_cache);

    void dispatch_all_scan(const scan_request& req, const std::vector<scan_result>& prev_results,
        std::shared_ptr<adaptive_cache_pool<scan_result>> out_cache);

    void task_first_scan_all(const scan_request& req, memory_region region,
        std::shared_ptr<i_process_memory_snapshot> current_snap,
        std::shared_ptr<adaptive_cache_pool<scan_result>> out_cache);

    void task_next_scan_all(const scan_request& req,
        const std::vector<scan_result>& old_batch,
        std::shared_ptr<i_process_memory_snapshot> current_snapshot,
        std::shared_ptr<i_process_memory_snapshot> previous_snapshot,
        std::shared_ptr<adaptive_cache_pool<scan_result>> out_cache);

    void task_full_scan_with_next_condition_all(const scan_request& req, memory_region region,
        std::shared_ptr<i_process_memory_snapshot> current_snapshot,
        std::shared_ptr<i_process_memory_snapshot> previous_snapshot,
        std::shared_ptr<adaptive_cache_pool<scan_result>> out_cache);

    template <typename T>
    void task_first_scan(const scan_request& req, memory_region region,
        std::shared_ptr<i_process_memory_snapshot> current_snapshot,
        std::shared_ptr<adaptive_cache_pool<scan_result>> out_cache);

    template <typename T>
    void task_next_scan(const scan_request& req,
        const std::vector<scan_result>& old_batch,
        std::shared_ptr<i_process_memory_snapshot> current_snapshot,
        std::shared_ptr<i_process_memory_snapshot> previous_snapshot,
        std::shared_ptr<adaptive_cache_pool<scan_result>> out_cache);

    template <typename T>
    void task_full_scan_with_next_condition(const scan_request& req, memory_region region,
        std::shared_ptr<i_process_memory_snapshot> current_snapshot,
        std::shared_ptr<i_process_memory_snapshot> previous_snapshot,
        std::shared_ptr<adaptive_cache_pool<scan_result>> out_cache);

    void perform_string_search(const std::vector<uint8_t>& buf, uint64_t base, const string_params& p, scan_data_type type, std::vector<uint64_t>& matched);
    void perform_aob_search(const std::vector<uint8_t>& buf, uint64_t base, const aob_params& p, std::vector<uint64_t>& matched);

    mutable std::mutex m_stats_mutex;
    std::atomic<bool> m_cancel{ false };
    std::atomic<int>  m_progress{ 0 };
    std::atomic<int>  m_total_items{ 0 };

    std::atomic<int> m_potential_address{ 0 };
    process_memory_snapshot_manager* m_process_snapshot_manager;
};

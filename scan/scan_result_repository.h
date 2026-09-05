#pragma once
#include "type/scan_data_stream_define.h"
#include "scan/adaptive_cache.h"
#include <vector>
#include <atomic>
#include <mutex>
#include <memory>

struct scan_metadata {
    int scanned_regions = 0;
    int scanned_addresses = 0;
    int matched_addresses = 0;
    size_t total_bytes = 0;
    scan_mode scan_mode = scan_mode::first;
    scan_type scan_type = scan_type::exact_value;
    bool is_first_unknown_scan = false;
    bool is_completed = false;
};

struct previous_scan_snapshot {
    std::vector<scan_result> results;
};

constexpr size_t POOL_MEMORY_THRESHOLD = 500'000;

class scan_result_repository {
public:
    void replace_all_results(std::vector<scan_result>&& new_results);
    void replace_all_results_from_pool(std::shared_ptr<adaptive_cache_pool<scan_result>> pool);

    size_t get_result_count() const;
    const scan_result* get_result_at(size_t index) const;
    uint64_t get_address_at_index(size_t index) const;
    scan_data_type get_matched_type_at_index(size_t index) const;
    int get_current_generation() const;
    size_t get_result_count_relaxed() const;
    std::vector<scan_result> read_pool_chunk(size_t start, size_t count) const;
    std::vector<uint64_t> read_address_range(size_t start, size_t count) const;
    std::vector<scan_result> get_results() const;

    void clear();
    void set_scan_metadata(const scan_metadata& meta);
    const scan_metadata& get_scan_metadata() const;
    void clear_scan_metadata();

    void save_as_previous_results();
    bool has_previous_results() const;
    bool swap_with_previous();
    void clear_previous_results();

private:
    std::vector<scan_result> m_result_data;
    std::shared_ptr<adaptive_cache_pool<scan_result>> m_result_pool;

    mutable std::mutex m_mutex;
    std::atomic<int> m_generation{ 0 };

    scan_metadata m_metadata;

    previous_scan_snapshot m_previous_scan_result_snapshot;
    mutable std::mutex m_prev_mutex;
};

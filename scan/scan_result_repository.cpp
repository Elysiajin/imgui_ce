#include "scan\scan_result_repository.h"
#include <cstring>

static scan_data_type type_mask_to_primary_type(uint16_t mask) {
    static constexpr int k_small_first[] = { 0, 1, 2, 4, 3, 5 };
    static constexpr scan_data_type k_type_map[] = {
        scan_data_type::int8,    // 0
        scan_data_type::int16,   // 1
        scan_data_type::int32,   // 2
        scan_data_type::int64,   // 3
        scan_data_type::float32, // 4
        scan_data_type::float64  // 5
    };
    if (mask == 0) return scan_data_type::int32;
    for (int ti : k_small_first) {
        if (mask & (1 << ti)) {
            return k_type_map[ti];
        }
    }
    return scan_data_type::int32;
}

void scan_result_repository::replace_all_results(std::vector<scan_result>&& new_results) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_result_pool) {
        m_result_pool->clear();
        m_result_pool.reset();
    }
    m_result_data = std::move(new_results);
    m_generation.fetch_add(1, std::memory_order_release);
}

void scan_result_repository::replace_all_results_from_pool(std::shared_ptr<adaptive_cache_pool<scan_result>> pool) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_result_pool && m_result_pool != pool) {
        m_result_pool->clear();
    }
    m_result_pool.reset();
    m_result_data.clear();
    m_result_data.shrink_to_fit();

    size_t total_count = pool ? pool->total_size() : 0;
    if (total_count <= POOL_MEMORY_THRESHOLD) {
        if (total_count > 0) {
            m_result_data = pool->read_chunk(0, total_count);
        }
        pool->clear();
    } else {
        m_result_pool = std::move(pool);
    }
    m_generation.fetch_add(1, std::memory_order_release);
}

size_t scan_result_repository::get_result_count() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_result_pool) {
        return m_result_pool->total_size();
    }
    return m_result_data.size();
}

size_t scan_result_repository::get_result_count_relaxed() const
{
    if (m_result_pool) {
        return m_result_pool->total_size();
    }
    return m_result_data.size();
}

const scan_result* scan_result_repository::get_result_at(size_t index) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_result_pool) {
        return nullptr;
    }
    return (index < m_result_data.size()) ? &m_result_data[index] : nullptr;
}

uint64_t scan_result_repository::get_address_at_index(size_t index) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_result_pool) {
        auto chunk = m_result_pool->read_chunk(index, 1);
        if (!chunk.empty()) {
            return chunk[0].address;
        }
        return 0;
    }
    return (index < m_result_data.size()) ? m_result_data[index].address : 0;
}

scan_data_type scan_result_repository::get_matched_type_at_index(size_t index) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    uint16_t mask = 0;
    if (m_result_pool) {
        auto chunk = m_result_pool->read_chunk(index, 1);
        if (!chunk.empty()) {
            mask = chunk[0].type_mask;
        }
    } else if (index < m_result_data.size()) {
        mask = m_result_data[index].type_mask;
    }
    return type_mask_to_primary_type(mask);
}

int scan_result_repository::get_current_generation() const
{
    return m_generation.load();
}

std::vector<scan_result> scan_result_repository::read_pool_chunk(size_t start, size_t count) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_result_pool) {
        return m_result_pool->read_chunk(start, count);
    }
    std::vector<scan_result> chunk;
    if (start >= m_result_data.size()) return chunk;
    size_t end = std::min(start + count, m_result_data.size());
    chunk.assign(m_result_data.begin() + start, m_result_data.begin() + end);
    return chunk;
}

std::vector<uint64_t> scan_result_repository::read_address_range(size_t start, size_t count) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<uint64_t> addresses;
    if (m_result_pool) {
        auto chunk = m_result_pool->read_chunk(start, count);
        addresses.reserve(chunk.size());
        for (const auto& r : chunk) {
            addresses.push_back(r.address);
        }
    } else {
        if (start >= m_result_data.size()) return addresses;
        size_t end = std::min(start + count, m_result_data.size());
        addresses.reserve(end - start);
        for (size_t i = start; i < end; ++i) {
            addresses.push_back(m_result_data[i].address);
        }
    }
    return addresses;
}

std::vector<scan_result> scan_result_repository::get_results() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_result_pool) {
        size_t total = m_result_pool->total_size();
        return m_result_pool->read_chunk(0, total);
    }
    return m_result_data;
}

void scan_result_repository::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_result_data.clear();
    if (m_result_pool) {
        m_result_pool->clear();
        m_result_pool.reset();
    }
    m_generation.fetch_add(1, std::memory_order_release);
}

void scan_result_repository::set_scan_metadata(const scan_metadata& meta) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_metadata = meta;
}

const scan_metadata& scan_result_repository::get_scan_metadata() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_metadata;
}

void scan_result_repository::clear_scan_metadata() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_metadata = scan_metadata{};
}

void scan_result_repository::save_as_previous_results()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::lock_guard<std::mutex> prev_lock(m_prev_mutex);

    if (m_result_pool) {
        size_t total = m_result_pool->total_size();
        if (total > 10'000'000) {
            m_result_pool->clear();
            m_result_pool.reset();
            m_previous_scan_result_snapshot.results.clear();
        } else {
            m_previous_scan_result_snapshot.results = m_result_pool->read_chunk(0, total);
            m_result_pool->clear();
            m_result_pool.reset();
        }
    } else {
        m_previous_scan_result_snapshot.results = std::move(m_result_data);
    }
    m_result_data.clear();
    m_generation.fetch_add(1, std::memory_order_release);
}

bool scan_result_repository::has_previous_results() const
{
    std::lock_guard<std::mutex> prev_lock(m_prev_mutex);
    return !m_previous_scan_result_snapshot.results.empty();
}

bool scan_result_repository::swap_with_previous()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::lock_guard<std::mutex> prev_lock(m_prev_mutex);

    if (m_previous_scan_result_snapshot.results.empty())
        return false;

    if (m_result_pool) {
        m_result_pool->clear();
        m_result_pool.reset();
    }

    m_result_data.swap(m_previous_scan_result_snapshot.results);
    m_generation.fetch_add(1, std::memory_order_release);
    return true;
}

void scan_result_repository::clear_previous_results()
{
    std::lock_guard<std::mutex> prev_lock(m_prev_mutex);
    m_previous_scan_result_snapshot.results.clear();
}

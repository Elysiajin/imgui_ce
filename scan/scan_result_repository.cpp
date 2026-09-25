#include "scan\scan_result_repository.h"
#include <algorithm>
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

scan_result_store scan_result_repository::pack_from_vector(const std::vector<scan_result>& results) {
    std::vector<scan_result> copy = results;
    sort_by_address(copy);
    scan_result_store store;
    store.build(copy);
    return store;
}

bool scan_result_repository::replace_all_results(std::vector<scan_result>&& new_results,
                                                 std::string* error) {
    // 排序/压缩在锁外完成（大结果集耗时可达分钟级），最后短临界区交换
    std::shared_ptr<scan_result_store> new_store;
    std::vector<scan_result> sorted;
    try {
        sorted = std::move(new_results);
        sort_by_address(sorted);
        if (sorted.size() > POOL_MEMORY_THRESHOLD) {
            new_store = std::make_shared<scan_result_store>();
            new_store->build(sorted);
        }
    } catch (const std::bad_alloc&) {
        if (error) *error = "结果集过大，内存不足入库失败（试试缩小扫描范围或换更精确的条件）";
        return false;
    } catch (const std::exception& e) {
        if (error) *error = std::string("结果入库失败: ") + e.what();
        return false;
    } catch (...) {
        if (error) *error = "结果入库失败（未知异常）";
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_result_store = std::move(new_store);   // 可能为空（小结果集走向量）
    if (m_result_store) {
        m_result_data.clear();
        m_result_data.shrink_to_fit();
    } else {
        m_result_data = std::move(sorted);
    }
    m_generation.fetch_add(1, std::memory_order_release);
    return true;
}

bool scan_result_repository::replace_all_results_from_pool(
    std::shared_ptr<adaptive_cache_pool<scan_result>> pool, std::string* error) {
    // 打包阶段（整池读出 + 排序 + 压缩）对大结果集可达数秒到数分钟，绝不能
    // 持有 m_mutex 执行 —— UI 线程每帧 read_pool_chunk 都在等同一把锁，持锁
    // 打包等于把界面冻住。耗时工作全部在锁外完成，最后短临界区交换。
    // 峰值内存 = 全量 vector + 压缩存储，超大结果集可能 bad_alloc：在这里
    // 兜底转成错误返回，不让异常逃出到扫描线程（会 terminate 崩进程）。
    std::shared_ptr<scan_result_store> new_store;
    std::vector<scan_result> all;
    try {
        const size_t total_count = pool ? pool->total_size() : 0;
        if (total_count > 0) {
            // 一次性读出后排序：原始 vector 只在入库瞬间存在，随后按阈值
            // 走 小结果集向量 或 压缩驻留（0.2~0.9 倍），不再保留磁盘池。
            all = pool->read_chunk(0, total_count);
            pool->clear();
            sort_by_address(all);
            if (all.size() > POOL_MEMORY_THRESHOLD) {
                new_store = std::make_shared<scan_result_store>();
                new_store->build(all);
            }
        }
    } catch (const std::bad_alloc&) {
        if (error) *error = "结果集过大，内存不足入库失败（试试缩小扫描范围或换更精确的条件）";
        return false;
    } catch (const std::exception& e) {
        if (error) *error = std::string("结果入库失败: ") + e.what();
        return false;
    } catch (...) {
        if (error) *error = "结果入库失败（未知异常）";
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_result_store = std::move(new_store);   // 可能为空（小结果集走向量）
    if (m_result_store) {
        m_result_data.clear();
        m_result_data.shrink_to_fit();
    } else {
        m_result_data = std::move(all);
    }
    m_generation.fetch_add(1, std::memory_order_release);
    return true;
}

size_t scan_result_repository::get_result_count() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_result_store) {
        return m_result_store->size();
    }
    return m_result_data.size();
}

size_t scan_result_repository::get_result_count_relaxed() const
{
    if (m_result_store) {
        return m_result_store->size();
    }
    return m_result_data.size();
}

const scan_result* scan_result_repository::get_result_at(size_t index) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_result_store) {
        return nullptr; // 压缩存储无稳定地址，调用方走 get_address_at_index
    }
    return (index < m_result_data.size()) ? &m_result_data[index] : nullptr;
}

uint64_t scan_result_repository::get_address_at_index(size_t index) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_result_store) {
        return (index < m_result_store->size()) ? m_result_store->at(index).address : 0;
    }
    return (index < m_result_data.size()) ? m_result_data[index].address : 0;
}

scan_data_type scan_result_repository::get_matched_type_at_index(size_t index) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    uint16_t mask = 0;
    if (m_result_store) {
        if (index < m_result_store->size())
            mask = m_result_store->at(index).type_mask;
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
    if (m_result_store) {
        std::vector<scan_result> chunk;
        const size_t total = m_result_store->size();
        if (start >= total) return chunk;
        const size_t end = std::min(start + count, total);
        chunk.reserve(end - start);
        for (size_t i = start; i < end; ++i)
            chunk.push_back(m_result_store->at(i));
        return chunk;
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
    if (m_result_store) {
        const size_t total = m_result_store->size();
        if (start >= total) return addresses;
        const size_t end = std::min(start + count, total);
        addresses.reserve(end - start);
        for (size_t i = start; i < end; ++i)
            addresses.push_back(m_result_store->at(i).address);
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
    if (m_result_store) {
        std::vector<scan_result> all;
        m_result_store->decode_all(all);
        return all;
    }
    return m_result_data;
}

void scan_result_repository::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_result_data.clear();
    m_result_data.shrink_to_fit();
    m_result_store.reset();
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

    if (m_result_store) {
        // 压缩存储直接移交（vector 成员 move，代价 O(1)）
        m_previous_scan_result_snapshot.store = std::move(*m_result_store);
        m_result_store.reset();
    } else {
        m_previous_scan_result_snapshot.store = pack_from_vector(m_result_data);
        m_result_data.clear();
        m_result_data.shrink_to_fit();
    }
    m_generation.fetch_add(1, std::memory_order_release);
}

bool scan_result_repository::has_previous_results() const
{
    std::lock_guard<std::mutex> prev_lock(m_prev_mutex);
    return m_previous_scan_result_snapshot.store.size() > 0;
}

bool scan_result_repository::swap_with_previous()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::lock_guard<std::mutex> prev_lock(m_prev_mutex);

    if (m_previous_scan_result_snapshot.store.size() == 0)
        return false;

    // 当前结果打包成压缩存储
    scan_result_store cur_store;
    if (m_result_store) {
        cur_store = std::move(*m_result_store);
        m_result_store.reset();
    } else {
        cur_store = pack_from_vector(m_result_data);
    }

    // 上一轮 ↔ 当前 交换
    scan_result_store old_prev = std::move(m_previous_scan_result_snapshot.store);
    m_previous_scan_result_snapshot.store = std::move(cur_store);

    // 旧上一轮成为当前结果
    if (old_prev.size() > POOL_MEMORY_THRESHOLD) {
        m_result_store = std::make_shared<scan_result_store>(std::move(old_prev));
        m_result_data.clear();
        m_result_data.shrink_to_fit();
    } else {
        old_prev.decode_all(m_result_data);
        m_result_store.reset();
    }
    m_generation.fetch_add(1, std::memory_order_release);
    return true;
}

void scan_result_repository::clear_previous_results()
{
    std::lock_guard<std::mutex> prev_lock(m_prev_mutex);
    m_previous_scan_result_snapshot.store = scan_result_store{};
}

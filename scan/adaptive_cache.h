#pragma once
#include "scan\temp_path_manager.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>


#define THEAD_LOCAL_SIZE 50LL * 1024 * 1024 // 50MB

/// @brief 泛型自适应缓存容器（子缓存单元）
template <typename T> class adaptive_cache {
  static_assert(std::is_trivially_copyable_v<T>,
                "adaptive_cache requires trivially copyable types.");

public:
  // 【修改】构造函数增加唯一标识，防止多线程创建文件冲突
  explicit adaptive_cache(size_t mem_threshold = 100'000,
                         std::string id = "default")
      : m_count(0), m_threshold(mem_threshold), m_on_disk(false), m_unique_id(id) {
    m_buffer.reserve(std::min(mem_threshold, (size_t)10240));
  }

  ~adaptive_cache() { clear(); }

  // 禁用拷贝，支持移动
  adaptive_cache(adaptive_cache &&other) noexcept { move_from(std::move(other)); }
  adaptive_cache &operator=(adaptive_cache &&other) noexcept {
    if (this != &other) {
      clear();
      move_from(std::move(other));
    }
    return *this;
  }

  adaptive_cache(const adaptive_cache &) = delete;
  adaptive_cache &operator=(const adaptive_cache &) = delete;

  void push_back(const T &item) {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (!m_on_disk) {
      m_buffer.push_back(item);
      if (m_buffer.size() >= m_threshold)
        flush_to_disk();
    } else {
      ensure_file_open();
      m_disk_out.write(reinterpret_cast<const char *>(&item), sizeof(T));
    }
    ++m_count;
  }

  void push_back_batch(const std::vector<T> &items) {
    if (items.empty())
      return;
    std::lock_guard<std::mutex> lock(m_mtx);

    // 只有当前已经在磁盘，或者 内存+新数据 超过阈值时，才写磁盘
    if (m_on_disk || (m_buffer.size() + items.size() >= m_threshold)) {
      if (!m_on_disk)
        flush_to_disk(); // 第一次超过阈值，把内存里的全倒进去

      ensure_file_open();
      m_disk_out.write(reinterpret_cast<const char *>(items.data()),
                      items.size() * sizeof(T));
      // 注意：不要在这里调用 flush()，让操作系统自己调度合并写入，提高性能
    } else {
      // 还没到阈值，直接进内存
      m_buffer.insert(m_buffer.end(), items.begin(), items.end());
    }
    m_count += items.size();
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_count;
  }

  // 从当前子缓存读取切片
  std::vector<T> read_chunk(size_t start, size_t count) const {
    std::lock_guard<std::mutex> lock(m_mtx);
    std::vector<T> chunk;
    if (start >= m_count)
      return chunk;

    size_t end = std::min(start + count, m_count);
    size_t actual_to_read = end - start;
    chunk.reserve(actual_to_read);

    if (!m_on_disk) {
      chunk.assign(m_buffer.begin() + start, m_buffer.begin() + end);
    } else {
      // 注意：读取前确保写入流已关闭或 flush，这里使用独立的 ifstream
      std::ifstream file(m_disk_path, std::ios::binary);
      if (file) {
        file.seekg(start * sizeof(T));
        T tmp;
        for (size_t i = 0; i < actual_to_read &&
                           file.read(reinterpret_cast<char *>(&tmp), sizeof(T));
             ++i) {
          chunk.push_back(tmp);
        }
      }
    }
    return chunk;
  }

  void clear() {
    std::lock_guard<std::mutex> lock(m_mtx);
    m_buffer.clear();
    m_buffer.shrink_to_fit();
    if (m_disk_out.is_open())
      m_disk_out.close();
    if (!m_disk_path.empty()) {
      std::error_code ec;
      std::filesystem::remove(m_disk_path, ec);
      m_disk_path.clear();
    }
    m_count = 0;
    m_on_disk = false;
  }

private:
  void move_from(adaptive_cache &&other) {
    std::lock_guard<std::mutex> lock(other.m_mtx);
    m_buffer = std::move(other.m_buffer);
    m_disk_path = std::move(other.m_disk_path);
    m_disk_out = std::move(other.m_disk_out);
    m_count = other.m_count;
    m_threshold = other.m_threshold;
    m_on_disk = other.m_on_disk;
    m_unique_id = std::move(other.m_unique_id);
    other.m_count = 0;
    other.m_on_disk = false;
  }

  void ensure_file_open() {
    if (!m_disk_out.is_open()) {
      if (m_disk_path.empty()) {
        auto dir = std::filesystem::path(temp_path_manager::get_work_dir());
        auto ts = std::chrono::high_resolution_clock::now()
                      .time_since_epoch()
                      .count();
        // 【修改】文件名包含唯一标识，避免冲突
        m_disk_path =
            (dir / ("ACache_" + m_unique_id + "_" + std::to_string(ts) + ".tmp"))
                .string();
      }
      m_disk_out.open(m_disk_path, std::ios::binary | std::ios::app);
    }
  }

  void flush_to_disk() {
    if (m_buffer.empty())
      return;
    ensure_file_open();
    m_disk_out.write(reinterpret_cast<const char *>(m_buffer.data()),
                    m_buffer.size() * sizeof(T));
    m_disk_out.flush(); // 确保写入磁盘
    m_buffer.clear();
    m_buffer.shrink_to_fit();
    m_on_disk = true;
  }

  mutable std::mutex m_mtx;
  std::vector<T> m_buffer;
  std::string m_disk_path;
  std::ofstream m_disk_out;
  size_t m_count;
  size_t m_threshold;
  bool m_on_disk;
  std::string m_unique_id; // 【新增】实例唯一 ID
};

// =================================================================
// 【新增】adaptive_cache_pool：管理多个扫描线程的子缓存
// =================================================================
template <typename T> class adaptive_cache_pool {
public:
  explicit adaptive_cache_pool(size_t per_thread_threshold = THEAD_LOCAL_SIZE)
      : m_per_thread_threshold(per_thread_threshold) {}

  /// @brief 获取当前线程专属的子缓存并添加数据（完全无锁竞争）
  void push_back(const T &item) { get_thread_local_cache().push_back(item); }

  void push_back_batch(const std::vector<T> &items) {
    if (items.empty())
      return;
    // get_thread_local_cache 会自动返回当前线程关联的 adaptive_cache 实例
    get_thread_local_cache().push_back_batch(items);
  }

  /// @brief 获取总元素数量
  size_t total_size() const {
    std::lock_guard<std::mutex> lock(m_pool_mtx);
    size_t total = 0;
    for (const auto &[id, cache] : m_sub_caches) {
      total += cache->size();
    }
    return total;
  }

  /// @brief 统一分页读取：聚合所有子缓存的数据
  std::vector<T> read_chunk(size_t start, size_t count) const {
    std::lock_guard<std::mutex> lock(m_pool_mtx);
    std::vector<T> result;
    size_t current_offset = 0;
    size_t remaining_to_read = count;

    for (const auto &[id, cache] : m_sub_caches) {
      size_t cache_size = cache->size();
      // 检查当前 chunk 是否落在这个子缓存的范围内
      if (start < current_offset + cache_size) {
        size_t internal_start =
            (start > current_offset) ? (start - current_offset) : 0;
        size_t internal_count =
            std::min(remaining_to_read, cache_size - internal_start);

        auto sub_chunk = cache->read_chunk(internal_start, internal_count);
        result.insert(result.end(), sub_chunk.begin(), sub_chunk.end());

        remaining_to_read -= sub_chunk.size();
        start += sub_chunk.size(); // 移动起点
        if (remaining_to_read == 0)
          break;
      }
      current_offset += cache_size;
    }
    return result;
  }

  void clear() {
    std::lock_guard<std::mutex> lock(m_pool_mtx);
    for (auto &[id, cache] : m_sub_caches) {
      cache->clear();
    }
    m_sub_caches.clear();
  }

private:
  // gemini方案：线程本地变量 +
  // 池内查表（每线程首次访问时触发，后续直接无锁访问）
  //    adaptive_cache<T>& get_thread_local_cache() {
  //         //
  //         使用两个线程本地变量，同时记录“最后一次服务的池实例”和“对应的子缓存”
  //         thread_local const void* last_seen_pool_id = nullptr;
  //         thread_local adaptive_cache<T>* local_cache_instance = nullptr;

  //         // ★ 核心防御逻辑：只有当当前执行线程服务的池实例就是当前 this
  //         实例时，才允许走极速无锁路径 if (last_seen_pool_id == this) [[likely]]
  //         {
  //             return *local_cache_instance;
  //         }

  //         // 如果 pool
  //         实例变了（说明是新一轮扫描），或者首次进入，乖乖走加锁查表流程（每轮扫描每条线程仅触发一次）
  //         std::lock_guard<std::mutex> lock(m_pool_mtx);

  //         std::stringstream ss;
  //         ss << std::this_thread::get_id();
  //         std::string tid = ss.str();

  //         auto it = m_sub_caches.find(tid);
  //         if (it == m_sub_caches.end()) {
  //             auto new_cache =
  //             std::make_unique<adaptive_cache<T>>(m_per_thread_threshold, tid);
  //             local_cache_instance = new_cache.get();
  //             m_sub_caches[tid] = std::move(new_cache);
  //         } else {
  //             local_cache_instance = it->second.get();
  //         }

  //         // 重新绑定，刷新当前线程的认知
  //         last_seen_pool_id = this;
  //         return *local_cache_instance;
  //     }

  adaptive_cache<T> &get_thread_local_cache() {
    thread_local std::string tid = [] {
      std::stringstream ss;
      ss << std::this_thread::get_id();
      return ss.str();
    }();

    // 只有在第一次创建该线程的子缓存时才加锁
    std::lock_guard<std::mutex> lock(m_pool_mtx);
    auto it = m_sub_caches.find(tid);
    if (it == m_sub_caches.end()) {
      auto new_cache =
          std::make_unique<adaptive_cache<T>>(m_per_thread_threshold, tid);
      auto &ref = *new_cache;
      m_sub_caches[tid] = std::move(new_cache);
      return ref;
    }
    return *(it->second);
  }

  mutable std::mutex m_pool_mtx;
  size_t m_per_thread_threshold;
  std::map<std::string, std::unique_ptr<adaptive_cache<T>>> m_sub_caches;
};
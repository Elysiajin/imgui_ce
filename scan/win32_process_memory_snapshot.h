#pragma once
#include "scan/iprocess_memory_snapshot.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <mutex>

// 进程内存快照：一次扫描时刻的目标进程内存副本（供"再次扫描"做前后值比较）。
//
// ── 为什么要改（旧实现的两处硬伤）────────────────────────────────────────────
//  1. 旧实现用 std::ofstream 把整份内存写成普通 .bin，再用 std::ifstream
//     seekg()+read() 取值。task_next_scan 是"每个结果一次 read_value"，
//     100 万个结果就是 200 万次系统调用 → 再次扫描极卡；而且整份内存实打实
//     落在磁盘上，扫描几次就是几个 GB。
//  2. 更糟的是 ifstream 是 thread_local 且只在首次 open：同一个线程上第二次
//     快照仍然读的是第一个文件（路径没变过）→ 比较结果直接错乱。
//
// ── 现在的实现（对齐 CE：临时文件 + 内存映射）──────────────────────────────
//  · 文件由 process_memory_snapshot_manager 以
//    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE 创建：
//      - TEMPORARY：告诉系统尽量把页面留在系统缓存、不写回磁盘（磁盘占用趋近 0）；
//      - DELETE_ON_CLOSE：最后一个句柄关闭即自动删除，崩溃也不留垃圾。
//  · 读取走 CreateFileMapping + MapViewOfFile：
//      - 快照 <= k_full_map_limit（768MB）→ 一次性映射整个文件，读退化为 memcpy；
//      - 更大的快照改用滑动窗口，只在窗口未命中时重映射。
//    映射窗口是共享状态，用互斥锁串行化（重映射期间不允许别的线程持有旧指针）。
class win32_process_memory_snapshot : public i_process_memory_snapshot {
public:
    // 接管已创建并写完数据的文件句柄所有权（manager 用 DELETE_ON_CLOSE 打开）
    win32_process_memory_snapshot(HANDLE h_file, std::string path,
                                  std::map<uint64_t, size_t> index,
                                  uint64_t file_size);
    ~win32_process_memory_snapshot() override;

    win32_process_memory_snapshot(const win32_process_memory_snapshot&)            = delete;
    win32_process_memory_snapshot& operator=(const win32_process_memory_snapshot&) = delete;

    bool read_data(uint64_t address, uint8_t* buffer, size_t size) const override;
    const std::string& path() const override { return m_path; }
    const std::map<uint64_t, size_t>& index() const override { return m_index; }

private:
    // 保证 [offset, offset+size) 落在当前映射窗口内；false = 需要走 ReadFile 回退
    bool ensure_window(uint64_t offset, size_t size) const;
    bool read_via_file(uint64_t offset, uint8_t* buffer, size_t size) const;
    // 地址 → 文件内偏移，顺带做区域边界检查；失败返回 false
    bool translate(uint64_t address, size_t size, uint64_t& offset_out) const;

    std::string                  m_path;
    std::map<uint64_t, size_t>   m_index;

    HANDLE   m_h_file    = INVALID_HANDLE_VALUE;
    HANDLE   m_h_mapping = nullptr;
    uint64_t m_file_size = 0;

    // 映射窗口（mutable：read_data 是 const，但窗口属于缓存状态）
    mutable std::mutex m_window_mtx;
    mutable uint8_t*   m_window      = nullptr;
    mutable uint64_t   m_window_off  = 0;    // 窗口起始偏移（分配粒度对齐）
    mutable size_t     m_window_size = 0;
    mutable bool       m_full_map    = false; // 整文件映射，永不重映射

    // 64 位进程虚拟地址空间足够大，整文件映射只是"预留地址"，真正占用的物理页
    // 按需提交；比滑动窗口换来的是零锁、零系统调用的纯 memcpy 读取。
    static constexpr uint64_t k_full_map_limit = 1536ull * 1024 * 1024; // 1.5GB 以内一次映射完
    static constexpr size_t   k_window_size    = 16ull * 1024 * 1024;   // 超限后的滑动窗口
};

#include "scan/win32_process_memory_snapshot.h"

#include <cstring>
#include <iterator>

namespace {
// MapViewOfFile 的偏移必须按"分配粒度"（通常 64KB）对齐
size_t allocation_granularity()
{
    static const size_t g = [] {
        SYSTEM_INFO si = {};
        GetSystemInfo(&si);
        return static_cast<size_t>(si.dwAllocationGranularity);
    }();
    return g;
}

inline uint64_t align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
inline uint64_t align_up(uint64_t v, uint64_t a) { return align_down(v + a - 1, a); }
}

win32_process_memory_snapshot::win32_process_memory_snapshot(HANDLE h_file, std::string path,
                                                             std::map<uint64_t, size_t> index,
                                                             uint64_t file_size)
    : m_path(std::move(path)), m_index(std::move(index)),
      m_h_file(h_file), m_file_size(file_size)
{
    if (m_h_file == INVALID_HANDLE_VALUE || m_file_size == 0)
        return;

    // 大小传 0 = 用文件当前大小；PAGE_READWRITE 只是建立映射对象，视图只读
    m_h_mapping = CreateFileMappingA(m_h_file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    if (!m_h_mapping)
        return;

    if (m_file_size <= k_full_map_limit) {
        // 0 长度 = 从偏移 0 一直映射到文件尾
        void* view = MapViewOfFile(m_h_mapping, FILE_MAP_READ, 0, 0, 0);
        if (view) {
            m_window      = static_cast<uint8_t*>(view);
            m_window_off  = 0;
            m_window_size = static_cast<size_t>(m_file_size);
            m_full_map    = true;
        }
    }
}

win32_process_memory_snapshot::~win32_process_memory_snapshot()
{
    if (m_window)   UnmapViewOfFile(m_window);
    if (m_h_mapping) CloseHandle(m_h_mapping);
    // 关闭最后一个文件句柄 → DELETE_ON_CLOSE 生效，文件自动消失，无需手动 remove
    if (m_h_file != INVALID_HANDLE_VALUE) CloseHandle(m_h_file);
}

bool win32_process_memory_snapshot::translate(uint64_t address, size_t size,
                                              uint64_t& offset_out) const
{
    auto it = m_index.upper_bound(address);
    if (it == m_index.begin())
        return false;
    --it;

    const uint64_t region_base = it->first;
    size_t region_data_size;
    auto next = std::next(it);
    if (next != m_index.end())
        region_data_size = next->second - it->second;
    else
        region_data_size = static_cast<size_t>(m_file_size - it->second);

    const size_t offset_in_region = static_cast<size_t>(address - region_base);
    if (offset_in_region + size > region_data_size)
        return false;   // 跨区域读取：拒绝，避免拿到相邻区域的数据

    offset_out = static_cast<uint64_t>(it->second) + offset_in_region;
    return true;
}

bool win32_process_memory_snapshot::ensure_window(uint64_t offset, size_t size) const
{
    if (!m_h_mapping)
        return false;
    if (m_full_map)
        return m_window != nullptr;

    if (m_window && offset >= m_window_off &&
        offset + size <= m_window_off + m_window_size)
        return true;    // 命中当前窗口

    const size_t gran   = allocation_granularity();
    const uint64_t base = align_down(offset, gran);

    // 窗口至少要覆盖本次请求，再放大到 k_window_size
    size_t span = static_cast<size_t>((offset - base) + size);
    span = static_cast<size_t>(align_up(span, gran));
    if (span < k_window_size)
        span = k_window_size;
    if (base + span > m_file_size)
        span = static_cast<size_t>(m_file_size - base);
    if (span < size)
        return false;   // 极端情况：请求超过文件尾部，交给 ReadFile 回退

    if (m_window) {
        UnmapViewOfFile(m_window);
        m_window = nullptr;
    }
    void* view = MapViewOfFile(m_h_mapping, FILE_MAP_READ,
                               static_cast<DWORD>(base >> 32),
                               static_cast<DWORD>(base & 0xffffffffu),
                               span);
    if (!view)
        return false;

    m_window      = static_cast<uint8_t*>(view);
    m_window_off  = base;
    m_window_size = span;
    return true;
}

bool win32_process_memory_snapshot::read_via_file(uint64_t offset, uint8_t* buffer, size_t size) const
{
    if (m_h_file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(m_h_file, pos, nullptr, FILE_BEGIN))
        return false;

    DWORD got = 0;
    // 调用方已持有 m_window_mtx，定位+读取是原子的，多线程不会互相踩偏移
    if (!ReadFile(m_h_file, buffer, static_cast<DWORD>(size), &got, nullptr))
        return false;
    return got == static_cast<DWORD>(size);
}

bool win32_process_memory_snapshot::read_data(uint64_t address, uint8_t* buffer, size_t size) const
{
    if (size == 0)
        return true;

    uint64_t offset = 0;
    if (!translate(address, size, offset))
        return false;

    // ★ 快路径：整文件映射时窗口在构造后就是不可变的，多线程只读完全不需要加锁。
    //   实测 16 线程抢同一把 mutex 会把"每个地址 13µs"打回"每个地址 ~0.2µs"，
    //   锁的开销比 memcpy 本身高两个数量级，这是再次扫描卡顿的主因。
    if (m_full_map && m_window) {
        std::memcpy(buffer, m_window + offset, size);
        return true;
    }

    std::lock_guard<std::mutex> lock(m_window_mtx);
    if (ensure_window(offset, size)) {
        std::memcpy(buffer, m_window + (offset - m_window_off), size);
        return true;
    }
    return read_via_file(offset, buffer, size);
}

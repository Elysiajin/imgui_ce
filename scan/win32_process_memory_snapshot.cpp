#include "scan/win32_process_memory_snapshot.h"
#include <cstring>
#include <fstream>
#include <filesystem>

win32_process_memory_snapshot::win32_process_memory_snapshot(const std::string& path, std::map<uint64_t, size_t> index)
    : m_path(path), m_index(std::move(index)) {
    init_mapping();
}

win32_process_memory_snapshot::~win32_process_memory_snapshot() {
    if (m_p_buffer) UnmapViewOfFile(m_p_buffer);
    if (m_h_mapping) CloseHandle(m_h_mapping);
    if (m_h_file != INVALID_HANDLE_VALUE) CloseHandle(m_h_file);

    // RAII 删除自身 .bin 快照文件。
    // 每次扫描都会新建 snapshot_<tick>.bin；set_previous_snapshot 只是换掉
    // shared_ptr，旧文件的 .bin 若没人删会无限累积占用磁盘。这里在最后一个
    // 引用被释放时删除（m_first / m_prev 持有时不删，正好只保留两个快照）。
    std::error_code ec;
    std::filesystem::remove(m_path, ec);
}

void win32_process_memory_snapshot::init_mapping() {
    // 内存优化：不再整文件 mmap。
    // 参考项目用 MapViewOfFile 整段映射，扫描时逐字节读会导致所有页常驻本进程工作集
    // （大目标进程可飙升到几百 MB）。改为普通文件流顺序读取，页进入系统缓存而非本进程 WS，
    // 只有被结果区「显示行」按需随机读到的少量页才会短暂进入工作集。
    m_h_file = CreateFileA(m_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (m_h_file == (HANDLE)(LONG_PTR)-1) return;

    LARGE_INTEGER size;
    if (GetFileSizeEx(m_h_file, &size)) {
        m_file_size = static_cast<size_t>(size.QuadPart);
    }
    // m_h_mapping / m_p_buffer 保持为空，read_data 走 ifstream 路径。
}

bool win32_process_memory_snapshot::read_data(uint64_t address, uint8_t* buffer, size_t size) const {
    auto it = m_index.upper_bound(address);
    if (it == m_index.begin()) return false;
    --it;

    uint64_t region_base = it->first;
    size_t region_data_size;
    auto next = std::next(it);
    if (next != m_index.end()) {
        region_data_size = next->second - it->second;
    } else {
        region_data_size = m_file_size - it->second;
    }

    size_t offset_in_region = static_cast<size_t>(address - region_base);
    if (offset_in_region + size > region_data_size) return false;

    size_t read_offset = it->second + offset_in_region;

    // 顺序扫描用普通文件流（页进入系统缓存而非本进程工作集）
    thread_local std::ifstream local_stream;
    if (!local_stream.is_open()) local_stream.open(m_path, std::ios::binary);
    if (!local_stream) return false;

    local_stream.seekg(read_offset);
    local_stream.read(reinterpret_cast<char*>(buffer), size);
    return local_stream.gcount() == static_cast<std::streamsize>(size);
}

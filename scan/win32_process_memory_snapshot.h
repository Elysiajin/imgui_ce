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

class win32_process_memory_snapshot : public i_process_memory_snapshot {
public:
    win32_process_memory_snapshot(const std::string& path, std::map<uint64_t, size_t> index);
    ~win32_process_memory_snapshot() override;

    bool read_data(uint64_t address, uint8_t* buffer, size_t size) const override;
    const std::string& path() const override { return m_path; }
    const std::map<uint64_t, size_t>& index() const override { return m_index; }

private:
    std::string m_path;
    std::map<uint64_t, size_t> m_index;

    HANDLE m_h_file = INVALID_HANDLE_VALUE;
    HANDLE m_h_mapping = nullptr;
    uint8_t* m_p_buffer = nullptr;
    size_t m_file_size = 0;

    void init_mapping();
};

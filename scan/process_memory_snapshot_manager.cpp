#include "scan/process_memory_snapshot_manager.h"
#include "scan/win32_process_memory_snapshot.h"
#include "core/process_manager.h"
#include "scan/temp_path_manager.h"
#include <filesystem>
#include <fstream>
#include <map>

std::shared_ptr<i_process_memory_snapshot> process_memory_snapshot_manager::create_snapshot(const std::vector<memory_region>& regions) {
    std::string path = (std::filesystem::path(temp_path_manager::get_work_dir()) /
        ("snapshot_" + std::to_string(GetTickCount64()) + ".bin")).string();

    std::ofstream out_file(path, std::ios::binary);
    if (!out_file) return nullptr;

    std::map<uint64_t, size_t> index;
    size_t current_file_offset = 0;

    auto accessor = process_manager::instance().memory();
    if (!accessor) return nullptr;
    static constexpr size_t READ_CHUNK_SIZE = 64 * 1024;
    for (const auto& reg : regions) {
        uint64_t chunk_base = reg.base;
        size_t remaining = reg.size;
        bool region_has_any_writes = false;

        while (remaining > 0) {
            size_t to_read = (std::min)(READ_CHUNK_SIZE, remaining);
            std::vector<uint8_t> buffer(to_read);

            if (!region_has_any_writes) {
                index[reg.base] = current_file_offset;
                region_has_any_writes = true;
            }

            if (accessor->read(chunk_base, buffer.data(), to_read)) {
                out_file.write(reinterpret_cast<const char*>(buffer.data()), to_read);
            } else {
                std::vector<uint8_t> zeros(to_read, 0);
                out_file.write(reinterpret_cast<const char*>(zeros.data()), to_read);
            }
            current_file_offset += to_read;

            chunk_base += to_read;
            remaining -= to_read;
        }
    }

    out_file.close();
    return std::make_shared<win32_process_memory_snapshot>(path, std::move(index));
}

void process_memory_snapshot_manager::clear() {
    if (m_first) {
        std::string path = m_first->path();
        m_first.reset();

        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    if (m_prev) {
        std::string path = m_prev->path();
        m_prev.reset();

        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
}

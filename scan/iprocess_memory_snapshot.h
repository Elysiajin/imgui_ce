#pragma once
#include <string>
#include <map>
#include <cstdint>
#include <vector>

class i_process_memory_snapshot {
public:
    virtual ~i_process_memory_snapshot() = default;

    virtual bool read_data(uint64_t address, uint8_t* buffer, size_t size) const = 0;

    template <typename T>
    bool read_value(uint64_t addr, T& out_val) const {
        return read_data(addr, reinterpret_cast<uint8_t*>(&out_val), sizeof(T));
    }

    virtual const std::string& path() const = 0;
    virtual const std::map<uint64_t, size_t>& index() const = 0;
};

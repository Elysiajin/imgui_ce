#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

class IMemoryAccessor {
public:
    virtual ~IMemoryAccessor() = default;

    virtual bool attach(uint32_t pid) = 0;
    virtual void detach() = 0;

    virtual bool read(uint64_t addr, void* buffer, size_t size) = 0;
    virtual bool write(uint64_t addr, const void* buffer, size_t size) = 0;
    virtual bool is_process_alive() const = 0;
    virtual std::string name() const = 0;
};

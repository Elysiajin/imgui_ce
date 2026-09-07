#pragma once

#include <windows.h>

#include "core/imemory_accessor.h"
#include "type/process_arch.h"

// 基于 ReadProcessMemory/WriteProcessMemory 的 Win32 内存读写实现
class Win32MemoryAccessor : public IMemoryAccessor {
public:
    bool attach(uint32_t pid) override;
    void detach() override;

    bool read(uint64_t addr, void* buffer, size_t size) override;
    bool write(uint64_t addr, const void* buffer, size_t size) override;
    bool is_process_alive() const override;
    std::string name() const override;
    process_arch architecture() const override;

private:
    HANDLE       h_process_ = nullptr;
    process_arch arch_ = process_arch::unknown;
};

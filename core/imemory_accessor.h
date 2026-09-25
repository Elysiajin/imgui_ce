#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#include "type/process_arch.h"

class IMemoryAccessor {
public:
    virtual ~IMemoryAccessor() = default;

    virtual bool attach(uint32_t pid) = 0;
    virtual void detach() = 0;

    virtual bool read(uint64_t addr, void* buffer, size_t size) = 0;
    virtual bool write(uint64_t addr, const void* buffer, size_t size) = 0;
    virtual bool is_process_alive() const = 0;
    virtual std::string name() const = 0;

    // 在目标进程分配 size 字节可执行内存（PAGE_EXECUTE_READWRITE），返回基址；
    // 失败返回 0。AA 脚本的 alloc 用。
    virtual uint64_t alloc(size_t size) = 0;
    // 释放 alloc 返回的地址（MEM_RELEASE）；成功返回 true。
    virtual bool free_mem(uint64_t addr) = 0;

    // 附加进程的 CPU 架构（反汇编/指针宽度据此切换）；未附加返回 unknown。
    virtual process_arch architecture() const = 0;
};

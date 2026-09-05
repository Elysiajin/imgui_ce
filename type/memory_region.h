#pragma once

#include <cstdint>

// 纯数据结构：一块连续内存区域
// protect/state/type 存的是 Windows 的 MEM_* / PAGE_* 位标志值，
// 供扫描引擎做 writable/executable/copy_on_write 等内存属性过滤。
struct memory_region {
    uint64_t base;
    uint64_t size;
    uint32_t protect;   // PAGE_* 位
    uint32_t state;     // MEM_COMMIT / MEM_RESERVE / MEM_FREE
    uint32_t type;      // MEM_PRIVATE / MEM_IMAGE / MEM_MAPPED
};

// 兼容旧别名（本工程既有代码使用小写 memory_region）
using memory_region = memory_region;

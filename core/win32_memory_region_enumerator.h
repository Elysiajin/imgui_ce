#pragma once

#include "core/imemory_region_enumerator.h"

// 基于 VirtualQueryEx 的 Win32 内存区域枚举实现（支持 scan_request 过滤）
class Win32MemoryRegionEnumerator : public IMemoryRegionEnumerator {
public:
    std::vector<memory_region> enumerate() override;
    std::vector<memory_region> enumerate(const scan_request& req) override;
};

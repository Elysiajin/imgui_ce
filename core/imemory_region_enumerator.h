#pragma once

#include <vector>

#include "type/memory_region.h"
#include "type/scan_data_stream_define.h"

// 内存区域枚举抽象接口
class IMemoryRegionEnumerator {
public:
    virtual ~IMemoryRegionEnumerator() = default;

    // 默认：扫描全部已提交可读/可写内存
    virtual std::vector<memory_region> enumerate() = 0;
    // 带请求过滤（模块范围 / 内存属性）
    virtual std::vector<memory_region> enumerate(const scan_request& req) = 0;
};

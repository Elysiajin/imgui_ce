#pragma once

#include <cstdint>
#include <vector>

#include "type/module_info.h"

// 模块枚举抽象接口
class IModuleEnumerator {
public:
    virtual ~IModuleEnumerator() = default;
    virtual std::vector<module_info> enumerate(uint32_t pid) = 0;
};

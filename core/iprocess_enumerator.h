#pragma once

#include <vector>

#include "type/process_info.h"

// 进程枚举抽象接口
class IProcessEnumerator {
public:
    virtual ~IProcessEnumerator() = default;
    virtual std::vector<process_info> enumerate() = 0;
};

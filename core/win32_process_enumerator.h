#pragma once

#include "core/iprocess_enumerator.h"

// 基于 Toolhelp32 快照的 Win32 进程枚举实现
class Win32ProcessEnumerator : public IProcessEnumerator {
public:
    std::vector<process_info> enumerate() override;
};

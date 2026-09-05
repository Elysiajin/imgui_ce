#pragma once

#include "core/imodule_enumerator.h"

// 基于 Toolhelp32 快照的 Win32 模块枚举实现
class Win32ModuleEnumerator : public IModuleEnumerator {
public:
    std::vector<module_info> enumerate(uint32_t pid) override;
};

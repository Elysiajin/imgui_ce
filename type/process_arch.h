#pragma once

#include <cstdint>

// 附加进程的 CPU 架构。反汇编/指针宽度等都需要据此切换。
enum class process_arch : uint8_t {
    unknown = 0,
    x86_32  = 1,
    x86_64  = 2,
};

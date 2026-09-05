#pragma once

#include <cstdint>
#include <string>

struct module_info {
    std::string name;
    uint64_t    base;
    uint64_t    size;
};

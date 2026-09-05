#ifndef PROCESS_INFO_H
#define PROCESS_INFO_H

#include <cstdint>
#include <string>

struct process_info {
    uint32_t pid;
    uint32_t ppid;
    std::string name;
    uint32_t thread_count;
};


#endif // PROCESS_INFO_H

#ifndef PROCESS_INFO_H
#define PROCESS_INFO_H

#include <cstdint>
#include <string>

struct process_info {
    uint32_t pid;
    uint32_t ppid;
    std::string  name;
    uint32_t thread_count;

    std::wstring image_path;    // 可执行文件完整路径（用于提取图标）
};


#endif // PROCESS_INFO_H

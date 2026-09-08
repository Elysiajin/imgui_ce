#pragma once
#include "scan/iprocess_memory_snapshot.h"
#include "core/process_manager.h"

#include <cstring>

// 实时快照：直接 ReadProcessMemory 读目标进程，不建文件、不占磁盘。
//
// 用在"再次扫描且已有结果集"的场景：此时只需要读上轮存活地址上的当前值，
// 完全没必要把整个进程内存再抓一遍（实测 640MB 目标上这一步要 870ms，
// 占整个再次扫描耗时的 3/4）。
class live_process_memory_snapshot : public i_process_memory_snapshot {
public:
    bool read_data(uint64_t address, uint8_t* buffer, size_t size) const override
    {
        if (size == 0) return true;
        auto* acc = process_manager::instance().memory();
        if (!acc) return false;
        // 读失败（页面已释放/受保护）时返回 false，让调用方跳过该地址。
        // 不填 0 —— 填 0 会让 unchanged 之类条件产生假匹配。
        return acc->read(address, buffer, size);
    }

    const std::string& path() const override { static const std::string empty; return empty; }
    const std::map<uint64_t, size_t>& index() const override { static const std::map<uint64_t, size_t> empty; return empty; }
};

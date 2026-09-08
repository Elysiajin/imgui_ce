#pragma once
#include "scan/iprocess_memory_snapshot.h"
#include "scan/scan_result_store.h"

#include <algorithm>
#include <cstring>
#include <vector>

// 稀疏快照：只保存"上一轮命中地址 → 各自的值"，供下一轮 changed/unchanged/
// increased/decreased 比较使用。
//
// 存储复用 scan_result_store（PE 重定位表）压缩地址 + 一个与地址 index 对齐的
// 值数组。地址在结果集里本来就按升序持有，这里不再像早期实现那样每个结果重复
// 存 8 字节绝对地址 —— 等价于 CE 的 MEMORY.TMP（与 ADDRESSES.TMP 平行的纯值文件）。
//
// 内存占用 ≈ 压缩地址(~2 字节/结果) + value_size 字节/结果，
// 100 万条 int32 结果约 6MB，而不是早期 16 字节/结果的 16MB。
class sparse_memory_snapshot : public i_process_memory_snapshot {
public:
    // results 必须已按 address 升序且无重复；values 与之 index 对齐，
    // 每项 value_size 字节（1~8）。
    sparse_memory_snapshot(std::vector<scan_result> results,
                           std::vector<uint8_t> values,
                           size_t value_size)
        : m_value_size(value_size)
    {
        m_store.build(results);
        m_values = std::move(values);
    }

    bool read_data(uint64_t address, uint8_t* buffer, size_t size) const override
    {
        if (size == 0) return true;
        if (size > m_value_size) return false;   // 存的字节数不够这次读取

        // 地址升序 → 二分定位 index，再取平行值数组
        size_t lo = 0, hi = m_store.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) >> 1;
            scan_result r;
            m_store.get(mid, r);
            if (r.address < address) lo = mid + 1;
            else                     hi = mid;
        }
        if (lo >= m_store.size()) return false;

        scan_result r;
        m_store.get(lo, r);
        if (r.address != address) return false;

        std::memcpy(buffer, m_values.data() + lo * m_value_size, size);
        return true;
    }

    const std::string& path() const override { static const std::string empty; return empty; }
    const std::map<uint64_t, size_t>& index() const override { static const std::map<uint64_t, size_t> empty; return empty; }

    size_t entry_count() const { return m_store.size(); }

private:
    scan_result_store      m_store;       // PE 表压缩地址
    std::vector<uint8_t>   m_values;      // index 对齐的值数组
    size_t                 m_value_size;
};

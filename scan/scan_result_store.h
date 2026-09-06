#pragma once

#include "type/scan_data_stream_define.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// PE 重定位表风格的扫描结果紧凑存储。
//
// 思路与 .PE 重定位表一致：地址高度聚集时，为一段连续命中共用一个"块基址"，
// 后续每个地址只存相对基址的小偏移（一般 2 字节即可，替代原来 8 字节的绝对地址）。
//
// 编码格式（小端，记录流）：
//   0x01 块记录（聚集地址，最常见）：
//     u64 base              块基址（= 块内首地址）
//     u16 count             条目数（<= k_max_block_entries）
//     u8  offset_width      偏移字节宽：2 或 4
//     u8  flags             bit0: 附带 count * u16 type_mask 数组
//     count * offset        地址相对 base 的偏移（升序保证非负）
//     [count * u16 mask]
//   0x02 孤立记录（无 mask）：u64 绝对地址          —— 9 字节 < 原始 10 字节
//   0x03 孤立记录（带 mask）：u64 绝对地址 + u16 mask —— 11 字节 ≈ 原始 10 字节
//
// 关键约束：build() 的输入地址必须升序（扫描结果在入库前排序）。
// 支持整表解码与按下标随机访问（块索引二分 + 块内顺序解码）。
class scan_result_store {
public:
    static constexpr size_t k_max_block_entries = 4096;

    // 用升序地址序列构建紧凑存储。
    void build(const std::vector<scan_result>& results) {
        data_.clear();
        block_index_.clear();
        count_ = results.size();
        has_mask_any_ = false;

        size_t i = 0;
        while (i < results.size()) {
            // 记录入口在索引中标记为该记录的起始偏移（含类型字节）
            block_index_.push_back({ static_cast<uint32_t>(i), static_cast<uint32_t>(data_.size()) });

            const uint64_t base = results[i].address;
            size_t end = std::min(i + k_max_block_entries, results.size());

            // 块内所有偏移必须能装进 u32（跨 4GB 的稀疏跳变处切断新块）
            size_t j = i + 1;
            while (j < end && results[j].address - base <= 0xFFFFFFFFull) ++j;

            if (j - i == 1) {
                // 孤立地址：用 9 字节 solo 记录，保证不比原始记录更占空间
                if (results[i].type_mask != 0) {
                    has_mask_any_ = true;
                    data_.push_back(0x03);
                    append_u64(data_, results[i].address);
                    append_u16(data_, results[i].type_mask);
                } else {
                    data_.push_back(0x02);
                    append_u64(data_, results[i].address);
                }
            } else {
                const uint64_t max_diff = results[j - 1].address - base;
                const uint8_t width = (max_diff <= 0xFFFFull) ? 2 : 4;

                bool block_has_mask = false;
                for (size_t k = i; k < j; ++k) {
                    if (results[k].type_mask != 0) { block_has_mask = true; break; }
                }
                if (block_has_mask) has_mask_any_ = true;

                data_.push_back(0x01);
                append_u64(data_, base);
                append_u16(data_, static_cast<uint16_t>(j - i));
                data_.push_back(width);
                data_.push_back(block_has_mask ? 1 : 0);
                for (size_t k = i; k < j; ++k) {
                    const uint64_t diff = results[k].address - base;
                    if (width == 2) append_u16(data_, static_cast<uint16_t>(diff));
                    else            append_u32(data_, static_cast<uint32_t>(diff));
                }
                if (block_has_mask) {
                    for (size_t k = i; k < j; ++k)
                        append_u16(data_, results[k].type_mask);
                }
            }
            i = j;
        }
    }

    // 整表解码（顺序与 build 输入一致）
    void decode_all(std::vector<scan_result>& out) const {
        out.clear();
        out.resize(count_);
        const uint8_t* p = data_.data();
        const uint8_t* end = data_.data() + data_.size();
        size_t idx = 0;
        while (p < end) {
            const uint8_t type = *p; ++p;
            if (type == 0x02) {
                out[idx].address = read_u64(p); p += 8;
                out[idx].type_mask = 0;
                ++idx;
            } else if (type == 0x03) {
                out[idx].address = read_u64(p); p += 8;
                out[idx].type_mask = read_u16(p); p += 2;
                ++idx;
            } else {
                const uint64_t base = read_u64(p); p += 8;
                const uint16_t cnt = read_u16(p); p += 2;
                const uint8_t width = *p; p += 1;
                const uint8_t flags = *p; p += 1;
                for (uint16_t k = 0; k < cnt; ++k) {
                    const uint64_t diff = (width == 2) ? read_u16(p) : read_u32(p);
                    p += width;
                    out[idx].address = base + diff;
                    ++idx;
                }
                if (flags & 1) {
                    for (uint16_t k = 0; k < cnt; ++k) {
                        out[idx - cnt + k].type_mask = read_u16(p);
                        p += 2;
                    }
                }
            }
        }
    }

    // 随机访问：第 index 条结果（0-based）
    scan_result at(size_t index) const {
        scan_result r{};
        get(index, r);
        return r;
    }

    void get(size_t index, scan_result& out) const {
        // 块索引二分：找到最后一个 first_index <= index 的记录
        size_t lo = 0, hi = block_index_.size();
        while (lo + 1 < hi) {
            size_t mid = (lo + hi) / 2;
            if (block_index_[mid].first_index <= index) lo = mid;
            else hi = mid;
        }
        const uint8_t* p = data_.data() + block_index_[lo].header_pos;
        const uint8_t type = *p; ++p;
        const size_t local = index - block_index_[lo].first_index;

        out.type_mask = 0;
        if (type == 0x02) {
            out.address = read_u64(p);
        } else if (type == 0x03) {
            out.address = read_u64(p); p += 8;
            out.type_mask = read_u16(p);
        } else {
            const uint64_t base = read_u64(p); p += 8;
            const uint16_t cnt = read_u16(p); p += 2;
            const uint8_t width = *p; p += 1;
            const uint8_t flags = *p; p += 1;

            const uint8_t* off_p = p + local * width;
            const uint64_t diff = (width == 2) ? read_u16(off_p) : read_u32(off_p);
            out.address = base + diff;

            if (flags & 1) {
                out.type_mask = read_u16(p + cnt * width + local * 2);
            }
        }
    }

    size_t size() const { return count_; }
    size_t encoded_size() const { return data_.size(); }
    static size_t raw_size_of(size_t count) { return count * sizeof(scan_result); }
    double compression_ratio() const {
        if (count_ == 0) return 1.0;
        return double(encoded_size()) / double(raw_size_of(count_));
    }
    bool uses_mask_arrays() const { return has_mask_any_; }
    const std::vector<uint8_t>& encoded_bytes() const { return data_; }

private:
    struct block_index_entry {
        uint32_t first_index;   // 记录内首条结果的全局下标
        uint32_t header_pos;    // 记录（含类型字节）在 encoded 字节流中的偏移
    };

    static void append_u64(std::vector<uint8_t>& v, uint64_t x) {
        v.insert(v.end(), reinterpret_cast<const uint8_t*>(&x), reinterpret_cast<const uint8_t*>(&x) + 8);
    }
    static void append_u32(std::vector<uint8_t>& v, uint32_t x) {
        v.insert(v.end(), reinterpret_cast<const uint8_t*>(&x), reinterpret_cast<const uint8_t*>(&x) + 4);
    }
    static void append_u16(std::vector<uint8_t>& v, uint16_t x) {
        v.insert(v.end(), reinterpret_cast<const uint8_t*>(&x), reinterpret_cast<const uint8_t*>(&x) + 2);
    }
    static uint64_t read_u64(const uint8_t* p) { uint64_t x; std::memcpy(&x, p, 8); return x; }
    static uint32_t read_u32(const uint8_t* p) { uint32_t x; std::memcpy(&x, p, 4); return x; }
    static uint16_t read_u16(const uint8_t* p) { uint16_t x; std::memcpy(&x, p, 2); return x; }

    std::vector<uint8_t> data_;
    std::vector<block_index_entry> block_index_;
    size_t count_ = 0;
    bool has_mask_any_ = false;
};

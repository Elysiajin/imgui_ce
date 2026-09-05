#pragma once
#include <immintrin.h>
#include <vector>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <intrin.h>

// 扫描比较操作类型
enum class SimdOp { equal, greater, less, not_equal };

// 反转 SIMD 操作符：equal ↔ not_equal, greater ↔ less
inline SimdOp invert_simd_op(SimdOp op) {
    switch (op) {
    case SimdOp::equal:    return SimdOp::not_equal;
    case SimdOp::not_equal: return SimdOp::equal;
    case SimdOp::greater:  return SimdOp::less;
    case SimdOp::less:     return SimdOp::greater;
    }
    return op;
}

/**
 * @brief SIMD 比较内核
 * @tparam DataType 被扫描的数据类型 (如 int8_t, int32_t, float, double)
 */

template<typename DataType>
struct simd_kernel {
    // ---------- 整数比较 ----------
    static inline __m256i compare_integers(__m256i current_memory_vector, __m256i target_value_vector, SimdOp operation) {
        if constexpr (sizeof(DataType) == 1) {
            if (operation == SimdOp::equal)   return _mm256_cmpeq_epi8(current_memory_vector, target_value_vector);
            if (operation == SimdOp::greater) return _mm256_cmpgt_epi8(current_memory_vector, target_value_vector);
            if (operation == SimdOp::less)    return _mm256_cmpgt_epi8(target_value_vector, current_memory_vector);
        }
        else if constexpr (sizeof(DataType) == 2) {
            if (operation == SimdOp::equal)   return _mm256_cmpeq_epi16(current_memory_vector, target_value_vector);
            if (operation == SimdOp::greater) return _mm256_cmpgt_epi16(current_memory_vector, target_value_vector);
            if (operation == SimdOp::less)    return _mm256_cmpgt_epi16(target_value_vector, current_memory_vector);
        }
        else if constexpr (sizeof(DataType) == 4) {
            if (operation == SimdOp::equal)   return _mm256_cmpeq_epi32(current_memory_vector, target_value_vector);
            if (operation == SimdOp::greater) return _mm256_cmpgt_epi32(current_memory_vector, target_value_vector);
            if (operation == SimdOp::less)    return _mm256_cmpgt_epi32(target_value_vector, current_memory_vector);
        }
        else if constexpr (sizeof(DataType) == 8) {
            if (operation == SimdOp::equal)   return _mm256_cmpeq_epi64(current_memory_vector, target_value_vector);
            if (operation == SimdOp::greater) return _mm256_cmpgt_epi64(current_memory_vector, target_value_vector);
            if (operation == SimdOp::less)    return _mm256_cmpgt_epi64(target_value_vector, current_memory_vector);
        }

        if (operation == SimdOp::not_equal) {
            __m256i eq = compare_integers(current_memory_vector, target_value_vector, SimdOp::equal);
            return _mm256_xor_si256(eq, _mm256_set1_epi8(0xFF)); // 按位取反
        }
        return _mm256_setzero_si256();
    }

    // ---------- 单精度浮点比较 ----------
    static inline __m256 compare_floats_ps(__m256 current_memory_vector, __m256 target_value_vector, SimdOp operation) {
        switch (operation) {
        case SimdOp::equal:    return _mm256_cmp_ps(current_memory_vector, target_value_vector, _CMP_EQ_OQ);
        case SimdOp::greater:  return _mm256_cmp_ps(current_memory_vector, target_value_vector, _CMP_GT_OQ);
        case SimdOp::less:     return _mm256_cmp_ps(current_memory_vector, target_value_vector, _CMP_LT_OQ);
        case SimdOp::not_equal: return _mm256_cmp_ps(current_memory_vector, target_value_vector, _CMP_NEQ_OQ);
        default: return _mm256_setzero_ps();
        }
    }

    // ---------- 双精度浮点比较 ----------
    static inline __m256d compare_doubles_pd(__m256d current_memory_vector, __m256d target_value_vector, SimdOp operation) {
        switch (operation) {
        case SimdOp::equal:    return _mm256_cmp_pd(current_memory_vector, target_value_vector, _CMP_EQ_OQ);
        case SimdOp::greater:  return _mm256_cmp_pd(current_memory_vector, target_value_vector, _CMP_GT_OQ);
        case SimdOp::less:     return _mm256_cmp_pd(current_memory_vector, target_value_vector, _CMP_LT_OQ);
        case SimdOp::not_equal: return _mm256_cmp_pd(current_memory_vector, target_value_vector, _CMP_NEQ_OQ);
        default: return _mm256_setzero_pd();
        }
    }
};

/**
 * @brief SIMD 加速扫描器
 */
class simd_scanner {
private:
    // ── 内部辅助：掩码展开与广播 ──

    // 从 32 字节整数比较结果中提取逐字节的 32-bit mask
    static inline uint32_t mask32_from_epi8(__m256i cmp_result) {
        return static_cast<uint32_t>(_mm256_movemask_epi8(cmp_result));
    }

    // 按类型尺寸将 per-element mask 展开为 per-byte mask
    // type_size: 1,2,4,8
    // cmp_result: 该类型的 SIMD 比较结果
    // op: 操作类型（Equal/not_equal/Greater/Less）
    static inline uint32_t expand_mask_by_type(__m256i cmp_result, size_t type_size, SimdOp op) {
        uint32_t raw = mask32_from_epi8(cmp_result);
        if (type_size == 1) return raw;
        uint32_t result = 0;
        const uint32_t all_bits = (1u << type_size) - 1u;
        // ★ 对于 _mm256_cmpeq_epi32/_epi64 等向量比较指令：
        //   - 元素相等时 → 所有位为 0xFF → movemask 返回 all_bits（如 0xF）
        //   - 元素不等时 → 所有位为 0x00 → movemask 返回 0
        // ★ 对于 not_equal: elem_bits == 0 才代表"不相等"（原始值改变了）
        if (op == SimdOp::not_equal) {
            for (size_t i = 0; i < 32; i += type_size) {
                uint32_t elem_bits = (raw >> i) & all_bits;
                if (elem_bits == 0)          // ★ 修复：elem_bits == 0 → 元素不相等
                    result |= (all_bits << i);
            }
        } else {
            for (size_t i = 0; i < 32; i += type_size) {
                uint32_t elem_bits = (raw >> i) & all_bits;
                if (elem_bits == all_bits)    // elem_bits == all_bits → 元素相等（或条件真）
                    result |= (all_bits << i);
            }
        }
        return result;
    }

    // 从浮点 movemask_ps(8bit) 展开为 per-byte mask
    static inline uint32_t expand_f32_mask(uint32_t ps_mask8bit, SimdOp) {
        uint32_t result = 0;
        for (int i = 0; i < 8; ++i) {
            if (ps_mask8bit & (1u << i)) {
                result |= (0xFu << (i * 4));
            }
        }
        return result;
    }

    // 从浮点 movemask_pd(4bit) 展开为 per-byte mask
    static inline uint32_t expand_f64_mask(uint32_t pd_mask4bit, SimdOp) {
        uint32_t result = 0;
        for (int i = 0; i < 4; ++i) {
            if (pd_mask4bit & (1u << i)) {
                result |= (0xFFu << (i * 8));
            }
        }
        return result;
    }

    // 从 64-bit 值按类型广播
    static inline __m256i broadcast_int8(uint64_t val) {
        return _mm256_set1_epi8(static_cast<int8_t>(val));
    }
    static inline __m256i broadcast_int16(uint64_t val) {
        return _mm256_set1_epi16(static_cast<int16_t>(val));
    }
    static inline __m256i broadcast_int32(uint64_t val) {
        return _mm256_set1_epi32(static_cast<int32_t>(val));
    }
    static inline __m256i broadcast_int64(uint64_t val) {
        return _mm256_set1_epi64x(static_cast<int64_t>(val));
    }
    static inline __m256 broadcast_float(uint64_t raw_bits) {
        float f;
        std::memcpy(&f, &raw_bits, sizeof(float));
        return _mm256_set1_ps(f);
    }
    static inline __m256d broadcast_double(uint64_t raw_bits) {
        double d;
        std::memcpy(&d, &raw_bits, sizeof(double));
        return _mm256_set1_pd(d);
    }

public:
    /**
     * @brief 对一块内存区域执行 SIMD 批量比较，记录所有匹配地址
     * @tparam ScalarType   要比较的数据类型 (int32_t, float …)
     * @param process_memory      进程当前内存数据的起始指针
     * @param target_filled_block  用目标值填充的内存块，大小与 process_memory 相同
     * @param memory_block_size    内存块字节数
     * @param base_address        该内存块在进程空间中的基地址
     * @param alignment          地址对齐要求 (字节)，0 表示按数据类型大小对齐
     * @param operation          比较操作
     * @param out_matched_addresses  输出：所有匹配的地址
     */
    template<typename ScalarType>
    static void scan_memory_block_for_matches(
        const uint8_t* process_memory,
        const uint8_t* target_filled_block,
        size_t                  memory_block_size,
        uint64_t                base_address,
        size_t                  alignment,
        SimdOp                  operation,
        std::vector<uint64_t>& out_matched_addresses)
    {
        const size_t simd_byte_width = 32;           // 一次处理32字节(256位)
        const size_t scalar_size = sizeof(ScalarType);
        const size_t effective_alignment = (alignment > 0) ? alignment : scalar_size;

        // --- 主循环：每次处理一个 32 字节块 ---
        for (size_t offset = 0; offset + simd_byte_width <= memory_block_size; offset += simd_byte_width)
        {
            // 加载当前内存块和目标值块
            __m256i current_chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(process_memory + offset));
            __m256i target_chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(target_filled_block + offset));
            uint32_t match_bitmask = 0;

            // 根据数据类型调用对应的比较，生成匹配掩码
            if constexpr (std::is_same_v<ScalarType, float>) {
                match_bitmask = _mm256_movemask_ps(
                    simd_kernel<ScalarType>::compare_floats_ps(
                        _mm256_castsi256_ps(current_chunk),
                        _mm256_castsi256_ps(target_chunk),
                        operation));
            }
            else if constexpr (std::is_same_v<ScalarType, double>) {
                match_bitmask = _mm256_movemask_pd(
                    simd_kernel<ScalarType>::compare_doubles_pd(
                        _mm256_castsi256_pd(current_chunk),
                        _mm256_castsi256_pd(target_chunk),
                        operation));
            }
            else {
                match_bitmask = _mm256_movemask_epi8(
                    simd_kernel<ScalarType>::compare_integers(current_chunk, target_chunk, operation));
            }

            // 如果有任意匹配，遍历块中的每一个元素
            if (match_bitmask != 0)
            {
                const size_t inner_step = (effective_alignment >= scalar_size) ? scalar_size : effective_alignment;

                for (size_t byte_index = 0; byte_index + scalar_size <= simd_byte_width; byte_index += inner_step) {
                    uint64_t candidate_address = base_address + offset + byte_index;
                    if (candidate_address % effective_alignment != 0) continue;

                    if (effective_alignment >= scalar_size) {
                        // 快速路径：利用 SIMD 掩码直接判断
                        constexpr size_t bits_per_element = std::is_floating_point_v<ScalarType> ? 1 : scalar_size;
                        uint32_t current_elem_mask = (1u << bits_per_element) - 1u;
                        uint32_t shift;
                        if constexpr (std::is_floating_point_v<ScalarType>) {
                            shift = static_cast<uint32_t>(byte_index / scalar_size);
                        } else {
                            shift = static_cast<uint32_t>(byte_index);
                        }
                        uint32_t element_bits = (match_bitmask >> shift) & current_elem_mask;
                        if (element_bits == current_elem_mask) {
                            out_matched_addresses.push_back(candidate_address);
                        }
                    } else {
                        // 非对齐路径：逐字节标量比较
                        ScalarType current_value, target_value;
                        std::memcpy(&current_value, process_memory + offset + byte_index, scalar_size);
                        std::memcpy(&target_value, target_filled_block + offset + byte_index, scalar_size);

                        bool is_match = false;
                        if (operation == SimdOp::equal)        is_match = (current_value == target_value);
                        else if (operation == SimdOp::greater)  is_match = (current_value > target_value);
                        else if (operation == SimdOp::less)     is_match = (current_value < target_value);
                        else if (operation == SimdOp::not_equal) is_match = (current_value != target_value);

                        if (is_match)
                            out_matched_addresses.push_back(candidate_address);
                    }
                }
            }
        }

        // --- 尾部不足 32 字节的部分，用标量方式处理 ---
        size_t start_of_tail = (memory_block_size / simd_byte_width) * simd_byte_width;
        const size_t tail_step = (effective_alignment >= scalar_size) ? scalar_size : effective_alignment;
        for (size_t byte_index = start_of_tail; byte_index + scalar_size <= memory_block_size; byte_index += tail_step)
        {
            uint64_t candidate_address = base_address + byte_index;
            if (candidate_address % effective_alignment != 0)
                continue;

            ScalarType current_value, target_value;
            std::memcpy(&current_value, process_memory + byte_index, scalar_size);
            std::memcpy(&target_value, target_filled_block + byte_index, scalar_size);

            bool is_match = false;
            if (operation == SimdOp::equal)        is_match = (current_value == target_value);
            else if (operation == SimdOp::greater)  is_match = (current_value > target_value);
            else if (operation == SimdOp::less)     is_match = (current_value < target_value);
            else if (operation == SimdOp::not_equal) is_match = (current_value != target_value);

            if (is_match)
                out_matched_addresses.push_back(candidate_address);
        }
    }

    /**
     * @brief SIMD 范围扫描：找出 [range_min, range_max] 区间内的所有值
     * @tparam ScalarType   数据类型 (int32_t, float, double …)
     */
    template<typename ScalarType>
    static void scan_memory_block_for_range(
        const uint8_t* process_memory,
        size_t                  memory_block_size,
        uint64_t                base_address,
        size_t                  alignment,
        ScalarType              range_min,
        ScalarType              range_max,
        std::vector<uint64_t>& out_matched_addresses)
    {
        const size_t simd_byte_width = 32;
        const size_t scalar_size = sizeof(ScalarType);
        const size_t effective_alignment = (alignment > 0) ? alignment : scalar_size;

        for (size_t offset = 0; offset + simd_byte_width <= memory_block_size; offset += simd_byte_width)
        {
            __m256i current_chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(process_memory + offset));
            uint32_t match_bitmask = 0;

            if constexpr (std::is_same_v<ScalarType, float>) {
                __m256 val_vec = _mm256_castsi256_ps(current_chunk);
                __m256 v_min = _mm256_set1_ps(range_min);
                __m256 v_max = _mm256_set1_ps(range_max);
                __m256 ge_mask = _mm256_cmp_ps(val_vec, v_min, _CMP_GE_OQ);
                __m256 le_mask = _mm256_cmp_ps(val_vec, v_max, _CMP_LE_OQ);
                match_bitmask = _mm256_movemask_ps(_mm256_and_ps(ge_mask, le_mask));
            }
            else if constexpr (std::is_same_v<ScalarType, double>) {
                __m256d val_vec = _mm256_castsi256_pd(current_chunk);
                __m256d v_min = _mm256_set1_pd(range_min);
                __m256d v_max = _mm256_set1_pd(range_max);
                __m256d ge_mask = _mm256_cmp_pd(val_vec, v_min, _CMP_GE_OQ);
                __m256d le_mask = _mm256_cmp_pd(val_vec, v_max, _CMP_LE_OQ);
                match_bitmask = _mm256_movemask_pd(_mm256_and_pd(ge_mask, le_mask));
            }
            else {
                __m256i v_min_vec, v_max_vec;
                if constexpr (sizeof(ScalarType) == 1) {
                    v_min_vec = _mm256_set1_epi8(static_cast<int8_t>(range_min));
                    v_max_vec = _mm256_set1_epi8(static_cast<int8_t>(range_max));
                } else if constexpr (sizeof(ScalarType) == 2) {
                    v_min_vec = _mm256_set1_epi16(static_cast<int16_t>(range_min));
                    v_max_vec = _mm256_set1_epi16(static_cast<int16_t>(range_max));
                } else if constexpr (sizeof(ScalarType) == 4) {
                    v_min_vec = _mm256_set1_epi32(static_cast<int32_t>(range_min));
                    v_max_vec = _mm256_set1_epi32(static_cast<int32_t>(range_max));
                } else if constexpr (sizeof(ScalarType) == 8) {
                    v_min_vec = _mm256_set1_epi64x(static_cast<int64_t>(range_min));
                    v_max_vec = _mm256_set1_epi64x(static_cast<int64_t>(range_max));
                }

                __m256i ge_mask = _mm256_xor_si256(
                    simd_kernel<ScalarType>::compare_integers(v_min_vec, current_chunk, SimdOp::greater),
                    _mm256_set1_epi8(0xFF));
                __m256i le_mask = _mm256_xor_si256(
                    simd_kernel<ScalarType>::compare_integers(current_chunk, v_max_vec, SimdOp::greater),
                    _mm256_set1_epi8(0xFF));
                match_bitmask = _mm256_movemask_epi8(_mm256_and_si256(ge_mask, le_mask));
            }

            if (match_bitmask != 0)
            {
                const size_t inner_step = (effective_alignment >= scalar_size) ? scalar_size : effective_alignment;
                for (size_t byte_index = 0; byte_index + scalar_size <= simd_byte_width; byte_index += inner_step) {
                    uint64_t candidate_address = base_address + offset + byte_index;
                    if (candidate_address % effective_alignment != 0) continue;

                    if (effective_alignment >= scalar_size) {
                        constexpr size_t bits_per_element = std::is_floating_point_v<ScalarType> ? 1 : scalar_size;
                        uint32_t current_elem_mask = (1u << bits_per_element) - 1u;
                        uint32_t shift;
                        if constexpr (std::is_floating_point_v<ScalarType>) {
                            shift = static_cast<uint32_t>(byte_index / scalar_size);
                        } else {
                            shift = static_cast<uint32_t>(byte_index);
                        }
                        uint32_t element_bits = (match_bitmask >> shift) & current_elem_mask;
                        if (element_bits == current_elem_mask) {
                            out_matched_addresses.push_back(candidate_address);
                        }
                    } else {
                        ScalarType current_value;
                        std::memcpy(&current_value, process_memory + offset + byte_index, scalar_size);
                        if (current_value >= range_min && current_value <= range_max) {
                            out_matched_addresses.push_back(candidate_address);
                        }
                    }
                }
            }
        }

        size_t start_of_tail = (memory_block_size / simd_byte_width) * simd_byte_width;
        const size_t tail_step = (effective_alignment >= scalar_size) ? scalar_size : effective_alignment;
        for (size_t byte_index = start_of_tail; byte_index + scalar_size <= memory_block_size; byte_index += tail_step)
        {
            uint64_t candidate_address = base_address + byte_index;
            if (candidate_address % effective_alignment != 0) continue;

            ScalarType current_value;
            std::memcpy(&current_value, process_memory + byte_index, scalar_size);
            if (current_value >= range_min && current_value <= range_max) {
                out_matched_addresses.push_back(candidate_address);
            }
        }
    }

    /**
     * @brief SIMD 批量比较两个内存块（当前 vs 上一次快照）
     */
    template<typename ScalarType>
    static void compare_two_memory_blocks(
        const uint8_t* current_memory,
        const uint8_t* previous_memory,
        size_t                  memory_block_size,
        uint64_t                base_address,
        size_t                  alignment,
        SimdOp                  operation,
        std::vector<uint64_t>& out_matched_addresses)
    {
        const size_t simd_byte_width = 32;
        const size_t scalar_size = sizeof(ScalarType);
        const size_t effective_alignment = (alignment > 0) ? alignment : scalar_size;

        for (size_t offset = 0; offset + simd_byte_width <= memory_block_size; offset += simd_byte_width)
        {
            __m256i current_chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(current_memory + offset));
            __m256i previous_chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(previous_memory + offset));
            uint32_t match_bitmask = 0;

            if constexpr (std::is_same_v<ScalarType, float>) {
                __m256 cur = _mm256_castsi256_ps(current_chunk);
                __m256 prev = _mm256_castsi256_ps(previous_chunk);
                switch (operation) {
                case SimdOp::equal:    match_bitmask = _mm256_movemask_ps(_mm256_cmp_ps(cur, prev, _CMP_EQ_OQ)); break;
                case SimdOp::not_equal: match_bitmask = _mm256_movemask_ps(_mm256_cmp_ps(cur, prev, _CMP_NEQ_OQ)); break;
                case SimdOp::greater:  match_bitmask = _mm256_movemask_ps(_mm256_cmp_ps(cur, prev, _CMP_GT_OQ)); break;
                case SimdOp::less:     match_bitmask = _mm256_movemask_ps(_mm256_cmp_ps(cur, prev, _CMP_LT_OQ)); break;
                }
            } else if constexpr (std::is_same_v<ScalarType, double>) {
                __m256d cur = _mm256_castsi256_pd(current_chunk);
                __m256d prev = _mm256_castsi256_pd(previous_chunk);
                switch (operation) {
                case SimdOp::equal:    match_bitmask = _mm256_movemask_pd(_mm256_cmp_pd(cur, prev, _CMP_EQ_OQ)); break;
                case SimdOp::not_equal: match_bitmask = _mm256_movemask_pd(_mm256_cmp_pd(cur, prev, _CMP_NEQ_OQ)); break;
                case SimdOp::greater:  match_bitmask = _mm256_movemask_pd(_mm256_cmp_pd(cur, prev, _CMP_GT_OQ)); break;
                case SimdOp::less:     match_bitmask = _mm256_movemask_pd(_mm256_cmp_pd(cur, prev, _CMP_LT_OQ)); break;
                }
            } else {
                switch (operation) {
                case SimdOp::equal:    match_bitmask = _mm256_movemask_epi8(simd_kernel<ScalarType>::compare_integers(current_chunk, previous_chunk, SimdOp::equal)); break;
                case SimdOp::not_equal: match_bitmask = _mm256_movemask_epi8(simd_kernel<ScalarType>::compare_integers(current_chunk, previous_chunk, SimdOp::not_equal)); break;
                case SimdOp::greater:  match_bitmask = _mm256_movemask_epi8(simd_kernel<ScalarType>::compare_integers(current_chunk, previous_chunk, SimdOp::greater)); break;
                case SimdOp::less:     match_bitmask = _mm256_movemask_epi8(simd_kernel<ScalarType>::compare_integers(current_chunk, previous_chunk, SimdOp::less)); break;
                }
            }

            if (match_bitmask != 0)
            {
                const size_t inner_step = (effective_alignment >= scalar_size) ? scalar_size : effective_alignment;
                for (size_t byte_index = 0; byte_index + scalar_size <= simd_byte_width; byte_index += inner_step) {
                    uint64_t candidate_address = base_address + offset + byte_index;
                    if (candidate_address % effective_alignment != 0) continue;

                    if (effective_alignment >= scalar_size) {
                        constexpr size_t bits_per_element = std::is_floating_point_v<ScalarType> ? 1 : scalar_size;
                        uint32_t current_elem_mask = (1u << bits_per_element) - 1u;
                        uint32_t shift;
                        if constexpr (std::is_floating_point_v<ScalarType>) {
                            shift = static_cast<uint32_t>(byte_index / scalar_size);
                        } else {
                            shift = static_cast<uint32_t>(byte_index);
                        }
                        uint32_t element_bits = (match_bitmask >> shift) & current_elem_mask;
                        if (element_bits == current_elem_mask) {
                            out_matched_addresses.push_back(candidate_address);
                        }
                    } else {
                        ScalarType cur_val, old_val;
                        std::memcpy(&cur_val, current_memory + offset + byte_index, scalar_size);
                        std::memcpy(&old_val, previous_memory + offset + byte_index, scalar_size);
                        bool is_match = false;
                        if (operation == SimdOp::equal)        is_match = (cur_val == old_val);
                        else if (operation == SimdOp::not_equal) is_match = (cur_val != old_val);
                        else if (operation == SimdOp::greater)  is_match = (cur_val > old_val);
                        else if (operation == SimdOp::less)     is_match = (cur_val < old_val);
                        if (is_match)
                            out_matched_addresses.push_back(candidate_address);
                    }
                }
            }
        }

        size_t start_of_tail = (memory_block_size / simd_byte_width) * simd_byte_width;
        const size_t tail_step = (effective_alignment >= scalar_size) ? scalar_size : effective_alignment;
        for (size_t byte_index = start_of_tail; byte_index + scalar_size <= memory_block_size; byte_index += tail_step)
        {
            uint64_t candidate_address = base_address + byte_index;
            if (candidate_address % effective_alignment != 0) continue;

            ScalarType cur_val, old_val;
            std::memcpy(&cur_val, current_memory + byte_index, scalar_size);
            std::memcpy(&old_val, previous_memory + byte_index, scalar_size);
            bool is_match = false;
            if (operation == SimdOp::equal)        is_match = (cur_val == old_val);
            else if (operation == SimdOp::not_equal) is_match = (cur_val != old_val);
            else if (operation == SimdOp::greater)  is_match = (cur_val > old_val);
            else if (operation == SimdOp::less)     is_match = (cur_val < old_val);
            if (is_match)
                out_matched_addresses.push_back(candidate_address);
        }
    }

    /**
     * @brief 快速查找某个字节值在内存中所有出现的位置
     */
    static void find_first_char(
        const uint8_t* memory_to_search,
        size_t                  memory_size,
        uint8_t                 byte_to_find,
        std::vector<size_t>& out_found_offsets)
    {
        const size_t simd_byte_width = 32;
        __m256i broadcast_byte = _mm256_set1_epi8(static_cast<char>(byte_to_find));

        for (size_t offset = 0; offset + simd_byte_width <= memory_size; offset += simd_byte_width)
        {
            __m256i current_chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(memory_to_search + offset));
            uint32_t match_bitmask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(current_chunk, broadcast_byte));

            while (match_bitmask != 0)
            {
                unsigned long bit_position;
                if (_BitScanForward(&bit_position, match_bitmask)) {
                    out_found_offsets.push_back(offset + bit_position);
                    match_bitmask &= ~(1 << bit_position);
                }
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  All 类型扫描专用批量加速接口
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief All 类型 首次扫描（exact_value）批量加速
     *
     * 一次性加载 32 字节，同时做 6 种类型的 SIMD 比较。
     * 对每个偏移，记录所有能匹配的数据类型（完整 type_mask）。
     * 调用者负责处理尾部不足32字节的部分（标量兜底）。
     */
    static void scan_all_types_first(
        const uint8_t* mem_buf, size_t mem_size,
        uint64_t base_addr,
        const uint64_t type_values[6],
        SimdOp op,
        std::vector<std::pair<uint64_t, uint16_t>>& out_pairs)
    {
        constexpr size_t BLOCK = 32;
        static constexpr uint8_t k_type_size[] = { 1, 2, 4, 8, 4, 8 };

        for (size_t off = 0; off + BLOCK <= mem_size; off += BLOCK) {
            __m256i chunk = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(mem_buf + off));
            uint64_t chunk_base = base_addr + off;

            // ── 一次性计算 6 种类型的 per-byte 匹配掩码 ──
            uint32_t masks[6];

            // Byte (0)
            masks[0] = _mm256_movemask_epi8(
                simd_kernel<int8_t>::compare_integers(chunk, broadcast_int8(type_values[0]), op));

            // Int16 (1)
            masks[1] = expand_mask_by_type(
                simd_kernel<int16_t>::compare_integers(chunk, broadcast_int16(type_values[1]), op), 2, op);

            // Int32 (2)
            masks[2] = expand_mask_by_type(
                simd_kernel<int32_t>::compare_integers(chunk, broadcast_int32(type_values[2]), op), 4, op);

            // Int64 (3)
            masks[3] = expand_mask_by_type(
                simd_kernel<int64_t>::compare_integers(chunk, broadcast_int64(type_values[3]), op), 8, op);

            // Float32 (4): _mm256_cmp_ps → 8-bit mask → expand to 32-bit
            {
                __m256 f_cmp = simd_kernel<float>::compare_floats_ps(
                    _mm256_castsi256_ps(chunk), broadcast_float(type_values[4]), op);
                masks[4] = expand_f32_mask(_mm256_movemask_ps(f_cmp), op);
            }

            // Float64 (5): _mm256_cmp_pd → 4-bit mask → expand to 32-bit
            {
                __m256d d_cmp = simd_kernel<double>::compare_doubles_pd(
                    _mm256_castsi256_pd(chunk), broadcast_double(type_values[5]), op);
                masks[5] = expand_f64_mask(_mm256_movemask_pd(d_cmp), op);
            }

            // ── 对 32 字节逐个偏移，构建完整 type_mask ──
            for (size_t byte_off = 0; byte_off < BLOCK; ++byte_off) {
                uint32_t bit = 1u << byte_off;
                uint16_t type_mask = 0;
                bool any_match = false;
                // 检查每种类型
                for (int ti = 0; ti < 6; ++ti) {
                    if (byte_off % k_type_size[ti] != 0) continue;
                    if (masks[ti] & bit) {
                        type_mask |= (1 << ti);
                        any_match = true;
                    }
                }
                if (any_match) {
                    out_pairs.emplace_back(chunk_base + byte_off, type_mask);
                }
            }
        }
    }

    /**
     * @brief All 类型 unknown_initial 再次扫描 — Changed/Unchanged 专用
     *
     * 核心：_mm256_cmpeq_epi8(cur, old) 一次性出 32 字节相等掩码，
     * 对每个偏移，记录所有能匹配的数据类型（完整 type_mask）。
     */
    static void scan_all_types_changed_unchanged(
        const uint8_t* cur_buf, const uint8_t* old_buf,
        size_t mem_size, uint64_t base_addr,
        bool is_unchanged,
        std::vector<std::pair<uint64_t, uint16_t>>& out_pairs)
    {
        constexpr size_t BLOCK = 32;
        static constexpr uint8_t k_type_size[] = { 1, 2, 4, 8, 4, 8 };

        for (size_t off = 0; off + BLOCK <= mem_size; off += BLOCK) {
            __m256i cur = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(cur_buf + off));
            __m256i old = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(old_buf + off));
            uint64_t chunk_base = base_addr + off;

            // 核心：逐字节相等掩码
            uint32_t eq_mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(cur, old));

            // ── Byte (0) ──
            uint32_t m0 = is_unchanged ? eq_mask : ~eq_mask;

            // ── Int16 (1)：每连续 2 字节都满足条件 ──
            uint32_t m1;
            if (is_unchanged) {
                m1 = eq_mask & (eq_mask >> 1);
                m1 = (m1 & 0x55555555u);        // 保留偶数偏移
                m1 = (m1 << 1) | m1;            // 展开回 2 字节
            } else {
                uint32_t neq = ~eq_mask;
                m1 = neq | (neq >> 1);
                m1 = (m1 & 0x55555555u);
                m1 = (m1 << 1) | m1;
            }

            // ── Int32 (2)：每连续 4 字节都满足条件 ──
            uint32_t m2;
            {
                __m256i cmp = _mm256_cmpeq_epi32(cur, old);
                m2 = expand_mask_by_type(cmp, 4, is_unchanged ? SimdOp::equal : SimdOp::not_equal);
            }

            // ── Float32 (4)：同 Int32（4字节对齐） ──
            uint32_t m4;
            {
                __m256 f_cur = _mm256_castsi256_ps(cur);
                __m256 f_old = _mm256_castsi256_ps(old);
                uint32_t ps8 = _mm256_movemask_ps(
                    is_unchanged ? _mm256_cmp_ps(f_cur, f_old, _CMP_EQ_OQ)
                                : _mm256_cmp_ps(f_cur, f_old, _CMP_NEQ_OQ));
                m4 = expand_f32_mask(ps8, SimdOp::equal);
            }

            // ── Int64 (3)：每连续 8 字节都满足条件 ──
            uint32_t m3;
            {
                __m256i cmp = _mm256_cmpeq_epi64(cur, old);
                m3 = expand_mask_by_type(cmp, 8, is_unchanged ? SimdOp::equal : SimdOp::not_equal);
            }

            // ── Float64 (5)：同 Int64（8字节对齐） ──
            uint32_t m5;
            {
                __m256d d_cur = _mm256_castsi256_pd(cur);
                __m256d d_old = _mm256_castsi256_pd(old);
                uint32_t pd4 = _mm256_movemask_pd(
                    is_unchanged ? _mm256_cmp_pd(d_cur, d_old, _CMP_EQ_OQ)
                                : _mm256_cmp_pd(d_cur, d_old, _CMP_NEQ_OQ));
                m5 = expand_f64_mask(pd4, SimdOp::equal);
            }

            // ── 对 32 字节逐个偏移，构建完整 type_mask ──
            uint32_t masks[] = { m0, m1, m2, m3, m4, m5 };
            for (size_t byte_off = 0; byte_off < BLOCK; ++byte_off) {
                uint32_t bit = 1u << byte_off;
                uint16_t type_mask = 0;
                bool any_match = false;
                for (int ti = 0; ti < 6; ++ti) {
                    if (byte_off % k_type_size[ti] != 0) continue;
                    if (masks[ti] & bit) {
                        type_mask |= (1 << ti);
                        any_match = true;
                    }
                }
                if (any_match) {
                    out_pairs.emplace_back(chunk_base + byte_off, type_mask);
                }
            }
        }
    }
};

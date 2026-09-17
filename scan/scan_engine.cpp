#include "scan\scan_engine.h"
#include "core\process_manager.h"
#include "scan\thread_pool.h"
#include "scan\scan_value_target.h"
#include <cstring>
#include <unordered_map>

// =============================================================================
// 辅助函数：将 scan_type / next_scan_type 转换为 SimdOp（用于 All 类型 SIMD 加速）
// =============================================================================
static inline SimdOp scan_first_type_to_simd_op(scan_type st) {
    switch (st) {
    case scan_type::exact_value:   return SimdOp::equal;
    case scan_type::greater_than:  return SimdOp::greater;
    case scan_type::less_than:     return SimdOp::less;
    default:                     return SimdOp::equal; // fallback
    }
}
static inline SimdOp scan_next_type_to_simd_op(next_scan_type nt) {
    switch (nt) {
    case next_scan_type::equal:    return SimdOp::equal;
    case next_scan_type::not_equal: return SimdOp::not_equal;
    case next_scan_type::changed:  return SimdOp::not_equal;
    case next_scan_type::unchanged:return SimdOp::equal;
    case next_scan_type::increased:return SimdOp::greater;
    case next_scan_type::decreased:return SimdOp::less;
    default:                     return SimdOp::equal;
    }
}
// 判断是否可以使用 SIMD All 加速（首次扫描 exact_value/greater_than/less_than，无近似值，非 not_match）
static inline bool can_use_simd_all_first(const scan_request& req) {
    return req.alignment == 1
        && !req.contain_approximate_value
        && !req.not_match
        && (req.first_type == scan_type::exact_value ||
            req.first_type == scan_type::greater_than ||
            req.first_type == scan_type::less_than);
}
// 判断是否可以使用 SIMD All 加速（再次扫描 Changed/Unchanged，非 not_match）
static inline bool can_use_simd_all_changed_unchanged(const scan_request& req) {
    return req.alignment == 1
        && !req.not_match
        && (req.next_type == next_scan_type::changed ||
            req.next_type == next_scan_type::unchanged);
}

// =============================================================================
// 统一的首次扫描比较泛型函数

static bool need_old_value_for_next_scan(next_scan_type nt) {
    switch (nt) {
    case next_scan_type::increased:
    case next_scan_type::decreased:
    case next_scan_type::changed:
    case next_scan_type::unchanged:
    case next_scan_type::increased_by:
    case next_scan_type::decreased_by:
    case next_scan_type::compare_to_first_scan:
        return true;
    default:
        return false;
    }
}

template<typename T>
static inline bool compare_value_first(T val, T v1, T v2, scan_type st, bool use_approx, bool not_match) {
    bool match = false;
    // 如果是浮点数且勾选了近似值，由外层计算好 v1(min) 和 v2(max)，转为 Between 区间判定
    if constexpr (std::is_floating_point_v<T>) {
        if (use_approx && st == scan_type::exact_value) {
            match = (val >= v1 && val <= v2);
            return not_match ? !match : match;
        }
    }
    
    switch (st) {
        case scan_type::exact_value:  match = (val == v1); break;
        case scan_type::greater_than: match = (val >  v1); break;
        case scan_type::less_than:    match = (val <  v1); break;
        case scan_type::between:     match = (val >= v1 && val <= v2); break;
        default: break;
    }
    return not_match ? !match : match;
}

// 统一的再次扫描比较泛型函数
template<typename T>
static inline bool compare_value_next(T cur, T old, T v1, T v2, next_scan_type nt, bool use_approx, bool not_match) {
    bool match = false;
    if constexpr (std::is_floating_point_v<T>) {
        if (use_approx && (nt == next_scan_type::equal || nt == next_scan_type::not_equal)) {
            bool in_range = (cur >= v1 && cur <= v2);
            match = (nt == next_scan_type::equal) ? in_range : !in_range;
            return not_match ? !match : match;
        }
    }

    switch (nt) {
        case next_scan_type::equal:     match = (cur == v1); break;
        case next_scan_type::not_equal:  match = (cur != v1); break;
        case next_scan_type::greater_than: match = (cur >  v1); break;
        case next_scan_type::less_than:    match = (cur <  v1); break;
        case next_scan_type::increased:   match = (cur >  old); break;
        case next_scan_type::decreased:   match = (cur <  old); break;
        case next_scan_type::changed:     match = (cur != old); break;
        case next_scan_type::unchanged:   match = (cur == old); break;
        case next_scan_type::between:     match = (cur >= v1 && cur <= v2); break;
        // ★ 修正：CE 的"增加了多少/减少了多少"是"精确等于旧值±增量"，
        //   而非"大于/小于"。参考 memscan.pas：newvalue == oldvalue(+/-)value。
        case next_scan_type::increased_by: match = (cur == old + v1); break;
        case next_scan_type::decreased_by: match = (cur == old - v1); break;
        case next_scan_type::ignore_value: match = true; break;   // 忽略值：保留当前结果
        case next_scan_type::compare_to_first_scan: match = (cur == old); break;
        default: break;
    }
    return not_match ? !match : match;
}

// 1. 定义函数指针别名
using FirstMatchFn = bool(*)(const uint8_t* ptr, uint64_t raw_v1, uint64_t raw_v2, const scan_request& req);
using NextMatchFn  = bool(*)(const uint8_t* cur_ptr, const uint8_t* old_ptr, uint64_t raw_v1, uint64_t raw_v2, const scan_request& req);

// 2. 泛型实例化机制：负责类型安全的内存转换与位还原
template<typename T>
static bool instantiate_first_match(const uint8_t* ptr, uint64_t raw_v1, uint64_t raw_v2, const scan_request& req) {
    T val; std::memcpy(&val, ptr, sizeof(T));
    T v1, v2;
    if constexpr (std::is_floating_point_v<T>) {
        // 浮点数边界在外部已转换为位模式，在此进行还原
        std::memcpy(&v1, &raw_v1, sizeof(T));
        std::memcpy(&v2, &raw_v2, sizeof(T));
    } else {
        v1 = static_cast<T>(raw_v1);
        v2 = static_cast<T>(raw_v2);
    }
    return compare_value_first<T>(val, v1, v2, req.first_type, req.contain_approximate_value, req.not_match);
}

template<typename T>
static bool instantiate_next_match(const uint8_t* cur_ptr, const uint8_t* old_ptr, uint64_t raw_v1, uint64_t raw_v2, const scan_request& req) {
    // ★ Changed/Unchanged 使用 memcmp 位比较，避免浮点 NaN（NaN != NaN → true）导致误匹配
    // ★ 注意处理 req.not_match：not_match+Changed = 未改变（取反），not_match+Unchanged = 改变了（取反）
    if (req.next_type == next_scan_type::changed) {
        if (!old_ptr) return false;
        bool is_changed = (std::memcmp(cur_ptr, old_ptr, sizeof(T)) != 0);
        return req.not_match ? !is_changed : is_changed;
    }
    if (req.next_type == next_scan_type::unchanged) {
        if (!old_ptr) return false;
        bool is_unchanged = (std::memcmp(cur_ptr, old_ptr, sizeof(T)) == 0);
        return req.not_match ? !is_unchanged : is_unchanged;
    }

    T cur; std::memcpy(&cur, cur_ptr, sizeof(T));
    T old = 0;
    if (old_ptr) {
        std::memcpy(&old, old_ptr, sizeof(T));
    }
    T v1, v2;
    if constexpr (std::is_floating_point_v<T>) {
        std::memcpy(&v1, &raw_v1, sizeof(T));
        std::memcpy(&v2, &raw_v2, sizeof(T));
    } else {
        v1 = static_cast<T>(raw_v1);
        v2 = static_cast<T>(raw_v2);
    }
    return compare_value_next<T>(cur, old, v1, v2, req.next_type, req.contain_approximate_value, req.not_match);
}

// 3. 构建统一的元跳转表项结构
struct AllTypeInvokerEntry {
    scan_data_type type;
    size_t size;
    size_t alignment;
    FirstMatchFn match_first;
    NextMatchFn  match_next;
};

// 4. 全数据类型静态匹配跳转阵列 (严格遵循 CE 官方顺序: Byte -> Int16 -> Int32 -> Int64 -> Float -> Double)
static constexpr AllTypeInvokerEntry k_all_type_invokers[] = {
    { scan_data_type::int8,    1, 1, &instantiate_first_match<int8_t>,   &instantiate_next_match<int8_t>   },
    { scan_data_type::int16,   2, 2, &instantiate_first_match<int16_t>,  &instantiate_next_match<int16_t>  },
    { scan_data_type::int32,   4, 4, &instantiate_first_match<int32_t>,  &instantiate_next_match<int32_t>  },
    { scan_data_type::int64,   8, 8, &instantiate_first_match<int64_t>,  &instantiate_next_match<int64_t>  },
    { scan_data_type::float32, 4, 4, &instantiate_first_match<float>,    &instantiate_next_match<float>    },
    { scan_data_type::float64, 8, 8, &instantiate_first_match<double>,   &instantiate_next_match<double>   }
};
static constexpr int k_all_num_types = 6;

// 建快照阶段要读的字节数换算成"进度单位"，让进度条在这一段也能动起来。
static int snapshot_units_for(const std::vector<memory_region>& regions)
{
	uint64_t total = 0;
	for (const auto& r : regions) total += r.size;
	const uint64_t unit = process_memory_snapshot_manager::k_progress_unit_bytes;
	return static_cast<int>((total + unit - 1) / unit);
}

// 把结果池里的地址全部取出来（分页读，避免一次性拷贝大数组）
static std::vector<uint64_t> collect_result_addresses(
	const std::shared_ptr<adaptive_cache_pool<scan_result>>& cache)
{
	std::vector<uint64_t> addrs;
	const size_t n = cache->total_size();
	addrs.reserve(n);
	constexpr size_t k_page = 65536;
	for (size_t off = 0; off < n; off += k_page) {
		auto page = cache->read_chunk(off, k_page);
		for (const auto& r : page) addrs.push_back(r.address);
	}
	return addrs;
}


scan_engine::scan_engine(process_memory_snapshot_manager* process_snapshot_manager):
	m_process_snapshot_manager(process_snapshot_manager) 
{

}

scan_engine::scan_report scan_engine::execute(const scan_request& request, const std::vector<scan_result>& prev_results) {
	m_cancel.store(false);
	m_progress.store(0);
	auto results = std::make_shared<adaptive_cache_pool<scan_result>>(THEAD_LOCAL_SIZE);

	// 适配全部数据类型
	switch (request.data_type) {
	case scan_data_type::bit:     dispatch_scan<uint8_t>(request, prev_results, results); break;
	case scan_data_type::int8:    dispatch_scan<int8_t>(request, prev_results, results); break;
	case scan_data_type::int16:   dispatch_scan<int16_t>(request, prev_results, results); break;
	case scan_data_type::int32:   dispatch_scan<int32_t>(request, prev_results, results); break;
	case scan_data_type::int64:   dispatch_scan<int64_t>(request, prev_results, results); break;
	case scan_data_type::float32: dispatch_scan<float>(request, prev_results, results); break;
	case scan_data_type::float64: dispatch_scan<double>(request, prev_results, results); break;
	case scan_data_type::ascii_string:
	case scan_data_type::utf8_string:
	case scan_data_type::utf16_string:
	case scan_data_type::byte_array:
		dispatch_scan<uint8_t>(request, prev_results, results); break;
	case scan_data_type::all:
		dispatch_all_scan(request, prev_results, results);
		break;
	case scan_data_type::structure: break;
	}
	return { results, request.data_type};
}

// =============================================================================
// dispatch_all_scan — All 扫描调度入口
// =============================================================================
void scan_engine::dispatch_all_scan(const scan_request& request,
	const std::vector<scan_result>& prev_results,
	std::shared_ptr<adaptive_cache_pool<scan_result>> out_cache)
{
	auto regions = process_manager::instance().get_memory_regions(request);

	// ★ 进度条：把"读目标进程内存、建快照"这一段也算进总进度（见 dispatch_scan 注释）
	const bool need_full_snapshot =
		(request.mode == scan_mode::first && request.first_type == scan_type::unknown_initial) ||
		(request.mode == scan_mode::next && prev_results.empty() && m_potential_address.load() > 0);
	const int snap_units = need_full_snapshot ? snapshot_units_for(regions) : 0;
	const int scan_items = (request.mode == scan_mode::first)
		? static_cast<int>(regions.size())
		: ((prev_results.empty() && m_potential_address.load() > 0)
			? static_cast<int>(regions.size())
			: static_cast<int>(prev_results.size()));
	m_total_items.store(snap_units + scan_items);

	std::shared_ptr<i_process_memory_snapshot> current_snap;
	if (need_full_snapshot)
		current_snap = m_process_snapshot_manager->create_snapshot(regions,
			[this](int units) { m_progress.fetch_add(units, std::memory_order_relaxed); });
	else
		current_snap = m_process_snapshot_manager->create_live_snapshot();
	auto prev_snap = m_process_snapshot_manager->get_previous_process_memory_snapshot();

	std::vector<std::future<void>> futures;

	if (request.mode == scan_mode::first) {
		if (request.first_type == scan_type::unknown_initial) {
			m_potential_address.store(0);
			for (const auto& region : regions) {
				size_t count = region.size / 1;
				m_potential_address.fetch_add(static_cast<int>(count), std::memory_order_relaxed);
				m_progress.fetch_add(1);
			}
			m_process_snapshot_manager->set_first_snapshot(current_snap);
			m_process_snapshot_manager->set_previous_snapshot(current_snap);
		} else {
			for (const auto& region : regions) {
				futures.push_back(global_thread_pool::instance().enqueue(
					[this, request, region, current_snap, out_cache] {
						task_first_scan_all(request, region, current_snap, out_cache);
					}));
			}
			// first 快照在扫描结束后用结果地址构造（见尾部）
		}
	} else {
		if (prev_results.empty() && m_potential_address.load() > 0) {
			m_potential_address.store(0);
			for (const auto& region : regions) {
				futures.push_back(global_thread_pool::instance().enqueue(
					[this, request, region, current_snap, prev_snap, out_cache] {
						task_full_scan_with_next_condition_all(request, region, current_snap, prev_snap, out_cache);
					}));
			}
		} else {
			const size_t batch_size = 4096;
			for (size_t i = 0; i < prev_results.size(); i += batch_size) {
				std::vector<scan_result> batch;
				size_t end = (std::min)(i + batch_size, prev_results.size());
				batch.assign(prev_results.begin() + i, prev_results.begin() + end);
				futures.push_back(global_thread_pool::instance().enqueue(
					[this, request, batch, current_snap, prev_snap, out_cache] {
						task_next_scan_all(request, batch, current_snap, prev_snap, out_cache);
					}));
			}
		}
	}

	for (auto& fut : futures) {
		if (fut.valid()) fut.get();
	}

	if (need_full_snapshot) {
		m_process_snapshot_manager->set_previous_snapshot(current_snap);
	} else {
		// All 类型每个地址读 8 字节（覆盖 int64/double），稀疏快照按 8 字节保存
		auto sparse = m_process_snapshot_manager->create_sparse_snapshot(
			collect_result_addresses(out_cache), 8, current_snap);
		m_process_snapshot_manager->set_previous_snapshot(sparse);
		if (request.mode == scan_mode::first)
			m_process_snapshot_manager->set_first_snapshot(sparse);
	}
}

/// 官方 CE 兼容的 All 类型选择：
/// - 再次扫描：按小→大顺序检查，Byte→Int16→Int32→Float32→Int64→Float64，
///   任何对齐兼容类型都会匹配。
template<bool IsFirst>
static uint16_t pick_all_matches(const uint8_t* data_ptr, const uint8_t* old_ptr,
    uint64_t addr, size_t max_avail_size,
    const uint64_t type_v1[], const uint64_t type_v2[],
    const scan_request& request)
{
    uint16_t mask = 0;
    for (int ti = 0; ti < k_all_num_types; ++ti) {
        const auto& invoker = k_all_type_invokers[ti];
        
        // 检查对齐和剩余空间
        if (addr % invoker.alignment != 0) continue;
        if (invoker.size > max_avail_size) continue;

        bool is_match = false;
        if constexpr (IsFirst) {
            is_match = invoker.match_first(data_ptr, type_v1[ti], type_v2[ti], request);
        } else {
            is_match = invoker.match_next(data_ptr, old_ptr, type_v1[ti], type_v2[ti], request);
        }

        if (is_match) {
            mask |= (1 << ti); // 记录所有匹配的类型位
        }
    }
    return mask;
}

// =============================================================================
// task_first_scan_all — All 首次扫描：对每个对齐地址逐类型尝试
// =============================================================================
void scan_engine::task_first_scan_all(const scan_request& request, memory_region region,
	std::shared_ptr<i_process_memory_snapshot> current_snap,
	std::shared_ptr<adaptive_cache_pool<scan_result>> out_cache)
{
	if (m_cancel.load()) return;
	if (region.size == 0) { m_progress.fetch_add(1); return; }

	auto* p = std::get_if<value_params>(&request.params);
	uint64_t type_v1[k_all_num_types] = {0};
	uint64_t type_v2[k_all_num_types] = {0};

	if (p) {
		// 整数类型(0~3)直接传递原始整数值
		for (int ti = 0; ti < 4; ++ti) {
			type_v1[ti] = p->value1;
			type_v2[ti] = p->value2;
		}
		// ★ 修复：浮点类型(4=Float32,5=Float64)将输入的整数值转换为正确的浮点数值
		//   例：输入值 0x7FF700000000（十进制 549609508634624），
		//   Float32 应为 549609508634624.0f，Float64 应为 549609508634624.0
		{
			float f1 = static_cast<float>(p->value1);
			std::memcpy(&type_v1[4], &f1, sizeof(float));
			double d1 = static_cast<double>(p->value1);
			std::memcpy(&type_v1[5], &d1, sizeof(double));
		}
		{
			float f2 = static_cast<float>(p->value2);
			std::memcpy(&type_v2[4], &f2, sizeof(float));
			double d2 = static_cast<double>(p->value2);
			std::memcpy(&type_v2[5], &d2, sizeof(double));
		}
		// 近似值容差基于已正确转换的浮点值
		if (request.contain_approximate_value && request.first_type == scan_type::exact_value) {
			// Float32 (index 4)
			{
				float target; std::memcpy(&target, &type_v1[4], sizeof(float));
				float lo = target * 0.95f, hi = target * 1.05f;
				if (target >= 0 && lo < -0.0001f) lo = 0.0f;
				if (hi - lo < 0.0001f) { lo = target - 0.0001f; hi = target + 0.0001f; }
				std::memcpy(&type_v1[4], &lo, sizeof(float));
				std::memcpy(&type_v2[4], &hi, sizeof(float));
			}
			// Float64 (index 5)
			{
				double target; std::memcpy(&target, &type_v1[5], sizeof(double));
				double lo = target * 0.95, hi = target * 1.05;
				if (target >= 0 && lo < -0.0001) lo = 0.0;
				if (hi - lo < 0.0001) { lo = target - 0.0001; hi = target + 0.0001; }
				std::memcpy(&type_v1[5], &lo, sizeof(double));
				std::memcpy(&type_v2[5], &hi, sizeof(double));
			}
		}
	}

	const size_t chunk_size = 64 * 1024;
	const size_t max_read_size = 8;
	std::vector<uint8_t> mem_buf(chunk_size + max_read_size);
	std::vector<scan_result> batch_results;
	batch_results.reserve(2048);

	// ★ SIMD 快速路径判定：alignment == 1 && 无近似值
	const bool use_simd = can_use_simd_all_first(request);
	const SimdOp simd_op = scan_first_type_to_simd_op(request.first_type);

	for (size_t base_offset = 0; base_offset < region.size && !m_cancel.load(); base_offset += chunk_size) {
		size_t to_read = (std::min)(chunk_size, region.size - base_offset);
		uint64_t chunk_base = region.base + base_offset;
		if (!current_snap->read_data(chunk_base, mem_buf.data(), to_read)) continue;

		if (use_simd) {
				// SIMD 快速路径：一次 32 字节，6 种类型同时比较。
				// 内核直接追加 scan_result，省去 pair 中间缓冲与逐条拷贝。
				size_t simd_bytes = (to_read / 32) * 32;
				if (simd_bytes > 0) {
					simd_scanner::scan_all_types_first(
						mem_buf.data(), simd_bytes, chunk_base,
						type_v1, simd_op, batch_results);
					if (batch_results.size() >= 1024) {
						out_cache->push_back_batch(batch_results);
						batch_results.clear();
					}
				}
			// 尾部 < 32 字节：标量兜底
			for (size_t off = simd_bytes; off + 1 <= to_read; off += 1) {
				uint64_t addr = chunk_base + off;
				size_t max_avail = to_read - off;
				uint16_t mask = pick_all_matches<true>(
					mem_buf.data() + off, nullptr,
					addr, max_avail,
					type_v1, type_v2, request);
				if (mask != 0) {
					batch_results.push_back({ addr, mask });
					if (batch_results.size() >= 1024) {
						out_cache->push_back_batch(batch_results);
						batch_results.clear();
					}
				}
			}
		} else {
			// ── 标量回退 ──
			for (size_t off = 0; off + 1 <= to_read; off += 1) {
				uint64_t addr = chunk_base + off;
				size_t max_avail = to_read - off;
				uint16_t mask = pick_all_matches<true>(
					mem_buf.data() + off, nullptr,
					addr, max_avail,
					type_v1, type_v2, request);

				if (mask != 0) {
					batch_results.push_back({ addr, mask });
					if (batch_results.size() >= 1024) {
						out_cache->push_back_batch(batch_results);
						batch_results.clear();
					}
				}
			}
		}
	}

	if (!batch_results.empty()) out_cache->push_back_batch(batch_results);
	m_progress.fetch_add(1);
}

// 辅助函数：在 All 类型列表中查找给定 scan_data_type 的索引
// 返回 -1 表示未找到（通常是字符串/AOB等非数值类型）
static inline int find_type_index(scan_data_type dt) {
    for (int i = 0; i < k_all_num_types; ++i) {
        if (k_all_type_invokers[i].type == dt) return i;
    }
    return -1;
}

// =============================================================================
// task_next_scan_all — All 再次扫描：对每个地址重新尝试所有类型
// =============================================================================
void scan_engine::task_next_scan_all(const scan_request& request,
	const std::vector<scan_result>& old_batch,
	std::shared_ptr<i_process_memory_snapshot> current_snapshot,
	std::shared_ptr<i_process_memory_snapshot> previous_snapshot,
	std::shared_ptr<adaptive_cache_pool<scan_result>> out_cache)
{
	if (m_cancel.load()) return;

	auto first_snap = m_process_snapshot_manager->get_first_process_memory_snapshot();
	auto* p = std::get_if<value_params>(&request.params);
	uint64_t type_v1[k_all_num_types] = {0};
	uint64_t type_v2[k_all_num_types] = {0};

	if (p) {
		// 整数类型(0~3)直接传递原始整数值
		for (int ti = 0; ti < 4; ++ti) {
			type_v1[ti] = p->value1;
			type_v2[ti] = p->value2;
		}
		// ★ 修复：浮点类型(4=Float32,5=Float64)将输入的整数值转换为正确的浮点数值
		{
			float f1 = static_cast<float>(p->value1);
			std::memcpy(&type_v1[4], &f1, sizeof(float));
			double d1 = static_cast<double>(p->value1);
			std::memcpy(&type_v1[5], &d1, sizeof(double));
		}
		{
			float f2 = static_cast<float>(p->value2);
			std::memcpy(&type_v2[4], &f2, sizeof(float));
			double d2 = static_cast<double>(p->value2);
			std::memcpy(&type_v2[5], &d2, sizeof(double));
		}
		// 近似值容差基于已正确转换的浮点值
		if (request.contain_approximate_value && (request.next_type == next_scan_type::equal || request.next_type == next_scan_type::not_equal)) {
			// Float32 (index 4)
			{
				float target; std::memcpy(&target, &type_v1[4], sizeof(float));
				float lo = target * 0.95f, hi = target * 1.05f;
				if (target >= 0 && lo < -0.0001f) lo = 0.0f;
				if (hi - lo < 0.0001f) { lo = target - 0.0001f; hi = target + 0.0001f; }
				std::memcpy(&type_v1[4], &lo, sizeof(float));
				std::memcpy(&type_v2[4], &hi, sizeof(float));
			}
			// Float64 (index 5)
			{
				double target; std::memcpy(&target, &type_v1[5], sizeof(double));
				double lo = target * 0.95, hi = target * 1.05;
				if (target >= 0 && lo < -0.0001) lo = 0.0;
				if (hi - lo < 0.0001) { lo = target - 0.0001; hi = target + 0.0001; }
				std::memcpy(&type_v1[5], &lo, sizeof(double));
				std::memcpy(&type_v2[5], &hi, sizeof(double));
			}
		}
	}

	std::vector<scan_result> survivors;
	survivors.reserve(old_batch.size());

	const size_t max_read = 8;
	uint8_t cur_buf[max_read];
	uint8_t old_buf[max_read];

	bool needs_old = need_old_value_for_next_scan(request.next_type);
	auto* src_snap = (request.next_type == next_scan_type::compare_to_first_scan) ? first_snap.get() : previous_snapshot.get();

	for (const auto& res : old_batch) {
		if (m_cancel.load()) break;
		uint64_t addr = res.address;

		if (!current_snapshot->read_data(addr, cur_buf, max_read)) continue;

		uint8_t* target_old_ptr = nullptr;
		if (needs_old) {
			if (src_snap && src_snap->read_data(addr, old_buf, max_read)) {
				target_old_ptr = old_buf;
			} else {
				continue;
			}
		}

		// ★ 使用 type_mask 全面匹配：基于旧的 type_mask 偏好 + 全面兜底
		uint16_t mask = 0;
		if (res.type_mask != 0) {
			// 优先尝试旧的 type_mask 中标记的类型
			for (int ti = 0; ti < k_all_num_types; ++ti) {
				if (!(res.type_mask & (1 << ti))) continue;
				const auto& invoker = k_all_type_invokers[ti];
				if (addr % invoker.alignment != 0) continue;
				if (invoker.size > max_read) continue;
				if (invoker.match_next(cur_buf, target_old_ptr, type_v1[ti], type_v2[ti], request)) {
					mask |= (1 << ti);
				}
			}
		}
		// ★ 如果没有旧的 type_mask 或旧类型都没匹配上，兜底全部重新匹配
		if (mask == 0) {
			mask = pick_all_matches<false>(cur_buf, target_old_ptr,
				addr, max_read,
				type_v1, type_v2, request);
		}

		if (mask != 0) {
			survivors.push_back({ addr, mask });
		}
	}

	if (!survivors.empty()) out_cache->push_back_batch(survivors);
	m_progress.fetch_add(static_cast<int>(old_batch.size()));
}


// =============================================================================
// task_full_scan_with_next_condition_all — All + unknown_initial 再次扫描
// =============================================================================
void scan_engine::task_full_scan_with_next_condition_all(const scan_request& request, memory_region region,
	std::shared_ptr<i_process_memory_snapshot> current_snapshot,
	std::shared_ptr<i_process_memory_snapshot> previous_snapshot,
	std::shared_ptr<adaptive_cache_pool<scan_result>> out_cache)
{
	if (m_cancel.load()) return;
	if (region.size == 0) { m_progress.fetch_add(1); return; }

	auto first_snap = m_process_snapshot_manager->get_first_process_memory_snapshot();
	auto* p = std::get_if<value_params>(&request.params);
	uint64_t type_v1[k_all_num_types] = {0};
	uint64_t type_v2[k_all_num_types] = {0};

	if (p) {
		// 整数类型(0~3)直接传递原始整数值
		for (int ti = 0; ti < 4; ++ti) {
			type_v1[ti] = p->value1;
			type_v2[ti] = p->value2;
		}
		// ★ 修复：浮点类型(4=Float32,5=Float64)将输入的整数值转换为正确的浮点数值
		{
			float f1 = static_cast<float>(p->value1);
			std::memcpy(&type_v1[4], &f1, sizeof(float));
			double d1 = static_cast<double>(p->value1);
			std::memcpy(&type_v1[5], &d1, sizeof(double));
		}
		{
			float f2 = static_cast<float>(p->value2);
			std::memcpy(&type_v2[4], &f2, sizeof(float));
			double d2 = static_cast<double>(p->value2);
			std::memcpy(&type_v2[5], &d2, sizeof(double));
		}
		// 近似值容差基于已正确转换的浮点值
		if (request.contain_approximate_value && (request.next_type == next_scan_type::equal || request.next_type == next_scan_type::not_equal)) {
			{
				float target; std::memcpy(&target, &type_v1[4], sizeof(float));
				float lo = target * 0.95f, hi = target * 1.05f;
				if (target >= 0 && lo < -0.0001f) lo = 0.0f;
				if (hi - lo < 0.0001f) { lo = target - 0.0001f; hi = target + 0.0001f; }
				std::memcpy(&type_v1[4], &lo, sizeof(float));
				std::memcpy(&type_v2[4], &hi, sizeof(float));
			}
			{
				double target; std::memcpy(&target, &type_v1[5], sizeof(double));
				double lo = target * 0.95, hi = target * 1.05;
				if (target >= 0 && lo < -0.0001) lo = 0.0;
				if (hi - lo < 0.0001) { lo = target - 0.0001; hi = target + 0.0001; }
				std::memcpy(&type_v1[5], &lo, sizeof(double));
				std::memcpy(&type_v2[5], &hi, sizeof(double));
			}
		}
	}

	const size_t max_read = 8;
	const size_t chunk_size = 128 * 1024;
	std::vector<uint8_t> cur_buf(chunk_size + max_read);
	std::vector<uint8_t> prev_buf(chunk_size + max_read);
	std::vector<scan_result> batch_results;
	batch_results.reserve(4096);

	bool needs_prev_buf = need_old_value_for_next_scan(request.next_type);
	auto* src_snap = (request.next_type == next_scan_type::compare_to_first_scan) ? first_snap.get() : previous_snapshot.get();

	// ★ SIMD 快速路径判定：Changed / Unchanged + alignment==1 + 无近似值
	const bool use_simd_changed = can_use_simd_all_changed_unchanged(request);

	for (size_t base_offset = 0; base_offset < region.size && !m_cancel.load(); base_offset += chunk_size) {
		size_t to_read = (std::min)(chunk_size, region.size - base_offset);
		uint64_t chunk_base = region.base + base_offset;
		if (!current_snapshot->read_data(chunk_base, cur_buf.data(), to_read)) continue;

		if (needs_prev_buf) {
			if (!src_snap || !src_snap->read_data(chunk_base, prev_buf.data(), to_read)) continue;
		}

		if (use_simd_changed) {
			// ── SIMD 快速路径：Changed/Unchanged，一次 32 字节，6 种类型同时判定。
			// 内核直接追加 scan_result，省去 pair 中间缓冲与逐条拷贝。
			size_t simd_bytes = (to_read / 32) * 32;
			if (simd_bytes > 0) {
				simd_scanner::scan_all_types_changed_unchanged(
					cur_buf.data(), prev_buf.data(),
					simd_bytes, chunk_base,
					(request.next_type == next_scan_type::unchanged),
					batch_results);
				if (batch_results.size() >= 4096) {
					out_cache->push_back_batch(batch_results);
					batch_results.clear();
				}
			}
			// 尾部 < 32 字节：标量兜底
			for (size_t off = simd_bytes; off + 1 <= to_read && !m_cancel.load(); off += 1) {
				uint64_t addr = chunk_base + off;
				size_t max_avail = to_read - off;
				const uint8_t* target_old_ptr = needs_prev_buf ? (prev_buf.data() + off) : nullptr;
				uint16_t mask = pick_all_matches<false>(
					cur_buf.data() + off, target_old_ptr,
					addr, max_avail,
					type_v1, type_v2, request);
				if (mask != 0) {
					batch_results.push_back({ addr, mask });
					if (batch_results.size() >= 4096) {
						out_cache->push_back_batch(batch_results);
						batch_results.clear();
					}
				}
			}
		} else {
			// ── 标量路径：使用 pick_all_matches 获取完整 type_mask ──
			for (size_t off = 0; off + 1 <= to_read && !m_cancel.load(); off += 1) {
				uint64_t addr = chunk_base + off;
				size_t max_avail = to_read - off;
				const uint8_t* target_old_ptr = needs_prev_buf ? (prev_buf.data() + off) : nullptr;

				uint16_t mask = pick_all_matches<false>(
					cur_buf.data() + off, target_old_ptr,
					addr, max_avail,
					type_v1, type_v2, request);

				if (mask != 0) {
					batch_results.push_back({ addr, mask });
					if (batch_results.size() >= 4096) {
						out_cache->push_back_batch(batch_results);
						batch_results.clear();
					}
				}
			}
		}
	}

	if (!batch_results.empty()) out_cache->push_back_batch(batch_results);
	m_progress.fetch_add(1);
}


template <typename T>
void scan_engine::dispatch_scan(const scan_request& request, const std::vector<scan_result>& prev_results,
	std::shared_ptr<adaptive_cache_pool<scan_result>> out_cache)
{
	auto regions = process_manager::instance().get_memory_regions(request);

	// ★ 只有"未知初始值"路径才需要整份内存快照：
	//   - 普通首次扫描直接边读边匹配（CE 的 firstScan 就是这么干的，读 512KB 块，
	//     命中即入结果区，中间不落整份内存文件）；
	//   - unknown_initial 首扫没有结果集，下一轮要做全内存比较，才需要保留全量快照；
	//   - unknown_initial 之后的"全内存再扫描"同样需要当前全量快照。
	const bool need_full_snapshot =
		(request.mode == scan_mode::first && request.first_type == scan_type::unknown_initial) ||
		(request.mode == scan_mode::next && prev_results.empty() && m_potential_address.load() > 0);

	// ★ 进度条：把"读目标进程内存、建快照"这一段也算进总进度。
	//   旧实现只在快照建完之后才设置 m_total_items，导致点击扫描后进度条
	//   长时间停在 0%（大目标进程尤其明显），看起来像卡死。
	const int snap_units = need_full_snapshot ? snapshot_units_for(regions) : 0;
	const int scan_items = (request.mode == scan_mode::first)
		? static_cast<int>(regions.size())
		: ((prev_results.empty() && m_potential_address.load() > 0)
			? static_cast<int>(regions.size())
			: static_cast<int>(prev_results.size()));
	m_total_items.store(snap_units + scan_items);

	std::shared_ptr<i_process_memory_snapshot> current_snap;
	if (need_full_snapshot)
		current_snap = m_process_snapshot_manager->create_snapshot(regions,
			[this](int units) { m_progress.fetch_add(units, std::memory_order_relaxed); });
	else
		current_snap = m_process_snapshot_manager->create_live_snapshot();
	auto prev_snap = m_process_snapshot_manager->get_previous_process_memory_snapshot();


	// 用于等待所有线程完成的期值列表
	std::vector<std::future<void>> futures;

	if (request.mode == scan_mode::first) {
		// ── unknown_initial 首次扫描：只计数，不存地址 ──
		if (request.first_type == scan_type::unknown_initial) {
			m_potential_address.store(0);
			for (const auto& memory_region_section : regions) {
				// 直接在此计数，无需提交到 out_cache，避免海量地址占用内存
				size_t count = memory_region_section.size / request.alignment;
				m_potential_address.fetch_add(static_cast<int>(count), std::memory_order_relaxed);
				m_progress.fetch_add(1);
			}
			m_process_snapshot_manager->set_first_snapshot(current_snap);
			m_process_snapshot_manager->set_previous_snapshot(current_snap);
		} else {
			for (const auto& memory_region_section : regions) {
				futures.push_back(global_thread_pool::instance().enqueue([this, request, memory_region_section, current_snap, out_cache] {
					task_first_scan<T>(request, memory_region_section, current_snap, out_cache);
					}));
			}
			// ★ 首次扫描（非 unknown_initial）走实时读，first 快照在扫描结束后
			//   用结果地址构造（见下方尾部），这里不再把 live 快照当 first 存。
		}
	}
	else {
		// ── unknown_initial 之后的再次扫描：全内存遍历 + next-scan 条件 ──
		if (prev_results.empty() && m_potential_address.load() > 0) {
			m_potential_address.store(0); // 重置，后续再次扫描走常规逻辑
			for (const auto& memory_region_section : regions) {
				futures.push_back(global_thread_pool::instance().enqueue(
					[this, request, memory_region_section, current_snap, prev_snap, out_cache] {
						task_full_scan_with_next_condition<T>(request, memory_region_section, current_snap, prev_snap, out_cache);
					}));
			}
		} else {
			const size_t batch_size = 4096;
			for (size_t i = 0; i < prev_results.size(); i += batch_size) {
				std::vector<scan_result> batch;
				size_t end = (std::min)(i + batch_size, prev_results.size());
				batch.assign(prev_results.begin() + i, prev_results.begin() + end);
				futures.push_back(global_thread_pool::instance().enqueue(
					[this, request, batch, current_snap, prev_snap, out_cache] {
						task_next_scan<T>(request, batch, current_snap, prev_snap, out_cache);
					}));
			}
		}
	}



	for (auto& fut : futures) {
		if (fut.valid()) fut.get();
	}

	if (need_full_snapshot) {
		m_process_snapshot_manager->set_previous_snapshot(current_snap);
	} else {
		// 实时读模式：把存活地址上的当前值固化成稀疏快照，作为下一轮的"上一次值"。
		// 100 万结果也只占 ~16MB 内存，比再来一份几百 MB 的全量快照便宜得多。
		auto sparse = m_process_snapshot_manager->create_sparse_snapshot(
			collect_result_addresses(out_cache), sizeof(T), current_snap);
		m_process_snapshot_manager->set_previous_snapshot(sparse);
		// 首次扫描还需提供 first 快照，供"与首次扫描比较"（compare_to_first_scan）用
		if (request.mode == scan_mode::first)
			m_process_snapshot_manager->set_first_snapshot(sparse);
	}
}

template <typename T>
void scan_engine::task_first_scan(const scan_request& request, memory_region region,
	std::shared_ptr<i_process_memory_snapshot> current_snap,
	std::shared_ptr<adaptive_cache_pool<scan_result>> out_cache)
{
	if (m_cancel.load()) return;

	// ── 字符串 / 字节数组 首次扫描（仅 T=uint8_t 时编译，独立 Chunk 循环）──
	if constexpr (sizeof(T) == 1) {
		if (is_string_type(request.data_type) || is_byte_array_type(request.data_type)) {
			if (region.size == 0) { m_progress.fetch_add(1); return; }
			// 计算最大模式长度，用于 chunk 间的重叠
			size_t max_pattern_len = 0;
			if (is_string_type(request.data_type)) {
				if (auto* sp = std::get_if<string_params>(&request.params)) {
					// sp->text.length() 对于 utf16_string 已经是 UTF-16 LE 原始字节长度
					// 对于 Ascii/Utf8 也是字节长度，所以不需要额外乘 2
					max_pattern_len = sp->text.length();
				}
			} else {
				if (auto* ap = std::get_if<aob_params>(&request.params))
					max_pattern_len = ap->pattern.size();
			}
			if (max_pattern_len == 0) { m_progress.fetch_add(1); return; }
			const size_t chunk_size = 64 * 1024;
			const size_t overlap = max_pattern_len; // 确保跨越边界的模式不会漏掉
			std::vector<uint8_t> mem_buf(chunk_size + overlap);
			std::vector<scan_result> batch_results; batch_results.reserve(2048);
			bool first_chunk = true;
			for (size_t off = 0; off < region.size && !m_cancel.load(); off += chunk_size) {
				size_t to_read = (std::min)(chunk_size + (first_chunk ? 0 : overlap), region.size - off);
				// 从内存快照 snapshot 中读取数据，而非直接读进程内存
				if (!current_snap->read_data(region.base + off, mem_buf.data(), to_read)) continue;
				std::vector<uint64_t> hits;
				// 非首 chunk 时，只提交 chunk_size 之后的新数据（避免重复匹配 overlap 区域）
				if (is_string_type(request.data_type)) {
					if (auto* sp = std::get_if<string_params>(&request.params)) {
						std::vector<uint8_t> search_view(mem_buf.begin(), mem_buf.begin() + to_read);
						perform_string_search(search_view, region.base + off, *sp, request.data_type, hits);
						// 去重：滤掉 overlap 区域中的匹配（只保留 chunk_size 之后的）
						hits.erase(std::remove_if(hits.begin(), hits.end(), [&](uint64_t addr) {
							return !first_chunk && addr < region.base + off + overlap;
						}), hits.end());
					}
				} else {
					if (auto* ap = std::get_if<aob_params>(&request.params)) {
						perform_aob_search(mem_buf, region.base + off, *ap, hits);
						hits.erase(std::remove_if(hits.begin(), hits.end(), [&](uint64_t addr) {
							return !first_chunk && addr < region.base + off + overlap;
						}), hits.end());
					}
				}
				for (auto addr : hits) {
					batch_results.push_back({ addr });
					if (batch_results.size() >= 1024) { out_cache->push_back_batch(batch_results); batch_results.clear(); }
				}
				first_chunk = false;
			}
			if (!batch_results.empty()) out_cache->push_back_batch(batch_results);
			m_progress.fetch_add(1);
			return;
		}
	}

	// ── 数值类型首次扫描：使用 SIMD 从 snapshot 中读取并匹配 ──
	if (region.size < sizeof(T)) { m_progress.fetch_add(1); return; }

	const size_t step = request.alignment;
	const size_t chunk_size = 64 * 1024; // 64KB 分块
	std::vector<uint8_t> mem_buf(chunk_size + sizeof(T));
	std::vector<uint8_t> target_buf(chunk_size + sizeof(T)); // 用于 SIMD 比较的目标填充块
	std::vector<scan_result> batch_results;
	batch_results.reserve(2048);

	// 目标值构造（位还原/近似区间逻辑在 scan_value_target.h，便于单测覆盖）
	T v1 = 0, v2 = 0;
	const bool is_float_approx = build_first_scan_targets<T>(request, v1, v2);

	// 填充 target_buf（浮点数含近似值 exact_value 走标量区间，不需要 SIMD 目标块）
	const bool need_target_buf = !is_float_approx
		&& request.first_type != scan_type::unknown_initial
		&& request.first_type != scan_type::between;
	if (need_target_buf) {
		for (size_t i = 0; i < target_buf.size(); i += sizeof(T))
			std::memcpy(target_buf.data() + i, &v1, sizeof(T));
	}

	// 复用同一个结果向量：旧写法在每 64KB chunk 里新建 vector，
	// 640MB 目标就有 1 万次堆分配 + 反复扩容，纯浪费。
	std::vector<uint64_t> matched_addrs;
	matched_addrs.reserve(4096);

	for (size_t base_offset = 0; base_offset < region.size; base_offset += chunk_size) {
		if (m_cancel.load()) break;
		size_t to_read = std::min(chunk_size, region.size - base_offset);
		// 从内存快照 snapshot 中读取数据，而非直接读进程内存
		if (!current_snap->read_data(region.base + base_offset, mem_buf.data(), to_read)) continue;

		if (request.first_type == scan_type::unknown_initial) {
			// 未知初始值：记录该区域内所有对齐地址
			for (size_t off = 0; off + sizeof(T) <= to_read; off += step) {
				batch_results.push_back({ region.base + base_offset + off });
				if (batch_results.size() >= 1024) { out_cache->push_back_batch(batch_results); batch_results.clear(); }
			}
		}
		else if (!is_float_approx && (request.first_type == scan_type::exact_value) && !request.not_match) {
			// ── 精确值匹配 SIMD ──
			matched_addrs.clear();
			simd_scanner::scan_memory_block_for_matches<T>(mem_buf.data(), target_buf.data(), to_read,
				region.base + base_offset, step, SimdOp::equal, matched_addrs);
			for (auto addr : matched_addrs) {
				batch_results.push_back({ addr });
				if (batch_results.size() >= 1024) { out_cache->push_back_batch(batch_results); batch_results.clear(); }
			}
		}
		else if (!is_float_approx && (request.first_type == scan_type::exact_value) && request.not_match) {
			// ── 勾选了"非" + 精确值 → SIMD not_equal ──
			matched_addrs.clear();
			simd_scanner::scan_memory_block_for_matches<T>(mem_buf.data(), target_buf.data(), to_read,
				region.base + base_offset, step, SimdOp::not_equal, matched_addrs);
			for (auto addr : matched_addrs) {
				batch_results.push_back({ addr });
				if (batch_results.size() >= 1024) { out_cache->push_back_batch(batch_results); batch_results.clear(); }
			}
		}
		else if (!is_float_approx && !request.not_match && (request.first_type == scan_type::greater_than || request.first_type == scan_type::less_than)) {
			// 使用 SIMD 加速
			SimdOp op = (request.first_type == scan_type::greater_than) ? SimdOp::greater : SimdOp::less;
			matched_addrs.clear();
			simd_scanner::scan_memory_block_for_matches<T>(mem_buf.data(), target_buf.data(), to_read,
				region.base + base_offset, step, op, matched_addrs);
			for (auto addr : matched_addrs) {
				batch_results.push_back({ addr });
				if (batch_results.size() >= 1024) { out_cache->push_back_batch(batch_results); batch_results.clear(); }
			}
		}
		else if (request.first_type == scan_type::between && !request.not_match) {
			// Between 类型：使用 SIMD 范围扫描加速
			matched_addrs.clear();
			simd_scanner::scan_memory_block_for_range<T>(mem_buf.data(), to_read,
				region.base + base_offset, step, v1, v2, matched_addrs);
			for (auto addr : matched_addrs) {
				batch_results.push_back({ addr });
				if (batch_results.size() >= 1024) { out_cache->push_back_batch(batch_results); batch_results.clear(); }
			}
		}
		else {
			// ── 标量回退：浮点数近似值 exact_value / not_match+greater_than/less_than/Between ──
			for (size_t off = 0; off + sizeof(T) <= to_read; off += step) {
				T cur_val;
				std::memcpy(&cur_val, mem_buf.data() + off, sizeof(T));
				bool match = false;
				if (request.not_match) {
					// 非模式：反转条件
					switch (request.first_type) {
					case scan_type::exact_value: // 浮点近似值取反
						match = (cur_val < v1 || cur_val > v2);
						break;
					case scan_type::greater_than:
						match = (cur_val <= v1);
						break;
					case scan_type::less_than:
						match = (cur_val >= v1);
						break;
					case scan_type::between:
						match = (cur_val < v1 || cur_val > v2);
						break;
					default:
						match = false;
						break;
					}
				} else {
					// 正常模式
					switch (request.first_type) {
					case scan_type::exact_value: // 浮点近似值在 [v1,v2] 内
					case scan_type::greater_than:
					case scan_type::less_than:
						match = (cur_val >= v1 && cur_val <= v2);
						break;
					case scan_type::between:
						match = (cur_val >= v1 && cur_val <= v2);
						break;
					default:
						match = false;
						break;
					}
				}
				if (match) {
					batch_results.push_back({ region.base + base_offset + off });
					if (batch_results.size() >= 1024) { out_cache->push_back_batch(batch_results); batch_results.clear(); }
				}
			}
		}
	}

	if (!batch_results.empty()) out_cache->push_back_batch(batch_results);
	m_progress.fetch_add(1);
}

template <typename T>
void scan_engine::task_next_scan(const scan_request& request,
	const std::vector<scan_result>& old_batch,
	std::shared_ptr<i_process_memory_snapshot> current_snapshot,
	std::shared_ptr<i_process_memory_snapshot> previous_snapshot,
	std::shared_ptr<adaptive_cache_pool<scan_result>> out_cache)
{
	std::vector<scan_result> survivors;
	survivors.reserve(old_batch.size());
	auto first_snap = m_process_snapshot_manager->get_first_process_memory_snapshot();

	auto* p = std::get_if<value_params>(&request.params);
	T v1 = 0, v2 = 0;
	bool float_approx_range = false;   // 浮点近似相等/不等时用 [v1,v2] 区间判定
	if (p) {
		if constexpr (std::is_floating_point_v<T>) {
			T target;
			std::memcpy(&target, &p->value1, sizeof(T));
			const bool use_approx = request.contain_approximate_value
				&& (request.next_type == next_scan_type::equal || request.next_type == next_scan_type::not_equal);
			if (use_approx) {
                // ±5% 相对容差
				float_approx_range = true;
				constexpr T relative_epsilon = static_cast<T>(0.05); // ±5%
				T lo = target * (static_cast<T>(1.0) - relative_epsilon);
				T hi = target * (static_cast<T>(1.0) + relative_epsilon);
				T abs_min = static_cast<T>(0.0001);
				if (target >= static_cast<T>(0)) {
					if (lo < -abs_min) lo = static_cast<T>(0);
				}
				if (hi - lo < abs_min) { lo = target - abs_min; hi = target + abs_min; }
				std::memcpy(&v1, &lo, sizeof(T));
				std::memcpy(&v2, &hi, sizeof(T));
			} else {
				std::memcpy(&v1, &target, sizeof(T));
				std::memcpy(&v2, &p->value2, sizeof(T));
			}
		} else {
			std::memcpy(&v1, &p->value1, sizeof(T));
			std::memcpy(&v2, &p->value2, sizeof(T));
		}
	}

	// ★★★ 性能优化（模糊搜索 CPU 利用率 <50% 的主因）：
	// 旧实现对每个结果地址都做一次 ReadProcessMemory（live_snapshot->read_data 逐个
	// 系统调用）+ 一次快照 read_value，数百万地址 = 数百万次 syscall，CPU 大量时间
	// 阻塞在系统调用/内存延迟上，利用率上不去。
	// 修复：先把结果地址按升序排序，把地址相近的结果聚成"连续窗口"，每窗口只做
	// 一次整段读取（当前 + 上一次各一次），再从缓存的字节块里直接取各元素比较。
	// 典型 unknown_initial 场景下 1 次 RPC 覆盖 ~2048 个地址，syscall 数量骤减，
	// 计算负载回到 SIMD/标量主循环，CPU 利用率贴近精确扫描。
	const bool needs_old = need_old_value_for_next_scan(request.next_type);
	auto* src_snap = (request.next_type == next_scan_type::compare_to_first_scan)
		? first_snap.get() : previous_snapshot.get();
	const size_t max_read = sizeof(T);

	// ★★★ SIMD 快速路径（此前模糊 next-scan 逐地址标量 memcpy+switch，CPU 上不去）：
	//   仅整数数值、非浮点近似、且为"两缓冲比较 / 差值比较"类条件时启用。
	//   窗口整段 SIMD 得到候选地址，再与结果集求交（结果集地址正是窗口内子集）。
	SimdOp simd_op = SimdOp::equal;
	bool simd_path_diff = false;      // 走 compare_two_memory_blocks_diff_eq
	bool simd_diff_decrement = false;
	bool use_simd = false;
	{
		const bool numeric = !std::is_floating_point_v<T> &&
			!(sizeof(T) == 1 && (is_string_type(request.data_type) || is_byte_array_type(request.data_type)));
		if (numeric && !float_approx_range) {
			switch (request.next_type) {
			case next_scan_type::increased:           simd_op = SimdOp::greater;     simd_path_diff = false; use_simd = true; break;
			case next_scan_type::decreased:           simd_op = SimdOp::less;        simd_path_diff = false; use_simd = true; break;
			case next_scan_type::changed:             simd_op = SimdOp::not_equal;   simd_path_diff = false; use_simd = true; break;
			case next_scan_type::unchanged:           simd_op = SimdOp::equal;       simd_path_diff = false; use_simd = true; break;
			case next_scan_type::compare_to_first_scan: simd_op = SimdOp::equal;     simd_path_diff = false; use_simd = true; break;
			case next_scan_type::increased_by:        simd_path_diff = true;  simd_diff_decrement = false; use_simd = !request.not_match; break;
			case next_scan_type::decreased_by:        simd_path_diff = true;  simd_diff_decrement = true;  use_simd = !request.not_match; break;
			default: use_simd = false; break;
			}
			if (use_simd && !simd_path_diff && request.not_match)
				simd_op = invert_simd_op(simd_op);
		}
	}

	// 排序索引（不移动原 batch，避免破坏外部内存布局假设）
	std::vector<size_t> ord(old_batch.size());
	for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;
	std::sort(ord.begin(), ord.end(), [&](size_t a, size_t b) {
		return old_batch[a].address < old_batch[b].address;
	});

	// 地址 → 原结果 映射（SIMD 候选归约用）
	std::unordered_map<uint64_t, scan_result> res_by_addr;
	res_by_addr.reserve(old_batch.size() * 2);
	for (const auto& r : old_batch) res_by_addr.emplace(r.address, r);

	// 单地址求值（窗口读与逐地址回退共用）
	auto eval = [&](uint64_t addr, const uint8_t* curp, const uint8_t* oldp) -> bool {
		// ── 字符串 / 字节数组：保持逐地址读（模式长度不固定，不适合窗口）──
		if constexpr (sizeof(T) == 1) {
			if (is_string_type(request.data_type)) {
				if (auto* sp = std::get_if<string_params>(&request.params)) {
					if (sp && !sp->text.empty()) {
						size_t len = sp->text.length();
						std::vector<uint8_t> buf(len);
						if (current_snapshot->read_data(addr, buf.data(), len)) {
							std::vector<uint64_t> matched;
							perform_string_search(buf, addr, *sp, request.data_type, matched);
							return !matched.empty();
						}
					}
				}
				return false;
			}
			if (is_byte_array_type(request.data_type)) {
				if (auto* ap = std::get_if<aob_params>(&request.params)) {
					if (ap && !ap->pattern.empty()) {
						std::vector<uint8_t> buf(ap->pattern.size());
						if (current_snapshot->read_data(addr, buf.data(), buf.size())) {
							std::vector<uint64_t> matched;
							perform_aob_search(buf, addr, *ap, matched);
							return !matched.empty();
						}
					}
				}
				return false;
			}
		}

		T cur_val; std::memcpy(&cur_val, curp, sizeof(T));
		T old_val = T(0);
		if (needs_old && oldp) std::memcpy(&old_val, oldp, sizeof(T));
		const bool have_old = needs_old && oldp != nullptr;

		bool match = false;
		switch (request.next_type) {
		case next_scan_type::equal:    match = float_approx_range ? (cur_val >= v1 && cur_val <= v2) : (cur_val == v1); break;
		case next_scan_type::not_equal: match = float_approx_range ? (cur_val < v1 || cur_val > v2) : (cur_val != v1); break;
		case next_scan_type::greater_than: match = (cur_val > v1); break;
		case next_scan_type::less_than:    match = (cur_val < v1); break;
		case next_scan_type::increased:  match = have_old && (cur_val >  old_val); break;
		case next_scan_type::decreased:  match = have_old && (cur_val <  old_val); break;
		case next_scan_type::changed:    match = have_old && (cur_val != old_val); break;
		case next_scan_type::unchanged:  match = have_old && (cur_val == old_val); break;
		case next_scan_type::between:    match = (cur_val >= v1 && cur_val <= v2); break;
		// ★ CE 语义：增加了多少 = 精确等于 旧值+增量；减少了多少 = 精确等于 旧值-增量
		case next_scan_type::increased_by: match = have_old && (cur_val == old_val + v1); break;
		case next_scan_type::decreased_by: match = have_old && (cur_val == old_val - v1); break;
		case next_scan_type::ignore_value: match = true; break;
		case next_scan_type::compare_to_first_scan: match = have_old && (cur_val == old_val); break;
		default: match = false; break;
		}
		if (request.not_match) match = !match;
		return match;
	};

	// 窗口缓冲（覆盖窗口跨度 + 最大元素尺寸，保证 off+sizeof(T) 不越界）
	constexpr size_t k_win = 8192;
	std::vector<uint8_t> cur_buf(k_win + sizeof(T));
	std::vector<uint8_t> old_buf(k_win + sizeof(T));

	size_t i = 0;
	while (i < ord.size()) {
		if (m_cancel.load()) break;
		const uint64_t base = old_batch[ord[i]].address;

		// 收集窗口内的地址（base 起 k_win 字节跨度的连续地址）
		size_t j = i;
		while (j < ord.size() && (old_batch[ord[j]].address - base) < k_win) ++j;
		const size_t win_len = static_cast<size_t>(old_batch[ord[j - 1]].address - base) + sizeof(T);

		const bool cur_ok = current_snapshot->read_data(base, cur_buf.data(), win_len);
		const bool old_ok = (!needs_old) || (src_snap && src_snap->read_data(base, old_buf.data(), win_len));

		if (use_simd && cur_ok && old_ok) {
			// ★ SIMD 整窗比较：一次处理 32 字节，候选地址与结果集求交
			std::vector<uint64_t> matched;
			if (simd_path_diff) {
				simd_scanner::compare_two_memory_blocks_diff_eq<T>(
					cur_buf.data(), old_buf.data(), win_len, base, request.alignment,
					v1, simd_diff_decrement, matched);
			} else {
				simd_scanner::compare_two_memory_blocks<T>(
					cur_buf.data(), old_buf.data(), win_len, base, request.alignment,
					simd_op, matched);
			}
			for (uint64_t cand : matched) {
				auto it = res_by_addr.find(cand);
				if (it != res_by_addr.end()) {
					survivors.push_back(it->second);
					if (survivors.size() >= 4096) { out_cache->push_back_batch(survivors); survivors.clear(); }
				}
			}
		} else {
			for (size_t k = i; k < j; ++k) {
				const scan_result& res = old_batch[ord[k]];
				const size_t off = static_cast<size_t>(res.address - base);
				if (off + sizeof(T) > win_len) continue;

				bool hit;
				if (cur_ok && (old_ok || !needs_old)) {
					hit = eval(res.address, cur_buf.data() + off,
						needs_old ? (old_buf.data() + off) : nullptr);
				} else {
					// 整窗口读失败（跨越不可读页等）→ 该地址单独回退
					uint8_t c[8] = {}, o[8] = {};
					if (!current_snapshot->read_data(res.address, c, max_read)) continue;
					const uint8_t* op = nullptr;
					if (needs_old) {
						if (!src_snap || !src_snap->read_data(res.address, o, max_read)) continue;
						op = o;
					}
					hit = eval(res.address, c, op);
				}
				if (hit) {
					survivors.push_back(res);
					if (survivors.size() >= 4096) { out_cache->push_back_batch(survivors); survivors.clear(); }
				}
			}
		}
		i = j;
	}
	if (!survivors.empty()) out_cache->push_back_batch(survivors);
	m_progress.fetch_add(static_cast<int>(old_batch.size()));
}


template <typename T>
void scan_engine::task_full_scan_with_next_condition(const scan_request& request, memory_region region,
	std::shared_ptr<i_process_memory_snapshot> current_snapshot,
	std::shared_ptr<i_process_memory_snapshot> previous_snapshot,
	std::shared_ptr<adaptive_cache_pool<scan_result>> out_cache)
{
	if (m_cancel.load()) return;

	const size_t step = request.alignment;
	const size_t scalar_size = sizeof(T);
	const size_t chunk_size = 128 * 1024; // 128KB 分块（更大块更有利于 SIMD）
	std::vector<uint8_t> mem_buf(chunk_size + scalar_size);
	std::vector<uint8_t> prev_buf(chunk_size + scalar_size); // 存放上一次/首次快照的批量数据
	std::vector<scan_result> batch_results;
	batch_results.reserve(4096);

	auto first_snap = m_process_snapshot_manager->get_first_process_memory_snapshot();

	// ===== 解析参数 v1/v2 =====
	auto* p = std::get_if<value_params>(&request.params);
	T v1 = 0, v2 = 0;
	bool use_range_for_equal_not_equal = false; // 浮点数近似匹配时用范围比较
	if (p) {
		if constexpr (std::is_floating_point_v<T>) {
			T target;
			std::memcpy(&target, &p->value1, sizeof(T));
			const bool use_approx = request.contain_approximate_value
				&& (request.next_type == next_scan_type::equal || request.next_type == next_scan_type::not_equal);
			if (use_approx) {
				use_range_for_equal_not_equal = true;
				constexpr T relative_epsilon = static_cast<T>(0.05);
				T lo = target * (static_cast<T>(1.0) - relative_epsilon);
				T hi = target * (static_cast<T>(1.0) + relative_epsilon);
				T abs_min = static_cast<T>(0.0001);
				if (target >= static_cast<T>(0)) {
					if (lo < -abs_min) lo = static_cast<T>(0);
				}
				if (hi - lo < abs_min) { lo = target - abs_min; hi = target + abs_min; }
				std::memcpy(&v1, &lo, sizeof(T));
				std::memcpy(&v2, &hi, sizeof(T));
			} else {
				std::memcpy(&v1, &target, sizeof(T));
				std::memcpy(&v2, &p->value2, sizeof(T));
			}
		} else {
			std::memcpy(&v1, &p->value1, sizeof(T));
			std::memcpy(&v2, &p->value2, sizeof(T));
		}
	}

	// ===== 判断走哪条快速路 =====
	enum class SimdPath {
			None,                  // 标量回退
			CompareWithTarget,     // Equal / not_equal → 用 simd_scanner::scan_memory_block_for_matches
			RangeFilter,           // Between / 浮点近似 Equal/not_equal → 用 scan_memory_block_for_range
			CompareTwoBuffers,     // Changed / Unchanged / Increased / Decreased / compare_to_first_scan
			                       // → 用 compare_two_memory_blocks
			DiffEq                 // increased_by / decreased_by → 用 compare_two_memory_blocks_diff_eq
		};
	SimdPath simd_path = SimdPath::None;
	bool needs_prev_buf = false;

	switch (request.next_type) {
	case next_scan_type::equal:
		if constexpr (std::is_floating_point_v<T>) {
			simd_path = use_range_for_equal_not_equal ? SimdPath::RangeFilter : SimdPath::CompareWithTarget;
		} else {
			simd_path = SimdPath::CompareWithTarget;
		}
		break;
	case next_scan_type::not_equal:
		if constexpr (std::is_floating_point_v<T>) {
			simd_path = use_range_for_equal_not_equal ? SimdPath::RangeFilter : SimdPath::CompareWithTarget;
		} else {
			simd_path = SimdPath::CompareWithTarget;
		}
		break;
	case next_scan_type::between:
		simd_path = SimdPath::RangeFilter;
		break;
	case next_scan_type::changed:   simd_path = SimdPath::CompareTwoBuffers; needs_prev_buf = true; break;
	case next_scan_type::unchanged: simd_path = SimdPath::CompareTwoBuffers; needs_prev_buf = true; break;
	case next_scan_type::increased: simd_path = SimdPath::CompareTwoBuffers; needs_prev_buf = true; break;
	case next_scan_type::decreased: simd_path = SimdPath::CompareTwoBuffers; needs_prev_buf = true; break;
	case next_scan_type::compare_to_first_scan: simd_path = SimdPath::CompareTwoBuffers; needs_prev_buf = true; break;
		// increased_by / decreased_by → 差值 SIMD（勾选"非"时取补逻辑复杂，回退标量）
		case next_scan_type::increased_by:
		case next_scan_type::decreased_by:
			if (!request.not_match) { simd_path = SimdPath::DiffEq; needs_prev_buf = true; }
			break;
	default: break;
	}

	// Pre-allocate target buffer for CompareWithTarget path
	std::vector<uint8_t> target_buf;
	if (simd_path == SimdPath::CompareWithTarget) {
		target_buf.resize(chunk_size + scalar_size);
	}

	for (size_t base_offset = 0; base_offset < region.size && !m_cancel.load(); base_offset += chunk_size) {
		size_t to_read = (std::min)(chunk_size, region.size - base_offset);
		uint64_t chunk_base = region.base + base_offset;

		// ★ 批量读取当前快照内存（vs 逐地址虚拟调用）
		if (!current_snapshot->read_data(chunk_base, mem_buf.data(), to_read)) continue;

		// ★ 批量读取上一次/首次快照（vs 逐地址虚拟调用 * 数十亿次）
		if (needs_prev_buf) {
			auto* src_snap = (request.next_type == next_scan_type::compare_to_first_scan) ? first_snap.get() : previous_snapshot.get();
			if (!src_snap || !src_snap->read_data(chunk_base, prev_buf.data(), to_read)) continue;
		}

		// ===== SIMD 快速路径 =====
		if (simd_path == SimdPath::CompareWithTarget) {
			// 填充目标值缓冲区
			T target_val = v1;
			for (size_t i = 0; i < to_read; i += scalar_size)
				std::memcpy(target_buf.data() + i, &target_val, scalar_size);

			SimdOp op = request.not_match ? invert_simd_op((request.next_type == next_scan_type::equal) ? SimdOp::equal : SimdOp::not_equal)
			                             : ((request.next_type == next_scan_type::equal) ? SimdOp::equal : SimdOp::not_equal);
			std::vector<uint64_t> matched;
			simd_scanner::scan_memory_block_for_matches<T>(
				mem_buf.data(), target_buf.data(), to_read, chunk_base, step, op, matched);

			for (auto addr : matched) {
				batch_results.push_back({ addr });
				if (batch_results.size() >= 4096) {
					out_cache->push_back_batch(batch_results);
					batch_results.clear();
				}
			}
		}
		else if (simd_path == SimdPath::RangeFilter) {
			// ★ 注意：浮点数 not_equal 近似模式需要的是"不在 [v1,v2] 内"
			//   但 SIMD 范围扫描只能找"在范围内"的，not_equal 需要取补
			//   所以我们先用 range 找匹配的，然后用 std::vector<uint64_t> 收集再反转...
			//   但那样性能反而更差。对于 not_equal 近似模式，仍然走标量回退。
			// ★ 如果勾选了"非"（not_match=true），需要取补：匹配不在 [v1,v2] 的值
			const bool inverted_range = request.not_match;
			if constexpr (std::is_floating_point_v<T>) {
				if (inverted_range || request.next_type == next_scan_type::not_equal) {
					// ★ 取补模式：标量回退（范围取补不好 SIMD）
					for (size_t off = 0; off + scalar_size <= to_read; off += step) {
						if (m_cancel.load()) break;
						T cur_val;
						std::memcpy(&cur_val, mem_buf.data() + off, sizeof(T));
						bool in_range = (cur_val >= v1 && cur_val <= v2);
						if (inverted_range ? !in_range : in_range) {
							batch_results.push_back({ chunk_base + off });
							if (batch_results.size() >= 4096) {
								out_cache->push_back_batch(batch_results);
								batch_results.clear();
							}
						}
					}
				} else {
					std::vector<uint64_t> matched;
					simd_scanner::scan_memory_block_for_range<T>(
						mem_buf.data(), to_read, chunk_base, step, v1, v2, matched);
					for (auto addr : matched) {
						batch_results.push_back({ addr });
						if (batch_results.size() >= 4096) {
							out_cache->push_back_batch(batch_results);
							batch_results.clear();
						}
					}
				}
			} else {
				if (inverted_range) {
					// ★ 整数取补模式：标量回退
					for (size_t off = 0; off + scalar_size <= to_read; off += step) {
						if (m_cancel.load()) break;
						T cur_val;
						std::memcpy(&cur_val, mem_buf.data() + off, sizeof(T));
						if (cur_val < v1 || cur_val > v2) {
							batch_results.push_back({ chunk_base + off });
							if (batch_results.size() >= 4096) {
								out_cache->push_back_batch(batch_results);
								batch_results.clear();
							}
						}
					}
				} else {
					std::vector<uint64_t> matched;
					simd_scanner::scan_memory_block_for_range<T>(
						mem_buf.data(), to_read, chunk_base, step, v1, v2, matched);
					for (auto addr : matched) {
						batch_results.push_back({ addr });
						if (batch_results.size() >= 4096) {
							out_cache->push_back_batch(batch_results);
							batch_results.clear();
						}
					}
				}
			}
		}
		else if (simd_path == SimdPath::CompareTwoBuffers) {
			// ★ SIMD 批量比较两个内存块（当前 vs 上次/首次快照）
				// ★ 根据 not_match 反转比较操作
				SimdOp base_op;
				switch (request.next_type) {
				case next_scan_type::changed:              base_op = SimdOp::not_equal; break;
				case next_scan_type::unchanged:            base_op = SimdOp::equal; break;
				case next_scan_type::increased:            base_op = SimdOp::greater; break;
				case next_scan_type::decreased:            base_op = SimdOp::less; break;
				case next_scan_type::compare_to_first_scan: base_op = SimdOp::equal; break;
				default:                                 base_op = SimdOp::equal; break;
				}
				SimdOp op = request.not_match ? invert_simd_op(base_op) : base_op;

			std::vector<uint64_t> matched;
			simd_scanner::compare_two_memory_blocks<T>(
				mem_buf.data(), prev_buf.data(), to_read, chunk_base, step, op, matched);

			for (auto addr : matched) {
					batch_results.push_back({ addr });
					if (batch_results.size() >= 4096) {
						out_cache->push_back_batch(batch_results);
						batch_results.clear();
					}
				}
			}
			else if (simd_path == SimdPath::DiffEq) {
				// ★ increased_by / decreased_by：cur == prev ± v1，SIMD 差值比较
				std::vector<uint64_t> matched;
				simd_scanner::compare_two_memory_blocks_diff_eq<T>(
					mem_buf.data(), prev_buf.data(),
					to_read, chunk_base, step,
					v1, (request.next_type == next_scan_type::decreased_by), matched);
				for (auto addr : matched) {
					batch_results.push_back({ addr });
					if (batch_results.size() >= 4096) {
						out_cache->push_back_batch(batch_results);
						batch_results.clear();
					}
				}
			}
			else {
				// ===== 标量回退（浮点 not_equal 近似 + 勾选"非"的 _by）=====
			// 虽然有虚拟调用开销，但 prev_buf 整块预读已消除逐地址调用
			T placeholder_prev;
			for (size_t off = 0; off + scalar_size <= to_read; off += step) {
				if (m_cancel.load()) break;
				uint64_t addr = chunk_base + off;
				T cur_val;
				std::memcpy(&cur_val, mem_buf.data() + off, sizeof(T));

					bool match = false;
					switch (request.next_type) {
					case next_scan_type::equal:
						if constexpr (std::is_floating_point_v<T>) {
							match = (cur_val >= v1 && cur_val <= v2);
						} else {
							match = (cur_val == v1);
						}
						break;
					case next_scan_type::not_equal:
						if constexpr (std::is_floating_point_v<T>) {
							match = (cur_val < v1 || cur_val > v2);
						} else {
							match = (cur_val != v1);
						}
						break;
					case next_scan_type::greater_than: match = (cur_val > v1); break;
					case next_scan_type::less_than:    match = (cur_val < v1); break;
					case next_scan_type::ignore_value: match = true; break;
					case next_scan_type::increased_by:
						std::memcpy(&placeholder_prev, prev_buf.data() + off, sizeof(T));
						match = (cur_val == placeholder_prev + v1);   // ★ CE：精确等于旧值+增量
						break;
					case next_scan_type::decreased_by:
						std::memcpy(&placeholder_prev, prev_buf.data() + off, sizeof(T));
						match = (cur_val == placeholder_prev - v1);   // ★ CE：精确等于旧值-增量
						break;
					default:
						// 安全回退
						match = false;
						break;
					}

				if (match) {
					batch_results.push_back({ addr });
					if (batch_results.size() >= 4096) {
						out_cache->push_back_batch(batch_results);
						batch_results.clear();
					}
				}
			}
		}
	}

	if (!batch_results.empty()) out_cache->push_back_batch(batch_results);
	m_progress.fetch_add(1);
}

void scan_engine::perform_aob_search(const std::vector<uint8_t>& buf, uint64_t base, const aob_params& p, std::vector<uint64_t>& matched) {

	if (p.pattern.empty() || buf.size() < p.pattern.size()) return;

    const size_t pat_len = p.pattern.size();

    // ── 全字节匹配无通配符：用 SIMD 找首字节候选 + memcmp 验证 ──
    const bool all_full_byte = std::all_of(p.mask.begin(), p.mask.end(),
        [](uint8_t m) { return m == 0xFF; });
    if (all_full_byte) {
        std::vector<size_t> candidates;
        simd_scanner::find_first_char(buf.data(), buf.size(), p.pattern[0], candidates);
        for (size_t offset : candidates) {
            if (offset + pat_len <= buf.size()) {
                if (std::memcmp(buf.data() + offset, p.pattern.data(), pat_len) == 0) {
                    matched.push_back(base + offset);
                }
            }
        }
        return;
    }

    // ── 有 nibble 级通配符：逐字节 nibble 比较 ──
    for (size_t i = 0; i <= buf.size() - pat_len; ++i) {
        bool match = true;
        for (size_t k = 0; k < pat_len; ++k) {
            const uint8_t m = p.mask[k];
            if (m == 0x00) continue;                         // "??" 完全通配，跳过
            const uint8_t cur = buf[i + k];
            const uint8_t pat = p.pattern[k];
            if (m == 0xFF) {                                 // "3E" 全字节匹配
                if (cur != pat) { match = false; break; }
            } else if (m == 0xF0) {                          // "3?" 仅高半字节
                if ((cur & 0xF0) != pat) { match = false; break; }
            } else if (m == 0x0F) {                          // "?E" 仅低半字节
                if ((cur & 0x0F) != pat) { match = false; break; }
            } else {
                // 未知掩码 — 安全回退：全字节比较
                if (cur != pat) { match = false; break; }
            }
        }
        if (match) matched.push_back(base + i);
    }
}


void scan_engine::perform_string_search(const std::vector<uint8_t>& buf, uint64_t base,
	const string_params& p, scan_data_type type,
	std::vector<uint64_t>& matched)
{
	if (p.text.empty() || buf.size() < p.text.length()) return;

	if (type == scan_data_type::ascii_string || type == scan_data_type::utf8_string) {
		const std::string& target = p.text;
		size_t t_len = target.length();
		if (buf.size() < t_len) return;

		// 性能优化：区分大小写时先用 SIMD 找首字节
		if (p.case_sensitive) {
			std::vector<size_t> candidates;
			simd_scanner::find_first_char(buf.data(), buf.size(), static_cast<uint8_t>(target[0]), candidates);

			for (size_t offset : candidates) {
				if (offset + t_len <= buf.size()) {
					if (std::memcmp(buf.data() + offset, target.data(), t_len) == 0) {
						matched.push_back(base + offset);
					}
				}
			}
		}
		else {
			// 不区分大小写：逐字节比较（仅对 ASCII 范围内的字母正确，
			// 对多字节 UTF-8 非 ASCII 字符会逐字节 tolower，能满足大部分场景）
			for (size_t i = 0; i <= buf.size() - t_len; ++i) {
				bool match = true;
				for (size_t k = 0; k < t_len; ++k) {
					if (compare_byte_insensitive(buf[i + k], static_cast<uint8_t>(target[k]))) {
						// 继续
					} else {
						match = false;
						break;
					}
				}
				if (match) matched.push_back(base + i);
			}
		}
	}
	else if (type == scan_data_type::utf16_string) {
		// p.text 来自 parse_string_params()，存的是 UTF-16 LE 原始字节
		// 直接将其作为 uint16_t 数组使用，无需 MultiByteToWideChar 二次转换
		size_t t_bytes = p.text.length();
		if (buf.size() < t_bytes || t_bytes < 2 || (t_bytes % 2) != 0) return;

		const uint16_t* target16 = reinterpret_cast<const uint16_t*>(p.text.data());
		size_t target_len = t_bytes / 2;

		// UTF-16 扫描按 2 字节对齐（小端序 LE）
		for (size_t i = 0; i <= buf.size() - t_bytes; i += 2) {
			const uint16_t* ptr = reinterpret_cast<const uint16_t*>(buf.data() + i);
			bool match = true;
			for (size_t k = 0; k < target_len; ++k) {
				if (p.case_sensitive) {
					if (ptr[k] != target16[k]) { match = false; break; }
				}
				else {
					if (!compare_utf16_insensitive(ptr[k], target16[k])) { match = false; break; }
				}
			}
			if (match) matched.push_back(base + i);
		}
	}
}


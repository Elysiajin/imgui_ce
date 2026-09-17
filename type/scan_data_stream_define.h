#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <variant>

enum class scan_mode { first, next };

enum class scan_type
{
    exact_value,
    greater_than,
    less_than,
    between,
    unknown_initial, //只有首次扫描才会有这个选项，表示不限制初始值，后续再根据用户输入的数值进行过滤
    string_scan
};

enum class next_scan_type
{
    equal,
    not_equal,
    greater_than,     // 再次扫描：大于固定值（CE Bigger than...）
    less_than,        // 再次扫描：小于固定值（CE Smaller than...）
    increased,
    decreased,
    changed,
    unchanged,
    between,
    increased_by,
    decreased_by,
    ends_with,
    ignore_value,     // 再次扫描：忽略值（CE soForgot，保留当前结果不做过滤）
    compare_to_first_scan
};

enum class scan_data_type : uint8_t {
    bit,
    int8,
    int16,
    int32,
    int64,
    float32,
    float64,
    ascii_string,
    utf8_string,
    utf16_string,
    byte_array,
    all,
    structure
};

inline size_t scan_data_type_size(scan_data_type t) {
    switch (t) {
    case scan_data_type::bit:     return 1;
    case scan_data_type::int8:    return 1;
    case scan_data_type::int16:   return 2;
    case scan_data_type::int32:   return 4;
    case scan_data_type::int64:   return 8;
    case scan_data_type::float32: return 4;
    case scan_data_type::float64: return 8;
    case scan_data_type::ascii_string: return 0;
    case scan_data_type::utf8_string:  return 0;
    case scan_data_type::utf16_string: return 0;
    case scan_data_type::byte_array:   return 0;
    case scan_data_type::all:         return 8;
    case scan_data_type::structure:   return 0;
    default:                        return 0;
    }
}

// 内存属性过滤结构体（用于限定扫描范围）
struct memory_filter {
    static constexpr uint32_t commit  = 0x1000;
    static constexpr uint32_t reserve = 0x2000;
    static constexpr uint32_t free    = 0x10000;

    static constexpr uint32_t type_private = 0x20000;
    static constexpr uint32_t type_image   = 0x1000000;
    static constexpr uint32_t type_mapped  = 0x40000;

    static constexpr uint32_t access_read    = 1;
    static constexpr uint32_t access_write   = 2;
    static constexpr uint32_t access_execute = 4;

    static constexpr uint32_t protect_no_access        = 0x0001;
    static constexpr uint32_t protect_read_only        = 0x0002;
    static constexpr uint32_t protect_read_write       = 0x0004;
    static constexpr uint32_t protect_write_copy       = 0x0008;
    static constexpr uint32_t protect_execute         = 0x0010;
    static constexpr uint32_t protect_execute_read     = 0x0020;
    static constexpr uint32_t protect_execute_read_write = 0x0040;
    static constexpr uint32_t protect_execute_write_copy = 0x0080;
    static constexpr uint32_t protect_guard           = 0x0100;

    uint32_t state_filter   = commit;
    uint32_t type_filter    = type_private | type_image | type_mapped;
    uint32_t access_filter  = access_read;
    uint32_t protect_filter = 0;

    bool writable          = false;
    bool executable        = false;
    bool copy_on_write       = false;
};

// 仅在 Between 时使用
struct value_params {
    uint64_t value1 = 0;
    uint64_t value2 = 0;
};

// 字符串参数
struct string_params {
    std::string text;
    bool case_sensitive = true;
};

// 字节数组 (AOB) 参数，mask[i] 为 nibble 级掩码
struct aob_params {
    std::vector<uint8_t> pattern;
    std::vector<uint8_t> mask;
};

// 结构体中的一个成员
struct structure_member {
    scan_data_type type;
    value_params criteria;
    size_t offset_from_prev;
};

// 结构体扫描参数
struct structure_params {
    std::vector<structure_member> members;

    size_t total_span() const {
        size_t span = 0;
        for (const auto& m : members) {
            span += m.offset_from_prev + scan_data_type_size(m.type);
        }
        return span;
    }
};

using scan_params = std::variant<value_params, string_params, aob_params, structure_params>;

namespace all_type_mask {
    enum : uint16_t {
        Int8    = 1 << 0,
        Int16   = 1 << 1,
        Int32   = 1 << 2,
        Int64   = 1 << 3,
        Float   = 1 << 4,
        Double  = 1 << 5
    };
}

#pragma pack(push, 1)
struct scan_result
{
    uint64_t address;
    uint16_t type_mask = 0;

    bool has_type(uint16_t mask) const { return (type_mask & mask) != 0; }
};
#pragma pack(pop)

// 扫描请求（唯一入口）
struct scan_request {
    scan_mode     mode = scan_mode::first;
    scan_data_type data_type = scan_data_type::int32;

    size_t       alignment = 1;
    scan_type     first_type = scan_type::exact_value;
    next_scan_type next_type  = next_scan_type::equal;

    uint64_t module_base = 0;
    uint64_t module_size = 0;

    scan_params  params;

    memory_filter mem_filter;

    bool percent_mode = false;
    bool contain_approximate_value = false;
    bool not_match = false;

    std::shared_ptr<const std::vector<scan_result>> prev_results = nullptr;
};

inline bool is_floating_point(scan_data_type t) {
    return t == scan_data_type::float32 || t == scan_data_type::float64;
}

inline bool is_string_type(scan_data_type t) {
    return t == scan_data_type::ascii_string || t == scan_data_type::utf16_string || t == scan_data_type::utf8_string;
}

inline bool is_byte_array_type(scan_data_type t) {
    return t == scan_data_type::byte_array;
}

inline std::string scan_data_type_to_string(scan_data_type t) {
    switch (t) {
    case scan_data_type::bit:         return "位";
    case scan_data_type::int8:        return "字节";
    case scan_data_type::int16:       return "2 字节";
    case scan_data_type::int32:       return "4 字节";
    case scan_data_type::int64:       return "8 字节";
    case scan_data_type::float32:     return "单精度浮点数";
    case scan_data_type::float64:     return "双精度浮点数";
    case scan_data_type::ascii_string: return "字符串 (ASCII)";
    case scan_data_type::utf8_string:  return "字符串 (UTF-8)";
    case scan_data_type::utf16_string: return "字符串 (UTF-16)";
    case scan_data_type::byte_array:   return "字节数组";
    case scan_data_type::all:         return "全部数据类型";
    case scan_data_type::structure:   return "结构体";
    default:                        return "未知";
    }
}

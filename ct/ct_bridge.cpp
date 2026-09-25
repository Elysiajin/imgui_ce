#include "ct/ct_bridge.h"

#include "ct/address_parser.h"
#include "ui/address_list_panel.h"

namespace ct_bridge {

namespace {

// --------------------------------------------------- ct → 扁平 ----

void entry_to_record(const ct_entry& e, address_record& r)
{
    r.description  = e.description;
    r.is_group     = e.is_group;
    r.type         = e.type;
    r.ct_raw_type  = e.raw_type;
    r.address      = e.address_text;
    r.script       = e.script;
    r.show_signed  = e.show_signed;
    r.show_signed_present = e.show_signed_present;
    r.unicode_string      = e.unicode_string;
    r.string_length       = e.string_length;
    r.codepage            = e.codepage;
    r.zero_terminate      = e.zero_terminate;
    r.custom_type_name    = e.custom_type_name;
    r.bit_start    = e.bit_start;
    r.bit_length   = e.bit_length;
    r.byte_length  = e.byte_length;
    r.has_color    = e.has_color;
    r.color        = e.color;
    r.show_hex     = e.show_hex;
    r.radix        = e.show_hex ? value_radix::hex : value_radix::decimal;
    r.expanded     = !e.collapsed_hint;

    for (const ct_offset& o : e.offsets)
        r.offsets.push_back(o.value);
}

void append_tree(const ct_entry& e, uint64_t parent_id, int depth,
                 address_list_panel& list,
                 const std::vector<module_info>& modules)
{
    address_record r;
    entry_to_record(e, r);
    r.parent_id = parent_id;
    r.depth     = depth;

    // 可解释地址 → 真实地址（解析失败保留原文，附加后重试）
    if (!e.address_text.empty()) {
        const parsed_address pa =
            parse_interpretable_address(e.address_text, modules);
        if (pa.ok)
            r.real_address = pa.address;
    }

    list.add_record(r);   // add_record 分配 id（先序插入保证树序）
    const uint64_t my_id = list.records().back().id;

    for (const ct_entry& c : e.children)
        append_tree(c, my_id, depth + 1, list, modules);
}

// --------------------------------------------------- 扁平 → ct ----

void record_to_entry(const address_record& r, ct_entry& e)
{
    e.description  = r.description;
    e.is_group     = r.is_group;
    e.type         = r.type;
    e.raw_type     = r.ct_raw_type;
    e.address_text = r.address;
    e.script       = r.script;
    e.show_signed  = r.show_signed;
    e.show_signed_present = r.show_signed_present;
    e.unicode_string      = r.unicode_string;
    e.string_length       = r.string_length;
    e.codepage            = r.codepage;
    e.zero_terminate      = r.zero_terminate;
    e.custom_type_name    = r.custom_type_name;
    e.bit_start    = r.bit_start;
    e.bit_length   = r.bit_length;
    e.byte_length  = r.byte_length;
    e.has_color    = r.has_color;
    e.color        = r.color;
    e.show_hex     = (r.radix == value_radix::hex);

    for (uint64_t o : r.offsets)
        e.offsets.push_back({o});
}

// 先序扁平数组按 depth 递归组树
void build_level(const std::vector<address_record>& records, size_t& i,
                 int depth, std::vector<ct_entry>& out)
{
    while (i < records.size() && records[i].depth == depth) {
        const address_record& r = records[i++];
        ct_entry e;
        record_to_entry(r, e);
        if (i < records.size() && records[i].depth > depth) {
            build_level(records, i, depth + 1, e.children);
            e.collapsed_hint = !r.expanded;   // 只有真正有子节点才写折叠
        }
        out.push_back(std::move(e));
    }
}

} // namespace

void apply_ct_table(const cheat_table& ct, address_list_panel& list,
                    const std::vector<module_info>& modules)
{
    list.clear();
    list.user_symbols.clear();
    for (const ct_user_symbol& s : ct.user_symbols)
        list.user_symbols.emplace_back(s.name, s.address_text);

    for (const ct_entry& e : ct.entries)
        append_tree(e, 0, 0, list, modules);
}

cheat_table build_ct_table(const address_list_panel& list)
{
    cheat_table ct;
    const auto& records = list.records();

    for (const auto& s : list.user_symbols) {
        ct_user_symbol us;
        us.name = s.first;
        us.address_text = s.second;
        ct.user_symbols.push_back(std::move(us));
    }

    size_t i = 0;
    build_level(records, i, 0, ct.entries);
    return ct;
}

} // namespace ct_bridge

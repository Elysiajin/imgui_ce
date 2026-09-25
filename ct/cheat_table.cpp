#include "ct/cheat_table.h"

#include "core/string_conversion.h"

#include <pugixml.hpp>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

std::string to_lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim_copy(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

// 子节点文本（不存在返回空串）
std::string child_text(const pugi::xml_node& parent, const char* name)
{
    pugi::xml_node n = parent.child(name);
    return n ? n.text().as_string() : std::string();
}

// 剥掉 CE Description 包裹的引号
std::string strip_quotes(const std::string& s)
{
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

// CE 偏移文本：十六进制（可带 0x / 负号）
bool parse_offset_text(const std::string& s, uint64_t& out)
{
    std::string t = trim_copy(s);
    if (t.empty())
        return false;
    bool neg = false;
    if (t[0] == '-') { neg = true; t = t.substr(1); }
    if (t.size() > 1 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))
        t = t.substr(2);
    uint64_t v = 0;
    for (char c : t) {
        if (!std::isxdigit((unsigned char)c))
            return false;
        v = v * 16 + (uint64_t)(std::isdigit((unsigned char)c)
                                    ? c - '0'
                                    : std::tolower((unsigned char)c) - 'a' + 10);
    }
    out = neg ? (uint64_t)(0 - (int64_t)v) : v;
    return true;
}

// 与 parse_offset_text 对称的写回格式（CE 习惯：hex 无前缀，负值 '-' 前缀）
std::string format_offset_text(uint64_t v)
{
    char buf[32];
    if ((int64_t)v < 0)
        std::snprintf(buf, sizeof(buf), "-%llX",
                      (unsigned long long)(uint64_t)(0 - (int64_t)v));
    else
        std::snprintf(buf, sizeof(buf), "%llX", (unsigned long long)v);
    return buf;
}

// Color 文本：6 位十六进制
uint32_t parse_color_text(const std::string& s)
{
    return (uint32_t)std::strtoul(s.c_str(), nullptr, 16);
}

std::string format_color_text(uint32_t c)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06X", c & 0xFFFFFF);
    return buf;
}

// ---------------------------------------------------------------- 加载 ----

void load_entry(const pugi::xml_node& node, ct_entry& e)
{
    e.description = strip_quotes(child_text(node, "Description"));

    pugi::xml_node opts = node.child("Options");
    if (opts) {
        for (const pugi::xml_attribute& a : opts.attributes()) {
            if (std::strcmp(a.name(), "moHideChildren") == 0)
                e.collapsed_hint = (std::strcmp(a.value(), "1") == 0);
            else
                e.extra_options.emplace_back(a.name(), a.value());
        }
    }

    e.is_group = (child_text(node, "GroupHeader") == "1");

    pugi::xml_node hex = node.child("ShowAsHex");
    if (hex) e.show_hex = (hex.text().as_string() == std::string("1"));

    pugi::xml_node sgn = node.child("ShowAsSigned");
    if (sgn) {
        e.show_signed_present = true;
        e.show_signed = (sgn.text().as_string() == std::string("1"));
    }

    pugi::xml_node col = node.child("Color");
    if (col) {
        e.has_color = true;
        e.color = parse_color_text(col.text().as_string());
    }

    pugi::xml_node bin = node.child("ShowAsBinary");
    if (bin) e.show_binary = (bin.text().as_string() == std::string("1"));

    // ---- 类型（缺省 Byte，同 CE StringToVariableType 的兜底）----
    std::string vt = trim_copy(child_text(node, "VariableType"));
    if (!vt.empty()) {
        bool unicode = false, known = false;
        e.type = ct_string_to_vartype(vt, unicode, known);
        e.unicode_string = unicode;
        if (!known)
            e.raw_type = vt;   // 未识别类型：按 text 占位显示，保存时写回原文
    }

    // 类型专属子标签
    switch (e.type) {
    case value_type::byte_array:
        e.byte_length = node.child("ByteLength").text().as_int(0);
        break;
    case value_type::text:
        e.string_length = node.child("Length").text().as_int(0);
        e.codepage = node.child("CodePage").text().as_int(0);
        e.zero_terminate = child_text(node, "ZeroTerminate") != "0";
        break;
    case value_type::binary:
        e.bit_start = node.child("BitStart").text().as_int(0);
        e.bit_length = node.child("BitLength").text().as_int(0);
        break;
    case value_type::auto_assembler: {
        pugi::xml_node sc = node.child("AssemblerScript");
        if (sc) {
            e.script = sc.text().as_string();
            e.script_async = (std::strcmp(sc.attribute("Async").as_string(""), "1") == 0);
        }
        break;
    }
    case value_type::four_bytes:   // Custom 在 VariableType 上识别不出，
    default:                       // 由 <CustomType> 子节点兜底
        break;
    }
    e.custom_type_name = trim_copy(child_text(node, "CustomType"));

    // ---- 地址 / 指针链 ----
    e.address_text = trim_copy(child_text(node, "Address"));
    pugi::xml_node offs = node.child("Offsets");
    if (offs) {
        for (const pugi::xml_node& o : offs.children("Offset")) {
            ct_offset co;
            if (parse_offset_text(o.text().as_string(), co.value))
                e.offsets.push_back(co);
        }
    }

    // ---- 子条目（树的父子就靠这层嵌套）----
    pugi::xml_node kids = node.child("CheatEntries");
    if (kids) {
        for (const pugi::xml_node& c : kids.children("CheatEntry")) {
            ct_entry child;
            load_entry(c, child);
            e.children.push_back(std::move(child));
        }
    }
}

} // namespace

ct_load_result load_ct_file(const std::string& utf8_path)
{
    ct_load_result out;

    const std::wstring wpath = utf8_to_wstring(utf8_path);

    // 老版二进制表（CE 5.6 "CHEATENGINE" 魔数）不支持
    {
        // 宽字符重载：UTF-8 路径经 ANSI 代码页解释会打不开中文文件名
        std::ifstream f(wpath.c_str(), std::ios::binary);
        if (!f) {
            out.error = "无法打开文件";
            return out;
        }
        char head[11] = {};
        f.read(head, sizeof(head));
        std::string h(head, (size_t)f.gcount());
        for (char& c : h) c = (char)std::toupper((unsigned char)c);
        if (h.rfind("CHEATENGINE", 0) == 0) {
            out.error = "不支持 CE 5.x 老版二进制表，请用 CE 7.x 另存为 XML 格式";
            return out;
        }
    }

    auto doc = std::make_unique<pugi::xml_document>();
    pugi::xml_parse_result pr = doc->load_file(wpath.c_str());
    if (!pr) {
        out.error = "XML 解析失败: " + std::string(pr.description());
        return out;
    }

    pugi::xml_node root = doc->child("CheatTable");
    if (!root) {
        out.error = "不是有效的 Cheat Table（缺少 <CheatTable> 根节点）";
        return out;
    }
    out.table.version = root.attribute("CheatEngineTableVersion").as_int(46);

    for (const pugi::xml_node& n : root.children()) {
        const std::string tag = n.name();
        if (tag == "CheatEntries") {
            for (const pugi::xml_node& c : n.children("CheatEntry")) {
                ct_entry e;
                load_entry(c, e);
                out.table.entries.push_back(std::move(e));
            }
        } else if (tag == "UserdefinedSymbols") {
            for (const pugi::xml_node& se : n.children("SymbolEntry")) {
                ct_user_symbol s;
                s.name = child_text(se, "Name");
                s.address_text = child_text(se, "Address");
                if (!s.name.empty())
                    out.table.user_symbols.push_back(std::move(s));
            }
        } else if (tag == "Comments") {
            out.table.comments = n.text().as_string();
        }
        // 其余表级节点（LuaScript/Forms/Files/Structures/...）留在 doc 里，
        // 保存时原样写回。
    }

    out.table.doc = std::move(doc);
    out.ok = true;
    return out;
}

// ---------------------------------------------------------------- 保存 ----

namespace {

// VariableType → XML 字符串（对照 CEFuncProc.pas VariableTypeToString）
std::string vartype_xml(const ct_entry& e)
{
    if (!e.raw_type.empty())
        return e.raw_type;   // 未识别类型写回原文
    return ct_vartype_to_string(e.type, e.unicode_string);
}

// 类型专属子标签（对照 MemoryRecordUnit.pas getXMLNode 的类型分支）
void write_type_children(pugi::xml_node& ce, const ct_entry& e)
{
    auto add = [&](const char* name, const std::string& text) {
        ce.append_child(name).text().set(text.c_str());
    };
    switch (e.type) {
    case value_type::text:
        if (e.string_length > 0)
            add("Length", std::to_string(e.string_length));
        add("Unicode", e.unicode_string ? "1" : "0");
        if (e.codepage > 0)
            add("CodePage", std::to_string(e.codepage));
        if (!e.zero_terminate)
            add("ZeroTerminate", "0");
        break;
    case value_type::byte_array:
        add("ByteLength", std::to_string(e.byte_length));
        break;
    case value_type::binary:
        add("BitStart", std::to_string(e.bit_start));
        add("BitLength", std::to_string(e.bit_length));
        add("ShowAsBinary", e.show_binary ? "1" : "0");
        break;
    case value_type::auto_assembler: {
        pugi::xml_node sc = ce.append_child("AssemblerScript");
        if (e.script_async)
            sc.append_attribute("Async") = "1";
        sc.text().set(e.script.c_str());
        break;
    }
    default:
        if (!e.custom_type_name.empty())
            add("CustomType", e.custom_type_name);
        break;
    }
}

// <Address> + 指针链 <Offsets>（group 头带地址时同样写出）
void write_address_and_offsets(pugi::xml_node& ce, const ct_entry& e)
{
    if (!e.address_text.empty())
        ce.append_child("Address").text().set(e.address_text.c_str());
    if (!e.offsets.empty()) {
        pugi::xml_node offs = ce.append_child("Offsets");
        for (const ct_offset& o : e.offsets)
            offs.append_child("Offset").text().set(
                format_offset_text(o.value).c_str());
    }
}

void write_entry(const ct_entry& e, pugi::xml_node parent, int& next_id)
{
    pugi::xml_node ce = parent.append_child("CheatEntry");

    ce.append_child("ID").text().set(next_id++);

    // Description 保存时被 "..." 包裹（同 CE）
    ce.append_child("Description").text().set(
        ("\"" + e.description + "\"").c_str());

    // Options：折叠状态 + 透传属性
    if (e.collapsed_hint || !e.extra_options.empty()) {
        pugi::xml_node opts = ce.append_child("Options");
        if (e.collapsed_hint)
            opts.append_attribute("moHideChildren") = "1";
        for (const auto& kv : e.extra_options)
            opts.append_attribute(kv.first.c_str()) = kv.second.c_str();
    }

    if (e.show_hex)
        ce.append_child("ShowAsHex").text().set("1");
    if (e.show_signed_present)
        ce.append_child("ShowAsSigned").text().set(e.show_signed ? "1" : "0");
    if (e.has_color)
        ce.append_child("Color").text().set(format_color_text(e.color).c_str());

    if (e.is_group) {
        // 分组头：与 VariableType 互斥；带地址的分组头（fisAddressGroupHeader）
        // 仍然写 Address/Offsets
        ce.append_child("GroupHeader").text().set("1");
        write_address_and_offsets(ce, e);
    } else {
        ce.append_child("VariableType").text().set(vartype_xml(e).c_str());
        write_type_children(ce, e);
        if (e.type != value_type::auto_assembler)
            write_address_and_offsets(ce, e);
    }

    // 子条目递归
    if (!e.children.empty()) {
        pugi::xml_node kids = ce.append_child("CheatEntries");
        for (const ct_entry& c : e.children)
            write_entry(c, kids, next_id);
    }
}

} // namespace

bool save_ct_file(cheat_table& t, const std::string& utf8_path, std::string& error)
{
    // 无原始文档（新建表）时从零搭骨架
    if (!t.doc)
        t.doc = std::make_unique<pugi::xml_document>();

    pugi::xml_document& doc = *t.doc;

    pugi::xml_node root = doc.child("CheatTable");
    if (!root) {
        pugi::xml_node decl = doc.append_child(pugi::node_declaration);
        decl.append_attribute("version") = "1.0";
        decl.append_attribute("encoding") = "utf-8";
        root = doc.append_child("CheatTable");
    }
    root.attribute("CheatEngineTableVersion") = std::to_string(t.version).c_str();

    // 原地重建三个受管节点，其余表级节点不动（LuaScript 等保真）。
    // 注意 <CheatEntries> 等是 <CheatTable> 的直接子节点，必须从 root 删除；
    // 从 doc 删会落空导致文件里出现新旧两份 entries。
    while (root.remove_child("CheatEntries")) {}
    while (root.remove_child("UserdefinedSymbols")) {}
    while (root.remove_child("Comments")) {}

    if (!t.entries.empty()) {
        pugi::xml_node entries = root.append_child("CheatEntries");
        int next_id = 1;
        for (const ct_entry& e : t.entries)
            write_entry(e, entries, next_id);
    }

    if (!t.user_symbols.empty()) {
        pugi::xml_node us = root.append_child("UserdefinedSymbols");
        for (const ct_user_symbol& s : t.user_symbols) {
            pugi::xml_node se = us.append_child("SymbolEntry");
            se.append_child("Name").text().set(s.name.c_str());
            se.append_child("Address").text().set(s.address_text.c_str());
        }
    } else {
        root.append_child("UserdefinedSymbols");   // CE 通常保留空标签
    }

    if (!t.comments.empty())
        root.append_child("Comments").text().set(t.comments.c_str());

    const std::wstring wpath = utf8_to_wstring(utf8_path);
    if (!doc.save_file(wpath.c_str(), "  ")) {
        error = "写入文件失败（路径占用或无权限？）";
        return false;
    }
    return true;
}

// ------------------------------------------------- VariableType 映射 ----

value_type ct_string_to_vartype(const std::string& s, bool& unicode, bool& ok)
{
    unicode = false;
    ok = true;
    const std::string v = to_lower(trim_copy(s));
    if (v == "byte")                return value_type::one_byte;
    if (v == "2 bytes")             return value_type::two_bytes;
    if (v == "4 bytes")             return value_type::four_bytes;
    if (v == "8 bytes")             return value_type::eight_bytes;
    if (v == "float")               return value_type::float32;
    if (v == "double")              return value_type::float64;
    if (v == "string")              return value_type::text;
    if (v == "unicode string") { unicode = true; return value_type::text; }
    if (v == "array of byte")       return value_type::byte_array;
    if (v == "binary")              return value_type::binary;
    if (v == "auto assembler script") return value_type::auto_assembler;
    // Custom 按"未识别"处理：raw_type 保留原文（保存写回 "Custom"），
    // 类型名由 <CustomType> 子节点独立往返
    ok = false;
    return value_type::text;   // 未知：text 占位，raw_type 保真
}

std::string ct_vartype_to_string(value_type t, bool unicode)
{
    switch (t) {
    case value_type::one_byte:       return "Byte";
    case value_type::two_bytes:      return "2 Bytes";
    case value_type::four_bytes:     return "4 Bytes";
    case value_type::eight_bytes:    return "8 Bytes";
    case value_type::float32:        return "Float";
    case value_type::float64:        return "Double";
    case value_type::text:           return unicode ? "Unicode String" : "String";
    case value_type::byte_array:     return "Array of byte";
    case value_type::binary:         return "Binary";
    case value_type::auto_assembler: return "Auto Assembler Script";
    }
    return "Byte";
}

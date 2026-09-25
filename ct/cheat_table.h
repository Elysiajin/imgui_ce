#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <pugixml.hpp>

#include "type/value_type.h"

// CT（Cheat Table）文件模型与加载/保存。
//
// 格式（对照 CE 7.x，cheat-engine-master/OpenSave.pas + MemoryRecordUnit.pas）：
//   <CheatTable CheatEngineTableVersion="46">
//     <CheatEntries>              ← 树状条目，递归嵌套
//       <CheatEntry>
//         <ID> <Description>".."</Description> <Options .../>
//         <GroupHeader>1</GroupHeader>        （分组头；与 VariableType 互斥）
//         <ShowAsHex>1</ShowAsHex> <ShowAsSigned>0</ShowAsSigned> <Color>..</Color>
//         <VariableType>4 Bytes</VariableType> <Address>Sword2.exe+13740C</Address>
//         <Offsets><Offset>10</Offset>...</Offsets>   （存在即指针链）
//         <AssemblerScript>…多行文本…</AssemblerScript>
//         <CheatEntries>…子条目…</CheatEntries>
//       </CheatEntry>
//     </CheatEntries>
//     <UserdefinedSymbols/> <Comments/> <LuaScript/> …
//
// 保真策略：加载时保留整个 XML 文档；保存时只原地重建 <CheatEntries> /
// <UserdefinedSymbols> / <Comments>，其余表级节点（LuaScript、Forms、Files、
// Structures 等）原样写回——保证"打开→保存"不破坏我们不支持的脚本/结构体。

// 指针链的一级偏移
struct ct_offset {
    uint64_t value = 0;    // 十六进制文本解析
};

// CT 条目（树状节点）
struct ct_entry {
    std::string description;       // 不含 CE 包裹的引号
    bool        is_group = false;  // <GroupHeader>1</GroupHeader>
    value_type  type = value_type::four_bytes;
    std::string raw_type;          // 未识别 VariableType 原文（保存时优先写回）

    std::string address_text;      // <Address> 原文（可解释地址）
    std::vector<ct_offset> offsets;   // 非空即指针链

    int         byte_length = 0;   // Array of byte 的 <ByteLength>
    std::string script;            // Auto Assembler 脚本全文
    bool        script_async = false;

    bool show_signed = false;          // <ShowAsSigned> 的值
    bool show_signed_present = false;  // 标签是否存在（保存时才写回）
    bool show_hex = false;             // <ShowAsHex>
    bool show_binary = false;          // Binary 类型的 <ShowAsBinary>
    bool has_color = false;
    uint32_t color = 0;                // <Color> 6 位十六进制

    // String 类型
    bool unicode_string = false;       // "Unicode String" 标志
    int  string_length = 0;
    int  codepage = 0;
    bool zero_terminate = true;

    // Binary 类型
    int bit_start = 0;
    int bit_length = 0;

    // Custom 类型
    std::string custom_type_name;

    // <Options>：moHideChildren 由 collapsed_hint 表达；其余属性原样透传
    bool collapsed_hint = false;
    std::vector<std::pair<std::string, std::string>> extra_options;

    std::vector<ct_entry> children;    // <CheatEntries> 嵌套
};

// <UserdefinedSymbols> 的一项
struct ct_user_symbol {
    std::string name;           // 符号名
    std::string address_text;   // <Address> 原文（可解释地址，加载后按需再解析）
};

// 整张表
struct cheat_table {
    int version = 46;
    std::vector<ct_entry>       entries;
    std::vector<ct_user_symbol> user_symbols;
    std::string                 comments;

    // 加载时的原始文档；保存时在其上原地更新。新建表时为空。
    std::unique_ptr<pugi::xml_document> doc;
};

struct ct_load_result {
    bool        ok = false;
    std::string error;
    cheat_table table;
};

// utf8 路径。老版二进制 CT（"CHEATENGINE" 魔数）不支持，返回错误。
ct_load_result load_ct_file(const std::string& utf8_path);

// utf8 路径。成功返回 true；失败时 error 带原因。
bool save_ct_file(cheat_table& t, const std::string& utf8_path, std::string& error);

// VariableType 字符串 ↔ value_type（大小写不敏感）。
// 未知字符串返回 false，raw_type 由调用方保留。
value_type ct_string_to_vartype(const std::string& s, bool& unicode, bool& ok);
std::string ct_vartype_to_string(value_type t, bool unicode);

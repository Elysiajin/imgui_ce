// CT 加载/保存 round-trip 回归测试（独立控制台程序，不依赖 GUI/目标进程）。
//
// 用法：ct_roundtrip.exe <真实CT文件> [更多CT文件...]
// 每个文件：load → save(临时) → 再 load → 两棵树逐字段比对；
// 任何字段丢失即报错退出。另附 Sword2.CT 的结构断言（若文件名匹配）。

#include "ct/cheat_table.h"
#include "core/string_conversion.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <windows.h>
#include <shellapi.h>

static int g_fail = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            std::printf("  [FAIL] %s\n", msg);                  \
            ++g_fail;                                           \
        }                                                       \
    } while (0)

static void compare_entry(const ct_entry& a, const ct_entry& b, const std::string& path)
{
    CHECK(a.description == b.description, (path + " description").c_str());
    CHECK(a.is_group == b.is_group, (path + " is_group").c_str());
    CHECK(a.type == b.type, (path + " type").c_str());
    CHECK(a.raw_type == b.raw_type, (path + " raw_type").c_str());
    CHECK(a.address_text == b.address_text, (path + " address").c_str());
    CHECK(a.offsets.size() == b.offsets.size(), (path + " offsets.size").c_str());
    for (size_t i = 0; i < a.offsets.size() && i < b.offsets.size(); ++i)
        CHECK(a.offsets[i].value == b.offsets[i].value,
              (path + " offsets[" + std::to_string(i) + "]").c_str());
    CHECK(a.byte_length == b.byte_length, (path + " byte_length").c_str());
    CHECK(a.script == b.script, (path + " script").c_str());
    CHECK(a.script_async == b.script_async, (path + " script_async").c_str());
    CHECK(a.show_signed == b.show_signed, (path + " show_signed").c_str());
    CHECK(a.show_signed_present == b.show_signed_present, (path + " show_signed_present").c_str());
    CHECK(a.show_hex == b.show_hex, (path + " show_hex").c_str());
    CHECK(a.show_binary == b.show_binary, (path + " show_binary").c_str());
    CHECK(a.has_color == b.has_color, (path + " has_color").c_str());
    if (a.has_color && b.has_color)
        CHECK(a.color == b.color, (path + " color").c_str());
    CHECK(a.unicode_string == b.unicode_string, (path + " unicode").c_str());
    CHECK(a.string_length == b.string_length, (path + " string_length").c_str());
    CHECK(a.bit_start == b.bit_start, (path + " bit_start").c_str());
    CHECK(a.bit_length == b.bit_length, (path + " bit_length").c_str());
    CHECK(a.custom_type_name == b.custom_type_name, (path + " custom_type").c_str());
    CHECK(a.collapsed_hint == b.collapsed_hint, (path + " collapsed").c_str());
    CHECK(a.extra_options.size() == b.extra_options.size(), (path + " extra_options").c_str());
    CHECK(a.children.size() == b.children.size(), (path + " children.size").c_str());
    for (size_t i = 0; i < a.children.size() && i < b.children.size(); ++i)
        compare_entry(a.children[i], b.children[i],
                      path + "/" + std::to_string(i));
}

static void test_file(const std::string& file)
{
    const int fail_at_entry = g_fail;   // 局部判定：只看本文件新增的失败
    std::printf("== %s\n", file.c_str());
    const auto t0 = std::chrono::steady_clock::now();

    ct_load_result first = load_ct_file(file);
    CHECK(first.ok, "load ok");
    if (!first.ok) {
        std::printf("  error: %s\n", first.error.c_str());
        return;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::printf("  load %lld ms, version=%d, top=%zu entries, symbols=%zu, comments=%zu bytes\n",
                ms, first.table.version, first.table.entries.size(),
                first.table.user_symbols.size(), first.table.comments.size());

    // save → load → 树比对（输出到当前目录，避开 temp_directory_path 的环境差异）
    const std::string tmp = "ct_roundtrip_test.ct";
    std::string err;
    CHECK(save_ct_file(first.table, tmp, err), "save ok");
    if (!err.empty()) std::printf("  save error: %s\n", err.c_str());

    ct_load_result second = load_ct_file(tmp);
    CHECK(second.ok, "reload ok");
    if (!second.ok) {
        std::printf("  reload error: %s\n", second.error.c_str());
        return;
    }
    CHECK(first.table.entries.size() == second.table.entries.size(), "top count");
    CHECK(first.table.user_symbols.size() == second.table.user_symbols.size(), "symbol count");
    CHECK(first.table.comments == second.table.comments, "comments");
    for (size_t i = 0; i < first.table.entries.size(); ++i)
        compare_entry(first.table.entries[i], second.table.entries[i],
                      std::to_string(i));
    std::printf("  round-trip %s\n",
                g_fail == fail_at_entry ? "OK" : "HAS FAILURES");
}

// Sword2.CT 的结构断言（分组 + 脚本 + 数值 + ShowAsSigned + Comments）
static void test_sword2()
{
    const std::string file = "D:/Cheat Table/Sword2.CT";
    if (!std::filesystem::exists(file))
        return;
    std::printf("== Sword2.CT 结构断言\n");
    ct_load_result r = load_ct_file(file);
    CHECK(r.ok, "load");
    if (!r.ok) return;

    const auto& es = r.table.entries;
    CHECK(es.size() == 4, "4 top entries");
    if (es.size() < 4) return;

    CHECK(es[0].is_group, "entry0 = GroupHeader");
    CHECK(es[0].description == "角色属性", "entry0 desc");
    CHECK(es[0].collapsed_hint, "entry0 moHideChildren → 折叠");
    CHECK(es[0].children.size() == 1, "entry0 有 1 个子条目");
    if (es[0].children.size() == 1) {
        CHECK(es[0].children[0].description == "金钱", "子条目 desc");
        CHECK(es[0].children[0].type == value_type::four_bytes, "子条目 4 Bytes");
        CHECK(es[0].children[0].address_text == "Sword2.exe+13740C", "子条目 地址");
    }

    CHECK(es[1].type == value_type::auto_assembler, "entry1 = AA 脚本");
    CHECK(es[1].script.find("[ENABLE]") != std::string::npos, "脚本含 [ENABLE]");
    CHECK(es[1].script.find("db 90 90") != std::string::npos, "脚本含 db 90 90");
    CHECK(es[1].script.find("[DISABLE]") != std::string::npos, "脚本含 [DISABLE]");

    CHECK(es[3].show_signed_present && !es[3].show_signed, "entry3 ShowAsSigned=0");
    CHECK(!r.table.comments.empty(), "Comments 非空");
    CHECK(r.table.comments.find("人物扣血的指令") != std::string::npos, "Comments 内容");
}

// CE教程.CT：alloc/label/registersymbol 脚本内容原样保真
static void test_tutorial()
{
    const std::string file = "D:/Cheat Table/CE教程.CT";
    if (!std::filesystem::exists(file))
        return;
    std::printf("== CE教程.CT 断言\n");
    ct_load_result r = load_ct_file(file);
    CHECK(r.ok, "load");
    if (!r.ok) return;
    const auto& es = r.table.entries;
    CHECK(es.size() == 3, "3 top entries");
    if (es.size() < 3) return;
    CHECK(es[0].type == value_type::auto_assembler, "entry0 脚本");
    CHECK(es[1].script.find("alloc(damage,4)") != std::string::npos, "alloc 原文(entry1)");
    CHECK(es[1].script.find("registersymbol(demage)") != std::string::npos,
          "registersymbol 原文(entry1)");
    CHECK(es[2].address_text == "demage", "符号地址条目");
}

int main_impl(int argc, char** argv)
{
    test_sword2();
    test_tutorial();
    for (int i = 1; i < argc; ++i)
        test_file(argv[i]);

    if (g_fail == 0)
        std::printf("\nALL PASSED\n");
    else
        std::printf("\n%d FAILURES\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}

// MinGW 的 argv 按本地代码页（GBK）解码，中文路径会损坏；
// 统一从 GetCommandLineW 取 UTF-16 再转 UTF-8。
int main()
{
    int argc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i)
        args.push_back(wstring_to_utf8(wargv[i]));
    LocalFree(wargv);

    std::vector<char*> argvp;
    for (auto& a : args)
        argvp.push_back(a.data());
    argvp.push_back(nullptr);
    return main_impl(argc, argvp.data());
}

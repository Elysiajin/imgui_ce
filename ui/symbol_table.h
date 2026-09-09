#ifndef SYMBOL_TABLE_H
#define SYMBOL_TABLE_H

#include <cstdint>
#include <string>
#include <vector>

// 单个导出符号（所在模块内按地址升序存放）
struct symbol_entry {
    uint64_t address;      // 模块基址 + 函数 RVA
    uint32_t name_offset;  // 名字在 module_symbols::name_storage 中的偏移
};

// 一个模块的导出符号集
struct module_symbols {
    uint64_t base = 0;
    uint64_t size = 0;
    std::string name;
    std::string path;

    bool parsed      = false;  // 是否已尝试解析（避免反复读同一个无导出表的文件）
    bool has_exports = false;
    std::vector<symbol_entry> symbols;  // 按 address 升序（二分查找用）
    std::string name_storage;           // 所有符号名连续存放，降低内存碎片

    const char* symbol_name(const symbol_entry& e) const {
        return name_storage.c_str() + e.name_offset;
    }
};

// 导出符号表（对标 CE 的 TSymbolListHandler 的精简实现）：
//   - update_target() 跟随附加进程刷新模块列表（pid 变化才重新枚举）
//   - 惰性解析：真正查询某模块符号时才从磁盘读它的 PE 导出表，结果缓存
//   - 查询全部走二分查找（模块按基址有序 / 符号按地址有序）
//   - 名字连续存储，单个符号只占 16 字节 entry，内存占用小
//   - 仅限 UI 线程使用（与本项目其它 ImGui 绘制代码一致）
class symbol_table {
public:
    static symbol_table& instance();

    // 跟随附加进程：pid 变化（含脱离）时重建模块列表
    void update_target(uint32_t pid);

    // 确保指定模块的导出表已解析；返回该模块是否有可用符号
    bool ensure_loaded(uint64_t module_base);
    // 确保 addr 所在模块的导出表已解析（反汇编符号显示的惰性入口）
    bool ensure_loaded_for_address(uint64_t addr);

    // 地址 -> 最近符号（<= addr 的最大地址符号）。
    // out_off = addr - 符号地址；未命中返回 false
    bool find_symbol(uint64_t addr, const module_symbols** out_mod,
                     const symbol_entry** out_sym, uint64_t* out_off) const;

    // 便捷封装：返回 "模块名.符号名(+偏移)"；未命中返回空串。
    // 注意：调用前需 ensure_loaded_for_address() 确保模块已解析
    std::string format_symbol(uint64_t addr, bool with_module = true) const;

    const std::vector<module_symbols>& modules() const { return modules_; }

    // 按基址精确查找模块（已解析/未解析均可）；找不到返回 nullptr
    const module_symbols* find_module(uint64_t module_base) const;

private:
    void refresh_modules(uint32_t pid);
    static bool parse_export_table(module_symbols& m);

    uint32_t pid_ = 0;
    std::vector<module_symbols> modules_;  // 按 base 升序
};

#endif // SYMBOL_TABLE_H

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/imemory_accessor.h"
#include "type/memory_region.h"
#include "type/module_info.h"

// Auto Assembler（AA）脚本执行——CE 脚本条目的 [ENABLE]/[DISABLE] 块。
//
// 完整支持（两遍汇编，对标 CE 的 assembler 语义）：
//   [ENABLE] / [DISABLE]          段标记（大小写不敏感）
//   "地址:"                       绝对标号（hex / 模块±偏移 / 符号）
//   label(x) / x:                 局部标号声明与定义（支持前向引用，
//                                 迭代定长直到布局稳定——同汇编器两遍扫描）
//   alloc(x, size)                目标进程分配 RWX 内存并注册进会话
//   dealloc(x)                    释放会话中 alloc 的内存（[DISABLE] 用）
//   define(x, val)                常量符号
//   registersymbol(x)             把标号/alloc 地址注册为全局符号
//   unregistersymbol(x)           移除全局符号
//   aobscan(x, "AA BB ??")        全进程可读区特征码扫描（?? 通配）
//   aobscanmodule(x, mod, "AA")   模块内特征码扫描
//   db/dw/dd/dq                   数据写入（每项推进当前地址）
//   汇编指令                      asm_parse 解析 + Zydis 编码 + 写入
//   ';' 与 '//'                   注释
// 不支持：{$LUA}（整块拒绝执行，明确报错）。
struct aa_session {
    // [ENABLE] 的 alloc 结果（name → 地址），[DISABLE] 的 dealloc 据此释放；
    // 由条目（address_record）跨激活/取消持有。
    std::unordered_map<std::string, uint64_t> allocs;
};

struct aa_result {
    bool ok = false;
    std::vector<std::string> errors;    // "line N: 原因"
    std::vector<std::string> warnings;  // 跳过/降级信息
    // registersymbol 的执行结果（enable 时）：调用方应并入全局符号表
    std::vector<std::pair<std::string, uint64_t>> registered;
    // unregistersymbol 的执行结果（disable 时）：调用方从全局符号表移除
    std::vector<std::string> unregistered;
};

// enable=true 执行 [ENABLE] 块，否则 [DISABLE] 块；无段标记时整段生效。
//
// symbols_in：解析引用时的完整查询表（CT UserdefinedSymbols + 已注册全局符号；
//             键小写）。enable 时新注册的符号写进 result.registered 而不是直接
//             改表——由调用方决定落库时机。
// session：   alloc 的跨块状态（同一脚本的 enable/disable 传同一实例）。
// mem/arch/regions：内存接口、目标架构、目标进程的内存区域列表
//             （aobscan 扫描用；便于 mock 单测——mem==nullptr 时取
//             process_manager 的）。
aa_result aa_run_block(const std::string& script, bool enable,
                       const std::vector<module_info>& modules,
                       const std::unordered_map<std::string, uint64_t>& symbols_in,
                       aa_session& session,
                       IMemoryAccessor* mem = nullptr,
                       process_arch arch = process_arch::unknown,
                       const std::vector<memory_region>& regions = {});

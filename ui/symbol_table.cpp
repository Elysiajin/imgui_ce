#include "symbol_table.h"

#include "core/process_manager.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

// ── 小端读取（PE 结构按 1 字节对齐，避免直接解引用引发未对齐访问）──
static uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

symbol_table& symbol_table::instance() {
    static symbol_table t;
    return t;
}

void symbol_table::update_target(uint32_t pid) {
    if (pid_ == pid)
        return;
    pid_ = pid;
    modules_.clear();
    if (pid != 0)
        refresh_modules(pid);
}

void symbol_table::refresh_modules(uint32_t pid) {
    const auto list = process_manager::instance().modules().enumerate(pid);
    modules_.reserve(list.size());
    for (const auto& m : list) {
        module_symbols ms;
        ms.base = m.base;
        ms.size = m.size;
        ms.name = m.name;
        ms.path = m.path;
        modules_.push_back(std::move(ms));
    }
    std::sort(modules_.begin(), modules_.end(),
              [](const module_symbols& a, const module_symbols& b) {
                  return a.base < b.base;
              });
}

const module_symbols* symbol_table::find_module(uint64_t module_base) const {
    const auto it = std::lower_bound(
        modules_.begin(), modules_.end(), module_base,
        [](const module_symbols& m, uint64_t b) { return m.base < b; });
    return (it != modules_.end() && it->base == module_base) ? &*it : nullptr;
}

bool symbol_table::ensure_loaded(uint64_t module_base) {
    auto it = std::lower_bound(
        modules_.begin(), modules_.end(), module_base,
        [](const module_symbols& m, uint64_t b) { return m.base < b; });
    if (it == modules_.end() || it->base != module_base)
        return false;

    module_symbols& m = *it;
    if (m.parsed)
        return m.has_exports;

    m.parsed = true;
    parse_export_table(m);
    return m.has_exports;
}

bool symbol_table::ensure_loaded_for_address(uint64_t addr) {
    // 找 base <= addr 的最后一个模块，再检查 addr 是否落在其范围内
    auto it = std::upper_bound(
        modules_.begin(), modules_.end(), addr,
        [](uint64_t a, const module_symbols& m) { return a < m.base; });
    if (it == modules_.begin())
        return false;
    --it;
    if (it->size == 0 || addr >= it->base + it->size)
        return false;
    return ensure_loaded(it->base);
}

bool symbol_table::find_symbol(uint64_t addr, const module_symbols** out_mod,
                               const symbol_entry** out_sym,
                               uint64_t* out_off) const {
    const auto mit = std::upper_bound(
        modules_.begin(), modules_.end(), addr,
        [](uint64_t a, const module_symbols& m) { return a < m.base; });
    if (mit == modules_.begin())
        return false;
    const module_symbols& m = *(mit - 1);
    if (!m.parsed || !m.has_exports || m.size == 0 || addr >= m.base + m.size)
        return false;

    // 符号按地址升序：取 address <= addr 的最后一个
    const auto sit = std::upper_bound(
        m.symbols.begin(), m.symbols.end(), addr,
        [](uint64_t a, const symbol_entry& e) { return a < e.address; });
    if (sit == m.symbols.begin())
        return false;
    const symbol_entry& hit = *(sit - 1);

    *out_mod = &m;
    *out_sym = &hit;
    *out_off = addr - hit.address;
    return true;
}

std::string symbol_table::format_symbol(uint64_t addr, bool with_module) const {
    const module_symbols* mod = nullptr;
    const symbol_entry*   sym = nullptr;
    uint64_t off = 0;
    if (!find_symbol(addr, &mod, &sym, &off))
        return {};

    std::string out;
    if (with_module) {
        out.reserve(mod->name.size() + 2 + sym->name_offset); // 近似预留
        out += mod->name;
        out += '.';
    }
    out += mod->symbol_name(*sym);
    if (off) {
        char b[24];
        std::snprintf(b, sizeof b, "+%llx", (unsigned long long)off);
        out += b;
    }
    return out;
}

// ── PE 导出表解析（从磁盘读模块文件，不碰目标进程内存）───────────────
// 流程：DOS 头 -> PE 头 -> 数据目录[0]（导出表 RVA/Size）-> 节表做 RVA→文件偏移
//       -> 遍历名字表/序号表/函数表，得到 (基址+函数RVA, 名字) 列表。
// 转发导出（forwarder，函数 RVA 落在导出目录内部）指向的是字符串而非代码，
// 直接跳过，避免反汇编/符号查找命中假地址。
bool symbol_table::parse_export_table(module_symbols& m) {
    m.has_exports = false;
    if (m.path.empty())
        return false;

    std::ifstream f(m.path, std::ios::binary);
    if (!f)
        return false;
    f.seekg(0, std::ios::end);
    const std::streamoff fsz = f.tellg();
    if (fsz <= 0 || fsz > (64 << 20))   // 防御：异常大文件直接放弃
        return false;
    f.seekg(0);
    std::vector<uint8_t> buf((size_t)fsz);
    f.read(reinterpret_cast<char*>(buf.data()), fsz);
    if (f.gcount() != fsz)
        return false;

    // DOS 头：'MZ' + e_lfanew
    if (buf.size() < 0x40 || rd16(buf.data()) != 0x5A4D)
        return false;
    const uint32_t e_lfanew = rd32(buf.data() + 0x3C);
    if (e_lfanew + 4 + 20 > buf.size() || rd32(buf.data() + e_lfanew) != 0x00004550)
        return false;  // 'PE\0\0'

    // COFF 头：节数量(+2)、OptionalHeader 大小(+16)
    const uint8_t* coff = buf.data() + e_lfanew + 4;
    const uint16_t num_sections = rd16(coff + 2);
    const uint16_t opt_size     = rd16(coff + 16);
    const uint8_t* opt = coff + 20;
    if (opt_size < 96 || opt + (size_t)opt_size > buf.data() + buf.size())
        return false;

    // OptionalHeader magic：0x20B = PE32+（DataDirectory 偏移 112），0x10B = PE32（96）
    const uint16_t magic  = rd16(opt);
    const uint32_t dd_off = (magic == 0x20B) ? 112u : 96u;
    if (dd_off + 8 > opt_size)
        return false;
    const uint32_t exp_rva  = rd32(opt + dd_off);
    const uint32_t exp_size = rd32(opt + dd_off + 4);
    if (!exp_rva || !exp_size)
        return false;

    // 节表：RVA -> 文件偏移
    const uint8_t* sec = opt + opt_size;
    if (sec + (size_t)num_sections * 40 > buf.data() + buf.size())
        return false;
    auto rva_to_off = [&](uint32_t rva) -> long long {
        for (int i = 0; i < num_sections; ++i) {
            const uint8_t* s = sec + (size_t)i * 40;
            uint32_t vsz = rd32(s + 8);
            const uint32_t va  = rd32(s + 12);
            const uint32_t rsz = rd32(s + 16);
            const uint32_t raw = rd32(s + 20);
            if (vsz == 0) vsz = rsz;
            if (raw == 0) continue;                     // 无文件映像的节
            if (rva >= va && rva < va + vsz)
                return (long long)(rva - va) + raw;
        }
        return -1;
    };

    // 导出目录：NumberOfNames(+24)、函数表(+28)、名字表(+32)、序号表(+36)
    const long long eo = rva_to_off(exp_rva);
    if (eo < 0 || eo + 40 > (long long)buf.size())
        return false;
    const uint32_t n_names = rd32(buf.data() + eo + 24);
    const long long fo_funcs = rva_to_off(rd32(buf.data() + eo + 28));
    const long long fo_names = rva_to_off(rd32(buf.data() + eo + 32));
    const long long fo_ords  = rva_to_off(rd32(buf.data() + eo + 36));
    if (fo_funcs < 0 || fo_names < 0 || fo_ords < 0)
        return false;

    m.symbols.reserve(n_names);
    m.name_storage.reserve((size_t)n_names * 24);

    for (uint32_t i = 0; i < n_names; ++i) {
        if (fo_names + (long long)(i + 1) * 4 > (long long)buf.size()) break;
        const uint32_t name_rva = rd32(buf.data() + fo_names + (size_t)i * 4);
        const long long no = rva_to_off(name_rva);
        if (no < 0 || no >= (long long)buf.size()) continue;
        const char* nb = reinterpret_cast<const char*>(buf.data()) + no;
        const size_t len = ::strnlen(nb, buf.size() - (size_t)no);
        if (len == 0) continue;

        const uint16_t ord = rd16(buf.data() + fo_ords + (size_t)i * 2);
        if (fo_funcs + (long long)(ord + 1) * 4 > (long long)buf.size()) continue;
        const uint32_t func_rva = rd32(buf.data() + fo_funcs + (size_t)ord * 4);
        if (func_rva == 0) continue;                          // 无实现的具名导出
        if (func_rva >= exp_rva && func_rva < exp_rva + exp_size) continue; // 转发导出

        symbol_entry e;
        e.address     = m.base + func_rva;
        e.name_offset = (uint32_t)m.name_storage.size();
        m.name_storage.append(nb, len);
        m.name_storage.push_back('\0');
        m.symbols.push_back(e);
    }

    std::sort(m.symbols.begin(), m.symbols.end(),
              [](const symbol_entry& a, const symbol_entry& b) {
                  return a.address < b.address;
              });
    m.has_exports = !m.symbols.empty();
    return m.has_exports;
}

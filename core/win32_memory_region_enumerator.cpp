#include "core/win32_memory_region_enumerator.h"
#include "core/process_manager.h"

#include <windows.h>

#include <cstdint>

namespace {
    // PAGE_* 保护位是否包含可读
    bool is_readable(uint32_t protect) {
        return protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                          PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                          PAGE_EXECUTE_WRITECOPY);
    }
    bool is_writable(uint32_t protect) {
        return protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                          PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
    }
    bool is_executable(uint32_t protect) {
        return protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                          PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
    }
    bool is_writecopy(uint32_t protect) {
        return protect & (PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY);
    }
}

std::vector<memory_region> Win32MemoryRegionEnumerator::enumerate()
{
    // 默认：扫描全部已提交的可读/可写私有/映像/映射内存（CE 标准行为）
    scan_request default_req;
    default_req.mem_filter.state_filter  = memory_filter::commit;
    default_req.mem_filter.type_filter   = memory_filter::type_private | memory_filter::type_image | memory_filter::type_mapped;
    default_req.mem_filter.access_filter = memory_filter::access_read | memory_filter::access_write;
    return enumerate(default_req);
}

std::vector<memory_region> Win32MemoryRegionEnumerator::enumerate(const scan_request& req)
{
    std::vector<memory_region> regions;
    if (process_manager::instance().attached_pid() == 0)
        return regions;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE,
                                  process_manager::instance().attached_pid());
    if (!hProcess)
        return regions;

    MEMORY_BASIC_INFORMATION mbi;
    uint64_t addr = 0;

    // 如果请求指定了模块范围，设置搜索限制
    uint64_t limit = 0x7FFFFFFFFFFFFFFF;
    if (req.module_base != 0 && req.module_size != 0) {
        addr = req.module_base;
        limit = req.module_base + req.module_size;
    }

    while (addr < limit && VirtualQueryEx(hProcess, (LPCVOID)addr, &mbi, sizeof(mbi))) {
        const auto& mf = req.mem_filter;
        auto next_block = [&]() -> uint64_t {
            return (uint64_t)mbi.BaseAddress + mbi.RegionSize;
        };

        // ---- 阶段 1：状态过滤 ----
        bool state_ok = false;
        if (mf.state_filter & memory_filter::commit)  state_ok = state_ok || (mbi.State == MEM_COMMIT);
        if (mf.state_filter & memory_filter::reserve) state_ok = state_ok || (mbi.State == MEM_RESERVE);
        if (mf.state_filter & memory_filter::free)    state_ok = state_ok || (mbi.State == MEM_FREE);
        if (!state_ok) { addr = next_block(); continue; }

        // ---- 阶段 2：类型过滤 ----
        bool type_ok = false;
        if (mf.type_filter & memory_filter::type_private) type_ok = type_ok || (mbi.Type == MEM_PRIVATE);
        if (mf.type_filter & memory_filter::type_image)   type_ok = type_ok || (mbi.Type == MEM_IMAGE);
        if (mf.type_filter & memory_filter::type_mapped)  type_ok = type_ok || (mbi.Type == MEM_MAPPED);
        if (!type_ok) { addr = next_block(); continue; }

        // ---- 阶段 3：抽象访问权限过滤 ----
        bool canRead    = is_readable(mbi.Protect) && mbi.State == MEM_COMMIT;
        bool canWrite   = is_writable(mbi.Protect);
        bool canExecute = is_executable(mbi.Protect);

        bool passwritable    = !mf.writable    || canWrite;
        bool passexecutable  = !mf.executable  || canExecute;
        bool passcopy_on_write = !mf.copy_on_write || is_writecopy(mbi.Protect);
        if (!passwritable && !passexecutable && !passcopy_on_write) {
            addr = next_block(); continue;
        }

        if (mf.access_filter & memory_filter::access_read && !canRead)     { addr = next_block(); continue; }
        if (mf.access_filter & memory_filter::access_write && !canWrite)    { addr = next_block(); continue; }
        if (mf.access_filter & memory_filter::access_execute && !canExecute){ addr = next_block(); continue; }

        // ---- 阶段 4：具体保护属性过滤 ----
        if (mf.protect_filter != 0) {
            // 简化：保护属性过滤此处仅按位匹配，兼容参考实现
            uint32_t mapped = mbi.Protect & 0xFF;
            if ((mapped & mf.protect_filter) != mf.protect_filter) {
                addr = next_block(); continue;
            }
        }

        {
            uint64_t base = (uint64_t)mbi.BaseAddress;
            size_t size = mbi.RegionSize;

            if (base < addr) {
                size -= (size_t)(addr - base);
                base = addr;
            }
            if (base + size > limit) {
                size = (size_t)(limit - base);
            }

            memory_region r;
            r.base   = base;
            r.size   = size;
            r.protect = mbi.Protect;
            r.state   = mbi.State;
            r.type    = mbi.Type;
            regions.push_back(r);
        }

        addr = next_block();
    }

    CloseHandle(hProcess);
    return regions;
}

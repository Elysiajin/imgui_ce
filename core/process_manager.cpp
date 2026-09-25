#include "core/process_manager.h"

#include "core/win32_memory_accessor.h"
#include "core/win32_memory_region_enumerator.h"
#include "core/win32_module_enumerator.h"
#include "core/win32_process_enumerator.h"

#include <memory>
#include <mutex>

process_manager& process_manager::instance()
{
    static process_manager mgr;
    return mgr;
}

bool process_manager::attach(uint32_t pid)
{
    detach();

    memory_accessor_ = std::make_unique<Win32MemoryAccessor>();
    if (!memory_accessor_->attach(pid)) {
        memory_accessor_.reset();
        return false;
    }

    if (!snapshot_manager_)
        snapshot_manager_ = std::make_shared<process_memory_snapshot_manager>();

    attached_pid_ = pid;
    modules();          // 确保惰性枚举器已创建，否则 update_modules 会把模块缓存清空
    update_modules();
    return true;
}

void process_manager::detach()
{
    if (memory_accessor_)
        memory_accessor_->detach();

    if (snapshot_manager_)
        snapshot_manager_->clear();

    memory_accessor_.reset();
    module_enumerator_.reset();
    region_enumerator_.reset();

    attached_pid_ = 0;
    modules_.clear();
}

IProcessEnumerator& process_manager::processes()
{
    if (!process_enumerator_)
        process_enumerator_ = std::make_unique<Win32ProcessEnumerator>();
    return *process_enumerator_;
}

IModuleEnumerator& process_manager::modules()
{
    if (!module_enumerator_)
        module_enumerator_ = std::make_unique<Win32ModuleEnumerator>();
    return *module_enumerator_;
}

IMemoryRegionEnumerator& process_manager::regions()
{
    if (!region_enumerator_)
        region_enumerator_ = std::make_unique<Win32MemoryRegionEnumerator>();
    return *region_enumerator_;
}

bool process_manager::is_process_alive() const
{
    return memory_accessor_ && memory_accessor_->is_process_alive();
}

std::vector<memory_region> process_manager::get_memory_regions()
{
    // region_enumerator_ 在 detach() 时会被置空；这里走懒加载的 regions()，
    // 确保 attach 后首次获取区域列表时枚举器被自动创建，否则扫描会拿到空列表。
    std::vector<memory_region> out;
    if (attached_pid_ != 0)
        out = regions().enumerate();
    return out;
}

std::vector<memory_region> process_manager::get_memory_regions(const scan_request& req)
{
    std::vector<memory_region> out;
    if (attached_pid_ != 0)
        out = regions().enumerate(req);
    return out;
}

bool process_manager::resolve_address(uint64_t addr, std::string& out_display, bool& is_base) const
{
    std::shared_lock lock(modules_mutex_);
    for (const auto& mod : modules_) {
        if (addr >= mod.base && addr < mod.base + mod.size) {
            uint64_t offset = addr - mod.base;
            if (offset == 0) {
                out_display = mod.name;
            } else {
                char buf[17];
                snprintf(buf, sizeof(buf), "%llx", (unsigned long long)offset);
                out_display = mod.name + "+0x" + buf;
            }
            is_base = (offset == 0);
            return true;
        }
    }
    char buf[19];
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)addr);
    out_display = buf;
    is_base = false;
    return false;
}

void process_manager::update_modules()
{
    std::unique_lock lock(modules_mutex_);
    if (module_enumerator_ && attached_pid_)
        modules_ = module_enumerator_->enumerate(attached_pid_);
    else
        modules_.clear();
}

std::vector<module_info> process_manager::module_snapshot() const
{
    std::shared_lock lock(modules_mutex_);
    return modules_;
}

bool process_manager::terminate_process(uint32_t pid) {
    HANDLE h_process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if(!h_process) return false;
    BOOL result = TerminateProcess(h_process, 1);
    CloseHandle(h_process);
    return result == TRUE;
}
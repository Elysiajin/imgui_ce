#pragma once

#include <cstdint>

#include "core/imemory_accessor.h"
#include "core/imodule_enumerator.h"
#include "core/imemory_region_enumerator.h"
#include "core/iprocess_enumerator.h"
#include "type/memory_region.h"
#include "type/module_info.h"
#include "type/process_info.h"
#include "scan/process_memory_snapshot_manager.h"
#include "type/scan_data_stream_define.h"

#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

class process_manager {
public:
    static process_manager& instance();

    process_manager(const process_manager&) = delete;
    process_manager& operator=(const process_manager&) = delete;

    // 进程附加/分离
    bool attach(uint32_t pid);
    void detach();
    uint32_t attached_pid() const { return attached_pid_; }
    bool is_attached() const { return attached_pid_ != 0; }
    bool is_process_alive() const;

    // 首次访问时才 new
    IProcessEnumerator&      processes();
    IModuleEnumerator&       modules();
    IMemoryRegionEnumerator& regions();

    // 内存读写访问器（可空指针；附加成功后非空）
    IMemoryAccessor* memory() { return memory_accessor_.get(); }
    IMemoryAccessor* memory_naked() { return memory_accessor_.get(); }

    // ---- 扫描模块核心依赖 ----
    std::vector<memory_region> get_memory_regions();
    std::vector<memory_region> get_memory_regions(const scan_request& req);

    bool resolve_address(uint64_t addr, std::string& out_display, bool& is_base) const;
    std::shared_ptr<process_memory_snapshot_manager> get_snapshot_manager() { return snapshot_manager_; }

private:
    process_manager() = default;
    ~process_manager() = default;

    void update_modules();

    uint32_t attached_pid_ = 0;

    std::unique_ptr<IProcessEnumerator>      process_enumerator_;
    std::unique_ptr<IModuleEnumerator>       module_enumerator_;
    std::unique_ptr<IMemoryRegionEnumerator> region_enumerator_;
    std::unique_ptr<IMemoryAccessor>         memory_accessor_;
    std::shared_ptr<process_memory_snapshot_manager> snapshot_manager_;

    std::vector<module_info>  modules_;
    mutable std::shared_mutex modules_mutex_;
};

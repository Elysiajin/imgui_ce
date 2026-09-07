#include "core/win32_memory_accessor.h"

#include <wow64apiset.h>

namespace {

// 用进程句柄探测目标进程的 CPU 架构。
// 优先 IsWow64Process2（一次返回目标与宿主机器类型）；返回值不可信/失败时
// 逐级回退 IsWow64Process → 宿主位数猜测。绝不让调用方拿到 unknown 死胡同。
process_arch detect_arch(HANDLE h)
{
    USHORT process_machine = 0, native_machine = 0;
    if (IsWow64Process2(h, &process_machine, &native_machine) &&
        process_machine != IMAGE_FILE_MACHINE_UNKNOWN) {
        switch (process_machine) {
        case IMAGE_FILE_MACHINE_AMD64:
        case IMAGE_FILE_MACHINE_ARM64:
            return process_arch::x86_64;
        case IMAGE_FILE_MACHINE_I386:
        case IMAGE_FILE_MACHINE_ARM:
            return process_arch::x86_32;
        default:
            break;   // 未识别的机器类型 → 走下面的回退
        }
    }

    // 回退 1：IsWow64Process。宿主 64 位时 wow=true 说明目标是 32 位。
    BOOL wow = FALSE;
    if (IsWow64Process(h, &wow)) {
#if defined(_M_X64) || defined(__x86_64__)
        return wow ? process_arch::x86_32 : process_arch::x86_64;
#else
        return wow ? process_arch::x86_64 : process_arch::x86_32;
#endif
    }

    // 回退 2：按宿主位数猜测（查询全失败时的最后兜底）
#if defined(_M_X64) || defined(__x86_64__)
    return process_arch::x86_64;
#else
    return process_arch::x86_32;
#endif
}

} // namespace

bool Win32MemoryAccessor::attach(uint32_t pid)
{
    detach();
    h_process_ = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE |
                             PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION |
                             SYNCHRONIZE, FALSE, pid);
    if (!h_process_)
        return false;
    arch_ = detect_arch(h_process_);
    return true;
}

void Win32MemoryAccessor::detach()
{
    if (h_process_) {
        CloseHandle(h_process_);
        h_process_ = nullptr;
    }
    arch_ = process_arch::unknown;
}

bool Win32MemoryAccessor::read(uint64_t addr, void* buffer, size_t size)
{
    if (!h_process_)
        return false;
    SIZE_T bytes_read = 0;
    if (!ReadProcessMemory(h_process_, reinterpret_cast<LPCVOID>(addr),
                           buffer, size, &bytes_read))
        return false;
    return bytes_read == size;   // 部分读取视为失败
}

bool Win32MemoryAccessor::write(uint64_t addr, const void* buffer, size_t size)
{
    if (!h_process_)
        return false;
    SIZE_T bytes_written = 0;
    if (WriteProcessMemory(h_process_, reinterpret_cast<LPVOID>(addr),
                           buffer, size, &bytes_written) &&
        bytes_written == size)
        return true;

    // 写失败：目标页可能是只读/写拷贝/守护页。临时把保护改为可写，
    // 写入后再恢复原保护，兼容只读映射区（某些地址直接写会被系统拒绝）。
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQueryEx(h_process_, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)))
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;

    const DWORD original = mbi.Protect;
    DWORD new_protect = PAGE_READWRITE;
    // 可执行页保留执行权限，避免改保护后指令区不可执行。
    if (original & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
        new_protect = PAGE_EXECUTE_READWRITE;

    DWORD old_protect = 0;
    if (!VirtualProtectEx(h_process_, mbi.BaseAddress, mbi.RegionSize, new_protect, &old_protect))
        return false;

    bytes_written = 0;
    const bool ok = WriteProcessMemory(h_process_, reinterpret_cast<LPVOID>(addr),
                                       buffer, size, &bytes_written) &&
                    bytes_written == size;

    DWORD unused = 0;
    VirtualProtectEx(h_process_, mbi.BaseAddress, mbi.RegionSize, original, &unused);
    return ok;
}

bool Win32MemoryAccessor::is_process_alive() const
{
    if (!h_process_)
        return false;
    return WaitForSingleObject(h_process_, 0) == WAIT_TIMEOUT;
}

std::string Win32MemoryAccessor::name() const
{
    return "Win32 API";
}

process_arch Win32MemoryAccessor::architecture() const
{
    return arch_;
}

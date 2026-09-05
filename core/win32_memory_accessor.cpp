#include "core/win32_memory_accessor.h"

bool Win32MemoryAccessor::attach(uint32_t pid)
{
    detach();
    h_process_ = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE |
                             PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION |
                             SYNCHRONIZE, FALSE, pid);
    return h_process_ != nullptr;
}

void Win32MemoryAccessor::detach()
{
    if (h_process_) {
        CloseHandle(h_process_);
        h_process_ = nullptr;
    }
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
    if (!WriteProcessMemory(h_process_, reinterpret_cast<LPVOID>(addr),
                            buffer, size, &bytes_written))
        return false;
    return bytes_written == size;
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

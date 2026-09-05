#include "core/win32_module_enumerator.h"

#include <windows.h>
#include <tlhelp32.h>

#include "core/string_conversion.h"

std::vector<module_info> Win32ModuleEnumerator::enumerate(uint32_t pid)
{
    std::vector<module_info> modules;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE)
        return modules;

    MODULEENTRY32W entry;
    entry.dwSize = sizeof(entry);

    if (Module32FirstW(snapshot, &entry)) {
        do {
            module_info info;
            info.name = wstring_to_utf8(entry.szModule);
            info.base = reinterpret_cast<uint64_t>(entry.modBaseAddr);
            info.size = entry.modBaseSize;
            modules.push_back(std::move(info));
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return modules;
}

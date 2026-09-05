#include "core/win32_process_enumerator.h"

#include <windows.h>
#include <tlhelp32.h>

std::vector<process_info> Win32ProcessEnumerator::enumerate()
{
    std::vector<process_info> result;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return result;

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            // 宽字符进程名 -> UTF-8 窄字符
            int len = WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1,
                                          nullptr, 0, nullptr, nullptr);
            std::string name;
            if (len > 0) {
                name.resize(len);   // 预留 null 终止符空间
                WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1,
                                    name.data(), len, nullptr, nullptr);
                name.resize(len - 1);   // 去掉末尾 null
            }

            process_info info;
            info.pid          = entry.th32ProcessID;
            info.ppid         = entry.th32ParentProcessID;
            info.name         = std::move(name);
            info.thread_count = entry.cntThreads;
            result.push_back(std::move(info));
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return result;
}

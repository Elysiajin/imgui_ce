#include "core/win32_process_enumerator.h"

#include <windows.h>
#include <tlhelp32.h>

#include <memory>

namespace {

// 查询进程可执行文件完整路径；失败返回空。
// 权限不足（如系统进程）时先用受限权限尝试，再回退到仅查询信息。
std::wstring query_image_path(uint32_t pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h)
        return {};

    std::wstring path;
    DWORD size = MAX_PATH;
    path.resize(size);
    if (!QueryFullProcessImageNameW(h, 0, path.data(), &size)) {
        CloseHandle(h);
        return {};
    }
    path.resize(size);

    CloseHandle(h);
    return path;
}

} // namespace

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
            info.image_path   = query_image_path(entry.th32ProcessID);
            result.push_back(std::move(info));
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return result;
}

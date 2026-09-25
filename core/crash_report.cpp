#include "crash_report.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <string>

namespace {

// exe 同目录的 crash_log.txt（追加）
FILE* open_log()
{
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH))
        return nullptr;
    std::wstring dir(path);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        dir.resize(slash + 1);
    dir += L"crash_log.txt";
    return _wfopen(dir.c_str(), L"a");
}

void write_time(FILE* f)
{
    const time_t t = time(nullptr);
    const tm*  lt = localtime(&t);
    char buf[64] = {};
    if (lt)
        strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", lt);
    fprintf(f, "==== %s ====\n", buf);
}

// 主模块基址（崩溃栈几乎都落在 exe 本体）
uintptr_t exe_base()
{
    static const uintptr_t base = [] {
        HMODULE h = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           nullptr, &h);
        return reinterpret_cast<uintptr_t>(h);
    }();
    return base;
}

// MinGW x64 的默认链接基址：addr2line -e exe 需要链接时 VA。
// 开 ASLR 后运行时地址 ≠ 链接 VA，故记录模块内偏移，
// 用 va = kDefaultImageBase + off 换算后喂给 addr2line。
constexpr uintptr_t kDefaultImageBase = 0x140000000;

void write_stack(FILE* f)
{
    void*         frames[64] = {};
    const USHORT  n = RtlCaptureStackBackTrace(0, 64, frames, nullptr);
    const uintptr_t base = exe_base();
    for (USHORT i = 0; i < n; ++i) {
        const uintptr_t pc  = reinterpret_cast<uintptr_t>(frames[i]);
        const uintptr_t off = pc >= base ? pc - base : 0;
        fprintf(f, "  #%-2d off 0x%llX  (addr2line va 0x%llX)\n",
                (int)i,
                (unsigned long long)off,
                (unsigned long long)(kDefaultImageBase + off));
    }
}

LONG WINAPI seh_filter(EXCEPTION_POINTERS* ep)
{
    if (FILE* f = open_log()) {
        write_time(f);
        fprintf(f, "SEH exception 0x%08lX at %p\n",
                (unsigned long)ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress);
        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2) {
            fprintf(f, "  %s address 0x%p\n",
                    ep->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read from",
                    (void*)(uintptr_t)ep->ExceptionRecord->ExceptionInformation[1]);
        }
        fprintf(f, "stack:\n");
        write_stack(f);
        fclose(f);
    }
    // 让进程按默认方式终止（不进入 WER 调试循环）
    return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] void on_terminate()
{
    if (FILE* f = open_log()) {
        write_time(f);
        fprintf(f, "std::terminate called\n");
        if (auto eptr = std::current_exception()) {
            try {
                std::rethrow_exception(eptr);
            } catch (const std::exception& e) {
                fprintf(f, "  exception: %s\n", e.what());
            } catch (...) {
                fprintf(f, "  exception: (non-std)\n");
            }
        } else {
            fprintf(f, "  (无活动异常 —— 可能是 assert/abort 路径)\n");
        }
        fprintf(f, "stack:\n");
        write_stack(f);
        fclose(f);
    }
    std::abort();
}

} // namespace

namespace crash_report
{
    void install()
    {
        SetUnhandledExceptionFilter(seh_filter);
        std::set_terminate(on_terminate);
    }
}

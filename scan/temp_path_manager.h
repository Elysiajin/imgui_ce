#pragma once
#include <string>
#include <filesystem>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#endif

class temp_path_manager {
public:
    // 自定义缓存根目录（可由设置页配置）；为空则用默认 %TEMP%/MyScanApp_Data/<pid>
    static inline void set_base_dir(const std::string& dir) { s_base_dir = dir; }
    static inline std::string get_base_dir() { return s_base_dir; }

    static inline std::string get_work_dir() {
        static std::string path = "";
        if (path.empty()) {
            std::filesystem::path temp_base;
            if (!s_base_dir.empty()) {
                temp_base = std::filesystem::path(s_base_dir);
            } else {
                temp_base = std::filesystem::temp_directory_path() / "MyScanApp_Data";
            }
            auto pid_dir = temp_base / std::to_string(get_current_pid());

            std::error_code ec;
            std::filesystem::create_directories(pid_dir, ec);
            path = pid_dir.string();
        }
        return path;
    }

    static inline void cleanup_orphaned_dirs() {
        std::filesystem::path temp_base;
        if (!s_base_dir.empty()) {
            temp_base = std::filesystem::path(s_base_dir);
        } else {
            temp_base = std::filesystem::temp_directory_path() / "MyScanApp_Data";
        }
        if (!std::filesystem::exists(temp_base)) return;

        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(temp_base, ec)) {
            if (entry.is_directory()) {
                try {
                    uint32_t pid = std::stoul(entry.path().filename().string());
                    if (!is_process_running(pid)) {
                        std::filesystem::remove_all(entry.path(), ec);
                    }
                }
                catch (...) {
                }
            }
        }
    }

    // 清理缓存：删除所有已退出进程的残留目录，并清理本进程遗留的临时文件。
    static inline void cleanup() {
        cleanup_orphaned_dirs();

        // 清理本进程目录下的遗留临时文件（snapshot / ACache 残留）
        auto work_dir = get_work_dir();
        std::error_code ec;
        if (std::filesystem::exists(work_dir)) {
            for (const auto& entry : std::filesystem::directory_iterator(work_dir, ec)) {
                if (entry.is_regular_file()) {
                    std::filesystem::remove(entry.path(), ec);
                }
            }
        }
    }

private:
    static inline std::string s_base_dir;   // 自定义缓存根目录

    static inline uint32_t get_current_pid() {
#ifdef _WIN32
        return GetCurrentProcessId();
#else
        return getpid();
#endif
    }

    static inline bool is_process_running(uint32_t pid) {
#ifdef _WIN32
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process) {
            CloseHandle(process);
            return true;
        }
        return false;
#else
        return kill(pid, 0) == 0;
#endif
    }
};

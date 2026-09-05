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
    static inline std::string get_work_dir() {
        static std::string path = "";
        if (path.empty()) {
            auto temp_base = std::filesystem::temp_directory_path() / "MyScanApp_Data";
            auto pid_dir = temp_base / std::to_string(get_current_pid());

            std::error_code ec;
            std::filesystem::create_directories(pid_dir, ec);
            path = pid_dir.string();
        }
        return path;
    }

    static inline void cleanup_orphaned_dirs() {
        auto temp_base = std::filesystem::temp_directory_path() / "MyScanApp_Data";
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

private:
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

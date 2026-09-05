#pragma once

// =============================================================================
// 线程池：用 Barak Shoshany 的 BS::thread_pool 替换本工程旧的简易自定义实现。
// 仅保留 global_thread_pool::instance().enqueue(...) 这套接口供 scan_engine 使用，
// 使 scan_engine.cpp 无需任何改动即可换用更成熟、单头文件、MIT 许可的线程池。
// =============================================================================

#include "BS_thread_pool.hpp"

#include <future>
#include <thread>
#include <type_traits>

class thread_pool {
public:
    explicit thread_pool(size_t threads) : pool_(threads) {}

    // 提交任务并返回 std::future<R>
    template<class F>
    auto enqueue(F&& f) -> std::future<std::invoke_result_t<F>> {
        return pool_.submit_task(std::forward<F>(f));
    }

private:
    BS::thread_pool<BS::tp::none> pool_;
};

// 全局单例
class global_thread_pool {
public:
    static thread_pool& instance() {
#ifndef _DEBUG
        static thread_pool pool(std::thread::hardware_concurrency());
#else
        static thread_pool pool(1);
#endif
        return pool;
    }
};

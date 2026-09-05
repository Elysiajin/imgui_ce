#pragma once

// 轻量级信号槽（拟合 Qt 的 signal/slot 心智，但无依赖、纯头文件）。
//
// 设计要点：
//  - signal<Args...>::connect(接收方 shared_ptr, 槽函数) 返回 connection 句柄。
//  - 接收方以 std::weak_ptr 记录；接收者销毁后连接被跳过，杜绝悬垂回调。
//  - emit 逐连接同步调用；connect 时若接收方已过期则跳过。
//  - post_to_main(executor)：把任务压入线程安全队列，由主线程每帧 drain()
//    后执行。等价 Qt 的 QueuedConnection，用于后台线程 -> UI 线程的异步通知，
//    避免直接跨线程调用造成竞态。
//
// 用法示例：
//   zc::signal<int> s;
//   std::shared_ptr<MyWidget> w = std::make_shared<MyWidget>();
//   zc::connection c = s.connect(w, [w](int v){ w->setValue(v); });
//   s.emit(42);        // 同步触发
//   w.reset();         // 之后 emit 不再触发该槽

#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

namespace zc {

class connection {
public:
    explicit connection(std::weak_ptr<void> owner) : owner_(std::move(owner)) {}

    bool alive() const { return !owner_.expired(); }

private:
    std::weak_ptr<void> owner_;
};

namespace detail {

// 每个连接的槽 = 一份可调用对象 + 目标接收者的弱引用（判断生死）。
class slot_entry {
public:
    // factory 接收一个 std::weak_ptr<void>，返回一个可执行体。
    explicit slot_entry(bool alive, std::function<void()> run)
        : alive_(alive), run_(std::move(run)) {}

    bool alive() const { return alive_; }
    void run() { if (alive_) run_(); }

private:
    bool alive_;
    std::function<void()> run_;
};

// 主线程事件队列（post_to_main 用）。
struct main_queue {
    static main_queue& instance() {
        static main_queue q;
        return q;
    }

    void post(std::function<void()> job) {
        std::lock_guard<std::mutex> lk(mtx_);
        jobs_.push(std::move(job));
    }

    void drain() {
        std::queue<std::function<void()>> local;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            local.swap(jobs_);
        }
        while (!local.empty()) {
            auto job = std::move(local.front());
            local.pop();
            job();
        }
    }

private:
    std::mutex mtx_;
    std::queue<std::function<void()>> jobs_;
};

} // namespace detail

// 主循环入口：每帧调用一次，把后台线程 post 的任务在主线程上执行。
// 通常放在 ImGui::NewFrame() 之后、渲染控件之前。
inline void drain_main_queue() {
    detail::main_queue::instance().drain();
}

// 后台线程调用：把 job 调度到主线程队列，主线程下帧 drain 时执行。
inline void post_to_main(std::function<void()> job) {
    detail::main_queue::instance().post(std::move(job));
}

// 一个带弱引用的槽：存一份可执行体和接收者的弱引用，接收者销毁则跳过。
// Args 在实例化时固定，invoke 里按参数调用。
namespace detail {
template <typename... Args>
struct weak_slot {
    std::weak_ptr<void> owner;
    std::function<void(Args...)> fn;
    bool has_owner;   // 无接收者的独立槽始终存活

    bool alive() const { return !has_owner || !owner.expired(); }
    void invoke(Args... args) const {
        if (has_owner && owner.expired()) return;
        fn(std::forward<Args>(args)...);
    }
};
} // namespace detail

template <typename... Args>
class signal {
public:
    using fn_type = std::function<void(Args...)>;

    // 接收方以 shared_ptr 传入；内部只保存弱引用。
    template <typename Owner>
    connection connect(std::shared_ptr<Owner> owner, fn_type fn) {
        return connect_raw(std::weak_ptr<void>(owner), std::move(fn));
    }

    // 无接收者（独立槽）：不检查生命周期，直接调用。
    connection connect(fn_type fn) {
        std::lock_guard<std::mutex> lk(mtx_);
        slots_.push_back({std::weak_ptr<void>{}, std::move(fn), false});
        return connection(std::weak_ptr<void>{});
    }

    void disconnect_all() {
        std::lock_guard<std::mutex> lk(mtx_);
        slots_.clear();
    }

    // 同步发出：对所有仍存活的槽逐一调用。
    // 先快照再调用，避免槽函数里再次 emit（重入）导致死锁。
    void emit(Args... args) {
        std::vector<detail::weak_slot<Args...>> snapshot;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            snapshot = slots_;
        }
        for (const auto& s : snapshot)
            if (s.alive()) s.invoke(args...);
    }

private:
    connection connect_raw(std::weak_ptr<void> owner, fn_type fn) {
        std::lock_guard<std::mutex> lk(mtx_);
        slots_.push_back({owner, std::move(fn), true});
        return connection(owner);
    }

    std::mutex mtx_;
    std::vector<detail::weak_slot<Args...>> slots_;
};

} // namespace zc

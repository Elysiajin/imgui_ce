#pragma once

#include "ui/address_list_panel.h"
#include "core/event/signal.h"
#include "ui/ui_state.h"

#include <memory>

// 组合根（Composition Root）。
//
// ImGui 没有 Qt 的信号槽，模块间通讯此前靠 文件级 static + 每帧轮询 +
// 各处直接抓单例，导致耦合散乱。AppContext 充当应用的"上下文根"，持有所有
// 需要跨模块共享的长生命周期对象与信号，供各面板按引用注入：
//
//   - address_list      下方地址列表（result_panel "Add to address list" 写入）
//   - selected_result   result_panel 当前选中行（替代文件级 static）
//   - scan_started / scan_finished   扫描生命周期通知（替代每帧轮询）
//
// 各面板持有的是 application_context& 引用，由 main.cpp 统一创建并传入，
// 不再各自堆 static / 直接抓单例。信号采用 zc::signal，接收方以 shared_ptr
// 传入，内部只保存弱引用，接收者销毁自动断开。
class application_context {
public:
    static application_context& instance() {
        static application_context ctx;
        return ctx;
    }

    application_context(const application_context&) = delete;
    application_context& operator=(const application_context&) = delete;

    // ---- 共享长生命周期对象 ----
    address_list_panel  address_list;
    int                 selected_result_index = -1;

    // ---- 跨模块信号 ----
    // scan_finished 在后台扫描线程结束时，经 post_to_main 调度到主线程触发，
    // 订阅方在主循环 drain() 时收到，安全更新 UI。
    zc::signal<> scan_started;
    zc::signal<> scan_finished;

    // 请求打开内存浏览器并跳转到指定视图/地址。
    // 由 main.cpp 订阅，统一改 ui_state（面板不直接访问 ui_state）。
    zc::signal<memory_viewer_mode, uint64_t> open_memory_viewer;

private:
    application_context() = default;
};

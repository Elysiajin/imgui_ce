// scan_service.cpp
#include "scan\scan_service.h"
#include "core\process_manager.h"
#include "core\event\signal.h"
#include "ui\app_context.h"
#include <filesystem>

scan_service& scan_service::instance()
{
    static scan_service svc;
    return svc;
}

scan_service::scan_service()
    : m_process_snapshot_manager(std::make_shared<process_memory_snapshot_manager>()),
      m_data_provider(std::make_unique<scan_data_provider>(m_process_snapshot_manager.get(), scan_data_type::int32)),
      m_engine(std::make_unique<scan_engine>(m_process_snapshot_manager.get())),
      m_repository(std::make_unique<scan_result_repository>())
{
}

scan_service::~scan_service()
{
    cancel();
    if (m_worker.joinable())
        m_worker.join();
}

void scan_service::start_scan(const scan_request& request)
{
    if (m_scanning.exchange(true))
        return;

    if (m_worker.joinable())
        m_worker.join();   // 等待上一轮任务完全退出

    // UI 线程准备段。顺序至关重要：
    //   1) 先取出上一轮结果作为再次扫描的输入（get_results）
    //   2) 再 save_as_previous_results 把结果移出仓库（供"撤销扫描"）
    // 若反过来（先移出再取），再次扫描拿到的是空结果集，所有需要与上轮
    // 比较的条件（变化/未变化/增加/减少…）全部得 0。
    // 解码在 UI 线程完成；大结果集的 decode_all 是紧凑遍历，秒级内可接受，
    // 换来的正确性优先。异常不允许逃出 UI 线程的调用栈。
    std::vector<scan_result> current_results;
    try {
        m_expect_empty_results = (request.mode == scan_mode::first &&
                                request.first_type == scan_type::unknown_initial);

        if (request.mode == scan_mode::next)
            current_results = m_repository->get_results();   // 必须在 save_as_previous 之前

        if (m_repository && m_repository->get_result_count() > 0) {
            m_repository->save_as_previous_results();
        }

        if (request.mode == scan_mode::first && request.first_type == scan_type::unknown_initial) {
            m_expect_empty_results = true;
        }
    } catch (const std::bad_alloc&) {
        m_scanning.store(false, std::memory_order_release);
        application_context::instance().scan_failed.emit("扫描启动失败：内存不足（上一轮结果过大）");
        return;
    } catch (const std::exception& e) {
        m_scanning.store(false, std::memory_order_release);
        application_context::instance().scan_failed.emit(
            std::string("扫描启动失败: ") + e.what());
        return;
    }

    m_scanning.store(true, std::memory_order_release);
    application_context::instance().scan_started.emit();
    m_worker = std::thread([this, request, current_results = std::move(current_results)]() {
        // 任何异常都不允许逃出线程（会 std::terminate 崩掉整个进程）：
        // 大结果集的 解码/排序/入库 都可能 bad_alloc，一律转成错误上报 UI。
        std::string error;
        try {
            scan_engine::scan_report pack = m_engine->execute(request, current_results);
            // 用户点了撤销：跳过入库，尽快结束线程让 cancel() 的 join() 返回。
            if (!m_engine->is_cancelled() && pack.results && pack.results->total_size() > 0) {
                std::string pack_err;
                if (!m_repository->replace_all_results_from_pool(pack.results, &pack_err))
                    error = pack_err;
            }
            // 更新结果显示类型
            if (m_data_provider)
                m_data_provider->set_display_type(pack.data_type);
            if (error.empty())
                m_scan_finished.store(true, std::memory_order_release);
        } catch (const std::bad_alloc&) {
            error = "扫描失败：内存不足（结果集或内存快照过大）";
        } catch (const std::exception& e) {
            error = std::string("扫描失败: ") + e.what();
        } catch (...) {
            error = "扫描失败：未知异常";
        }
        m_scanning.store(false, std::memory_order_release);
        if (!error.empty()) {
            zc::post_to_main([error] {
                application_context::instance().scan_failed.emit(error);
            });
        } else {
            // 通知订阅方：跨线程，调度到主线程队列，主循环 drain() 时安全触发。
            zc::post_to_main([] {
                application_context::instance().scan_finished.emit();
            });
        }
    });
}

void scan_service::cancel()
{
    if (m_engine) m_engine->cancel();
    if (m_worker.joinable())
        m_worker.join();
    m_scanning.store(false, std::memory_order_release);
}

bool scan_service::has_results() const
{
    if (m_expect_empty_results.load())
        return true;
    return m_repository && m_repository->get_result_count() > 0;
}

int scan_service::total_results() const
{
    return static_cast<int>(m_repository->get_result_count());
}

float scan_service::progress() const
{
    if (!m_engine) return 0.0f;
    int total = m_engine->total_items();
    if (total <= 0) return 0.0f;
    float r = static_cast<float>(m_engine->progress()) / static_cast<float>(total);
    return r < 1.0f ? r : 1.0f;
}

bool scan_service::has_previous_results() const
{
    return m_repository && m_repository->has_previous_results();
}

bool scan_service::restore_previous_results()
{
    if (!m_repository) return false;
    return m_repository->swap_with_previous();
}

void scan_service::clear()
{
    cancel();

    m_repository->clear();
    m_repository->clear_previous_results();

    if (m_process_snapshot_manager) {
        m_process_snapshot_manager->clear();
    }

    m_engine->clear();
    m_expect_empty_results = false;
}

void scan_service::reset()
{
    clear();
    m_engine->cancel();
    m_scanning = false;
}

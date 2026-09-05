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

    m_expect_empty_results = (request.mode == scan_mode::first &&
                            request.first_type == scan_type::unknown_initial);

    std::vector<scan_result> current_results;
    if (request.mode == scan_mode::next) {
        current_results = m_repository->get_results();
    }

    if (m_repository && m_repository->get_result_count() > 0) {
        m_repository->save_as_previous_results();
    }

    if (request.mode == scan_mode::first && request.first_type == scan_type::unknown_initial) {
        m_expect_empty_results = true;
    }

    m_scanning.store(true, std::memory_order_release);
    application_context::instance().scan_started.emit();
    m_worker = std::thread([this, request, current_results = std::move(current_results)]() {
        //   写回 repository
        scan_engine::scan_report pack = m_engine->execute(request, current_results);
        if (pack.results && pack.results->total_size() > 0) {
            m_repository->replace_all_results_from_pool(pack.results);
        }
        // 更新结果显示类型
        if (m_data_provider)
            m_data_provider->set_display_type(pack.data_type);
        m_scan_finished.store(true, std::memory_order_release);
        m_scanning.store(false, std::memory_order_release);
        // 通知订阅方：跨线程，调度到主线程队列，主循环 drain() 时安全触发。
        zc::post_to_main([] {
            application_context::instance().scan_finished.emit();
        });
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

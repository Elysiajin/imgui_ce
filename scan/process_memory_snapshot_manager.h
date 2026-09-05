#pragma once
#include "scan/iprocess_memory_snapshot.h"
#include "type/memory_region.h"
#include <memory>
#include <vector>

class process_memory_snapshot_manager {
public:
    explicit process_memory_snapshot_manager() = default;

    std::shared_ptr<i_process_memory_snapshot> create_snapshot(const std::vector<memory_region>& regions);

    void set_first_snapshot(std::shared_ptr<i_process_memory_snapshot> snapshot) { m_first = snapshot; }
    void set_previous_snapshot(std::shared_ptr<i_process_memory_snapshot> snapshot) { m_prev = snapshot; }

    std::shared_ptr<i_process_memory_snapshot> get_first_process_memory_snapshot() const { return m_first; }
    std::shared_ptr<i_process_memory_snapshot> get_previous_process_memory_snapshot() const { return m_prev; }

    void clear();

private:
    std::shared_ptr<i_process_memory_snapshot> m_first = nullptr;
    std::shared_ptr<i_process_memory_snapshot> m_prev = nullptr;
};

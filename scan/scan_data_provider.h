// scan_data_provider.h
#pragma once
#include "scan/iscan_value_provider.h"
#include "scan/process_memory_snapshot_manager.h"
#include <memory>
#include <string>
#include <functional>

class scan_data_provider : public i_scan_value_provider {
public:
    scan_data_provider(process_memory_snapshot_manager* process_snapshot_manager,
        scan_data_type display_type);

    void set_display_type(scan_data_type type) { m_display_type = type; }
    scan_data_type get_display_type() const { return m_display_type; }

    std::string get_current_value(uint64_t address, scan_data_type type) const override;
    std::string get_previous_value(uint64_t address, scan_data_type type) const override;
    std::string get_first_value(uint64_t address, scan_data_type type) const override;
    std::string get_address_display(uint64_t address) const override;
    bool is_module_base(uint64_t address) const override;

    void set_hex_display(bool on) override { m_hex_display = on; }
    bool is_hex_display() const override { return m_hex_display; }

private:
    template<typename ReaderFunc>
    std::string read_and_format_generic(uint64_t address, scan_data_type type, ReaderFunc&& read_fn) const;

    std::string read_value_from_snapshot(uint64_t address, scan_data_type type, const std::shared_ptr<i_process_memory_snapshot>& snapshot) const;

    process_memory_snapshot_manager* m_process_snapshot_manager;
    scan_data_type m_display_type;
    bool m_hex_display = false;
};

#pragma once
#include <string>
#include "type/scan_data_stream_define.h"

class i_scan_value_provider {
public:
    virtual ~i_scan_value_provider() = default;

    virtual std::string get_current_value(uint64_t address, scan_data_type type) const = 0;
    virtual std::string get_previous_value(uint64_t address, scan_data_type type) const = 0;
    virtual std::string get_first_value(uint64_t address, scan_data_type type) const = 0;
    virtual std::string get_address_display(uint64_t address) const = 0;
    virtual bool is_module_base(uint64_t address) const = 0;

    virtual void set_hex_display(bool on) = 0;
    virtual bool is_hex_display() const = 0;
};

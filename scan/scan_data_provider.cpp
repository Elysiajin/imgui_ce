#include "scan\scan_data_provider.h"
#include "scan\encoding_formatter.h"
#include "core\process_manager.h"

scan_data_provider::scan_data_provider(process_memory_snapshot_manager* process_snapshot_manager, scan_data_type type)
    : m_process_snapshot_manager(process_snapshot_manager), m_display_type(type) {}

bool scan_data_provider::is_module_base(uint64_t address) const {
    std::string dummy; bool is_base = false;
    process_manager::instance().resolve_address(address, dummy, is_base);
    return is_base;
}

template<typename ReaderFunc>
std::string scan_data_provider::read_and_format_generic(uint64_t address, scan_data_type type, ReaderFunc&& read_fn) const {
    size_t size = scan_data_type_size(type);

    if (size == 0) {
        const size_t max_read = (type == scan_data_type::byte_array) ? 32 : 64;
        std::vector<uint8_t> buf(max_read);

        if (!read_fn(address, buf.data(), buf.size()))
            return "---";

        if (is_string_type(type)) {
            if (type == scan_data_type::utf16_string) {
                const uint16_t* u16 = reinterpret_cast<const uint16_t*>(buf.data());
                size_t u16len = max_read / 2;
                size_t real_len = 0;
                while (real_len < u16len && u16[real_len] != 0) ++real_len;
                return encoding_formatter::format_utf16_string(u16, real_len);
            } else {
                std::string str(reinterpret_cast<char*>(buf.data()), strnlen(reinterpret_cast<char*>(buf.data()), max_read));
                return encoding_formatter::format_string(str, type);
            }
        }
        else if (is_byte_array_type(type)) {
            return encoding_formatter::format_byte_array(buf.data(), max_read);
        }
        return "---";
    }

    uint64_t raw = 0;
    if (!read_fn(address, reinterpret_cast<uint8_t*>(&raw), size))
        return "---";

    return encoding_formatter::format_value(raw, type, m_hex_display);
}

std::string scan_data_provider::get_current_value(uint64_t address, scan_data_type type) const {
    return read_and_format_generic(address, type, [](uint64_t addr, uint8_t* dst, size_t sz) {
        auto mem = process_manager::instance().memory();
        return mem ? mem->read(addr, dst, sz) : false;
    });
}

std::string scan_data_provider::read_value_from_snapshot(uint64_t address, scan_data_type type,
    const std::shared_ptr<i_process_memory_snapshot>& snapshot) const
{
    if (!snapshot) return "---";
    return read_and_format_generic(address, type, [&snapshot](uint64_t addr, uint8_t* dst, size_t sz) {
        return snapshot->read_data(addr, dst, sz);
    });
}

std::string scan_data_provider::get_previous_value(uint64_t address, scan_data_type type) const {
    return read_value_from_snapshot(address, type, m_process_snapshot_manager->get_previous_process_memory_snapshot());
}

std::string scan_data_provider::get_first_value(uint64_t address, scan_data_type type) const {
    return read_value_from_snapshot(address, type, m_process_snapshot_manager->get_first_process_memory_snapshot());
}

std::string scan_data_provider::get_address_display(uint64_t address) const {
    std::string display; bool is_base = false;
    process_manager::instance().resolve_address(address, display, is_base);
    return display;
}

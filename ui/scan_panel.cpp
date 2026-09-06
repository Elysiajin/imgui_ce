#include "scan_panel.h"
#include "app_context.h"
#include "core/event/signal.h"
#include "imgui.h"
#include "core/process_manager.h"
#include "scan/scan_service.h"
#include "scan/scan_value_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

scan_panel::scan_panel(ui_state& ui, application_context& ctx) : state_(ui), ctx_(ctx) {
    // 订阅扫描完成信号：由后台线程通过 post_to_main 调度到主线程触发。
    // connect 需要 std::shared_ptr 接收方，这里用无接收者版本（不检查生命周期）。
    ctx_.scan_finished.connect([this] {
        // 消费一次性标记：更新 UI 状态（首次扫描完成后切换条件列表）。
        if (scan_service::instance().scan_finished())
            scan_service::instance().consume_scan_done();
        state_.first_scan_done = true;
        state_.scan_mode = scan_mode::first;
    });
    subscribed_ = true;
}

static void load_modules_into_state(ui_state& state) {
    auto& pm = process_manager::instance();
    if (!pm.is_attached()) {
        state.modules_loaded = false;
        return;
    }
    if (state.modules_loaded && state.attached_pid == pm.attached_pid())
        return;

    state.module_infos = pm.modules().enumerate(pm.attached_pid());

    state.module_names.clear();
    state.module_names.push_back("<All Modules>");
    for (const auto& m : state.module_infos)
        state.module_names.push_back(m.name);

    state.attached_pid    = pm.attached_pid();
    state.modules_loaded  = true;
    if (state.module_selected < 0 || state.module_selected >= (int)state.module_names.size())
        state.module_selected = 0;
}

static std::string trim_text(const char* s) {
    std::string str = s ? s : "";
    size_t b = 0, e = str.size();
    while (b < e && std::isspace((unsigned char)str[b])) ++b;
    while (e > b && std::isspace((unsigned char)str[e - 1])) --e;
    return str.substr(b, e - b);
}

static bool parse_value_params(const ui_state& state, scan_request& req, std::string& err) {
    const scan_data_type dt = req.data_type;
    value_params vp;
    const std::string t1 = trim_text(state.scan_value);
    const std::string t2 = trim_text(state.scan_value2);
    const bool need_two = (req.mode == scan_mode::first) ? (req.first_type == scan_type::between)
                                                         : (req.next_type == next_scan_type::between);

    auto parse_one = [&](const std::string& text, uint64_t& out) -> bool {
        return parse_scan_value(text, dt, state.hex, out);
    };

    if (!parse_one(t1, vp.value1)) {
        err = "Invalid first value";
        return false;
    }
    if (need_two) {
        if (!parse_one(t2, vp.value2)) {
            err = "Invalid second value";
            return false;
        }
    }
    req.params = vp;
    return true;
}

static bool build_request(ui_state& state, scan_request& req, std::string& err) {
    req.mode     = state.scan_mode;
    req.data_type = state.data_type_;
    if (req.mode == scan_mode::first)
        req.first_type = state.first_scan_type_;
    else
        req.next_type = state.next_scan_type_;

    req.alignment = state.fast_scan ? std::max<size_t>(1, scan_data_type_size(req.data_type)) : 1;

    if (state.module_selected > 0 && state.module_selected <= (int)state.module_infos.size()) {
        const auto& m = state.module_infos[state.module_selected - 1];
        req.module_base = m.base;
        req.module_size = m.size;
    }

    req.mem_filter.writable    = state.writable;
    req.mem_filter.executable  = state.executable;
    req.mem_filter.copy_on_write = state.copy_on_write;

    req.not_match = state.not_match;
    req.contain_approximate_value = false;

    if (req.data_type == scan_data_type::structure) {
        err = "Structure scan not implemented yet";
        return false;
    }
    else if (is_string_type(req.data_type)) {
        string_params sp;
        sp.case_sensitive = state.case_sensitive;
        if (req.data_type == scan_data_type::utf16_string) {
            const auto* u16 = reinterpret_cast<const char16_t*>(state.scan_value);
            size_t len = std::char_traits<char16_t>::length(u16);
            sp.text.assign(reinterpret_cast<const char*>(u16), len * 2);
        } else {
            const std::string t = trim_text(state.scan_value);
            sp.text = t;
        }
        req.params = sp;
    }
    else if (is_byte_array_type(req.data_type)) {
        aob_params ap;
        const std::string t = trim_text(state.scan_value);
        const char* p = t.c_str();
        while (*p) {
            while (*p == ' ' || *p == ',') ++p;
            if (!*p) break;
            char tok[8] = {0};
            int k = 0;
            while (*p && *p != ' ' && *p != ',' && k < 7) tok[k++] = *p++;
            std::string s(tok);
            std::transform(s.begin(), s.end(), s.begin(), ::toupper);
            if (s == "??") {
                ap.pattern.push_back(0);
                ap.mask.push_back(0x00);
            } else if (s.size() == 2 && s[0] == '?') {
                uint32_t v = strtoul(s.c_str() + 1, nullptr, 16);
                ap.pattern.push_back(static_cast<uint8_t>(v & 0x0F));
                ap.mask.push_back(0x0F);
            } else if (s.size() == 2 && s[1] == '?') {
                uint32_t v = strtoul(s.c_str(), nullptr, 16);
                ap.pattern.push_back(static_cast<uint8_t>((v & 0x0F) << 4));
                ap.mask.push_back(0xF0);
            } else {
                uint32_t v = strtoul(s.c_str(), nullptr, 16);
                if (v > 0xFF) { err = "Byte value exceeds 0xFF"; return false; }
                ap.pattern.push_back(static_cast<uint8_t>(v));
                ap.mask.push_back(0xFF);
            }
        }
        if (ap.pattern.empty()) { err = "Empty byte array"; return false; }
        req.params = ap;
    }
    else {
        if (!parse_value_params(state, req, err))
            return false;
    }

    state.scan_error.clear();
    return true;
}

void scan_panel::render() {
    load_modules_into_state(state_);

    auto& svc      = scan_service::instance();
    bool  scanning = svc.is_scanning();
    bool  attached = process_manager::instance().is_attached();

    const bool can_next = attached && !scanning && svc.has_results();

    if (svc.scan_finished()) {
        svc.consume_scan_done();
        state_.first_scan_done = true;
        state_.scan_mode = scan_mode::first;
    }

    if (attached) {
        char pid_buf[64];
        snprintf(pid_buf, sizeof(pid_buf), "Target: PID %u",
                 process_manager::instance().attached_pid());
        ImGui::TextUnformatted(pid_buf);

        const char* preview = (state_.module_selected >= 0 &&
                               state_.module_selected < (int)state_.module_names.size())
                                  ? state_.module_names[state_.module_selected].c_str()
                                  : "<All Modules>";
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::BeginCombo("##module_combo", preview)) {
            for (int i = 0; i < (int)state_.module_names.size(); ++i) {
                const bool is_selected = (state_.module_selected == i);
                if (ImGui::Selectable(state_.module_names[i].c_str(), is_selected))
                    state_.module_selected = i;
                if (is_selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::Separator();
    }

    if (ImGui::Button("First Scan")) {
        if (attached && !scanning) {
            state_.scan_mode = scan_mode::first;
            scan_request req;
            if (build_request(state_, req, state_.scan_error)) {
                fprintf(stderr, "[diag] FirstScan: type=%d data_type=%d align=%zu module_base=0x%llx val1=0x%llx\n",
                        (int)req.first_type, (int)req.data_type, req.alignment,
                        (unsigned long long)req.module_base,
                        (unsigned long long)(req.params.index() == 0 ? std::get<value_params>(req.params).value1 : 0));
                svc.start_scan(req);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Next Scan")) {
        if (attached && !scanning && can_next) {
            state_.scan_mode = scan_mode::next;
            scan_request req;
            if (build_request(state_, req, state_.scan_error)) {
                svc.start_scan(req);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Undo Scan")) {
        if (!scanning && svc.has_previous_results())
            svc.restore_previous_results();
    }
    ImGui::SameLine();
    if (ImGui::Button("New Scan")) {
        if (!scanning) {
            svc.clear();
            state_.first_scan_done = false;
            state_.scan_mode = scan_mode::first;
            state_.modules_loaded = false;
        }
    }

    if (!state_.scan_error.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", state_.scan_error.c_str());

    ImGui::Spacing();

    ImGui::Checkbox("Hex", &state_.hex);
    ImGui::SameLine();
    ImGui::InputText("Scan Value", state_.scan_value, IM_ARRAYSIZE(state_.scan_value),
                     ImGuiInputTextFlags_EnterReturnsTrue);

    struct data_type_entry { scan_data_type type; const char* label; };
    static const data_type_entry k_data_types[] = {
        { scan_data_type::bit,         "Bit" },
        { scan_data_type::int8,        "Byte" },
        { scan_data_type::int16,       "2 Bytes" },
        { scan_data_type::int32,       "4 Bytes" },
        { scan_data_type::int64,       "8 Bytes" },
        { scan_data_type::float32,     "Float" },
        { scan_data_type::float64,     "Double" },
        { scan_data_type::ascii_string, "String (Ascii)" },
        { scan_data_type::utf8_string,  "String (UTF-8)" },
        { scan_data_type::utf16_string, "String (UTF-16)" },
        { scan_data_type::byte_array,   "Array of Byte" },
        { scan_data_type::all,         "All" },
        { scan_data_type::structure,   "Structure" },
    };
    const char* dt_labels[IM_ARRAYSIZE(k_data_types)];
    for (int i = 0; i < IM_ARRAYSIZE(k_data_types); ++i) dt_labels[i] = k_data_types[i].label;

    int dt_idx = 0;
    for (int i = 0; i < IM_ARRAYSIZE(k_data_types); ++i)
        if (k_data_types[i].type == state_.data_type_) { dt_idx = i; break; }
    ImGui::Combo("Value Type", &dt_idx, dt_labels, IM_ARRAYSIZE(dt_labels));
    state_.data_type_ = k_data_types[dt_idx].type;

    const bool first_list = !state_.first_scan_done || state_.scan_mode == scan_mode::first;
    if (first_list) {
        static const struct { scan_type t; const char* label; } k_ip[] = {
            { scan_type::exact_value,     "Exact Value" },
            { scan_type::greater_than,    "Greater than" },
            { scan_type::less_than,       "Less than" },
            { scan_type::between,        "Between" },
            { scan_type::unknown_initial, "Unknown initial value" },
        };
        const char* labels[IM_ARRAYSIZE(k_ip)];
        for (int i = 0; i < IM_ARRAYSIZE(k_ip); ++i) labels[i] = k_ip[i].label;
        int idx = 0;
        for (int i = 0; i < IM_ARRAYSIZE(k_ip); ++i)
            if (k_ip[i].t == state_.first_scan_type_) { idx = i; break; }
        ImGui::Combo("Scan Type", &idx, labels, IM_ARRAYSIZE(k_ip));
        state_.first_scan_type_ = k_ip[idx].t;
    } else {
        static const struct { next_scan_type t; const char* label; } k_np[] = {
            { next_scan_type::equal,    "Exact Value" },
            { next_scan_type::changed,  "Changed value" },
            { next_scan_type::unchanged,"Unchanged value" },
            { next_scan_type::increased,"Increased value" },
            { next_scan_type::decreased,"Decreased value" },
            { next_scan_type::increased_by, "Increased by" },
            { next_scan_type::decreased_by, "Decreased by" },
            { next_scan_type::between,  "Between" },
            { next_scan_type::compare_to_first_scan, "Compare to first scan" },
        };
        const char* labels[IM_ARRAYSIZE(k_np)];
        for (int i = 0; i < IM_ARRAYSIZE(k_np); ++i) labels[i] = k_np[i].label;
        int idx = 0;
        for (int i = 0; i < IM_ARRAYSIZE(k_np); ++i)
            if (k_np[i].t == state_.next_scan_type_) { idx = i; break; }
        ImGui::Combo("Scan Type", &idx, labels, IM_ARRAYSIZE(k_np));
        state_.next_scan_type_ = k_np[idx].t;
    }

    const bool need_two = first_list ? (state_.first_scan_type_ == scan_type::between)
                                     : (state_.next_scan_type_ == next_scan_type::between);
    if (need_two)
        ImGui::InputText("Scan Value 2", state_.scan_value2, IM_ARRAYSIZE(state_.scan_value2));

    if (is_string_type(state_.data_type_)) {
        ImGui::Checkbox("Case sensitive", &state_.case_sensitive);
    }

    if (ImGui::CollapsingHeader("Memory Scan Options", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("writable",           &state_.writable);
        ImGui::Checkbox("executable",         &state_.executable);
        ImGui::Checkbox("Copy on write",      &state_.copy_on_write);
        ImGui::Checkbox("Fast Scan",          &state_.fast_scan);
        if (first_list)
            ImGui::Checkbox("Not match",      &state_.not_match);
    }
}

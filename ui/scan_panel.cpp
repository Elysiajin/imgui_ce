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
    //   扫描完成后切到"再次扫描"语义（CE 的 First Scan → Next Scan）：
    //   条件列表随之换成 Increased/Decreased/Changed/Unchanged 那一套。
    ctx_.scan_finished.connect([this] {
        // 消费一次性标记：更新 UI 状态（首次扫描完成后切换条件列表）。
        if (scan_service::instance().scan_finished())
            scan_service::instance().consume_scan_done();
        state_.first_scan_done = true;
        state_.scan_mode = scan_mode::next;
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
    state.module_names.push_back("<全部模块>");
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

    // ★ 扫描区域类型：任一类型勾选即只扫该类型；全不勾 = 扫描全部类型（CE 行为）
    req.mem_filter.type_filter = 0;
    if (state.scan_private) req.mem_filter.type_filter |= memory_filter::type_private;
    if (state.scan_image)   req.mem_filter.type_filter |= memory_filter::type_image;
    if (state.scan_mapped)  req.mem_filter.type_filter |= memory_filter::type_mapped;
    if (req.mem_filter.type_filter == 0)
        req.mem_filter.type_filter = memory_filter::type_private | memory_filter::type_image | memory_filter::type_mapped;

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
        // ★ 之前这里把 scan_mode 复位成 first，导致"Scan Type"永远停在首次扫描的
        //   5 个选项上，再次扫描的 未变动/增加/减少 等值类型根本选不到。
        //   CE 的行为就是：首次扫描结束后按钮语义变成 Next Scan，条件列表同步切换。
        state_.scan_mode = scan_mode::next;
    }

    if (attached) {
        // 附加信息（PID / 进程名）已由主窗口顶部常驻状态进度条统一显示
        const char* preview = (state_.module_selected >= 0 &&
                               state_.module_selected < (int)state_.module_names.size())
                                  ? state_.module_names[state_.module_selected].c_str()
                                  : "<全部模块>";
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

    if (ImGui::Button("首次扫描")) {
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
    if (ImGui::Button("再次扫描")) {
        if (attached && !scanning && can_next) {
            state_.scan_mode = scan_mode::next;
            scan_request req;
            if (build_request(state_, req, state_.scan_error)) {
                svc.start_scan(req);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("撤销扫描")) {
        if (!scanning && svc.has_previous_results())
            svc.restore_previous_results();
    }
    ImGui::SameLine();
    if (ImGui::Button("新扫描")) {
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

    ImGui::Checkbox("十六进制", &state_.hex);
    ImGui::SameLine();
    ImGui::InputText("扫描值", state_.scan_value, IM_ARRAYSIZE(state_.scan_value),
                     ImGuiInputTextFlags_EnterReturnsTrue);

    struct data_type_entry { scan_data_type type; const char* label; };
    static const data_type_entry k_data_types[] = {
        { scan_data_type::bit,         "位" },
        { scan_data_type::int8,        "字节" },
        { scan_data_type::int16,       "2 字节" },
        { scan_data_type::int32,       "4 字节" },
        { scan_data_type::int64,       "8 字节" },
        { scan_data_type::float32,     "单精度浮点数" },
        { scan_data_type::float64,     "双精度浮点数" },
        { scan_data_type::ascii_string, "字符串 (ASCII)" },
        { scan_data_type::utf8_string,  "字符串 (UTF-8)" },
        { scan_data_type::utf16_string, "字符串 (UTF-16)" },
        { scan_data_type::byte_array,   "字节数组 (AOB)" },
        { scan_data_type::all,         "全部数据类型" },
        { scan_data_type::structure,   "结构体" },
    };
    const char* dt_labels[IM_ARRAYSIZE(k_data_types)];
    for (int i = 0; i < IM_ARRAYSIZE(k_data_types); ++i) dt_labels[i] = k_data_types[i].label;

    int dt_idx = 0;
    for (int i = 0; i < IM_ARRAYSIZE(k_data_types); ++i)
        if (k_data_types[i].type == state_.data_type_) { dt_idx = i; break; }
    ImGui::Combo("数值类型", &dt_idx, dt_labels, IM_ARRAYSIZE(dt_labels));
    state_.data_type_ = k_data_types[dt_idx].type;

    // ★ 只按"当前按钮语义"决定列表：First Scan → 首次条件；Next Scan → 再次条件。
    //   旧写法是 `!first_scan_done || scan_mode == first`，配合上面把 scan_mode
    //   复位成 first 的 bug，首次列表会被永久钉住。
    const bool first_list = (state_.scan_mode == scan_mode::first);
    if (first_list) {
        // CE 首次扫描的 5 个条件
        static const struct { scan_type t; const char* label; } k_ip[] = {
            { scan_type::exact_value,     "精确数值" },
            { scan_type::greater_than,    "大于" },
            { scan_type::less_than,       "小于" },
            { scan_type::between,         "介于...之间" },
            { scan_type::unknown_initial, "未知初始值" },
        };
        const char* labels[IM_ARRAYSIZE(k_ip)];
        for (int i = 0; i < IM_ARRAYSIZE(k_ip); ++i) labels[i] = k_ip[i].label;
        int idx = 0;
        for (int i = 0; i < IM_ARRAYSIZE(k_ip); ++i)
            if (k_ip[i].t == state_.first_scan_type_) { idx = i; break; }
        ImGui::Combo("扫描条件", &idx, labels, IM_ARRAYSIZE(k_ip));
        state_.first_scan_type_ = k_ip[idx].t;
    } else {
        // CE 再次扫描条件（顺序对齐 CE：Exact / Increased / Increased by /
        // Decreased / Decreased by / Changed / Unchanged / Bigger / Smaller / Between）
        static const struct { next_scan_type t; const char* label; } k_np[] = {
            { next_scan_type::equal,                 "精确数值" },
            { next_scan_type::increased,             "增加的数值" },
            { next_scan_type::increased_by,          "增加了多少" },
            { next_scan_type::decreased,             "减少的数值" },
            { next_scan_type::decreased_by,          "减少了多少" },
            { next_scan_type::changed,               "变化的数值" },
            { next_scan_type::unchanged,             "未变化的数值" },
            { next_scan_type::greater_than,          "大于" },
            { next_scan_type::less_than,             "小于" },
            { next_scan_type::between,               "介于...之间" },
            { next_scan_type::ignore_value,          "忽略数值" },
            { next_scan_type::compare_to_first_scan, "与首次扫描比较" },
        };
        const char* labels[IM_ARRAYSIZE(k_np)];
        for (int i = 0; i < IM_ARRAYSIZE(k_np); ++i) labels[i] = k_np[i].label;
        int idx = 0;
        for (int i = 0; i < IM_ARRAYSIZE(k_np); ++i)
            if (k_np[i].t == state_.next_scan_type_) { idx = i; break; }
        ImGui::Combo("扫描条件", &idx, labels, IM_ARRAYSIZE(k_np));
        state_.next_scan_type_ = k_np[idx].t;
    }

    const bool need_two = first_list ? (state_.first_scan_type_ == scan_type::between)
                                     : (state_.next_scan_type_ == next_scan_type::between);
    if (need_two)
        ImGui::InputText("扫描值 2", state_.scan_value2, IM_ARRAYSIZE(state_.scan_value2));

    if (is_string_type(state_.data_type_)) {
        ImGui::Checkbox("区分大小写", &state_.case_sensitive);
    }

    if (ImGui::CollapsingHeader("内存扫描选项", ImGuiTreeNodeFlags_DefaultOpen)) {
        // 扫描区域类型（CE 的"也扫描这些类型的内存区域"）
        ImGui::TextUnformatted("扫描的内存区域类型:");
        ImGui::Checkbox("私有内存", &state_.scan_private);
        ImGui::SameLine();
        ImGui::Checkbox("映像内存", &state_.scan_image);
        ImGui::SameLine();
        ImGui::Checkbox("映射内存", &state_.scan_mapped);
        ImGui::Checkbox("可写",       &state_.writable);
        ImGui::SameLine();
        ImGui::Checkbox("可执行",     &state_.executable);
        ImGui::SameLine();
        ImGui::Checkbox("写入时复制", &state_.copy_on_write);
        ImGui::Checkbox("快速扫描",   &state_.fast_scan);
        ImGui::SameLine();
        // 首次与再次扫描都支持"非"取反（CE 在两种扫描下都提供 Not 语义）
        ImGui::Checkbox("非(不匹配)", &state_.not_match);
    }
}

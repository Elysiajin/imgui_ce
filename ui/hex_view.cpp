#include "hex_view.h"

#include "address_value.h"
#include "app_context.h"
#include "core/process_manager.h"

#include "imgui.h"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace {

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 解析地址输入：默认按十六进制（CE 习惯），0x 前缀亦可。
bool parse_address_text(const char* s, uint64_t& out)
{
    if (!s || !*s)
        return false;
    char* end = nullptr;
    const bool has_0x = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'));
    const unsigned long long v = strtoull(s, &end, has_0x ? 0 : 16);
    if (end == s)
        return false;
    out = (uint64_t)v;
    return true;
}

// 把 vsize 字节（小端）按类型格式化为显示文本
void format_value(const uint8_t* raw, int size, bool is_int, bool hex,
                  char* out, size_t n)
{
    if (is_int) {
        uint64_t v = 0;
        std::memcpy(&v, raw, (size_t)size);
        if (hex) snprintf(out, n, "%0*llX", size * 2, (unsigned long long)v);
        else     snprintf(out, n, "%llu", (unsigned long long)v);
    } else if (size == 4) {
        float f = 0;
        std::memcpy(&f, raw, 4);
        snprintf(out, n, "%g", (double)f);
    } else {
        double d = 0;
        std::memcpy(&d, raw, 8);
        snprintf(out, n, "%g", d);
    }
}

} // namespace

// ---- 显示类型表 ----

int hex_view::type_count() { return 6; }

const hex_view::display_type_info& hex_view::type_info(int idx)
{
    static const display_type_info k_types[] = {
        { "字节",   1, true,  value_type::one_byte  },
        { "字",     2, true,  value_type::two_bytes },
        { "双字",   4, true,  value_type::four_bytes },
        { "四字",   8, true,  value_type::eight_bytes },
        { "单精度浮点", 4, false, value_type::float32 },
        { "双精度浮点", 8, false, value_type::float64 },
    };
    return k_types[idx];
}

// ---- 页缓存 ----

hex_view::page* hex_view::get_page(uint64_t page_base)
{
    auto it = pages_.find(page_base);
    if (it != pages_.end())
        return &it->second;

    if (pages_.size() >= k_max_pages)
        pages_.clear();   // 简单淘汰：整体清空

    page p;
    p.base = page_base;
    p.data.assign(k_page_size, 0);

    auto* mem = process_manager::instance().memory();
    if (mem && mem->read(page_base, p.data.data(), k_page_size)) {
        p.readable_mask = ~0ull;
    } else if (mem) {
        // 整页读取失败（可能跨内存区边界）：按 64 字节块降级读取
        for (size_t c = 0; c < k_page_size / k_chunk_size; ++c) {
            if (mem->read(page_base + c * k_chunk_size,
                          p.data.data() + c * k_chunk_size, k_chunk_size))
                p.readable_mask |= (1ull << c);
        }
    }
    return &(pages_.emplace(page_base, std::move(p)).first->second);
}

uint8_t hex_view::byte_at(uint64_t addr, bool& valid)
{
    page* pg = get_page(addr & ~(uint64_t)(k_page_size - 1));
    const size_t off = (size_t)(addr & (k_page_size - 1));
    valid = (pg->readable_mask >> (off / k_chunk_size)) & 1;
    return pg->data[off];
}

// 定时重读已缓存页，diff 出变化字节并打时间戳
void hex_view::refresh_pages()
{
    auto* mem = process_manager::instance().memory();
    if (!mem || pages_.empty())
        return;

    const int64_t now = now_ms();
    std::vector<uint8_t> tmp(k_page_size);

    for (auto& [base, pg] : pages_) {
        if (!mem->read(base, tmp.data(), k_page_size))
            continue;   // 读不到保留旧数据
        for (size_t i = 0; i < k_page_size; ++i) {
            if (tmp[i] != pg.data[i]) {
                pg.data[i] = tmp[i];
                change_ticks_[base + i] = now;
            }
        }
    }

    // 清理过期高亮
    for (auto it = change_ticks_.begin(); it != change_ticks_.end();) {
        if (now - it->second > k_fade_ms * 2)
            it = change_ticks_.erase(it);
        else
            ++it;
    }
}

void hex_view::handle_pid_change()
{
    const uint32_t pid = process_manager::instance().attached_pid();
    if (pid != cached_pid_) {
        cached_pid_ = pid;
        invalidate();
    }
}

void hex_view::invalidate()
{
    pages_.clear();
    change_ticks_.clear();
    has_selection_ = false;
    edit_open_ = false;
}

// ---- 渲染 ----

void hex_view::render()
{
    handle_pid_change();

    auto& pm = process_manager::instance();
    if (!pm.is_attached()) {
        ImGui::TextDisabled("未附加进程");
        return;
    }

    // 跳转目标变化（右键 View in dump / goto）→ 以目标为第一行
    if (state_.dump_view_address != last_target_) {
        goto_address(state_.dump_view_address);
    }

    // 实时刷新
    const int64_t now = now_ms();
    if (now - last_refresh_ms_ > k_refresh_ms) {
        last_refresh_ms_ = now;
        refresh_pages();
    }

    render_toolbar();
    render_table();
}

void hex_view::goto_address(uint64_t addr)
{
    // 跳转前压栈（CE 的 THexView.AddToBackList），供 Back 回退。
    // 地址未变时不重复压栈。
    if (view_base_ != 0 && view_base_ != (addr & ~(uint64_t)(k_bytes_per_row - 1))) {
        back_stack_.push_back(view_base_);
        if (back_stack_.size() > 512)
            back_stack_.erase(back_stack_.begin());
    }

    last_target_ = addr;
    view_base_ = addr & ~(uint64_t)(k_bytes_per_row - 1);
    selected_addr_ = addr;
    has_selection_ = true;
    edit_open_ = false;
    want_scroll_top_ = true;
}

void hex_view::go_back()
{
    if (back_stack_.empty())
        return;
    const uint64_t addr = back_stack_.back();
    back_stack_.pop_back();
    // 把外部跳转目标标记为"已消费"，避免下一帧 render() 因
    // state_.dump_view_address != last_target_ 再次触发 goto_address 覆盖回退结果。
    last_target_ = state_.dump_view_address;
    view_base_ = addr;
    selected_addr_ = addr;
    has_selection_ = true;
    edit_open_ = false;
    want_scroll_top_ = true;
}

void hex_view::render_toolbar()
{
    const auto& ti = type_info(display_type_);

    ImGui::SetNextItemWidth(90);
    if (ImGui::BeginCombo("##hv_type", ti.name)) {
        for (int i = 0; i < type_count(); ++i) {
            if (ImGui::Selectable(type_info(i).name, i == display_type_))
                display_type_ = i;
            if (i == display_type_)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (!ti.is_int)
        ImGui::BeginDisabled();
    ImGui::Checkbox("十六进制", &hex_display_);
    if (!ti.is_int)
        ImGui::EndDisabled();

    ImGui::SameLine();
    // 跳转回退（CE 的 hexview Back）：仅当有历史时可用。
    // 用局部变量固定 has_back，无条件配对 Begin/EndDisabled，避免点击后同一帧
    // 状态翻转导致 EndDisabled 多调一次而触发断言。
    const bool has_back = this->has_back();
    ImGui::BeginDisabled(!has_back);
    if (ImGui::Button("后退")) {
        go_back();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    if (ImGui::InputTextWithHint("##hv_goto", "转到 (十六进制), 回车",
                                 goto_buf_, sizeof(goto_buf_),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        uint64_t a = 0;
        if (parse_address_text(goto_buf_, a))
            goto_address(a);
    }
}

void hex_view::render_table()
{
    const auto& ti = type_info(display_type_);
    const int vsize = ti.size;
    const int value_cols = k_bytes_per_row / vsize;

    auto* mem = process_manager::instance().memory();
    const bool arch64 = mem && mem->architecture() == process_arch::x86_64;
    // const int addr_digits = arch64 ? 16 : 8;

    // 行高（CellPadding 垂直压 0 让行高 = 文本高，便于精确计算重锚步长；
    // 水平给 6px 让各列/数值格/ASCII 之间有空隙，避免太拥挤）
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.f, 0.f));
    const float row_h = ImGui::GetTextLineHeight();
    float avail_h = ImGui::GetContentRegionAvail().y;
    if (avail_h < row_h * 4.f)
        avail_h = row_h * 4.f;
    const int visible_rows = (int)(avail_h / row_h) < 4
                                 ? 4 : (int)(avail_h / row_h);
    const int page_rows = visible_rows * 3;   // 滑动窗口 = 可视行数的 3 倍

    // 列宽：比例字体下用"最宽内容样例"估宽，保证内容不被截断
    const char* addr_sample = arch64 ? "FFFFFFFFFFFFFFFF" : "FFFFFFFF";
    char value_sample[32];
    if (ti.is_int) {
        const int n = hex_display_ ? vsize * 2
                                   : (vsize == 1 ? 3 : vsize == 2 ? 5
                                       : vsize == 4 ? 10 : 20);
        for (int i = 0; i < n && i < 30; ++i) value_sample[i] = '9';
        value_sample[n] = 0;
    } else {
        snprintf(value_sample, sizeof(value_sample), "%s",
                 vsize == 4 ? "-999999.99999" : "-999999.9999999");
    }

    char tid[32];
    snprintf(tid, sizeof(tid), "##hexdump%d", display_type_);

    if (ImGui::BeginTable(tid, 2 + value_cols,
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
                          ImVec2(0.f, avail_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("地址", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize(addr_sample).x + 12.f);
        const float value_w = ImGui::CalcTextSize(value_sample).x + 10.f;
        for (int c = 0; c < value_cols; ++c)
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, value_w);
        ImGui::TableSetupColumn("ASCII", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("WWWWWWWWWWWWWWWW").x + 12.f);
        ImGui::TableHeadersRow();

        // 只渲染滑动窗口内的行；窗口随滚动重锚，等效 CE 式无限滚动
        for (int row = 0; row < page_rows; ++row) {
            const uint64_t row_addr = view_base_ + (uint64_t)row * k_bytes_per_row;

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);

            ImGui::TextColored(ImVec4(ImColor(127, 178, 199, 255)), "0x%llX", (unsigned long long)row_addr);

            if (vsize == 1)
                render_row_byte_mode(row_addr);
            else
                render_row_value_mode(row_addr, vsize);

            ImGui::TableSetColumnIndex(1 + value_cols);
            render_ascii_cell(row_addr);
        }

        // 重锚：滚到滑动窗口边缘就整体前进/后退一格可视页。
        // 必须在 EndTable 之前调用——此时 Get/SetScrollY 作用于表格内部滚动区。
        const float sy = ImGui::GetScrollY();
        const float smax = ImGui::GetScrollMaxY();
        const float page_h = (float)visible_rows * row_h;
        const float wheel = ImGui::GetIO().MouseWheel;
        const uint64_t step = (uint64_t)visible_rows * k_bytes_per_row;

        if (want_scroll_top_) {
            ImGui::SetScrollY(0.f);
            want_scroll_top_ = false;
        } else if (smax > 0.f && sy >= smax - 0.5f) {
            view_base_ += step;
            ImGui::SetScrollY(sy - page_h);
        } else if (ImGui::IsWindowHovered() && wheel > 0.f && sy <= 0.5f &&
                   view_base_ > step) {
            view_base_ -= step;
            ImGui::SetScrollY(page_h);
        }

        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

void hex_view::render_row_byte_mode(uint64_t row_addr)
{
    for (int i = 0; i < k_bytes_per_row; ++i) {
        ImGui::TableSetColumnIndex(1 + i);
        const uint64_t a = row_addr + (uint64_t)i;
        bool valid = false;
        const uint8_t v = byte_at(a, valid);
        char buf[4];
        if (valid) snprintf(buf, sizeof(buf), "%02X", v);
        else       snprintf(buf, sizeof(buf), "??");

        ImGui::PushID((int)(a & 0xffffffff));
        draw_data_cell(a, buf, valid, 1, ec_hex, true);
        ImGui::PopID();
    }
}

void hex_view::render_row_value_mode(uint64_t row_addr, int vsize)
{
    const auto& ti = type_info(display_type_);
    const int cols = k_bytes_per_row / vsize;

    for (int c = 0; c < cols; ++c) {
        ImGui::TableSetColumnIndex(1 + c);
        const uint64_t a = row_addr + (uint64_t)c * vsize;

        uint8_t raw[8] = {};
        bool all_valid = true;
        for (int i = 0; i < vsize; ++i) {
            bool v = false;
            raw[i] = byte_at(a + (uint64_t)i, v);
            all_valid = all_valid && v;
        }

        char buf[40] = {};
        if (all_valid)
            format_value(raw, vsize, ti.is_int, hex_display_, buf, sizeof(buf));
        else
            snprintf(buf, sizeof(buf), "??");

        ImGui::PushID((int)(a & 0xffffffff));
        draw_data_cell(a, buf, all_valid, vsize, ec_value, ti.is_int && hex_display_);
        ImGui::PopID();
    }
}

void hex_view::render_ascii_cell(uint64_t row_addr)
{
    char line[k_bytes_per_row + 1];
    bool valid[k_bytes_per_row];
    for (int i = 0; i < k_bytes_per_row; ++i) {
        bool v = false;
        const uint8_t b = byte_at(row_addr + (uint64_t)i, v);
        valid[i] = v;
        if (!v)
            line[i] = '?';
        else
            line[i] = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
    }
    line[k_bytes_per_row] = 0;

    ImGui::PushID((int)(row_addr & 0xffffffff));

    const bool editing_here = edit_open_ && edit_kind_ == ec_ascii &&
                              edit_addr_ >= row_addr &&
                              edit_addr_ < row_addr + k_bytes_per_row;

    if (editing_here) {
        if (edit_focus_) {
            ImGui::SetKeyboardFocusHere();
            edit_focus_ = false;
        }
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputText("##edit", edit_buf_, sizeof(edit_buf_),
                             ImGuiInputTextFlags_EnterReturnsTrue |
                             ImGuiInputTextFlags_AutoSelectAll)) {
            if (commit_ascii_edit())
                edit_open_ = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            edit_open_ = false;
    } else {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float line_h = ImGui::GetTextLineHeight();

        // 逐字符累积宽度（比例字体下每个字符宽度不同）
        float xs[k_bytes_per_row + 1];
        xs[0] = 0.f;
        for (int i = 0; i < k_bytes_per_row; ++i)
            xs[i + 1] = xs[i] + ImGui::CalcTextSize(line + i, line + i + 1).x;
        const float total_w = xs[k_bytes_per_row] + 6.f;

        // 整格交互按钮：把鼠标 x 精确映射到字符下标
        ImGui::InvisibleButton("##ascii_btn", ImVec2(total_w, line_h));
        auto char_at = [&](float mx) {
            for (int i = 0; i < k_bytes_per_row; ++i)
                if (mx < p.x + xs[i + 1])
                    return i;
            return k_bytes_per_row - 1;
        };
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            selected_addr_ = row_addr + (uint64_t)char_at(ImGui::GetIO().MousePos.x);
            has_selection_ = true;
        }
        if (ImGui::IsItemHovered() &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            open_ascii_editor(row_addr +
                              (uint64_t)char_at(ImGui::GetIO().MousePos.x));
        }
        if (ImGui::BeginPopupContextItem("##ascii_menu")) {
            render_cell_menu(selected_addr_);
            ImGui::EndPopup();
        }

        // 绘制：仅选中字节对应的字符画高亮底色，不可读字符置灰
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 sel_bg = ImGui::GetColorU32(ImGuiCol_Header);
        const ImU32 txt_col = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 bad_col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        for (int i = 0; i < k_bytes_per_row; ++i) {
            const bool sel = has_selection_ &&
                             selected_addr_ == row_addr + (uint64_t)i;
            if (sel)
                dl->AddRectFilled(ImVec2(p.x + xs[i] - 1.f, p.y),
                                  ImVec2(p.x + xs[i + 1] + 1.f, p.y + line_h),
                                  sel_bg);
            dl->AddText(ImVec2(p.x + xs[i], p.y),
                        valid[i] ? txt_col : bad_col, line + i, line + i + 1);
        }
    }

    ImGui::PopID();
}

void hex_view::draw_data_cell(uint64_t addr, const char* text, bool valid,
                              int size, int kind, bool hex_input)
{
    // 编辑态：格内嵌 InputText
    if (edit_open_ && edit_addr_ == addr && edit_kind_ == kind) {
        if (edit_focus_) {
            ImGui::SetKeyboardFocusHere();
            edit_focus_ = false;
        }
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                    ImGuiInputTextFlags_AutoSelectAll;
        if (hex_input)
            flags |= ImGuiInputTextFlags_CharsHexadecimal;
        if (ImGui::InputText("##edit", edit_buf_, sizeof(edit_buf_), flags)) {
            if (commit_edit())
                edit_open_ = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            edit_open_ = false;
        return;
    }

    // 变化高亮（渐变消退）
    bool changed = false;
    float fade = 0.f;
    auto it = change_ticks_.find(addr);
    if (it != change_ticks_.end()) {
        const float dt = (float)(now_ms() - it->second);
        if (dt < (float)k_fade_ms) {
            changed = true;
            fade = 1.f - dt / (float)k_fade_ms;
        }
    }

    if (changed || !valid) {
        ImVec4 col;
        if (!valid) {
            col = ImVec4(0.5f, 0.5f, 0.5f, 1.f);
        } else {
            // 手动 lerp：正常文本色 → 高亮橙
            const ImVec4 base = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            col = ImVec4(base.x + (1.0f - base.x) * fade,
                         base.y + (0.55f - base.y) * fade,
                         base.z + (0.10f - base.z) * fade,
                         base.w);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, col);
    }

    if (ImGui::Selectable(text, has_selection_ &&
                                   selected_addr_ >= addr &&
                                   selected_addr_ < addr + (uint64_t)size)) {
        selected_addr_ = addr;
        has_selection_ = true;
    }
    if (valid && ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        open_editor(addr, kind, size, display_type_, hex_input, text);
    }
    if (changed || !valid)
        ImGui::PopStyleColor();

    if (ImGui::BeginPopupContextItem("##cell_menu")) {
        render_cell_menu(addr);
        ImGui::EndPopup();
    }
}

// ---- 编辑 ----

void hex_view::open_editor(uint64_t addr, int kind, int size, int type_idx,
                           bool hex_input, const char* initial)
{
    edit_open_ = true;
    edit_focus_ = true;
    edit_addr_ = addr;
    edit_kind_ = kind;
    edit_size_ = size;
    edit_type_ = type_idx;
    edit_hex_input_ = hex_input;
    snprintf(edit_buf_, sizeof(edit_buf_), "%s", initial ? initial : "");
}

void hex_view::open_ascii_editor(uint64_t addr)
{
    std::string s;
    for (int i = 0; i < 64; ++i) {
        bool v = false;
        const uint8_t b = byte_at(addr + (uint64_t)i, v);
        if (!v || b < 0x20 || b >= 0x7f)
            break;
        s.push_back((char)b);
    }
    open_editor(addr, ec_ascii, 1, display_type_, false, s.c_str());
}

bool hex_view::commit_edit()
{
    const char* s = edit_buf_;
    while (*s == ' ' || *s == '\t') ++s;
    if (!*s)
        return false;

    uint8_t raw[8] = {};
    int size = edit_size_;

    if (edit_kind_ == ec_hex) {
        unsigned v = 0;
        if (sscanf(s, "%2x", &v) != 1)
            return false;
        raw[0] = (uint8_t)v;
        size = 1;
    } else {
        const auto& ti = type_info(edit_type_);
        if (ti.is_int) {
            char* end = nullptr;
            const unsigned long long v =
                strtoull(s, &end, edit_hex_input_ ? 16 : 10);
            if (end == s)
                return false;
            const int bits = size * 8;
            if (bits < 64 && (v >> bits) != 0)
                return false;   // 超出类型范围
            std::memcpy(raw, &v, (size_t)size);
        } else if (size == 4) {
            float f = 0;
            if (sscanf(s, "%f", &f) != 1)
                return false;
            std::memcpy(raw, &f, 4);
        } else {
            double d = 0;
            if (sscanf(s, "%lf", &d) != 1)
                return false;
            std::memcpy(raw, &d, 8);
        }
    }
    return write_bytes(edit_addr_, raw, (size_t)size);
}

bool hex_view::commit_ascii_edit()
{
    const size_t n = strlen(edit_buf_);
    if (n == 0)
        return true;   // 空输入视作取消
    return write_bytes(edit_addr_, edit_buf_, n);
}

bool hex_view::write_bytes(uint64_t addr, const void* data, size_t n)
{
    auto* mem = process_manager::instance().memory();
    if (!mem)
        return false;
    if (!mem->write(addr, data, n))
        return false;

    // 同步页缓存 + 打变化标记（写入的字节即刻高亮）
    const int64_t now = now_ms();
    const uint8_t* bytes = (const uint8_t*)data;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t a = addr + i;
        page* pg = get_page(a & ~(uint64_t)(k_page_size - 1));
        const size_t off = (size_t)(a & (k_page_size - 1));
        pg->data[off] = bytes[i];
        pg->readable_mask |= (1ull << (off / k_chunk_size));
        change_ticks_[a] = now;
    }
    return true;
}

// ---- 右键菜单 ----

void hex_view::render_cell_menu(uint64_t addr)
{
    if (ImGui::MenuItem("复制地址")) {
        char b[24];
        snprintf(b, sizeof(b), "%016llX", (unsigned long long)addr);
        ImGui::SetClipboardText(b);
    }
    if (ImGui::MenuItem("添加到地址列表")) {
        address_record rec;
        rec.real_address = addr;
        char b[24];
        snprintf(b, sizeof(b), "%016llX", (unsigned long long)addr);
        rec.address = b;
        rec.description = "十六进制视图";
        rec.valid = true;
        rec.type = type_info(display_type_).vtype;
        rec.previous_value = read_address_value(addr, rec.type);
        rec.value = rec.previous_value;
        application_context::instance().address_list.add_record(rec);
    }

    auto* mem = process_manager::instance().memory();
    if (mem && mem->architecture() != process_arch::unknown &&
        ImGui::MenuItem("跟随指针")) {
        const int psz = (mem->architecture() == process_arch::x86_64) ? 8 : 4;
        uint8_t raw[8] = {};
        bool ok = true;
        for (int i = 0; i < psz; ++i) {
            bool v = false;
            raw[i] = byte_at(addr + (uint64_t)i, v);
            ok = ok && v;
        }
        if (ok) {
            uint64_t target = 0;
            std::memcpy(&target, raw, (size_t)psz);
            if (target)
                goto_address(target);
        }
    }
}

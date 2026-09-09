#include "file_browser.h"

#include <cstring>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <cwctype>

#include <windows.h>

namespace fs = std::filesystem;

// 路径 → 窄字符串（char8_t → char）。C++20 下 fs::path::u8string() 返回 char8_t*，
// ImGui 需要 char*，这里统一转换。
static std::string path_to_string(const fs::path& p) {
    const std::u8string& u = p.u8string();
    return std::string(u.begin(), u.end());
}

// ── helpers ────────────────────────────────────────────────

static std::string ws_to_utf8(const wchar_t* ws) {
    if (!ws || *ws == 0) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n, 0);
    ::WideCharToMultiByte(CP_UTF8, 0, ws, -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

static bool is_ascii(const std::string& s) {
    for (unsigned char c : s) if (c >= 0x80) return false;
    return true;
}

// Display name: prefer ASCII filename; otherwise fall back to UTF-8 conversion.
static std::string safe_display_name(const fs::path& p) {
    std::string raw = p.filename().string();    // narrow path may use local codepage
    if (is_ascii(raw)) return raw;
    return ws_to_utf8(p.filename().c_str());
}

// Human-readable byte count.
static std::string format_size(std::intmax_t bytes) {
    char buf[64];
    if (bytes < 0) { snprintf(buf, sizeof buf, "?"); return buf; }
    double v = (double)bytes;
    if (v < 1024.0)               { snprintf(buf, sizeof buf, "%.0f B", v); }
    else if (v < 1024.0 * 1024)   { snprintf(buf, sizeof buf, "%.2f KB", v / 1024.0); }
    else if (v < 1024.0 * 1024 * 1024) { snprintf(buf, sizeof buf, "%.2f MB", v / (1024.0*1024.0)); }
    else                          { snprintf(buf, sizeof buf, "%.2f GB", v / (1024.0*1024.0*1024.0)); }
    return buf;
}

static std::string format_time(const fs::file_time_type& ft) {
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
    std::tm tmv{};
    localtime_s(&tmv, &tt);
    char buf[64];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

// Filter match: filter has the form "dll" / "dll,exe" / empty.
static bool matches_filter(const fs::path& p, bool is_dir, const std::string& filter) {
    if (is_dir) return true;                     // directories are never filtered
    if (filter.empty()) return true;
    std::string ext = p.extension().string();
    if (ext.empty()) return false;
    if (ext.front() == '.') ext.erase(0, 1);     // strip the leading dot
    for (size_t i = 0, n = filter.size(); i < n; ) {
        size_t j = filter.find(',', i);
        if (j == std::string::npos) j = n;
        std::string tok = filter.substr(i, j - i);
        for (auto& c : tok) c = (char)tolower((unsigned char)c);
        if (tok == ext) return true;
        i = j + 1;
    }
    return false;
}

// ── ctor / refresh ─────────────────────────────────────────

file_browser::file_browser() {
    current_path_ = fs::current_path();
    refresh();
}

void file_browser::set_initial_path(const fs::path& p) {
    if (fs::exists(p) && fs::is_directory(p)) {
        current_path_ = p;
        refresh();
    }
}

void file_browser::clear_filter() { filter_.clear(); if (filter_buf_[0]) filter_buf_[0] = 0; }
void file_browser::set_filter(const std::string& ext) {
    filter_ = ext;
    for (auto& c : filter_) c = (char)tolower((unsigned char)c);
    strncpy(filter_buf_, filter_.c_str(), sizeof filter_buf_ - 1);
}

void file_browser::refresh() {
    items_.clear();
    std::error_code ec;
    fs::directory_iterator it(current_path_, ec), end;
    if (ec) return;

    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const fs::directory_entry& e = *it;
        fs::path p = e.path();
        std::string name = safe_display_name(p);
        if (name.empty()) continue;

        bool is_dir = e.is_directory(ec);
        if (ec) { ec.clear(); continue; }
        bool hidden = (name.size() >= 1 && name[0] == '.');
        if (hidden && !show_hidden_) continue;

        file_item item;
        item.path = p;
        item.display_name = name;
        item.is_directory = is_dir;
        item.is_hidden = hidden;
        item.size = is_dir ? 0 : (e.is_regular_file(ec) ? e.file_size(ec) : 0);
        if (ec) { ec.clear(); }
        auto ft = e.last_write_time(ec);
        if (!ec) item.last_write_time = format_time(ft);
        else { ec.clear(); }

        DWORD fattrs = ::GetFileAttributesW(p.c_str());
        if (fattrs != INVALID_FILE_ATTRIBUTES) {
            item.is_hidden = (fattrs & FILE_ATTRIBUTE_HIDDEN) != 0;
            item.is_readonly = (fattrs & FILE_ATTRIBUTE_READONLY) != 0;
        }

        items_.push_back(std::move(item));
    }

    // Sort: directories first, then case-insensitive by name.
    auto cmp = [](const file_item& a, const file_item& b) {
        if (a.is_directory != b.is_directory) return a.is_directory;
        std::string la, lb;
        for (auto c : a.display_name) la += (char)tolower((unsigned char)c);
        for (auto c : b.display_name) lb += (char)tolower((unsigned char)c);
        return la < lb;
    };
    std::sort(items_.begin(), items_.end(), cmp);
}

bool file_browser::navigate(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return false;
    current_path_ = dir;
    selected_path_.clear();
    refresh();
    return true;
}

// ── render ─────────────────────────────────────────────────

void file_browser::render() {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float list_avail = ImGui::GetContentRegionAvail().y;

    // ── 顶部：面包屑导航（每段可点击跳到对应目录）+ 刷新 ──
    if (ImGui::Button("refresh")) refresh();

    // 面包屑：从根路径到当前路径，逐段渲染可点击按钮。
    ImGui::SameLine();
    fs::path acc;
    bool first = true;
    for (const auto& part : current_path_) {
        acc /= part;
        if (!first) {
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted("/");
        }
        first = false;
        ImGui::SameLine();
        std::string seg = path_to_string(acc);
        if (ImGui::Button(seg.c_str())) navigate(acc);
    }

    ImGui::Separator();

    // ── 过滤器栏 ──
    ImGui::SetNextItemWidth(avail_w * 0.30f);
    if (ImGui::InputTextWithHint("##filter", "Filter ext (e.g. dll,exe)", filter_buf_, sizeof filter_buf_)) {
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            filter_ = filter_buf_;
            for (auto& c : filter_) c = (char)tolower((unsigned char)c);
            refresh();
        }
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Show hidden", &show_hidden_)) refresh();

    ImGui::Separator();

    // ── body: file list (left) + info panel (right) ──
    const float list_w = avail_w * 0.70f;
    const float info_w = avail_w - list_w - st.ItemSpacing.x;
    const bool  no_footer = true;

    ImGui::BeginChild("##flist", ImVec2(list_w, list_avail), ImGuiChildFlags_Borders);

    // 双击文件夹的导航延迟到表格遍历结束后执行：
    // navigate -> refresh 会 clear/重建 items_，若在遍历中途调用会使迭代器失效而崩溃。
    fs::path pending_nav;

    bool any_match = false;
    if (ImGui::BeginTable("##fitems", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.60f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableHeadersRow();

        for (auto& item : items_) {
            if (!item.is_directory && !matches_filter(item.path, false, filter_)) continue;
            any_match = true;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            std::string label = (item.is_directory ? "[Dir] " : "[File] ") + item.display_name;
            bool is_selected = (item.path == selected_path_);
            ImGui::PushID(label.c_str());
            if (ImGui::Selectable(label.c_str(), is_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                selected_path_ = item.path;
            }
            bool dbl = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            if (dbl && item.is_directory) {
                pending_nav = item.path;
                ImGui::PopID();
                break;
            }
            if (dbl && !item.is_directory) {
                selected_path_ = item.path;      // double-click a file = select it
            }
            ImGui::PopID();

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(item.is_directory ? "Folder" : "File");
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(item.is_directory ? "-" : format_size(item.size).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(item.last_write_time.c_str());
        }
        if (!any_match) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("(empty or no matching files)");
        }
        ImGui::EndTable();
        if (!pending_nav.empty()) navigate(pending_nav);
        (void)no_footer;
    }
    ImGui::EndChild();

    // ── right-hand info panel ──
    ImGui::SameLine();
    ImGui::BeginChild("##finfo", ImVec2(info_w, list_avail), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("File info");
    ImGui::Separator();
    if (!has_selection()) {
        ImGui::TextDisabled("(nothing selected)");
    } else {
        auto sel_it = std::find_if(items_.begin(), items_.end(),
                                   [&](const file_item& i){ return i.path == selected_path_; });
        if (sel_it == items_.end()) {
            ImGui::TextUnformatted("(file is not in the current dir or was filtered)");
        } else {
            const file_item& sel = *sel_it;
            ImGui::Text("Name: %s", sel.display_name.c_str());
            ImGui::Text("Kind: %s", sel.is_directory ? "Folder" : "File");
            ImGui::Text("Size: %s", sel.is_directory ? "-" : format_size(sel.size).c_str());
            ImGui::TextWrapped("Path:\n%s", path_to_string(sel.path).c_str());
            ImGui::Text("Modified: %s", sel.last_write_time.c_str());
            if (sel.is_readonly) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Attr: read-only");
            else                 ImGui::Text("Attr: normal");
        }
    }
    ImGui::EndChild();
    (void)list_avail;
}
#include "inject_window.h"

#include <cstdio>
#include <cwchar>
#include "imgui.h"
#include "core/process_manager.h"

namespace
{
    // 宽字符串转 UTF-8（S-inject 参数用窄/UTF-8 字符串）
    std::string ws_to_utf8(const std::wstring& w) {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::string s(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
        if (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }

    // 判断某进程是否为 64 位（非 Wow64）原生 64 位进程
    bool is_process_64(DWORD pid) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return false;
        BOOL wow64 = FALSE;
        BOOL ok = IsWow64Process(h, &wow64);
        CloseHandle(h);
        return ok && !wow64;
    }
}

inject_window::inject_window() {
    browser_.set_filter("dll");
    method_names_ = { "Remote thread (LoadLibrary)", "Reflective", "APC", "Context" };
    refresh_processes();
}

void inject_window::refresh_processes() {
    procs_.clear();
    selected_proc_ = -1;

    auto& pm = process_manager::instance();
    if (!pm.is_attached()) {
        attached_pid_ = 0;
        return;
    }

    // 自动跟随当前已附加进程（只能注入它）。
    DWORD pid = pm.attached_pid();
    attached_pid_ = pid;

    if (!is_process_64(pid)) {
        // 32 位进程：不支持注入，明确提示
        ProcessInfo p; p.pid = pid; p.processName = L"<32-bit process - not supported>";
        procs_.push_back(p);
        selected_proc_ = 0;
        return;
    }

    ProcessInfo p;
    p.pid = pid;
    // 进程名：从注入后端枚举匹配一次（尽力而为，失败则仅显示 PID）
    for (auto& p2 : XInject::Injector::listInjectable())
        if (p2.pid == pid) { p.processName = p2.processName; break; }
    procs_.push_back(p);
    selected_proc_ = 0;
}

void inject_window::do_inject() {
    auto& pm = process_manager::instance();
    if (!pm.is_attached()) {
        log("Attach a process first (Open Process)");
        return;
    }
    if (attached_pid_ != pm.attached_pid()) refresh_processes(); // 进程可能变化，重取一次
    if (selected_proc_ < 0 || selected_proc_ >= (int)procs_.size()) {
        log("No process selected");
        return;
    }
    if (!is_process_64(attached_pid_)) {
        log("Cannot inject into a 32-bit process (64-bit only)");
        return;
    }
    if (!browser_.has_selection() || !browser_.selected_path().has_extension()) {
        log("No DLL selected");
        return;
    }
    ProcessInfo& proc = procs_[selected_proc_];
    DWORD pid = proc.pid;
    // 用所选 DLL 的绝对路径（UTF-8）
    std::string dll = ws_to_utf8(browser_.selected_path().wstring());

    char buf[512];
    bool ok = false;
    switch (method_) {
    case 0: ok = XInject::Injector::remoteThreadInject(pid, 0, dll); break;   // DLL 文件
    case 1: ok = XInject::Injector::reflectInject(pid, 0, dll);    break;
    case 2: ok = XInject::Injector::apcInject(pid, 0, dll);        break;
    case 3: ok = XInject::Injector::contextInject(pid, 0, dll);    break;
    default: ok = false; break;
    }
    snprintf(buf, sizeof buf, "[inject pid=%lu %s] %s", (unsigned long)pid,
             method_names_[method_].c_str(), ok ? "OK" : "FAILED");
    log(buf);
}

void inject_window::do_unload() {
    auto& pm = process_manager::instance();
    if (!pm.is_attached()) {
        log("Attach a process first (Open Process)");
        return;
    }
    if (attached_pid_ != pm.attached_pid()) refresh_processes();
    if (selected_proc_ < 0 || selected_proc_ >= (int)procs_.size()) {
        log("No process selected");
        return;
    }
    if (unload_name_[0] == 0) {
        log("Enter a DLL base name to unload (e.g. mydll.dll)");
        return;
    }
    DWORD pid = attached_pid_;
    bool ok = XInject::Injector::unInject(pid, unload_name_);
    char buf[256];
    snprintf(buf, sizeof buf, "[unload pid=%lu %s] %s", (unsigned long)pid,
             unload_name_, ok ? "OK" : "FAILED");
    log(buf);
}

void inject_window::log(const std::string& line) {
    log_text_ += line + "\n";
    if (log_text_.size() > 8192) log_text_.erase(0, log_text_.size() - 8192);
}

void inject_window::render() {
    if (!open_) return;

    if (!ImGui::Begin("Inject", &open_, 0)) {
        ImGui::End();
        return;
    }

    const float avail = ImGui::GetContentRegionAvail().x;

    // ── 目标进程：自动锁定为已附加进程，不支持切换（只能注入当前附加的进程）──
    auto& pm = process_manager::instance();
    if (pm.is_attached()) {
        if (attached_pid_ != pm.attached_pid()) refresh_processes();

        std::string procname = (selected_proc_ >= 0 && selected_proc_ < (int)procs_.size())
                                   ? ws_to_utf8(procs_[selected_proc_].processName)
                                   : std::string();
        ImGui::TextUnformatted("Attached process (fixed)");
        char info[256];
        if (!is_process_64(pm.attached_pid())) {
            snprintf(info, sizeof info, "%s  (pid=%lu)  [32-bit - injection not supported]",
                     procname.c_str(), (unsigned long)pm.attached_pid());
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", info);
        } else {
            snprintf(info, sizeof info, "%s  (pid=%lu)  [64-bit]",
                     procname.c_str(), (unsigned long)pm.attached_pid());
            ImGui::TextUnformatted(info);
        }
    } else {
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f),
                           "No process attached. Open a process first (File -> Open Process).");
    }

    ImGui::Separator();

    // ── 选择 DLL（文件浏览器，过滤 dll）──
    ImGui::TextUnformatted("Select a DLL to inject");
    browser_.render();

    if (browser_.has_selection()) {
        std::string sel = ws_to_utf8(browser_.selected_path().wstring());
        ImGui::TextWrapped("Selected: %s", sel.c_str());
    }

    ImGui::Separator();

    // ── 注入方式 ──
    ImGui::TextUnformatted("Injection method");
    ImGui::SetNextItemWidth(avail * 0.6f);
    if (ImGui::BeginCombo("##m", method_names_[method_].c_str())) {
        for (int i = 0; i < (int)method_names_.size(); ++i)
            if (ImGui::Selectable(method_names_[i].c_str(), i == method_)) method_ = i;
        ImGui::EndCombo();
    }

    // ── 操作 ──
    if (ImGui::Button("Inject", ImVec2(120, 0))) do_inject();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    ImGui::InputText("##unload", unload_name_, sizeof unload_name_);
    ImGui::SameLine();
    if (ImGui::Button("Unload", ImVec2(100, 0))) do_unload();

    ImGui::Separator();

    // ── 状态日志 ──
    ImGui::TextUnformatted("Log");
    ImGui::BeginChild("##injectlog", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextWrapped("%s", log_text_.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::End();
}
#include "debug_panel.h"
#include "imgui.h"

#include <psapi.h>

void debug_panel::render(bool& open) {
    if (!open) return;

    // Throttle sampling to every ~250ms (measured via performance counter).
    sample();

    ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(200, 100), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Debug Panel", &open)) {
        ImGui::Text("FPS:%.1f", ImGui::GetIO().Framerate);
        ImGui::Text("CPU (system): %.1f%%   cores: %u", cpu_now_, cpu_cores_);
        ImGui::Text("CPU (this process): %.1f%%", pcpu_now_);
        ImGui::Text("MEM (system): %.1f%%   %llu MB free / %llu MB total",
                    mem_now_, (unsigned long long)mem_avail_mb_,
                    (unsigned long long)mem_total_mb_);
        ImGui::Text("MEM (this process): working set %.1f MB, commit %.1f MB",
                    ws_bytes_ / 1048576.0, commit_bytes_ / 1048576.0);
        if (gpu_ok_)
            ImGui::Text("GPU: %ls   VRAM %.1f GB",
                        gpu_name_.c_str(), gpu_vram_bytes_ / (1073741824.0));
        else
            ImGui::Text("GPU: <unknown>");

        ImGui::Separator();

        // Charts: draw using ImDrawList polylines over the sample history.
        const float h = 70.0f;
        auto plot = [&](const char* label, const std::array<float, k_history>& hist,
                        float minv, float maxv, const char* fmt) {
            ImGui::Text("%s   %s", label, fmt);
            ImVec2 avail = ImGui::GetContentRegionAvail();
            float w = avail.x;
            if (w < 100) return;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImVec2 p1 = ImVec2(p0.x + w, p0.y + h);
            dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 80));
            dl->AddRect(p0, p1, IM_COL32(120, 120, 120, 255));
            // draw sample points
            int n = count_;
            if (n < 2) { ImGui::Dummy(ImVec2(w, h)); return; }
            float stepx = w / (float)(k_history - 1);
            ImVec2 prev;
            bool has = false;
            float vmin = minv, vmax = maxv;
            float range = (vmax - vmin) > 1e-3f ? (vmax - vmin) : 1.0f;
            for (int i = 0; i < n; ++i) {
                // index into ring: oldest first
                int idx = (head_ - n + i + k_history * 2) % k_history;
                float v = hist[idx];
                float x = p0.x + (float)i * stepx;
                float y = p1.y - (v - vmin) / range * h;
                if (y < p0.y) y = p0.y;
                if (y > p1.y) y = p1.y;
                ImVec2 cur(x, y);
                if (has) dl->AddLine(prev, cur, IM_COL32(80, 200, 120, 255), 1.5f);
                prev = cur; has = true;
            }
            ImGui::Dummy(ImVec2(w, h));
        };

        plot("CPU (system)", cpu_hist_, 0.0f, 100.0f, "");
        plot("CPU (proc)",   pcpu_hist_, 0.0f, 100.0f, "");
        plot("MEM (system)", mem_hist_,  0.0f, 100.0f, "");
    }
    ImGui::End();
}

void debug_panel::sample() {
    // Time gating
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    double ms = now.QuadPart * 1000.0 / freq.QuadPart;
    if (last_sample_ms_ == 0.0)
        last_sample_ms_ = ms;
    if (ms - last_sample_ms_ < 250.0)
        return;
    double dt = ms - last_sample_ms_;
    last_sample_ms_ = ms;

    // System CPU% via GetSystemTimes deltas
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        ULONGLONG idle_f = ((ULONGLONG)idle.dwHighDateTime << 32) | idle.dwLowDateTime;
        ULONGLONG kern_f = ((ULONGLONG)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
        ULONGLONG user_f = ((ULONGLONG)user.dwHighDateTime << 32) | user.dwLowDateTime;
        if (last_idle_ != 0) {
            ULONGLONG d_idle = idle_f - last_idle_;
            ULONGLONG d_kern = kern_f - last_kernel_;
            ULONGLONG d_user = user_f - last_user_;
            // kernel already includes idle on Windows
            ULONGLONG busy = (d_kern - d_idle) + d_user;
            ULONGLONG total = (d_kern + d_user);
            cpu_now_ = (total > 0) ? (float)(busy) / (float)total * 100.0f : 0.0f;
        }
        last_idle_ = idle_f; last_kernel_ = kern_f; last_user_ = user_f;
    }

    // This process CPU%
    {
        FILETIME ck, cu, ex, eo;
        HANDLE h = GetCurrentProcess();
        if (GetProcessTimes(h, &ck, &cu, &ex, &eo)) {
            ULONGLONG k = ((ULONGLONG)ex.dwHighDateTime << 32) | ex.dwLowDateTime;
            ULONGLONG u = ((ULONGLONG)eo.dwHighDateTime << 32) | eo.dwLowDateTime;
            ULONGLONG pc_k = k, pc_u = u;
            if (last_pc_kernel_ != 0) {
                ULONGLONG d_k = pc_k - last_pc_kernel_;
                ULONGLONG d_u = pc_u - last_pc_user_;
                // FILETIME units are 100ns; convert to seconds then to "percent of one core over dt"
                double cpu_sec = (double)(d_k + d_u) / 10000000.0;
                double pct = cpu_sec / (dt / 1000.0) * 100.0;   // can exceed 100 with many threads
                if (pct < 0) pct = 0;
                pcpu_now_ = (float)pct;
            }
            last_pc_kernel_ = pc_k; last_pc_user_ = pc_u;
        }
    }

    // System memory
    MEMORYSTATUSEX msx;
    msx.dwLength = sizeof(msx);
    if (GlobalMemoryStatusEx(&msx)) {
        mem_total_mb_ = msx.ullTotalPhys / 1048576;
        mem_avail_mb_ = msx.ullAvailPhys / 1048576;
        mem_now_ = (float)(1.0 - (double)msx.ullAvailPhys / (double)msx.ullTotalPhys) * 100.0f;
    }
    // CPU core count from system info (MEMORYSTATUSEX has no such field)
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        cpu_cores_ = si.dwNumberOfProcessors;
    }

    // This process memory
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        ws_bytes_ = pmc.WorkingSetSize;
        commit_bytes_ = pmc.PagefileUsage;
    }

    // GPU (once)
    if (!gpu_ok_) sample_gpu();

    // push history
    cpu_hist_[head_] = cpu_now_;
    mem_hist_[head_] = mem_now_;
    pcpu_hist_[head_] = pcpu_now_;
    head_ = (head_ + 1) % k_history;
    if (count_ < k_history) ++count_;
}

void debug_panel::sample_gpu() {
    IDXGIFactory* factory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory)))
        return;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(factory->EnumAdapters(0, &adapter))) {
        DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            gpu_name_ = desc.Description;
            gpu_vram_bytes_ = desc.DedicatedVideoMemory;
            gpu_ok_ = true;
        }
        adapter->Release();
    }
    factory->Release();
}

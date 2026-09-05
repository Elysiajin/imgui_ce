#pragma once

#include <windows.h>
#include <dxgi.h>

#include <array>
#include <cstdint>
#include <string>
#include <wchar.h>

// System monitor panel rendered inside the "About" menu's Debug Panel window.
// Samples system CPU%, our process CPU%, system memory%, and lists the primary
// GPU adapter + dedicated VRAM. Samples are throttled to keep overhead tiny and
// plotted as rolling line charts.
class debug_panel {
public:
    void render(bool& open);

private:
    void sample();
    void sample_gpu();

    // Last raw counters (for CPU% deltas between samples)
    ULONGLONG last_idle_ = 0, last_kernel_ = 0, last_user_ = 0;
    ULONGLONG last_pc_kernel_ = 0, last_pc_user_ = 0;
    double last_sample_ms_ = 0.0;

    // Rolling history
    static constexpr int k_history = 120;
    std::array<float, k_history> cpu_hist_{};
    std::array<float, k_history> mem_hist_{};
    std::array<float, k_history> pcpu_hist_{};
    int head_ = 0;
    int count_ = 0;

    // Current computed values (updated on sample)
    float cpu_now_ = 0.0f;
    float mem_now_ = 0.0f;
    float pcpu_now_ = 0.0f;
    UINT64 mem_total_mb_ = 0;
    UINT64 mem_avail_mb_ = 0;
    UINT64 ws_bytes_ = 0;
    UINT64 commit_bytes_ = 0;
    uint32_t cpu_cores_ = 0;

    // GPU (sampled once)
    bool gpu_ok_ = false;
    std::wstring gpu_name_;
    UINT64 gpu_vram_bytes_ = 0;
};

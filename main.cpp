#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "ui/scan_panel.h"
#include "ui/result_panel.h"
#include "ui/top_menu.h"
#include "ui/process_list_window.h"
#include "ui/process_detail_window.h"
#include "ui/debug_panel.h"
#include "ui/settings_window.h"
#include "ui/theme.h"
#include "ui/app_context.h"
#include "ui/address_list_panel.h"
#include "ui/process_icon_cache.h"
#include "ui/memory_window.h"
// #include "ui/assembler_window.h"
#include "core/event/signal.h"
#include "scan/scan_service.h"

#include <d3d11.h>
#include <tchar.h>
#include <windows.h>

#include <cmath>

static ID3D11Device*            g_device = nullptr;
static ID3D11DeviceContext*     g_context = nullptr;
static IDXGISwapChain*          g_swapChain = nullptr;
static ID3D11RenderTargetView*  g_rtv = nullptr;
static UINT                     g_resizeW = 0, g_resizeH = 0;

bool CreateDeviceD3D(HWND hwnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l))
        return true;
    switch (msg) {
    case WM_SIZE:
        if (w == SIZE_MINIMIZED) return 0;
        g_resizeW = LOWORD(l); g_resizeH = HIWORD(l);
        return 0;
    case WM_SYSCOMMAND:
        if ((w & 0xfff0) == SC_KEYMENU) return 0;  // 禁用 Alt 菜单
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

// ── 常驻状态进度条 ──────────────────────────────────────────
// 左侧：附加状态（"未附加" / "PID xxxx · 进程名"）；右侧：扫描状态（"未扫描" / 百分比）。
// 视觉：圆角药丸条 + 多层描边辉光；扫描中辉光随时间脉动。
// 颜色：取当前主题的强调色（ImGuiCol_CheckMark，每套主题都定义为各自的主色调），
//       再按背景明暗归一化饱和度/亮度 —— 色相跟随主题，保证 Dark / Light / Cyan /
//       Midnight / Light Blue 下既醒目又不与整体风格割裂。
static void render_scan_status_bar()
{
    auto& svc = scan_service::instance();
    auto& pm  = process_manager::instance();

    const bool  scanning = svc.is_scanning();
    const bool  attached = pm.is_attached();
    const float progress = scanning ? svc.progress() : 0.0f;

    // 附加进程名缓存：仅在 pid 变化时枚举一次系统进程（避免每帧快照开销）
    static uint32_t    s_pid = 0;
    static std::string s_name;
    if (attached) {
        if (s_pid != pm.attached_pid()) {
            s_pid = pm.attached_pid();
            s_name.clear();
            for (const auto& p : pm.processes().enumerate())
                if (p.pid == s_pid) { s_name = p.name; break; }
        }
    } else {
        s_pid  = 0;
        s_name.clear();
    }

    const ImGuiStyle& st = ImGui::GetStyle();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float bar_h = ImGui::GetTextLineHeight() + st.FramePadding.y * 2.0f + 6.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float  bar_w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p1 = ImVec2(p0.x + bar_w, p0.y + bar_h);
    ImGui::Dummy(ImVec2(bar_w, bar_h));   // 常驻占位：无扫描时也保持布局稳定

    const ImVec4 wbg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const float  lum = wbg.x * 0.299f + wbg.y * 0.587f + wbg.z * 0.114f;
    const bool   is_light = lum > 0.5f;

    // 强调色 = 主题强调色的色相 + 按明暗归一化的饱和度/亮度
    ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    float ah, as, av;
    ImGui::ColorConvertRGBtoHSV(accent.x, accent.y, accent.z, ah, as, av);
    if (as < 0.05f) { as = 0.65f; ah = 0.55f; }   // 主题强调色接近灰色时兜底为蓝色系
    if (is_light) {
        as = as < 0.65f ? 0.65f : as;             // 浅色背景：压暗到可读范围
        av = 0.55f;
    } else {
        as = as < 0.85f ? 0.85f : as;             // 深色背景：提亮到高饱和鲜亮
        av = av < 0.95f ? 0.95f : av;
    }
    ImGui::ColorConvertHSVtoRGB(ah, as, av, accent.x, accent.y, accent.z);
    accent.w = 1.0f;

    const float rounding = bar_h * 0.5f;

    // 辉光：由外向内叠画多层圆角矩形，透明度递增；扫描中随时间脉动
    const float t = (float)ImGui::GetTime();
    const float pulse = scanning ? 0.70f + 0.30f * sinf(t * 5.0f) : 0.45f;
    for (int i = 4; i >= 1; --i) {
        const float expand = (float)i * 2.5f;
        const float alpha  = pulse * (is_light ? 0.16f : 0.30f) * (1.0f - (float)(i - 1) / 4.0f);
        dl->AddRectFilled(ImVec2(p0.x - expand, p0.y - expand),
                          ImVec2(p1.x + expand, p1.y + expand),
                          ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, alpha)),
                          rounding + expand);
    }

    // 条底与描边（描边用强调色低透明度，替代主题 Border，保证形态可辨）
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
    dl->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, 0.55f)), rounding);

    // 进度填充 + 顶部高光
    float frac = progress;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const float fill_w = bar_w * frac;
    if (fill_w >= 2.0f) {
        const ImVec2 fp1(p0.x + fill_w, p1.y);
        dl->AddRectFilled(p0, fp1, ImGui::GetColorU32(accent), rounding);
        const ImVec4 hi(accent.x + (1.0f - accent.x) * 0.30f,
                        accent.y + (1.0f - accent.y) * 0.30f,
                        accent.z + (1.0f - accent.z) * 0.30f, 0.45f);
        dl->AddRectFilled(ImVec2(p0.x + 1.0f, p0.y + 1.0f),
                          ImVec2(fp1.x - 1.0f, p0.y + (bar_h - 2.0f) * 0.45f),
                          ImGui::GetColorU32(hi), rounding * 0.8f,
                          ImDrawFlags_RoundCornersTop);
    }

    // 左侧附加状态 / 右侧扫描状态（分居两端，避免拥挤）
    char left_buf[300];
    if (attached) {
        if (!s_name.empty())
            snprintf(left_buf, sizeof(left_buf), "PID %u   ·   %s", (unsigned)s_pid, s_name.c_str());
        else
            snprintf(left_buf, sizeof(left_buf), "PID %u", (unsigned)s_pid);
    } else {
        snprintf(left_buf, sizeof(left_buf), "未附加");
    }

    char right_buf[32];
    const char* right_text;
    if (scanning) {
        snprintf(right_buf, sizeof(right_buf), "%.1f%%", frac * 100.0f);
        right_text = right_buf;
    } else {
        right_text = "未扫描";
    }

    const ImU32 text_col = ImGui::GetColorU32(accent);
    const ImVec2 lsz = ImGui::CalcTextSize(left_buf);
    dl->AddText(ImVec2(p0.x + st.FramePadding.x + 3.0f, p0.y + (bar_h - lsz.y) * 0.5f), text_col, left_buf);
    const ImVec2 rsz = ImGui::CalcTextSize(right_text);
    dl->AddText(ImVec2(p1.x - rsz.x - st.FramePadding.x - 3.0f, p0.y + (bar_h - rsz.y) * 0.5f), text_col, right_text);
}

int main()
{
    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0,
                      GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
                      L"EditorWnd", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"My Editor", WS_OVERLAPPEDWINDOW,
                              100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) { CleanupDeviceD3D(); UnregisterClassW(wc.lpszClassName, wc.hInstance); return 1; }
    ShowWindow(hwnd, SW_HIDE);
    // ShowWindow(hwnd, SW_NORMAL);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_ViewportsEnable;
    io.ConfigViewportsNoAutoMerge = true;
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Users\\HP\\Downloads\\zh-cn.ttf", 16.0f, nullptr,
                                 io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    IM_ASSERT(font != nullptr);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    process_icon_cache::instance().set_device(g_device);

    static ui_state g_ui_state;
    // 启动时应用保存的主题（Dark/Light/Cyan...），替代原先固定的 StyleColorsDark()
    theme::apply((theme_id)g_ui_state.theme);
    process_list_window process_window(g_ui_state);
    process_detail_window process_detail(g_ui_state);
    debug_panel dpanel;
    auto& app_ctx = application_context::instance();
    settings_window settings(g_ui_state);
    memory_window   hex_window(g_ui_state);
    scan_panel scan_panel(g_ui_state, app_ctx);
    result_panel result_panel(app_ctx);
    top_menu main_menu(g_ui_state);
    // assembler_window assembler_window(g_ui_state);

    // 内存浏览器跳转：面板只发信号，由这里统一改 ui_state 的可见性与视图状态。
    app_ctx.open_memory_viewer.connect([](memory_viewer_mode mode, uint64_t addr) {
        g_ui_state.show_memory_window = true;
        g_ui_state.memory_view_mode   = mode;
        if (mode == memory_viewer_mode::disassembly)
            g_ui_state.disasm_view_address = addr;
        else
            g_ui_state.dump_view_address   = addr;
    });

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_resizeW && g_resizeH) {
            CleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // 主线程事件队列：执行后台线程 post_to_main 的任务（如 scan_finished）。
        zc::drain_main_queue();

        // 附加的进程退出后自动脱离：清空扫描结果/地址区，回到未附加状态。
        {
            auto& pm = process_manager::instance();
            if (pm.is_attached() && !pm.is_process_alive()) {
                auto& svc = scan_service::instance();
                if (svc.is_scanning()) svc.cancel();
                svc.clear();
                app_ctx.address_list.clear();
                g_ui_state.first_scan_done = false;
                g_ui_state.scan_mode = scan_mode::first;
                g_ui_state.modules_loaded = false;
                pm.detach();
            }
        }

        ImGui::SetNextWindowSize(ImVec2(900, 620), ImGuiCond_FirstUseEver);
        static bool is_open = true;

        if (ImGui::Begin("Test Window", &is_open, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoCollapse)) {
            main_menu.render();
            if(g_ui_state.show_process_window){
                process_window.render();
            }

            if(g_ui_state.show_about_window){
                ImGui::SetNextWindowPos(ImVec2(700, 500), ImGuiCond_FirstUseEver);

                if(ImGui::Begin("About", &g_ui_state.show_about_window, ImGuiWindowFlags_NoCollapse)) {
                    ImGui::Text("Ahuthor:Jin");
                    ImGui::Text("QQ:3264688446");
                }
                ImGui::End();
            }

            if(g_ui_state.show_memory_window){
                hex_window.render();
            }

            // Debug panel (system monitor) — opened from About menu
            if (g_ui_state.show_debug_window) {
                dpanel.render(g_ui_state.show_debug_window);
            }

            // Settings window — Edit menu
            if (g_ui_state.show_settings_window) {
                settings.render();
            }
            const float bottom_h = 200.0f;
            // 常驻状态进度条（未附加 / 已附加(PID+进程名) / 扫描进度），带辉光效果
            render_scan_status_bar();
            float top_h = ImGui::GetContentRegionAvail().y - bottom_h - ImGui::GetStyle().ItemSpacing.y;
            if (top_h < ImGui::GetFrameHeight()) top_h = ImGui::GetFrameHeight();

            // 左：扫描面板
            ImGui::BeginChild("left", ImVec2(520, top_h), ImGuiChildFlags_Borders);
            scan_panel.render();
            ImGui::EndChild();

            // 右：结果区
            ImGui::SameLine();
            ImGui::BeginChild("right", ImVec2(0, top_h), ImGuiChildFlags_Borders);
            result_panel.render();
            ImGui::EndChild();

            // 下：地址列表（由 application_context 持有的持久对象渲染）
            app_ctx.address_list.render();
        }
        ImGui::End();
        if (!is_open) done = true;
        process_detail.render();


        ImGui::Render();
        const float clear[4] = { 0.12f, 0.12f, 0.14f, 1.0f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
        g_swapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

bool CreateDeviceD3D(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL fl[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                                createDeviceFlags, fl, 2, D3D11_SDK_VERSION, &sd,
                                                &g_swapChain, &g_device, &featureLevel, &g_context);
    if (res == DXGI_ERROR_UNSUPPORTED)   // 硬件失败回退到 WARP 软件驱动
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                            createDeviceFlags, fl, 2, D3D11_SDK_VERSION, &sd,
                                            &g_swapChain, &g_device, &featureLevel, &g_context);
    if (res != S_OK)
        return false;

    IDXGIFactory* fac;
    if (SUCCEEDED(g_swapChain->GetParent(IID_PPV_ARGS(&fac)))) {
        fac->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        fac->Release();
    }
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context)   { g_context->Release();   g_context = nullptr; }
    if (g_device)    { g_device->Release();    g_device = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBack;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&pBack));
    g_device->CreateRenderTargetView(pBack, nullptr, &g_rtv);
    pBack->Release();
}

void CleanupRenderTarget()
{
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}
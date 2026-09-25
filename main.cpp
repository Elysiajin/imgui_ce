#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "ui/scan_panel.h"
#include "ui/result_panel.h"
#include "ui/top_menu.h"
#include "ui/menu_shell.h"
#include "ui/process_list_window.h"
#include "ui/process_detail_window.h"
#include "ui/debug_panel.h"
#include "ui/settings_window.h"
#include "ui/theme.h"
#include "ui/app_context.h"
#include "ui/address_list_panel.h"
#include "ui/process_icon_cache.h"
#include "ui/memory_window.h"
#include "ui/fonts.h"
// #include "ui/assembler_window.h"
#include "core/event/signal.h"
#include "core/crash_report.h"
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

// 扫描状态辉光条已提取至 ui/menu_shell.cpp（render_scan_status_bar），
// 经典布局与菜单风外壳共用。

int main()
{
    crash_report::install();   // 崩溃自报告：SEH/terminate 现场写入 crash_log.txt

    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0,
                      GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
                      L"EditorWnd", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"内存修改器", WS_OVERLAPPEDWINDOW,
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
    fonts::regular = font;
    fonts::load_icon();   // 图标字体（内存 TTF，供菜单风侧边栏/圆形按钮使用）

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
    menu_shell shell(g_ui_state, app_ctx, scan_panel, result_panel, main_menu);
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

        // 主线程事件队列：执行后台线程 post_to_main 的任务。
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

        ImGui::SetNextWindowSize(ImVec2(905, 624), ImGuiCond_FirstUseEver);
        static bool is_open = true;

        // 布局模式：0 = 经典（菜单栏 + 上下分栏），1 = 菜单风（侧边栏外壳）。
        // 菜单风下窗口 NoBackground：半透明圆角面板底由 menu_shell 手绘。
        const bool menu_mode = g_ui_state.layout_mode == 1;
        const ImGuiWindowFlags main_flags = menu_mode
            ? (ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground)
            : (ImGuiWindowFlags_MenuBar  | ImGuiWindowFlags_NoCollapse);

        if (ImGui::Begin("主界面", &is_open, main_flags)) {
            if (menu_mode)
                shell.render();      // 菜单风：侧边栏 + tab 内容区 + 状态条
            else
                main_menu.render();  // 经典：窗口菜单栏 + CT 弹窗

            if(g_ui_state.show_process_window){
                process_window.render();
            }

            if(g_ui_state.show_about_window){
                ImGui::SetNextWindowPos(ImVec2(700, 500), ImGuiCond_FirstUseEver);

                if(ImGui::Begin("关于", &g_ui_state.show_about_window, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
                    ImGui::Text("作者:Jin");
                    ImGui::Text("QQ:3264688446");
                }
                ImGui::End();
            }

            if(g_ui_state.show_memory_window){
                hex_window.render();
            }

            if (g_ui_state.show_debug_window) {
                dpanel.render(g_ui_state.show_debug_window);
            }

            if (g_ui_state.show_settings_window) {
                settings.render();
            }

            if (!menu_mode) {
                // 常进度条
                render_scan_status_bar();

                // 上方区域高度：首次按窗口高度比例（上 55%）分配；窗口缩放
                // （最大化/还原）时等比跟随，保持用户拖出的上下比例；拖拽
                // splitter 只改像素值，不会重置比例。
                static float above_height = 0.0f;
                static float last_avail_total = 0.0f;
                const float avail_total = ImGui::GetContentRegionAvail().y; // 上+分隔+下 总高
                if (above_height <= 0.0f)
                    above_height = avail_total * 0.55f;
                else if (last_avail_total > 1.0f && fabsf(avail_total - last_avail_total) > 1.0f)
                    above_height *= avail_total / last_avail_total;
                last_avail_total = avail_total;
                if (above_height < 40.0f) above_height = 40.0f;
                const float max_above = avail_total - 10.0f - 8.0f; // 下方地址列表至少保留 10px
                if (above_height > max_above) above_height = max_above;

                // 1. 绘制上方区域
                ImGui::BeginChild("##Above Panel", ImVec2(0, above_height), ImGuiChildFlags_Borders);
                {
                    // 内部左右分栏动态适应高度
                    float inner_h = ImGui::GetContentRegionAvail().y;
                    ImGui::BeginChild("left", ImVec2(520, inner_h), ImGuiChildFlags_Borders);
                    scan_panel.render();
                    ImGui::EndChild();

                    ImGui::SameLine();

                    ImGui::BeginChild("right", ImVec2(0, inner_h), ImGuiChildFlags_Borders);
                    result_panel.render();
                    ImGui::EndChild();
                }
                ImGui::EndChild();

                ImGui::InvisibleButton("##splitter", ImVec2(-1.0f, 4.0f)); // 宽度占满，高度4像素
                if (ImGui::IsItemActive()) {
                    // 鼠标拖拽时，实时修改上方高度
                    above_height += ImGui::GetIO().MouseDelta.y;
                }
                if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                    // 鼠标悬停或拖拽时，改变鼠标指针为上下箭头
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                }

                ImGui::BeginChild("##Below Panel", ImVec2(0, 0), ImGuiChildFlags_Borders);
                app_ctx.address_list.render();
                ImGui::EndChild();
            } // !menu_mode（经典布局专属部分）
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
        // 垂直同步
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
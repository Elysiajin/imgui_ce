#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "ui/scan_panel.h"
#include "ui/result_panel.h"
#include "ui/top_menu.h"
#include "ui/process_list_window.h"
#include "ui/process_detail_window.h"
#include "ui/debug_panel.h"
#include "ui/app_context.h"
#include "ui/address_list_panel.h"
#include "core/event/signal.h"
#include "scan/scan_service.h"

#include <d3d11.h>
#include <tchar.h>
#include <windows.h>

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
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_ViewportsEnable;
    io.ConfigViewportsNoAutoMerge = true;
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Users\\HP\\Downloads\\zh-cn.ttf", 16.0f, nullptr,
                                 io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    IM_ASSERT(font != nullptr);
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    static ui_state g_ui_state;
    process_list_window process_window(g_ui_state);
    process_detail_window process_detail(g_ui_state);
    debug_panel dpanel;
    auto& app_ctx = application_context::instance();
    scan_panel scan_panel(g_ui_state, app_ctx);
    result_panel result_panel(app_ctx);
    top_menu main_menu(g_ui_state);

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

            // Debug panel (system monitor) — opened from About menu
            if (g_ui_state.show_debug_window) {
                dpanel.render(g_ui_state.show_debug_window);
            }
            const float bottom_h = 200.0f;
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
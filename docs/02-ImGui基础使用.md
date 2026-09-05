# 第二章 · ImGui 基础使用

> 目标：从 Hello World 起步，亲手把一个 ImGui 窗口跑起来，掌握核心 API 和布局，最后能定制主题。这是动手的第一章，代码请**自己敲**，敲不动再对照 [`ref/`](../ref)。

---

## 2.1 环境与项目搭建

### 2.1.1 你现有的东西

你的工作区已经准备好了：

- `libs/Imgui/` —— Dear ImGui 1.92.5 docking 分支的完整源码 + Win32/DX11 后端（已接入，**别动它**）。
- `Imgui_Study.pro` —— 一个只编译 `main.cpp` 的 Hello World qmake 工程。
- `main.cpp` —— 打印 `Hello World!` 的控制台程序。

你的任务是把这三样东西"接起来"，让 ImGui 真正跑起来。

### 2.1.2 最小集成需要编译哪些源文件

一个 ImGui + Win32 + DX11 的应用，至少要编译这些 `.cpp`：

| 文件 | 作用 |
|---|---|
| `libs/Imgui/imgui.cpp` | 核心 |
| `libs/Imgui/imgui_draw.cpp` | 绘图 |
| `libs/Imgui/imgui_tables.cpp` | 表格（核心依赖它，必须加） |
| `libs/Imgui/imgui_widgets.cpp` | 控件 |
| `libs/Imgui/imgui_demo.cpp` | 演示窗口（初期强烈建议加，用来验证） |
| `libs/Imgui/backends/imgui_impl_win32.cpp` | Win32 平台后端 |
| `libs/Imgui/backends/imgui_impl_dx11.cpp` | DX11 渲染后端 |
| `main.cpp` | 你的入口 |

### 2.1.3 重写 `Imgui_Study.pro`

用 **C++17**（不是 c++20），声明 Win32 相关宏，指定头文件搜索路径，链接 DX11 的导入库。

> 🖐 **自己动手**：把你现在的 `.pro` 改成下面的样子（可以先只加 `main.cpp`，等你写了模块再往里加）。

```qmake
TEMPLATE = app
CONFIG += console c++17
CONFIG -= app_bundle
CONFIG -= qt

DEFINES += UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX

INCLUDEPATH += libs/Imgui src

SOURCES += \
    libs/Imgui/imgui.cpp \
    libs/Imgui/imgui_draw.cpp \
    libs/Imgui/imgui_tables.cpp \
    libs/Imgui/imgui_widgets.cpp \
    libs/Imgui/imgui_demo.cpp \
    libs/Imgui/backends/imgui_impl_win32.cpp \
    libs/Imgui/backends/imgui_impl_dx11.cpp \
    main.cpp

LIBS += -ld3d11 -ldxgi -ld3dcompiler
```

几个关键点解释：

- `CONFIG -= qt`：我们不用 Qt 模块，这是纯 C++ 程序。
- `UNICODE _UNICODE`：让 Win32 API 使用宽字符版本（`CreateWindowW` 等），ImGui 的 Win32 后端就是这么用的。
- `WIN32_LEAN_AND_MEAN`：精简 Windows 头文件，减少编译时间、避免宏污染。
- `NOMINMAX`：禁止 Windows 定义 `min`/`max` 宏，否则会跟 C++ 标准库冲突。
- `-ld3d11 -ldxgi -ld3dcompiler`：链接 DirectX 的导入库。**MinGW 不认 MSVC 的 `#pragma comment(lib, ...)`**，所以必须在这里显式指定。

### 2.1.4 后端初始化的三段式

这是所有 ImGui 应用（无论什么后端）通用的三段式，请背下来：

```cpp
// ① 创建上下文
IMGUI_CHECKVERSION();
ImGui::CreateContext();
ImGuiIO& io = ImGui::GetIO();
io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // 键盘导航
io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;       // 停靠（docking 分支）
io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;     // 多视口（可选）

ImGui::StyleColorsDark();     // 深色主题

// ② 初始化后端（先平台，后渲染）
ImGui_ImplWin32_Init(hwnd);
ImGui_ImplDX11_Init(pd3dDevice, pd3dDeviceContext);

// ③ 主循环
while (!done) {
    // 处理消息 ...
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    /* 你的 UI 代码 */

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    /* Present ... */
}

// 退出清理
ImGui_ImplDX11_Shutdown();
ImGui_ImplWin32_Shutdown();
ImGui::DestroyContext();
```

> 注意：后端初始化的**顺序不能乱**——必须先有 `ImGui::CreateContext()`，才能初始化后端；渲染后端需要你先把 DX11 的 device 和 context 创建好再传进去。

### 2.1.5 Win32 窗口 + DX11 设备

这部分代码比较"样板化"，不同项目几乎一样。核心是四件事：

1. 注册窗口类 + 创建窗口（`RegisterClassExW` / `CreateWindowW`）；
2. 创建 DX11 设备与交换链（`D3D11CreateDeviceAndSwapChain`）；
3. `WndProc` 里把消息转发给 `ImGui_ImplWin32_WndProcHandler`；
4. 主循环里处理 `WM_QUIT`、窗口最小化、窗口尺寸变化。

> 🖐 **自己动手**：把 `main.cpp` 从 Hello World 改成下面的骨架（这是"里程碑 0"的完整目标）。先不追求理解每一行，重点是**跑起来、看到窗口**。完整的可运行版本见 [`ref/main.cpp`](../ref/main.cpp)，但请先自己写。

```cpp
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
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
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

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

        /* ===== 你的 UI 代码从这里开始 ===== */


        /* ===== 你的 UI 代码到这里结束 ===== */

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
```

DX11 设备创建与渲染目标管理的三个函数，参考官方 `example_win32_directx11` 的写法即可（`CreateDeviceD3D` 里用 `D3D11CreateDeviceAndSwapChain`，硬件失败时回退到 WARP 软件驱动；`CreateRenderTarget` 里从交换链取后备缓冲建 RTV）。

> ✅ **验证点 0**：编译通过、运行后看到一个空的深色窗口，能拖动、缩放、关闭。如果看不到窗口或崩溃，先检查 `.pro` 里 LIBS 是否写对、后端初始化顺序是否颠倒。常见报错见[第五章](05-常见问题与性能优化.md)。

---

## 2.2 核心 API 使用与参数说明

把下面的代码放进主循环里 `/* 你的 UI 代码 */` 的位置，然后逐行理解。

### 2.2.1 窗口与文本

```cpp
ImGui::Begin("Hello");                       // 开始窗口"Hello"
ImGui::Text("这是一行文本");                  // 格式化文本
ImGui::Text("value = %d", 42);               // 支持 printf 风格格式
ImGui::TextUnformatted("不做格式化，原样输出");  // 遇到 % 不会崩
ImGui::End();                                // 结束窗口
```

- `Begin(name)`：开始一个窗口。第二个参数 `bool* p_open` 可选，传了就带关闭按钮。
- `Text(fmt, ...)`：格式化文本。**注意**：如果字符串里有 `%` 必须用 `TextUnformatted`，否则会被当成格式化占位符。

### 2.2.2 按钮

```cpp
if (ImGui::Button("点我")) {
    // 这一帧按钮被点击时执行
}
ImGui::SameLine();
ImGui::Button("并排");   // SameLine 后放在同一行
```

`Button` 返回 `bool`：**点击的那一帧**返回 `true`。这是 ImGui 交互的通用模式——**每个控件都是"绘制 + 询问是否交互"二合一**。

### 2.2.3 状态控件（数据存在你的变量里）

```cpp
static bool   b = false;
static int    i = 0;
static float  f = 0.5f;
static char   text[256] = "hello";

ImGui::Checkbox("开关", &b);          // 勾选框，绑定 bool
ImGui::SliderInt("整数", &i, 0, 100); // 整型滑块，范围 [0,100]
ImGui::SliderFloat("浮点", &f, 0.0f, 1.0f);
ImGui::InputText("文本", text, sizeof(text));  // 文本输入框，绑定 char 缓冲区
```

**关键点**：这些控件的值都存在**你的变量**里（`b`、`i`、`f`、`text`），ImGui 只是"读写"它们。这就是即时模式的核心——没有控件对象替你保存状态。`static` 让变量跨帧存活（每帧都会执行这段代码，非 static 的局部变量每帧都会重新初始化）。

### 2.2.4 下拉框与颜色

```cpp
static const char* items[] = { "C++", "Python", "JSON", "Markdown" };
static int current = 0;
ImGui::Combo("语言", &current, items, IM_ARRAYSIZE(items));

static ImVec4 color = ImVec4(0.2f, 0.5f, 0.8f, 1.0f);
ImGui::ColorEdit3("颜色", &color.x);   // 编辑 RGB
ImGui::ColorEdit4("颜色+透明度", &color.x); // 编辑 RGBA
```

### 2.2.5 常用参数说明速查

| 函数 | 关键参数 | 说明 |
|---|---|---|
| `Begin(name, p_open, flags)` | `p_open` 关闭按钮指针；`flags` 窗口行为 | 如 `ImGuiWindowFlags_NoResize` |
| `Button(label, size)` | `size` 自定义按钮大小 | 默认按文本尺寸 |
| `InputText(label, buf, buf_size, flags)` | `buf`/`buf_size` 你的缓冲区 | 返回 true 表示内容被编辑 |
| `SliderFloat(label, &v, min, max, fmt)` | 范围 + 显示格式 | `"%.2f"` 保留两位 |
| `Checkbox(label, &b)` | 绑定 bool | |
| `Combo(label, &cur, items, count)` | `items` 是 `const char*[]` | |

> 想查某个函数的所有参数？直接看 `imgui.h` 里对应函数的注释，或 `ShowDemoWindow()` 里对应的示例，那是权威文档。

---

## 2.3 布局管理与窗口组织

### 2.3.1 流式布局

默认每个控件占一整行、竖着排。用 `SameLine` 让它们并排：

```cpp
ImGui::Button("A");
ImGui::SameLine();
ImGui::Button("B");
ImGui::SameLine();
ImGui::Button("C");
```

### 2.3.2 独立的子区域

`BeginChild` 创建一个带独立滚动条的子区域，适合做"侧边栏 + 主内容"：

```cpp
ImGui::BeginChild("left", ImVec2(200, 0), true);  // 固定宽 200，高填满剩余
// ... 左侧内容 ...
ImGui::EndChild();

ImGui::SameLine();

ImGui::BeginChild("right", ImVec2(0, 0), true);   // 宽高都填满剩余
// ... 右侧内容 ...
ImGui::EndChild();
```

### 2.3.3 表格布局

```cpp
if (ImGui::BeginTable("tbl", 3, ImGuiTableFlags_Borders)) {
    for (int row = 0; row < 3; ++row) {
        ImGui::TableNextRow();
        for (int col = 0; col < 3; ++col) {
            ImGui::TableSetColumnIndex(col);
            ImGui::Text("%d,%d", row, col);
        }
    }
    ImGui::EndTable();
}
```

### 2.3.4 停靠布局（DockSpace）

编辑器类软件的标准做法：让整个主窗口成为一个可停靠的空间，所有面板都能拖进去分栏。

```cpp
// 在初始化时（已经做了）：io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

// 每帧 UI 代码的最开始：
ImGui::DockSpaceOverViewport(ImGui::GetMainViewport());
```

之后你创建的每个窗口，都能拖到主窗口边缘吸附、分栏。要做"默认布局"（比如左侧文件树、中央编辑器），用 `DockBuilder`（见第三章实战）。

### 2.3.5 ID 冲突问题

ImGui 用"名字 + 位置"生成内部 ID 来区分控件。两个同名的 `BeginChild("left")` 或同名的按钮，可能发生 ID 冲突（状态串味）。解决办法：

- 给名字加 `##` 后缀，`##` 后面的部分不显示、只用于区分 ID：

```cpp
ImGui::Button("打开##A");
ImGui::Button("打开##B");   // 显示都是"打开"，但 ID 不同
```

- 或用 `PushID/PopID` 包裹一段代码：

```cpp
ImGui::PushID("item");
ImGui::Button("删除");   // ID 变成 "item/删除"
ImGui::PopID();
```

---

## 2.4 样式定制与主题

### 2.4.1 内置主题

```cpp
ImGui::StyleColorsDark();   // 深色（默认，推荐）
ImGui::StyleColorsLight();  // 浅色
ImGui::StyleColorsClassic();// 经典配色
```

### 2.4.2 手动调整样式

样式都在全局的 `ImGuiStyle` 里：

```cpp
ImGuiStyle& style = ImGui::GetStyle();
style.WindowRounding    = 8.0f;   // 窗口圆角
style.FrameRounding     = 4.0f;   // 控件圆角
style.FramePadding      = ImVec2(6, 4);  // 控件内边距
style.ItemSpacing       = ImVec2(8, 6);  // 控件间距
style.ScrollbarRounding = 4.0f;
```

### 2.4.3 自定义颜色

```cpp
ImVec4* colors = ImGui::GetStyle().Colors;
colors[ImGuiCol_Text]           = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
colors[ImGuiCol_WindowBg]       = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
colors[ImGuiCol_Button]         = ImVec4(0.20f, 0.20f, 0.25f, 1.00f);
colors[ImGuiCol_ButtonHovered]  = ImVec4(0.30f, 0.30f, 0.38f, 1.00f);
colors[ImGuiCol_ButtonActive]   = ImVec4(0.15f, 0.15f, 0.20f, 1.00f);
colors[ImGuiCol_TitleBg]        = ImVec4(0.14f, 0.14f, 0.18f, 1.00f);
colors[ImGuiCol_TitleBgActive]  = ImVec4(0.20f, 0.20f, 0.28f, 1.00f);
```

可用的颜色常量以 `ImGuiCol_` 开头，全部枚举在 `imgui.h` 里。想要好看的配色，可以参考 ImGui 官方的 `StyleColorsDark()` 实现，或者用社区现成的主题（如 ImGui 的 `misc/` 之外的一些配色表）。

### 2.4.4 加载字体（显示中文）

默认字体只含基本 ASCII 字符，**显示不了中文**。要显示中文，必须加载一个包含中文字形的字体：

```cpp
ImGuiIO& io = ImGui::GetIO();
io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 18.0f,
                             nullptr, io.Fonts->GetGlyphRangesChineseFull());
```

- 第一个参数：字体文件路径。Windows 自带的中文字体如 `C:\Windows\Fonts\msyh.ttc`（微软雅黑）、`simhei.ttf`（黑体）。
- 第二个参数：字号（像素）。
- 最后一个参数：字形范围。`GetGlyphRangesChineseFull()` 加载全部中文，`GetGlyphRangesChineseSimplifiedCommon()` 只加载常用简体字（内存更省）。

> 必须在 `ImGui_ImplXXX_Init` **之前**加载字体（或加载后调用重建），因为字体要光栅化进图集。我们的编辑器会加载一个等宽字体（如 `Consolas`）+ 一个中文字体，见第三章。

### 2.4.5 按窗口临时改样式

用 `PushStyleColor` / `PushStyleVar` / `PopStyleColor` / `PopStyleVar` 可以只对某段代码临时改样式（比如把某个按钮画成红色）：

```cpp
ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
ImGui::Button("危险操作");
ImGui::PopStyleColor();
```

---

## 2.5 本章小结与自测

**你应当已经掌握：**

1. 三段式初始化（上下文 → 后端 → 主循环），顺序不能乱。
2. `.pro` 里该编译哪些文件、链接哪些库、定义哪些宏。
3. 常用控件的用法——记住：**数据在你的变量里，交互靠返回值**。
4. 流式布局、`SameLine`、`BeginChild`、`DockSpace`。
5. 改样式、改颜色、加载中文字体。

**自测题：**

- 为什么 `static char text[256]` 要加 `static`？
- `InputText` 绑定的是谁的内存？
- 两个同名按钮怎么避免 ID 冲突？
- 想显示中文，关键一步是什么？

**动手任务**：把这一章的所有控件都放进你的主循环里跑一遍，做一个"控件实验台"窗口，用 `Checkbox` 控制"显示/隐藏"另一个窗口。做完后，进入[第四章 · 项目实现步骤](04-项目实现步骤.md)开始真正的编辑器开发，同时配合[第三章 · 文本编辑器实战](03-文本编辑器实战.md)理解每个模块的原理。

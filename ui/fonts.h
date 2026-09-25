#ifndef UI_FONTS_H
#define UI_FONTS_H

struct ImFont;

// ── 全局字体入口 ──────────────────────────────────────────
// main.cpp 在 ImGui 初始化阶段填充，各 UI 模块经此取字体，
// 避免把 ImFont* 一路透传。图标字体以 ASCII 字母为字形索引
// （'A'~'Z' 部分字母映射为图标），用法见 menu_widgets。
namespace fonts
{
    inline ImFont* regular = nullptr;   // 主字体（中文 zh-cn.ttf）
    inline ImFont* icon    = nullptr;   // 图标字体（libs/assets/ico_font.h 子集）

    // 从内存加载图标字体（须在 ImGui::CreateContext 之后、首帧之前调用一次）
    void load_icon();
}

#endif // UI_FONTS_H

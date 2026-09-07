#pragma once

#include "type/process_info.h"
#include "imgui.h"

#include <string>
#include <unordered_map>

struct ID3D11Device;

// 缓存进程图标为 D3D11 纹理，供进程列表 Image() 显示。
// 每个可执行文件路径只加载一次，避免每帧都重新提取图标。
class process_icon_cache {
public:
    static process_icon_cache& instance();

    // 必须在 ImGui_ImplDX11_Init 之后、渲染循环之前调用一次。
    void set_device(ID3D11Device* device);

    // 返回图标纹理 id；未就绪 / 加载失败返回 0（调用方显示占位）。
    ImTextureID icon_for(const process_info& p);

    void clear();

private:
    process_icon_cache() = default;

    ImTextureID load_icon(const std::wstring& path);

    ID3D11Device* device_ = nullptr;
    std::unordered_map<std::wstring, ImTextureID> cache_;
};

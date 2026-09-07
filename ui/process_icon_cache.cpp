#include "ui/process_icon_cache.h"

#include <d3d11.h>
#include <windows.h>
#include <shellapi.h>

#include <cstring>
#include <vector>

namespace {

// GDI 图标 HICON -> 原始 RGBA 像素（自顶向下）。
// 返回 true 表示成功并把像素填到 out（out 需至少 w*h*4 字节）。
bool icon_to_rgba(HICON icon, int w, int h, std::vector<uint8_t>& out)
{
    out.assign((size_t)w * h * 4, 0);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;   // 负值 = 自顶向下
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc)
        return false;

    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmp) {
        DeleteDC(hdc);
        return false;
    }

    HGDIOBJ old = SelectObject(hdc, hbmp);
    bool ok = DrawIconEx(hdc, 0, 0, icon, w, h, 0, nullptr, DI_NORMAL) != 0;
    SelectObject(hdc, old);

    if (ok)
        std::memcpy(out.data(), bits, out.size());

    DeleteObject(hbmp);
    DeleteDC(hdc);
    return ok;
}

} // namespace

process_icon_cache& process_icon_cache::instance()
{
    static process_icon_cache cache;
    return cache;
}

void process_icon_cache::set_device(ID3D11Device* device)
{
    device_ = device;
}

ImTextureID process_icon_cache::icon_for(const process_info& p)
{
    if (!device_)
        return 0;

    const std::wstring& path = p.image_path;
    if (path.empty())
        return 0;

    auto it = cache_.find(path);
    if (it != cache_.end())
        return it->second;

    ImTextureID id = load_icon(path);
    cache_[path] = id;
    return id;
}

void process_icon_cache::clear()
{
    // 纹理对象由调用方/设备负责释放；这里只清空 id 映射。
    cache_.clear();
}

ImTextureID process_icon_cache::load_icon(const std::wstring& path)
{
    // SHGetFileInfoW 返回的 HICON 需要 DestroyIcon 释放。
    SHFILEINFOW sfi = {};
    DWORD_PTR res = SHGetFileInfoW(path.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi,
                                   sizeof(sfi),
                                   SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES);
    if (!res || !sfi.hIcon)
        return 0;

    const int w = 16, h = 16;
    std::vector<uint8_t> pixels;
    if (!icon_to_rgba(sfi.hIcon, w, h, pixels)) {
        DestroyIcon(sfi.hIcon);
        return 0;
    }
    DestroyIcon(sfi.hIcon);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = w;
    desc.Height           = h;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem          = pixels.data();
    init.SysMemPitch      = w * 4;

    ID3D11Texture2D* tex = nullptr;
    if (device_->CreateTexture2D(&desc, &init, &tex) != S_OK || !tex)
        return 0;

    ID3D11ShaderResourceView* srv = nullptr;
    if (device_->CreateShaderResourceView(tex, nullptr, &srv) != S_OK || !srv) {
        tex->Release();
        return 0;
    }
    tex->Release();

    return (ImTextureID)(intptr_t)srv;
}

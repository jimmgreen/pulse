#include "menu_icon.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace pulse {
namespace {

BITMAPINFO BitmapInfo(uint32_t width, uint32_t height) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    return info;
}

} // namespace

HBITMAP MenuIconBitmap(const MenuIcon& icon) {
    if (!icon.Valid()) return nullptr;
    auto info = BitmapInfo(icon.width, icon.height);
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (bitmap && pixels) std::memcpy(pixels, icon.bgra.data(), icon.bgra.size());
    return bitmap;
}

MenuIcon ReadMenuBitmap(HBITMAP bitmap) {
    BITMAP object{};
    // GDI handles can be sign-extended on x64; only the reserved values are special.
    if (reinterpret_cast<UINT_PTR>(bitmap) <= 11 || bitmap == HBMMENU_CALLBACK ||
        !GetObjectW(bitmap, sizeof(object), &object) ||
        object.bmWidth <= 0 || object.bmWidth > 64 ||
        object.bmHeight <= 0 || object.bmHeight > 64) return {};
    MenuIcon icon;
    icon.width = static_cast<uint32_t>(object.bmWidth);
    icon.height = static_cast<uint32_t>(object.bmHeight);
    icon.bgra.resize(static_cast<size_t>(icon.width) * icon.height * 4);
    auto info = BitmapInfo(icon.width, icon.height);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return {};
    const int rows = GetDIBits(dc, bitmap, 0, icon.height, icon.bgra.data(), &info, DIB_RGB_COLORS);
    DeleteDC(dc);
    if (rows != static_cast<int>(icon.height)) return {};
    bool has_alpha = false, straight = false;
    for (size_t i = 0; i < icon.bgra.size(); i += 4) {
        const auto alpha = icon.bgra[i + 3];
        has_alpha = has_alpha || alpha != 0;
        straight = straight || icon.bgra[i] > alpha || icon.bgra[i + 1] > alpha ||
            icon.bgra[i + 2] > alpha;
    }
    for (size_t i = 0; i < icon.bgra.size(); i += 4) {
        if (!has_alpha) icon.bgra[i + 3] = 255; // Legacy GDI bitmaps have no alpha.
        else if (straight) {
            for (size_t c = 0; c < 3; ++c)
                icon.bgra[i + c] = static_cast<uint8_t>((icon.bgra[i + c] * icon.bgra[i + 3] + 127) / 255);
        }
    }
    return icon;
}

MenuIcon MenuIconFromBackgrounds(uint32_t width, uint32_t height,
                                 const uint8_t* black, const uint8_t* white) {
    if (!width || !height || width > 64 || height > 64 || !black || !white) return {};
    MenuIcon icon{width, height, std::vector<uint8_t>(static_cast<size_t>(width) * height * 4)};
    bool visible = false;
    for (size_t i = 0; i < icon.bgra.size(); i += 4) {
        int difference = 0;
        for (size_t c = 0; c < 3; ++c)
            difference = std::max(difference, static_cast<int>(white[i + c]) - black[i + c]);
        const auto alpha = static_cast<uint8_t>(255 - std::clamp(difference, 0, 255));
        for (size_t c = 0; c < 3; ++c) icon.bgra[i + c] = std::min(black[i + c], alpha);
        icon.bgra[i + 3] = alpha;
        visible = visible || alpha != 0;
    }
    return visible ? icon : MenuIcon{};
}

MenuIcon LoadMenuIcon(const std::wstring& location) {
    if (location.empty() || location.size() > 32767) return {};
    const DWORD needed = ExpandEnvironmentStringsW(location.c_str(), nullptr, 0);
    if (!needed || needed > 32768) return {};
    std::wstring path(needed, L'\0');
    if (!ExpandEnvironmentStringsW(location.c_str(), path.data(), needed)) return {};
    path.resize(needed - 1);
    if (!path.empty() && path.front() == L'@') path.erase(0, 1);
    const int index = PathParseIconLocationW(path.data());
    path.resize(wcslen(path.c_str()));
    HICON handle = nullptr;
    if (FAILED(SHDefExtractIconW(path.c_str(), index, 0, &handle, nullptr, 32)) || !handle) {
        handle = static_cast<HICON>(LoadImageW(nullptr, path.c_str(), IMAGE_ICON, 32, 32, LR_LOADFROMFILE));
    }
    if (!handle) return {};
    MenuIcon black{32, 32, std::vector<uint8_t>(32 * 32 * 4, 0)};
    MenuIcon white{32, 32, std::vector<uint8_t>(32 * 32 * 4, 255)};
    HBITMAP black_bitmap = MenuIconBitmap(black), white_bitmap = MenuIconBitmap(white);
    HDC dc = CreateCompatibleDC(nullptr);
    bool drawn = false;
    if (dc && black_bitmap && white_bitmap) {
        HGDIOBJ old = SelectObject(dc, black_bitmap);
        drawn = DrawIconEx(dc, 0, 0, handle, 32, 32, 0, nullptr, DI_NORMAL) != FALSE;
        SelectObject(dc, white_bitmap);
        drawn = DrawIconEx(dc, 0, 0, handle, 32, 32, 0, nullptr, DI_NORMAL) != FALSE && drawn;
        SelectObject(dc, old);
        GdiFlush();
        black = ReadMenuBitmap(black_bitmap);
        white = ReadMenuBitmap(white_bitmap);
    }
    if (dc) DeleteDC(dc);
    if (black_bitmap) DeleteObject(black_bitmap);
    if (white_bitmap) DeleteObject(white_bitmap);
    DestroyIcon(handle);
    return drawn && black.Valid() && white.Valid()
        ? MenuIconFromBackgrounds(32, 32, black.bgra.data(), white.bgra.data()) : MenuIcon{};
}

} // namespace pulse

#include "menu_icon.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

namespace pulse::common {

// Icons with an alpha channel use it; classic ones take the AND mask.
// Monochrome icons are skipped.
MenuIconRef IconPixels(HICON icon) {
    if (!icon) return nullptr;
    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) return nullptr;
    MenuIconRef result;
    BITMAP color{};
    if (info.hbmColor && GetObjectW(info.hbmColor, sizeof(color), &color) &&
        color.bmWidth > 0 && color.bmHeight > 0 &&
        color.bmWidth <= static_cast<LONG>(kMaxMenuIconEdge) &&
        color.bmHeight <= static_cast<LONG>(kMaxMenuIconEdge)) {
        auto pixels = std::make_shared<MenuIconPixels>();
        pixels->width = static_cast<uint32_t>(color.bmWidth);
        pixels->height = static_cast<uint32_t>(color.bmHeight);
        const size_t count = static_cast<size_t>(pixels->width) * pixels->height;
        pixels->bgra.resize(count * 4u);
        std::vector<uint8_t> mask(count * 4u);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = color.bmWidth;
        bi.bmiHeader.biHeight = -color.bmHeight;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        if (HDC dc = CreateCompatibleDC(nullptr)) {
            const UINT rows = static_cast<UINT>(color.bmHeight);
            const bool ok = GetDIBits(dc, info.hbmColor, 0, rows, pixels->bgra.data(), &bi,
                                      DIB_RGB_COLORS) == color.bmHeight;
            const bool has_mask = info.hbmMask &&
                GetDIBits(dc, info.hbmMask, 0, rows, mask.data(), &bi, DIB_RGB_COLORS) ==
                    color.bmHeight;
            DeleteDC(dc);
            if (ok) {
                auto& bgra = pixels->bgra;
                bool any_alpha = false;
                for (size_t i = 3; i < bgra.size(); i += 4) any_alpha = any_alpha || bgra[i] != 0;
                if (!any_alpha) {
                    // 24bpp-style icon: the mask (white = transparent) is the alpha.
                    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
                        const bool opaque = !has_mask || mask[i] == 0;
                        bgra[i + 3] = opaque ? 255 : 0;
                        if (!opaque) bgra[i] = bgra[i + 1] = bgra[i + 2] = 0;
                    }
                } else {
                    // GetIconInfo hands out straight alpha; D2D wants premultiplied.
                    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
                        const uint32_t a = bgra[i + 3];
                        if (a == 255) continue;
                        for (size_t c = 0; c < 3; ++c)
                            bgra[i + c] = static_cast<uint8_t>((bgra[i + c] * a + 127u) / 255u);
                    }
                }
                result = std::move(pixels);
            }
        }
    }
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return result;
}

bool ParseIconReference(const std::wstring& reference, std::wstring& file, int& index) {
    if (reference.empty() || reference.size() >= MAX_PATH * 2 || reference[0] == L'@') return false;
    wchar_t buffer[MAX_PATH * 2]{};
    wcsncpy_s(buffer, reference.c_str(), _TRUNCATE);
    index = PathParseIconLocationW(buffer);
    PathUnquoteSpacesW(buffer);
    wchar_t expanded[MAX_PATH * 2]{};
    const DWORD n = ExpandEnvironmentStringsW(buffer, expanded, ARRAYSIZE(expanded));
    file = n > 0 && n <= ARRAYSIZE(expanded) ? expanded : buffer;
    return !file.empty();
}

MenuIconRef LoadIconPixels(const std::wstring& reference, uint32_t edge) {
    std::wstring file;
    int index = 0;
    if (edge == 0 || edge > kMaxMenuIconEdge || !ParseIconReference(reference, file, index)) return nullptr;
    HICON icon = nullptr;
    if (FAILED(SHDefExtractIconW(file.c_str(), index, 0, &icon, nullptr,
                                 MAKELONG(edge, edge))) || !icon)
        return nullptr;
    MenuIconRef pixels = IconPixels(icon);
    DestroyIcon(icon);
    return pixels;
}

} // namespace pulse::common

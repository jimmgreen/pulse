#pragma once
#include <windows.h>
#include <shobjidl.h>
#include <algorithm>
#include <cstring>
#include "../common/menu_icon.h"

namespace pulse::shell {
inline pulse::common::MenuIconRef MenuBitmapPixels(HBITMAP bitmap) {
    BITMAP info{};
    if (!GetObjectW(bitmap, sizeof(info), &info) || info.bmWidth <= 0 ||
        info.bmHeight <= 0 ||
        info.bmWidth > static_cast<LONG>(pulse::common::kMaxMenuIconEdge) ||
        info.bmHeight > static_cast<LONG>(pulse::common::kMaxMenuIconEdge))
        return nullptr;
    auto pixels = std::make_shared<pulse::common::MenuIconPixels>();
    pixels->width = static_cast<uint32_t>(info.bmWidth);
    pixels->height = static_cast<uint32_t>(info.bmHeight);
    pixels->bgra.resize(static_cast<size_t>(pixels->width) * pixels->height * 4u);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = info.bmWidth;
    bi.bmiHeader.biHeight = -info.bmHeight;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return nullptr;
    const int lines = GetDIBits(dc, bitmap, 0, static_cast<UINT>(info.bmHeight),
                                pixels->bgra.data(), &bi, DIB_RGB_COLORS);
    DeleteDC(dc);
    if (lines != info.bmHeight) return nullptr;
    auto& bgra = pixels->bgra;
    bool any_alpha = false, straight = false;
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
        const uint8_t a = bgra[i + 3];
        if (a) any_alpha = true;
        if (bgra[i] > a || bgra[i + 1] > a || bgra[i + 2] > a) straight = true;
    }
    if (info.bmBitsPixel != 32 || !any_alpha) {
        for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;
    } else if (straight) {
        for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
            const uint32_t a = bgra[i + 3];
            for (size_t c = 0; c < 3; ++c)
                bgra[i + c] = static_cast<uint8_t>((bgra[i + c] * a + 127u) / 255u);
        }
    }
    return pixels;
}

inline bool DispatchMenuIconMessage(IContextMenu2* menu2, IContextMenu3* menu3,
                                    UINT message, LPARAM data) {
    __try {
        if (menu3) {
            LRESULT result = 0;
            return menu3->HandleMenuMsg2(message, 0, data, &result) == S_OK;
        }
        return menu2 && menu2->HandleMenuMsg(message, 0, data) == S_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline pulse::common::MenuIconRef CallbackMenuIcon(HMENU menu, const MENUITEMINFOW& item,
                                                  IContextMenu2* menu2, IContextMenu3* menu3) {
    if (!menu2 && !menu3) return {};
    MEASUREITEMSTRUCT measure{};
    measure.CtlType = ODT_MENU;
    measure.itemID = item.wID;
    measure.itemData = item.dwItemData;
    measure.itemWidth = measure.itemHeight = static_cast<UINT>(GetSystemMetrics(SM_CYMENUCHECK));
    if (!DispatchMenuIconMessage(menu2, menu3, WM_MEASUREITEM, reinterpret_cast<LPARAM>(&measure)) ||
        !measure.itemWidth || !measure.itemHeight ||
        measure.itemWidth > common::kMaxMenuIconEdge || measure.itemHeight > common::kMaxMenuIconEdge)
        return {};
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(measure.itemWidth);
    bi.bmiHeader.biHeight = -static_cast<LONG>(measure.itemHeight);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return {};
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) { DeleteDC(dc); return {}; }
    HGDIOBJ previous = SelectObject(dc, bitmap);
    DRAWITEMSTRUCT draw{};
    draw.CtlType = ODT_MENU;
    draw.itemID = item.wID;
    draw.itemAction = ODA_DRAWENTIRE;
    draw.itemState = (item.fState & (MFS_DISABLED | MFS_GRAYED)) ? ODS_DISABLED : 0;
    if (item.fState & MFS_CHECKED) draw.itemState |= ODS_CHECKED;
    draw.hwndItem = reinterpret_cast<HWND>(menu);
    draw.hDC = dc;
    draw.rcItem = {0, 0, static_cast<LONG>(measure.itemWidth), static_cast<LONG>(measure.itemHeight)};
    draw.itemData = item.dwItemData;
    const size_t bytes = static_cast<size_t>(measure.itemWidth) * measure.itemHeight * 4;
    auto pixels = std::make_shared<common::MenuIconPixels>();
    pixels->width = measure.itemWidth;
    pixels->height = measure.itemHeight;
    pixels->bgra.resize(bytes);
    // Two provider draws recover coverage for legacy GDI icons without an
    // alpha channel. Untouched background remains transparent, not a box.
    PatBlt(dc, 0, 0, measure.itemWidth, measure.itemHeight, BLACKNESS);
    bool ok = DispatchMenuIconMessage(menu2, menu3, WM_DRAWITEM, reinterpret_cast<LPARAM>(&draw));
    GdiFlush();
    std::memcpy(pixels->bgra.data(), bits, bytes);
    PatBlt(dc, 0, 0, measure.itemWidth, measure.itemHeight, WHITENESS);
    ok = ok && DispatchMenuIconMessage(menu2, menu3, WM_DRAWITEM, reinterpret_cast<LPARAM>(&draw));
    GdiFlush();
    const auto* white = static_cast<const uint8_t*>(bits);
    bool painted = false;
    for (size_t i = 0; i < bytes; i += 4) {
        int background = 0;
        for (size_t c = 0; c < 3; ++c)
            background = (std::max)(background, int(white[i + c]) - int(pixels->bgra[i + c]));
        const auto alpha = static_cast<uint8_t>(255 - background);
        pixels->bgra[i + 3] = alpha;
        painted = painted || alpha != 0;
        for (size_t c = 0; c < 3; ++c) pixels->bgra[i + c] = (std::min)(pixels->bgra[i + c], alpha);
    }
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return ok && painted ? pixels : common::MenuIconRef{};
}

inline pulse::common::MenuIconRef MenuItemIcon(HMENU menu, UINT pos,
                                              IContextMenu2* menu2 = nullptr,
                                              IContextMenu3* menu3 = nullptr) {
    MENUITEMINFOW item{sizeof(item)};
    item.fMask = MIIM_BITMAP | MIIM_CHECKMARKS | MIIM_STATE | MIIM_ID | MIIM_DATA;
    if (!GetMenuItemInfoW(menu, pos, TRUE, &item)) return {};
    if (item.hbmpItem == HBMMENU_CALLBACK) return CallbackMenuIcon(menu, item, menu2, menu3);
    HBITMAP bitmap = item.hbmpItem;
    // Older handlers attach their software icon through SetMenuItemBitmaps.
    // Explorer paints the state-appropriate checked/unchecked bitmap too.
    if (!bitmap) bitmap = (item.fState & MFS_CHECKED) ? item.hbmpChecked : item.hbmpUnchecked;
    if (!bitmap || reinterpret_cast<ULONG_PTR>(bitmap) <=
                   reinterpret_cast<ULONG_PTR>(HBMMENU_POPUP_MINIMIZE) || bitmap == HBMMENU_CALLBACK)
        return {};
    return MenuBitmapPixels(bitmap);
}

} // namespace pulse::shell


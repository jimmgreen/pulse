#ifdef PULSE_BASELINE_MENU_ICONS
#include "../../bench_data/context-menu-20261125/icons/baseline_ctx_menu_icon.h"
#else
#include "../shell_host/ctx_menu_icon.h"
#endif
#include <cstdio>

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
class Callback final : public IContextMenu3 {
public:
    bool fail = false;
    int draws = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU, UINT, UINT, UINT, UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR, UINT, UINT*, LPSTR, UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE HandleMenuMsg(UINT message, WPARAM wp, LPARAM lp) override {
        return HandleMenuMsg2(message, wp, lp, nullptr);
    }
    HRESULT STDMETHODCALLTYPE HandleMenuMsg2(UINT message, WPARAM, LPARAM lp, LRESULT*) override {
        if (fail) return E_FAIL;
        if (message == WM_MEASUREITEM) {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            measure->itemWidth = measure->itemHeight = 4;
            return S_OK;
        }
        if (message == WM_DRAWITEM) {
            auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            Check(draw->CtlType == ODT_MENU && draw->itemID == 73 && draw->itemData == 0x1234,
                  "callback receives original menu ID and provider data");
            SetPixelV(draw->hDC, 1, 1, RGB(32, 64, 128));
            ++draws;
            return S_OK;
        }
        return E_NOTIMPL;
    }
};
}

int main() {
    using namespace pulse;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 2; bi.bmiHeader.biHeight = -2;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) { std::printf("CreateDIBSection failed: %lu\n", GetLastError()); return 2; }
    const uint8_t rgba[] = {16,32,64,255, 10,20,30,255, 40,50,60,255, 1,2,3,255};
    std::memcpy(bits, rgba, sizeof(rgba));
    HMENU menu = CreatePopupMenu();
    HMENU child = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 70, L"Bitmap provider");
    AppendMenuW(menu, MF_STRING, 71, L"Legacy unchecked provider");
    AppendMenuW(menu, MF_STRING | MF_CHECKED, 72, L"Legacy checked provider");
    AppendMenuW(menu, MF_STRING, 73, L"Callback provider");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(child), L"Nested provider");
    AppendMenuW(child, MF_STRING, 74, L"Nested legacy provider");
    MENUITEMINFOW info{sizeof(info)};
    info.fMask = MIIM_BITMAP; info.hbmpItem = bitmap;
    SetMenuItemInfoW(menu, 0, TRUE, &info);
    SetMenuItemBitmaps(menu, 1, MF_BYPOSITION, bitmap, nullptr);
    SetMenuItemBitmaps(menu, 2, MF_BYPOSITION, nullptr, bitmap);
    SetMenuItemBitmaps(child, 0, MF_BYPOSITION, bitmap, nullptr);
    info.fMask = MIIM_BITMAP | MIIM_DATA; info.hbmpItem = HBMMENU_CALLBACK; info.dwItemData = 0x1234;
    SetMenuItemInfoW(menu, 3, TRUE, &info);
    auto same_pixels = [&](common::MenuIconRef pixels) {
        return pixels && pixels->Valid() && pixels->bgra == std::vector<uint8_t>(rgba, rgba + sizeof(rgba));
    };
    Check(same_pixels(shell::MenuItemIcon(menu, 0)), "MIIM_BITMAP preserves real provider pixels");
    Check(same_pixels(shell::MenuItemIcon(menu, 1)), "MIIM_CHECKMARKS captures unchecked provider software icon");
    Check(same_pixels(shell::MenuItemIcon(menu, 2)), "MIIM_CHECKMARKS captures checked provider software icon");
    Check(same_pixels(shell::MenuItemIcon(child, 0)), "native child menu captures legacy provider icon");
    Callback callback;
    auto captured = shell::MenuItemIcon(menu, 3, &callback, &callback);
    Check(captured && captured->width == 4 && captured->height == 4 &&
          captured->bgra[20] == 128 && captured->bgra[21] == 64 &&
          captured->bgra[22] == 32 && captured->bgra[23] == 255 &&
          captured->bgra[3] == 0 && callback.draws == 2,
          "HBMMENU_CALLBACK captures original provider drawing with transparent background");
    callback.fail = true;
    Check(!shell::MenuItemIcon(menu, 3, &callback, &callback), "failed callback leaves no fabricated icon");
    Check(!shell::MenuItemIcon(menu, 4), "missing provider bitmap leaves no fabricated icon");
    DestroyMenu(menu);
    DeleteObject(bitmap);
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}

#include "ctx_menu_icons.h"

namespace pulse::shell {
namespace {

bool SafeDraw(IContextMenu2* menu2, IContextMenu3* menu3, DRAWITEMSTRUCT* draw) {
    __try {
        if (menu3) {
            LRESULT result = 0;
            return menu3->HandleMenuMsg2(WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(draw), &result) == S_OK;
        }
        return menu2 && menu2->HandleMenuMsg(WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(draw)) == S_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

MenuIcon CollectMenuIcon(IContextMenu2* menu2, IContextMenu3* menu3,
                         HMENU menu, const MENUITEMINFOW& item) {
    if (item.hbmpItem && item.hbmpItem != HBMMENU_CALLBACK)
        return ReadMenuBitmap(item.hbmpItem);
    if (item.hbmpItem != HBMMENU_CALLBACK && !(item.fType & MFT_OWNERDRAW)) return {};
    if (!menu2 && !menu3) return {};
    // Some legacy extensions draw their icon on demand instead of exposing a bitmap.
    MenuIcon black{32, 32, std::vector<uint8_t>(32 * 32 * 4, 0)};
    MenuIcon white{32, 32, std::vector<uint8_t>(32 * 32 * 4, 255)};
    HBITMAP black_bitmap = MenuIconBitmap(black), white_bitmap = MenuIconBitmap(white);
    HDC dc = CreateCompatibleDC(nullptr);
    bool drawn = false;
    if (dc && black_bitmap && white_bitmap) {
        DRAWITEMSTRUCT draw{};
        draw.CtlType = ODT_MENU;
        draw.itemID = item.wID;
        draw.itemAction = ODA_DRAWENTIRE;
        draw.itemState = (item.fState & (MFS_DISABLED | MFS_GRAYED)) ? ODS_DISABLED : 0;
        draw.hwndItem = reinterpret_cast<HWND>(menu);
        draw.hDC = dc;
        draw.rcItem = {0, 0, 32, 32};
        draw.itemData = item.dwItemData;
        HGDIOBJ old = SelectObject(dc, black_bitmap);
        const int saved_black = SaveDC(dc);
        drawn = SafeDraw(menu2, menu3, &draw);
        if (saved_black) RestoreDC(dc, saved_black);
        SelectObject(dc, white_bitmap);
        const int saved_white = SaveDC(dc);
        drawn = SafeDraw(menu2, menu3, &draw) && drawn;
        if (saved_white) RestoreDC(dc, saved_white);
        SelectObject(dc, old);
        GdiFlush();
        black = ReadMenuBitmap(black_bitmap);
        white = ReadMenuBitmap(white_bitmap);
    }
    if (dc) DeleteDC(dc);
    if (black_bitmap) DeleteObject(black_bitmap);
    if (white_bitmap) DeleteObject(white_bitmap);
    return drawn && black.Valid() && white.Valid()
        ? MenuIconFromBackgrounds(32, 32, black.bgra.data(), white.bgra.data()) : MenuIcon{};
}

} // namespace pulse::shell

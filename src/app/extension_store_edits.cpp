#include "extension_store_edits.h"
#include "app_state.h"
#include "extension_store_seven_zip.h"
#include "../common/json_value.h"
#include "../common/localization.h"
#include "../ui/edit_host.h"
#include "../ui/FluentTokens.h"
#include "../ui/typography.h"
#include "../ui/ui_renderer.h"
#include "../ui/ui_view_model.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>

namespace pulse {
namespace {
constexpr UINT_PTR kSubclass = 0x53544544;
struct Edits {
    HWND search = nullptr, folder = nullptr;
    HFONT font = nullptr;
    HBRUSH brush = nullptr;
    COLORREF background = CLR_INVALID;
    float scale = 0;
    bool updating = false, dirty = false, destroying = false, composing = false;
    std::wstring folder_value;
};
// Window-thread owned; ReleaseOwner must call DestroyStoreEdits before AppState dies.
std::unordered_map<AppState*, std::unique_ptr<Edits>> owners;
Edits* Find(AppState& s) {
    const auto it = owners.find(&s);
    return it == owners.end() ? nullptr : it->second.get();
}
COLORREF Color(D2D1_COLOR_F c) {
    const auto byte = [](float value) { return static_cast<BYTE>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255)); };
    return RGB(byte(c.r), byte(c.g), byte(c.b));
}
void Colors(AppState& s, D2D1_COLOR_F& back, D2D1_COLOR_F& fore) {
    const auto theme = ui::MakeTheme(s.darkMode, s.accentColor);
    // Hosted EDIT fields use the same opaque fill as Painter::TextFieldFillColor.
    back = theme.fill_input_focus;
    fore = theme.text;
}
std::wstring Text(HWND edit) {
    const int length = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(edit, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}
void Redraw(AppState& s) { InvalidateRect(s.hwnd, nullptr, FALSE); }
void Commit(AppState& s, Edits& edits) {
    if (edits.destroying || edits.updating || !edits.dirty || edits.composing) return;
    const auto value = Text(edits.folder);
    if (SetSevenZipState(s, "folder", json::Utf8(value))) {
        edits.folder_value = value;
        edits.dirty = false;
        s.store_saved_at = GetTickCount64();
    }
    Redraw(s);
}
LRESULT CALLBACK EditProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto& s = *reinterpret_cast<AppState*>(ref);
    auto* edits = Find(s);
    if (!edits) return DefSubclassProc(hwnd, message, wp, lp);
    if (message == WM_GETDLGCODE) return DefSubclassProc(hwnd, message, wp, lp) | DLGC_WANTALLKEYS;
    if (message == WM_IME_STARTCOMPOSITION) edits->composing = true;
    if (message == WM_IME_ENDCOMPOSITION) edits->composing = false;
    if (message == WM_KEYDOWN && !edits->composing) {
        if (wp == VK_RETURN) {
            if (hwnd == edits->folder) Commit(s, *edits);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            if (hwnd == edits->folder) {
                edits->updating = true;
                SetWindowTextW(hwnd, edits->folder_value.c_str());
                edits->dirty = false;
                edits->updating = false;
            }
            SetFocus(s.hwnd);
            Redraw(s);
            return 0;
        }
        if (wp == VK_TAB) {
            HWND next = GetNextDlgTabItem(s.hwnd, hwnd, GetKeyState(VK_SHIFT) < 0);
            SetFocus(next && IsWindowVisible(next) ? next : s.hwnd);
            return 0;
        }
    }
    if (message == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB)) return 0;
    if (message == WM_CHAR && wp == 1) { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
    D2D1_COLOR_F back{}, fore{};
    Colors(s, back, fore);
    LRESULT result = 0;
    const bool handled = ui::HandleChildEditMessage(s.compositor, s.compositor.TextFormat(), fore, back,
        StoreEditsCtlColor(s, nullptr, hwnd), hwnd, message, wp, lp, result);
    if (!handled) result = ui::DefPresentedChildEditProc(s.compositor, s.compositor.TextFormat(),
                                                        fore, back, hwnd, message, wp, lp);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, EditProc, kSubclass);
        if (edits->search == hwnd) edits->search = nullptr;
        if (edits->folder == hwnd) edits->folder = nullptr;
        return result;
    }
    if (!edits->updating && !edits->destroying) {
        switch (message) {
        case WM_CHAR: case WM_KEYDOWN: case WM_PASTE: case WM_CUT: case WM_CLEAR:
        case WM_UNDO: case EM_UNDO: case EM_REPLACESEL: case WM_SETTEXT:
        case WM_IME_COMPOSITION: case WM_IME_ENDCOMPOSITION: {
            const auto value = Text(hwnd);
            if (hwnd == edits->search && s.store_search != value) {
                s.store_search = value;
                s.settings.SetScroll(0, 0);
                Redraw(s);
            } else if (hwnd == edits->folder) {
                edits->dirty = value != edits->folder_value;
            }
            break;
        }
        default: break;
        }
        if (message == WM_KILLFOCUS && hwnd == edits->folder) Commit(s, *edits);
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS) Redraw(s);
    }
    return result;
}
void Hide(AppState& s, HWND edit) {
    if (!edit) return;
    if (GetFocus() == edit) SetFocus(s.hwnd);
    if (GetWindowLongPtrW(edit, GWL_STYLE) & WS_VISIBLE) ShowWindow(edit, SW_HIDE);
}
}
HBRUSH StoreEditsCtlColor(AppState& s, HDC hdc, HWND edit) {
    auto* edits = Find(s);
    if (!edits || !edit || (edit != edits->search && edit != edits->folder)) return nullptr;
    D2D1_COLOR_F back{}, fore{};
    Colors(s, back, fore);
    const auto color = Color(back);
    if (!edits->brush || edits->background != color) {
        if (edits->brush) DeleteObject(edits->brush);
        edits->brush = CreateSolidBrush(color);
        edits->background = color;
    }
    if (hdc) { SetTextColor(hdc, Color(fore)); SetBkColor(hdc, color); SetBkMode(hdc, OPAQUE); }
    return edits->brush;
}
void SyncStoreEdits(AppState& s, const ui::WindowViewModel& vm) {
    const auto& view = vm.settings_store_view;
    const bool store = vm.settings_open && vm.settings_page == 8;
    const bool search = store && !view.detail;
    const bool folder = store && view.detail && view.detail_tab == 0 && view.seven_zip.available && view.seven_zip.location == 1;
    auto* edits = Find(s);
    if (!search && !folder) {
        if (edits) { Hide(s, edits->search); Hide(s, edits->folder); }
        return;
    }
    if (!edits) {
        auto value = std::make_unique<Edits>();
        edits = value.get(); owners.emplace(&s, std::move(value));
    }
    auto ensure = [&](HWND& edit) {
        if (edit) return;
        edit = ui::CreateChildEdit(s.hwnd);
        if (!edit) return;
        SetWindowTheme(edit, L"", L"");
        SendMessageW(edit, EM_LIMITTEXT, 32767, 0);
        SetWindowSubclass(edit, EditProc, kSubclass, reinterpret_cast<DWORD_PTR>(&s));
    };
    if (search) ensure(edits->search);
    if (folder) ensure(edits->folder);
    if (!edits->font || edits->scale != s.scale) {
        HFONT replacement = ui::typography::CreateEditFont(s.scale);
        if (replacement) {
            HFONT previous = edits->font; edits->font = replacement; edits->scale = s.scale;
            for (HWND edit : {edits->search, edits->folder}) if (edit) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(replacement), TRUE);
            if (previous) DeleteObject(previous);
        }
    }
    auto place = [&](HWND edit, int field, bool visible, const std::wstring& value) {
        if (!edit) return;
        auto bounds = s.renderer.SettingsStoreField(field);
        const auto clip = s.renderer.SettingsStoreClip();
        if (!visible || bounds.right <= bounds.left || bounds.bottom <= bounds.top ||
            bounds.left < clip.left || bounds.right > clip.right || bounds.top < clip.top || bounds.bottom > clip.bottom) {
            Hide(s, edit); return;
        }
        const auto& panel = s.renderer.AiPanel();
        if (panel.FloatingVisible() && (panel.Contains(bounds.left, bounds.top) || panel.Contains(bounds.right, bounds.top) ||
            panel.Contains(bounds.left, bounds.bottom) || panel.Contains(bounds.right, bounds.bottom))) { Hide(s, edit); return; }
        if (GetFocus() != edit && !(field == 1 && edits->dirty) && Text(edit) != value) {
            edits->updating = true; SetWindowTextW(edit, value.c_str()); edits->updating = false;
        }
        if (field == 1 && !edits->dirty && GetFocus() != edit) edits->folder_value = value;
        const auto hint = l10n::Get(field == 0 ? l10n::StringId::StoreSearchHint : l10n::StringId::SzFolderHint);
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(hint.c_str()));
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(edits->font), FALSE);
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        bounds.left += (field == 0 ? 32.0f : 10.0f) * s.scale;
        bounds.right -= 10.0f * s.scale;
        const int height = ui::EditLineHeight(edit, edits->font, (std::max)(1, static_cast<int>(bounds.bottom - bounds.top - 4*s.scale)));
        const int x = static_cast<int>(std::lround(bounds.left));
        const int y = static_cast<int>(std::lround((bounds.top + bounds.bottom - height) * .5f));
        const int width = (std::max)(1, static_cast<int>(std::lround(bounds.right - bounds.left)));
        RECT current{}; GetWindowRect(edit, &current); MapWindowPoints(nullptr, s.hwnd, reinterpret_cast<POINT*>(&current), 2);
        if (!IsWindowVisible(edit) || current.left != x || current.top != y || current.right - current.left != width || current.bottom - current.top != height)
            SetWindowPos(edit, HWND_TOP, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        D2D1_COLOR_F back{}, fore{}; Colors(s, back, fore);
        ui::PresentChildEdit(s.compositor, s.compositor.TextFormat(), fore, back, edit);
    };
    place(edits->search, 0, search, s.store_search);
    place(edits->folder, 1, folder, view.seven_zip.folder);
}
void DestroyStoreEdits(AppState& s) {
    auto* edits = Find(s);
    if (!edits) return;
    edits->destroying = true;
    if (edits->search) DestroyWindow(edits->search);
    if (edits->folder) DestroyWindow(edits->folder);
    if (edits->font) DeleteObject(edits->font);
    if (edits->brush) DeleteObject(edits->brush);
    owners.erase(&s);
}
}

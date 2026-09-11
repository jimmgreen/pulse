#include "../ui/edit_host.h"
// app_hosted_edit.cpp — extracted from app_main.cpp.
#include "app_internal.h"
#include "tab_shortcuts.h"
#include "../ui/address_search_layout.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/fluent_menu.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../ui/quick_preview_window.h"
#include "../ui/typography.h"
#include "../ui/color_picker.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include "../common/diagnostics_exporter.h"
#include "snapshot_patch.h"
#include "session.h"
#include "context_menu.h"
#include "batch_rename.h"
#include "link_resolve.h"
#include "resource.h"
#include "../ops/clipboard.h"
#include "../ipc/ctx_menu_util.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <thread>
#include <unordered_set>

using namespace pulse;

namespace pulse {
std::wstring FormatAddressPath(const std::wstring& text) {
    std::wstring t = text;
    if (t.empty()) return L"C:\\";
    auto start = t.find_first_not_of(L" \"");
    auto end = t.find_last_not_of(L" \"");
    if (start == std::wstring::npos) return L"C:\\";
    t = t.substr(start, end - start + 1);
    if (t.size() == 2 && t[1] == L':') t += L'\\';
    return t;
}

LRESULT CALLBACK AddressEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData);
LRESULT CALLBACK RenameEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData);
LRESULT CALLBACK TagRenameEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData);
LRESULT CALLBACK FilterEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData);
void EnsureEditVisuals(AppState& s);

static bool EraseHostedEditBackground(HWND hwnd, WPARAM wParam, AppState* s) {
    if (!s) return false;
    EnsureEditVisuals(*s);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    FillRect(reinterpret_cast<HDC>(wParam), &rc, s->editBrush);
    return true;
}

void PlaceHostedEdit(HWND hwnd, HWND owner, const D2D1_RECT_F& cell, float scale,
                            int left_margin_dip, int right_margin_dip) {
    POINT pt{ static_cast<int>(std::lround(cell.left)),
              static_cast<int>(std::lround(cell.top)) };
    (void)owner; // cell is already in the parent client coordinate space.
    const int w = std::max(40, static_cast<int>(std::lround(cell.right - cell.left)));
    const int cellH = std::max(18, static_cast<int>(std::lround(cell.bottom - cell.top)));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    int lineH = cellH;
    if (font) {
        HDC hdc = GetDC(hwnd);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);
        SelectObject(hdc, old);
        ReleaseDC(hwnd, hdc);
        lineH = std::max(1, static_cast<int>(tm.tmHeight));
    }
    lineH = std::min(lineH, cellH);
    const int y = pt.y + std::max(0, (cellH - lineH) / 2);
    const int left = std::max(0, static_cast<int>(std::lround(static_cast<float>(left_margin_dip) * scale)));
    const int right = std::max(0, static_cast<int>(std::lround(static_cast<float>(right_margin_dip) * scale)));
    SendMessageW(hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(left, right));
    SetWindowPos(hwnd, HWND_TOP, pt.x, y, w, lineH, SWP_NOACTIVATE);
}

HWND CreateHostedEdit(AppState& s, SUBCLASSPROC proc) {
    EnsureEditVisuals(s);
    HWND hwnd = ui::CreateChildEdit(s.hwnd);
    if (!hwnd) return nullptr;
    SetWindowTheme(hwnd, L"", L"");
    // LumaText presents with UpdateLayeredWindow. Mixing that with
    // SetLayeredWindowAttributes lets EDIT's GetDC/ClearType paint show
    // through on mouse-down. GDI-only fallback still uses LWA_ALPHA.
    if (!s.compositor.LumaTextEnabled())
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)s.editFont, TRUE);
    SetWindowSubclass(hwnd, proc, 1, reinterpret_cast<DWORD_PTR>(&s));
    return hwnd;
}

void LayoutAddressEditor(AppState& s) {
    if (!s.hwndAddressEdit || !s.hwnd) return;
    D2D1_RECT_F addr = s.renderer.AddressBarRect((float)s.compositor.Width());
    if (s.addressSearching) {
        PlaceHostedEdit(s.hwndAddressEdit, s.hwnd, ui::LayoutAddressSearch(addr, s.scale).input,
                        s.scale, 0, 0);
        return;
    }
    const float insetX = 10.0f * s.scale;
    const float insetY = 2.0f * s.scale;
    PlaceHostedEdit(s.hwndAddressEdit, s.hwnd,
        D2D1::RectF(addr.left + insetX, addr.top + insetY,
                    addr.right - insetX, addr.bottom - insetY),
        s.scale, 0, 0);
}

void EnsureEditVisuals(AppState& s) {
    if (!s.editFont) {
        const int height = -std::max(1, static_cast<int>(std::lround(14.0f * s.scale)));
        const wchar_t* family = ui::typography::PreferredTextFamily();
        s.editFont = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, family);
        if (!s.editFont) {
            s.editFont = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        }
    }
    if (!s.editBrush) {
        s.editBrush = CreateSolidBrush(s.darkMode ? RGB(30, 30, 30) : RGB(255, 255, 255));
    }
}

void HideRenameOverlay(AppState& s, bool commit);

void ShowAddressEditor(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    if (!s.tagRenameId.empty()) HideTagRenameOverlay(s, true);
    if (s.filterEditing) HideFilterEditor(s, true);
    s.addressSearching = false;
    s.addressEditing = true;
    // Same ordering rule as ShowRenameOverlay: let go of the old editor with
    // the kill-focus guard already armed, and never keep a dead handle.
    s.addressIgnoreKillFocus = true;
    if (s.hwndAddressEdit) {
        if (IsWindow(s.hwndAddressEdit)) DestroyWindow(s.hwndAddressEdit);
        s.hwndAddressEdit = nullptr;
    }
    if (!s.hwndAddressEdit) {
        s.hwndAddressEdit = CreateHostedEdit(s, AddressEditProc);
        if (!s.hwndAddressEdit) {
            s.addressEditing = false;
            s.addressIgnoreKillFocus = false;
            return;
        }
    }
    const std::wstring shown = ClipboardPath(tab->current_path);
    SendMessageW(s.hwndAddressEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L""));
    s.addressIgnoreKillFocus = true;
    SetWindowTextW(s.hwndAddressEdit, shown.empty() ? l10n::Get(l10n::StringId::ThisPc).c_str() : shown.c_str());
    LayoutAddressEditor(s);
    ShowWindow(s.hwndAddressEdit, SW_SHOW);
    SetForegroundWindow(GetAncestor(s.hwndAddressEdit, GA_ROOT));
    SetFocus(s.hwndAddressEdit);
    SendMessageW(s.hwndAddressEdit, EM_SETSEL, 0, -1);
    s.addressIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void HideAddressEditor(AppState& s, bool navigate) {
    if (!s.hwndAddressEdit) return;
    FlushAddressSearch(s);
    SaveAddressSearchDraft(s);
    if (navigate) {
        wchar_t buf[MAX_PATH * 4];
        GetWindowTextW(s.hwndAddressEdit, buf, ARRAYSIZE(buf));
        NavigateTo(s, FormatAddressPath(buf));
    }
    s.addressIgnoreKillFocus = true;
    ShowWindow(s.hwndAddressEdit, SW_HIDE);
    s.addressEditing = false;
    s.addressSearching = false;
    s.addressAnimationTick = GetTickCount64();
    if (s.hwnd) SetFocus(s.hwnd);
    s.addressIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ApplyFilterCue(AppState& s) {
    if (!s.hwndFilterEdit) return;
    s.filterCue = l10n::Get(s.filterSelectMode
        ? l10n::StringId::SelectWildcardHint
        : l10n::StringId::FilterPlaceholder);
    SendMessageW(s.hwndFilterEdit, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(s.filterCue.c_str()));
}

void LayoutFilterEditor(AppState& s) {
    if (!s.hwndFilterEdit || !s.hwnd || !s.filterEditing) return;
    const float expand = s.pane ? s.pane->filter_expand : 0.0f;
    PlaceHostedEdit(s.hwndFilterEdit, s.hwnd,
                    s.renderer.FilterEditRect(FocusedPaneRect(s), expand),
                    s.scale, 0, 0);
}

void ShowFilterEditor(AppState& s, bool select_mode) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    if (select_mode) {
        s.filterSelectMode = true;
        s.filterSelectRestore = tab->filter_text;
    } else if (!s.filterEditing) {
        s.filterSelectMode = false;
        s.filterSelectRestore.clear();
    }
    if (s.filterEditing && s.hwndFilterEdit && !s.filterFocusPending) {
        ApplyFilterCue(s);
        SetForegroundWindow(GetAncestor(s.hwndFilterEdit, GA_ROOT));
        SetFocus(s.hwndFilterEdit);
        SendMessageW(s.hwndFilterEdit, EM_SETSEL, 0, -1);
        return;
    }
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    if (!s.tagRenameId.empty()) HideTagRenameOverlay(s, true);
    if (s.addressEditing) HideAddressEditor(s, false);
    s.filterEditing = true;
    if (!s.hwndFilterEdit) {
        s.hwndFilterEdit = CreateHostedEdit(s, FilterEditProc);
        if (!s.hwndFilterEdit) {
            s.filterEditing = false;
            s.filterSelectMode = false;
            return;
        }
    }
    s.filterIgnoreKillFocus = true;
    SetWindowTextW(s.hwndFilterEdit, tab->filter_text.c_str());
    ApplyFilterCue(s);
    ShowWindow(s.hwndFilterEdit, SW_HIDE);
    s.filterFocusPending = true;
    s.filterIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ShowWildcardSelect(AppState& s) {
    ShowFilterEditor(s, true);
}

void HideFilterEditor(AppState& s, bool commit) {
    if (!s.hwndFilterEdit || !s.filterEditing) return;
    const bool select_mode = s.filterSelectMode;
    const std::wstring restore = s.filterSelectRestore;
    wchar_t buf[512]{};
    GetWindowTextW(s.hwndFilterEdit, buf, ARRAYSIZE(buf));
    s.filterIgnoreKillFocus = true;
    ShowWindow(s.hwndFilterEdit, SW_HIDE);
    s.filterEditing = false;
    s.filterFocusPending = false;
    s.filterSelectMode = false;
    s.filterSelectRestore.clear();
    if (s.hwnd) SetFocus(s.hwnd);
    s.filterIgnoreKillFocus = false;
    if (app::Tab* tab = ActiveTab(s)) {
        if (select_mode) {
            if (commit) {
                tab->filter_text = buf;
                std::vector<int> matches;
                app::CollectFilterMatches(*tab, &s.places, matches);
                tab->SelectIndices(matches);
                tab->filter_text.clear();
                tab->view_filter_map.reset();
                tab->view_cache_filter_text.clear();
                if (tab->selected_index >= 0) EnsureRowVisible(s, *tab, tab->selected_index);
            } else {
                tab->filter_text = restore;
            }
        } else if (commit) {
            tab->filter_text = buf;
        }
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void LayoutRenameOverlay(AppState& s) {
    if (!s.hwndRenameEdit || s.renameIndex < 0 || !s.hwnd) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    ui::WindowViewModel vm = BuildVm(s, false);
    const D2D1_RECT_F pane = FocusedPaneRect(s);
    float extra = tab->banner_message.empty() ? 0.0f : 36.0f * s.scale;
    std::wstring virtual_kind;
    if (app::ParsePulsePath(tab->current_path, &virtual_kind, nullptr) &&
        virtual_kind == L"recent") {
        extra += 40.0f * s.scale;
    }
    const D2D1_RECT_F list = s.renderer.PaneListRect(pane, extra, tab->view_mode);
    D2D1_RECT_F field = s.renderer.RenameFieldRect(vm.pane, list, s.renameIndex);
    if (field.right <= field.left) return;
    // Seat the EDIT inside the Fluent frame: frame stroke + text padding.
    const float insetX = 3.0f * s.scale;
    const float insetY = 2.0f * s.scale;
    field.left += insetX;
    field.right -= insetX;
    field.top += insetY;
    field.bottom -= insetY;
    PlaceHostedEdit(s.hwndRenameEdit, s.hwnd, field, s.scale, 4, 4);
}
void ShowRenameOverlay(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || tab->selected_index < 0) return;
    if (tab->net_readonly || IsRecycleTab(tab)) return;
    if (tab->SelectedCount() >= 2) {
        ShowBatchRename(s);
        return;
    }
    if (tab->selected_index >= (int)tab->snapshot->size()) return;
    if (!s.tagRenameId.empty()) HideTagRenameOverlay(s, true);
    if (s.addressEditing) HideAddressEditor(s, false);
    if (s.filterEditing) HideFilterEditor(s, true);
    s.renameIndex = tab->selected_index;
    EnsureRowVisible(s, *tab, s.renameIndex);

    // Arm the kill-focus guard before the old editor is torn down: destroying a
    // focused EDIT synchronously re-enters HideRenameOverlay through
    // WM_KILLFOCUS, which would clear renameIndex and leave the lookups below
    // indexing -1 (and a visible editor that nothing can hide).
    s.renameIgnoreKillFocus = true;
    if (s.hwndRenameEdit) {
        if (IsWindow(s.hwndRenameEdit)) DestroyWindow(s.hwndRenameEdit);
        s.hwndRenameEdit = nullptr;
    }
    s.hwndRenameEdit = CreateHostedEdit(s, RenameEditProc);
    if (!s.hwndRenameEdit) {
        s.renameIndex = -1;
        s.renameIgnoreKillFocus = false;
        return;
    }

    const std::wstring& name = (*tab->snapshot)[s.renameIndex].name;
    SetWindowTextW(s.hwndRenameEdit, name.c_str());
    LayoutRenameOverlay(s);
    ShowWindow(s.hwndRenameEdit, SW_SHOW);
    SetForegroundWindow(GetAncestor(s.hwndRenameEdit, GA_ROOT));
    SetFocus(s.hwndRenameEdit);
    int stem = (int)name.find_last_of(L'.');
    bool isDir = (*tab->snapshot)[s.renameIndex].is_dir;
    SendMessageW(s.hwndRenameEdit, EM_SETSEL, 0, (stem > 0 && !isDir) ? stem : -1);
    s.renameIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void HideRenameOverlay(AppState& s, bool commit) {
    if (!s.hwndRenameEdit) return;
    if (s.renameIndex < 0) {
        // Editor left behind by an interrupted setup (renameIndex cleared while
        // the box stayed on screen): hide it so it cannot stick over the list.
        if (IsWindow(s.hwndRenameEdit) && IsWindowVisible(s.hwndRenameEdit))
            ShowWindow(s.hwndRenameEdit, SW_HIDE);
        return;
    }
    const int index = s.renameIndex;
    s.renameIndex = -1;
    if (commit) {
        app::Tab* tab = ActiveTab(s);
        wchar_t buf[512];
        GetWindowTextW(s.hwndRenameEdit, buf, ARRAYSIZE(buf));
        if (tab && buf[0] && index < (int)(tab->snapshot ? tab->snapshot->size() : 0)) {
            std::wstring full = EntryFullPath(*tab, index);
            if (!full.empty() && buf != (*tab->snapshot)[index].name) {
                ops::OpRequest req;
                req.type = ops::OpType::Rename;
                req.sources.push_back(full);
                req.new_name = buf;
                tab->pending_selected_name = buf;
                tab->pending_selected_names = { buf };
                s.ops.Submit(std::move(req));
            }
        }
    }
    s.renameIgnoreKillFocus = true;
    ShowWindow(s.hwndRenameEdit, SW_HIDE);
    if (s.hwnd) SetFocus(s.hwnd);
    s.renameIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool TagRenameCell(AppState& s, const app::TagId& tag_id, D2D1_RECT_F& cell) {
    ui::WindowViewModel vm = BuildVm(s);
    const std::wstring path = app::MakeTagPath(tag_id);
    for (int group = 0; group < static_cast<int>(vm.sidebar.size()); ++group) {
        for (int item = 0; item < static_cast<int>(vm.sidebar[group].items.size()); ++item) {
            if (vm.sidebar[group].items[item].path != path) continue;
            if (!s.renderer.TagItemRect(vm, static_cast<float>(s.compositor.Width()),
                                        static_cast<float>(s.compositor.Height()),
                                        group, item, &cell)) {
                return false;
            }
            cell.left += 34.0f * s.scale;
            cell.right -= 38.0f * s.scale;
            cell.top += 2.0f * s.scale;
            cell.bottom -= 2.0f * s.scale;
            return cell.right > cell.left;
        }
    }
    return false;
}

void LayoutTagRenameOverlay(AppState& s) {
    if (!s.hwndTagRenameEdit || s.tagRenameId.empty() || !s.hwnd) return;
    D2D1_RECT_F cell{};
    if (TagRenameCell(s, s.tagRenameId, cell))
        PlaceHostedEdit(s.hwndTagRenameEdit, s.hwnd, cell, s.scale, 0, 0);
}

void ShowTagRenameOverlay(AppState& s, const app::TagId& tag_id) {
    const app::ColorTag* tag = s.places.FindTag(tag_id);
    if (!tag) return;
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    if (s.addressEditing) HideAddressEditor(s, false);
    if (s.filterEditing) HideFilterEditor(s, true);
    s.tagRenameId = tag_id;
    if (!s.hwndTagRenameEdit)
        s.hwndTagRenameEdit = CreateHostedEdit(s, TagRenameEditProc);
    if (!s.hwndTagRenameEdit) {
        s.tagRenameId.clear();
        return;
    }
    s.tagRenameIgnoreKillFocus = true;
    SetWindowTextW(s.hwndTagRenameEdit, tag->name.c_str());
    LayoutTagRenameOverlay(s);
    ShowWindow(s.hwndTagRenameEdit, SW_SHOW);
    SetForegroundWindow(GetAncestor(s.hwndTagRenameEdit, GA_ROOT));
    SetFocus(s.hwndTagRenameEdit);
    SendMessageW(s.hwndTagRenameEdit, EM_SETSEL, 0, -1);
    s.tagRenameIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void HideTagRenameOverlay(AppState& s, bool commit) {
    if (!s.hwndTagRenameEdit || s.tagRenameId.empty()) return;
    const app::TagId tag_id = s.tagRenameId;
    if (commit) {
        wchar_t text[512]{};
        GetWindowTextW(s.hwndTagRenameEdit, text, ARRAYSIZE(text));
        const app::ColorTag* before = s.places.FindTag(tag_id);
        if (before && before->name != text) {
            const std::vector<std::wstring> affected = s.places.PathsForTag(tag_id);
            if (s.places.RenameTag(tag_id, text))
                QueueTagAds(s, BuildTagAdsUpdates(s.places, affected));
            else
                MessageBeep(MB_ICONWARNING);
        }
    }
    s.tagRenameId.clear();
    s.tagRenameIgnoreKillFocus = true;
    ShowWindow(s.hwndTagRenameEdit, SW_HIDE);
    if (s.hwnd) SetFocus(s.hwnd);
    s.tagRenameIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
constexpr UINT_PTR kEditCaretTimer = 71;

D2D1_COLOR_F HostedEditForeground(const AppState& s) {
    return s.darkMode ? D2D1::ColorF(1.0f, 1.0f, 1.0f)
                      : D2D1::ColorF(26.0f / 255.0f, 26.0f / 255.0f, 26.0f / 255.0f);
}

D2D1_COLOR_F HostedEditBackground(const AppState& s) {
    return s.darkMode ? D2D1::ColorF(30.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f)
                      : D2D1::ColorF(1.0f, 1.0f, 1.0f);
}

IDWriteTextFormat* HostedEditFormat(AppState& s, HWND hwnd) {
    if (hwnd == s.hwndAddressEdit) return s.compositor.AddressFormat();
    return s.compositor.TextFormat();
}

bool HandleHostedEditMessage(AppState& s, HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam, LRESULT& result) {
    if ((msg == WM_KEYDOWN || msg == WM_CHAR) &&
        app::IsTabShortcut(static_cast<UINT>(wParam),
                           (GetKeyState(VK_CONTROL) & 0x8000) != 0,
                           (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                           (GetKeyState(VK_MENU) & 0x8000) != 0)) {
        if (msg == WM_KEYDOWN) HandleKeyDown(&s, s.hwnd, msg, wParam, lParam);
        result = 0;
        return true;
    }
    if (s.compositor.LumaTextEnabled() &&
        (msg == WM_PRINT || msg == WM_PRINTCLIENT || msg == WM_NCPAINT)) {
        result = 0;
        return true;
    }
    if (s.compositor.LumaTextEnabled() && msg == WM_ERASEBKGND) {
        result = 1;
        return true;
    }
    switch (msg) {
    case WM_LBUTTONDOWN: {
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE:
    case WM_CAPTURECHANGED:
        if (!s.compositor.LumaTextEnabled()) return false;
        result = s.compositor.CallLumaEditMouse(
            hwnd, msg, wParam, lParam, HostedEditFormat(s, hwnd));
        if (msg != WM_MOUSEMOVE || GetCapture() == hwnd) {
            s.compositor.PresentLumaEdit(hwnd, HostedEditFormat(s, hwnd),
                                         HostedEditForeground(s), HostedEditBackground(s));
        }
        return true;
    }
    case WM_PAINT: {
        if (!s.compositor.LumaTextEnabled()) return false;
        HideCaret(hwnd);
        if (!s.compositor.PresentLumaEdit(hwnd, HostedEditFormat(s, hwnd),
                                          HostedEditForeground(s), HostedEditBackground(s))) {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            EnsureEditVisuals(s);
            FillRect(hdc, &ps.rcPaint, s.editBrush);
            EndPaint(hwnd, &ps);
        }
        result = 0;
        return true;
    }
    case WM_SETFOCUS: {
        result = DefSubclassProc(hwnd, msg, wParam, lParam);
        if (s.compositor.LumaTextEnabled()) {
            // LumaText paints the text and its own blinking caret, so the native
            // caret stays hidden and the blink timer drives the repaints.
            HideCaret(hwnd);
            SetTimer(hwnd, kEditCaretTimer, GetCaretBlinkTime(), nullptr);
            s.compositor.PresentLumaEdit(hwnd, HostedEditFormat(s, hwnd),
                                         HostedEditForeground(s), HostedEditBackground(s));
        } else {
            // Native EDIT fallback: leave its caret alone. Hiding it here left
            // the field with no insertion point, because nothing shows it again.
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return true;
    }
    case WM_KILLFOCUS:
        KillTimer(hwnd, kEditCaretTimer);
        return false;
    case WM_TIMER:
        if (wParam == kEditCaretTimer) {
            if (GetCapture() != hwnd && s.compositor.LumaTextEnabled()) {
                s.compositor.PresentLumaEdit(hwnd, HostedEditFormat(s, hwnd),
                                             HostedEditForeground(s), HostedEditBackground(s));
            } else if (GetCapture() != hwnd) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            result = 0;
            return true;
        }
        return false;
    default:
        return false;
    }
}

LRESULT CALLBACK AddressEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    AppState* s = reinterpret_cast<AppState*>(dwRefData);
    if (s && msg == WM_IME_STARTCOMPOSITION) s->addressSearchComposing = true;
    if (s && msg == WM_IME_ENDCOMPOSITION) {
        s->addressSearchComposing = false;
        QueueAddressSearch(*s);
    }

    if (s && s->addressSearchComposing && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_CHAR))
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    if (s && msg == WM_LBUTTONUP && s->addressSearching && !s->searchHistoryOpen)
        PostMessageW(s->hwnd, WM_SEARCH_HISTORY, 0, 0);
    LRESULT handled = 0;
    if (s && HandleHostedEditMessage(*s, hwnd, msg, wParam, lParam, handled)) return handled;
    switch (msg) {
    case WM_KEYDOWN:
        if (s->addressSearching && wParam == VK_DOWN) {
            ShowAddressSearchHistory(*s);
            return 0;
        }
        if (s->addressSearching && (GetKeyState(VK_CONTROL) & 0x8000) &&
            (wParam == L'K' || wParam == L'L')) {
            HideAddressEditor(*s, false);
            ShowOmnibar(*s, wParam == L'K' ? OmnibarMode::Mixed : OmnibarMode::Path);
            return 0;
        }
        if (s->addressSearching && wParam == VK_TAB) {
            ShowAddressSearchScope(*s);
            return 0;
        }
        if (wParam == VK_RETURN) {
            if (s->addressSearching) SubmitAddressSearch(*s);
            else HideAddressEditor(*s, true);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            HideAddressEditor(*s, false);
            return 0;
        }
        break;
    case WM_CHAR:
        if (s->addressSearching && (wParam == VK_RETURN || wParam == VK_ESCAPE || wParam == VK_TAB))
            return 0;
        break;
    case WM_KILLFOCUS:
        if (s->addressSearching && reinterpret_cast<HWND>(wParam) == s->hwnd) break;
        if (!s->addressIgnoreKillFocus) HideAddressEditor(*s, false);
        break;
    case WM_ERASEBKGND: {
        return EraseHostedEditBackground(hwnd, wParam, s) ? 1 : 0;
    }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK FilterEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    AppState* s = reinterpret_cast<AppState*>(dwRefData);
    LRESULT handled = 0;
    if (s && HandleHostedEditMessage(*s, hwnd, msg, wParam, lParam, handled)) return handled;
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
            HideFilterEditor(*s, wParam == VK_RETURN);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wParam != VK_RETURN && wParam != VK_ESCAPE) {
            LRESULT lr = DefSubclassProc(hwnd, msg, wParam, lParam);
            if (app::Tab* tab = ActiveTab(*s)) {
                wchar_t buf[512];
                GetWindowTextW(hwnd, buf, ARRAYSIZE(buf));
                tab->filter_text = buf;
                InvalidateRect(s->hwnd, nullptr, FALSE);
            }
            return lr;
        }
        break;
    case WM_KILLFOCUS:
        if (!s->filterIgnoreKillFocus) HideFilterEditor(*s, true);
        break;
    case WM_ERASEBKGND: {
        return EraseHostedEditBackground(hwnd, wParam, s) ? 1 : 0;
    }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK RenameEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    AppState* s = reinterpret_cast<AppState*>(dwRefData);
    LRESULT handled = 0;
    if (s && HandleHostedEditMessage(*s, hwnd, msg, wParam, lParam, handled)) return handled;
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {
            HideRenameOverlay(*s, true);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            HideRenameOverlay(*s, false);
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        if (!s->renameIgnoreKillFocus) HideRenameOverlay(*s, true);
        break;
    case WM_ERASEBKGND: {
        return EraseHostedEditBackground(hwnd, wParam, s) ? 1 : 0;
    }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK TagRenameEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    AppState* s = reinterpret_cast<AppState*>(dwRefData);
    LRESULT handled = 0;
    if (s && HandleHostedEditMessage(*s, hwnd, msg, wParam, lParam, handled)) return handled;
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {
            HideTagRenameOverlay(*s, true);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            HideTagRenameOverlay(*s, false);
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        if (!s->tagRenameIgnoreKillFocus) HideTagRenameOverlay(*s, true);
        break;
    case WM_ERASEBKGND: {
        return EraseHostedEditBackground(hwnd, wParam, s) ? 1 : 0;
    }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

} // namespace pulse

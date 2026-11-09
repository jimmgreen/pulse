#include "window_overview_ui.h"
#include "window_management_ui.h"
#include "window_arrange_ui.h"
#include "app_internal.h"
#include "app_navigation.h"
#include "tab_transfer.h"
#include "tray_reveal.h"
#include "window_factory.h"
#include "window_identity_ui.h"
#include "window_merge.h"
#include "window_overview.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include "../fs/fs_enum.h"
#include "../ui/ui_renderer.h"

#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <optional>
#include <string>
#include <vector>

namespace pulse {
namespace {

constexpr size_t kCardChips = 10;  // more tabs: "还有 N 个标签页", which goes to the window

std::vector<HWND> OpenWindows(const AppState& s) {
    std::vector<HWND> out;
    s.core.windows.ForEach([&](HWND hwnd, AppState&) { out.push_back(hwnd); });
    return out;
}

std::wstring FormatCount(l10n::StringId id, int n) {
    wchar_t buf[160]{};
    swprintf_s(buf, l10n::Get(id).c_str(), n);
    return buf;
}

std::wstring FolderOf(const app::LayoutTab& tab) {
    const app::Tab* view = tab.ActiveFolder();
    if (!view) return {};
    const std::wstring& path = view->current_path;
    return fs::IsVirtualPath(path) ? app::TabTitle(path) : path::StripExtendedPathPrefix(path);
}

ui::WindowOverviewCard CardFor(const AppState& s, AppState& w) {
    ui::WindowOverviewCard card;
    card.name = WindowDisplayName(w);
    card.rgb = app::ValidWindowColor(w.identity.color) ? app::kWindowColors[w.identity.color] : 0x94A3B8;
    card.current = &w == &s;
    const size_t count = w.window_tabs.items.size();
    card.meta = count == 1 ? l10n::Get(l10n::StringId::WindowSwitcherTabOne)
                           : FormatCount(l10n::StringId::WindowSwitcherTabs, static_cast<int>(count));
    if (w.hwnd && IsIconic(w.hwnd))
        card.meta += L" \u00B7 " + l10n::Get(l10n::StringId::WindowOverviewMinimized);
    for (size_t i = 0; i < count && card.chips.size() < kCardChips; ++i) {
        const auto& tab = w.window_tabs.items[i];
        if (!tab) continue;
        ui::WindowOverviewChip chip;
        chip.title = app::LayoutTabTitle(*tab);
        chip.path = FolderOf(*tab);
        chip.tab = i;
        chip.active = i == w.window_tabs.active;
        card.chips.push_back(std::move(chip));
    }
    if (count > card.chips.size())
        card.more = FormatCount(l10n::StringId::WindowOverviewMore, static_cast<int>(count - card.chips.size()));
    return card;
}

void Kick(AppState& s) {
    if (s.framePump.Running()) s.framePump.Arm();
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Cards again from the live windows; their places stay (SyncOverviewOrder).
void Refresh(AppState& s) {
    auto& view = s.renderer.WindowOverview();
    if (!s.overview.open) {
        view.Hide();
        Kick(s);
        return;
    }
    app::SyncOverviewOrder(s.overview.order, OpenWindows(s));
    ui::WindowOverviewData data;
    data.title = l10n::Get(l10n::StringId::WindowOverviewTitle);
    data.hint = l10n::Get(l10n::StringId::WindowOverviewHint);
    data.notice = s.overview.notice;
    data.arrange = l10n::Get(l10n::StringId::SettingsSecArrangeWindows);
    for (int i=0;i<8;++i) data.arrangements.push_back(l10n::Get(WindowArrangeLabel(static_cast<app::WindowArrangeLayout>(i))));
    data.merge_all = l10n::Get(l10n::StringId::WindowOverviewMergeAll);
    data.done = l10n::Get(l10n::StringId::WindowOverviewDone);
    data.switch_to = l10n::Get(l10n::StringId::WindowOverviewSwitch);
    data.current = l10n::Get(l10n::StringId::WindowOverviewCurrent);
    data.new_window = l10n::Get(l10n::StringId::WindowOverviewNewWindow);
    data.can_merge_all = s.overview.order.size() > 1;
    for (HWND hwnd : s.overview.order) {
        AppState* w = s.core.windows.Lookup(hwnd);
        data.cards.push_back(w ? CardFor(s, *w) : ui::WindowOverviewCard{});
    }
    RECT client{};
    GetClientRect(s.hwnd, &client);
    view.Show(std::move(data),
              D2D1::RectF(0.0f, 0.0f, static_cast<float>(client.right), static_cast<float>(client.bottom)),
              s.scale);
    Kick(s);
}

void EndPress(AppState& s) {
    const bool had_capture = s.overview.pressed && GetCapture() == s.hwnd;
    s.overview.pressed = false;
    s.overview.dragging = false;
    s.overview.press_card = -1;
    s.renderer.WindowOverview().EndDrag();
    if (had_capture) ReleaseCapture();
}

void Close(AppState& s) {
    EndPress(s);
    s.overview.open = false;
    s.overview.order.clear();
    s.overview.notice.clear();
    s.renderer.WindowOverview().Hide();
    Kick(s);
}

AppState* WindowOfCard(AppState& s, int card) {
    if (card < 0 || static_cast<size_t>(card) >= s.overview.order.size()) return nullptr;
    return s.core.windows.Lookup(s.overview.order[static_cast<size_t>(card)]);
}

// Goes to a window (and one of its tabs); the overview closes.
void GoTo(AppState& s, int card, std::optional<size_t> tab) {
    AppState* w = WindowOfCard(s, card);
    Close(s);
    if (!w) return;
    if (tab && *tab < w->window_tabs.items.size() && *tab != w->window_tabs.active) SwitchTab(*w, *tab);
    if (w != &s) RevealWindow(*w);  // restores a minimized or hidden window
}

void MergeAll(AppState& s) {
    std::vector<HWND> others;
    for (HWND hwnd : s.overview.order)
        if (hwnd != s.hwnd) others.push_back(hwnd);
    Close(s);
    MergeWindowsInto(others, s);  // one toast here undoes all of them
}

// Windows open and go away below: activation changes must not close the
// overview meanwhile, and it takes the foreground back afterwards.
template <class F> void Busy(AppState& s, F&& f) {
    s.overview.busy = true;
    f();
    s.overview.busy = false;
    if (IsWindow(s.hwnd) && GetForegroundWindow() != s.hwnd) SetForegroundWindow(s.hwnd);
}

// A dragged tab released over `over`. Returns false when `s` went away.
bool Drop(AppState& s, const ui::OverviewHit& over) {
    const int source_card = s.overview.press_card;
    const size_t tab = s.overview.press_tab;
    AppState* src = WindowOfCard(s, source_card);
    if (!src || tab >= src->window_tabs.items.size()) return true;
    const bool on_card = over.kind == ui::OverviewHitKind::Card || over.kind == ui::OverviewHitKind::Chip ||
                         over.kind == ui::OverviewHitKind::Switch || over.kind == ui::OverviewHitKind::More;
    const app::OverviewDrop drop = app::DecideOverviewDrop(source_card, src->window_tabs.items.size(),
                                                           on_card ? over.card : -1,
                                                           over.kind == ui::OverviewHitKind::NewWindow);
    AppState* dst = on_card ? WindowOfCard(s, over.card) : nullptr;
    switch (drop) {
    case app::OverviewDrop::None:
        break;
    case app::OverviewDrop::AlreadyAlone:
        s.overview.notice = l10n::Get(l10n::StringId::WindowOverviewAlone);
        break;
    case app::OverviewDrop::MoveTab:
        if (dst) MoveLayoutTab(*src, tab, *dst, dst->window_tabs.items.size());
        break;
    case app::OverviewDrop::MoveLastTab:
        if (!dst) break;
        if (src == &s) {
            // This window's last tab: this window merges into that one, as
            // when it is dragged onto its tab strip (undo toast there).
            WINDOWPLACEMENT before{sizeof(before)};
            GetWindowPlacement(s.hwnd, &before);
            Close(s);
            return !MergeWindowInto(s, *dst, dst->window_tabs.items.size(), before);
        }
        Busy(s, [&] {
            const HWND gone = src->hwnd;
            ShowWindow(gone, SW_HIDE);  // it must not paint without a tab
            if (MoveLayoutTab(*src, tab, *dst, dst->window_tabs.items.size(), TransferSource::Closing))
                DestroyWindow(gone);
            else
                ShowWindow(gone, SW_SHOWNOACTIVATE);
        });
        break;
    case app::OverviewDrop::NewWindow:
        Busy(s, [&] {
            WindowInit init;
            init.path = FolderOf(*src->window_tabs.items[tab]);  // only if the tab cannot move
            init.adopt_from = src;
            init.adopt_index = tab;
            OpenPulseWindow(*src, init);
        });
        break;
    }
    return true;
}

void Hover(AppState& s, float x, float y) {
    if (s.renderer.WindowOverview().SetHover(s.renderer.WindowOverview().HitTest(x, y))) Kick(s);
}

bool Down(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

bool IsToggleKey(WPARAM vk) { return IsWindowOverviewKey(vk); }

void Open(AppState& s) {
    s.overview = {};
    s.overview.open = true;
    s.overview.order = OpenWindows(s);  // most recent first: this window's card leads
    Refresh(s);
}

}  // namespace

bool IsWindowOverviewKey(WPARAM vk) {
    return vk == 'O' && Down(VK_CONTROL) && Down(VK_SHIFT) && !Down(VK_MENU);
}

bool MergeAllWindows(AppState& s) {
    std::vector<HWND> others;
    for (HWND hwnd : OpenWindows(s))
        if (hwnd != s.hwnd) others.push_back(hwnd);
    if (s.overview.open) Close(s);
    if (others.empty()) return false;
    return MergeWindowsInto(others, s) > 0;  // one toast here undoes all of them
}

void ToggleWindowOverview(AppState& s) {
    if (s.overview.open)
        Close(s);
    else
        Open(s);
}

bool WindowOverviewMessage(AppState& s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result) {
    if (hwnd != s.hwnd) return false;
    result = 0;
    if (!s.overview.open) {
        if (msg == WM_KEYDOWN && IsToggleKey(wParam) && !s.switcher.IsOpen()) {
            Open(s);
            return true;
        }
        return false;
    }
    auto& view = s.renderer.WindowOverview();
    const float x = static_cast<float>(GET_X_LPARAM(lParam));
    const float y = static_cast<float>(GET_Y_LPARAM(lParam));
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (s.overview.dragging) {
                EndPress(s);  // the drag stops, the overview stays
                Kick(s);
            } else {
                Close(s);
            }
        } else if (IsToggleKey(wParam)) {
            Close(s);
        }
        return true;  // nothing else acts behind the panel
    case WM_KEYUP:
    case WM_CHAR:
    case WM_DEADCHAR:
        return true;
    case WM_SYSKEYDOWN:
        Close(s);
        return false;  // Alt+F4 and the system menu still work
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        RECT bounds{};
        GetClientRect(hwnd, &bounds);
        const int border = static_cast<int>(8*s.scale);
        if (!IsZoomed(hwnd) && (pt.x < border || pt.y < border || pt.x >= bounds.right-border || pt.y >= bounds.bottom-border))
            return false;
        const auto hit = view.HitTest(static_cast<float>(pt.x), static_cast<float>(pt.y));
        result = hit.kind == ui::OverviewHitKind::Caption || hit.kind == ui::OverviewHitKind::Outside ? HTCAPTION : HTCLIENT;
        return true;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) != HTCLIENT) return false;
        SetCursor(LoadCursorW(nullptr, s.overview.dragging ? IDC_SIZEALL : IDC_ARROW));
        result = TRUE;
        return true;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
    case WM_NCRBUTTONDOWN:
        return false;  // Keep the overview open during native move/resize.
    case WM_CANCELMODE:
        EndPress(s);
        return false;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        s.overview.notice.clear();
        const ui::OverviewHit hit = view.HitTest(x, y);
        switch (hit.kind) {
        case ui::OverviewHitKind::Chip: {
            const auto& order = s.overview.order;
            AppState* w = WindowOfCard(s, hit.card);
            if (!w || static_cast<size_t>(hit.card) >= order.size()) break;
            // Which tab: the chip's index into the window's tabs.
            ui::WindowOverviewCard card = CardFor(s, *w);
            if (hit.chip < 0 || static_cast<size_t>(hit.chip) >= card.chips.size()) break;
            s.overview.pressed = true;
            s.overview.dragging = false;
            s.overview.press_card = hit.card;
            s.overview.press_tab = card.chips[static_cast<size_t>(hit.chip)].tab;
            s.overview.press_pt = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            s.overview.press_chip = hit.chip;
            SetCapture(hwnd);
            break;
        }
        case ui::OverviewHitKind::Switch:
        case ui::OverviewHitKind::More:
            GoTo(s, hit.card, std::nullopt);
            return true;
        case ui::OverviewHitKind::Arrange: {
            const auto order = s.overview.order;
            POINT anchor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd, &anchor);
            int layout = -1;
            Busy(s, [&] { layout = PickWindowArrangement(s, anchor); });
            if (layout == 8) {
                MergeAll(s);
            } else if (layout >= 0) {
                Close(s);
                ArrangeWindows(s, static_cast<app::WindowArrangeLayout>(layout), (GetKeyState(VK_SHIFT)&0x8000)!=0, order);
            } else {
                Refresh(s);
            }
            return true;
        }
        case ui::OverviewHitKind::MergeAll:
            MergeAll(s);
            return true;
        case ui::OverviewHitKind::Done:
        case ui::OverviewHitKind::Outside:
            Close(s);
            return true;
        default:
            break;
        }
        Refresh(s);  // the notice went away
        return true;
    }
    case WM_MOUSEMOVE:
        if (s.overview.pressed) {
            const POINT now{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (!s.overview.dragging &&
                app::OverviewDragStarted(s.overview.press_pt, now,
                                         SIZE{GetSystemMetrics(SM_CXDRAG), GetSystemMetrics(SM_CYDRAG)}))
                s.overview.dragging = true;
            if (s.overview.dragging) {
                view.SetDrag(s.overview.press_card, s.overview.press_chip, x, y, view.HitTest(x, y));
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                Kick(s);
            }
            return true;
        }
        Hover(s, x, y);
        return true;
    case WM_LBUTTONUP: {
        if (!s.overview.pressed) return true;
        const bool dragging = s.overview.dragging;
        const int card = s.overview.press_card;
        const size_t tab = s.overview.press_tab;
        const ui::OverviewHit over = view.HitTest(x, y);
        if (!dragging) {
            EndPress(s);
            GoTo(s, card, tab);  // a click on a tab goes to it
            return true;
        }
        const bool alive = Drop(s, over);
        if (!alive || !s.overview.open) return true;  // `s` merged away, or closed
        EndPress(s);
        Refresh(s);
        Hover(s, x, y);
        return true;
    }
    case WM_CAPTURECHANGED:
        if (s.overview.pressed && reinterpret_cast<HWND>(lParam) != hwnd) {
            EndPress(s);
            Kick(s);
        }
        return false;
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (s.overview.dragging) {
            EndPress(s);
            Kick(s);
        }
        return true;
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_MOUSEHWHEEL:
        return true;
    case WM_MOUSEWHEEL:
        if (view.Scroll(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA * 64.0f)) Kick(s);
        return true;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            Close(s);
        } else {
            Refresh(s);
        }
        return false;
    case WM_KILLFOCUS:
        if (!s.overview.busy) Close(s);
        return false;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && !s.overview.busy) Close(s);
        return false;
    default:
        return false;
    }
}

}  // namespace pulse

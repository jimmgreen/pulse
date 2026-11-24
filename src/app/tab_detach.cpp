// tab_detach.cpp - Multi-window M6a: drags a tab off the strip into its own
// window (plan §8.2).
#include "tab_detach.h"
#include "../ui/ui_renderer.h"

#include "app_internal.h"
#include "../common/localization.h"
#include "app_messages.h"
#include "pane_drop.h"
#include "tab_transfer.h"
#include "window_factory.h"
#include "window_identity_ui.h"
#include "window_merge.h"
#include "window_registry.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace pulse {
namespace {
// Esc inside the system move loop: the loop reads the key itself, but a
// thread keyboard hook still sees it. Only one move loop runs at a time.
HHOOK g_escape_hook = nullptr;
bool g_escape_pressed = false;
// Where the cursor holds the window being dragged, relative to its top-left
// corner. The move loop starts a few mouse moves after the press or the
// detach; the window first catches up so the grab point stays put.
struct Grab {
    HWND hwnd = nullptr;
    POINT offset{};
};
Grab g_grab;
// The window in our move loop, and the strip spot its tab would drop on
// (windows are looked up by handle: one may close during the loop).
HWND g_moving = nullptr;
AppState* g_moving_state = nullptr;
// The window the dragged tab or pane came from (none for a window's only tab).
HWND g_source = nullptr;
// zone None: on the tab strip before `index`. Center: a new tab at `index`
// (after the active one). An edge: one more pane of the active tab (M6d).
// Why a content drop is a new tab rather than a split (the hint says so, M7h).
enum class NoSplit { None, Full, Never };
struct Drop {
    HWND window = nullptr;
    size_t index = 0;
    app::ContentDrop zone = app::ContentDrop::None;  // an edge: the side of the pane aimed at (M7i)
    app::PanePlacement placement;                   // where the panes go when it splits (M7i)
    std::vector<D2D1_RECT_F> rects;                 // the content-area preview, client px
    NoSplit no_split = NoSplit::None;
    // The Pulse window under the cursor, with or without a drop spot, and
    // whether the cursor is on its content waiting to rest (M7h).
    HWND over = nullptr;
    bool resting = false;
};
Drop g_drop;
// A drag out of a maximized window is over its own content all the way, so
// on the window the tab came from content drops only arm once the cursor
// rests there; a quick drag still makes a window (M6d; M7h: 400 -> 150 ms).
// Any other window shows its drop at once, as the prototype (M7j).
constexpr UINT kContentDropDwellMs = 150;
constexpr UINT_PTR kContentDropTimer = 0x4D36;
constexpr float kContentDropBand = 0.25f;
struct Dwell {
    HWND window = nullptr;
    POINT at{};
    ULONGLONG since = 0;
    bool armed = false;
};
Dwell g_dwell;
// A dragged tab over a Pulse window (M7h): that window draws it at the cursor.
HWND g_ghost = nullptr;
// A title-bar drag over a strip shows the window see-through (M6c), as the
// prototype's .38 opacity.
constexpr BYTE kSeeThroughAlpha = 97;
// A title-bar drag (M6c): the system runs a move loop we did not start. The
// placement from before the drag is what an undo of the merge restores.
bool g_caption_move = false;
// A pane dragged out of a split (M6d): after Esc it goes back there.
struct PaneBack {
    HWND from = nullptr;
    const app::LayoutTab* into = nullptr;
    app::PaneOrigin origin;
};
PaneBack g_pane_back;
WINDOWPLACEMENT g_before{sizeof(WINDOWPLACEMENT)};
// Cached strip geometry of the target window under a move loop (#645): avoids
// rebuilding the full WindowViewModel and probing the clipboard on every mouse tick.
struct StripCache {
    HWND hwnd = nullptr;
    float width = 0.0f;
    float scale = 0.0f;
    size_t count = 0;
    size_t active = 0;
    bool vertical = false;
    int layout = 0;
    int max_rows = 0;
    std::vector<TabSpan> spans;
};
StripCache g_strip_cache;

AppState* WindowOf(AppState& any, HWND hwnd) {
    if (!hwnd) return nullptr;
    AppState& found = app::FindWindowSharingCore(any, [hwnd](const AppState& w) { return w.hwnd == hwnd; });
    return found.hwnd == hwnd ? &found : nullptr;
}

bool SameRect(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool SameRects(const std::vector<D2D1_RECT_F>& a, const std::vector<D2D1_RECT_F>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), SameRect);
}

void ShowDrop(AppState& any, const Drop& next) {
    if (g_drop.window == next.window && g_drop.index == next.index && g_drop.zone == next.zone &&
        g_drop.placement == next.placement && SameRects(g_drop.rects, next.rects)) {
        g_drop = next;  // the window under the cursor and the hint may still change
        return;
    }
    if (AppState* old = WindowOf(any, g_drop.window)) {
        old->tabDropIndex = -1;
        old->contentDrop = false;
        old->contentDropRects.clear();
        InvalidateRect(old->hwnd, nullptr, FALSE);
    }
    g_drop = next;
    if (AppState* target = WindowOf(any, next.window)) {
        if (next.zone == app::ContentDrop::None) {
            target->tabDropIndex = static_cast<int>(next.index);
        } else {
            target->contentDrop = true;
            target->contentDropAsTab = next.zone == app::ContentDrop::Center;
            target->contentDropRects = next.rects;
        }
        InvalidateRect(target->hwnd, nullptr, FALSE);
    }
}

void CALLBACK DwellElapsed(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    if (g_moving == hwnd && g_moving_state) TrackTabDrop(*g_moving_state);
}

void ForgetDwell() {
    if (g_dwell.window && g_moving) KillTimer(g_moving, kContentDropTimer);
    g_dwell = Dwell{};
}

// True once the cursor has rested on `window`'s content long enough; stays
// armed while it remains on that content.
bool ContentDropArmed(HWND window, POINT cursor) {
    const ULONGLONG now = GetTickCount64();
    const bool moved = std::abs(cursor.x - g_dwell.at.x) > GetSystemMetrics(SM_CXDRAG) * 2 ||
                       std::abs(cursor.y - g_dwell.at.y) > GetSystemMetrics(SM_CYDRAG) * 2;
    if (g_dwell.window != window || (!g_dwell.armed && moved)) {
        g_dwell = Dwell{window, cursor, now, false};
        SetTimer(g_moving, kContentDropTimer, kContentDropDwellMs + 15, DwellElapsed);
        return false;
    }
    if (!g_dwell.armed && now - g_dwell.since >= kContentDropDwellMs) g_dwell.armed = true;
    return g_dwell.armed;
}

// Over another window the dragged window would hide the drop preview: a
// dragged tab's window hides while that window draws the tab (M7h, alpha 0);
// a window dragged by its title bar turns see-through over a strip (M6c).
// 255 makes it opaque again.
void SetMovingAlpha(HWND hwnd, BYTE alpha) {
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const bool layered = (ex & WS_EX_LAYERED) != 0;
    if (alpha == 255) {
        if (layered) SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
        return;
    }
    BYTE current = 255;
    DWORD flags = 0;
    if (layered && GetLayeredWindowAttributes(hwnd, nullptr, &current, &flags) && (flags & LWA_ALPHA) &&
        current == alpha)
        return;
    if (!layered) SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
}

std::wstring Fill(std::wstring text, std::wstring_view slot, const std::wstring& value) {
    for (size_t at = text.find(slot); at != std::wstring::npos; at = text.find(slot, at + value.size()))
        text.replace(at, slot.size(), value);
    return text;
}

std::wstring DropText(l10n::StringId id) { return l10n::Get(id); }

// Fills %1 with `name` and %2 with `value`, and marks where `value` went:
// the hint draws it in the accent color (M7i).
void FillHint(ui::DragGhostView& ghost, l10n::StringId id, const std::wstring& name, const std::wstring& value) {
    const std::wstring text = Fill(DropText(id), L"%1", name);
    const size_t at = text.find(L"%2");
    ghost.hint = Fill(text, L"%2", value);
    ghost.em_start = at == std::wstring::npos ? 0 : at;
    ghost.em_length = at == std::wstring::npos ? 0 : value.size();
}

// Where the new panes end up in the content, in words: a half (left, top...)
// or a quarter (top left...) of it (M7i).
l10n::StringId PlaceWord(const std::vector<D2D1_RECT_F>& rects, const D2D1_RECT_F& content, app::ContentDrop side) {
    if (rects.empty()) return l10n::StringId::DropSideLeft;
    D2D1_RECT_F all = rects.front();
    for (const D2D1_RECT_F& r : rects) {
        all.left = (std::min)(all.left, r.left);
        all.top = (std::min)(all.top, r.top);
        all.right = (std::max)(all.right, r.right);
        all.bottom = (std::max)(all.bottom, r.bottom);
    }
    const float tx = (content.right - content.left) * 0.04f, ty = (content.bottom - content.top) * 0.04f;
    const bool l = all.left - content.left <= tx, r = content.right - all.right <= tx;
    const bool t = all.top - content.top <= ty, b = content.bottom - all.bottom <= ty;
    const int h = l && !r ? -1 : r && !l ? 1 : 0;
    const int v = t && !b ? -1 : b && !t ? 1 : 0;
    if (h < 0 && v < 0) return l10n::StringId::DropSideTopLeft;
    if (h > 0 && v < 0) return l10n::StringId::DropSideTopRight;
    if (h < 0 && v > 0) return l10n::StringId::DropSideBottomLeft;
    if (h > 0 && v > 0) return l10n::StringId::DropSideBottomRight;
    if (h < 0) return l10n::StringId::DropSideLeft;
    if (h > 0) return l10n::StringId::DropSideRight;
    if (v < 0) return l10n::StringId::DropSideTop;
    if (v > 0) return l10n::StringId::DropSideBottom;
    return side == app::ContentDrop::Left    ? l10n::StringId::DropSideLeft
           : side == app::ContentDrop::Right ? l10n::StringId::DropSideRight
           : side == app::ContentDrop::Top   ? l10n::StringId::DropSideTop
                                             : l10n::StringId::DropSideBottom;
}

// What letting go of the dragged tab does, as the prototype words it: the
// window by name, the spot on its strip, where the panes go.
void DescribeDrop(AppState& moving, const Drop& drop, ui::DragGhostView& ghost) {
    AppState* const target = WindowOf(moving, drop.window ? drop.window : drop.over);
    const std::wstring name = target ? WindowDisplayName(*target) : std::wstring();
    ghost.detail.clear();
    ghost.em_start = ghost.em_length = 0;
    if (!drop.window) {
        ghost.hint = DropText(l10n::StringId::DropHintNewWindow);
        if (drop.resting) ghost.detail = Fill(DropText(l10n::StringId::DropHintRest), L"%1", name);
        return;
    }
    switch (drop.zone) {
    case app::ContentDrop::None:
        FillHint(ghost, l10n::StringId::DropHintStrip, name, std::to_wstring(drop.index + 1));
        return;
    case app::ContentDrop::Center:
        ghost.hint = Fill(DropText(l10n::StringId::DropHintTab), L"%1", name);
        ghost.detail = DropText(drop.no_split == NoSplit::Full    ? l10n::StringId::DropHintFull
                                : drop.no_split == NoSplit::Never ? l10n::StringId::DropHintNoSplit
                                                                  : l10n::StringId::DropHintEdges);
        return;
    default: {
        const D2D1_RECT_F content = target ? target->renderer.ContentRect(static_cast<float>(target->compositor.Width()),
                                                                          static_cast<float>(target->compositor.Height()))
                                           : D2D1_RECT_F{};
        FillHint(ghost, l10n::StringId::DropHintSplit, name, DropText(PlaceWord(drop.rects, content, drop.zone)));
        return;
    }
    }
}

// Moves the drawn tab to the window under the cursor (`drop.over`), or clears
// it when there is none.
void ShowGhost(AppState& moving, const Drop& drop, POINT cursor) {
    AppState* const target = drop.over ? WindowOf(moving, drop.over) : nullptr;
    if (g_ghost && (!target || target->hwnd != g_ghost)) {
        if (AppState* old = WindowOf(moving, g_ghost)) {
            old->dragGhost = ui::DragGhostView{};
            InvalidateRect(old->hwnd, nullptr, FALSE);
        }
    }
    g_ghost = target ? target->hwnd : nullptr;
    if (!target || moving.window_tabs.items.empty() || !moving.window_tabs.items[0]) return;
    const app::LayoutTab& tab = *moving.window_tabs.items[0];
    ui::DragGhostView& ghost = target->dragGhost;
    POINT client = cursor;
    ScreenToClient(target->hwnd, &client);
    ghost.on = true;
    ghost.x = static_cast<float>(client.x);
    ghost.y = static_cast<float>(client.y);
    // The folder by name: the pill counts the panes itself ("2 panes"), where
    // the strip title says "name · 2".
    const app::Tab* const view = tab.ActiveFolder();
    ghost.title = !tab.title.empty() || !view ? app::LayoutTabTitle(tab)
                  : view->virtual_title.empty() ? app::TabTitle(view->current_path)
                                                : view->virtual_title;
    ghost.panes = static_cast<int>((std::max)(size_t{1}, tab.VisiblePanes().size()));
    DescribeDrop(moving, drop, ghost);
    InvalidateRect(target->hwnd, nullptr, FALSE);
}

// Paints the windows showing the drop and the dragged tab now.
void PaintDropNow() {
    if (g_drop.window) UpdateWindow(g_drop.window);
    if (g_ghost && g_ghost != g_drop.window) UpdateWindow(g_ghost);
}

void HideGhost(AppState& any) {
    if (AppState* old = WindowOf(any, g_ghost)) {
        old->dragGhost = ui::DragGhostView{};
        InvalidateRect(old->hwnd, nullptr, FALSE);
    }
    g_ghost = nullptr;
}

bool IsSettingsPane(const app::Pane* pane) { return pane && IsSettingsTab(pane->ActiveTab()); }

bool Cloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

// The content-area drop at `client` in `target` for the only tab of
// `moving`: its panes split the pane under the cursor at the side it is
// nearest (M7i, anywhere on the pane M7j), where the layout has room for them
// (up to four in all, M7h), else a tab.
Drop ContentDropAt(AppState& moving, AppState& target, POINT client, float width) {
    const D2D1_RECT_F content =
        target.renderer.ContentRect(width, static_cast<float>(target.compositor.Height()));
    if (app::ContentDropZone(content, client, kContentDropBand) == app::ContentDrop::None ||
        moving.window_tabs.items.size() != 1)
        return {};
    const app::LayoutTab* incoming = moving.window_tabs.items[0].get();
    const app::LayoutTab* active = target.window_tabs.Active();
    if (!active || !incoming) return {};
    Drop drop{target.hwnd, target.window_tabs.active + 1, app::ContentDrop::Center, {}, {content}};
    const bool settings = IsSettingsTab(ActiveTab(target)) ||
                          std::any_of(incoming->panes.begin(), incoming->panes.end(),
                                      [](const std::unique_ptr<app::Pane>& pane) { return IsSettingsPane(pane.get()); });
    if (settings || app::DropPaneCount(*incoming) == 0) drop.no_split = NoSplit::Never;
    else if (!app::HasRoomForPanes(*active, *incoming)) drop.no_split = NoSplit::Full;
    if (drop.no_split != NoSplit::None) return drop;
    app::ContentDrop side = app::ContentDrop::Center;
    const app::PanePlacement placement =
        app::AimPaneDrop(*active, app::DropPaneCount(*incoming), content, target.scale, client, &side);
    if (!placement.Splits() || !app::CanPlacePanes(*active, *incoming, placement)) return drop;
    drop.zone = side;
    drop.placement = placement;
    drop.rects = app::PlacementRects(*active, placement, content, target.scale, incoming);
    return drop;
}

// The drop spot under `cursor` (screen px) in the topmost window below
// `moving`, when that window is another Pulse window on the same core: its
// tab strip, or (`content`) its content area, on the window the tab came from
// after a rest.
Drop FindDrop(AppState& moving, POINT cursor, bool content) {
    for (HWND below = GetWindow(moving.hwnd, GW_HWNDNEXT); below; below = GetWindow(below, GW_HWNDNEXT)) {
        if (!IsWindowVisible(below) || IsIconic(below) || Cloaked(below)) continue;
        RECT bounds{};
        if (!GetWindowRect(below, &bounds) || !PtInRect(&bounds, cursor)) continue;
        AppState* target = WindowOf(moving, below);
        if (!target || target == &moving) {  // another program covers the spot
            ForgetDwell();
            return {};
        }
        const float width = static_cast<float>(target->compositor.Width());
        const float scale = target->scale > 0.0f ? target->scale : 1.0f;
        const size_t tab_count = target->window_tabs.items.size();
        if (g_strip_cache.hwnd != below || g_strip_cache.width != width || g_strip_cache.scale != scale ||
            g_strip_cache.count != tab_count || g_strip_cache.active != target->window_tabs.active ||
            g_strip_cache.vertical != target->appPrefs.vertical_tabs ||
            g_strip_cache.layout != target->appPrefs.tab_layout ||
            g_strip_cache.max_rows != target->appPrefs.tab_max_rows) {
            const ui::WindowViewModel vm = BuildVm(*target, false);
            g_strip_cache.hwnd = below;
            g_strip_cache.width = width;
            g_strip_cache.scale = scale;
            g_strip_cache.count = tab_count;
            g_strip_cache.active = target->window_tabs.active;
            g_strip_cache.vertical = target->appPrefs.vertical_tabs;
            g_strip_cache.layout = target->appPrefs.tab_layout;
            g_strip_cache.max_rows = target->appPrefs.tab_max_rows;
            g_strip_cache.spans.clear();
            for (int i = 0; i < static_cast<int>(vm.tabs.size()); ++i) {
                D2D1_RECT_F rc{};
                if (!target->renderer.TabItemRect(vm, width, i, &rc)) continue;
                g_strip_cache.spans.push_back(TabSpan{static_cast<size_t>(i),
                                                      RECT{std::lround(rc.left), std::lround(rc.top),
                                                           std::lround(rc.right), std::lround(rc.bottom)}});
            }
        }
        POINT client = cursor;
        ScreenToClient(below, &client);
        const LONG caption_buttons = std::lround(3.0f * 46.0f * scale);  // minimize, maximize, close
        const int index = TabDropIndex(g_strip_cache.spans.data(), g_strip_cache.spans.size(), tab_count, client,
                                       std::lround(12.0f * scale), std::lround(width) - caption_buttons);
        Drop over;
        over.over = below;
        if (index >= 0) {
            ForgetDwell();
            over.window = below;
            over.index = static_cast<size_t>(index);
            return over;
        }
        if (!content) return over;
        Drop drop = ContentDropAt(moving, *target, client, width);
        drop.over = below;
        if (!drop.window) {
            ForgetDwell();
            return over;  // on its toolbar, sidebar or status bar: a new window
        }
        if (below != g_source) {
            ForgetDwell();
            return drop;
        }
        if (ContentDropArmed(below, cursor)) return drop;
        over.resting = true;
        return over;
    }
    ForgetDwell();
    return {};
}

LRESULT CALLBACK EscapeHook(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && wparam == VK_ESCAPE && (lparam & 0x80000000) == 0)
        g_escape_pressed = true;
    return CallNextHookEx(g_escape_hook, code, wparam, lparam);
}

bool PrimaryButtonDown() {
    const int button = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    return (GetAsyncKeyState(button) & 0x8000) != 0;
}

// Drops the strip drag as WM_CAPTURECHANGED does; the tabs never reordered.
void EndStripDrag(AppState& s) {
    s.tabDragPending = false;
    s.tabDragging = false;
    s.tabDragIndex = -1;
    s.tabDragRunPos = 0;
    s.tabDragRunLen = 1;
    s.tabDragFromChip = false;
    s.tabDragGroupId = 0;
    s.tabDragSlots = 1.0f;
    s.tabOrder.clear();
    s.tabTracks.clear();
    s.tabOffsets.clear();
    s.chipTracks.clear();
    s.chipOffsets.clear();
    if (GetCapture() == s.hwnd) ReleaseCapture();
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
// Opens tab `index` of `s` as a window of the size of `s` whose client point
// `grab` (where the cursor holds its tab) is under the cursor, and starts its
// move loop.
HWND OpenDetachedWindow(AppState& s, size_t index, POINT grab) {
    RECT window{};
    GetWindowRect(s.hwnd, &window);
    POINT origin{0, 0};
    ClientToScreen(s.hwnd, &origin);
    POINT offset{origin.x - window.left, origin.y - window.top};
    SIZE size{window.right - window.left, window.bottom - window.top};
    if (IsZoomed(s.hwnd)) {
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (GetWindowPlacement(s.hwnd, &placement)) {
            size = SIZE{placement.rcNormalPosition.right - placement.rcNormalPosition.left,
                        placement.rcNormalPosition.bottom - placement.rcNormalPosition.top};
        }
        offset = POINT{0, 0};
    }
    POINT cursor{};
    GetCursorPos(&cursor);

    WindowInit init;
    if (const app::Tab* view = s.window_tabs.items[index]->ActiveFolder()) init.path = view->current_path;
    init.adopt_from = &s;
    init.adopt_index = index;
    init.rect = DetachedWindowRect(size, cursor, grab, offset);
    const HWND hwnd = OpenPulseWindow(s, init);
    if (hwnd) {
        g_grab = Grab{hwnd, POINT{cursor.x - init.rect->left, cursor.y - init.rect->top}};
        PostMessageW(hwnd, WM_BEGIN_TAB_DETACH, reinterpret_cast<WPARAM>(s.hwnd), static_cast<LPARAM>(index));
    }
    return hwnd;
}

// The first visible tab of `s`'s strip, client px.
bool FirstTabRect(AppState& s, const ui::WindowViewModel& vm, D2D1_RECT_F* rect) {
    const float width = static_cast<float>(s.compositor.Width());
    for (int i = 0; i < static_cast<int>(vm.tabs.size()); ++i)
        if (s.renderer.TabItemRect(vm, width, i, rect)) return true;
    return false;
}
}  // namespace

bool TabDetachDistanceReached(POINT press, POINT now, float scale) {
    const float limit = kTabDetachDistanceDip * (scale > 0.0f ? scale : 1.0f);
    return std::fabs(static_cast<float>(now.y - press.y)) >= limit;
}

RECT DetachedWindowRect(SIZE size, POINT cursor, POINT grab_client, POINT client_offset) {
    const LONG left = cursor.x - grab_client.x - client_offset.x;
    const LONG top = cursor.y - grab_client.y - client_offset.y;
    return RECT{left, top, left + size.cx, top + size.cy};
}

int TabDropIndex(const TabSpan* spans, size_t span_count, size_t count, POINT pt, LONG slack,
                 LONG strip_right) {
    if (!spans || span_count == 0) return -1;
    LONG top = spans[0].rect.top, bottom = spans[0].rect.bottom;
    LONG left = spans[0].rect.left;
    for (size_t i = 1; i < span_count; ++i) {
        top = (std::min)(top, spans[i].rect.top);
        bottom = (std::max)(bottom, spans[i].rect.bottom);
        left = (std::min)(left, spans[i].rect.left);
    }
    if (pt.y < top - slack || pt.y > bottom + slack || pt.x < left - slack || pt.x > strip_right) return -1;
    for (size_t i = 0; i < span_count; ++i) {
        if (pt.x < (spans[i].rect.left + spans[i].rect.right) / 2) return static_cast<int>(spans[i].index);
    }
    return static_cast<int>(count);
}

void TrackTabDrop(AppState& s) {
    if (g_moving != s.hwnd) return;
    POINT cursor{};
    GetCursorPos(&cursor);
    // Content drops are for a dragged tab, never for a window moved by its
    // title bar (M6c merges at the strip only).
    const bool tab_drag = !g_caption_move && s.window_tabs.items.size() == 1;
    const Drop drop = s.window_tabs.items.empty() ? Drop{} : FindDrop(s, cursor, tab_drag);
    ShowDrop(s, drop);
    if (!tab_drag) {
        SetMovingAlpha(s.hwnd, drop.window ? kSeeThroughAlpha : 255);
        return;
    }
    // A dragged tab over a Pulse window (M7h): the window under the cursor
    // draws it as a pill with a hint, and its own window hides so the drop
    // preview is never covered. Elsewhere it is a window again.
    ShowGhost(s, drop, cursor);
    SetMovingAlpha(s.hwnd, drop.over ? 0 : 255);
}

void BeginWindowMove(AppState& s) {
    if (g_moving || s.window_tabs.items.empty()) return;  // our own loop ends itself (M6a, M6b)
    g_moving = s.hwnd;
    g_moving_state = &s;
    g_caption_move = true;
    g_drop = Drop{};
    g_strip_cache = StripCache{};
    g_before = WINDOWPLACEMENT{sizeof(WINDOWPLACEMENT)};
    GetWindowPlacement(s.hwnd, &g_before);
    g_escape_pressed = false;
    if (!g_escape_hook) g_escape_hook = SetWindowsHookExW(WH_KEYBOARD, EscapeHook, nullptr, GetCurrentThreadId());
}

void EndWindowMove(AppState& s) {
    if (!g_caption_move || g_moving != s.hwnd) return;
    g_caption_move = false;
    ForgetDwell();
    g_strip_cache = StripCache{};
    g_moving = nullptr;
    g_moving_state = nullptr;
    if (g_escape_hook) UnhookWindowsHookEx(g_escape_hook);
    g_escape_hook = nullptr;
    const Drop drop = g_escape_pressed ? Drop{} : g_drop;
    ShowDrop(s, Drop{});
    HideGhost(s);
    SetMovingAlpha(s.hwnd, 255);
    // The move loop is still unwinding: merge once it has returned.
    if (drop.window)
        PostMessageW(s.hwnd, WM_MERGE_WINDOW, reinterpret_cast<WPARAM>(drop.window), static_cast<LPARAM>(drop.index));
}

void FinishWindowMerge(AppState& s, HWND target, size_t index) {
    AppState* const dst = WindowOf(s, target);
    if (!dst || dst == &s || !IsWindowVisible(target)) return;
    MergeWindowInto(s, *dst, index, g_before);
}

bool TryDetachDraggedTab(AppState& s, int mx, int my) {
    if ((!s.tabDragPending && !s.tabDragging) || s.tabDragFromChip) return false;
    const int index = s.tabDragIndex;
    if (index < 0 || index >= static_cast<int>(s.window_tabs.items.size())) return false;
    const POINT now{mx, my};
    if (s.window_tabs.items.size() < 2) {
        // A window's only tab carries the window (plan §5 P2).
        if (std::abs(mx - s.tabDragStartPt.x) < GetSystemMetrics(SM_CXDRAG) &&
            std::abs(my - s.tabDragStartPt.y) < GetSystemMetrics(SM_CYDRAG))
            return false;
        POINT press = s.tabDragStartPt;
        ClientToScreen(s.hwnd, &press);
        RECT window{};
        GetWindowRect(s.hwnd, &window);
        g_grab = Grab{s.hwnd, POINT{press.x - window.left, press.y - window.top}};
        EndStripDrag(s);
        PostMessageW(s.hwnd, WM_BEGIN_TAB_DETACH, 0, 0);
        return true;
    }
    if (!TabDetachDistanceReached(s.tabDragStartPt, now, s.scale)) return false;

    // Where the cursor held the tab, so it holds the new window's tab there.
    const ui::WindowViewModel vm = BuildVm(s, false);
    const float width = static_cast<float>(s.compositor.Width());
    D2D1_RECT_F first{}, self{};
    if (!FirstTabRect(s, vm, &first) || !s.renderer.TabItemRect(vm, width, index, &self)) return false;
    const POINT grab{static_cast<LONG>(std::lround(first.left)) + (s.tabDragStartPt.x -
                                                                     static_cast<LONG>(std::lround(self.left))),
                     s.tabDragStartPt.y};
    EndStripDrag(s);
    OpenDetachedWindow(s, static_cast<size_t>(index), grab);
    return true;
}

void ArmPaneDrag(AppState& s, const ui::HitTestResult& hit, int mx, int my) {
    s.paneDragPending = false;
    if (hit.region != ui::HitTestResult::PaneHeader || hit.pane_index < 0) return;
    const app::LayoutTab* const tab = s.window_tabs.Active();
    app::Pane* const pane = PaneAtSlot(s, hit.pane_index);
    if (!tab || !pane || tab->VisiblePanes().size() < 2) return;
    s.paneDragPending = true;
    s.paneDragPane = pane;
    s.paneDragStartPt = POINT{mx, my};
}

bool TryDetachDraggedPane(AppState& s, int mx, int my) {
    if (!s.paneDragPending) return false;
    if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
        s.paneDragPending = false;
        return false;
    }
    if (std::abs(mx - s.paneDragStartPt.x) < GetSystemMetrics(SM_CXDRAG) &&
        std::abs(my - s.paneDragStartPt.y) < GetSystemMetrics(SM_CYDRAG))
        return false;
    s.paneDragPending = false;

    // The cursor holds the new window's tab as far into it as it held the
    // pane header, within the tab.
    const ui::WindowViewModel vm = BuildVm(s, false);
    D2D1_RECT_F first{};
    const app::LayoutTab* const into = s.window_tabs.Active();
    const size_t back_to = s.window_tabs.active;
    LONG header_left = 0;
    for (size_t i = 0; i < vm.pane_slots.size(); ++i)
        if (PaneAtSlot(s, static_cast<int>(i)) == s.paneDragPane)
            header_left = static_cast<LONG>(std::lround(vm.pane_slots[i].rect.left));
    if (!into || !FirstTabRect(s, vm, &first)) return false;
    const LONG edge = static_cast<LONG>(std::lround(12.0f * s.scale));
    const LONG tab_left = static_cast<LONG>(std::lround(first.left));
    const LONG tab_width = (std::max)(static_cast<LONG>(std::lround(first.right - first.left)), 2 * edge + 1);
    const POINT grab{tab_left + std::clamp(s.paneDragStartPt.x - header_left, edge, tab_width - edge),
                     static_cast<LONG>(std::lround((first.top + first.bottom) / 2.0f))};

    app::PaneOrigin origin;
    const std::optional<size_t> index = SplitPaneIntoTab(s, s.paneDragPane, origin);
    s.paneDragPane = nullptr;
    if (!index) return false;
    if (GetCapture() == s.hwnd) ReleaseCapture();
    if (OpenDetachedWindow(s, *index, grab)) g_pane_back = PaneBack{s.hwnd, into, std::move(origin)};
    // The split, one pane fewer, stays in front here (if no window opened,
    // the pane is left as a tab of its own).
    SwitchTab(s, back_to);
    return true;
}

void RunDetachedTabMove(AppState& s, HWND from, size_t index) {
    const HWND hwnd = s.hwnd;
    const Grab grab = g_grab;
    g_grab = Grab{};
    PaneBack pane_back = std::move(g_pane_back);
    g_pane_back = PaneBack{};
    // Released before the loop could start: the window stays where it opened.
    if (!PrimaryButtonDown() || g_escape_hook) return;
    POINT cursor{};
    GetCursorPos(&cursor);
    if (grab.hwnd == hwnd && !IsZoomed(hwnd)) {
        SetWindowPos(hwnd, nullptr, cursor.x - grab.offset.x, cursor.y - grab.offset.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    g_escape_pressed = false;
    g_escape_hook = SetWindowsHookExW(WH_KEYBOARD, EscapeHook, nullptr, GetCurrentThreadId());
    g_moving = hwnd;
    g_moving_state = &s;
    g_source = from;
    g_drop = Drop{};
    g_dwell = Dwell{};
    g_strip_cache = StripCache{};
    // The window takes a moment to open while the hand keeps going, so the
    // cursor may already rest on a drop spot: look once now. Drops are only
    // tracked on WM_MOVING, and until the first mouse move the loop below
    // dispatches nothing, not even WM_PAINT, so paint the drop right away.
    TrackTabDrop(s);
    PaintDropNow();
    if (from) {
        SendMessageW(hwnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, MAKELPARAM(cursor.x, cursor.y));
    } else {
        // The only tab drags the window exactly as its title bar would,
        // which also brings a maximized window down to its normal size.
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(cursor.x, cursor.y));
    }
    if (g_escape_hook) UnhookWindowsHookEx(g_escape_hook);
    g_escape_hook = nullptr;
    if (IsWindow(hwnd)) ForgetDwell();
    g_dwell = Dwell{};
    g_moving = nullptr;
    g_moving_state = nullptr;
    g_source = nullptr;
    const HWND drop_window = g_drop.window;
    const size_t drop_index = g_drop.index;
    const app::ContentDrop drop_zone = g_drop.zone;
    const app::PanePlacement drop_placement = g_drop.placement;
    ShowDrop(s, Drop{});
    HideGhost(s);
    if (!IsWindow(hwnd)) return;

    // Released on another window's tab strip: the tab joins it there (M6b);
    // on its content: a new tab, or its panes at an edge (M6d, M7h). The
    // window, hidden over the target (M7h), stays hidden on the way out.
    AppState* const target = g_escape_pressed ? nullptr : WindowOf(s, drop_window);
    const bool joining = target && target != &s && s.window_tabs.items.size() == 1;
    const bool going_back = from && g_escape_pressed && s.window_tabs.items.size() == 1;
    if (joining || going_back) ShowWindow(hwnd, SW_HIDE);
    SetMovingAlpha(hwnd, 255);
    if (joining) {
        const bool pane = drop_zone != app::ContentDrop::None && drop_zone != app::ContentDrop::Center;
        if (pane ? MoveTabIntoPane(s, 0, *target, drop_placement, TransferSource::Closing)
                 : MoveLayoutTab(s, 0, *target, drop_index, TransferSource::Closing)) {
            DestroyWindow(hwnd);
            SetForegroundWindow(target->hwnd);
            return;
        }
        ShowWindow(hwnd, SW_SHOWNORMAL);
    }
    if (!from || !g_escape_pressed) return;

    // Esc: the tab goes back to its place (a pane to its split) and the new
    // window goes away.
    AppState& source = app::FindWindowSharingCore(s, [from](const AppState& w) { return w.hwnd == from; });
    if (&source == &s || s.window_tabs.items.size() != 1) {
        ShowWindow(hwnd, SW_SHOWNORMAL);
        return;
    }
    const bool back = pane_back.from == from &&
                      ReturnTabAsPane(s, 0, source, pane_back.into, pane_back.origin, TransferSource::Closing);
    if (!back && !MoveLayoutTab(s, 0, source, index, TransferSource::Closing)) {
        ShowWindow(hwnd, SW_SHOWNORMAL);
        return;
    }
    DestroyWindow(hwnd);
    SetForegroundWindow(from);
}
}  // namespace pulse

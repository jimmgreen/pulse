#include "app_list_selection.h"
#include "app_input.h"
#include "app_navigation.h"
#include "app_details_tree.h"
#include "file_press_selection.h"
#include "../common/runtime_log.h"
#include "../ui/ui_renderer.h"
#include <algorithm>
#include <cmath>

namespace pulse {
void ResetMarquee(AppState& s) {
    if (s.listSelection.auto_scroll && s.hwnd) KillTimer(s.hwnd, kTimerMarqueeScroll);
    s.listSelection.ResetBand();
    s.blankClickPane = nullptr;
    s.blankClickTab = nullptr;
    s.blankClickGeneration = 0;
}

void CancelListSelection(AppState& s) {
    s.deferredFileActivation = false;
    const bool visible = s.listSelection.BandActive();
    ResetMarquee(s);
    s.listSelection.Cancel();
    if (visible && s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

POINT MarqueeStartOnScreen(const AppState& s) {
    POINT start = s.listSelection.start;
    if (const app::Tab* tab = s.pane ? s.pane->ActiveTab() : nullptr) {
        start.x -= static_cast<LONG>(std::lround(tab->scroll_x - s.listSelection.origin_x));
        start.y -= static_cast<LONG>(std::lround(tab->scroll_y - s.listSelection.origin_y));
    }
    return start;
}

D2D1_RECT_F MarqueeDisplayRect(const AppState& s) {
    const POINT start = MarqueeStartOnScreen(s);
    return D2D1::RectF(static_cast<float>(std::min(start.x, s.listSelection.current.x)),
                       static_cast<float>(std::min(start.y, s.listSelection.current.y)),
                       static_cast<float>(std::max(start.x, s.listSelection.current.x)),
                       static_cast<float>(std::max(start.y, s.listSelection.current.y)));
}

// Bug 4: items are tested in content space, so rows scrolled out of view stay
// selected and the start stays anchored to the item where the drag began.
bool MarqueeOwnerMatches(const AppState& s, const app::Tab* tab) {
    return tab && s.listSelection.context == app::MarqueeContext{
        s.pane, tab, tab->snapshot.get(), tab->TreeEntries(), tab->view_generation } &&
        (!s.listSelection.revision_tracked || tab->selection_revision == s.listSelection.revision);
}

bool FilePressOwnerMatches(AppState& s, const app::Tab* tab) {
    const auto match = app::MatchFilePress(s.listSelection, s.pane, tab);
    if (match == app::FilePressMatch::Valid) return true;
    diagnostics::runtime::Event("file_press_cancel", {
        {"source_missing", match == app::FilePressMatch::SourceMissing},
        {"context_changed", match == app::FilePressMatch::ContextChanged},
        {"selection_changed", match == app::FilePressMatch::SelectionChanged},
        {"rebases", s.listSelection.file_rebases}});
    return false;
}

void ApplyMarqueeSelection(AppState& s) {
    s.listSelection.dirty = false;
    app::Tab* tab = ActiveTab(s);
    if (!MarqueeOwnerMatches(s, tab) || !tab->snapshot) {
        ResetMarquee(s);
        if (s.hwnd && GetCapture() == s.hwnd) ReleaseCapture();
        return;
    }
    const int n = static_cast<int>(tab->EntryCount());
    const D2D1_RECT_F list = ListRect(s);
    const POINT start = MarqueeStartOnScreen(s);
    // The pointer end stops at the list edge (auto-scroll brings more in);
    // the start end may lie far off screen.
    const float curX = std::clamp(static_cast<float>(s.listSelection.current.x), list.left, list.right);
    const float curY = std::clamp(static_cast<float>(s.listSelection.current.y), list.top, list.bottom);
    const float left = std::min(static_cast<float>(start.x), curX);
    const float top = std::min(static_cast<float>(start.y), curY);
    // A straight vertical (or horizontal) drag still crosses the rows under
    // it, as in File Explorer: give the band at least 1 px of extent.
    const float right = std::max(std::max(static_cast<float>(start.x), curX), left + 1.0f);
    const float bottom = std::max(std::max(static_cast<float>(start.y), curY), top + 1.0f);

    ui::PaneViewModel pane;
    app::FillPaneViewModel(pane, *s.pane, &s.places);
    auto& hits = s.listSelection.hits;
    hits.clear();
    if (n > 0 && right > left && bottom > top) {
        s.renderer.ItemsInRectInPane(pane, FocusedPaneRect(s),
            D2D1::RectF(left, top, right, bottom), hits);
        std::erase_if(hits, [&](int source) { return IsDetailsTreePlaceholder(*tab, source); });
    }
    if (s.listSelection.delta.Apply(tab->selected, tab->all_selected, s.listSelection.base,
            s.listSelection.additive, s.listSelection.toggle, n, hits)) ++tab->selection_revision;
    s.listSelection.revision = tab->selection_revision;
    s.listSelection.revision_tracked = true;
    if (!tab->all_selected && tab->selected.empty()) {
        tab->selected_index = -1;
        return;
    }
    const int focus = s.renderer.ItemFromPointInPane(pane, FocusedPaneRect(s), curX, curY);
    if (tab->IsSelected(focus)) tab->selected_index = focus;
    else if (!tab->all_selected) tab->selected_index = *tab->selected.begin();
    else if (!tab->IsSelected(tab->selected_index)) tab->selected_index = 0;
    if (tab->selection_anchor < 0) tab->selection_anchor = tab->selected_index;
}

namespace {
// Scroll speed in pixels per millisecond for a pointer `beyond` pixels past
// the edge zone: about 125 px/s at the edge, up to 3750 px/s far outside.
// Time-based, so uneven frame or timer intervals do not change the speed.
float MarqueeScrollSpeed(float beyond, float scale) {
    return std::clamp(beyond * 0.022f, 0.125f * scale, 3.75f * scale);
}

// Signed distance past the auto-scroll zone on each axis (0 = inside).
void MarqueeEdgeOverflow(const AppState& s, float& dx, float& dy) {
    const D2D1_RECT_F list = ListRect(s);
    const float zone = 12.0f * s.scale;   // a maximized window has no room below the list
    const float x = static_cast<float>(s.listSelection.current.x), y = static_cast<float>(s.listSelection.current.y);
    dx = dy = 0.0f;
    if (y < list.top + zone) dy = y - (list.top + zone);
    else if (y > list.bottom - zone) dy = y - (list.bottom - zone);
    if (x < list.left + zone) dx = x - (list.left + zone);
    else if (x > list.right - zone) dx = x - (list.right - zone);
}

void StopMarqueeAutoScroll(AppState& s) {
    if (s.listSelection.auto_scroll && s.hwnd) KillTimer(s.hwnd, kTimerMarqueeScroll);
    s.listSelection.auto_scroll = false;
}

// Requests the next frame: on the display clock when the pump runs.
void RequestMarqueeFrame(AppState& s) {
    if (s.framePump.Running()) s.framePump.Arm();
    else if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}
} // namespace

void UpdateMarqueeAutoScroll(AppState& s) {
    float dx = 0.0f, dy = 0.0f;
    if (s.listSelection.BandActive()) MarqueeEdgeOverflow(s, dx, dy);
    const app::Tab* tab = ActiveTab(s);
    const bool horizontal = tab && tab->view_mode == ui::ViewMode::List;
    const bool want = s.listSelection.BandActive() && (horizontal ? dx != 0.0f : dy != 0.0f);
    if (want == s.listSelection.auto_scroll || !s.hwnd) return;
    if (!want) { StopMarqueeAutoScroll(s); return; }
    s.listSelection.auto_scroll = true;
    s.listSelection.scroll_last = std::chrono::steady_clock::now();
    // The timer keeps frames coming while the pointer rests (no mouse moves).
    SetTimer(s.hwnd, kTimerMarqueeScroll, 16, nullptr);
    RequestMarqueeFrame(s);
}

void TickMarqueeAutoScroll(AppState& s) {
    if (!s.listSelection.BandActive() || !ActiveTab(s) || (GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
        StopMarqueeAutoScroll(s);
        return;
    }
    RequestMarqueeFrame(s);
}

// Called by Render once per frame before the view model is built.
void UpdateMarqueeFrame(AppState& s) {
    if ((s.listSelection.BandActive() || s.listSelection.PendingBand()) && !MarqueeOwnerMatches(s, ActiveTab(s))) {
        ResetMarquee(s);
        if (s.hwnd && GetCapture() == s.hwnd) ReleaseCapture();
        return;
    }
    if (s.listSelection.auto_scroll) {
        app::Tab* tab = ActiveTab(s);
        if (!s.listSelection.BandActive() || !tab || (GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
            StopMarqueeAutoScroll(s);
        } else {
            // The pointer may rest outside the window; follow it without mouse moves.
            POINT cursor{};
            if (GetCursorPos(&cursor) && ScreenToClient(s.hwnd, &cursor)) s.listSelection.current = cursor;
            const auto now = std::chrono::steady_clock::now();
            const float elapsed = static_cast<float>(std::clamp(
                std::chrono::duration<double, std::milli>(now - s.listSelection.scroll_last).count(), 0.0, 50.0));
            s.listSelection.scroll_last = now;
            float dx = 0.0f, dy = 0.0f;
            MarqueeEdgeOverflow(s, dx, dy);
            const bool horizontal = tab->view_mode == ui::ViewMode::List;
            const float over = horizontal ? dx : dy;
            if (over == 0.0f) {
                StopMarqueeAutoScroll(s);
            } else {
                if (s.scrollAnimating) CancelScrollAnimation(s);
                float& position = horizontal ? tab->scroll_x : tab->scroll_y;
                const float before = position;
                const float step = MarqueeScrollSpeed(std::abs(over), s.scale) * elapsed;
                position += over < 0.0f ? -step : step;
                ClampScroll(s);
                s.scrollTargetX = tab->scroll_x;
                s.scrollTargetY = tab->scroll_y;
                if (position != before) {
                    s.listSelection.dirty = true;
                    RefreshScrolledHover(s);
                    MaybePrefetchSearchPage(s);
                }
            }
        }
    }
    if (s.listSelection.dirty && s.listSelection.BandActive()) ApplyMarqueeSelection(s);
}

void HandleListRowClick(AppState& s, int index, bool ctrl, bool shift) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    s.listSelection.ClearDeferred();
    if (shift) {
        const int anchor = tab->selection_anchor >= 0 ? tab->selection_anchor
            : (tab->selected_index >= 0 ? tab->selected_index : index);
        ui::PaneViewModel view;
        app::FillPaneViewModel(view, *s.pane, &s.places);
        // Ctrl+Shift adds the range to what is already selected (Explorer).
        if (ctrl) tab->AddRange(anchor, index, &view);
        else tab->SelectRange(anchor, index, &view);
    } else if (ctrl) {
        // A selected item remains part of the drag payload. Only a completed
        // Ctrl-click (without a drag) removes it from the selection.
        if (tab->IsSelected(index)) {
            tab->selected_index = index;
            s.listSelection.DeferToggle(index);
        } else {
            tab->ToggleSelect(index);
        }
    } else if (tab->IsSelected(index) && tab->SelectedCount() > 1) {
        tab->selected_index = index;
        s.listSelection.DeferReplace(index);
    } else {
        tab->SelectOnly(index);
    }
}

void FinishListRowClick(AppState& s) {
    if (app::Tab* tab = ActiveTab(s); tab && s.listSelection.DeferredIndex() >= 0 &&
        (!s.listSelection.PendingFile() || FilePressOwnerMatches(s, tab))) {
        if (s.listSelection.ToggleOnRelease()) tab->ToggleSelect(s.listSelection.DeferredIndex());
        else tab->SelectOnly(s.listSelection.DeferredIndex());
        // A no-button mouse move can finish the click without WM_LBUTTONUP.
        // Hover damage alone cannot erase the other previously selected rows.
        if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
    }
    s.listSelection.ClearDeferred();
}

void CommitListRowClick(AppState& s, int index, bool ctrl, bool shift) {
    HandleListRowClick(s, index, ctrl, shift);
    FinishListRowClick(s);
}

// Esc during a marquee puts this selection back.
static void RememberSelectionForMarquee(AppState& s, app::Tab* tab) {
    s.listSelection.delta.Reset();
    s.listSelection.hits.clear();
    s.listSelection.context = {s.pane, tab, tab ? tab->snapshot.get() : nullptr,
        tab ? tab->TreeEntries() : nullptr, tab ? tab->view_generation : 0};
    s.listSelection.revision_tracked = false;
    s.listSelection.before.clear();
    s.listSelection.before_all = tab && tab->all_selected;
    s.listSelection.before_index = tab ? tab->selected_index : -1;
    s.listSelection.before_anchor = tab ? tab->selection_anchor : -1;
    if (tab && !tab->all_selected) s.listSelection.before = tab->selected;
}

void BeginListBand(AppState& s, app::Tab* tab, POINT point, int row, bool ctrl, bool shift) {
    CancelListSelection(s);
    RememberSelectionForMarquee(s, tab);
    auto& gesture = s.listSelection;
    gesture.ArmBand();
    gesture.additive = ctrl || shift;
    gesture.toggle = ctrl;
    gesture.row_index = row;
    gesture.row_ctrl = ctrl;
    gesture.row_shift = shift;
    gesture.start = gesture.current = point;
    gesture.origin_x = tab ? tab->scroll_x : 0;
    gesture.origin_y = tab ? tab->scroll_y : 0;
    if (tab) {
        if (ctrl || shift) {
            tab->MaterializeSelection();
            gesture.base = tab->selected;
        } else if (row >= 0) {
            CommitListRowClick(s, row, false, false);
        } else {
            tab->ClearSelection();
        }
    }
    gesture.revision = tab ? tab->selection_revision : 0;
    gesture.revision_tracked = true;
}

void BeginListFilePress(AppState& s, int row, bool ctrl, bool shift, POINT point) {
    CancelListSelection(s);
    HandleListRowClick(s, row, ctrl, shift);
    const auto* tab = ActiveTab(s);
    auto& gesture = s.listSelection;
    gesture.context = {s.pane, tab, tab ? tab->snapshot.get() : nullptr,
        tab ? tab->TreeEntries() : nullptr, tab ? tab->view_generation : 0};
    gesture.revision = tab ? tab->selection_revision : 0;
    gesture.revision_tracked = true;
    gesture.file_start = point;
    if (tab && row >= 0 && static_cast<size_t>(row) < tab->EntryCount()) {
        app::CaptureFilePressIdentity(gesture, *tab, row);
    }
    gesture.ArmFile();
}

int FinishListBand(AppState& s, POINT point) {
    auto& gesture = s.listSelection;
    const bool valid = MarqueeOwnerMatches(s, ActiveTab(s));
    const int row = valid && gesture.PendingBand() ? gesture.row_index : -1;
    const bool ctrl = gesture.row_ctrl, shift = gesture.row_shift;
    if (valid && gesture.BandActive()) {
        gesture.current = point;
        ApplyMarqueeSelection(s);
    }
    ResetMarquee(s);
    if (row >= 0 && (ctrl || shift)) CommitListRowClick(s, row, ctrl, shift);
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
    return row;
}

bool CancelMarquee(AppState& s) {
    if (!s.listSelection.BandActive() && !s.listSelection.PendingBand()) return false;
    if (app::Tab* tab = ActiveTab(s); MarqueeOwnerMatches(s, tab) && s.listSelection.BandActive()) {
        const bool same_selection = tab->all_selected == s.listSelection.before_all
            ? tab->selected == s.listSelection.before
            : (tab->all_selected ? s.listSelection.before.size() : tab->selected.size()) == tab->EntryCount();
        if (!same_selection) ++tab->selection_revision;
        tab->all_selected = s.listSelection.before_all;
        tab->selected = s.listSelection.before;
        tab->selected_index = s.listSelection.before_index;
        tab->selection_anchor = s.listSelection.before_anchor;
    }
    ResetMarquee(s);
    if (s.hwnd && GetCapture() == s.hwnd) ReleaseCapture();
    return true;
}

} // namespace pulse

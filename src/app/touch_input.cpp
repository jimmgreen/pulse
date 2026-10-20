#include "touch_input.h"
#include "app_runtime.h"
#include "app_state.h"
#include "app_model.h"
#include "app_commands.h"
#include "app_input.h"
#include "app_navigation.h"
#include "app_ops_ui.h"
#include "pane_layout.h"
#include "../ui/ui_renderer.h"
#include <windowsx.h>
#include <algorithm>

namespace pulse::app {

TouchInputAdapter::TouchInputAdapter(pulse::AppState& state) : state_(state) {}

bool TouchInputAdapter::ClassifyPointer(WPARAM wParam, uint32_t& out_pointer_id, POINTER_INPUT_TYPE& out_type) {
    const uint32_t id = static_cast<uint32_t>(GET_POINTERID_WPARAM(wParam));
    out_pointer_id = id;

    if (owned_touch_pointers_.contains(id)) {
        out_type = PT_TOUCH;
        return true;
    }

    POINTER_INPUT_TYPE type = PT_POINTER;
    if (!GetPointerType(id, &type)) {
        out_type = PT_POINTER;
        return false;
    }

    out_type = type;
    return type == PT_TOUCH;
}

bool TouchInputAdapter::HandlePointerMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& out_result) {
    uint32_t pointer_id = static_cast<uint32_t>(GET_POINTERID_WPARAM(wParam));
    POINTER_INPUT_TYPE type = PT_POINTER;
    if (msg == WM_POINTERDOWN && owned_touch_pointers_.erase(pointer_id) != 0)
        CancelActiveGesture();

    if (!ClassifyPointer(wParam, pointer_id, type)) {
        return false; // Leave PT_MOUSE, PT_PEN, etc. to default Windows processing
    }

    // Updates or capture notifications after UP must not revive a finished
    // contact or cancel the inertia it just started (pointer ids are reusable).
    if (msg != WM_POINTERDOWN && !owned_touch_pointers_.contains(pointer_id))
        return false;

    if (IS_POINTER_CANCELED_WPARAM(wParam)) {
        CancelActiveGesture();
        if (msg == WM_POINTERUP || msg == WM_POINTERCAPTURECHANGED)
            owned_touch_pointers_.erase(pointer_id);
        out_result = 0;
        return true;
    }

    const uint64_t now_ms = GetTickCount64();
    POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    ScreenToClient(hwnd, &pt);
    const float x = static_cast<float>(pt.x);
    const float y = static_cast<float>(pt.y);

    switch (msg) {
    case WM_POINTERDOWN: {
        // Deactivation/capture loss can discard a terminal message. Do not let
        // such an old contact permanently turn later single touches into multi-touch.
        bool removed_stale_contact = false;
        for (auto it = owned_touch_pointers_.begin(); it != owned_touch_pointers_.end();) {
            POINTER_INFO info{};
            if (!GetPointerInfo(*it, &info) || (info.pointerFlags & POINTER_FLAG_INCONTACT) == 0) {
                it = owned_touch_pointers_.erase(it);
                removed_stale_contact = true;
            } else {
                ++it;
            }
        }
        if (removed_stale_contact) CancelActiveGesture();
        owned_touch_pointers_.insert(pointer_id);
        inertia_.Stop();
        CancelScrollAnimation(state_);
        if (owned_touch_pointers_.size() > 1) {
            CancelActiveGesture();
        } else {
            reducer_.OnDown(pointer_id, x, y, now_ms, state_.scale);
            SetupScrollTarget(x, y);
            if (Tab* tab = ActiveTab(state_)) {
                state_.scrollTargetX = tab->scroll_x;
                state_.scrollTargetY = tab->scroll_y;
            }
        }
        out_result = 0;
        return true;
    }

    case WM_POINTERUPDATE: {
        const TouchAction action = reducer_.OnUpdate(pointer_id, x, y, now_ms, state_.scale);
        if (action.kind == TouchActionKind::PanStart || action.kind == TouchActionKind::PanUpdate) {
            ApplyScrollDelta(action.delta_x, action.delta_y, hwnd);
        }
        out_result = 0;
        return true;
    }

    case WM_POINTERUP: {
        if (scroll_target_.kind != TouchScrollTargetKind::None && !RefreshScrollTarget())
            CancelActiveGesture();
        const TouchAction action = reducer_.OnUp(pointer_id, x, y, now_ms, state_.scale);
        owned_touch_pointers_.erase(pointer_id);

        if (action.kind == TouchActionKind::Tap) {
            scroll_target_ = {};
            DispatchTap(hwnd, action.x, action.y);
        } else if (action.kind == TouchActionKind::DoubleTap) {
            scroll_target_ = {};
            DispatchDoubleTap(hwnd, action.x, action.y);
        } else if (action.kind == TouchActionKind::PanEnd &&
                   scroll_target_.kind != TouchScrollTargetKind::None) {
            // The target supports one axis only; unused velocity must not keep
            // a timer alive after the scrollable axis has hit an edge.
            inertia_.StartForScrollAxis(action.velocity_x, action.velocity_y,
                                        scroll_target_.column_major);
            last_tick_time_ms_ = now_ms;
        }
        out_result = 0;
        return true;
    }

    case WM_POINTERCAPTURECHANGED: {
        owned_touch_pointers_.erase(pointer_id);
        CancelActiveGesture();
        out_result = 0;
        return true;
    }

    default:
        break;
    }

    return false;
}

void TouchInputAdapter::CancelActiveGesture() {
    reducer_.OnCancel(reducer_.ActivePointerId());
    inertia_.Stop();
    scroll_target_ = {};
    last_tick_time_ms_ = 0;
    // Keep consuming canceled contacts until their terminal messages arrive.
}

void TouchInputAdapter::OnTimerTick(HWND hwnd) {
    const uint64_t now_ms = GetTickCount64();
    if (scroll_target_.kind != TouchScrollTargetKind::None && !RefreshScrollTarget()) {
        CancelActiveGesture();
        return;
    }

    // Check for hold threshold
    const TouchAction hold = reducer_.CheckHold(now_ms);
    if (hold.kind == TouchActionKind::Hold) {
        scroll_target_ = {};
        DispatchHold(hwnd, hold.x, hold.y);
    }

    // A newly started wheel animation is a new owner, not a second simulator.
    if (inertia_.IsActive() && state_.scrollAnimating) {
        CancelActiveGesture();
        return;
    }

    // Advance inertia
    if (inertia_.IsActive()) {
        float dt = (last_tick_time_ms_ > 0) ? static_cast<float>(now_ms - last_tick_time_ms_) / 1000.0f : 0.016f;
        dt = std::clamp(dt, 0.001f, 0.1f);
        float dx_dip = 0.0f;
        float dy_dip = 0.0f;
        if (inertia_.Step(dt, dx_dip, dy_dip)) {
            ApplyScrollDelta(dx_dip * state_.scale, dy_dip * state_.scale, hwnd);
        }
    }

    last_tick_time_ms_ = now_ms;
    if (!inertia_.IsActive() && reducer_.State() != TouchGestureState::Pending &&
        reducer_.State() != TouchGestureState::Panning) scroll_target_ = {};
}

void TouchInputAdapter::SetupScrollTarget(float x, float y) {
    scroll_target_ = {};
    const ui::WindowViewModel vm = BuildVm(state_, false);
    const D2D1_RECT_F rect = D2D1::RectF(0, 0,
        static_cast<float>(state_.compositor.Width()),
        static_cast<float>(state_.compositor.Height()));
    const ui::HitTestResult hit = state_.renderer.HitTest(vm, rect, x, y);

    if (vm.settings_open && y >= state_.renderer.TitleBarHeight() &&
        hit.region != ui::HitTestResult::StatusBar &&
        hit.region != ui::HitTestResult::StatusBarTask &&
        hit.region != ui::HitTestResult::StatusBarCancelSearch &&
        hit.region != ui::HitTestResult::StatusHintAction) {
        // Settings controls and blank content do not form a contiguous enum range.
        scroll_target_.kind = TouchScrollTargetKind::Settings;
        scroll_target_.pane = state_.pane;
        scroll_target_.settings_page = state_.settings.page();
    } else if ((hit.region >= ui::HitTestResult::SidebarHeader &&
                hit.region <= ui::HitTestResult::SidebarBlank) ||
               (hit.region == ui::HitTestResult::Scrollbar && hit.sub_index == 2)) {
        scroll_target_.kind = TouchScrollTargetKind::Sidebar;
    } else if (!vm.settings_open && hit.pane_index >= 0 &&
               static_cast<size_t>(hit.pane_index) < vm.pane_slots.size() &&
               hit.region != ui::HitTestResult::ColumnStripRow &&
               hit.region != ui::HitTestResult::ColumnStripBlank &&
               hit.region != ui::HitTestResult::ColumnStripDivider &&
               hit.region != ui::HitTestResult::ColumnStripHScroll) {
        scroll_target_.kind = TouchScrollTargetKind::Pane;
        scroll_target_.pane = PaneAtSlot(state_, hit.pane_index);
        FocusPane(state_, scroll_target_.pane);
    }

    scroll_target_.layout = state_.window_tabs.Active();
    if (scroll_target_.pane) {
        scroll_target_.tab = scroll_target_.pane->ActiveTab();
        scroll_target_.path = scroll_target_.tab->current_path;
        scroll_target_.view_generation = scroll_target_.tab->view_generation;
        scroll_target_.view_mode = static_cast<int>(scroll_target_.tab->view_mode);
        scroll_target_.column_major = scroll_target_.kind == TouchScrollTargetKind::Pane &&
            scroll_target_.tab->view_mode == ui::ViewMode::List;
    }
    if (!RefreshScrollTarget()) scroll_target_ = {};
}

bool TouchInputAdapter::RefreshScrollTarget() {
    LayoutTab* layout = state_.window_tabs.Active();
    if (!layout || layout != scroll_target_.layout) return false;

    const float width = static_cast<float>(state_.compositor.Width());
    const float height = static_cast<float>(state_.compositor.Height());
    if (scroll_target_.kind == TouchScrollTargetKind::Sidebar) {
        const ui::WindowViewModel vm = BuildVm(state_, false);
        if (vm.settings_open) return false;
        scroll_target_.max_scroll_y = state_.renderer.SidebarMaxScroll(vm, width, height);
        return true;
    }
    if (scroll_target_.kind == TouchScrollTargetKind::None) return false;

    // Compare addresses against current ownership before dereferencing any
    // saved pane/tab: tab close, layout replacement and navigation can occur
    // between two pointer messages or inertia ticks.
    const auto live = std::find_if(layout->panes.begin(), layout->panes.end(),
        [this](const auto& pane) { return pane.get() == scroll_target_.pane; });
    if (live == layout->panes.end() || !*live || state_.pane != live->get()) return false;
    Tab* tab = (*live)->ActiveTab();
    if (tab != scroll_target_.tab || tab->current_path != scroll_target_.path ||
        tab->view_generation != scroll_target_.view_generation ||
        static_cast<int>(tab->view_mode) != scroll_target_.view_mode) return false;

    if (scroll_target_.kind == TouchScrollTargetKind::Settings) {
        if (!IsSettingsTab(tab) || state_.settings.page() != scroll_target_.settings_page) return false;
        const ui::WindowViewModel vm = BuildVm(state_, false);
        scroll_target_.max_scroll_y = state_.renderer.SettingsMaxScroll(vm, width, height);
        return true;
    }
    if (IsSettingsTab(tab) || !layout->root) return false;
    std::vector<std::pair<Pane*, D2D1_RECT_F>> panes;
    LayoutWindowPanes(*layout->root, state_.renderer.ContentRect(width, height), state_.scale, panes);
    const auto bounds = std::find_if(panes.begin(), panes.end(),
        [this](const auto& pane) { return pane.first == scroll_target_.pane; });
    if (bounds == panes.end()) return false;
    ui::PaneViewModel pane_vm;
    FillPaneViewModel(pane_vm, **live, &state_.places);
    scroll_target_.max_scroll_x = state_.renderer.MaxScrollXForPane(pane_vm, bounds->second);
    scroll_target_.max_scroll_y = state_.renderer.MaxScrollForPane(pane_vm, bounds->second);
    return true;
}

void TouchInputAdapter::ApplyScrollDelta(float delta_x, float delta_y, HWND hwnd) {
    if (!RefreshScrollTarget()) {
        CancelActiveGesture();
        return;
    }

    CancelScrollAnimation(state_);
    TouchScrollStep step;
    if (scroll_target_.kind == TouchScrollTargetKind::Pane) {
        Tab& tab = *scroll_target_.tab; // Validated against live ownership above.
        state_.scrollTargetX = tab.scroll_x;
        state_.scrollTargetY = tab.scroll_y;
        if (scroll_target_.column_major) {
            step = ApplyTouchScrollAxis(tab.scroll_x, state_.scrollTargetX,
                                        delta_x, scroll_target_.max_scroll_x);
        } else {
            step = ApplyTouchScrollAxis(tab.scroll_y, state_.scrollTargetY,
                                        delta_y, scroll_target_.max_scroll_y);
        }
        if (step.changed) MaybePrefetchSearchPage(state_);
    } else if (scroll_target_.kind == TouchScrollTargetKind::Sidebar) {
        float unused_target = state_.sidebarScroll;
        step = ApplyTouchScrollAxis(state_.sidebarScroll, unused_target,
                                    delta_y, scroll_target_.max_scroll_y);
    } else if (scroll_target_.kind == TouchScrollTargetKind::Settings) {
        float position = state_.settings.scroll();
        float unused_target = position;
        step = ApplyTouchScrollAxis(position, unused_target, delta_y, scroll_target_.max_scroll_y);
        state_.settings.SetScroll(position, scroll_target_.max_scroll_y);
    }
    if (step.blocked) inertia_.Stop();
    if (step.changed) InvalidateRect(hwnd, nullptr, FALSE);
}

void TouchInputAdapter::DispatchTap(HWND hwnd, float x, float y) {
    const ui::WindowViewModel vm = BuildVm(state_);
    const D2D1_RECT_F rect = D2D1::RectF(0, 0,
        static_cast<float>(state_.compositor.Width()),
        static_cast<float>(state_.compositor.Height()));
    const ui::HitTestResult hit = state_.renderer.HitTest(vm, rect, x, y);

    if (hit.pane_index >= 0) {
        if (Pane* p = PaneAtSlot(state_, hit.pane_index)) FocusPane(state_, p);
    }

    Tab* tab = ActiveTab(state_);

    switch (hit.region) {
    case ui::HitTestResult::RowCheckbox:
        if (tab && hit.index >= 0) {
            tab->ToggleSelect(hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;

    case ui::HitTestResult::HeaderCheckbox:
        if (tab && tab->CountBound() > 0) {
            bool all_sel = true;
            size_t selectable_count = 0;
            std::vector<int> selectable;
            selectable.reserve(static_cast<size_t>(tab->CountBound()));
            for (int i = 0; i < tab->CountBound(); ++i) {
                if (!tab->EntryVisible(i)) continue;
                ++selectable_count;
                selectable.push_back(i);
                if (!tab->IsSelected(i)) all_sel = false;
            }
            if (selectable_count > 0) {
                if (all_sel) tab->ClearSelection();
                else tab->SelectIndices(selectable);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        break;

    case ui::HitTestResult::Row:
        if (tab && hit.index >= 0) {
            tab->SelectOnly(hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;

    case ui::HitTestResult::Tab:
        if (hit.index >= 0) {
            SwitchTab(state_, static_cast<size_t>(hit.index));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;

    case ui::HitTestResult::TabClose:
        if (hit.index >= 0) {
            CloseLayoutTab(state_, static_cast<size_t>(hit.index));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;

    case ui::HitTestResult::TabNew:
        OpenNewTab(state_);
        InvalidateRect(hwnd, nullptr, FALSE);
        break;

    case ui::HitTestResult::NavBack:
        GoBack(state_);
        InvalidateRect(hwnd, nullptr, FALSE);
        break;

    case ui::HitTestResult::NavForward:
        GoForward(state_);
        InvalidateRect(hwnd, nullptr, FALSE);
        break;

    case ui::HitTestResult::NavUp:
        GoUp(state_);
        InvalidateRect(hwnd, nullptr, FALSE);
        break;

    case ui::HitTestResult::NavRefresh:
        DispatchMenuCommand(state_, app::CmdRefresh);
        InvalidateRect(hwnd, nullptr, FALSE);
        break;

    case ui::HitTestResult::Minimize:
        ShowWindow(hwnd, SW_MINIMIZE);
        break;

    case ui::HitTestResult::Maximize:
        ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        break;

    case ui::HitTestResult::Close:
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        break;

    case ui::HitTestResult::SettingsButton:
        OpenSettingsTab(state_, 0);
        InvalidateRect(hwnd, nullptr, FALSE);
        break;

    case ui::HitTestResult::ThemeToggle:
        ToggleTheme(state_);
        InvalidateRect(hwnd, nullptr, FALSE);
        break;

    case ui::HitTestResult::Cut:
        DispatchMenuCommand(state_, app::CmdCut);
        break;

    case ui::HitTestResult::Copy:
        DispatchMenuCommand(state_, app::CmdCopy);
        break;

    case ui::HitTestResult::Paste:
        DispatchMenuCommand(state_, app::CmdPaste);
        break;

    case ui::HitTestResult::Rename:
        DispatchMenuCommand(state_, app::CmdRename);
        break;

    case ui::HitTestResult::Delete:
        DeleteSelected(state_, false);
        break;

    case ui::HitTestResult::SidebarItem:
        if (!hit.path.empty()) {
            NavigateTo(state_, hit.path);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;

    case ui::HitTestResult::AddressBar:
    case ui::HitTestResult::BreadcrumbSegment:
        ShowOmnibar(state_, OmnibarMode::Path);
        InvalidateRect(hwnd, nullptr, FALSE);
        break;

    case ui::HitTestResult::SettingsNav:
        OpenSettingsTab(state_, hit.index);
        InvalidateRect(hwnd, nullptr, FALSE);
        break;

    case ui::HitTestResult::SettingsToggle:
        state_.settings.ToggleUi(hit.index);
        InvalidateRect(hwnd, nullptr, FALSE);
        break;

    default:
        break;
    }
}

void TouchInputAdapter::DispatchDoubleTap(HWND hwnd, float x, float y) {
    (void)hwnd;
    const ui::WindowViewModel vm = BuildVm(state_);
    const D2D1_RECT_F rect = D2D1::RectF(0, 0,
        static_cast<float>(state_.compositor.Width()),
        static_cast<float>(state_.compositor.Height()));
    const ui::HitTestResult hit = state_.renderer.HitTest(vm, rect, x, y);

    // Double-tap opens item body only; checkbox never triggers double open
    if (hit.region == ui::HitTestResult::Row && hit.index >= 0) {
        if (Tab* tab = ActiveTab(state_)) {
            tab->SelectOnly(hit.index);
            OpenSelected(state_);
        }
    }
}

void TouchInputAdapter::DispatchHold(HWND hwnd, float x, float y) {
    POINT sp{ static_cast<int>(x), static_cast<int>(y) };
    ClientToScreen(hwnd, &sp);

    const ui::WindowViewModel vm = BuildVm(state_);
    const D2D1_RECT_F rect = D2D1::RectF(0, 0,
        static_cast<float>(state_.compositor.Width()),
        static_cast<float>(state_.compositor.Height()));
    const ui::HitTestResult hit = state_.renderer.HitTest(vm, rect, x, y);

    if (hit.region == ui::HitTestResult::Row && hit.index >= 0) {
        if (Tab* tab = ActiveTab(state_)) {
            if (!tab->IsSelected(hit.index)) {
                tab->SelectOnly(hit.index);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            ShowItemContextMenu(state_, sp);
        }
    } else {
        HandleRButtonUp(&state_, hwnd, WM_RBUTTONUP, 0, MAKELPARAM(static_cast<int>(x), static_cast<int>(y)));
    }
}

} // namespace pulse::app

#include "touch_input.h"
#include "app_runtime.h"
#include "app_state.h"
#include "app_model.h"
#include "app_commands.h"
#include "app_input.h"
#include "../ui/ui_renderer.h"
#include <windowsx.h>
#include <algorithm>

namespace pulse::app {

TouchInputAdapter::TouchInputAdapter(AppState& state) : state_(state) {}

bool TouchInputAdapter::ClassifyPointer(WPARAM wParam, uint32_t& out_pointer_id, POINTER_INPUT_TYPE& out_type) {
    const uint32_t id = static_cast<uint32_t>(GET_POINTERID_WPARAM(wParam));
    out_pointer_id = id;

    if (fallback_pointers_.contains(id)) {
        out_type = PT_POINTER;
        return false;
    }

    if (owned_touch_pointers_.contains(id)) {
        out_type = PT_TOUCH;
        return true;
    }

    POINTER_INPUT_TYPE type = PT_POINTER;
    if (!GetPointerType(id, &type)) {
        fallback_pointers_.insert(id);
        out_type = PT_POINTER;
        return false;
    }

    out_type = type;
    if (type == PT_TOUCH) {
        owned_touch_pointers_.insert(id);
        return true;
    }

    fallback_pointers_.insert(id);
    return false;
}

bool TouchInputAdapter::HandlePointerMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& out_result) {
    uint32_t pointer_id = 0;
    POINTER_INPUT_TYPE type = PT_POINTER;

    if (!ClassifyPointer(wParam, pointer_id, type)) {
        return false; // Leave PT_MOUSE, PT_PEN, etc. to default Windows processing
    }

    const uint64_t now_ms = GetTickCount64();
    POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    ScreenToClient(hwnd, &pt);
    const float x = static_cast<float>(pt.x);
    const float y = static_cast<float>(pt.y);

    switch (msg) {
    case WM_POINTERDOWN: {
        inertia_.Stop();
        SetupScrollTarget(x, y);
        reducer_.OnDown(pointer_id, x, y, now_ms, state_.scale);
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
        const TouchAction action = reducer_.OnUp(pointer_id, x, y, now_ms, state_.scale);
        owned_touch_pointers_.erase(pointer_id);

        if (action.kind == TouchActionKind::Tap) {
            DispatchTap(hwnd, action.x, action.y);
        } else if (action.kind == TouchActionKind::DoubleTap) {
            DispatchDoubleTap(hwnd, action.x, action.y);
        } else if (action.kind == TouchActionKind::PanEnd) {
            inertia_.Start(action.velocity_x, action.velocity_y);
        }
        out_result = 0;
        return true;
    }

    case WM_POINTERCAPTURECHANGED: {
        owned_touch_pointers_.erase(pointer_id);
        reducer_.OnCancel(pointer_id);
        inertia_.Stop();
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
    owned_touch_pointers_.clear();
}

void TouchInputAdapter::OnTimerTick(HWND hwnd) {
    const uint64_t now_ms = GetTickCount64();

    // Check for hold threshold
    const TouchAction hold = reducer_.CheckHold(now_ms);
    if (hold.kind == TouchActionKind::Hold) {
        DispatchHold(hwnd, hold.x, hold.y);
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
}

void TouchInputAdapter::SetupScrollTarget(float x, float y) {
    scroll_target_ = {};
    const ui::WindowViewModel vm = BuildVm(state_);
    const D2D1_RECT_F rect = D2D1::RectF(0, 0,
        static_cast<float>(state_.compositor.Width()),
        static_cast<float>(state_.compositor.Height()));
    const ui::HitTestResult hit = state_.renderer.HitTest(vm, rect, x, y);

    if (hit.pane_index >= 0) {
        scroll_target_.kind = TouchScrollTargetKind::Pane;
        scroll_target_.pane = PaneAtSlot(state_, hit.pane_index);
        scroll_target_.tab = scroll_target_.pane ? scroll_target_.pane->ActiveTab() : nullptr;
        if (scroll_target_.tab) {
            scroll_target_.column_major = (scroll_target_.tab->view_mode == ui::ViewMode::List);
            const D2D1_RECT_F bounds = FocusedPaneRect(state_);
            const auto& pane_vm = (hit.pane_index >= 0 && static_cast<size_t>(hit.pane_index) < vm.panes.size())
                ? vm.panes[static_cast<size_t>(hit.pane_index)]
                : vm.pane;
            scroll_target_.max_scroll_x = state_.renderer.MaxScrollXForPane(pane_vm, bounds);
            scroll_target_.max_scroll_y = state_.renderer.MaxScrollForPane(pane_vm, bounds);
        }
    } else if (hit.region >= ui::HitTestResult::SidebarHeader && hit.region <= ui::HitTestResult::SidebarBlank) {
        scroll_target_.kind = TouchScrollTargetKind::Sidebar;
        scroll_target_.max_scroll_y = state_.renderer.SidebarMaxScroll(vm, rect.right, rect.bottom);
    } else if (hit.region >= ui::HitTestResult::SettingsNav && hit.region <= ui::HitTestResult::SettingsDensity) {
        scroll_target_.kind = TouchScrollTargetKind::Settings;
        scroll_target_.settings_page = state_.settings.page();
        scroll_target_.max_scroll_y = state_.renderer.SettingsMaxScroll(vm, rect.right, rect.bottom);
    }
}

void TouchInputAdapter::ApplyScrollDelta(float delta_x, float delta_y, HWND hwnd) {
    if (scroll_target_.kind == TouchScrollTargetKind::Pane && scroll_target_.tab) {
        if (scroll_target_.column_major) {
            scroll_target_.tab->scroll_x = std::clamp(
                scroll_target_.tab->scroll_x - delta_x, 0.0f, scroll_target_.max_scroll_x);
        } else {
            scroll_target_.tab->scroll_y = std::clamp(
                scroll_target_.tab->scroll_y - delta_y, 0.0f, scroll_target_.max_scroll_y);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (scroll_target_.kind == TouchScrollTargetKind::Sidebar) {
        state_.sidebarScroll = std::clamp(
            state_.sidebarScroll - delta_y, 0.0f, scroll_target_.max_scroll_y);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (scroll_target_.kind == TouchScrollTargetKind::Settings) {
        if (state_.settings.page() == scroll_target_.settings_page) {
            const float new_s = std::clamp(
                state_.settings.scroll() - delta_y, 0.0f, scroll_target_.max_scroll_y);
            state_.settings.SetScroll(new_s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }
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
            SwitchTab(state_, hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;

    case ui::HitTestResult::TabClose:
        if (hit.index >= 0) {
            CloseTab(state_, hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;

    case ui::HitTestResult::TabNew:
        NewTab(state_);
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
        ToggleSettings(state_);
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
        state_.settings.Navigate(hit.index);
        InvalidateRect(hwnd, nullptr, FALSE);
        break;

    case ui::HitTestResult::SettingsToggle:
        state_.settings.Toggle(hit.index);
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

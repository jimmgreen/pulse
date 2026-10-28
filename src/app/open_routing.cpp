// open_routing.cpp - Folders from outside Pulse, plan §6 (see open_routing.h).
#include "open_routing.h"

#include "app_internal.h"
#include "../common/localization.h"
#include "app_messages.h"
#include "closed_windows.h"
#include "default_file_manager.h"
#include "external_open.h"
#include "tray_reveal.h"
#include "window_factory.h"
#include "window_merge.h"
#include "window_registry.h"

#include <algorithm>
#include <vector>

namespace pulse {
namespace {

// Interrupting this window would cost the user something: a rename or an
// address being typed, a dialog it owns, or mouse capture anywhere on the UI
// thread (every Pulse window shares it): a tab, splitter or file drag (OLE
// captures too), an open menu, the move loop of a dragged-out tab.
bool IsBusy(const AppState& s) {
    if (s.renameIndex >= 0 || s.addressEditing) return true;
    if (s.hwnd && !IsWindowEnabled(s.hwnd)) return true;
    return GetCapture() != nullptr;
}

int LayoutTabIndex(const AppState& s, const void* tab) {
    if (!tab) return -1;
    for (size_t i = 0; i < s.window_tabs.items.size(); ++i)
        if (s.window_tabs.items[i].get() == tab) return static_cast<int>(i);
    return -1;
}

const void* ActiveLayoutTab(const AppState& s) {
    return s.window_tabs.active < s.window_tabs.items.size() ? s.window_tabs.items[s.window_tabs.active].get()
                                                             : nullptr;
}

app::ExternalOpenPolicy OpenPolicy(const AppState& s) {
    const int value = s.appPrefs.external_open;
    return value >= 0 && value <= 2 ? static_cast<app::ExternalOpenPolicy>(value) : app::ExternalOpenPolicy::Smart;
}

// Rule 7: says where the folder went and offers the other way (one click,
// WM_CONVERT_EXTERNAL_OPEN). A capsule in the window that shows it (M7i).
void ShowOpenToast(AppState& s, app::OpenConversion offer) {
    const app::LayoutTab* active = s.window_tabs.Active();
    const bool to_tab = offer == app::OpenConversion::ToTab;
    s.notification_toast.ShowSnack(
        s.hwnd, to_tab ? ui::SnackIcon::Window : ui::SnackIcon::Tab,
        l10n::Get(to_tab ? l10n::StringId::OpenedInNewWindow : l10n::StringId::OpenedInTab),
        active ? app::LayoutTabTitle(*active) : std::wstring(),
        {{l10n::Get(to_tab ? l10n::StringId::SnackToTab : l10n::StringId::SnackToWindow), WM_CONVERT_EXTERNAL_OPEN}});
}

void ForgetOpenToast(AppCore& core) {
    core.open_convert = app::OpenConversion::None;
    core.open_convert_window = nullptr;
    core.open_convert_tab = nullptr;
    core.open_convert_into = nullptr;
}

}  // namespace

AppState& OpenExternalFolder(AppState& mru, const std::wstring& raw) {
    // Win+E's shell command launches Pulse without a path. Each launch is a
    // new-window request, even when the current folder is already visible —
    // unless the wake-only setting turns it into activating the recent window.
    const bool new_window = raw.empty();
    const bool this_pc = new_window || app::IsThisPcArgument(raw);
    const auto* current = ActiveTab(mru);
    const std::wstring path = new_window ? (current ? current->current_path : std::wstring()) :
        this_pc ? std::wstring() : ResolveOpenFolderPath(raw);
    // Screenshot runs stay one window (plan §8.7); nothing to open stays as before.
    if (mru.shot.active || (!this_pc && path.empty())) {
        OpenFolderInNewTab(mru, raw);
        return mru;
    }
    AppCore& core = mru.core;
    const uint64_t now = GetTickCount64();
    // Read before any Pulse window comes forward: where the request came from.
    const HWND foreground = GetForegroundWindow();

    std::vector<AppState*> windows;
    std::vector<app::ExternalOpenWindow> snapshot;
    app::ExternalOpenBatch batch = core.open_batch;
    bool batch_tab_movable = false;
    app::ForEachWindowSharingCore(mru, [&](AppState& w) {
        if (!w.hwnd) return;
        app::ExternalOpenWindow e;
        e.shows_folder = !this_pc && ShowsFolder(w, path);
        e.minimized = IsIconic(w.hwnd) != FALSE;
        e.hidden = !IsWindowVisible(w.hwnd);
        e.busy = IsBusy(w);
        e.last_input_tick = w.last_input_tick;
        e.batch_window = core.open_batch_window && w.hwnd == core.open_batch_window;
        if (e.batch_window)
            batch_tab_movable = LayoutTabIndex(w, core.open_batch_tab) >= 0 && w.window_tabs.items.size() >= 2;
        windows.push_back(&w);
        snapshot.push_back(e);
    });
    if (windows.empty()) {
        OpenFolderInNewTab(mru, raw);
        return mru;
    }
    // The first tab can only move out while it is still there and not alone.
    batch.tab_movable = batch.tab_movable && batch_tab_movable;

    const app::ExternalOpenPolicy policy = OpenPolicy(mru);
    app::ExternalOpenRequest request;
    request.new_window = new_window;
    request.activate_only = new_window && mru.appPrefs.win_e_activate_existing;
    request.can_dedupe = !this_pc;
    request.tick = now;
    const app::ExternalOpenDecision decision = app::DecideOpenTarget(request, snapshot, batch, policy);

    AppState* target = nullptr;
    bool opened = false;
    HWND into = nullptr;  // where "make it a tab" sends the tabs back
    const auto open_window = [&](AppState& from, AppState* adopt_from, size_t adopt_index) -> AppState* {
        WindowInit init;
        init.path = path;
        init.adopt_from = adopt_from;
        init.adopt_index = adopt_index;
        init.monitor = foreground ? MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL) : nullptr;
        const HWND hwnd = OpenPulseWindow(from, init);
        return hwnd ? core.windows.Lookup(hwnd) : nullptr;
    };
    switch (decision.action) {
    case app::ExternalOpenAction::Activate:
    case app::ExternalOpenAction::Tab:
    case app::ExternalOpenAction::JoinBatch:
        // OpenFolderInNewTab restores a minimized or hidden window (a fresh
        // start from the tray included) and brings an open folder forward.
        target = windows[decision.window];
        OpenFolderInNewTab(*target, raw);
        opened = decision.action != app::ExternalOpenAction::Activate;
        break;
    case app::ExternalOpenAction::NewWindow:
        if ((target = open_window(mru, nullptr, 0)) != nullptr) {
            opened = true;
            into = windows.front()->hwnd;
            if (!this_pc) SelectLaunchedFile(*target, raw);
            RevealWindow(*target);
        }
        break;
    case app::ExternalOpenAction::PromoteBatch: {
        AppState& from = *windows[decision.window];
        const int index = LayoutTabIndex(from, core.open_batch_tab);
        // The new window takes the batch's first tab alive (as Ctrl+Shift+M
        // does), then this folder opens next to it.
        if (index >= 0 && (target = open_window(from, &from, static_cast<size_t>(index))) != nullptr) {
            opened = true;
            into = from.hwnd;
            OpenFolderInNewTab(*target, raw);
        }
        break;
    }
    }
    if (!target) {
        // A window could not be created: a tab in the window that has it.
        target = &mru;
        OpenFolderInNewTab(mru, raw);
    }

    const bool tab_movable = decision.action == app::ExternalOpenAction::Tab && target->window_tabs.items.size() >= 2;
    // Whether the folder sits in a window opened for it or for its batch.
    const bool in_new_window = decision.action == app::ExternalOpenAction::NewWindow ||
                               decision.action == app::ExternalOpenAction::PromoteBatch ||
                               (decision.action == app::ExternalOpenAction::JoinBatch && batch.window_is_new);
    if (decision.action == app::ExternalOpenAction::JoinBatch && target->hwnd == core.open_convert_window)
        into = core.open_convert_into;  // the batch's toast already knew where it came from
    core.open_batch = app::NextOpenBatch(batch, request, decision, opened, tab_movable);
    if (opened) {
        core.open_batch_window = target->hwnd;
        core.open_batch_tab = decision.action == app::ExternalOpenAction::Tab ? ActiveLayoutTab(*target) : nullptr;
    } else if (core.open_batch.count == 0) {
        core.open_batch_window = nullptr;
        core.open_batch_tab = nullptr;
    }
    const app::OpenConversion offer =
        opened ? app::OfferedConversion(decision, policy, in_new_window, target->window_tabs.items.size() >= 2)
               : app::OpenConversion::None;
    // "Make it a tab" needs a window to go back to.
    if (offer == app::OpenConversion::None || (offer == app::OpenConversion::ToTab && !into)) {
        if (opened) ForgetOpenToast(core);
        return *target;
    }
    core.open_convert = offer;
    core.open_convert_window = target->hwnd;
    core.open_convert_tab = ActiveLayoutTab(*target);
    core.open_convert_into = into;
    ShowOpenToast(*target, offer);
    return *target;
}

void ConvertExternalOpen(AppState& s) {
    AppCore& core = s.core;
    const app::OpenConversion offer = core.open_convert;
    if (offer == app::OpenConversion::None || core.open_convert_window != s.hwnd) return;  // an older toast
    const void* const tab = core.open_convert_tab;
    const HWND into_hwnd = core.open_convert_into;
    ForgetOpenToast(core);
    core.open_batch = {};  // its window or its tab is about to change
    core.open_batch_window = nullptr;
    core.open_batch_tab = nullptr;

    AppState* shown = nullptr;
    if (offer == app::OpenConversion::ToTab) {
        // Every tab of the window opened for the folder (its batch, or tabs
        // added since) goes back after the active tab of the window it would
        // otherwise have opened in; this window closes.
        AppState* into = into_hwnd && into_hwnd != s.hwnd ? core.windows.Lookup(into_hwnd) : nullptr;
        if (!into) into = core.windows.MostRecentExcept(s.hwnd);
        if (!into || s.window_tabs.items.empty()) return;
        const size_t insert_at = into->window_tabs.active + 1;
        const HWND hwnd = s.hwnd;
        ShowWindow(hwnd, SW_HIDE);  // it must not paint while its tabs leave
        (void)MergeTabsInto(s, *into, (std::min)(insert_at, into->window_tabs.items.size()));
        if (!s.window_tabs.items.empty()) {
            ShowWindow(hwnd, SW_SHOWNORMAL);
            return;
        }
        DestroyWindow(hwnd);  // `s` is gone from here on
        shown = into;
        RevealWindow(*shown);
    } else {
        const int index = LayoutTabIndex(s, tab);
        if (index < 0 || s.window_tabs.items.size() < 2) return;
        WindowInit init;
        if (const app::Tab* view = s.window_tabs.items[static_cast<size_t>(index)]->ActiveFolder())
            init.path = view->current_path;
        init.adopt_from = &s;
        init.adopt_index = static_cast<size_t>(index);
        const HWND hwnd = OpenPulseWindow(s, init);
        if (!hwnd || !(shown = core.windows.Lookup(hwnd))) return;
        SetForegroundWindow(hwnd);
    }
    // Rule 7: the same way 3 times in a row -> ask once whether that should
    // be the default (WM_ADOPT_OPEN_POLICY). Never changed unasked.
    if (!app::NoteOpenConversion(core.open_convert_streak, offer)) return;
    core.open_suggested = app::SuggestedOpenPolicy(offer);
    shown->notification_toast.ShowSnack(
        shown->hwnd, ui::SnackIcon::Suggest,
        l10n::Get(offer == app::OpenConversion::ToTab ? l10n::StringId::OpenSuggestTab : l10n::StringId::OpenSuggestWindow),
        l10n::Get(l10n::StringId::SnackStreak),
        {{l10n::Get(l10n::StringId::SnackSetDefault), WM_ADOPT_OPEN_POLICY}, {l10n::Get(l10n::StringId::SnackNotNow), 0}},
        {}, 9000);
}

void AdoptSuggestedOpenPolicy(AppState& s) {
    const app::ExternalOpenPolicy policy = s.core.open_suggested;
    if (policy == app::ExternalOpenPolicy::Smart) return;
    s.core.open_suggested = app::ExternalOpenPolicy::Smart;
    s.settings.ExternalOpen(static_cast<int>(policy));  // saves, as the settings page does
    app::ForEachWindowSharingCore(s, [](AppState& w) { InvalidateRect(w.hwnd, nullptr, FALSE); });
}

void NoteUserInput(AppState& any, const MSG& msg) {
    switch (msg.message) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK:
    case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        break;
    default:
        return;
    }
    if (!msg.hwnd) return;
    if (AppState* w = any.core.windows.Lookup(GetAncestor(msg.hwnd, GA_ROOT))) {
        const uint64_t now = GetTickCount64();
        w->previous_use_tick = app::PreviousUseTick(w->previous_use_tick, w->last_input_tick, now);
        w->last_input_tick = now;
    }
}

}  // namespace pulse

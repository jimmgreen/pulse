#include "../common/runtime_log.h"
#include "../common/picker_places_snapshot.h"
#include "global_search_controller.h"
#include "global_search_handoff.h"
#include "shortcut_page.h"
#include "app_internal.h"
#include "tray_reveal.h"
#include "app_runtime.h"
#include "app_hosted_edit.h"
#include "app_ops_ui.h"
#include "app_commands.h"
#include "app_catalog.h"
#include "places.h"
#include "session.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include "../ui/confirm_dialog.h"
#include "../index/index_config.h"
#include "../ui/ui_renderer.h"
#include <algorithm>
#include <thread>

namespace pulse {
namespace {
// Test instances leave the hotkey to the installed Pulse. Selftest builds can
// opt in (PULSE_TEST_GLOBAL_HOTKEY) to drive the popup with the hotkey stored
// in their own isolated data dir.
bool HotkeyAllowed(const AppState& s) {
    if (!s.isolatedTest) return true;
#ifdef PULSE_WITH_TEST_HOOKS
    if (GetEnvironmentVariableW(L"PULSE_TEST_DOUBLE_CTRL_SETTINGS", nullptr, 0) &&
        GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0)) return true;
#endif
#ifdef PULSE_WITH_SELFTEST
    return GetEnvironmentVariableW(L"PULSE_TEST_GLOBAL_HOTKEY", nullptr, 0) > 0;
#else
    return false;
#endif
}

// 跳转 candidates: this window's folders first, then other windows, Quick
// access and recently opened folders; each folder once. `open` marks folders
// shown in a tab.
std::vector<app::DialogJumpFolder> KnownFolders(const AppState& s) {
    std::vector<app::DialogJumpFolder> folders;
    auto add = [&](const std::wstring& path, const std::wstring& label, bool open, bool pinned = false) {
        if (path.empty() || fs::IsVirtualPath(path) || folders.size() >= 96) return;
        for (auto& known : folders)
            if (path::EqualInsensitive(known.path, path)) {
                if (known.label.empty()) known.label = label;
                known.open = known.open || open;
                known.pinned = known.pinned || pinned;
                return;
            }
        folders.push_back({path, label, open, pinned});
    };
    if (const auto* pane = s.pane) add(pane->view.current_path, {}, true);
    auto add_window = [&](AppState& window) {
        ForEachPane(window, [&](app::Pane& pane) { add(pane.view.current_path, {}, true); });
    };
    add_window(const_cast<AppState&>(s));
    s.core.windows.ForEach([&](HWND, AppState& window) { if (&window != &s) add_window(window); });
    // Quick access rows carry the shown names ("下载" for Downloads).
    for (const auto& entry : s.sidebar.quick_access) add(entry.path, entry.label, false, true);
    for (const auto& path : s.places.quick_access_paths) add(path, {}, false, true);
    for (const auto& item : s.places.RecentItems())
        if (item.kind == app::PlaceItemKind::Folder) add(item.path, {}, false);
    return folders;
}

// What the popup shows with an empty query: the first Quick access pins as
// scopes, recent searches as chips and recently opened items as rows.
GlobalSearchContext MakeGlobalSearchContext(const AppState& s) {
    GlobalSearchContext context;
    for (const auto& path : s.places.quick_access_paths) {
        if (context.pinned_folders.size() >= 3) break;
        if (!path.empty() && !fs::IsVirtualPath(path)) context.pinned_folders.push_back(path);
    }
    for (const auto& entry : s.searchHistory.entries) {
        if (context.recent_queries.size() >= 6) break;
        if (entry.query.empty()) continue;
        if (std::find(context.recent_queries.begin(), context.recent_queries.end(), entry.query) != context.recent_queries.end()) continue;
        context.recent_queries.push_back(entry.query);
    }
    for (const auto& item : s.places.RecentItems()) {
        if (context.recent_files.size() >= 6) break;
        if (item.path.empty() || fs::IsVirtualPath(item.path)) continue;
        context.recent_files.push_back({item.path, item.kind == app::PlaceItemKind::Folder});
    }
    // Frequent places: the most visited first among equal matches, and the
    // ones not known otherwise as extra candidates.
    auto folders = KnownFolders(s);
    const auto& history = *s.core.jumpHistory;
    for (const auto& path : history.Top({}, 32)) {
        if (fs::IsVirtualPath(path) || app::IsAppPath(path)) continue;
        if (std::none_of(folders.begin(), folders.end(), [&](const auto& f) { return path::EqualInsensitive(f.path, path); }))
            folders.push_back({path, {}, false, false});
    }
    std::vector<std::pair<double, size_t>> order;
    for (size_t i = 0; i < folders.size(); ++i) order.push_back({history.Score(folders[i].path, {}), i});
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [score, i] : order) context.jump_folders.push_back({std::move(folders[i].path), std::move(folders[i].label)});
    context.show_apps = s.appPrefs.global_search_apps;
    context.history = s.core.jumpHistory;
    context.width = s.appPrefs.global_search_width;
    context.height = s.appPrefs.global_search_height;
    context.positioned = s.appPrefs.global_search_positioned;
    context.x = s.appPrefs.global_search_x;
    context.y = s.appPrefs.global_search_y;
    return context;
}

} // namespace

// Events land on whichever window is most recent: the popup outlives the
// window that opened it.
GlobalSearchEvents MakeGlobalSearchEvents(AppState& s) {
    GlobalSearchEvents events;
    auto* core = &s.core;
    events.toggle_theme = [core] {
        if (AppState* window = core->windows.MostRecent()) {
            ToggleTheme(*window);
            window->globalSearchWindow.SetAppearance(window->darkMode,
                ui::WindowEffectFromId(window->appPrefs.window_effect),
                window->appPrefs.background_image, window->accentColor);
        }
    };
    events.clear_search_history = [core] {
        if (core->searchHistory.Clear() && core->searchHistory.persist)
            core->searchHistoryWriter.Submit(core->searchHistory);
    };
    events.stage_paths = [core](const std::vector<std::wstring>& paths) {
        if (AppState* window = core->windows.MostRecent()) StagePaths(*window, paths, std::nullopt);
    };
    events.staged_paths = [core] {
        std::vector<std::wstring> paths;
        for (const auto& batch : core->tray.batches())
            for (const auto& item : batch.items) paths.push_back(item.path);
        return paths;
    };
    events.unstage_path = [core](const std::wstring& path) {
        if (AppState* window = core->windows.MostRecent()) UnstagePath(*window, path);
    };
    events.clear_staging = [core] {
        if (AppState* window = core->windows.MostRecent()) ClearTrayWithUndo(*window);
    };
    events.open_path = [core](const std::wstring& path, bool directory, bool location) {
        AppState* window = core->windows.MostRecent();
        if (!window || (!directory && !location)) return false;
        const auto& prefs = window->appPrefs;
        if (!prefs.open_folders_in_pulse && !(prefs.integration_enabled && prefs.integration_folders)) return false;
        // Use the normal external-open queue so busy windows defer the request
        // and files are selected in their containing folder without Explorer.
        using Coordinator = app::SingleInstanceCoordinator;
        Coordinator::OpenRequest request;
        GUID id{};
        auto submitted = Coordinator::OpenSubmission::Invalid;
        if (SUCCEEDED(CoCreateGuid(&id))) {
            CopyMemory(request.id.data(), &id, sizeof(id));
            const auto now = GetTickCount64();
            request.deadline = now + 10000;
            request.path = path;
            request.reveal = location;
            submitted = window->single_instance.SubmitOpenRequest(std::move(request), now,
                core->host.Hwnd(), WM_OPEN_EXTERNAL);
        }
        if (submitted != Coordinator::OpenSubmission::Accepted && submitted != Coordinator::OpenSubmission::Duplicate) {
            RevealWindow(*window);
            auto detail = l10n::Get(l10n::StringId::ExternalOpenQueueFailedDetail);
            if (const auto token = detail.find(L"{n}"); token != std::wstring::npos) detail.replace(token, 3, L"1");
            window->notification_toast.ShowError(window->hwnd,
                l10n::Get(l10n::StringId::ExternalOpenQueueFailedTitle),
                detail);
        }
        return true;
    };
    events.placement_changed = [core](int x, int y, int width, int height) {
        AppState* window = core->windows.MostRecent();
        if (!window) return;
        window->appPrefs.global_search_width = std::clamp(width, 0, 4000);
        window->appPrefs.global_search_height = std::clamp(height, 0, 4000);
        window->appPrefs.global_search_positioned = true;
        window->appPrefs.global_search_x = x;
        window->appPrefs.global_search_y = y;
        window->appPrefs.Save();
    };
    events.pin_folder = [core](const std::wstring& path) {
        AppState* window = core->windows.MostRecent();
        if (!window) return;
        window->places.SetQuickAccessPinned({path}, true);
        if (window->hwnd) InvalidateRect(window->hwnd, nullptr, FALSE);
    };
    events.opened = [core](const GlobalSearchHandoff& search, const std::wstring& path, bool directory) {
        AppState* window = core->windows.MostRecent();
        if (!window) return;
        // Programs started from the 应用 group are not files: they only
        // count for the ranking.
        const bool program = app::IsAppPath(path);
        if (!program) RecordRecentOpen(*window, path, directory ? app::PlaceItemKind::Folder : app::PlaceItemKind::File);
        if (directory || program) core->jumpHistory->Record(path, {});
        if (const auto history = GlobalSearchHandoffPath(search); !history.empty()) RecordSearchHistory(*window, history);
    };
    return events;
}

namespace {
// Appearance, context and callbacks: before every summon and the pre-warm.
void PrepareGlobalSearchWindow(AppState& s) {
    s.globalSearchWindow.SetAppearance(s.darkMode, ui::WindowEffectFromId(s.appPrefs.window_effect),
        s.appPrefs.background_image, s.accentColor);
    // The search window is process-wide and outlives the window that opened
    // it (M4b): continue in the most recent window when the handoff happens.
    s.globalSearchWindow.SetHandoffHandler([core = &s.core](const GlobalSearchHandoff& request) {
        if (AppState* window = core->windows.MostRecent()) ContinueGlobalSearchInMain(*window, request);
    });
    s.globalSearchWindow.SetContext(MakeGlobalSearchContext(s));
    s.globalSearchWindow.SetEvents(MakeGlobalSearchEvents(s));
}
}

void ApplyGlobalSearchSettings(AppState& s) {
    ApplyKeymapPrefs(s);  // the shortcut conflict checks see the hotkey
    // Kept in memory only when prefs are not saved.
    if (!s.core.jumpHistory->IsOpen())
        s.core.jumpHistory->Open(s.appPrefs.persist ? app::GetPulseDataDir() + L"\\jump_history.tsv" : std::wstring{});
    s.core.jumpHistory->SetEnabled(s.appPrefs.jump_history);
    s.core.dialogJump.SetHistory(s.core.jumpHistory);
    if (!HotkeyAllowed(s)) return;
    s.core.dialogJump.Update(ServiceWindow(s), WM_DIALOG_JUMP_CONTEXT, s.appPrefs.dialog_jump_bar);
    SendDialogJumpContext(s);
    const bool enabled = s.appPrefs.global_search_enabled && s.appPrefs.search_enabled;
    const bool ok = s.globalSearchHotkey.Update(ServiceWindow(s), enabled,
        s.appPrefs.global_search_modifiers, s.appPrefs.global_search_key);
    s.settings.SetGlobalSearchError(ok ? std::wstring{} : l10n::Get(
        s.globalSearchHotkey.Error() == ERROR_HOTKEY_ALREADY_REGISTERED
            ? l10n::StringId::GlobalSearchConflict : l10n::StringId::GlobalSearchRegisterFailed));
    const bool double_ctrl = enabled && s.appPrefs.global_search_double_ctrl;
    s.core.doubleCtrl.Update(ServiceWindow(s), WM_DOUBLE_CTRL, double_ctrl,
        static_cast<uint32_t>(s.appPrefs.global_search_double_ctrl_ms));
    if (!enabled || (!ok && !double_ctrl)) s.globalSearchWindow.Hide();
    else s.globalSearchWindow.Prewarm(3000, [core = &s.core] {
        if (AppState* window = core->windows.MostRecent()) PrepareGlobalSearchWindow(*window);
    });
    s.tray_controller.SetVisible(WantsTrayIcon(s, s.hidden_to_tray));
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ApplySearchSwitch(AppState& s, bool purge_index) {
    const bool on = s.appPrefs.search_enabled;
    s.renderer.SetSearchHidden(!on);
    if (!on) {
        if (s.addressSearching || IsAddressSearchResults(ActiveTab(s))) ExitAddressSearch(s);
        s.globalSearchWindow.Hide();
        // Stop the clients first so none of them starts the service again.
        s.index.Stop();
        s.contentSearch.Stop();
        s.networkIndex.Stop();
        s.folderSizes.SetIndexEnabled(false);
    } else if (!s.isolatedTest && !s.shot.active) {
        const HWND service_window = ServiceWindow(s);
        s.index.Start(service_window, WM_INDEX_NOTIFY, WM_INDEX_SEARCH);
        s.networkIndex.Start(service_window, WM_NETWORK_INDEX_NOTIFY, WM_NETWORK_INDEX_SEARCH);
        s.contentSearch.Start(service_window, WM_CONTENT_SEARCH,
            s.contentIndexObserver ? index::ContentAgentMode::Observer : index::ContentAgentMode::Instant);
        s.folderSizes.SetIndexEnabled(true);
    }
    if (!s.isolatedTest && !s.shot.active) {
        const bool installed = s.index.ServiceInstalled();
        // The service records the switch itself (no elevation); this can wait a
        // few seconds for a starting service, so it runs off the UI thread.
        const std::wstring user_index = purge_index ? index::UserIndexRoot() : std::wstring();
        std::thread([on, purge_index, installed, user_index] {
            if (installed) index::IndexClient::SetServiceSearch(on, purge_index);
            if (!on && purge_index) index::IndexClient::PurgeIndexFolder(user_index);
        }).detach();
    }
    ApplyGlobalSearchSettings(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ToggleSearchSwitch(AppState& s) {
    bool purge = false;
    if (s.appPrefs.search_enabled) {
        ui::ConfirmDialogSpec spec;
        spec.title = l10n::Get(l10n::StringId::SearchSwitchOffTitle);
        spec.message = l10n::Get(l10n::StringId::SearchSwitchOffMessage);
        spec.confirm_text = l10n::Get(l10n::StringId::SearchSwitchOffPurge);
        spec.secondary_text = l10n::Get(l10n::StringId::SearchSwitchOffKeep);
        spec.cancel_text = l10n::Get(l10n::StringId::Cancel);
        spec.glyph = L"\xE721";
        spec.default_choice = ui::ConfirmChoice::Secondary;
        const auto choice = ui::ShowConfirmDialogEx(s.hwnd, spec, s.darkMode, s.accentColor);
        if (choice == ui::ConfirmChoice::Cancel) return;
        purge = choice == ui::ConfirmChoice::Confirm;
    }
    s.appPrefs.search_enabled = !s.appPrefs.search_enabled;
    if (!s.isolatedTest && !s.appPrefs.Save()) {
        s.appPrefs.search_enabled = !s.appPrefs.search_enabled;
        return;
    }
    ApplySearchSwitch(s, purge);
}

void PublishPickerPlaces(AppState& s, bool force) {
    if (s.isolatedTest) return;
    static ULONGLONG last = 0;
    const auto now = GetTickCount64();
    if (!force && now - last < 2000) return;
    last = now;
    AppState* window = s.core.windows.MostRecent();
    if (!window) window = &s;
    picker_places::Snapshot snapshot;
    snapshot.endpoint = s.core.host.Hwnd();
    for (const auto& w : window->places.workspaces)
        snapshot.workspaces.push_back({w.root, w.name, w.color_rgb});
    for (const auto& item : window->places.starred_items)
        snapshot.starred_items.push_back({item.path, static_cast<uint32_t>(item.kind), item.group});
    for (const auto& group : window->places.starred_groups)
        snapshot.starred_groups.push_back({group.id, group.name});
    if (const auto* tab = ActiveTab(*window); tab && !fs::IsVirtualPath(tab->current_path)) snapshot.current = tab->current_path;
    for (const auto& folder : KnownFolders(*window)) snapshot.folders.push_back({folder.path, folder.label, folder.open, folder.pinned});
    picker_places::Publish(snapshot);
}

void SendDialogJumpContext(AppState& s) {
    AppState* window = s.core.windows.MostRecent();
    if (!window) window = &s;
    app::DialogJumpContext context;
    context.folders = KnownFolders(*window);
    if (const auto* tab = ActiveTab(*window); tab && !fs::IsVirtualPath(tab->current_path))
        context.current = tab->current_path;
    context.dark = window->darkMode;
    context.accent = window->accentColor;
    context.pinyin = window->appPrefs.search_pinyin;
    context.index = window->appPrefs.search_enabled;
    context.remember = window->appPrefs.jump_history;
    context.auto_jump = window->appPrefs.dialog_jump_auto;
    s.core.dialogJump.SetContext(std::move(context));
}

bool HandleGlobalSearchHotkeyCapture(AppState& s, unsigned key) {
    if (!s.settings.global_search_hotkey_capturing()) return false;
    if (!IsSettingsTab(ActiveTab(s))) {
        s.settings.CancelGlobalSearchHotkeyCapture();
        return false;
    }
    UINT modifiers = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) modifiers |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) modifiers |= MOD_WIN;
    const bool consumed = s.settings.CaptureGlobalSearchHotkey(key, modifiers);
    if (consumed) InvalidateRect(s.hwnd, nullptr, FALSE);
    return consumed;
}

void ToggleGlobalSearch(AppState& s, bool double_ctrl) {
    diagnostics::runtime::Event("search_summon", {{"double_ctrl", double_ctrl},
        {"visible", s.globalSearchWindow.Visible()}});
    if (!s.appPrefs.global_search_enabled || !s.appPrefs.search_enabled) return;
    // Queued input may arrive after the setting was disabled.
    if (double_ctrl && !s.appPrefs.global_search_double_ctrl) return;
    if (double_ctrl ? !s.core.doubleCtrl.Running() : !s.globalSearchHotkey.Registered()) return;
    // Two Ctrl taps over an Open / Save dialog go to its jump bar.
    if (double_ctrl && s.core.dialogJump.FocusSearch()) return;
    if (s.globalSearchWindow.Visible()) {
        s.globalSearchWindow.Hide();
        return;
    }
    std::wstring folder;
    if (const auto* tab = ActiveTab(s); tab && !fs::IsVirtualPath(tab->current_path)) folder = tab->current_path;
    PrepareGlobalSearchWindow(s);
    if (!s.globalSearchWindow.Show(s.hwnd, s.darkMode, s.scale, folder, s.appPrefs.search_pinyin)) {
        RevealWindow(s);
        s.notification_toast.ShowError(s.hwnd, l10n::Get(l10n::StringId::GlobalSearch),
            l10n::Get(l10n::StringId::GlobalSearchOpenFailed));
    }
}

void ShutdownGlobalSearch(AppState& s) {
    s.globalSearchHotkey.Reset();
    s.core.doubleCtrl.Stop();
    s.core.dialogJump.Stop();
    s.globalSearchWindow.Shutdown();
}
}

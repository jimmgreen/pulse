#include "../app/settings_controller.h"
#include "../app/session.h"
#include "../app/context_menu_controller.h"
#include "../app/single_instance_coordinator.h"
#include "../app/tray_controller.h"
#include "../index/index_client.h"
#include "../index/network_agent_client.h"

#include <cstdio>
#include <condition_variable>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>

namespace pulse::app {

std::wstring GetPulseDataDir() {
    return {};
}
struct SettingsControllerTestPeer {
    static bool Start(SettingsController& controller, SettingsTask task,
                      SettingsTaskOperation operation, SettingsTaskCompletion completion) {
        return controller.StartTask(std::move(task), std::move(operation),
                                    std::move(completion));
    }
};

} // namespace pulse::app

namespace {

bool Report(const char* name, bool passed) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
    return passed;
}

} // namespace

int wmain() {
    using pulse::app::HasEffect;
    using pulse::app::SettingsController;
    using pulse::app::SettingsEffect;
    using pulse::app::ContextMenuController;
    using pulse::app::SingleInstanceCoordinator;
    using pulse::app::TrayController;

    bool passed = true;
    const std::wstring mutex_name = L"Local\\Pulse.ControllerTest." +
        std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    SingleInstanceCoordinator primary;
    SingleInstanceCoordinator duplicate;
    passed &= Report("single-instance coordinator owns the primary mutex",
        primary.Acquire(mutex_name) == SingleInstanceCoordinator::AcquireResult::Primary);
    passed &= Report("single-instance coordinator detects a duplicate",
        duplicate.Acquire(mutex_name) == SingleInstanceCoordinator::AcquireResult::Existing);
    primary.Release();
    passed &= Report("single-instance mutex is released by its owner",
        duplicate.Acquire(mutex_name) == SingleInstanceCoordinator::AcquireResult::Primary);

    std::wstring decoded;
    std::wstring path = L"C:\\目录\\file.txt";
    COPYDATASTRUCT data{};
    data.dwData = SingleInstanceCoordinator::OpenPathMessageId();
    data.cbData = static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t));
    data.lpData = path.data();
    passed &= Report("single-instance IPC decodes a terminated UTF-16 path",
        SingleInstanceCoordinator::DecodeOpenPath(&data, decoded) && decoded == path);
    data.cbData -= sizeof(wchar_t);
    passed &= Report("single-instance IPC rejects a non-terminated path",
        !SingleInstanceCoordinator::DecodeOpenPath(&data, decoded));
    data.cbData = 3;
    passed &= Report("single-instance IPC rejects odd byte counts",
        !SingleInstanceCoordinator::DecodeOpenPath(&data, decoded));
    wchar_t embedded[] = {L'C', L':', L'\0', L'x', L'\0'};
    data.cbData = sizeof(embedded);
    data.lpData = embedded;
    passed &= Report("single-instance IPC rejects embedded NUL characters",
        !SingleInstanceCoordinator::DecodeOpenPath(&data, decoded));

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
        L"Pulse tray controller test", WS_POPUP, -32000, -32000, 100, 100,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    TrayController tray;
    tray.Attach(hwnd, GetModuleHandleW(nullptr));
    tray.RestoreWindow();
    passed &= Report("tray controller owns window restore behavior",
        hwnd && IsWindowVisible(hwnd));
    ShowWindow(hwnd, SW_HIDE);
    tray.Detach();
    if (hwnd) DestroyWindow(hwnd);

    pulse::app::AppPrefs prefs;
    pulse::app::ContextMenuPrefs context;
    prefs.persist = false;
    context.persist = false;
    SettingsController settings;
    settings.SelectPage(2);
    settings.ScrollBy(-120.0f, 1.0f, 100.0f);
    passed &= Report("settings controller owns page and clamped scroll state",
        settings.page() == 2 && settings.scroll() == 48.0f);
    passed &= Report("settings controller maps stable page routes",
        SettingsController::PageFromName(L"index") == 1 &&
        std::wstring(SettingsController::PageName(2)) == L"context" &&
        SettingsController::PageFromName(L"about") == 3 &&
        std::wstring(SettingsController::PageName(3)) == L"about");

    pulse::app::SettingsTaskResult completed_task;
    completed_task.task.kind = pulse::app::SettingsTaskKind::InstallService;
    completed_task.ok = true;
    auto task_effect = settings.CompleteTask(completed_task, true);
    passed &= Report("settings completion owns service refresh state",
        task_effect.refresh_index && settings.service_installed());
    completed_task.task.kind = pulse::app::SettingsTaskKind::NetworkAdd;
    completed_task.task.path = L"\\\\server\\share";
    completed_task.task.pin = true;
    task_effect = settings.CompleteTask(completed_task, true);
    passed &= Report("settings completion returns network pin side effect",
        !task_effect.refresh_index && task_effect.pin_network == completed_task.task.path);
    completed_task.task.kind = pulse::app::SettingsTaskKind::DiagnosticsExport;
    completed_task.task.path = L"C:\\diagnostics-export";
    task_effect = settings.CompleteTask(completed_task, true);
    passed &= Report("diagnostics completion opens export without index refresh",
        !task_effect.refresh_index && task_effect.open_path == completed_task.task.path);
    completed_task.task.kind = pulse::app::SettingsTaskKind::NetworkRemove;
    completed_task.ok = false;
    task_effect = settings.CompleteTask(completed_task, true);
    passed &= Report("settings completion owns operation error text",
        !task_effect.refresh_index && settings.error() == L"无法移除服务器文件夹。");

    pulse::index::IndexClient index_client;
    pulse::index::NetworkAgentClient network_client;
    SettingsController& settings_ui = settings;
    int applied_effects = 0;
    SettingsEffect last_effect = SettingsEffect::None;
    bool picked_image = false;
    SettingsController::UiCallbacks ui_callbacks;
    ui_callbacks.pick_image = [&](std::wstring& selected) {
        picked_image = true;
        selected = L"C:\\wallpaper.png";
        return true;
    };
    ui_callbacks.apply_effects = [&](SettingsEffect effect) {
        last_effect = effect;
        if (effect != SettingsEffect::None) ++applied_effects;
    };
    settings_ui.BindUi(prefs, context, index_client, network_client,
                       std::move(ui_callbacks));
    settings_ui.WindowEffect(L"mica");
    passed &= Report("settings window effect returns material invalidation",
        prefs.window_effect == L"mica" &&
        HasEffect(last_effect, SettingsEffect::WindowMaterial));
    settings_ui.AccentChoice(false, 0x12AB34);
    passed &= Report("settings accent is normalized and reports its side effect",
        prefs.accent_rgb == L"12AB34" && HasEffect(last_effect, SettingsEffect::Accent));
    settings_ui.RowHeight(2);
    passed &= Report("settings density owns preference mutation",
        prefs.row_height == 40 && HasEffect(last_effect, SettingsEffect::RowHeight));
    settings_ui.TrayIconSize(0);
    passed &= Report("settings tray size owns preference mutation",
        prefs.tray_icon_size == 40 && HasEffect(last_effect, SettingsEffect::TrayDeckIcon));
    settings_ui.Language(L"en-US");
    passed &= Report("settings language uses a stable identifier",
        prefs.language == L"en-US" && HasEffect(last_effect, SettingsEffect::Language));
    settings_ui.ToggleUi(2);
    passed &= Report("settings close behavior requests tray synchronization",
        prefs.keep_running_on_close && HasEffect(last_effect, SettingsEffect::TrayVisibility));
    settings_ui.WindowEffect(L"mica-alt");
    settings_ui.RowHeight(0);
    settings_ui.AccentChoice(false, 0x2468AC);
    passed &= Report("settings UI controller routes preference commands",
        prefs.window_effect == L"mica-alt" && prefs.row_height == 28 &&
        prefs.accent_rgb == L"2468AC" && applied_effects == 9);
    settings_ui.Wallpaper(0);
    passed &= Report("settings UI controller owns image selection flow",
        picked_image);
    settings_ui.ResetUi();
    passed &= Report("settings UI controller has explicit binding lifecycle",
        !settings_ui.ui_bound());

    pulse::app::LayoutTabSnapshot saved;
    saved.pinned = true;
    saved.group = 4;
    saved.layout = 1;
    saved.focused = 1;
    saved.panes.push_back({ L"C:\\first", pulse::ui::ViewMode::Tiles, { 0.2f, 0.5f, 0.8f }, {} });
    saved.panes.push_back({ L"D:\\loose", pulse::ui::ViewMode::Details, {}, {} });
    saved.panes.push_back({ L"", pulse::ui::ViewMode::Details, {}, {} });
    pulse::app::LayoutTab restored;
    std::vector<std::wstring> loaded_paths;
    pulse::app::RestoreLayoutTab(restored, saved,
        [&](pulse::app::Tab& tab, const std::wstring& loaded) {
            tab.current_path = loaded;
            loaded_paths.push_back(loaded);
        });
    passed &= Report("session layout restore skips empty panes and grows to the preset",
        restored.pinned && restored.tab_group == 0 &&
        restored.layout == pulse::app::LayoutPreset::TwoVertical &&
        restored.panes.size() == 2 && restored.focused_index == 1 &&
        loaded_paths.size() >= 2 && loaded_paths[0] == L"C:\\first");
    const auto captured = pulse::app::CaptureLayoutTab(restored);
    passed &= Report("session layout capture preserves folder presentation state",
        captured.pinned && captured.layout == 1 && captured.panes.size() == 2 &&
        captured.panes[0].view == pulse::ui::ViewMode::Tiles &&
        captured.panes[0].columns[1] == 0.5f);
    pulse::app::LayoutTab empty_restored;
    pulse::app::RestoreLayoutTab(empty_restored, {},
        [](pulse::app::Tab& tab, const std::wstring& loaded) {
            tab.current_path = loaded;
        });
    passed &= Report("session layout restore supplies a usable fallback folder",
        empty_restored.panes.size() == 1 &&
        empty_restored.panes[0]->view.current_path == L"C:\\");
    pulse::app::WindowTabs restored_tabs;
    std::vector<pulse::app::GroupSessionSnapshot> groups{
        { 4, L"work", 0x00112233, false },
        { 4, L"duplicate", 0x00445566, true },
        { -1, L"invalid", 0, false },
    };
    saved.pinned = false;
    saved.group = 4;
    pulse::app::RestoreWindowTabs(restored_tabs, { saved }, groups, 0,
        [&](pulse::app::Tab& tab, const std::wstring& loaded) {
            tab.current_path = loaded;
        });
    passed &= Report("session window tabs restore validates groups",
        restored_tabs.tab_groups.size() == 1 &&
        restored_tabs.next_tab_group_id == 5 && restored_tabs.items.size() == 1 &&
        restored_tabs.items[0]->tab_group == 4);

    ContextMenuController context_menu;
    uint32_t queried_token = 0;
    uint32_t invoked_command = 0;
    std::wstring invoked_verb;
    std::wstring invoked_text;
    int closed_sessions = 0;
    int folder_refreshes = 0;
    context_menu.SetShellOperations({
        [&](std::vector<std::wstring> paths, HWND, bool background, bool extended,
            std::vector<std::wstring>) {
            queried_token = paths == std::vector<std::wstring>{L"C:\\one.txt"} &&
                !background && !extended ? 17u : 0u;
            return queried_token;
        },
        [&](uint32_t) { ++closed_sessions; },
        [&](uint32_t, uint32_t command, std::wstring verb, std::wstring text) {
            invoked_command = command;
            invoked_verb = std::move(verb);
            invoked_text = std::move(text);
        },
        {}, {},
    });
    passed &= Report("context menu deduplicates static prefetch requests",
        context_menu.RequestStaticPrefetch(L".txt") &&
        !context_menu.RequestStaticPrefetch(L".txt"));
    context_menu.CompleteStaticVerbs(
        L".txt", { { L"edit", L"Edit text", L"" } });
    context_menu.StartQuery(context, nullptr, { L"C:\\one.txt" }, false, L".txt",
        false, [](const std::wstring& path) { return path; }, {});
    passed &= Report("context menu begins a typed shell session",
        queried_token == 17);
    std::vector<pulse::ops::ShellMenuItem> com_items;
    pulse::ops::ShellMenuItem com_item;
    com_item.id = 23;
    com_item.verb = L"customverb";
    com_item.text = L"Custom action";
    com_items.push_back(com_item);
    const auto partial = context_menu.CompleteComQuery(
        17, com_items, GetTickCount64(), true);
    passed &= Report("context menu partial COM snapshot keeps the session open",
        partial.accepted && partial.partial);
    passed &= Report("context menu accepts only the active COM response",
        !context_menu.CompleteComQuery(99, {}, 1200).accepted);
    const auto completion = context_menu.CompleteComQuery(
        17, com_items, GetTickCount64());
    passed &= Report("context menu final COM snapshot finishes the session",
        completion.accepted && !completion.partial && completion.cache_key == L".txt");
    pulse::ui::FluentMenuItem base_item;
    base_item.command = 1;
    base_item.text = L"Base";
    bool prefs_changed = false;
    const auto display = context_menu.BuildDisplay(context, { base_item }, prefs_changed);
    passed &= Report("context menu composes cached static and live COM rows",
        display.size() >= 4 && prefs_changed);
    context_menu.InvalidateCaches();
    context_menu.OpenMenu({ std::move(base_item) });
    passed &= Report("context menu owns popup baseline state",
        context_menu.menu_open() && !context_menu.base_items().empty());
    pulse::ops::ShellMenuItem live_item = com_item;
    live_item.id = 42;
    passed &= Report("context menu remaps snapshot ids onto the live session",
        context_menu.CompleteComQuery(17, { live_item }, GetTickCount64()).accepted);
    context_menu.CloseMenu();
    context_menu.ScheduleFolderRefresh(2000);
    passed &= Report("context menu owns two-stage refresh deadlines",
        context_menu.ConsumeDueRefreshes(2399) == 0 &&
        context_menu.ConsumeDueRefreshes(2400) == 1 &&
        context_menu.ConsumeDueRefreshes(3600) == 1);
    const bool handled_shell = context_menu.ExecuteShellCommand(
        pulse::app::CmdShellComBase + 23, {}, [&] { ++folder_refreshes; });
    passed &= Report("context menu invokes by verb when live ids differ",
        handled_shell && invoked_command == 42 &&
        invoked_verb == L"customverb" && invoked_text == L"Custom action" &&
        closed_sessions == 0 && folder_refreshes == 1);

    std::mutex task_mutex;
    std::condition_variable task_cv;
    bool task_done = false;
    bool task_started = false;
    bool release_task = false;
    pulse::app::SettingsTaskResult task_result;
    pulse::app::SettingsTask volume_task;
    volume_task.kind = pulse::app::SettingsTaskKind::Volume;
    volume_task.key = L"volume-async";
    volume_task.enabled = true;
    passed &= Report("settings async task admits one volume request",
        pulse::app::SettingsControllerTestPeer::Start(settings, volume_task,
            [&](const pulse::app::SettingsTask&, std::wstring&) {
                std::unique_lock<std::mutex> lock(task_mutex);
                task_started = true;
                task_cv.notify_one();
                task_cv.wait(lock, [&] { return release_task; });
                return true;
            }, [&](pulse::app::SettingsTaskResult result) {
                std::lock_guard<std::mutex> lock(task_mutex);
                task_result = std::move(result);
                task_done = true;
                task_cv.notify_one();
            }));
    {
        std::unique_lock<std::mutex> lock(task_mutex);
        task_cv.wait_for(lock, std::chrono::seconds(2), [&] { return task_started; });
    }
    passed &= Report("settings async task coalesces duplicate volume request",
        !pulse::app::SettingsControllerTestPeer::Start(settings, volume_task,
            [](const pulse::app::SettingsTask&, std::wstring&) { return true; },
            [](pulse::app::SettingsTaskResult) {}));
    pulse::app::SettingsTask exclude_task;
    exclude_task.kind = pulse::app::SettingsTaskKind::Exclude;
    exclude_task.path = L"C:\\excluded";
    passed &= Report("settings serializes all local index configuration",
        !pulse::app::SettingsControllerTestPeer::Start(settings, std::move(exclude_task),
            [](const pulse::app::SettingsTask&, std::wstring&) { return true; },
            [](pulse::app::SettingsTaskResult) {}));
    {
        std::lock_guard<std::mutex> lock(task_mutex);
        release_task = true;
        task_cv.notify_all();
    }
    {
        std::unique_lock<std::mutex> lock(task_mutex);
        const bool completed = task_cv.wait_for(lock, std::chrono::seconds(2),
                                                [&] { return task_done; });
        passed &= Report("settings async task invokes completion",
                         completed && task_result.ok && task_result.task.key == L"volume-async");
    }
    passed &= Report("settings async task releases volume pending state",
        !settings.VolumePending(L"volume-async"));

    task_done = false;
    pulse::app::SettingsTask failed_task;
    failed_task.kind = pulse::app::SettingsTaskKind::NetworkAdd;
    failed_task.path = L"\\\\server\\missing";
    passed &= Report("settings async task propagates operation failure",
        pulse::app::SettingsControllerTestPeer::Start(settings, std::move(failed_task),
            [](const pulse::app::SettingsTask&, std::wstring& error) {
                error = L"offline";
                return false;
            }, [&](pulse::app::SettingsTaskResult result) {
                std::lock_guard<std::mutex> lock(task_mutex);
                task_result = std::move(result);
                task_done = true;
                task_cv.notify_one();
            }));
    {
        std::unique_lock<std::mutex> lock(task_mutex);
        const bool completed = task_cv.wait_for(lock, std::chrono::seconds(2),
                                                [&] { return task_done; });
        passed &= Report("settings async task preserves failure detail",
                         completed && !task_result.ok && task_result.error == L"offline");
    }
    passed &= Report("settings async task releases network pending state",
        !settings.network_pending());

    bool lifecycle_completed = false;
    {
        SettingsController lifecycle;
        pulse::app::SettingsTask task;
        task.kind = pulse::app::SettingsTaskKind::NetworkRebuild;
        pulse::app::SettingsControllerTestPeer::Start(lifecycle, std::move(task),
            [](const pulse::app::SettingsTask&, std::wstring&) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                return true;
            }, [&](pulse::app::SettingsTaskResult) { lifecycle_completed = true; });
    }
    passed &= Report("settings controller joins tasks during destruction",
        lifecycle_completed);

    std::printf("\n== app controller tests: %s ==\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}

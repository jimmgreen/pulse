#include "../common/windows_compat.h"
#include "quick_access.h"
// app_main.cpp — Pulse UI process entry point, window, input, shot mode.
#include "../ui/ui_compositor.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/ui_renderer.h"
#include "../ui/fluent_menu.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../ui/quick_preview_window.h"
#include "../ui/typography.h"
#include "../common/crash_reporter.h"
#include "../common/diagnostics_exporter.h"
#include "../common/localization.h"
#include "pulse_version.h"
#include "../fs/fs_enum.h"
#include "../fs/fs_recycle.h"
#include "../fs/fs_snapshot.h"
#include "../fs/fs_watch.h"
#include "../fs/fs_net_cache.h"
#include "app_model.h"
#include "app_worker.h"
#include "snapshot_patch.h"
#include "session.h"
#include "context_menu.h"
#include "context_menu_controller.h"
#include "shell_verbs.h"
#include "places.h"
#include "batch_rename.h"
#include "details_meta.h"
#include "context_menu_prefs.h"
#include "app_prefs.h"
#include "saved_search.h"
#include "settings_controller.h"
#include "single_instance_coordinator.h"
#include "tray_controller.h"
#include "tab_controller.h"
#include "update_checker.h"
#include "app_updates.h"
#include "link_resolve.h"
#include "../ui/color_picker.h"
#include "../ui/bloom_accent_picker.h"
#ifdef PULSE_WITH_SELFTEST
#include "selftest_1b2.h"
#endif
#include "resource.h"
#include "../index/index_client.h"
#include "../index/network_index.h"
#include "../index/network_agent_client.h"
#include "../index/content_search_client.h"
#include "../ops/ops_manager.h"
#include "../ops/clipboard.h"
#include "../ipc/ctx_menu_util.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include <windows.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <prsht.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <process.h>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "psapi.lib")
#include "app_internal.h"
#include "duplicate_scan.h"
#include <commctrl.h>

using namespace pulse;

uint64_t FileTimeValue(const FILETIME& value) {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

void UpdateProcessMetrics(AppState& s) {
    const ULONGLONG now = GetTickCount64();
    if (s.processSampleTick != 0 && now - s.processSampleTick < 500) return;

    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const uint64_t processTime = FileTimeValue(kernel) + FileTimeValue(user);
        if (s.processSampleTick != 0 && processTime >= s.lastProcessTime100ns) {
            const double wall100ns = static_cast<double>(now - s.processSampleTick) * 10000.0;
            const DWORD processors = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
            s.processCpuPercent = 100.0 * static_cast<double>(processTime - s.lastProcessTime100ns)
                / (wall100ns * processors);
        }
        s.lastProcessTime100ns = processTime;
    }

    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        s.workingSetMb = static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
    }
    s.processSampleTick = now;
}

void Render(AppState& s) {
    auto t0 = std::chrono::steady_clock::now();

    if (s.compositor.NeedsRecovery()) {
        if (!s.compositor.Recover()) return;
        s.compositor.RecreateTextFormats(s.scale);
        s.renderer.SetCompositor(&s.compositor);
        s.renderer.SetScale(s.scale);
    }

    if (s.hwnd) {
        RECT rc;
        GetClientRect(s.hwnd, &rc);
        int w = rc.right;
        int h = rc.bottom;
        if (w != s.compositor.Width() || h != s.compositor.Height()) {
            s.compositor.Resize(w, h);
        }
    }

    ID2D1DeviceContext* dc = s.compositor.Dc();
    const auto draw_start = std::chrono::steady_clock::now();
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    bool hc = s.shot_high_contrast || ui::IsHighContrast();
    ui::Theme theme = hc ? ui::MakeHighContrastTheme() : ui::MakeTheme(s.darkMode, s.accentColor);

    UpdateProcessMetrics(s);
    ui::WindowViewModel vm = BuildVm(s);
    vm.backdrop_enabled = !hc && s.compositor.UsesTransparentComposition() && s.backdropActive;
    vm.pane.hover_index = s.hoverRow;

    D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s.compositor.Width(), (float)s.compositor.Height());
    s.renderer.Render(vm, rect, theme);
    s.notification_toast.Draw(s.compositor, theme, s.scale, hc);

    const HRESULT end_hr = dc->EndDraw();
    const auto draw_end = std::chrono::steady_clock::now();
    s.timing.draw_ms = std::chrono::duration<double, std::milli>(draw_end - draw_start).count();
    if (end_hr == D2DERR_RECREATE_TARGET || end_hr == DXGI_ERROR_DEVICE_REMOVED ||
        end_hr == DXGI_ERROR_DEVICE_RESET || end_hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
        s.compositor.NotifyDeviceLost(end_hr);
        return;
    }
    const auto present_start = std::chrono::steady_clock::now();
    wchar_t hidden_frame[4]{};
    const bool hidden_capture = s.shot.active &&
        GetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", hidden_frame, ARRAYSIZE(hidden_frame)) == 1 &&
        hidden_frame[0] == L'1';
    // An occluded flip chain advances to an unpainted buffer after Present.
    if (!hidden_capture) s.compositor.Present();
    const auto present_end = std::chrono::steady_clock::now();
    s.timing.present_ms = std::chrono::duration<double, std::milli>(present_end - present_start).count();

    if (s.renameIndex >= 0) LayoutRenameOverlay(s);
    if (!s.tagRenameId.empty()) LayoutTagRenameOverlay(s);
    if (s.addressEditing) LayoutAddressEditor(s);
    if (s.filterEditing) LayoutFilterEditor(s);

    if (s.shot.active && !s.timing.first_frame_recorded) {
        s.timing.first_frame_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - s.shot.start).count();
        s.timing.first_frame_recorded = true;
    }

    auto t1 = std::chrono::steady_clock::now();
    s.lastFrameMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double dt = std::chrono::duration<double>(t1 - s.lastFrameTime).count();
    if (dt > 0.0) s.lastFps = 1.0 / dt;
    s.lastFrameTime = t1;
}

LRESULT CALLBACK WndProcImpl(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* s = GetAppState(hwnd);
    if (s && s->notification_toast.HandleMessage(hwnd, msg, wParam, lParam)) return 0;

    switch (msg) {
    case WM_NCCALCSIZE: {
        if (wParam && IsZoomed(hwnd)) {
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromRect(&params->rgrc[0],
                    MONITOR_DEFAULTTONEAREST), &monitor)) {
                // Our custom frame has no invisible maximized border to inset.
                // Use the destination monitor's work area, including taskbar offsets.
                params->rgrc[0] = monitor.rcWork;
            }
        }
        return 0;
    }

    case WM_CREATE: {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        s = reinterpret_cast<AppState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        s->hwnd = hwnd;
        s->tray_controller.Attach(hwnd, cs->hInstance);

        s->scale = s->shot_scale_override > 0.0f
            ? s->shot_scale_override : (float)pulse::compat::WindowDpi(hwnd) / 96.0f;
        s->accentColor = ui::GetAccentColor();
        s->darkMode = ui::ShouldUseDarkMode(s->themeOverride);
        s->backdropActive = ui::ApplyWindowEffect(hwnd, ui::WindowEffect::MicaAlt, s->darkMode);

        if (!s->compositor.Init(hwnd)) {
            const std::wstring message = L"Failed to initialize D3D/D2D/DWrite\n\n" +
                s->compositor.InitializationError() + L"\n\nLog: %LOCALAPPDATA%\\Pulse\\pulse_graphics.log";
            MessageBoxW(hwnd, message.c_str(), L"Pulse", MB_OK | MB_ICONERROR);
            return -1;
        }
        s->compositor.RecreateTextFormats(s->scale);
        if (s->shot.active) {
            wchar_t toast_test[2]{};
            if (GetEnvironmentVariableW(L"PULSE_TEST_TOAST", toast_test, 2) == 1 && toast_test[0] == L'1')
                s->notification_toast.Show(hwnd, L"索引迁移未完成", L"目标磁盘空间不足，原索引已保留。请释放空间后重试。");
        }
        s->renderer.SetCompositor(&s->compositor);
        s->renderer.SetScale(s->scale);
        s->renderer.SetIconNotifyWindow(hwnd);
        s->quickPreview.Initialize(hwnd, WM_QUICK_PREVIEW_NAVIGATE,
                                   WM_QUICK_PREVIEW_OPEN);

        s->window_tabs.EnsureDefault();
        BindCurrentLayout(*s);
        s->tabs.SetCallbacks({
            [s](app::Tab& tab) { StartLoadingPath(*s, tab, tab.current_path); },
            [hwnd] { InvalidateRect(hwnd, nullptr, FALSE); },
            [s] { BindCurrentLayout(*s); },
            [s] { RememberLayoutFocus(*s); },
        });
        if (s->isolatedTest) {
            s->places.persist = false;
            s->appPrefs.persist = false;
        }
        s->savedSearches.Load();
        SyncSavedSearchSidebar(*s);
        s->places.Load();
        ProbePinnedNetworks(*s);
        s->ctxMenuPrefs.Load();
        s->appPrefs.Load();
        s->showFps = s->forceStatusPerformance || s->appPrefs.show_status_performance;
        s->duplicateScan.scope = static_cast<app::DuplicateScanScope>(
            std::clamp(s->appPrefs.duplicate_scan_scope, 0, 2));
        s->duplicateScan.folder_path = s->appPrefs.duplicate_scan_folder;
        s->duplicateScan.drive_root =
            app::DuplicateScanSession::NormalizeDriveRoot(s->appPrefs.duplicate_scan_drive);
        s->duplicateScan.minimum_file_bytes =
            app::DuplicateScanSession::DefaultMinimumBytes(s->duplicateScan.scope);
        if (s->shot.active && l10n::IsLanguageId(s->shot.language) &&
            s->shot.language != L"system")
            s->appPrefs.language = s->shot.language;
        l10n::Initialize(cs->hInstance, s->appPrefs.language);
        if (s->shot.update_available) {
            s->update_result_ready = true;
            s->update_result.update_available = true;
            s->update_result.version = L"9.8.7";
            s->update_result.download_page = L"https://updates.example.test/pulse";
            s->update_result.installer_sha256 =
                L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        }
        s->sidebar = app::BuildSidebarModel(&s->recycle_info);
        SyncSavedSearchSidebar(*s);
        ui::typography::InvalidateCaches();
        s->compositor.RecreateTextFormats(s->scale);
        if (s->safeMode) {
            s->appPrefs.window_effect = L"none";
            s->appPrefs.background_image.clear();
        } else {
            SeedShellVerbCache(*s);
            StartShellRegistryWatch(hwnd);
        }
        s->renderer.SetRowHeightDip(static_cast<float>(s->appPrefs.row_height));
        s->renderer.SetTrayIconDip(static_cast<float>(s->appPrefs.tray_icon_size));
        ApplyAccentFromPrefs(*s, true);
        ApplyAppWindowChrome(*s);
        if (s->appPrefs.keep_running_on_close) s->tray_controller.SetVisible(true);
        s->index.Start(hwnd, WM_INDEX_NOTIFY, WM_INDEX_SEARCH);
        s->networkIndex.Start(hwnd, WM_NETWORK_INDEX_NOTIFY, WM_NETWORK_INDEX_SEARCH);
        s->contentSearch.Start(hwnd, WM_CONTENT_SEARCH);
        s->duplicateSearch.Start(hwnd, WM_DUPLICATE_SCAN);
        s->settings.SetServiceInstalled(s->index.ServiceInstalled());
        app::SettingsController::UiCallbacks settings_callbacks;
        settings_callbacks.pick_image = [hwnd](std::wstring& path) {
            return PickImageFile(hwnd, path);
        };
        settings_callbacks.pick_folder = [hwnd](std::wstring& path, std::wstring_view title) {
            const std::wstring owned_title(title);
            return PickFolder(hwnd, path, owned_title.c_str());
        };
        settings_callbacks.apply_effects = [s](app::SettingsEffect effects) {
            ApplySettingsEffects(*s, effects);
        };
        settings_callbacks.task_completion = SettingsCompletion(hwnd);
        settings_callbacks.open_path = [hwnd](const std::wstring& path) {
            ShellExecuteW(hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        };
        settings_callbacks.open_diagnostics = [hwnd] {
            const std::wstring path = app::GetPulseDataDir() + L"\\Diagnostics\\Crashes";
            CreateDirectoryW((app::GetPulseDataDir() + L"\\Diagnostics").c_str(), nullptr);
            CreateDirectoryW(path.c_str(), nullptr);
            ShellExecuteW(hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        };
        settings_callbacks.clear_diagnostics = [hwnd](std::wstring& error) {
            if (MessageBoxW(hwnd,
                    l10n::Get(l10n::StringId::DiagnosticsClearConfirm).c_str(),
                    l10n::Get(l10n::StringId::Diagnostics).c_str(),
                    MB_ICONWARNING | MB_OKCANCEL | MB_DEFBUTTON2) != IDOK)
                return true;
            if (diagnostics::ClearCrashReports(app::GetPulseDataDir(), &error)) return true;
            error = l10n::Get(l10n::StringId::DiagnosticsClearFailed);
            return false;
        };
        settings_callbacks.prepare_diagnostics_export =
            [hwnd](std::wstring& destination, bool& include_service) {
            if (MessageBoxW(hwnd,
                    l10n::Get(l10n::StringId::DiagnosticsPrivacyMessage).c_str(),
                    l10n::Get(l10n::StringId::DiagnosticsPrivacyTitle).c_str(),
                    MB_ICONWARNING | MB_OKCANCEL | MB_DEFBUTTON2) != IDOK)
                return false;
            const int service = MessageBoxW(hwnd,
                l10n::Get(l10n::StringId::DiagnosticsIncludeService).c_str(),
                l10n::Get(l10n::StringId::DiagnosticsPrivacyTitle).c_str(),
                MB_ICONQUESTION | MB_YESNOCANCEL | MB_DEFBUTTON2);
            if (service == IDCANCEL) return false;
            include_service = service == IDYES;
            return PickFolder(hwnd, destination,
                l10n::Get(l10n::StringId::DiagnosticsExportLocation).c_str());
        };
        settings_callbacks.export_diagnostics =
            [](const std::wstring& destination, bool include_service,
               std::wstring& error) {
            WIN32_FIND_DATAW data{};
            HANDLE find = FindFirstFileW((destination + L"\\*").c_str(), &data);
            bool empty = true;
            if (find != INVALID_HANDLE_VALUE) {
                do {
                    if (wcscmp(data.cFileName, L".") != 0 &&
                        wcscmp(data.cFileName, L"..") != 0) {
                        empty = false;
                        break;
                    }
                } while (FindNextFileW(find, &data));
                FindClose(find);
            }
            if (!empty) return false;

            diagnostics::ExportOptions options;
            options.source_root = app::GetPulseDataDir();
            options.include_dumps = true;
            options.require_empty_destination = true;
            if (!include_service) {
                options.destination = destination;
                return diagnostics::Export(options, &error);
            }

            const std::wstring service_dir = destination + L"\\IndexService";
            if (!CreateDirectoryW(service_dir.c_str(), nullptr) ||
                !index::IndexClient::ExportDiagnosticsElevated(service_dir))
                return false;
            const std::wstring user_dir = destination + L"\\User";
            if (!CreateDirectoryW(user_dir.c_str(), nullptr)) return false;
            options.destination = user_dir;
            return diagnostics::Export(options, &error);
        };
        s->settings.BindUi(s->appPrefs, s->ctxMenuPrefs, s->index,
                           s->networkIndex, std::move(settings_callbacks));

        s->worker.Start([s](app::WorkResult res) { PostWorkerResult(*s, std::move(res)); });
        RequestRecycleOccupancy(*s);

        // Ops layer: queue worker + shell host IPC; notify repaints the status bar.
        const std::wstring data_dir = app::GetPulseDataDir();
        if (!s->isolatedTest && !data_dir.empty())
            s->ops.SetJournalPath(data_dir + L"\\operations.json");
        s->ops.SetVerifyCopies(s->appPrefs.verify_copies);
        s->ops.Start([hwnd] { PostMessageW(hwnd, WM_OPS_NOTIFY, 0, 0); });
        const ops::RecoverySnapshot recovery = s->isolatedTest
            ? ops::RecoverySnapshot{} : s->ops.PendingRecovery();
        if (!recovery.entries.empty()) {
            std::wstring prompt = L"检测到上次退出时未完成的文件操作（" +
                std::to_wstring(recovery.entries.size()) + L" 项）。\n\n是否重试可安全恢复的项目？";
            if (recovery.has_uncertain_destructive)
                prompt += L"\n\n未确认状态的永久删除不会重试。";
            if (MessageBoxW(hwnd, prompt.c_str(), L"Pulse 文件操作恢复",
                            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES)
                s->ops.RetryRecovery();
            else
                s->ops.DiscardRecovery();
        }
        s->context_menu.SetShellOperations({
            [s](std::vector<std::wstring> paths, HWND owner, bool background, bool extended,
                std::vector<std::wstring> disabled) {
                if (s->safeMode) return uint32_t{0};
                return s->ops.QueryShellMenu(std::move(paths), owner, background, extended,
                                             std::move(disabled));
            },
            [s](uint32_t token) { s->ops.CloseShellMenu(token); },
            [s](uint32_t token, uint32_t command, std::wstring verb, std::wstring text) {
                s->ops.InvokeShellMenu(token, command, std::move(verb), std::move(text));
            },
            [s](const std::wstring& path, const std::wstring& verb) {
                s->ops.ExecuteVerb(path, verb);
            },
            [s](const std::wstring& app_path, const std::wstring& path) {
                s->ops.OpenWithApp(app_path, path);
            },
            [s](const std::wstring& command, const std::wstring& path) {
                s->ops.ExecuteCommand(command, path);
            },
        });
        // Explorer verbs arrive on the shell client's reader thread; hop to
        // the UI thread with an owned payload (freed by the WM handler).
        s->ops.SetShellMenuCallback([hwnd](uint32_t token,
                                           std::vector<ops::ShellMenuItem> items,
                                           bool partial,
                                           std::vector<std::wstring> slow_clsids) {
            auto* payload = new ShellCtxItemsPayload{
                std::move(items), partial, std::move(slow_clsids) };
            if (!PostMessageW(hwnd, WM_SHELLCTX_ITEMS, token,
                              reinterpret_cast<LPARAM>(payload)))
                delete payload;
        });
        if (!s->pending_undo_json.empty()) s->ops.UndoFromJson(s->pending_undo_json);
        s->operationWindow = std::make_unique<ui::FileOperationWindow>();
        ui::FileOperationCallbacks operation_callbacks;
        operation_callbacks.cancel = [hwnd] {
            if (AppState* state = GetAppState(hwnd)) state->ops.CancelCurrent();
        };
        operation_callbacks.pause = [hwnd] {
            if (AppState* state = GetAppState(hwnd)) state->ops.PauseCurrent();
        };
        operation_callbacks.resume = [hwnd] {
            if (AppState* state = GetAppState(hwnd)) state->ops.ResumeCurrent();
        };
        operation_callbacks.dismiss = [hwnd] {
            if (AppState* state = GetAppState(hwnd)) {
                state->operationDismissedTaskId = state->ops.Status().task_id;
                state->operationPinnedByUser = false;
            }
        };
        s->operationWindow->Create(hwnd, std::move(operation_callbacks));
        s->operationWindow->SetTheme(s->darkMode, s->accentColor);

        // OLE drop target: list rows, pane headers, breadcrumbs, sidebar, tray.
        {
            ui::DropTargetCallbacks dcb;
            dcb.drag_over = [hwnd](const std::vector<std::wstring>& srcs, POINT pt,
                                   DWORD keys, DWORD allowed) -> DWORD {
                AppState* st = GetAppState(hwnd);
                return st ? ResolveDropTarget(*st, srcs, pt, keys, allowed) : DROPEFFECT_NONE;
            };
            dcb.drag_leave = [hwnd] {
                if (AppState* st = GetAppState(hwnd)) ClearDropFeedback(*st);
            };
            dcb.drop = [hwnd](const std::vector<std::wstring>& srcs, POINT pt,
                              DWORD keys, DWORD preferred) -> DWORD {
                AppState* st = GetAppState(hwnd);
                return st ? DropExecute(*st, srcs, pt, keys, preferred) : DROPEFFECT_NONE;
            };
            s->dropTarget = new ui::WindowDropTarget(hwnd, std::move(dcb));
            RegisterDragDrop(hwnd, s->dropTarget);
        }

        if (!s->shot.active && !s->session_layout_tabs.empty()) {
            s->pane = nullptr;
            s->targetPane = nullptr;
            app::RestoreWindowTabs(
                s->window_tabs, s->session_layout_tabs, s->session_tab_groups,
                s->session_active_layout_tab,
                [s](app::Tab& tab, const std::wstring& path) {
                    WarmupUnc(*s, path);
                    StartLoadingPath(*s, tab, path);
                });
            for (size_t i = 0; i < s->window_tabs.items.size(); ++i) {
                app::RebuildLayoutRoot(*s->window_tabs.items[i]);
                if (i < s->session_layout_tabs.size() &&
                    s->window_tabs.items[i]->root &&
                    !s->session_layout_tabs[i].split_ratios.empty()) {
                    app::ApplySplitRatios(*s->window_tabs.items[i]->root,
                                          s->session_layout_tabs[i].split_ratios);
                }
            }
            BindCurrentLayout(*s);
            if (!s->session_path.empty())
                RememberPath(*s, s->session_path);
            else if (app::Tab* t = ActiveTab(*s))
                RememberPath(*s, t->current_path);
        } else {
        std::wstring startPath = s->shot.active ? s->shot.path : L"C:\\";
        if (!s->shot.active && !s->session_path.empty()) startPath = s->session_path;
        else if (!s->shot.active && !s->open_path.empty())
            startPath = ResolveOpenFolderPath(s->open_path);
        s->pane->NewTab(startPath);
        if (s->shot.active) s->pane->ActiveTab()->view_mode = s->shot.view_mode;
        StartLoadingPath(*s, *s->pane->ActiveTab(), startPath);
        RememberPath(*s, startPath);
        }

        if (!s->shot.active && !s->open_path.empty() &&
            (!s->session_layout_tabs.empty() || !s->session_path.empty())) {
            const std::wstring open_path = ResolveOpenFolderPath(s->open_path);
            if (!open_path.empty() && !ActivateExistingFolderTab(*s, open_path))
                NewTab(*s, open_path);
        }

        s->lastFrameTime = std::chrono::steady_clock::now();
        s->renderer.SetDetailsPanelVisible(s->showDetailsPanel);
        s->renderer.SetDetailsPanelWidth(s->detailsPanelWidth);
        SetTimer(hwnd, kTimerUi, 16, nullptr);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
            SWP_NOACTIVATE | SWP_FRAMECHANGED);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        // Arrives before WM_CREATE; GWLP_USERDATA is not set yet.
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        float sc = s ? s->scale : 1.0f;
        mmi->ptMinTrackSize.x = (LONG)(640 * sc);
        mmi->ptMinTrackSize.y = (LONG)(420 * sc);
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
            mmi->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left;
            mmi->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top;
            mmi->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left;
            mmi->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top;
            mmi->ptMaxTrackSize.x = std::max(mmi->ptMaxTrackSize.x, mmi->ptMaxSize.x);
            mmi->ptMaxTrackSize.y = std::max(mmi->ptMaxTrackSize.y, mmi->ptMaxSize.y);
        }
        return 0;
    }

    case WM_NCHITTEST: {
        POINT screenPt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (!IsZoomed(hwnd)) {
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            const UINT dpi = pulse::compat::WindowDpi(hwnd);
            const int frameX = pulse::compat::SystemMetricsForDpi(SM_CXSIZEFRAME, dpi)
                             + pulse::compat::SystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            const int frameY = pulse::compat::SystemMetricsForDpi(SM_CYSIZEFRAME, dpi)
                             + pulse::compat::SystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            const bool left = screenPt.x >= wr.left && screenPt.x < wr.left + frameX;
            const bool right = screenPt.x < wr.right && screenPt.x >= wr.right - frameX;
            const bool top = screenPt.y >= wr.top && screenPt.y < wr.top + frameY;
            const bool bottom = screenPt.y < wr.bottom && screenPt.y >= wr.bottom - frameY;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        if (!s) break;
        POINT pt = screenPt;
        ScreenToClient(hwnd, &pt);
        float x = (float)pt.x;
        float y = (float)pt.y;
        float tbH = s->renderer.TitleBarHeight();
        if (y >= 0 && y < tbH) {
            D2D1_RECT_F bounds = D2D1::RectF(0, 0,
                (float)s->compositor.Width(), (float)s->compositor.Height());
            ui::HitTestResult hit = s->renderer.HitTest(BuildVm(*s), bounds, x, y);
            if (hit.region == ui::HitTestResult::Maximize) return HTMAXBUTTON;
            if (hit.region == ui::HitTestResult::Minimize) return HTMINBUTTON;
            if (hit.region == ui::HitTestResult::Close) return HTCLOSE;
            if (hit.region != ui::HitTestResult::None) return HTCLIENT;
            return HTCAPTION;
        }
        break;
    }

    case WM_NCMOUSEMOVE: {
        if (!s) break;
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);
        D2D1_RECT_F bounds = D2D1::RectF(0, 0,
            (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(BuildVm(*s), bounds,
            (float)pt.x, (float)pt.y);
        s->hoverPoint = pt;
        const int region = static_cast<int>(hit.region);
        if (region != s->hoverRegion || hit.index != s->hoverControlIndex) {
            s->hoverRegion = region;
            s->hoverControlIndex = hit.index;
            s->hoverSubIndex = hit.sub_index;
            s->hoverPath = hit.path;
            s->hoverSince = GetTickCount64();
            s->tooltipText.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd, 0 };
        TrackMouseEvent(&tme);
        break;
    }

    case WM_NCMOUSELEAVE:
        if (s) {
            s->hoverRegion = 0;
            s->hoverControlIndex = -1;
            s->hoverSubIndex = -1;
            s->hoverSince = 0;
            s->tooltipText.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_NCLBUTTONDOWN: {
        if (!s) break;
        if (wParam == HTMINBUTTON) {
            ShowWindow(hwnd, SW_MINIMIZE);
            return 0;
        }
        if (wParam == HTMAXBUTTON) {
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            return 0;
        }
        if (wParam == HTCLOSE) {
            if (s->appPrefs.keep_running_on_close) s->tray_controller.HideWindow();
            else DestroyWindow(hwnd);
            return 0;
        }
        break;
    }

    case WM_CLOSE: {
        if (s && s->appPrefs.keep_running_on_close) {
            s->tray_controller.HideWindow();
            return 0;
        }
        break;
    }

    case WM_COPYDATA: {
        auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lParam);
        std::wstring path;
        if (!s || !app::SingleInstanceCoordinator::DecodeOpenPath(cds, path)) return FALSE;
        OpenFolderInNewTab(*s, path);
        return TRUE;
    }

    case app::TrayController::kCallbackMessage: {
        if (!s) return 0;
        const auto result = s->tray_controller.HandleCallback(lParam);
        if (result == app::TrayController::CallbackResult::ExitRequested)
            DestroyWindow(hwnd);
        return 0;
    }

    case WM_DPICHANGED: {
        s->scale = (float)HIWORD(wParam) / 96.0f;
        RECT* rc = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, rc->left, rc->top,
            rc->right - rc->left, rc->bottom - rc->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        s->compositor.RecreateTextFormats(s->scale);
        s->renderer.SetScale(s->scale);
        if (s->editFont) {
            DeleteObject(s->editFont);
            s->editFont = nullptr;
        }
        EnsureEditVisuals(*s);
        if (s->hwndAddressEdit) SendMessageW(s->hwndAddressEdit, WM_SETFONT, (WPARAM)s->editFont, TRUE);
        if (s->hwndRenameEdit) SendMessageW(s->hwndRenameEdit, WM_SETFONT, (WPARAM)s->editFont, TRUE);
        if (s->hwndTagRenameEdit) SendMessageW(s->hwndTagRenameEdit, WM_SETFONT, (WPARAM)s->editFont, TRUE);
        if (s->addressEditing) LayoutAddressEditor(*s);
        if (!s->tagRenameId.empty()) LayoutTagRenameOverlay(*s);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_SETTINGCHANGE:
        if (s && (lParam == 0 ||
                  wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0 ||
                  wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"HighContrast") == 0)) {
            if (s->themeOverride == ui::ThemeMode::Auto)
                s->darkMode = ui::ShouldUseDarkMode(s->themeOverride);
            if (s->appPrefs.accent_rgb.empty())
                ApplyAccentFromPrefs(*s, true);
            ApplyAppWindowChrome(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_ACTIVATE:
        if (s) s->renderer.NotifyPreviewActivate(LOWORD(wParam) != WA_INACTIVE);
        break;

    case WM_ACTIVATEAPP:
        if (s) s->renderer.NotifyPreviewActivate(wParam != 0);
        return 0;

    case WM_MOVE:
        if (s) {
            s->compositor.UpdateTextRenderingParams(
                MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
            s->renderer.NotifyPreviewOwnerMoved();
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_SIZE: {
        if (s) {
            s->compositor.Resize(LOWORD(lParam), HIWORD(lParam));
            s->maximized = (wParam == SIZE_MAXIMIZED);
            if (s->addressEditing) LayoutAddressEditor(*s);
            if (s->filterEditing && !s->filterFocusPending) LayoutFilterEditor(*s);
            if (!s->tagRenameId.empty()) LayoutTagRenameOverlay(*s);
            ClampScroll(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (s) Render(*s);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_CTLCOLOREDIT: {
        if (!s) break;
        EnsureEditVisuals(*s);
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, s->darkMode ? RGB(255, 255, 255) : RGB(26, 26, 26));
        SetBkColor(hdc, s->darkMode ? RGB(30, 30, 30) : RGB(255, 255, 255));
        SetBkMode(hdc, OPAQUE);
        return reinterpret_cast<LRESULT>(s->editBrush);
    }

    case WM_TIMER: {
        if (s && wParam == kTimerUi) {
            bool dirty = false;
            DrainDirNotifies(*s);
            const ULONGLONG now = GetTickCount64();
            TickUpdates(*s, now);
            if (TickAddressSearch(*s, now)) dirty = true;
            const int shell_refreshes = s->context_menu.ConsumeDueRefreshes(now);
            for (int i = 0; i < shell_refreshes; ++i) {
                RefreshActiveTab(*s);
                dirty = true;
            }
            if (PumpRecycleRefresh(*s, now)) dirty = true;
            MaybePrefetchHoverCtxMenu(*s);
            s->places.FlushPendingSave(false);
            if (s->renameClickCandidate && s->renameClickDue != 0 &&
                now >= s->renameClickDue) {
                app::Tab* tab = ActiveTab(*s);
                const int index = s->renameClickIndex;
                const bool valid = s->pane == s->renameClickPane &&
                    tab == s->renameClickTab && tab &&
                    tab->SelectedCount() == 1 && tab->selected_index == index &&
                    index >= 0 && index < tab->CountBound() &&
                    (s->renameClickPath.empty() ||
                     EntryFullPath(*tab, index) == s->renameClickPath) &&
                    s->renameIndex < 0 && s->tagRenameId.empty() &&
                    !s->addressEditing && !s->filterEditing &&
                    !s->dragPending && !s->marqueePending && !s->marqueeActive &&
                    !s->scrollbarDragging && !s->splitterDragging;
                CancelRenameClick(*s);
                if (valid) ShowRenameOverlay(*s);
            }
            // Search affordance: expand only the focused pane, then reveal
            // the hosted edit once it has enough room for stable text layout.
            for (auto& pane : Panes(*s)) {
                const float filterTarget =
                    (s->filterEditing && pane.get() == s->pane) ? 1.0f : 0.0f;
                const float filterStep = (filterTarget - pane->filter_expand) * 0.24f;
                if (std::abs(filterStep) > 0.008f) {
                    pane->filter_expand += filterStep;
                    dirty = true;
                } else if (pane->filter_expand != filterTarget) {
                    pane->filter_expand = filterTarget;
                    dirty = true;
                }
            }
            const float focusedExpand = s->pane ? s->pane->filter_expand : 0.0f;
            if (s->filterFocusPending && focusedExpand >= 0.985f &&
                s->hwndFilterEdit) {
                if (s->pane) s->pane->filter_expand = 1.0f;
                LayoutFilterEditor(*s);
                s->filterIgnoreKillFocus = true;
                ShowWindow(s->hwndFilterEdit, SW_SHOW);
                SetForegroundWindow(s->hwndFilterEdit);
                SetFocus(s->hwndFilterEdit);
                SendMessageW(s->hwndFilterEdit, EM_SETSEL, 0, -1);
                s->filterFocusPending = false;
                s->filterIgnoreKillFocus = false;
                dirty = true;
            }
            // Scrollbar hover expansion.
            float target = s->scrollbarHovered ? (s->renderer.Margin() * 1.5f - 6.0f * s->scale) : 0.0f;
            target = std::max(0.0f, target);
            float step = (target - s->scrollbarHoverWidth) * 0.25f;
            if (std::abs(step) > 0.1f) {
                s->scrollbarHoverWidth += step;
                dirty = true;
            } else if (s->scrollbarHoverWidth != target) {
                s->scrollbarHoverWidth = target;
                dirty = true;
            }
            // Smooth scroll.
            if (s->scrollAnimating) {
                UpdateSmoothScroll(*s);
                dirty = true;
            }
            // Tag slide animation.
            if (!s->tagTracks.empty() || s->tagGapFrom != s->tagGapTo) {
                TickTagTransitions(*s);
                dirty = true;
            }
            if (!s->tabTracks.empty() || !s->chipTracks.empty()) {
                TickTabTransitions(*s);
                dirty = true;
            }
            if (TickTrayDeck(*s)) dirty = true;
            if (app::Tab* tab = ActiveTab(*s)) {
                std::wstring kind, rest;
                if (app::ParsePulsePath(tab->current_path, &kind, &rest) &&
                    kind == L"settings") {
                    const int page = app::SettingsController::PageFromName(rest);
                    if (page == 0 && s->bloom_accent.Tick(0.016f)) dirty = true;
                    if (page == 4 && s->duplicateScan.scanning) dirty = true;
                }
            }
            if (s->hoverRegion != 0 && s->tooltipText.empty() && s->hoverSince != 0 &&
                GetTickCount64() - s->hoverSince >= 400) {
                s->tooltipText = TooltipForHover(*s);
                dirty = !s->tooltipText.empty() || dirty;
            }
            QueueVisibleTagDiscovery(*s);
            UpdateOperationWindow(*s, false);
            if (dirty) InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_SETCURSOR: {
        if (!s || LOWORD(lParam) != HTCLIENT) break;
        if (s->starDragActive || s->tagDragActive || s->tabDragging) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        ui::WindowViewModel vm = BuildVm(*s);
        D2D1_RECT_F bounds = D2D1::RectF(0, 0,
            (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(vm, bounds, (float)pt.x, (float)pt.y);
        if (s->detailsPanelResizing || hit.region == ui::HitTestResult::DetailsResize) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        if (s->columnResizing || hit.region == ui::HitTestResult::ColumnDivider) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        const bool splitter = s->splitterDragging || hit.region == ui::HitTestResult::Splitter;
        if (splitter) {
            const bool vertical = s->splitterDragging
                ? (s->splitterOrientation == app::SplitOrientation::Vertical)
                : (hit.index >= 0 && hit.index < static_cast<int>(vm.splitters.size()) &&
                   vm.splitters[static_cast<size_t>(hit.index)].vertical);
            SetCursor(LoadCursorW(nullptr, vertical ? IDC_SIZEWE : IDC_SIZENS));
            return TRUE;
        }
        if (hit.region == ui::HitTestResult::AddressSearch ||
            hit.region == ui::HitTestResult::AddressSearchScope ||
            hit.region == ui::HitTestResult::AddressSearchClear ||
            hit.region == ui::HitTestResult::AddressSearchClose) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        if (hit.region == ui::HitTestResult::AddressBar || s->addressEditing) {
            SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
            return TRUE;
        }
        if (hit.region == ui::HitTestResult::StatusBarTask) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        if (hit.region == ui::HitTestResult::SettingsAccent) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }

    case WM_MOUSEMOVE:
        return HandleMouseMove(s, hwnd, msg, wParam, lParam);

    case WM_MOUSELEAVE:
        return HandleMouseLeave(s, hwnd, msg, wParam, lParam);

    case WM_LBUTTONDOWN:
        return HandleLButtonDown(s, hwnd, msg, wParam, lParam);

    case WM_LBUTTONDBLCLK:
        return HandleLButtonDblClk(s, hwnd, msg, wParam, lParam);

    case WM_LBUTTONUP:
        return HandleLButtonUp(s, hwnd, msg, wParam, lParam);

    case WM_CAPTURECHANGED:
        return HandleCaptureChanged(s, hwnd, msg, wParam, lParam);

    case WM_RBUTTONDOWN:
        return HandleRButtonDown(s, hwnd, msg, wParam, lParam);

    case WM_RBUTTONUP:
        return HandleRButtonUp(s, hwnd, msg, wParam, lParam);

    case WM_MOUSEWHEEL:
        return HandleMouseWheel(s, hwnd, msg, wParam, lParam);

    case WM_APPCOMMAND:
        // DefWindowProc translates side-button releases, including child controls.
        if (s && HandleBrowserNavigation(*s, lParam)) return TRUE;
        break;

    case WM_SYSKEYDOWN:
        if (s && wParam == L'D' && (GetKeyState(VK_MENU) & 0x8000)) {
            ShowOmnibar(*s, OmnibarMode::Path);
            return 0;
        }
        break;

    case WM_KEYDOWN:
        return HandleKeyDown(s, hwnd, msg, wParam, lParam);

    case WM_COMMAND: {
        if (s && reinterpret_cast<HWND>(lParam) == s->hwndAddressEdit &&
            (HIWORD(wParam) == EN_CHANGE || HIWORD(wParam) == EN_UPDATE)) {
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (s && wParam == 1001) {
            RefreshActiveTab(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    }

    case WM_WORKER_RESULT: {
        if (s) {
            ProcessPendingResults(*s);
            ClampScroll(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_RECYCLE_INFO: {
        auto* info = reinterpret_cast<fs::RecycleBinInfo*>(lParam);
        if (s && info && ApplyQueriedRecycleInfo(*s, *info))
            InvalidateRect(hwnd, nullptr, FALSE);
        delete info;
        return 0;
    }

    case WM_OPS_NOTIFY: {
        if (s) {
            ops::OpStatus st = s->ops.Status();
            UpdateOperationWindow(*s, true);
            if (s->ops.TakeCtxInvokeDone()) {
                if (app::Tab* tab = ActiveTab(*s)) s->store.MarkDirty(tab->current_path);
                RefreshActiveTab(*s);
                ScheduleRecycleRefresh(*s);
                RefreshRecycleViews(*s, false);
            }
            if (st.completed_ops != s->opsCompleted) {
                s->opsCompleted = st.completed_ops;
                std::vector<std::wstring> tag_metadata_paths;
                for (const auto& completed : s->ops.DrainCompletions()) {
                    // Invalidate both sides of every successful mutation. The
                    // focused tab refreshes below; background tabs must not
                    // reuse a stale snapshot when they are shown later.
                    for (const auto& source : completed.sources) {
                        const std::wstring parent = fs::ParentPath(source);
                        if (!parent.empty()) s->store.MarkDirty(parent);
                    }
                    for (const auto& destination : completed.destinations) {
                        const std::wstring parent = fs::ParentPath(destination);
                        if (!parent.empty()) s->store.MarkDirty(parent);
                    }
                    if (completed.type == ops::OpType::Copy) {
                        for (size_t i = 0; i < completed.sources.size() &&
                                           i < completed.destinations.size(); ++i) {
                            s->places.CloneAssignments(completed.sources[i],
                                                       completed.destinations[i]);
                            tag_metadata_paths.push_back(completed.destinations[i]);
                        }
                    } else if (completed.type == ops::OpType::Move ||
                               completed.type == ops::OpType::Rename ||
                               completed.type == ops::OpType::BatchRename) {
                        for (size_t i = 0; i < completed.sources.size() &&
                                           i < completed.destinations.size(); ++i) {
                            s->places.RemapPaths(completed.sources[i],
                                                 completed.destinations[i]);
                            tag_metadata_paths.push_back(completed.destinations[i]);
                        }
                        if (completed.type == ops::OpType::Move &&
                            s->pendingCutClipboardSequence != 0) {
                            s->completedCutClipboardPaths.insert(
                                s->completedCutClipboardPaths.end(), completed.sources.begin(),
                                completed.sources.end());
                            const bool all_done = std::all_of(
                                s->pendingCutClipboardPaths.begin(),
                                s->pendingCutClipboardPaths.end(), [&](const std::wstring& expected) {
                                    const std::wstring normalized = fs::NormalizePath(expected);
                                    return std::any_of(s->completedCutClipboardPaths.begin(),
                                        s->completedCutClipboardPaths.end(),
                                        [&](const std::wstring& actual) {
                                            return _wcsicmp(normalized.c_str(), actual.c_str()) == 0;
                                        });
                                });
                            if (all_done) {
                                ops::CompleteCutClipboard(s->pendingCutClipboardSequence,
                                                          s->pendingCutClipboardPaths);
                                s->pendingCutClipboardSequence = 0;
                                s->pendingCutClipboardPaths.clear();
                                s->completedCutClipboardPaths.clear();
                            }
                        }
                    } else if (completed.type == ops::OpType::RecycleDelete ||
                               completed.type == ops::OpType::RealDelete) {
                        for (const auto& source : completed.sources)
                            s->places.RemoveAssignments(source, true);
                        s->duplicateScan.RemoveDeleted(completed.sources);
                    }
                    if (completed.type == ops::OpType::RecycleDelete ||
                        completed.type == ops::OpType::RealDelete ||
                        completed.type == ops::OpType::RestoreRecycle ||
                        completed.type == ops::OpType::EmptyRecycle) {
                        bool recycle_visible = false;
                        ForEachPane(*s, [&](app::Pane& pane) {
                            if (IsRecycleTab(pane.ActiveTab())) recycle_visible = true;
                        });
                        const auto n = static_cast<int64_t>(completed.sources.size());
                        ScheduleRecycleRefresh(*s);
                        if (completed.type == ops::OpType::RecycleDelete)
                            BumpRecycleOccupancy(*s, n);
                        else if (completed.type == ops::OpType::EmptyRecycle)
                            ClearRecycleOccupancy(*s);
                        else if (completed.type == ops::OpType::RestoreRecycle ||
                                 (completed.type == ops::OpType::RealDelete &&
                                  recycle_visible))
                            BumpRecycleOccupancy(*s, -n);
                        RefreshRecycleViews(*s, false);
                    }
                    RefreshStarredViews(*s);
                    RefreshRecentViews(*s);
                }
                if (!tag_metadata_paths.empty())
                    QueueTagAds(*s, BuildTagAdsUpdates(s->places, tag_metadata_paths, true));
                // An op finished: refresh the view (watcher also fires, this is immediate).
                app::Tab* tab = ActiveTab(*s);
                if (tab) {
                    s->store.MarkDirty(tab->current_path);
                    RefreshActiveTab(*s);
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_SHELLCTX_ITEMS: {
        auto* payload = reinterpret_cast<ShellCtxItemsPayload*>(lParam);
        const uint32_t token = static_cast<uint32_t>(wParam);
        if (s && payload) {
            const auto completed = s->context_menu.CompleteComQuery(
                token, std::move(payload->items), GetTickCount64(), payload->partial);
            if (completed.accepted) {
                bool prefs_changed = false;
                if (!completed.partial &&
                    s->ctxMenuPrefs.RecordComTiming(completed.cache_key,
                                                    completed.elapsed_ms))
                    prefs_changed = true;
                if (!completed.partial) {
                    for (const auto& clsid : payload->slow_clsids) {
                        if (s->ctxMenuPrefs.RecordComTiming(
                                ipc::HandlerCatalogKey(clsid), 1000))
                            prefs_changed = true;
                    }
                }
                if (prefs_changed) s->ctxMenuPrefs.Save();
                RefreshOpenCtxMenu(*s);
            } else if (!payload->partial) {
                s->ops.CloseShellMenu(token);
            }
        }
        delete payload;
        return 0;
    }

    case WM_SHELL_VERBS: {
        auto* result = reinterpret_cast<ShellVerbsResult*>(lParam);
        if (s && result) {
            if (s->context_menu.CompleteStaticVerbs(
                    result->ext, std::move(result->verbs))) {
                RefreshOpenCtxMenu(*s);
            }
        }
        delete result;
        return 0;
    }

    case WM_SHELL_CACHE_INVALIDATE: {
        if (s) {
            s->context_menu.InvalidateCaches();
            SeedShellVerbCache(*s);
        }
        return 0;
    }

    case WM_DETAILS_META: {
        auto* result = reinterpret_cast<DetailsMetaResult*>(lParam);
        if (s && result && result->path == s->detailsSelPath) {
            s->detailsSelValid = result->attrs_valid;
            if (result->attrs_valid) {
                s->detailsCreated = result->created;
                s->detailsModified = result->modified;
                s->detailsAccessed = result->accessed;
            }
            s->detailsTypeName = std::move(result->type_name);
            s->detailsMetaPath = result->path;
            s->detailsOwner = std::move(result->meta.owner);
            s->detailsPermissions = std::move(result->meta.permissions);
            s->detailsDrive = std::move(result->meta.drive);
            s->detailsFileSystem = std::move(result->meta.file_system);
            s->detailsFreeSpace = std::move(result->meta.free_space);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        delete result;
        return 0;
    }

    case WM_INDEX_NOTIFY: {
        if (s) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_NETWORK_INDEX_NOTIFY: {
        if (s) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_SETTINGS_TASK_RESULT: {
        auto* result = reinterpret_cast<app::SettingsTaskResult*>(lParam);
        if (s && result) {
            if (!result->ok && result->task.kind ==
                    app::SettingsTaskKind::DiagnosticsExport)
                result->error = l10n::Get(l10n::StringId::DiagnosticsExportFailed);
            const auto effect = s->settings.CompleteTask(*result, s->index.ServiceInstalled());
            if (!result->ok && result->task.kind == app::SettingsTaskKind::ConfigureIndexPath &&
                !result->error.empty()) {
                s->notification_toast.Show(hwnd, l10n::Get(l10n::StringId::IndexLocation), result->error);
            }
            if (effect.refresh_index) s->index.RefreshVolumesAsync();
            if (!effect.pin_network.empty()) {
                s->places.PinNetwork(effect.pin_network, L"");
                RequestUncProbe(*s, effect.pin_network);
            }
            if (!effect.open_path.empty()) {
                ShellExecuteW(hwnd, L"open", effect.open_path.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        delete result;
        return 0;
    }

    case WM_INDEX_SEARCH: {
        if (!s) return 0;
        const uint32_t id = static_cast<uint32_t>(wParam);
        index::SearchResult result;
        if (!s->index.TakeResult(id, result)) return 0;
        AcceptIndexProviderResult(*s, id, std::move(result), false);
        return 0;
    }

    case WM_NETWORK_INDEX_SEARCH: {
        if (!s) return 0;
        const uint32_t id = static_cast<uint32_t>(wParam);
        index::SearchResult result;
        if (!s->networkIndex.TakeResult(id, result)) return 0;
        AcceptIndexProviderResult(*s, id, std::move(result), true);
        return 0;
    }

    case WM_UPDATE_RESULT:
        if (s) CompleteUpdateCheck(*s);
        return 0;
    case WM_UPDATE_DOWNLOADED:
        if (s) CompleteUpdateDownload(*s);
        return 0;
    case WM_UPDATE_INSTALL:
        if (s) InstallUpdate(*s);
        return 0;

    case WM_CONTENT_SEARCH: {
        if (!s) return 0;
        index::ContentSearchUpdate update;
        while (s->contentSearch.TakeUpdate(update))
            ApplyContentSearchUpdate(*s, std::move(update));
        return 0;
    }

    case WM_DUPLICATE_SCAN: {
        if (!s) return 0;
        index::ContentSearchUpdate update;
        while (s->duplicateSearch.TakeUpdate(update))
            s->duplicateScan.ApplyUpdate(update.progress, update.hits);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_DUP_VOLUMES: {
        auto* payload = reinterpret_cast<std::vector<index::VolumeInfo>*>(lParam);
        if (s && payload) ApplyDuplicateVolumeCache(*s, std::move(*payload));
        delete payload;
        return 0;
    }

    case WM_QUICK_PREVIEW_NAVIGATE:
        if (s) NavigateQuickPreview(*s, static_cast<int>(wParam));
        return 0;

    case WM_QUICK_PREVIEW_OPEN:
        if (s) OpenSelected(*s);
        return 0;

    case WM_NET_PROBE: {
        auto* result = reinterpret_cast<fs::UncProbeResult*>(lParam);
        if (s && result) {
            s->probeBusy = false;
            s->places.SetNetworkStatus(result->unc, result->status, result->rtt_ms);
            if (result->status == fs::NetStatus::Offline) {
                ForEachPane(*s, [&](app::Pane& pane) {
                    app::Tab* tab = pane.ActiveTab();
                    if (!tab || tab->current_path != result->unc) return;
                    if (tab->snapshot) {
                        tab->net_readonly = true;
                        tab->banner_title = L"离线";
                        tab->banner_message = L"只读浏览上次快照";
                    }
                });
            }
            PumpUncProbe(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        delete result;
        return 0;
    }

    case WM_TAG_ADS_WARNING: {
        std::unique_ptr<std::vector<std::wstring>> volumes(
            reinterpret_cast<std::vector<std::wstring>*>(lParam));
        if (s && volumes) {
            for (const auto& volume : *volumes) {
                const std::wstring shown = ClipboardPath(volume);
                if (!s->tagFallbackVolumes.insert(shown).second) continue;
                if (app::Tab* tab = ActiveTab(*s)) {
                    tab->banner_title = L"标签已保存在本机";
                    tab->banner_message = shown
                        + L" 不支持文件标签元数据；换电脑后可能不可见。";
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_TAG_ADS_DISCOVERED: {
        std::unique_ptr<std::vector<TagAdsDiscovery>> discoveries(
            reinterpret_cast<std::vector<TagAdsDiscovery>*>(lParam));
        if (s && discoveries) {
            const uint64_t before = s->places.TagRevision();
            for (const auto& discovery : *discoveries) {
                const std::wstring key = TagDiscoveryKey(discovery.path);
                s->tagAdsDiscoveryQueued.erase(key);
                s->tagAdsDiscoveryChecked.insert(key);
                s->places.MergeAdsRecords(discovery.path, discovery.records,
                                          discovery.legacy_names);
            }
            if (s->places.TagRevision() != before)
                InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_DESTROY: {
        if (s) {
            StopShellRegistryWatch();
            s->watches.Stop();
            s->settings.ResetUi();
            s->settings.Stop();
            s->update_checker.Stop();
            s->update_installer.Stop();
            s->contentSearch.Stop();
            s->duplicateSearch.Stop();
            s->networkIndex.Stop();
            s->index.Stop();
            s->worker.Stop();
            ShutdownDetailsSizeWalk(*s);

            if (s->dropTarget) {
                RevokeDragDrop(hwnd);
                s->dropTarget->Release();
                s->dropTarget = nullptr;
            }
            s->menu.reset();
            s->operationWindow.reset();
            if (s->hwndAddressEdit) {
                DestroyWindow(s->hwndAddressEdit);
                s->hwndAddressEdit = nullptr;
            }
            if (s->hwndRenameEdit) {
                DestroyWindow(s->hwndRenameEdit);
                s->hwndRenameEdit = nullptr;
            }
            if (s->hwndTagRenameEdit) {
                DestroyWindow(s->hwndTagRenameEdit);
                s->hwndTagRenameEdit = nullptr;
            }
            if (s->hwndFilterEdit) {
                DestroyWindow(s->hwndFilterEdit);
                s->hwndFilterEdit = nullptr;
            }
            if (s->editFont) {
                DeleteObject(s->editFont);
                s->editFont = nullptr;
            }
            if (s->editBrush) {
                DeleteObject(s->editBrush);
                s->editBrush = nullptr;
            }
            // Visual-regression runs must never overwrite the user's real
            // window, path, tray, or undo session.
            if (!s->shot.active && !s->menushot && !s->isolatedTest) {
                app::SessionSnapshot snap;
                WINDOWPLACEMENT wp{ sizeof(wp) };
                if (GetWindowPlacement(hwnd, &wp)) {
                    snap.window_rect = wp.rcNormalPosition;
                    snap.maximized = (wp.showCmd == SW_SHOWMAXIMIZED);
                }
                snap.dark = s->darkMode;
                app::Tab* tab = ActiveTab(*s);
                if (tab) snap.active_path = tab->current_path;
                RememberLayoutFocus(*s);
                snap.active_layout_tab = static_cast<int>(s->window_tabs.active);
                for (const auto& group : s->window_tabs.tab_groups)
                    snap.tab_groups.push_back(
                        {group.id, group.name, group.color_rgb, group.collapsed});
                for (const auto& layout : s->window_tabs.items)
                    snap.layout_tabs.push_back(app::CaptureLayoutTab(*layout));
                snap.tray = s->tray;
                snap.undo_json = s->ops.UndoToJson();
                snap.sidebar_collapsed = static_cast<int>(s->sidebarCollapsedMask);
                snap.starred_expanded = s->starredExpanded;
                snap.details_panel = s->showDetailsPanel;
                snap.details_panel_width = static_cast<int>(std::lround(s->detailsPanelWidth));
                app::SaveSession(snap);
                s->places.Save();
                s->ctxMenuPrefs.Save();
                s->appPrefs.Save();
            }

            s->tray_controller.Detach();
            s->ops.Stop();
            s->single_instance.Release();

            s->renderer.SetIconNotifyWindow(nullptr);
            s->renderer.SetCompositor(nullptr);
            s->compositor.Shutdown();
            s->hwnd = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    __try {
        return WndProcImpl(hwnd, msg, wParam, lParam);
    } __except (pulse::crash::AddBreadcrumb(1, msg, static_cast<int32_t>(GetExceptionCode())),
                pulse::crash::ReportFatal(GetExceptionInformation(), "wndproc")) {
        TerminateProcess(GetCurrentProcess(), GetExceptionCode());
        return 0;
    }
}

bool WaitForShotReady(AppState& s) {
    auto deadline = s.shot.start + std::chrono::seconds(5);
    MSG msg{};
    while (std::chrono::steady_clock::now() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        ProcessPendingResults(s);
        app::Tab* tab = ActiveTab(s);
        if (tab && !tab->loading && tab->snapshot) {
            return true;
        }
        if (s.hwnd) {
            RedrawWindow(s.hwnd, nullptr, nullptr, RDW_UPDATENOW | RDW_INTERNALPAINT);
        }
        Sleep(20);
    }
    return false;
}

// Staged tag states for GUI-verification shots. Kept out of ShotModeMain's
// SEH frame, which forbids C++ locals with destructors.
void StageTagShotStates(AppState& state) {
    if (state.shot_tag_rename && !state.places.tags.empty()) {
        // Point the rename state at a real loaded tag (read-only): the
        // renderer draws the Fluent field frame on that row.
        state.tagRenameId =
            state.places.tags[1 % state.places.tags.size()].id;
    }
    if (state.shot_tag_drag && state.places.tags.size() >= 4) {
        // Stage a mid-drag frame at slot 0 (the regression case): the third
        // tag floats at the list top; the first slides down mid-flight.
        ui::WindowViewModel vm0 = BuildVm(state);
        int g0 = -1;
        for (int g = 0; g < static_cast<int>(vm0.sidebar.size()); ++g) {
            if (!vm0.sidebar[g].items.empty() && vm0.sidebar[g].items[0].is_tag) {
                g0 = g;
                break;
            }
        }
        D2D1_RECT_F a{}, b{};
        const bool okA = g0 >= 0 &&
            state.renderer.TagItemRect(vm0, static_cast<float>(state.compositor.Width()),
                                       static_cast<float>(state.compositor.Height()),
                                       g0, 0, &a);
        const bool okB = g0 >= 0 &&
            state.renderer.TagItemRect(vm0, static_cast<float>(state.compositor.Width()),
                                       static_cast<float>(state.compositor.Height()),
                                       g0, 1, &b);
        if (okA && okB) {
            const float pitch = b.top - a.top;
            state.tagDragActive = true;
            state.tagDragTag = 2;
            state.tagOrder.resize(state.places.tags.size());
            for (int i = 0; i < static_cast<int>(state.tagOrder.size()); ++i)
                state.tagOrder[static_cast<size_t>(i)] = i;
            state.tagOrder[0] = 2;
            state.tagOrder[1] = 0;
            state.tagOrder[2] = 1;
            state.tagDragY = a.top + pitch * 0.5f;
            state.tagOffsets[state.places.tags[0].name] = -0.5f;
            state.tagGapVisible = true;
            state.tagGapLineY = state.tagGapFrom = state.tagGapTo = a.top;
        }
    }
}

// No C++ objects with destructors here: SEH (__try/__except) forbids unwinding.
int ShotModeMain(AppState& state, HWND hwnd) {
    bool ok = false;
    __try {
        WaitForShotReady(state);
        StageTagShotStates(state);
        if (state.shot_details && state.pane && state.pane->ActiveTab() &&
            state.pane->ActiveTab()->snapshot &&
            !state.pane->ActiveTab()->snapshot->empty()) {
            // Select rows after enumeration so the panel has deterministic data.
            state.pane->ActiveTab()->SelectOnly(0);
            if (state.shot_details_multi) {
                const int count = static_cast<int>(state.pane->ActiveTab()->snapshot->size());
                for (int i = 1; i < std::min(3, count); ++i)
                    state.pane->ActiveTab()->ToggleSelect(i);
            }
        }
        MSG msg{};
        for (int i = 0; i < 20; ++i) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(40);
        }
        Render(state);
        ok = state.compositor.SaveSnapshot(state.shot.output.c_str());
    } __except (pulse::crash::ReportFatal(GetExceptionInformation(), "shot")) {
        return 2;
    }
    if (ok) {
        // Drop timing numbers next to the shot for the phase report.
        wchar_t timingPath[MAX_PATH]{};
        wcsncpy_s(timingPath, state.shot.output.c_str(), _TRUNCATE);
        if (wchar_t* dot = wcsrchr(timingPath, L'.')) *dot = L'\0';
        wcscat_s(timingPath, L".timing.txt");
        if (HANDLE f = CreateFileW(timingPath, GENERIC_WRITE, 0, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); f != INVALID_HANDLE_VALUE) {
            char buf[256];
            int n = snprintf(buf, sizeof(buf),
                "path=%ls\nfirst_frame_ms=%.2f\nenum_sort_done_ms=%.2f\n",
                state.pane && state.pane->ActiveTab() ? state.pane->ActiveTab()->current_path.c_str() : L"",
                state.timing.first_frame_ms, state.timing.sort_done_ms);
            if (n > 0) {
                DWORD written = 0;
                WriteFile(f, buf, (DWORD)n, &written, nullptr);
            }
            CloseHandle(f);
        }
    }
    DestroyWindow(hwnd);
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return ok ? 0 : 1;
}

bool SkipSingletonFromArgv() {
    for (int i = 1; i < __argc; ++i) {
        if (wcscmp(__wargv[i], L"--selftest") == 0 ||
            wcscmp(__wargv[i], L"--material-selftest") == 0 ||
            wcscmp(__wargv[i], L"--seed-shell-verbs") == 0 ||
            wcscmp(__wargv[i], L"--shot") == 0 ||
            wcscmp(__wargv[i], L"--menushot") == 0 ||
            wcscmp(__wargv[i], L"--test-instance") == 0 ||
            wcscmp(__wargv[i], L"--colorpickshot") == 0 ||
            wcscmp(__wargv[i], L"--colorpickdialog") == 0)
            return true;
    }
    return false;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    pulse::crash::Initialize({pulse::crash::ProcessRole::App, false, {}});
    pulse::compat::EnableDpiAwareness();
    // OLE init (drag & drop + clipboard); implies STA COM init.
    OleInitialize(nullptr);
    for (int i = 1; i < __argc; ++i) {
        if (wcscmp(__wargv[i], L"--material-selftest") == 0) {
            const int rc = ui::RunMaterialSelfTest();
            OleUninitialize();
            return rc;
        }
        if (wcscmp(__wargv[i], L"--seed-shell-verbs") == 0) {
            const int rc = app::SeedMachineStaticVerbCache() ? 0 : 1;
            OleUninitialize();
            return rc;
        }
    }

    AppState state;
    state.safeMode = pulse::crash::SafeModeRequested();
    for (int i = 1; i < __argc; ++i)
        if (wcscmp(__wargv[i], L"--test-instance") == 0) state.isolatedTest = true;

#ifdef PULSE_WITH_SELFTEST
    // Optional headless suite: excluded from production builds together with
    // the in-process index engine it exercises.
    for (int i = 1; i < __argc; ++i) {
        if (wcscmp(__wargv[i], L"--selftest") == 0) {
            int rc = app::RunSelfTest1B2();
            OleUninitialize();
            return rc;
        }
    }
#endif

    // Load previous session before parsing overrides.
    app::SessionSnapshot session;
    if (!state.isolatedTest && app::LoadSession(session)) {
        state.session_path = session.active_path;
        state.session_layout_tabs = std::move(session.layout_tabs);
        state.session_tab_groups = std::move(session.tab_groups);
        state.session_active_layout_tab = session.active_layout_tab;
        state.tray = session.tray;
        state.darkMode = session.dark;
        state.sidebarCollapsedMask = static_cast<uint32_t>(session.sidebar_collapsed);
        state.starredExpanded = session.starred_expanded;
        state.pending_undo_json = session.undo_json;
        state.showDetailsPanel = session.details_panel;
        state.detailsPanelWidth = static_cast<float>(session.details_panel_width);
        state.renderer.SetDetailsPanelWidth(state.detailsPanelWidth);
        if (state.darkMode) state.themeOverride = ui::ThemeMode::Dark;
    }

    // Parse command line.
    for (int i = 1; i < __argc; ++i) {
        if (wcscmp(__wargv[i], L"--shot") == 0 && i + 1 < __argc) {
            state.shot.active = true;
            state.shot.output = __wargv[++i];
        } else if (wcscmp(__wargv[i], L"--menushot") == 0 && i + 1 < __argc) {
            state.menushot = true;
            state.menushot_out = __wargv[++i];
        } else if (wcscmp(__wargv[i], L"--menushot-search") == 0) {
            state.menushot_search = true;
        } else if (wcscmp(__wargv[i], L"--colorpickshot") == 0 && i + 1 < __argc) {
            state.colorpickshot = true;
            state.colorpickshot_out = __wargv[++i];
            if (i + 1 < __argc && wcscmp(__wargv[i + 1], L"light") == 0) {
                state.colorpickshot_dark = false;
                ++i;
            }
        } else if (wcscmp(__wargv[i], L"--colorpickdialog") == 0) {
            state.colorpickdialog = true;
        } else if (wcscmp(__wargv[i], L"--shot-tray") == 0) {
            state.shot_tray = true;
            // Optional item count: --shot-tray 1 stages a single file.
            if (i + 1 < __argc && __wargv[i + 1][0] >= L'0' && __wargv[i + 1][0] <= L'9')
                state.shot_tray_count = std::max(1, _wtoi(__wargv[++i]));
        } else if (wcscmp(__wargv[i], L"--shot-tab-colors") == 0) {
            state.shot_tab_colors = true;
        } else if (wcscmp(__wargv[i], L"--shot-pinned-tab") == 0) {
            state.shot_pinned_tab = true;
        } else if (wcscmp(__wargv[i], L"--shot-tag-rename") == 0) {
            state.shot_tag_rename = true;
        } else if (wcscmp(__wargv[i], L"--shot-tag-drag") == 0) {
            state.shot_tag_drag = true;
        } else if (wcscmp(__wargv[i], L"--shot-details") == 0) {
            state.shot_details = true;
        } else if (wcscmp(__wargv[i], L"--shot-details-multi") == 0) {
            state.shot_details = true;
            state.shot_details_multi = true;
        } else if (wcscmp(__wargv[i], L"--shot-scale") == 0 && i + 1 < __argc) {
            state.shot_scale_override = std::clamp(
                static_cast<float>(_wtof(__wargv[++i])), 1.0f, 2.5f);
        } else if (wcscmp(__wargv[i], L"--shot-language") == 0 && i + 1 < __argc) {
            state.shot.language = __wargv[++i];
        } else if (wcscmp(__wargv[i], L"--shot-update-available") == 0) {
            state.shot.update_available = true;
        } else if (wcscmp(__wargv[i], L"--shot-update-state") == 0 && i + 1 < __argc) {
            state.shot.update_available = true;
            state.shot.update_state = __wargv[++i];
        } else if (wcscmp(__wargv[i], L"--shot-high-contrast") == 0) {
            state.shot_high_contrast = true;
        } else if (wcscmp(__wargv[i], L"--dark") == 0) {
            state.shot.force_dark = true;
            state.themeOverride = ui::ThemeMode::Dark;
        } else if (wcscmp(__wargv[i], L"--light") == 0) {
            state.shot.force_dark = false;
            state.themeOverride = ui::ThemeMode::Light;
        } else if (wcscmp(__wargv[i], L"--test-instance") == 0) {
            continue;
        } else if (wcscmp(__wargv[i], L"--fps") == 0) {
            state.forceStatusPerformance = true;
            state.showFps = true;
        } else if (wcscmp(__wargv[i], L"--view") == 0 && i + 1 < __argc) {
            state.shot.view_mode = ui::ParseViewMode(__wargv[++i]);
        } else if (wcscmp(__wargv[i], L"--size") == 0 && i + 1 < __argc) {
            int requestedWidth = 0;
            int requestedHeight = 0;
            if (swscanf_s(__wargv[++i], L"%dx%d", &requestedWidth, &requestedHeight) == 2) {
                state.shot.width = std::max(320, requestedWidth);
                state.shot.height = std::max(240, requestedHeight);
            }
        } else if (i == __argc - 1) {
            state.shot.path = __wargv[i];
        } else if (__wargv[i][0] != L'-' && state.shot.path.empty()) {
            state.shot.path = __wargv[i]; // tolerate path not being the last argument
        }
    }
    if (state.shot.active && state.shot.path.empty()) {
        state.shot.path = L"C:\\";
    }
    if (!state.shot.active && !state.menushot && !state.colorpickshot &&
        !state.colorpickdialog) {
        state.open_path = state.shot.path;
        state.shot.path.clear();
    }
    state.shot.start = std::chrono::steady_clock::now();

    if (!SkipSingletonFromArgv()) {
        const auto result = state.single_instance.Acquire();
        if (result == app::SingleInstanceCoordinator::AcquireResult::Existing) {
            state.single_instance.ForwardOpenPath(state.open_path);
            OleUninitialize();
            return 0;
        }
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_PULSE));
    wc.hIconSm = reinterpret_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_PULSE), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = app::SingleInstanceCoordinator::WindowClassName();
    RegisterClassExW(&wc);

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = (int)(1600 * state.scale), h = (int)(960 * state.scale);
    if (session.window_rect.right > session.window_rect.left) {
        x = session.window_rect.left;
        y = session.window_rect.top;
        w = session.window_rect.right - session.window_rect.left;
        h = session.window_rect.bottom - session.window_rect.top;
    }
    if (state.shot.active && state.shot.width > 0 && state.shot.height > 0) {
        w = state.shot.width;
        h = state.shot.height;
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName,
        L"Pulse",
        // Pulse paints the entire title bar. WS_POPUP prevents Win32 from
        // restoring an overlapped caption, while the remaining styles retain
        // resizing, the system menu, min/max and Snap Layout behavior.
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        x, y, w, h,
        nullptr, nullptr, hInstance, &state);

    if (!hwnd) return 1;
    if (wc.hIcon) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
    if (wc.hIconSm) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(wc.hIconSm));

    if (session.maximized) nCmdShow = SW_SHOWMAXIMIZED;
    wchar_t hidden_shot[4]{};
    const bool test_hidden = (state.shot.active || state.menushot || state.colorpickshot) &&
        GetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", hidden_shot, ARRAYSIZE(hidden_shot)) == 1 &&
        hidden_shot[0] == L'1';
    ShowWindow(hwnd, test_hidden ? SW_HIDE : state.shot.active ? SW_SHOWNORMAL : nCmdShow);
    UpdateWindow(hwnd);

    if (state.menushot) {
        // Render the built-in item menu to a PNG (GUI verification for 1B-2).
        ui::FluentMenu m;
        bool ok = m.Create(hwnd, &state.compositor, state.scale);
        if (ok && state.menushot_search && state.pane && state.pane->ActiveTab()) {
            // BuildFinderItemMenu only checks current_path for the search bit.
            state.pane->ActiveTab()->current_path = L"pulse:search:widgets.json";
        }
        if (ok) {
            m.SetTheme(state.darkMode, state.accentColor);
            auto debug_items = BuildFinderItemMenu(state, true, L"撤销移动 a.txt");
            wchar_t quick_menu[4]{};
            if (GetEnvironmentVariableW(L"PULSE_TEST_QUICK_MENU", quick_menu, ARRAYSIZE(quick_menu)) == 1 &&
                quick_menu[0] == L'1') {
                debug_items = app::BuildBackgroundMenu(false, true, L"撤销移动 a.txt");
                app::AppendBackgroundViewCommands(debug_items, {});
                AppendQuickAccessCommand(state, debug_items,
                    state.shot.path.empty() ? QuickAccessTargets(ActiveTab(state), true)
                                            : std::vector<std::wstring>{state.shot.path});
            }
            for (auto& item : debug_items) {
                if (item.quick_swatches.empty()) continue;
                item.quick_swatches.front().checked = true;
                if (item.quick_swatches.size() > 1) item.quick_swatches[1].mixed = true;
                break;
            }
            // Sample Explorer section: a flat verb plus a software-owned
            // flyout group (renders the › chevron in the shortcut column).
            app::ShellMenuEntry group;
            group.text = L"Bandizip";
            group.children = { { app::CmdShellComBase + 1, L"压缩为 zip", true },
                               { app::CmdShellComBase + 2, L"用 Bandizip 打开", true } };
            app::AppendShellSection(debug_items,
                { { app::CmdShellStaticBase + 0, L"打印", true }, group });
            ok = m.SaveDebugSnapshot(state.menushot_out.c_str(), std::move(debug_items));
        }
        DestroyWindow(hwnd);
        OleUninitialize();
        return ok ? 0 : 1;
    }

    if (state.colorpickshot) {
        // Render the color picker popup to a PNG (GUI verification).
        const bool ok = ui::ColorPickerPopup::SaveDebugSnapshot(
            &state.compositor, state.colorpickshot_out.c_str(),
            state.colorpickshot_dark, state.accentColor, state.scale);
        DestroyWindow(hwnd);
        OleUninitialize();
        return ok ? 0 : 1;
    }

    if (state.colorpickdialog) {
        // Interactive smoke test: open the picker centered on the work area.
        uint32_t rgb = 0x2FDFF1;
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        POINT pt{ (work.left + work.right) / 2, (work.top + work.bottom) / 2 };
        ui::ColorPickerPopup::Pick(hwnd, &state.compositor, nullptr, state.scale, pt,
                                   rgb, state.darkMode, rgb);
        DestroyWindow(hwnd);
        OleUninitialize();
        return 0;
    }

    if (state.shot.active) {
        if (state.shot_tray) {
            // Stage a few real files from the shot folder so the deck is
            // visible in the verification screenshot. Drop any restored
            // session tray first so the shot is deterministic.
            state.tray.Clear();
            std::vector<std::wstring> staged;
            const std::wstring root = fs::NormalizePath(state.shot.path);
            WIN32_FIND_DATAW fd{};
            HANDLE hFind = FindFirstFileW((root + L"\\*").c_str(), &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
                    if (fd.cFileName[0] == L'.') continue;
                    staged.push_back(root + L"\\" + fd.cFileName);
                    if (staged.size() >= static_cast<size_t>(state.shot_tray_count)) break;
                } while (FindNextFileW(hFind, &fd));
                FindClose(hFind);
            }
            if (!staged.empty()) state.tray.Collect(staged, false);
        }
        if (state.shot_tab_colors && state.pane) {
            NewTab(state, state.shot.path);
            NewTab(state, state.shot.path);
            if (state.window_tabs.items.size() >= 3) {
                // Group 1: tabs 0-1, red, named. Group 2: tab 2, blue, unnamed.
                app::TabGroup g1; g1.id = 1; g1.name = L"设计"; g1.color_rgb = 0xE74856;
                app::TabGroup g2; g2.id = 2; g2.color_rgb = 0x0078D4;
                state.window_tabs.tab_groups.push_back(g1);
                state.window_tabs.tab_groups.push_back(g2);
                state.window_tabs.next_tab_group_id = 3;
                state.window_tabs.items[0]->tab_group = 1;
                state.window_tabs.items[1]->tab_group = 1;
                state.window_tabs.items[2]->tab_group = 2;
                SwitchTab(state, 1); // active grouped tab in the middle
            }
        }
        if (state.shot_pinned_tab && !state.window_tabs.items.empty()) {
            state.window_tabs.items[state.window_tabs.active]->pinned = true;
        }
        if (state.shot_details) {
            state.showDetailsPanel = true;
            state.renderer.SetDetailsPanelVisible(true);
        }
        int rc = ShotModeMain(state, hwnd);
        OleUninitialize();
        return rc;
    }

    MSG msg{};
    BOOL ret;
    while ((ret = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (ret == -1) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    OleUninitialize();
    return (int)msg.wParam;
}

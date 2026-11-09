#include "window_management_test.h"
#include "app_internal.h"
#include "explorer_shortcuts.h"
#include "keymap_input.h"
#include "open_routing.h"
#include "external_open_dispatch.h"
#include "shell_window_sync.h"
#include "global_search_controller.h"
#include "global_search_routing_test_support.h"
#include "app_runtime.h"
#include "window_arrange_ui.h"
#include "window_overview_ui.h"
#include "window_management_ui.h"
#include "window_factory.h"
#include "app_input.h"
#include "app_navigation.h"
#include "workspace_path.h"
#include "tray_reveal.h"
#include "../common/utf8_file.h"
#include "../ui/ui_renderer.h"
#include "../ui/view_layout.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <objbase.h>

namespace pulse::ui {
struct ItemGripTest {
    static void Paint(MainRenderer& renderer, const PaneViewModel& view, const D2D1_RECT_F& bounds, const Theme& theme) {
        const auto list=renderer.PaneListRect(view,bounds);
        renderer.DrawList(view,list.left,list.top,list.right-list.left,list.bottom-list.top,theme,0,-1,true,-1);
    }
    static void Remember(MainRenderer& renderer, const PaneViewModel& view, const D2D1_RECT_F& bounds,
                         D2D1_RECT_F artwork) {
        const auto& entry=view.entries.at(0);
        renderer.item_artwork_hits_.Remember({view.view_generation,0,static_cast<int>(view.view_mode),
            entry.path,entry.name,entry.modified_value,entry.size_value,renderer.ItemRectInPane(view,bounds,0)},artwork);
    }
};
}

namespace pulse {
namespace {
struct Report {
    FILE *file = nullptr;
    int failed = 0;
    void Check(bool ok, const char *name) {
        std::fprintf(file, "[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok)
            ++failed;
        std::fflush(file);
    }
    void Skip(const char *name) {
        std::fprintf(file, "[SKIP] %s: work area cannot fit minimum window sizes\n", name);
    }
};
RECT Bounds(HWND hwnd) {
    RECT r{};
    GetWindowRect(hwnd, &r);
    return r;
}
bool Same(RECT a, RECT b) {
    return EqualRect(&a, &b) != FALSE;
}
std::vector<HWND> Windows(AppState &s) {
    std::vector<HWND> result;
    s.core.windows.ForEach([&](HWND hwnd, AppState &) { result.push_back(hwnd); });
    return result;
}
void Undo(AppState &s) {
    UndoWindowArrange(s, WindowArrangeGeneration());
}
void CheckExplorerRollback(AppState& s, Report& log) {
    wchar_t directory[32768]{};
    GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", directory, ARRAYSIZE(directory));
    const auto folder = std::filesystem::path(directory) / L"explorer-rollback";
    std::filesystem::create_directories(folder);
    FILE* fixture = nullptr;
    _wfopen_s(&fixture, (folder / L"selected.txt").c_str(), L"wb");
    if (fixture) { std::fputs("rollback fixture", fixture); std::fclose(fixture); }
    log.Check(fixture != nullptr, "rollback fixture created in isolated data directory");
    const auto pump_until = [](auto ready, uint64_t deadline) {
        while (!ready() && GetTickCount64() < deadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); return false; }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
        return ready();
    };
    OpenTabAt(s, folder.wstring());
    auto* old_folder = ActiveTab(s);
    const bool loaded = old_folder && pump_until([&] { return !old_folder->loading && old_folder->snapshot && !old_folder->pending_generation; }, GetTickCount64() + 15000);
    log.Check(loaded, "existing folder enumerated before rollback cases");
    if (!loaded) return;
    SelectNameInTab(s, *old_folder, L"selected.txt");
    const auto selection = old_folder->SelectedIndices();
    const auto revision = old_folder->selection_revision;
    const auto snapshot = old_folder->snapshot;
    OpenSettingsTab(s, 0);
    const auto* settings = s.window_tabs.Active();
    const auto count = s.window_tabs.items.size();
    for (int mode = 0; mode != 3; ++mode) {
        app::ExplorerOpenContext request;
        request.folder = folder.wstring();
        request.names = {L"selected.txt"};
        request.selection_read = true;
        request.handoff = std::make_shared<app::ExplorerHandoff>(GetTickCount64() + 8000);
        log.Check(request.handoff->Receive(GetTickCount64()), "rollback request received exactly once");
        const auto dispatched = HandleExplorerTakeover(s, request);
        log.Check(dispatched.status == ExternalOpenStatus::Dispatched && dispatched.target == &s &&
            s.window_tabs.items.size() == count + 1 && s.window_tabs.Active() == settings,
            "dispatched background page preserves existing active settings");
        const bool prepared = pump_until([&] { return request.handoff->state != app::HandoffState::Pending; }, request.handoff->deadline_tick);
        log.Check(prepared && request.handoff->state == app::HandoffState::Ready,
            "real background enumeration and selection prepared before failure injection");
        if (mode == 0) request.handoff->Cancel();
        else if (mode == 1) {
            pump_until([&] { return GetTickCount64() >= request.handoff->deadline_tick; }, request.handoff->deadline_tick + 100);
            TickExplorerPresentation(s);
            log.Check(request.handoff->state == app::HandoffState::Expired, "real deadline expires dispatched background page");
        } else {
            const bool claimed = request.handoff->ClaimClose(GetTickCount64());
            log.Check(claimed, "Quit failure case claims a genuinely prepared transfer");
            if (claimed) {
                const auto pending_index = s.window_tabs.items.size() - 1;
                CloseLayoutTab(s, pending_index);
                auto moved = s.window_tabs.TakeTab(pending_index);
                log.Check(s.window_tabs.items.size() == count + 1 && !moved,
                    "Closing page cannot be closed or detached before native outcome");
                request.handoff->FailClose();
            }
        }
        TickExplorerPresentation(s);
        log.Check(s.window_tabs.items.size() == count && s.window_tabs.Active() == settings,
            mode == 0 ? "cancel rolls back only new page and retains settings" : mode == 1 ?
            "expiry rolls back only new page and retains settings" : "Quit failure rolls back only new page and retains settings");
        log.Check(old_folder->snapshot == snapshot && old_folder->selection_revision == revision &&
            old_folder->SelectedIndices() == selection, "rollback preserves preexisting folder listing and selection");
    }
}
void CheckItemGrip(AppState& s, Report& log);
void CheckGlobalSearchRouting(AppState& s, Report& log) {
    const auto prefs = s.appPrefs;
    s.appPrefs.persist = false;
    s.appPrefs.external_open = 1;
    s.appPrefs.open_folders_in_pulse = false;
    s.appPrefs.integration_enabled = true;
    s.appPrefs.integration_folders = true;
    wchar_t data[32768]{};
    GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data, ARRAYSIZE(data));
    const auto root = std::filesystem::path(data) / L"global-search-routing";
    const auto folder = root / L"folder 中文";
    std::filesystem::create_directories(folder);
    const auto file = root / L"定位 文件.txt";
    FILE* fixture = nullptr;
    _wfopen_s(&fixture, file.c_str(), L"wb");
    if (fixture) { std::fputs("global search routing fixture", fixture); std::fclose(fixture); }
    log.Check(fixture != nullptr, "create isolated file reveal fixture");
    auto events = MakeGlobalSearchEvents(s);
    GlobalSearchWindow popup;
    popup.SetEvents(events);
    const auto explorer_before = ExplorerRoutingSnapshot::Read();
    std::fprintf(log.file, "[DIAG] Explorer baseline frames=%zu locations=%zu complete=%d unreadable_shell_entries=%u\n",
        explorer_before.frames.size(), explorer_before.locations.size(), explorer_before.complete ? 1 : 0,
        explorer_before.unreadable_shell_entries);
    log.Check(explorer_before.complete, "read Explorer frame and navigation baseline");
    bool explorer_unchanged = explorer_before.complete;
    auto observe_explorer = [&] {
        const auto current = ExplorerRoutingSnapshot::Read();
        if (!current.SameAs(explorer_before)) {
            explorer_unchanged = false;
            std::fprintf(log.file, "[DIAG] Explorer snapshot changed: frames=%zu baseline=%zu complete=%d\n",
                current.frames.size(), explorer_before.frames.size(), current.complete ? 1 : 0);
            for (const auto& [identity, url] : current.locations)
                std::fprintf(log.file, "[DIAG] Explorer hwnd=%p index=%ld URL=%ls\n",
                    static_cast<void*>(identity.first), identity.second, url.c_str());
        }
    };
    auto open_result = [&](const std::filesystem::path& target, bool directory, bool location) {
        GlobalSearchContext context;
        context.show_apps = false;
        context.recent_files = {{target.wstring(), directory}};
        popup.SetContext(std::move(context));
        const bool shown = popup.Show(s.hwnd, s.darkMode, s.scale, L"");
        log.Check(shown, "create real quick-search popup with isolated result");
        const HWND input = GetFocus();
        DWORD input_process = 0;
        const DWORD input_thread = GetWindowThreadProcessId(input, &input_process);
        wchar_t input_class[128]{};
        GetClassNameW(input, input_class, ARRAYSIZE(input_class));
        const bool owned_input = shown && input_thread == GetCurrentThreadId() &&
            input_process == GetCurrentProcessId() && _wcsicmp(input_class, L"Edit") == 0;
        log.Check(owned_input, "quick-search input belongs to this isolated test thread");
        BYTE saved_keys[256]{};
        if (owned_input && GetKeyboardState(saved_keys)) {
            BYTE keys[256]{};
            if (location) keys[VK_CONTROL] = 0x80;
            if (SetKeyboardState(keys)) {
                SendMessageW(input, WM_KEYDOWN, VK_DOWN, 0);
                SendMessageW(input, WM_KEYDOWN, VK_RETURN, 0);
                log.Check(SetKeyboardState(saved_keys) != FALSE, "restore test-thread keyboard state");
            } else log.Check(false, "set test-thread result action modifiers");
        } else log.Check(false, "read test-thread keyboard state");
        observe_explorer();
    };
    auto wait_for = [&](const std::wstring& expected, bool selected) {
        const auto normalized = fs::NormalizePath(expected);
        const auto deadline = GetTickCount64() + 5000;
        do {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            bool found = false;
            s.core.windows.ForEach([&](HWND hwnd, AppState& window) {
                const auto* tab = ActiveTab(window);
                if (!tab || tab->loading || !IsWindowVisible(hwnd) || GetForegroundWindow() != hwnd) return;
                if (!selected) found |= tab->current_path == normalized;
                else {
                    const auto paths = SelectedFullPaths(*tab);
                    found |= tab->current_path == fs::NormalizePath(std::filesystem::path(expected).parent_path().wstring()) &&
                        paths.size() == 1 && paths.front() == normalized;
                }
            });
            observe_explorer();
            if (found) return true;
            Sleep(10);
        } while (GetTickCount64() < deadline);
        std::fprintf(log.file, "[DIAG] expected=%ls selected=%d\n", normalized.c_str(), selected ? 1 : 0);
        s.core.windows.ForEach([&](HWND hwnd, AppState& window) {
            if (const auto* tab = ActiveTab(window)) {
                std::fprintf(log.file, "[DIAG] tab=%ls loading=%d visible=%d\n",
                    tab->current_path.c_str(), tab->loading ? 1 : 0, IsWindowVisible(hwnd) ? 1 : 0);
                for (const auto& path : SelectedFullPaths(*tab))
                    std::fprintf(log.file, "[DIAG] selected=%ls\n", path.c_str());
            }
        });
        return false;
    };
    open_result(folder, true, false);
    log.Check(wait_for(folder.wstring(), false), "real popup directory Open reaches its full Pulse path");
    const auto windows_before_reuse = Windows(s).size();
    const HWND original_target_window = GetForegroundWindow();
    const void* original_target_tab = nullptr;
    s.core.windows.ForEach([&](HWND hwnd, AppState& window) {
        if (hwnd == original_target_window) original_target_tab = ActiveTab(window);
    });
    open_result(folder, true, false);
    const bool reused_path = wait_for(folder.wstring(), false);
    const void* reused_tab = nullptr;
    s.core.windows.ForEach([&](HWND hwnd, AppState& window) {
        if (hwnd == GetForegroundWindow()) reused_tab = ActiveTab(window);
    });
    log.Check(reused_path && Windows(s).size() == windows_before_reuse &&
        GetForegroundWindow() == original_target_window && reused_tab && reused_tab == original_target_tab,
        "opening an existing target reuses its exact Pulse window and tab");
    open_result(folder, true, true);
    log.Check(wait_for(folder.wstring(), true), "real popup folder location selects only the exact folder in its parent");
    open_result(std::filesystem::path(fs::NormalizePath(folder.wstring()) + L"\\"), true, true);
    log.Check(wait_for(folder.wstring(), true), "extended folder path with trailing separator reveals and selects its leaf");
    open_result(file, false, true);
    log.Check(wait_for(file.wstring(), true), "real popup location Open selects the exact file in Pulse");
    const auto other_file = folder / file.filename();
    FILE* other_fixture = nullptr;
    _wfopen_s(&other_fixture, other_file.c_str(), L"wb");
    if (other_fixture) { std::fputs("different target with identical leaf name", other_fixture); std::fclose(other_fixture); }
    log.Check(other_fixture != nullptr, "create same-name file in a different directory");
    open_result(other_file, false, true);
    log.Check(wait_for(other_file.wstring(), true), "same-name reveal selects the different complete path");
    open_result(other_file, false, true);
    log.Check(wait_for(other_file.wstring(), true), "repeated same-name result keeps the unique exact file selected");
    open_result(file, false, true);
    open_result(other_file, false, true);
    open_result(other_file, false, true);
    log.Check(wait_for(other_file.wstring(), true), "rapid repeated result opens retain the final exact target");
    log.Check(!events.open_path(file.wstring(), false, false), "regular files retain their application association");
    s.appPrefs.integration_enabled = false;
    log.Check(!events.open_path(folder.wstring(), true, false), "disabled integration preserves system folder routing");
    s.appPrefs.open_folders_in_pulse = true;
    open_result(folder, true, false);
    log.Check(wait_for(folder.wstring(), false), "legacy folder takeover routes a real popup result into Pulse");
    const auto observation_end = GetTickCount64() + 2000;
    while (GetTickCount64() < observation_end) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        observe_explorer();
        Sleep(25);
    }
    log.Check(explorer_unchanged, "no new Explorer frame or existing Explorer navigation during routing and settling");
    popup.Shutdown();
    events.stage_paths({file.wstring(), file.wstring()});
    const auto staged = events.staged_paths();
    const auto normalized_file = fs::NormalizePath(file.wstring());
    log.Check(std::count(staged.begin(), staged.end(), normalized_file) == 1 && !s.core.tray.batches().empty(),
        "search staging shares the main tray and deduplicates paths");
    events.unstage_path(file.wstring());
    const auto removed = events.staged_paths();
    log.Check(std::find(removed.begin(), removed.end(), normalized_file) == removed.end(),
        "search unstaging removes the shared main tray item");
    const bool before_dark = s.darkMode;
    events.toggle_theme();
    log.Check(s.darkMode != before_dark && s.appPrefs.theme_mode == (before_dark ? 1 : 2),
        "search theme command changes the main window and its shared theme preference");
    events.toggle_theme();
    log.Check(s.darkMode == before_dark, "second search theme command restores the main window appearance");
    const auto saved_history = s.searchHistory;
    s.searchHistory.persist = false;
    log.Check(s.searchHistory.Record(L"routing fixture", L"pulse:search:routing-fixture") &&
        !s.searchHistory.entries.empty(), "seed a valid shared search history entry before clearing");
    events.clear_search_history();
    log.Check(s.searchHistory.entries.empty() && s.core.searchHistory.entries.empty(),
        "search clear-history command clears the same history used by the main address bar");
    s.searchHistory = saved_history;
    s.appPrefs = prefs;
}
void CheckPaneHover(AppState& s, Report& log) {
    s.appPrefs.persist = false;
    ShowWindow(s.hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(s.hwnd);
    SetFocus(s.hwnd);
    s.renderer.SetScale(s.scale);
    s.compositor.RecreateTextFormats(s.scale);
    auto settle = [&] {
        BuildVm(s);
        const auto deadline = GetTickCount64()+3000;
        bool loading = true;
        do {
            MSG message{};
            while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            Sleep(10); BuildVm(s);
            loading = false;
            for (auto* pane : LiveLayout(s).VisiblePanes())
                if (auto* tab = pane->ActiveTab()) loading |= tab->loading;
        } while ((s.paneLayoutMotion.start || loading) && GetTickCount64()<deadline);
        log.Check(!loading,"pane contents finish loading before hover selection check");
    };
    POINT original_cursor{}; GetCursorPos(&original_cursor);
    auto dwell = [&] {
        const auto until = GetTickCount64()+260;
        while (GetTickCount64()<until) {
            MSG message{};
            while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            Sleep(5);
        }
    };
    auto move = [&](int slot, WPARAM buttons = 0, bool wait = true) {
        const auto vm = BuildVm(s);
        const auto r = vm.pane_slots[static_cast<size_t>(slot)].rect;
        const int x = static_cast<int>((r.left+r.right)*0.5f);
        const int y = static_cast<int>(r.bottom-24*s.scale);
        POINT screen{x,y}; ClientToScreen(s.hwnd,&screen); SetCursorPos(screen.x,screen.y);
        s.hoverPoint = {-1,-1};
        HandleMouseMove(&s,s.hwnd,WM_MOUSEMOVE,buttons,MAKELPARAM(x,y));
        if (wait && !buttons) dwell();
    };
    for (const auto layout : {app::LayoutPreset::TwoVertical, app::LayoutPreset::TwoHorizontal,
                              app::LayoutPreset::Three, app::LayoutPreset::FourGrid}) {
        ApplyLayoutPreset(s,layout);
        settle();
        const auto vm = BuildVm(s);
        bool focused = true, unchanged = true;
        for (size_t i=0;i<vm.pane_slots.size();++i) {
            auto* pane = PaneAtSlot(s,static_cast<int>(i));
            const auto revision = pane->ActiveTab()->selection_revision;
            move(static_cast<int>(i));
            if (s.pane != pane) {
                const auto current = BuildVm(s);
                const auto r = current.pane_slots[i].rect;
                const auto hit = s.renderer.HitTest(current,D2D1::RectF(0,0,(float)s.compositor.Width(),(float)s.compositor.Height()),
                    (r.left+r.right)*0.5f,r.bottom-24*s.scale);
                std::fprintf(log.file,"layout=%d slot=%zu hit=%d pane=%d focus=%d capture=%d\n",
                    (int)layout,i,(int)hit.region,hit.pane_index,GetFocus()==s.hwnd,GetCapture()!=nullptr);
                std::fprintf(log.file,"bounds=%.0f,%.0f,%.0f,%.0f viewport=%u,%u scale=%.1f\n",
                    r.left,r.top,r.right,r.bottom,s.compositor.Width(),s.compositor.Height(),s.scale);
            }
            focused &= s.pane == pane;
            unchanged &= pane->ActiveTab()->selection_revision == revision;
        }
        log.Check(focused,"pointer movement activates every visible pane");
        log.Check(unchanged,"hover never selects or clears files");
    }
    ApplyLayoutPreset(s,app::LayoutPreset::TwoVertical);
    settle();
    FocusPane(s,PaneAtSlot(s,0));
    auto* source = s.pane;
    move(1,0,false);
    log.Check(s.pane==source,"crossing another pane does not activate immediately");
    const auto vm = BuildVm(s);
    const auto corner = vm.pane_slots[0].rect;
    POINT toolbar{static_cast<LONG>(corner.right-30*s.scale),static_cast<LONG>(corner.top+16*s.scale)};
    POINT screen = toolbar; ClientToScreen(s.hwnd,&screen); SetCursorPos(screen.x,screen.y);
    HandleMouseMove(&s,s.hwnd,WM_MOUSEMOVE,0,MAKELPARAM(toolbar.x,toolbar.y));
    dwell();
    log.Check(s.pane==source,"passing neighbor en route to current toolbar cancels activation");
    const auto peer = vm.pane_slots[1].rect;
    for (float x : {peer.left+3*s.scale,peer.right-3*s.scale}) {
        POINT point{static_cast<LONG>(x),static_cast<LONG>((peer.top+peer.bottom)/2)};
        screen=point; ClientToScreen(s.hwnd,&screen); SetCursorPos(screen.x,screen.y);
        HandleMouseMove(&s,s.hwnd,WM_MOUSEMOVE,0,MAKELPARAM(point.x,point.y)); dwell();
    }
    log.Check(s.pane==source,"pane edges do not trigger hover activation");
    for (float x : {peer.left+30*s.scale,peer.right-30*s.scale}) {
        POINT point{static_cast<LONG>(x),static_cast<LONG>(peer.top+16*s.scale)};
        screen=point; ClientToScreen(s.hwnd,&screen); SetCursorPos(screen.x,screen.y);
        HandleMouseMove(&s,s.hwnd,WM_MOUSEMOVE,0,MAKELPARAM(point.x,point.y)); dwell();
    }
    log.Check(s.pane==source,"both corners of neighboring toolbar retain current pane");
    move(1,MK_LBUTTON);
    log.Check(s.pane==source,"pressed mouse button retains source pane");
    SetCapture(s.hwnd); move(1); ReleaseCapture();
    log.Check(s.pane==source,"captured drag retains source pane");
    const HWND edit = CreateWindowExW(0,L"EDIT",L"probe",WS_CHILD|WS_VISIBLE,0,0,30,20,s.hwnd,nullptr,nullptr,nullptr);
    SetFocus(edit); move(1);
    log.Check(edit && GetFocus()==edit && s.pane==source,"editing keeps keyboard focus and source pane");
    DestroyWindow(edit); SetFocus(s.hwnd);
    s.filterEditing=true; move(1); s.filterEditing=false;
    log.Check(s.pane==source,"expanded filter retains its pane");
    move(1);
    log.Check(s.pane==PaneAtSlot(s,1),"hover activation resumes after editing or dragging");
    SetCursorPos(original_cursor.x,original_cursor.y);

    CheckItemGrip(s, log);
}
void CheckItemGrip(AppState& s, Report& log) {
    app::Pane fixture;
    auto rows = std::make_shared<std::vector<fs::DirEntry>>(2);
    (*rows)[0].name=L"a.png"; (*rows)[1].name=L"b.txt";
    fixture.view.SetSnapshot(rows);
    const auto bounds = D2D1::RectF(0,0,800*s.scale,600*s.scale);
    for (const auto mode : {ui::ViewMode::Details,ui::ViewMode::List,ui::ViewMode::SmallIcons,
                            ui::ViewMode::MediumIcons,ui::ViewMode::LargeIcons,ui::ViewMode::ExtraLargeIcons}) {
        fixture.view.view_mode=mode;
        ui::PaneViewModel view;
        app::FillPaneViewModel(view,fixture,nullptr);
        view.snapshot.reset();
        view.entries.resize(2);
        view.entries[0].name=L"a.png"; view.entries[1].name=L"b.txt";
        const auto list = s.renderer.PaneListRect(view,bounds);
        const ui::ViewLayout layout(mode,list,view.EntryCount(),0,0,s.scale,s.renderer.ListRowHeightDip(view,list),view.Groups());
        const auto cell = layout.ItemRect(0);
        const float x = cell.right-2*s.scale, y = (cell.top+cell.bottom)*0.5f;
        log.Check(!s.renderer.PointInItemGrip(view,bounds,0,x,y,true),"selected non-text cell area remains marquee space");
        log.Check(!s.renderer.PointInItemGrip(view,bounds,0,cell.right+2*s.scale,y,true),"space outside selected item is not a drag grip");
        const bool grid = mode==ui::ViewMode::MediumIcons || mode==ui::ViewMode::LargeIcons || mode==ui::ViewMode::ExtraLargeIcons;
        const auto picture = grid ? layout.ThumbnailRect(0) : layout.IconRect(0);
        auto artwork=picture;
        if (grid) {
            log.Check(!s.renderer.PointInItemGrip(view,bounds,0,picture.left+s.scale,picture.top+s.scale,true),
                "unpainted thumbnail slot is not a drag target");
            const float pad=(picture.right-picture.left)*0.2f;
            artwork=D2D1::RectF(picture.left+pad,picture.top+pad,picture.right-pad,picture.bottom);
            ui::ItemGripTest::Remember(s.renderer,view,bounds,artwork);
            log.Check(!s.renderer.PointInItemGrip(view,bounds,0,picture.left+s.scale,picture.top+s.scale,false) &&
                !s.renderer.PointInItemGrip(view,bounds,0,picture.left+s.scale,picture.top+s.scale,true),
                "transparent thumbnail margins remain marquee space regardless of selection");
            auto stale=view; ++stale.view_generation;
            log.Check(!s.renderer.PointInItemGrip(stale,bounds,0,(artwork.left+artwork.right)*0.5f,(artwork.top+artwork.bottom)*0.5f,true),
                "new folder generation cannot reuse old image drag bounds");
        }
        log.Check(s.renderer.PointInItemGrip(view,bounds,0,(artwork.left+artwork.right)*0.5f,(artwork.top+artwork.bottom)*0.5f,false) &&
            s.renderer.PointInItemGrip(view,bounds,0,(artwork.left+artwork.right)*0.5f,(artwork.top+artwork.bottom)*0.5f,true),
            "icon or thumbnail drags regardless of selection");
        const auto name = layout.NameRect(0);
        const float text_x = grid ? (name.left+name.right)*0.5f : name.left+4*s.scale;
        const float text_y = name.top+5*s.scale;
        log.Check(s.renderer.PointInItemGrip(view,bounds,0,text_x,text_y,false) &&
            s.renderer.PointInItemGrip(view,bounds,0,text_x,text_y,true), "filename text drags regardless of selection");
        if (mode==ui::ViewMode::Details)
            log.Check(!s.renderer.PointInItemGrip(view,bounds,0,x,y,false),"unselected detail-row whitespace remains marquee space");
        if (grid) {
            ui::PaneViewModel painted;
            painted.view_mode=mode; painted.view_generation=12345;
            ui::ListEntryView folder; folder.name=L"empty"; folder.is_dir=true;
            painted.entries.push_back(folder); painted.empty_folders.insert(0);
            auto* dc=s.compositor.Dc(); dc->BeginDraw();
            ui::ItemGripTest::Paint(s.renderer,painted,bounds,ui::MakeTheme(s.darkMode,s.accentColor));
            log.Check(SUCCEEDED(dc->EndDraw()),"actual grid artwork paints successfully");
            const auto painted_list=s.renderer.PaneListRect(painted,bounds);
            const ui::ViewLayout painted_layout(mode,painted_list,1,0,0,s.scale,s.renderer.ListRowHeightDip(painted,painted_list));
            const auto icon=painted_layout.IconRect(0), slot=painted_layout.ThumbnailRect(0);
            log.Check(!s.renderer.PointInItemGrip(painted,bounds,0,slot.left+s.scale,slot.top+s.scale,true),
                "actual painted folder transparent margin starts marquee");
            log.Check(s.renderer.PointInItemGrip(painted,bounds,0,(icon.left+icon.right)*0.5f,icon.bottom-3*s.scale,true),
                "actual painted folder content remains draggable");
        }
    }
    {
        ui::PaneViewModel view;
        view.view_mode=ui::ViewMode::Details;
        ui::ListEntryView entry;
        entry.name=L"a.txt"; entry.date_text=L"2026/11/01 12:30"; entry.type_text=L"Text document"; entry.size_text=L"123 KB";
        entry.created_text=entry.accessed_text=entry.date_text;
        view.entries.push_back(entry);
        const auto old_mask=s.renderer.DetailsColumnsMask();
        s.renderer.SetDetailsColumns(old_mask | ui::kDetailsColumnCreated | ui::kDetailsColumnAccessed);
        const auto metadata_bounds=D2D1::RectF(0,0,1600*s.scale,600*s.scale);
        const auto columns=s.renderer.DetailsColumns(metadata_bounds,view);
        const auto cell=s.renderer.ItemRectInPane(view,metadata_bounds,0);
        const float cy=(cell.top+cell.bottom)*0.5f;
        for (const auto kind : {ui::MainRenderer::ColumnKind::Date,ui::MainRenderer::ColumnKind::Type,ui::MainRenderer::ColumnKind::Size,
                               ui::MainRenderer::ColumnKind::Created,ui::MainRenderer::ColumnKind::Accessed}) {
            const float left=columns.Left(kind)+8*s.scale;
            const float right=columns.Left(kind)+columns.Width(kind)-8*s.scale;
            log.Check(columns.Has(kind),"metadata fixture displays the tested column");
            bool hit=false, blank=false, independent=true;
            for(float px=left;px<right;px+=s.scale) {
                const bool grip=s.renderer.PointInItemGrip(view,metadata_bounds,0,px,cy,false);
                hit|=grip; blank|=!grip;
                independent &= grip==s.renderer.PointInItemGrip(view,metadata_bounds,0,px,cy,true);
            }
            log.Check(hit,"metadata text provides a drag grip");
            log.Check(blank,"metadata column retains blank marquee area");
            log.Check(independent,"metadata hit target is selection independent");
        }
        s.renderer.SetDetailsColumns(old_mask);
    }
    auto* tab=ActiveTab(s);
    tab->SetSnapshot(rows); tab->SelectIndices({0,1});
    HandleListRowClick(s,0,false,false);
    log.Check(tab->SelectedCount()==2,"pressing a selected item retains multi-selection until release");
    tab->view_mode=ui::ViewMode::Details;
    const auto live = BuildVm(s);
    for (const auto& slot : live.pane_slots) if (slot.focused) {
        const auto cell=s.renderer.ItemRectInPane(slot.pane,slot.rect,slot.pane.ViewIndex(0));
        const int x=static_cast<int>(cell.left+(cell.right-cell.left)*0.65f);
        const int y=static_cast<int>((cell.top+cell.bottom)*0.5f);
        HandleLButtonDown(&s,s.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
        log.Check(s.listSelection.PendingBand() && !s.listSelection.PendingFile(),
            "pressing selected non-text area arms marquee instead of dragging files");
        ReleaseCapture();
        HandleCaptureChanged(&s,s.hwnd,WM_CAPTURECHANGED,0,0);
        tab->ClearSelection();
        HandleLButtonDown(&s,s.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
        log.Check(s.listSelection.PendingBand() && !s.listSelection.PendingFile(),"pressing unselected row whitespace arms marquee");
        ReleaseCapture();
        HandleCaptureChanged(&s,s.hwnd,WM_CAPTURECHANGED,0,0);
    }
}
void CheckWorkspaceRecovery(AppState& s, Report& log) {
    s.appPrefs.persist=false;
    s.places.persist=false;
    s.shot.active=true;
    wchar_t fixture[32768]{};
    GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR",fixture,ARRAYSIZE(fixture));
    const auto root=std::filesystem::path(fixture)/L"workspace-root";
    const auto other=std::filesystem::path(fixture)/L"workspace-other";
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(other);
    app::Workspace workspace;
    workspace.name=L"Recovery fixture";
    workspace.root=root.wstring();
    workspace.layout=static_cast<int>(app::LayoutPreset::TwoVertical);
    workspace.pane_paths={app::MakeWorkspacePath(0),other.wstring()};
    s.places.workspaces={workspace};
    OpenWorkspace(s,0);
    auto visible=LiveLayout(s).VisiblePanes();
    log.Check(visible.size()==2 && visible[0]->ActiveTab()->current_path==fs::NormalizePath(root.wstring()) &&
        visible[1]->ActiveTab()->current_path==fs::NormalizePath(other.wstring()),
        "workspace pane containing its own action link restores as a folder without recursion");
    for (auto preset:{app::LayoutPreset::Single,app::LayoutPreset::TwoVertical,app::LayoutPreset::TwoHorizontal,
                      app::LayoutPreset::Three,app::LayoutPreset::FourGrid}) {
        ApplyLayoutPreset(s,preset);
        visible=LiveLayout(s).VisiblePanes();
        bool ok=visible.size()==app::LayoutPresetCount(preset);
        for (const auto* pane:visible)
            ok=ok && !pane->ActiveTab()->current_path.starts_with(app::kWorkspacePathPrefix);
        log.Check(ok,"switching workspace split layout retains concrete pane locations");
    }
    app::SessionSnapshot saved;
    auto layout=app::CaptureLayoutTab(LiveLayout(s));
    layout.panes[0].path=app::MakeWorkspacePath(0);
    layout.panes[1].path=app::MakeWorkspacePath(999);
    saved.layout_tabs={layout};
    const auto session_file=(std::filesystem::path(fixture)/L"legacy-session.json").wstring();
    log.Check(WriteUtf8FileAtomic(session_file,app::SessionToJson(saved)),"save legacy workspace links in isolated session");
    app::SessionSnapshot loaded;
    std::wstring session_json;
    log.Check(ReadUtf8File(session_file,session_json) && app::ParseSessionJson(session_json,loaded) &&
        !loaded.layout_tabs.empty(),"reload isolated session containing legacy links");
    s.pane=nullptr;
    s.targetPane=nullptr;
    app::RestoreWindowTabs(s.window_tabs,loaded.layout_tabs,loaded.tab_groups,0,
        [&](app::Tab& tab,const std::wstring& path) { StartLoadingPath(s,tab,path,PathLoadReason::RestoreSession); });
    for (auto& tab:s.window_tabs.items) app::RebuildLayoutRoot(*tab);
    BindCurrentLayout(s);
    visible=LiveLayout(s).VisiblePanes();
    log.Check(visible.size()==4 && visible[0]->ActiveTab()->current_path==fs::NormalizePath(root.wstring()) &&
        visible[1]->ActiveTab()->current_path.empty(),
        "startup restore resolves valid links and safely opens This PC for removed workspaces");
    ApplyLayoutPreset(s,app::LayoutPreset::TwoHorizontal);
    log.Check(LiveLayout(s).VisiblePanes().size()==2,"recovered session can change split layout again");
    NavigateTo(s,app::MakeWorkspacePath(0));
    log.Check(LayoutOf(s)==app::LayoutPreset::TwoVertical,"explicit workspace navigation still restores saved layout");
    workspace.root=app::MakeWorkspacePath(0);
    log.Check(app::ResolveWorkspaceFolderPath(app::MakeWorkspacePath(0),{workspace}).empty(),
        "cyclic workspace root resolves without recursion");
    log.Check(!app::WorkspacePathIndex(L"pulse:workspace:99999999999999999999999999",1) &&
        !app::WorkspacePathIndex(L"pulse:workspace:-1",1) && !app::WorkspacePathIndex(L"pulse:workspace:x",1),
        "malformed workspace links cannot select an unintended workspace");
}
__declspec(noinline) void CheckWinEActivate(AppState& original, Report& log) {
        const auto prefs = original.appPrefs;
        std::wstring data(32768, L'\0');
        data.resize(GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data.data(), static_cast<DWORD>(data.size())));
        const auto folder = std::filesystem::path(data) / L"win-e-activate";
        std::filesystem::create_directories(folder);
        const auto settings = folder / L"roundtrip.json";
        for (int mode : {0, 1, 2, -1, 3}) {
            app::AppPrefs saved, loaded;
            saved.open_win_e = mode;
            saved.open_pulse_icon = mode;
            std::wstring json;
            log.Check(WriteUtf8FileAtomic(settings.wstring(), saved.ToJson()) &&
                ReadUtf8File(settings.wstring(), json) && loaded.FromJson(json) &&
                loaded.open_win_e == (mode >= 0 && mode <= 2 ? mode : 0) &&
                loaded.open_pulse_icon == (mode >= 0 && mode <= 2 ? mode : 0),
                "WinEActivate preferences disk roundtrip and invalid value normalization");
        }
        app::AppPrefs invalid;
        log.Check(invalid.FromJson(L"{\"open_win_e\":3}") && invalid.open_win_e == 0,
            "WinEActivate rejects unsupported persisted mode");
        WindowInit init;
        init.path = folder.wstring();
        const HWND peer = OpenPulseWindow(original, init);
        auto* target = original.core.windows.Lookup(peer);
        log.Check(target != nullptr, "WinEActivate creates second isolated window");
        if (target) {
            target->isolatedTest = true;
            original.appPrefs.open_win_e = 2;
            original.appPrefs.open_pulse_icon = 2;
            original.appPrefs.new_window_shows = 1;
            const auto count = original.core.windows.Count();
            for (AppState* chosen : {target, &original}) {
                for (const int show : {SW_SHOWNOACTIVATE, SW_MINIMIZE, SW_HIDE}) {
                    for (const wchar_t* request : {L"pulse:win-e", L"", L"pulse:activate"}) {
                        original.appPrefs.open_pulse_icon = std::wstring_view(request) == L"pulse:activate" ? 0 : 2;
                        auto* tab = ActiveTab(*chosen);
                        const auto path = tab ? tab->current_path : std::wstring();
                        const auto tabs = chosen->window_tabs.items.size();
                        const auto other_tabs = (chosen == target ? original : *target).window_tabs.items.size();
                        ShowWindow(chosen->hwnd, show);
                        if (std::wstring_view(request) == L"pulse:activate" && show == SW_HIDE)
                            chosen->hidden_to_tray = true;
                        original.core.windows.Touch(chosen->hwnd);
                        const auto result = OpenExternalFolder(chosen == target ? original : *target, request);
                        log.Check(result.status == ExternalOpenStatus::Dispatched && result.target == chosen &&
                            original.core.windows.Count() == count && chosen->window_tabs.items.size() == tabs &&
                            (chosen == target ? original : *target).window_tabs.items.size() == other_tabs &&
                            ActiveTab(*chosen) == tab && tab && tab->current_path == path &&
                            IsWindowVisible(chosen->hwnd) && !IsIconic(chosen->hwnd),
                            "WinEActivate, icon and taskbar activation reveal MRU without changing window or tab counts");
                    }
                }
            }
            original.core.windows.Touch(peer);
            EnableWindow(peer, FALSE);
            const auto deferred = OpenExternalFolder(original, L"pulse:activate");
            log.Check(deferred.status == ExternalOpenStatus::Deferred && original.core.windows.Count() == count,
                "taskbar activation defers busy MRU without creating a fallback window");
            EnableWindow(peer, TRUE);
            DestroyWindow(peer);
        }
        original.appPrefs = prefs;
}
} // namespace
int RunTrayForegroundTestHost(const wchar_t *title) {
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseTrayForegroundTestHost";
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
        if (msg == WM_APP + 1) { SetForegroundWindow(hwnd); return 0; }
        if (msg == WM_TIMER) { DestroyWindow(hwnd); return 0; }
        if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(hwnd, msg, wp, lp);
    };
    if (!RegisterClassW(&wc)) return 2;
    const HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, title,
        WS_POPUP, 40, 40, 200, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 3;
    if (!SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE)) {
        DestroyWindow(hwnd);
        return 4;
    }
    // WaitForInputIdle can observe an earlier startup thread's queue; publish
    // readiness only after this exact fixture window has been created/shown.
    const auto ready_name = std::wstring(title) + L"-ready";
    if (const HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name.c_str())) {
        SetEvent(ready);
        CloseHandle(ready);
    }
    SetTimer(hwnd, 1, 15000, nullptr);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
__declspec(noinline) static int RunWindowManagementFullTest(AppState &original, const std::wstring &report_path) {
    Report log;
    if (_wfopen_s(&log.file, report_path.c_str(), L"wb") != 0 || !log.file)
        return 2;
    if (!original.isolatedTest || !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0)) {
        log.Check(false, "requires isolated test instance and PULSE_TEST_DATA_DIR");
        std::fclose(log.file);
        return 2;
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_FEEDBACK_REOPEN_ONLY", nullptr, 0)) {
        auto& s = original;
        const auto prefs = s.appPrefs;
        s.appPrefs.persist = false;
        s.appPrefs.keep_running_on_close = true;
        s.appPrefs.notify_icon_mode = 2;
        s.appPrefs.open_pulse_icon = 2;
        s.appPrefs.close_window_with_last_tab = false;
        wchar_t data[32768]{};
        GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data, ARRAYSIZE(data));
        const auto a = std::filesystem::path(data) / L"reopen-A";
        const auto b = std::filesystem::path(data) / L"reopen-B";
        std::filesystem::create_directories(a);
        std::filesystem::create_directories(b);
        for (int startup : {0, 1}) {
            s.appPrefs.startup_open = startup;
            s.appPrefs.home_folder.clear();
            s.appPrefs.new_window_shows = 1;
            StartFreshAt(s, a.wstring());
            const auto a_index = s.window_tabs.active;
            OpenTabAt(s, b.wstring());
            CloseLayoutTab(s, a_index);
            log.Check(ActiveTab(s) && ActiveTab(s)->current_path == fs::NormalizePath(b.wstring()),
                "closing A after opening B keeps B as active folder");
            ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
            SendMessageW(s.hwnd, WM_CLOSE, 0, 0);
            log.Check(IsWindow(s.hwnd) && !IsWindowVisible(s.hwnd), "real close hides last background window");
            const auto opened = OpenExternalFolder(s, L"");
            const auto expected = startup == 0 ? fs::NormalizePath(b.wstring()) : std::wstring();
            log.Check(opened.target == &s && IsWindowVisible(s.hwnd) && ActiveTab(s) &&
                ActiveTab(s)->current_path == expected,
                startup == 0 ? "reopen restores B rather than the closed A" :
                    "reopen uses default This PC instead of stale A");
        }
        s.appPrefs = prefs;
        std::fprintf(log.file, "Failures: %d\n", log.failed);
        std::fclose(log.file);
        return log.failed ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_TRAY_MENU_ONLY", nullptr, 0)) {
        auto &s = original;
        s.appPrefs.persist = false;
        ShowWindow(s.hwnd, SW_SHOWNORMAL);
        if (GetEnvironmentVariableW(L"PULSE_TEST_TRAY_MENU_INSPECT", nullptr, 0)) {
            log.Check(s.tray_controller.SetVisible(true), "isolated notification icon installed for shell inspection");
            const ULONGLONG deadline = GetTickCount64() + 60000;
            while (IsWindow(s.hwnd) && GetTickCount64() < deadline) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
                MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
            s.tray_controller.SetVisible(false);
            std::fprintf(log.file, "Failures: %d\n", log.failed);
            std::fclose(log.file);
            return log.failed ? 1 : 0;
        }
        SetWindowArrangeAnimations(false);
        WindowInit init; init.path = L""; init.rect = Bounds(s.hwnd);
        const HWND other = OpenPulseWindow(s, init);
        log.Check(other != nullptr, "tray count fixture creates second window");
        if (other) {
            ShowWindow(other, SW_HIDE);
            const auto candidates = WindowArrangementCandidates(s, false, {}, true);
            const auto items = TrayMenuItems(s);
            log.Check(candidates.size() == 2 && items.size() == 5 && items[2].shortcut == L"2",
                "tray menu counts hidden windows with the arrangement candidate rule");
            log.Check(items[0].command == 1 && items[1].command == 3 && items[3].command == 4 &&
                items[4].command == 2 && items[2].children.size() == 6,
                "tray keeps open, new, six arrangements, merge and exit commands");
            EnableWindow(other, FALSE);
            const auto one = TrayMenuItems(s);
            log.Check(one[2].shortcut == L"1" && !one[2].enabled && one[2].children.empty() && !one[3].enabled,
                "one candidate disables arrangement flyout and merge");
            EnableWindow(s.hwnd, FALSE);
            const auto zero = TrayMenuItems(s);
            log.Check(zero[2].shortcut == L"0" && !zero[2].enabled && zero[2].children.empty() && !zero[3].enabled,
                "zero candidates have no arrangement flyout or merge action");
            EnableWindow(s.hwnd, TRUE);
            EnableWindow(other, TRUE);
            if (auto *state = s.core.windows.Lookup(other)) {
                state->hwndRenameEdit = other;
                log.Check(TrayMenuItems(s)[2].shortcut == L"1", "tray count excludes renaming windows");
                state->hwndRenameEdit = nullptr;
            }
            const auto snapshot = std::filesystem::path(report_path).parent_path();
            {
                ui::FluentMenu menu;
                if (menu.Create(s.hwnd, &s.compositor, s.scale)) {
                    menu.SetTheme(s.darkMode, s.accentColor);
                    log.Check(menu.SaveDebugSnapshot((snapshot / L"tray-menu-zero.png").c_str(), zero),
                        "zero-candidate disabled menu snapshot saved");
                    log.Check(menu.SaveDebugSnapshot((snapshot / L"tray-menu-one.png").c_str(), one),
                        "one-candidate disabled menu snapshot saved");
                } else log.Check(false, "disabled tray menu snapshot renderer creates");
            }
            for (bool dark : {false, true}) for (float scale : {1.0f, 1.5f, 2.0f}) {
                ui::FluentMenu menu;
                const bool created = menu.Create(s.hwnd, &s.compositor, scale);
                log.Check(created, "tray Fluent menu creates at target DPI");
                if (!created) continue;
                menu.SetTheme(dark, s.accentColor);
                const auto name = std::wstring(L"tray-menu-") + (dark ? L"dark-" : L"light-") +
                    std::to_wstring(static_cast<int>(scale * 100));
                log.Check(menu.SaveDebugSnapshot((snapshot / (name + L".png")).c_str(), items),
                    "tray menu theme and DPI snapshot saved");
                log.Check(menu.SaveDebugSnapshot((snapshot / (name + L"-layouts.png")).c_str(), items[2].children),
                    "tray arrangement flyout snapshot saved");
            }
            const HWND hidden_owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Tray menu hidden owner",
                WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            log.Check(hidden_owner && !IsWindowVisible(hidden_owner), "tray fixture uses a hidden owner like production");
            static bool tray_popup_seen = false;
            static RECT tray_popup_bounds{};
            const auto activate_fixture = [&] {
                ShowWindow(s.hwnd, SW_SHOWNORMAL);
                if (SetForegroundWindow(s.hwnd) && GetForegroundWindow() == s.hwnd) return true;
                const DWORD foreground_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
                const bool attached = foreground_thread && foreground_thread != GetCurrentThreadId() &&
                    AttachThreadInput(GetCurrentThreadId(), foreground_thread, TRUE);
                SetForegroundWindow(s.hwnd);
                if (attached) AttachThreadInput(GetCurrentThreadId(), foreground_thread, FALSE);
                return GetForegroundWindow() == s.hwnd;
            };
            tray_popup_seen = false;
            tray_popup_bounds = {};
            const auto timer = SetTimer(nullptr, 0, 200, [](HWND, UINT, UINT_PTR id, DWORD) {
                KillTimer(nullptr, id);
                EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM) -> BOOL {
                    wchar_t name[64]{};
                    GetClassNameW(hwnd, name, ARRAYSIZE(name));
                    if (wcscmp(name, L"PulseFluentMenu") == 0 && IsWindowVisible(hwnd)) {
                        tray_popup_seen = true;
                        GetWindowRect(hwnd, &tray_popup_bounds);
                        PostMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
                    }
                    return TRUE;
                }, 0);
            });
            log.Check(timer != 0, "tray popup cancellation timer created");
            if (timer) {
                const BOOL activated = activate_fixture();
                std::fprintf(log.file, "[INFO] tray Escape fixture foreground request: %d; foreground belongs to UI: %d\n",
                    activated != FALSE, GetForegroundWindow() == s.hwnd);
                MONITORINFO monitor{sizeof(monitor)};
                GetMonitorInfoW(MonitorFromWindow(s.hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
                const POINT anchor{(monitor.rcWork.left + monitor.rcWork.right) / 2, monitor.rcWork.bottom - 60};
                const int command = TrackTrayMenu(s, anchor, hidden_owner);
                KillTimer(nullptr, timer);
                log.Check(tray_popup_seen && command == 0, "real Fluent tray popup opens and Escape cancels");
                std::fprintf(log.file, "[INFO] tray anchor=(%ld,%ld) popup=(%ld,%ld,%ld,%ld)\n",
                    anchor.x, anchor.y, tray_popup_bounds.left, tray_popup_bounds.top,
                    tray_popup_bounds.right, tray_popup_bounds.bottom);
                log.Check(tray_popup_seen && tray_popup_bounds.bottom + ui::FluentMenu::CardOrigin({0, 0}).y <= anchor.y &&
                    std::abs((tray_popup_bounds.left + tray_popup_bounds.right) / 2 - anchor.x) <= 1,
                    "real tray popup is centered above its anchor rather than below pointer");
            }
            wchar_t executable[32768]{};
            GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
            const auto title = L"PulseTrayForeground-" + std::to_wstring(GetCurrentProcessId());
            std::wstring command_line = L"\"" + std::wstring(executable) + L"\" --test-instance --tray-foreground-test-host " + title;
            STARTUPINFOW startup{sizeof(startup)};
            startup.dwFlags = STARTF_USESHOWWINDOW;
            startup.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION process{};
            const auto ready_name = title + L"-ready";
            const HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
            log.Check(ready != nullptr, "foreground fixture readiness event created");
            const bool launched = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0,
                nullptr, nullptr, &startup, &process) != FALSE;
            log.Check(launched, "independent foreground fixture process starts");
            if (launched) {
                const HANDLE waits[] = {ready, process.hProcess};
                const DWORD readiness = ready ? WaitForMultipleObjects(2, waits, FALSE, 5000) : WAIT_FAILED;
                static HWND foreign_window = nullptr;
                static bool foreground_popup_seen = false;
                static bool topmost_popup_seen = false;
                static bool watchdog_fired = false;
                static bool handoff_allowed = false;
                static DWORD foreign_process = 0;
                foreign_window = FindWindowW(L"PulseTrayForegroundTestHost", title.c_str());
                DWORD window_pid = 0, child_exit = 0;
                if (foreign_window) GetWindowThreadProcessId(foreign_window, &window_pid);
                GetExitCodeProcess(process.hProcess, &child_exit);
                wchar_t window_class[128]{};
                if (foreign_window) GetClassNameW(foreign_window, window_class, ARRAYSIZE(window_class));
                std::fprintf(log.file, "[INFO] foreground fixture: pid=%lu readiness=%lu hwnd=%p window_pid=%lu class=%ls visible=%d style=0x%llx exstyle=0x%llx exit=%lu\n",
                    process.dwProcessId, readiness, static_cast<void*>(foreign_window), window_pid, window_class,
                    foreign_window && IsWindowVisible(foreign_window),
                    static_cast<unsigned long long>(GetWindowLongPtrW(foreign_window, GWL_STYLE)),
                    static_cast<unsigned long long>(GetWindowLongPtrW(foreign_window, GWL_EXSTYLE)), child_exit);
                foreground_popup_seen = topmost_popup_seen = watchdog_fired = handoff_allowed = false;
                foreign_process = process.dwProcessId;
                log.Check(readiness == WAIT_OBJECT_0 && foreign_window && window_pid == process.dwProcessId &&
                    IsWindowVisible(foreign_window) && (GetWindowLongPtrW(foreign_window, GWL_EXSTYLE) & WS_EX_TOPMOST),
                    "independent topmost fixture is ready");
                const auto foreign_timer = SetTimer(nullptr, 0, 250, [](HWND, UINT, UINT_PTR id, DWORD) {
                    KillTimer(nullptr, id);
                    const HWND active = GetForegroundWindow();
                    wchar_t name[64]{};
                    GetClassNameW(active, name, ARRAYSIZE(name));
                    foreground_popup_seen = wcscmp(name, L"PulseFluentMenu") == 0;
                    topmost_popup_seen = (GetWindowLongPtrW(active, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
                    handoff_allowed = foreground_popup_seen && AllowSetForegroundWindow(foreign_process) != FALSE;
                    PostMessageW(foreign_window, WM_APP + 1, 0, 0);
                });
                // A failed activation must fail promptly, not leave an unattended modal loop.
                const auto watchdog = SetTimer(nullptr, 0, 3000, [](HWND, UINT, UINT_PTR id, DWORD) {
                    KillTimer(nullptr, id);
                    watchdog_fired = true;
                    EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM) -> BOOL {
                        wchar_t name[64]{}; GetClassNameW(hwnd, name, ARRAYSIZE(name));
                        if (wcscmp(name, L"PulseFluentMenu") == 0) PostMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
                        return TRUE;
                    }, 0);
                });
                if (foreign_window && foreign_timer && watchdog) {
                    const BOOL activated = activate_fixture();
                    std::fprintf(log.file, "[INFO] tray external fixture foreground request: %d; foreground belongs to UI: %d\n",
                        activated != FALSE, GetForegroundWindow() == s.hwnd);
                    const auto bounds = Bounds(s.hwnd);
                    const int selected = TrackTrayMenu(s, {bounds.left + 80, bounds.top + 80}, hidden_owner);
                    log.Check(foreground_popup_seen && topmost_popup_seen,
                        "hidden-owner tray popup activates above independent topmost window");
                    log.Check(handoff_allowed, "foreground tray popup authorizes external fixture activation");
                    log.Check(selected == 0 && !watchdog_fired && GetForegroundWindow() == foreign_window,
                        "cross-process foreground change dismisses tray menu without local Escape");
                }
                if (foreign_timer) KillTimer(nullptr, foreign_timer);
                if (watchdog) KillTimer(nullptr, watchdog);
                if (foreign_window) PostMessageW(foreign_window, WM_CLOSE, 0, 0);
                log.Check(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0, "foreground fixture process exits");
                GetExitCodeProcess(process.hProcess, &child_exit);
                std::fprintf(log.file, "[INFO] foreground fixture final exit=%lu\n", child_exit);
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            }
            if (ready) CloseHandle(ready);
            if (hidden_owner) DestroyWindow(hidden_owner);
            log.Check(ArrangeWindows(s, app::WindowArrangeLayout::Cascade, false, {}, true) && IsWindowVisible(other),
                "tray arrangement restores and arranges hidden eligible windows");
            UndoWindowArrange(s, WindowArrangeGeneration());
            DestroyWindow(other);
        }
        std::fprintf(log.file, "Failures: %d\n", log.failed);
        std::fclose(log.file);
        return log.failed ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_OVERVIEW_ONLY", nullptr, 0)) {
        auto& s = original;
        s.appPrefs.persist = false;
        ShowWindow(s.hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(s.hwnd);
        SetFocus(s.hwnd);
        SetWindowPos(s.hwnd, nullptr, 80, 80, 1100, 740, SWP_NOZORDER);
        ToggleWindowOverview(s);
        const auto snapshot = std::filesystem::path(report_path).parent_path();
        for (int width : {1100, 720}) {
            s.darkMode = width == 720;
            SetWindowPos(s.hwnd, nullptr, 100, 100, width, 700, SWP_NOZORDER);
            auto vm = BuildVm(s);
            vm.dark = s.darkMode;
            vm.window_effect = ui::WindowEffect::None;
            vm.backdrop_enabled = false;
            auto* dc = s.compositor.Dc();
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(s.darkMode ? 0x202020 : 0xFFFFFF));
            s.renderer.Render(vm, D2D1::RectF(0,0,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height())),
                ui::MakeTheme(s.darkMode,s.accentColor));
            log.Check(SUCCEEDED(dc->EndDraw()), "overview fixture draws before swap-chain presentation");
            log.Check(s.overview.open, "overview remains open after move and resize");
            POINT caption{60, 40}; ClientToScreen(s.hwnd, &caption);
            log.Check(SendMessageW(s.hwnd, WM_NCHITTEST, 0, MAKELPARAM(caption.x,caption.y)) == HTCAPTION,
                "overview title supports native dragging");
            const auto wr = Bounds(s.hwnd);
            log.Check(SendMessageW(s.hwnd, WM_NCHITTEST, 0, MAKELPARAM(wr.right-1,wr.bottom-1)) == HTBOTTOMRIGHT,
                "overview retains native corner resize hit");
            int actions = 0; bool prior = false;
            for (int x=0; x<width; ++x) {
                const bool action = s.renderer.WindowOverview().HitTest(static_cast<float>(x),40).kind == ui::OverviewHitKind::Arrange;
                if (action && !prior) ++actions;
                prior = action;
            }
            bool found = actions == 1;
            for (int y=0; !found && y<200; ++y)
                for (int x=0; x<width; ++x)
                    if (s.renderer.WindowOverview().HitTest(static_cast<float>(x),static_cast<float>(y)).kind == ui::OverviewHitKind::Arrange) {
                        found = true; break;
                    }
            log.Check(found && actions <= 1, "overview exposes one compact arrangement entry");
            log.Check(s.compositor.SaveSnapshot((snapshot/(L"overview-"+std::to_wstring(width)+L".png")).c_str()),
                "overview screenshot captured");
        }
        if (EnsureMenu(s)) {
            log.Check(s.menu->SaveDebugSnapshot((snapshot/L"overview-layout-menu.png").c_str(), WindowArrangementMenuItems()),
                "shared nine-tile arrangement and merge menu screenshot captured");
            static AppState* menu_owner = nullptr;
            static bool menu_opened = false;
            menu_owner = &s; menu_opened = false;
            const auto timer = SetTimer(nullptr, 0, 150, [](HWND, UINT, UINT_PTR id, DWORD) {
                KillTimer(nullptr,id);
                menu_opened = menu_owner->menu->IsOpen();
                menu_owner->menu->Dismiss();
            });
            POINT action{-1,-1};
            for (int y=0; action.x<0 && y<200; ++y)
                for (int x=0; x<720; ++x)
                    if (s.renderer.WindowOverview().HitTest(static_cast<float>(x),static_cast<float>(y)).kind == ui::OverviewHitKind::Arrange) {
                        action = {x,y}; break;
                    }
            if (timer && action.x>=0) SendMessageW(s.hwnd,WM_LBUTTONDOWN,0,MAKELPARAM(action.x,action.y));
            if (timer) KillTimer(nullptr,timer);
            log.Check(menu_opened && s.overview.open, "arrangement menu opens and cancellation retains overview");
            menu_owner = nullptr;
        }
        SendMessageW(s.hwnd, WM_CANCELMODE, 0, 0);
        log.Check(s.overview.open, "native move cancellation does not dismiss overview");
        SendMessageW(s.hwnd, WM_NCLBUTTONDBLCLK, HTCAPTION, 0);
        log.Check(s.overview.open && IsZoomed(s.hwnd), "overview supports title double-click maximize");
        ShowWindow(s.hwnd, SW_RESTORE);
        const auto windows = s.core.windows.Count();
        const auto tabs = s.window_tabs.items.size();
        log.Check(!ArrangeWindows(s, app::WindowArrangeLayout::SideBySide, false) &&
            s.core.windows.Count()==windows && s.window_tabs.items.size()==tabs,
            "single-window arrangement leaves windows and tabs unchanged");
        if (s.overview.open) ToggleWindowOverview(s);
        const RECT before = Bounds(s.hwnd);
        WindowInit init; init.path = L""; init.rect = before;
        const HWND other = OpenPulseWindow(s, init);
        log.Check(other != nullptr, "second isolated window created");
        if (other) {
            const RECT other_before = Bounds(other);
            log.Check(ArrangeWindows(s, app::WindowArrangeLayout::Cascade, false, {s.hwnd,other}),
                "arrange existing windows succeeds");
            const auto deadline = GetTickCount64()+350;
            while (GetTickCount64()<deadline) {
                MSG message{};
                while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
                Sleep(10);
            }
            log.Check(!Same(before,Bounds(s.hwnd)) && !Same(Bounds(s.hwnd),Bounds(other)) &&
                s.core.windows.Count()==windows+1, "arrangement moves both existing windows without spawning extras");
            const auto first_arranged = Bounds(s.hwnd), second_arranged = Bounds(other);
            log.Check(first_arranged.right-first_arranged.left==before.right-before.left &&
                first_arranged.bottom-first_arranged.top==before.bottom-before.top &&
                second_arranged.right-second_arranged.left==other_before.right-other_before.left &&
                second_arranged.bottom-second_arranged.top==other_before.bottom-other_before.top,
                "cascade preserves both actual window sizes");
            Undo(s);
            log.Check(Same(before,Bounds(s.hwnd)) && Same(other_before,Bounds(other)),
                "undo restores both window bounds");
            ShowWindow(other,SW_MAXIMIZE);
            SetWindowArrangeAnimations(false);
            const bool maximized_arranged = ArrangeWindows(s,app::WindowArrangeLayout::Cascade,false,{s.hwnd,other});
            const auto restored_bounds = Bounds(other);
            log.Check(maximized_arranged && !IsZoomed(other) &&
                restored_bounds.right-restored_bounds.left==other_before.right-other_before.left &&
                restored_bounds.bottom-restored_bounds.top==other_before.bottom-other_before.top,
                "cascade uses maximized window restore size");
            Undo(s);
            log.Check(IsZoomed(other)!=FALSE,"cascade undo restores maximized state");
            ShowWindow(other, SW_RESTORE);
            for (auto layout : {app::WindowArrangeLayout::MainRight, app::WindowArrangeLayout::MainTop}) {
                const bool arranged = ArrangeWindows(s, layout, false, {s.hwnd, other});
                const auto a = Bounds(s.hwnd), b = Bounds(other);
                std::fprintf(log.file, "layout=%d arranged=%d first=%ld,%ld,%ld,%ld second=%ld,%ld,%ld,%ld\n",
                    static_cast<int>(layout), arranged, a.left,a.top,a.right,a.bottom,b.left,b.top,b.right,b.bottom);
                log.Check(arranged && (layout == app::WindowArrangeLayout::MainRight ? a.left >= b.right : a.bottom <= b.top),
                          "new main layout positions real windows on requested side");
                Undo(s);
            }
            SetWindowArrangeAnimations(true);
            const auto grid = WindowArrangementMenuItems();
            log.Check(grid.size() == 3 && grid.back().quick_swatches.size() == 3 &&
                      grid.back().quick_swatches.back().command == 18,
                      "merge shares the nine-tile arrangement panel");
            log.Check(MergeAllWindows(s) && s.core.windows.Count() == windows &&
                      s.window_tabs.items.size() == tabs + 1,
                      "integrated merge retains peer tab in current window");
            if (IsWindow(other)) DestroyWindow(other);
        }
        std::fprintf(log.file,"Failures: %d\n",log.failed);
        std::fclose(log.file);
        return log.failed ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_WORKSPACE_RECOVERY_ONLY", nullptr, 0)) {
        CheckWorkspaceRecovery(original, log);
        std::fprintf(log.file,"Failures: %d\n",log.failed);
        std::fclose(log.file);
        return log.failed ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_ITEM_GRIP_ONLY", nullptr, 0)) {
        CheckItemGrip(original, log);
        const int result = log.failed ? 1 : 0;
        std::fclose(log.file);
        return result;
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_PANE_HOVER_ONLY", nullptr, 0)) {
        CheckPaneHover(original, log);
        std::fprintf(log.file,"Failures: %d\n",log.failed);
        std::fclose(log.file);
        return log.failed ? 1 : 0;
    }
    wchar_t selected[2]{};
    const bool external_only = GetEnvironmentVariableW(L"PULSE_TEST_EXTERNAL_OPEN_ONLY", selected, ARRAYSIZE(selected)) == 1 && selected[0] == L'1';
    wchar_t inspect_value[2]{};
    const bool notice_inspect = GetEnvironmentVariableW(L"PULSE_TEST_EXTERNAL_OPEN_NOTICE_INSPECT", inspect_value,
        ARRAYSIZE(inspect_value)) == 1 && inspect_value[0] == L'1';
    std::filesystem::path inspect_directory;
    if (notice_inspect) {
        wchar_t directory[32768]{};
        const DWORD length = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", directory, ARRAYSIZE(directory));
        const DWORD attributes = length && length < ARRAYSIZE(directory) ? GetFileAttributesW(directory) : INVALID_FILE_ATTRIBUTES;
        inspect_directory = directory;
        if (!external_only || !length || length >= ARRAYSIZE(directory) || !inspect_directory.is_absolute() ||
            attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            log.Check(false, "notice inspection requires external-only and an existing absolute isolated data directory");
            std::fclose(log.file); return 2;
        }
    }
    std::fprintf(log.file, "Mode: %s\n", external_only ? "external-open-only" : "full-window-management");
    if (external_only) std::fprintf(log.file, "[SKIP] keymap/menu/arrangement cases excluded by explicit external-open-only filter\n");
    if (!external_only) {
        app::Keymap keys;
        const app::KeyChord chord{static_cast<uint8_t>(app::kKeyCtrl | app::kKeyAlt), VK_F8};
        keys.SetBindings(app::KeyAction::ArrangeColumns, {chord});
        log.Check(app::RouteKey(keys, chord, 0).action == app::KeyAction::ArrangeColumns,
                  "unbound arrangement action routes after assignment");
        log.Check(app::ExplorerShortcut(L'N', true, false, false).action == app::ExplorerKeyAction::NewWindow,
                  "Ctrl+N opens a new window");
        ui::FluentMenuItem heading;
        heading.heading = true;
        ui::FluentMenuItem row;
        ui::FluentMenuSwatch tile;
        tile.command = 1;
        tile.label = L"Layout";
        row.quick_swatches.push_back(tile);
        ui::FluentMenuModel model;
        model.SetItems({heading, row});
        model.Layout(nullptr, 1.0f);
        log.Check(model.RowHeightPx(1) == 72.0f && model.FirstEnabled() == 1,
                  "layout tiles have space and keyboard skips heading");
    }
    const auto before_windows = Windows(original);
    if (before_windows.size() != 1) {
        log.Check(false, "fixture requires one initial isolated window");
        std::fclose(log.file);
        return 2;
    }
    const auto prefs = original.appPrefs;
    WINDOWPLACEMENT original_placement{sizeof(original_placement)};
    GetWindowPlacement(original.hwnd, &original_placement);
    const bool original_visible = IsWindowVisible(original.hwnd) != FALSE;
    original.appPrefs.persist = false;
    original.appPrefs.arrange_include_minimized = false;
    original.appPrefs.arrange_gap = 0;
    SetWindowArrangeAnimations(false);
    ShowWindow(original.hwnd, SW_HIDE);
    MONITORINFO monitor{sizeof(monitor)};
    const HMONITOR target_monitor = MonitorFromWindow(original.hwnd, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(target_monitor, &monitor);
    const auto make_window = [&]() {
        WindowInit init;
        init.monitor = target_monitor;
        init.rect = RECT{monitor.rcWork.left + 20, monitor.rcWork.top + 20, monitor.rcWork.left + 720,
                         monitor.rcWork.top + 560};
        HWND hwnd = OpenPulseWindow(original, init);
        if (auto *state = original.core.windows.Lookup(hwnd)) {
            state->isolatedTest = true;
            // WM_CREATE establishes the target monitor DPI. A 540px fixture is
            // below the 630px minimum at 150%, and SetWindowPlacement correctly
            // grows that invalid baseline when undo restores it.
            MINMAXINFO minimum{};
            SendMessageW(hwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&minimum));
            const RECT bounds = Bounds(hwnd);
            SetWindowPos(hwnd, nullptr, bounds.left, bounds.top,
                         (std::max)(bounds.right - bounds.left, minimum.ptMinTrackSize.x),
                         (std::max)(bounds.bottom - bounds.top, minimum.ptMinTrackSize.y),
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return hwnd;
    };
    const HWND first = make_window();
    AppState *fixture = original.core.windows.Lookup(first);
    log.Check(fixture != nullptr, "create isolated fixture window");
    if (fixture) {
        {
            const auto routing_prefs = original.appPrefs;
            const auto base_count = original.core.windows.Count();
            const auto base_tabs = fixture->window_tabs.items.size();
            original.core.windows.Touch(first);
            original.appPrefs.open_pulse_icon = 2;
            const auto activated = OpenExternalFolder(original, L"");
            log.Check(activated.status == ExternalOpenStatus::Dispatched && (activated.target ? activated.target->hwnd : nullptr) == first && original.core.windows.Count() == base_count &&
                          fixture->window_tabs.items.size() == base_tabs,
                      "icon activate selects MRU without adding window or tab");
            const HWND peer = make_window();
            if (auto *target = original.core.windows.Lookup(peer)) {
                original.appPrefs.open_pulse_icon = 1;
                original.appPrefs.new_window_shows = 1;
                for (const int show : {SW_HIDE, SW_MINIMIZE}) {
                    ShowWindow(peer, show);
                    original.core.windows.Touch(peer);
                    log.Check(original.core.windows.MostRecent() == target,
                              "launch fixture hidden or minimized peer is MRU before routing");
                    const auto old_tabs = target->window_tabs.items.size();
                    const HWND before_mru = original.core.windows.MostRecentHwnd();
                    const int before_iconic = IsIconic(peer);
                    const int before_visible = IsWindowVisible(peer);
                    const auto opened = OpenExternalFolder(*fixture, L"");
                    std::fprintf(log.file,
                        "[DIAG] launch show=%d peer=%p mru_before=%p actual=%p tabs_before=%zu tabs_after=%zu "
                        "iconic_before=%d iconic_after=%d visible_before=%d visible_after=%d rename=%d enabled=%d capture=%p\n",
                        show, static_cast<void*>(peer), static_cast<void*>(before_mru), static_cast<void*>((opened.target ? opened.target->hwnd : nullptr)),
                        old_tabs, target->window_tabs.items.size(), before_iconic, IsIconic(peer), before_visible,
                        IsWindowVisible(peer), target->renameIndex, IsWindowEnabled(peer), static_cast<void*>(GetCapture()));
                    log.Check(opened.status == ExternalOpenStatus::Dispatched && (opened.target ? opened.target->hwnd : nullptr) == peer && target->window_tabs.items.size() == old_tabs + 1 &&
                                  IsWindowVisible(peer) && !IsIconic(peer),
                              "icon new tab restores hidden or minimized MRU target");
                }
                DestroyWindow(peer);
            } else
                log.Check(false, "create launch routing MRU peer");
            wchar_t test_directory[32768]{};
            GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", test_directory, ARRAYSIZE(test_directory));
            OpenTabAt(*fixture, test_directory);
            const auto *current = ActiveTab(*fixture);
            const std::wstring current_path = current ? current->current_path : std::wstring();
            original.appPrefs.open_pulse_icon = 0;
            original.appPrefs.new_window_shows = 2;
            original.core.windows.Touch(first);
            const auto current_window = OpenExternalFolder(*fixture, L"");
            const HWND current_hwnd = (current_window.target ? current_window.target->hwnd : nullptr);
            const auto *current_folder = current_window.target ? ActiveTab(*current_window.target) : nullptr;
            log.Check(current_window.status == ExternalOpenStatus::Dispatched && current_hwnd && current_hwnd != first && original.core.windows.Count() == base_count + 1 && current_folder &&
                          current_folder->current_path == current_path,
                      "icon new window uses current folder preference");
            if (current_hwnd != first && current_hwnd != original.hwnd)
                DestroyWindow(current_hwnd);
            original.appPrefs.new_window_shows = 1;
            original.core.windows.Touch(first);
            const auto pc_window = OpenExternalFolder(*fixture, L"");
            const HWND pc_hwnd = (pc_window.target ? pc_window.target->hwnd : nullptr);
            const auto *pc_folder = pc_window.target ? ActiveTab(*pc_window.target) : nullptr;
            log.Check(pc_window.status == ExternalOpenStatus::Dispatched && pc_hwnd && pc_hwnd != first && pc_folder && pc_folder->current_path.empty(),
                      "icon new window uses This PC preference");
            if (pc_hwnd != first && pc_hwnd != original.hwnd)
                DestroyWindow(pc_hwnd);
            original.appPrefs.open_pulse_icon = 2;
            original.appPrefs.new_window_shows = 2;
            original.core.windows.Touch(first);
            const auto explicit_pc = OpenExternalFolder(*fixture, L"pulse:this-pc");
            const HWND explicit_hwnd = (explicit_pc.target ? explicit_pc.target->hwnd : nullptr);
            const auto *explicit_folder = explicit_pc.target ? ActiveTab(*explicit_pc.target) : nullptr;
            log.Check(explicit_pc.status == ExternalOpenStatus::Dispatched && explicit_hwnd && explicit_hwnd != first && explicit_folder && explicit_folder->current_path.empty(),
                      "explicit This PC overrides icon activation and current folder preference");
            if (explicit_hwnd != first && explicit_hwnd != original.hwnd)
                DestroyWindow(explicit_hwnd);
            original.appPrefs.open_pulse_icon = 1;
            original.core.windows.Touch(first);
            const auto busy_count = original.core.windows.Count();
            const auto busy_tabs = fixture->window_tabs.items.size();
            const auto original_tabs = original.window_tabs.items.size();
            const bool first_enabled = IsWindowEnabled(first) != FALSE;
            const bool original_enabled = IsWindowEnabled(original.hwnd) != FALSE;
            HWND modal = CreateWindowW(L"STATIC", L"Pulse external-open test modal", WS_POPUP | WS_CAPTION | WS_VISIBLE,
                monitor.rcWork.left + 60, monitor.rcWork.top + 60, 300, 100, first, nullptr, GetModuleHandleW(nullptr), nullptr);
            log.Check(modal != nullptr, "create owned modal fixture");
            if (modal) {
                EnableWindow(first, FALSE); EnableWindow(original.hwnd, FALSE);
                const auto busy = OpenExternalFolder(*fixture, L"");
                log.Check(busy.status == ExternalOpenStatus::Deferred && !busy.target &&
                    original.core.windows.Count() == busy_count && fixture->window_tabs.items.size() == busy_tabs &&
                    original.window_tabs.items.size() == original_tabs && IsWindowVisible(modal) && IsWindowEnabled(modal) &&
                    !IsWindowEnabled(first) && !IsWindowEnabled(original.hwnd) && GetWindow(modal, GW_OWNER) == first,
                    "all busy launch defers without new window tab or interruption of owned modal");
                using Coordinator = app::SingleInstanceCoordinator;
                Coordinator::OpenRequest request;
                GUID id{};
                const HRESULT generated = CoCreateGuid(&id);
                if (SUCCEEDED(generated)) std::memcpy(request.id.data(), &id, sizeof(id));
                request.deadline = GetTickCount64() + 10000;
                request.path = L"";
                request.delivery = std::make_shared<std::atomic<Coordinator::OpenDeliveryState>>(Coordinator::OpenDeliveryState::Pending);
                const auto submitted = SUCCEEDED(generated) ? original.single_instance.SubmitOpenRequest(request,
                    GetTickCount64(), original.core.host.Hwnd(), WM_OPEN_EXTERNAL) : Coordinator::OpenSubmission::Invalid;
                log.Check(submitted == Coordinator::OpenSubmission::Accepted, "owned dispatcher accepts one bounded request while all windows busy");
                if (submitted == Coordinator::OpenSubmission::Accepted) {
                    DispatchQueuedExternalOpen(original);
                    log.Check(request.delivery->load() == Coordinator::OpenDeliveryState::Queued &&
                        original.core.windows.Count() == busy_count && fixture->window_tabs.items.size() == busy_tabs &&
                        original.window_tabs.items.size() == original_tabs && IsWindowVisible(modal) && IsWindowEnabled(modal),
                        "real dispatcher retains busy request without completing or disrupting modal");
                }
                DestroyWindow(modal);
                EnableWindow(first, first_enabled); EnableWindow(original.hwnd, original_enabled);
                original.core.windows.Touch(first);
                if (submitted == Coordinator::OpenSubmission::Accepted) {
                    DispatchQueuedExternalOpen(original);
                    log.Check(request.delivery->load() == Coordinator::OpenDeliveryState::Dispatched &&
                        original.core.windows.Count() == busy_count && fixture->window_tabs.items.size() == busy_tabs + 1,
                        "real dispatcher resumes exactly one tab after modal releases owner");
                    const auto duplicate = original.single_instance.SubmitOpenRequest(request, GetTickCount64(),
                        original.core.host.Hwnd(), WM_OPEN_EXTERNAL);
                    DispatchQueuedExternalOpen(original);
                    log.Check(duplicate == Coordinator::OpenSubmission::Duplicate &&
                        original.core.windows.Count() == busy_count && fixture->window_tabs.items.size() == busy_tabs + 1,
                        "same owned request and extra wake do not dispatch a second time");
                }
            }
            const auto explicit_before = original.core.windows.Count();
            const auto explicit_one = OpenExternalFolder(*fixture, L"pulse:new-window");
            const auto explicit_two = OpenExternalFolder(*fixture, L"pulse:new-window");
            const HWND explicit_one_hwnd = explicit_one.target ? explicit_one.target->hwnd : nullptr;
            const HWND explicit_two_hwnd = explicit_two.target ? explicit_two.target->hwnd : nullptr;
            log.Check(explicit_one.status == ExternalOpenStatus::Dispatched && explicit_two.status == ExternalOpenStatus::Dispatched &&
                explicit_one_hwnd && explicit_two_hwnd && explicit_one_hwnd != explicit_two_hwnd &&
                explicit_one_hwnd != first && explicit_two_hwnd != first && original.core.windows.Count() == explicit_before + 2,
                "explicit new-window requests remain distinct rather than being merged");
            for (HWND opened : {explicit_one_hwnd, explicit_two_hwnd})
                if (opened && opened != first && opened != original.hwnd && IsWindow(opened)) DestroyWindow(opened);
            while (fixture->window_tabs.items.size() > base_tabs)
                CloseLayoutTab(*fixture, fixture->window_tabs.items.size() - 1);
            original.appPrefs = routing_prefs;
            ShowWindow(original.hwnd, SW_HIDE);
            ShowWindow(first, SW_SHOWNOACTIVATE);
            original.core.windows.Touch(first);
            log.Check(original.core.windows.Count() == base_count && fixture->window_tabs.items.size() == base_tabs,
                      "launch routing fixtures restore window and tab counts");
        }
        if (notice_inspect) {
            // Visual fixture inputs, not synthetic queue-delivery success. The
            // normal Tick path selects production localized text and draws Toast.
            const auto deadline = GetTickCount64() + 60000;
            bool seeded = false, captured = false, painted = false, quit = false;
            uint64_t visible_since = 0;
            const auto screenshot = inspect_directory / (L"external-open-notice-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(GetTickCount64()) + L".png");
            SetWindowTextW(first, L"Pulse external-open notice visual inspection");
            ShowWindow(first, SW_SHOW);
            original.core.windows.Touch(first);
            std::fprintf(log.file, "[INSPECT] up to 60 seconds; close only this fixture to finish early; fixture counts timeout=2 failure=1\n");
            std::fflush(log.file);
            while (GetTickCount64() < deadline && IsWindow(first) && !quit) {
                // Lookup afresh: user WM_CLOSE may destroy the AppState.
                auto* current_fixture = original.core.windows.Lookup(first);
                if (!current_fixture) break;
                if (!seeded && !current_fixture->notification_toast.IsVisible()) {
                    original.core.open_failure_notice.TimedOut(false);
                    original.core.open_failure_notice.TimedOut(false);
                    original.core.open_failure_notice.Failed(false);
                    seeded = true;
                    TickExternalOpen(*current_fixture);
                }
                if (seeded && current_fixture->notification_toast.IsVisible() && !visible_since) {
                    visible_since = GetTickCount64();
                    std::fprintf(log.file, "[INSPECT] actual mixed-failure toast visible; dpi_scale=%.3f\n", current_fixture->scale);
                    std::fflush(log.file);
                }
                if (!captured && painted && visible_since && GetTickCount64() - visible_since >= 500 &&
                    current_fixture->notification_toast.IsVisible()) {
                    const auto bounds = current_fixture->notification_toast.Bounds();
                    const bool saved = current_fixture->compositor.SaveSnapshot(screenshot.c_str());
                    captured = true;
                    log.Check(saved, "save actual rendered notice surface (visual review still required)");
                    std::fprintf(log.file, "[INSPECT] bounds=(%.1f,%.1f,%.1f,%.1f); snapshot path recorded below as UTF-16 code units\n",
                        bounds.left, bounds.top, bounds.right, bounds.bottom);
                    for (wchar_t ch : screenshot.wstring()) std::fprintf(log.file, "\\u%04X", static_cast<unsigned>(ch));
                    std::fprintf(log.file, "\n"); std::fflush(log.file);
                }
                MSG message{};
                unsigned pumped = 0;
                while (pumped++ < 64 && GetTickCount64() < deadline && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); quit = true; break; }
                    const bool own_paint = message.hwnd == first && message.message == WM_PAINT;
                    TranslateMessage(&message); DispatchMessageW(&message);
                    if (own_paint && visible_since) painted = true;
                    if (!IsWindow(first)) break;
                }
                if (!quit && IsWindow(first)) MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
            log.Check(seeded && visible_since != 0, "normal TickExternalOpen displayed mixed timeout/failure fixture");
            if (!captured) std::fprintf(log.file, "[INSPECT] no surface captured before close/deadline; not visual acceptance\n");
            std::fflush(log.file);
            fixture = original.core.windows.Lookup(first);
        }
        if (!external_only) {
            const auto positions = [&](app::WindowArrangeLayout layout, size_t count) {
                MINMAXINFO minimum{};
                SendMessageW(first, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&minimum));
                const int dpi = static_cast<int>(96.0f * fixture->scale);
                return app::CalculateWindowArrangement(layout, monitor.rcWork, count, 0, minimum.ptMinTrackSize.x,
                                                       minimum.ptMinTrackSize.y, MulDiv(32, dpi, 96));
            };
            const auto destroy_extras = [&]() {
                CleanupWindowArrange();
                for (HWND hwnd : Windows(original))
                    if (hwnd != original.hwnd && hwnd != first)
                        DestroyWindow(hwnd);
            };
            const auto layout = app::WindowArrangeLayout::SideBySide;
            if (!positions(layout, 2).empty()) {
                const auto count = original.core.windows.Count();
                const auto *initial_tab = fixture->window_tabs.Active();
                log.Check(!ArrangeWindows(*fixture, layout), "single window arrangement is rejected");
                log.Check(original.core.windows.Count() == count && fixture->window_tabs.Active() == initial_tab,
                          "rejected arrangement preserves window and tab");
                destroy_extras();
                OpenTabAt(*fixture, L"");
                std::vector<const app::LayoutTab *> tabs;
                for (const auto &tab : fixture->window_tabs.items)
                    tabs.push_back(tab.get());
                log.Check(!ArrangeWindows(*fixture, layout), "single window with tabs is rejected");
                bool restored = fixture->window_tabs.items.size() == tabs.size();
                for (size_t i = 0; restored && i < tabs.size(); ++i)
                    restored = fixture->window_tabs.items[i].get() == tabs[i];
                log.Check(restored && original.core.windows.Count() == count, "rejected arrangement preserves tab identity and order");
                destroy_extras();
            } else
                log.Skip("single-window arrangement rejection");
            const HWND second = make_window();
            const HWND third = make_window();
            const std::vector<HWND> order{first, second, third};
            if (second && third) {
                struct Foreign {
                    HWND hwnd = nullptr;
                    RECT rect{};
                } foreign;
                EnumWindows(
                    [](HWND hwnd, LPARAM parameter) -> BOOL {
                        DWORD pid = 0;
                        GetWindowThreadProcessId(hwnd, &pid);
                        if (pid != GetCurrentProcessId() && IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
                            auto &value = *reinterpret_cast<Foreign *>(parameter);
                            value.hwnd = hwnd;
                            GetWindowRect(hwnd, &value.rect);
                            return FALSE;
                        }
                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(&foreign));
                for (const auto kind : {app::WindowArrangeLayout::SideBySide, app::WindowArrangeLayout::Stacked,
                                        app::WindowArrangeLayout::Columns, app::WindowArrangeLayout::Tile,
                                        app::WindowArrangeLayout::Main, app::WindowArrangeLayout::Cascade}) {
                    std::fprintf(log.file, "Layout: %d\n", static_cast<int>(kind));
                    const auto expected = positions(kind, order.size());
                    if (expected.empty()) {
                        log.Skip("layout");
                        continue;
                    }
                    std::vector<RECT> initial;
                    for (HWND hwnd : order)
                        initial.push_back(Bounds(hwnd));
                    log.Check(ArrangeWindows(*fixture, kind, false, order), "arrange real windows");
                    bool actual_matches = true;
                    for (size_t i = 0; i < order.size(); ++i)
                        actual_matches &= Same(Bounds(order[i]), i < expected.size() ? expected[i] : initial[i]);
                    log.Check(actual_matches, "GetWindowRect matches arrangement algorithm");
                    Undo(*fixture);
                    bool undo_matches = true;
                    for (size_t i = 0; i < order.size(); ++i) {
                        const RECT actual = Bounds(order[i]);
                        const RECT before = initial[i];
                        const bool matches = Same(actual, before);
                        if (!matches) {
                            WINDOWPLACEMENT placement{sizeof(placement)};
                            GetWindowPlacement(order[i], &placement);
                            const RECT normal = placement.rcNormalPosition;
                            std::fprintf(log.file, "Undo window %zu expected=(%ld,%ld,%ld,%ld) actual=(%ld,%ld,%ld,%ld) normal=(%ld,%ld,%ld,%ld) show=%u flags=%u\n",
                                i, before.left, before.top, before.right, before.bottom,
                                actual.left, actual.top, actual.right, actual.bottom,
                                normal.left, normal.top, normal.right, normal.bottom,
                                placement.showCmd, placement.flags);
                        }
                        undo_matches &= matches;
                    }
                    log.Check(undo_matches, "undo restores real window rectangles");
                }
                if (foreign.hwnd && IsWindow(foreign.hwnd))
                    log.Check(original.core.windows.Lookup(foreign.hwnd) == nullptr &&
                                  Same(Bounds(foreign.hwnd), foreign.rect),
                              "foreign process window excluded and unchanged");
                else
                    std::fprintf(log.file, "[SKIP] no stable visible foreign process window\n");
                if (!positions(layout, 2).empty()) {
                    ShowWindow(third, SW_MINIMIZE);
                    log.Check(ArrangeWindows(*fixture, layout, false, order), "arrange excludes minimized window");
                    log.Check(IsIconic(third) != FALSE, "excluded window remains minimized");
                    Undo(*fixture);
                    if (!positions(app::WindowArrangeLayout::Tile, 4).empty()) {
                        // force_include includes the hidden original fixture owner too.
                        log.Check(ArrangeWindows(*fixture, app::WindowArrangeLayout::Tile, false, order, true),
                                  "force include arrangement");
                        log.Check(!IsIconic(third) && IsWindowVisible(third), "force include restores minimized window");
                        Undo(*fixture);
                        log.Check(IsIconic(third) != FALSE && !IsWindowVisible(original.hwnd),
                                  "undo restores minimized and hidden state");
                    } else
                        log.Skip("force include");
                    ShowWindow(third, SW_RESTORE);
                    if (!positions(layout, order.size()).empty()) {
                        ShowWindow(first, SW_MAXIMIZE);
                        const RECT maximized = Bounds(first);
                        log.Check(ArrangeWindows(*fixture, layout, false, order), "arrange maximized window");
                        Undo(*fixture);
                        log.Check(IsZoomed(first) && Same(Bounds(first), maximized),
                                  "undo restores maximized state and rectangle");
                    } else
                        log.Skip("maximized undo");
                }
            } else
                log.Check(false, "create arrangement peers");
        }
    }
    CleanupWindowArrange();
    for (HWND hwnd : Windows(original))
        if (std::find(before_windows.begin(), before_windows.end(), hwnd) == before_windows.end())
            DestroyWindow(hwnd);
    original.appPrefs = prefs;
    SetWindowPlacement(original.hwnd, &original_placement);
    if (!original_visible)
        ShowWindow(original.hwnd, SW_HIDE);
    SetWindowArrangeAnimations(true);
    log.Check(Windows(original).size() == before_windows.size(), "cleanup removes all generated windows");
    std::fprintf(log.file, "Failures: %d\n", log.failed);
    std::fclose(log.file);
    return log.failed ? 1 : 0;
}
int RunWindowManagementTest(AppState& original, const std::wstring& report_path) {
    if (GetEnvironmentVariableW(L"PULSE_TEST_EXPLORER_ROLLBACK_ONLY", nullptr, 0)) {
        Report log;
        if (_wfopen_s(&log.file, report_path.c_str(), L"wb") != 0 || !log.file) return 2;
        if (!original.isolatedTest || !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0))
            log.Check(false, "rollback probe requires isolated instance and data directory");
        else CheckExplorerRollback(original, log);
        std::fprintf(log.file, "Failures: %d\n", log.failed);
        std::fclose(log.file);
        return log.failed ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_GLOBAL_SEARCH_ROUTING_ONLY", nullptr, 0)) {
        Report log;
        if (_wfopen_s(&log.file, report_path.c_str(), L"wb") != 0 || !log.file) return 2;
        if (!original.isolatedTest || !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0))
            log.Check(false, "requires isolated test instance and PULSE_TEST_DATA_DIR");
        else CheckGlobalSearchRouting(original, log);
        std::fprintf(log.file, "Failures: %d\n", log.failed);
        std::fclose(log.file);
        return log.failed ? 1 : 0;
    }
    if (!GetEnvironmentVariableW(L"PULSE_TEST_WIN_E_ACTIVATE_ONLY", nullptr, 0))
        return RunWindowManagementFullTest(original, report_path);
    Report log;
    if (_wfopen_s(&log.file, report_path.c_str(), L"wb") != 0 || !log.file) return 2;
    if (!original.isolatedTest || !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0)) {
        log.Check(false, "requires isolated test instance and PULSE_TEST_DATA_DIR");
    } else {
        CheckWinEActivate(original, log);
    }
    std::fprintf(log.file, "Failures: %d\n", log.failed);
    std::fclose(log.file);
    return log.failed ? 1 : 0;
}
} // namespace pulse

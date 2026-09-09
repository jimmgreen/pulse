// selftest_1b2.cpp — Stage 1B-2 console self-test.
//
// Headless coverage for the parts `--shot` cannot reach (interactive menus,
// OLE drag & drop): menu model construction + hit-test + verb dispatch,
// IDataObject CF_HDROP contents, drop-effect modifier semantics, breadcrumb
// splitting, and real ops-layer create/move through pulse_shell.exe.
// All file operations are confined to bench_data/opstest/selftest_1b2 and
// cleaned up afterwards.
#include "selftest_1b2.h"
#include "quick_access.h"
#include "../common/windows_compat.h"
#include "app_input.h"
#include "app_hosted_edit.h"
#include "app_navigation.h"
#include "../ui/address_search_layout.h"
#include "search_query.h"
#include "../common/localization.h"
#include "app_model.h"
#include "app_worker.h"
#include "snapshot_patch.h"
#include "session.h"
#include "app_prefs.h"
#include "context_menu.h"
#include "context_menu_prefs.h"
#include "shell_verbs.h"
#include "places.h"
#include "batch_rename.h"
#include "link_resolve.h"
#include "details_meta.h"
#include "../ipc/ctx_menu_util.h"
#include "../index/index_engine.h"
#include "../fs/fs_enum.h"
#include "../fs/fs_recycle.h"
#include "../fs/fs_snapshot.h"
#include "../fs/fs_watch.h"
#include "../fs/fs_net_cache.h"
#include "../ui/fluent_menu.h"
#include "../ui/typography.h"
#include "../ui/color_picker.h"
#include "../ui/bloom_accent_picker.h"
#include "../ui/drag_drop.h"
#include "../ui/ui_renderer.h"
#include "../ops/ops_manager.h"
#include "../ops/clipboard.h"
#include "../common/text_format.h"
#include "../common/utf8_file.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <dwrite_3.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pulse::app {

namespace {

bool IsFile(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring WorkspaceRoot() {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (length == 0 || length >= ARRAYSIZE(path)) return L".";

    std::wstring current(path, length);
    const size_t executable_separator = current.find_last_of(L"\\/");
    if (executable_separator == std::wstring::npos) return L".";
    current.resize(executable_separator);

    // Support both Ninja's build/pulse.exe and multi-config build/Release/pulse.exe.
    for (int depth = 0; depth < 4; ++depth) {
        if (IsFile(current + L"\\CMakeLists.txt") &&
            IsFile(current + L"\\src\\app\\selftest_1b2.cpp")) {
            return current;
        }
        const size_t separator = current.find_last_of(L"\\/");
        if (separator == std::wstring::npos) break;
        current.resize(separator);
    }
    return L".";
}

std::wstring WorkspacePath(const wchar_t* relative) {
    return WorkspaceRoot() + L"\\" + relative;
}

const std::wstring kOpsTestRoot = WorkspacePath(L"bench_data\\opstest");
const std::wstring kSandbox = kOpsTestRoot + L"\\selftest_1b2";
const std::wstring kLogPath = WorkspacePath(L"bench_data\\selftest_1b2_last.log");

int g_pass = 0;
int g_fail = 0;
FILE* g_log = nullptr; // also mirror output here (no console when piped)

void LogLine(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf, fmt, ap);
    va_end(ap);
    wprintf(L"%s", buf);
    if (g_log) {
        fwprintf(g_log, L"%s", buf);
        fflush(g_log);
    }
}

void Check(bool cond, const wchar_t* name) {
    if (cond) {
        ++g_pass;
        LogLine(L"[PASS] %s\n", name);
    } else {
        ++g_fail;
        LogLine(L"[FAIL] %s\n", name);
    }
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

ops::OpsManager g_ops;

bool WaitOpDone(uint64_t prev_completed, int timeout_ms = 60000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (g_ops.Status().completed_ops > prev_completed) return true;
        Sleep(10);
    }
    return false;
}

ops::OpStatus RunOp(ops::OpRequest req) {
    uint64_t prev = g_ops.Status().completed_ops;
    g_ops.Submit(std::move(req));
    if (!WaitOpDone(prev)) fprintf(stderr, "[selftest] op TIMEOUT\n");
    return g_ops.Status();
}

void CleanSandbox() {
    // Shallow recursive delete, sandbox only.
    std::wstring pat = std::wstring(kSandbox) + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        std::wstring p = std::wstring(kSandbox) + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::wstring sub = p + L"\\*";
            WIN32_FIND_DATAW fd2{};
            HANDLE h2 = FindFirstFileW(sub.c_str(), &fd2);
            if (h2 != INVALID_HANDLE_VALUE) {
                do {
                    if (fd2.cFileName[0] == L'.') continue;
                    DeleteFileW((p + L"\\" + fd2.cFileName).c_str());
                } while (FindNextFileW(h2, &fd2));
                FindClose(h2);
            }
            RemoveDirectoryW(p.c_str());
        } else {
            DeleteFileW(p.c_str());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// --- individual test groups -------------------------------------------------

void TestBreadcrumb() {
    auto segs = ui::SplitBreadcrumb(L"C:\\Users\\SS\\Desktop");
    Check(segs.size() == 5, L"breadcrumb: C:\\Users\\SS\\Desktop -> 5 segments");
    if (segs.size() == 5) {
        Check(segs[0].text == L"此电脑" && segs[0].path.empty(),
              L"breadcrumb: This PC root segment");
        Check(segs[1].text == L"C:" && segs[1].path == L"C:\\", L"breadcrumb: drive segment");
        Check(segs[2].path == L"C:\\Users" && segs[4].path == L"C:\\Users\\SS\\Desktop",
              L"breadcrumb: cumulative paths");
    }
    auto one = ui::SplitBreadcrumb(L"C:\\");
    Check(one.size() == 2 && one[0].path.empty() && one[1].path == L"C:\\",
          L"breadcrumb: drive root sits under This PC");
    auto pc = ui::SplitBreadcrumb(L"");
    Check(pc.size() == 1 && pc[0].text == L"此电脑" && pc[0].path.empty(),
          L"breadcrumb: empty path -> This PC only");
    auto unc = ui::SplitBreadcrumb(L"\\\\server\\share\\dir");
    Check(unc.size() == 3 && unc[0].text == L"server" && unc[0].path == L"\\\\server" &&
          unc[1].text == L"share" && unc[1].path == L"\\\\server\\share" &&
          unc[2].path == L"\\\\server\\share\\dir",
          L"breadcrumb: UNC splits server and share segments");
    auto uncRoot = ui::SplitBreadcrumb(L"\\\\server\\share");
    Check(uncRoot.size() == 2 && uncRoot[0].path == L"\\\\server" &&
          uncRoot[1].path == L"\\\\server\\share",
          L"breadcrumb: UNC share root -> server + share");
    auto uncSrv = ui::SplitBreadcrumb(L"\\\\server");
    Check(uncSrv.size() == 1 && uncSrv[0].text == L"server" &&
          uncSrv[0].path == L"\\\\server",
          L"breadcrumb: bare UNC server -> single segment");
    auto uncLong = ui::SplitBreadcrumb(L"\\\\?\\UNC\\192.168.0.254\\share\\dir");
    Check(uncLong.size() == 3 && uncLong[0].path == L"\\\\192.168.0.254" &&
          uncLong[1].path == L"\\\\192.168.0.254\\share" &&
          uncLong[2].path == L"\\\\192.168.0.254\\share\\dir",
          L"breadcrumb: \\\\?\\UNC prefix restores leading \\\\");
    auto longp = ui::SplitBreadcrumb(L"\\\\?\\C:\\A\\B");
    Check(longp.size() == 4 && longp[3].path == L"C:\\A\\B",
          L"breadcrumb: long-path prefix stripped");
    const std::wstring search_path =
        L"pulse:search:path:C:\\Users\\SS\\Desktop\\PulseSearchTest content:\u53d1\u7968";
    auto search = ui::SplitBreadcrumb(search_path);
    Check(search.size() == 1, L"breadcrumb: search query is a single segment");
    if (search.size() == 1) {
        Check(search[0].path == search_path, L"breadcrumb: search click keeps pulse:search:");
        Check(search[0].text.find(L'\\') == std::wstring::npos,
              L"breadcrumb: search label has no backslash");
    }

    Pane pane;
    pane.NewTab(L"\\\\192.168.0.254\\share\\folder");
    SidebarModel sidebar;
    const auto vm = BuildWindowViewModel(
        pane, sidebar, true, false, true, nullptr, 0);
    Check(vm.pane.path == L"\\\\192.168.0.254\\share\\folder",
          L"breadcrumb: pane display path keeps UNC prefix");
    auto fromVm = ui::SplitBreadcrumb(vm.pane.path);
    Check(fromVm.size() == 3 && fromVm[0].path == L"\\\\192.168.0.254" &&
          fromVm[1].path == L"\\\\192.168.0.254\\share" &&
          fromVm[2].path == L"\\\\192.168.0.254\\share\\folder",
          L"breadcrumb: click target stays UNC not CWD-relative");

    // UNC workspace gets the network glyph/color and a 服务器 badge.
    auto colorIs = [](const D2D1_COLOR_F& c, uint32_t rgb) {
        const auto want = ui::HexColor(rgb);
        return c.r == want.r && c.g == want.g && c.b == want.b;
    };
    PlacesCatalog cat;
    cat.persist = false;
    cat.PinWorkspace(L"C:\\local", L"local", 0, { L"C:\\local" });
    cat.PinWorkspace(L"\\\\server\\share", L"nas", 0, { L"\\\\server\\share" });
    cat.workspaces[1].frequent.push_back({ L"\\\\server\\share\\sub", 3 });
    const auto wvm = BuildWindowViewModel(pane, sidebar, true, false, true, &cat, 0);
    Check(!wvm.sidebar.empty() && wvm.sidebar[0].items.size() == 3,
          L"sidebar: workspace group holds both workspaces + frequent child");
    if (!wvm.sidebar.empty() && wvm.sidebar[0].items.size() == 3) {
        const auto& local = wvm.sidebar[0].items[0];
        const auto& nas = wvm.sidebar[0].items[1];
        const auto& sub = wvm.sidebar[0].items[2];
        Check(local.badge.empty() && local.icon_glyph == L"\xE8B7",
              L"sidebar: local workspace keeps default glyph, no badge");
        Check(nas.badge == L"当前 · 服务器" && nas.icon_glyph == L"\xE968" &&
              colorIs(nas.icon_color, 0x38BDF8),
              L"sidebar: active UNC workspace shows server badge + network glyph");
        Check(sub.badge.empty() && sub.icon_glyph == L"\xE968" &&
              colorIs(sub.icon_color, 0x38BDF8),
              L"sidebar: UNC frequent child gets network glyph, no badge");
    }

    const auto access = BuildSidebarModel();
    const ui::SidebarItem* desktop = nullptr;
    Pane home;
    home.NewTab(L"C:\\");
    const auto avm = BuildWindowViewModel(home, access, true, false, false, nullptr, 0);
    for (const auto& group : avm.sidebar) {
        if (group.header != L"\u5FEB\u901F\u8BBF\u95EE") continue; // 快速访问
        for (const auto& item : group.items) {
            if (item.label == L"\u684C\u9762") { // 桌面
                desktop = &item;
                break;
            }
        }
    }
    Check(desktop && desktop->badge == L"\u684C\u9762" &&
          desktop->badge_color.a > 0.0f && !desktop->path.empty(),
          L"sidebar: quick access Desktop exposes editable badge color");
}

void TestThisPcEnumeration() {
    std::vector<fs::DirEntry> entries;
    fs::EnumerateDirectory(L"", entries);
    Check(!entries.empty(), L"thispc: empty path enumerates at least one drive");
    bool all_dirs = true;
    bool has_c = false;
    for (const auto& e : entries) {
        if (!e.is_dir) all_dirs = false;
        if (e.name.find(L"C:") != std::wstring::npos ||
            e.full_path.starts_with(L"\\\\?\\C:\\"))
            has_c = true;
    }
    Check(all_dirs, L"thispc: all entries are directories");
    Check(has_c, L"thispc: contains the C: drive");
}

void TestLoadingPresentation() {
    Pane pane;
    pane.NewTab(L"C:\\pending-folder");
    SidebarModel sidebar;
    const auto vm = BuildWindowViewModel(
        pane, sidebar, true, false, true, nullptr, 0);
    Check(vm.pane.header_text == L"pending-folder",
          L"loading: directory title remains visible");
    Check(vm.status.status_text.empty(),
          L"loading: normal directory does not show search-like status");
}

LRESULT CALLBACK NavigationTestProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_APPCOMMAND && state && HandleBrowserNavigation(*state, lparam)) return TRUE;
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void TestMouseHistoryNavigation() {
    auto state = std::make_unique<AppState>();
    state->places.persist = false;
    Pane pane;
    pane.NewTab(L"pulse:settings/general");
    state->pane = &pane;
    auto* tab = pane.ActiveTab();
    tab->back_stack.push(L"pulse:settings/about");
    tab->back_stack.push(L"pulse:settings/appearance");
    WNDCLASSW wc{};
    wc.lpfnWndProc = NavigationTestProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseMouseHistorySelftest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP,
                                0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    Check(hwnd != nullptr, L"mouse history: hidden test window created");
    if (!hwnd) return;
    state->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.get()));
    SendMessageW(hwnd, WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON1, XBUTTON1), 0);
    Check(tab->back_stack.size() == 2, L"mouse history: button down does not navigate");
    SendMessageW(hwnd, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), 0);
    Check(tab->current_path == L"pulse:settings/appearance" && tab->back_stack.size() == 1 &&
          tab->forward_stack.size() == 1, L"mouse history: side button back navigates exactly once");
    SendMessageW(hwnd, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2), 0);
    Check(tab->current_path == L"pulse:settings/general" && tab->forward_stack.empty(),
          L"mouse history: side button forward restores location");
    SendMessageW(hwnd, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2), 0);
    Check(tab->current_path == L"pulse:settings/general" && tab->back_stack.size() == 2,
          L"mouse history: empty forward history is a no-op");
    HWND edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD, 0, 0, 1, 1,
                                hwnd, nullptr, wc.hInstance, nullptr);
    Check(edit != nullptr, L"mouse history: child edit created");
    if (edit) SendMessageW(edit, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), 0);
    Check(tab->current_path == L"pulse:settings/appearance",
          L"mouse history: side button over child edit reaches navigation");
    SendMessageW(hwnd, WM_APPCOMMAND, 0, MAKELPARAM(0, APPCOMMAND_BROWSER_FORWARD));
    Check(tab->current_path == L"pulse:settings/general",
          L"mouse history: driver browser command navigates forward");
    Check(!HandleBrowserNavigation(*state, MAKELPARAM(0, APPCOMMAND_VOLUME_UP)),
          L"mouse history: unrelated commands remain unhandled");
    tab->back_stack = {};
    SendMessageW(hwnd, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), 0);
    Check(tab->current_path == L"pulse:settings/general",
          L"mouse history: empty back history is a no-op");
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
    state->hwnd = nullptr;
    state->pane = nullptr;
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

void TestNotificationToast() {
    HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
        0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, L"toast: hidden window created");
    if (!window) return;
    ui::Compositor compositor;
    if (compositor.Init(window)) {
        compositor.RecreateTextFormats(1.0f);
        ui::NotificationToast toast;
        toast.Show(window, L"Index", L"The original index is preserved.");
        compositor.Dc()->BeginDraw();
        toast.Draw(compositor, ui::MakeTheme(false, D2D1::ColorF(0x0078D4)), 1.0f, false);
        Check(SUCCEEDED(compositor.Dc()->EndDraw()), L"toast: Fluent card renders");
        const auto bounds = toast.Bounds();
        Check(bounds.left >= 0 && bounds.right <= 640 && bounds.top >= 0 && bounds.bottom <= 480,
              L"toast: card stays inside window");
        const auto body = MAKELPARAM(static_cast<int>(bounds.left + 60), static_cast<int>(bounds.top + 20));
        Check(toast.HandleMessage(window, WM_LBUTTONDOWN, 0, body), L"toast: body blocks underlying click");
        Check(toast.HandleMessage(window, WM_LBUTTONUP, 0, MAKELPARAM(0, 0)) && GetCapture() != window && toast.IsVisible(),
              L"toast: releasing outside clears capture without dismissing");
        const auto close = MAKELPARAM(static_cast<int>(bounds.right - 16), static_cast<int>((bounds.top + bounds.bottom) / 2));
        toast.HandleMessage(window, WM_LBUTTONDOWN, 0, close);
        toast.HandleMessage(window, WM_LBUTTONUP, 0, close);
        Check(!toast.IsVisible(), L"toast: close dismisses persistent message");
        compositor.Shutdown();
    } else {
        Check(false, L"toast: graphics initialized");
    }
    DestroyWindow(window);
}

void TestNavigationReturnSelection() {
    Check(NavigationReturnChildName(
              L"C:\\projects\\pulse", L"C:\\projects") == L"pulse",
          L"navigation: parent return selects the folder just left");
    Check(NavigationReturnChildName(
              L"C:\\projects\\pulse\\src\\app", L"C:\\projects") == L"pulse",
          L"navigation: deep ancestor return selects its immediate child");
    Check(NavigationReturnChildName(
              L"C:\\projects\\pulse", L"D:\\archive").empty(),
          L"navigation: unrelated destination has no return selection");
    Check(NavigationReturnChildName(
              L"\\\\192.168.0.254\\工程项目盘-2025\\00_软件",
              L"\\\\192.168.0.254\\工程项目盘-2025") == L"00_软件",
          L"navigation: UNC parent return selects the folder just left");
    Check(NavigationReturnChildName(
              L"\\\\192.168.0.254\\工程项目盘-2025\\00_软件\\工具",
              L"\\\\192.168.0.254\\工程项目盘-2025") == L"00_软件",
          L"navigation: deep UNC return selects the immediate child");
}

void TestAddressSearch() {
    bool layouts_ok = true;
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f, 2.5f}) {
        for (const float width : {104.0f, 180.0f, 259.0f, 260.0f, 419.0f, 420.0f, 800.0f}) {
            const auto field = D2D1::RectF(10.0f * scale, 10.0f * scale,
                                          (10.0f + width) * scale, 46.0f * scale);
            const auto layout = ui::LayoutAddressSearch(field, scale);
            layouts_ok &= layout.scope.right <= layout.input.left &&
                layout.input.right <= layout.clear.left && layout.clear.right <= layout.close.left &&
                layout.close.right <= field.right && layout.input.right - layout.input.left >= 39.9f * scale;
        }
    }
    Check(layouts_ok, L"address search: controls and input fit narrow and high-DPI layouts");
    auto state = std::make_unique<AppState>();
    Check(!state->addressSearchCurrent, L"address search: default scope is entire index");
    state->addressSearching = true;
    ULONGLONG now = 1000;
    bool bounded = true;
    for (int i = 0; i < 40; ++i) {
        const float before = state->addressSearchAnimation;
        TickAddressSearch(*state, now += 16);
        bounded &= state->addressSearchAnimation >= before && state->addressSearchAnimation <= 1.0f;
    }
    Check(bounded && state->addressSearchAnimation == 1.0f,
          L"address search: entrance animation settles without overshoot");
    state->addressSearching = false;
    state->addressScopeAnimation = 1.0f;
    for (int i = 0; i < 40; ++i) TickAddressSearch(*state, now += 16);
    Check(state->addressSearchAnimation == 0.0f && state->addressScopeAnimation == 0.0f &&
          !TickAddressSearch(*state, now + 16), L"address search: exit and scope feedback stop repainting");
    AdvancedSearchSpec spec;
    spec.name = L"report 2026";
    spec.current_folder = L"C:\\Users\\W\\Desktop";
    Check(SplitSearchQueryText(CompileSearchQuery(spec)).path_prefix.empty(),
          L"address search: entire index omits current directory constraint");
    spec.location = LocationScope::CurrentFolder;
    Check(!SplitSearchQueryText(CompileSearchQuery(spec)).path_prefix.empty(),
          L"address search: current folder adds recursive directory scope");
}

void TestContinuousSearch() {
    auto state = std::make_unique<AppState>();
    Pane first;
    Pane second;
    state->pane = &first;
    first.view.current_path = MakeSearchPath(L"contract path:C:\\Documents");
    ui::WindowViewModel vm;
    FillAddressSearchView(*state, vm);
    Check(vm.address_searching && vm.address_search_text == L"contract" && vm.address_search_current,
          L"continuous search: results restore query and current-folder scope");
    state->hwndAddressEdit = CreateWindowExW(0, L"EDIT", L"invoice", WS_POPUP,
                                            0, 0, 100, 30, nullptr, nullptr, nullptr, nullptr);
    Check(state->hwndAddressEdit != nullptr, L"continuous search: create hidden native edit fixture");
    if (state->hwndAddressEdit) {
        state->addressSearching = true;
        state->addressSearchRoot = L"C:\\Documents";
        state->addressSearchCurrent = false;
        SaveAddressSearchDraft(*state);
        state->addressSearching = false;
        vm = {};
        FillAddressSearchView(*state, vm);
        Check(vm.address_search_text == L"invoice" && !vm.address_search_current,
              L"continuous search: unsubmitted draft and changed scope survive blur");
        state->pane = &second;
        second.view.current_path = MakeSearchPath(L"other");
        vm = {};
        FillAddressSearchView(*state, vm);
        Check(vm.address_search_text == L"other" && !vm.address_search_current,
              L"continuous search: another tab has independent input");
        state->pane = &first;
        SetWindowTextW(state->hwndAddressEdit, L"");
        state->addressSearching = true;
        SaveAddressSearchDraft(*state);
        state->addressSearching = false;
        vm = {};
        FillAddressSearchView(*state, vm);
        Check(vm.address_search_text.empty() && !vm.address_search_has_text &&
              first.view.current_path == MakeSearchPath(L"contract path:C:\\Documents"),
              L"continuous search: clearing input preserves existing query results");
        DestroyWindow(state->hwndAddressEdit);
        state->hwndAddressEdit = nullptr;
    }
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(1);
    (*entries)[0].name = L"old.txt";
    first.view.SetSnapshot(entries);
    first.view.loading = true;
    first.view.search_retaining_results = true;
    first.view.SelectAll();
    Check(first.view.snapshot->size() == 1 && first.view.CountBound() == 0 &&
          first.view.SelectedIndices().empty(), L"continuous search: old results stay visible but inactive");
    first.view.pending_search_offset = 0;
    index::SearchResult result;
    ApplySearchHits(first.view, L"empty", std::move(result));
    Check(!first.view.loading && !first.view.search_retaining_results && first.view.snapshot->empty(),
          L"continuous search: empty response replaces old results and completes loading");
    state->pane = nullptr;
}

void TestCtrlDragSelection() {
    auto state = std::make_unique<AppState>();
    Pane pane;
    state->pane = &pane;
    auto& tab = pane.view;
    tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(3));
    tab.SelectOnly(0);
    HandleListRowClick(*state, 0, true, false);
    Check(tab.IsSelected(0) && tab.SelectedCount() == 1,
          L"Ctrl-drag: selected file remains available on press");
    FinishListRowClick(*state);
    Check(!tab.IsSelected(0), L"Ctrl-click: release still deselects selected file");
    tab.SelectOnly(0);
    tab.ToggleSelect(1);
    HandleListRowClick(*state, 0, true, false);
    Check(tab.IsSelected(0) && tab.IsSelected(1) && tab.SelectedCount() == 2,
          L"Ctrl-drag: preserves all selected files and folders");
    // Starting a drag clears the deferred click, as does capture cancellation.
    state->clickCollapseIndex = -1;
    FinishListRowClick(*state);
    Check(tab.SelectedCount() == 2, L"Ctrl-drag: drag completion does not deselect source");
    HandleListRowClick(*state, 2, true, false);
    FinishListRowClick(*state);
    Check(tab.IsSelected(2) && tab.SelectedCount() == 3,
          L"Ctrl-click: unselected item is added exactly once");
    HandleListRowClick(*state, 1, false, false);
    Check(tab.SelectedCount() == 3, L"drag: plain press preserves multi-selection");
    FinishListRowClick(*state);
    Check(tab.IsSelected(1) && tab.SelectedCount() == 1,
          L"click: plain release collapses multi-selection");
    state->pane = nullptr;
}

void TestMenuModel() {
    const auto breadcrumb = BuildBreadcrumbMenu(true);
    const std::vector<int> breadcrumb_commands{CmdOpenInNewTab, CmdOpen, CmdCopyPath,
                                              CmdCopy, CmdOpenTerminal, CmdProperties};
    bool breadcrumb_ok = breadcrumb.size() == breadcrumb_commands.size();
    for (size_t i = 0; i < breadcrumb.size() && i < breadcrumb_commands.size(); ++i) {
        breadcrumb_ok &= breadcrumb[i].command == breadcrumb_commands[i] &&
                         breadcrumb[i].enabled && !breadcrumb[i].text.empty() &&
                         breadcrumb[i].shortcut.empty();
    }
    Check(breadcrumb_ok, L"breadcrumb: explicit folder actions with new tab first");
    const auto virtual_breadcrumb = BuildBreadcrumbMenu(false);
    Check(virtual_breadcrumb.size() == 3 &&
          virtual_breadcrumb[0].command == CmdOpenInNewTab &&
          virtual_breadcrumb[1].command == CmdOpen &&
          virtual_breadcrumb[2].command == CmdCopyPath,
          L"breadcrumb: virtual locations omit filesystem actions");
    ui::ComPtr<IDWriteFactory2> dwrite;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
                        reinterpret_cast<IUnknown**>(&dwrite));

    // Item menu: 打开 + icon strip (cut/copy/delete/rename) + verbs + undo.
    auto items = BuildItemMenu(false, L"");
    Check(items.size() == 9, L"menu: item menu is 打开+图标条+verbs+undo");
    // Command-id ranges must not overlap: CmdTabJoinGroupBase once collided
    // with CmdTabCloseOthers and "join group" closed every other tab.
    static_assert(CmdTabJoinGroupBase > CmdTabCloseRight &&
                  CmdTabJoinGroupBase + 32 <= CmdRecentBase,
                  "join-group ids must sit between tab commands and recents");
    std::vector<int> want{ CmdOpen, CmdNone, CmdCopyPath, CmdOpenTerminal, CmdProperties,
                           CmdPinWorkspace, CmdPinNetwork, CmdTags, CmdUndo };
    bool ids_ok = items.size() >= want.size();
    for (size_t i = 0; i < want.size() && i < items.size(); ++i)
        if (items[i].command != want[i]) ids_ok = false;
    Check(ids_ok, L"menu: verb order and ids");
    Check(items.size() > 1 && items[1].quick_swatches.size() == 4 &&
          items[1].quick_swatches[0].command == CmdCut &&
          items[1].quick_swatches[1].command == CmdCopy &&
          items[1].quick_swatches[2].command == CmdDelete &&
          items[1].quick_swatches[3].command == CmdRename &&
          !items[1].quick_swatches[0].glyph.empty(),
          L"menu: icon strip carries 剪切/复制/删除/重命名");
    Check(items.size() > 2 && items[2].shortcut == L"Ctrl+Shift+C",
          L"menu: 复制路径 carries Ctrl+Shift+C");
    auto folder_items = BuildItemMenu(false, L"", true);
    Check(folder_items.size() == 10, L"menu: folder item menu adds 在新标签打开");
    Check(folder_items.size() > 3 &&
          folder_items[2].command == CmdOpenInNewTab &&
          folder_items[2].text == L"在新标签打开" &&
          folder_items[3].command == CmdCopyPath,
          L"menu: 在新标签打开 sits before 复制路径");
    Check(!items.back().enabled, L"menu: undo disabled without a stack");

    auto recycle_items = BuildRecycleItemMenu(false, L"");
    Check(!recycle_items.empty() && recycle_items[0].command == CmdRestoreRecycle &&
          recycle_items[1].command == CmdDelete,
          L"menu: recycle item menu is restore + permanent delete");
    auto recycle_bg = BuildRecycleBackgroundMenu(true, L"撤销", true);
    Check(!recycle_bg.empty() && recycle_bg[0].command == CmdEmptyRecycle &&
          recycle_bg[0].enabled,
          L"menu: recycle background can empty the bin");
    auto recycle_place = BuildRecyclePlaceMenu(true);
    Check(recycle_place.size() == 2 && recycle_place[0].command == CmdOpenRecycle &&
          recycle_place[1].command == CmdEmptyRecycle,
          L"menu: recycle quick access pin opens and empties");
    {
        std::vector<ui::FluentMenuItem> en_place;
        ui::FluentMenuItem open_en;
        open_en.command = CmdOpenRecycle;
        open_en.text = L"Open";
        en_place.push_back(std::move(open_en));
        ui::FluentMenuItem empty_en;
        empty_en.command = CmdEmptyRecycle;
        empty_en.text = L"Empty Recycle Bin";
        en_place.push_back(std::move(empty_en));
        ui::FluentMenuModel en_model;
        en_model.SetItems(std::move(en_place));
        en_model.Layout(dwrite.get(), 1.0f);
        ui::ComPtr<IDWriteTextFormat> fmt;
        ui::typography::CreateTextFormat(dwrite.get(),
            {ui::typography::FontRole::Text, 14.0f, DWRITE_FONT_WEIGHT_NORMAL}, &fmt);
        float text_w = 0.0f;
        if (fmt.get()) {
            ui::ComPtr<IDWriteTextLayout> layout;
            const wchar_t* label = L"Empty Recycle Bin";
            dwrite->CreateTextLayout(label, static_cast<UINT32>(wcslen(label)),
                                     fmt.get(), 10000.0f, 100.0f, &layout);
            DWRITE_TEXT_METRICS metrics{};
            if (layout.get()) layout->GetMetrics(&metrics);
            text_w = (std::max)(metrics.width, metrics.widthIncludingTrailingWhitespace);
        }
        Check(static_cast<float>(en_model.WidthPx()) + 0.5f >= 60.0f + text_w,
              L"menu: Empty Recycle Bin fits in the flyout");
    }

    ui::FluentMenuModel model;
    model.SetItems(items);
    model.Layout(dwrite.get(), 1.0f);    int seps = 0;
    for (const auto& it : items) if (it.separator_after) ++seps;
    int expect_h = (int)(4 * 2 + 36.0f * (int)items.size() + 5 * seps + 0.5f);
    Check(model.HeightPx() == expect_h, L"menu: layout height = rows*36 + separators");
    Check(model.WidthPx() >= 160 && model.WidthPx() <= 320, L"menu: width within clamp");

    auto long_undo = BuildItemMenu(true,
        L"撤销删除 万锦学校一期_结施_1#创新融合中心及运动场馆(运动场馆).dwg");
    ui::FluentMenuModel wide;
    wide.SetItems(std::move(long_undo));
    wide.Layout(dwrite.get(), 1.0f);
    Check(wide.WidthPx() <= 320, L"menu: long undo label does not stretch the flyout");

    std::vector<index::Hit> path_hits{
        {L"C:\\Users\\W\\.codex", L".codex", true},
        {L"C:\\Users\\W\\plugins\\.codex-plugin", L".codex-plugin", true}
    };
    auto path_items = BuildCommandPalette(L".codex", {}, path_hits, false, path_hits.size());
    Check(path_items.size() >= 3 && path_items[0].shortcut_inline &&
          path_items[1].shortcut_inline && !path_items.back().shortcut_inline,
          L"menu: search paths use inline captions, result count remains trailing");
    ui::FluentMenuModel path_model;
    path_model.SetItems(path_items);
    path_model.Layout(dwrite.get(), 1.0f, 2600.0f);
    const float short_column = path_model.InlineLabelWidthPx();
    Check(short_column > 0.0f && short_column < 160.0f,
          L"menu: wide search keeps paths near short filenames");
    path_model.Layout(dwrite.get(), 1.0f, 640.0f);
    Check(std::abs(path_model.InlineLabelWidthPx() - short_column) < 1.0f,
          L"menu: window width does not push short-name paths away");
    path_items[1].text = std::wstring(200, L'W');
    path_model.SetItems(path_items);
    path_model.Layout(dwrite.get(), 1.0f, 2600.0f);
    Check(path_model.InlineLabelWidthPx() <= 240.0f,
          L"menu: long filename leaves room for path");
    path_model.Layout(dwrite.get(), 1.0f, 320.0f);
    Check(path_model.InlineLabelWidthPx() < 100.0f,
          L"menu: narrow search shares space without overlap");
    path_model.Layout(dwrite.get(), 2.0f, 5200.0f);
    Check(std::abs(path_model.InlineLabelWidthPx() - 480.0f) < 1.0f,
          L"menu: filename column scales at 200 percent DPI");

    // Hit-test: first row, separator dead zone, disabled row skipped by nav.
    int row0 = model.HitTestRow(model.RowTopPx(0) + 1.0f);
    Check(row0 == 0, L"menu: hit-test row 0");
    int sep_row = -1;
    for (int i = 0; i < model.Count(); ++i)
        if (model.At(i)->separator_after) { sep_row = i; break; }
    float sep_y = model.RowTopPx(sep_row) + model.RowHeightPx() + 2.0f;
    Check(model.HitTestRow(sep_y) == -1, L"menu: separator is a dead zone");
    int last = model.Count() - 1; // undo, disabled
    Check(model.NextEnabled(last - 1, 1) == 0, L"menu: nav wraps and skips disabled undo");
    Check(model.FirstEnabled() == 0, L"menu: first enabled is 打开");

    // Simulated dispatch: hit-test -> command id -> verb.
    int row = model.HitTestRow(model.RowTopPx(0) + 1.0f);
    int cmd = (row >= 0 && model.At(row)->enabled) ? model.At(row)->command : 0;
    Check(cmd == CmdOpen, L"menu: hit-test -> dispatch 打开");

    // Background menu + new dropdown (toolbar reuses the same component).
    auto bg = BuildBackgroundMenu(true, true, L"撤销移动 a.txt");
    Check(!bg.empty() && bg[0].command == CmdNewFolder, L"menu: background menu starts with 新建文件夹");
    bool has_invert = false, has_wildcard = false, has_select_all = false;
    for (const auto& it : bg) {
        if (it.command == CmdInvertSelection) has_invert = true;
        if (it.command == CmdSelectWildcard) has_wildcard = true;
        if (it.command == CmdSelectAll) has_select_all = true;
    }
    Check(has_select_all && has_invert && has_wildcard,
          L"menu: background has 全选 / 反选 / 通配选择");
    BackgroundViewOptions options;
    options.view_mode = ui::ViewMode::LargeIcons;
    options.sort_column = ui::SortColumn::Size;
    options.sort_direction = ui::SortDirection::Desc;
    options.details_panel = true;
    AppendBackgroundViewCommands(bg, options);
    Check(bg[0].children.size() == 9 && bg[0].children[1].radio &&
          bg[0].children.back().checked, L"menu: background reflects view and details pane");
    const auto& sort = bg[1].children;
    Check(sort.size() == 6 && sort[3].command == CmdSortSize && sort[3].radio &&
          !sort[4].radio && sort[5].radio,
          L"menu: background reflects size descending with separate radio groups");
    Check(bg[2].command == CmdRefresh && bg[2].shortcut == L"F5" &&
          bg.back().command == CmdFolderProperties,
          L"menu: background refresh and explicit folder properties");
    options.filesystem = false;
    options.show_path = true;
    options.sort_column = ui::SortColumn::Path;
    auto virtual_bg = BuildBackgroundMenu(false, false, L"");
    AppendBackgroundViewCommands(virtual_bg, options);
    Check(virtual_bg[1].children.size() == 7 && virtual_bg[1].children[4].radio &&
          virtual_bg.back().command != CmdFolderProperties,
          L"menu: search offers path sorting without folder properties");
    options.indexed_search = true;
    std::vector<ui::FluentMenuItem> search_menu;
    AppendBackgroundViewCommands(search_menu, options);
    Check(search_menu[1].children[0].enabled && !search_menu[1].children[2].enabled &&
          !search_menu[1].children[4].enabled,
          L"menu: indexed search disables unsupported type and path sorts");
    for (float scale : { 1.0f, 1.5f, 2.0f }) {
        ui::FluentMenuModel sort_model;
        sort_model.SetItems(sort);
        sort_model.Layout(dwrite.get(), scale);
        bool hit_tests_ok = true;
        for (int i = 0; i < sort_model.Count(); ++i)
            hit_tests_ok &= sort_model.HitTestRow(sort_model.RowTopPx(i) +
                sort_model.RowHeightPx() * 0.5f) == i;
        Check(hit_tests_ok && sort_model.WidthPx() <= static_cast<int>(320 * scale),
              L"menu: sort flyout layout and hit testing at 100/150/200 percent DPI");
    }
    options.can_sort = false;
    std::vector<ui::FluentMenuItem> curated;
    AppendBackgroundViewCommands(curated, options);
    Check(!curated[1].enabled && std::none_of(curated[1].children.begin(),
          curated[1].children.end(), [](const auto& item) { return item.enabled || item.radio; }),
          L"menu: curated views disable sorting and do not claim a selected sort");
    auto nw = BuildNewMenu();
    Check(nw.size() == 2 && nw[0].command == CmdNewFolder && nw[1].command == CmdNewTextFile,
          L"menu: 新建▾ dropdown has 文件夹/文本文档");
}

// Explorer merge rules (优化.md §7): built-in verbs filtered, duplicate texts
// dropped, section appended at the bottom; software-owned submenus keep one
// level of hierarchy as a flyout (never flattened into the main list).
void TestShellMenuMerge() {
    Check(ipc::IsBuiltinContextVerb(L"Open", false), L"shellmenu: open filtered");
    Check(ipc::IsBuiltinContextVerb(L"copyaspath", false), L"shellmenu: copyaspath filtered");
    Check(ipc::IsBuiltinContextVerb(L"pintohome", false), L"shellmenu: pintohome filtered");
    Check(!ipc::IsBuiltinContextVerb(L"MergePdf", false), L"shellmenu: third-party verb kept");
    Check(!ipc::IsBuiltinContextVerb(L"refresh", false), L"shellmenu: refresh only filtered on background");
    Check(ipc::IsBuiltinContextVerb(L"refresh", true), L"shellmenu: background refresh filtered");
    Check(ipc::IsDroppedContextSubmenu(L"OpenAs"), L"shellmenu: 打开方式 submenu dropped (registry provides it)");
    Check(!ipc::IsDroppedContextSubmenu(L"sendto"), L"shellmenu: 发送到 submenu kept as a flyout");

    Check(ipc::CleanMenuText(L"打开方式(&H)...\tCtrl+O") == L"打开方式(H)...",
          L"shellmenu: text loses & mnemonic and \\t shortcut");
    Check(ipc::CleanMenuText(L"A && B") == L"A & B", L"shellmenu: && stays a literal ampersand");

    // AppendShellSection: dedupe against built-ins + across entries, flat rows.
    auto items = BuildItemMenu(false, L"");
    const size_t base_count = items.size();
    std::vector<ShellMenuEntry> entries{
        { CmdShellStaticBase + 0, L"打印", true },
        { CmdShellStaticBase + 1, L"属性", true },       // dup of built-in row
        { CmdShellComBase + 5, L"合并 PDF", true },
        { CmdShellComBase + 6, L"打印", true },          // dup of static entry
        { CmdShellComBase + 7, L"", true },              // empty text dropped
    };
    AppendShellSection(items, entries);
    Check(items.size() == base_count + 2, L"shellmenu: section dedupes 属性/打印 and empty rows");
    Check(items[base_count - 1].separator_after, L"shellmenu: separator before Explorer section");
    Check(items[base_count].command == CmdShellStaticBase + 0 &&
          items[base_count + 1].command == CmdShellComBase + 5,
          L"shellmenu: statics precede COM extras at the bottom");
    for (size_t i = base_count; i < items.size(); ++i)
        Check(items[i].quick_swatches.empty(), L"shellmenu: Explorer rows stay flat");

    // Software-owned submenu: header + children survive as a one-level flyout
    // (command 0 header row carries the children; never flattened).
    ShellMenuEntry group;
    group.text = L"Bandizip";
    group.children = { { CmdShellComBase + 20, L"压缩为 zip", true },
                       { CmdShellComBase + 21, L"压缩为 7z", true } };
    auto fly = BuildItemMenu(false, L"");
    const size_t fly_base = fly.size();
    AppendShellSection(fly, { group });
    Check(fly.size() == fly_base + 1 && fly.back().command == CmdNone &&
          fly.back().children.size() == 2 &&
          fly.back().children[0].command == CmdShellComBase + 20 &&
          fly.back().children[1].text == L"压缩为 7z",
          L"shellmenu: 软件子菜单保留为一级飞出");
    ui::FluentMenuModel fly_model;
    fly_model.SetItems(fly);
    Check(fly_model.At((int)fly_base) &&
          fly_model.At((int)fly_base)->children.size() == 2 &&
          fly_model.At((int)fly_base)->shortcut.empty(),
          L"shellmenu: 飞出行带 children、不占用 shortcut 列");

    ui::FluentMenuModel patch_model;
    ui::FluentMenuItem patch_a;
    patch_a.command = CmdShellComBase + 1;
    patch_a.text = L"合并 PDF";
    ui::FluentMenuItem patch_b;
    patch_b.command = CmdShellComBase + 2;
    patch_b.text = L"打印";
    patch_model.SetItems({ patch_a, patch_b });
    patch_a.command = CmdShellComBase + 9;
    patch_b.command = CmdShellComBase + 8;
    Check(patch_model.PatchCommands({ patch_a, patch_b }) &&
          patch_model.At(0) && patch_model.At(0)->command == CmdShellComBase + 9 &&
          patch_model.At(1) && patch_model.At(1)->command == CmdShellComBase + 8,
          L"shellmenu: display-equal COM ids patch in place");
    Check(!patch_model.PatchCommands({ patch_a }),
          L"shellmenu: refuse to shrink an open menu structure");
    ShellMenuEntry empty_group;
    empty_group.text = L"空分组";
    auto no_fly = BuildItemMenu(false, L"");
    const size_t no_fly_base = no_fly.size();
    AppendShellSection(no_fly, { empty_group });
    Check(no_fly.size() == no_fly_base, L"shellmenu: 无子项的分组行被丢弃");

    // Cap: never more than 48 Explorer rows.
    std::vector<ShellMenuEntry> many;
    for (int i = 0; i < 60; ++i)
        many.push_back({ CmdShellComBase + 100 + i, L"动词 " + std::to_wstring(i), true });
    auto capped = BuildItemMenu(false, L"");
    const size_t cap_base = capped.size();
    AppendShellSection(capped, many);
    Check(capped.size() == cap_base + 48, L"shellmenu: section capped at 48 rows");

    // DedupeStaticVerbs mirrors the same rules for the registry side.
    std::vector<StaticVerb> verbs{
        { L"print", L"打印", L"" },
        { L"edit", L"编辑", L"" },
        { L"edit2", L"编辑", L"" },   // dup display
        { L"openas", L"打开方式…", L"" },
    };
    auto deduped = DedupeStaticVerbs(std::move(verbs), { L"编辑" }, 8);
    Check(deduped.size() == 2 && deduped[0].display == L"打印" &&
          deduped[1].display == L"打开方式…",
          L"shellmenu: static verbs dedupe against built-ins and each other");

    ipc::StaticVerbRegFlags cascade_ok;
    cascade_ok.has_subcommands = true;
    Check(ipc::KeepStaticVerb(cascade_ok), L"shellmenu: SubCommands verbs are kept");
    ipc::StaticVerbRegFlags handler_ok;
    handler_ok.has_explorer_command = true;
    Check(ipc::KeepStaticVerb(handler_ok), L"shellmenu: ExplorerCommandHandler verbs are kept");
    ipc::StaticVerbRegFlags ext_skip;
    ext_skip.has_command = true;
    ext_skip.extended = true;
    Check(!ipc::KeepStaticVerb(ext_skip), L"shellmenu: Extended static verbs stay hidden");
    ipc::StaticVerbRegFlags empty_skip;
    Check(!ipc::KeepStaticVerb(empty_skip), L"shellmenu: verbs without a launch path are dropped");

    ipc::CtxFlyoutChild nested;
    nested.nested = true;
    nested.text = L"分组";
    ipc::CtxFlyoutChild leaf;
    leaf.id = 42;
    leaf.verb = L"share_phone";
    leaf.text = L"手机";
    nested.nested_leaves.push_back(leaf);
    ipc::CtxFlyoutChild builtin;
    builtin.verb = L"open";
    builtin.text = L"打开";
    std::vector<ipc::CtxFlyoutChild> leaves;
    ipc::FlattenFlyoutChildren({ nested, builtin }, false, leaves);
    Check(leaves.size() == 1 && leaves[0].id == 42 && leaves[0].text == L"手机" &&
          !leaves[0].nested,
          L"shellmenu: nested flyout children flatten to leaves");
    Check(ipc::KeepFlyoutParentWithoutLeaves(100, 50, 200),
          L"shellmenu: parent with a live id is kept when the flyout is empty");
    Check(!ipc::KeepFlyoutParentWithoutLeaves(0, 50, 200),
          L"shellmenu: parent id 0 is not kept as a clickable row");
    Check(!ipc::KeepFlyoutParentWithoutLeaves(10, 50, 200),
          L"shellmenu: parent ids outside the handler range are dropped");
}

void TestContextMenuPrefs() {
    using ipc::ClassifyExplorerItem;
    using ipc::CtxMenuCategory;
    Check(ClassifyExplorerItem(L"sendto", L"发送到", true) == CtxMenuCategory::Share,
          L"prefs: 发送到 is share");
    Check(ClassifyExplorerItem(L"wallpaper", L"设置为桌面背景", false) == CtxMenuCategory::Wallpaper,
          L"prefs: wallpaper classified");
    Check(ClassifyExplorerItem(L"rotate90", L"向右旋转", false) == CtxMenuCategory::Rotate,
          L"prefs: rotate classified");
    Check(ClassifyExplorerItem(L"link", L"创建快捷方式", false) == CtxMenuCategory::Shortcut,
          L"prefs: shortcut classified");
    Check(ClassifyExplorerItem(L"print", L"打印", false) == CtxMenuCategory::Print,
          L"prefs: print classified");
    Check(ClassifyExplorerItem(L"__openwith", L"用 记事本 打开", false) == CtxMenuCategory::OpenWith,
          L"prefs: 用 X 打开 classified");
    Check(ClassifyExplorerItem(L"", L"合并 PDF", false) == CtxMenuCategory::Software,
          L"prefs: type verb stays software");
    Check(ClassifyExplorerItem(L"", L"Bandizip", true) == CtxMenuCategory::Software,
          L"prefs: vendor flyout stays software");
    Check(ClassifyExplorerItem(L"", L"泛泰快传", true) == CtxMenuCategory::Share,
          L"prefs: 泛泰快传 flyout is share");
    Check(ClassifyExplorerItem(L"", L"泛泰快传", false) == CtxMenuCategory::Share,
          L"prefs: 泛泰快传 row is share");
    Check(ipc::IsCompressVendorFlyout(L"Bandizip") &&
          ipc::IsCompressTopLevel(L"压缩为「photo.zip」"),
          L"prefs: compress flyout vs top-level zip row");

    ContextMenuPrefs prefs;
    prefs.persist = false;

    ShellMenuEntry sendto;
    sendto.text = L"发送到";
    sendto.verb = L"sendto";
    sendto.from_com = true;
    sendto.children = { { CmdShellComBase + 1, L"文档", true } };

    ShellMenuEntry bandizip;
    bandizip.text = L"Bandizip";
    bandizip.from_com = true;
    bandizip.children = { { CmdShellComBase + 20, L"压缩为 zip", true } };

    std::vector<ShellMenuEntry> raw{
        { CmdShellStaticBase + 0, L"打印", true, {}, L"print", false },
        { CmdShellStaticBase + 1, L"用 记事本 打开", true, {}, L"__openwith", false },
        { CmdShellStaticBase + 2, L"用 画图 打开", true, {}, L"__openwith", false },
        { CmdShellStaticBase + 3, L"用 Word 打开", true, {}, L"__openwith", false },
        { CmdShellStaticBase + 4, L"打开方式…", true, {}, L"openas", false },
        { CmdShellComBase + 5, L"设置为桌面背景", true, {}, L"wallpaper", true },
        { CmdShellComBase + 6, L"向右旋转", true, {}, L"rotate90", true },
        { CmdShellComBase + 7, L"压缩为「photo.zip」", true, {}, L"", true },
        { CmdShellComBase + 8, L"合并 PDF", true, {}, L"", true },
        sendto,
        bandizip,
    };

    auto filtered = ApplyExplorerPrefs(prefs, raw);
    auto has = [&](std::wstring_view text) {
        for (const auto& e : filtered)
            if (e.text == text) return true;
        return false;
    };
    Check(!has(L"发送到") && !has(L"设置为桌面背景") && !has(L"向右旋转"),
          L"prefs: factory drops share / wallpaper / rotate");
    Check(has(L"Bandizip") && has(L"合并 PDF") && has(L"打印"),
          L"prefs: factory keeps vendor flyout, type verb, print");
    Check(!has(L"压缩为「photo.zip」"),
          L"prefs: compress top-level collapsed when flyout exists");
    bool in_bandizip = false;
    for (const auto& e : filtered)
        if (e.text == L"Bandizip")
            for (const auto& c : e.children)
                if (c.text == L"压缩为「photo.zip」") in_bandizip = true;
    Check(in_bandizip, L"prefs: named compress row folds into vendor flyout");

    std::vector<ShellMenuEntry> named_only{
        { CmdShellComBase + 30, L"压缩为 \"a.zip\"", true, {}, L"", true },
        { CmdShellComBase + 31, L"压缩为 \"b.7z\"", true, {}, L"", true },
        { CmdShellComBase + 32, L"合并 PDF", true, {}, L"", true },
    };
    auto grouped = ApplyExplorerPrefs(prefs, named_only);
    Check(grouped.size() == 2 && grouped[0].text == L"压缩" &&
          grouped[0].children.size() == 2 && grouped[1].text == L"合并 PDF",
          L"prefs: named compress rows synthesize one 压缩 flyout");

    Check(ipc::IsDisabledHandler(L"{11111111-1111-1111-1111-111111111111}",
                                 { L"{11111111-1111-1111-1111-111111111111}" }),
          L"prefs: disabled handler CLSID matches");

    ContextMenuPrefs compress_prefs;
    compress_prefs.persist = false;
    compress_prefs.RecordSeen(ipc::CatalogKey(L"压缩为「old.zip」", false), L"压缩为「old.zip」", false,
                     CtxMenuCategory::Software, true);
    compress_prefs.RecordSeen(ipc::CatalogKey(L"压缩为「new.7z」", false), L"压缩为「new.7z」", false,
                     CtxMenuCategory::Software, true);
    ContextMenuPrefs migrated;
    migrated.persist = false;
    Check(migrated.FromJson(compress_prefs.ToJson()), L"prefs: json loads for compress coalesce");
    int compress_rows = 0;
    bool has_canonical = false;
    for (const auto& item : migrated.seen) {
        if (ipc::IsCompressTopLevel(item.text) && item.key != ipc::CompressCatalogKey())
            ++compress_rows;
        if (item.key == ipc::CompressCatalogKey()) has_canonical = true;
    }
    Check(compress_rows == 0 && has_canonical,
          L"prefs: filename compress seen rows coalesce to 压缩为…");

    const std::wstring adobe_clsid = L"{22222222-2222-2222-2222-222222222222}";
    ContextMenuPrefs handler_prefs;
    handler_prefs.persist = false;
    ShellMenuEntry adobe;
    adobe.command = CmdShellComBase + 40;
    adobe.text = L"Adobe PDF";
    adobe.from_com = true;
    adobe.clsid = adobe_clsid;
    adobe.handler = L"Adobe PDF";
    handler_prefs.SetItemEnabled(ipc::HandlerCatalogKey(adobe_clsid), false);
    auto without_adobe = ApplyExplorerPrefs(handler_prefs, { adobe, named_only[2] });
    Check(without_adobe.size() == 1 && without_adobe[0].text == L"合并 PDF",
          L"prefs: disabled handler CLSID is omitted from the fusion zone");
    Check(!handler_prefs.HandlerEnabled(adobe_clsid),
          L"prefs: explicit handler disable skips CoCreate");

    ContextMenuPrefs slow_handler;
    slow_handler.persist = false;
    const std::wstring slow_key = ipc::HandlerCatalogKey(adobe_clsid);
    slow_handler.RecordComTiming(slow_key, 1000);
    slow_handler.RecordComTiming(slow_key, 1000);
    Check(!slow_handler.ComDisabled(slow_key), L"prefs: two handler timeouts do not disable");
    slow_handler.RecordComTiming(slow_key, 1000);
    Check(slow_handler.ComDisabled(slow_key) && !slow_handler.HandlerEnabled(adobe_clsid),
          L"prefs: three handler timeouts disable the CLSID");
    auto disabled_clsids = slow_handler.DisabledHandlerClsids();
    bool listed = false;
    for (const auto& c : disabled_clsids)
        if (ipc::ToLowerVerb(c) == ipc::ToLowerVerb(adobe_clsid)) listed = true;
    Check(listed, L"prefs: disabled handler CLSIDs are sent to the host");
    slow_handler.SetItemEnabled(slow_key, true);
    Check(slow_handler.HandlerEnabled(adobe_clsid),
          L"prefs: re-enabling a handler clears the timeout skip");
    Check(ipc::IsHandlerCatalogKey(slow_key) &&
          ipc::HandlerClsidFromKey(slow_key) == ipc::ToLowerVerb(adobe_clsid),
          L"prefs: handler catalog key round-trips the CLSID");
    Check(has(L"用 记事本 打开") && has(L"用 画图 打开") && !has(L"用 Word 打开") &&
          has(L"打开方式…"),
          L"prefs: static open-with MRU capped at 2 plus 打开方式");
    Check(filtered.size() >= 4 && filtered[0].text == L"Bandizip" &&
          filtered[1].text == L"合并 PDF",
          L"prefs: order is flyout then type verb");

    prefs.SetItemEnabled(ipc::CatalogKey(L"发送到", true), true);
    auto restored = ApplyExplorerPrefs(prefs, raw);
    bool sendto_back = false;
    for (const auto& e : restored)
        if (e.text == L"发送到") sendto_back = true;
    Check(sendto_back, L"prefs: per-item override re-enables 发送到");

    prefs.RecordSeen(ipc::CatalogKey(L"Bandizip", true), L"Bandizip", true,
                     CtxMenuCategory::Software, true);
    const std::wstring json = prefs.ToJson();
    ContextMenuPrefs loaded;
    loaded.persist = false;
    Check(loaded.FromJson(json) && loaded.item_enabled[ipc::CatalogKey(L"发送到", true)] &&
          loaded.seen.size() == 1 && loaded.seen[0].from_com &&
          loaded.explorer_cap == 32 && !loaded.share && loaded.print,
          L"prefs: JSON round-trip keeps override, seen, and defaults");

    Check(prefs.RecordComTiming(L".dwg", 800) && prefs.ComDeferred(L".dwg") == false, L"prefs: one slow COM hit does not defer");
    prefs.RecordComTiming(L".dwg", 800);
    prefs.RecordComTiming(L".dwg", 800);
    Check(prefs.ComDeferred(L".dwg") && !prefs.ComDisabled(L".dwg"),
          L"prefs: three 500ms+ COM hits defer the extension");
    prefs.RecordComTiming(L".cad", 1200);
    prefs.RecordComTiming(L".cad", 1200);
    prefs.RecordComTiming(L".cad", 1200);
    Check(prefs.ComDisabled(L".cad"), L"prefs: three 1000ms+ COM hits disable the extension");
    const std::wstring json_slow = prefs.ToJson();
    ContextMenuPrefs slow_loaded;
    slow_loaded.persist = false;
    Check(slow_loaded.FromJson(json_slow) && slow_loaded.ComDeferred(L".dwg") &&
              slow_loaded.ComDisabled(L".cad"),
          L"prefs: slow COM stats round-trip");

    loaded.ResetToDefaults();
    Check(loaded.seen.empty() && loaded.item_enabled.empty() && !loaded.share,
          L"prefs: restore defaults clears seen and overrides");
    Check(ContextMenuPrefs{}.explorer_cap == 32 && loaded.explorer_cap == 32,
          L"prefs: factory Explorer cap is 32");

    std::vector<ShellMenuEntry> many;
    for (int i = 0; i < 20; ++i)
        many.push_back({ CmdShellComBase + 200 + i, L"动词 " + std::to_wstring(i), true, {}, L"", true });
    prefs.explorer_cap = 12;
    auto capped = ApplyExplorerPrefs(prefs, many);
    Check(capped.size() == 12, L"prefs: Explorer section respects explorer_cap");
}

void TestAppPrefsAndSettingsPath() {
    std::wstring kind, rest;
    Check(ParsePulsePath(L"pulse:settings:general", &kind, &rest) &&
          kind == L"settings" && rest == L"general",
          L"settings: parse general path");
    Check(ParsePulsePath(L"pulse:settings:context", &kind, &rest) &&
          kind == L"settings" && rest == L"context",
          L"settings: parse context path");
    Check(ParsePulsePath(L"pulse:settings:index", &kind, &rest) &&
          kind == L"settings" && rest == L"index",
          L"settings: parse index path");
    Check(MakeSettingsPath() == L"pulse:settings:general",
          L"settings: default page is general");
    Check(MakeSettingsPath(L"context") == L"pulse:settings:context",
          L"settings: context page path");
    Check(MakeSettingsPath(L"index") == L"pulse:settings:index",
          L"settings: index page path");
    Check(fs::IsVirtualPath(L"pulse:settings:general"),
          L"settings: pulse:settings is virtual");

    AppPrefs prefs;
    prefs.persist = false;
    Check(prefs.FromJson(L"{\"launch_on_startup\":true,\"keep_running_on_close\":true}") &&
          prefs.launch_on_startup && prefs.keep_running_on_close &&
          !prefs.open_folders_in_pulse,
          L"appprefs: parse json");
    Check(prefs.ApplyLaunchOnStartup(false) && !prefs.launch_on_startup,
          L"appprefs: persist=false toggle does not write Run key");
    Check(prefs.ApplyFolderOpen(true) && prefs.open_folders_in_pulse,
          L"appprefs: persist=false folder-open toggle does not write HKCU");
    const std::wstring json = prefs.ToJson();
    AppPrefs loaded;
    loaded.persist = false;
    Check(loaded.FromJson(json) && !loaded.launch_on_startup && loaded.keep_running_on_close &&
          loaded.open_folders_in_pulse && loaded.language == L"system" &&
          !loaded.show_status_performance,
          L"appprefs: json round-trip");
    Check(json.find(L"\"launch_on_startup\":false") != std::wstring::npos &&
          json.find(L"\"keep_running_on_close\":true") != std::wstring::npos &&
          json.find(L"\"open_folders_in_pulse\":true") != std::wstring::npos &&
          json.find(L"\"language\":\"system\"") != std::wstring::npos &&
          json.find(L"\"show_status_performance\":false") != std::wstring::npos,
          L"appprefs: json contains both flags");
    Check(prefs.FromJson(L"{\"show_status_performance\":true}") &&
          prefs.show_status_performance,
          L"appprefs: parse show_status_performance");
    Check(FolderOpenCommandLine(L"C:\\Pulse\\pulse.exe") ==
              L"\"C:\\Pulse\\pulse.exe\" \"%1\"",
          L"appprefs: folder-open command quotes exe and %1");
    Check(FolderOpenCommandIsOurs(L"\"C:\\Pulse\\pulse.exe\" \"%1\"",
                                  L"C:\\Pulse\\pulse.exe"),
          L"appprefs: folder-open command matches our exe");
    Check(!FolderOpenCommandIsOurs(L"\"C:\\Windows\\explorer.exe\" \"%1\"",
                                   L"C:\\Pulse\\pulse.exe"),
          L"appprefs: folder-open command ignores explorer");
    AppPrefs density;
    density.persist = false;
    density.row_height = 40;
    AppPrefs density_loaded;
    density_loaded.persist = false;
    Check(density_loaded.FromJson(density.ToJson()) && density_loaded.row_height == 40,
          L"appprefs: row_height round-trip");
    Check(density_loaded.FromJson(L"{\"row_height\":99}") && density_loaded.row_height == 34,
          L"appprefs: row_height out of range falls back to default");

    bool effect_ids_ok = true;
    for (int i = 0; i < ui::kWindowEffectCount; ++i) {
        const auto e = static_cast<ui::WindowEffect>(i);
        if (ui::WindowEffectFromId(ui::WindowEffectId(e)) != e || !ui::WindowEffectLabel(e)[0])
            effect_ids_ok = false;
    }
    Check(effect_ids_ok, L"appprefs: window effect id/label round-trip");
    AppPrefs effect_prefs;
    effect_prefs.persist = false;
    Check(effect_prefs.FromJson(
              L"{\"window_effect\":\"none\",\"background_image\":\"C:\\\\wall.jpg\"}") &&
          effect_prefs.window_effect == L"none" &&
          effect_prefs.background_image == L"C:\\wall.jpg",
          L"appprefs: parse none effect + background image");

    uint32_t accent = 0;
    Check(!ParseAccentRgb(L"", accent), L"appprefs: empty accent follows Windows");
    Check(ParseAccentRgb(L"2FDFF1", accent) && accent == 0x2FDFF1u,
          L"appprefs: parse accent 2FDFF1");
    Check(ParseAccentRgb(L"#2fdff1", accent) && accent == 0x2FDFF1u,
          L"appprefs: parse accent #2fdff1");
    Check(!ParseAccentRgb(L"xyz", accent), L"appprefs: reject bad accent hex");
    AppPrefs follow;
    follow.persist = false;
    Check(follow.FromJson(L"{}") && follow.accent_rgb.empty(),
          L"appprefs: missing accent_rgb follows Windows");
    AppPrefs custom;
    custom.persist = false;
    Check(custom.FromJson(L"{\"accent_rgb\":\"2FDFF1\"}") &&
          custom.accent_rgb == L"2FDFF1",
          L"appprefs: accent_rgb from json");
    AppPrefs custom_round;
    custom_round.persist = false;
    Check(custom_round.FromJson(custom.ToJson()) && custom_round.accent_rgb == L"2FDFF1" &&
          custom.ToJson().find(L"\"accent_rgb\":\"2FDFF1\"") != std::wstring::npos,
          L"appprefs: accent_rgb round-trip");
    AppPrefs bad_accent;
    bad_accent.persist = false;
    Check(bad_accent.FromJson(L"{\"accent_rgb\":\"gggggg\"}") && bad_accent.accent_rgb.empty(),
          L"appprefs: invalid accent_rgb falls back to follow");
}

void TestBloomAccentGeometry() {
    Check(ui::kBloomDotCount == 19, L"bloom: 19 dots");
    Check(std::fabs(ui::BloomDotHue(1, 0, 6) - 90.0f) < 0.01f,
          L"bloom: ring1 index0 hue 90");
    Check(std::fabs(ui::BloomDotHue(2, 0, 12) - 90.0f) < 0.01f,
          L"bloom: ring2 index0 hue 90");
    Check(std::fabs(ui::BloomDotHue(1, 3, 6) - 270.0f) < 0.01f,
          L"bloom: ring1 index3 hue 270");
    Check(std::fabs(ui::BloomDotHue(1, 1, 6) - 150.0f) < 0.01f,
          L"bloom: ring1 index1 hue 150");
    Check(ui::BloomDotRgb(0) == 0xFFFFFFu, L"bloom: center is white");
    Check(ui::BloomDotAt(0).ring == 0 && ui::BloomDotAt(7).ring == 2,
          L"bloom: index 0 center, 7 first outer");
    const uint32_t pastel = ui::BloomDotRgb(1);
    const uint32_t sat = ui::BloomDotRgb(7);
    Check(pastel != sat && pastel != 0xFFFFFFu && sat != 0xFFFFFFu,
          L"bloom: pastel and saturated dots differ");
}

void TestBloomSpring() {
    auto run = [](float from, float to, float k, float d, float& peak, float& lo, float& end) {
        float v = from;
        float vel = 0.0f;
        peak = from;
        lo = from;
        bool finite = true;
        for (int i = 0; i < 180; ++i) {
            ui::BloomStepSpring(v, vel, to, k, d, 0.016f);
            if (!std::isfinite(v) || !std::isfinite(vel)) finite = false;
            peak = (std::max)(peak, v);
            lo = (std::min)(lo, v);
        }
        end = v;
        Check(finite, L"bloom: spring stayed finite");
    };
    float peak = 0.0f, lo = 0.0f, end = 0.0f;
    run(1.0f, 1.18f, 200.0f, 30.0f, peak, lo, end);
    Check(peak < 1.35f && lo > 0.85f && std::fabs(end - 1.18f) < 0.05f,
          L"bloom: hover scale must not explode");
    run(0.0f, 5.0f, 100.0f, 30.0f, peak, lo, end);
    Check(peak < 6.0f && lo > -1.0f && std::fabs(end - 5.0f) < 0.15f,
          L"bloom: push spring must not explode");
}

void TestDragDropPure() {
    Check(ui::VolumeRoot(L"C:\\A\\B") == L"C:\\", L"dnd: volume root drive");
    Check(ui::VolumeRoot(L"\\\\srv\\share\\x") == L"\\\\srv\\share", L"dnd: volume root UNC");
    const DWORD both = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    Check(ui::ComputeDropEffect(0, L"C:\\a.txt", L"C:\\dst", both) == DROPEFFECT_MOVE,
          L"dnd: same volume defaults to move");
    Check(ui::ComputeDropEffect(0, L"C:\\a.txt", L"D:\\dst", both) == DROPEFFECT_COPY,
          L"dnd: cross volume defaults to copy");
    Check(ui::ComputeDropEffect(MK_CONTROL, L"C:\\a.txt", L"C:\\dst", both) == DROPEFFECT_COPY,
          L"dnd: Ctrl forces copy");
    Check(ui::ComputeDropEffect(MK_SHIFT, L"D:\\a.txt", L"C:\\dst", both) == DROPEFFECT_MOVE,
          L"dnd: Shift forces move");
    Check(ui::ComputeDropEffect(0, L"C:\\a.txt", L"C:\\dst", DROPEFFECT_COPY) == DROPEFFECT_COPY,
          L"dnd: falls back to allowed effect");
    Check(ui::ComputeDropEffect(0, L"C:\\a.txt", L"C:\\dst", DROPEFFECT_LINK) == DROPEFFECT_LINK,
          L"dnd: no copy/move allowed -> link");

    Check(ui::FirstDroppableFolder({}).empty(), L"dnd: no sources -> no header folder");
    Check(ui::FirstDroppableFolder({ L"C:\\pulse_no_such_file.txt" }).empty(),
          L"dnd: missing path is not a header folder");
    wchar_t windir[MAX_PATH]{};
    if (GetWindowsDirectoryW(windir, ARRAYSIZE(windir)) > 0) {
        Check(ui::FirstDroppableFolder({ windir }) == windir,
              L"dnd: directory is a header-drop folder");
        wchar_t self[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, self, ARRAYSIZE(self)) > 0) {
            Check(ui::FirstDroppableFolder({ self }).empty(),
                  L"dnd: file is not a header-drop folder");
            Check(ui::FirstDroppableFolder({ self, windir }) == windir,
                  L"dnd: first real directory wins among mixed sources");
        }
    }
    Check(ui::LooksLikeFolderShortcut(L"C:\\Projects\\work.lnk"),
          L"dnd: .lnk suffix is a header-drop shortcut");
    Check(ui::LooksLikeFolderShortcut(L"C:\\Projects\\Work.LNK"),
          L"dnd: .lnk suffix is case-insensitive");
    Check(!ui::LooksLikeFolderShortcut(L"C:\\Projects\\work.txt"),
          L"dnd: non-lnk is not a header-drop shortcut");
    Check(!ui::LooksLikeFolderShortcut(L"lnk"), L"dnd: short names are not shortcuts");
}

void TestDirWatch() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp), temp);
    const std::wstring dir = std::wstring(temp) + L"PulseWatchTest-" +
                             std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);

    std::atomic<int> hits{0};
    std::mutex mu;
    std::vector<fs::DirNotifyEvent> events;
    fs::DirWatch watch;
    const bool started = watch.Start(dir, [&](bool, std::vector<fs::DirNotifyEvent> batch) {
        hits.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(mu);
        for (auto& event : batch) events.push_back(std::move(event));
    });
    Check(started, L"watch: start on temp directory");
    Sleep(250);
    const int baseline = hits.load(std::memory_order_relaxed);
    Check(baseline <= 2, L"watch: overlapped pending is not treated as a change storm");

    const std::wstring file = dir + L"\\created.txt";
    HANDLE hf = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const char body[] = "pulse";
    DWORD written = 0;
    if (hf != INVALID_HANDLE_VALUE) {
        WriteFile(hf, body, sizeof(body) - 1, &written, nullptr);
        CloseHandle(hf);
    }
    const ULONGLONG deadline = GetTickCount64() + 2000;
    bool saw_added = false;
    while (GetTickCount64() < deadline) {
        {
            std::lock_guard<std::mutex> lock(mu);
            for (const auto& event : events) {
                if (event.action == FILE_ACTION_ADDED &&
                    _wcsicmp(event.name.c_str(), L"created.txt") == 0) {
                    saw_added = true;
                    break;
                }
            }
        }
        if (saw_added) break;
        Sleep(20);
    }
    Check(hits.load(std::memory_order_relaxed) > baseline, L"watch: new file notifies");
    Check(saw_added, L"watch: current directory create reports FILE_ACTION_ADDED");
    Sleep(200);
    Check(hits.load(std::memory_order_relaxed) - baseline < 30,
          L"watch: a single create does not spin the callback");
    watch.Stop();
    DeleteFileW(file.c_str());
    RemoveDirectoryW(dir.c_str());
}

bool SnapshotHasName(const fs::SnapshotPtr& snap, const wchar_t* name) {
    if (!snap) return false;
    for (const auto& entry : *snap) {
        if (_wcsicmp(entry.name.c_str(), name) == 0) return true;
    }
    return false;
}

void TestNavigateAlwaysEnumerates() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp), temp);
    const std::wstring parent = std::wstring(temp) + L"PulseEnumParent-" +
                                std::to_wstring(GetCurrentProcessId());
    const std::wstring child = parent + L"\\child";
    CreateDirectoryW(parent.c_str(), nullptr);
    CreateDirectoryW(child.c_str(), nullptr);

    const std::wstring before = parent + L"\\before.txt";
    HANDLE hf = CreateFileW(before.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);

    std::mutex mu;
    std::condition_variable cv;
    std::atomic<int> hits{0};
    WorkResult last;
    WorkerPool worker;
    worker.Start([&](WorkResult res) {
        std::lock_guard<std::mutex> lock(mu);
        last = std::move(res);
        hits.fetch_add(1, std::memory_order_relaxed);
        cv.notify_one();
    });

    const std::wstring path = fs::NormalizePath(parent);
    worker.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Asc);
    {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait_for(lock, std::chrono::seconds(10), [&] { return hits.load() >= 1; });
    }
    Check(!last.error && last.snapshot, L"enum: first parent listing succeeds");
    Check(SnapshotHasName(last.snapshot, L"before.txt"),
          L"enum: first listing contains the original file");
    Check(SnapshotHasName(last.snapshot, L"child"),
          L"enum: first listing contains the child folder");

    fs::DirectoryIdentity id1, id2;
    Check(fs::QueryDirectoryIdentity(path, id1), L"enum: query parent identity");

    const std::wstring after = parent + L"\\after-up.txt";
    hf = CreateFileW(after.c_str(), GENERIC_WRITE, 0, nullptr,
                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);

    Check(fs::QueryDirectoryIdentity(path, id2) && fs::SameDirectoryIdentity(id1, id2),
          L"enum: creating a child file does not change directory identity");

    const int at = hits.load();
    worker.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Asc);
    {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait_for(lock, std::chrono::seconds(10), [&] { return hits.load() > at; });
    }
    Check(!last.error && last.snapshot, L"enum: second parent listing succeeds");
    Check(SnapshotHasName(last.snapshot, L"after-up.txt"),
          L"enum: parent listing includes file created while viewing a child");

    worker.Stop();
    DeleteFileW(before.c_str());
    DeleteFileW(after.c_str());
    RemoveDirectoryW(child.c_str());
    RemoveDirectoryW(parent.c_str());
}

void TestSnapshotPatch() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp), temp);
    const std::wstring dir = std::wstring(temp) + L"PulsePatchTest-" +
                             std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring a = dir + L"\\alpha.txt";
    const std::wstring b = dir + L"\\beta.txt";
    HANDLE hf = CreateFileW(a.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);

    std::vector<fs::DirEntry> entries;
    fs::DirNotifyEvent added;
    added.action = FILE_ACTION_ADDED;
    added.name = L"alpha.txt";
    Check(ApplyDirNotify(entries, dir, added, ui::SortColumn::Name, ui::SortDirection::Asc) ==
              NotifyPatch::Applied &&
          entries.size() == 1 && _wcsicmp(entries[0].name.c_str(), L"alpha.txt") == 0,
          L"patch: ADDED inserts a real file");

    hf = CreateFileW(b.c_str(), GENERIC_WRITE, 0, nullptr,
                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
    added.name = L"beta.txt";
    Check(ApplyDirNotify(entries, dir, added, ui::SortColumn::Name, ui::SortDirection::Asc) ==
              NotifyPatch::Applied &&
          entries.size() == 2,
          L"patch: second ADDED keeps sort order");
    Check(_wcsicmp(entries[0].name.c_str(), L"alpha.txt") == 0 &&
          _wcsicmp(entries[1].name.c_str(), L"beta.txt") == 0,
          L"patch: name sort is alpha then beta");

    fs::DirNotifyEvent renamed;
    renamed.action = FILE_ACTION_RENAMED_NEW_NAME;
    renamed.old_name = L"beta.txt";
    renamed.name = L"gamma.txt";
    MoveFileW(b.c_str(), (dir + L"\\gamma.txt").c_str());
    Check(ApplyDirNotify(entries, dir, renamed, ui::SortColumn::Name, ui::SortDirection::Asc) ==
              NotifyPatch::Applied,
          L"patch: RENAMED applies");
    Check(entries.size() == 2 && _wcsicmp(entries[1].name.c_str(), L"gamma.txt") == 0,
          L"patch: rename updates the listing name");

    fs::DirNotifyEvent removed;
    removed.action = FILE_ACTION_REMOVED;
    removed.name = L"alpha.txt";
    DeleteFileW(a.c_str());
    Check(ApplyDirNotify(entries, dir, removed, ui::SortColumn::Name, ui::SortDirection::Asc) ==
              NotifyPatch::Applied &&
          entries.size() == 1,
          L"patch: REMOVED drops the file");

    fs::DirNotifyEvent nested;
    nested.action = FILE_ACTION_ADDED;
    nested.name = L"sub\\file.txt";
    Check(ApplyDirNotify(entries, dir, nested, ui::SortColumn::Name, ui::SortDirection::Asc) ==
              NotifyPatch::NeedFullEnum,
          L"patch: nested names require a full enumeration");

    DeleteFileW((dir + L"\\gamma.txt").c_str());
    RemoveDirectoryW(dir.c_str());
}

void TestSnapshotStorePutKeepsWorkerGeneration() {
    fs::SnapshotStore store(8);
    auto first = std::make_shared<std::vector<fs::DirEntry>>();
    fs::DirEntry a;
    a.name = L"a.txt";
    first->push_back(a);
    store.Update(L"C:\\pulse-gen", 3, first);

    auto incremental = std::make_shared<std::vector<fs::DirEntry>>(*first);
    fs::DirEntry b;
    b.name = L"b.txt";
    incremental->push_back(b);
    Check(store.Put(L"C:\\pulse-gen", incremental) == 3,
          L"snapshot: Put keeps the last worker generation");

    auto worker = std::make_shared<std::vector<fs::DirEntry>>();
    fs::DirEntry c;
    c.name = L"c.txt";
    worker->push_back(c);
    store.Update(L"C:\\pulse-gen", 4, worker);
    auto snap = store.Peek(L"C:\\pulse-gen");
    Check(snap && snap->size() == 1 && snap->front().name == L"c.txt",
          L"snapshot: later worker Update is not discarded after Put");

    store.Update(L"C:\\pulse-gen", 2, first);
    snap = store.Peek(L"C:\\pulse-gen");
    Check(snap && snap->front().name == L"c.txt",
          L"snapshot: older worker generation is still ignored");

    store.MarkDirty(L"C:\\pulse-gen");
    uint64_t gen = 0;
    Check(!store.GetOrStart(L"C:\\pulse-gen", gen) && gen == 4,
          L"snapshot: dirty GetOrStart does not bump past the worker generation");
    store.Update(L"C:\\pulse-gen", 5, first);
    snap = store.Peek(L"C:\\pulse-gen");
    Check(snap && snap->front().name == L"a.txt",
          L"snapshot: worker Update after dirty GetOrStart still applies");
}

void TestDataObject() {
    std::vector<std::wstring> paths{ L"C:\\fake_a.txt", L"D:\\fake_b.txt" };
    auto* obj = ui::FileDataObject::Create(paths);

    FORMATETC fmt{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    Check(obj->QueryGetData(&fmt) == S_OK, L"dnd: IDataObject advertises CF_HDROP");
    FORMATETC mixed{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1,
                     TYMED_HGLOBAL | TYMED_ISTREAM | TYMED_ISTORAGE };
    Check(obj->QueryGetData(&mixed) == S_OK, L"dnd: tymed is a bit field");
    FORMATETC stream_only{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM };
    Check(obj->QueryGetData(&stream_only) != S_OK, L"dnd: stream-only HDROP is unsupported");
    FORMATETC fnamew{ (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILENAMEW), nullptr,
                      DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    Check(obj->QueryGetData(&fnamew) == S_OK, L"dnd: FileNameW advertised");
    FORMATETC idl{ (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_SHELLIDLIST), nullptr,
                   DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    Check(obj->QueryGetData(&idl) == S_OK, L"dnd: Shell IDList advertised");
    STGMEDIUM med{};
    bool names_ok = false;
    if (SUCCEEDED(obj->GetData(&fmt, &med)) && med.hGlobal) {
        HDROP hd = static_cast<HDROP>(med.hGlobal);
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        wchar_t buf[MAX_PATH]{};
        names_ok = n == 2 &&
            DragQueryFileW(hd, 0, buf, MAX_PATH) && paths[0] == buf &&
            DragQueryFileW(hd, 1, buf, MAX_PATH) && paths[1] == buf;
        ReleaseStgMedium(&med);
    }
    Check(names_ok, L"dnd: CF_HDROP carries both paths in order");

    STGMEDIUM fnmed{};
    bool fn_ok = false;
    if (SUCCEEDED(obj->GetData(&fnamew, &fnmed)) && fnmed.hGlobal) {
        if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(fnmed.hGlobal))) {
            fn_ok = paths[0] == p;
            GlobalUnlock(fnmed.hGlobal);
        }
        ReleaseStgMedium(&fnmed);
    }
    Check(fn_ok, L"dnd: FileNameW is the first path");

    IEnumFORMATETC* en = nullptr;
    Check(SUCCEEDED(obj->EnumFormatEtc(DATADIR_GET, &en)) && en,
          L"dnd: EnumFormatEtc works");
    if (en) en->Release();

    // Target writes the performed effect back (Explorer move semantics).
    Check(ui::PreferredDropEffect(obj) == (DROPEFFECT_COPY | DROPEFFECT_MOVE),
          L"dnd: preferred effect advertised");
    obj->Release();
}

void TestClipboardText() {
    Check(ops::WriteClipboardText(L"C:\\A\\B"), L"clipboard: write text");
    bool ok = false;
    bool opened = false;
    for (int attempt = 0; attempt < 20 && !opened; ++attempt) {
        opened = OpenClipboard(nullptr) != FALSE;
        if (!opened) Sleep(5);
    }
    if (opened) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                ok = (p == std::wstring(L"C:\\A\\B"));
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
    Check(ok, L"clipboard: text round-trip");
}

void TestUniqueName() {
    CreateDirectoryW(kOpsTestRoot.c_str(), nullptr);
    CreateDirectoryW(kSandbox.c_str(), nullptr);
    CleanSandbox();
    std::wstring d = kSandbox;
    Check(UniqueChildName(d, L"新建文件夹", L"") == L"新建文件夹", L"new: first name");
    CreateDirectoryW((d + L"\\新建文件夹").c_str(), nullptr);
    Check(UniqueChildName(d, L"新建文件夹", L"") == L"新建文件夹 (2)", L"new: collision bumps index");
    Check(UniqueChildName(d, L"新建文本文档", L".txt") == L"新建文本文档.txt",
          L"new: text file extension");
}

void TestMultiSelect() {
    Tab tab;
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(5);
    for (int i = 0; i < 5; ++i) {
        (*entries)[static_cast<size_t>(i)].name = L"f" + std::to_wstring(i);
        (*entries)[static_cast<size_t>(i)].size = static_cast<uint64_t>(i + 1) * 100;
    }
    tab.SetSnapshot(entries);

    tab.SelectOnly(1);
    Check(tab.IsSelected(1) && tab.SelectedCount() == 1 && tab.selected_index == 1,
          L"select: click replaces");

    tab.ToggleSelect(3);
    Check(tab.IsSelected(1) && tab.IsSelected(3) && tab.SelectedCount() == 2,
          L"select: ctrl-click adds");

    tab.ToggleSelect(1);
    Check(!tab.IsSelected(1) && tab.IsSelected(3) && tab.SelectedCount() == 1,
          L"select: ctrl-click removes");

    tab.SelectRange(0, 3);
    Check(tab.SelectedCount() == 4 && tab.IsSelected(0) && tab.IsSelected(3) && !tab.IsSelected(4),
          L"select: shift range");

    tab.SelectAll();
    Check(tab.all_selected && tab.SelectedCount() == 5 && tab.IsSelected(4) && tab.selected.empty(),
          L"select: ctrl-a uses all_selected flag");

    tab.MoveFocus(2, false);
    Check(tab.SelectedCount() == 1 && tab.selected_index == 2 && !tab.all_selected,
          L"select: move focus replaces");

    tab.MoveFocus(4, true);
    Check(tab.SelectedCount() == 3 && tab.IsSelected(2) && tab.IsSelected(4) && tab.selected_index == 4,
          L"select: shift-extend");

    tab.ClearSelection();
    Check(tab.SelectedCount() == 0 && !tab.IsSelected(0) && tab.selected_index < 0,
          L"select: escape clears");

    tab.SelectOnly(0);
    tab.ToggleSelect(2);
    tab.ToggleSelect(4);
    std::vector<std::wstring> names{ L"f0", L"f2", L"f4" };
    auto resorted = std::make_shared<std::vector<fs::DirEntry>>(5);
    for (int i = 0; i < 5; ++i) {
        (*resorted)[static_cast<size_t>(i)].name = L"f" + std::to_wstring(4 - i);
    }
    tab.SetSnapshot(resorted);
    tab.RemapSelection(names, L"f2");
    Check(tab.IsSelected(0) && tab.IsSelected(2) && tab.IsSelected(4)
          && tab.selected_index == 2 && tab.SelectedCount() == 3,
          L"select: remap by name after sort");

    ui::PaneViewModel pane;
    pane.selected_index = 1;
    pane.all_selected = true;
    pane.selected_count = 5;
    Check(pane.IsRowSelected(0) && pane.IsRowSelected(4), L"select: view-model all_selected");
    pane.all_selected = false;
    std::unordered_set<int> selected_indices{ 3 };
    pane.selected_indices = &selected_indices;
    Check(pane.IsRowSelected(1) && pane.IsRowSelected(3) && !pane.IsRowSelected(0),
          L"select: view-model mixed set + focus");

    Check(NameMatchesPattern(L"photo.JPG", L"*.jpg") &&
          NameMatchesPattern(L"a1.txt", L"a?.txt") &&
          !NameMatchesPattern(L"photo.png", L"*.jpg") &&
          NameMatchesPattern(L"Report.pdf", L"report"),
          L"select: wildcard and substring name matching");

    auto mixed = std::make_shared<std::vector<fs::DirEntry>>(4);
    (*mixed)[0].name = L"keep.txt";
    (*mixed)[1].name = L"shot.jpg";
    (*mixed)[2].name = L"shot.JPG";
    (*mixed)[3].name = L"notes.md";
    tab.SetSnapshot(mixed);
    tab.SelectOnly(1);
    tab.InvertIndices({0, 1, 2, 3});
    Check(tab.IsSelected(0) && !tab.IsSelected(1) && tab.IsSelected(2) && tab.IsSelected(3) &&
          tab.SelectedCount() == 3,
          L"select: invert listing");

    tab.SelectOnly(0);
    tab.InvertIndices({1, 2});
    Check(tab.IsSelected(0) && tab.IsSelected(1) && tab.IsSelected(2) && !tab.IsSelected(3),
          L"select: invert only the visible subset");

    tab.filter_text = L"*.jpg";
    std::vector<int> jpg;
    CollectFilterMatches(tab, nullptr, jpg);
    Check(jpg.size() == 2 && jpg[0] == 1 && jpg[1] == 2, L"select: filter wildcard *.jpg");
    tab.SelectIndices(jpg);
    Check(tab.IsSelected(1) && tab.IsSelected(2) && !tab.IsSelected(0) && tab.SelectedCount() == 2,
          L"select: select matches *.jpg");
    tab.filter_text.clear();

    tab.SelectAll();
    tab.InvertIndices({0, 1, 2, 3});
    Check(tab.SelectedCount() == 0, L"select: invert of all_selected clears");
}

void TestHiddenFiles() {
    AppPrefs prefs;
    Check(prefs.FromJson(L"{}") && !prefs.show_hidden_files,
          L"hidden: old preferences default to hidden off");
    prefs.show_hidden_files = true;
    AppPrefs loaded;
    Check(loaded.FromJson(prefs.ToJson()) && loaded.show_hidden_files,
          L"hidden: preference JSON roundtrip");
    Pane pane;
    auto& tab = pane.view;
    tab.current_path = L"\\\\server\\share";
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(4);
    (*entries)[0].name = L"visible.txt";
    (*entries)[1].name = L"desktop.ini";
    (*entries)[1].attrs = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
    (*entries)[2].name = L".ordinary";
    (*entries)[3].name = L"hidden-folder";
    (*entries)[3].attrs = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_DIRECTORY;
    (*entries)[3].is_dir = true;
    tab.SetSnapshot(entries);
    ui::PaneViewModel vm;
    FillPaneViewModel(vm, pane);
    Check(vm.EntryCount() == 2 && vm.SourceIndex(1) == 2 && vm.ViewIndex(1) == -1,
          L"hidden: cached UNC attributes filter files and folders, not dot names");
    const auto cached = vm.filter_map;
    FillPaneViewModel(vm, pane);
    Check(vm.filter_map == cached && tab.snapshot == entries,
          L"hidden: visibility reuses map and preserves the complete snapshot");
    tab.SelectRange(0, 2);
    Check(tab.SelectedIndices() == std::vector<int>({0, 2}),
          L"hidden: range selection cannot include invisible files");
    tab.SelectAll();
    Check(tab.SelectedIndices() == std::vector<int>({0, 2}),
          L"hidden: select all excludes invisible files");
    tab.SetShowHiddenFiles(true);
    FillPaneViewModel(vm, pane);
    Check(vm.EntryCount() == 4 && tab.SelectedCount() == 0,
          L"hidden: toggle restores files without enumeration or stale selection");
    tab.SelectOnly(1);
    tab.SetShowHiddenFiles(false);
    FillPaneViewModel(vm, pane);
    Check(vm.EntryCount() == 2 && tab.SelectedCount() == 0 && tab.file_count == 2,
          L"hidden: toggle back resets selection and visible counts");
    auto changed = std::make_shared<std::vector<fs::DirEntry>>(*entries);
    tab.SelectOnly(0);
    (*changed)[0].attrs = FILE_ATTRIBUTE_HIDDEN;
    tab.SetSnapshot(changed);
    FillPaneViewModel(vm, pane);
    Check(vm.EntryCount() == 1 && tab.SelectedCount() == 0,
          L"hidden: watcher attributes invalidate visibility and stale selection");
    tab.SetSnapshot(entries);
    tab.InvertIndices({0, 1, 2, 3});
    Check(tab.SelectedIndices() == std::vector<int>({0, 2}),
          L"hidden: inverse selection excludes hidden entries");
    tab.filter_text = L"*.ini";
    FillPaneViewModel(vm, pane);
    Check(vm.EntryCount() == 0, L"hidden: name filtering composes with hidden filtering");
    tab.filter_text.clear();
    tab.current_path = L"pulse:recycle";
    tab.SetSnapshot(entries);
    FillPaneViewModel(vm, pane);
    Check(vm.EntryCount() == 4, L"hidden: recycle payloads remain accessible");
}

void TestSplitLayout() {
    Pane a, b, c, d;
    std::vector<Pane*> two{ &a, &b };
    auto tree = MakePresetTree(LayoutPreset::TwoVertical, two);
    Check(tree && !tree->is_leaf, L"layout: two-vertical is a split");
    std::vector<Pane*> vis;
    tree->CollectPanes(vis);
    Check(vis.size() == 2 && vis[0] == &a && vis[1] == &b, L"layout: two-vertical leaves");

    D2D1_RECT_F bounds = D2D1::RectF(0, 0, 1000, 800);
    std::vector<std::pair<Pane*, D2D1_RECT_F>> laid;
    LayoutSplitTree(*tree, bounds, 8.0f, laid);
    Check(laid.size() == 2, L"layout: two leaves placed");
    Check(laid.size() == 2 && laid[0].second.right <= laid[1].second.left + 0.1f,
          L"layout: left pane is left of right");

    std::vector<SplitterLayout> splitters;
    laid.clear();
    LayoutSplitTree(*tree, bounds, 8.0f, laid, &splitters);
    Check(splitters.size() == 1 && splitters[0].orientation == SplitOrientation::Vertical,
          L"layout: two-vertical exposes a vertical splitter");
    ApplySplitRatio(*tree, bounds, 8.0f, 250.0f, 400.0f);
    laid.clear();
    LayoutSplitTree(*tree, bounds, 8.0f, laid);
    Check(laid.size() == 2 && laid[0].second.right < 300.0f &&
          laid[1].second.left > laid[0].second.right,
          L"layout: dragging splitter resizes panes");
    std::vector<float> ratios;
    CollectSplitRatios(*tree, ratios);
    Check(ratios.size() == 1 && ratios[0] < 0.35f, L"layout: collect dragged split ratio");
    auto restored = MakePresetTree(LayoutPreset::TwoVertical, two);
    ApplySplitRatios(*restored, ratios);
    laid.clear();
    LayoutSplitTree(*restored, bounds, 8.0f, laid);
    Check(laid.size() == 2 && laid[0].second.right < 300.0f,
          L"layout: apply saved split ratio");

    std::vector<Pane*> four{ &a, &b, &c, &d };
    auto grid = MakePresetTree(LayoutPreset::FourGrid, four);
    vis.clear();
    grid->CollectPanes(vis);
    Check(vis.size() == 4, L"layout: four-grid has 4 leaves");
    Check(LayoutPresetCount(LayoutPreset::Three) == 3, L"layout: three count");

    auto splitMenu = BuildSplitMenu(1);
    Check(splitMenu.size() >= 5 && splitMenu[1].command == CmdLayoutTwoVertical,
          L"menu: split presets");
    Check(splitMenu[1].radio && splitMenu[0].radio_group && !splitMenu[0].radio,
          L"menu: split radio marks current preset on the leading edge");
    Check(splitMenu[1].text.find(L"●") == std::wstring::npos,
          L"menu: split selection is not a text bullet");
    Check(splitMenu[0].pictogram == ui::fluent::MenuPictogram::LayoutSingle &&
          splitMenu[1].pictogram == ui::fluent::MenuPictogram::LayoutSideBySide &&
          splitMenu[2].pictogram == ui::fluent::MenuPictogram::LayoutStacked &&
          splitMenu[3].pictogram == ui::fluent::MenuPictogram::LayoutThree &&
          splitMenu[4].pictogram == ui::fluent::MenuPictogram::LayoutFour,
          L"menu: split presets use distinct layout pictograms");
    auto pal = BuildCommandPalette({ L"C:\\Users" });
    Check(!pal.empty() && pal.back().command == CmdRecentBase, L"menu: command palette recent");
    Check(pal.back().badge_text == L"历史路径" && pal.back().shortcut.empty(),
          L"menu: recent rows use 历史路径 badge");
    auto palQ = BuildCommandPalette(L"四宫", { L"C:\\Users" }, {}, false);
    Check(!palQ.empty() && palQ[0].command == CmdLayoutFourGrid, L"menu: palette filters by query");
    auto palLong = BuildCommandPalette(L"", { L"\\\\?\\C:\\Users\\W" }, {}, false);
    bool recent_ok = false;
    for (const auto& it : palLong) {
        if (it.command != CmdRecentBase) continue;
        Check(it.shortcut.find(L"\\\\?\\") == std::wstring::npos,
              L"menu: palette recent must not keep \\\\?\\ prefix");
        Check(it.badge_text == L"历史路径", L"menu: long path recent uses badge");
        Check(it.text == L"W", L"menu: recent title is folder name");
        recent_ok = true;
    }
    Check(recent_ok, L"menu: palette shows Windows-style path title");

    const auto qDot = ParseOmnibarQuery(L".codex");
    Check(qDot.kind == OmnibarQuery::Kind::Mixed && qDot.needle == L".codex" &&
          !LooksLikeFilesystemPath(qDot.needle), L"omnibar: dot folder uses filename search");
    const auto qDotCommand = ParseOmnibarQuery(L">.codex");
    Check(qDotCommand.kind == OmnibarQuery::Kind::Command,
          L"omnibar: explicit command prefix remains supported");
    const auto qDotProject = ParseOmnibarQuery(L".codex", true);
    Check(qDotProject.kind == OmnibarQuery::Kind::Project,
          L"omnibar: project scope preserved");
    const auto qCmd = ParseOmnibarQuery(L">新建");
    Check(qCmd.kind == OmnibarQuery::Kind::Command && qCmd.needle == L"新建" && qCmd.prefix == L'>',
          L"omnibar: > prefix is command");
    const auto qSearch = ParseOmnibarQuery(L"?foo");
    Check(qSearch.kind == OmnibarQuery::Kind::Search && qSearch.needle == L"foo",
          L"omnibar: ? prefix is search");
    const auto qSlash = ParseOmnibarQuery(L"/bar");
    Check(qSlash.kind == OmnibarQuery::Kind::Search && qSlash.needle == L"bar" && qSlash.prefix == L'/',
          L"omnibar: / prefix is search alias");
    const auto qPath = ParseOmnibarQuery(L"C:\\Windows");
    Check(qPath.kind == OmnibarQuery::Kind::Mixed && qPath.needle == L"C:\\Windows",
          L"omnibar: bare path has no prefix");
    Check(LooksLikeFilesystemPath(L"C:\\Windows") && LooksLikeFilesystemPath(L"\\\\server\\share") &&
          !LooksLikeFilesystemPath(L"新建文件夹"),
          L"omnibar: path-like detection");
    auto palCmd = BuildCommandPalette(L">新建", { L"C:\\新建" }, {}, false);
    Check(!palCmd.empty() && palCmd[0].command == CmdNewFolder, L"omnibar: >新建 matches 新建文件夹");
    bool cmd_has_layout = false;
    for (const auto& it : palCmd)
        if (it.command == CmdLayoutFourGrid) cmd_has_layout = true;
    Check(!cmd_has_layout, L"omnibar: >新建 does not keep unrelated commands");
    Check(palCmd.back().command == CmdRecentBase && palCmd.back().badge_text == L"历史路径",
          L"omnibar: command mode still lists recents at the end");
    auto palSearch = BuildCommandPalette(L"?foo", { L"C:\\Users" }, {}, false);
    bool search_has_cmd = false;
    for (const auto& it : palSearch)
        if (it.command == CmdSettings || it.command == CmdNewFolder) search_has_cmd = true;
    Check(!search_has_cmd, L"omnibar: ? search hides commands");
    auto palDup = BuildCommandPalette(L"", { L"C:\\A", L"C:\\A", L"C:\\B" }, {}, false, 0, L"C:\\B");
    int recent_n = 0;
    int recent_idx = -1;
    for (const auto& it : palDup) {
        if (it.command >= CmdRecentBase && it.command < CmdIndexBase) {
            ++recent_n;
            recent_idx = it.command - CmdRecentBase;
        }
    }
    Check(recent_n == 1 && recent_idx == 0, L"omnibar: recents dedupe and skip current path");
    index::Hit hit;
    hit.name = L"Users";
    hit.path = L"C:\\Users";
    hit.is_dir = true;
    auto palSkipHit = BuildCommandPalette(L"", { L"C:\\Users" }, { hit }, false);
    bool recent_after_hit = false;
    for (const auto& it : palSkipHit)
        if (it.command == CmdRecentBase) recent_after_hit = true;
    Check(!recent_after_hit, L"omnibar: recent skipped when index already listed the path");

    ui::MainRenderer chrome;
    chrome.SetScale(1.0f);
    ui::WindowViewModel chrome_vm;
    ui::TabView chrome_tab;
    chrome_tab.title = L"Test";
    chrome_tab.active = true;
    chrome_vm.tabs.push_back(chrome_tab);
    Check(std::abs(chrome.TitleBarHeight() - ui::kTitleBarHeight) < 0.01f,
          L"chrome: title bar uses kTitleBarHeight");
    D2D1_RECT_F tab_rc{};
    Check(chrome.TabItemRect(chrome_vm, 1400.0f, 0, &tab_rc) &&
          std::abs((tab_rc.bottom - tab_rc.top) - (ui::kTitleBarHeight - 8.0f)) < 0.01f,
          L"chrome: tab height is title bar minus 8dip padding");
    Check(tab_rc.right - tab_rc.left > 170.0f,
          L"chrome: a single tab can grow past the old 176px cap");

    ui::WindowViewModel pinned_vm;
    ui::TabView pinned_tab;
    pinned_tab.title = L"Pinned";
    pinned_tab.active = true;
    pinned_tab.pinned = true;
    pinned_vm.tabs.push_back(pinned_tab);
    D2D1_RECT_F pinned_rc{};
    Check(chrome.TabItemRect(pinned_vm, 758.0f, 0, &pinned_rc) &&
          std::abs((pinned_rc.right - pinned_rc.left) - 36.0f) < 0.01f,
          L"chrome: pinned tab keeps its icon-only width");
    const float pinned_y = (pinned_rc.top + pinned_rc.bottom) * 0.5f;
    const auto pinned_hit = chrome.HitTest(pinned_vm, D2D1::RectF(0, 0, 758, 269),
                                           pinned_rc.right - 1.0f, pinned_y);
    const auto new_tab_hit = chrome.HitTest(pinned_vm, D2D1::RectF(0, 0, 758, 269),
                                            pinned_rc.right + 5.0f, pinned_y);
    Check(pinned_hit.region == ui::HitTestResult::Tab &&
          new_tab_hit.region == ui::HitTestResult::TabNew,
          L"chrome: pinned tab and new-tab button do not overlap");

    ui::PaneViewModel pvm;
    pvm.filter_text = L"foo";
    pvm.filter_map = std::make_shared<ui::PaneViewModel::FilterMap>(
        ui::PaneViewModel::FilterMap{ 2, 5, 9 });
    Check(pvm.EntryCount() == 3 && pvm.SourceIndex(1) == 5 && pvm.ViewIndex(9) == 2,
          L"filter: view/source index mapping");
}

void TestQuickAccess() {
    PlacesCatalog cat;
    cat.persist = false;
    Check(cat.SetQuickAccessPinned({L"C:\\", L"C:\\Projects", L"c:/projects/",
                                   L"\\\\offline-host\\share\\folder", L"pulse:recent"}, true) &&
          cat.quick_access_paths.size() == 3, L"quick access: roots, UNC and normalized deduplication");
    Check(!cat.SetQuickAccessPinned({L"C:\\Projects"}, true), L"quick access: pin is idempotent");
    cat.ToggleStarred(L"C:\\Projects", PlaceItemKind::Folder);
    cat.RemapPaths(L"C:\\Projects", L"C:\\Renamed");
    Check(cat.IsQuickAccessPinned(L"C:\\Renamed") && !cat.IsQuickAccessPinned(L"C:\\Projects"),
          L"quick access: rename remaps pinned path");
    cat.SetQuickAccessPinned({L"C:\\Renamed"}, false);
    Check(cat.IsStarred(L"C:\\Renamed"), L"quick access: unpin preserves independent favorite");
    cat.SetQuickAccessPinned({L"C:\\tree\\child", L"C:\\other\\child"}, true);
    cat.RemapPaths(L"C:\\tree", L"C:\\other");
    Check(cat.quick_access_paths.size() == 3, L"quick access: descendant remap deduplicates destination");

    Tab tab;
    tab.current_path = L"C:\\";
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    fs::DirEntry folder; folder.name = L"folder"; folder.is_dir = true;
    fs::DirEntry file; file.name = L"file.txt";
    entries->push_back(folder); entries->push_back(folder); entries->push_back(file);
    tab.snapshot = entries;
    tab.SelectOnly(0);
    Check(QuickAccessTargets(&tab, false).size() == 1, L"quick access: selected directory");
    tab.selected.insert(1);
    Check(QuickAccessTargets(&tab, false).size() == 2, L"quick access: multiple directories");
    tab.selected.insert(2);
    Check(QuickAccessTargets(&tab, false).empty(), L"quick access: mixed selection rejected");
    Check(QuickAccessTargets(&tab, true) == std::vector<std::wstring>{L"C:\\"},
          L"quick access: background targets current path, ignores selection");
    tab.current_path = MakeRecyclePath();
    Check(QuickAccessTargets(&tab, false).empty() && QuickAccessTargets(&tab, true).empty(),
          L"quick access: recycle location rejected");

    {
        AppState state;
        state.places.persist = false;
        state.ctxMenuPrefs.persist = false;
        std::vector<ui::FluentMenuItem> menu;
        AppendQuickAccessCommand(state, menu, {L"C:\\one", L"C:\\two"});
        Check(menu.size() == 1 && menu[0].command == CmdPinQuickAccess &&
              menu[0].text == L"固定到快速访问", L"quick access: menu label and pin command");
        state.places.SetQuickAccessPinned({L"C:\\one"}, true);
        menu.clear();
        AppendQuickAccessCommand(state, menu, {L"C:\\one", L"C:\\two"});
        Check(menu.size() == 1 && menu[0].command == CmdPinQuickAccess,
              L"quick access: mixed pin state offers idempotent pin");
        state.places.SetQuickAccessPinned({L"C:\\two"}, true);
        menu.clear();
        AppendQuickAccessCommand(state, menu, {L"C:\\one", L"C:\\two"});
        Check(menu.size() == 1 && menu[0].command == CmdUnpinQuickAccess &&
              menu[0].text == L"从快速访问取消固定", L"quick access: all pinned offers unpin");
        state.ctxMenuPrefs.SetItemEnabled(L"pulse:quick-access", false);
        menu.clear();
        AppendQuickAccessCommand(state, menu, {L"C:\\one"});
        Check(menu.empty(), L"quick access: menu preference is respected");
        Pane pane; pane.NewTab(L"C:\\");
        const auto sidebar = BuildSidebarModel();
        const auto vm = BuildWindowViewModel(pane, sidebar, true, false, false, &state.places, 0);
        const auto group = std::find_if(vm.sidebar.begin(), vm.sidebar.end(), [](const auto& value) {
            return !value.items.empty() && std::any_of(value.items.begin(), value.items.end(),
                [](const auto& item) { return item.path == fs::NormalizePath(L"C:\\two"); });
        });
        Check(group != vm.sidebar.end() && group->items.back().indent == 0 &&
              group->items.back().path == fs::NormalizePath(L"C:\\two"),
              L"quick access: independent sidebar entry appended at top level");
    }

    wchar_t previous[32768]{};
    GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", previous, ARRAYSIZE(previous));
    const auto test_dir = kSandbox + L"\\quick_access_profile";
    CreateDirectoryW(test_dir.c_str(), nullptr);
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", test_dir.c_str());
    {
        cat.persist = true;
        Check(cat.Save(), L"quick access: save profile");
        PlacesCatalog loaded;
        Check(loaded.Load() && loaded.quick_access_paths == cat.quick_access_paths,
              L"quick access: disk roundtrip retains order and offline paths");
        WriteUtf8FileAtomic(test_dir + L"\\places.json", L"{\"starred_items\":[]}");
        Check(loaded.Load() && loaded.quick_access_paths.empty(), L"quick access: old profile defaults empty");
    }
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", previous[0] ? previous : nullptr);
    cat.persist = false;
    Check(compat::WindowDpi(GetDesktopWindow()) > 0 &&
          compat::SystemMetricsForDpi(SM_CXSIZEFRAME, 144) > 0, L"compat: current DPI path");
    wchar_t previous_compat[16]{};
    GetEnvironmentVariableW(L"PULSE_COMPAT_81", previous_compat, ARRAYSIZE(previous_compat));
    SetEnvironmentVariableW(L"PULSE_COMPAT_81", L"1");
    Check(!compat::ModernWindows() && compat::WindowDpi(GetDesktopWindow()) > 0 &&
          compat::SystemMetricsForDpi(SM_CXSIZEFRAME, 144) > 0, L"compat: legacy DPI path");
    SetEnvironmentVariableW(L"PULSE_COMPAT_81", previous_compat[0] ? previous_compat : nullptr);
}

void TestPlacesAndIndex() {
    Check(fs::IsVirtualPath(L"pulse:tag:0") && fs::IsVirtualPath(L"pulse:search:foo") &&
          fs::IsVirtualPath(L"pulse:starred"),
          L"places: pulse: paths are virtual");
    Check(!fs::IsVirtualPath(L"C:\\Users"), L"places: filesystem path is not virtual");
    std::wstring kind, rest;
    Check(ParsePulsePath(L"pulse:tag:3", &kind, &rest) && kind == L"tag" && rest == L"3",
          L"places: parse tag virtual path");
    Check(ParsePulsePath(L"pulse:search:hello", &kind, &rest) && kind == L"search" && rest == L"hello",
          L"places: parse search virtual path");
    Check(ParsePulsePath(L"pulse:starred", &kind, &rest) && kind == L"starred" && rest.empty(),
          L"places: parse starred virtual path");
    Check(MakeStarredPath() == L"pulse:starred", L"places: starred path helper");
    Check(ParsePulsePath(L"pulse:recent", &kind, &rest) && kind == L"recent" && rest.empty() &&
          MakeRecentPath() == L"pulse:recent", L"places: recent virtual path");
    Check(ParsePulsePath(L"pulse:recycle", &kind, &rest) && kind == L"recycle" && rest.empty() &&
          MakeRecyclePath() == L"pulse:recycle" && fs::IsRecycleViewPath(L"pulse:recycle"),
          L"places: recycle virtual path");

    PlacesCatalog cat;
    cat.persist = false;
    cat.EnsureDefaults();
    Check(cat.tags.size() == 7, L"places: seven default color tags");
    Check(!cat.tags[0].id.empty() && cat.ResolveTagRef(L"0") == cat.tags[0].id &&
          cat.ResolveTagRef(cat.tags[0].id) == cat.tags[0].id,
          L"places: stable tag id and legacy index resolution");
    Check(cat.PinWorkspace(L"C:\\proj", L"proj", 1, { L"C:\\proj" }) == 0, L"places: pin workspace");
    Check(cat.FindWorkspace(L"C:\\proj") == 0, L"places: find workspace");
    Check(cat.PinWorkspace(L"C:\\other", L"other", 0, { L"C:\\other" }) == 1,
          L"places: pin second workspace");
    Check(cat.active_workspace == 1, L"places: latest pin becomes active");
    Check(cat.UnpinWorkspace(0) && cat.workspaces.size() == 1 &&
          cat.FindWorkspace(L"C:\\other") == 0 && cat.active_workspace == 0,
          L"places: unpin first workspace shifts later index");
    Check(cat.UnpinWorkspace(L"C:\\other") && cat.workspaces.empty() &&
          cat.active_workspace == -1,
          L"places: unpin active workspace clears current");
    Check(!cat.UnpinWorkspace(0) && !cat.UnpinWorkspace(L"C:\\missing"),
          L"places: unpin missing workspace is a no-op");
    Check(cat.PinWorkspace(L"C:\\proj", L"proj", 1, { L"C:\\proj" }) == 0, L"places: re-pin workspace");
    Check(cat.ToggleTag(0, L"C:\\proj\\a.txt") && cat.PathHasTag(L"C:\\proj\\a.txt", 0),
          L"places: toggle tag on");
    Check(cat.ToggleTag(0, L"C:\\proj\\a.txt") && !cat.PathHasTag(L"C:\\proj\\a.txt", 0),
          L"places: toggle tag off");
    Check(cat.ToggleStarred(L"C:\\proj\\a.txt", PlaceItemKind::File) &&
          cat.IsStarred(L"C:\\proj\\a.txt") && cat.IsStarred(L"\\\\?\\C:\\proj\\a.txt") &&
          cat.starred_items.size() == 1 &&
          cat.starred_items[0].path.find(L".lnk") == std::wstring::npos &&
          cat.starred_items[0].path.find(L"a.txt") != std::wstring::npos,
          L"places: star indexes the real path, not a .lnk");
    Check(cat.ToggleStarred(L"C:\\proj\\folder", PlaceItemKind::Folder) &&
          cat.IsStarred(L"C:\\proj\\folder") && cat.starred_items.size() == 2 &&
          cat.starred_items[0].kind == PlaceItemKind::Folder,
          L"places: folders are indexed the same way as files");
    Check(cat.SetStarredBadge(L"C:\\proj\\folder", L"  xx项目  ", 0x123456) &&
          cat.FindStarred(L"c:\\PROJ\\folder") &&
          cat.FindStarred(L"c:\\PROJ\\folder")->badge == L"xx项目" &&
          cat.FindStarred(L"C:\\proj\\folder")->badge_rgb == 0x123456,
          L"places: starred badge trims text and preserves color");
    Check(!cat.ToggleStarred(L"C:\\proj\\a.txt") && !cat.IsStarred(L"C:\\proj\\a.txt") &&
          cat.IsStarred(L"C:\\proj\\folder"),
          L"places: unstar removes only that index");
    Check(!cat.ToggleStarred(L"C:\\proj\\folder") && cat.starred_items.empty(),
          L"places: unstar last folder clears the index");
    Check(!cat.ToggleStarred(L"pulse:starred") && cat.starred_items.empty(),
          L"places: virtual paths cannot be starred");
    for (int i = 0; i < 105; ++i)
        cat.RecordRecent(L"C:\\recent\\item" + std::to_wstring(i), PlaceItemKind::File);
    Check(cat.recent_items.size() == 100 &&
          cat.recent_items.front().path.find(L"item104") != std::wstring::npos,
          L"places: recent list caps at 100 and keeps newest first");
    cat.RecordRecent(L"C:\\recent\\item50", PlaceItemKind::Folder);
    Check(cat.recent_items.front().path.find(L"item50") != std::wstring::npos &&
          cat.recent_items.front().kind == PlaceItemKind::Folder &&
          cat.RecentItems(RecentFilter::Folders).size() == 1,
          L"places: repeated recent item moves to front and updates kind");
    Check(cat.RemoveRecent(L"c:\\RECENT\\item50") &&
          cat.RecentItems(RecentFilter::Folders).empty(),
          L"places: remove recent is case insensitive");
    Check(cat.ClearRecent() && cat.recent_items.empty(), L"places: clear recent");
    Check(cat.SetTagged(1, L"C:\\proj\\a.txt", true) && cat.PathHasTag(L"C:\\proj\\a.txt", 1),
          L"places: set tagged");
    const uint64_t tag_revision = cat.TagRevision();
    std::vector<TagAdsUpdate> deferred_ads;
    Check(cat.SetTaggedBatch(2,
                             { L"C:\\proj\\batch-a.txt", L"C:\\proj\\batch-b.txt",
                               L"C:\\proj\\batch-a.txt" },
                             true, &deferred_ads) &&
          cat.TagRevision() == tag_revision + 1 && deferred_ads.size() == 2 &&
          cat.PathHasTag(L"C:\\proj\\batch-a.txt", 2) &&
          cat.PathHasTag(L"C:\\proj\\batch-b.txt", 2),
          L"places: batch tagging rebuilds once and deduplicates paths");
    deferred_ads.clear();
    Check(cat.ToggleTagBatch(2,
                             { L"C:\\proj\\batch-a.txt", L"C:\\proj\\batch-b.txt" },
                             &deferred_ads) && deferred_ads.size() == 2 &&
          !cat.PathHasTag(L"C:\\proj\\batch-a.txt", 2) &&
          !cat.PathHasTag(L"C:\\proj\\batch-b.txt", 2),
          L"places: batch tag toggle");
    const TagId custom_id = cat.CreateTag(L"  自定义标签  ", 0x123456);
    Check(!custom_id.empty() && cat.FindTag(custom_id) &&
          cat.FindTag(custom_id)->name == L"自定义标签",
          L"places: create tag trims name and assigns stable id");
    std::vector<std::wstring> mixed_paths{ L"C:\\proj\\one.txt", L"C:\\proj\\two.txt" };
    Check(cat.SetTagsBatch(custom_id, { mixed_paths[0] }, true) &&
          cat.GetSelectionState(custom_id, mixed_paths) == TagSelectionState::Mixed,
          L"places: mixed multi-selection state");
    Check(cat.SetTagsBatch(custom_id, mixed_paths, true) &&
          cat.GetSelectionState(custom_id, mixed_paths) == TagSelectionState::All,
          L"places: mixed click unifies to all");
    Check(cat.RenameTag(custom_id, L"已重命名") &&
          cat.SetTagColor(custom_id, 0x654321) &&
          cat.FindTag(custom_id)->name == L"已重命名" &&
          cat.FindTag(custom_id)->rgb == 0x654321,
          L"places: rename and recolor preserve tag id");
    cat.RemapPaths(L"C:\\proj\\one.txt", L"C:\\moved\\one.txt");
    cat.CloneAssignments(L"C:\\proj\\two.txt", L"C:\\copy\\two.txt");
    Check(cat.GetSelectionState(custom_id, { L"C:\\moved\\one.txt" }) == TagSelectionState::All &&
          cat.GetSelectionState(custom_id, { L"C:\\copy\\two.txt" }) == TagSelectionState::All,
          L"places: move remaps and copy clones assignments");
    Check(cat.DeleteTag(custom_id) == 3 && !cat.FindTag(custom_id),
          L"places: deleting tag removes all assignments");

    const std::wstring ads_file = kSandbox + L"\\tag_ads_v2.tmp";
    HANDLE ads_handle = CreateFileW(ads_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (ads_handle != INVALID_HANDLE_VALUE) CloseHandle(ads_handle);
    const std::vector<TagAdsRecord> ads_records{
        { L"stable-id", L"设计", 0x3B82F6 }, { L"stable-id-2", L"复核", 0xEF4444 }
    };
    Check(WriteTagAdsV2(ads_file, ads_records), L"tags: write v2 ADS");
    const auto loaded_ads = ReadTagAdsV2(ads_file);
    Check(loaded_ads.size() == 2 && loaded_ads[0].id == L"stable-id" &&
          loaded_ads[0].name == L"设计" && loaded_ads[0].rgb == 0x3B82F6,
          L"tags: read v2 ADS metadata");
    PlacesCatalog imported;
    imported.persist = false;
    imported.EnsureDefaults();
    const size_t imported_definition_count = imported.tags.size();
    const TagId existing_id = imported.tags[0].id;
    const std::wstring existing_name = imported.tags[0].name;
    Check(WriteTagAdsV2(ads_file, { { L"external-id", existing_name, 0x010203 } }),
          L"tags: write same-name external ADS record");
    imported.ReadAdsIntoCatalog(ads_file);
    Check(imported.tags.size() == imported_definition_count &&
          imported.GetSelectionState(existing_id, { ads_file }) == TagSelectionState::All &&
          !imported.FindTag(L"external-id"),
          L"tags: ADS import reuses case-insensitive existing definition");
    DeleteFileW(ads_file.c_str());
    Check(cat.PinNetwork(L"\\\\server\\share", L"share") == 0, L"places: pin UNC");

    index::Engine engine;
    engine.AddForTest(L"C:\\proj\\src", L"src", true);
    engine.AddForTest(L"C:\\proj\\readme.md", L"readme.md", false);
    index::Query q;
    q.needle = L"read";
    auto hits = engine.Search(q);
    Check(hits.total == 1 && hits.hits.size() == 1 && hits.hits[0].name == L"readme.md",
          L"index: substring search");
    q.needle = L"src";
    q.folders_only = true;
    hits = engine.Search(q);
    Check(hits.total == 1 && hits.hits[0].is_dir, L"index: folders_only");

    engine.AddForTest(L"C:\\proj\\notes.txt", L"notes.txt", false);
    engine.AddForTest(L"C:\\other\\read.exe", L"read.exe", false);
    engine.AddForTest(L"C:\\proj\\src.bak", L"src.bak", false);
    q.folders_only = false;
    q.rank = false;
    q.limit = 48;
    q.needle = L"read md";
    hits = engine.Search(q);
    Check(hits.total == 1 && hits.hits[0].name == L"readme.md", L"index: AND terms");
    q.needle = L"readme.md | notes.txt";
    hits = engine.Search(q);
    Check(hits.total == 2, L"index: OR terms");
    q.needle = L"read !md";
    hits = engine.Search(q);
    Check(hits.total == 1 && hits.hits[0].name == L"read.exe", L"index: NOT term");
    q.needle = L"ext:md";
    hits = engine.Search(q);
    Check(hits.total == 1 && hits.hits[0].name == L"readme.md", L"index: ext: filter");
    q.needle = L"folder:src";
    hits = engine.Search(q);
    Check(hits.total == 1 && hits.hits[0].is_dir, L"index: folder: filter");
    q.needle = L"readme.*";
    hits = engine.Search(q);
    Check(hits.total == 1 && hits.hits[0].name == L"readme.md", L"index: wildcard");
    q.needle = L"\"readme.md\"";
    hits = engine.Search(q);
    Check(hits.total == 1, L"index: quoted exact");
    q.needle = L"path:other ext:exe";
    hits = engine.Search(q);
    Check(hits.total == 1 && hits.hits[0].name == L"read.exe", L"index: path: filter");
    q.needle = L"src";
    q.rank = true;
    q.limit = 1;
    hits = engine.Search(q);
    Check(hits.total >= 2 && hits.hits[0].name == L"src" && hits.hits[0].is_dir,
          L"index: exact folder ranks above prefix file");
    q.rank = false;
    q.limit = 48;
    q.needle = L"re";
    hits = engine.Search(q);
    const size_t n_re = hits.total;
    q.needle = L"read";
    hits = engine.Search(q);
    Check(hits.total >= 1 && hits.total <= n_re, L"index: longer needle narrows or equals");
    engine.AddForTest(L"C:\\proj\\big.bin", L"big.bin", false, 5ull * 1024 * 1024, 0);
    q.needle = L"ext:bin size:>1mb";
    q.rank = false;
    q.limit = 48;
    hits = engine.Search(q);
    Check(hits.total == 1 && hits.hits[0].name == L"big.bin", L"index: size: filter");

    index::Engine paged;
    paged.AddForTest(L"C:\\pages\\page_d.txt", L"page_d.txt", false);
    paged.AddForTest(L"C:\\pages\\page_b.txt", L"page_b.txt", false);
    paged.AddForTest(L"C:\\pages\\page_a.txt", L"page_a.txt", false);
    paged.AddForTest(L"C:\\pages\\page_c.txt", L"page_c.txt", false);
    index::Query page_query;
    page_query.needle = L"page_";
    page_query.rank = false;
    page_query.sort = index::ResultSort::Name;
    page_query.limit = 2;
    auto page1 = paged.Search(page_query);
    page_query.offset = 2;
    auto page2 = paged.Search(page_query);
    Check(page1.total == 4 && page1.hits.size() == 2 &&
          page1.hits[0].name == L"page_a.txt" && page1.hits[1].name == L"page_b.txt",
          L"index: sorted first page");
    Check(page2.total == 4 && page2.hits.size() == 2 &&
          page2.hits[0].name == L"page_c.txt" && page2.hits[1].name == L"page_d.txt",
          L"index: sorted later page uses offset");

    auto cache_sample = std::make_shared<std::vector<fs::DirEntry>>();
    for (int i = 0; i < 32; ++i) {
        fs::DirEntry entry;
        entry.name = L"snapshot-entry-" + std::to_wstring(i) + std::wstring(32, L'x');
        cache_sample->push_back(std::move(entry));
    }
    fs::SnapshotStore measured_store(8, 0);
    measured_store.Update(L"C:\\cache-a", 1, cache_sample);
    const size_t one_snapshot_bytes = measured_store.ResidentBytes();
    fs::SnapshotStore bounded_store(8, one_snapshot_bytes + one_snapshot_bytes / 2);
    bounded_store.Update(L"C:\\cache-a", 1, cache_sample);
    bounded_store.Update(L"C:\\cache-b", 2, cache_sample);
    Check(bounded_store.EntryCount() == 1 && !bounded_store.Peek(L"C:\\cache-a") &&
          bounded_store.Peek(L"C:\\cache-b"),
          L"snapshot: byte budget evicts least-recently-used directory");

    fs::SnapshotStore recent_store(2, 0);
    recent_store.Update(L"C:\\cache-a", 1, cache_sample);
    recent_store.Update(L"C:\\cache-b", 2, cache_sample);
    uint64_t recent_generation = 0;
    Check(recent_store.GetOrStart(L"C:\\cache-a", recent_generation) == cache_sample &&
          recent_generation == 1, L"snapshot: cache hit retains generation");
    recent_store.Update(L"C:\\cache-c", 3, cache_sample);
    Check(recent_store.Peek(L"C:\\cache-a") && !recent_store.Peek(L"C:\\cache-b") &&
          recent_store.ResidentBytes() == 2 * one_snapshot_bytes,
          L"snapshot: cache hit protects directory from capacity eviction");
    recent_store.Put(L"C:\\cache-a", cache_sample);
    recent_store.Update(L"C:\\cache-d", 4, cache_sample);
    Check(recent_store.Peek(L"C:\\cache-a") && !recent_store.Peek(L"C:\\cache-c") &&
          recent_store.ResidentBytes() == 2 * one_snapshot_bytes,
          L"snapshot: put refreshes recency without inflating memory accounting");

    Check(fs::IsUncPath(L"\\\\server\\share") && !fs::IsUncPath(L"C:\\Users"),
          L"net: UNC detection");
    {
        auto entries = std::make_shared<std::vector<fs::DirEntry>>();
        fs::DirEntry e;
        e.name = L"cached.txt";
        e.size = 42;
        e.is_dir = false;
        entries->push_back(e);
        const std::wstring unc = L"\\\\pulse-selftest\\share";
        Check(fs::SaveNetSnapshot(unc, entries), L"netcache: save UNC snapshot");
        uint64_t ts = 0;
        auto loaded = fs::LoadNetSnapshot(unc, &ts);
        Check(loaded && loaded->size() == 1 && (*loaded)[0].name == L"cached.txt" &&
              (*loaded)[0].size == 42, L"netcache: load UNC snapshot");
        Check(!fs::FormatCacheAge(ts).empty(), L"netcache: age string");
    }

    cat.PinWorkspace(L"C:\\proj", L"proj", 0, { L"C:\\proj" });
    cat.RecordVisit(L"C:\\proj\\src");
    cat.RecordVisit(L"C:\\proj\\src");
    cat.RecordVisit(L"C:\\proj\\docs");
    auto freq = cat.FrequentChildren(cat.FindWorkspace(L"C:\\proj"), 8);
    Check(!freq.empty() && freq[0].find(L"src") != std::wstring::npos,
          L"places: frequent child is most visited");

    Pane pane;
    pane.NewTab(L"C:\\proj");
    auto snap = std::make_shared<std::vector<fs::DirEntry>>();
    fs::DirEntry file;
    file.name = L"a.txt";
    snap->push_back(file);
    pane.ActiveTab()->SetSnapshot(snap);
    pane.ActiveTab()->filter_text = L"#红";
    cat.SetTagged(0, L"C:\\proj\\a.txt", true);
    ui::PaneViewModel pvm2;
    FillPaneViewModel(pvm2, pane, &cat);
    Check(pvm2.filter_map && pvm2.filter_map->size() == 1,
          L"filter: #红 matches default red tag");
    ui::PaneViewModel cached_pvm;
    FillPaneViewModel(cached_pvm, pane, &cat);
    Check(cached_pvm.filter_map == pvm2.filter_map,
          L"filter: unchanged view reuses cached index");
    Check(cached_pvm.tag_dots == pvm2.tag_dots,
          L"tags: unchanged view reuses cached row dots");

    Pane large_pane;
    large_pane.NewTab(L"C:\\large");
    auto large_snapshot = std::make_shared<std::vector<fs::DirEntry>>();
    large_snapshot->resize(100000);
    for (size_t i = 0; i < large_snapshot->size(); ++i)
        (*large_snapshot)[i].name = L"item-" + std::to_wstring(i) + L".dat";
    large_pane.ActiveTab()->SetSnapshot(large_snapshot);
    cat.SetTagged(0, L"C:\\large\\item-99999.dat", true);
    ui::PaneViewModel large_vm;
    FillPaneViewModel(large_vm, large_pane, &cat);
    Check(large_vm.EntryCount() == 100000 && large_vm.entries.empty() &&
          large_vm.tag_dots && large_vm.tag_dots->empty() &&
          large_vm.row_cache && large_vm.row_cache->rows.empty(),
          L"tags: 100k view keeps rows and tag dots lazily materialized");
}

void TestRecycleAndBatchRename() {
    Check(fs::RecycleIndexPath(L"C:\\$Recycle.Bin\\S-1-5-18\\$RABC123") ==
              L"C:\\$Recycle.Bin\\S-1-5-18\\$IABC123",
          L"recycle: $R content path maps to $I index");
    Check(fs::RecycleIndexPath(L"C:\\$Recycle.Bin\\sid\\$rxyz") ==
              L"C:\\$Recycle.Bin\\sid\\$ixyz",
          L"recycle: lowercase $r maps to $i");

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring index_path = std::wstring(temp) + L"pulse_recycle_index_test.$ITEST";
    const std::wstring original = L"C:\\Users\\SS\\Desktop\\photo.jpg";
    FILETIME deleted{};
    deleted.dwLowDateTime = 1;
    deleted.dwHighDateTime = 2;
    {
        HANDLE file = CreateFileW(index_path.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(file != INVALID_HANDLE_VALUE, L"recycle: create temp $I file");
        if (file != INVALID_HANDLE_VALUE) {
            uint64_t ver = 2;
            uint64_t size = 4096;
            uint32_t nchars = static_cast<uint32_t>(original.size() + 1);
            DWORD written = 0;
            WriteFile(file, &ver, 8, &written, nullptr);
            WriteFile(file, &size, 8, &written, nullptr);
            WriteFile(file, &deleted, 8, &written, nullptr);
            WriteFile(file, &nchars, 4, &written, nullptr);
            WriteFile(file, original.c_str(), nchars * 2, &written, nullptr);
            CloseHandle(file);
        }
    }
    fs::RecycleItem item;
    Check(fs::ReadRecycleIndex(index_path, item) && item.name == L"photo.jpg" &&
          item.original_path == original && item.size == 4096 &&
          item.deleted.dwHighDateTime == 2,
          L"recycle: parse Windows $I v2 metadata");
    DeleteFileW(index_path.c_str());

    fs::RecycleBinInfo info;
    fs::QueryRecycleBinInfo(info);
    Check(true, L"recycle: SHQueryRecycleBin occupancy probe");

    std::wstring stem, ext;
    SplitFileName(L"photo.jpg", stem, ext);
    Check(stem == L"photo" && ext == L".jpg", L"batch-rename: split stem/ext");
    SplitFileName(L".gitignore", stem, ext);
    Check(stem == L".gitignore" && ext.empty(), L"batch-rename: leading-dot name has no ext");
    Check(!IsValidFileName(L"a:b") && !IsValidFileName(L"a.") && IsValidFileName(L"ok.txt"),
          L"batch-rename: invalid Windows names");

    BatchRenameRule rule;
    rule.find = L"photo";
    rule.replace = L"img";
    Check(ApplyBatchRenameRule(L"Photo.jpg", 0, rule) == L"img.jpg",
          L"batch-rename: case-insensitive find/replace on stem");
    rule = {};
    rule.prefix = L"new-";
    rule.insert_number = true;
    rule.start = 1;
    Check(ApplyBatchRenameRule(L"photo.jpg", 0, rule) == L"new-photo (1).jpg",
          L"batch-rename: prefix plus Windows-style number");

    Check(!BatchRenamePatternUsesIndex(L"{name}{ext}") &&
              BatchRenamePatternUsesIndex(L"{name} ({n}){ext}") &&
              BatchRenamePatternUsesIndex(L"{name}_{n:3}{ext}"),
          L"batch-rename: index token detection ignores {name}");

    rule = {};
    rule.pattern = L"{name}_{n:3}{ext}";
    rule.start = 1;
    Check(ApplyBatchRenameRule(L"photo.jpg", 0, rule) == L"photo_001.jpg",
          L"batch-rename: {n:3} zero-pads the index");

    rule = {};
    rule.pattern = L"{name}.png";
    Check(ApplyBatchRenameRule(L"photo.jpg", 0, rule) == L"photo.png",
          L"batch-rename: template can change extension");

    rule = {};
    rule.find = L"photo";
    rule.replace = L"img";
    rule.pattern = L"{name}_{n:3}{ext}";
    Check(ApplyBatchRenameRule(L"Photo.jpg", 0, rule) == L"img_001.jpg",
          L"batch-rename: find/replace then template");

    rule = {};
    rule.find = L"jpg";
    rule.replace = L"png";
    Check(ApplyBatchRenameRule(L"photo.jpg", 0, rule) == L"photo.jpg",
          L"batch-rename: find/replace stays on stem by default");
    rule.replace_in_extension = true;
    Check(ApplyBatchRenameRule(L"photo.jpg", 0, rule) == L"photo.png",
          L"batch-rename: find/replace can include extension");

    rule = {};
    rule.pattern = L"same{ext}";
    const auto collided = PreviewBatchRename(
        { L"C:\\pulse_ren_a.txt", L"C:\\pulse_ren_b.txt" }, rule);
    Check(collided.size() == 2 &&
              collided[0].status == BatchRenameStatus::Collision &&
              collided[1].status == BatchRenameStatus::Collision &&
              collided[0].new_name == L"same.txt",
          L"batch-rename: two files mapping to the same name collide");

    const std::wstring a = std::wstring(temp) + L"pulse_batch_a.txt";
    const std::wstring b = std::wstring(temp) + L"pulse_batch_b.txt";
    HANDLE fa = CreateFileW(a.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE fb = CreateFileW(b.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fa != INVALID_HANDLE_VALUE) CloseHandle(fa);
    if (fb != INVALID_HANDLE_VALUE) CloseHandle(fb);
    rule = {};
    rule.prefix = L"renamed-";
    const auto preview = PreviewBatchRename({ a, b }, rule);
    Check(preview.size() == 2 && preview[0].status == BatchRenameStatus::Ok &&
          preview[1].status == BatchRenameStatus::Ok &&
          preview[0].new_name == L"renamed-pulse_batch_a.txt",
          L"batch-rename: preview two files");
    rule.prefix.clear();
    rule.find = L"pulse_batch";
    rule.replace = L"pulse_batch";
    const auto unchanged = PreviewBatchRename({ a }, rule);
    Check(!unchanged.empty() && unchanged[0].status == BatchRenameStatus::Unchanged,
          L"batch-rename: unchanged when name stays the same");
    DeleteFileW(a.c_str());
    DeleteFileW(b.c_str());

    const auto qa = BuildSidebarModel();
    Check(!qa.quick_access.empty() && qa.quick_access.back().path == MakeRecyclePath(),
          L"sidebar: recycle bin is a quick access pin");
    fs::RecycleBinInfo occupied;
    occupied.valid = true;
    occupied.items = 4;
    occupied.bytes = 2048;
    const auto qa_occ = BuildSidebarModel(&occupied);
    Check(!qa_occ.quick_access.empty() &&
          qa_occ.quick_access.back().path == MakeRecyclePath() &&
          !qa_occ.quick_access.back().detail.empty(),
          L"sidebar: recycle occupancy is on the quick access row");
    Pane recycle_home;
    recycle_home.NewTab(L"C:\\");
    const auto recycle_vm = BuildWindowViewModel(
        recycle_home, qa_occ, true, false, false, nullptr, 0);
    bool recycle_in_access = false;
    if (recycle_vm.sidebar.size() > 1) {
        for (const auto& pin : recycle_vm.sidebar[1].items) {
            if (pin.path == MakeRecyclePath()) {
                recycle_in_access = !pin.detail.empty();
                break;
            }
        }
    }
    Check(recycle_in_access, L"sidebar: recycle bin sits in the quick access group");

    const auto crumbs = ui::SplitBreadcrumb(L"pulse:recycle");
    Check(crumbs.size() == 2 && crumbs[0].path.empty() && crumbs[1].path == L"pulse:recycle",
          L"recycle: breadcrumb is This PC / Recycle Bin");
}

void TestOpsThroughShell() {
    std::wstring dir = kSandbox;
    g_ops.Start([] {});
    Sleep(800); // let the ops worker bring up pulse_shell.exe

    Check(ops::TerminalCommandLine(L"C:\\A\\B") == L"-d \"C:\\A\\B\"",
          L"ops: wt.exe command line");
    Check(ops::TerminalCommandLine(L"C:\\") == L"-d \"C:\\\\\"",
          L"ops: wt.exe command line keeps drive root");
    Check(ops::TerminalCommandLine(L"C:\\quoted\" folder\\") ==
              L"-d \"C:\\quoted\\\" folder\\\\\"",
          L"ops: wt.exe command line escapes quotes and trailing slashes");

    // CreateFolder via the ops layer (menu 新建文件夹 path).
    {
        ops::OpRequest r;
        r.type = ops::OpType::CreateFolder;
        r.sources.push_back(dir + L"\\newfolder");
        RunOp(std::move(r));
        Check(Exists(dir + L"\\newfolder"), L"ops: CreateFolder creates the directory");
        Check(g_ops.CanUndo(), L"ops: CreateFolder is undoable");
    }
    // Undo of create = recycle-delete the created item.
    {
        uint64_t prev = g_ops.Status().completed_ops;
        g_ops.Undo();
        WaitOpDone(prev);
        Check(!Exists(dir + L"\\newfolder"), L"ops: undo create removes the folder");
    }
    // CreateTextFile.
    {
        ops::OpRequest r;
        r.type = ops::OpType::CreateTextFile;
        r.sources.push_back(dir + L"\\新建文本文档.txt");
        RunOp(std::move(r));
        Check(Exists(dir + L"\\新建文本文档.txt"), L"ops: CreateTextFile creates the file");
    }
    // Simulated drop execution: move the file into a subfolder using the same
    // effect computation the drop handler runs.
    {
        CreateDirectoryW((dir + L"\\dst").c_str(), nullptr);
        DWORD effect = ui::ComputeDropEffect(0, dir + L"\\新建文本文档.txt", dir + L"\\dst",
                                             DROPEFFECT_COPY | DROPEFFECT_MOVE);
        ops::OpRequest r;
        r.type = effect == DROPEFFECT_MOVE ? ops::OpType::Move : ops::OpType::Copy;
        r.sources.push_back(dir + L"\\新建文本文档.txt");
        r.dest_dir = dir + L"\\dst";
        RunOp(std::move(r));
        Check(!Exists(dir + L"\\新建文本文档.txt") && Exists(dir + L"\\dst\\新建文本文档.txt"),
              L"ops: drop-execute same-volume move lands");
    }

    {
        const std::wstring target = dir + L"\\ctrl-copy";
        const std::wstring folder = dir + L"\\dst";
        const std::wstring file = folder + L"\\新建文本文档.txt";
        CreateDirectoryW(target.c_str(), nullptr);
        const DWORD effect = ui::ComputeDropEffect(MK_CONTROL, file, target,
                                                   DROPEFFECT_COPY | DROPEFFECT_MOVE);
        ops::OpRequest request;
        request.type = effect == DROPEFFECT_COPY ? ops::OpType::Copy : ops::OpType::Move;
        request.sources = {file, folder};
        request.dest_dir = target;
        RunOp(std::move(request));
        Check(Exists(file) && Exists(folder) &&
              Exists(target + L"\\新建文本文档.txt") &&
              Exists(target + L"\\dst\\新建文本文档.txt"),
              L"Ctrl-drop: copies files and folders while preserving sources");
        Check(ui::ComputeDropEffect(0, file, target, DROPEFFECT_COPY | DROPEFFECT_MOVE)
              == DROPEFFECT_MOVE, L"Ctrl-drop: releasing Ctrl restores same-volume move");
    }

    // Explorer context-menu session end-to-end: REQ_CTX_QUERY through the pipe,
    // pulse_shell builds the real IContextMenu on its session STA thread, the
    // filtered/flattened items come back via RSP_CTX_ITEMS. No invoke here
    // (it would launch an app); the session is closed like a dismissed menu.
    {
        const std::wstring file = dir + L"\\dst\\新建文本文档.txt";
        std::mutex m;
        std::condition_variable cv;
        bool got = false;
        uint32_t got_token = 0;
        std::vector<ops::ShellMenuItem> got_items;
        g_ops.SetShellMenuCallback([&](uint32_t token, std::vector<ops::ShellMenuItem> items,
                                       bool partial, std::vector<std::wstring>) {
            std::lock_guard<std::mutex> lk(m);
            got_token = token;
            got_items = std::move(items);
            if (!partial) got = true;
            cv.notify_one();
        });
        const uint32_t token = g_ops.QueryShellMenu({ file }, nullptr, false, false);
        Check(token != 0, L"ctx: query issues a session token");
        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait_for(lk, std::chrono::seconds(20), [&] { return got; });
        }
        Check(got && got_token == token, L"ctx: RSP_CTX_ITEMS arrives for the session");
        bool clean = true;
        for (const auto& it : got_items) {
            const bool invalid = it.text.empty() || ipc::IsBuiltinContextVerb(it.verb, false);
            if (invalid) {
                clean = false;
                LogLine(L"[info] ctx: unfiltered verb='%s' text='%s'\n",
                        it.verb.c_str(), it.text.c_str());
            }
        }
        Check(clean, L"ctx: items are filtered (no built-in verbs, no empty rows)");
        LogLine(L"[info] ctx: %zu Explorer items for .txt\n", got_items.size());
        g_ops.CloseShellMenu(token);
        g_ops.SetShellMenuCallback(nullptr);
    }

    g_ops.Stop();
}

void TestViewLayouts() {
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        ui::ViewLayout gutter(ui::ViewMode::Details, D2D1::RectF(0, 0, 600 * scale, 300 * scale),
                              20, 0, 0, scale);
        Check(gutter.HitTest(4 * scale, 10 * scale) == -1 &&
              gutter.HitTest(582 * scale, 10 * scale) == -1 &&
              gutter.HitTest(24 * scale, 10 * scale) == 0 &&
              gutter.ItemRect(0).left == 8 * scale,
              L"layout: details side gutters are empty hit targets at each DPI");
    }
    const D2D1_RECT_F viewport = D2D1::RectF(10.0f, 20.0f, 1010.0f, 720.0f);
    for (int i = 0; i < 8; ++i) {
        const ui::ViewMode mode = ui::ViewModeFromIndex(i);
        ui::ViewLayout layout(mode, viewport, 100000, 0.0f, 0.0f, 1.0f);
        const auto range = layout.VisibleRange();
        Check(range.first == 0 && range.second >= range.first && range.second < 1000,
              L"view: 100k layout materializes visible range only");
        const D2D1_RECT_F first = layout.ItemRect(0);
        Check(layout.HitTest((first.left + first.right) * 0.5f,
                             (first.top + first.bottom) * 0.5f) == 0,
              L"view: item rect and hit-test agree");
        Check(ui::ParseViewMode(ui::ViewModeName(mode)) == mode,
              L"view: stable persistence name round-trips");
    }
    ui::ViewLayout narrow(ui::ViewMode::ExtraLargeIcons,
        D2D1::RectF(0, 0, 180, 500), 20, 0, 0, 1.0f);
    Check(narrow.Metrics().columns == 1, L"view: narrow icon pane degrades to one column");
    const D2D1_RECT_F iconCell = narrow.ItemRect(0);
    const D2D1_RECT_F iconRect = narrow.IconRect(0);
    const D2D1_RECT_F iconName = narrow.NameRect(0);
    const float cellCenter = (iconCell.left + iconCell.right) * 0.5f;
    Check(std::abs((iconRect.left + iconRect.right) * 0.5f - cellCenter) < 0.01f &&
          std::abs((iconName.left + iconName.right) * 0.5f - cellCenter) < 0.01f,
          L"view: icon and filename share the cell center axis");
    ui::ViewLayout hidpiIcons(ui::ViewMode::ExtraLargeIcons,
        D2D1::RectF(0, 0, 900, 700), 20, 0, 0, 1.5f);
    Check(std::abs(hidpiIcons.Metrics().icon_size - 240.0f) < 0.01f &&
          std::abs(hidpiIcons.Metrics().cell_width - 280.0f) < 0.01f,
          L"view: extra-large Explorer tier remains capped in physical pixels");
    Check(hidpiIcons.NameRect(0).top >= hidpiIcons.IconRect(0).bottom,
          L"view: extra-large filename never overlaps icon bounds");
    ui::ViewLayout hidpiLarge(ui::ViewMode::LargeIcons,
        D2D1::RectF(0, 0, 900, 700), 20, 0, 0, 1.5f);
    Check(std::abs(hidpiLarge.Metrics().icon_size - 144.0f) < 0.01f &&
          hidpiLarge.Metrics().cell_width >= 180.0f &&
          hidpiLarge.NameRect(0).top >= hidpiLarge.IconRect(0).bottom,
          L"view: large icons scale with DPI and keep filename below image");
    ui::ViewLayout hidpiMedium(ui::ViewMode::MediumIcons,
        D2D1::RectF(0, 0, 900, 700), 20, 0, 0, 1.5f);
    Check(std::abs(hidpiMedium.Metrics().icon_size - 72.0f) < 0.01f &&
          hidpiMedium.NameRect(0).top >= hidpiMedium.IconRect(0).bottom,
          L"view: medium icons scale with DPI and keep filename below image");
    ui::ViewLayout list(ui::ViewMode::List, viewport, 1000, 0, 0, 1.0f);
    Check(list.MaxScrollX() > 0 && list.MaxScrollY() == 0,
          L"view: list is column-major with horizontal scrolling");
    auto menu = BuildViewMenu(ui::ViewMode::Details);
    Check(menu.size() == 9 && menu[5].radio && !menu[0].radio &&
          menu[8].command == CmdDetailsPanel,
          L"view: menu has eight view choices plus details panel");
    Check(menu[0].glyph_scale > menu[1].glyph_scale &&
          menu[1].glyph_scale > menu[2].glyph_scale,
          L"view: icon menu communicates extra-large, large, and medium scale");

    ui::MainRenderer columns;
    columns.SetScale(1.0f);
    const D2D1_RECT_F pane = D2D1::RectF(0.0f, 0.0f, 1000.0f, 400.0f);
    const auto search = columns.DetailsColumns(pane, {}, true);
    Check(search.count == 5 && search.widths[1] >= 110.0f,
          L"search: details view has a path column");
    const float name_path = search.DividerX(0);
    const auto dragged = columns.ResizeSearchColumnDivider(pane, {}, 0, name_path - 80.0f);
    const auto widened = columns.DetailsColumns(pane, {}, true, dragged);
    Check(dragged[0] > 0.0f && dragged[3] < 1.0f && widened.widths[1] > search.widths[1] + 40.0f,
          L"search: dragging the path divider widens the path column");
}

} // namespace

void TestLinkResolve() {
    Check(fs::StripLnkSuffix(L"计算图形.dwg.lnk") == L"计算图形.dwg",
          L"link: strip suffix");
    Check(fs::StripLnkSuffix(L"note.txt") == L"note.txt",
          L"link: strip keeps non-lnk name");
    Check(fs::StripLnkSuffix(L"A.LNK") == L"A", L"link: strip is case-insensitive");

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp), temp);
    const std::wstring dir = std::wstring(temp) + L"PulseLinkTest-" +
                             std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring target_file = dir + L"\\target.txt";
    const std::wstring target_dir = dir + L"\\folder";
    const std::wstring lnk_file = dir + L"\\target.txt.lnk";
    const std::wstring lnk_dir = dir + L"\\folder.lnk";
    const std::wstring lnk_dead = dir + L"\\dead.lnk";
    const char body[] = "hello";
    HANDLE hf = CreateFileW(target_file.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    WriteFile(hf, body, sizeof(body) - 1, &written, nullptr);
    CloseHandle(hf);
    CreateDirectoryW(target_dir.c_str(), nullptr);

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                COINIT_DISABLE_OLE1DDE);
    auto make_lnk = [](const std::wstring& link, const std::wstring& target) {
        Microsoft::WRL::ComPtr<IShellLinkW> sl;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&sl)))) return false;
        if (FAILED(sl->SetPath(target.c_str()))) return false;
        Microsoft::WRL::ComPtr<IPersistFile> pf;
        if (FAILED(sl.As(&pf))) return false;
        return SUCCEEDED(pf->Save(link.c_str(), TRUE));
    };
    Check(make_lnk(lnk_file, target_file) && make_lnk(lnk_dir, target_dir) &&
          make_lnk(lnk_dead, dir + L"\\gone.txt"), L"link: create fixtures");

    fs::DirEntry e1;
    Check(ResolveLink(lnk_file, e1) &&
          e1.link_target == fs::NormalizePath(target_file) &&
          !e1.link_target_is_dir && e1.link_target_size == sizeof(body) - 1,
          L"link: file target resolves with size");
    fs::DirEntry e2;
    Check(ResolveLink(lnk_dir, e2) && e2.link_target_is_dir,
          L"link: folder target resolves");
    fs::DirEntry e3;
    Check(!ResolveLink(lnk_dead, e3) && e3.link_target.empty(),
          L"link: missing target does not penetrate");
    if (com == S_OK) CoUninitialize();

    DeleteFileW(lnk_file.c_str());
    DeleteFileW(lnk_dir.c_str());
    DeleteFileW(lnk_dead.c_str());
    DeleteFileW(target_file.c_str());
    RemoveDirectoryW(target_dir.c_str());
    RemoveDirectoryW(dir.c_str());
}

void TestDetailsMeta() {
    Check(pulse::format::GroupedInt(0) == L"0", L"meta: grouped int zero");
    Check(pulse::format::GroupedInt(12345) == L"12,345", L"meta: grouped int small");
    Check(pulse::format::GroupedInt(2895851315ull) == L"2,895,851,315",
          L"meta: grouped int large");
    Check(FormatAccessMask(FILE_ALL_ACCESS, false) == L"完全控制",
          L"meta: mask full control");
    Check(FormatAccessMask(FILE_GENERIC_READ | FILE_GENERIC_WRITE, false) == L"修改",
          L"meta: mask modify");
    Check(FormatAccessMask(FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, true) ==
          L"读取和执行、列出文件夹内容、读取", L"meta: mask read-execute dir");
    Check(FormatAccessMask(FILE_GENERIC_WRITE, false) == L"写入", L"meta: mask write only");
    Check(FormatAccessMask(DELETE, false) == L"删除", L"meta: mask delete only");
    Check(FormatAccessMask(0, false) == L"特殊权限", L"meta: mask empty");
}

void TestMoveTabRun() {
    std::vector<int> order{ 0, 1, 2, 3, 4, 5 };
    Check(MoveTabRun(order, 1, 2, -1) == 0 &&
          order == (std::vector<int>{ 1, 2, 0, 3, 4, 5 }),
          L"tabrun: rotate left moves block");
    Check(MoveTabRun(order, 0, 2, 1) == 1 &&
          order == (std::vector<int>{ 0, 1, 2, 3, 4, 5 }),
          L"tabrun: rotate right restores");
    Check(MoveTabRun(order, 2, 2, 1) == 3 &&
          order == (std::vector<int>{ 0, 1, 4, 2, 3, 5 }),
          L"tabrun: rotate right displaces single");
    order = { 0, 1, 2, 3 };
    Check(MoveTabRun(order, 0, 2, -1) == 0 &&
          order == (std::vector<int>{ 0, 1, 2, 3 }),
          L"tabrun: left edge is a no-op");
    Check(MoveTabRun(order, 2, 2, 1) == 2 &&
          order == (std::vector<int>{ 0, 1, 2, 3 }),
          L"tabrun: right edge is a no-op");
    Check(MoveTabRun(order, 0, 1, 1) == 1 &&
          order == (std::vector<int>{ 1, 0, 2, 3 }),
          L"tabrun: len-1 degenerates to a swap");

    const std::vector<int> right_groups{ 7, 7, 0 };
    order = { 0, 1, 2 };
    Check(MoveTabGroupAcrossFreeTab(order, right_groups, 1, 1) &&
          order == (std::vector<int>{ 2, 0, 1 }),
          L"tabgroup: member crossing right free tab moves the whole group");
    order = { 0, 1, 2 };
    Check(MoveTabGroupAcrossFreeTab(order, right_groups, 0, 1) &&
          order == (std::vector<int>{ 2, 0, 1 }),
          L"tabgroup: any member preserves its group when crossing right");

    const std::vector<int> left_groups{ 0, 7, 7 };
    order = { 0, 1, 2 };
    Check(MoveTabGroupAcrossFreeTab(order, left_groups, 1, -1) &&
          order == (std::vector<int>{ 1, 2, 0 }),
          L"tabgroup: member crossing left free tab moves the whole group");
    order = { 0, 1, 2 };
    Check(!MoveTabGroupAcrossFreeTab(order, left_groups, 1, 1) &&
          order == (std::vector<int>{ 0, 1, 2 }),
          L"tabgroup: move rejects a non-free outside neighbor");
}

void TestNormalizeGroupRuns() {
    auto make = [](int group) {
        auto t = std::make_unique<LayoutTab>();
        t->tab_group = group;
        return t;
    };
    {   // Split run: 1,0,1,2 -> 1,1,0,2 and active tab follows its pointer.
        WindowTabs tabs;
        tabs.items.push_back(make(1)); tabs.items.push_back(make(0));
        tabs.items.push_back(make(1)); tabs.items.push_back(make(2));
        tabs.active = 2;
        const LayoutTab* active = tabs.items[2].get();
        NormalizeGroupRuns(tabs);
        Check(tabs.items.size() == 4 &&
              tabs.items[0]->tab_group == 1 && tabs.items[1]->tab_group == 1 &&
              tabs.items[2]->tab_group == 0 && tabs.items[3]->tab_group == 2,
              L"tabgroup: split run collapses into one run");
        Check(tabs.Active() == active, L"tabgroup: active tab survives normalize");
    }
    {   // Interleaved groups: 1,2,1,2 -> 1,1,2,2 (first-appearance order kept).
        WindowTabs tabs;
        for (int g : { 1, 2, 1, 2 }) tabs.items.push_back(make(g));
        NormalizeGroupRuns(tabs);
        Check(tabs.items[0]->tab_group == 1 && tabs.items[1]->tab_group == 1 &&
              tabs.items[2]->tab_group == 2 && tabs.items[3]->tab_group == 2,
              L"tabgroup: interleaved groups normalize pairwise");
    }
    {   // Already contiguous: untouched.
        WindowTabs tabs;
        for (int g : { 0, 1, 1, 0 }) tabs.items.push_back(make(g));
        NormalizeGroupRuns(tabs);
        Check(tabs.items[0]->tab_group == 0 && tabs.items[2]->tab_group == 1 &&
              tabs.items[3]->tab_group == 0, L"tabgroup: contiguous runs untouched");
    }
}

void TestChipBlockDragGeometry() {
    Check(CollapsedChipBlockW(24.0f, 6.0f) == 30.0f,
          L"chipdrag: block width is chip plus gap");
    Check(CollapsedChipBlockW(0.0f, 4.0f) == 4.0f,
          L"chipdrag: zero-width chip still reserves the gap");
    // Moving left: the block's left edge must pass the neighbor center.
    Check(ChipBlockCrossed(100.0f, 30.0f, 120.0f, -1),
          L"chipdrag: left edge past center crosses");
    Check(!ChipBlockCrossed(125.0f, 30.0f, 120.0f, -1),
          L"chipdrag: left edge short of center holds");
    // Moving right: the block's right edge must pass the neighbor center.
    Check(ChipBlockCrossed(100.0f, 30.0f, 120.0f, 1),
          L"chipdrag: right edge past center crosses");
    Check(!ChipBlockCrossed(100.0f, 30.0f, 140.0f, 1),
          L"chipdrag: right edge short of center holds");
    Check(!ChipBlockCrossed(100.0f, 30.0f, 120.0f, 0),
          L"chipdrag: zero direction never crosses");
    // Displaced unit track start = where it sits minus where it must land.
    Check(DisplacedRestDelta(200.0f, 0.0f, 260.0f) == -60.0f,
          L"chipdrag: displaced unit starts one block back");
    Check(DisplacedRestDelta(200.0f, -30.0f, 170.0f) == 0.0f,
          L"chipdrag: in-flight slide that already arrived needs no track");
    Check(DisplacedRestDelta(170.0f, 30.0f, 260.0f) == -60.0f,
          L"chipdrag: in-flight slide composes with the new delta");
}

void TestFindGroupRun() {
    const std::vector<int> gids{ 0, 7, 7, 7, 0 }; // group 7 = tabs 1..3
    {   // Identity order: the run spans display positions 1..3.
        const std::vector<int> order{ 0, 1, 2, 3, 4 };
        const GroupRun r = FindGroupRun(order, gids, 2, 7);
        Check(r.pos == 1 && r.len == 3, L"grouprun: run bounds from middle member");
        Check(FindGroupRun(order, gids, 1, 7).pos == 1 &&
              FindGroupRun(order, gids, 3, 7).pos == 1,
              L"grouprun: same run from any member");
    }
    {   // Permuted order (mid-drag): lookup follows order indirection.
        const std::vector<int> order{ 3, 0, 1, 2, 4 };
        const GroupRun r = FindGroupRun(order, gids, 2, 7);
        Check(r.pos == 2 && r.len == 2, L"grouprun: permuted run after split");
        const GroupRun head = FindGroupRun(order, gids, 0, 7);
        Check(head.pos == 0 && head.len == 1, L"grouprun: split-off member is its own run");
    }
    {   // Rejections: ungrouped tab, wrong gid, out of range.
        const std::vector<int> order{ 0, 1, 2, 3, 4 };
        Check(FindGroupRun(order, gids, 0, 0).len == 0,
              L"grouprun: gid 0 is never a run");
        Check(FindGroupRun(order, gids, 0, 7).len == 0,
              L"grouprun: tab at pos not in gid");
        Check(FindGroupRun(order, gids, -1, 7).len == 0 &&
              FindGroupRun(order, gids, 5, 7).len == 0,
              L"grouprun: out-of-range pos rejected");
        Check(FindGroupRun(order, gids, 2, 9).len == 0,
              L"grouprun: unknown gid rejected");
    }
    {   // Whole-strip group: run reaches both edges.
        const std::vector<int> g2{ 4, 4, 4 };
        const std::vector<int> order{ 0, 1, 2 };
        const GroupRun r = FindGroupRun(order, g2, 0, 4);
        Check(r.pos == 0 && r.len == 3, L"grouprun: edge-to-edge run");
    }
    {   // Hop keeps contiguity: MoveTabRun on the found run never inserts the
        // dragged tab between members.
        const std::vector<int> g3{ 0, 5, 5, 0 };
        std::vector<int> order{ 0, 1, 2, 3 }; // tab 0 drags right into group 5
        const GroupRun r = FindGroupRun(order, g3, 1, 5);
        MoveTabRun(order, r.pos, r.len, -1); // run slides left past tab 0
        Check(order == (std::vector<int>{ 1, 2, 0, 3 }),
              L"grouprun: hop lands the tab past the whole run");
    }
}

bool SameLayoutTabs(const std::vector<LayoutTabSnapshot>& a,
                    const std::vector<LayoutTabSnapshot>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].pinned != b[i].pinned || a[i].group != b[i].group ||
            a[i].title != b[i].title || a[i].layout != b[i].layout ||
            a[i].focused != b[i].focused || a[i].target != b[i].target ||
            a[i].split_ratios != b[i].split_ratios ||
            a[i].panes.size() != b[i].panes.size()) return false;
        for (size_t p = 0; p < a[i].panes.size(); ++p) {
            if (a[i].panes[p].path != b[i].panes[p].path ||
                a[i].panes[p].view != b[i].panes[p].view ||
                a[i].panes[p].columns != b[i].panes[p].columns ||
                a[i].panes[p].search_columns != b[i].panes[p].search_columns)
                return false;
        }
    }
    return true;
}

void TestSessionLayoutTabs() {
    {   // Round-trip: two layout tabs, groups, special characters, split ratios.
        std::vector<LayoutTabSnapshot> tabs(2);
        tabs[0].pinned = true;
        tabs[0].group = 1;
        tabs[0].title = L"工作";
        tabs[0].layout = 1;
        tabs[0].focused = 1;
        tabs[0].target = 0;
        tabs[0].split_ratios = { 0.42f };
        tabs[0].panes.push_back({ L"C:\\", ui::ViewMode::Details, { 0.42f, 0.61f, 0.82f },
                                  { 0.28f, 0.52f, 0.70f, 0.86f } });
        tabs[0].panes.push_back({ L"D:\\代码\\路径 \"quoted\"\\dir", ui::ViewMode::LargeIcons });
        tabs[1].group = 2;
        tabs[1].layout = 0;
        tabs[1].title = L"引\"号\\组\n名";
        tabs[1].panes.push_back({ L"\\\\?\\UNC\\server\\share\\dir", ui::ViewMode::List });

        const std::wstring json = LayoutTabsToJson(tabs);
        std::vector<LayoutTabSnapshot> back;
        Check(ParseLayoutTabs(json, back), L"layouttabs: round-trip parses");
        Check(SameLayoutTabs(tabs, back), L"layouttabs: round-trip preserves all fields");
    }
    {   // Missing searchCols still loads (older sessions).
        std::vector<LayoutTabSnapshot> out;
        Check(ParseLayoutTabs(
            L"[{\"pinned\":false,\"group\":0,\"layout\":0,\"focused\":0,\"panes\":["
            L"{\"path\":\"C:\\\\\",\"view\":\"details\",\"cols\":\"4200,6100,8200\"}]}]", out) &&
            !out.empty() && !out[0].panes.empty() &&
            out[0].panes[0].columns[0] > 0.0f &&
            out[0].panes[0].search_columns[0] == 0.0f,
              L"layouttabs: older JSON without searchCols stays default");
    }
    {   // Empty array and empty layout tab.
        std::vector<LayoutTabSnapshot> out;
        Check(ParseLayoutTabs(L"[]", out) && out.empty(),
              L"layouttabs: empty array");
        std::vector<LayoutTabSnapshot> tabs(1);
        out.clear();
        Check(ParseLayoutTabs(LayoutTabsToJson(tabs), out) &&
              SameLayoutTabs(tabs, out),
              L"layouttabs: empty tab round-trips");
    }
    {   // Unknown view falls back to details; out-of-range focused survives parsing.
        std::vector<LayoutTabSnapshot> out;
        Check(ParseLayoutTabs(
                  L"[{\"layout\":1,\"focused\":99,\"panes\":["
                  L"{\"path\":\"C:\\\\x\",\"view\":\"bogus\"}]}]",
                  out) &&
              out.size() == 1 && out[0].focused == 99 && out[0].layout == 1 &&
              out[0].panes.size() == 1 && out[0].panes[0].path == L"C:\\x" &&
              out[0].panes[0].view == ui::ViewMode::Details,
              L"layouttabs: unknown view + focused parsed as-is");
    }
    {   // Corrupt input: rejects or skips without crashing, never throws.
        std::vector<LayoutTabSnapshot> out;
        Check(!ParseLayoutTabs(L"", out), L"layouttabs: empty string rejected");
        Check(!ParseLayoutTabs(L"not json", out), L"layouttabs: garbage rejected");
        Check(!ParseLayoutTabs(L"[{\"layout\":1,\"panes\":[{\"path\":\"C:\\\\\"}]", out),
              L"layouttabs: truncated array rejected");
        Check(ParseLayoutTabs(L"[{\"panes\":[{}]}]", out) && out.size() == 1 &&
              out[0].panes.size() == 1 && out[0].panes[0].path.empty(),
              L"layouttabs: empty object tolerated");
    }
}

void TestLayoutOwnedTabs() {
    WindowTabs tabs;
    tabs.NewTab(L"C:\\work");
    LayoutTab& first = *tabs.Active();
    auto extra = std::make_unique<Pane>();
    extra->NewTab(L"C:\\assets");
    first.panes.push_back(std::move(extra));
    std::vector<Pane*> used{ first.panes[0].get(), first.panes[1].get() };
    first.root = MakePresetTree(LayoutPreset::TwoVertical, used);
    first.layout = LayoutPreset::TwoVertical;
    first.focused_index = 0;
    Check(LayoutTabTitle(first).find(L"3") == std::wstring::npos &&
          LayoutTabTitle(first).find(L"·") != std::wstring::npos,
          L"layouttabs: multi-pane title uses a light suffix");

    tabs.NewTab(L"D:\\raw");
    Check(tabs.items.size() == 2, L"layouttabs: two window tabs");
    Check(tabs.Active()->layout == LayoutPreset::Single &&
          tabs.Active()->panes.size() == 1,
          L"layouttabs: new tab is a single-pane layout");
    Check(tabs.Active()->panes[0]->view.current_path.find(L"raw") != std::wstring::npos,
          L"layouttabs: new tab opens the requested folder");

    tabs.SwitchTab(0);
    Check(tabs.Active()->layout == LayoutPreset::TwoVertical &&
          tabs.Active()->panes.size() == 2,
          L"layouttabs: switch restores pane count and preset");
    Check(tabs.Active()->panes[0]->view.current_path.find(L"work") != std::wstring::npos &&
          tabs.Active()->panes[1]->view.current_path.find(L"assets") != std::wstring::npos,
          L"layouttabs: switch restores each column path");

    const LayoutTabSnapshot first_snap = CaptureLayoutTab(*tabs.items[0]);
    const LayoutTabSnapshot second_snap = CaptureLayoutTab(*tabs.items[1]);
    WindowTabs restored;
    RestoreWindowTabs(restored, { first_snap, second_snap }, {}, 1,
        [](Tab& tab, const std::wstring& path) { tab.current_path = path; });
    Check(restored.items.size() == 2 && restored.active == 1,
          L"layouttabs: session restore keeps two tabs and active index");
    Check(restored.items[0]->layout == LayoutPreset::TwoVertical &&
          restored.items[0]->panes.size() == 2 &&
          restored.items[0]->panes[0]->view.current_path.find(L"work") != std::wstring::npos &&
          restored.items[0]->panes[1]->view.current_path.find(L"assets") != std::wstring::npos,
          L"layouttabs: session restore keeps the first tab's split folders");
    Check(restored.items[1]->layout == LayoutPreset::Single &&
          restored.items[1]->panes.size() == 1 &&
          restored.items[1]->panes[0]->view.current_path.find(L"raw") != std::wstring::npos,
          L"layouttabs: session restore keeps the second tab's single folder");
}

void TestUtf8PersistFile() {
    wchar_t temp_dir[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp_dir), temp_dir);
    const std::wstring path = std::wstring(temp_dir) + L"pulse-utf8-persist-test.json";
    const std::wstring json =
        L"{\"name\":\"紧急修补\",\"unc\":\"\\\\192.168.0.254\\工程项目盘\"}\n";
    Check(WriteUtf8FileAtomic(path, json), L"utf8file: write Chinese JSON");
    std::wstring loaded;
    Check(ReadUtf8File(path, loaded) && loaded == json, L"utf8file: Chinese round-trip");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool utf8 = false;
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 4096) {
            std::vector<char> bytes(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            if (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) {
                const std::string raw(bytes.data(), bytes.data() + read);
                utf8 = raw.find("\xE7\xB4\xA7\xE6\x80\xA5\xE4\xBF\xAE\xE8\xA1\xA5") !=
                       std::string::npos;
            }
        }
        CloseHandle(file);
    }
    Check(utf8, L"utf8file: on-disk bytes are UTF-8");
    DeleteFileW(path.c_str());
    DeleteFileW((path + L".tmp").c_str());
}

void TestColorPickerModel() {
    // Known value from the QFluent reference screenshot (#2FDFF1).
    {
        const ui::HsvColor hsv = ui::HsvFromRgb(0x2FDFF1);
        Check(hsv.h == 186 && hsv.s == 205 && hsv.v == 241,
              L"colorpicker: HsvFromRgb(#2FDFF1) -> 186/205/241");
    }
    Check(ui::RgbFromHsv(0, 0, 128) == 0x808080, L"colorpicker: gray roundtrip exact");
    Check(ui::RgbFromHsv(360, 255, 255) == 0xFF0000, L"colorpicker: hue 360 wraps to red");
    Check(ui::RgbFromHsv(0, 255, 255) == 0xFF0000, L"colorpicker: pure red exact");
    // Roundtrip within +-2 per channel (hue quantization loses a little).
    bool roundtrip = true;
    for (const uint32_t rgb : { 0x000000u, 0xFFFFFFu, 0x2FDFF1u, 0xEF4444u,
                                0x123456u, 0xA855F7u, 0x94A3B8u, 0x0078D4u }) {
        const ui::HsvColor hsv = ui::HsvFromRgb(rgb);
        const uint32_t back = ui::RgbFromHsv(hsv.h, hsv.s, hsv.v);
        for (int shift = 0; shift < 24; shift += 8) {
            const int a = static_cast<int>((rgb >> shift) & 0xff);
            const int b = static_cast<int>((back >> shift) & 0xff);
            if (std::abs(a - b) > 2) roundtrip = false;
        }
    }
    Check(roundtrip, L"colorpicker: RGB<->HSV roundtrip within tolerance");

    uint32_t argb = 0;
    Check(ui::ParseHexColor(L"#ff2fdff1", argb) && argb == 0xFF2FDFF1u,
          L"colorpicker: parse #aarrggbb");
    Check(ui::ParseHexColor(L"2FDFF1", argb) && argb == 0xFF2FDFF1u,
          L"colorpicker: parse rrggbb implies opaque alpha");
    Check(!ui::ParseHexColor(L"#12345", argb), L"colorpicker: reject 5 digits");
    Check(!ui::ParseHexColor(L"#gggggg", argb), L"colorpicker: reject non-hex");
    Check(!ui::ParseHexColor(L"", argb), L"colorpicker: reject empty");
    Check(ui::FormatHexColor(0xFF2FDFF1u, true) == L"#ff2fdff1",
          L"colorpicker: format #aarrggbb lowercase");
    Check(ui::FormatHexColor(0xFF2FDFF1u, false) == L"#2fdff1",
          L"colorpicker: format #rrggbb lowercase");
}

int RunSelfTest1B2() {
    // These model assertions use the Chinese resource strings explicitly.
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONERR$", "w", stderr);
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    FILE* g_log_local = nullptr;
    g_log = _wfopen_s(&g_log_local, kLogPath.c_str(), L"w, ccs=UTF-8") == 0
        ? g_log_local : nullptr;

    TestBreadcrumb();
    TestThisPcEnumeration();
    TestLoadingPresentation();
    TestNavigationReturnSelection();
    TestMouseHistoryNavigation();
    TestNotificationToast();
    TestMenuModel();
    TestShellMenuMerge();
    TestContextMenuPrefs();
    TestAppPrefsAndSettingsPath();
    TestDragDropPure();
    TestAddressSearch();
    TestContinuousSearch();
    TestCtrlDragSelection();
    TestDirWatch();
    TestNavigateAlwaysEnumerates();
    TestSnapshotPatch();
    TestSnapshotStorePutKeepsWorkerGeneration();
    TestDataObject();
    TestClipboardText();
    TestUniqueName();
    TestMultiSelect();
    TestSplitLayout();
    TestViewLayouts();
    TestHiddenFiles();
    TestQuickAccess();
    TestPlacesAndIndex();
    TestRecycleAndBatchRename();
    TestLinkResolve();
    TestDetailsMeta();
    TestMoveTabRun();
    TestNormalizeGroupRuns();
    TestChipBlockDragGeometry();
    TestFindGroupRun();
    TestSessionLayoutTabs();
    TestLayoutOwnedTabs();
    TestUtf8PersistFile();
    TestColorPickerModel();
    TestBloomAccentGeometry();
    TestBloomSpring();
    TestOpsThroughShell();

    // Cleanup: real-delete the whole sandbox via the ops layer is overkill;
    // it only contains files we made, so delete in place.
    {
        ops::OpsManager cleanup;
        cleanup.Start([] {});
        Sleep(600);
        ops::OpRequest r;
        r.type = ops::OpType::RealDelete;
        r.sources.push_back(std::wstring(kSandbox));
        uint64_t prev = cleanup.Status().completed_ops;
        cleanup.Submit(std::move(r));
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (cleanup.Status().completed_ops == prev &&
               std::chrono::steady_clock::now() < deadline) Sleep(10);
        cleanup.Stop();
    }
    Check(!Exists(kSandbox), L"cleanup: sandbox removed");

    LogLine(L"\n== 1B-2 self test: %d passed, %d failed ==\n", g_pass, g_fail);
    if (g_log) fclose(g_log);
    fflush(stdout);
    return g_fail == 0 ? 0 : 1;
}

} // namespace pulse::app

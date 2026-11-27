// Explicitly opted-in, single-window live acceptance; never used by normal launches.
#include "explorer_source_scope.h"
#include "explorer_submission_port.h"
#include <exdisp.h>
#include <shlguid.h>
#include <wrl/client.h>
#include <set>
namespace {
using Microsoft::WRL::ComPtr;
bool NativeLiveView(IWebBrowser2* browser, std::wstring& path, std::vector<std::wstring>& names) {
    ComPtr<IServiceProvider> provider;
    ComPtr<IShellBrowser> shell;
    ComPtr<IShellView> view;
    ComPtr<IFolderView2> folder_view;
    ComPtr<IPersistFolder2> folder;
    if (FAILED(browser->QueryInterface(IID_PPV_ARGS(&provider))) ||
        FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&shell))) ||
        FAILED(shell->QueryActiveShellView(&view)) || FAILED(view.As(&folder_view)) ||
        FAILED(folder_view->GetFolder(IID_PPV_ARGS(&folder)))) return false;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(folder->GetCurFolder(&pidl)) || !pidl) return false;
    PWSTR text = nullptr;
    const HRESULT named = SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &text);
    CoTaskMemFree(pidl);
    if (FAILED(named) || !text) return false;
    path = text; CoTaskMemFree(text);
    int count = 0;
    if (FAILED(folder_view->ItemCount(SVGIO_SELECTION, &count))) return false;
    if (!count) return true;
    ComPtr<IShellItemArray> items;
    if (folder_view->GetSelection(FALSE, &items) != S_OK || !items) return false;
    DWORD size = 0;
    if (FAILED(items->GetCount(&size))) return false;
    for (DWORD i = 0; i < size; ++i) {
        ComPtr<IShellItem> item;
        if (FAILED(items->GetItemAt(i, &item)) ||
            FAILED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &text)) || !text) return false;
        names.emplace_back(text); CoTaskMemFree(text);
    }
    return true;
}
void NativeLivePump(DWORD duration) {
    const auto until = GetTickCount64() + duration;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return;
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        Sleep(10);
    } while (GetTickCount64() < until);
}
template<class Check>
void RunNativeExplorerLiveCheck(pulse::AppState& state, const std::filesystem::path& output,
                               std::ostream& log, Check&& check) {
    using namespace pulse;
    using namespace pulse::app;
    check(state.isolatedTest && !IsWindowVisible(state.hwnd), "live acceptance keeps the isolated Pulse window hidden");
    if (!state.isolatedTest || IsWindowVisible(state.hwnd)) return;
    const auto target = output / (L"native-only-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directory(target);
    const auto sample = target / L"sample.txt";
    std::ofstream(sample) << "Pulse native Explorer acceptance fixture\n";
    std::set<HWND> before;
    const bool captured = EnumWindows([](HWND window, LPARAM context) -> BOOL {
        reinterpret_cast<std::set<HWND>*>(context)->insert(window); return TRUE;
    }, reinterpret_cast<LPARAM>(&before)) != FALSE;
    check(captured, "existing desktop window identities captured before the single launch");
    if (!captured) return;
    ComPtr<IShellWindows> windows;
    const HRESULT connected = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows));
    check(SUCCEEDED(connected), "real IShellWindows observer connected before launching");
    if (FAILED(connected)) return;
    const DWORD queued = native_explorer::OpenAsync({sample.wstring(), native_explorer::Intent::SelectFile}, state.hwnd);
    log << "queue_error=" << queued << '\n';
    check(queued == ERROR_SUCCESS, "actual native OpenAsync accepts the one file-reveal request");
    if (queued != ERROR_SUCCESS) return;
    ComPtr<IWebBrowser2> owned;
    HWND window = nullptr;
    std::vector<std::wstring> selection;
    const auto deadline = GetTickCount64() + 8000;
    while (!owned && GetTickCount64() < deadline) {
        NativeLivePump(50);
        long count = 0;
        if (FAILED(windows->get_Count(&count))) continue;
        for (long i = 0; i < count; ++i) {
            VARIANT index{}; index.vt = VT_I4; index.lVal = i;
            ComPtr<IDispatch> dispatch; ComPtr<IWebBrowser2> browser;
            SHANDLE_PTR handle = 0;
            if (FAILED(windows->Item(index, &dispatch)) || !dispatch || FAILED(dispatch.As(&browser)) ||
                FAILED(browser->get_HWND(&handle)) || !handle) continue;
            const auto candidate = reinterpret_cast<HWND>(handle);
            if (before.contains(candidate)) continue;
            std::wstring folder;
            std::vector<std::wstring> names;
            if (!NativeLiveView(browser.Get(), folder, names) || _wcsicmp(folder.c_str(), target.c_str()) != 0) continue;
            owned = browser; window = candidate; selection = std::move(names); break;
        }
    }
    check(owned && window, "real native Explorer creates a new window for the exact fixture folder");
    if (!owned || !window) return;
    auto scope = std::make_shared<ExplorerSourceScope>();
    scope->token.hwnd = reinterpret_cast<uint64_t>(window);
    DWORD source_pid = 0;
    scope->token.tid = GetWindowThreadProcessId(window, &source_pid);
    scope->token.pid = source_pid;
    scope->token.owner = GetCurrentProcessId();
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, scope->token.pid);
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool birth = process && GetProcessTimes(process, &created, &exited, &kernel, &user);
    if (process) CloseHandle(process);
    scope->token.created = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    scope->folder = CanonicalExplorerScopePath(target.wstring());
    // Required by the existing scoped production watcher; keep it unsignalled
    // so a failing retention check cannot authorize a test-driven handoff close.
    scope->evidence->close_authorized_event = CreateEventW(nullptr, TRUE, FALSE,
        ExplorerScopeCloseEventName(scope->token).c_str());
    check(scope->evidence->close_authorized_event != nullptr,
          "scoped production watcher receives its required identity-bound close gate");
    const bool tagged = birth && !GetPropW(window, kExplorerScopeOwner) &&
        SetPropW(window, kExplorerScopeOwner, reinterpret_cast<HANDLE>(scope->token.owner));
    check(tagged && ExplorerScopeWindow(*scope, window), "live watcher is restricted to this new system Explorer HWND and process lifetime");
    if (tagged && ExplorerScopeWindow(*scope, window)) {
        SingleInstanceCoordinator coordinator;
        const auto wake = RegisterWindowMessageW(L"Pulse.NativeExplorer.Acceptance.QueueReady");
        const auto port = coordinator.CreateExplorerSubmissionPort(state.hwnd, wake);
        check(port != nullptr, "production takeover has a real submission port rather than a mock sink");
        ExplorerWindowTakeover watcher(port, scope);
        watcher.SetTakeThisPc(true);
        const auto retain_deadline = GetTickCount64() + 5000;
        while (!native_explorer::IsExplicitNativeWindow(window) && GetTickCount64() < retain_deadline)
            NativeLivePump(30);
        check(native_explorer::IsExplicitNativeWindow(window), "production takeover reads and retains the explicit native request");
        NativeLivePump(1200);
        std::wstring path; selection.clear();
        const bool read = NativeLiveView(owned.Get(), path, selection);
        check(read && _wcsicmp(path.c_str(), target.c_str()) == 0 && selection == std::vector<std::wstring>{L"sample.txt"},
              "real Explorer remains at the requested folder with the requested file selected");
        check(IsWindowVisible(window) && native_explorer::IsExplicitNativeWindow(window),
              "native window remains visible and retained while the production takeover observer runs");
        {
            std::lock_guard lock(scope->evidence->mutex);
            const auto& evidence = *scope->evidence;
            bool ready = false;
            const size_t start = evidence.trace_count > evidence.trace.size() ? evidence.trace_count - evidence.trace.size() : 0;
            for (size_t i = start; i < evidence.trace_count; ++i) {
                const auto& trace = evidence.trace[i % evidence.trace.size()];
                log << "watcher_trace=" << trace.stage << " hr=" << trace.result << '\n';
                ready = ready || std::string_view(trace.stage) == "start-scoped-ready";
            }
            check(ready, "real scoped takeover watcher completed startup and discovery");
            check(scope->evidence->submitted.folder.empty() && !scope->evidence->quit,
                  "takeover never submitted or closed the explicitly retained native window");
        }
        watcher.Stop();
    }
    // Never close an old window, a navigated-away window, or a window with new tabs.
    int tabs = 0;
    EnumChildWindows(window, [](HWND child, LPARAM context) -> BOOL {
        wchar_t name[80]{}; GetClassNameW(child, name, ARRAYSIZE(name));
        if (wcscmp(name, L"ShellTabWindowClass") == 0) ++*reinterpret_cast<int*>(context);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&tabs));
    std::wstring current; std::vector<std::wstring> names;
    const bool close_owned = tagged && !before.contains(window) && ExplorerScopeWindow(*scope, window) &&
        tabs == 1 && NativeLiveView(owned.Get(), current, names) && _wcsicmp(current.c_str(), target.c_str()) == 0;
    check(close_owned, "cleanup identity and single-tab checks authorize only this newly opened fixture window");
    if (close_owned) {
        const auto result = owned->Quit();
        const auto closed_deadline = GetTickCount64() + 2500;
        while (IsWindow(window) && GetTickCount64() < closed_deadline) NativeLivePump(30);
        check(SUCCEEDED(result) && !IsWindow(window), "only the newly opened fixture Explorer window was closed");
    } else if (tagged && IsWindow(window)) RemovePropW(window, kExplorerScopeOwner);
    log << "new_hwnd=" << reinterpret_cast<uintptr_t>(window) << " process=" << scope->token.pid
        << " tabs=" << tabs << " cleanup=" << close_owned << '\n';
}
}

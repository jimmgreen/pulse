// explorer_window_takeover.cpp — see explorer_window_takeover.h.
#include "explorer_window_takeover.h"

#include "shell_window_plan.h"
#include "shell_window_registry.h"   // TraceShellWindows

// shlobj.h first: it brings in objbase (`interface`) under WIN32_LEAN_AND_MEAN.
#include <shlobj.h>
#include <exdisp.h>
#include <exdispid.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cwchar>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <type_traits>

namespace pulse::app {

using Microsoft::WRL::ComPtr;

struct ExplorerWindowTakeover::Shared {
    HWND window = nullptr;
    UINT message = 0;
    std::atomic<bool> stopping{false};
    std::atomic<DWORD> thread_id{0};
    std::mutex handoffs_mutex;
    std::vector<std::weak_ptr<ExplorerHandoff>> handoffs;
};

namespace {

constexpr UINT kStopMessage = WM_APP + 1;   // thread messages
constexpr UINT kScanMessage = WM_APP + 2;
constexpr UINT kPollMs = 50;
constexpr ULONGLONG kQuitTimeoutMs = 2000;   // a source still open after Quit is shown again

struct PidlDeleter {
    void operator()(std::remove_pointer_t<PIDLIST_ABSOLUTE>* pidl) const noexcept { CoTaskMemFree(pidl); }
};
using UniquePidl = std::unique_ptr<std::remove_pointer_t<PIDLIST_ABSOLUTE>, PidlDeleter>;

bool IsExplorerProcess(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t image[MAX_PATH]{};
    DWORD size = ARRAYSIZE(image);
    const bool ok = QueryFullProcessImageNameW(process, 0, image, &size) != FALSE;
    CloseHandle(process);
    return ok && _wcsicmp(PathFindFileNameW(image), L"explorer.exe") == 0;
}

bool IsWindows11() {
    using VersionFn = LONG (WINAPI*)(OSVERSIONINFOW*);
    const auto version_fn = reinterpret_cast<VersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    return version_fn && version_fn(&version) == 0 && version.dwBuildNumber >= 22000;
}

bool HasClass(HWND hwnd, const wchar_t* name) {
    wchar_t text[64]{};
    return GetClassNameW(hwnd, text, ARRAYSIZE(text)) && wcscmp(text, name) == 0;
}

// Tabs of a File Explorer window: one ShellTabWindowClass each (IShellWindows
// lists only active views on Windows 11).
int CountTabs(HWND root) {
    int tabs = 0;
    EnumChildWindows(root, [](HWND child, LPARAM param) -> BOOL {
        if (HasClass(child, L"ShellTabWindowClass")) ++*reinterpret_cast<int*>(param);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&tabs));
    return tabs;
}

bool IsThisPc(PCIDLIST_ABSOLUTE pidl) {
    PIDLIST_ABSOLUTE computer = nullptr;
    if (FAILED(SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &computer))) return false;
    const bool same = ILIsEqual(pidl, computer) != FALSE;
    CoTaskMemFree(computer);
    return same;
}

// DShellWindowsEvents sink. The scan runs from the message loop rather than
// inside Explorer's callback.
class WindowEvents final : public IDispatch {
public:
    explicit WindowEvents(DWORD thread_id) : thread_id_(thread_id) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDispatch || riid == DIID_DShellWindowsEvents) {
            *ppv = static_cast<IDispatch*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const long refs = --refs_;
        if (refs == 0) delete this;
        return static_cast<ULONG>(refs);
    }
    IFACEMETHODIMP GetTypeInfoCount(UINT* count) override {
        if (!count) return E_POINTER;
        *count = 0;
        return S_OK;
    }
    IFACEMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo** info) override {
        if (info) *info = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return DISP_E_UNKNOWNNAME; }
    IFACEMETHODIMP Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override {
        if (id == DISPID_WINDOWREGISTERED) PostThreadMessageW(thread_id_, kScanMessage, 0, 0);
        return S_OK;
    }

private:
    ~WindowEvents() = default;
    std::atomic<long> refs_{1};
    const DWORD thread_id_;
};

struct Candidate {
    HWND hwnd = nullptr;
    ULONGLONG registered_at = 0;
    ULONGLONG view_ready_at = 0;
    bool close_committed = false;   // Pulse has the folder; only a late /select is awaited
    ComPtr<IWebBrowser2> browser;
    DWORD process_id = 0;
    ExplorerTakeoverRequest submitted;
};

class Watcher;
thread_local Watcher* t_watcher = nullptr;   // the WinEvent hooks' target

class Watcher {
public:
    explicit Watcher(ExplorerWindowTakeover::Shared& shared) : shared_(shared) {}
    ~Watcher() {
        for (HWINEVENTHOOK hook : hooks_)
            if (hook) UnhookWinEvent(hook);
        t_watcher = nullptr;
        for (auto& candidate : candidates_)
            if (candidate.submitted.handoff) candidate.submitted.handoff->Cancel();
        // Never leave an invisible window behind (Pulse closing, takeover off).
        while (!hidden_.empty()) Restore(hidden_.begin()->first);
        if (point_ && advise_cookie_) point_->Unadvise(advise_cookie_);
        if (timer_) KillTimer(nullptr, timer_);
    }
    Watcher(const Watcher&) = delete;
    Watcher& operator=(const Watcher&) = delete;

    bool Start() {
        HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows_));
        if (FAILED(hr)) {
            TraceShellWindows(L"takeover: CoCreateInstance(ShellWindows) hr=0x%08x", static_cast<unsigned>(hr));
            return false;
        }
        // Windows open now are the user's; only later ones are candidates.
        ForEachExplorerWindow([&](HWND hwnd, IWebBrowser2*) { known_.insert(hwnd); });
        ComPtr<IConnectionPointContainer> container;
        hr = windows_.As(&container);
        if (SUCCEEDED(hr)) hr = container->FindConnectionPoint(DIID_DShellWindowsEvents, &point_);
        if (SUCCEEDED(hr)) {
            sink_.Attach(new WindowEvents(GetCurrentThreadId()));
            hr = point_->Advise(sink_.Get(), &advise_cookie_);
        }
        // Out of context: delivered through this thread's message loop.
        t_watcher = this;
        // A new File Explorer window takes the foreground about 250 ms before
        // it shows (measured, Windows 11); its EVENT_OBJECT_CREATE only comes
        // after it is already on screen. Hide on the first of FOREGROUND and
        // SHOW, check again on SHOW. Both are rare, unlike LOCATIONCHANGE.
        hooks_[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &Watcher::OnShow, 0,
                                    0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        hooks_[1] = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, &Watcher::OnShow, 0, 0,
                                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        TraceShellWindows(L"takeover: watching, %zu windows already open, advise hr=0x%08x hooks=%d/%d",
                          known_.size(), static_cast<unsigned>(hr), hooks_[0] != nullptr, hooks_[1] != nullptr);
        return SUCCEEDED(hr);
    }

    void Scan() {
        for (auto it = known_.begin(); it != known_.end();) it = IsWindow(*it) ? std::next(it) : known_.erase(it);
        ForEachExplorerWindow([&](HWND hwnd, IWebBrowser2* browser) {
            if (!hwnd || !known_.insert(hwnd).second) return;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == GetCurrentProcessId() || !IsExplorerProcess(pid)) return;
            // Shift held: File Explorer is wanted this time.
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
                TraceShellWindows(L"takeover: hwnd=%p left alone (Shift)", hwnd);
                Restore(hwnd);
                return;
            }
            Candidate candidate;
            candidate.hwnd = hwnd;
            candidate.registered_at = GetTickCount64();
            candidate.browser = browser;
            candidate.process_id = pid;
            candidates_.push_back(std::move(candidate));
            TraceShellWindows(L"takeover: new Explorer window hwnd=%p", hwnd);
            // DISPID_WINDOWREGISTERED can arrive before EVENT_OBJECT_SHOW; Shown()
            // then skips the window (already known), so hide it here. Otherwise a
            // visible source whose /select lands late is aborted and both stay open.
            if (!hidden_.count(hwnd)) HideNew(hwnd, L"registered");
        });
        UpdateTimer();
    }

    void Poll() {
        SweepHidden();
        for (auto it = candidates_.begin(); it != candidates_.end();) {
            if (!IsWindow(it->hwnd)) {
                if (it->submitted.handoff && !it->close_committed) it->submitted.handoff->Cancel();
                it = candidates_.erase(it);
                continue;
            }
            if (it->submitted.handoff) {
                const auto handoff = it->submitted.handoff;
                ExplorerTakeoverRequest current;
                if (!it->close_committed) {
                    if (shared_.stopping || GetTickCount64() >= handoff->deadline_tick)
                        handoff->Cancel(HandoffState::Expired);
                    const auto state = handoff->state.load();
                    if (state == HandoffState::Pending) { ++it; continue; }
                    const SourceCloseStep step =
                        state == HandoffState::Ready ? DecideSourceClose(CloseProbe(*it, current))
                                                     : SourceCloseStep::Abort;
                    bool close_committed = false;
                    if (step != SourceCloseStep::Abort) {
                        std::lock_guard lock(shared_.handoffs_mutex);
                        close_committed = !shared_.stopping && handoff->ClaimClose(GetTickCount64());
                    }
                    // ClaimClose is the commitment point. Stop can cancel requests
                    // before it, but cannot revoke an already committed close.
                    if (!close_committed) {
                        handoff->Cancel();
                        Restore(it->hwnd);
                        it = candidates_.erase(it);
                        continue;
                    }
                    it->close_committed = true;
                    if (step == SourceCloseStep::Wait) { ++it; continue; }
                    FinishClose(*it, step, std::move(current));
                    it = candidates_.erase(it);
                    continue;
                }
                // Committed; the hidden source only waits for a late /select.
                // Pulse already shows the folder, so a hidden single-tab source
                // whose view cannot be read this time is still closed.
                const SourceCloseProbe probe = CloseProbe(*it, current);
                SourceCloseStep step = DecideSourceClose(probe);
                if (step == SourceCloseStep::Wait) { ++it; continue; }
                if (step == SourceCloseStep::Abort && probe.hidden && probe.single_tab)
                    step = SourceCloseStep::Close;
                if (step == SourceCloseStep::Abort) Restore(it->hwnd);
                else FinishClose(*it, step, std::move(current));
                it = candidates_.erase(it);
                continue;
            }
            ExplorerWindowProbe probe;
            const ULONGLONG now = GetTickCount64();
            probe.age_ms = static_cast<unsigned>(now - it->registered_at);
            ExplorerTakeoverRequest request;
            Read(*it, probe, request);
            if (probe.view_ready && !it->view_ready_at) it->view_ready_at = now;
            probe.view_age_ms = it->view_ready_at ? static_cast<unsigned>(now - it->view_ready_at) : 0;
            const ExplorerTakeoverStep step = DecideExplorerTakeover(probe);
            if (step == ExplorerTakeoverStep::Wait) {
                ++it;
                continue;
            }
            TraceShellWindows(L"takeover: hwnd=%p %s after %u ms folder=[%s] selected=%zu", it->hwnd,
                              step == ExplorerTakeoverStep::Take ? L"taken" : L"left", probe.age_ms,
                              request.folder.c_str(), request.names.size());
            if (step == ExplorerTakeoverStep::Take && Take(*it, std::move(request))) {
                ++it;
                continue;
            }
            Restore(it->hwnd);
            it = candidates_.erase(it);
        }
        UpdateTimer();
    }

private:
    template <class Fn>
    void ForEachExplorerWindow(Fn&& fn) {
        long count = 0;
        if (!windows_ || FAILED(windows_->get_Count(&count))) return;
        for (long i = 0; i < count; ++i) {
            VARIANT index;
            VariantInit(&index);
            index.vt = VT_I4;
            index.lVal = i;
            ComPtr<IDispatch> item;
            if (windows_->Item(index, &item) != S_OK || !item) continue;
            // Pulse's own panes (shell_window_registry) are IWebBrowserApp only.
            ComPtr<IWebBrowser2> browser;
            if (FAILED(item.As(&browser))) continue;
            SHANDLE_PTR handle = 0;
            if (FAILED(browser->get_HWND(&handle)) || !handle) continue;
            fn(reinterpret_cast<HWND>(handle), browser.Get());
        }
    }

    static void Read(const Candidate& candidate, ExplorerWindowProbe& probe, ExplorerTakeoverRequest& request) {
        ComPtr<IServiceProvider> provider;
        ComPtr<IShellBrowser> shell_browser;
        ComPtr<IShellView> view;
        ComPtr<IFolderView2> folder_view;
        ComPtr<IPersistFolder2> folder;
        if (FAILED(candidate.browser.As(&provider)) ||
            FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&shell_browser))) ||
            FAILED(shell_browser->QueryActiveShellView(&view)) || !view || FAILED(view.As(&folder_view)) ||
            FAILED(folder_view->GetFolder(IID_PPV_ARGS(&folder))))
            return;
        PIDLIST_ABSOLUTE raw = nullptr;
        if (FAILED(folder->GetCurFolder(&raw)) || !raw) return;
        UniquePidl pidl(raw);
        probe.view_ready = true;
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetNameFromIDList(pidl.get(), SIGDN_FILESYSPATH, &path)) && path) {
            // A real directory: not a .zip or other file shown as a folder.
            const DWORD attributes = GetFileAttributesW(path);
            probe.supported = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
            request.folder = path;
            CoTaskMemFree(path);
        } else if (IsThisPc(pidl.get())) {
            probe.supported = true;
        }
        if (!probe.supported) return;
        // GetSelection fails when nothing is selected, which is a valid (empty)
        // selection: a plain "explorer.exe <folder>" window has none.
        int selected = 0;
        if (FAILED(folder_view->ItemCount(SVGIO_SELECTION, &selected))) return;
        if (selected == 0) {
            request.selection_read = true;
            return;
        }
        ComPtr<IShellItemArray> items;
        DWORD count = 0;
        if (folder_view->GetSelection(FALSE, &items) != S_OK || !items || FAILED(items->GetCount(&count))) return;
        for (DWORD i = 0; i < count; ++i) {
            ComPtr<IShellItem> item;
            PWSTR name = nullptr;
            if (FAILED(items->GetItemAt(i, &item)) ||
                FAILED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &name)) || !name)
                return;
            request.names.emplace_back(name);
            CoTaskMemFree(name);
        }
        request.selection_read = true;
        probe.selected = request.names.size();
    }

    SourceCloseProbe CloseProbe(const Candidate& candidate, ExplorerTakeoverRequest& current) {
        SourceCloseProbe probe;
        DWORD pid = 0;
        GetWindowThreadProcessId(candidate.hwnd, &pid);
        SHANDLE_PTR handle = 0;
        const bool same_window = pid == candidate.process_id && IsExplorerProcess(pid) &&
                                 SUCCEEDED(candidate.browser->get_HWND(&handle)) &&
                                 reinterpret_cast<HWND>(handle) == candidate.hwnd;
        if (!same_window) return probe;
        const HWND root = GetAncestor(candidate.hwnd, GA_ROOT);
        size_t views = 0;
        ForEachExplorerWindow([&](HWND hwnd, IWebBrowser2*) {
            if (GetAncestor(hwnd, GA_ROOT) == root) ++views;
        });
        // IShellWindows lists only active views on Windows 11; the tab
        // windows show whether anything else would close with this one.
        const int tabs = CountTabs(root);
        probe.single_tab = views == 1 && (tabs == 1 || (tabs == 0 && !IsWindows11()));
        ExplorerWindowProbe view;
        Read(candidate, view, current);
        probe.identity_kept = view.view_ready && view.supported && current.selection_read &&
                              current.folder == candidate.submitted.folder;
        auto previous = candidate.submitted.names;
        auto now_selected = current.names;
        std::sort(previous.begin(), previous.end());
        std::sort(now_selected.begin(), now_selected.end());
        probe.selection_changed = previous != now_selected;
        probe.hidden = hidden_.count(candidate.hwnd) != 0;
        probe.sent_selection = !candidate.submitted.names.empty();
        probe.view_age_ms = candidate.view_ready_at
            ? static_cast<unsigned>(GetTickCount64() - candidate.view_ready_at) : 0;
        return probe;
    }

    void FinishClose(Candidate& candidate, SourceCloseStep step, ExplorerTakeoverRequest current) {
        TraceShellWindows(L"takeover: hwnd=%p closed%s, %zu selected", candidate.hwnd,
                          step == SourceCloseStep::CloseAndSelect ? L" (late selection follows)" : L"",
                          current.names.size());
        if (step == SourceCloseStep::CloseAndSelect) {
            auto follow = std::make_unique<ExplorerTakeoverRequest>();
            follow->folder = candidate.submitted.folder;
            follow->names = std::move(current.names);
            follow->select_only = true;
            if (PostMessageW(shared_.window, shared_.message, 0, reinterpret_cast<LPARAM>(follow.get())))
                follow.release();
        }
        if (auto found = hidden_.find(candidate.hwnd); found != hidden_.end())
            found->second.closing_at = GetTickCount64();
        candidate.browser->Quit();
    }

    // EVENT_SYSTEM_FOREGROUND / EVENT_OBJECT_SHOW: a new File Explorer window
    // goes transparent and leaves the taskbar before its first frame shows,
    // until it is taken or given back.
    static void CALLBACK OnShow(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG child, DWORD,
                                DWORD event_time) {
        if (object != OBJID_WINDOW || child != CHILDID_SELF || !t_watcher) return;
        t_watcher->Shown(hwnd, event, event_time);
    }

    void Shown(HWND hwnd, DWORD event, DWORD event_time) {
        if (!hwnd || shared_.stopping || GetAncestor(hwnd, GA_ROOT) != hwnd || !HasClass(hwnd, L"CabinetWClass"))
            return;
        if (hidden_.count(hwnd)) {
            // Explorer may rewrite its extended style while it sets itself up.
            if (event == EVENT_OBJECT_SHOW) Reassert(hwnd);
            return;
        }
        if (known_.count(hwnd)) return;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId() || !IsExplorerProcess(pid)) return;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) return;
        wchar_t reason[48]{};
        swprintf_s(reason, L"%s event %lu ms old", event == EVENT_SYSTEM_FOREGROUND ? L"foreground" : L"show",
                   GetTickCount() - event_time);
        HideNew(hwnd, reason);
    }

    // Extended style while hidden: transparent, and a tool window so the
    // taskbar shows no button for it.
    static LONG_PTR HiddenStyle(LONG_PTR style) {
        return (style | WS_EX_LAYERED | WS_EX_TOOLWINDOW) & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW);
    }

    ITaskbarList* Taskbar() {
        if (!taskbar_ && SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                                    IID_PPV_ARGS(&taskbar_))) &&
            FAILED(taskbar_->HrInit()))
            taskbar_.Reset();
        return taskbar_.Get();
    }

    // Makes a new File Explorer window transparent and takes it off the
    // taskbar until it is taken or given back.
    bool HideNew(HWND hwnd, const wchar_t* reason) {
        if (!hwnd || shared_.stopping || !IsWindow(hwnd) || hidden_.count(hwnd)) return false;
        const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        // Already layered: its own alpha could not be given back exactly.
        if (style & WS_EX_LAYERED) return false;
        const ULONGLONG set_from = GetTickCount64();
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, HiddenStyle(style));
        if (!SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA)) {
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style);
            return false;
        }
        // Shown already: its button exists and outlives the style change.
        const bool visible = IsWindowVisible(hwnd) != FALSE;
        if (visible)
            if (ITaskbarList* taskbar = Taskbar()) taskbar->DeleteTab(hwnd);
        hidden_[hwnd] = Hidden{style, GetTickCount64(), 0};
        TraceShellWindows(L"takeover: hwnd=%p hidden (%s, %s, hiding took %llu ms)", hwnd, reason,
                          visible ? L"visible" : L"not shown yet", GetTickCount64() - set_from);
        UpdateTimer();
        return true;
    }

    // Puts the hidden state back if Explorer changed its extended style.
    void Reassert(HWND hwnd) {
        if (!IsWindow(hwnd)) return;
        const LONG_PTR current = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        BYTE alpha = 255;
        DWORD flags = 0;
        const bool transparent = (current & WS_EX_LAYERED) &&
                                 GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) &&
                                 (flags & LWA_ALPHA) && alpha == 0;
        if (transparent && current == HiddenStyle(current)) return;
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, HiddenStyle(current));
        SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
        if (ITaskbarList* taskbar = Taskbar()) taskbar->DeleteTab(hwnd);
        TraceShellWindows(L"takeover: hwnd=%p hidden again (style was changed)", hwnd);
    }

    void Restore(HWND hwnd) {
        const auto found = hidden_.find(hwnd);
        if (found == hidden_.end()) return;
        const LONG_PTR original = found->second.style;
        hidden_.erase(found);
        if (!IsWindow(hwnd)) return;
        // Take back only what HideNew added; Explorer may have changed other bits since.
        const LONG_PTR added = static_cast<LONG_PTR>(WS_EX_LAYERED | WS_EX_TOOLWINDOW) & ~original;
        const LONG_PTR style =
            (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & ~added) | (original & static_cast<LONG_PTR>(WS_EX_APPWINDOW));
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (IsWindowVisible(hwnd))
            if (ITaskbarList* taskbar = Taskbar()) taskbar->AddTab(hwnd);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
        TraceShellWindows(L"takeover: hwnd=%p shown again", hwnd);
    }

    // Hidden windows that never became candidates (no shell view) or did not
    // close after Quit are given back.
    void SweepHidden() {
        const ULONGLONG now = GetTickCount64();
        std::vector<HWND> give_back;
        for (auto it = hidden_.begin(); it != hidden_.end();) {
            if (!IsWindow(it->first)) { it = hidden_.erase(it); continue; }
            const bool candidate = std::any_of(candidates_.begin(), candidates_.end(),
                                               [&](const Candidate& c) { return c.hwnd == it->first; });
            if (it->second.closing_at ? now - it->second.closing_at >= kQuitTimeoutMs
                                      : !candidate && now - it->second.since >= kExplorerViewTimeoutMs + kPollMs * 4)
                give_back.push_back(it->first);
            ++it;
        }
        for (HWND hwnd : give_back) Restore(hwnd);
    }

    bool Take(Candidate& candidate, ExplorerTakeoverRequest request) {
        if (!request.selection_read || shared_.stopping) return false;
        request.handoff = std::make_shared<ExplorerHandoff>(GetTickCount64() + 10000);
        candidate.submitted = request;
        std::lock_guard lock(shared_.handoffs_mutex);
        if (shared_.stopping) { request.handoff->Cancel(); return false; }
        std::erase_if(shared_.handoffs, [](const auto& value) { return value.expired(); });
        shared_.handoffs.push_back(request.handoff);
        auto posted = std::make_unique<ExplorerTakeoverRequest>(std::move(request));
        if (!PostMessageW(shared_.window, shared_.message, 0, reinterpret_cast<LPARAM>(posted.get()))) {
            candidate.submitted.handoff->Cancel();
            return false;
        }
        posted.release();
        return true;
    }

    void UpdateTimer() {
        const bool busy = !candidates_.empty() || !hidden_.empty();
        if (busy && !timer_) timer_ = SetTimer(nullptr, 0, kPollMs, nullptr);
        else if (!busy && timer_) {
            KillTimer(nullptr, timer_);
            timer_ = 0;
        }
    }

    struct Hidden {
        LONG_PTR style = 0;          // extended style before it went transparent
        ULONGLONG since = 0;
        ULONGLONG closing_at = 0;    // Quit sent
    };

    ExplorerWindowTakeover::Shared& shared_;
    std::map<HWND, Hidden> hidden_;
    HWINEVENTHOOK hooks_[2] = {};   // EVENT_SYSTEM_FOREGROUND, EVENT_OBJECT_SHOW
    ComPtr<ITaskbarList> taskbar_;
    ComPtr<IShellWindows> windows_;
    ComPtr<IConnectionPoint> point_;
    ComPtr<WindowEvents> sink_;
    DWORD advise_cookie_ = 0;
    std::set<HWND> known_;
    std::vector<Candidate> candidates_;
    UINT_PTR timer_ = 0;
};

DWORD WINAPI TakeoverThread(void* param) {
    std::shared_ptr<ExplorerWindowTakeover::Shared> shared =
        std::move(*static_cast<std::shared_ptr<ExplorerWindowTakeover::Shared>*>(param));
    delete static_cast<std::shared_ptr<ExplorerWindowTakeover::Shared>*>(param);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;
    {
        MSG msg{};
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // queue before Stop can post
        shared->thread_id = GetCurrentThreadId();
        Watcher watcher(*shared);
        // Without the shell (Explorer not running) there is nothing to take.
        if (!shared->stopping) watcher.Start();
        while (!shared->stopping && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!msg.hwnd && msg.message == kScanMessage) {
                watcher.Scan();
                continue;
            }
            if (!msg.hwnd && msg.message == WM_TIMER) {
                watcher.Poll();
                continue;
            }
            if (!msg.hwnd && msg.message == kStopMessage) continue;
            // COM delivers Explorer's events through this loop.
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    return 0;
}

} // namespace

ExplorerWindowTakeover::ExplorerWindowTakeover(HWND window, UINT message)
    : shared_(std::make_shared<Shared>()) {
    shared_->window = window;
    shared_->message = message;
    auto* param = new std::shared_ptr<Shared>(shared_);
    thread_ = CreateThread(nullptr, 0, &TakeoverThread, param, 0, nullptr);
    if (!thread_) delete param;
}

ExplorerWindowTakeover::~ExplorerWindowTakeover() { Stop(); }

void ExplorerWindowTakeover::Stop(DWORD timeout_ms) {
    if (!thread_) return;
    {
        std::lock_guard lock(shared_->handoffs_mutex);
        shared_->stopping = true;
        for (auto& weak : shared_->handoffs) if (auto handoff = weak.lock()) handoff->Cancel();
    }
    // Before the thread has a queue the post fails; it checks `stopping`
    // right after creating one.
    if (const DWORD thread_id = shared_->thread_id) PostThreadMessageW(thread_id, kStopMessage, 0, 0);
    WaitForSingleObject(thread_, timeout_ms);
    CloseHandle(thread_);
    thread_ = nullptr;
}

} // namespace pulse::app

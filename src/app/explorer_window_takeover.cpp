// explorer_window_takeover.cpp — see explorer_window_takeover.h.
#include "explorer_window_takeover.h"
#include "../common/native_browse_intent.h"
#include "native_explorer.h"
#include "explorer_source_lifetime.h"
#include "../common/runtime_log.h"
#include "explorer_source_scope.h"
#include "explorer_open_context.h"
#include "explorer_submission_port.h"
#include "explorer_window_veil.h"
#include "explorer_launch_origin.h"
#include "direct_reveal_targets.h"
#include "../common/os_version.h"

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
#include <mutex>
#include <map>
#include <set>
#include <type_traits>

namespace pulse::app {

using Microsoft::WRL::ComPtr;

struct ExplorerWindowTakeover::Shared : std::enable_shared_from_this<ExplorerWindowTakeover::Shared> {
    std::shared_ptr<const ExplorerSourceScope> scope;
    std::shared_ptr<ExplorerSubmissionPort> port;
    std::atomic<bool> stopping{false};
    std::atomic<bool> take_this_pc{true};
    std::atomic<DWORD> thread_id{0};
    std::mutex handoffs_mutex;
    std::vector<std::weak_ptr<ExplorerHandoff>> handoffs;
};

namespace {

constexpr UINT kStopMessage = WM_APP + 1;   // thread messages
constexpr UINT kScanMessage = WM_APP + 2;
constexpr UINT kPollMessage = WM_APP + 3;
constexpr UINT kPollMs = 50;
constexpr UINT kDiscoveryMs = 1000;
// A new Explorer window stays invisible at most this long while undecided or
// pending handoff; past it the window shows and the takeover carries on.
constexpr ULONGLONG kVeilMaxMs = 2500;
// How far back a launcher observation may explain a reveal being taken.
constexpr ULONGLONG kRevealOriginWindowMs = 15000;

struct PidlDeleter {
    void operator()(std::remove_pointer_t<PIDLIST_ABSOLUTE>* pidl) const noexcept { CoTaskMemFree(pidl); }
};
using UniquePidl = std::unique_ptr<std::remove_pointer_t<PIDLIST_ABSOLUTE>, PidlDeleter>;

bool IsExplorerProcess(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t image[32768]{}, windows[32768]{};
    DWORD size = ARRAYSIZE(image);
    const bool ok = QueryFullProcessImageNameW(process, 0, image, &size) != FALSE;
    CloseHandle(process);
    const UINT length = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
    if (!ok || !length || length >= ARRAYSIZE(windows)) return false;
    const auto actual = CanonicalExplorerScopePath(image);
    const auto expected = CanonicalExplorerScopePath(std::wstring(windows) + L"\\explorer.exe");
    return !actual.empty() && !expected.empty() && _wcsicmp(actual.c_str(), expected.c_str()) == 0;
}

// The kernel's build, not the (compatibility-layer shimmed) RtlGetVersion one.
bool IsWindows11() {
    return pulse::os::TrueWindowsBuild() >= 22000;
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
        if (id == DISPID_WINDOWREGISTERED) PostThreadMessageW(thread_id_, kScanMessage,
            (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 1 : 0, 0);
        return S_OK;
    }

private:
    ~WindowEvents() = default;
    std::atomic<long> refs_{1};
    const DWORD thread_id_;
};

// Window properties vanish when an HWND is destroyed, including reuse within
// the same Explorer process/thread. Never overwrite another watcher's marker.
struct WindowIdentity {
    HWND window = nullptr;
    std::wstring property = L"Pulse.ExplorerTakeover." + std::to_wstring(GetCurrentProcessId());
    bool Matches() const { return window && GetPropW(window, property.c_str()) == this; }
    ~WindowIdentity() { if (Matches()) RemovePropW(window, property.c_str()); }
};

struct Candidate {
    uint64_t diagnostic_id = diagnostics::runtime::NextId();
    HWND hwnd = nullptr;
    ULONGLONG registered_at = 0;
    ULONGLONG view_ready_at = 0;
    ExplorerSelectionStability selection_stability;
    ComPtr<IWebBrowser2> browser;
    DWORD process_id = 0;
    DWORD thread_id = 0;
    FILETIME process_created{};
    std::shared_ptr<WindowIdentity> identity;
    ExplorerTakeoverRequest submitted;
    std::shared_ptr<const ExplorerOpenContext> context;
};

bool SourceWindowGone(const Candidate& candidate) {
    return ExplorerSourceDestroyed(candidate.hwnd, candidate.process_id, candidate.thread_id);
}

class Watcher {
public:
    explicit Watcher(ExplorerWindowTakeover::Shared& shared) : shared_(shared) {}
    ~Watcher() {
        if (window_events_) UnhookWinEvent(window_events_);
        event_watcher_ = nullptr;
        const auto veiled = std::move(veiled_);
        for (const auto& [hwnd, since] : veiled) UnveilExplorerWindow(hwnd);
        for (auto& candidate : candidates_) {
            if (const auto handoff = candidate.submitted.handoff) {
                handoff->ConfirmClose(SourceWindowGone(candidate));
                handoff->FailClose();
                handoff->Cancel();
            }
        }
        if (point_ && advise_cookie_) point_->Unadvise(advise_cookie_);
        if (timer_) KillTimer(nullptr, timer_);
        if (discovery_timer_) KillTimer(nullptr, discovery_timer_);
    }
    Watcher(const Watcher&) = delete;
    Watcher& operator=(const Watcher&) = delete;

    bool Start() {
        const DWORD retention_error = native_intents_.Start();
        if (retention_error) {
            diagnostics::runtime::Event("native_retention_start_failed", {{"error", retention_error}});
            return false;
        }
        TraceExplorerScope(shared_.scope, "start-shellwindows");
        HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows_));
        TraceExplorerScope(shared_.scope, "start-shellwindows-result", hr);
        diagnostics::runtime::Event("explorer_watcher_start", {{"phase", 0}, {"hr", static_cast<uint32_t>(hr)}});
        if (FAILED(hr)) {
            TraceShellWindows(L"takeover: CoCreateInstance(ShellWindows) hr=0x%08x", static_cast<unsigned>(hr));
            return false;
        }
        // Windows open now are the user's; only later ones are candidates.
        if (shared_.scope) {
            if (!shared_.scope->evidence || !shared_.scope->evidence->close_authorized_event) {
                TraceExplorerScope(shared_.scope, "start-reject-close-event", E_HANDLE); return false;
            }
            if (!ExplorerScopeWindow(*shared_.scope, reinterpret_cast<HWND>(shared_.scope->token.hwnd))) {
                TraceExplorerScope(shared_.scope, "start-reject-identity", E_ACCESSDENIED); return false;
            }
        }
        TraceExplorerScope(shared_.scope, "start-baseline");
        // A previous Pulse may have exited while a window was veiled.
        if (!shared_.scope) UnveilAbandonedExplorerWindows();
        // A visible Explorer may not have registered its Shell view yet. It is
        // still user-owned at startup and must not be admitted by a later scan.
        if (!shared_.scope && !EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
                if (HasClass(hwnd, L"CabinetWClass"))
                    reinterpret_cast<Watcher*>(param)->known_.insert(hwnd);
                return TRUE;
            }, reinterpret_cast<LPARAM>(this))) {
            diagnostics::runtime::Event("explorer_watcher_start", {{"phase", 3},
                {"hr", static_cast<uint32_t>(HRESULT_FROM_WIN32(GetLastError()))}}, diagnostics::runtime::Level::Error);
            return false;
        }
        if (!ForEachExplorerWindow([&](HWND hwnd, IWebBrowser2*) {
            if (!shared_.scope || !ExplorerScopeWindow(*shared_.scope, hwnd)) known_.insert(hwnd);
        })) {
            diagnostics::runtime::Event("explorer_watcher_start", {{"phase", 1}, {"hr", static_cast<uint32_t>(E_FAIL)}}, diagnostics::runtime::Level::Error);
            TraceExplorerScope(shared_.scope, "start-reject-incomplete-baseline", E_FAIL); return false;
        }
        if (shared_.scope) { Scan(); TraceExplorerScope(shared_.scope, "start-scoped-ready", S_OK); return true; }
        // Capture the exception before Shell COM finishes registering the view.
        // The marker belongs to this HWND lifetime, so releasing Shift cannot
        // turn a deliberately retained Explorer window into a later candidate.
        event_watcher_ = this;
        window_events_ = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr,
            OnWindowEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        ComPtr<IConnectionPointContainer> container;
        hr = windows_.As(&container);
        if (SUCCEEDED(hr)) hr = container->FindConnectionPoint(DIID_DShellWindowsEvents, &point_);
        if (SUCCEEDED(hr)) {
            sink_.Attach(new WindowEvents(GetCurrentThreadId()));
            hr = point_->Advise(sink_.Get(), &advise_cookie_);
        }
        TraceShellWindows(L"takeover: watching, %zu windows already open, advise hr=0x%08x",
                          known_.size(), static_cast<unsigned>(hr));
        diagnostics::runtime::Event("explorer_watcher_start", {{"phase", 2}, {"hr", static_cast<uint32_t>(hr)},
            {"baseline_windows", known_.size()}, {"window_hook", window_events_ != nullptr}},
            FAILED(hr) ? diagnostics::runtime::Level::Error : diagnostics::runtime::Level::Info);
        // Registration callbacks can be missed during subscription or transient
        // Shell enumeration failure. Keep a slow discovery timer independent of
        // the short-lived handoff polling timer; no UI thread filesystem work.
        discovery_timer_ = SetTimer(nullptr, 0, kDiscoveryMs, nullptr);
        if (!discovery_timer_) diagnostics::runtime::Event("explorer_watcher_start", {{"phase", 4},
            {"hr", static_cast<uint32_t>(HRESULT_FROM_WIN32(GetLastError()))}}, diagnostics::runtime::Level::Error);
        if (FAILED(hr) && !discovery_timer_) return false;
        Scan();
        return true;
    }

    void Timer(UINT_PTR id) {
        if (id == discovery_timer_) Scan();
        else if (id == timer_) Poll();
    }

    void Scan(bool keep_new_windows = false) {
        const size_t previous_candidates = candidates_.size();
        TraceExplorerScope(shared_.scope, "scan-enter");
        for (auto it = known_.begin(); it != known_.end();) it = IsWindow(*it) ? std::next(it) : known_.erase(it);
        std::erase_if(shift_windows_, [](const auto& entry) { return !entry.second->Matches(); });
        const bool complete = ForEachExplorerWindow([&](HWND hwnd, IWebBrowser2* browser) {
            if (shared_.scope && !ExplorerScopeWindow(*shared_.scope, hwnd)) return;
            if (!hwnd || !known_.insert(hwnd).second) {
                TraceExplorerScope(shared_.scope, "scan-reject-known", S_FALSE); return;
            }
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == GetCurrentProcessId() || !IsExplorerProcess(pid)) {
                diagnostics::runtime::Event("explorer_candidate", {{"candidate_id", diagnostics::runtime::NextId()},
                    {"window", reinterpret_cast<uintptr_t>(hwnd)}, {"source_pid", pid}, {"reason", 3}});
                TraceExplorerScope(shared_.scope, "scan-reject-process", E_ACCESSDENIED); return;
            }
            if (native_explorer::IsExplicitNativeWindow(hwnd)) return;
            if (KeepNative(hwnd)) return;
            // Shift held: File Explorer is wanted this time.
            const auto retained = shift_windows_.find(hwnd);
            if (keep_new_windows || (GetAsyncKeyState(VK_SHIFT) & 0x8000) ||
                (retained != shift_windows_.end() && retained->second->Matches())) {
                diagnostics::runtime::Event("explorer_candidate", {{"candidate_id", diagnostics::runtime::NextId()},
                    {"window", reinterpret_cast<uintptr_t>(hwnd)}, {"source_pid", pid}, {"reason", 1}});
                TraceExplorerScope(shared_.scope, "scan-reject-shift", S_FALSE);
                TraceShellWindows(L"takeover: hwnd=%p left alone (Shift)", hwnd);
                return;
            }
            Candidate candidate;
            candidate.hwnd = hwnd;
            candidate.registered_at = GetTickCount64();
            candidate.browser = browser;
            candidate.process_id = pid;
            candidate.thread_id = GetWindowThreadProcessId(hwnd, nullptr);
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            FILETIME exited{}, kernel{}, user{};
            const bool identified = process && GetProcessTimes(process, &candidate.process_created,
                                                               &exited, &kernel, &user);
            if (process) CloseHandle(process);
            if (!identified || !candidate.thread_id) {
                diagnostics::runtime::Event("explorer_candidate", {{"candidate_id", candidate.diagnostic_id},
                    {"window", reinterpret_cast<uintptr_t>(hwnd)}, {"source_pid", pid}, {"reason", 4}});
                TraceExplorerScope(shared_.scope, "scan-reject-process-time", HRESULT_FROM_WIN32(GetLastError())); return;
            }
            if (!ScopedView(candidate)) {
                diagnostics::runtime::Event("explorer_candidate", {{"candidate_id", candidate.diagnostic_id},
                    {"window", reinterpret_cast<uintptr_t>(hwnd)}, {"source_pid", pid}, {"reason", 5}});
                return;
            }
            candidate.identity = std::make_shared<WindowIdentity>();
            candidate.identity->window = hwnd;
            if (GetPropW(hwnd, candidate.identity->property.c_str()) ||
                !SetPropW(hwnd, candidate.identity->property.c_str(), candidate.identity.get())) {
                diagnostics::runtime::Event("explorer_candidate", {{"candidate_id", candidate.diagnostic_id},
                    {"window", reinterpret_cast<uintptr_t>(hwnd)}, {"source_pid", pid}, {"reason", 6}});
                TraceExplorerScope(shared_.scope, "scan-reject-window-marker", E_ACCESSDENIED); return;
            }
            candidates_.push_back(std::move(candidate));
            TraceExplorerScope(shared_.scope, "scan-admitted", S_OK);
            TraceShellWindows(L"takeover: new Explorer window hwnd=%p", hwnd);
        });
        if (complete != scan_complete_) {
            diagnostics::runtime::Event("explorer_watcher_scan", {{"complete", complete}});
            scan_complete_ = complete;
        }
        ReconcileVeils();
        UpdateTimer();
        // Start the stability observation now, rather than after the first
        // timer period. COM stays outside registration/WinEvent callbacks.
        if (candidates_.size() > previous_candidates)
            PostThreadMessageW(GetCurrentThreadId(), kPollMessage, 0, 0);
    }

    void Poll() {
        for (auto it = candidates_.begin(); it != candidates_.end();) {
            if (const auto handoff = it->submitted.handoff;
                handoff && handoff->state == HandoffState::Closing) {
                // Observe destruction before timeout/stop: a successful HRESULT
                // is only acceptance and Explorer can ignore a close request.
                if (handoff->ConfirmClose(SourceWindowGone(*it))) {
                    WakePresentation(*handoff);
                    TraceExplorerScope(shared_.scope, "handoff-source-destroyed", S_OK);
                    it = candidates_.erase(it);
                } else if (GetTickCount64() >= handoff->deadline_tick) {
                    diagnostics::runtime::Event("explorer_source_close_unconfirmed", {
                        {"request_id", handoff->diagnostics.id},
                        {"source_exists", IsWindow(it->hwnd) != FALSE},
                        {"marker_matches", it->identity && it->identity->Matches()},
                        {"source_pid", it->process_id}});
                    handoff->FailClose(HandoffState::Expired);
                    WakePresentation(*handoff);
                    TraceExplorerScope(shared_.scope, "handoff-source-still-open", E_FAIL);
                    it = candidates_.erase(it);
                } else ++it;
                continue;
            }
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
                if (!it->submitted.handoff) diagnostics::runtime::Event("explorer_candidate",
                    {{"candidate_id", it->diagnostic_id}, {"window", reinterpret_cast<uintptr_t>(it->hwnd)},
                     {"source_pid", it->process_id}, {"reason", 1}});
                if (it->submitted.handoff) it->submitted.handoff->Cancel();
                it = candidates_.erase(it);
                continue;
            }
            if (native_explorer::IsExplicitNativeWindow(it->hwnd)) {
                if (it->submitted.handoff) it->submitted.handoff->Cancel();
                UnveilExplorerWindow(it->hwnd); veiled_.erase(it->hwnd);
                it = candidates_.erase(it); continue;
            }
            if (it->submitted.handoff) {
                const auto handoff = it->submitted.handoff;
                if (shared_.stopping) handoff->Cancel();
                else if (GetTickCount64() >= handoff->deadline_tick) handoff->Cancel(HandoffState::Expired);
                const auto state = handoff->state.load();
                if (state == HandoffState::Pending) {
                    // No close is possible before background preparation. Avoid repeatedly
                    // enumerating Shell COM while the UI is still pending; retain
                    // the full scoped window identity and unchanged deadline.
                    if (!IsWindow(it->hwnd) || (shared_.scope && !ExplorerScopeWindow(*shared_.scope, it->hwnd)))
                        handoff->Cancel();
                    if (shared_.stopping) handoff->Cancel();
                    else if (GetTickCount64() >= handoff->deadline_tick) handoff->Cancel(HandoffState::Expired);
                    ++it;
                    continue;
                }
                if (state != HandoffState::Ready) {
                    it = candidates_.erase(it);
                    continue;
                }
            }
            // Admission/pre-submit, Ready and the final Quit recheck continue to
            // require the complete folder, selection and single-tab evidence.
            const bool source_exists = IsWindow(it->hwnd) != FALSE;
            const bool scoped_view = source_exists && (it->submitted.handoff || ScopedView(*it));
            if (!source_exists || !scoped_view) {
                if (!it->submitted.handoff) diagnostics::runtime::Event("explorer_candidate",
                    {{"candidate_id", it->diagnostic_id}, {"reason", 7},
                     {"window", reinterpret_cast<uintptr_t>(it->hwnd)},
                     {"source_exists", source_exists}, {"scoped_view", scoped_view},
                     {"source_pid", it->process_id}, {"age_ms", GetTickCount64() - it->registered_at},
                     {"window_identity_matches", it->identity && it->identity->Matches()}});
                if (it->submitted.handoff) it->submitted.handoff->Cancel();
                it = candidates_.erase(it);
                continue;
            }
            if (native_explorer::IsExplicitNativeWindow(it->hwnd)) {
                if (it->submitted.handoff) it->submitted.handoff->Cancel();
                UnveilExplorerWindow(it->hwnd); veiled_.erase(it->hwnd);
                it = candidates_.erase(it); continue;
            }
            if (it->submitted.handoff) {
                const auto handoff = it->submitted.handoff;
                if (shared_.stopping || GetTickCount64() >= handoff->deadline_tick)
                    handoff->Cancel(HandoffState::Expired);
                const auto state = handoff->state.load();
                if (state == HandoffState::Pending) { ++it; continue; }
                // FinishClose owns the complete final COM revalidation. An
                // identical preliminary scan adds latency, not authorization.
                if (state == HandoffState::Ready) {
                    if (FinishClose(*it) == SourceCloseStep::Wait) { ++it; continue; }
                } else handoff->Cancel();
                if (handoff->state == HandoffState::Closing) ++it;
                else it = candidates_.erase(it);
                continue;
            }
            ExplorerWindowProbe probe;
            ExplorerTakeoverRequest request;
            Read(*it, probe, request);
            if (KeepNative(it->hwnd, request)) {
                if (it->submitted.handoff) it->submitted.handoff->Cancel();
                UnveilExplorerWindow(it->hwnd); veiled_.erase(it->hwnd);
                it = candidates_.erase(it); continue;
            }
            if (WaitNative(it->hwnd, request.folder, request.selection_read)) {
                UnveilExplorerWindow(it->hwnd); veiled_.erase(it->hwnd);
                ++it; continue;
            }
            if (native_explorer::RetainRequestedWindow(it->hwnd, request.folder, request.names, request.selection_read)) {
                UnveilExplorerWindow(it->hwnd); veiled_.erase(it->hwnd);
                it = candidates_.erase(it); continue;
            }
            // Explorer can report Home or an empty selection during /select.
            if (native_explorer::ShouldWaitForRequest(it->hwnd, request.folder)) { ++it; continue; }
            const ULONGLONG now = GetTickCount64();
            probe.age_ms = static_cast<unsigned>(now - it->registered_at);
            probe.selection_read = request.selection_read;
            probe.stable_age_ms = it->selection_stability.Observe(probe.age_ms,
                probe.view_ready && probe.supported && request.selection_read, request.folder, request.names);
            // This PC opened while 「此电脑」 is left to File Explorer: the user
            // chose Explorer for it (the desktop icon, Win+E set to This PC).
            const bool this_pc_retained = probe.supported && request.folder.empty() && !shared_.take_this_pc;
            if (this_pc_retained) probe.supported = false;
            if (probe.view_ready && !it->view_ready_at) it->view_ready_at = now;
            probe.view_age_ms = it->view_ready_at ? static_cast<unsigned>(now - it->view_ready_at) : 0;
            const ExplorerTakeoverStep step = DecideExplorerTakeover(probe);
            if (step == ExplorerTakeoverStep::Wait) {
                ++it;
                continue;
            }
            diagnostics::runtime::Event("explorer_candidate", {{"candidate_id", it->diagnostic_id},
                {"window", reinterpret_cast<uintptr_t>(it->hwnd)}, {"source_pid", it->process_id},
                {"reason", this_pc_retained ? 8u : !probe.view_ready ? 9u : !probe.supported ?
                    (request.folder.empty() ? 2u : 10u) : !request.selection_read ? 11u :
                    step == ExplorerTakeoverStep::Take ? 0u : 12u}, {"age_ms", probe.age_ms},
                {"view_age_ms", probe.view_age_ms}, {"stable_age_ms", probe.stable_age_ms}, {"view_ready", probe.view_ready},
                {"supported", probe.supported}, {"selection_read", request.selection_read}, {"selected", probe.selected},
                {"tabs", static_cast<uint64_t>(CountTabs(GetAncestor(it->hwnd, GA_ROOT)))}});
            TraceShellWindows(L"takeover: hwnd=%p %s after %u ms folder=[%s] selected=%zu", it->hwnd,
                              step == ExplorerTakeoverStep::Take ? L"taken" : L"left", probe.age_ms,
                              request.folder.c_str(), request.names.size());
            if (step == ExplorerTakeoverStep::Take && Take(*it, std::move(request))) {
                ++it;
                continue;
            }
            it = candidates_.erase(it);
        }
        ReconcileVeils();
        UpdateTimer();
    }

private:
    void WakePresentation(const ExplorerHandoff& handoff) {
        const bool posted = shared_.port && shared_.port->WakePresentation();
        diagnostics::runtime::Event("explorer_presentation_wake",
            {{"request_id", handoff.diagnostics.id}, {"posted", posted}});
    }
    static void CALLBACK OnWindowEvent(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                        LONG object, LONG child, DWORD, DWORD event_tick) {
        auto* watcher = event_watcher_;
        if (!watcher || !hwnd || object != OBJID_WINDOW || child != CHILDID_SELF) return;
        if (event == EVENT_OBJECT_DESTROY) {
            watcher->native_intents_.ForgetWindow(hwnd);
            watcher->shift_windows_.erase(hwnd);
            watcher->known_.erase(hwnd);
            watcher->veiled_.erase(hwnd);
            // The callback can run during a COM call. Defer confirmation to
            // the message loop; Poll still verifies the original lifetime.
            if (std::any_of(watcher->candidates_.begin(), watcher->candidates_.end(),
                    [hwnd](const Candidate& candidate) { return candidate.hwnd == hwnd; }))
                PostThreadMessageW(GetCurrentThreadId(), kPollMessage, 0, 0);
            return;
        }
        // Hide a new window before its first frame; the takeover decides next.
        if ((event == EVENT_OBJECT_CREATE || event == EVENT_OBJECT_SHOW) && !(GetAsyncKeyState(VK_SHIFT) & 0x8000))
            watcher->VeilNewWindow(hwnd);
        if ((event != EVENT_OBJECT_SHOW && !(GetAsyncKeyState(VK_SHIFT) & 0x8000)) ||
             !HasClass(hwnd, L"CabinetWClass") ||
            GetAncestor(hwnd, GA_ROOT) != hwnd) return;
        if (event == EVENT_OBJECT_SHOW) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            diagnostics::runtime::Event("explorer_window_shown", {
                {"window", reinterpret_cast<uintptr_t>(hwnd)}, {"source_pid", pid},
                {"already_known", watcher->known_.contains(hwnd)}, {"event_tick", event_tick}});
        }
        if (!(GetAsyncKeyState(VK_SHIFT) & 0x8000)) return;
        auto identity = std::make_shared<WindowIdentity>();
        identity->window = hwnd;
        // Do not replace the candidate marker if registration already arrived.
        if (GetPropW(hwnd, identity->property.c_str())) return;
        if (SetPropW(hwnd, identity->property.c_str(), identity.get()))
            watcher->shift_windows_[hwnd] = std::move(identity);
        else
            watcher->known_.insert(hwnd); // retaining Explorer is the safe failure mode
    }

    // Out-of-context events arrive asynchronously, but Explorer creates the
    // frame well before it first shows it; the style change is synchronous.
    void VeilNewWindow(HWND hwnd) {
        if (native_explorer::ShouldDelayVeil(hwnd)) return;
        if (shared_.scope || shared_.stopping || veiled_.contains(hwnd) || known_.contains(hwnd) ||
            !HasClass(hwnd, L"CabinetWClass") || GetAncestor(hwnd, GA_ROOT) != hwnd) return;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid || pid == GetCurrentProcessId() || !IsExplorerProcess(pid)) return;
        if (WaitNative(hwnd)) return;
        // Claim the entry before the cross-thread call can dispatch more events.
        veiled_[hwnd] = GetTickCount64();
        const bool was_visible = IsWindowVisible(hwnd) != FALSE;
        if (!VeilExplorerWindow(hwnd)) {
            veiled_.erase(hwnd);
            return;
        }
        diagnostics::runtime::Event("explorer_veil", {{"window", reinterpret_cast<uintptr_t>(hwnd)},
            {"source_pid", pid}, {"was_visible", was_visible}});
        UpdateTimer();
    }

    // Keeps a window veiled only while it may still become Pulse's: not yet
    // registered, under decision, or in an active handoff.
    void ReconcileVeils() {
        const ULONGLONG now = GetTickCount64();
        std::vector<std::pair<HWND, unsigned>> release;   // window, reason
        for (auto it = veiled_.begin(); it != veiled_.end();) {
            const HWND hwnd = it->first;
            if (!IsWindow(hwnd)) { it = veiled_.erase(it); continue; }
            const auto candidate = std::find_if(candidates_.begin(), candidates_.end(),
                [hwnd](const Candidate& value) { return value.hwnd == hwnd; });
            HandoffState state = HandoffState::Pending;
            const bool handoff = candidate != candidates_.end() && candidate->submitted.handoff;
            if (handoff) state = candidate->submitted.handoff->state.load();
            unsigned reason = 0;
            if (handoff && state == HandoffState::Closing) reason = 0;   // closing now
            else if (handoff && state != HandoffState::Pending && state != HandoffState::Ready) reason = 1;
            else if (candidate == candidates_.end() && known_.contains(hwnd)) reason = 2;   // left to Explorer
            else if (now - it->second >= kVeilMaxMs) reason = 3;
            if (!reason) { ++it; continue; }
            release.emplace_back(hwnd, reason);
            it = veiled_.erase(it);
        }
        for (const auto& [hwnd, reason] : release) {
            UnveilExplorerWindow(hwnd);
            diagnostics::runtime::Event("explorer_veil_released", {{"window", reinterpret_cast<uintptr_t>(hwnd)},
                {"reason", reason}});
        }
    }

    // A reveal taken from Explorer: remember the application that launched it
    // so the reveal bridge loads into it and its next reveal opens Pulse directly.
    static void LearnRevealOrigin(const ExplorerTakeoverRequest& request, uint64_t candidate_id) {
        if (request.folder.empty() || request.names.size() != 1) return;
        std::wstring selected = request.folder;
        if (selected.back() != L'\\') selected += L'\\';
        selected += request.names.front();
        const std::wstring origin = explorer_launch_origin::FindLauncherParent(selected, kRevealOriginWindowMs);
        if (origin.empty()) {
            diagnostics::runtime::Event("direct_reveal_origin", {{"candidate_id", candidate_id}, {"found", false}});
            return;
        }
        const bool learned = direct_reveal_targets::Learn(origin);
        const size_t slash = origin.find_last_of(L'\\');
        diagnostics::runtime::TextEvent("direct_reveal_origin",
            {{"app", std::wstring_view(origin).substr(slash == std::wstring::npos ? 0 : slash + 1)}},
            {{"candidate_id", candidate_id}, {"found", true}, {"learned", learned}});
    }

    bool ScopedView(const Candidate& candidate) {
        if (!shared_.scope) return true;
        if (!ExplorerScopeWindow(*shared_.scope, candidate.hwnd)) {
            TraceExplorerScope(shared_.scope, "view-reject-identity", E_ACCESSDENIED); return false;
        }
        SHANDLE_PTR handle = 0;
        if (FAILED(candidate.browser->get_HWND(&handle)) || reinterpret_cast<HWND>(handle) != candidate.hwnd)
            { TraceExplorerScope(shared_.scope, "view-reject-com-hwnd", E_FAIL); return false; }
        size_t views = 0;
        const bool complete = ForEachExplorerWindow([&](HWND hwnd, IWebBrowser2*) {
            if (GetAncestor(hwnd, GA_ROOT) == candidate.hwnd) ++views;
        });
        ExplorerWindowProbe probe;
        ExplorerTakeoverRequest current;
        Read(candidate, probe, current);
        if (!complete) { TraceExplorerScope(shared_.scope, "view-reject-enumeration", E_FAIL); return false; }
        if (!probe.view_ready) { TraceExplorerScope(shared_.scope, "view-not-ready", S_FALSE); return false; }
        if (!probe.supported) { TraceExplorerScope(shared_.scope, "view-unsupported", S_FALSE); return false; }
        if (!current.selection_read) { TraceExplorerScope(shared_.scope, "view-selection-unreadable", E_FAIL); return false; }
        const bool accepted = ExplorerScopeView(*shared_.scope, true, CanonicalExplorerScopePath(current.folder),
                              views, CountTabs(candidate.hwnd));
        TraceExplorerScope(shared_.scope, accepted ? "view-accepted" : "view-reject-folder-tabs", accepted ? S_OK : S_FALSE);
        return accepted;
    }

    template <class Fn>
    bool ForEachExplorerWindow(Fn&& fn) {
        long count = 0;
        if (!windows_) return false;
        const HRESULT count_hr = windows_->get_Count(&count);
        if (shared_.scope) shared_.scope->evidence->shell_count = count;
        TraceExplorerScope(shared_.scope, "shell-count", count_hr);
        if (FAILED(count_hr) || count < 0) return false;
        bool complete = true;
        for (long i = 0; i < count; ++i) {
            VARIANT index;
            VariantInit(&index);
            index.vt = VT_I4;
            index.lVal = i;
            ComPtr<IDispatch> item;
            if (shared_.scope) shared_.scope->evidence->shell_index = i;
            const HRESULT item_hr = windows_->Item(index, &item);
            const auto item_result = ClassifyShellWindowItem(item_hr, item.Get() != nullptr);
            if (item_result == ShellWindowItemResult::Absent) {
                TraceExplorerScope(shared_.scope, "shell-item-absent", item_hr); continue;
            }
            if (item_result == ShellWindowItemResult::Invalid) {
                TraceExplorerScope(shared_.scope, "shell-item-failed", item_hr); complete = false; continue;
            }
            // Pulse's own panes (shell_window_registry) are IWebBrowserApp only.
            ComPtr<IWebBrowser2> browser;
            const HRESULT browser_hr = item.As(&browser);
            if (FAILED(browser_hr)) { TraceExplorerScope(shared_.scope, "shell-non-browser", browser_hr); continue; }
            SHANDLE_PTR handle = 0;
            const HRESULT handle_hr = browser->get_HWND(&handle);
            if (FAILED(handle_hr) || !handle) {
                TraceExplorerScope(shared_.scope, "shell-hwnd-failed", handle_hr); complete = false; continue;
            }
            fn(reinterpret_cast<HWND>(handle), browser.Get());
        }
        return complete;
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
        // The complete close probe below already reads the folder, selection
        // and Shell view count. Apply the test scope to that same evidence;
        // recursively running ScopedView here repeats the expensive COM scan.
        if (shared_.scope && !ExplorerScopeWindow(*shared_.scope, candidate.hwnd)) return probe;
        DWORD pid = 0;
        const DWORD tid = GetWindowThreadProcessId(candidate.hwnd, &pid);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        FILETIME created{}, exited{}, kernel{}, user{};
        const bool same_process = process && GetProcessTimes(process, &created, &exited, &kernel, &user) &&
            CompareFileTime(&created, &candidate.process_created) == 0;
        if (process) CloseHandle(process);
        SHANDLE_PTR handle = 0;
        const bool same_window = candidate.identity && candidate.identity->Matches() &&
                                 same_process && tid == candidate.thread_id &&
                                 pid == candidate.process_id && IsExplorerProcess(pid) &&
                                 SUCCEEDED(candidate.browser->get_HWND(&handle)) &&
                                 reinterpret_cast<HWND>(handle) == candidate.hwnd;
        if (!same_window) return probe;
        const HWND root = GetAncestor(candidate.hwnd, GA_ROOT);
        size_t views = 0;
        const bool complete = ForEachExplorerWindow([&](HWND hwnd, IWebBrowser2*) {
            if (GetAncestor(hwnd, GA_ROOT) == root) ++views;
        });
        // IShellWindows lists only active views on Windows 11; the tab
        // windows show whether anything else would close with this one.
        const int tabs = CountTabs(root);
        probe.single_tab = complete && root && views == 1 && (tabs == 1 || (tabs == 0 && !IsWindows11()));
        ExplorerWindowProbe view;
        Read(candidate, view, current);
        if (shared_.scope && !ExplorerScopeView(*shared_.scope, true,
                CanonicalExplorerScopePath(current.folder), views, tabs)) return probe;
        probe.identity_kept = view.view_ready && view.supported && current.selection_read &&
                              current.folder == candidate.submitted.folder;
        auto previous = candidate.submitted.names;
        auto now_selected = current.names;
        std::sort(previous.begin(), previous.end());
        std::sort(now_selected.begin(), now_selected.end());
        probe.selection_changed = previous != now_selected;
        probe.sent_selection = !candidate.submitted.names.empty();
        probe.view_age_ms = candidate.view_ready_at
            ? static_cast<unsigned>(GetTickCount64() - candidate.view_ready_at) : 0;
        if (candidate.submitted.handoff && DecideSourceClose(probe) == SourceCloseStep::Abort)
            candidate.submitted.handoff->diagnostics.Emit(HandoffPhase::Rejected, 2,
                (!probe.identity_kept ? 1u : 0u) | (!probe.single_tab ? 2u : 0u) |
                (probe.selection_changed ? 4u : 0u));
        return probe;
    }

    SourceCloseStep FinishClose(Candidate& candidate) {
        if (KeepNative(candidate.hwnd, candidate.submitted) ||
            WaitNative(candidate.hwnd, candidate.submitted.folder, candidate.submitted.selection_read)) {
            candidate.submitted.handoff->Cancel();
            UnveilExplorerWindow(candidate.hwnd); veiled_.erase(candidate.hwnd);
            return SourceCloseStep::Abort;
        }
        if (native_explorer::IsExplicitNativeWindow(candidate.hwnd)) {
            candidate.submitted.handoff->Cancel(); return SourceCloseStep::Abort;
        }
        // Re-read immediately before claiming close: preparation may have
        // completed long after admission, and COM calls can pump messages.
        ExplorerTakeoverRequest current;
        const ULONGLONG probe_started = GetTickCount64();
        const auto step = DecideSourceClose(CloseProbe(candidate, current));
        diagnostics::runtime::Event("explorer_close_probe", {
            {"request_id", candidate.submitted.handoff->diagnostics.id},
            {"elapsed_ms", GetTickCount64() - probe_started}, {"decision", static_cast<uint64_t>(step)}});
        if (step == SourceCloseStep::Wait) return step;
        if (step != SourceCloseStep::Close) {
            if (step == SourceCloseStep::SelectAndKeep)
                candidate.submitted.handoff->diagnostics.Emit(HandoffPhase::Rejected, 6);
            candidate.submitted.handoff->Cancel();
            return step;
        }
        {
            std::lock_guard lock(shared_.handoffs_mutex);
            // Stop and ClaimClose share this linearization point. No COM while
            // holding it: Stop must not wait indefinitely on an Explorer call.
            DWORD pid = 0;
            const DWORD tid = GetWindowThreadProcessId(candidate.hwnd, &pid);
            const int tabs = CountTabs(GetAncestor(candidate.hwnd, GA_ROOT));
            if (shared_.stopping || native_explorer::IsExplicitNativeWindow(candidate.hwnd) || !candidate.identity->Matches() ||
                pid != candidate.process_id || tid != candidate.thread_id ||
                (tabs != 1 && (tabs != 0 || IsWindows11())) ||
                !candidate.submitted.handoff->ClaimClose(GetTickCount64())) {
                candidate.submitted.handoff->diagnostics.Emit(HandoffPhase::Rejected, 3, shared_.stopping);
                candidate.submitted.handoff->Cancel();
                return SourceCloseStep::Abort;
            }
        }
        if (shared_.scope) {
            if (!SetEvent(shared_.scope->evidence->close_authorized_event)) {
                TraceExplorerScope(shared_.scope, "handoff-close-signal-failed", HRESULT_FROM_WIN32(GetLastError()));
                candidate.submitted.handoff->FailClose();
                return SourceCloseStep::Abort;
            }
            TraceExplorerScope(shared_.scope, "handoff-close-authorized", S_OK);
        }
        if (native_explorer::IsExplicitNativeWindow(candidate.hwnd)) {
            candidate.submitted.handoff->FailClose(); return SourceCloseStep::Abort;
        }
        if (KeepNative(candidate.hwnd) ||
            WaitNative(candidate.hwnd, candidate.submitted.folder, candidate.submitted.selection_read)) {
            candidate.submitted.handoff->FailClose();
            UnveilExplorerWindow(candidate.hwnd); veiled_.erase(candidate.hwnd);
            return SourceCloseStep::Abort;
        }
        const HRESULT hr = candidate.browser->Quit();
        candidate.submitted.handoff->diagnostics.Emit(HandoffPhase::QuitResult, static_cast<uint32_t>(hr));
        if (shared_.scope) shared_.scope->evidence->quit = SUCCEEDED(hr);
        if (candidate.submitted.handoff->ConfirmClose(SourceWindowGone(candidate)))
            WakePresentation(*candidate.submitted.handoff);
        else if (FAILED(hr)) {
            candidate.submitted.handoff->FailClose();
            WakePresentation(*candidate.submitted.handoff);
        }
        TraceShellWindows(L"takeover: hwnd=%p Quit hr=0x%08x", candidate.hwnd, static_cast<unsigned>(hr));
        return SourceCloseStep::Close;
    }

    bool Take(Candidate& candidate, ExplorerTakeoverRequest request) {
        if (KeepNative(candidate.hwnd, request) || WaitNative(candidate.hwnd, request.folder, request.selection_read)) return false;
        if (!request.selection_read || shared_.stopping || native_explorer::IsExplicitNativeWindow(candidate.hwnd) ||
            native_explorer::ShouldWaitForRequest(candidate.hwnd, request.folder)) return false;
        const std::weak_ptr<ExplorerWindowTakeover::Shared> weak = shared_.weak_from_this();
        request.handoff = std::make_shared<ExplorerHandoff>(GetTickCount64() + 10000, [weak] {
            const auto shared = weak.lock();
            if (!shared || shared->stopping) return;
            // No watcher/COM capture: requests can outlive the worker's Stop.
            PostThreadMessageW(shared->thread_id, kPollMessage, 0, 0);
        });
        candidate.submitted = request;
        diagnostics::runtime::Event("explorer_candidate_link", {{"candidate_id", candidate.diagnostic_id},
            {"window", reinterpret_cast<uintptr_t>(candidate.hwnd)}, {"source_pid", candidate.process_id},
            {"request_id", request.handoff->diagnostics.id}});
        candidate.context = MakeExplorerOpenContext(request.folder, request.names, request.handoff, request.selection_read);
        if (!candidate.context) { request.handoff->Cancel(); return false; }
        {
            std::lock_guard lock(shared_.handoffs_mutex);
            if (shared_.stopping) { request.handoff->Cancel(); return false; }
            std::erase_if(shared_.handoffs, [](const auto& value) { return value.expired(); });
            shared_.handoffs.push_back(request.handoff);
        }
        // Publish before Submit can wake the UI. Rejection retains the same
        // evidence with a Cancelled handoff, never a second request identity.
        if (shared_.scope) {
            std::lock_guard evidence_lock(shared_.scope->evidence->mutex);
            shared_.scope->evidence->submitted = candidate.submitted;
        }
        // Never acquire the port's queue lock while holding handoffs_mutex:
        // Stop revokes the port before synchronizing close commitment.
        candidate.submitted.handoff->diagnostics.Emit(HandoffPhase::Submitted, 0, candidate.context->names.size());
        if (!shared_.port || !shared_.port->Submit(candidate.context)) {
            candidate.submitted.handoff->diagnostics.Emit(HandoffPhase::Rejected, 1);
            candidate.submitted.handoff->Cancel();
            TraceExplorerScope(shared_.scope, "handoff-submit-rejected", E_FAIL);
            return false;
        }
        TraceExplorerScope(shared_.scope, "handoff-submitted", S_OK);
        if (!shared_.scope) LearnRevealOrigin(candidate.submitted, candidate.diagnostic_id);
        return true;
    }

    void UpdateTimer() {
        const bool busy = !candidates_.empty() || !veiled_.empty();
        if (busy && !timer_) timer_ = SetTimer(nullptr, 0, kPollMs, nullptr);
        else if (!busy && timer_) {
            KillTimer(nullptr, timer_);
            timer_ = 0;
        }
    }

    bool KeepNative(HWND window) {
        const auto query = native_intents_.IsRetained(window);
        if (query.state == native_browse_intent::State::Unavailable) {
            diagnostics::runtime::Event("native_retention_unavailable", {{"window", reinterpret_cast<uintptr_t>(window)}, {"error", query.error}});
            return true;
        }
        return query.state == native_browse_intent::State::Retained;
    }
    bool KeepNative(HWND window, const ExplorerTakeoverRequest& request) {
        const auto query = native_intents_.MatchAndRetain(window, request.folder, request.names, request.selection_read);
        if (query.state == native_browse_intent::State::Unavailable) {
            diagnostics::runtime::Event("native_retention_unavailable", {{"window", reinterpret_cast<uintptr_t>(window)}, {"error", query.error}});
            return true;
        }
        if (query.state == native_browse_intent::State::Retained) {
            diagnostics::runtime::Event("native_fallback_retained", {{"window", reinterpret_cast<uintptr_t>(window)}, {"error", query.error}});
            return true;
        }
        return false;
    }
    bool WaitNative(HWND window, const std::wstring& folder = {}, bool ready = false) {
        const auto query = native_intents_.ShouldWait(window, folder, ready);
        // Uncertain ledger state never authorizes hiding or closing a window.
        return query.state != native_browse_intent::State::None;
    }

    native_browse_intent::Observer native_intents_;
    ExplorerWindowTakeover::Shared& shared_;
    ComPtr<IShellWindows> windows_;
    ComPtr<IConnectionPoint> point_;
    ComPtr<WindowEvents> sink_;
    DWORD advise_cookie_ = 0;
    inline static thread_local Watcher* event_watcher_ = nullptr;
    HWINEVENTHOOK window_events_ = nullptr;
    std::map<HWND, std::shared_ptr<WindowIdentity>> shift_windows_;
    std::set<HWND> known_;
    std::map<HWND, ULONGLONG> veiled_;   // veiled window -> veil tick
    std::vector<Candidate> candidates_;
    UINT_PTR timer_ = 0;
    UINT_PTR discovery_timer_ = 0;
    bool scan_complete_ = true;
};

void WaitForWatcherRetry(const ExplorerWindowTakeover::Shared& shared, DWORD delay) {
    const auto until = GetTickCount64() + delay;
    while (!shared.stopping) {
        const auto now = GetTickCount64();
        if (now >= until) break;
        const auto wait = MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(until - now),
            QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait != WAIT_OBJECT_0) break;
        MSG msg{};
        if (!PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) continue;
        if (msg.message == WM_QUIT) break;
        if (msg.hwnd) DispatchMessageW(&msg);
    }
}

DWORD WINAPI TakeoverThread(void* param) {
    std::shared_ptr<ExplorerWindowTakeover::Shared> shared =
        std::move(*static_cast<std::shared_ptr<ExplorerWindowTakeover::Shared>*>(param));
    delete static_cast<std::shared_ptr<ExplorerWindowTakeover::Shared>*>(param);
    if (shared->scope) shared->scope->evidence->thread_id = GetCurrentThreadId();
    TraceExplorerScope(shared->scope, "thread-initialize");
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    TraceExplorerScope(shared->scope, "thread-initialize-result", initialized);
    if (FAILED(initialized)) {
        diagnostics::runtime::Event("explorer_watcher_start", {{"phase", 5},
            {"hr", static_cast<uint32_t>(initialized)}}, diagnostics::runtime::Level::Error);
        if (shared->scope) shared->scope->evidence->stopped = true;
        return 1;
    }
    {
        MSG msg{};
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // queue before Stop can post
        shared->thread_id = GetCurrentThreadId();
        DWORD delay = 1000;
        for (uint64_t attempt = 1; !shared->stopping; ++attempt) {
            {
                Watcher watcher(*shared);
                if (watcher.Start()) {
                    diagnostics::runtime::Event("explorer_watcher_ready", {{"attempt", attempt}});
                    while (!shared->stopping && GetMessageW(&msg, nullptr, 0, 0) > 0) {
                        if (!msg.hwnd && msg.message == kScanMessage) watcher.Scan(msg.wParam != 0);
                        else if (!msg.hwnd && msg.message == kPollMessage) watcher.Poll();
                        else if (!msg.hwnd && msg.message == WM_TIMER) watcher.Timer(static_cast<UINT_PTR>(msg.wParam));
                        else if (msg.hwnd || msg.message != kStopMessage) {
                            TranslateMessage(&msg);
                            DispatchMessageW(&msg);
                        }
                    }
                    break;
                }
                TraceExplorerScope(shared->scope, "thread-start-failed", E_FAIL);
            }
            if (shared->stopping || shared->scope) break;
            // Sign-in can race the Shell service. Retry the entire baseline
            // snapshot without treating already-open windows as candidates.
            diagnostics::runtime::Event("explorer_watcher_retry", {{"attempt", attempt}, {"delay_ms", delay}});
            WaitForWatcherRetry(*shared, delay);
            delay = (std::min)(delay * 2, 30000ul);
        }
    }
    CoUninitialize();
    if (shared->scope) shared->scope->evidence->stopped = true;
    return 0;
}

} // namespace

ExplorerWindowTakeover::ExplorerWindowTakeover(std::shared_ptr<ExplorerSubmissionPort> port,
                                             std::shared_ptr<const ExplorerSourceScope> scope)
    : shared_(std::make_shared<Shared>()) {
    shared_->scope = std::move(scope);
    if (shared_->scope && !shared_->scope->evidence) return;
    shared_->port = std::move(port);
    auto* param = new std::shared_ptr<Shared>(shared_);
    thread_ = CreateThread(nullptr, 0, &TakeoverThread, param, 0, nullptr);
    if (shared_->scope) {
        shared_->scope->evidence->thread_created = thread_ != nullptr;
        if (!thread_) shared_->scope->evidence->stopped = true;
    }
    if (!thread_) {
        diagnostics::runtime::Event("explorer_watcher_start", {{"phase", 6},
            {"hr", static_cast<uint32_t>(HRESULT_FROM_WIN32(GetLastError()))}}, diagnostics::runtime::Level::Error);
        if (shared_->port) shared_->port->Revoke();
        delete param;
    }
}

ExplorerWindowTakeover::~ExplorerWindowTakeover() { Stop(); }

void ExplorerWindowTakeover::SetTakeThisPc(bool on) { shared_->take_this_pc = on; }

void ExplorerWindowTakeover::Stop(DWORD timeout_ms) {
    if (shared_->port) shared_->port->Revoke();
    if (!thread_) return;
    if (shared_->scope) shared_->scope->evidence->stop_requested = true;
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

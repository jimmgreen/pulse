#include "preview_handler_host.h"
#include "preview_handler_pan.h"
#include "../common/preview_extensions.h"
#include "../common/path_utils.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <propsys.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace pulse::ui {
namespace {

constexpr wchar_t kClassName[] = L"PulsePreviewHandlerHost";
constexpr UINT kOpenDelayMs = 100;
constexpr wchar_t kPreviewHandlerIid[] = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";
constexpr CLSID kQueryAssociations = {
    0xa07034fd, 0x6caa, 0x4954, {0xac, 0x3f, 0x97, 0xa2, 0x72, 0x16, 0xf9, 0x8a}
};
constexpr DWORD kRecallOnOpen = 0x00040000;
constexpr DWORD kRecallOnData = 0x00400000;
constexpr DWORD kPinned = 0x00080000;
constexpr HRESULT kServerExecFailure = static_cast<HRESULT>(0x80080005);

// One apartment hosts the system preview handler and owns the overlay window, so
// a provider that stays inside its open stalls the pane and every later preview.
// An apartment that has not started or finished what it was asked for inside
// this budget is retired: the selection falls back and the next one starts a
// fresh apartment, which keeps a cold provider from taking the pane down with it.
constexpr ULONGLONG kCommandBudgetMs = 3000;

// Slow is not the same as stuck. Opening a preview can mean starting a whole
// application - Office starts Excel the first time an .xlsx is previewed - which
// takes seconds, so the file the pane is still showing keeps its preview for
// that long instead of being retired into the "load failed" placeholder. Only a
// request the pane has moved on from, or a command the apartment never picked
// up, is cut short on the budget above.
constexpr ULONGLONG kSlowOpenBudgetMs = 20000;

// A provider that stalled is not asked again for a while; the thumbnail path
// answers instead, so the pane keeps showing something while the provider warms
// up. The cooldown grows while the same extension keeps stalling.
constexpr ULONGLONG kSlowProviderCooldownMs = 2 * 60 * 1000;
struct SlowProvider {
    ULONGLONG until = 0;
    uint32_t strikes = 0;
};
std::mutex g_slow_provider_mutex;
std::unordered_map<std::wstring, SlowProvider> g_slow_providers;

ULONGLONG CommandBudgetMs() {
#ifdef PULSE_PREVIEW_HANDLER_TESTING
    // Tests observe a retire without waiting for the production budget.
    wchar_t text[32]{};
    if (GetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_OPEN_BUDGET_MS", text,
                                ARRAYSIZE(text)) > 0) {
        const unsigned long long value = std::wcstoull(text, nullptr, 10);
        if (value > 0) return static_cast<ULONGLONG>(value);
    }
#endif
    return kCommandBudgetMs;
}

ULONGLONG SlowOpenBudgetMs() {
#ifdef PULSE_PREVIEW_HANDLER_TESTING
    // Tests observe the slow-open give-up without waiting for the real budget.
    wchar_t text[32]{};
    if (GetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_SLOW_OPEN_BUDGET_MS", text,
                                ARRAYSIZE(text)) > 0) {
        const unsigned long long value = std::wcstoull(text, nullptr, 10);
        if (value > 0) return static_cast<ULONGLONG>(value);
    }
#endif
    return kSlowOpenBudgetMs;
}

void NoteStalledProvider(const std::wstring& extension) {
    if (extension.empty()) return;
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_slow_provider_mutex);
    SlowProvider& provider = g_slow_providers[extension];
    provider.strikes = provider.until > now ? provider.strikes + 1 : 1;
    provider.until = now + kSlowProviderCooldownMs * provider.strikes;
}

bool ProviderCoolingDown(const std::wstring& extension) {
    if (extension.empty()) return false;
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_slow_provider_mutex);
    const auto it = g_slow_providers.find(extension);
    if (it == g_slow_providers.end()) return false;
    if (it->second.until <= now) {
        g_slow_providers.erase(it);
        return false;
    }
    return true;
}

void ResetSlowProvidersForTest() {
    std::lock_guard<std::mutex> lock(g_slow_provider_mutex);
    g_slow_providers.clear();
}

// A provider that answered - however long it took, and whether or not the pane
// still wanted the preview - is working, so it is not kept away from the pane:
// the open it was retired for can still come back, and the next selection should
// get the handler rather than the placeholder.
void ClearSlowProvider(const std::wstring& extension) {
    if (extension.empty()) return;
    std::lock_guard<std::mutex> lock(g_slow_provider_mutex);
    g_slow_providers.erase(extension);
}

// The overlay belongs to the preview apartment but is owned by our window, and
// that apartment may be stuck inside a provider call. Hiding the position
// change is posted, and the owner is dropped, so neither hides an empty box on
// screen forever nor makes window destruction wait for the stuck thread.
void DetachOverlay(HWND overlay) {
    if (!overlay) return;
    SetWindowPos(overlay, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW |
                 SWP_ASYNCWINDOWPOS);
    SetWindowLongPtrW(overlay, GWLP_HWNDPARENT, 0);
}

struct ClsidHash {
    size_t operator()(const CLSID& c) const noexcept {
        const auto* p = reinterpret_cast<const uint64_t*>(&c);
        return static_cast<size_t>(p[0] ^ p[1]);
    }
};
struct ClsidEq {
    bool operator()(const CLSID& a, const CLSID& b) const noexcept {
        return IsEqualCLSID(a, b);
    }
};

thread_local std::unordered_map<CLSID, ComPtr<IClassFactory>, ClsidHash, ClsidEq> g_factories;
std::mutex g_association_mutex;
std::once_flag g_register_class_once;
#ifdef PULSE_PREVIEW_HANDLER_TESTING
std::atomic<uint32_t> g_test_open_attempts{0};
// PlaceOverlay entries and returns. They differ only while the window manager
// is inside the provider's window, which is what makes a preview trail its
// owner during a drag.
std::atomic<uint32_t> g_test_place_calls{0};
std::atomic<uint32_t> g_test_place_done{0};
#endif

std::wstring ShellPath(const std::wstring& path) {
    return pulse::path::StripExtendedPathPrefix(path);
}

std::wstring ExtensionOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
        return {};
    std::wstring extension = path.substr(dot);
    for (wchar_t& c : extension) c = static_cast<wchar_t>(std::towlower(c));
    return extension;
}

bool IsOneOf(std::wstring_view extension, std::initializer_list<std::wstring_view> values) {
    return std::find(values.begin(), values.end(), extension) != values.end();
}

bool IsNativePreviewExtension(std::wstring_view extension) {
    return pulse::preview::IsNativeExtension(extension);
}

bool IsOfflinePlaceholder(DWORD attrs) {
    return (attrs & (kRecallOnOpen | kRecallOnData)) && !(attrs & kPinned);
}

bool FindPreviewHandlerClsid(const std::wstring& extension, CLSID& clsid) {
    static std::unordered_map<std::wstring, CLSID> cache;
    // A lookup that found nothing is remembered, but only for a while: the shell
    // can fail this query while it is busy with something else, and carrying
    // that answer for the whole session leaves the type unpreviewable until the
    // application restarts.
    constexpr ULONGLONG kNegativeTtlMs = 30 * 1000;
    static std::unordered_map<std::wstring, ULONGLONG> negative;
    {
        std::lock_guard<std::mutex> lock(g_association_mutex);
        if (auto it = cache.find(extension); it != cache.end()) {
            clsid = it->second;
            return true;
        }
        if (auto it = negative.find(extension); it != negative.end()) {
            if (GetTickCount64() - it->second < kNegativeTtlMs) return false;
            negative.erase(it);
        }
    }

    // The shell lookup runs outside the lock. This is reached from the paint
    // path too, and a lookup queued behind another thread's slow call used to
    // freeze the window along with it. Resolving the same extension twice is
    // harmless.
    ComPtr<IQueryAssociations> assoc;
    HRESULT hr = AssocCreate(kQueryAssociations, IID_PPV_ARGS(&assoc));
    if (FAILED(hr)) {
        return false;
    }
    hr = assoc->Init(ASSOCF_INIT_DEFAULTTOSTAR, extension.c_str(), nullptr, nullptr);
    if (FAILED(hr)) {
        return false;
    }
    wchar_t guid[64]{};
    DWORD chars = ARRAYSIZE(guid);
    CLSID resolved{};
    const bool found =
        SUCCEEDED(assoc->GetString(ASSOCF_NOTRUNCATE, ASSOCSTR_SHELLEXTENSION,
                                   kPreviewHandlerIid, guid, &chars)) &&
        SUCCEEDED(CLSIDFromString(guid, &resolved));
    {
        std::lock_guard<std::mutex> lock(g_association_mutex);
        if (found) cache[extension] = resolved;
        else negative[extension] = GetTickCount64();
    }
    if (!found) {
        return false;
    }
    clsid = resolved;
    return true;
}

ComPtr<IClassFactory> FactoryFor(const CLSID& clsid) {
    if (auto it = g_factories.find(clsid); it != g_factories.end()) {
        return it->second;
    }
    ComPtr<IClassFactory> factory;
    HRESULT hr = CoGetClassObject(clsid, CLSCTX_LOCAL_SERVER, nullptr,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) return {};
    g_factories[clsid] = factory;
    return factory;
}

void EvictFactory(const CLSID& clsid) {
    g_factories.erase(clsid);
}

ComPtr<IUnknown> CreateHandler(const CLSID& clsid) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        ComPtr<IClassFactory> factory = FactoryFor(clsid);
        if (factory) {
            ComPtr<IUnknown> unknown;
            const HRESULT hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&unknown));
            if (SUCCEEDED(hr) && unknown) return unknown;
            EvictFactory(clsid);
            if (hr != kServerExecFailure && attempt == 0) break;
            continue;
        }
        ComPtr<IUnknown> unknown;
        HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER,
                                      IID_PPV_ARGS(&unknown));
        if (SUCCEEDED(hr) && unknown) return unknown;
        if (hr != kServerExecFailure) break;
    }
    return {};
}

class PreviewFrame final : public IPreviewHandlerFrame {
public:
    explicit PreviewFrame(HWND hwnd) : hwnd_(hwnd) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPreviewHandlerFrame) {
            *ppv = static_cast<IPreviewHandlerFrame*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refs_));
    }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG refs = static_cast<ULONG>(InterlockedDecrement(&refs_));
        if (refs == 0) delete this;
        return refs;
    }
    IFACEMETHODIMP GetWindowContext(PREVIEWHANDLERFRAMEINFO* info) override {
        if (!info) return E_POINTER;
        info->haccel = nullptr;
        info->cAccelEntries = 0;
        return S_OK;
    }
    IFACEMETHODIMP TranslateAccelerator(MSG*) override { return S_FALSE; }

private:
    LONG refs_ = 1;
    HWND hwnd_ = nullptr;
};

bool InitWithStream(IUnknown* handler, const std::wstring& path, IUnknown** kept_stream) {
    ComPtr<IInitializeWithStream> init;
    HRESULT hr = handler->QueryInterface(IID_PPV_ARGS(&init));
    if (FAILED(hr)) return false;
    ComPtr<IStream> stream;
    hr = SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE,
                                nullptr, &stream);
    if (FAILED(hr)) return false;
    hr = init->Initialize(stream.Get(), STGM_READ);
    if (hr == E_NOTIMPL) return false;
    if (FAILED(hr)) return false;
    *kept_stream = stream.Detach();
    return true;
}

bool InitWithItem(IUnknown* handler, const std::wstring& path) {
    ComPtr<IInitializeWithItem> init;
    HRESULT hr = handler->QueryInterface(IID_PPV_ARGS(&init));
    if (FAILED(hr)) return false;
    ComPtr<IShellItem> item;
    hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr)) return false;
    hr = init->Initialize(item.Get(), STGM_READ);
    return hr != E_NOTIMPL && SUCCEEDED(hr);
}

bool InitWithFile(IUnknown* handler, const std::wstring& path) {
    ComPtr<IInitializeWithFile> init;
    HRESULT hr = handler->QueryInterface(IID_PPV_ARGS(&init));
    if (FAILED(hr)) return false;
    hr = init->Initialize(path.c_str(), STGM_READ);
    return hr != E_NOTIMPL && SUCCEEDED(hr);
}

void RegisterClassOnce() {
    std::call_once(g_register_class_once, [] {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = PreviewHandlerHost::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
    });
}

} // namespace

bool PreviewHandlerHost::CanHost(const std::wstring& path) {
    const std::wstring extension = ExtensionOf(path);
    if (extension.empty() || IsNativePreviewExtension(extension)) return false;
    // A provider that just stalled is left alone until it has warmed up: the
    // caller then takes the thumbnail path, which answers on its own process.
    if (ProviderCoolingDown(extension)) return false;
    CLSID clsid{};
    return FindPreviewHandlerClsid(extension, clsid);
}

struct PreviewHandlerHost::WorkerState {
    struct Command {
        HWND owner = nullptr;
        HWND notify = nullptr;
        RECT bounds{};
        std::wstring path;
        std::wstring identity;
        DWORD attrs = 0;
        bool enabled = false;
        bool app_active = true;
        bool immediate = false;
    };

    ~WorkerState() {
        if (thread) CloseHandle(thread);
        if (wake) CloseHandle(wake);
    }

    bool EnsureWindow() {
        if (hwnd && IsWindow(hwnd)) return true;
        hwnd = nullptr;
        overlay.store(nullptr, std::memory_order_release);
        if (!owner || !IsWindow(owner)) return false;
        RegisterClassOnce();
        hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"",
            WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 0, 0, owner, nullptr, GetModuleHandleW(nullptr), this);
        // Published for the owner, which may need to hide it while this thread
        // is inside a provider call. Cleared on WM_DESTROY so it cannot go stale.
        overlay.store(hwnd, std::memory_order_release);
        return hwnd != nullptr;
    }

    bool OverlayOwnsForeground() const {
        HWND foreground = GetForegroundWindow();
        if (!foreground || !hwnd) return false;
        if (foreground == hwnd || IsChild(hwnd, foreground) ||
            GetAncestor(foreground, GA_ROOT) == hwnd) return true;
        for (HWND walk = foreground; walk; walk = GetWindow(walk, GW_OWNER)) {
            if (walk == hwnd) return true;
        }
        return false;
    }

    void HideWindow() {
        pan.Disable();
        if (hwnd) {
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
        }
        shown = false;
        placed_x = INT_MIN;
        placed_y = INT_MIN;
    }

    void PlaceOverlay() {
        if (!hwnd || !owner) return;
#ifdef PULSE_PREVIEW_HANDLER_TESTING
        g_test_place_calls.fetch_add(1, std::memory_order_relaxed);
        struct PlaceDone {
            ~PlaceDone() { g_test_place_done.fetch_add(1, std::memory_order_relaxed); }
        } place_done;
#endif
        POINT origin{bounds.left, bounds.top};
        if (!ClientToScreen(owner, &origin)) return;
        const int width = std::max(1L, bounds.right - bounds.left);
        const int height = std::max(1L, bounds.bottom - bounds.top);
        if (shown && !app_active && !OverlayOwnsForeground()) {
            pan.Disable();
            SetWindowPos(hwnd, HWND_NOTOPMOST, origin.x, origin.y, width, height,
                         SWP_NOACTIVATE | SWP_HIDEWINDOW);
            return;
        }
        const bool size_changed = width != placed_w || height != placed_h;
        const bool moved = origin.x != placed_x || origin.y != placed_y || size_changed;
        if (!moved && shown && IsWindowVisible(hwnd)) {
            if (handler) pan.Enable(hwnd);
            return;
        }
        placed_x = origin.x;
        placed_y = origin.y;
        placed_w = width;
        placed_h = height;
        pan.Disable();
        // A move keeps the z-order it already has. Re-asserting HWND_TOPMOST on
        // every step of a window drag only makes the window manager re-evaluate
        // the topmost band, with the provider's window (another process) inside.
        const bool raise = !shown || size_changed;
        SetWindowPos(hwnd, raise ? HWND_TOPMOST : nullptr, origin.x, origin.y, width, height,
                     SWP_NOACTIVATE | (raise ? 0 : SWP_NOZORDER) |
                         (shown ? SWP_SHOWWINDOW : SWP_NOREDRAW));
        if (handler && shown && size_changed) {
            ComPtr<IPreviewHandler> preview;
            if (SUCCEEDED(handler->QueryInterface(IID_PPV_ARGS(&preview)))) {
                RECT client{0, 0, width, height};
                preview->SetRect(&client);
            }
        }
        if (handler && shown) pan.Enable(hwnd);
    }

    void Unload() {
        pan.Disable();
        if (handler) {
            ComPtr<IPreviewHandler> preview;
            if (SUCCEEDED(handler->QueryInterface(IID_PPV_ARGS(&preview))))
                preview->Unload();
            handler->Release();
            handler = nullptr;
        }
        if (stream) {
            stream->Release();
            stream = nullptr;
        }
        if (site) {
            site->Release();
            site = nullptr;
        }
        shown = false;
    }

    bool OpenCurrent() {
        Unload();
        if (path.empty() || !EnsureWindow()) return false;
        CLSID clsid{};
        if (!FindPreviewHandlerClsid(ExtensionOf(path), clsid)) return false;
        ComPtr<IUnknown> unknown = CreateHandler(clsid);
        if (!unknown) return false;

        site = new PreviewFrame(hwnd);
        ComPtr<IObjectWithSite> object_with_site;
        if (SUCCEEDED(unknown.As(&object_with_site)) && object_with_site)
            object_with_site->SetSite(site);

        const std::wstring open_path = ShellPath(path);
        const bool file_ok = InitWithFile(unknown.Get(), open_path);
        const bool item_ok = !file_ok && InitWithItem(unknown.Get(), open_path);
        const bool stream_ok = !file_ok && !item_ok &&
            InitWithStream(unknown.Get(), open_path, &stream);
        ComPtr<IPreviewHandler> preview;
        if (!(file_ok || item_ok || stream_ok) || FAILED(unknown.As(&preview)) || !preview) {
            Unload();
            return false;
        }
        handler = unknown.Detach();
        shown = true;
        PlaceOverlay();
        pan.Disable();
        RECT client{};
        GetClientRect(hwnd, &client);
        if (client.right <= client.left || client.bottom <= client.top ||
            FAILED(preview->SetWindow(hwnd, &client))) {
            Unload();
            HideWindow();
            return false;
        }
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        if (FAILED(preview->DoPreview())) {
            Unload();
            HideWindow();
            return false;
        }
        preview->SetRect(&client);
        // Office handlers may paint directly into the host. Wake their child
        // windows without erasing the pixels DoPreview has already produced.
        RedrawWindow(hwnd, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_NOERASE);
        pan.Enable(hwnd);
        return true;
    }

    // Callers hold mutex. Bumping the version makes the apartment re-read the
    // latest command, and the tick lets the owner tell how long it has waited.
    void BumpLocked() {
        ++command_version;
        command_tick = GetTickCount64();
    }

    std::mutex mutex;
    Command command;
    uint64_t command_version = 0;
    ULONGLONG command_tick = 0;
    bool stop = false;
    HANDLE wake = nullptr;
    HANDLE thread = nullptr;
    std::atomic<State> state{State::Idle};
    // Read by the owner while this thread is inside a provider call.
    std::atomic<uint64_t> applied_version{0};
    std::atomic<uint64_t> opens_started{0};
    std::atomic<uint64_t> opens_finished{0};
    std::atomic<ULONGLONG> open_started_tick{0};
    std::atomic<HWND> overlay{nullptr};

    HWND hwnd = nullptr;
    HWND owner = nullptr;
    HWND notify = nullptr;
    RECT bounds{};
    std::wstring path;
    std::wstring identity;
    std::wstring pending_identity;
    // The request the apartment is opening right now. Written by the worker and
    // read by the owner's watchdog, so both sides go through mutex. Empty while
    // the apartment has nothing in flight.
    std::wstring working_identity;
    DWORD attrs = 0;
    bool app_active = true;
    bool shown = false;
    int placed_x = INT_MIN;
    int placed_y = INT_MIN;
    int placed_w = 0;
    int placed_h = 0;
    IUnknown* handler = nullptr;
    IUnknown* stream = nullptr;
    IUnknown* site = nullptr;
    PreviewHandlerPan pan;
};

PreviewHandlerHost::PreviewHandlerHost() = default;

PreviewHandlerHost::~PreviewHandlerHost() {
    auto worker = worker_;
    worker_.reset();
    if (worker) {
        DetachOverlay(worker->overlay.load(std::memory_order_acquire));
        {
            std::lock_guard<std::mutex> lock(worker->mutex);
            worker->stop = true;
        }
        SetEvent(worker->wake);
        if (worker->thread) WaitForSingleObject(worker->thread, 100);
    }
    // Retired apartments stop themselves as soon as the provider call they are
    // inside returns; the references they hold keep them alive until then.
    for (const auto& retired : retired_) {
        DetachOverlay(retired->overlay.load(std::memory_order_acquire));
    }
    retired_.clear();
}

void PreviewHandlerHost::EnsureWorker() {
    if (worker_) return;
    auto worker = std::make_shared<WorkerState>();
    worker->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!worker->wake) return;
    auto* argument = new std::shared_ptr<WorkerState>(worker);
    worker->thread = CreateThread(nullptr, 0, WorkerMain, argument, 0, nullptr);
    if (!worker->thread) {
        delete argument;
        return;
    }
    worker_ = std::move(worker);
}

PreviewHandlerHost::State PreviewHandlerHost::state() const {
    return worker_ ? worker_->state.load(std::memory_order_acquire) : State::Idle;
}

void PreviewHandlerHost::SetNotifyWindow(HWND hwnd) {
    notify_ = hwnd;
    if (!hwnd) {
        Reset();
        return;
    }
    if (!worker_) return;
    {
        std::lock_guard<std::mutex> lock(worker_->mutex);
        worker_->command.notify = hwnd;
        worker_->BumpLocked();
    }
    SetEvent(worker_->wake);
}

void PreviewHandlerHost::Publish(bool enabled, HWND owner, const RECT& bounds,
                                 const std::wstring& path, const std::wstring& identity,
                                 DWORD attrs, bool immediate) {
    // A hide must not spawn an apartment of its own.
    if (!enabled && !worker_) return;
    EnsureWorker();
    if (!worker_) return;
    WorkerState::Command command;
    command.owner = owner;
    command.notify = notify_;
    command.bounds = bounds;
    command.path = path;
    command.identity = identity;
    command.attrs = attrs;
    command.enabled = enabled;
    command.app_active = app_active_;
    command.immediate = immediate;
    {
        std::lock_guard<std::mutex> lock(worker_->mutex);
        worker_->command = std::move(command);
        worker_->BumpLocked();
    }
    SetEvent(worker_->wake);
}

void PreviewHandlerHost::Hide() {
    if (!worker_) {
        last_enabled_ = false;
        last_identity_.clear();
        return;
    }
    RECT empty{};
    worker_->state.store(State::Idle, std::memory_order_release);
    Publish(false, nullptr, empty, {}, {}, 0, false);
    last_enabled_ = false;
    last_identity_.clear();
}

void PreviewHandlerHost::Reset() {
    Hide();
    last_owner_ = nullptr;
    last_bounds_ = {};
}

void PreviewHandlerHost::Reposition() {
    auto worker = worker_;
    if (!worker || !last_enabled_) return;
    {
        std::lock_guard<std::mutex> lock(worker->mutex);
        if (!worker->command.enabled) return;
        worker->BumpLocked();
    }
    SetEvent(worker->wake);
}

void PreviewHandlerHost::NotifyAppActivate(bool active) {
    if (app_active_ == active) return;
    app_active_ = active;
    if (!worker_) return;
    {
        std::lock_guard<std::mutex> lock(worker_->mutex);
        worker_->command.app_active = active;
        worker_->BumpLocked();
    }
    SetEvent(worker_->wake);
}

void PreviewHandlerHost::Sync(HWND owner, const D2D1_RECT_F& bounds, const std::wstring& path,
                              DWORD attrs, uint64_t generation, uint64_t modified, uint64_t size,
                              bool dark, const D2D1_COLOR_F& bg, const D2D1_COLOR_F& fg,
                              bool enabled, bool immediate) {
    // Called on every paint: an apartment that stopped answering is retired
    // first, and this frame falls through to the caller's fallback preview.
    const std::wstring identity = path + L"\n" + std::to_wstring(generation) + L":" +
        std::to_wstring(modified) + L":" + std::to_wstring(size);
    if (RetireStalledApartment(identity, enabled)) return;
    (void)dark;
    (void)bg;
    (void)fg;
    RECT target{};
    target.left = static_cast<LONG>(std::lround(bounds.left));
    target.top = static_cast<LONG>(std::lround(bounds.top));
    target.right = static_cast<LONG>(std::lround(bounds.right));
    target.bottom = static_cast<LONG>(std::lround(bounds.bottom));

    const bool usable = enabled && owner && IsWindow(owner) && !IsIconic(owner) &&
        IsWindowVisible(owner) && !IsOfflinePlaceholder(attrs) &&
        target.right > target.left + 8 && target.bottom > target.top + 8;
    if (!usable) {
        if (last_enabled_) Hide();
        return;
    }

    const bool same_bounds = EqualRect(&target, &last_bounds_) != FALSE;
    if (last_enabled_ && owner == last_owner_ && identity == last_identity_ && same_bounds)
        return;
    const bool new_content = !last_enabled_ || identity != last_identity_;
    EnsureWorker();
    if (!worker_) return;
    last_enabled_ = true;
    last_owner_ = owner;
    last_bounds_ = target;
    last_identity_ = identity;
    last_path_ = path;
    if (new_content) worker_->state.store(State::Loading, std::memory_order_release);
    Publish(true, owner, target, path, identity, attrs, immediate);
}

void PreviewHandlerHost::ReapRetired() {
    for (auto it = retired_.begin(); it != retired_.end();) {
        const HANDLE thread = (*it)->thread;
        if (!thread || WaitForSingleObject(thread, 0) == WAIT_OBJECT_0) it = retired_.erase(it);
        else ++it;
    }
}

bool PreviewHandlerHost::RetireStalledApartment(const std::wstring& requested_identity,
                                               bool requested) {
    ReapRetired();
    const auto worker = worker_;
    if (!worker) return false;
    const ULONGLONG now = GetTickCount64();

    // The apartment serves one request at a time, so a request the pane has
    // moved on from must not wait for the provider that is still busy with the
    // previous one. Only the request the apartment is working on right now - the
    // one the pane is therefore still showing - keeps the longer budget above.
    bool superseded = !requested;
    {
        std::lock_guard<std::mutex> lock(worker->mutex);
        if (worker->working_identity != requested_identity) superseded = true;
    }
    const ULONGLONG budget = superseded ? CommandBudgetMs() : SlowOpenBudgetMs();

    // An apartment is stuck when either the provider open it started never came
    // back, or a command sent to it was never applied.
    bool stalled = worker->opens_started.load(std::memory_order_acquire) >
            worker->opens_finished.load(std::memory_order_acquire) &&
        now - worker->open_started_tick.load(std::memory_order_acquire) >= budget;
    ULONGLONG sent = 0;
    {
        std::lock_guard<std::mutex> lock(worker->mutex);
        if (!stalled && worker->applied_version.load(std::memory_order_relaxed) !=
                            worker->command_version) {
            stalled = true;
            sent = worker->command_tick;
        }
    }
    if (!stalled) return false;
    if (sent && now - sent < budget) return false;

    // Remember the provider, so the pane asks the thumbnail path instead of
    // queueing behind the same provider on a fresh apartment.
    NoteStalledProvider(ExtensionOf(last_path_));
    DetachOverlay(worker->overlay.load(std::memory_order_acquire));
    {
        std::lock_guard<std::mutex> lock(worker->mutex);
        worker->stop = true;
    }
    SetEvent(worker->wake);
    worker_.reset();
    // The next paint re-reads CanHost, which now reports the cooldown, and
    // publishes again only once the provider is allowed back. Ask for that frame
    // now: this one is already inside the caller's paint and still holds the
    // handler path it chose before the retire.
    const HWND wake = notify_ ? notify_ : last_owner_;
    if (wake && IsWindow(wake)) InvalidateRect(wake, nullptr, FALSE);
    last_enabled_ = false;
    last_identity_.clear();
    last_owner_ = nullptr;
    retired_.push_back(std::move(worker));
    return true;
}

LRESULT CALLBACK PreviewHandlerHost::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WorkerState* self = reinterpret_cast<WorkerState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<WorkerState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    if (self->pan.HandleMessage(msg, wParam, lParam)) return 1;

    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND: {
        // Some Office previewers render into this host instead of an opaque
        // child. Erasing after DoPreview replaces valid content with white
        // until the next user input forces the handler to repaint.
        if (self->handler) return 1;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(reinterpret_cast<HDC>(wParam), &rc, brush);
        DeleteObject(brush);
        return 1;
    }
    case WM_DESTROY:
        if (self->hwnd == hwnd) self->hwnd = nullptr;
        self->overlay.store(nullptr, std::memory_order_release);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

DWORD WINAPI PreviewHandlerHost::WorkerMain(void* parameter) {
    std::unique_ptr<std::shared_ptr<WorkerState>> argument(
        static_cast<std::shared_ptr<WorkerState>*>(parameter));
    std::shared_ptr<WorkerState> self = *argument;
    const HRESULT com_result = CoInitializeEx(nullptr,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com_result)) {
        self->state.store(State::Failed, std::memory_order_release);
        return 0;
    }

    MSG message{};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    uint64_t applied_version = 0;
    std::optional<std::chrono::steady_clock::time_point> open_due;
    for (;;) {
        WorkerState::Command command;
        uint64_t version = 0;
        bool stop = false;
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            stop = self->stop;
            version = self->command_version;
            command = self->command;
        }
        if (stop) break;

        if (version != applied_version) {
            if (command.owner != self->owner && self->hwnd) {
                self->Unload();
                self->HideWindow();
                DestroyWindow(self->hwnd);
                self->hwnd = nullptr;
            }
            self->owner = command.owner;
            self->notify = command.notify;
            self->bounds = command.bounds;
            self->path = command.path;
            self->attrs = command.attrs;
            self->app_active = command.app_active;
            if (!command.enabled) {
                self->Unload();
                self->HideWindow();
                self->identity.clear();
                self->pending_identity.clear();
                {
                    std::lock_guard<std::mutex> lock(self->mutex);
                    self->working_identity.clear();
                }
                open_due.reset();
                self->state.store(State::Idle, std::memory_order_release);
            } else if (command.identity == self->identity) {
                self->pending_identity.clear();
                open_due.reset();
                self->shown = self->handler != nullptr;
                self->state.store(self->shown ? State::Shown : State::Failed,
                                  std::memory_order_release);
                self->PlaceOverlay();
            } else if (command.identity != self->pending_identity) {
                self->HideWindow();
                self->pending_identity = command.identity;
                {
                    std::lock_guard<std::mutex> lock(self->mutex);
                    self->working_identity = command.identity;
                }
                open_due = std::chrono::steady_clock::now() + std::chrono::milliseconds(
                    command.immediate ? 0 : kOpenDelayMs);
                self->state.store(State::Loading, std::memory_order_release);
            } else {
                self->PlaceOverlay();
            }
            applied_version = version;
            self->applied_version.store(version, std::memory_order_release);
        }

        if (open_due && std::chrono::steady_clock::now() >= *open_due) {
            const std::wstring opening_identity = self->pending_identity;
            // Bracket the whole open, provider calls included, so the owner can
            // tell that this apartment is busy and since when.
            self->open_started_tick.store(GetTickCount64(), std::memory_order_release);
            self->opens_started.fetch_add(1, std::memory_order_release);
#ifdef PULSE_PREVIEW_HANDLER_TESTING
            g_test_open_attempts.fetch_add(1, std::memory_order_relaxed);
            wchar_t delay_text[16]{};
            if (GetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_TEST_DELAY_MS",
                                        delay_text, ARRAYSIZE(delay_text)) > 0) {
                const int delay = _wtoi(delay_text);
                if (delay > 0) Sleep(static_cast<DWORD>(std::min(delay, 10000)));
            }
#endif
            const bool opened = self->OpenCurrent();
            self->opens_finished.fetch_add(1, std::memory_order_release);
            if (opened) ClearSlowProvider(ExtensionOf(self->path));
            bool current = false;
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                self->working_identity.clear();
                current = !self->stop && self->command.enabled &&
                    self->command.identity == opening_identity;
            }
            if (current) {
                self->identity = opening_identity;
                self->pending_identity.clear();
                self->state.store(opened ? State::Shown : State::Failed,
                                  std::memory_order_release);
                if (!opened) self->HideWindow();
                if (self->notify) InvalidateRect(self->notify, nullptr, FALSE);
            } else {
                self->Unload();
                self->HideWindow();
            }
            open_due.reset();
            continue;
        }

        DWORD timeout = INFINITE;
        if (open_due) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                *open_due - std::chrono::steady_clock::now()).count();
            timeout = static_cast<DWORD>(std::clamp<int64_t>(remaining, 0, 1000));
        }
        const DWORD wait = MsgWaitForMultipleObjectsEx(
            1, &self->wake, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0 + 1) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }

    self->Unload();
    self->HideWindow();
    if (self->hwnd) DestroyWindow(self->hwnd);
    self->hwnd = nullptr;
    self->state.store(State::Idle, std::memory_order_release);
    g_factories.clear();
    CoUninitialize();
    return 0;
}

#ifdef PULSE_PREVIEW_HANDLER_TESTING
void ResetPreviewHandlerOpenAttemptsForTest() {
    g_test_open_attempts.store(0, std::memory_order_relaxed);
}

uint32_t PreviewHandlerOpenAttemptsForTest() {
    return g_test_open_attempts.load(std::memory_order_relaxed);
}

void ResetOverlayPlaceCountsForTest() {
    g_test_place_calls.store(0, std::memory_order_relaxed);
    g_test_place_done.store(0, std::memory_order_relaxed);
}

uint32_t OverlayPlaceCallsForTest() {
    return g_test_place_calls.load(std::memory_order_relaxed);
}

uint32_t OverlayPlaceDoneForTest() {
    return g_test_place_done.load(std::memory_order_relaxed);
}

void ResetSlowPreviewProvidersForTest() {
    ResetSlowProvidersForTest();
}

HWND PreviewHandlerHost::overlay_window_for_test() const {
    return worker_ ? worker_->overlay.load(std::memory_order_acquire) : nullptr;
}

bool PreviewHandlerCanActivateIsolatedForTest(const std::wstring& path) {
    CLSID clsid{};
    if (!FindPreviewHandlerClsid(ExtensionOf(path), clsid)) return false;
    ComPtr<IClassFactory> factory;
    return SUCCEEDED(CoGetClassObject(clsid, CLSCTX_LOCAL_SERVER, nullptr,
                                     IID_PPV_ARGS(&factory))) && factory;
}
#endif

} // namespace pulse::ui

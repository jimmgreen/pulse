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
    if (extension.empty()) {
        return false;
    }
    static std::unordered_map<std::wstring, CLSID> cache;
    static std::unordered_map<std::wstring, bool> negative;
    std::lock_guard<std::mutex> lock(g_association_mutex);
    if (auto it = cache.find(extension); it != cache.end()) {
        clsid = it->second;
        return true;
    }
    if (negative.contains(extension)) {
        return false;
    }

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
    hr = assoc->GetString(ASSOCF_NOTRUNCATE, ASSOCSTR_SHELLEXTENSION,
                          kPreviewHandlerIid, guid, &chars);
    if (FAILED(hr)) {
        negative[extension] = true;
        return false;
    }
    hr = CLSIDFromString(guid, &clsid);
    if (FAILED(hr)) {
        negative[extension] = true;
        return false;
    }
    cache[extension] = clsid;
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
        if (!owner || !IsWindow(owner)) return false;
        RegisterClassOnce();
        hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"",
            WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 0, 0, owner, nullptr, GetModuleHandleW(nullptr), this);
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
        SetWindowPos(hwnd, HWND_TOPMOST, origin.x, origin.y, width, height,
                     SWP_NOACTIVATE | (shown ? SWP_SHOWWINDOW : SWP_NOREDRAW));
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

    std::mutex mutex;
    Command command;
    uint64_t command_version = 0;
    bool stop = false;
    HANDLE wake = nullptr;
    HANDLE thread = nullptr;
    std::atomic<State> state{State::Idle};

    HWND hwnd = nullptr;
    HWND owner = nullptr;
    HWND notify = nullptr;
    RECT bounds{};
    std::wstring path;
    std::wstring identity;
    std::wstring pending_identity;
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
    if (!worker) return;
    {
        std::lock_guard<std::mutex> lock(worker->mutex);
        worker->stop = true;
    }
    SetEvent(worker->wake);
    if (worker->thread) WaitForSingleObject(worker->thread, 100);
    worker_.reset();
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
        ++worker_->command_version;
    }
    SetEvent(worker_->wake);
}

void PreviewHandlerHost::Publish(bool enabled, HWND owner, const RECT& bounds,
                                 const std::wstring& path, const std::wstring& identity,
                                 DWORD attrs, bool immediate) {
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
        ++worker_->command_version;
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
        ++worker->command_version;
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
        ++worker_->command_version;
    }
    SetEvent(worker_->wake);
}

void PreviewHandlerHost::Sync(HWND owner, const D2D1_RECT_F& bounds, const std::wstring& path,
                              DWORD attrs, uint64_t generation, uint64_t modified, uint64_t size,
                              bool dark, const D2D1_COLOR_F& bg, const D2D1_COLOR_F& fg,
                              bool enabled, bool immediate) {
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

    const std::wstring identity = path + L"\n" + std::to_wstring(generation) + L":" +
        std::to_wstring(modified) + L":" + std::to_wstring(size);
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
    if (new_content) worker_->state.store(State::Loading, std::memory_order_release);
    Publish(true, owner, target, path, identity, attrs, immediate);
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
                open_due = std::chrono::steady_clock::now() + std::chrono::milliseconds(
                    command.immediate ? 0 : kOpenDelayMs);
                self->state.store(State::Loading, std::memory_order_release);
            } else {
                self->PlaceOverlay();
            }
            applied_version = version;
        }

        if (open_due && std::chrono::steady_clock::now() >= *open_due) {
            const std::wstring opening_identity = self->pending_identity;
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
            bool current = false;
            {
                std::lock_guard<std::mutex> lock(self->mutex);
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

bool PreviewHandlerCanActivateIsolatedForTest(const std::wstring& path) {
    CLSID clsid{};
    if (!FindPreviewHandlerClsid(ExtensionOf(path), clsid)) return false;
    ComPtr<IClassFactory> factory;
    return SUCCEEDED(CoGetClassObject(clsid, CLSCTX_LOCAL_SERVER, nullptr,
                                     IID_PPV_ARGS(&factory))) && factory;
}
#endif

} // namespace pulse::ui

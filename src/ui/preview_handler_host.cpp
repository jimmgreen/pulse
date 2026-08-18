#include "preview_handler_host.h"
#include "../common/path_utils.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <propsys.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string_view>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace pulse::ui {
namespace {

constexpr wchar_t kClassName[] = L"PulsePreviewHandlerHost";
constexpr UINT kOpenTimer = 1;
constexpr UINT kFixupTimer = 2;
constexpr UINT kOpenDelayMs = 100;
constexpr UINT kFixupDelayMs = 50;
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

std::unordered_map<CLSID, ComPtr<IClassFactory>, ClsidHash, ClsidEq> g_factories;
bool g_class_registered = false;

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
    return IsOneOf(extension, {
        L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".tif", L".tiff",
        L".webp", L".heic", L".ico",
        L".txt", L".md", L".log", L".json", L".xml", L".yaml", L".yml",
        L".ini", L".cfg", L".conf", L".csv", L".tsv", L".cpp", L".c",
        L".h", L".hpp", L".cc", L".cxx", L".cs", L".java", L".js",
        L".jsx", L".ts", L".tsx", L".py", L".rs", L".go", L".php",
        L".html", L".htm", L".css", L".scss", L".sql", L".ps1", L".bat",
        L".cmd", L".sh", L".qml", L".cmake", L".toml", L".properties"
    });
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
    if (FAILED(hr) || !factory) {
        factory.Reset();
        hr = CoGetClassObject(clsid, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_HANDLER,
                              nullptr, IID_PPV_ARGS(&factory));
    }
    if (FAILED(hr) || !factory) return {};
    factory->LockServer(TRUE);
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
        if (FAILED(hr) || !unknown) {
            unknown.Reset();
            hr = CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&unknown));
        }
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
    if (g_class_registered) return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = PreviewHandlerHost::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
        g_class_registered = true;
}

} // namespace

bool PreviewHandlerHost::CanHost(const std::wstring& path) {
    const std::wstring extension = ExtensionOf(path);
    if (extension.empty() || IsNativePreviewExtension(extension)) return false;
    CLSID clsid{};
    return FindPreviewHandlerClsid(extension, clsid);
}

PreviewHandlerHost::PreviewHandlerHost() = default;

PreviewHandlerHost::~PreviewHandlerHost() {
    Reset();
}

void PreviewHandlerHost::SetNotifyWindow(HWND hwnd) {
    notify_ = hwnd;
    if (!hwnd) Reset();
}

void PreviewHandlerHost::Hide() {
    if (hwnd_) {
        KillTimer(hwnd_, kOpenTimer);
        KillTimer(hwnd_, kFixupTimer);
        SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
    }
    shown_ = false;
    placed_x_ = INT_MIN;
    placed_y_ = INT_MIN;
    if (state_ == State::Shown) state_ = State::Idle;
}

void PreviewHandlerHost::Reset() {
    Unload();
    if (hwnd_) {
        KillTimer(hwnd_, kOpenTimer);
        KillTimer(hwnd_, kFixupTimer);
        HWND victim = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(victim);
    }
    owner_ = nullptr;
    path_.clear();
    identity_.clear();
    pending_identity_.clear();
    state_ = State::Idle;
    shown_ = false;
}

std::wstring PreviewHandlerHost::Identity(const std::wstring& path, uint64_t generation,
                                          uint64_t modified, uint64_t size) const {
    return path + L"\n" + std::to_wstring(generation) + L":" +
           std::to_wstring(modified) + L":" + std::to_wstring(size);
}

bool PreviewHandlerHost::EnsureWindow() {
    if (hwnd_ && IsWindow(hwnd_)) return true;
    hwnd_ = nullptr;
    if (!owner_ || !IsWindow(owner_)) return false;
    RegisterClassOnce();
    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"",
        WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 0, 0, owner_, nullptr, GetModuleHandleW(nullptr), this);
    return hwnd_ != nullptr;
}

bool PreviewHandlerHost::OverlayOwnsForeground() const {
    HWND fg = GetForegroundWindow();
    if (!fg || !hwnd_) return false;
    if (fg == hwnd_ || IsChild(hwnd_, fg) || GetAncestor(fg, GA_ROOT) == hwnd_)
        return true;
    HWND walk = fg;
    for (int i = 0; i < 8 && walk; ++i) {
        if (walk == hwnd_) return true;
        walk = GetWindow(walk, GW_OWNER);
    }
    return false;
}

void PreviewHandlerHost::PlaceOverlay() {
    if (!hwnd_ || !owner_) return;
    POINT origin{bounds_.left, bounds_.top};
    ClientToScreen(owner_, &origin);
    const int vw = std::max(1L, bounds_.right - bounds_.left);
    const int vh = std::max(1L, bounds_.bottom - bounds_.top);
    if (shown_ && !app_active_ && !OverlayOwnsForeground()) {
        SetWindowPos(hwnd_, HWND_NOTOPMOST, origin.x, origin.y, vw, vh,
                     SWP_NOACTIVATE | SWP_HIDEWINDOW);
        return;
    }
    const bool moved = origin.x != placed_x_ || origin.y != placed_y_ ||
        vw != placed_w_ || vh != placed_h_;
    if (!moved && shown_ && IsWindowVisible(hwnd_)) return;
    placed_x_ = origin.x;
    placed_y_ = origin.y;
    placed_w_ = vw;
    placed_h_ = vh;
    // TOPMOST only while Pulse is foreground: DComp would otherwise cover the
    // overlay, but a sticky topmost window sits on every other app.
    SetWindowPos(hwnd_, HWND_TOPMOST, origin.x, origin.y, vw, vh,
                 SWP_NOACTIVATE | (shown_ ? SWP_SHOWWINDOW : SWP_NOREDRAW));
    if (handler_ && shown_) {
        ComPtr<IPreviewHandler> preview;
        if (SUCCEEDED(handler_->QueryInterface(IID_PPV_ARGS(&preview)))) {
            RECT client{0, 0, vw, vh};
            preview->SetRect(&client);
        }
    }
}

void PreviewHandlerHost::NotifyAppActivate(bool active) {
    app_active_ = active;
    if (!hwnd_ || !shown_) return;
    if (!active) {
        if (OverlayOwnsForeground()) return;
        SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
        return;
    }
    placed_x_ = INT_MIN;
    PlaceOverlay();
}

void PreviewHandlerHost::Unload() {
    if (hwnd_) KillTimer(hwnd_, kOpenTimer);
    if (hwnd_) KillTimer(hwnd_, kFixupTimer);
    if (handler_) {
        ComPtr<IPreviewHandler> preview;
        if (SUCCEEDED(handler_->QueryInterface(IID_PPV_ARGS(&preview)))) {
            preview->Unload();
        }
        handler_->Release();
        handler_ = nullptr;
    }
    if (stream_) {
        stream_->Release();
        stream_ = nullptr;
    }
    if (site_) {
        site_->Release();
        site_ = nullptr;
    }
    shown_ = false;
}

void PreviewHandlerHost::ScheduleOpen() {
    if (!EnsureWindow()) {
        state_ = State::Failed;
        return;
    }
    KillTimer(hwnd_, kOpenTimer);
    SetTimer(hwnd_, kOpenTimer, kOpenDelayMs, nullptr);
}

bool PreviewHandlerHost::OpenCurrent() {
    Unload();
    if (path_.empty() || !EnsureWindow()) {
        return false;
    }
    CLSID clsid{};
    if (!FindPreviewHandlerClsid(ExtensionOf(path_), clsid)) {
        return false;
    }

    ComPtr<IUnknown> unknown = CreateHandler(clsid);
    if (!unknown) {
        return false;
    }

    site_ = new PreviewFrame(hwnd_);
    ComPtr<IObjectWithSite> object_with_site;
    HRESULT site_hr = unknown.As(&object_with_site);
    if (SUCCEEDED(site_hr) && object_with_site)
        object_with_site->SetSite(site_);

    const std::wstring open_path = ShellPath(path_);
    const bool file_ok = InitWithFile(unknown.Get(), open_path);
    const bool item_ok = file_ok ? false : InitWithItem(unknown.Get(), open_path);
    const bool stream_ok = (file_ok || item_ok)
        ? false : InitWithStream(unknown.Get(), open_path, &stream_);
    bool initialized = file_ok || item_ok || stream_ok;
    ComPtr<IPreviewHandler> preview;
    HRESULT preview_qi = unknown.As(&preview);
    if (initialized) initialized = SUCCEEDED(preview_qi) && preview;
    if (!initialized) {
        Unload();
        return false;
    }
    handler_ = unknown.Detach();

    shown_ = true;
    PlaceOverlay();
    RECT client{};
    GetClientRect(hwnd_, &client);
    if (client.right <= client.left || client.bottom <= client.top) {
        Unload();
        Hide();
        return false;
    }
    HRESULT hr = preview->SetWindow(hwnd_, &client);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    hr = preview->DoPreview();
    if (FAILED(hr)) {
        Unload();
        Hide();
        return false;
    }
    hr = preview->SetRect(&client);
    SetTimer(hwnd_, kFixupTimer, kFixupDelayMs, nullptr);
    return true;
}

void PreviewHandlerHost::Sync(HWND owner, const D2D1_RECT_F& bounds, const std::wstring& path,
                              DWORD attrs, uint64_t generation, uint64_t modified, uint64_t size,
                              bool dark, const D2D1_COLOR_F& bg, const D2D1_COLOR_F& fg, bool enabled) {
    owner_ = owner;
    dark_ = dark;
    bg_ = bg;
    fg_ = fg;
    bounds_.left = static_cast<LONG>(std::lround(bounds.left));
    bounds_.top = static_cast<LONG>(std::lround(bounds.top));
    bounds_.right = static_cast<LONG>(std::lround(bounds.right));
    bounds_.bottom = static_cast<LONG>(std::lround(bounds.bottom));

    const bool usable = enabled && owner && IsWindow(owner) && !IsIconic(owner) &&
        IsWindowVisible(owner) && !IsOfflinePlaceholder(attrs) && CanHost(path) &&
        bounds_.right > bounds_.left + 8 && bounds_.bottom > bounds_.top + 8;
    if (!usable) {
        const bool had_content = handler_ != nullptr || state_ == State::Loading;
        Unload();
        Hide();
        path_.clear();
        identity_.clear();
        pending_identity_.clear();
        attrs_ = 0;
        if (had_content) state_ = State::Idle;
        return;
    }

    const std::wstring identity = Identity(path, generation, modified, size);
    const bool same = identity == identity_ && handler_ && shown_;
    path_ = path;
    attrs_ = attrs;
    if (same) {
        state_ = State::Shown;
        if (EnsureWindow()) PlaceOverlay();
        return;
    }
    if (identity == pending_identity_ && state_ == State::Loading) {
        if (EnsureWindow()) PlaceOverlay();
        return;
    }
    if (identity == identity_ && state_ == State::Failed) {
        Hide();
        return;
    }

    Unload();
    Hide();
    identity_.clear();
    pending_identity_ = identity;
    state_ = State::Loading;
    ScheduleOpen();
}

LRESULT CALLBACK PreviewHandlerHost::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PreviewHandlerHost* self = reinterpret_cast<PreviewHandlerHost*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<PreviewHandlerHost*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_TIMER:
        if (wParam == kOpenTimer) {
            KillTimer(hwnd, kOpenTimer);
            const bool ok = self->OpenCurrent();
            self->identity_ = self->pending_identity_;
            self->pending_identity_.clear();
            self->state_ = ok ? State::Shown : State::Failed;
            if (!ok) self->Hide();
            if (self->notify_) InvalidateRect(self->notify_, nullptr, FALSE);
            return 0;
        }
        if (wParam == kFixupTimer) {
            KillTimer(hwnd, kFixupTimer);
            if (self->handler_ && self->shown_) {
                ComPtr<IPreviewHandler> preview;
                if (SUCCEEDED(self->handler_->QueryInterface(IID_PPV_ARGS(&preview)))) {
                    RECT client{};
                    GetClientRect(hwnd, &client);
                    preview->SetRect(&client);
                }
                self->PlaceOverlay();
            }
            return 0;
        }
        break;
    case WM_ACTIVATEAPP:
        self->NotifyAppActivate(wParam != 0);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND: {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(reinterpret_cast<HDC>(wParam), &rc, brush);
        DeleteObject(brush);
        return 1;
    }
    case WM_DESTROY:
        if (self->hwnd_ == hwnd) self->hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace pulse::ui


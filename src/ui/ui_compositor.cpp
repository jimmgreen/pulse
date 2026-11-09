#include "picker_proof_trace.h"
// ui_compositor.cpp
#include "ui_compositor.h"
#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "lumatext_renderer.h"
#include "typography.h"
#include "FluentTokens.h"
#include "text_contrast_scope.h"
#ifdef PULSE_STARTUP_TRACE_DIAGNOSTICS
#include "../app/startup_trace.h"
#endif
#include <commctrl.h>
#include <prsht.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace pulse::ui {
namespace {
void TraceGraphics(const char* label) noexcept {
#ifdef PULSE_STARTUP_TRACE_DIAGNOSTICS
    app::startup_trace::Mark(label);
#else
    (void)label;
#endif
}
}

namespace {

// One D3D11 + D2D device per UI thread, shared by every Compositor on it.
// Each hardware device costs the process driver worker threads (9 per device
// on current NVIDIA drivers) and ~15 MB of driver heaps, and Pulse keeps
// several composited windows alive at once: the main window plus the hidden,
// reusable quick preview and file operation windows, and any open dialog.
// Every window still owns its device context, swap chain and
// DirectComposition device. The D2D factory is single-threaded, hence the
// per-thread scope. A plain pointer (not a thread_local object) stays valid
// for compositors that are destroyed during static teardown.
struct SharedGraphics {
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGIDevice1> dxgi;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> d2d;
    unsigned users = 0;
};
thread_local SharedGraphics* t_shared_graphics = nullptr;

HRESULT CreateD3DDevice(ComPtr<ID3D11Device>& device) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr, nullptr);
    if (FAILED(hr)) {
        device.reset();
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr, nullptr);
    }
    return hr;
}

// PrefetchSharedDevice's hand-off. Never freed: a worker still inside the
// driver at process exit must not find it destroyed.
struct DevicePrefetch {
    std::mutex mutex;
    std::condition_variable ready;
    bool started = false;
    bool done = false;
    ComPtr<ID3D11Device> device;  // null once taken or when creation failed
};
DevicePrefetch& Prefetch() {
    static DevicePrefetch* prefetch = new DevicePrefetch;
    return *prefetch;
}

// The prefetched device, once; null when none was started, it failed, or
// it was taken already. Waits for a creation still in flight.
ComPtr<ID3D11Device> TakePrefetchedDevice() {
    DevicePrefetch& prefetch = Prefetch();
    std::unique_lock<std::mutex> lock(prefetch.mutex);
    if (!prefetch.started) return {};
    prefetch.ready.wait(lock, [&] { return prefetch.done; });
    return std::move(prefetch.device);
}

template <typename T>
void AssignRef(ComPtr<T>& target, T* value) {
    target.reset();
    if (value) value->AddRef();
    target.p = value;
}

} // namespace

Compositor::Compositor() = default;
Compositor::~Compositor() { Shutdown(); }

bool Compositor::CheckGraphics(HRESULT hr, const wchar_t* stage) {
    if (SUCCEEDED(hr)) return true;
    wchar_t message[256]{};
    swprintf_s(message, L"%s failed (HRESULT 0x%08X)", stage, static_cast<unsigned>(hr));
    initialization_error_ = message;
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        const std::wstring directory = std::wstring(local) + L"\\Pulse";
        CoTaskMemFree(local);
        CreateDirectoryW(directory.c_str(), nullptr);
        FILE* log = nullptr;
        if (_wfopen_s(&log, (directory + L"\\pulse_graphics.log").c_str(), L"a, ccs=UTF-8") == 0) {
            fwprintf(log, L"%s\n", message);
            fclose(log);
        }
    }
    return false;
}

bool Compositor::IsDeviceLost(HRESULT hr) {
    return hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED ||
           hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

namespace {
// Compositors of this thread (one shared device), for FrameSourceUnder.
thread_local std::vector<Compositor*> t_compositors;
std::atomic<Compositor::AcrylicPlateBuilder> g_acrylic_plate_builder{nullptr};
}

bool Compositor::Init(HWND hwnd) {
    initialization_error_.clear();
    hwnd_ = hwnd;
    if (std::find(t_compositors.begin(), t_compositors.end(), this) == t_compositors.end())
        t_compositors.push_back(this);
    device_lost_ = false;
    if (!InitD3D()) return false;
    TraceGraphics("graphics_device_ready");
    RECT rc;
    GetClientRect(hwnd_, &rc);
    width_ = std::max(1L, rc.right - rc.left);
    height_ = std::max(1L, rc.bottom - rc.top);
    if (!CreateSwapChain()) return false;
    TraceGraphics("graphics_swapchain_ready");
    ResizeSwapChain();
    TraceGraphics("graphics_target_ready");
    return targetBitmap_.get() != nullptr;
}

void Compositor::Shutdown() {
    t_compositors.erase(std::remove(t_compositors.begin(), t_compositors.end(), this), t_compositors.end());
    if (!dc_.get() && !d3dDevice_.get()) return; // already shut down
    SharedGraphics* shared = t_shared_graphics;
    if (shared && d3dDevice_.get() && d3dDevice_.get() == shared->d3d.get() &&
        --shared->users == 0) {
        // Last window on this thread: the device goes away with it.
        t_shared_graphics = nullptr;
        delete shared;
    }
    if (lumaText_) lumaText_->Shutdown();
    frameCopy_.reset();
    held_.reset();
    targetBitmap_.reset();
    if (dc_.get()) dc_->SetTarget(nullptr);
    if (compositionTarget_.get()) compositionTarget_->SetRoot(nullptr);
    if (compositionDevice_.get()) compositionDevice_->Commit();
    compositionVisual_.reset();
    compositionTarget_.reset();
    compositionDevice_.reset();
    swapChain_.reset();
    dc_.reset();
    d2dDevice_.reset();
    d2dFactory_.reset();
    dxgiDevice_.reset();
    d3dDevice_.reset();
    dwriteFactory_.reset();
    textRenderingParams_.reset();
    text_params_monitor_ = nullptr;
    typography::InvalidateCaches();
    textFormat_.reset();
    fileNameFormat_.reset();
    smallFormat_.reset();
    headerFormat_.reset();
    tabFormat_.reset();
    addressFormat_.reset();
    iconFormat_.reset();
    transparentComposition_ = false;
    hwnd_ = nullptr;
    device_lost_ = false;
}

void Compositor::PrefetchSharedDevice() {
    DevicePrefetch& prefetch = Prefetch();
    {
        std::lock_guard<std::mutex> lock(prefetch.mutex);
        if (prefetch.started) return;
        prefetch.started = true;
    }
    TraceGraphics("graphics_prefetch_scheduled");
    try {
        std::thread([] {
            TraceGraphics("graphics_prefetch_worker_begin");
            ComPtr<ID3D11Device> device;
            const bool ok = SUCCEEDED(CreateD3DDevice(device));
            TraceGraphics("graphics_prefetch_worker_created");
            DevicePrefetch& state = Prefetch();
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                if (ok) state.device = std::move(device);
                state.done = true;
            }
            state.ready.notify_all();
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lock(prefetch.mutex);
        prefetch.done = true;  // InitD3D creates the device itself
    }
}

bool Compositor::InitD3D() {
    SharedGraphics* shared = t_shared_graphics;
    if (shared && shared->d3d->GetDeviceRemovedReason() != S_OK) {
        // A removed device is never handed out again. Windows still using it
        // hold their own references until they recover onto the new device.
        t_shared_graphics = nullptr;
        delete shared;
        shared = nullptr;
    }
    HRESULT hr = S_OK;
    if (!shared) {
        auto created = std::make_unique<SharedGraphics>();
        // D3D11 devices are free-threaded, so one created on the prefetch
        // worker serves this thread like its own.
        created->d3d = TakePrefetchedDevice();
        TraceGraphics("graphics_prefetch_taken");
        if (created->d3d.get() && created->d3d->GetDeviceRemovedReason() != S_OK) created->d3d.reset();
        if (!created->d3d.get()) {
            hr = CreateD3DDevice(created->d3d);
            if (!CheckGraphics(hr, L"D3D11CreateDevice (hardware/WARP)")) return false;
        }
        TraceGraphics("graphics_d3d_ready");

        hr = created->d3d->QueryInterface(&created->dxgi);
        if (!CheckGraphics(hr, L"IDXGIDevice1")) return false;

        D2D1_FACTORY_OPTIONS opts{};
#ifdef _DEBUG
        opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
            &opts, reinterpret_cast<void**>(&created->factory));
        if (!CheckGraphics(hr, L"D2D1CreateFactory")) return false;
        TraceGraphics("graphics_d2d_factory_ready");

        hr = created->factory->CreateDevice(created->dxgi.get(), &created->d2d);
        if (!CheckGraphics(hr, L"D2D CreateDevice")) return false;
        shared = created.release();
        t_shared_graphics = shared;
    }
    // Init without Shutdown keeps its existing share of the device.
    if (d3dDevice_.get() != shared->d3d.get()) ++shared->users;
    AssignRef(d3dDevice_, shared->d3d.get());
    AssignRef(dxgiDevice_, shared->dxgi.get());
    AssignRef(d2dFactory_, shared->factory.get());
    AssignRef(d2dDevice_, shared->d2d.get());

    hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_);
    if (!CheckGraphics(hr, L"D2D CreateDeviceContext")) return false;
    TraceGraphics("graphics_d2d_context_ready");

    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
        reinterpret_cast<IUnknown**>(&dwriteFactory_));
    if (!CheckGraphics(hr, L"DWriteCreateFactory")) return false;
    TraceGraphics("graphics_dwrite_ready");

    UpdateTextRenderingParams(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
    if (!lumaText_) lumaText_ = std::make_unique<LumaTextRenderer>();
    lumaText_->Init(dwriteFactory_.get(), dc_.get());
    TraceGraphics("graphics_lumatext_ready");

    return true;
}

bool Compositor::DrawLumaText(std::wstring_view text, IDWriteTextFormat* format,
                              const D2D1_RECT_F& bounds,
                              const D2D1_COLOR_F& foreground,
                              const D2D1_COLOR_F& background,
                              DWRITE_TEXT_ALIGNMENT alignment) {
    if (!lumaText_ || !lumaText_->Enabled() || !typography::UseLumaTextForUi()) return false;
    if (lumaText_->Draw(text, format, bounds, foreground, background, alignment)) {
        return true;
    }
    lumaText_->RecordFallback(0);
    return false;
}

void Compositor::DrawTextLayout(D2D1_POINT_2F origin, IDWriteTextLayout* layout,
                                ID2D1SolidColorBrush* brush, D2D1_DRAW_TEXT_OPTIONS options) {
    if (!dc_.get() || !layout || !brush) return;
    // Colour fonts (emoji) keep DirectWrite's colour glyph path.
    if (!(options & D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT) && LumaTextEnabled()) {
        D2D1_COLOR_F color = brush->GetColor();
        color.a *= brush->GetOpacity();
        if (lumaText_->DrawLayout(origin, layout, color,
                                  (options & D2D1_DRAW_TEXT_OPTIONS_CLIP) != 0)) {
            return;
        }
        lumaText_->RecordFallback(1);
    }
    dc_->DrawTextLayout(origin, layout, brush, options);
}

bool Compositor::DrawLumaTextSplit(std::wstring_view text, IDWriteTextFormat* format,
                                   const D2D1_RECT_F& bounds,
                                   const D2D1_COLOR_F& foreground, std::uint32_t split,
                                   const D2D1_COLOR_F& split_foreground,
                                   const D2D1_COLOR_F& background) {
    if (!lumaText_ || !lumaText_->Enabled() || !typography::UseLumaTextForUi()) return false;
    if (lumaText_->DrawSplit(text, format, bounds, foreground, split, split_foreground,
                             background)) {
        return true;
    }
    lumaText_->RecordFallback(2);
    return false;
}

bool Compositor::MeasureLumaText(std::wstring_view text, IDWriteTextFormat* format,
                                 float& width, float* height) {
    if (!lumaText_ || !lumaText_->Enabled() || !typography::UseLumaTextForUi()) return false;
    return lumaText_->Measure(text, format, width, height);
}

bool Compositor::PaintLumaEdit(HWND hwnd, HDC hdc, IDWriteTextFormat* format,
                               const D2D1_COLOR_F& foreground,
                               const D2D1_COLOR_F& background) {
    if (!CustomEditEnabled()) return false;
    return lumaText_->PaintEdit(hwnd, hdc, format, foreground, background);
}

bool Compositor::DrawLumaEditOnSurface(HWND hwnd, IDWriteTextFormat* format,
    const D2D1_COLOR_F& foreground, const D2D1_COLOR_F& background, const D2D1_RECT_F& bounds) {
    return CustomEditEnabled() && lumaText_->DrawEditOnSurface(hwnd, format, foreground, background, bounds);
}

bool Compositor::PresentLumaEdit(HWND hwnd, IDWriteTextFormat* format,
                                 const D2D1_COLOR_F& foreground,
                                 const D2D1_COLOR_F& background, bool painted_plain_child) {
    if (!hwnd) return false;
#if defined(PULSE_TEST_LUMATEXT_PRESENT_FAILURE) || defined(PULSE_WITH_SELFTEST)
    if (GetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_PRESENT_FAILURE", nullptr, 0)) return false;
#endif
    // LWA_ALPHA children use Windows' redirected surface. Mixing that mode
    // with UpdateLayeredWindow fails; paint our bitmap into their DC instead.
    DWORD layered_flags = 0;
    const bool redirected = GetLayeredWindowAttributes(hwnd, nullptr, nullptr, &layered_flags) &&
        (layered_flags & LWA_ALPHA);
    // Both modes receive the same PaintEdit bitmap; a plain child uses its
    // normal redirected DC solely as the destination, never native text output.
    const bool use_dc = redirected || painted_plain_child;
    HDC hdc = use_dc ? GetDC(hwnd) : nullptr;
    const bool ok = (!use_dc || hdc) && PaintLumaEdit(hwnd, hdc, format, foreground, background);
    if (hdc) ReleaseDC(hwnd, hdc);
    ValidateRect(hwnd, nullptr);
    return ok;
}

LRESULT Compositor::CallLumaEditMouse(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                      IDWriteTextFormat* format) {
    if (CustomEditEnabled())
        return lumaText_->CallEditDefaultMouse(hwnd, msg, wParam, lParam, format);
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

bool Compositor::LumaTextAvailable() const noexcept {
    return lumaText_ && lumaText_->Enabled();
}

void Compositor::SynchronizeEditSelection(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (lumaText_) lumaText_->SynchronizeEditSelection(hwnd, msg, wParam, lParam);
}

bool Compositor::LumaTextEnabled() const noexcept {
    return LumaTextAvailable() && typography::UseLumaTextForUi() && !TextHighContrast();
}

bool Compositor::CustomEditEnabled() const noexcept {
    return LumaTextAvailable() && !TextHighContrast();
}

const LumaTextStats* Compositor::GetLumaTextStats() const noexcept {
    return lumaText_ ? &lumaText_->Stats() : nullptr;
}

bool Compositor::CreateSwapChain() {
    ComPtr<IDXGIAdapter> adapter;
    if (!CheckGraphics(dxgiDevice_->GetAdapter(&adapter), L"DXGI GetAdapter")) return false;
    ComPtr<IDXGIFactory2> factory;
    if (!CheckGraphics(adapter->GetParent(IID_PPV_ARGS(&factory)), L"IDXGIFactory2")) return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(std::max(1, width_));
    desc.Height = static_cast<UINT>(std::max(1, height_));
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    // Partial redraw needs buffers that keep their pixels between presents.
    desc.SwapEffect = compat::ModernWindows() && !PartialRedrawEnabled()
        ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    HRESULT hr = DCompositionCreateDevice(dxgiDevice_.get(), __uuidof(IDCompositionDevice),
        reinterpret_cast<void**>(&compositionDevice_));
    if (SUCCEEDED(hr)) {
        hr = factory->CreateSwapChainForComposition(
            d3dDevice_.get(), &desc, nullptr, &swapChain_);
        if (FAILED(hr) && desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD) {
            swapChain_.reset();
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            hr = factory->CreateSwapChainForComposition(d3dDevice_.get(), &desc, nullptr, &swapChain_);
        }
    }
    if (SUCCEEDED(hr)) hr = compositionDevice_->CreateTargetForHwnd(hwnd_, composition_above_children_ ? TRUE : FALSE, &compositionTarget_);
    if (SUCCEEDED(hr)) hr = compositionDevice_->CreateVisual(&compositionVisual_);
    if (SUCCEEDED(hr)) hr = compositionVisual_->SetContent(swapChain_.get());
    if (SUCCEEDED(hr)) hr = compositionTarget_->SetRoot(compositionVisual_.get());
    if (SUCCEEDED(hr)) hr = compositionDevice_->Commit();

    if (SUCCEEDED(hr)) {
        transparentComposition_ = true;
    } else {
        compositionVisual_.reset();
        compositionTarget_.reset();
        compositionDevice_.reset();
        swapChain_.reset();
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        hr = factory->CreateSwapChainForHwnd(d3dDevice_.get(), hwnd_, &desc,
            nullptr, nullptr, &swapChain_);
        if (!CheckGraphics(hr, L"CreateSwapChainForHwnd")) return false;
    }

    dxgiDevice_->SetMaximumFrameLatency(1);
    partial_swap_ = PartialRedrawEnabled() && desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    full_next_ = true;
    return true;
}

bool Compositor::PartialRedrawEnabled() {
    static const bool enabled = [] {
        wchar_t value[8]{};
        const DWORD n = GetEnvironmentVariableW(L"PULSE_PARTIAL_REDRAW", value, ARRAYSIZE(value));
        return !(n == 1 && value[0] == L'0');
    }();
    return enabled;
}

void Compositor::ResizeSwapChain() {
    if (!swapChain_.get() || width_ <= 0 || height_ <= 0) return;
    full_next_ = true;
    frameCopy_.reset();
    held_.reset();
    targetBitmap_.reset();
    dc_->SetTarget(nullptr);
    // D3D11 destroys released views lazily: the ones D2D made on the back
    // buffer can still hold it, and ResizeBuffers then fails with
    // DXGI_ERROR_INVALID_CALL. Flushing first is what Microsoft's
    // DeviceResources template does for the same reason.
    {
        ComPtr<ID3D11DeviceContext> context;
        if (d3dDevice_.get()) d3dDevice_->GetImmediateContext(&context);
        if (context.get()) context->Flush();
    }
    // A failure below, lost device or not, rebuilds the swap chain: the new
    // size is already recorded, so nothing would ask for it again and every
    // frame would draw into no target (a white window after the monitor it
    // was on went away).
    HRESULT hr = swapChain_->ResizeBuffers(0, (UINT)width_, (UINT)height_,
        DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        CheckGraphics(hr, L"ResizeBuffers");
        NotifyDeviceLost(hr);
        return;
    }

    ComPtr<IDXGISurface> surface;
    hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface));
    if (FAILED(hr)) {
        CheckGraphics(hr, L"SwapChain GetBuffer");
        NotifyDeviceLost(hr);
        return;
    }
    // Layout, hit-testing, and text formats already apply the window scale.
    // Keep the D2D target at 96 DPI so per-monitor scaling happens exactly once.
    // Using the monitor DPI here as well produced scale^2 sizing and clipped
    // off-screen flyout text on 125/150/200% displays.
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
            transparentComposition_ ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE),
        96.0f, 96.0f);
    hr = dc_->CreateBitmapFromDxgiSurface(surface.get(), &props, &targetBitmap_);
    if (FAILED(hr)) {
        CheckGraphics(hr, L"CreateBitmapFromDxgiSurface");
        NotifyDeviceLost(hr);
        return;
    }
    dc_->SetTarget(targetBitmap_.get());
}

void Compositor::Resize(int width, int height) {
    width_ = width;
    height_ = height;
    if (swapChain_.get()) ResizeSwapChain();
}

bool Compositor::UpdateTextRenderingParams(HMONITOR monitor) {
    if (!dwriteFactory_.get() || !dc_.get()) return false;
    if (!monitor && hwnd_) monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    const int mode = static_cast<int>(typography::CurrentTextRenderMode());
    if (textRenderingParams_.get() && monitor == text_params_monitor_ && mode == text_params_mode_) return true;

    ComPtr<IDWriteRenderingParams2> next;
    if (FAILED(typography::CreateRenderingParams(dwriteFactory_.get(), monitor, &next)) ||
        !next.get()) {
        return false;
    }
    const bool monitor_changed = text_params_monitor_ && text_params_monitor_ != monitor;
    textRenderingParams_ = std::move(next);
    text_params_monitor_ = monitor;
    text_params_mode_ = mode;
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc_->SetTextRenderingParams(textRenderingParams_.get());
    if (monitor_changed && hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

namespace {
// Cubic segments: opening eases out (1-(1-u)^3), closing eases in (u^3).
bool BuildZoomCurve(IDCompositionDevice* device, float from, float to, double duration,
                    bool ease_out, ComPtr<IDCompositionAnimation>& out) {
    if (FAILED(device->CreateAnimation(&out)) || !out.get()) return false;
    const double d = static_cast<double>(to) - static_cast<double>(from);
    const double t = duration;
    HRESULT hr = ease_out
        ? out->AddCubic(0.0, from, static_cast<float>(3.0 * d / t),
                        static_cast<float>(-3.0 * d / (t * t)), static_cast<float>(d / (t * t * t)))
        : out->AddCubic(0.0, from, 0.0f, 0.0f, static_cast<float>(d / (t * t * t)));
    if (SUCCEEDED(hr)) hr = out->End(duration, to);
    return SUCCEEDED(hr);
}
} // namespace

bool Compositor::HideContent() {
    if (!compositionDevice_.get() || !compositionVisual_.get()) return false;
    ComPtr<IDCompositionEffectGroup> effect;
    if (FAILED(compositionDevice_->CreateEffectGroup(&effect)) || !effect.get()) return false;
    effect->SetOpacity(0.0f);
    compositionVisual_->SetTransform(static_cast<IDCompositionTransform*>(nullptr));
    compositionVisual_->SetEffect(effect.get());
    return SUCCEEDED(compositionDevice_->Commit());
}

bool Compositor::PlayZoom(float origin_x, float origin_y, bool opening, double duration_s) {
    if (!compositionDevice_.get() || !compositionVisual_.get() || duration_s <= 0.0) return false;
    constexpr float kSmall = 0.6f;
    const float ox = std::clamp(origin_x, 0.0f, static_cast<float>(std::max(1, width_)));
    const float oy = std::clamp(origin_y, 0.0f, static_cast<float>(std::max(1, height_)));
    ComPtr<IDCompositionScaleTransform> scale;
    ComPtr<IDCompositionEffectGroup> effect;
    ComPtr<IDCompositionAnimation> scale_curve;
    ComPtr<IDCompositionAnimation> opacity_curve;
    if (FAILED(compositionDevice_->CreateScaleTransform(&scale)) || !scale.get() ||
        FAILED(compositionDevice_->CreateEffectGroup(&effect)) || !effect.get() ||
        !BuildZoomCurve(compositionDevice_.get(), opening ? kSmall : 1.0f, opening ? 1.0f : kSmall,
                        duration_s, opening, scale_curve) ||
        // Opacity settles a little earlier than scale when opening.
        !BuildZoomCurve(compositionDevice_.get(), opening ? 0.0f : 1.0f, opening ? 1.0f : 0.0f,
                        opening ? duration_s * 0.7 : duration_s, opening, opacity_curve)) {
        ClearZoom();
        return false;
    }
    scale->SetCenterX(ox);
    scale->SetCenterY(oy);
    scale->SetScaleX(scale_curve.get());
    scale->SetScaleY(scale_curve.get());
    effect->SetOpacity(opacity_curve.get());
    compositionVisual_->SetTransform(scale.get());
    compositionVisual_->SetEffect(effect.get());
    return SUCCEEDED(compositionDevice_->Commit());
}

void Compositor::ClearZoom() {
    if (!compositionDevice_.get() || !compositionVisual_.get()) return;
    compositionVisual_->SetTransform(static_cast<IDCompositionTransform*>(nullptr));
    compositionVisual_->SetEffect(nullptr);
    compositionDevice_->Commit();
}

namespace {
std::atomic<bool> g_frame_copy_enabled{false};
}

void Compositor::SetFrameCopyEnabled(bool enabled) noexcept {
    g_frame_copy_enabled.store(enabled, std::memory_order_relaxed);
}

bool Compositor::FrameCopyEnabled() noexcept {
    return g_frame_copy_enabled.load(std::memory_order_relaxed);
}

void Compositor::CopyFrame() {
    if (!FrameCopyEnabled() || !dc_.get() || !targetBitmap_.get()) {
        frameCopy_.reset();
        return;
    }
    const D2D1_SIZE_U size = targetBitmap_->GetPixelSize();
    if (frameCopy_.get()) {
        const D2D1_SIZE_U have = frameCopy_->GetPixelSize();
        if (have.width != size.width || have.height != size.height) frameCopy_.reset();
    }
    if (!frameCopy_.get()) {
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            targetBitmap_->GetPixelFormat(), 96.0f, 96.0f);
        if (FAILED(dc_->CreateBitmap(size, nullptr, 0, &props, &frameCopy_))) return;
    }
    // GPU to GPU; the frame's drawing has already ended.
    if (FAILED(frameCopy_->CopyFromBitmap(nullptr, targetBitmap_.get(), nullptr)))
        frameCopy_.reset();
    else
        ++frameCopySerial_;
}

void Compositor::SetAcrylicPlateBuilder(AcrylicPlateBuilder builder) noexcept {
    g_acrylic_plate_builder.store(builder, std::memory_order_relaxed);
}

Compositor::AcrylicPlateBuilder Compositor::GetAcrylicPlateBuilder() noexcept {
    return g_acrylic_plate_builder.load(std::memory_order_relaxed);
}

Compositor* Compositor::FrameSourceUnder(HWND dialog, const RECT& area) {
    if (!dialog || t_compositors.empty()) return nullptr;
    // Down the z-order from the dialog: the first window that overlaps it
    // decides. Ours lends its frame; anything else means no acrylic.
    int guard = 0;
    for (HWND w = GetWindow(dialog, GW_HWNDNEXT); w && guard < 1024; w = GetWindow(w, GW_HWNDNEXT), ++guard) {
        if (!IsWindowVisible(w) || IsIconic(w)) continue;
        const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
        if ((ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT)) continue;  // click-through, e.g. dialog shadows
        BOOL cloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) continue;
        RECT bounds{};
        RECT overlap{};
        if (!GetWindowRect(w, &bounds) || !IntersectRect(&overlap, &bounds, &area)) continue;
        Compositor* best = nullptr;
        long long best_area = 0;
        for (Compositor* compositor : t_compositors) {
            if (!compositor->frameCopy_.get() || !compositor->hwnd_ ||
                GetAncestor(compositor->hwnd_, GA_ROOT) != w)
                continue;
            RECT client{};
            GetClientRect(compositor->hwnd_, &client);
            MapWindowPoints(compositor->hwnd_, nullptr, reinterpret_cast<POINT*>(&client), 2);
            RECT part{};
            if (!IntersectRect(&part, &client, &area)) continue;
            const long long size = static_cast<long long>(part.right - part.left) * (part.bottom - part.top);
            if (size > best_area) {
                best_area = size;
                best = compositor;
            }
        }
        return best;
    }
    return nullptr;
}

HRESULT Compositor::Present(const RECT* dirty) {
#if defined(PULSE_PICKER_INPROCESS_PROOF)
    // Controlled proof only: the caller arms this after its second FileOk callback.
    const DWORD previous_error = GetLastError();
    wchar_t fail_present[2]{};
    const bool inject_failure = GetEnvironmentVariableW(L"PULSE_PICKER_UI_PROOF_FAIL_PRESENT", fail_present, 2) == 1 &&
        fail_present[0] == L'1';
    SetLastError(previous_error);
    if (inject_failure) return E_FAIL;
#endif
    if (device_lost_) return DXGI_ERROR_DEVICE_REMOVED;
    if (hwnd_) UpdateTextRenderingParams(
        MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
    PULSE_PICKER_TRACE_POINT("copy-frame-before");
    CopyFrame();
    PULSE_PICKER_TRACE_POINT("copy-frame-after");
    if (captureNext_) {
        captureNext_ = false;
        held_.reset();
        if (dc_.get() && targetBitmap_.get()) {
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                targetBitmap_->GetPixelFormat(), 96.0f, 96.0f);
            if (SUCCEEDED(dc_->CreateBitmap(targetBitmap_->GetPixelSize(), nullptr, 0, &props, &held_)) &&
                FAILED(held_->CopyFromBitmap(nullptr, targetBitmap_.get(), nullptr)))
                held_.reset();
        }
    }
    HRESULT hr = E_UNEXPECTED;
    if (swapChain_.get()) {
        if (dirty && CanPresentPartial()) {
            DXGI_PRESENT_PARAMETERS params{};
            RECT rect = *dirty;
            params.DirtyRectsCount = 1;
            params.pDirtyRects = &rect;
            PULSE_PICKER_TRACE_POINT("swapchain-present-partial-before");
            hr = swapChain_->Present1(1, 0, &params);
            PULSE_PICKER_TRACE_POINT("swapchain-present-partial-after");
        } else {
            PULSE_PICKER_TRACE_POINT("swapchain-present-before");
            hr = swapChain_->Present(1, 0);
            PULSE_PICKER_TRACE_POINT("swapchain-present-after");
        }
    }
    // Only a frame that reached the screen whole lets the next one be partial.
    full_next_ = FAILED(hr) || hr == DXGI_STATUS_OCCLUDED;
    if (FAILED(hr)) {
        if (IsDeviceLost(hr)) NotifyDeviceLost(hr);
        return hr;
    }
    const HRESULT present_status = hr;
    if (compositionDevice_.get()) {
        PULSE_PICKER_TRACE_POINT("composition-commit-before");
        hr = compositionDevice_->Commit();
        PULSE_PICKER_TRACE_POINT("composition-commit-after");
        if (hr != S_OK) {
            if (IsDeviceLost(hr)) NotifyDeviceLost(hr);
            return hr;
        }
    }
    return present_status;
}

void Compositor::NotifyDeviceLost(HRESULT) {
    if (device_lost_) return;
    device_lost_ = true;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

bool Compositor::Recover() {
    if (!device_lost_) return true;
    const HWND hwnd = hwnd_;
    if (!hwnd) return false;
    Shutdown();
    if (!Init(hwnd)) {
        device_lost_ = true;
        return false;
    }
    return true;
}

bool Compositor::SaveSnapshot(const wchar_t* path) {
    if (!dc_.get() || !targetBitmap_.get()) return false;
    // Present rotates flip-model buffers. A pre-Present frame copy identifies
    // the frame actually drawn, rather than the next writable back buffer.
    auto* source_bitmap = FrameCopyEnabled() && frameCopy_.get() ? frameCopy_.get() : targetBitmap_.get();
    const auto size = source_bitmap->GetPixelSize();
    const auto pixel_format = source_bitmap->GetPixelFormat();
    ComPtr<ID2D1Bitmap1> readable;
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        pixel_format, 96.0f, 96.0f);
    if (FAILED(dc_->CreateBitmap(size, nullptr, 0, &properties, &readable)) ||
        FAILED(readable->CopyFromBitmap(nullptr, source_bitmap, nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    struct Unmap {
        ID2D1Bitmap1* bitmap;
        ~Unmap() { bitmap->Unmap(); }
    } unmap{readable.get()};
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(size.width, size.height))) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    // PNG stores straight alpha; DComp surfaces store premultiplied alpha.
    // Let WIC perform the conversion, just as the extension-panel exporter does.
    const auto input_format = pixel_format.alphaMode == D2D1_ALPHA_MODE_PREMULTIPLIED
        ? GUID_WICPixelFormat32bppPBGRA : pixel_format.alphaMode == D2D1_ALPHA_MODE_IGNORE
        ? GUID_WICPixelFormat32bppBGR : GUID_WICPixelFormat32bppBGRA;
    ComPtr<IWICBitmap> source;
    if (FAILED(wic->CreateBitmapFromMemory(size.width, size.height, input_format,
        mapped.pitch, mapped.pitch * size.height, mapped.bits, &source))) return false;
    if (FAILED(frame->WriteSource(source.get(), nullptr)) || FAILED(frame->Commit())) return false;
    return SUCCEEDED(encoder->Commit());
}

static void CreateFormat(IDWriteFactory2* factory, float size, DWRITE_FONT_WEIGHT weight,
                         typography::FontRole role, ComPtr<IDWriteTextFormat>& fmt) {
    if (!factory) return;
    typography::CreateTextFormat(factory, {role, size, weight}, &fmt);
    if (fmt.get()) {
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        fmt->SetTrimming(&trimming, nullptr);
    }
}

void Compositor::RecreateTextFormats(float scale) {
    UpdateTextRenderingParams(hwnd_
        ? MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST) : nullptr);
    // Replacing formats can reuse their addresses, so retire cached widths.
    // A new compositor owns no old formats: its first initialization must
    // not discard the other windows' already measured text.
    if (TextOptimizationBaselineForTest() || textFormat_.get() || fileNameFormat_.get() || smallFormat_.get() ||
        headerFormat_.get() || tabFormat_.get() || addressFormat_.get() || iconFormat_.get())
        typography::InvalidateMeasurements();
    textFormat_.reset();
    fileNameFormat_.reset();
    smallFormat_.reset();
    headerFormat_.reset();
    tabFormat_.reset();
    addressFormat_.reset();
    iconFormat_.reset();
    CreateFormat(dwriteFactory_.get(), 14.0f * scale, DWRITE_FONT_WEIGHT_NORMAL,
        typography::FontRole::Text, textFormat_);
    if (textFormat_.get()) textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    CreateFormat(dwriteFactory_.get(), 13.0f * scale, DWRITE_FONT_WEIGHT_NORMAL,
        typography::FontRole::Text, fileNameFormat_);
    if (fileNameFormat_.get()) fileNameFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    CreateFormat(dwriteFactory_.get(), 12.0f * scale, DWRITE_FONT_WEIGHT_NORMAL,
        typography::FontRole::Text, smallFormat_);
    if (smallFormat_.get()) smallFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    CreateFormat(dwriteFactory_.get(), 14.0f * scale, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        typography::FontRole::Display, headerFormat_);
    CreateFormat(dwriteFactory_.get(), 13.0f * scale, DWRITE_FONT_WEIGHT_NORMAL,
        typography::FontRole::Text, tabFormat_);
    if (tabFormat_.get()) {
        tabFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        tabFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    CreateFormat(dwriteFactory_.get(), 14.0f * scale, DWRITE_FONT_WEIGHT_NORMAL,
        typography::FontRole::Text, addressFormat_);

    // Icon font for Fluent glyphs.
    typography::CreateTextFormat(dwriteFactory_.get(),
        {typography::FontRole::Icon, 16.0f * scale}, &iconFormat_);
    if (iconFormat_.get()) {
        iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

} // namespace pulse::ui

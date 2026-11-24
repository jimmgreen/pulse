#include "host/render/host_backdrop_layer.h"
#include "../../../../src/common/os_version.h"
#include "../../../../src/ui/window_material.h"
#include <dwmapi.h>
#include <DispatcherQueue.h>
#include <roapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <windows.ui.composition.interop.h>
#include <windows.graphics.effects.interop.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <d2d1effects.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace pulse::desktop::render {
namespace {
using namespace winrt;
using namespace winrt::Windows::UI::Composition;
namespace effects_abi = ABI::Windows::Graphics::Effects;
using winrt::Windows::Graphics::Effects::IGraphicsEffect;
using winrt::Windows::Graphics::Effects::IGraphicsEffectSource;

// Only the graph description is custom: Composition executes the built-in D2D
// blend effect. Brush inputs keep material updates independent of geometry.
struct MaterialBlend : winrt::implements<MaterialBlend, IGraphicsEffect,
        IGraphicsEffectSource, effects_abi::IGraphicsEffectD2D1Interop> {
    MaterialBlend(D2D1_BLEND_MODE mode, IGraphicsEffectSource background,
                  IGraphicsEffectSource foreground)
        : mode_(mode), background_(background), foreground_(foreground) {}
    winrt::hstring Name() const { return name_; }
    void Name(winrt::hstring const& value) { name_ = value; }
    HRESULT __stdcall GetEffectId(GUID* id) noexcept override {
        if (!id) return E_POINTER;
        *id = CLSID_D2D1Blend;
        return S_OK;
    }
    HRESULT __stdcall GetNamedPropertyMapping(LPCWSTR name, UINT* index,
            effects_abi::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) noexcept override {
        if (!name || !index || !mapping) return E_POINTER;
        if (wcscmp(name, L"Mode") != 0) return E_INVALIDARG;
        *index = D2D1_BLEND_PROP_MODE;
        *mapping = effects_abi::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
        return S_OK;
    }
    HRESULT __stdcall GetPropertyCount(UINT* count) noexcept override {
        if (!count) return E_POINTER;
        *count = 1;
        return S_OK;
    }
    HRESULT __stdcall GetProperty(UINT index,
            ABI::Windows::Foundation::IPropertyValue** value) noexcept override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (index != D2D1_BLEND_PROP_MODE) return E_INVALIDARG;
        try {
            auto property = winrt::Windows::Foundation::PropertyValue::CreateUInt32(mode_);
            return winrt::get_unknown(property)->QueryInterface(IID_PPV_ARGS(value));
        } catch (...) { return winrt::to_hresult(); }
    }
    HRESULT __stdcall GetSource(UINT index,
            effects_abi::IGraphicsEffectSource** source) noexcept override {
        if (!source) return E_POINTER;
        *source = nullptr;
        if (index > 1) return E_INVALIDARG;
        return winrt::get_unknown(index == 0 ? background_ : foreground_)
            ->QueryInterface(IID_PPV_ARGS(source));
    }
    HRESULT __stdcall GetSourceCount(UINT* count) noexcept override {
        if (!count) return E_POINTER;
        *count = 2;
        return S_OK;
    }
private:
    D2D1_BLEND_MODE mode_;
    IGraphicsEffectSource background_, foreground_;
    winrt::hstring name_;
};
bool EnsureDispatcherQueue() {
    struct Apartment {
        HRESULT hr = RoInitialize(RO_INIT_SINGLETHREADED);
        ~Apartment() { if (SUCCEEDED(hr)) RoUninitialize(); }
    };
    static thread_local Apartment apartment;
    if (FAILED(apartment.hr)) return false;
    // The host still starts on older Windows; CoreMessaging is an optional,
    // lazily loaded dependency and no Explorer window attributes are changed.
    static HMODULE module = LoadLibraryW(L"CoreMessaging.dll");
    if (!module) return false;
    using CreateQueue = HRESULT(WINAPI*)(DispatcherQueueOptions, ABI::Windows::System::IDispatcherQueueController**);
    const auto create = reinterpret_cast<CreateQueue>(GetProcAddress(module, "CreateDispatcherQueueController"));
    if (!create) return false;
    if (winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) return true;
    static thread_local winrt::Windows::System::DispatcherQueueController owner{nullptr};
    DispatcherQueueOptions opts{sizeof(opts), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
    return SUCCEEDED(create(opts, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(put_abi(owner))));
}
struct Card {
    float x, y, width, height, radius, opacity;
    bool operator==(const Card&) const = default;
};
}
struct HostBackdropLayer::Impl {
    HWND hwnd = nullptr;
    Compositor compositor{nullptr};
    winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget target{nullptr};
    ContainerVisual root{nullptr};
    CompositionBrush brush{nullptr};
    CompositionColorBrush tint{nullptr}, luminosity{nullptr};
    winrt::Windows::UI::Color current_tint{}, current_luminosity{};
    std::vector<Card> pending, current;
    bool recording = false;
    uint64_t updates = 0;
};
HostBackdropLayer::HostBackdropLayer() = default;
HostBackdropLayer::~HostBackdropLayer() { Reset(); }
bool HostBackdropLayer::Initialize(HWND hwnd) noexcept {
    Reset();
    // DWMWA_USE_HOSTBACKDROPBRUSH is Windows 11 Build 22000+ only; Windows 10 uses the fast
    // sampled blurred-wallpaper Acrylic path instead of full-work-area HostBackdrop blur.
    if (pulse::os::TrueWindowsBuild() < 22000) return false;
    BOOL previous_host_backdrop = FALSE;
    bool attribute_changed = false;
    try {
        if (!hwnd || (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) || !EnsureDispatcherQueue()) return false;
        DwmGetWindowAttribute(hwnd, DWMWA_USE_HOSTBACKDROPBRUSH, &previous_host_backdrop, sizeof(previous_host_backdrop));
        BOOL enabled = TRUE;
        winrt::check_hresult(DwmSetWindowAttribute(hwnd, DWMWA_USE_HOSTBACKDROPBRUSH, &enabled, sizeof(enabled)));
        attribute_changed = true;
        const MARGINS margins{-1, -1, -1, -1};
        winrt::check_hresult(DwmExtendFrameIntoClientArea(hwnd, &margins));
        auto state = std::make_unique<Impl>();
        state->hwnd = hwnd;
        state->compositor = Compositor();
        auto interop = state->compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
        winrt::check_hresult(interop->CreateDesktopWindowTarget(hwnd, FALSE,
            reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(winrt::put_abi(state->target))));
        state->root = state->compositor.CreateContainerVisual();
        state->brush = state->compositor.CreateHostBackdropBrush();
        state->target.Root(state->root);
        impl_ = std::move(state);
        ui::LogWindowMaterial("desktop live HostBackdropBrush active hwnd=%p", hwnd);
        return true;
    } catch (const winrt::hresult_error& error) {
        ui::LogWindowMaterial("desktop native acrylic unavailable hr=0x%08lX", static_cast<unsigned long>(error.code().value));
    } catch (...) {
        ui::LogWindowMaterial("desktop native acrylic unavailable unexpected initialization error");
    }
    if (attribute_changed && IsWindow(hwnd)) {
        DwmSetWindowAttribute(hwnd, DWMWA_USE_HOSTBACKDROPBRUSH, &previous_host_backdrop, sizeof(previous_host_backdrop));
        const MARGINS margins{};
        DwmExtendFrameIntoClientArea(hwnd, &margins);
    }
    return false;
}
bool HostBackdropLayer::SetMaterial(bool dark, float tint_opacity, float luminosity_opacity,
                                    uint32_t tint_argb) noexcept {
    if (!impl_) return false;
    try {
        if (!impl_->tint) {
            // Match WinUI's Acrylic graph: the historical Color/Luminosity
            // effect names are reversed; the ABI passes these numeric modes on.
            auto luminosity = winrt::make<MaterialBlend>(D2D1_BLEND_MODE_COLOR,
                CompositionEffectSourceParameter(L"Backdrop"), CompositionEffectSourceParameter(L"Luminosity"));
            auto tint = winrt::make<MaterialBlend>(D2D1_BLEND_MODE_LUMINOSITY,
                luminosity.as<IGraphicsEffectSource>(), CompositionEffectSourceParameter(L"Tint"));
            auto brush = impl_->compositor.CreateEffectFactory(tint).CreateBrush();
            impl_->tint = impl_->compositor.CreateColorBrush({0, 0, 0, 0});
            impl_->luminosity = impl_->compositor.CreateColorBrush({0, 0, 0, 0});
            brush.SetSourceParameter(L"Backdrop", impl_->brush);
            brush.SetSourceParameter(L"Luminosity", impl_->luminosity);
            brush.SetSourceParameter(L"Tint", impl_->tint);
            impl_->brush = brush;
            for (auto const& visual : impl_->root.Children())
                visual.as<SpriteVisual>().Brush(impl_->brush);
        }
        const auto alpha = [](float value) {
            if (!std::isfinite(value)) value = 0.0f;
            return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        (void)dark; // The caller supplies the theme-derived tint RGB.
        const winrt::Windows::UI::Color tint{alpha(tint_opacity * ((tint_argb >> 24) / 255.0f)),
            static_cast<uint8_t>(tint_argb >> 16), static_cast<uint8_t>(tint_argb >> 8),
            static_cast<uint8_t>(tint_argb)};
        const winrt::Windows::UI::Color luminosity{alpha(luminosity_opacity), tint.R, tint.G, tint.B};
        if (tint != impl_->current_tint) {
            impl_->tint.Color(tint);
            impl_->current_tint = tint;
        }
        if (luminosity != impl_->current_luminosity) {
            impl_->luminosity.Color(luminosity);
            impl_->current_luminosity = luminosity;
        }
        return true;
    } catch (const winrt::hresult_error& error) {
        ui::LogWindowMaterial("desktop native acrylic material update failed hr=0x%08lX", static_cast<unsigned long>(error.code().value));
    } catch (...) {}
    Reset();
    return false;
}
void HostBackdropLayer::Reset() noexcept {
    if (!impl_) return;
    try { impl_->target.Root(nullptr); impl_->target.Close(); } catch (...) {}
    if (IsWindow(impl_->hwnd)) {
        BOOL enabled = FALSE;
        DwmSetWindowAttribute(impl_->hwnd, DWMWA_USE_HOSTBACKDROPBRUSH, &enabled, sizeof(enabled));
        const MARGINS margins{};
        DwmExtendFrameIntoClientArea(impl_->hwnd, &margins);
    }
    impl_.reset();
}
void HostBackdropLayer::BeginFrame() noexcept {
    if (impl_) { impl_->pending.clear(); impl_->recording = true; }
}
bool HostBackdropLayer::AddCard(const D2D1_RECT_F& rect, float radius, float scale, float opacity) noexcept {
    if (!impl_ || !impl_->recording) return false;
    Card card{rect.left * scale, rect.top * scale, (rect.right - rect.left) * scale,
              (rect.bottom - rect.top) * scale, radius * scale, opacity};
    if (card.width <= 0 || card.height <= 0) return false;
    try {
        if (std::find(impl_->pending.begin(), impl_->pending.end(), card) == impl_->pending.end())
            impl_->pending.push_back(card);
        return true;
    } catch (...) {
        ui::LogWindowMaterial("desktop native acrylic card allocation failed; solid fallback");
        return false;
    }
}
void HostBackdropLayer::EndFrame() noexcept {
    if (!impl_ || !impl_->recording) return;
    impl_->recording = false;
    if (impl_->pending == impl_->current) return;
    try {
        impl_->root.Children().RemoveAll();
        for (const auto& card : impl_->pending) {
            auto sprite = impl_->compositor.CreateSpriteVisual();
            sprite.Offset({card.x, card.y, 0});
            sprite.Size({card.width, card.height});
            sprite.Opacity(card.opacity);
            sprite.Brush(impl_->brush);
            auto shape = impl_->compositor.CreateRoundedRectangleGeometry();
            shape.Size({card.width, card.height});
            shape.CornerRadius({card.radius, card.radius});
            sprite.Clip(impl_->compositor.CreateGeometricClip(shape));
            impl_->root.Children().InsertAtTop(sprite);
        }
        impl_->current = impl_->pending;
        ++impl_->updates;
    } catch (const winrt::hresult_error& error) {
        ui::LogWindowMaterial("desktop native acrylic geometry update failed hr=0x%08lX", static_cast<unsigned long>(error.code().value));
        Reset();
    } catch (...) { Reset(); }
}
bool HostBackdropLayer::Active() const noexcept { return impl_ != nullptr; }
uint32_t HostBackdropLayer::VisualCount() const noexcept { return impl_ ? static_cast<uint32_t>(impl_->current.size()) : 0; }
uint64_t HostBackdropLayer::UpdateCount() const noexcept { return impl_ ? impl_->updates : 0; }
}

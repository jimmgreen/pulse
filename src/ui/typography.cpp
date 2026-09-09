#include "typography.h"
#include "ui_compositor.h"
#include "../common/windows_compat.h"

#include "../common/localization.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace pulse::ui::typography {
namespace {

std::mutex g_fallback_mutex;
IDWriteFactory2* g_fallback_factory = nullptr;
IDWriteFontFallback* g_fallback = nullptr;
std::atomic_uint64_t g_generation{1};

template <typename T>
void Release(T*& value) noexcept {
    if (value) value->Release();
    value = nullptr;
}

bool FamilyExists(IDWriteFactory2* factory, const wchar_t* family) {
    if (!factory || !family) return false;
    IDWriteFontCollection* collection = nullptr;
    if (FAILED(factory->GetSystemFontCollection(&collection, FALSE)) || !collection) {
        return false;
    }
    UINT32 index = 0;
    BOOL exists = FALSE;
    const HRESULT result = collection->FindFamilyName(family, &index, &exists);
    collection->Release();
    return SUCCEEDED(result) && exists;
}

const wchar_t* ResolveFamily(IDWriteFactory2* factory, FontRole role) {
    const bool chinese = pulse::l10n::effective_language() == pulse::l10n::Language::ZhCN;
    const wchar_t* const text_zh[] = {
        L"Microsoft YaHei UI", L"Microsoft YaHei", L"Segoe UI", nullptr};
    const wchar_t* const text_en[] = {
        L"Segoe UI Variable Text", L"Segoe UI", L"Microsoft YaHei UI",
        L"Microsoft YaHei", nullptr};
    const wchar_t* const display_zh[] = {
        L"Microsoft YaHei UI", L"Microsoft YaHei", L"Segoe UI", nullptr};
    const wchar_t* const display_en[] = {
        L"Segoe UI Variable Display", L"Segoe UI Variable Text", L"Segoe UI",
        L"Microsoft YaHei UI", L"Microsoft YaHei", nullptr};
    const wchar_t* const icons[] = {
        L"Segoe Fluent Icons", L"Segoe MDL2 Assets", L"Segoe UI", nullptr};
    const wchar_t* const mono[] = {
        L"Cascadia Mono", L"Consolas", L"Microsoft YaHei UI", L"Segoe UI", nullptr};

    const wchar_t* const* candidates = nullptr;
    switch (role) {
    case FontRole::Display: candidates = chinese ? display_zh : display_en; break;
    case FontRole::Icon: candidates = icons; break;
    case FontRole::Monospace: candidates = mono; break;
    default: candidates = chinese ? text_zh : text_en; break;
    }
    for (size_t i = 0; candidates[i]; ++i) {
        if (FamilyExists(factory, candidates[i])) return candidates[i];
    }
    return L"Segoe UI";
}

bool BuildFallback(IDWriteFactory2* factory, IDWriteFontFallback** fallback) {
    if (!factory || !fallback) return false;
    *fallback = nullptr;
    IDWriteFontFallbackBuilder* builder = nullptr;
    if (FAILED(factory->CreateFontFallbackBuilder(&builder)) || !builder) return false;

    const DWRITE_UNICODE_RANGE cjk[] = {
        {0x2E80, 0x303F}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF},
        {0xF900, 0xFAFF}, {0xFF00, 0xFFEF}, {0x20000, 0x2FA1F},
    };
    const wchar_t* families[] = {L"Microsoft YaHei UI", L"Microsoft YaHei"};
    // Locale must be empty: a zh-CN mapping is skipped when the UI language
    // (and therefore the text format locale) is en-US, so Chinese in an
    // English shell would miss YaHei.
    builder->AddMapping(cjk, ARRAYSIZE(cjk), families, ARRAYSIZE(families),
                        nullptr, nullptr);

    IDWriteFontFallback* system = nullptr;
    if (SUCCEEDED(factory->GetSystemFontFallback(&system)) && system) {
        builder->AddMappings(system);
        system->Release();
    }
    const HRESULT result = builder->CreateFontFallback(fallback);
    builder->Release();
    return SUCCEEDED(result) && *fallback;
}

void ApplyFallbackInternal(IDWriteFactory2* factory, IDWriteTextFormat* format) {
    if (!factory || !format) return;
    std::lock_guard lock(g_fallback_mutex);
    if (g_fallback_factory != factory || !g_fallback) {
        Release(g_fallback);
        g_fallback_factory = factory;
        BuildFallback(factory, &g_fallback);
    }
    if (!g_fallback) return;
    IDWriteTextFormat1* format1 = nullptr;
    if (SUCCEEDED(format->QueryInterface(IID_PPV_ARGS(&format1))) && format1) {
        format1->SetFontFallback(g_fallback);
        format1->Release();
    }
}

} // namespace

const wchar_t* LocaleName() noexcept {
    return pulse::l10n::LocaleName();
}

bool HasIconFont(IDWriteFactory2* factory) {
    static const bool available = FamilyExists(factory, L"Segoe Fluent Icons") ||
                                   FamilyExists(factory, L"Segoe MDL2 Assets");
    return available;
}

const wchar_t* PreferredTextFamily() noexcept {
    return pulse::l10n::effective_language() == pulse::l10n::Language::ZhCN
        ? L"Microsoft YaHei UI" : L"Segoe UI Variable Text";
}

HRESULT CreateTextFormat(IDWriteFactory2* factory, const TextFormatSpec& spec,
                         IDWriteTextFormat** format) {
    if (!format) return E_POINTER;
    *format = nullptr;
    if (!factory || spec.size <= 0.0f) return E_INVALIDARG;
    const wchar_t* family = ResolveFamily(factory, spec.role);
    const wchar_t* locale = spec.role == FontRole::Icon ? L"en-US" : LocaleName();
    const HRESULT result = factory->CreateTextFormat(
        family, nullptr, spec.weight, spec.style, spec.stretch, spec.size, locale, format);
    if (SUCCEEDED(result) && *format && spec.role != FontRole::Icon) {
        ApplyFallbackInternal(factory, *format);
    }
    return result;
}

HRESULT CreateRenderingParams(IDWriteFactory2* factory, HMONITOR monitor,
                              IDWriteRenderingParams2** params) {
    if (!params) return E_POINTER;
    *params = nullptr;
    if (!factory) return E_INVALIDARG;

    IDWriteRenderingParams* base = nullptr;
    HRESULT result = monitor
        ? factory->CreateMonitorRenderingParams(monitor, &base)
        : factory->CreateRenderingParams(&base);
    if (FAILED(result) || !base) return result;

    float grayscale_contrast = base->GetEnhancedContrast();
    IDWriteRenderingParams2* base3 = nullptr;
    if (SUCCEEDED(base->QueryInterface(IID_PPV_ARGS(&base3))) && base3) {
        grayscale_contrast = base3->GetGrayscaleEnhancedContrast();
        base3->Release();
    }

    IDWriteFactory3* modern = nullptr;
    if (!compat::LegacyMode() && SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&modern)))) {
        IDWriteRenderingParams3* modern_params = nullptr;
        result = modern->CreateCustomRenderingParams(
            base->GetGamma(), base->GetEnhancedContrast(), grayscale_contrast,
            base->GetClearTypeLevel(), DWRITE_PIXEL_GEOMETRY_FLAT,
            DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC, DWRITE_GRID_FIT_MODE_DISABLED,
            &modern_params);
        modern->Release();
        if (SUCCEEDED(result) && modern_params) {
            *params = modern_params;
            base->Release();
            return result;
        }
    }
    result = factory->CreateCustomRenderingParams(
        base->GetGamma(), base->GetEnhancedContrast(), grayscale_contrast,
        base->GetClearTypeLevel(), DWRITE_PIXEL_GEOMETRY_FLAT,
        DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
        DWRITE_GRID_FIT_MODE_DISABLED, params);
    base->Release();
    return result;
}

void InvalidateCaches() {
    {
        std::lock_guard lock(g_fallback_mutex);
        Release(g_fallback);
        g_fallback_factory = nullptr;
    }
    g_generation.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t Generation() noexcept {
    return g_generation.load(std::memory_order_relaxed);
}

D2D1_RECT_F SnapVerticalBounds(const D2D1_RECT_F& bounds) noexcept {
    D2D1_RECT_F snapped = bounds;
    snapped.top = std::round(bounds.top);
    snapped.bottom = std::max(snapped.top + 1.0f, std::round(bounds.bottom));
    return snapped;
}

float InkPad(IDWriteTextFormat* format) noexcept {
    const float size = format ? format->GetFontSize() : 14.0f;
    return std::max(4.0f, size * 0.25f);
}

float MeasureAdvance(IDWriteFactory2* factory, IDWriteTextFormat* format,
                     std::wstring_view text) {
    if (!factory || !format || text.empty()) return 0.0f;
    const float fallback = format->GetFontSize() * static_cast<float>(text.size());
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
                                         format, 10000.0f, 100.0f, &layout)) || !layout) {
        return fallback;
    }
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS metrics{};
    DWRITE_OVERHANG_METRICS overhang{};
    const HRESULT metrics_hr = layout->GetMetrics(&metrics);
    layout->GetOverhangMetrics(&overhang);
    layout->Release();
    if (FAILED(metrics_hr)) return fallback;
    return metrics.widthIncludingTrailingWhitespace + std::max(0.0f, overhang.right) + 1.0f;
}

float MeasureLine(Compositor* compositor, IDWriteTextFormat* format,
                  std::wstring_view text) {
    const float dwrite_w = MeasureAdvance(
        compositor ? compositor->DwriteFactory() : nullptr, format, text);
    float luma = 0.0f;
    if (compositor && compositor->MeasureLumaText(text, format, luma) && luma > 0.0f) {
        return std::max(dwrite_w, luma) + InkPad(format);
    }
    return dwrite_w;
}

float MeasureWrapped(IDWriteFactory2* factory, IDWriteTextFormat* format,
                     std::wstring_view text, float width) {
    if (!factory || !format || text.empty() || width <= 0.0f) return 0.0f;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
                                         format, width, 4096.0f, &layout)) || !layout) {
        return format->GetFontSize() * 1.4f;
    }
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    DWRITE_TEXT_METRICS metrics{};
    const HRESULT hr = layout->GetMetrics(&metrics);
    layout->Release();
    if (FAILED(hr) || metrics.height <= 0.0f) return format->GetFontSize() * 1.4f;
    return std::ceil(metrics.height);
}

} // namespace pulse::ui::typography

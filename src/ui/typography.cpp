#include "typography.h"
#include "ui_compositor.h"
#include "../common/windows_compat.h"

#include "../common/localization.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace pulse::ui::typography {
namespace {

std::mutex g_fallback_mutex;
IDWriteFactory2* g_fallback_factory = nullptr;
IDWriteFontFallback* g_fallback = nullptr;
bool g_fallback_traditional = false;

bool TraditionalChinese() noexcept {
    return pulse::l10n::effective_language() == pulse::l10n::Language::ZhTW;
}
std::atomic_uint64_t g_generation{1};
std::atomic_int g_text_render_mode{0};
std::atomic_int g_ui_font_scale{100};
std::mutex g_family_mutex;
std::wstring g_ui_font_family;
// The picker asks for the family list every time it opens; walking every
// installed family costs milliseconds, so keep the last build until the locale
// or the font collection changes.
std::mutex g_families_mutex;
std::wstring g_families_locale;
std::vector<FontFamilyChoice> g_families;

template <typename T>
void Release(T*& value) noexcept {
    if (value) value->Release();
    value = nullptr;
}

std::wstring ReadFamilyName(IDWriteLocalizedStrings* names, UINT32 index) {
    UINT32 length = 0;
    if (!names || FAILED(names->GetStringLength(index, &length))) return {};
    std::wstring text(length + 1, L'\0');
    if (FAILED(names->GetString(index, text.data(), length + 1))) return {};
    text.resize(length);
    return text;
}

// The family name in `locale`, falling back to the English name and then to
// whatever the collection lists first.
std::wstring LocalizedFamilyName(IDWriteFontFamily* family, const wchar_t* locale) {
    IDWriteLocalizedStrings* names = nullptr;
    if (!family || FAILED(family->GetFamilyNames(&names)) || !names) return {};
    UINT32 index = 0;
    BOOL found = FALSE;
    if (FAILED(names->FindLocaleName(locale, &index, &found)) || !found) {
        if (FAILED(names->FindLocaleName(L"en-us", &index, &found)) || !found) index = 0;
    }
    std::wstring text = ReadFamilyName(names, index);
    names->Release();
    return text;
}

// A face that draws Latin letters belongs in the picker; icon and symbol faces
// do not. Chinese coverage only decides the order: text in a Chinese file name
// still renders through the fallback when the chosen family lacks it.
bool CoversLatinText(IDWriteFontFamily* family) {
    if (!family) return false;
    IDWriteFont* font = nullptr;
    if (FAILED(family->GetFont(0, &font)) || !font) return false;
    BOOL latin = FALSE;
    font->HasCharacter(L'A', &latin);
    font->Release();
    return latin;
}

bool CoversChineseText(IDWriteFontFamily* family) {
    if (!family) return false;
    IDWriteFont* font = nullptr;
    if (FAILED(family->GetFont(0, &font)) || !font) return false;
    BOOL cjk = FALSE;
    font->HasCharacter(0x4E2D, &cjk);
    font->Release();
    return cjk;
}

// Glyph and symbol collections would turn the interface into boxes.
bool SymbolFace(const std::wstring& name) {
    static const wchar_t* const markers[] = {
        L"Wingdings", L"Webdings", L"Marlett", L"Symbol", L"Emoji", L"Icons",
        L"MDL2", L"Fluent Icons", L"Bookshelf Symbol",
    };
    for (const wchar_t* marker : markers) {
        if (name.find(marker) != std::wstring::npos) return true;
    }
    return false;
}

// Order the picker by what a Chinese UI reaches for first, then by name.
int FamilyRank(const std::wstring& name) {
    static const wchar_t* const preferred[] = {
        L"Microsoft YaHei UI", L"Microsoft YaHei", L"Microsoft JhengHei UI",
        L"Microsoft JhengHei", L"Segoe UI Variable Text", L"Segoe UI",
        L"DengXian", L"SimSun", L"NSimSun", L"SimHei", L"KaiTi", L"FangSong",
        L"Noto Sans SC", L"Source Han Sans SC",
    };
    for (size_t i = 0; i < ARRAYSIZE(preferred); ++i) {
        if (CompareStringOrdinal(name.c_str(), -1, preferred[i], -1, TRUE) == CSTR_EQUAL)
            return static_cast<int>(i);
    }
    return static_cast<int>(ARRAYSIZE(preferred));
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

std::wstring ResolveFamily(IDWriteFactory2* factory, FontRole role) {
    const bool traditional = TraditionalChinese();
    // Taiwan glyph forms; YaHei only if JhengHei is missing.
    const wchar_t* const text_tw[] = {
        L"Microsoft JhengHei UI", L"Microsoft JhengHei", L"Microsoft YaHei UI", L"Segoe UI", nullptr};
    // Simplified Chinese uses Segoe UI Variable for Latin too; Han text
    // reaches PingFang SC / YaHei through the fallback in BuildFallback.
    const wchar_t* const text_en[] = {
        L"Segoe UI Variable Text", L"Segoe UI", L"Microsoft YaHei UI",
        L"Microsoft YaHei", nullptr};
    const wchar_t* const display_en[] = {
        L"Segoe UI Variable Display", L"Segoe UI Variable Text", L"Segoe UI",
        L"Microsoft YaHei UI", L"Microsoft YaHei", nullptr};
    const wchar_t* const icons[] = {
        L"Segoe Fluent Icons", L"Segoe MDL2 Assets", L"Segoe UI", nullptr};
    const wchar_t* const mono[] = {
        L"Cascadia Mono", L"Consolas", L"Microsoft YaHei UI", L"Segoe UI", nullptr};
    const wchar_t* const mono_tw[] = {
        L"Cascadia Mono", L"Consolas", L"Microsoft JhengHei UI", L"Segoe UI", nullptr};

    const wchar_t* const* candidates = nullptr;
    switch (role) {
    case FontRole::Display: candidates = traditional ? text_tw : display_en; break;
    case FontRole::Icon: candidates = icons; break;
    case FontRole::Monospace: candidates = traditional ? mono_tw : mono; break;
    default: candidates = traditional ? text_tw : text_en; break;
    }
    // The user's interface font replaces the built-in cascade for text and
    // display only; icons need their glyph face and code needs its monospace
    // one. A family that is no longer installed falls through to the cascade.
    if (role == FontRole::Text || role == FontRole::Display) {
        const std::wstring chosen = UiFontFamily();
        if (!chosen.empty() && FamilyExists(factory, chosen.c_str())) return chosen;
    }
    for (size_t i = 0; candidates[i]; ++i) {
        if (FamilyExists(factory, candidates[i])) return candidates[i];
    }
    return L"Segoe UI";
}

bool BuildFallback(IDWriteFactory2* factory, bool traditional, IDWriteFontFallback** fallback) {
    if (!factory || !fallback) return false;
    *fallback = nullptr;
    IDWriteFontFallbackBuilder* builder = nullptr;
    if (FAILED(factory->CreateFontFallbackBuilder(&builder)) || !builder) return false;

    const DWRITE_UNICODE_RANGE cjk[] = {
        {0x2E80, 0x303F}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF},
        {0xF900, 0xFAFF}, {0xFF00, 0xFFEF}, {0x20000, 0x2FA1F},
    };
    // PingFang SC only when the user installed it (Pulse never ships it).
    const bool pingfang = FamilyExists(factory, L"PingFang SC");
    const wchar_t* simplified_pingfang[] = {L"PingFang SC", L"Microsoft YaHei UI", L"Microsoft YaHei"};
    const wchar_t** simplified = pingfang ? simplified_pingfang : simplified_pingfang + 1;
    const UINT32 simplified_count = pingfang ? 3u : 2u;
    const wchar_t* hant[] = {L"Microsoft JhengHei UI", L"Microsoft JhengHei",
                             L"Microsoft YaHei UI", L"Microsoft YaHei"};
    // Locale must be empty: a zh-CN mapping is skipped when the UI language
    // (and therefore the text format locale) is en-US, so Chinese in an
    // English shell would miss YaHei.
    if (traditional)
        builder->AddMapping(cjk, ARRAYSIZE(cjk), hant, ARRAYSIZE(hant), nullptr, nullptr);
    else
        builder->AddMapping(cjk, ARRAYSIZE(cjk), simplified, simplified_count, nullptr, nullptr);

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
    const bool traditional = TraditionalChinese();
    if (g_fallback_factory != factory || !g_fallback || g_fallback_traditional != traditional) {
        Release(g_fallback);
        g_fallback_factory = factory;
        g_fallback_traditional = traditional;
        BuildFallback(factory, traditional, &g_fallback);
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

const wchar_t* PreferredTextFamily() {
    // thread_local so the pointer stays valid for the caller: CreateEditFont
    // reads it immediately, but the old per-language literals could be
    // returned from any thread.
    static thread_local std::wstring chosen;
    chosen = UiFontFamily();
    if (!chosen.empty()) return chosen.c_str();
    switch (pulse::l10n::effective_language()) {
    case pulse::l10n::Language::ZhCN: return L"Microsoft YaHei UI";
    case pulse::l10n::Language::ZhTW: return L"Microsoft JhengHei UI";
    default: return L"Segoe UI Variable Text";
    }
}

HRESULT CreateTextFormat(IDWriteFactory2* factory, const TextFormatSpec& spec,
                         IDWriteTextFormat** format) {
    if (!format) return E_POINTER;
    *format = nullptr;
    if (!factory || spec.size <= 0.0f) return E_INVALIDARG;
    const std::wstring family = ResolveFamily(factory, spec.role);
    const wchar_t* locale = spec.role == FontRole::Icon ? L"en-US" : LocaleName();
    const float size = spec.role == FontRole::Icon ? spec.size : spec.size * UiFontScale();
    const HRESULT result = factory->CreateTextFormat(
        family.c_str(), nullptr, spec.weight, spec.style, spec.stretch, size, locale, format);
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

    // Sharp hints glyphs to the pixel grid (GDI-style stems) so small UI text
    // stays crisp at 100-125% scaling. Auto keeps symmetric smoothing but
    // grid-fits vertically like the LumaText path; Smooth stays unhinted.
    const bool sharp = CurrentTextRenderMode() == TextRenderMode::Sharp;
    const bool grid_fit = CurrentTextRenderMode() != TextRenderMode::Smooth;
    IDWriteFactory3* modern = nullptr;
    if (!compat::LegacyMode() && SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&modern)))) {
        IDWriteRenderingParams3* modern_params = nullptr;
        result = modern->CreateCustomRenderingParams(
            base->GetGamma(), base->GetEnhancedContrast(), grayscale_contrast,
            base->GetClearTypeLevel(), DWRITE_PIXEL_GEOMETRY_FLAT,
            sharp ? DWRITE_RENDERING_MODE1_GDI_CLASSIC : DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC,
            grid_fit ? DWRITE_GRID_FIT_MODE_ENABLED : DWRITE_GRID_FIT_MODE_DISABLED,
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
        sharp ? DWRITE_RENDERING_MODE_GDI_CLASSIC : DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
        grid_fit ? DWRITE_GRID_FIT_MODE_ENABLED : DWRITE_GRID_FIT_MODE_DISABLED, params);
    base->Release();
    return result;
}

void SetTextRenderMode(TextRenderMode mode) noexcept {
    const int value = std::clamp(static_cast<int>(mode), 0, 2);
    if (g_text_render_mode.exchange(value, std::memory_order_relaxed) != value) {
        g_generation.fetch_add(1, std::memory_order_relaxed);
        EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
            SendMessageW(window, TextBackendChangedMessage(), 0, 0);
            EnumChildWindows(window, [](HWND child, LPARAM) -> BOOL {
                SendMessageW(child, TextBackendChangedMessage(), 0, 0);
                return TRUE;
            }, 0);
            return TRUE;
        }, 0);
    }
}

UINT TextBackendChangedMessage() noexcept {
    static const UINT message = RegisterWindowMessageW(L"Pulse.TextBackendChanged");
    return message;
}

TextRenderMode CurrentTextRenderMode() noexcept {
    return static_cast<TextRenderMode>(g_text_render_mode.load(std::memory_order_relaxed));
}

void SetUiFontScale(int percent) noexcept {
    const int value = percent == 90 || percent == 112 || percent == 125 ? percent : 100;
    if (g_ui_font_scale.exchange(value, std::memory_order_relaxed) != value)
        g_generation.fetch_add(1, std::memory_order_relaxed);
}

int UiFontScalePercent() noexcept {
    return g_ui_font_scale.load(std::memory_order_relaxed);
}

float UiFontScale() noexcept {
    return static_cast<float>(UiFontScalePercent()) / 100.0f;
}

void SetUiFontFamily(std::wstring family) {
    {
        std::lock_guard lock(g_family_mutex);
        if (g_ui_font_family == family) return;
        g_ui_font_family = std::move(family);
    }
    g_generation.fetch_add(1, std::memory_order_relaxed);
}

std::wstring UiFontFamily() {
    std::lock_guard lock(g_family_mutex);
    return g_ui_font_family;
}

std::vector<FontFamilyChoice> UiFontFamilies(IDWriteFactory2* factory) {
    const std::wstring locale = LocaleName();
    {
        std::lock_guard lock(g_families_mutex);
        if (!g_families.empty() && g_families_locale == locale) return g_families;
    }
    std::vector<FontFamilyChoice> choices;
    // The built-in cascade leads, with an empty label the settings UI fills in
    // with its own wording (this layer has no business owning UI strings).
    choices.push_back(FontFamilyChoice{});
    if (!factory) return choices;

    IDWriteFontCollection* collection = nullptr;
    if (FAILED(factory->GetSystemFontCollection(&collection, FALSE)) || !collection)
        return choices;

    struct Entry {
        std::wstring value;
        std::wstring label;
        int rank = 0;
        bool chinese = false;
    };
    std::vector<Entry> entries;
    const UINT32 count = collection->GetFontFamilyCount();
    entries.reserve(count);
    for (UINT32 i = 0; i < count; ++i) {
        IDWriteFontFamily* family = nullptr;
        if (FAILED(collection->GetFontFamily(i, &family)) || !family) continue;
        if (!CoversLatinText(family)) {
            family->Release();
            continue;
        }
        Entry entry;
        entry.chinese = CoversChineseText(family);
        entry.value = LocalizedFamilyName(family, L"en-us");
        entry.label = LocalizedFamilyName(family, LocaleName());
        family->Release();
        if (entry.value.empty()) continue;
        if (SymbolFace(entry.value)) continue;
        if (entry.label.empty()) entry.label = entry.value;
        entry.rank = FamilyRank(entry.value);
        entries.push_back(std::move(entry));
    }
    collection->Release();

    // Faces that draw Chinese first, so a Chinese interface does not have to
    // scroll past Latin-only serifs; the preferred few lead, then by name.
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.chinese != b.chinese) return a.chinese;
        if (a.rank != b.rank) return a.rank < b.rank;
        return CompareStringOrdinal(a.value.c_str(), -1, b.value.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    choices.reserve(choices.size() + entries.size());
    for (Entry& entry : entries) {
        FontFamilyChoice choice;
        choice.value = std::move(entry.value);
        choice.label = std::move(entry.label);
        choices.push_back(std::move(choice));
    }
    {
        std::lock_guard lock(g_families_mutex);
        g_families = choices;
        g_families_locale = locale;
    }
    return choices;
}

std::wstring UiFontFamilyLabel(IDWriteFactory2* factory, const std::wstring& family) {
    if (family.empty()) return {};
    const auto find = [&family] {
        std::lock_guard lock(g_families_mutex);
        for (const FontFamilyChoice& choice : g_families) {
            if (choice.value == family) return choice.label;
        }
        return std::wstring();
    };
    const std::wstring cached = find();
    if (!cached.empty()) return cached;
    // Cold cache (first settings frame of the session): build what the picker
    // offers, then look the saved family up in it.
    UiFontFamilies(factory);
    return find();
}

void InvalidateCaches() {
    {
        std::lock_guard lock(g_fallback_mutex);
        Release(g_fallback);
        g_fallback_factory = nullptr;
    }
    {
        // A family installed while the process runs shows up after the next
        // typography reset.
        std::lock_guard lock(g_families_mutex);
        g_families.clear();
        g_families_locale.clear();
    }
    g_generation.fetch_add(1, std::memory_order_relaxed);
}

void InvalidateMeasurements() noexcept {
    g_generation.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t Generation() noexcept {
    return g_generation.load(std::memory_order_relaxed);
}

int EditFontPixels(float scale, float dip) noexcept {
    return std::max(1, static_cast<int>(std::lround(dip * UiFontScale() * std::max(0.25f, scale))));
}

HFONT CreateEditFont(float scale, float dip) {
    // Grayscale like the DirectWrite text (flat pixel geometry), not ClearType.
    const int height = -EditFontPixels(scale, dip);
    HFONT font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, PreferredTextFamily());
    if (!font) {
        font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
    return font;
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

namespace {

float MeasureLineUncached(Compositor* compositor, IDWriteTextFormat* format,
                          std::wstring_view text) {
    const float dwrite_w = MeasureAdvance(
        compositor ? compositor->DwriteFactory() : nullptr, format, text);
    float luma = 0.0f;
    if (compositor && compositor->MeasureLumaText(text, format, luma) && luma > 0.0f) {
        return std::max(dwrite_w, luma) + InkPad(format);
    }
    return dwrite_w;
}

struct MeasureKey {
    const void* compositor = nullptr;
    const void* format = nullptr;
    float size = 0.0f;
    std::uint32_t style = 0; // weight | style << 16 | stretch << 20 | luma << 24
    std::wstring text;
    bool operator==(const MeasureKey& other) const noexcept {
        return compositor == other.compositor && format == other.format &&
               size == other.size && style == other.style && text == other.text;
    }
};

struct MeasureKeyHash {
    size_t operator()(const MeasureKey& key) const noexcept {
        size_t h = std::hash<std::wstring>{}(key.text);
        const auto mix = [&h](size_t v) {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        };
        mix(std::hash<const void*>{}(key.compositor));
        mix(std::hash<const void*>{}(key.format));
        mix(std::hash<float>{}(key.size));
        mix(key.style);
        return h;
    }
};

struct MeasureCache {
    std::uint64_t generation = 0;
    std::unordered_map<MeasureKey, float, MeasureKeyHash> widths;
};

constexpr size_t kMeasureCacheLimit = 16384;

} // namespace

float MeasureLine(Compositor* compositor, IDWriteTextFormat* format,
                  std::wstring_view text) {
    if (!format || text.empty() || text.size() > 1024)
        return MeasureLineUncached(compositor, format, text);
    // Text measurement runs for every visible row on every paint and on each
    // hit test; the inputs rarely change, so remember the widths per thread.
    thread_local MeasureCache cache;
    const std::uint64_t generation = Generation();
    if (cache.generation != generation) {
        cache.widths.clear();
        cache.generation = generation;
    }
    MeasureKey key;
    key.compositor = compositor;
    key.format = format;
    key.size = format->GetFontSize();
    key.style = static_cast<std::uint32_t>(format->GetFontWeight()) & 0xFFFFu;
    key.style |= (static_cast<std::uint32_t>(format->GetFontStyle()) & 0xFu) << 16;
    key.style |= (static_cast<std::uint32_t>(format->GetFontStretch()) & 0xFu) << 20;
    if (compositor && compositor->LumaTextEnabled()) key.style |= 1u << 24;
    key.text.assign(text.data(), text.size());
    if (const auto found = cache.widths.find(key); found != cache.widths.end())
        return found->second;
    const float width = MeasureLineUncached(compositor, format, text);
    if (cache.widths.size() >= kMeasureCacheLimit) cache.widths.clear();
    cache.widths.emplace(std::move(key), width);
    return width;
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

#include "lumatext_renderer.h"
#include "optional_lumatext.h"
#include "typography.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <imm.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <wrl/client.h>

#if defined(PULSE_HAS_LUMATEXT)
#include <lumatext/lumatext.hpp>
#endif

#pragma comment(lib, "imm32.lib")

namespace pulse::ui {
namespace {

bool EnvironmentEnabled() noexcept {
    wchar_t value[16]{};
    const DWORD length = GetEnvironmentVariableW(L"PULSE_LUMATEXT", value, ARRAYSIZE(value));
    if (length == 0 || length >= ARRAYSIZE(value)) return true;
    return _wcsicmp(value, L"0") != 0 && _wcsicmp(value, L"off") != 0 &&
           _wcsicmp(value, L"false") != 0;
}

std::wstring FontPath(const wchar_t* file_name) {
    wchar_t windows[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
    if (length == 0 || length >= ARRAYSIZE(windows)) return {};
    return (std::filesystem::path(windows) / L"Fonts" / file_name).wstring();
}

std::wstring FontFamily(IDWriteTextFormat* format) {
    if (!format) return {};
    const UINT32 length = format->GetFontFamilyNameLength();
    if (length == 0 || length > 256) return {};
    std::wstring result(length + 1, L'\0');
    if (FAILED(format->GetFontFamilyName(result.data(), length + 1))) return {};
    result.resize(length);
    return result;
}

bool Contains(const std::wstring& value, const wchar_t* fragment) {
    return value.find(fragment) != std::wstring::npos;
}

} // namespace

struct LumaTextRenderer::Impl {
    LumaTextStats stats;

#if defined(PULSE_HAS_LUMATEXT)
    struct LayoutKey {
        std::wstring text;
        std::uint32_t family = 0;
        std::uint16_t weight = 400;
        std::int32_t size_64 = 0;
        std::int32_t width_64 = 0;
        DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        lt_text_ellipsis ellipsis = LT_TEXT_ELLIPSIS_NONE;

        bool operator==(const LayoutKey&) const = default;
    };

    struct LayoutKeyHash {
        size_t operator()(const LayoutKey& key) const noexcept {
            size_t value = std::hash<std::wstring>{}(key.text);
            const auto combine = [&value](size_t next) {
                value ^= next + 0x9e3779b9u + (value << 6) + (value >> 2);
            };
            combine(std::hash<std::uint32_t>{}(key.family));
            combine(std::hash<std::uint16_t>{}(key.weight));
            combine(std::hash<std::int32_t>{}(key.size_64));
            combine(std::hash<std::int32_t>{}(key.width_64));
            combine(std::hash<int>{}(static_cast<int>(key.alignment)));
            combine(std::hash<int>{}(static_cast<int>(key.ellipsis)));
            return value;
        }
    };

    struct SurfaceKey {
        const lt_text_layout* layout = nullptr;
        // Keep the text identity alongside the native layout pointer. Layout
        // objects are LRU-evicted and their addresses can be reused; a stale
        // command list must never become a hit for a different string.
        std::size_t text_hash = 0;
        // The clip width is part of the recorded command list. Without it,
        // resizing a column can reuse a surface recorded at the old width.
        std::int32_t width_64 = 0;
        std::int32_t height_64 = 0;
        // LumaText's glyph cache has 1/8-pixel phase precision. Keeping the
        // surface key at 1/64 creates a new command list for every smooth
        // scroll tick even though the rasterized glyphs are identical.
        std::uint8_t x_phase_8 = 0;
        std::uint8_t y_phase_8 = 0;
        std::uint32_t foreground = 0;
        std::uint32_t background = 0;
        bool dark = false;

        bool operator==(const SurfaceKey&) const = default;
    };

    struct SurfaceKeyHash {
        size_t operator()(const SurfaceKey& key) const noexcept {
            size_t value = std::hash<const void*>{}(key.layout);
            const auto combine = [&value](size_t next) {
                value ^= next + 0x9e3779b9u + (value << 6) + (value >> 2);
            };
            combine(std::hash<std::size_t>{}(key.text_hash));
            combine(std::hash<std::int32_t>{}(key.width_64));
            combine(std::hash<std::int32_t>{}(key.height_64));
            combine(std::hash<std::uint32_t>{}(
                static_cast<std::uint32_t>(key.x_phase_8) << 24 |
                static_cast<std::uint32_t>(key.y_phase_8) << 16 |
                (key.dark ? 1u : 0u)));
            combine(std::hash<std::uint32_t>{}(key.foreground));
            combine(std::hash<std::uint32_t>{}(key.background));
            return value;
        }
    };

    struct SurfaceEntry {
        Microsoft::WRL::ComPtr<ID2D1CommandList> commands;
        std::uint64_t estimated_bytes = 0;
        std::list<SurfaceKey>::iterator lru;
    };

    struct LayoutSlot {
        LumaText::TextLayout layout;
        std::list<LayoutKey>::iterator lru;
    };

    LumaText::Context context;
    LumaText::Renderer renderer;
    LumaText::RenderProfile profile;
    LumaText::FontFace yahei_regular;
    LumaText::FontFace yahei_bold;
    LumaText::FontFace segoe_regular;
    LumaText::FontFace segoe_semibold;
    LumaText::FontFace segoe_bold;
    LumaText::FontCascade yahei_cascade;
    LumaText::FontCascade segoe_cascade;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> target_dc;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> recording_dc;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> blit_dc;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> blit_target;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> blit_staging;
    int blit_w = 0;
    int blit_h = 0;
    HDC present_dc = nullptr;
    HBITMAP present_dib = nullptr;
    HGDIOBJ present_old = nullptr;
    void* present_bits = nullptr;
    int present_w = 0;
    int present_h = 0;
    bool busy = false;
    HWND mouse_hwnd = nullptr;
    int mouse_anchor = 0;
    int mouse_caret = 0;
    static constexpr float kEditPad = 2.0f;
    std::unordered_map<LayoutKey, LayoutSlot, LayoutKeyHash> layouts;
    std::list<LayoutKey> layout_lru;
    std::unordered_map<SurfaceKey, SurfaceEntry, SurfaceKeyHash> surfaces;
    std::list<SurfaceKey> surface_lru;
    std::uint64_t surface_bytes = 0;

    static constexpr std::uint32_t kYaHei = 1;
    static constexpr std::uint32_t kSegoe = 2;
    // Each Compositor owns a renderer. Keep per-window caches deliberately
    // small so opening Quick Look or an operation dialog cannot multiply a
    // large global-looking font cache into the process working set.
    // Keep enough layouts for a full multi-pane frame. Evicting a layout also
    // evicts every command-list surface that references it, so a tiny layout
    // limit turns an otherwise warm frame into repeated rasterization.
    static constexpr size_t kLayoutCacheLimit = 512;
    static constexpr size_t kSurfaceCountLimit = 512;
    static constexpr std::uint64_t kSurfaceCacheLimit = 8ull * 1024ull * 1024ull;
    static constexpr std::uint64_t kGlyphCacheLimit = 512ull * 1024ull;
    static constexpr std::uint64_t kMaxCachedSurfaceBytes = 64ull * 1024ull;
    static constexpr std::int32_t kUnboundedWidth64 = 10000 * 64;

    static constexpr std::uint32_t Tag(char a, char b, char c, char d) noexcept {
        return (static_cast<std::uint32_t>(a) << 24) |
               (static_cast<std::uint32_t>(b) << 16) |
               (static_cast<std::uint32_t>(c) << 8) |
               static_cast<std::uint32_t>(d);
    }

    bool MakeFace(const std::wstring& path, LumaText::FontFace& face,
                  float variable_weight = 0.0f) {
        if (path.empty() || !std::filesystem::exists(path)) return false;
        lt_font_axis axis{Tag('w', 'g', 'h', 't'), variable_weight};
        auto desc = LumaText::Descriptor<lt_font_source_desc>();
        desc.source_type = LT_FONT_SOURCE_FILE;
        desc.file_path = path.c_str();
        if (variable_weight > 0.0f) {
            desc.axes = &axis;
            desc.axis_count = 1;
        }
        return lt_font_face_create(context.get(), &desc, face.put()) == LT_OK;
    }

    bool MakeCascade(const std::vector<lt_font_cascade_entry>& entries,
                     LumaText::FontCascade& cascade) {
        auto desc = LumaText::Descriptor<lt_font_cascade_desc>();
        desc.entries = entries.data();
        desc.entry_count = static_cast<std::uint32_t>(entries.size());
        desc.allow_system_fallback = false;
        return lt_font_cascade_create(context.get(), &desc, cascade.put()) == LT_OK;
    }

    static void PushFace(std::vector<lt_font_cascade_entry>& entries,
                         const LumaText::FontFace& face, std::uint16_t weight) {
        if (face) entries.push_back({face.get(), weight, 0});
    }

    void ClearSurfaces() {
        surfaces.clear();
        surface_lru.clear();
        surface_bytes = 0;
    }

    void DropSurfacesFor(const lt_text_layout* layout) {
        if (!layout) return;
        for (auto it = surfaces.begin(); it != surfaces.end(); ) {
            if (it->first.layout != layout) {
                ++it;
                continue;
            }
            surface_bytes -= it->second.estimated_bytes;
            surface_lru.erase(it->second.lru);
            stats.surface_cache_evictions++;
            it = surfaces.erase(it);
        }
    }

    void TrimSurfaces() {
        while (!surfaces.empty() &&
               (surfaces.size() > kSurfaceCountLimit ||
                surface_bytes > kSurfaceCacheLimit)) {
            const SurfaceKey oldest = surface_lru.back();
            if (const auto found = surfaces.find(oldest); found != surfaces.end()) {
                surface_bytes -= found->second.estimated_bytes;
                surfaces.erase(found);
                stats.surface_cache_evictions++;
            }
            surface_lru.pop_back();
        }
    }

    void TouchSurface(std::unordered_map<SurfaceKey, SurfaceEntry, SurfaceKeyHash>::iterator it) {
        surface_lru.splice(surface_lru.begin(), surface_lru, it->second.lru);
        it->second.lru = surface_lru.begin();
    }

    void StoreSurface(const SurfaceKey& key,
                      Microsoft::WRL::ComPtr<ID2D1CommandList> commands,
                      std::uint64_t estimated_bytes) {
        if (estimated_bytes == 0 || estimated_bytes > kMaxCachedSurfaceBytes) return;
        if (auto existing = surfaces.find(key); existing != surfaces.end()) {
            surface_bytes -= existing->second.estimated_bytes;
            surface_lru.erase(existing->second.lru);
            surfaces.erase(existing);
        }
        surface_lru.push_front(key);
        surfaces.emplace(key, SurfaceEntry{std::move(commands), estimated_bytes,
                                           surface_lru.begin()});
        surface_bytes += estimated_bytes;
        TrimSurfaces();
    }

    void EvictOldestLayout() {
        if (layout_lru.empty()) return;
        const LayoutKey oldest = layout_lru.back();
        if (const auto found = layouts.find(oldest); found != layouts.end()) {
            DropSurfacesFor(found->second.layout.get());
            layouts.erase(found);
        }
        layout_lru.pop_back();
    }

    void ClearLayouts() {
        ClearSurfaces();
        layouts.clear();
        layout_lru.clear();
    }

    void FillLayoutDesc(lt_text_layout_desc& desc, const LayoutKey& key,
                        lt_text_style& style, float font_size,
                        LumaText::FontCascade& cascade) const {
        style = LumaText::Descriptor<lt_text_style>();
        style.cascade = cascade.get();
        style.font_size = font_size;
        style.weight = key.weight;
        desc = LumaText::Descriptor<lt_text_layout_desc>();
        desc.text = key.text.data();
        desc.text_length = static_cast<std::uint32_t>(key.text.size());
        desc.base_style = style;
        desc.locale = key.family == kYaHei ? "zh-CN" : "en-US";
        desc.direction = LT_TEXT_DIRECTION_AUTO;
        desc.alignment = key.alignment == DWRITE_TEXT_ALIGNMENT_CENTER
            ? LT_TEXT_ALIGNMENT_CENTER
            : key.alignment == DWRITE_TEXT_ALIGNMENT_TRAILING
                ? LT_TEXT_ALIGNMENT_END : LT_TEXT_ALIGNMENT_START;
        desc.ellipsis = key.ellipsis;
        desc.max_width = key.width_64 / 64.0f;
    }

    bool Init(IDWriteFactory* dwrite, ID2D1RenderTarget* target) {
        auto context_desc = LumaText::Descriptor<lt_context_desc>();
        context_desc.dwrite_factory = dwrite;
        context_desc.cpu_cache_limit_bytes = kGlyphCacheLimit;
        if (lt_context_create(&context_desc, context.put()) != LT_OK) return false;

        if (FAILED(target->QueryInterface(IID_PPV_ARGS(&target_dc))) || !target_dc) {
            return false;
        }
        Microsoft::WRL::ComPtr<ID2D1Device> device;
        target_dc->GetDevice(&device);
        if (!device || FAILED(device->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &recording_dc)) || !recording_dc) {
            return false;
        }
        recording_dc->SetDpi(96.0f, 96.0f);
        recording_dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        if (FAILED(device->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &blit_dc)) || !blit_dc) {
            return false;
        }
        blit_dc->SetDpi(96.0f, 96.0f);
        blit_dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        auto renderer_desc = LumaText::Descriptor<lt_d2d_desc>();
        renderer_desc.render_target = recording_dc.Get();
        renderer_desc.manage_begin_end_draw = false;
        if (lt_d2d_renderer_create(context.get(), &renderer_desc, renderer.put()) != LT_OK) {
            return false;
        }

        if (!MakeFace(FontPath(L"msyh.ttc"), yahei_regular) ||
            !MakeFace(FontPath(L"msyhbd.ttc"), yahei_bold)) {
            return false;
        }

        // Win11's segoeui.ttf is a ~200 KB stub; Segoe UI Variable holds the
        // real outlines. Two faces share one mapped file in LumaText.
        const std::wstring variable = FontPath(L"SegUIVar.ttf");
        if (!MakeFace(variable, segoe_regular, 400.0f) ||
            !MakeFace(variable, segoe_bold, 700.0f)) {
            segoe_regular.reset();
            segoe_bold.reset();
            if (!MakeFace(FontPath(L"segoeui.ttf"), segoe_regular) ||
                !MakeFace(FontPath(L"segoeuib.ttf"), segoe_bold)) {
                return false;
            }
        }

        std::vector<lt_font_cascade_entry> yahei_entries;
        PushFace(yahei_entries, yahei_regular, 400);
        PushFace(yahei_entries, yahei_bold, 600);
        PushFace(yahei_entries, yahei_bold, 700);
        if (yahei_entries.empty() || !MakeCascade(yahei_entries, yahei_cascade)) return false;

        std::vector<lt_font_cascade_entry> segoe_entries;
        PushFace(segoe_entries, segoe_regular, 400);
        if (segoe_semibold) {
            PushFace(segoe_entries, segoe_semibold, 600);
        } else {
            PushFace(segoe_entries, segoe_bold, 600);
        }
        PushFace(segoe_entries, segoe_bold, 700);
        PushFace(segoe_entries, yahei_regular, 400);
        PushFace(segoe_entries, yahei_bold, 700);
        if (segoe_entries.empty() || !MakeCascade(segoe_entries, segoe_cascade)) return false;

        auto profile_desc = LumaText::Descriptor<lt_render_profile_desc>();
        profile_desc.light = LumaText::Descriptor<lt_render_config>();
        profile_desc.light.coverage_gamma = 0.85f;
        profile_desc.light.coverage_contrast = 1.00f;
        profile_desc.light.raster_filter = LT_RASTER_FILTER_MITCHELL;
        profile_desc.dark = profile_desc.light;
        profile_desc.regular_optical_weight = 0.06f;
        profile_desc.bold_optical_weight = 0.0f;
        return lt_render_profile_create(&profile_desc, profile.put()) == LT_OK;
    }

    void ReleasePresent() noexcept {
        if (present_dc && present_old) SelectObject(present_dc, present_old);
        present_old = nullptr;
        if (present_dib) DeleteObject(present_dib);
        present_dib = nullptr;
        present_bits = nullptr;
        present_w = 0;
        present_h = 0;
        if (present_dc) DeleteDC(present_dc);
        present_dc = nullptr;
    }

    bool EnsurePresent(int w, int h) {
        if (present_dib && present_dc && present_bits && present_w == w && present_h == h) {
            return true;
        }
        if (present_dc && present_old) {
            SelectObject(present_dc, present_old);
            present_old = nullptr;
        }
        if (present_dib) {
            DeleteObject(present_dib);
            present_dib = nullptr;
            present_bits = nullptr;
        }
        if (!present_dc) present_dc = CreateCompatibleDC(nullptr);
        if (!present_dc || w <= 0 || h <= 0) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        present_dib = CreateDIBSection(present_dc, &info, DIB_RGB_COLORS, &present_bits,
                                       nullptr, 0);
        if (!present_dib || !present_bits) {
            if (present_dib) DeleteObject(present_dib);
            present_dib = nullptr;
            present_bits = nullptr;
            return false;
        }
        present_old = SelectObject(present_dc, present_dib);
        present_w = w;
        present_h = h;
        return true;
    }

    bool PresentEditBits(HWND hwnd, HDC paint_dc, const void* bits, int w, int h) {
        if (!hwnd || !bits || w <= 0 || h <= 0) return false;
        const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
        const bool layered =
            (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
        if (layered) {
            // Own the redirected bitmap. SetLayeredWindowAttributes + WM_PAINT
            // lets EDIT's GetDC/ClearType selection paint win on mouse-down.
            if (!EnsurePresent(w, h)) return false;
            auto* dst = static_cast<std::uint8_t*>(present_bits);
            std::memcpy(dst, bits, bytes);
            for (int i = 0; i < w * h; ++i) dst[static_cast<size_t>(i) * 4u + 3u] = 255;
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            POINT dst_pt{wr.left, wr.top};
            SIZE sz{w, h};
            POINT src{0, 0};
            BLENDFUNCTION blend{};
            blend.BlendOp = AC_SRC_OVER;
            blend.SourceConstantAlpha = 255;
            blend.AlphaFormat = AC_SRC_ALPHA;
            // Layout owns the position. Child coordinates must never be replaced
            // with the screen-space rectangle during a text repaint.
            const bool child = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) != 0;
            const BOOL ok = UpdateLayeredWindow(hwnd, nullptr, child ? nullptr : &dst_pt, &sz, present_dc,
                                                &src, 0, &blend, ULW_ALPHA);
            return ok != FALSE;
        }
        if (!paint_dc) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        return SetDIBitsToDevice(paint_dc, 0, 0, static_cast<DWORD>(w),
                                 static_cast<DWORD>(h), 0, 0, 0, static_cast<UINT>(h),
                                 bits, &info, DIB_RGB_COLORS) > 0;
    }

    void Shutdown() noexcept {
        ClearLayouts();
        ReleasePresent();
        blit_target.Reset();
        blit_staging.Reset();
        blit_dc.Reset();
        blit_w = 0;
        blit_h = 0;
        busy = false;
        segoe_cascade.reset();
        yahei_cascade.reset();
        segoe_bold.reset();
        segoe_semibold.reset();
        segoe_regular.reset();
        yahei_bold.reset();
        yahei_regular.reset();
        profile.reset();
        renderer.reset();
        recording_dc.Reset();
        target_dc.Reset();
        context.reset();
    }

    LumaText::TextLayout* GetLayout(const LayoutKey& key, float font_size,
                                    LumaText::FontCascade& cascade) {
        if (auto found = layouts.find(key); found != layouts.end()) {
            layout_lru.splice(layout_lru.begin(), layout_lru, found->second.lru);
            found->second.lru = layout_lru.begin();
            return &found->second.layout;
        }
        lt_text_style style{};
        lt_text_layout_desc desc{};
        FillLayoutDesc(desc, key, style, font_size, cascade);

        LumaText::TextLayout layout;
        if (lt_text_layout_create(context.get(), &desc, layout.put()) != LT_OK) return nullptr;
        while (layouts.size() >= kLayoutCacheLimit) EvictOldestLayout();
        layout_lru.push_front(key);
        const auto inserted = layouts.emplace(key, LayoutSlot{std::move(layout),
                                                              layout_lru.begin()});
        return &inserted.first->second.layout;
    }

    bool DrawOn(ID2D1DeviceContext* dc, std::wstring_view text, IDWriteTextFormat* format,
                 const D2D1_RECT_F& bounds, const D2D1_COLOR_F& foreground,
                 const D2D1_COLOR_F& background, DWRITE_TEXT_ALIGNMENT alignment) {
        if (!dc || busy || !renderer || !format || text.empty() ||
            format->GetFontStyle() != DWRITE_FONT_STYLE_NORMAL ||
            format->GetFontStretch() != DWRITE_FONT_STRETCH_NORMAL ||
            text.size() > UINT32_MAX) {
            return false;
        }
        struct BusyGuard {
            bool& flag;
            explicit BusyGuard(bool& value) : flag(value) { flag = true; }
            ~BusyGuard() { flag = false; }
        } busy_guard(busy);
        const float width = bounds.right - bounds.left;
        const float height = bounds.bottom - bounds.top;
        const float font_size = format->GetFontSize();
        if (!(width > 0.0f) || !(height > 0.0f) || !(font_size > 0.0f)) return false;

        std::uint32_t family = 0;
        LumaText::FontCascade* cascade = nullptr;
        if (!ResolveCascade(format, text, family, cascade) || !cascade) return false;

        LayoutKey key;
        key.text.assign(text);
        key.family = family;
        key.weight = static_cast<std::uint16_t>(std::clamp<int>(
            static_cast<int>(format->GetFontWeight()), 1, 1000));
        key.size_64 = static_cast<std::int32_t>(std::lround(font_size * 64.0f));
        key.width_64 = kUnboundedWidth64;
        key.alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        LumaText::TextLayout* layout = GetLayout(key, font_size, *cascade);
        if (!layout || !*layout) return false;

        auto metrics = LumaText::Descriptor<lt_text_metrics>();
        if (lt_text_layout_get_metrics(layout->get(), &metrics) != LT_OK) return false;
        // Dest is an ink clip. Advance-only overflow left the last glyph's
        // optical weight / Mitchell lobe shaved off ("File" → "Fil").
        const float ink = pulse::ui::typography::InkPad(format);
        if (metrics.width + ink > width + 0.5f) {
            key.width_64 = static_cast<std::int32_t>(
                std::lround(std::max(1.0f, width - ink) * 64.0f));
            key.ellipsis = LT_TEXT_ELLIPSIS_END;
            layout = GetLayout(key, font_size, *cascade);
            if (!layout || !*layout) return false;
            if (lt_text_layout_get_metrics(layout->get(), &metrics) != LT_OK) return false;
        }
        const float floor_x = std::floor(bounds.left);
        const float floor_y = std::floor(bounds.top);
        const float phase_x = bounds.left - floor_x;
        const float phase_y = bounds.top - floor_y;
        const auto phase_bucket = [](float phase, int buckets) {
            const long rounded = std::lround(phase * static_cast<float>(buckets));
            return static_cast<std::uint8_t>(std::clamp<long>(
                rounded, 0l, static_cast<long>(buckets - 1)));
        };
        const std::uint8_t x_phase_8 = phase_bucket(phase_x, 8);
        // Keep four vertical raster buckets so scrolling preserves more of
        // LumaText's baseline precision without returning to 1/64 variants.
        const std::uint8_t y_phase_4 = phase_bucket(phase_y, 4);
        const float cached_phase_x = static_cast<float>(x_phase_8) / 8.0f;
        const float cached_phase_y = static_cast<float>(y_phase_4) / 4.0f;
        const auto color_byte = [](float value) {
            return static_cast<std::uint32_t>(std::clamp(
                std::lround(value * 255.0f), 0l, 255l));
        };
        const std::uint32_t packed_foreground =
            color_byte(foreground.r) |
            (color_byte(foreground.g) << 8) |
            (color_byte(foreground.b) << 16) |
            (color_byte(foreground.a) << 24);
        const std::uint32_t packed_background =
            color_byte(background.r) |
            (color_byte(background.g) << 8) |
            (color_byte(background.b) << 16) |
            (color_byte(background.a) << 24);
        const bool dark = 0.2126f * background.r + 0.7152f * background.g +
                          0.0722f * background.b < 0.5f;
        const SurfaceKey surface_key{
            layout->get(),
            std::hash<std::wstring_view>{}(text),
            static_cast<std::int32_t>(std::lround(width * 64.0f)),
            static_cast<std::int32_t>(std::lround(height * 64.0f)),
            x_phase_8,
            y_phase_4,
            packed_foreground,
            packed_background,
            dark,
        };
        if (auto cached = surfaces.find(surface_key); cached != surfaces.end()) {
                TouchSurface(cached);
                const auto offset = D2D1::Point2F(
                    floor_x + phase_x - cached_phase_x,
                    floor_y + phase_y - cached_phase_y);
                // The command list already contains the canonical text clip;
                // avoid repeating target clip state changes for every row.
                dc->DrawImage(cached->second.commands.Get(), &offset, nullptr,
                              D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                              D2D1_COMPOSITE_MODE_SOURCE_OVER);
                stats.surface_cache_hits++;
                stats.draw_calls++;
                return true;
        }
        stats.surface_cache_misses++;

        Microsoft::WRL::ComPtr<ID2D1CommandList> commands;
        if (FAILED(recording_dc->CreateCommandList(&commands)) || !commands) return false;
        recording_dc->SetTarget(commands.Get());
        recording_dc->BeginDraw();
        auto frame_desc = LumaText::Descriptor<lt_frame_desc>();
        frame_desc.dpi_x = 96.0f;
        frame_desc.dpi_y = 96.0f;
        LumaText::Frame frame;
        if (lt_frame_begin(renderer.get(), &frame_desc, frame.put()) != LT_OK) {
            recording_dc->EndDraw();
            recording_dc->SetTarget(nullptr);
            return false;
        }

        auto draw = LumaText::Descriptor<lt_draw_text_desc>();
        draw.origin_x = cached_phase_x;
        if (alignment == DWRITE_TEXT_ALIGNMENT_CENTER) {
            draw.origin_x += std::max(0.0f, (width - metrics.width) * 0.5f);
        } else if (alignment == DWRITE_TEXT_ALIGNMENT_TRAILING) {
            draw.origin_x += std::max(0.0f, width - metrics.width);
        }
        draw.origin_y = cached_phase_y + (height - metrics.height) * 0.5f;
        draw.clip = {cached_phase_x, cached_phase_y,
                     cached_phase_x + width, cached_phase_y + height};
        draw.clip_enabled = true;
        draw.foreground = {foreground.r, foreground.g, foreground.b, foreground.a};
        draw.background = {background.r, background.g, background.b, background.a};
        draw.background_type = LT_BACKGROUND_TRANSPARENT;
        draw.render_config = LumaText::Descriptor<lt_render_config>();
        draw.render_config.coverage_gamma = 0.85f;
        draw.render_config.coverage_contrast = 1.00f;
        draw.render_config.raster_filter = LT_RASTER_FILTER_MITCHELL;
        draw.profile = profile.get();

        const lt_result result = lt_frame_draw_text_layout(frame.get(), layout->get(), &draw);
        auto frame_stats = LumaText::Descriptor<lt_frame_stats>();
        if (result == LT_OK && lt_frame_get_stats(frame.get(), &frame_stats) == LT_OK) {
            stats.freetype_glyphs += frame_stats.freetype_glyphs;
            stats.cache_hits += frame_stats.glyph_cache_hits;
        }
        const lt_result end_result = lt_frame_end(frame.get());
        const HRESULT recording_result = recording_dc->EndDraw();
        recording_dc->SetTarget(nullptr);
        if (result != LT_OK || end_result != LT_OK || FAILED(recording_result) ||
            FAILED(commands->Close())) {
            return false;
        }

        const float ink_width = std::min(width, std::max(1.0f, metrics.width + 2.0f));
        const std::uint64_t estimated_bytes = static_cast<std::uint64_t>(
            std::max(1l, std::lround(std::ceil(ink_width)))) *
            static_cast<std::uint64_t>(std::max(1l, std::lround(std::ceil(height)))) * 4ull;
        StoreSurface(surface_key, commands, estimated_bytes);

        const auto offset = D2D1::Point2F(
            floor_x + phase_x - cached_phase_x,
            floor_y + phase_y - cached_phase_y);
        dc->DrawImage(commands.Get(), &offset, nullptr,
                      D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                      D2D1_COMPOSITE_MODE_SOURCE_OVER);
        stats.draw_calls++;
        return true;
    }

    bool Draw(std::wstring_view text, IDWriteTextFormat* format,
              const D2D1_RECT_F& bounds, const D2D1_COLOR_F& foreground,
              const D2D1_COLOR_F& background, DWRITE_TEXT_ALIGNMENT alignment) {
        return DrawOn(target_dc.Get(), text, format, bounds, foreground, background,
                      alignment);
    }

    static bool ContainsCjk(std::wstring_view text) noexcept {
        for (const wchar_t c : text) {
            if ((c >= 0x2E80 && c <= 0x9FFF) ||
                (c >= 0xF900 && c <= 0xFAFF) ||
                (c >= 0xFF00 && c <= 0xFFEF)) {
                return true;
            }
        }
        return false;
    }

    bool ResolveCascade(IDWriteTextFormat* format, std::wstring_view text,
                        std::uint32_t& family, LumaText::FontCascade*& cascade) {
        if (!format) return false;
        const std::wstring family_name = FontFamily(format);
        const bool yahei = Contains(family_name, L"Microsoft YaHei");
        const bool segoe_text = Contains(family_name, L"Segoe UI") &&
                   !Contains(family_name, L"Icons") &&
                   !Contains(family_name, L"Assets") &&
                   !Contains(family_name, L"Emoji");
        if (!yahei && !segoe_text) return false;
        // Chinese runs keep YaHei first even when the shell language (and the
        // DirectWrite family) is English / Segoe.
        if (ContainsCjk(text) && yahei_cascade.get()) {
            family = kYaHei;
            cascade = &yahei_cascade;
            return true;
        }
        if (yahei) {
            family = kYaHei;
            cascade = &yahei_cascade;
            return yahei_cascade.get() != nullptr;
        }
        family = kSegoe;
        cascade = &segoe_cascade;
        return segoe_cascade.get() != nullptr;
    }

    bool Measure(std::wstring_view text, IDWriteTextFormat* format,
                 float& width, float* height) {
        width = 0.0f;
        if (height) *height = 0.0f;
        if (!renderer || !format || text.empty()) return text.empty();
        std::uint32_t family = 0;
        LumaText::FontCascade* cascade = nullptr;
        if (!ResolveCascade(format, text, family, cascade) || !cascade) return false;
        const float font_size = format->GetFontSize();
        if (!(font_size > 0.0f)) return false;
        LayoutKey key;
        key.text.assign(text);
        key.family = family;
        key.weight = static_cast<std::uint16_t>(std::clamp<int>(
            static_cast<int>(format->GetFontWeight()), 1, 1000));
        key.size_64 = static_cast<std::int32_t>(std::lround(font_size * 64.0f));
        key.width_64 = kUnboundedWidth64;
        key.alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        LumaText::TextLayout* layout = GetLayout(key, font_size, *cascade);
        if (!layout || !*layout) return false;
        auto metrics = LumaText::Descriptor<lt_text_metrics>();
        if (lt_text_layout_get_metrics(layout->get(), &metrics) != LT_OK) return false;
        width = metrics.width;
        if (height) *height = metrics.height;
        return true;
    }

    LumaText::TextLayout* GetLeadingLayout(std::wstring_view text, IDWriteTextFormat* format) {
        if (!format || text.empty()) return nullptr;
        std::uint32_t family = 0;
        LumaText::FontCascade* cascade = nullptr;
        if (!ResolveCascade(format, text, family, cascade) || !cascade) return nullptr;
        const float font_size = format->GetFontSize();
        if (!(font_size > 0.0f)) return nullptr;
        LayoutKey key;
        key.text.assign(text);
        key.family = family;
        key.weight = static_cast<std::uint16_t>(std::clamp<int>(
            static_cast<int>(format->GetFontWeight()), 1, 1000));
        key.size_64 = static_cast<std::int32_t>(std::lround(font_size * 64.0f));
        key.width_64 = kUnboundedWidth64;
        key.alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        return GetLayout(key, font_size, *cascade);
    }

    static std::wstring ReadEditText(HWND hwnd) {
        const int len = GetWindowTextLengthW(hwnd);
        if (len <= 0) return {};
        std::wstring text(static_cast<size_t>(len) + 1, L'\0');
        const int copied = GetWindowTextW(hwnd, text.data(), len + 1);
        text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
        return text;
    }

    float CaretOriginX(const lt_text_layout* layout, std::uint32_t position,
                       std::uint32_t length) {
        if (!layout || length == 0 || position == 0) return 0.0f;
        auto hit = LumaText::Descriptor<lt_hit_test_metrics>();
        float x = 0.0f;
        float y = 0.0f;
        const bool trailing = position >= length;
        const std::uint32_t probe = trailing ? length : position;
        if (lt_text_layout_hit_test_position(layout, probe, trailing, &x, &y, &hit) != LT_OK) {
            return 0.0f;
        }
        return x;
    }

    int HitCharIndex(const lt_text_layout* layout, float x, int length) {
        if (!layout || length <= 0 || x <= 0.0f) return 0;
        auto hit = LumaText::Descriptor<lt_hit_test_metrics>();
        if (lt_text_layout_hit_test_point(layout, x, 1.0f, &hit) != LT_OK) return length;
        int index = static_cast<int>(hit.text_position);
        if (hit.is_trailing) index += static_cast<int>(hit.text_length);
        return std::clamp(index, 0, length);
    }

    static float EditScrollX(float caret_x, float width) {
        const float inner = std::max(1.0f, width - kEditPad);
        float scroll = 0.0f;
        if (caret_x - scroll > inner) scroll = caret_x - inner;
        if (caret_x - scroll < kEditPad) scroll = std::max(0.0f, caret_x - kEditPad);
        return scroll;
    }

    void ApplyMouseSelection(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                             IDWriteTextFormat* format) {
        if (!hwnd || !format) return;
        if (msg != WM_LBUTTONDOWN && msg != WM_LBUTTONDBLCLK && msg != WM_MOUSEMOVE)
            return;
        if (msg == WM_MOUSEMOVE &&
            (GetCapture() != hwnd || (wParam & MK_LBUTTON) == 0)) {
            return;
        }

        const std::wstring text = ReadEditText(hwnd);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const float width = static_cast<float>(rc.right - rc.left);
        DWORD sel0 = 0, sel1 = 0;
        SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&sel0),
                     reinterpret_cast<LPARAM>(&sel1));
        int caret_for_scroll = static_cast<int>(sel1);
        if (mouse_hwnd == hwnd) caret_for_scroll = mouse_caret;
        caret_for_scroll = std::clamp(caret_for_scroll, 0, static_cast<int>(text.size()));

        LumaText::TextLayout* layout = GetLeadingLayout(text, format);
        const lt_text_layout* native = (layout && *layout) ? layout->get() : nullptr;
        const float caret_x = CaretOriginX(native,
            static_cast<std::uint32_t>(caret_for_scroll),
            static_cast<std::uint32_t>(text.size()));
        const float scroll = EditScrollX(caret_x, width);
        const float layout_x = static_cast<float>(GET_X_LPARAM(lParam)) - kEditPad + scroll;
        const int index = HitCharIndex(native, layout_x, static_cast<int>(text.size()));

        if (msg == WM_LBUTTONDBLCLK) {
            int start = index;
            int end = index;
            while (start > 0 && !std::iswspace(text[static_cast<size_t>(start) - 1]))
                --start;
            while (end < static_cast<int>(text.size()) &&
                   !std::iswspace(text[static_cast<size_t>(end)]))
                ++end;
            mouse_hwnd = hwnd;
            mouse_anchor = start;
            mouse_caret = end;
            SendMessageW(hwnd, EM_SETSEL, static_cast<WPARAM>(start), static_cast<LPARAM>(end));
            return;
        }
        if (msg == WM_LBUTTONDOWN) {
            if ((wParam & MK_SHIFT) && mouse_hwnd == hwnd) {
                mouse_caret = index;
            } else {
                mouse_hwnd = hwnd;
                mouse_anchor = index;
                mouse_caret = index;
            }
            SendMessageW(hwnd, EM_SETSEL, static_cast<WPARAM>(mouse_anchor),
                         static_cast<LPARAM>(mouse_caret));
            return;
        }
        if (mouse_hwnd != hwnd) {
            mouse_hwnd = hwnd;
            mouse_anchor = static_cast<int>(sel0);
        }
        mouse_caret = index;
        SendMessageW(hwnd, EM_SETSEL, static_cast<WPARAM>(mouse_anchor),
                     static_cast<LPARAM>(mouse_caret));
    }

    bool EnsureBlit(int w, int h) {
        if (blit_target && blit_staging && blit_w == w && blit_h == h) return true;
        blit_target.Reset();
        blit_staging.Reset();
        blit_w = 0;
        blit_h = 0;
        if (!blit_dc || w <= 0 || h <= 0) return false;
        const D2D1_SIZE_U size{static_cast<UINT32>(w), static_cast<UINT32>(h)};
        const auto pixel = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                             D2D1_ALPHA_MODE_PREMULTIPLIED);
        const auto target_props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET, pixel, 96.0f, 96.0f);
        const auto cpu_props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            pixel, 96.0f, 96.0f);
        if (FAILED(blit_dc->CreateBitmap(size, nullptr, 0, &target_props, &blit_target))) {
            return false;
        }
        if (FAILED(blit_dc->CreateBitmap(size, nullptr, 0, &cpu_props, &blit_staging))) {
            blit_target.Reset();
            return false;
        }
        blit_w = w;
        blit_h = h;
        return true;
    }

    bool PaintEdit(HWND hwnd, HDC hdc, IDWriteTextFormat* format,
                   const D2D1_COLOR_F& foreground, const D2D1_COLOR_F& background) {
        if (!hwnd || !format || !blit_dc) return false;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return false;

        if (!EnsureBlit(w, h)) return false;

        const int len = GetWindowTextLengthW(hwnd);
        std::wstring text;
        if (len > 0) {
            text.resize(static_cast<size_t>(len) + 1);
            const int copied = GetWindowTextW(hwnd, text.data(), len + 1);
            text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
        }

        DWORD sel0 = 0, sel1 = 0;
        SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&sel0),
                     reinterpret_cast<LPARAM>(&sel1));
        const int sel_lo = static_cast<int>(std::min(sel0, sel1));
        const int sel_hi = static_cast<int>(std::max(sel0, sel1));
        int caret = static_cast<int>(sel1);
        if (mouse_hwnd == hwnd && (mouse_caret == sel_lo || mouse_caret == sel_hi))
            caret = mouse_caret;
        caret = std::clamp(caret, 0, static_cast<int>(text.size()));

        LumaText::TextLayout* layout = GetLeadingLayout(text, format);
        const lt_text_layout* native = (layout && *layout) ? layout->get() : nullptr;
        const auto caret_origin = [&](int position) {
            return CaretOriginX(native, static_cast<std::uint32_t>(std::max(0, position)),
                                static_cast<std::uint32_t>(text.size()));
        };
        const float caret_x = caret_origin(caret);
        const float sel_x0 = caret_origin(sel_lo);
        const float sel_x1 = caret_origin(sel_hi);
        const float pad = kEditPad;
        const float scroll = EditScrollX(caret_x, static_cast<float>(w));

        blit_dc->SetTarget(blit_target.Get());
        blit_dc->BeginDraw();
        blit_dc->Clear(background);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        blit_dc->CreateSolidColorBrush(background, &brush);
        if (sel_hi > sel_lo && GetFocus() == hwnd && brush) {
            brush->SetColor(D2D1::ColorF(0.0f, 0.47f, 0.83f, 0.35f));
            blit_dc->FillRectangle(
                D2D1::RectF(pad + sel_x0 - scroll, 1.0f, pad + sel_x1 - scroll,
                            static_cast<float>(h) - 1.0f),
                brush.Get());
        }
        std::wstring visible = text;
        D2D1_COLOR_F visible_color = foreground;
        if (visible.empty()) {
            wchar_t cue[256]{};
            if (SendMessageW(hwnd, EM_GETCUEBANNER, reinterpret_cast<WPARAM>(cue),
                             ARRAYSIZE(cue)) && cue[0] != 0) {
                visible = cue;
                visible_color = D2D1::ColorF(foreground.r, foreground.g, foreground.b,
                                             foreground.a * 0.45f);
            }
        }
        if (!visible.empty()) {
            float text_w = 0.0f;
            Measure(visible, format, text_w, nullptr);
            const float draw_w = std::max(static_cast<float>(w) + scroll, text_w + 8.0f);
            const D2D1_RECT_F text_bounds{
                pad - scroll, 0.0f, pad - scroll + draw_w, static_cast<float>(h)};
            DrawOn(blit_dc.Get(), visible, format, text_bounds, visible_color, background,
                   DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        const UINT blink = GetCaretBlinkTime();
        const bool caret_on = GetFocus() == hwnd && GetCapture() != hwnd &&
            (blink == INFINITE || ((GetTickCount() / std::max(1u, blink)) % 2u) == 0u);
        if (caret_on && brush) {
            brush->SetColor(foreground);
            const float x = std::floor(pad + caret_x - scroll) + 0.5f;
            blit_dc->FillRectangle(
                D2D1::RectF(x, 2.0f, x + 1.0f, static_cast<float>(h) - 2.0f),
                brush.Get());
        }
        const HRESULT end = blit_dc->EndDraw();
        blit_dc->SetTarget(nullptr);
        if (FAILED(end)) return false;
        if (FAILED(blit_staging->CopyFromBitmap(nullptr, blit_target.Get(), nullptr))) {
            return false;
        }
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(blit_staging->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
        const size_t row_bytes = static_cast<size_t>(w) * 4u;
        const void* dib_bits = mapped.bits;
        std::vector<std::uint8_t> tight_pixels;
        if (mapped.pitch != row_bytes) {
            tight_pixels.resize(row_bytes * static_cast<size_t>(h));
            const auto* source = static_cast<const std::uint8_t*>(mapped.bits);
            for (int row = 0; row < h; ++row) {
                std::memcpy(tight_pixels.data() + static_cast<size_t>(row) * row_bytes,
                            source + static_cast<size_t>(row) * mapped.pitch,
                            row_bytes);
            }
            dib_bits = tight_pixels.data();
        }
        const bool presented = PresentEditBits(hwnd, hdc, dib_bits, w, h);
        blit_staging->Unmap();

        HIMC imc = ImmGetContext(hwnd);
        if (imc) {
            COMPOSITIONFORM form{};
            form.dwStyle = CFS_POINT;
            form.ptCurrentPos = {static_cast<LONG>(pad + caret_x - scroll), 0};
            ImmSetCompositionWindow(imc, &form);
            ImmReleaseContext(hwnd, imc);
        }
        return presented;
    }
#else
    bool Init(IDWriteFactory*, ID2D1RenderTarget*) { return false; }
    void Shutdown() noexcept {}
#endif
};

LumaTextRenderer::LumaTextRenderer() : impl_(std::make_unique<Impl>()) {}
LumaTextRenderer::~LumaTextRenderer() = default;

LRESULT LumaTextRenderer::CallEditDefaultMouse(HWND hwnd, UINT msg, WPARAM wParam,
                                               LPARAM lParam, IDWriteTextFormat* format) {
#if defined(PULSE_HAS_LUMATEXT)
    if (format && impl_ && impl_->renderer) {
        HideCaret(hwnd);
        switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            SetCapture(hwnd);
            impl_->ApplyMouseSelection(hwnd, msg, wParam, lParam, format);
            if (GetFocus() != hwnd) SetFocus(hwnd);
            HideCaret(hwnd);
            return 0;
        case WM_MOUSEMOVE:
            if (GetCapture() == hwnd)
                impl_->ApplyMouseSelection(hwnd, msg, wParam, lParam, format);
            return 0;
        case WM_LBUTTONUP:
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED:
            return 0;
        default:
            break;
        }
    }
#endif
    SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
    SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
    HideCaret(hwnd);
    return result;
}

bool LumaTextRenderer::Init(IDWriteFactory* dwrite, ID2D1RenderTarget* target) {
    Shutdown();
    if (!EnvironmentEnabled() || !dwrite || !target || !LoadOptionalLumaText()) return false;
    return impl_->Init(dwrite, target);
}

void LumaTextRenderer::Shutdown() noexcept {
    if (impl_) impl_->Shutdown();
}

bool LumaTextRenderer::Enabled() const noexcept {
#if defined(PULSE_HAS_LUMATEXT)
    return impl_ && static_cast<bool>(impl_->renderer);
#else
    return false;
#endif
}

bool LumaTextRenderer::Draw(std::wstring_view text, IDWriteTextFormat* format,
                            const D2D1_RECT_F& bounds,
                            const D2D1_COLOR_F& foreground,
                            const D2D1_COLOR_F& background,
                            DWRITE_TEXT_ALIGNMENT alignment) {
#if defined(PULSE_HAS_LUMATEXT)
    return impl_ && impl_->Draw(text, format, bounds, foreground, background, alignment);
#else
    (void)text;
    (void)format;
    (void)bounds;
    (void)foreground;
    (void)background;
    (void)alignment;
    return false;
#endif
}

bool LumaTextRenderer::Measure(std::wstring_view text, IDWriteTextFormat* format,
                               float& width, float* height) {
#if defined(PULSE_HAS_LUMATEXT)
    return impl_ && impl_->Measure(text, format, width, height);
#else
    (void)text;
    (void)format;
    width = 0.0f;
    if (height) *height = 0.0f;
    return false;
#endif
}

bool LumaTextRenderer::PaintEdit(HWND hwnd, HDC hdc, IDWriteTextFormat* format,
                                 const D2D1_COLOR_F& foreground,
                                 const D2D1_COLOR_F& background) {
#if defined(PULSE_HAS_LUMATEXT)
    return impl_ && impl_->PaintEdit(hwnd, hdc, format, foreground, background);
#else
    (void)hwnd;
    (void)hdc;
    (void)format;
    (void)foreground;
    (void)background;
    return false;
#endif
}

const LumaTextStats& LumaTextRenderer::Stats() const noexcept {
    return impl_->stats;
}

void LumaTextRenderer::RecordFallback() noexcept {
    impl_->stats.fallback_draws++;
}

} // namespace pulse::ui

#include "lumatext_renderer.h"
#include "optional_lumatext.h"
#include "typography.h"
#include "edit_word_range.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <imm.h>

#include <algorithm>
#include <array>
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
#if defined(PULSE_TEST_BATCH_RENAME_PRESENT)
void ObserveEditUploadForTest(HWND hwnd, const void* bits, int width, int height);
#endif
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
    bool setting_mouse_selection = false;
    int mouse_anchor = 0;
    int mouse_caret = 0;
    bool applying_mouse_selection = false;
    // A drag keeps its selection here until release. Sending EM_SETSEL for
    // every WM_MOUSEMOVE runs the native redraw guard, which briefly hides
    // the redirected edit surface and made dragged selections flicker.
    bool mouse_selection_deferred = false;
    // After a double-click the held button extends by whole words, as in
    // native editors; a pointer that shifts slightly keeps the whole word.
    bool mouse_word_drag = false;
    int mouse_word_start = 0;
    int mouse_word_end = 0;
    std::unordered_map<HWND, std::pair<int, int>> edit_selections;
    // Horizontal scroll persists like native EDIT's offset: the text moves
    // only when the caret would leave the field. Paint and mouse hit-testing
    // share it so the pointer maps to the characters actually on screen.
    std::unordered_map<HWND, float> edit_scrolls;
    static constexpr float kEditPad = 2.0f;
    // Inputs use DirectWrite in every text mode, sharing the list rendering
    // parameters while EDIT continues to own text, undo and IME.
    Microsoft::WRL::ComPtr<IDWriteFactory2> dw_factory;
    Microsoft::WRL::ComPtr<IDWriteRenderingParams2> dw_params;
    std::uint64_t dw_params_generation = 0;
    HMONITOR dw_params_monitor = nullptr;
    struct EditLayoutSlot {
        std::wstring text;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        std::uint64_t generation = 0;
        float height = 0;
        float font_size = 0;
        DWRITE_READING_DIRECTION reading = DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
        DWRITE_FLOW_DIRECTION flow = DWRITE_FLOW_DIRECTION_TOP_TO_BOTTOM;
        DWRITE_LINE_SPACING_METHOD spacing_method = DWRITE_LINE_SPACING_METHOD_DEFAULT;
        float line_spacing = 0;
        float baseline = 0;
        float tab_stop = 0;
    };
    std::array<EditLayoutSlot, 4> edit_layouts;
    size_t next_edit_layout = 0;
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
        for (auto& entry : edit_layouts) entry = {};
        next_edit_layout = 0;
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
        // Inputs fail back to native EDIT if their DirectWrite path is unavailable.
        dwrite->QueryInterface(IID_PPV_ARGS(&dw_factory));

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
        profile_desc.regular_optical_weight = 0.0f;
        profile_desc.bold_optical_weight = 0.0f;
#if defined(PULSE_TEST_LUMATEXT_INIT_FAILURE)
        wchar_t fail_init[2]{};
        if (GetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_INIT_FAILURE", fail_init, ARRAYSIZE(fail_init)))
            return false;
#endif
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

    bool PresentEditBits(HWND hwnd, HDC paint_dc, const void* bits, int w, int h, bool preserve_alpha) {
        if (!hwnd || !bits || w <= 0 || h <= 0) return false;
        const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
        const bool layered =
            (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
        if (layered && !paint_dc) {
            // Own the redirected bitmap. SetLayeredWindowAttributes + WM_PAINT
            // lets EDIT's GetDC/ClearType selection paint win on mouse-down.
            if (!EnsurePresent(w, h)) return false;
            auto* dst = static_cast<std::uint8_t*>(present_bits);
            std::memcpy(dst, bits, bytes);
            if (!preserve_alpha)
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
#if defined(PULSE_TEST_BATCH_RENAME_PRESENT)
            if (ok) {
                pulse::ui::ObserveEditUploadForTest(hwnd, present_bits, w, h);
            }
#endif
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
        const bool ok = SetDIBitsToDevice(paint_dc, 0, 0, static_cast<DWORD>(w),
                                 static_cast<DWORD>(h), 0, 0, 0, static_cast<UINT>(h),
                                 bits, &info, DIB_RGB_COLORS) > 0;
#if defined(PULSE_TEST_BATCH_RENAME_PRESENT)
        if (ok) pulse::ui::ObserveEditUploadForTest(hwnd, bits, w, h);
#endif
        return ok;
    }

    void Shutdown() noexcept {
        ClearLayouts();
        ReleasePresent();
        dw_params.Reset();
        dw_factory.Reset();
        dw_params_monitor = nullptr;
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
            // An ellipsized layout that still overflows would be clipped
            // mid-glyph with no "..." (long Explorer verbs in the fixed-width
            // context menu). DirectWrite trims correctly; let it draw.
            if (metrics.line_count > 1 || metrics.width + ink > width + 0.5f) return false;
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

    static std::wstring ReadEditText(HWND hwnd) {
        const int len = GetWindowTextLengthW(hwnd);
        if (len <= 0) return {};
        std::wstring text(static_cast<size_t>(len) + 1, L'\0');
        const int copied = GetWindowTextW(hwnd, text.data(), len + 1);
        text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
        return text;
    }

    Microsoft::WRL::ComPtr<IDWriteTextLayout> DwEditLayout(std::wstring_view text,
                                                           IDWriteTextFormat* format,
                                                           float height) {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (!dw_factory || !format || text.size() > UINT32_MAX) return layout;
        const float line_height = std::max(1.0f, height);
        const float font_size = format->GetFontSize();
        const auto reading = format->GetReadingDirection();
        const auto flow = format->GetFlowDirection();
        DWRITE_LINE_SPACING_METHOD spacing_method = DWRITE_LINE_SPACING_METHOD_DEFAULT;
        float line_spacing = 0, baseline = 0;
        format->GetLineSpacing(&spacing_method, &line_spacing, &baseline);
        const float tab_stop = format->GetIncrementalTabStop();
        const auto generation = pulse::ui::typography::Generation();
        // Caret blinking and drag selection reuse the same layout. Retaining
        // the format prevents its address being recycled into a false hit.
        const bool cacheable = text.size() <= 4096;
        if (cacheable) {
            for (const auto& entry : edit_layouts) {
                if (entry.layout && entry.format.Get() == format && entry.generation == generation &&
                    entry.height == line_height && entry.font_size == font_size && entry.reading == reading &&
                    entry.flow == flow && entry.spacing_method == spacing_method &&
                    entry.line_spacing == line_spacing && entry.baseline == baseline &&
                    entry.tab_stop == tab_stop && entry.text == text) {
                    ++stats.edit_layout_cache_hits;
                    return entry.layout;
                }
            }
        }
        if (FAILED(dw_factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
                format, 1.0e6f, line_height, &layout)) || !layout) {
            return nullptr;
        }
        // One unwrapped, untrimmed line; the edit scrolls it horizontally.
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        // Keep the unbounded line at the physical left origin even for RTL
        // paragraphs; LEADING would place those glyphs near x = 1e6.
        layout->SetTextAlignment(reading == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT ?
            DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        const DWRITE_TRIMMING none{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
        layout->SetTrimming(&none, nullptr);
        ++stats.edit_layout_builds;
        if (cacheable) {
            auto& entry = edit_layouts[next_edit_layout];
            next_edit_layout = (next_edit_layout + 1) % edit_layouts.size();
            entry = {std::wstring(text), format, layout, generation, line_height, font_size, reading, flow,
                spacing_method, line_spacing, baseline, tab_stop};
        }
        return layout;
    }

    static float DwCaretX(IDWriteTextLayout* layout, int position, int length) {
        if (!layout || length <= 0) return 0.0f;
        const bool trailing = position >= length;
        const UINT32 probe = static_cast<UINT32>(trailing ? length - 1 : position);
        float x = 0.0f;
        float y = 0.0f;
        DWRITE_HIT_TEST_METRICS hit{};
        if (FAILED(layout->HitTestTextPosition(probe, trailing ? TRUE : FALSE, &x, &y, &hit)))
            return 0.0f;
        return x;
    }

    static int DwHitIndex(IDWriteTextLayout* layout, float x, int length) {
        if (!layout || length <= 0) return 0;
        BOOL trailing = FALSE;
        BOOL inside = FALSE;
        DWRITE_HIT_TEST_METRICS hit{};
        if (FAILED(layout->HitTestPoint(x, 1.0f, &trailing, &inside, &hit))) return length;
        const int index = static_cast<int>(hit.textPosition) +
            (trailing ? static_cast<int>(hit.length) : 0);
        return std::clamp(index, 0, length);
    }

    IDWriteRenderingParams* DwParams(HWND hwnd) {
        const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        const std::uint64_t generation = pulse::ui::typography::Generation();
        if (!dw_params || dw_params_generation != generation || dw_params_monitor != monitor) {
            dw_params.Reset();
            pulse::ui::typography::CreateRenderingParams(dw_factory.Get(), monitor, &dw_params);
            dw_params_generation = generation;
            dw_params_monitor = monitor;
        }
        return dw_params.Get();
    }

    static float EditScrollX(float caret_x, float width, float previous = 0.0f,
                             float text_width = -1.0f) {
        const float inner = std::max(1.0f, width - kEditPad);
        float scroll = std::max(0.0f, previous);
        if (caret_x - scroll > inner) scroll = caret_x - inner;
        if (caret_x - scroll < kEditPad) scroll = std::max(0.0f, caret_x - kEditPad);
        if (text_width >= 0.0f) scroll = std::min(scroll, std::max(0.0f, text_width - inner));
        return std::max(0.0f, scroll);
    }

    static float EditTextWidth(IDWriteTextLayout* layout) {
        DWRITE_TEXT_METRICS metrics{};
        if (!layout || FAILED(layout->GetMetrics(&metrics))) return -1.0f;
        return metrics.widthIncludingTrailingWhitespace;
    }

    float EditScroll(HWND hwnd, IDWriteTextLayout* layout, float caret_x, float width) const {
        const auto saved = edit_scrolls.find(hwnd);
        return EditScrollX(caret_x, width, saved != edit_scrolls.end() ? saved->second : 0.0f,
                           EditTextWidth(layout));
    }

    // Character under the pointer (not the nearest caret stop), so a click on
    // the right half of a letter still belongs to that letter's word.
    static int DwCharIndex(IDWriteTextLayout* layout, float x, int length) {
        if (!layout || length <= 0) return 0;
        BOOL trailing = FALSE;
        BOOL inside = FALSE;
        DWRITE_HIT_TEST_METRICS hit{};
        if (FAILED(layout->HitTestPoint(x, 1.0f, &trailing, &inside, &hit))) return length - 1;
        return std::clamp(static_cast<int>(hit.textPosition), 0, length - 1);
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
        if (mouse_hwnd != hwnd) {
            const auto saved = edit_selections.find(hwnd);
            if (saved != edit_selections.end() &&
                std::min(saved->second.first, saved->second.second) == static_cast<int>(sel0) &&
                std::max(saved->second.first, saved->second.second) == static_cast<int>(sel1)) {
                mouse_hwnd = hwnd;
                mouse_anchor = saved->second.first;
                mouse_caret = saved->second.second;
            }
        }
        int caret_for_scroll = static_cast<int>(sel1);
        if (mouse_hwnd == hwnd && (mouse_caret == static_cast<int>(sel0) || mouse_caret == static_cast<int>(sel1)))
            caret_for_scroll = mouse_caret;
        caret_for_scroll = std::clamp(caret_for_scroll, 0, static_cast<int>(text.size()));

        const int length = static_cast<int>(text.size());
        const auto dw = DwEditLayout(text, format, static_cast<float>(rc.bottom - rc.top));
        if (!dw) return;
        const float caret_x = DwCaretX(dw.Get(), caret_for_scroll, length);
        // Hit-test against the offset last painted, not one re-derived from
        // the caret: deriving it again slid the text under a held pointer.
        const auto painted = edit_scrolls.find(hwnd);
        const float scroll = painted != edit_scrolls.end()
            ? painted->second : EditScroll(hwnd, dw.Get(), caret_x, width);
        const float text_x = static_cast<float>(GET_X_LPARAM(lParam)) - kEditPad + scroll;
        const int index = DwHitIndex(dw.Get(), text_x, length);
        ++stats.edit_directwrite_hits;

        if (msg == WM_LBUTTONDBLCLK) {
            const auto [start, end] = EditWordRangeAt(text, DwCharIndex(dw.Get(), text_x, length));
            mouse_hwnd = hwnd;
            mouse_anchor = start;
            mouse_caret = end;
            mouse_word_drag = true;
            mouse_word_start = start;
            mouse_word_end = end;
            mouse_selection_deferred = false;
            SetMouseSelection(hwnd);
            return;
        }
        if (msg == WM_LBUTTONDOWN) {
            mouse_word_drag = false;
            if (wParam & MK_SHIFT) {
                if (mouse_hwnd != hwnd) mouse_anchor = static_cast<int>(sel0);
                mouse_hwnd = hwnd;
                mouse_caret = index;
            } else {
                mouse_hwnd = hwnd;
                mouse_anchor = index;
                mouse_caret = index;
            }
            mouse_selection_deferred = false;
            SetMouseSelection(hwnd);
            return;
        }
        if (mouse_word_drag && mouse_hwnd == hwnd) {
            const auto [start, end] = EditWordRangeAt(text, DwCharIndex(dw.Get(), text_x, length));
            if (start < mouse_word_start) {
                mouse_anchor = mouse_word_end;
                mouse_caret = start;
            } else {
                mouse_anchor = mouse_word_start;
                mouse_caret = std::max(end, mouse_word_end);
            }
            mouse_selection_deferred = true;
            return;
        }
        if (mouse_hwnd != hwnd) {
            mouse_hwnd = hwnd;
            mouse_anchor = static_cast<int>(sel0);
        }
        mouse_word_drag = false;
        mouse_caret = index;
        mouse_selection_deferred = true;
    }

    bool DeferredSelection(HWND hwnd) const {
        return mouse_selection_deferred && mouse_hwnd == hwnd;
    }

    void CommitMouseSelection(HWND hwnd) {
        if (!DeferredSelection(hwnd)) return;
        mouse_selection_deferred = false;
        if (IsWindow(hwnd)) SetMouseSelection(hwnd);
    }

    void SetMouseSelection(HWND hwnd) {
        setting_mouse_selection = true;
        SendMessageW(hwnd, EM_SETSEL, static_cast<WPARAM>(mouse_anchor), static_cast<LPARAM>(mouse_caret));
        setting_mouse_selection = false;
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
        if (!hwnd || !format || !blit_dc || !dw_factory) return false;
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
        if (DeferredSelection(hwnd)) {
            const int length = static_cast<int>(text.size());
            sel0 = static_cast<DWORD>(std::clamp(mouse_anchor, 0, length));
            sel1 = static_cast<DWORD>(std::clamp(mouse_caret, 0, length));
        }
        const int sel_lo = static_cast<int>(std::min(sel0, sel1));
        const int sel_hi = static_cast<int>(std::max(sel0, sel1));
        int caret = static_cast<int>(sel1);
        if (mouse_hwnd == hwnd && (mouse_caret == sel_lo || mouse_caret == sel_hi))
            caret = mouse_caret;
        else if (const auto saved = edit_selections.find(hwnd); saved != edit_selections.end() &&
            std::min(saved->second.first, saved->second.second) == sel_lo &&
            std::max(saved->second.first, saved->second.second) == sel_hi)
            caret = saved->second.second;
        caret = std::clamp(caret, 0, static_cast<int>(text.size()));

        const auto dw_layout = DwEditLayout(text, format, static_cast<float>(h));
        IDWriteRenderingParams* params = DwParams(hwnd);
        if (!dw_layout || !params) return false;
        const auto caret_origin = [&](int position) {
            return DwCaretX(dw_layout.Get(), position, static_cast<int>(text.size()));
        };
        const float caret_x = caret_origin(caret);
        const float pad = kEditPad;
        const float scroll = EditScroll(hwnd, dw_layout.Get(), caret_x, static_cast<float>(w));
        edit_scrolls[hwnd] = scroll;

        blit_dc->SetTarget(blit_target.Get());
        blit_dc->BeginDraw();
        blit_dc->Clear(background);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        blit_dc->CreateSolidColorBrush(background, &brush);
        bool selection_drawn = sel_hi <= sel_lo || GetFocus() != hwnd;
        if (sel_hi > sel_lo && GetFocus() == hwnd && brush) {
            brush->SetColor(D2D1::ColorF(0.0f, 0.47f, 0.83f, 0.35f));
            UINT32 count = 0;
            dw_layout->HitTestTextRange(static_cast<UINT32>(sel_lo), static_cast<UINT32>(sel_hi - sel_lo),
                pad - scroll, 0, nullptr, 0, &count);
            std::vector<DWRITE_HIT_TEST_METRICS> ranges(count);
            if (count && SUCCEEDED(dw_layout->HitTestTextRange(static_cast<UINT32>(sel_lo),
                static_cast<UINT32>(sel_hi - sel_lo), pad - scroll, 0, ranges.data(), count, &count))) {
                for (const auto& range : ranges)
                    blit_dc->FillRectangle(D2D1::RectF(range.left, 1.0f,
                        range.left + range.width, static_cast<float>(h) - 1.0f), brush.Get());
                selection_drawn = true;
            }
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
        bool text_drawn = visible.empty();
        if (!visible.empty()) {
            // Same layout, params and grayscale AA as the surrounding UI text.
            const auto visible_layout = visible == text && dw_layout
                ? dw_layout : DwEditLayout(visible, format, static_cast<float>(h));
            if (visible_layout && brush) {
                DWRITE_TEXT_METRICS metrics{};
                if (FAILED(visible_layout->GetMetrics(&metrics))) {
                    blit_dc->EndDraw();
                    blit_dc->SetTarget(nullptr);
                    return false;
                }
                const float y = std::round((static_cast<float>(h) - metrics.height) * 0.5f) - metrics.top;
                brush->SetColor(visible_color);
                blit_dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                blit_dc->SetTextRenderingParams(params);
                blit_dc->DrawTextLayout(D2D1::Point2F(std::round(pad - scroll), y), visible_layout.Get(),
                                        brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                blit_dc->SetTextRenderingParams(nullptr);
                text_drawn = true;
            }
        }
        const UINT blink = GetCaretBlinkTime();
        const bool caret_on = GetFocus() == hwnd && GetCapture() != hwnd &&
            (blink == INFINITE || ((GetTickCount() / std::max(1u, blink)) % 2u) == 0u);
        if (caret_on && brush) {
            brush->SetColor(foreground);
            // Fill whole pixels: a half-pixel origin splits the caret into two
            // half-intensity columns that blend into the first placeholder glyph.
            DWORD caret_width = 1;
            SystemParametersInfoW(SPI_GETCARETWIDTH, 0, &caret_width, 0);
            const float x = std::floor(pad + caret_x - scroll);
            blit_dc->FillRectangle(
                D2D1::RectF(x, 2.0f, x + static_cast<float>(std::max<DWORD>(1, caret_width)),
                            static_cast<float>(h) - 2.0f),
                brush.Get());
        }
        const HRESULT end = blit_dc->EndDraw();
        blit_dc->SetTarget(nullptr);
        if (FAILED(end) || !text_drawn || !selection_drawn || !brush) return false;
        ++stats.edit_directwrite_draws;
        stats.edit_rendering_mode = params->GetRenderingMode();
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
        const bool presented = PresentEditBits(hwnd, hdc, dib_bits, w, h, background.a < 1.0f);
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
LumaTextRenderer::~LumaTextRenderer() { Shutdown(); }

void LumaTextRenderer::TrackEdit(HWND hwnd) {
    if (!hwnd || std::find(tracked_edits_.begin(), tracked_edits_.end(), hwnd) != tracked_edits_.end()) return;
    if (SetWindowSubclass(hwnd, EditSelectionProc, reinterpret_cast<UINT_PTR>(this),
        reinterpret_cast<DWORD_PTR>(this))) tracked_edits_.push_back(hwnd);
}
LRESULT CALLBACK LumaTextRenderer::EditSelectionProc(HWND hwnd, UINT message, WPARAM wparam,
                                                    LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
    auto* self = reinterpret_cast<LumaTextRenderer*>(data);
#if defined(PULSE_HAS_LUMATEXT)
    if (self->impl_ && self->impl_->DeferredSelection(hwnd)) {
        // Input and focus changes act on the dragged selection the user sees;
        // an explicit EM_SETSEL replaces it.
        switch (message) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_CHAR: case WM_IME_STARTCOMPOSITION:
        case WM_KILLFOCUS: case WM_SETTEXT: case EM_REPLACESEL: case WM_CUT: case WM_COPY:
        case WM_PASTE: case WM_CLEAR: case WM_UNDO: case EM_UNDO:
            self->impl_->CommitMouseSelection(hwnd);
            break;
        case EM_SETSEL:
            if (!self->impl_->setting_mouse_selection) self->impl_->mouse_selection_deferred = false;
            break;
        default:
            break;
        }
    }
#endif
    const LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
    self->SyncEditSelection(hwnd, message, wparam, lparam);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, EditSelectionProc, id);
        std::erase(self->tracked_edits_, hwnd);
    }
    return result;
}
void LumaTextRenderer::SyncEditSelection(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
#if defined(PULSE_HAS_LUMATEXT)
    if (!impl_) return;
    if (message == WM_NCDESTROY || message == WM_KILLFOCUS) {
        if (impl_->mouse_hwnd == hwnd) {
            impl_->mouse_hwnd = nullptr;
            impl_->mouse_selection_deferred = false;
        }
        if (message == WM_NCDESTROY) {
            impl_->edit_selections.erase(hwnd);
            impl_->edit_scrolls.erase(hwnd);
        }
        return;
    }
    // New text starts at the left edge, as native EDIT does.
    if (message == WM_SETTEXT) impl_->edit_scrolls.erase(hwnd);
    if (impl_->applying_mouse_selection) return;
    if (message != EM_SETSEL && message != WM_KEYDOWN && message != WM_CHAR &&
        message != WM_SETTEXT && message != EM_REPLACESEL && message != WM_CUT &&
        message != WM_PASTE && message != WM_CLEAR && message != WM_UNDO && message != EM_UNDO &&
        message != WM_IME_COMPOSITION && message != WM_IME_ENDCOMPOSITION) return;
    DWORD lo = 0, hi = 0;
    SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&lo), reinterpret_cast<LPARAM>(&hi));
    int anchor = static_cast<int>(lo), caret = static_cast<int>(hi);
    if (message == EM_SETSEL && static_cast<int>(wparam) >= 0) {
        const int length = GetWindowTextLengthW(hwnd);
        anchor = std::clamp(static_cast<int>(wparam), 0, length);
        caret = lparam < 0 ? length : std::clamp(static_cast<int>(lparam), 0, length);
    } else if (lo != hi) {
        if (message == WM_KEYDOWN && (wparam == VK_LEFT || wparam == VK_HOME || wparam == VK_UP)) {
            anchor = static_cast<int>(hi); caret = static_cast<int>(lo);
        } else if (impl_->mouse_hwnd == hwnd && impl_->mouse_anchor == static_cast<int>(hi)) {
            anchor = static_cast<int>(hi); caret = static_cast<int>(lo);
        }
    }
    impl_->mouse_hwnd = hwnd; impl_->mouse_anchor = anchor; impl_->mouse_caret = caret;
    impl_->edit_selections[hwnd] = {anchor, caret};
#else
    (void)hwnd; (void)message; (void)wparam; (void)lparam;
#endif
}

void LumaTextRenderer::SynchronizeEditSelection(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
#if defined(PULSE_HAS_LUMATEXT)
    if (!impl_ || impl_->setting_mouse_selection) return;
    if (msg == WM_NCDESTROY || msg == WM_KILLFOCUS) {
        if (impl_->mouse_hwnd == hwnd) impl_->mouse_hwnd = nullptr;
        return;
    }
    DWORD lo = 0, hi = 0;
    SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&lo), reinterpret_cast<LPARAM>(&hi));
    int anchor = static_cast<int>(lo), caret = static_cast<int>(hi);
    if (msg == EM_SETSEL && static_cast<int>(wParam) >= 0) {
        const int length = GetWindowTextLengthW(hwnd);
        anchor = std::min(static_cast<int>(wParam), length);
        caret = static_cast<int>(lParam) < 0 ? length : std::min(static_cast<int>(lParam), length);
    } else if (lo != hi && impl_->mouse_hwnd == hwnd && impl_->mouse_anchor == static_cast<int>(hi)) {
        anchor = static_cast<int>(hi);
        caret = static_cast<int>(lo);
    }
    impl_->mouse_hwnd = hwnd;
    impl_->mouse_anchor = anchor;
    impl_->mouse_caret = caret;
#else
    (void)hwnd; (void)msg; (void)wParam; (void)lParam;
#endif
}

LRESULT LumaTextRenderer::CallEditDefaultMouse(HWND hwnd, UINT msg, WPARAM wParam,
                                               LPARAM lParam, IDWriteTextFormat* format) {
#if defined(PULSE_HAS_LUMATEXT)
    if (format && impl_ && impl_->renderer) {
        TrackEdit(hwnd);
        struct MouseSelectionGuard {
            Impl& state;
            HWND window;
            MouseSelectionGuard(Impl& value, HWND hwnd) : state(value), window(hwnd) {
                state.applying_mouse_selection = true;
            }
            ~MouseSelectionGuard() {
                state.applying_mouse_selection = false;
                if (state.mouse_hwnd == window && IsWindow(window))
                    state.edit_selections[window] = {state.mouse_anchor, state.mouse_caret};
            }
        } guard(*impl_, hwnd);
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
            impl_->CommitMouseSelection(hwnd);
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED:
            impl_->CommitMouseSelection(hwnd);
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
    if (impl_->Init(dwrite, target)) return true;
    // The renderer is allocated before fonts, cascades and the profile. A late
    // failure must not leave Enabled() true and suppress native EDIT painting.
    Shutdown();
    return false;
}

void LumaTextRenderer::Shutdown() noexcept {
    for (HWND hwnd : tracked_edits_)
        RemoveWindowSubclass(hwnd, EditSelectionProc, reinterpret_cast<UINT_PTR>(this));
    tracked_edits_.clear();
#if defined(PULSE_HAS_LUMATEXT)
    if (impl_) {
        impl_->edit_selections.clear();
        impl_->edit_scrolls.clear();
        impl_->mouse_hwnd = nullptr;
        impl_->mouse_selection_deferred = false;
    }
#endif
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
    TrackEdit(hwnd);
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

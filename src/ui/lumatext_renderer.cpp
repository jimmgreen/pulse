#include "picker_proof_trace.h"
#include "lumatext_renderer.h"
#include "optional_lumatext.h"
#include "typography.h"
#include "edit_word_range.h"
#include "file_name_breaks.h"

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

// A font the user installed (per-user first, then for all users). Pulse
// never ships these faces; it only uses them when they are already present.
std::wstring InstalledFontPath(const wchar_t* file_name) {
    wchar_t local[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local));
    if (length != 0 && length < ARRAYSIZE(local)) {
        std::error_code error;
        const auto user = std::filesystem::path(local) / L"Microsoft" / L"Windows" / L"Fonts" / file_name;
        if (std::filesystem::exists(user, error)) return user.wstring();
    }
    return FontPath(file_name);
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
    bool Fail(std::uint64_t& counter) noexcept { ++counter; return false; }
    bool trace_timings = false;
    std::uint64_t TraceClock() const noexcept {
        if (!trace_timings) return 0;
        static const LONGLONG frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f.QuadPart; }();
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return static_cast<std::uint64_t>(static_cast<double>(now.QuadPart) * 1000000.0 / static_cast<double>(frequency));
    }
    void TraceElapsed(std::uint64_t start, std::uint64_t& total) const noexcept {
        if (trace_timings) total += TraceClock() - start;
    }


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
        // Whole-pixel baseline below the surface origin. Hinted glyphs sit on
        // whole rows, so the vertical position is an integer, not a phase.
        std::int32_t baseline_px = 0;
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
                static_cast<std::uint32_t>(key.baseline_px & 0xFFFF) << 8 |
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
    // Draws DirectWrite layouts (multi-line text, ranges with their own
    // colour) straight onto the window target with LumaText glyphs. A second
    // renderer, because retargeting one would flush its device caches.
    LumaText::Renderer layout_renderer;
    LumaText::FontFace yahei_regular;
    LumaText::FontFace yahei_bold;
    LumaText::FontFace segoe_regular;
    LumaText::FontFace segoe_semibold;
    LumaText::FontFace segoe_bold;
    LumaText::FontFace pingfang_regular;
    LumaText::FontFace pingfang_semibold;
    // One cascade serves every UI string: Segoe UI Variable for Latin, then
    // PingFang SC (when installed) and YaHei for CJK, chosen per grapheme.
    LumaText::FontCascade text_cascade;
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
    // Wrapped edits taller than their field: lines scrolled off the top.
    std::unordered_map<HWND, float> edit_vscrolls;
    // Caret blink restarts (solid) at the last caret move, typed character or
    // click, as in native editors. Phasing the blink off the global tick
    // count left the caret hidden for up to a blink interval after an arrow
    // key, and invisible throughout when the keys landed in the off phase.
    std::unordered_map<HWND, DWORD> caret_epochs;
    static constexpr float kEditPad = 2.0f;
    // Edits carrying this property wrap onto as many lines as the text needs
    // (the icon-grid rename field). The EDIT itself stays single-line, so
    // Enter still commits and no line break can be typed or pasted; only the
    // presentation, hit-testing and Up/Down are line-aware.
    static bool EditWraps(HWND hwnd) {
        return hwnd && (GetPropW(hwnd, L"Pulse.EditWrap") != nullptr || EditMultilineLine(hwnd) > 0.0f);
    }
    // A real multi-line EDIT (the assistant's composer) sets its line height
    // here, in 1/64 px. It wraps like body text: left-aligned, top-aligned,
    // ordinary word breaks, one uniform line height so the owner's measured
    // height and the painted lines agree. Line breaks are the EDIT's own.
    static float EditMultilineLine(HWND hwnd) {
        if (!hwnd) return 0.0f;
        const auto given = reinterpret_cast<UINT_PTR>(GetPropW(hwnd, L"Pulse.EditMultiline"));
        return given ? static_cast<float>(given) / 64.0f : 0.0f;
    }
    // Text padding: a multi-line field lines its text up with the owner's
    // drawn text, so it has none.
    static float EditPad(HWND hwnd) { return EditMultilineLine(hwnd) > 0.0f ? 0.0f : kEditPad; }
    // Line width of a wrapped field. The owner may give it exactly (in
    // 1/64 px, "Pulse.EditWrapW"): the window's whole-pixel size can lose a
    // pixel against the name it replaces and move a break.
    static float EditWrapWidth(HWND hwnd, float client_w) {
        const auto given = reinterpret_cast<UINT_PTR>(GetPropW(hwnd, L"Pulse.EditWrapW"));
        if (given) return std::max(1.0f, static_cast<float>(given) / 64.0f);
        return std::max(1.0f, client_w - 2.0f * EditPad(hwnd));
    }
    // Last painted wrapped layout and its origin, shared by mouse hit-testing
    // and Up/Down so both act on the lines actually on screen.
    struct EditPainted {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        float x = 0.0f;
        float y = 0.0f;
    };
    std::unordered_map<HWND, EditPainted> edit_painted;
    // A wrapped field is laid out with U+2028 at the file-name breaks the
    // icon grids use (file_name_breaks.h), so its layout positions run ahead
    // of the EDIT's by the separators before them. Kept per layout; entries
    // go once nothing else holds their layout.
    struct LayoutBreaks {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        pulse::ui::BrokenFileName breaks;
    };
    static std::vector<LayoutBreaks>& BreakTable() {
        static std::vector<LayoutBreaks> table;
        return table;
    }
    static const pulse::ui::BrokenFileName* BreaksOf(IDWriteTextLayout* layout) {
        if (!layout) return nullptr;
        for (const auto& entry : BreakTable())
            if (entry.layout.Get() == layout) return &entry.breaks;
        return nullptr;
    }
    static void RememberBreaks(IDWriteTextLayout* layout, pulse::ui::BrokenFileName breaks) {
        auto& table = BreakTable();
        table.erase(std::remove_if(table.begin(), table.end(), [](LayoutBreaks& entry) {
            entry.layout->AddRef();
            return entry.layout->Release() == 1;
        }), table.end());
        breaks.text.clear();
        table.push_back({layout, std::move(breaks)});
    }
    // EDIT position -> layout position (for a caret or the character there).
    static UINT32 ToLayoutPos(IDWriteTextLayout* layout, int position) {
        const UINT32 at = static_cast<UINT32>(std::max(0, position));
        const auto* breaks = BreaksOf(layout);
        return breaks ? breaks->ToLayout(at) : at;
    }
    // Layout position -> EDIT position; a separator maps to the line start.
    static int ToEditPos(IDWriteTextLayout* layout, UINT32 position) {
        const auto* breaks = BreaksOf(layout);
        return static_cast<int>(breaks ? breaks->ToName(position) : position);
    }
    // Up/Down keep the column they started from across short lines.
    HWND line_goal_hwnd = nullptr;
    int line_goal_caret = -1;
    float line_goal_x = 0.0f;
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
        float wrap = 0;
        float multiline = 0;
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

    // CoreText-like UI text: exact FreeType coverage with linear (gamma 1.0)
    // edges, light autohinting in y only (PingFang's CFF carries no useful
    // hints; x positions and HarfBuzz widths stay unhinted), and symmetric
    // outline dilation that grows with the text colour's luminance, the way
    // CoreGraphics font smoothing thickens light-on-dark text more than
    // dark-on-light. Constants come from fitting macOS 15 CoreText renders
    // (strength 0.0145 em dark text .. 0.030 em light text, capped) and are
    // scaled by 0.92 to offset the extra contrast of hinted rows.
    static void ApplySharpConfig(lt_render_config& config,
                                 const D2D1_COLOR_F& foreground) noexcept {
        config.coverage_gamma = 1.00f;
        config.coverage_contrast = 1.00f;
        config.raster_filter = LT_RASTER_FILTER_DIRECT;
        config.flags |= LT_RENDER_CONFIG_DISABLE_STEM_COMPENSATION |
                        LT_RENDER_CONFIG_AUTOHINT_LIGHT |
                        LT_RENDER_CONFIG_DISABLE_OPTICAL_GAMMA;
        const float luminance = std::clamp(0.2126f * foreground.r + 0.7152f * foreground.g +
                                           0.0722f * foreground.b, 0.0f, 1.0f);
        const float level = std::round(luminance * 4.0f) / 4.0f;
        constexpr float kFit = 0.92f;
        config.dilation_per_em = static_cast<std::uint8_t>(
            std::lround((0.0145f + 0.0155f * level) * kFit * 4096.0f));
        config.dilation_max = static_cast<std::uint8_t>(
            std::lround((0.45f + 0.20f * level) * kFit * 64.0f));
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
        wchar_t trace[16]{};
        const DWORD trace_length = GetEnvironmentVariableW(L"PULSE_STARTUP_TRACE", trace, ARRAYSIZE(trace));
        trace_timings = trace_length > 0 && trace_length < ARRAYSIZE(trace) && trace[0] != L'0';
#if defined(PULSE_STARTUP_TRACE_DIAGNOSTICS)
        // Runtime slow-frame reports need timings even without a startup trace file.
        trace_timings = true;
#endif
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
        renderer_desc.render_target = target_dc.Get();
        if (lt_d2d_renderer_create(context.get(), &renderer_desc, layout_renderer.put()) != LT_OK) {
            layout_renderer.reset();
        }

        if (!MakeFace(FontPath(L"msyh.ttc"), yahei_regular) ||
            !MakeFace(FontPath(L"msyhbd.ttc"), yahei_bold)) {
            return false;
        }

        // Win11's segoeui.ttf is a ~200 KB stub; Segoe UI Variable holds the
        // real outlines. Two faces share one mapped file in LumaText.
        const std::wstring variable = FontPath(L"SegUIVar.ttf");
        if (MakeFace(variable, segoe_regular, 400.0f) &&
            MakeFace(variable, segoe_bold, 700.0f)) {
            if (!MakeFace(variable, segoe_semibold, 600.0f)) segoe_semibold.reset();
        } else {
            segoe_regular.reset();
            segoe_bold.reset();
            if (!MakeFace(FontPath(L"segoeui.ttf"), segoe_regular) ||
                !MakeFace(FontPath(L"segoeuib.ttf"), segoe_bold)) {
                return false;
            }
        }

        // PingFang SC only when the user installed it; YaHei stays behind it
        // for anything PingFang lacks and on machines without it.
        if (!MakeFace(InstalledFontPath(L"PingFangSC-Regular.otf"), pingfang_regular) ||
            !MakeFace(InstalledFontPath(L"PingFangSC-Semibold.otf"), pingfang_semibold)) {
            pingfang_regular.reset();
            pingfang_semibold.reset();
        }

        // Faces are picked per grapheme: nearest weight first, then cascade
        // order, so Latin lands on Segoe and CJK on PingFang / YaHei.
        std::vector<lt_font_cascade_entry> entries;
        const auto push_weight = [&](std::uint16_t weight, const LumaText::FontFace& segoe,
                                     const LumaText::FontFace& pingfang,
                                     const LumaText::FontFace& yahei) {
            PushFace(entries, segoe, weight);
            PushFace(entries, pingfang, weight);
            PushFace(entries, yahei, weight);
        };
        push_weight(400, segoe_regular, pingfang_regular, yahei_regular);
        push_weight(600, segoe_semibold ? segoe_semibold : segoe_bold, pingfang_semibold, yahei_bold);
        push_weight(700, segoe_bold, pingfang_semibold, yahei_bold);
        if (entries.empty() || !MakeCascade(entries, text_cascade)) return false;

#if defined(PULSE_TEST_LUMATEXT_INIT_FAILURE)
        wchar_t fail_init[2]{};
        if (GetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_INIT_FAILURE", fail_init, ARRAYSIZE(fail_init)))
            return false;
#endif
        return true;
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
        text_cascade.reset();
        pingfang_semibold.reset();
        pingfang_regular.reset();
        segoe_bold.reset();
        segoe_semibold.reset();
        segoe_regular.reset();
        yahei_bold.reset();
        yahei_regular.reset();
        layout_renderer.reset();
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
        const auto layout_start = TraceClock();
        const auto layout_result = lt_text_layout_create(context.get(), &desc, layout.put());
        TraceElapsed(layout_start, stats.layout_time_us);
        if (layout_result != LT_OK) return nullptr;
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
            return Fail(stats.fail_input);
        }
        struct BusyGuard {
            bool& flag;
            explicit BusyGuard(bool& value) : flag(value) { flag = true; }
            ~BusyGuard() { flag = false; }
        } busy_guard(busy);
        const float width = bounds.right - bounds.left;
        const float height = bounds.bottom - bounds.top;
        const float font_size = format->GetFontSize();
        if (!(width > 0.0f) || !(height > 0.0f) || !(font_size > 0.0f)) return Fail(stats.fail_input);

        std::uint32_t family = 0;
        LumaText::FontCascade* cascade = nullptr;
        if (!ResolveCascade(format, text, family, cascade) || !cascade) return Fail(stats.fail_font);

        LayoutKey key;
        key.text.assign(text);
        key.family = family;
        key.weight = static_cast<std::uint16_t>(std::clamp<int>(
            static_cast<int>(format->GetFontWeight()), 1, 1000));
        key.size_64 = static_cast<std::int32_t>(std::lround(font_size * 64.0f));
        key.width_64 = kUnboundedWidth64;
        key.alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        LumaText::TextLayout* layout = GetLayout(key, font_size, *cascade);
        if (!layout || !*layout) return Fail(stats.fail_layout);

        auto metrics = LumaText::Descriptor<lt_text_metrics>();
        if (lt_text_layout_get_metrics(layout->get(), &metrics) != LT_OK) return Fail(stats.fail_backend);
        // Dest is an ink clip. Advance-only overflow left the last glyph's
        // optical weight / Mitchell lobe shaved off ("File" → "Fil").
        const float ink = pulse::ui::typography::InkPad(format);
        if (metrics.width + ink > width + 0.5f) {
            key.width_64 = static_cast<std::int32_t>(
                std::lround(std::max(1.0f, width - ink) * 64.0f));
            key.ellipsis = LT_TEXT_ELLIPSIS_END;
            layout = GetLayout(key, font_size, *cascade);
            if (!layout || !*layout) return Fail(stats.fail_layout);
            if (lt_text_layout_get_metrics(layout->get(), &metrics) != LT_OK) return Fail(stats.fail_backend);
            // An ellipsized layout that still overflows would be clipped
            // mid-glyph with no "..." (long Explorer verbs in the fixed-width
            // context menu). DirectWrite trims correctly; let it draw.
            if (metrics.line_count > 1 || metrics.width + ink > width + 0.5f) return Fail(stats.fail_bounds);
        }
        const float floor_x = std::floor(bounds.left);
        const float phase_x = bounds.left - floor_x;
        // Center the line, then put its baseline on a whole pixel row: a
        // half-pixel baseline split every horizontal stroke over two rows.
        const float base_y = std::round(bounds.top);
        const std::int32_t baseline_px = static_cast<std::int32_t>(std::lround(
            bounds.top + (height - metrics.height) * 0.5f + metrics.ascent - base_y));
        const auto phase_bucket = [](float phase, int buckets) {
            const long rounded = std::lround(phase * static_cast<float>(buckets));
            return static_cast<std::uint8_t>(std::clamp<long>(
                rounded, 0l, static_cast<long>(buckets - 1)));
        };
        const std::uint8_t x_phase_8 = phase_bucket(phase_x, 8);
        const float cached_phase_x = static_cast<float>(x_phase_8) / 8.0f;
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
            baseline_px,
            packed_foreground,
            packed_background,
            dark,
        };
        if (auto cached = surfaces.find(surface_key); cached != surfaces.end()) {
                TouchSurface(cached);
                const auto offset = D2D1::Point2F(
                    floor_x + phase_x - cached_phase_x, base_y);
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
        if (FAILED(recording_dc->CreateCommandList(&commands)) || !commands) return Fail(stats.fail_backend);
        recording_dc->SetTarget(commands.Get());
        recording_dc->BeginDraw();
        auto frame_desc = LumaText::Descriptor<lt_frame_desc>();
        frame_desc.dpi_x = 96.0f;
        frame_desc.dpi_y = 96.0f;
        LumaText::Frame frame;
        if (lt_frame_begin(renderer.get(), &frame_desc, frame.put()) != LT_OK) {
            recording_dc->EndDraw();
            recording_dc->SetTarget(nullptr);
            return Fail(stats.fail_backend);
        }

        auto draw = LumaText::Descriptor<lt_draw_text_desc>();
        draw.origin_x = cached_phase_x;
        if (alignment == DWRITE_TEXT_ALIGNMENT_CENTER) {
            draw.origin_x += std::max(0.0f, (width - metrics.width) * 0.5f);
        } else if (alignment == DWRITE_TEXT_ALIGNMENT_TRAILING) {
            draw.origin_x += std::max(0.0f, width - metrics.width);
        }
        draw.origin_y = static_cast<float>(baseline_px) - metrics.ascent;
        draw.clip = {cached_phase_x, 0.0f,
                     cached_phase_x + width, std::max(1.0f, std::round(height))};
        draw.clip_enabled = true;
        draw.foreground = {foreground.r, foreground.g, foreground.b, foreground.a};
        draw.background = {background.r, background.g, background.b, background.a};
        draw.background_type = LT_BACKGROUND_TRANSPARENT;
        draw.render_config = LumaText::Descriptor<lt_render_config>();
        ApplySharpConfig(draw.render_config, foreground);
        // No render profile: it would pick a config by background and drop
        // the foreground-dependent dilation set above.

        const auto draw_start = TraceClock();
        const lt_result result = lt_frame_draw_text_layout(frame.get(), layout->get(), &draw);
        TraceElapsed(draw_start, stats.draw_time_us);
        auto frame_stats = LumaText::Descriptor<lt_frame_stats>();
        if (result == LT_OK && lt_frame_get_stats(frame.get(), &frame_stats) == LT_OK) {
            stats.freetype_glyphs += frame_stats.freetype_glyphs;
            stats.cache_hits += frame_stats.glyph_cache_hits;
            if (trace_timings) {
                stats.shaping_time_us += frame_stats.shaping_time_us;
                stats.raster_time_us += frame_stats.raster_time_us;
                stats.upload_time_us += frame_stats.upload_time_us;
                stats.composition_time_us += frame_stats.composition_time_us;
            }
        }
        const auto end_start = TraceClock();
        const lt_result end_result = lt_frame_end(frame.get());
        TraceElapsed(end_start, stats.frame_end_time_us);
        const auto recording_start = TraceClock();
        const HRESULT recording_result = recording_dc->EndDraw();
        TraceElapsed(recording_start, stats.recording_end_time_us);
        recording_dc->SetTarget(nullptr);
        if (result != LT_OK || end_result != LT_OK || FAILED(recording_result) ||
            FAILED(commands->Close())) {
            return Fail(stats.fail_backend);
        }

        const float ink_width = std::min(width, std::max(1.0f, metrics.width + 2.0f));
        const std::uint64_t estimated_bytes = static_cast<std::uint64_t>(
            std::max(1l, std::lround(std::ceil(ink_width)))) *
            static_cast<std::uint64_t>(std::max(1l, std::lround(std::ceil(height)))) * 4ull;
        StoreSurface(surface_key, commands, estimated_bytes);

        const auto offset = D2D1::Point2F(floor_x + phase_x - cached_phase_x, base_y);
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

    // A DirectWrite layout keeps its own shaping, fallback fonts, wrapping and
    // trimming; LumaText only rasterizes the glyph runs. Ranges coloured with
    // SetDrawingEffect(ID2D1SolidColorBrush) keep their colour.
    bool DrawLayout(D2D1_POINT_2F origin, IDWriteTextLayout* layout,
                    const D2D1_COLOR_F& foreground, bool clip) {
        if (!layout_renderer || !target_dc || !layout) return Fail(stats.fail_input);
        float dpi_x = 96.0f, dpi_y = 96.0f;
        target_dc->GetDpi(&dpi_x, &dpi_y);
        auto frame_desc = LumaText::Descriptor<lt_frame_desc>();
        frame_desc.dpi_x = dpi_x;
        frame_desc.dpi_y = dpi_y;
        LumaText::Frame frame;
        if (lt_frame_begin(layout_renderer.get(), &frame_desc, frame.put()) != LT_OK) return Fail(stats.fail_backend);
        auto draw = LumaText::Descriptor<lt_draw_text_desc>();
        draw.origin_x = origin.x;
        draw.origin_y = origin.y;
        draw.clip_enabled = clip;
        if (clip) {
            draw.clip = {origin.x, origin.y, origin.x + layout->GetMaxWidth(),
                         origin.y + layout->GetMaxHeight()};
        }
        draw.foreground = {foreground.r, foreground.g, foreground.b, foreground.a};
        draw.background_type = LT_BACKGROUND_TRANSPARENT;
        draw.render_config = LumaText::Descriptor<lt_render_config>();
        ApplySharpConfig(draw.render_config, foreground);
        const auto draw_start = TraceClock();
        const lt_result result = lt_frame_draw_layout(frame.get(), layout, &draw);
        TraceElapsed(draw_start, stats.draw_time_us);
        auto frame_stats = LumaText::Descriptor<lt_frame_stats>();
        if (result == LT_OK && lt_frame_get_stats(frame.get(), &frame_stats) == LT_OK) {
            stats.freetype_glyphs += frame_stats.freetype_glyphs;
            stats.cache_hits += frame_stats.glyph_cache_hits;
            if (trace_timings) {
                stats.shaping_time_us += frame_stats.shaping_time_us;
                stats.raster_time_us += frame_stats.raster_time_us;
                stats.upload_time_us += frame_stats.upload_time_us;
                stats.composition_time_us += frame_stats.composition_time_us;
            }
        }
        const auto end_start = TraceClock();
        const lt_result end_result = lt_frame_end(frame.get());
        TraceElapsed(end_start, stats.frame_end_time_us);
        stats.draw_calls++;
        return (result == LT_OK && end_result == LT_OK) || Fail(stats.fail_backend);
    }

    // Locate the extension with the complete shaped line, not a separately
    // measured prefix. Simple Latin extensions can then share one pixel phase.
    bool DrawSplit(std::wstring_view text, IDWriteTextFormat* format,
                   const D2D1_RECT_F& bounds, const D2D1_COLOR_F& foreground,
                   std::uint32_t split, const D2D1_COLOR_F& split_foreground,
                   const D2D1_COLOR_F& background) {
        ID2D1DeviceContext* dc = target_dc.Get();
        if (!dc || busy || !renderer || !format || split == 0 || split >= text.size()) return Fail(stats.fail_input);
        std::uint32_t family = 0;
        LumaText::FontCascade* cascade = nullptr;
        const float font_size = format->GetFontSize();
        if (!(font_size > 0.0f)) return Fail(stats.fail_input);
        if (!ResolveCascade(format, text, family, cascade) || !cascade) return Fail(stats.fail_font);
        LayoutKey key;
        key.text.assign(text);
        key.family = family;
        key.weight = static_cast<std::uint16_t>(std::clamp<int>(
            static_cast<int>(format->GetFontWeight()), 1, 1000));
        key.size_64 = static_cast<std::int32_t>(std::lround(font_size * 64.0f));
        key.width_64 = kUnboundedWidth64;
        key.alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        LumaText::TextLayout* layout = GetLayout(key, font_size, *cascade);
        if (!layout || !*layout) return Fail(stats.fail_layout);
        auto metrics = LumaText::Descriptor<lt_text_metrics>();
        if (lt_text_layout_get_metrics(layout->get(), &metrics) != LT_OK) return Fail(stats.fail_backend);
        // DrawOn would ellipsize this line, and the split would no longer
        // index the drawn glyphs.
        const float ink = pulse::ui::typography::InkPad(format);
        if (metrics.width + ink > bounds.right - bounds.left + 0.5f) return Fail(stats.fail_bounds);
        float split_x = 0.0f, split_y = 0.0f;
        auto hit = LumaText::Descriptor<lt_hit_test_metrics>();
        if (lt_text_layout_hit_test_position(layout->get(), split, false,
                                             &split_x, &split_y, &hit) != LT_OK) {
            return Fail(stats.fail_backend);
        }
        const float cut = std::round(bounds.left + split_x);
        const auto tail = text.substr(split);
        const bool stable_tail = tail.front() == L'.' && std::all_of(tail.begin() + 1, tail.end(), [](wchar_t c) {
            return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                   (c >= L'0' && c <= L'9') || c == L'_';
        });
        float tail_width = 0;
        if (stable_tail && !Measure(tail, format, tail_width, nullptr)) return Fail(stats.fail_backend);
        const float top = bounds.top - 8.0f, bottom = bounds.bottom + 8.0f;
        dc->PushAxisAlignedClip(D2D1::RectF(std::floor(bounds.left) - 8.0f, top, cut, bottom),
                                D2D1_ANTIALIAS_MODE_ALIASED);
        const bool head = DrawOn(dc, text, format, bounds, foreground, background,
                                 DWRITE_TEXT_ALIGNMENT_LEADING);
        dc->PopAxisAlignedClip();
        if (!head) return false; // DrawOn already recorded the failure category.
        dc->PushAxisAlignedClip(D2D1::RectF(cut, top, std::ceil(bounds.right) + 8.0f, bottom),
                                D2D1_ANTIALIAS_MODE_ALIASED);
        if (stable_tail) {
            // The suffix starts on a whole pixel regardless of the prefix's
            // fractional advance. A fixed width also reuses its cached surface
            // across filenames; the full-line fit above still owns truncation.
            const auto tail_bounds = D2D1::RectF(cut, bounds.top, cut + tail_width + ink + 1.0f, bounds.bottom);
            DrawOn(dc, tail, format, tail_bounds, split_foreground, background,
                   DWRITE_TEXT_ALIGNMENT_LEADING);
        } else {
            // Keep contextual shaping intact for non-Latin extensions.
            DrawOn(dc, text, format, bounds, split_foreground, background,
                   DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        dc->PopAxisAlignedClip();
        return true;
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
        // Chinese runs shape as zh-CN even when the shell language (and the
        // DirectWrite family) is English / Segoe; the faces are the same.
        family = (yahei || ContainsCjk(text)) ? kYaHei : kSegoe;
        cascade = &text_cascade;
        return text_cascade.get() != nullptr;
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
        // A password field (an API key) is laid out, hit-tested and drawn as
        // its mask: one character per character, so caret indices still match.
        if (const auto mask = static_cast<wchar_t>(SendMessageW(hwnd, EM_GETPASSWORDCHAR, 0, 0)))
            std::fill(text.begin(), text.end(), mask);
        return text;
    }

    Microsoft::WRL::ComPtr<IDWriteTextLayout> DwEditLayout(std::wstring_view text,
                                                           IDWriteTextFormat* format,
                                                           float height, float wrap_width = 0.0f,
                                                           float multiline = 0.0f) {
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
                    entry.tab_stop == tab_stop && entry.wrap == wrap_width && entry.multiline == multiline &&
                    entry.text == text) {
                    ++stats.edit_layout_cache_hits;
                    return entry.layout;
                }
            }
        }
        // Wrapped fields break where the icon grid breaks the same name:
        // same rules, same DirectWrite measure, same one-pixel spare.
        pulse::ui::BrokenFileName broken;
        const bool names = wrap_width > 0.0f && multiline <= 0.0f;
        if (names) {
            broken = pulse::ui::BreakFileName([&](const std::wstring& part) {
                return pulse::ui::typography::MeasureAdvance(dw_factory.Get(), format, part);
            }, std::wstring(text), std::max(1.0f, wrap_width - 1.0f));
        }
        const std::wstring_view laid = names ? std::wstring_view(broken.text) : text;
        if (FAILED(dw_factory->CreateTextLayout(laid.data(), static_cast<UINT32>(laid.size()),
                format, wrap_width > 0.0f ? wrap_width : 1.0e6f, line_height, &layout)) || !layout) {
            return nullptr;
        }
        if (!broken.breaks.empty()) RememberBreaks(layout.Get(), broken);
        if (multiline > 0.0f) {
            // Body text: wraps at word breaks, starts at the reading edge, one
            // uniform line height (baseline as the panel's own text, 0.78).
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, multiline, multiline * 0.78f);
        } else if (wrap_width > 0.0f) {
            // Wrapped fields center their lines like the icon names they edit.
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
        // One unwrapped, untrimmed line; the edit scrolls it horizontally.
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        // Keep the unbounded line at the physical left origin even for RTL
        // paragraphs; LEADING would place those glyphs near x = 1e6.
        layout->SetTextAlignment(reading == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT ?
            DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        const DWRITE_TRIMMING none{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
        layout->SetTrimming(&none, nullptr);
        ++stats.edit_layout_builds;
        if (cacheable) {
            auto& entry = edit_layouts[next_edit_layout];
            next_edit_layout = (next_edit_layout + 1) % edit_layouts.size();
            entry = {std::wstring(text), format, layout, generation, line_height, font_size, reading, flow,
                spacing_method, line_spacing, baseline, tab_stop, wrap_width, multiline};
        }
        return layout;
    }

    // Up/Down in a wrapped field move to the neighbouring line at the same
    // column; past the first or last line they go to the start or end.
    bool MoveEditLine(HWND hwnd, bool down, bool extend) {
        if (!EditWraps(hwnd)) return false;
        const auto shown = edit_painted.find(hwnd);
        if (shown == edit_painted.end() || !shown->second.layout) return false;
        IDWriteTextLayout* layout = shown->second.layout.Get();
        const int length = GetWindowTextLengthW(hwnd);
        DWORD lo = 0, hi = 0;
        SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&lo), reinterpret_cast<LPARAM>(&hi));
        int anchor = static_cast<int>(lo), caret = static_cast<int>(hi);
        if (const auto saved = edit_selections.find(hwnd); saved != edit_selections.end() &&
            std::min(saved->second.first, saved->second.second) == static_cast<int>(lo) &&
            std::max(saved->second.first, saved->second.second) == static_cast<int>(hi)) {
            anchor = saved->second.first;
            caret = saved->second.second;
        }
        // Without Shift a selection collapses from the edge in that direction.
        if (!extend && lo != hi) caret = down ? static_cast<int>(hi) : static_cast<int>(lo);
        caret = std::clamp(caret, 0, length);
        float x = 0.0f, y = 0.0f, line_h = 0.0f;
        DwCaretPoint(layout, caret, length, x, y, line_h);
        if (line_goal_hwnd == hwnd && line_goal_caret == caret) x = line_goal_x;
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        const float target_y = down ? y + std::max(1.0f, line_h) * 1.5f : y - std::max(1.0f, line_h) * 0.5f;
        int target = 0;
        if (target_y < 0.0f) target = 0;
        else if (target_y >= metrics.height) target = length;
        else target = DwHitIndex(layout, x, length, target_y);
        if (!extend) anchor = target;
        SendMessageW(hwnd, EM_SETSEL, static_cast<WPARAM>(anchor), static_cast<LPARAM>(target));
        line_goal_hwnd = hwnd;
        line_goal_caret = target;
        line_goal_x = x;
        return true;
    }

    static float DwCaretX(IDWriteTextLayout* layout, int position, int length) {
        if (!layout || length <= 0) return 0.0f;
        const bool trailing = position >= length;
        const UINT32 probe = static_cast<UINT32>(trailing ? length - 1 : position);
        float x = 0.0f;
        float y = 0.0f;
        DWRITE_HIT_TEST_METRICS hit{};
        if (FAILED(layout->HitTestTextPosition(ToLayoutPos(layout, static_cast<int>(probe)),
                trailing ? TRUE : FALSE, &x, &y, &hit)))
            return 0.0f;
        return x;
    }

    // Caret top-left and line height; works on empty text too, which a
    // centered wrapped field places in the middle of its line.
    static void DwCaretPoint(IDWriteTextLayout* layout, int position, int length,
                             float& x, float& y, float& height) {
        x = 0.0f; y = 0.0f; height = 0.0f;
        if (!layout) return;
        const bool trailing = length > 0 && position >= length;
        const UINT32 probe = static_cast<UINT32>(trailing ? length - 1 : std::max(0, position));
        DWRITE_HIT_TEST_METRICS hit{};
        if (FAILED(layout->HitTestTextPosition(ToLayoutPos(layout, static_cast<int>(probe)),
                trailing ? TRUE : FALSE, &x, &y, &hit))) return;
        y = hit.top;
        height = hit.height;
    }

    static int DwHitIndex(IDWriteTextLayout* layout, float x, int length, float y = 1.0f) {
        if (!layout || length <= 0) return 0;
        BOOL trailing = FALSE;
        BOOL inside = FALSE;
        DWRITE_HIT_TEST_METRICS hit{};
        if (FAILED(layout->HitTestPoint(x, y, &trailing, &inside, &hit))) return length;
        const int index = ToEditPos(layout, hit.textPosition + (trailing ? hit.length : 0));
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
    static int DwCharIndex(IDWriteTextLayout* layout, float x, int length, float y = 1.0f) {
        if (!layout || length <= 0) return 0;
        BOOL trailing = FALSE;
        BOOL inside = FALSE;
        DWRITE_HIT_TEST_METRICS hit{};
        if (FAILED(layout->HitTestPoint(x, y, &trailing, &inside, &hit))) return length - 1;
        // Past a wrapped line's end the hit is its separator: the line's
        // last character, not the next line's first.
        int index = ToEditPos(layout, hit.textPosition);
        if (index > 0 && ToLayoutPos(layout, index) != hit.textPosition) --index;
        return std::clamp(index, 0, length - 1);
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
        const bool wraps = EditWraps(hwnd);
        const auto dw = DwEditLayout(text, format, static_cast<float>(rc.bottom - rc.top),
                                     wraps ? EditWrapWidth(hwnd, width) : 0.0f, EditMultilineLine(hwnd));
        if (!dw) return;
        const float caret_x = DwCaretX(dw.Get(), caret_for_scroll, length);
        // Hit-test against the offset last painted, not one re-derived from
        // the caret: deriving it again slid the text under a held pointer.
        const auto painted = edit_scrolls.find(hwnd);
        const float scroll = painted != edit_scrolls.end()
            ? painted->second : EditScroll(hwnd, dw.Get(), caret_x, width);
        float text_x = static_cast<float>(GET_X_LPARAM(lParam)) - kEditPad + scroll;
        float text_y = 1.0f;
        if (wraps) {
            const auto shown = edit_painted.find(hwnd);
            const float origin_x = shown != edit_painted.end() ? shown->second.x : EditPad(hwnd);
            const float origin_y = shown != edit_painted.end() ? shown->second.y : 0.0f;
            text_x = static_cast<float>(GET_X_LPARAM(lParam)) - origin_x;
            text_y = static_cast<float>(GET_Y_LPARAM(lParam)) - origin_y;
        }
        const int index = DwHitIndex(dw.Get(), text_x, length, text_y);
        ++stats.edit_directwrite_hits;

        if (msg == WM_LBUTTONDBLCLK) {
            const auto [start, end] = EditWordRangeAt(text, DwCharIndex(dw.Get(), text_x, length, text_y));
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
            const auto [start, end] = EditWordRangeAt(text, DwCharIndex(dw.Get(), text_x, length, text_y));
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
                   const D2D1_COLOR_F& foreground, const D2D1_COLOR_F& background, const D2D1_RECT_F* surface_bounds = nullptr) {
        if (!hwnd || !format || !blit_dc || !dw_factory) return false;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return false;

        PULSE_PICKER_TRACE_POINT("edit-ensure-bitmap-before");
        if (!EnsureBlit(w, h)) return false;
        PULSE_PICKER_TRACE_POINT("edit-ensure-bitmap-after");

        PULSE_PICKER_TRACE_POINT("edit-read-before");
        std::wstring text = ReadEditText(hwnd);
        PULSE_PICKER_TRACE_POINT("edit-read-after");

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

        const bool wraps = EditWraps(hwnd);
        const float wrap_width = wraps ? EditWrapWidth(hwnd, static_cast<float>(w)) : 0.0f;
        const float multiline = EditMultilineLine(hwnd);
        PULSE_PICKER_TRACE_POINT("edit-layout-before");
        const auto dw_layout = DwEditLayout(text, format, static_cast<float>(h), wrap_width, multiline);
        PULSE_PICKER_TRACE_POINT("edit-layout-after");
        PULSE_PICKER_TRACE_POINT("edit-font-params-before");
        IDWriteRenderingParams* params = DwParams(hwnd);
        PULSE_PICKER_TRACE_POINT("edit-font-params-after");
        if (!dw_layout || !params) return false;
        const auto caret_origin = [&](int position) {
            return DwCaretX(dw_layout.Get(), position, static_cast<int>(text.size()));
        };
        float caret_x = caret_origin(caret);
        const float pad = EditPad(hwnd);
        // Wrapped fields never scroll: every line is visible, and the caret
        // and selection carry the line's vertical position instead.
        float caret_top = 2.0f;
        float caret_bottom = static_cast<float>(h) - 2.0f;
        float wrap_y = 0.0f;
        if (wraps) {
            DWRITE_TEXT_METRICS wrapped{};
            // Multi-line text starts at the top like the owner's own text.
            if (SUCCEEDED(dw_layout->GetMetrics(&wrapped)))
                wrap_y = multiline > 0.0f ? -wrapped.top
                                          : std::round((static_cast<float>(h) - wrapped.height) * 0.5f) - wrapped.top;
            float line_y = 0.0f, line_h = 0.0f;
            DwCaretPoint(dw_layout.Get(), caret, static_cast<int>(text.size()), caret_x, line_y, line_h);
            // Taller than the field (the rename field stops at six lines): the
            // lines scroll inside it, keeping the caret's line in view.
            const float inset = multiline > 0.0f ? 0.0f : 1.0f;
            const float view_h = static_cast<float>(h) - 2.0f * inset;
            if (wrapped.height > view_h) {
                const auto saved = edit_vscrolls.find(hwnd);
                float scroll_y = saved != edit_vscrolls.end() ? saved->second : 0.0f;
                const float line_top = line_y - wrapped.top;
                if (line_top < scroll_y) scroll_y = line_top;
                if (line_top + line_h > scroll_y + view_h) scroll_y = line_top + line_h - view_h;
                scroll_y = std::clamp(scroll_y, 0.0f, wrapped.height - view_h);
                edit_vscrolls[hwnd] = scroll_y;
                wrap_y = inset - scroll_y - wrapped.top;
            } else {
                edit_vscrolls.erase(hwnd);
            }
            caret_top = wrap_y + line_y + 1.0f;
            caret_bottom = wrap_y + line_y + std::max(2.0f, line_h) - 1.0f;
            edit_painted[hwnd] = {dw_layout, pad, wrap_y};
            // The EDIT's own caret still tracks its one unwrapped line; keep
            // it (IME and accessibility follow it) on the painted caret.
            if (GetFocus() == hwnd)
                SetCaretPos(static_cast<int>(std::floor(pad + caret_x)), static_cast<int>(caret_top));
        }
        const float scroll = wraps ? 0.0f : EditScroll(hwnd, dw_layout.Get(), caret_x, static_cast<float>(w));
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
            const UINT32 range_lo = ToLayoutPos(dw_layout.Get(), sel_lo);
            const UINT32 range_n = ToLayoutPos(dw_layout.Get(), sel_hi) - range_lo;
            dw_layout->HitTestTextRange(range_lo, range_n, pad - scroll, wrap_y, nullptr, 0, &count);
            std::vector<DWRITE_HIT_TEST_METRICS> ranges(count);
            if (count && SUCCEEDED(dw_layout->HitTestTextRange(range_lo, range_n,
                pad - scroll, wrap_y, ranges.data(), count, &count))) {
                for (const auto& range : ranges)
                    blit_dc->FillRectangle(wraps
                        ? D2D1::RectF(range.left, range.top, range.left + range.width, range.top + range.height)
                        : D2D1::RectF(range.left, 1.0f, range.left + range.width, static_cast<float>(h) - 1.0f),
                        brush.Get());
                selection_drawn = true;
            }
        }
        std::wstring visible = text;
        D2D1_COLOR_F visible_color = foreground;
        bool cue_shown = false;
        if (visible.empty()) {
            wchar_t cue[256]{};
            // A multi-line EDIT has no cue banner; its owner hands the text over.
            const auto* owner_cue = static_cast<const wchar_t*>(GetPropW(hwnd, L"Pulse.EditCue"));
            if (multiline > 0.0f && owner_cue && owner_cue[0] != 0) {
                visible = owner_cue;
                // The owner's placeholder colour (0x01BBGGRR), else 45 % text.
                const auto cue_rgb = reinterpret_cast<UINT_PTR>(GetPropW(hwnd, L"Pulse.EditCueColor"));
                visible_color = cue_rgb
                    ? D2D1::ColorF(GetRValue(cue_rgb) / 255.0f, GetGValue(cue_rgb) / 255.0f,
                                   GetBValue(cue_rgb) / 255.0f, 1.0f)
                    : D2D1::ColorF(foreground.r, foreground.g, foreground.b, foreground.a * 0.45f);
            } else if (SendMessageW(hwnd, EM_GETCUEBANNER, reinterpret_cast<WPARAM>(cue),
                             ARRAYSIZE(cue)) && cue[0] != 0) {
                visible = cue;
                cue_shown = true;
                visible_color = D2D1::ColorF(foreground.r, foreground.g, foreground.b,
                                             foreground.a * 0.45f);
            }
        }
        bool text_drawn = visible.empty();
        if (!visible.empty()) {
            // Same layout, params and grayscale AA as the surrounding UI text.
            auto visible_layout = visible == text && dw_layout
                ? dw_layout
                : DwEditLayout(visible, format, static_cast<float>(h), multiline > 0.0f ? wrap_width : 0.0f, multiline);
            // A host can ask for a smaller hint than the typed text (quick
            // search); the cached layout is shared, so size a private copy.
            const auto cue_px = static_cast<float>(
                reinterpret_cast<INT_PTR>(GetPropW(hwnd, L"Pulse.EditCueSize")));
            if (cue_shown && cue_px > 0 && visible_layout && dw_factory) {
                Microsoft::WRL::ComPtr<IDWriteTextLayout> hint;
                if (SUCCEEDED(dw_factory->CreateTextLayout(visible.c_str(),
                        static_cast<UINT32>(visible.size()), format, 100000.0f,
                        static_cast<float>(h), &hint))) {
                    hint->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    hint->SetFontSize(cue_px, {0, static_cast<UINT32>(visible.size())});
                    visible_layout = hint;
                }
            }
            if (visible_layout && brush) {
                DWRITE_TEXT_METRICS metrics{};
                if (FAILED(visible_layout->GetMetrics(&metrics))) {
                    blit_dc->EndDraw();
                    blit_dc->SetTarget(nullptr);
                    return false;
                }
                const float y = wraps && visible == text ? wrap_y
                    : multiline > 0.0f ? -metrics.top
                    : std::round((static_cast<float>(h) - metrics.height) * 0.5f) - metrics.top;
                brush->SetColor(visible_color);
                // A wrapped file name dims its extension like the icon grid.
                DWRITE_TEXT_RANGE ext_range{0, 0};
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ext_brush;
                if (wraps && visible == text && GetPropW(hwnd, L"Pulse.EditDimExt")) {
                    const size_t dot = pulse::ui::FileNameExtensionDot(text);
                    if (dot != std::wstring::npos) {
                        const UINT32 lo = ToLayoutPos(visible_layout.Get(), static_cast<int>(dot));
                        const UINT32 hi = ToLayoutPos(visible_layout.Get(), static_cast<int>(text.size()));
                        if (hi > lo && SUCCEEDED(blit_dc->CreateSolidColorBrush(D2D1::ColorF(visible_color.r,
                                visible_color.g, visible_color.b, visible_color.a * 0.6f), &ext_brush))) {
                            ext_range = {lo, hi - lo};
                            visible_layout->SetDrawingEffect(ext_brush.Get(), ext_range);
                        }
                    }
                }
                // A leading marker the owner names (the command palette's ">")
                // takes the owner's colour; a hint follows it while nothing
                // else is typed. Colour only, so the caret metrics hold.
                const auto* lead = static_cast<const wchar_t*>(GetPropW(hwnd, L"Pulse.EditLead"));
                const size_t lead_n = lead ? wcslen(lead) : 0;
                const bool lead_on = !wraps && multiline <= 0.0f && visible == text && lead_n > 0 &&
                                     text.compare(0, lead_n, lead) == 0;
                DWRITE_TEXT_RANGE lead_range{0, 0};
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> lead_brush;
                if (lead_on) {
                    const auto rgb = reinterpret_cast<UINT_PTR>(GetPropW(hwnd, L"Pulse.EditLeadColor"));
                    const UINT32 hi = ToLayoutPos(visible_layout.Get(), static_cast<int>(lead_n));
                    if (rgb && hi > 0 && SUCCEEDED(blit_dc->CreateSolidColorBrush(D2D1::ColorF(
                            GetRValue(rgb) / 255.0f, GetGValue(rgb) / 255.0f, GetBValue(rgb) / 255.0f, 1.0f),
                            &lead_brush))) {
                        lead_range = {0, hi};
                        visible_layout->SetDrawingEffect(lead_brush.Get(), lead_range);
                    }
                }
                blit_dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                blit_dc->SetTextRenderingParams(params);
                blit_dc->DrawTextLayout(D2D1::Point2F(std::round(pad - scroll), y), visible_layout.Get(),
                                        brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                // The layout is cached and shared: leave no brush on it.
                if (ext_range.length) visible_layout->SetDrawingEffect(nullptr, ext_range);
                if (lead_range.length) visible_layout->SetDrawingEffect(nullptr, lead_range);
                const auto* tail = static_cast<const wchar_t*>(GetPropW(hwnd, L"Pulse.EditTail"));
                if (lead_on && text.size() == lead_n && tail && tail[0] != 0) {
                    const auto tail_layout = DwEditLayout(tail, format, static_cast<float>(h), 0.0f, 0.0f);
                    DWRITE_TEXT_METRICS tm{};
                    if (tail_layout && SUCCEEDED(tail_layout->GetMetrics(&tm))) {
                        const float gap = std::round(format->GetFontSize() * 0.5f);
                        const float tx = std::round(pad - scroll + metrics.widthIncludingTrailingWhitespace + gap);
                        const float ty = std::round((static_cast<float>(h) - tm.height) * 0.5f) - tm.top;
                        brush->SetColor(D2D1::ColorF(foreground.r, foreground.g, foreground.b, foreground.a * 0.45f));
                        blit_dc->DrawTextLayout(D2D1::Point2F(tx, ty), tail_layout.Get(), brush.Get(),
                                                D2D1_DRAW_TEXT_OPTIONS_NONE);
                    }
                }
                blit_dc->SetTextRenderingParams(nullptr);
                text_drawn = true;
            }
        }
        const UINT blink = GetCaretBlinkTime();
        const auto epoch = caret_epochs.find(hwnd);
        const DWORD since = GetTickCount() - (epoch != caret_epochs.end() ? epoch->second : 0u);
        const bool caret_on = GetFocus() == hwnd && GetCapture() != hwnd &&
            (blink == INFINITE || ((since / std::max(1u, blink)) % 2u) == 0u);
        if (caret_on && brush) {
            brush->SetColor(foreground);
            // Fill whole pixels: a half-pixel origin splits the caret into two
            // half-intensity columns that blend into the first placeholder glyph.
            DWORD caret_width = 1;
            SystemParametersInfoW(SPI_GETCARETWIDTH, 0, &caret_width, 0);
            const float x = std::floor(pad + caret_x - scroll);
            blit_dc->FillRectangle(
                D2D1::RectF(x, caret_top, x + static_cast<float>(std::max<DWORD>(1, caret_width)),
                            caret_bottom),
                brush.Get());
        }
        PULSE_PICKER_TRACE_POINT("edit-blit-end-draw-before");
        const HRESULT end = blit_dc->EndDraw();
        PULSE_PICKER_TRACE_POINT("edit-blit-end-draw-after");
        blit_dc->SetTarget(nullptr);
        if (FAILED(end) || !text_drawn || !selection_drawn || !brush) return false;
        ++stats.edit_directwrite_draws;
        stats.edit_rendering_mode = params->GetRenderingMode();
        bool presented = false;
        if (surface_bounds) {
            if (!target_dc) return false;
            D2D1_MATRIX_3X2_F previous{}; target_dc->GetTransform(&previous);
            target_dc->SetTransform(D2D1::Matrix3x2F::Identity());
            target_dc->PushAxisAlignedClip(*surface_bounds, D2D1_ANTIALIAS_MODE_ALIASED);
            target_dc->DrawBitmap(blit_target.Get(), surface_bounds, 1.0f,
                D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr, nullptr);
            target_dc->PopAxisAlignedClip();
            target_dc->SetTransform(previous);
            // blit_target is reused for the next editor. Finish its main-surface
            // draw before another edit changes this shared bitmap's contents.
            PULSE_PICKER_TRACE_POINT("main-flush-before");
            presented = SUCCEEDED(target_dc->Flush());
            PULSE_PICKER_TRACE_POINT("main-flush-after");
        } else {
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
            presented = PresentEditBits(hwnd, hdc, dib_bits, w, h, background.a < 1.0f);
            blit_staging->Unmap();
        }

        HIMC imc = ImmGetContext(hwnd);
        if (imc) {
            COMPOSITIONFORM form{};
            form.dwStyle = CFS_POINT;
            form.ptCurrentPos = {static_cast<LONG>(pad + caret_x - scroll),
                                 wraps ? static_cast<LONG>(caret_top - 1.0f) : 0};
            ImmSetCompositionWindow(imc, &form);
            // The EDIT cannot answer IMR_QUERYCHARPOSITION, so without this an
            // IME guesses where candidates go; past the end of a long name it
            // put them at the top-left of the window (#188).
            CANDIDATEFORM candidate{};
            candidate.dwIndex = 0;
            candidate.dwStyle = CFS_EXCLUDE;
            candidate.ptCurrentPos = form.ptCurrentPos;
            candidate.rcArea = {form.ptCurrentPos.x, wraps ? form.ptCurrentPos.y : 0, form.ptCurrentPos.x + 1,
                                wraps ? static_cast<LONG>(std::ceil(caret_bottom + 1.0f)) : h};
            ImmSetCandidateWindow(imc, &candidate);
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
    if (message == WM_KEYDOWN && (wparam == VK_UP || wparam == VK_DOWN) && self->impl_ &&
        (GetKeyState(VK_CONTROL) & 0x8000) == 0 &&
        self->impl_->MoveEditLine(hwnd, wparam == VK_DOWN, (GetKeyState(VK_SHIFT) & 0x8000) != 0))
        return 0;
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
            impl_->edit_vscrolls.erase(hwnd);
            impl_->caret_epochs.erase(hwnd);
            impl_->edit_painted.erase(hwnd);
        }
        return;
    }
    // New text starts at the left edge, as native EDIT does.
    if (message == WM_SETTEXT) {
        impl_->edit_scrolls.erase(hwnd);
        impl_->edit_vscrolls.erase(hwnd);
    }
    if (message == WM_SETFOCUS || message == EM_SETSEL || message == WM_KEYDOWN ||
        message == WM_CHAR || message == WM_SETTEXT || message == EM_REPLACESEL ||
        message == WM_CUT || message == WM_PASTE || message == WM_CLEAR || message == WM_UNDO ||
        message == EM_UNDO || message == WM_IME_COMPOSITION || message == WM_IME_ENDCOMPOSITION)
        impl_->caret_epochs[hwnd] = GetTickCount();
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
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_LBUTTONUP)
            impl_->caret_epochs[hwnd] = GetTickCount();
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
    // The renderer is allocated before fonts and cascades. A late
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
        impl_->edit_vscrolls.clear();
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

bool LumaTextRenderer::DrawLayout(D2D1_POINT_2F origin, IDWriteTextLayout* layout,
                                  const D2D1_COLOR_F& foreground, bool clip) {
#if defined(PULSE_HAS_LUMATEXT)
    return impl_ && impl_->DrawLayout(origin, layout, foreground, clip);
#else
    (void)origin;
    (void)layout;
    (void)foreground;
    (void)clip;
    return false;
#endif
}

bool LumaTextRenderer::DrawSplit(std::wstring_view text, IDWriteTextFormat* format,
                                 const D2D1_RECT_F& bounds, const D2D1_COLOR_F& foreground,
                                 std::uint32_t split, const D2D1_COLOR_F& split_foreground,
                                 const D2D1_COLOR_F& background) {
#if defined(PULSE_HAS_LUMATEXT)
    return impl_ && impl_->DrawSplit(text, format, bounds, foreground, split,
                                     split_foreground, background);
#else
    (void)text;
    (void)format;
    (void)bounds;
    (void)foreground;
    (void)split;
    (void)split_foreground;
    (void)background;
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

bool LumaTextRenderer::DrawEditOnSurface(HWND hwnd, IDWriteTextFormat* format,
    const D2D1_COLOR_F& foreground, const D2D1_COLOR_F& background, const D2D1_RECT_F& bounds) {
#if defined(PULSE_HAS_LUMATEXT)
    TrackEdit(hwnd);
    return impl_ && impl_->PaintEdit(hwnd, nullptr, format, foreground, background, &bounds);
#else
    (void)hwnd; (void)format; (void)foreground; (void)background; (void)bounds;
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

void LumaTextRenderer::RecordFallback(unsigned api) noexcept {
    impl_->stats.fallback_draws++;
    if (api == 0) ++impl_->stats.fallback_text;
    else if (api == 1) ++impl_->stats.fallback_layout;
    else ++impl_->stats.fallback_split;
}

} // namespace pulse::ui

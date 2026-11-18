#include "visible_text_renderer.h"
// Expose optional paint COM declarations locally. Runtime support remains QI-gated;
// the application's minimum Windows version and imported functions are unchanged.
#pragma push_macro("NTDDI_VERSION")
#undef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN10_CU
#include <d2d1_3.h>
#include <dwrite_3.h>
#pragma pop_macro("NTDDI_VERSION")
#include <wrl/client.h>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <vector>
#include <limits>
#include <new>

namespace pulse::ui {
namespace {
using Microsoft::WRL::ComPtr;
constexpr auto kFormats = static_cast<DWRITE_GLYPH_IMAGE_FORMATS>(
    DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE | DWRITE_GLYPH_IMAGE_FORMATS_CFF |
    DWRITE_GLYPH_IMAGE_FORMATS_COLR | DWRITE_GLYPH_IMAGE_FORMATS_SVG |
    DWRITE_GLYPH_IMAGE_FORMATS_PNG | DWRITE_GLYPH_IMAGE_FORMATS_JPEG |
    DWRITE_GLYPH_IMAGE_FORMATS_TIFF | DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8);
constexpr auto kUnboundedFormats = static_cast<DWRITE_GLYPH_IMAGE_FORMATS>(
    DWRITE_GLYPH_IMAGE_FORMATS_SVG | DWRITE_GLYPH_IMAGE_FORMATS_PNG |
    DWRITE_GLYPH_IMAGE_FORMATS_JPEG | DWRITE_GLYPH_IMAGE_FORMATS_TIFF |
    DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8);

class Renderer final : public IDWriteTextRenderer {
public:
    Renderer(ID2D1DeviceContext4* dc, IDWriteFactory4* factory, ID2D1Brush* brush, D2D1_RECT_F clip, VisibleTextDrawStats* stats)
        : dc_(dc), factory_(factory), brush_(brush), clip_(clip), stats_(stats) {
        dc->QueryInterface(IID_PPV_ARGS(&paint_dc_));
        factory->QueryInterface(IID_PPV_ARGS(&paint_factory_));
    }
    HRESULT first_error = S_OK;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IDWriteTextRenderer) &&
            iid != __uuidof(IDWritePixelSnapping)) return E_NOINTERFACE;
        *out = static_cast<IDWriteTextRenderer*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --refs_;
        if (!refs) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override {
        if (FAILED(first_error)) return first_error;
        if (!disabled) return Remember(E_POINTER);
        *disabled = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* out) override {
        if (FAILED(first_error)) return first_error;
        if (!out) return Remember(E_POINTER);
        D2D1_MATRIX_3X2_F m{};
        dc_->GetTransform(&m);
        *out = {m._11, m._12, m._21, m._22, m._31, m._32};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* out) override {
        if (FAILED(first_error)) return first_error;
        if (!out) return Remember(E_POINTER);
        float x = 96, y = 96;
        dc_->GetDpi(&x, &y);
        *out = x / 96.0f;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode,
        const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION* description,
        IUnknown* effect) override {
        if (FAILED(first_error)) return first_error;
        try {
            return DrawGlyphRunChecked(x, y, mode, run, description, effect);
        } catch (const std::bad_alloc&) {
            return Remember(E_OUTOFMEMORY);
        }
    }
    HRESULT DrawGlyphRunChecked(FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode,
        const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION* description,
        IUnknown* effect) {
        if (!run || !run->fontFace) return Remember(E_INVALIDARG);
        auto& font = Font(run->fontFace);
        IDWriteFontFace4* format_face = font.face4.Get();
        const HRESULT face_result = font.result;
        const bool paint_face = format_face &&
            (font.formats & DWRITE_GLYPH_IMAGE_FORMATS_COLR_PAINT_TREE) != 0;
        // Complex paint runs retain the existing native/cache path until their
        // preparation cost is bounded as tightly as the single-glyph case.
        if (paint_face && (run->glyphCount > 1 || !paint_dc_ || !paint_factory_))
            return Remember(E_NOTIMPL);
        VisibleTextRunDiagnostic* diagnostic = nullptr;
        if (stats_ && stats_->emoji_diagnostic_count < 3 && description && description->string) {
            for (UINT32 i = 0; i < description->stringLength; ++i) {
                const UINT32 c = description->string[i];
                if ((c >= 0xD800 && c <= 0xDFFF) || c == 0xFE0F) {
                    diagnostic = &stats_->emoji_diagnostics[stats_->emoji_diagnostic_count++];
                    diagnostic->utf16_first = c;
                    diagnostic->utf16_second = i + 1 < description->stringLength ? description->string[i + 1] : 0;
                    diagnostic->face_result = face_result;
                    diagnostic->formats = format_face ? static_cast<UINT32>(font.formats) : 0;
                    diagnostic->glyph_count = run->glyphCount;
                    diagnostic->baseline_y = y;
                    break;
                }
            }
        }
        if (SUCCEEDED(face_result) &&
            (static_cast<UINT32>(font.formats) &
             ~static_cast<UINT32>(kFormats | DWRITE_GLYPH_IMAGE_FORMATS_COLR_PAINT_TREE)) != 0) return Remember(E_NOTIMPL);
        if (format_face && (font.formats & kUnboundedFormats) != 0)
            return Remember(E_NOTIMPL);
        ComPtr<ID2D1Brush> effect_brush;
        if (effect && FAILED(effect->QueryInterface(IID_PPV_ARGS(&effect_brush)))) return Remember(E_NOTIMPL);
        const bool color_face = format_face &&
            (font.formats & DWRITE_GLYPH_IMAGE_FORMATS_COLR) != 0;
        if (stats_) {
            ++stats_->total_runs;
            if (color_face) ++stats_->color_runs;
        }
        if (paint_face ? PaintOutside(y, *run, diagnostic) : Outside(y, *run, diagnostic)) {
            if (diagnostic) diagnostic->culled = true;
            if (stats_) {
                ++stats_->culled_runs;
                if (color_face) ++stats_->color_culled_runs;
            }
            return S_OK;
        }
        if (stats_) ++stats_->drawn_runs;
        ID2D1Brush* brush = effect_brush ? effect_brush.Get() : brush_;
        DWRITE_MATRIX matrix{};
        GetCurrentTransform(nullptr, &matrix);
        float dx = 96, dy = 96;
        dc_->GetDpi(&dx, &dy);
        matrix.m11 *= dx / 96; matrix.m21 *= dx / 96; matrix.dx *= dx / 96;
        matrix.m12 *= dy / 96; matrix.m22 *= dy / 96; matrix.dy *= dy / 96;
        ComPtr<IDWriteColorGlyphRunEnumerator1> layers;
        HRESULT hr = paint_face ? paint_factory_->TranslateColorGlyphRun(D2D1::Point2F(x, y), run, description,
            static_cast<DWRITE_GLYPH_IMAGE_FORMATS>(kFormats | DWRITE_GLYPH_IMAGE_FORMATS_COLR_PAINT_TREE),
            paint_dc_->GetPaintFeatureLevel(), mode, &matrix, 0, &layers) :
            factory_->TranslateColorGlyphRun(D2D1::Point2F(x, y), run, description,
                kFormats, mode, &matrix, 0, &layers);
        if (diagnostic) diagnostic->translate_result = hr;
        if (hr == DWRITE_E_NOCOLOR) {
            dc_->DrawGlyphRun(D2D1::Point2F(x, y), run, brush, mode);
            return S_OK;
        }
        if (FAILED(hr)) return Remember(hr);
        if (stats_) ++stats_->color_translated_runs;
        bool had_layer = false;
        for (;;) {
            BOOL more = FALSE;
            hr = layers->MoveNext(&more);
            if (FAILED(hr) || !more) {
                if (stats_ && !had_layer && SUCCEEDED(hr)) ++stats_->color_empty_runs;
                return Remember(hr);
            }
            const DWRITE_COLOR_GLYPH_RUN1* layer = nullptr;
            hr = layers->GetCurrentRun(&layer);
            if (FAILED(hr) || !layer) return Remember(FAILED(hr) ? hr : E_FAIL);
            had_layer = true;
            if (stats_) {
                if (stats_->color_layers++ == 0) {
                    stats_->first_color_x = layer->baselineOriginX;
                    stats_->first_color_y = layer->baselineOriginY;
                    stats_->first_color_alpha = layer->runColor.a;
                    stats_->first_color_format = static_cast<UINT32>(layer->glyphImageFormat);
                }
                if (layer->paletteIndex != 0xFFFF && layer->runColor.a == 0)
                    ++stats_->color_zero_alpha_layers;
            }
            const auto at = D2D1::Point2F(layer->baselineOriginX, layer->baselineOriginY);
            const auto format = layer->glyphImageFormat;
            if (format == DWRITE_GLYPH_IMAGE_FORMATS_COLR_PAINT_TREE) {
                if (!paint_dc_) return Remember(E_NOTIMPL);
                paint_dc_->DrawPaintGlyphRun(at, &layer->glyphRun, brush, 0, layer->measuringMode);
            } else if (format == DWRITE_GLYPH_IMAGE_FORMATS_SVG) {
                dc_->DrawSvgGlyphRun(at, &layer->glyphRun, brush, nullptr, 0, layer->measuringMode);
            } else if ((format & kUnboundedFormats) != 0) {
                dc_->DrawColorBitmapGlyphRun(format, at, &layer->glyphRun, layer->measuringMode);
            } else if (format == DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE ||
                       format == DWRITE_GLYPH_IMAGE_FORMATS_CFF || format == DWRITE_GLYPH_IMAGE_FORMATS_COLR) {
                ComPtr<ID2D1SolidColorBrush> color;
                if (layer->paletteIndex != 0xFFFF) {
                    hr = dc_->CreateSolidColorBrush(D2D1::ColorF(layer->runColor.r, layer->runColor.g,
                        layer->runColor.b, layer->runColor.a), &color);
                    if (FAILED(hr)) return Remember(hr);
                }
                dc_->DrawGlyphRun(at, &layer->glyphRun, color ? color.Get() : brush, layer->measuringMode);
            } else return Remember(E_NOTIMPL);
        }
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT x, FLOAT y, const DWRITE_UNDERLINE* line,
        IUnknown* effect) override {
        if (FAILED(first_error)) return first_error;
        return line ? Decoration(x, y, line->width, line->offset, line->thickness,
            line->readingDirection, line->flowDirection, effect) : Remember(E_POINTER);
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT x, FLOAT y, const DWRITE_STRIKETHROUGH* line,
        IUnknown* effect) override {
        if (FAILED(first_error)) return first_error;
        return line ? Decoration(x, y, line->width, line->offset, line->thickness,
            line->readingDirection, line->flowDirection, effect) : Remember(E_POINTER);
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*,
        BOOL, BOOL, IUnknown*) override {
        if (FAILED(first_error)) return first_error;
        // Arbitrary inline objects may paint directly outside renderer callbacks.
        // Reject the recording so native rendering preserves their contract.
        return Remember(E_NOTIMPL);
    }
private:
    HRESULT Remember(HRESULT hr) {
        if (FAILED(hr) && SUCCEEDED(first_error)) first_error = hr;
        return hr;
    }
    struct FontInfo {
        ComPtr<IDWriteFontFace> face;
        ComPtr<IDWriteFontFace4> face4;
        HRESULT result = E_NOINTERFACE;
        DWRITE_GLYPH_IMAGE_FORMATS formats = DWRITE_GLYPH_IMAGE_FORMATS_NONE;
        DWRITE_FONT_METRICS1 metrics{};
    };
    FontInfo& Font(IDWriteFontFace* face) {
        auto found = std::find_if(fonts_.begin(), fonts_.end(), [&](const FontInfo& entry) {
            return entry.face.Get() == face;
        });
        if (found != fonts_.end()) return *found;
        FontInfo entry;
        entry.face = face;
        entry.result = face->QueryInterface(IID_PPV_ARGS(&entry.face4));
        if (entry.face4) {
            entry.formats = entry.face4->GetGlyphImageFormats();
            entry.face4->GetMetrics(&entry.metrics);
        }
        fonts_.push_back(std::move(entry));
        return fonts_.back();
    }
    struct PaintReader {
        ComPtr<IDWriteFontFace> face;
        ComPtr<IDWritePaintReader> reader;
    };
    struct PaintBox {
        ComPtr<IDWriteFontFace> face;
        UINT16 glyph = 0;
        D2D1_RECT_F box{};
        bool bounded = false;
    };
    bool PaintOutside(float baseline, const DWRITE_GLYPH_RUN& run, VisibleTextRunDiagnostic* diagnostic) {
        if (run.isSideways || !run.glyphCount || !run.glyphIndices ||
            !std::isfinite(baseline) || !std::isfinite(run.fontEmSize)) return false;
        float dx = 96, dy = 96;
        dc_->GetDpi(&dx, &dy);
        const float fringe = 2.0f * 96.0f / dy;
        float top = (std::numeric_limits<float>::infinity)();
        float bottom = -(std::numeric_limits<float>::infinity)();
        // Horizontal glyph advances do not affect the vertical union.
        for (UINT32 i = 0; i < run.glyphCount; ++i) {
            const auto glyph = run.glyphIndices[i];
            auto found = std::find_if(paint_boxes_.begin(), paint_boxes_.end(), [&](const PaintBox& entry) {
                return entry.face.Get() == run.fontFace && entry.glyph == glyph;
            });
            if (found == paint_boxes_.end()) {
                PaintBox entry;
                entry.face = run.fontFace;
                entry.glyph = glyph;
                auto reader_entry = std::find_if(paint_readers_.begin(), paint_readers_.end(), [&](const PaintReader& value) {
                    return value.face.Get() == run.fontFace;
                });
                if (reader_entry == paint_readers_.end()) {
                    PaintReader value;
                    value.face = run.fontFace;
                    ComPtr<IDWriteFontFace7> face;
                    if (SUCCEEDED(run.fontFace->QueryInterface(IID_PPV_ARGS(&face))))
                        face->CreatePaintReader(DWRITE_GLYPH_IMAGE_FORMATS_COLR_PAINT_TREE,
                            paint_dc_->GetPaintFeatureLevel(), &value.reader);
                    paint_readers_.push_back(std::move(value));
                    reader_entry = paint_readers_.end() - 1;
                }
                DWRITE_PAINT_ELEMENT element{};
                bool plain_glyph = false;
                if (reader_entry->reader &&
                    SUCCEEDED(reader_entry->reader->SetCurrentGlyph(glyph, &element, &entry.box))) {
                    // SetCurrentGlyph explicitly reports NONE for a non-color glyph;
                    // unlike an empty clipBox, this permits the original outline path.
                    plain_glyph = element.paintType == DWRITE_PAINT_TYPE_NONE;
                    if (plain_glyph && stats_) ++stats_->paint_plain_glyphs;
                    entry.bounded = !plain_glyph &&
                        entry.box.left < entry.box.right && entry.box.top < entry.box.bottom &&
                        std::isfinite(entry.box.left) && std::isfinite(entry.box.right) &&
                    std::isfinite(entry.box.top) && std::isfinite(entry.box.bottom);
                }
                // A paint-capable font can also contain ordinary outline glyphs.
                // Query this glyph across every ppem before using outline bounds.
                if (!entry.bounded) {
                    auto& font = Font(run.fontFace);
                    DWRITE_GLYPH_IMAGE_FORMATS formats = DWRITE_GLYPH_IMAGE_FORMATS_NONE;
                    constexpr UINT32 outline_formats = DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE |
                        DWRITE_GLYPH_IMAGE_FORMATS_CFF | DWRITE_GLYPH_IMAGE_FORMATS_COLR;
                    const bool outline_format = font.face4 &&
                        SUCCEEDED(font.face4->GetGlyphImageFormats(glyph, 0,
                            (std::numeric_limits<UINT32>::max)(), &formats)) &&
                        formats != DWRITE_GLYPH_IMAGE_FORMATS_NONE &&
                        (static_cast<UINT32>(formats) & ~outline_formats) == 0;
                    if ((plain_glyph || outline_format) &&
                        font.metrics.designUnitsPerEm && font.metrics.glyphBoxTop > font.metrics.glyphBoxBottom) {
                        const float em = static_cast<float>(font.metrics.designUnitsPerEm);
                        // Font metrics use upward-positive Y; paint clip boxes use downward-positive Y.
                        entry.box = D2D1::RectF(font.metrics.glyphBoxLeft / em, -font.metrics.glyphBoxTop / em,
                            font.metrics.glyphBoxRight / em, -font.metrics.glyphBoxBottom / em);
                        entry.bounded = true;
                    }
                }
                if (!entry.bounded && stats_) ++stats_->paint_unbounded_glyphs;
                paint_boxes_.push_back(std::move(entry));
                found = paint_boxes_.end() - 1;
            }
            if (!found->bounded) return false;
            const float offset = run.glyphOffsets ? run.glyphOffsets[i].ascenderOffset : 0.0f;
            if (!std::isfinite(offset)) return false;
            const float glyph_top = baseline + found->box.top * run.fontEmSize - offset - fringe;
            const float glyph_bottom = baseline + found->box.bottom * run.fontEmSize - offset + fringe;
            if (!std::isfinite(glyph_top) || !std::isfinite(glyph_bottom)) return false;
            top = (std::min)(top, glyph_top);
            bottom = (std::max)(bottom, glyph_bottom);
        }
        if (diagnostic) { diagnostic->bound_top = top; diagnostic->bound_bottom = bottom; }
        return bottom < clip_.top || top > clip_.bottom;
    }
    bool Outside(float baseline, const DWRITE_GLYPH_RUN& run, VisibleTextRunDiagnostic* diagnostic) {
        if (run.isSideways || !std::isfinite(baseline) || !std::isfinite(run.fontEmSize)) return false;
        auto& font = Font(run.fontFace);
        if (!font.face4 || (font.formats & kUnboundedFormats) != 0) return false;
        const auto& metrics = font.metrics;
        if (!metrics.designUnitsPerEm || metrics.glyphBoxTop <= metrics.glyphBoxBottom) return false;
        float min_offset = 0, max_offset = 0;
        if (run.glyphOffsets) for (UINT32 i = 0; i < run.glyphCount; ++i) {
            const float offset = run.glyphOffsets[i].ascenderOffset;
            if (!std::isfinite(offset)) return false;
            min_offset = (std::min)(min_offset, offset);
            max_offset = (std::max)(max_offset, offset);
        }
        const float scale = run.fontEmSize / metrics.designUnitsPerEm;
        // Extra physical-pixel fringe protects antialiasing and baseline snapping.
        float dx = 96, dy = 96;
        dc_->GetDpi(&dx, &dy);
        const float fringe = 2.0f * 96.0f / dy;
        const float top = baseline - metrics.glyphBoxTop * scale - max_offset - fringe;
        const float bottom = baseline - metrics.glyphBoxBottom * scale - min_offset + fringe;
        if (diagnostic) {
            diagnostic->bound_top = top;
            diagnostic->bound_bottom = bottom;
        }
        return bottom < clip_.top || top > clip_.bottom;
    }
    HRESULT Decoration(float x, float y, float width, float offset, float thickness,
        DWRITE_READING_DIRECTION reading, DWRITE_FLOW_DIRECTION flow, IUnknown* effect) {
        if (flow != DWRITE_FLOW_DIRECTION_TOP_TO_BOTTOM ||
            (reading != DWRITE_READING_DIRECTION_LEFT_TO_RIGHT && reading != DWRITE_READING_DIRECTION_RIGHT_TO_LEFT))
            return Remember(E_NOTIMPL);
        ComPtr<ID2D1Brush> brush;
        if (effect && FAILED(effect->QueryInterface(IID_PPV_ARGS(&brush)))) return Remember(E_NOTIMPL);
        if (reading == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT) x -= width;
        dc_->FillRectangle(D2D1::RectF(x, y + offset, x + width, y + offset + thickness), brush ? brush.Get() : brush_);
        return S_OK;
    }
    std::atomic<ULONG> refs_{1};
    ComPtr<ID2D1DeviceContext7> paint_dc_;
    ComPtr<IDWriteFactory8> paint_factory_;
    std::vector<FontInfo> fonts_;
    std::vector<PaintBox> paint_boxes_;
    std::vector<PaintReader> paint_readers_;
    ID2D1DeviceContext4* dc_;
    IDWriteFactory4* factory_;
    ID2D1Brush* brush_;
    D2D1_RECT_F clip_;
    VisibleTextDrawStats* stats_;
};
}

bool DrawVisibleTextLayout(ID2D1DeviceContext* dc, IDWriteTextLayout* layout,
    ID2D1Brush* brush, D2D1_POINT_2F origin, D2D1_RECT_F clip, VisibleTextDrawStats* stats) {
    if (stats) *stats = {};
    if (!dc || !layout || !brush) return false;
    D2D1_MATRIX_3X2_F transform{};
    dc->GetTransform(&transform);
    // The conservative vertical bounds below assume untransformed drawing.
    if (transform._11 != 1 || transform._12 != 0 || transform._21 != 0 ||
        transform._22 != 1 || transform._31 != 0 || transform._32 != 0) return false;
    ComPtr<ID2D1DeviceContext4> modern;
    ComPtr<IDWriteFactory4> factory;
    if (FAILED(dc->QueryInterface(IID_PPV_ARGS(&modern))) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory4),
            reinterpret_cast<IUnknown**>(factory.GetAddressOf())))) return false;
    ComPtr<ID2D1Device> device;
    dc->GetDevice(&device);
    ComPtr<ID2D1DeviceContext> recording_context;
    ComPtr<ID2D1DeviceContext4> recorder;
    ComPtr<ID2D1CommandList> commands;
    if (!device || FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &recording_context)) ||
        FAILED(recording_context.As(&recorder)) || FAILED(recorder->CreateCommandList(&commands))) return false;
    float dpi_x = 96, dpi_y = 96;
    dc->GetDpi(&dpi_x, &dpi_y);
    recorder->SetDpi(dpi_x, dpi_y);
    recorder->SetTextAntialiasMode(dc->GetTextAntialiasMode());
    ComPtr<IDWriteRenderingParams> params;
    dc->GetTextRenderingParams(&params);
    recorder->SetTextRenderingParams(params.Get());
    recorder->SetTarget(commands.Get());
    ComPtr<Renderer> renderer;
    renderer.Attach(new (std::nothrow) Renderer(recorder.Get(), factory.Get(), brush, clip, stats));
    if (!renderer) {
        recorder->SetTarget(nullptr);
        return false;
    }
    // Only publish a complete recording. A failed callback cannot leave partially
    // drawn text on the destination before its native fallback.
    recorder->BeginDraw();
    // Match DrawTextLayout's CLIP option independently of the caller's viewport.
    recorder->PushAxisAlignedClip(D2D1::RectF(origin.x, origin.y,
        origin.x + layout->GetMaxWidth(), origin.y + layout->GetMaxHeight()),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const HRESULT draw_result = layout->Draw(nullptr, renderer.Get(), origin.x, origin.y);
    recorder->PopAxisAlignedClip();
    const HRESULT end_result = recorder->EndDraw();
    recorder->SetTarget(nullptr);
    if (FAILED(renderer->first_error) || FAILED(draw_result) || FAILED(end_result) || FAILED(commands->Close())) return false;
    dc->DrawImage(commands.Get(), D2D1::Point2F(0.0f, 0.0f));
    return true;
}
} // namespace pulse::ui

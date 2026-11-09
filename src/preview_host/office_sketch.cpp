// office_sketch.cpp - see office_sketch.h.
#include "office_sketch.h"
#include "office_model.h"
#include "office_doc_model.h"
#include "office_sheet_model.h"
#include "office_slide_model.h"
#include "zip_entry.h"
#include <shlwapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace pulse::preview {
namespace {

using Microsoft::WRL::ComPtr;

using D2D1CreateFactoryFn = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID,
                                              const D2D1_FACTORY_OPTIONS*, void**);
using DWriteCreateFactoryFn = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);

// Resolved on first use so the host does not import d2d1/dwrite at start-up.
template <typename Fn>
Fn Resolve(const wchar_t* dll, const char* name) {
    const HMODULE module = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module ? reinterpret_cast<Fn>(GetProcAddress(module, name)) : nullptr;
}

IDWriteFactory* DWrite() {
    static ComPtr<IDWriteFactory> factory = [] {
        ComPtr<IDWriteFactory> f;
        static const auto create = Resolve<DWriteCreateFactoryFn>(L"dwrite.dll", "DWriteCreateFactory");
        if (create) create(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                           reinterpret_cast<IUnknown**>(f.GetAddressOf()));
        return f;
    }();
    return factory.Get();
}

ID2D1Factory* D2D() {
    static ComPtr<ID2D1Factory> factory = [] {
        ComPtr<ID2D1Factory> f;
        static const auto create = Resolve<D2D1CreateFactoryFn>(L"d2d1.dll", "D2D1CreateFactory");
        D2D1_FACTORY_OPTIONS options{};
        if (create) create(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &options,
                           reinterpret_cast<void**>(f.GetAddressOf()));
        return f;
    }();
    return factory.Get();
}

void SetError(std::wstring* error, const wchar_t* text) {
    if (error) *error = text;
}

constexpr wchar_t kPageFont[] = L"SimSun";
constexpr wchar_t kUiFont[] = L"Microsoft YaHei UI";
constexpr float kReadablePx = 4.0f;   // smaller text becomes grey bars

// A WIC bitmap with a software Direct2D target, read back as PBGRA.
struct Canvas {
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> rt;
    UINT width = 0, height = 0;

    bool Create(UINT w, UINT h) {
        width = (std::max)(1u, w);
        height = (std::max)(1u, h);
        ComPtr<IWICImagingFactory> wic;
        if (!D2D() || !DWrite() ||
            FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
            FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)))
            return false;
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (FAILED(D2D()->CreateWicBitmapRenderTarget(bitmap.Get(), props, &rt))) return false;
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(D2D1::ColorF::White));
        return true;
    }

    ComPtr<ID2D1SolidColorBrush> Brush(UINT32 rgb, float alpha = 1.0f) {
        ComPtr<ID2D1SolidColorBrush> brush;
        rt->CreateSolidColorBrush(D2D1::ColorF(rgb, alpha), &brush);
        return brush;
    }

    bool Finish(std::vector<unsigned char>& pixels, UINT& w, UINT& h, UINT& stride) {
        if (FAILED(rt->EndDraw())) return false;
        rt.Reset();
        stride = width * 4;
        pixels.resize(static_cast<size_t>(stride) * height);
        if (FAILED(bitmap->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data())))
            return false;
        w = width;
        h = height;
        return true;
    }
};

ComPtr<IDWriteTextFormat> Format(const wchar_t* family, float px, bool bold,
                                 DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(DWrite()->CreateTextFormat(family, nullptr,
            bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, (std::max)(px, 1.0f), L"zh-cn", &format)))
        return nullptr;
    format->SetTextAlignment(align);
    return format;
}

// One line, clipped with an ellipsis.
ComPtr<IDWriteTextFormat> LineFormat(const wchar_t* family, float px, bool bold, UINT32 lines = 1) {
    ComPtr<IDWriteTextFormat> format = Format(family, px, bold);
    if (!format) return nullptr;
    if (lines == 1) format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    ComPtr<IDWriteInlineObject> ellipsis;
    DWrite()->CreateEllipsisTrimmingSign(format.Get(), &ellipsis);
    format->SetTrimming(&trimming, ellipsis.Get());
    return format;
}

float TextHeight(IDWriteTextLayout* layout) {
    DWRITE_TEXT_METRICS metrics{};
    return SUCCEEDED(layout->GetMetrics(&metrics)) ? metrics.height : 0.0f;
}

// Width in "em" units: CJK and other wide characters count 1, the rest 0.5.
float EmWidth(const std::wstring& text) {
    float em = 0;
    for (wchar_t c : text) em += (c >= 0x2E80 || (c >= 0x1100 && c <= 0x115F)) ? 1.0f : 0.5f;
    return em;
}

std::wstring Collapse(const std::wstring& text) {
    std::wstring out;
    bool space = false;
    for (wchar_t c : text) {
        if (std::iswspace(c) || c == 0x3000) { space = !out.empty(); continue; }
        if (space) out += L' ';
        space = false;
        out += c;
    }
    return out;
}

void Frame(Canvas& canvas) {
    canvas.rt->DrawRectangle(D2D1::RectF(0.5f, 0.5f, canvas.width - 0.5f, canvas.height - 0.5f),
                             canvas.Brush(0xC9CED6).Get(), 1.0f);
}

// ---- Word: page layout sketch -------------------------------------------------------

class PageFlow {
public:
    PageFlow(Canvas& canvas, const DocModel& doc, float scale)
        : canvas_(canvas), doc_(doc), s_(scale) {
        col_width_ = (doc.page_width - doc.margin_left - doc.margin_right -
                      (doc.columns - 1) * doc.column_gap) / doc.columns;
        y_ = doc.margin_top;
        bottom_ = doc.page_height - doc.margin_top;   // the model keeps one vertical margin
        ink_ = canvas.Brush(0x111111);
        bar_ = canvas.Brush(0xB9BEC7);
        rule_ = canvas.Brush(0x8A8F98);
    }

    // False once the last column is full.
    bool Paragraph(const DocBlock& block) {
        const float pt = block.points, line = pt * 1.3f;
        if (block.text.empty()) return Advance(line);
        const float px = pt * s_;
        if (px >= kReadablePx) {
            DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING;
            if (block.align == DocAlign::Center) align = DWRITE_TEXT_ALIGNMENT_CENTER;
            else if (block.align == DocAlign::Right) align = DWRITE_TEXT_ALIGNMENT_TRAILING;
            else if (block.align == DocAlign::Justify) align = DWRITE_TEXT_ALIGNMENT_JUSTIFIED;
            auto format = Format(kPageFont, px, block.bold, align);
            if (!format) return true;
            format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line * s_, line * s_ * 0.8f);
            ComPtr<IDWriteTextLayout> layout;
            if (FAILED(DWrite()->CreateTextLayout(block.text.c_str(), static_cast<UINT32>(block.text.size()),
                    format.Get(), col_width_ * s_, 10000.0f, &layout)))
                return true;
            const float height = TextHeight(layout.Get()) / s_;
            if (!Fit(height)) return false;
            canvas_.rt->DrawTextLayout(D2D1::Point2F(X() * s_, y_ * s_), layout.Get(), ink_.Get(),
                                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
            y_ += height;
            return true;
        }
        // Grey bars: one per estimated line, the last one shorter.
        const float per_line = (std::max)(1.0f, std::floor(col_width_ / pt));
        const float em = EmWidth(block.text);
        const int lines = (std::max)(1, static_cast<int>(std::ceil(em / per_line)));
        for (int i = 0; i < lines; ++i) {
            if (!Fit(line)) return false;
            const float fraction = i + 1 < lines ? 1.0f
                : (std::max)(0.15f, (em - per_line * (lines - 1)) / per_line);
            const float w = col_width_ * (std::min)(1.0f, fraction);
            float x = X();
            if (block.align == DocAlign::Center) x += (col_width_ - w) / 2;
            else if (block.align == DocAlign::Right) x += col_width_ - w;
            Bar(x, y_ + pt * 0.38f, w, pt * 0.55f);
            y_ += line;
        }
        return true;
    }

    bool Table(const DocBlock& block) {
        size_t columns = 0;
        for (const auto& row : block.rows) columns = (std::max)(columns, row.size());
        if (!columns) return true;
        const float cell_pt = 10.5f, row_h = cell_pt * 1.3f + 2.0f;
        const float cell_w = col_width_ / static_cast<float>(columns);
        const float stroke = (std::max)(1.0f, 0.75f * s_);
        for (const auto& row : block.rows) {
            if (!Fit(row_h)) return false;
            for (size_t c = 0; c < columns; ++c) {
                const float x = X() + cell_w * c;
                canvas_.rt->DrawRectangle(D2D1::RectF(x * s_, y_ * s_, (x + cell_w) * s_, (y_ + row_h) * s_),
                                          rule_.Get(), stroke * 0.6f);
                if (c >= row.size() || row[c].empty()) continue;
                const float inner = cell_w - 6.0f;
                if (cell_pt * s_ >= kReadablePx) {
                    if (auto format = LineFormat(kPageFont, cell_pt * s_, false)) {
                        canvas_.rt->DrawText(row[c].c_str(), static_cast<UINT32>(row[c].size()), format.Get(),
                            D2D1::RectF((x + 3) * s_, (y_ + 1) * s_, (x + 3 + inner) * s_, (y_ + row_h) * s_),
                            ink_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                } else {
                    const float w = (std::min)(inner, EmWidth(row[c]) * cell_pt);
                    Bar(x + 3, y_ + (row_h - 5.5f) / 2, w, 5.5f);
                }
            }
            y_ += row_h;
        }
        return Advance(2.0f);
    }

private:
    float X() const { return doc_.margin_left + column_ * (col_width_ + doc_.column_gap); }

    // Moves to the next column when `height` does not fit below.
    bool Fit(float height) {
        if (y_ + height <= bottom_ || y_ == doc_.margin_top) return y_ < bottom_;
        if (column_ + 1 >= doc_.columns) return false;
        ++column_;
        y_ = doc_.margin_top;
        return true;
    }

    bool Advance(float height) {
        if (!Fit(height)) return false;
        y_ += height;
        return true;
    }

    void Bar(float x, float y, float w, float h) {
        const float hp = (std::max)(1.0f, h * s_);
        canvas_.rt->FillRoundedRectangle(
            D2D1_ROUNDED_RECT{D2D1::RectF(x * s_, y * s_, (x + w) * s_, y * s_ + hp), hp / 3, hp / 3},
            bar_.Get());
    }

    Canvas& canvas_;
    const DocModel& doc_;
    float s_;                 // pixels per point
    float col_width_ = 0, y_ = 0, bottom_ = 0;
    int column_ = 0;
    ComPtr<ID2D1SolidColorBrush> ink_, bar_, rule_;
};

bool DrawWordPage(const DocModel& doc, UINT edge, Canvas& canvas) {
    const float scale = edge / (std::max)(doc.page_width, doc.page_height);
    if (!canvas.Create(static_cast<UINT>(std::lround(doc.page_width * scale)),
                       static_cast<UINT>(std::lround(doc.page_height * scale))))
        return false;
    canvas.rt->PushAxisAlignedClip(D2D1::RectF(0, 0, static_cast<float>(canvas.width),
                                               static_cast<float>(canvas.height)),
                                   D2D1_ANTIALIAS_MODE_ALIASED);
    PageFlow flow(canvas, doc, scale);
    for (const DocBlock& block : doc.blocks) {
        if (!(block.table ? flow.Table(block) : flow.Paragraph(block))) break;
    }
    canvas.rt->PopAxisAlignedClip();
    Frame(canvas);
    return true;
}

// ---- Word: summary card -------------------------------------------------------------

bool DrawWordCard(const DocModel& doc, UINT edge, Canvas& canvas) {
    const float f = edge / 256.0f;
    if (!canvas.Create(static_cast<UINT>(std::lround(edge * 0.75f)), edge)) return false;
    const float pad = edge * 0.06f;
    const float w = canvas.width - 2 * pad, bottom = canvas.height - pad;
    auto ink = canvas.Brush(0x111111), body = canvas.Brush(0x333333), soft = canvas.Brush(0x777777);

    // Title: the largest of the first ten non-empty paragraphs.
    const DocBlock* title = nullptr;
    int seen = 0;
    for (const DocBlock& block : doc.blocks) {
        if (block.table || block.text.empty()) continue;
        if (!title || block.points > title->points) title = &block;
        if (++seen >= 10) break;
    }
    float y = pad;
    if (title) {
        const std::wstring text = Collapse(title->text);
        if (auto format = LineFormat(kUiFont, 15.0f * f, true, 2)) {
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(DWrite()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                    format.Get(), w, 15.0f * f * 1.25f * 2.05f, &layout))) {
                canvas.rt->DrawTextLayout(D2D1::Point2F(pad, y), layout.Get(), ink.Get(),
                                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
                y += (std::min)(TextHeight(layout.Get()), 15.0f * f * 1.35f * 2) + 6.0f * f;
            }
        }
    }
    auto line_format = LineFormat(kUiFont, 10.0f * f, false);
    auto key_format = LineFormat(kUiFont, 9.5f * f, false);
    const float line_h = 10.0f * f * 1.45f, kv_h = 9.5f * f * 1.45f;
    int used = 0;
    for (const DocBlock& block : doc.blocks) {
        if (used >= 12 || y + line_h > bottom) break;
        if (!block.table) {
            if (block.text.empty() || &block == title) continue;
            const std::wstring text = Collapse(block.text);
            if (line_format)
                canvas.rt->DrawText(text.c_str(), static_cast<UINT32>(text.size()), line_format.Get(),
                                    D2D1::RectF(pad, y, pad + w, y + line_h), body.Get(),
                                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
            y += line_h;
            ++used;
            continue;
        }
        for (const auto& row : block.rows) {
            if (used >= 12 || y + kv_h > bottom) break;
            std::wstring key = row.empty() ? std::wstring() : Collapse(row[0]), rest;
            for (size_t i = 1; i < row.size(); ++i) {
                if (row[i].empty()) continue;
                if (!rest.empty()) rest += L' ';
                rest += Collapse(row[i]);
            }
            if (key_format) {
                canvas.rt->DrawText(key.c_str(), static_cast<UINT32>(key.size()), key_format.Get(),
                    D2D1::RectF(pad, y, pad + w * 0.5f - 3 * f, y + kv_h), soft.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                canvas.rt->DrawText(rest.c_str(), static_cast<UINT32>(rest.size()), key_format.Get(),
                    D2D1::RectF(pad + w * 0.5f + 3 * f, y, pad + w, y + kv_h), body.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            y += kv_h;
            ++used;
        }
    }
    Frame(canvas);
    return true;
}

// ---- Excel: sheet sketch --------------------------------------------------------------

std::wstring ColumnName(int column) {
    std::wstring name;
    for (int n = column; n > 0; n = (n - 1) / 26)
        name.insert(name.begin(), static_cast<wchar_t>(L'A' + (n - 1) % 26));
    return name;
}

bool DrawSheet(const std::wstring& path, UINT edge, Canvas& canvas, std::wstring* error) {
    const float f = edge / 256.0f;
    const float cw = std::round(46 * f), rh = std::round(15 * f), hw = std::round(18 * f);
    const UINT height = static_cast<UINT>(std::lround(edge * 0.78f));
    const int cols = (std::max)(1, static_cast<int>((edge - hw) / cw));
    const int rows = (std::max)(1, static_cast<int>((height - rh) / rh));
    SheetModel sheet;
    if (!ReadSheetModel(path, rows, cols, sheet, error)) return false;   // .xlsx or BIFF, by content
    if (!canvas.Create(edge, height)) return false;
    auto grid = canvas.Brush(0xDFE2E6), head_fill = canvas.Brush(0xF1F3F5);
    auto head_ink = canvas.Brush(0x8A9099), ink = canvas.Brush(0x222222);
    const float W = static_cast<float>(canvas.width), H = static_cast<float>(canvas.height);
    canvas.rt->FillRectangle(D2D1::RectF(0, 0, W, rh), head_fill.Get());
    canvas.rt->FillRectangle(D2D1::RectF(0, 0, hw, H), head_fill.Get());
    auto head_format = Format(kUiFont, 8.5f * f, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    auto cell_format = LineFormat(kUiFont, 8.5f * f, false);
    auto bold_format = LineFormat(kUiFont, 8.5f * f, true);
    if (head_format) head_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (cell_format) cell_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (bold_format) bold_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    for (int c = 0; c < cols; ++c) {
        const float x = hw + cw * c;
        const std::wstring name = ColumnName(sheet.first_col + c);
        if (head_format)
            canvas.rt->DrawText(name.c_str(), static_cast<UINT32>(name.size()), head_format.Get(),
                                D2D1::RectF(x, 0, x + cw, rh), head_ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    for (int r = 0; r < rows; ++r) {
        const float y = rh + rh * r;
        const std::wstring number = std::to_wstring(sheet.first_row + r);
        if (head_format)
            canvas.rt->DrawText(number.c_str(), static_cast<UINT32>(number.size()), head_format.Get(),
                                D2D1::RectF(0, y, hw, y + rh), head_ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (static_cast<size_t>(r) >= sheet.grid.size()) continue;
        IDWriteTextFormat* format = r == 0 ? bold_format.Get() : cell_format.Get();
        for (int c = 0; c < cols && static_cast<size_t>(c) < sheet.grid[r].size(); ++c) {
            const std::wstring& text = sheet.grid[r][c];
            if (text.empty() || !format) continue;
            const float x = hw + cw * c;
            // Cell text runs into empty neighbours like Excel does.
            float right = x + cw;
            for (int n = c + 1; n < cols && (static_cast<size_t>(n) >= sheet.grid[r].size() ||
                                             sheet.grid[r][n].empty()); ++n) right += cw;
            canvas.rt->PushAxisAlignedClip(D2D1::RectF(x + 1, y, right - 1, y + rh), D2D1_ANTIALIAS_MODE_ALIASED);
            canvas.rt->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format,
                                D2D1::RectF(x + 2 * f, y, right - 2 * f, y + rh), ink.Get(),
                                D2D1_DRAW_TEXT_OPTIONS_CLIP);
            canvas.rt->PopAxisAlignedClip();
        }
    }
    for (int c = 0; c <= cols; ++c) {
        const float x = std::floor(hw + cw * c) + 0.5f;
        canvas.rt->DrawLine(D2D1::Point2F(x, 0), D2D1::Point2F(x, H), grid.Get(), 1.0f);
    }
    for (int r = 0; r <= rows; ++r) {
        const float y = std::floor(rh + rh * r) + 0.5f;
        canvas.rt->DrawLine(D2D1::Point2F(0, y), D2D1::Point2F(W, y), grid.Get(), 1.0f);
    }
    Frame(canvas);
    return true;
}

// ---- PowerPoint: first slide sketch ----------------------------------------------------

// Where a placeholder sits when the slide takes its place from the layout
// (the layouts are not read): PowerPoint's default Title and Content slide.
SlideRect DefaultRect(SlideTextKind kind) {
    switch (kind) {
    case SlideTextKind::Title: return {0.07, 0.05, 0.86, 0.17};
    case SlideTextKind::CenterTitle: return {0.12, 0.26, 0.76, 0.24};
    case SlideTextKind::Subtitle: return {0.16, 0.53, 0.68, 0.18};
    case SlideTextKind::Body: return {0.07, 0.25, 0.86, 0.66};
    default: return {0.07, 0.25, 0.86, 0.66};
    }
}

float DefaultPoints(SlideTextKind kind) {
    switch (kind) {
    case SlideTextKind::Title:
    case SlideTextKind::CenterTitle: return 44.0f;
    case SlideTextKind::Subtitle: return 24.0f;
    case SlideTextKind::Body: return 26.0f;
    default: return 18.0f;
    }
}

void DrawSlideText(Canvas& canvas, const SlideText& text, float scale) {
    const SlideRect rect = text.rect.x >= 0 && text.rect.w > 0 && text.rect.h > 0 ? text.rect : DefaultRect(text.kind);
    const float W = static_cast<float>(canvas.width), H = static_cast<float>(canvas.height);
    const D2D1_RECT_F box = D2D1::RectF(static_cast<float>(rect.x) * W, static_cast<float>(rect.y) * H,
                                        static_cast<float>(rect.x + rect.w) * W, static_cast<float>(rect.y + rect.h) * H);
    const float box_w = box.right - box.left, box_h = box.bottom - box.top;
    if (box_w < 2 || box_h < 2) return;
    const bool title = text.kind == SlideTextKind::Title || text.kind == SlideTextKind::CenterTitle;
    const bool centred = text.kind == SlideTextKind::CenterTitle || text.kind == SlideTextKind::Subtitle;
    const bool bullets = text.kind == SlideTextKind::Body;
    float px = (text.points > 0 ? static_cast<float>(text.points) : DefaultPoints(text.kind)) * scale;
    auto ink = canvas.Brush(title ? 0x1F1F1F : 0x404040);

    std::wstring all;
    for (const std::wstring& paragraph : text.paragraphs) {
        if (!all.empty()) all += L'\n';
        const std::wstring line = Collapse(paragraph);
        if (bullets && !line.empty()) all += L"\u2022 ";
        all += line;
    }
    if (px >= kReadablePx) {
        // Like PowerPoint's shrink-on-overflow: one step down, never below 60%.
        for (int attempt = 0; attempt < 2; ++attempt) {
            auto format = Format(kUiFont, px, title,
                                 centred ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
            if (!format) return;
            if (title || centred) format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ComPtr<IDWriteTextLayout> layout;
            if (FAILED(DWrite()->CreateTextLayout(all.c_str(), static_cast<UINT32>(all.size()), format.Get(),
                                                  box_w, box_h, &layout)))
                return;
            const float height = TextHeight(layout.Get());
            if (attempt == 0 && height > box_h * 1.05f) {
                const float shrunk = px * (std::max)(0.6f, std::sqrt(box_h / height));
                if (shrunk >= kReadablePx) {
                    px = shrunk;
                    continue;
                }
            }
            canvas.rt->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_ALIASED);
            canvas.rt->DrawTextLayout(D2D1::Point2F(box.left, box.top), layout.Get(), ink.Get(),
                                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
            canvas.rt->PopAxisAlignedClip();
            return;
        }
        return;
    }
    // Too small to read: one grey bar per paragraph, as long as its text.
    auto bar = canvas.Brush(title ? 0x8A8F98 : 0xB9BEC7);
    const float line_h = (std::max)(3.0f, px * 1.5f), thick = (std::max)(1.5f, px * 0.6f);
    float y = box.top + (title || centred ? (box_h - line_h * static_cast<float>(text.paragraphs.size())) / 2 : 0.0f);
    y = (std::max)(y, box.top);
    for (const std::wstring& paragraph : text.paragraphs) {
        if (y + line_h > box.bottom + 0.5f) break;
        const float w = (std::min)(box_w, (std::max)(box_w * 0.15f, EmWidth(paragraph) * px));
        const float x = centred ? box.left + (box_w - w) / 2 : box.left;
        if (!paragraph.empty())
            canvas.rt->FillRoundedRectangle(
                D2D1_ROUNDED_RECT{D2D1::RectF(x, y + (line_h - thick) / 2, x + w, y + (line_h + thick) / 2),
                                  thick / 3, thick / 3},
                bar.Get());
        y += line_h;
    }
}

bool DrawSlide(const SlideModel& model, UINT edge, Canvas& canvas) {
    const double aspect = (std::clamp)(model.width_pt / (std::max)(1.0, model.height_pt), 0.5, 3.0);
    const UINT w = aspect >= 1.0 ? edge : static_cast<UINT>(std::lround(edge * aspect));
    const UINT h = aspect >= 1.0 ? static_cast<UINT>(std::lround(edge / aspect)) : edge;
    if (!canvas.Create(w, h)) return false;
    const float W = static_cast<float>(canvas.width), H = static_cast<float>(canvas.height);
    const float scale = W / static_cast<float>((std::max)(1.0, model.width_pt));
    auto fill = canvas.Brush(0xE9ECF0), edge_line = canvas.Brush(0xD5DAE1);
    for (const SlideRect& picture : model.pictures) {
        const D2D1_RECT_F box = D2D1::RectF(static_cast<float>(picture.x) * W, static_cast<float>(picture.y) * H,
            static_cast<float>(picture.x + picture.w) * W, static_cast<float>(picture.y + picture.h) * H);
        canvas.rt->FillRectangle(box, fill.Get());
        canvas.rt->DrawRectangle(box, edge_line.Get(), 1.0f);
    }
    for (const SlideText& text : model.texts) DrawSlideText(canvas, text, scale);
    Frame(canvas);
    return true;
}

// ---- OpenDocument: the thumbnail every ODF writer stores --------------------------------

bool DrawOdfThumbnail(const std::wstring& path, UINT edge, Canvas& canvas,
                      std::wstring* error) {
    std::vector<unsigned char> png;
    if (!ReadZipEntry(path, "Thumbnails/thumbnail.png", 8u << 20, png, error)) return false;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(png.data(), static_cast<UINT>(png.size())));
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT tw = 0, th = 0;
    if (!stream ||
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                     0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&tw, &th)) || !tw || !th) {
        SetError(error, L"odf-thumbnail-decode-failed");
        return false;
    }
    const float fit = static_cast<float>(edge) / static_cast<float>((std::max)(tw, th));
    if (!canvas.Create(static_cast<UINT>(std::lround(tw * fit)), static_cast<UINT>(std::lround(th * fit))))
        return false;
    ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(canvas.rt->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap))) {
        SetError(error, L"odf-thumbnail-decode-failed");
        return false;
    }
    canvas.rt->DrawBitmap(bitmap.Get(), D2D1::RectF(0, 0, static_cast<float>(canvas.width),
                                                     static_cast<float>(canvas.height)),
                          1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    Frame(canvas);
    return true;
}

bool IsWordLike(std::wstring_view e) {
    return e == L".docx" || e == L".doc" || e == L".rtf" || e == L".docm" || e == L".wps";
}
bool IsSheetLike(std::wstring_view e) {
    return e == L".xlsx" || e == L".xlsm" || e == L".xls" || e == L".et";
}
bool IsSlideLike(std::wstring_view e) {
    return e == L".pptx" || e == L".pptm" || e == L".ppsx" || e == L".ppt" || e == L".pps" || e == L".dps";
}
bool IsOpenDocument(std::wstring_view e) {
    return e == L".odt" || e == L".ods" || e == L".odp" || e == L".odg";
}

} // namespace

bool IsBlankThumbnail(const std::vector<unsigned char>& pixels, UINT width, UINT height, UINT stride) {
    if (!width || !height || stride < width * 4 || pixels.size() < static_cast<size_t>(stride) * height)
        return false;
    for (UINT y = 0; y < height; ++y) {
        const unsigned char* row = pixels.data() + static_cast<size_t>(stride) * y;
        for (UINT x = 0; x < width; ++x) {
            const unsigned char* p = row + x * 4;
            if (p[3] < 16) continue;                                      // transparent
            if (p[0] < 248 || p[1] < 248 || p[2] < 248) return false;     // any ink
        }
    }
    return true;
}

bool IsOfficeSketchExtension(std::wstring_view extension) {
    return IsWordLike(extension) || IsSheetLike(extension) || IsSlideLike(extension) || IsOpenDocument(extension);
}

bool IsSketchOnlyExtension(std::wstring_view extension) {
    return extension == L".wps" || extension == L".et" || extension == L".dps" || IsOpenDocument(extension);
}

bool RenderOfficeSketch(const std::wstring& path, std::wstring_view extension, UINT max_edge,
                        std::vector<unsigned char>& pixels, UINT& width, UINT& height,
                        UINT& stride, std::wstring* error) {
    pixels.clear();
    width = height = stride = 0;
    if (max_edge < kOfficeSketchMinEdge) {
        SetError(error, L"sketch-too-small");
        return false;
    }
    const UINT edge = (std::min)(max_edge, 1024u);
    Canvas canvas;
    bool drawn = false;
    if (IsSheetLike(extension)) {
        drawn = DrawSheet(path, edge, canvas, error);
    } else if (IsSlideLike(extension)) {
        // By content: WPS saves .dps in the PowerPoint 97 format, and .ppt
        // files are sometimes .pptx under the old name.
        SlideModel slides;
        if (!ReadSlideModel(path, slides, error)) return false;
        drawn = DrawSlide(slides, edge, canvas);
    } else if (IsOpenDocument(extension)) {
        drawn = DrawOdfThumbnail(path, edge, canvas, error);
    } else if (IsWordLike(extension)) {
        // By content: .doc files are often .docx or RTF under the old name.
        DocModel doc;
        if (!ReadWordModel(path, doc, error)) return false;
        drawn = edge >= kOfficeSketchPageEdge ? DrawWordPage(doc, edge, canvas)
                                              : DrawWordCard(doc, edge, canvas);
    } else {
        SetError(error, L"sketch-unsupported");
        return false;
    }
    if (!drawn || !canvas.rt) {
        if (error && error->empty()) *error = L"sketch-draw-failed";
        return false;
    }
    if (!canvas.Finish(pixels, width, height, stride)) {
        pixels.clear();
        width = height = stride = 0;
        SetError(error, L"sketch-draw-failed");
        return false;
    }
    return true;
}

} // namespace pulse::preview
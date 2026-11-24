#include "git_image_compare.h"

#include "../app/git_terms.h"
#include "../common/localization.h"
#include <objbase.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace pulse::ui {
namespace {

std::wstring FormatImageBytes(uint64_t bytes) {
    wchar_t buf[64]{};
    if (bytes >= 1024ull * 1024ull) {
        swprintf_s(buf, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        swprintf_s(buf, L"%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        swprintf_s(buf, L"%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

}  // namespace

DecodedGitImage DecodeGitImageBytes(std::string_view bytes, uint32_t max_edge) {
    DecodedGitImage out;
    if (bytes.empty() || bytes.size() > (std::numeric_limits<DWORD>::max)()) return out;
    out.file_bytes = bytes.size();

    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_owned = (com_hr == S_OK || com_hr == S_FALSE);
    {
        ComPtr<IWICImagingFactory> wic;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&wic))) && wic.get()) {
            ComPtr<IWICStream> stream;
            if (SUCCEEDED(wic->CreateStream(&stream)) && stream.get() &&
                SUCCEEDED(stream->InitializeFromMemory(
                    reinterpret_cast<WICInProcPointer>(const_cast<char*>(bytes.data())),
                    static_cast<DWORD>(bytes.size())))) {
                ComPtr<IWICBitmapDecoder> decoder;
                if (SUCCEEDED(wic->CreateDecoderFromStream(stream.get(), nullptr,
                                                           WICDecodeMetadataCacheOnLoad, &decoder)) &&
                    decoder.get()) {
                    ComPtr<IWICBitmapFrameDecode> frame;
                    if (SUCCEEDED(decoder->GetFrame(0, &frame)) && frame.get()) {
                        UINT w = 0, h = 0;
                        if (SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0) {
                            out.orig_w = w;
                            out.orig_h = h;
                            UINT dw = w, dh = h;
                            if (max_edge > 0 && (w > max_edge || h > max_edge)) {
                                const double k = static_cast<double>(max_edge) /
                                                 static_cast<double>((std::max)(w, h));
                                dw = (std::max)(1u, static_cast<UINT>(std::lround(w * k)));
                                dh = (std::max)(1u, static_cast<UINT>(std::lround(h * k)));
                            }
                            ComPtr<IWICBitmapSource> source;
                            if (dw != w || dh != h) {
                                ComPtr<IWICBitmapScaler> scaler;
                                if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) && scaler.get() &&
                                    SUCCEEDED(scaler->Initialize(frame.get(), dw, dh,
                                                                 WICBitmapInterpolationModeFant))) {
                                    scaler->QueryInterface(IID_PPV_ARGS(&source));
                                }
                            }
                            if (!source.get()) frame->QueryInterface(IID_PPV_ARGS(&source));

                            ComPtr<IWICFormatConverter> conv;
                            if (source.get() && SUCCEEDED(wic->CreateFormatConverter(&conv)) && conv.get() &&
                                SUCCEEDED(conv->Initialize(source.get(), GUID_WICPixelFormat32bppPBGRA,
                                                           WICBitmapDitherTypeNone, nullptr, 0.0,
                                                           WICBitmapPaletteTypeCustom))) {
                                const UINT stride = dw * 4;
                                out.pixels.resize(static_cast<size_t>(stride) * dh);
                                if (SUCCEEDED(conv->CopyPixels(nullptr, stride,
                                                               static_cast<UINT>(out.pixels.size()),
                                                               out.pixels.data()))) {
                                    out.bmp_w = dw;
                                    out.bmp_h = dh;
                                } else {
                                    out.pixels.clear();
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    if (com_owned) CoUninitialize();
    return out;
}

ComPtr<ID2D1Bitmap> CreateBitmapFromDecodedImage(ID2D1DeviceContext* dc, const DecodedGitImage& img) {
    ComPtr<ID2D1Bitmap> bmp;
    if (!dc || !img.ok()) return bmp;
    const D2D1_BITMAP_PROPERTIES props =
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    dc->CreateBitmap(D2D1::SizeU(img.bmp_w, img.bmp_h), img.pixels.data(), img.bmp_w * 4, &props, &bmp);
    return bmp;
}

GitImageCompareLayout LayoutGitImageCompare(const D2D1_RECT_F& bounds,
                                            uint32_t img_w, uint32_t img_h,
                                            float split_ratio, float scale) {
    GitImageCompareLayout lay{};
    const float pad = 20.0f * scale;
    const float meta_h = 30.0f * scale;
    const float avail_w = (std::max)(40.0f * scale, bounds.right - bounds.left - 2.0f * pad);
    const float avail_h = (std::max)(40.0f * scale, bounds.bottom - bounds.top - 2.0f * pad - meta_h);
    lay.stage = D2D1::RectF(bounds.left + pad, bounds.top + pad, bounds.right - pad, bounds.bottom - pad - meta_h);
    lay.meta = D2D1::RectF(bounds.left + pad, lay.stage.bottom + 8.0f * scale, bounds.right - pad, bounds.bottom - 8.0f * scale);

    const float iw = static_cast<float>((std::max)(1u, img_w));
    const float ih = static_cast<float>((std::max)(1u, img_h));
    const float k = (std::min)(avail_w / iw, avail_h / ih);
    const float draw_w = std::floor(iw * k);
    const float draw_h = std::floor(ih * k);
    const float ix = std::floor((lay.stage.left + lay.stage.right - draw_w) * 0.5f);
    const float iy = std::floor((lay.stage.top + lay.stage.bottom - draw_h) * 0.5f);
    lay.image = D2D1::RectF(ix, iy, ix + draw_w, iy + draw_h);

    const float r = std::clamp(split_ratio, 0.0f, 1.0f);
    lay.split_x = lay.image.left + draw_w * r;
    const float line_w = (std::max)(2.0f, std::round(2.0f * scale));
    lay.divider = D2D1::RectF(lay.split_x - line_w * 0.5f, lay.image.top,
                              lay.split_x + line_w * 0.5f, lay.image.bottom);
    const float hr = 15.0f * scale;
    const float cy = (lay.image.top + lay.image.bottom) * 0.5f;
    lay.handle = D2D1::RectF(lay.split_x - hr, cy - hr, lay.split_x + hr, cy + hr);
    return lay;
}

float GitImageSplitFromX(const GitImageCompareLayout& lay, float x) {
    const float w = lay.image.right - lay.image.left;
    if (w <= 1.0f) return 0.5f;
    return std::clamp((x - lay.image.left) / w, 0.0f, 1.0f);
}

void DrawGitImageCompare(ID2D1DeviceContext* dc, fluent::Painter& painter,
                         IDWriteTextFormat* small_fmt, ID2D1SolidColorBrush* brush,
                         const Theme& theme, float scale, const D2D1_RECT_F& bounds,
                         ID2D1Bitmap* old_bmp, const DecodedGitImage* old_meta,
                         ID2D1Bitmap* new_bmp, const DecodedGitImage* new_meta,
                         float split_ratio, bool plain_terms, bool dragging) {
    if (!dc || !brush || !small_fmt) return;
    const DecodedGitImage* ref = new_meta && new_meta->ok() ? new_meta : old_meta;
    const uint32_t img_w = ref && ref->orig_w ? ref->orig_w : 640;
    const uint32_t img_h = ref && ref->orig_h ? ref->orig_h : 360;
    const GitImageCompareLayout lay = LayoutGitImageCompare(bounds, img_w, img_h, split_ratio, scale);

    // Subtle dark checkerboard/frame backdrop behind the image.
    painter.FillRoundedRect(lay.image, 8.0f * scale, D2D1::ColorF(0, 0.10f));
    painter.StrokeRoundedRect(lay.image, 8.0f * scale, theme.stroke_card, 1.0f);

    const bool both = (old_bmp != nullptr && new_bmp != nullptr);
    if (both) {
        dc->DrawBitmap(old_bmp, lay.image, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        dc->PushAxisAlignedClip(
            D2D1::RectF(lay.split_x, lay.image.top, lay.image.right, lay.image.bottom),
            D2D1_ANTIALIAS_MODE_ALIASED);
        dc->DrawBitmap(new_bmp, lay.image, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        dc->PopAxisAlignedClip();

        // Divider + center drag knob
        brush->SetColor(D2D1::ColorF(0, 0.25f));
        dc->FillRectangle(D2D1::RectF(lay.divider.left - 1.0f, lay.divider.top,
                                      lay.divider.right + 1.0f, lay.divider.bottom), brush);
        brush->SetColor(D2D1::ColorF(0xFFFFFF, 0.96f));
        dc->FillRectangle(lay.divider, brush);

        const D2D1_ELLIPSE el = D2D1::Ellipse(
            D2D1::Point2F(lay.split_x, (lay.handle.top + lay.handle.bottom) * 0.5f),
            (lay.handle.right - lay.handle.left) * 0.5f,
            (lay.handle.bottom - lay.handle.top) * 0.5f);
        brush->SetColor(D2D1::ColorF(0, 0.22f));
        dc->DrawEllipse(el, brush, 2.0f * scale);
        brush->SetColor(dragging ? theme.accent : D2D1::ColorF(0xFFFFFF, 1.0f));
        dc->FillEllipse(el, brush);

        const auto old_align = small_fmt->GetTextAlignment();
        small_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        brush->SetColor(dragging ? D2D1::ColorF(0xFFFFFF, 1.0f) : D2D1::ColorF(0x222222, 1.0f));
        const std::wstring arrows = L"\u2194";
        dc->DrawTextW(arrows.c_str(), static_cast<UINT32>(arrows.size()), small_fmt, lay.handle, brush);
        small_fmt->SetTextAlignment(old_align);

        // Corner labels
        auto draw_pill = [&](const std::wstring& txt, float px, bool right_align) {
            const float pw = (std::min)(140.0f * scale, static_cast<float>(txt.size()) * 9.0f * scale + 20.0f * scale);
            const D2D1_RECT_F rc = right_align
                ? D2D1::RectF(px - pw, lay.image.top + 10.0f * scale, px, lay.image.top + 32.0f * scale)
                : D2D1::RectF(px, lay.image.top + 10.0f * scale, px + pw, lay.image.top + 32.0f * scale);
            painter.FillRoundedRect(rc, 6.0f * scale, D2D1::ColorF(0, 0.52f));
            const auto oa = small_fmt->GetTextAlignment();
            small_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            brush->SetColor(D2D1::ColorF(0xFFFFFF, 0.95f));
            dc->DrawTextW(txt.c_str(), static_cast<UINT32>(txt.size()), small_fmt, rc, brush);
            small_fmt->SetTextAlignment(oa);
        };
        draw_pill(app::GitStr(plain_terms, l10n::StringId::GitDiffPrevSidePlain, l10n::StringId::GitDiffHeadSideGit),
                  lay.image.left + 10.0f * scale, false);
        draw_pill(app::GitStr(plain_terms, l10n::StringId::GitDiffNowSidePlain, l10n::StringId::GitDiffWorkSideGit),
                  lay.image.right - 10.0f * scale, true);
    } else if (new_bmp) {
        dc->DrawBitmap(new_bmp, lay.image, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else if (old_bmp) {
        dc->DrawBitmap(old_bmp, lay.image, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    // Bottom metadata strip
    std::wstring meta_text;
    if (both) {
        meta_text = app::GitStr(plain_terms, l10n::StringId::GitImgDragHintPlain, l10n::StringId::GitImgDragHintGit);
    } else if (new_meta && !old_meta) {
        meta_text = l10n::Get(l10n::StringId::GitImgNewPlain);
    }
    if (old_meta && new_meta && old_meta->ok() && new_meta->ok()) {
        std::wstring dim;
        if (old_meta->orig_w == new_meta->orig_w && old_meta->orig_h == new_meta->orig_h) {
            dim = app::GitFmt(l10n::Get(l10n::StringId::GitImgDimSame), L"{w}", std::to_wstring(new_meta->orig_w));
            dim = app::GitFmt(std::move(dim), L"{h}", std::to_wstring(new_meta->orig_h));
        } else {
            dim = app::GitFmt(l10n::Get(l10n::StringId::GitImgDimChanged), L"{w1}", std::to_wstring(old_meta->orig_w));
            dim = app::GitFmt(std::move(dim), L"{h1}", std::to_wstring(old_meta->orig_h));
            dim = app::GitFmt(std::move(dim), L"{w2}", std::to_wstring(new_meta->orig_w));
            dim = app::GitFmt(std::move(dim), L"{h2}", std::to_wstring(new_meta->orig_h));
        }
        meta_text += L"  \u00B7  " + dim + L"  \u00B7  " +
                     FormatImageBytes(old_meta->file_bytes) + L" \u2192 " +
                     FormatImageBytes(new_meta->file_bytes);
    } else if (ref && ref->ok()) {
        meta_text += L"  \u00B7  " + std::to_wstring(ref->orig_w) + L" \u00D7 " +
                     std::to_wstring(ref->orig_h) + L"  \u00B7  " + FormatImageBytes(ref->file_bytes);
    }

    const auto oa = small_fmt->GetTextAlignment();
    small_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    brush->SetColor(theme.text_secondary);
    dc->DrawTextW(meta_text.c_str(), static_cast<UINT32>(meta_text.size()), small_fmt, lay.meta, brush);
    small_fmt->SetTextAlignment(oa);
}

}  // namespace pulse::ui

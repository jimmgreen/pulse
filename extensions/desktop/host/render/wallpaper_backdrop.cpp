// wallpaper_backdrop.cpp - 1/4-res WIC/preset wallpaper generator, separable box-blur baker,
// dominant vibrant accent extractor, and non-blocking <= 6 MB LRU thumbnail/icon cache.
#include "host/render/wallpaper_backdrop.h"
#include "host/render/desktop_appearance.h"
#include "../../../../src/ui/window_material.h"

#include <d2d1effects.h>
#include <d2d1effects_2.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <malloc.h>
#include <vector>

namespace pulse::desktop::render {
namespace {

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

constexpr float kPi = 3.14159265358979323846f;

D2D1_COLOR_F ColorFromArgb(uint32_t argb, float alpha_mul = 1.0f) noexcept {
    const float a = (static_cast<float>((argb >> 24) & 0xFFu) / 255.0f) * alpha_mul;
    const float r = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
    const float g = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
    const float b = static_cast<float>(argb & 0xFFu) / 255.0f;
    return D2D1::ColorF(r, g, b, std::clamp(a, 0.0f, 1.0f));
}

bool IsRasterImageExtension(std::wstring_view ext) noexcept {
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".bmp" ||
           ext == L".gif" || ext == L".webp" || ext == L".tif" || ext == L".tiff";
}

std::wstring LowerExtension(const std::wstring& path) {
    std::wstring ext = fs::path(path).extension().wstring();
    for (wchar_t& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
    return ext;
}

void RgbToHsl(float r, float g, float b, float& out_h, float& out_s, float& out_l) noexcept {
    const float mx = std::max({r, g, b});
    const float mn = std::min({r, g, b});
    out_l = 0.5f * (mx + mn);
    const float d = mx - mn;
    if (d < 1e-5f) {
        out_h = 0.0f;
        out_s = 0.0f;
        return;
    }
    out_s = out_l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    if (mx == r) {
        out_h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    } else if (mx == g) {
        out_h = (b - r) / d + 2.0f;
    } else {
        out_h = (r - g) / d + 4.0f;
    }
    out_h /= 6.0f;
}

float Hue2Rgb(float p, float q, float t) noexcept {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

uint32_t HslToArgb(float h, float s, float l) noexcept {
    float r = l, g = l, b = l;
    if (s > 1e-4f) {
        const float q = l < 0.5f ? l * (1.0f + s) : (l + s - l * s);
        const float p = 2.0f * l - q;
        r = Hue2Rgb(p, q, h + 1.0f / 3.0f);
        g = Hue2Rgb(p, q, h);
        b = Hue2Rgb(p, q, h - 1.0f / 3.0f);
    }
    const uint32_t ri = static_cast<uint32_t>(std::clamp(r * 255.0f + 0.5f, 0.0f, 255.0f));
    const uint32_t gi = static_cast<uint32_t>(std::clamp(g * 255.0f + 0.5f, 0.0f, 255.0f));
    const uint32_t bi = static_cast<uint32_t>(std::clamp(b * 255.0f + 0.5f, 0.0f, 255.0f));
    return 0xFF000000u | (ri << 16) | (gi << 8) | bi;
}

void FillPolygonNormalized(ID2D1Factory* factory, ID2D1RenderTarget* rt, const D2D1_RECT_F& rc,
                           const D2D1_POINT_2F* norm_pts, size_t count, ID2D1Brush* brush) {
    if (!factory || !rt || !norm_pts || count < 3 || !brush) return;
    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(factory->CreatePathGeometry(&geo)) || !geo) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geo->Open(&sink)) || !sink) return;
    const float w = rc.right - rc.left;
    const float h = rc.bottom - rc.top;
    sink->BeginFigure(D2D1::Point2F(rc.left + norm_pts[0].x * w, rc.top + norm_pts[0].y * h),
                      D2D1_FIGURE_BEGIN_FILLED);
    for (size_t i = 1; i < count; ++i) {
        sink->AddLine(D2D1::Point2F(rc.left + norm_pts[i].x * w, rc.top + norm_pts[i].y * h));
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (SUCCEEDED(sink->Close())) {
        rt->FillGeometry(geo.Get(), brush);
    }
}

void SynthesizePresetFallbackCpu(uint8_t* dst, uint32_t w, uint32_t h,
                                 model::WallpaperPreset preset, bool dark_mode) noexcept {
    uint32_t c0 = 0xFF0A1529u;
    uint32_t c1 = 0xFF1D4F8Cu;
    switch (preset) {
    case model::WallpaperPreset::Dusk:
        c0 = dark_mode ? 0xFF1A1228u : 0xFFFDE1D3u;
        c1 = dark_mode ? 0xFF7C3448u : 0xFFD9C2EEu;
        break;
    case model::WallpaperPreset::Forest:
        c0 = dark_mode ? 0xFF0B1D19u : 0xFFD4EDE3u;
        c1 = dark_mode ? 0xFF215C48u : 0xFFA8D4C2u;
        break;
    case model::WallpaperPreset::Photo:
        c0 = dark_mode ? 0xFF2A2550u : 0xFFE89B58u;
        c1 = dark_mode ? 0xFF8A5EA8u : 0xFFFBE3B8u;
        break;
    case model::WallpaperPreset::Bloom:
    case model::WallpaperPreset::System:
    default:
        c0 = dark_mode ? 0xFF0A1529u : 0xFFCFE4FFu;
        c1 = dark_mode ? 0xFF1D4F8Cu : 0xFFB9D4FAu;
        break;
    }

    const float r0 = static_cast<float>((c0 >> 16) & 0xFFu);
    const float g0 = static_cast<float>((c0 >> 8) & 0xFFu);
    const float b0 = static_cast<float>(c0 & 0xFFu);
    const float r1 = static_cast<float>((c1 >> 16) & 0xFFu);
    const float g1 = static_cast<float>((c1 >> 8) & 0xFFu);
    const float b1 = static_cast<float>(c1 & 0xFFu);

    for (uint32_t y = 0; y < h; ++y) {
        const float ty = h > 1 ? static_cast<float>(y) / static_cast<float>(h - 1) : 0.0f;
        uint8_t* row = dst + static_cast<size_t>(y) * w * 4u;
        for (uint32_t x = 0; x < w; ++x) {
            const float tx = w > 1 ? static_cast<float>(x) / static_cast<float>(w - 1) : 0.0f;
            const float t = std::clamp(0.55f * tx + 0.45f * ty, 0.0f, 1.0f);
            row[x * 4 + 0] = static_cast<uint8_t>(b0 + (b1 - b0) * t);
            row[x * 4 + 1] = static_cast<uint8_t>(g0 + (g1 - g0) * t);
            row[x * 4 + 2] = static_cast<uint8_t>(r0 + (r1 - r0) * t);
            row[x * 4 + 3] = 255u;
        }
    }
}

void SynthesizeFallbackWallpaper(uint8_t* dst, uint32_t w, uint32_t h) noexcept {
    const DWORD desk_col = GetSysColor(COLOR_DESKTOP);
    const float base_r = static_cast<float>(GetRValue(desk_col));
    const float base_g = static_cast<float>(GetGValue(desk_col));
    const float base_b = static_cast<float>(GetBValue(desk_col));
    const bool near_black = (base_r + base_g + base_b) < 24.0f;
    if (near_black) {
        SynthesizePresetFallbackCpu(dst, w, h, model::WallpaperPreset::Bloom, true);
        return;
    }

    const float r0 = base_r * 0.65f;
    const float g0 = base_g * 0.70f;
    const float b0 = base_b * 0.85f;
    const float r1 = std::min(255.0f, base_r * 1.25f + 35.0f);
    const float g1 = std::min(255.0f, base_g * 1.20f + 45.0f);
    const float b1 = std::min(255.0f, base_b * 1.15f + 65.0f);

    for (uint32_t y = 0; y < h; ++y) {
        const float ty = h > 1 ? static_cast<float>(y) / static_cast<float>(h - 1) : 0.0f;
        uint8_t* row = dst + static_cast<size_t>(y) * w * 4u;
        for (uint32_t x = 0; x < w; ++x) {
            const float tx = w > 1 ? static_cast<float>(x) / static_cast<float>(w - 1) : 0.0f;
            const float t = std::clamp(0.55f * tx + 0.45f * ty, 0.0f, 1.0f);
            row[x * 4 + 0] = static_cast<uint8_t>(b0 + (b1 - b0) * t);
            row[x * 4 + 1] = static_cast<uint8_t>(g0 + (g1 - g0) * t);
            row[x * 4 + 2] = static_cast<uint8_t>(r0 + (r1 - r0) * t);
            row[x * 4 + 3] = 255u;
        }
    }
}

void BoxBlurHorizontal(const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h, int radius) noexcept {
    if (radius <= 0 || w <= 1) {
        std::memcpy(dst, src, static_cast<size_t>(w) * h * 4u);
        return;
    }
    const uint32_t win = static_cast<uint32_t>(2 * radius + 1);
    const int max_x = static_cast<int>(w) - 1;

    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* srow = src + static_cast<size_t>(y) * w * 4u;
        uint8_t* drow = dst + static_cast<size_t>(y) * w * 4u;

        uint32_t sum_b = 0, sum_g = 0, sum_r = 0;
        for (int k = -radius; k <= radius; ++k) {
            const int cx = std::clamp<int>(k, 0, max_x);
            const uint8_t* p = srow + cx * 4;
            sum_b += p[0];
            sum_g += p[1];
            sum_r += p[2];
        }
        for (int x = 0; x <= max_x; ++x) {
            uint8_t* out = drow + x * 4;
            out[0] = static_cast<uint8_t>((sum_b + win / 2) / win);
            out[1] = static_cast<uint8_t>((sum_g + win / 2) / win);
            out[2] = static_cast<uint8_t>((sum_r + win / 2) / win);
            out[3] = 255u;

            const int out_idx = std::clamp<int>(x - radius, 0, max_x);
            const int in_idx = std::clamp<int>(x + radius + 1, 0, max_x);
            const uint8_t* p_out = srow + out_idx * 4;
            const uint8_t* p_in = srow + in_idx * 4;
            sum_b = sum_b + p_in[0] - p_out[0];
            sum_g = sum_g + p_in[1] - p_out[1];
            sum_r = sum_r + p_in[2] - p_out[2];
        }
    }
}

void BoxBlurVertical(const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h, int radius) noexcept {
    if (radius <= 0 || h <= 1) {
        std::memcpy(dst, src, static_cast<size_t>(w) * h * 4u);
        return;
    }
    const uint32_t win = static_cast<uint32_t>(2 * radius + 1);
    const int max_y = static_cast<int>(h) - 1;
    const size_t stride = static_cast<size_t>(w) * 4u;

    for (uint32_t x = 0; x < w; ++x) {
        const size_t col_off = static_cast<size_t>(x) * 4u;
        uint32_t sum_b = 0, sum_g = 0, sum_r = 0;
        for (int k = -radius; k <= radius; ++k) {
            const int cy = std::clamp<int>(k, 0, max_y);
            const uint8_t* p = src + static_cast<size_t>(cy) * stride + col_off;
            sum_b += p[0];
            sum_g += p[1];
            sum_r += p[2];
        }
        for (int y = 0; y <= max_y; ++y) {
            uint8_t* out = dst + static_cast<size_t>(y) * stride + col_off;
            out[0] = static_cast<uint8_t>((sum_b + win / 2) / win);
            out[1] = static_cast<uint8_t>((sum_g + win / 2) / win);
            out[2] = static_cast<uint8_t>((sum_r + win / 2) / win);
            out[3] = 255u;

            const int out_idx = std::clamp<int>(y - radius, 0, max_y);
            const int in_idx = std::clamp<int>(y + radius + 1, 0, max_y);
            const uint8_t* p_out = src + static_cast<size_t>(out_idx) * stride + col_off;
            const uint8_t* p_in = src + static_cast<size_t>(in_idx) * stride + col_off;
            sum_b = sum_b + p_in[0] - p_out[0];
            sum_g = sum_g + p_in[1] - p_out[1];
            sum_r = sum_r + p_in[2] - p_out[2];
        }
    }
}

void SeparableBoxBlur3Pass(const uint8_t* input, std::vector<uint8_t>& buf_a, std::vector<uint8_t>& buf_b,
                           uint32_t w, uint32_t h, int radius) noexcept {
    BoxBlurHorizontal(input, buf_a.data(), w, h, radius);
    BoxBlurVertical(buf_a.data(), buf_b.data(), w, h, radius);
    BoxBlurHorizontal(buf_b.data(), buf_a.data(), w, h, radius);
    BoxBlurVertical(buf_a.data(), buf_b.data(), w, h, radius);
    BoxBlurHorizontal(buf_b.data(), buf_a.data(), w, h, radius);
    BoxBlurVertical(buf_a.data(), buf_b.data(), w, h, radius);
}

void BoostSaturationInPlace(uint8_t* bgra, size_t pixel_count, float sat_boost) noexcept {
    for (size_t i = 0; i < pixel_count; ++i) {
        uint8_t* p = bgra + i * 4;
        const float b = static_cast<float>(p[0]);
        const float g = static_cast<float>(p[1]);
        const float r = static_cast<float>(p[2]);
        const float luma = 0.114f * b + 0.587f * g + 0.299f * r;
        p[0] = static_cast<uint8_t>(std::clamp(luma + (b - luma) * sat_boost, 0.0f, 255.0f));
        p[1] = static_cast<uint8_t>(std::clamp(luma + (g - luma) * sat_boost, 0.0f, 255.0f));
        p[2] = static_cast<uint8_t>(std::clamp(luma + (r - luma) * sat_boost, 0.0f, 255.0f));
        p[3] = 255u;
    }
}

ComPtr<ID2D1Bitmap1> UploadImmutablePbgraBitmap(ID2D1DeviceContext* dc, const uint8_t* pixels,
                                                uint32_t w, uint32_t h) {
    if (!dc || !pixels || w == 0 || h == 0) return nullptr;
    D2D1_BITMAP_PROPERTIES1 props{};
    props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
    ComPtr<ID2D1Bitmap1> bmp;
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(w, h), pixels, w * 4u, &props, &bmp))) {
        return nullptr;
    }
    return bmp;
}

bool DecodeWicFileToPbgra(IWICImagingFactory* wic, const std::wstring& path,
                          uint32_t max_w, uint32_t max_h,
                          uint32_t& out_w, uint32_t& out_h, std::vector<uint8_t>& out_pbgra) {
    out_w = 0;
    out_h = 0;
    out_pbgra.clear();
    if (!wic || path.empty() || max_w == 0 || max_h == 0) return false;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnDemand, &decoder)) || !decoder) {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame) return false;

    UINT src_w = 0, src_h = 0;
    if (FAILED(frame->GetSize(&src_w, &src_h)) || src_w == 0 || src_h == 0) return false;

    const double scale = std::min({1.0, static_cast<double>(max_w) / src_w, static_cast<double>(max_h) / src_h});
    const uint32_t dst_w = std::max(1u, static_cast<uint32_t>(std::lround(src_w * scale)));
    const uint32_t dst_h = std::max(1u, static_cast<uint32_t>(std::lround(src_h * scale)));

    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateBitmapScaler(&scaler)) || !scaler ||
        FAILED(scaler->Initialize(frame.Get(), dst_w, dst_h, WICBitmapInterpolationModeLinear))) {
        return false;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter)) || !converter ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return false;
    }

    const uint32_t total_bytes = dst_w * dst_h * 4u;
    out_pbgra.resize(total_bytes);
    if (FAILED(converter->CopyPixels(nullptr, dst_w * 4u, total_bytes, out_pbgra.data()))) {
        out_pbgra.clear();
        return false;
    }
    out_w = dst_w;
    out_h = dst_h;
    return true;
}

bool ConvertAndDownscaleWicSource(IWICImagingFactory* wic, IWICBitmapSource* source, uint32_t max_px,
                                  uint32_t& out_w, uint32_t& out_h, std::vector<uint8_t>& out_pbgra) {
    if (!wic || !source || max_px == 0) return false;
    UINT iw = 0, ih = 0;
    if (FAILED(source->GetSize(&iw, &ih)) || iw == 0 || ih == 0) return false;

    ComPtr<IWICBitmapSource> scaled_src = source;
    uint32_t dst_w = iw;
    uint32_t dst_h = ih;
    if (iw > max_px || ih > max_px) {
        const double scale = std::min(static_cast<double>(max_px) / iw, static_cast<double>(max_px) / ih);
        dst_w = std::max(1u, static_cast<uint32_t>(std::lround(iw * scale)));
        dst_h = std::max(1u, static_cast<uint32_t>(std::lround(ih * scale)));
        ComPtr<IWICBitmapScaler> scaler;
        if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) && scaler &&
            SUCCEEDED(scaler->Initialize(source, dst_w, dst_h, WICBitmapInterpolationModeHighQualityCubic))) {
            scaled_src = scaler;
        } else {
            dst_w = iw;
            dst_h = ih;
        }
    }

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) || !conv ||
        FAILED(conv->Initialize(scaled_src.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return false;
    }
    const uint32_t total = dst_w * dst_h * 4u;
    out_pbgra.resize(total);
    if (FAILED(conv->CopyPixels(nullptr, dst_w * 4u, total, out_pbgra.data()))) {
        out_pbgra.clear();
        return false;
    }
    out_w = dst_w;
    out_h = dst_h;
    return true;
}

// The icon Explorer shows for the item, rendered and downscaled to the requested pixel size.
bool ExtractShellItemImageToPbgra(IWICImagingFactory* wic, const std::wstring& path, uint32_t target_px,
                                  uint32_t& out_w, uint32_t& out_h, std::vector<uint8_t>& out_pbgra) {
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory))) || !factory) return false;
    HBITMAP hbmp = nullptr;
    const SIZE size{static_cast<LONG>(target_px), static_cast<LONG>(target_px)};
    if (FAILED(factory->GetImage(size, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbmp)) || !hbmp) return false;
    bool ok = false;
    ComPtr<IWICBitmap> wic_bmp;
    if (SUCCEEDED(wic->CreateBitmapFromHBITMAP(hbmp, nullptr, WICBitmapUseAlpha, &wic_bmp)) && wic_bmp) {
        ok = ConvertAndDownscaleWicSource(wic, wic_bmp.Get(), target_px, out_w, out_h, out_pbgra);
    }
    DeleteObject(hbmp);
    return ok;
}

bool ExtractShellIconToPbgra(IWICImagingFactory* wic, const std::wstring& path, bool is_folder,
                             bool shared_type_icon, uint32_t target_px,
                             uint32_t& out_w, uint32_t& out_h, std::vector<uint8_t>& out_pbgra) {
    out_w = 0;
    out_h = 0;
    out_pbgra.clear();
    if (!wic || path.empty()) return false;
    if (!shared_type_icon &&
        (path.starts_with(L"::{") || GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) &&
        ExtractShellItemImageToPbgra(wic, path, target_px, out_w, out_h, out_pbgra)) {
        return true;
    }
    if (path.starts_with(L"::{")) return false;  // no file-type fallback for a namespace item

    SHFILEINFOW sfi{};
    const DWORD attrs = is_folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    UINT flags = SHGFI_SYSICONINDEX;
    const std::wstring ext = LowerExtension(path);
    if (shared_type_icon || (!is_folder && ext != L".exe" && ext != L".lnk" && ext != L".ico" && ext != L".url")) {
        flags |= SHGFI_USEFILEATTRIBUTES;
    } else if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        flags |= SHGFI_USEFILEATTRIBUTES;
    }

    if (!SHGetFileInfoW(path.c_str(), attrs, &sfi, sizeof(sfi), flags)) {
        return false;
    }

    HICON hicon = nullptr;
    ComPtr<IImageList> img_list;
    const int list_kind = (target_px > 48u) ? SHIL_JUMBO : SHIL_EXTRALARGE;
    if (SUCCEEDED(SHGetImageList(list_kind, IID_PPV_ARGS(&img_list))) && img_list) {
        img_list->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &hicon);
    }
    if (!hicon && list_kind != SHIL_EXTRALARGE &&
        SUCCEEDED(SHGetImageList(SHIL_EXTRALARGE, IID_PPV_ARGS(&img_list))) && img_list) {
        img_list->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &hicon);
    }
    if (!hicon) {
        SHFILEINFOW sfi_icon{};
        if (SHGetFileInfoW(path.c_str(), attrs, &sfi_icon, sizeof(sfi_icon), flags | SHGFI_ICON | SHGFI_LARGEICON)) {
            hicon = sfi_icon.hIcon;
        }
    }
    if (!hicon) return false;

    bool ok = false;
    ComPtr<IWICBitmap> wic_icon;
    if (SUCCEEDED(wic->CreateBitmapFromHICON(hicon, &wic_icon)) && wic_icon) {
        ok = ConvertAndDownscaleWicSource(wic, wic_icon.Get(), target_px, out_w, out_h, out_pbgra);
    }
    DestroyIcon(hicon);
    return ok;
}

} // namespace

WallpaperBackdrop::~WallpaperBackdrop() {
    ShutdownAsyncWorker();
    ReleaseDeviceResources();
}

uint32_t WallpaperBackdrop::PresetAccentArgb(model::WallpaperPreset preset, bool dark_mode) noexcept {
    switch (preset) {
    case model::WallpaperPreset::Dusk:
        return dark_mode ? 0xFFE68A6Eu : 0xFFB84A32u;
    case model::WallpaperPreset::Forest:
        return dark_mode ? 0xFF5FD08Au : 0xFF1E7F45u;
    case model::WallpaperPreset::Photo:
        return dark_mode ? 0xFF7FB2FFu : 0xFF2459B8u;
    case model::WallpaperPreset::Bloom:
    case model::WallpaperPreset::System:
    default:
        return dark_mode ? 0xFF4CC2FFu : 0xFF0067C0u;
    }
}

void WallpaperBackdrop::DrawPresetScene(ID2D1Factory* factory, ID2D1RenderTarget* rt,
                                        const D2D1_RECT_F& rect, model::WallpaperPreset preset, bool dark_mode) {
    if (!factory || !rt) return;
    const float w = rect.right - rect.left;
    const float h = rect.bottom - rect.top;
    if (w <= 0.0f || h <= 0.0f) return;

    if (preset == model::WallpaperPreset::Photo) {
        // 1. Alpine twilight / warm sunrise sky vertical gradient
        D2D1_GRADIENT_STOP sky_stops[3]{};
        sky_stops[0] = {0.00f, ColorFromArgb(dark_mode ? 0xFF2A2550u : 0xFFE89B58u)};
        sky_stops[1] = {0.55f, ColorFromArgb(dark_mode ? 0xFF5A3D7Au : 0xFFF6B26Bu)};
        sky_stops[2] = {1.00f, ColorFromArgb(dark_mode ? 0xFF8A5EA8u : 0xFFFBE3B8u)};
        ComPtr<ID2D1GradientStopCollection> sky_col;
        if (SUCCEEDED(rt->CreateGradientStopCollection(sky_stops, 3, &sky_col)) && sky_col) {
            ComPtr<ID2D1LinearGradientBrush> sky_br;
            if (SUCCEEDED(rt->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(rect.left, rect.top),
                        D2D1::Point2F(rect.left, rect.top + h * 0.73f)),
                    sky_col.Get(), &sky_br)) && sky_br) {
                rt->FillRectangle(rect, sky_br.Get());
            }
        }

        // 2. Glowing sun / moon halo
        const D2D1_POINT_2F sun_c = D2D1::Point2F(rect.left + w * 0.68f, rect.top + h * 0.34f);
        D2D1_GRADIENT_STOP glow_stops[3]{};
        glow_stops[0] = {0.00f, ColorFromArgb(dark_mode ? 0xFFFFDCA8u : 0xFFFFF8E0u, dark_mode ? 0.92f : 0.96f)};
        glow_stops[1] = {0.28f, ColorFromArgb(dark_mode ? 0xFFFFC088u : 0xFFFFE0A8u, dark_mode ? 0.38f : 0.45f)};
        glow_stops[2] = {1.00f, ColorFromArgb(0x00FFC088u, 0.0f)};
        ComPtr<ID2D1GradientStopCollection> glow_col;
        if (SUCCEEDED(rt->CreateGradientStopCollection(glow_stops, 3, &glow_col)) && glow_col) {
            ComPtr<ID2D1RadialGradientBrush> glow_br;
            const float gr = h * 0.28f;
            if (SUCCEEDED(rt->CreateRadialGradientBrush(
                    D2D1::RadialGradientBrushProperties(sun_c, D2D1::Point2F(0, 0), gr, gr),
                    glow_col.Get(), &glow_br)) && glow_br) {
                rt->FillEllipse(D2D1::Ellipse(sun_c, gr, gr), glow_br.Get());
            }
        }
        ComPtr<ID2D1SolidColorBrush> solid;
        rt->CreateSolidColorBrush(ColorFromArgb(dark_mode ? 0xFFFFEED0u : 0xFFFFFBEAu, 0.94f), &solid);
        if (solid) {
            const float sr = std::max(4.0f, h * 0.038f);
            rt->FillEllipse(D2D1::Ellipse(sun_c, sr, sr), solid.Get());

            // 3. Distant & foreground mountain ridges
            static constexpr D2D1_POINT_2F kFarRidge[] = {
                {0.00f, 0.72f}, {0.12f, 0.55f}, {0.25f, 0.62f}, {0.40f, 0.45f},
                {0.55f, 0.58f}, {0.71f, 0.42f}, {0.86f, 0.54f}, {1.00f, 0.48f},
                {1.00f, 0.72f}
            };
            solid->SetColor(ColorFromArgb(dark_mode ? 0xFF3B3463u : 0xFFC98A78u));
            FillPolygonNormalized(factory, rt, rect, kFarRidge, std::size(kFarRidge), solid.Get());

            static constexpr D2D1_POINT_2F kNearRidge[] = {
                {0.00f, 0.73f}, {0.18f, 0.62f}, {0.34f, 0.68f}, {0.52f, 0.54f},
                {0.68f, 0.65f}, {0.84f, 0.58f}, {1.00f, 0.64f}, {1.00f, 0.73f}
            };
            solid->SetColor(ColorFromArgb(dark_mode ? 0xFF1F1B3Du : 0xFF8F5D6Bu));
            FillPolygonNormalized(factory, rt, rect, kNearRidge, std::size(kNearRidge), solid.Get());
        }

        // 4. Lake reflection gradient + water shimmer strokes
        const D2D1_RECT_F lake_rc = D2D1::RectF(rect.left, rect.top + h * 0.71f, rect.right, rect.bottom);
        D2D1_GRADIENT_STOP lake_stops[3]{};
        lake_stops[0] = {0.00f, ColorFromArgb(dark_mode ? 0xFF5C4F8Fu : 0xFFEAB08Au)};
        lake_stops[1] = {0.45f, ColorFromArgb(dark_mode ? 0xFF352C5Eu : 0xFFB87E88u)};
        lake_stops[2] = {1.00f, ColorFromArgb(dark_mode ? 0xFF1A1634u : 0xFF7D5874u)};
        ComPtr<ID2D1GradientStopCollection> lake_col;
        if (SUCCEEDED(rt->CreateGradientStopCollection(lake_stops, 3, &lake_col)) && lake_col) {
            ComPtr<ID2D1LinearGradientBrush> lake_br;
            if (SUCCEEDED(rt->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(rect.left, lake_rc.top),
                        D2D1::Point2F(rect.left, lake_rc.bottom)),
                    lake_col.Get(), &lake_br)) && lake_br) {
                rt->FillRectangle(lake_rc, lake_br.Get());
            }
        }
        if (solid) {
            for (int i = 0; i < 11; ++i) {
                const float t = static_cast<float>(i) / 10.0f;
                const float sy = rect.top + h * (0.735f + t * 0.21f);
                const float half_len = w * (0.045f - t * 0.025f) + ((i % 3) * 6.0f);
                solid->SetColor(ColorFromArgb(0xFFFFE2B4u, (1.0f - t * 0.72f) * 0.26f));
                rt->DrawLine(D2D1::Point2F(sun_c.x - half_len, sy),
                             D2D1::Point2F(sun_c.x + half_len, sy), solid.Get(), 1.4f);
            }
        }
        return;
    }

    // Bloom / Dusk / Forest multi-layer diagonal gradient + 7 rotated Windows-Bloom aurora petals
    uint32_t stops_hex[4]{};
    uint32_t petal_rgb = 0xFF5FB4FFu;
    switch (preset) {
    case model::WallpaperPreset::Dusk:
        stops_hex[0] = dark_mode ? 0xFF1A1228u : 0xFFFDE1D3u;
        stops_hex[1] = dark_mode ? 0xFF3C1E3Eu : 0xFFF5C6D6u;
        stops_hex[2] = dark_mode ? 0xFF7C3448u : 0xFFD9C2EEu;
        stops_hex[3] = dark_mode ? 0xFF22182Du : 0xFFFBE9DCu;
        petal_rgb = dark_mode ? 0xFFFF9470u : 0xFFE6765Cu;
        break;
    case model::WallpaperPreset::Forest:
        stops_hex[0] = dark_mode ? 0xFF0B1D19u : 0xFFD4EDE3u;
        stops_hex[1] = dark_mode ? 0xFF15382Fu : 0xFFBFE2D4u;
        stops_hex[2] = dark_mode ? 0xFF215C48u : 0xFFA8D4C2u;
        stops_hex[3] = dark_mode ? 0xFF0D231Du : 0xFFE4F3ECu;
        petal_rgb = dark_mode ? 0xFF5FDC9Bu : 0xFF3EB278u;
        break;
    case model::WallpaperPreset::Bloom:
    case model::WallpaperPreset::System:
    default:
        stops_hex[0] = dark_mode ? 0xFF0A1529u : 0xFFCFE4FFu;
        stops_hex[1] = dark_mode ? 0xFF142A52u : 0xFFDDE8FFu;
        stops_hex[2] = dark_mode ? 0xFF1D4F8Cu : 0xFFB9D4FAu;
        stops_hex[3] = dark_mode ? 0xFF0E1D3Au : 0xFFE8F0FEu;
        petal_rgb = dark_mode ? 0xFF5FB4FFu : 0xFF468CF5u;
        break;
    }

    D2D1_GRADIENT_STOP base_stops[4] = {
        {0.00f, ColorFromArgb(stops_hex[0])},
        {0.36f, ColorFromArgb(stops_hex[1])},
        {0.72f, ColorFromArgb(stops_hex[2])},
        {1.00f, ColorFromArgb(stops_hex[3])},
    };
    ComPtr<ID2D1GradientStopCollection> base_col;
    if (SUCCEEDED(rt->CreateGradientStopCollection(base_stops, 4, &base_col)) && base_col) {
        ComPtr<ID2D1LinearGradientBrush> base_br;
        if (SUCCEEDED(rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(rect.left, rect.top),
                    D2D1::Point2F(rect.right, rect.bottom)),
                base_col.Get(), &base_br)) && base_br) {
            rt->FillRectangle(rect, base_br.Get());
        }
    }

    D2D1_MATRIX_3X2_F saved_xf{};
    rt->GetTransform(&saved_xf);

    const float cx = rect.left + w * 0.60f;
    const float cy = rect.top + h * 0.52f;
    const float rx = w * 0.22f;
    const float ry = h * 0.135f;

    D2D1_GRADIENT_STOP petal_stops[3] = {
        {0.00f, ColorFromArgb(petal_rgb, dark_mode ? 0.36f : 0.48f)},
        {0.55f, ColorFromArgb(petal_rgb, dark_mode ? 0.14f : 0.20f)},
        {1.00f, ColorFromArgb(petal_rgb, 0.0f)},
    };
    ComPtr<ID2D1GradientStopCollection> petal_col;
    if (SUCCEEDED(rt->CreateGradientStopCollection(petal_stops, 3, &petal_col)) && petal_col) {
        for (int i = 0; i < 7; ++i) {
            const float ang_rad = (static_cast<float>(i) / 7.0f) * 2.0f * kPi + 0.30f;
            const float ang_deg = ang_rad * (180.0f / kPi);
            const D2D1_POINT_2F pc = D2D1::Point2F(
                cx + std::cos(ang_rad) * w * 0.045f,
                cy + std::sin(ang_rad) * h * 0.045f);

            ComPtr<ID2D1RadialGradientBrush> pbr;
            if (SUCCEEDED(rt->CreateRadialGradientBrush(
                    D2D1::RadialGradientBrushProperties(pc, D2D1::Point2F(0, 0), rx, ry),
                    petal_col.Get(), &pbr)) && pbr) {
                rt->SetTransform(D2D1::Matrix3x2F::Rotation(ang_deg, pc) * saved_xf);
                rt->FillEllipse(D2D1::Ellipse(pc, rx, ry), pbr.Get());
            }
        }
    }
    rt->SetTransform(saved_xf);

    // Central luminous bloom core
    D2D1_GRADIENT_STOP core_stops[2] = {
        {0.0f, ColorFromArgb(0xFFFFFFFFu, dark_mode ? 0.12f : 0.32f)},
        {1.0f, ColorFromArgb(0x00FFFFFFu, 0.0f)},
    };
    ComPtr<ID2D1GradientStopCollection> core_col;
    if (SUCCEEDED(rt->CreateGradientStopCollection(core_stops, 2, &core_col)) && core_col) {
        ComPtr<ID2D1RadialGradientBrush> core_br;
        const float cr = w * 0.18f;
        if (SUCCEEDED(rt->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), cr, cr),
                core_col.Get(), &core_br)) && core_br) {
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), cr, cr), core_br.Get());
        }
    }
}

bool WallpaperBackdrop::RenderPresetWallpaperFile(const std::wstring& png_path,
                                                  uint32_t width_px, uint32_t height_px,
                                                  model::WallpaperPreset preset, bool dark_mode) {
    if (png_path.empty() || preset == model::WallpaperPreset::System) return false;
    const uint32_t w = std::clamp(width_px, 320u, 3840u);
    const uint32_t h = std::clamp(height_px, 240u, 2400u);

    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic))) || !wic) {
        return false;
    }

    ComPtr<ID2D1Factory> d2d_fac;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&d2d_fac))) || !d2d_fac) {
        return false;
    }

    ComPtr<IWICBitmap> wic_bmp;
    if (FAILED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA,
                                 WICBitmapCacheOnLoad, &wic_bmp)) || !wic_bmp) {
        return false;
    }

    const D2D1_RENDER_TARGET_PROPERTIES rt_props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(d2d_fac->CreateWicBitmapRenderTarget(wic_bmp.Get(), rt_props, &rt)) || !rt) {
        return false;
    }

    rt->BeginDraw();
    DrawPresetScene(d2d_fac.Get(), rt.Get(),
                    D2D1::RectF(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)),
                    preset, dark_mode);
    if (FAILED(rt->EndDraw())) return false;
    rt.Reset();

    const std::wstring tmp_path = png_path + L".tmp";
    bool encoded = false;
    {
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> props;
        if (SUCCEEDED(wic->CreateStream(&stream)) && stream &&
            SUCCEEDED(stream->InitializeFromFilename(tmp_path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && enc &&
            SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(enc->CreateNewFrame(&frame, &props)) && frame &&
            SUCCEEDED(frame->Initialize(props.Get())) &&
            SUCCEEDED(frame->SetSize(w, h))) {
            WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
            if (SUCCEEDED(frame->SetPixelFormat(&pf)) &&
                SUCCEEDED(frame->WriteSource(wic_bmp.Get(), nullptr)) &&
                SUCCEEDED(frame->Commit()) &&
                SUCCEEDED(enc->Commit())) {
                encoded = true;
            }
        }
    }
    if (!encoded) {
        DeleteFileW(tmp_path.c_str());
        return false;
    }
    return MoveFileExW(tmp_path.c_str(), png_path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

void WallpaperBackdrop::DrawPhotoTileScene(ID2D1Factory* factory, ID2D1RenderTarget* rt,
                                           const D2D1_RECT_F& rect, int scene_index, bool /*dark_mode*/) {
    if (!factory || !rt) return;
    const float w = rect.right - rect.left;
    const float h = rect.bottom - rect.top;
    if (w <= 0.0f || h <= 0.0f) return;

    struct ScenePalette {
        uint32_t sky0;
        uint32_t sky1;
        uint32_t sky2;
        uint32_t sun;
        uint32_t far_mtn;
        uint32_t near_mtn;
        uint32_t lake0;
        uint32_t lake1;
        float sun_x;
        float sun_y;
    };
    static constexpr ScenePalette kScenes[3] = {
        // 0: HTML SCENES[0] — 山脊暮色
        {0xFF1D2B53u, 0xFF7E2553u, 0xFFFF7757u, 0xFFFFDBA8u, 0xFF2D1B3Du, 0xFF160F29u, 0xFF4B244Au, 0xFF160F29u, 0.72f, 0.30f},
        // 1: 湖畔松林晨雾
        {0xFF0F3047u, 0xFF276678u, 0xFF9AD0C2u, 0xFFE8FFF4u, 0xFF1B4552u, 0xFF0E2630u, 0xFF2D6A73u, 0xFF0E232Cu, 0.30f, 0.28f},
        // 2: 金色峡谷晚霞
        {0xFF2C1654u, 0xFF853B6Eu, 0xFFF6A365u, 0xFFFFEBC2u, 0xFF422153u, 0xFF221133u, 0xFF5D2D5Au, 0xFF1C0E2Bu, 0.64f, 0.33f},
    };
    const ScenePalette& pal = kScenes[((scene_index % 3) + 3) % 3];

    // 1. Multi-stop twilight sky
    D2D1_GRADIENT_STOP sky_stops[3] = {
        {0.00f, ColorFromArgb(pal.sky0)},
        {0.52f, ColorFromArgb(pal.sky1)},
        {1.00f, ColorFromArgb(pal.sky2)},
    };
    ComPtr<ID2D1GradientStopCollection> sky_col;
    if (SUCCEEDED(rt->CreateGradientStopCollection(sky_stops, 3, &sky_col)) && sky_col) {
        ComPtr<ID2D1LinearGradientBrush> sky_br;
        if (SUCCEEDED(rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(rect.left, rect.top),
                    D2D1::Point2F(rect.left, rect.top + h * 0.78f)),
                sky_col.Get(), &sky_br)) && sky_br) {
            rt->FillRectangle(rect, sky_br.Get());
        }
    }

    // 2. Sun / moon halo + crisp disc
    const D2D1_POINT_2F sun_pt = D2D1::Point2F(rect.left + w * pal.sun_x, rect.top + h * pal.sun_y);
    D2D1_GRADIENT_STOP halo_stops[3] = {
        {0.00f, ColorFromArgb(pal.sun, 0.68f)},
        {0.40f, ColorFromArgb(pal.sun, 0.24f)},
        {1.00f, ColorFromArgb(pal.sun, 0.00f)},
    };
    ComPtr<ID2D1GradientStopCollection> halo_col;
    if (SUCCEEDED(rt->CreateGradientStopCollection(halo_stops, 3, &halo_col)) && halo_col) {
        ComPtr<ID2D1RadialGradientBrush> halo_br;
        const float hr = h * 0.30f;
        if (SUCCEEDED(rt->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(sun_pt, D2D1::Point2F(0, 0), hr, hr),
                halo_col.Get(), &halo_br)) && halo_br) {
            rt->FillEllipse(D2D1::Ellipse(sun_pt, hr, hr), halo_br.Get());
        }
    }

    ComPtr<ID2D1SolidColorBrush> solid;
    rt->CreateSolidColorBrush(ColorFromArgb(pal.sun, 0.92f), &solid);
    if (solid) {
        const float disc_r = std::max(8.0f, h * 0.082f);
        rt->FillEllipse(D2D1::Ellipse(sun_pt, disc_r, disc_r), solid.Get());

        // 3. Far & near mountain ridges
        static constexpr D2D1_POINT_2F kFarMtn[] = {
            {0.00f, 0.75f}, {0.17f, 0.49f}, {0.34f, 0.61f}, {0.55f, 0.40f},
            {0.76f, 0.56f}, {0.91f, 0.44f}, {1.00f, 0.52f}, {1.00f, 0.78f}, {0.00f, 0.78f}
        };
        solid->SetColor(ColorFromArgb(pal.far_mtn, 0.88f));
        FillPolygonNormalized(factory, rt, rect, kFarMtn, std::size(kFarMtn), solid.Get());

        static constexpr D2D1_POINT_2F kNearMtn[] = {
            {0.00f, 0.79f}, {0.22f, 0.62f}, {0.43f, 0.71f}, {0.66f, 0.56f},
            {0.85f, 0.68f}, {1.00f, 0.61f}, {1.00f, 0.80f}, {0.00f, 0.80f}
        };
        solid->SetColor(ColorFromArgb(pal.near_mtn, 0.95f));
        FillPolygonNormalized(factory, rt, rect, kNearMtn, std::size(kNearMtn), solid.Get());
    }

    // 4. Calm lake reflection + water shimmer lines
    const D2D1_RECT_F lake_rc = D2D1::RectF(rect.left, rect.top + h * 0.76f, rect.right, rect.bottom);
    D2D1_GRADIENT_STOP lake_stops[2] = {
        {0.0f, ColorFromArgb(pal.lake0)},
        {1.0f, ColorFromArgb(pal.lake1)},
    };
    ComPtr<ID2D1GradientStopCollection> lake_col;
    if (SUCCEEDED(rt->CreateGradientStopCollection(lake_stops, 2, &lake_col)) && lake_col) {
        ComPtr<ID2D1LinearGradientBrush> lake_br;
        if (SUCCEEDED(rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(rect.left, lake_rc.top),
                    D2D1::Point2F(rect.left, lake_rc.bottom)),
                lake_col.Get(), &lake_br)) && lake_br) {
            rt->FillRectangle(lake_rc, lake_br.Get());
        }
    }
    if (solid) {
        for (int i = 0; i < 6; ++i) {
            const float t = static_cast<float>(i) / 5.0f;
            const float sy = rect.top + h * (0.79f + t * 0.16f);
            const float hw = w * (0.075f - t * 0.04f);
            solid->SetColor(ColorFromArgb(pal.sun, (1.0f - t * 0.65f) * 0.30f));
            rt->DrawLine(D2D1::Point2F(sun_pt.x - hw, sy), D2D1::Point2F(sun_pt.x + hw, sy), solid.Get(), 1.2f);
        }
    }
}

uint32_t ExtractVibrantAccentArgb(const uint8_t* pbgra, uint32_t width, uint32_t height, uint32_t stride) noexcept {
    if (!pbgra || width == 0 || height == 0 || stride < width * 4u) {
        return 0xFF4CC2FFu;
    }

    struct HueBucket {
        double weight = 0.0;
        double sum_r = 0.0;
        double sum_g = 0.0;
        double sum_b = 0.0;
    };
    std::array<HueBucket, 24> buckets{};

    const uint32_t step_x = std::max(1u, width / 160u);
    const uint32_t step_y = std::max(1u, height / 90u);
    for (uint32_t y = 0; y < height; y += step_y) {
        const uint8_t* row = pbgra + static_cast<size_t>(y) * stride;
        for (uint32_t x = 0; x < width; x += step_x) {
            const uint8_t* px = row + static_cast<size_t>(x) * 4u;
            if (px[3] < 128u) continue;
            const float b = static_cast<float>(px[0]) / 255.0f;
            const float g = static_cast<float>(px[1]) / 255.0f;
            const float r = static_cast<float>(px[2]) / 255.0f;

            float h = 0.0f, s = 0.0f, l = 0.0f;
            RgbToHsl(r, g, b, h, s, l);
            if (l < 0.12f || l > 0.90f || s < 0.16f) continue;

            const double vibrancy = static_cast<double>(s) * (1.0 - std::abs(2.0 * static_cast<double>(l) - 1.0));
            const size_t idx = std::min<size_t>(23, static_cast<size_t>(h * 24.0f));
            buckets[idx].weight += vibrancy;
            buckets[idx].sum_r += static_cast<double>(r) * vibrancy;
            buckets[idx].sum_g += static_cast<double>(g) * vibrancy;
            buckets[idx].sum_b += static_cast<double>(b) * vibrancy;
        }
    }

    size_t best_idx = 0;
    double best_weight = 0.0;
    for (size_t i = 0; i < buckets.size(); ++i) {
        if (buckets[i].weight > best_weight) {
            best_weight = buckets[i].weight;
            best_idx = i;
        }
    }
    if (best_weight <= 1e-4) {
        return 0xFF4CC2FFu;
    }

    const float avg_r = static_cast<float>(buckets[best_idx].sum_r / best_weight);
    const float avg_g = static_cast<float>(buckets[best_idx].sum_g / best_weight);
    const float avg_b = static_cast<float>(buckets[best_idx].sum_b / best_weight);
    float h = 0.0f, s = 0.0f, l = 0.0f;
    RgbToHsl(avg_r, avg_g, avg_b, h, s, l);
    s = std::clamp(s * 1.15f, 0.55f, 0.92f);
    l = std::clamp(l, 0.52f, 0.70f);
    return HslToArgb(h, s, l);
}

std::wstring WallpaperBackdrop::QueryCurrentWallpaperPath() {
    ComPtr<IDesktopWallpaper> dw;
    if (SUCCEEDED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr,
                                   CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dw))) && dw) {
        LPWSTR raw_path = nullptr;
        if (SUCCEEDED(dw->GetWallpaper(nullptr, &raw_path)) && raw_path) {
            std::wstring out(raw_path);
            CoTaskMemFree(raw_path);
            if (!out.empty() && GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return out;
            }
        }
    }
    wchar_t spi_buf[MAX_PATH * 2]{};
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(spi_buf), spi_buf, 0) && spi_buf[0] != L'\0') {
        if (GetFileAttributesW(spi_buf) != INVALID_FILE_ATTRIBUTES) {
            return spi_buf;
        }
    }
    return {};
}

DESKTOP_WALLPAPER_POSITION WallpaperBackdrop::QueryWallpaperPosition() {
    ComPtr<IDesktopWallpaper> dw;
    DESKTOP_WALLPAPER_POSITION pos = DWPOS_FILL;
    if (SUCCEEDED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dw))) && dw) {
        dw->GetPosition(&pos);
    }
    return pos;
}

text_contrast::Samples WallpaperBackdrop::SampleColors(const D2D1_RECT_F& r) const noexcept {
    if (!screen_w_ || !screen_h_) return {};
    const float kx = static_cast<float>(quarter_w_) / screen_w_;
    const float ky = static_cast<float>(quarter_h_) / screen_h_;
    return text_contrast::SamplePbgra(wallpaper_pixels_, quarter_w_, quarter_h_,
        (r.left * dip_scale_ + origin_x_px_) * kx, (r.top * dip_scale_ + origin_y_px_) * ky,
        (r.right * dip_scale_ + origin_x_px_) * kx, (r.bottom * dip_scale_ + origin_y_px_) * ky);
}

float WallpaperBackdrop::AverageLuminance(const D2D1_RECT_F& r) const noexcept {
    const auto samples = SampleColors(r);
    if (!samples.count) return -1.0f;
    double sum = 0;
    for (size_t i = 0; i < samples.count; ++i) sum += text_contrast::RelativeLuminance(samples.colors[i]);
    return static_cast<float>(sum / samples.count);
}

void WallpaperBackdrop::SetDipMapping(float dip_scale, float origin_x_px, float origin_y_px) noexcept {
    const float new_scale = dip_scale > 0.0f ? dip_scale : 1.0f;
    if (new_scale != dip_scale_) {
        for (auto& brush : sampled_mica_brushes_) brush.Reset();
        for (auto& failed : sampled_mica_failed_) failed = false;
        ReleaseMenuMaterialCache();
    }
    dip_scale_ = new_scale;
    origin_x_px_ = origin_x_px;
    origin_y_px_ = origin_y_px;
    ApplyBrushTransforms();
}

// The blurred quarter-size wallpaper covers the monitor; the scene is drawn in DIPs from the layer's origin.
void WallpaperBackdrop::ApplyBrushTransforms() const noexcept {
    if (noise_brush_) noise_brush_->SetTransform(D2D1::Matrix3x2F::Scale(1.0f / dip_scale_, 1.0f / dip_scale_));
    if (!quarter_w_ || !quarter_h_) return;
    const float sx = static_cast<float>(screen_w_) / quarter_w_, sy = static_cast<float>(screen_h_) / quarter_h_;
    const D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Scale(sx / dip_scale_, sy / dip_scale_) *
                                D2D1::Matrix3x2F::Translation(-origin_x_px_ / dip_scale_, -origin_y_px_ / dip_scale_);
    if (acrylic_brush_) acrylic_brush_->SetTransform(m);
    if (mica_brush_) mica_brush_->SetTransform(m);
    for (auto& brush : sampled_mica_brushes_) if (brush) brush->SetTransform(m);
    // One grain texel per physical pixel: scaled with the DIPs it would turn into blotches.
    if (noise_brush_) noise_brush_->SetTransform(D2D1::Matrix3x2F::Scale(1.0f / dip_scale_, 1.0f / dip_scale_));
}

ID2D1Bitmap1* WallpaperBackdrop::QuarterWallpaperBitmap() const {
    if (!wallpaper_quarter_ && owner_dc_ && !wallpaper_pixels_.empty() && quarter_w_ > 0 && quarter_h_ > 0) {
        wallpaper_quarter_ = UploadImmutablePbgraBitmap(owner_dc_.Get(), wallpaper_pixels_.data(), quarter_w_, quarter_h_);
    }
    return wallpaper_quarter_.Get();
}

bool WallpaperBackdrop::EnsureAcrylicBlur() const {
    if (acrylic_blur_ && acrylic_brush_) return true;
    if (!owner_dc_ || wallpaper_pixels_.empty() || quarter_w_ == 0 || quarter_h_ == 0) return false;

    const size_t pixel_count = static_cast<size_t>(quarter_w_) * quarter_h_;
    const size_t byte_count = pixel_count * 4u;
    {
        std::vector<uint8_t> scratch_a(byte_count);
        std::vector<uint8_t> scratch_b(byte_count);
        const int r_acrylic = std::max(2, static_cast<int>(std::lround(7.0 * quarter_w_ / 960.0)));
        SeparableBoxBlur3Pass(wallpaper_pixels_.data(), scratch_a, scratch_b, quarter_w_, quarter_h_, r_acrylic);
        BoostSaturationInPlace(scratch_b.data(), pixel_count, 1.28f);
        acrylic_blur_ = UploadImmutablePbgraBitmap(owner_dc_.Get(), scratch_b.data(), quarter_w_, quarter_h_);
    }
    _heapmin();
    if (!acrylic_blur_) return false;

    D2D1_BITMAP_BRUSH_PROPERTIES1 blur_brush_props{};
    blur_brush_props.extendModeX = D2D1_EXTEND_MODE_CLAMP;
    blur_brush_props.extendModeY = D2D1_EXTEND_MODE_CLAMP;
    blur_brush_props.interpolationMode = D2D1_INTERPOLATION_MODE_LINEAR;

    acrylic_brush_.Reset();
    owner_dc_->CreateBitmapBrush(acrylic_blur_.Get(), &blur_brush_props, nullptr, &acrylic_brush_);
    ApplyBrushTransforms();
    ComPtr<ID2D1Device> dev;
    owner_dc_->GetDevice(&dev);
    if (dev) dev->ClearResources(0);
    return acrylic_brush_ != nullptr;
}

bool WallpaperBackdrop::EnsureMicaBlur() const {
    if (mica_blur_ && mica_brush_) return true;
    if (!owner_dc_ || wallpaper_pixels_.empty() || quarter_w_ == 0 || quarter_h_ == 0) return false;

    const size_t pixel_count = static_cast<size_t>(quarter_w_) * quarter_h_;
    const size_t byte_count = pixel_count * 4u;
    {
        std::vector<uint8_t> scratch_a(byte_count);
        std::vector<uint8_t> scratch_b(byte_count);
        const int r_mica = std::max(6, static_cast<int>(std::lround(21.0 * quarter_w_ / 960.0)));
        SeparableBoxBlur3Pass(wallpaper_pixels_.data(), scratch_a, scratch_b, quarter_w_, quarter_h_, r_mica);
        BoostSaturationInPlace(scratch_b.data(), pixel_count, 1.45f);
        mica_blur_ = UploadImmutablePbgraBitmap(owner_dc_.Get(), scratch_b.data(), quarter_w_, quarter_h_);
    }
    _heapmin();
    if (!mica_blur_) return false;

    D2D1_BITMAP_BRUSH_PROPERTIES1 blur_brush_props{};
    blur_brush_props.extendModeX = D2D1_EXTEND_MODE_CLAMP;
    blur_brush_props.extendModeY = D2D1_EXTEND_MODE_CLAMP;
    blur_brush_props.interpolationMode = D2D1_INTERPOLATION_MODE_LINEAR;

    mica_brush_.Reset();
    owner_dc_->CreateBitmapBrush(mica_blur_.Get(), &blur_brush_props, nullptr, &mica_brush_);
    ApplyBrushTransforms();
    ComPtr<ID2D1Device> dev;
    owner_dc_->GetDevice(&dev);
    if (dev) dev->ClearResources(0);
    return mica_brush_ != nullptr;
}

ID2D1Bitmap1* WallpaperBackdrop::AcrylicBlurBitmap() const {
    EnsureAcrylicBlur();
    return acrylic_blur_.Get();
}

ID2D1Bitmap1* WallpaperBackdrop::MicaBlurBitmap() const {
    EnsureMicaBlur();
    return mica_blur_.Get();
}

ID2D1BitmapBrush1* WallpaperBackdrop::AcrylicBrush() const {
    EnsureAcrylicBlur();
    return acrylic_brush_.Get();
}

ID2D1BitmapBrush1* WallpaperBackdrop::MicaBrush() const {
    EnsureMicaBlur();
    return mica_brush_.Get();
}

namespace {

bool DecodePositionedWallpaperPbgra(IWICImagingFactory* wic,
                                    const std::wstring& path,
                                    DESKTOP_WALLPAPER_POSITION pos,
                                    uint32_t screen_w,
                                    uint32_t screen_h,
                                    uint32_t target_w,
                                    uint32_t target_h,
                                    std::vector<uint8_t>& out_pbgra) noexcept {
    if (!wic || path.empty() || target_w == 0 || target_h == 0 || screen_w == 0 || screen_h == 0) {
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnDemand, &decoder)) || !decoder) {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame) return false;

    UINT iw = 0, ih = 0;
    if (FAILED(frame->GetSize(&iw, &ih)) || iw == 0 || ih == 0) return false;

    const size_t total_bytes = static_cast<size_t>(target_w) * target_h * 4u;
    try {
        out_pbgra.assign(total_bytes, 0);
    } catch (...) {
        return false;
    }

    // Fill background with Windows' desktop solid color for Fit / Center letterbox margins.
    const COLORREF desk_rgb = GetSysColor(COLOR_DESKTOP);
    const uint8_t bg_b = GetBValue(desk_rgb), bg_g = GetGValue(desk_rgb), bg_r = GetRValue(desk_rgb);
    for (size_t i = 0; i < static_cast<size_t>(target_w) * target_h; ++i) {
        out_pbgra[i * 4 + 0] = bg_b;
        out_pbgra[i * 4 + 1] = bg_g;
        out_pbgra[i * 4 + 2] = bg_r;
        out_pbgra[i * 4 + 3] = 255u;
    }

    auto scale_source_into = [&](IWICBitmapSource* src, uint32_t dw, uint32_t dh,
                                 std::vector<uint8_t>& buf) -> bool {
        if (dw == 0 || dh == 0) return false;
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(&scaler)) || !scaler) return false;
        if (FAILED(scaler->Initialize(src, dw, dh, WICBitmapInterpolationModeHighQualityCubic))) {
            if (FAILED(scaler->Initialize(src, dw, dh, WICBitmapInterpolationModeLinear))) return false;
        }
        ComPtr<IWICFormatConverter> conv;
        if (FAILED(wic->CreateFormatConverter(&conv)) || !conv) return false;
        if (FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
            return false;
        }
        buf.resize(static_cast<size_t>(dw) * dh * 4u);
        return SUCCEEDED(conv->CopyPixels(nullptr, dw * 4u, static_cast<UINT>(buf.size()), buf.data()));
    };

    const double screen_aspect = static_cast<double>(screen_w) / static_cast<double>(screen_h);
    const double img_aspect = static_cast<double>(iw) / static_cast<double>(ih);

    if (pos == DWPOS_STRETCH) {
        if (!scale_source_into(frame.Get(), target_w, target_h, out_pbgra)) return false;
    } else if (pos == DWPOS_FIT) {
        uint32_t dw = target_w, dh = target_h;
        if (img_aspect > screen_aspect) {
            dh = std::max(1u, static_cast<uint32_t>(std::lround(target_w / img_aspect)));
        } else {
            dw = std::max(1u, static_cast<uint32_t>(std::lround(target_h * img_aspect)));
        }
        std::vector<uint8_t> fitted;
        if (!scale_source_into(frame.Get(), dw, dh, fitted)) return false;
        const uint32_t ox = (target_w - dw) / 2u, oy = (target_h - dh) / 2u;
        for (uint32_t y = 0; y < dh && (oy + y) < target_h; ++y) {
            std::memcpy(out_pbgra.data() + (static_cast<size_t>(oy + y) * target_w + ox) * 4u,
                        fitted.data() + static_cast<size_t>(y) * dw * 4u,
                        static_cast<size_t>(dw) * 4u);
        }
    } else if (pos == DWPOS_CENTER || pos == DWPOS_TILE) {
        const double kx = static_cast<double>(target_w) / static_cast<double>(screen_w);
        const double ky = static_cast<double>(target_h) / static_cast<double>(screen_h);
        const uint32_t tw = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(iw * kx)), 1u, 4096u);
        const uint32_t th = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(ih * ky)), 1u, 4096u);
        std::vector<uint8_t> tile;
        if (!scale_source_into(frame.Get(), tw, th, tile)) return false;
        if (pos == DWPOS_TILE) {
            for (uint32_t y = 0; y < target_h; ++y) {
                const uint8_t* srow = tile.data() + static_cast<size_t>(y % th) * tw * 4u;
                uint8_t* drow = out_pbgra.data() + static_cast<size_t>(y) * target_w * 4u;
                for (uint32_t x = 0; x < target_w; ++x) {
                    std::memcpy(drow + static_cast<size_t>(x) * 4u, srow + static_cast<size_t>(x % tw) * 4u, 4u);
                }
            }
        } else {
            const int32_t ox = (static_cast<int32_t>(target_w) - static_cast<int32_t>(tw)) / 2;
            const int32_t oy = (static_cast<int32_t>(target_h) - static_cast<int32_t>(th)) / 2;
            for (uint32_t y = 0; y < target_h; ++y) {
                const int32_t sy = static_cast<int32_t>(y) - oy;
                if (sy < 0 || sy >= static_cast<int32_t>(th)) continue;
                for (uint32_t x = 0; x < target_w; ++x) {
                    const int32_t sx = static_cast<int32_t>(x) - ox;
                    if (sx < 0 || sx >= static_cast<int32_t>(tw)) continue;
                    std::memcpy(out_pbgra.data() + (static_cast<size_t>(y) * target_w + x) * 4u,
                                tile.data() + (static_cast<size_t>(sy) * tw + sx) * 4u, 4u);
                }
            }
        }
    } else {
        // DWPOS_FILL (default) and DWPOS_SPAN: aspect-preserving cover crop.
        ComPtr<IWICBitmapSource> source = frame;
        WICRect crop{0, 0, static_cast<INT>(iw), static_cast<INT>(ih)};
        if (img_aspect > screen_aspect) {
            crop.Width = std::max(1, static_cast<INT>(std::lround(ih * screen_aspect)));
            crop.X = (static_cast<INT>(iw) - crop.Width) / 2;
        } else {
            crop.Height = std::max(1, static_cast<INT>(std::lround(iw / screen_aspect)));
            crop.Y = (static_cast<INT>(ih) - crop.Height) / 2;
        }
        ComPtr<IWICBitmapClipper> clipper;
        if (SUCCEEDED(wic->CreateBitmapClipper(&clipper)) && clipper &&
            SUCCEEDED(clipper->Initialize(frame.Get(), &crop))) {
            source = clipper;
        }
        if (!scale_source_into(source.Get(), target_w, target_h, out_pbgra)) return false;
    }

    for (size_t i = 3; i < out_pbgra.size(); i += 4) {
        out_pbgra[i] = 255u;
    }
    return true;
}

} // namespace

ID2D1Bitmap1* WallpaperBackdrop::GlassSharpBitmap(ID2D1DeviceContext* dc,
                                                  bool allow_sync_decode,
                                                  uint32_t& out_w,
                                                  uint32_t& out_h) {
    if (!dc) dc = owner_dc_.Get();
    if (!dc || screen_w_ == 0 || screen_h_ == 0) {
        out_w = out_h = 0;
        return nullptr;
    }
    DrainCompletedUploads(dc);

    if (glass_sharp_ && glass_sharp_w_ > 0 && glass_sharp_h_ > 0) {
        out_w = glass_sharp_w_;
        out_h = glass_sharp_h_;
        return glass_sharp_.Get();
    }

    // Half-resolution sharp source bounded to 1920px long edge (~8.3 MB VRAM at 4K, 0 persistent CPU bytes).
    uint32_t gw = std::max(1u, (screen_w_ + 1u) / 2u);
    uint32_t gh = std::max(1u, (screen_h_ + 1u) / 2u);
    const uint32_t long_edge = std::max(gw, gh);
    if (long_edge > 1920u) {
        gw = std::max(1u, static_cast<uint32_t>(static_cast<uint64_t>(gw) * 1920u / long_edge));
        gh = std::max(1u, static_cast<uint32_t>(static_cast<uint64_t>(gh) * 1920u / long_edge));
    }

    if (active_preset_ != model::WallpaperPreset::System) {
        // Synthesize vector preset directly on GPU without CPU allocation or disk I/O.
        ComPtr<ID2D1Device> dev;
        ComPtr<ID2D1Factory> fac;
        dc->GetDevice(&dev);
        dc->GetFactory(&fac);
        ComPtr<ID2D1DeviceContext> bake_dc;
        if (dev && fac && SUCCEEDED(dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &bake_dc))) {
            const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            ComPtr<ID2D1Bitmap1> target;
            if (SUCCEEDED(bake_dc->CreateBitmap(D2D1::SizeU(gw, gh), nullptr, 0, &bp, &target)) && target) {
                bake_dc->SetTarget(target.Get());
                bake_dc->SetDpi(96.0f, 96.0f);
                bake_dc->BeginDraw();
                DrawPresetScene(fac.Get(), bake_dc.Get(),
                                D2D1::RectF(0.0f, 0.0f, static_cast<float>(gw), static_cast<float>(gh)),
                                active_preset_, preset_dark_mode_);
                if (SUCCEEDED(bake_dc->EndDraw())) {
                    bake_dc->SetTarget(nullptr);
                    glass_sharp_ = std::move(target);
                    glass_sharp_w_ = gw;
                    glass_sharp_h_ = gh;
                    ++card_material_builds_;
                    out_w = gw;
                    out_h = gh;
                    return glass_sharp_.Get();
                }
                bake_dc->SetTarget(nullptr);
            }
        }
    } else if (!wallpaper_path_.empty() && !glass_sharp_failed_) {
        if (allow_sync_decode) {
            std::vector<uint8_t> pbgra;
            if (IWICImagingFactory* wic = EnsureWicFactory()) {
                if (DecodePositionedWallpaperPbgra(wic, wallpaper_path_, QueryWallpaperPosition(),
                                                   screen_w_, screen_h_, gw, gh, pbgra)) {
                    glass_sharp_ = UploadImmutablePbgraBitmap(dc, pbgra.data(), gw, gh);
                }
            }
            wic_factory_.Reset();
            std::vector<uint8_t>{}.swap(pbgra);
            _heapmin();
            if (glass_sharp_) {
                glass_sharp_w_ = gw;
                glass_sharp_h_ = gh;
                ++card_material_builds_;
                out_w = gw;
                out_h = gh;
                return glass_sharp_.Get();
            }
            glass_sharp_failed_ = true;
        } else if (!glass_sharp_pending_) {
            glass_sharp_pending_ = true;
            std::thread retired;
            {
                std::lock_guard<std::mutex> lk(async_mtx_);
                DecodeJob job{};
                job.key = L"__glass_wallpaper_sharp__";
                job.path = wallpaper_path_;
                job.is_wallpaper_sharp = true;
                job.max_w = gw;
                job.max_h = gh;
                if (pending_keys_.find(job.key) == pending_keys_.end()) {
                    pending_keys_.insert(job.key);
                    decode_queue_.push_front(std::move(job));
                    if (!worker_running_) {
                        if (worker_thread_.joinable()) retired = std::move(worker_thread_);
                        worker_stop_ = false;
                        worker_running_ = true;
                        worker_thread_ = std::thread([this]() { AsyncWorkerLoop(); });
                    }
                    async_cv_.notify_one();
                }
            }
            if (retired.joinable()) retired.join();
        }
    }

    // Immediate non-blocking fallback while async high-res decode completes (or if wallpaper is synthesized).
    out_w = quarter_w_;
    out_h = quarter_h_;
    return QuarterWallpaperBitmap();
}

ID2D1Bitmap1* WallpaperBackdrop::GlassFrostBitmap(ID2D1DeviceContext* dc,
                                                  uint32_t& out_w,
                                                  uint32_t& out_h) const {
    if (!dc) dc = owner_dc_.Get();
    if (!dc || wallpaper_pixels_.empty() || quarter_w_ == 0 || quarter_h_ == 0) {
        out_w = out_h = 0;
        return nullptr;
    }
    if (!glass_frost_) {
        const size_t pixel_count = static_cast<size_t>(quarter_w_) * quarter_h_;
        const size_t byte_count = pixel_count * 4u;
        {
            std::vector<uint8_t> scratch_a(byte_count);
            std::vector<uint8_t> scratch_b(byte_count);
            const int r_frost = std::max(3, static_cast<int>(std::lround(9.0 * quarter_w_ / 960.0)));
            SeparableBoxBlur3Pass(wallpaper_pixels_.data(), scratch_a, scratch_b, quarter_w_, quarter_h_, r_frost);
            BoostSaturationInPlace(scratch_b.data(), pixel_count, 1.18f);
            glass_frost_ = UploadImmutablePbgraBitmap(dc, scratch_b.data(), quarter_w_, quarter_h_);
        }
        _heapmin();
    }
    out_w = quarter_w_;
    out_h = quarter_h_;
    return glass_frost_.Get();
}

void WallpaperBackdrop::ReleaseGlassSource() const noexcept {
    glass_sharp_.Reset();
    glass_frost_.Reset();
    glass_sharp_w_ = 0;
    glass_sharp_h_ = 0;
    glass_sharp_pending_ = false;
}

namespace {
void CreateAcrylicNoiseBrush(ID2D1DeviceContext* dc, ComPtr<ID2D1Bitmap1>& bitmap,
                            ComPtr<ID2D1BitmapBrush1>& brush) {
    constexpr uint32_t kNoiseDim = 128u;
    std::vector<uint8_t> noise(kNoiseDim * kNoiseDim * 4u);
    uint32_t rng = 0x9E3779B9u;
    for (size_t i = 0; i < kNoiseDim * kNoiseDim; ++i) {
        rng = rng * 747796405u + 2891336453u;
        uint32_t word = ((rng >> ((rng >> 28u) + 4u)) ^ rng) * 277803737u;
        word = (word >> 22u) ^ word;
        // Bipolar monochrome snow-grain matching desktop_pack_mockup.html's `noiseURI()` +
        // `mix-blend-mode: overlay; opacity: .07`: half brightening white (a,a,a,a), half
        // darkening black (0,0,0,a), so cards, header bands, and StickyNote show crisp tactile
        // grain in both dark and light themes.
        const int32_t signed_val = static_cast<int32_t>(word & 0xFFu) - 128;
        const uint8_t alpha = static_cast<uint8_t>((std::abs(signed_val) * 8 + 64) >> 7);
        const uint8_t lum = signed_val >= 0 ? alpha : 0u;
        noise[i * 4 + 0] = lum;
        noise[i * 4 + 1] = lum;
        noise[i * 4 + 2] = lum;
        noise[i * 4 + 3] = alpha;
    }
    bitmap = UploadImmutablePbgraBitmap(dc, noise.data(), kNoiseDim, kNoiseDim);
    if (bitmap) {
        D2D1_BITMAP_BRUSH_PROPERTIES1 nprops{};
        nprops.extendModeX = D2D1_EXTEND_MODE_WRAP;
        nprops.extendModeY = D2D1_EXTEND_MODE_WRAP;
        nprops.interpolationMode = D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
        brush.Reset();
        dc->CreateBitmapBrush(bitmap.Get(), &nprops, nullptr, &brush);
    }
}
} // namespace

ID2D1BitmapBrush1* WallpaperBackdrop::EnsureNoiseBrush(ID2D1DeviceContext* dc) {
    if (!dc) return noise_brush_.Get();
    if (!noise_bitmap_ || !noise_brush_) {
        CreateAcrylicNoiseBrush(dc, noise_bitmap_, noise_brush_);
        if (noise_brush_) {
            noise_brush_->SetTransform(D2D1::Matrix3x2F::Scale(1.0f / dip_scale_, 1.0f / dip_scale_));
        }
    }
    return noise_brush_.Get();
}

namespace {

HRESULT BuildDesktopWallpaperMaterial(ID2D1DeviceContext* dc, ID2D1Bitmap1* source,
    uint32_t width, uint32_t height, float blur, model::CardMaterial kind, bool dark, uint8_t opacity,
    ID2D1ImageBrush** output) {
    if (!dc || !source || !output) return E_POINTER;
    *output = nullptr;
    // Bake on a temporary context from the same D2D device: the caller may be
    // inside BeginDraw. Only the finite source-sized bitmap survives this call.
    ComPtr<ID2D1Device> device;
    dc->GetDevice(&device);
    ComPtr<ID2D1DeviceContext> bake_dc;
    HRESULT hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &bake_dc);
    if (FAILED(hr)) return hr;
    ID2D1DeviceContext* output_dc = dc;
    dc = bake_dc.Get();
    // Keep Mica wallpaper-only and opaque, but use a desktop-surface profile:
    // a full luminosity layer was washing the wallpaper into a flat gray slab.
    (void)opacity;
    if (kind != model::CardMaterial::Mica && kind != model::CardMaterial::MicaAlt) return E_INVALIDARG;
    ComPtr<ID2D1Image> image;
    const auto profile = CardMicaProfile(kind == model::CardMaterial::MicaAlt, dark);
    hr = ui::WindowMaterial::BuildMicaImageWithProfile(dc, source, kind == model::CardMaterial::MicaAlt,
        dark, blur, profile.luminosity_opacity, profile.tint_opacity, &image);
    if (FAILED(hr)) return hr;
    ComPtr<ID2D1Bitmap1> baked;
    const auto bitmap_props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    if (FAILED(hr = dc->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, bitmap_props, &baked))) return hr;
    dc->SetTarget(baked.Get());
    dc->SetDpi(96.0f, 96.0f);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0.0f));
    dc->DrawImage(image.Get(), D2D1::Point2F(0, 0),
        D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)));
    hr = dc->EndDraw();
    dc->SetTarget(nullptr);
    if (FAILED(hr)) return hr;
    const auto props = D2D1::ImageBrushProperties(
        D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)),
        D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_INTERPOLATION_MODE_LINEAR);
    return output_dc->CreateImageBrush(baked.Get(), props, output);
}
}

ID2D1ImageBrush* WallpaperBackdrop::SampledMicaBrush(ID2D1DeviceContext* dc, bool alternate, bool dark,
                                                   uint8_t opacity) {
    const unsigned index = (alternate ? 2u : 0u) + (dark ? 1u : 0u);
    auto& brush = sampled_mica_brushes_[index];
    if (brush) return brush.Get();
    if (sampled_mica_failed_[index]) return nullptr;
    const auto fail = [&](HRESULT hr) -> ID2D1ImageBrush* {
        sampled_mica_failed_[index] = true;
        ui::LogWindowMaterial("desktop sampled wallpaper fallback alternate=%d dark=%d hr=0x%08lX",
                              alternate, dark, static_cast<unsigned long>(hr));
        return nullptr;
    };
    ID2D1Bitmap1* quarter_bmp = QuarterWallpaperBitmap();
    if (!dc || !quarter_bmp || !screen_w_) return fail(E_UNEXPECTED);
    // Four bounded source-sized recipe bitmaps avoid rebaking mixed-material
    // cards. The opaque Mica recipe is independent of user alpha.
    const float blur = 80.0f * dip_scale_ * quarter_w_ / screen_w_;
    ++card_material_builds_;
    const HRESULT hr = BuildDesktopWallpaperMaterial(dc, quarter_bmp, quarter_w_, quarter_h_,
        blur, alternate ? model::CardMaterial::MicaAlt : model::CardMaterial::Mica, dark, opacity, brush.GetAddressOf());
    if (FAILED(hr)) return fail(hr);
    ApplyBrushTransforms();
    return brush.Get();
}

void WallpaperBackdrop::ReleaseMenuMaterialCache() const noexcept {
    menu_glass_pool_.Reset();
    menu_underlay_frost_.Reset();
    menu_underlay_bitmap_.Reset();
    std::vector<uint8_t>{}.swap(menu_underlay_pixels_);
    menu_underlay_w_ = 0;
    menu_underlay_h_ = 0;
    menu_underlay_screen_px_ = {};
    menu_frost_bitmap_.Reset();
    menu_noise_brush_.Reset();
    menu_noise_bitmap_.Reset();
    menu_noise_failed_ = false;
    menu_material_brush_.Reset();
    menu_wallpaper_.Reset();
    menu_dc_.Reset();
    menu_failed_ = false;
}

void WallpaperBackdrop::SetMenuUnderlaySnapshot(std::vector<uint8_t> pbgra,
                                                uint32_t width,
                                                uint32_t height,
                                                RECT screen_rect_px) const noexcept {
    menu_underlay_frost_.Reset();
    menu_underlay_bitmap_.Reset();
    if (pbgra.empty() || width == 0 || height == 0 ||
        pbgra.size() < static_cast<size_t>(width) * height * 4u ||
        screen_rect_px.right <= screen_rect_px.left ||
        screen_rect_px.bottom <= screen_rect_px.top) {
        std::vector<uint8_t>{}.swap(menu_underlay_pixels_);
        menu_underlay_w_ = 0;
        menu_underlay_h_ = 0;
        menu_underlay_screen_px_ = {};
        return;
    }
    menu_underlay_pixels_ = std::move(pbgra);
    menu_underlay_w_ = width;
    menu_underlay_h_ = height;
    menu_underlay_screen_px_ = screen_rect_px;
}

ID2D1BitmapBrush1* WallpaperBackdrop::MenuNoiseBrush(ID2D1DeviceContext* dc) const {
    if (!dc) return nullptr;
    if (menu_dc_.Get() != dc) {
        ReleaseMenuMaterialCache();
        menu_dc_ = dc;
    }
    if (menu_noise_brush_ || menu_noise_failed_) return menu_noise_brush_.Get();
    CreateAcrylicNoiseBrush(dc, menu_noise_bitmap_, menu_noise_brush_);
    if (!menu_noise_brush_) {
        menu_noise_failed_ = true;
        ui::LogWindowMaterial("desktop menu acrylic noise creation failed");
    }
    // The menu DC is pixel-based (96 DPI): one grain texel per screen pixel.
    return menu_noise_brush_.Get();
}

bool WallpaperBackdrop::DrawMenuMaterial(ID2D1DeviceContext* dc, const D2D1_RECT_F& card_px,
    POINT popup_screen_px, float radius_px, model::CardMaterial material, bool dark,
    uint8_t opacity, uint8_t dispersion_level,
    const model::BeautifySettings* glass_cfg) const {
    if (!dc || !screen_w_) return false;
    if (material != model::CardMaterial::Mica &&
        material != model::CardMaterial::MicaAlt &&
        material != model::CardMaterial::LiquidGlass) {
        return false;
    }
    if (menu_dc_.Get() != dc) {
        // Preserve freshly staged `menu_underlay_pixels_` across initial menu DC binding.
        auto saved_px = std::move(menu_underlay_pixels_);
        const uint32_t sw = menu_underlay_w_, sh = menu_underlay_h_;
        const RECT sr = menu_underlay_screen_px_;
        ReleaseMenuMaterialCache();
        menu_dc_ = dc;
        if (!saved_px.empty()) {
            SetMenuUnderlaySnapshot(std::move(saved_px), sw, sh, sr);
        }
    }

    if (material == model::CardMaterial::LiquidGlass) {
        ComPtr<ID2D1Factory> base_fac;
        dc->GetFactory(&base_fac);
        ComPtr<ID2D1Factory1> fac1;
        if (!base_fac || FAILED(base_fac.As(&fac1)) || !fac1) return false;

        ID2D1Bitmap1* sharp_bmp = nullptr;
        ID2D1Bitmap1* frost_bmp = nullptr;
        uint32_t sharp_w = 0, sharp_h = 0, frost_w = 0, frost_h = 0;
        uint32_t logical_w = screen_w_, logical_h = screen_h_;
        float orig_x = static_cast<float>(popup_screen_px.x - monitor_screen_origin_.x);
        float orig_y = static_cast<float>(popup_screen_px.y - monitor_screen_origin_.y);

        // Prefer the captured desktop underlay snapshot (refracts BOTH wallpaper AND underlying cards, G10)
        // when the popup card lies within the snapshot's screen bounds (submenus outside fall back to wallpaper).
        const RECT popup_card_scr{
            popup_screen_px.x + static_cast<LONG>(std::floor(card_px.left)),
            popup_screen_px.y + static_cast<LONG>(std::floor(card_px.top)),
            popup_screen_px.x + static_cast<LONG>(std::ceil(card_px.right)),
            popup_screen_px.y + static_cast<LONG>(std::ceil(card_px.bottom)),
        };
        const bool covered_by_underlay =
            menu_underlay_w_ > 0 && menu_underlay_h_ > 0 &&
            popup_card_scr.left >= menu_underlay_screen_px_.left &&
            popup_card_scr.top >= menu_underlay_screen_px_.top &&
            popup_card_scr.right <= menu_underlay_screen_px_.right &&
            popup_card_scr.bottom <= menu_underlay_screen_px_.bottom;

        if (covered_by_underlay) {
            if (!menu_underlay_bitmap_ && !menu_underlay_pixels_.empty()) {
                menu_underlay_bitmap_ = UploadImmutablePbgraBitmap(
                    dc, menu_underlay_pixels_.data(), menu_underlay_w_, menu_underlay_h_);
                if (menu_underlay_bitmap_) {
                    ++menu_uploads_;
                    const size_t pc = static_cast<size_t>(menu_underlay_w_) * menu_underlay_h_;
                    std::vector<uint8_t> sa(pc * 4u), sb(pc * 4u);
                    SeparableBoxBlur3Pass(menu_underlay_pixels_.data(), sa, sb,
                                          menu_underlay_w_, menu_underlay_h_, 8);
                    BoostSaturationInPlace(sb.data(), pc, 1.18f);
                    menu_underlay_frost_ = UploadImmutablePbgraBitmap(
                        dc, sb.data(), menu_underlay_w_, menu_underlay_h_);
                }
            }
            if (menu_underlay_bitmap_) {
                sharp_bmp = menu_underlay_bitmap_.Get();
                sharp_w = menu_underlay_w_;
                sharp_h = menu_underlay_h_;
                frost_bmp = menu_underlay_frost_ ? menu_underlay_frost_.Get() : sharp_bmp;
                frost_w = sharp_w;
                frost_h = sharp_h;
                logical_w = static_cast<uint32_t>(menu_underlay_screen_px_.right - menu_underlay_screen_px_.left);
                logical_h = static_cast<uint32_t>(menu_underlay_screen_px_.bottom - menu_underlay_screen_px_.top);
                orig_x = static_cast<float>(popup_screen_px.x - menu_underlay_screen_px_.left);
                orig_y = static_cast<float>(popup_screen_px.y - menu_underlay_screen_px_.top);
            }
        }

        if (!sharp_bmp) {
            if (wallpaper_pixels_.empty() || quarter_w_ == 0 || quarter_h_ == 0) return false;
            if (!menu_wallpaper_) {
                menu_wallpaper_ = UploadImmutablePbgraBitmap(dc, wallpaper_pixels_.data(), quarter_w_, quarter_h_);
                if (!menu_wallpaper_) return false;
                ++menu_uploads_;
            }
            if (!menu_frost_bitmap_) {
                const size_t pc = static_cast<size_t>(quarter_w_) * quarter_h_;
                std::vector<uint8_t> sa(pc * 4u), sb(pc * 4u);
                const int rf = std::max(3, static_cast<int>(std::lround(9.0 * quarter_w_ / 960.0)));
                SeparableBoxBlur3Pass(wallpaper_pixels_.data(), sa, sb, quarter_w_, quarter_h_, rf);
                BoostSaturationInPlace(sb.data(), pc, 1.18f);
                menu_frost_bitmap_ = UploadImmutablePbgraBitmap(dc, sb.data(), quarter_w_, quarter_h_);
            }
            sharp_bmp = menu_wallpaper_.Get();
            sharp_w = quarter_w_;
            sharp_h = quarter_h_;
            frost_bmp = menu_frost_bitmap_ ? menu_frost_bitmap_.Get() : sharp_bmp;
            frost_w = quarter_w_;
            frost_h = quarter_h_;
        }

        // Menus always keep enough readability frost (>= 26%) even if desktop cards are set to clear 0%.
        GlassCardRequest req{};
        req.left_dip = card_px.left;
        req.top_dip = card_px.top;
        req.right_dip = card_px.right;
        req.bottom_dip = card_px.bottom;
        req.radius_dip = radius_px;
        if (glass_cfg) {
            req.glass_options = glass_cfg->glass_options;
            req.reference_mode = glass_cfg->glass_reference;
            req.blur_radius = glass_cfg->glass_blur_radius;
            req.blur_edge = glass_cfg->glass_blur_edge;
            req.thickness_dip = std::clamp<uint8_t>(glass_cfg->glass_thickness, 1u, 80u);
            req.ior = static_cast<float>(std::clamp<uint16_t>(glass_cfg->glass_ior_x100, 100u, 400u)) * 0.01f;
            req.dispersion_gain = std::min<uint8_t>(glass_cfg->glass_dispersion, 50u);
            req.glare_percent = std::min<uint8_t>(glass_cfg->glass_glare, 120u);
            req.glare_angle_deg = std::clamp<int16_t>(glass_cfg->glass_glare_angle, -180, 180);
            req.frost_percent = std::max<uint8_t>(glass_cfg->glass_frost, 26u);
        } else {
            req.thickness_dip = 22u;
            req.ior = 1.50f;
            req.dispersion_gain = model::MapLegacyGlassDispersionToGain(dispersion_level);
            req.glare_percent = 85u;
            req.glare_angle_deg = -45;
            req.frost_percent = std::max<uint8_t>(opacity / 2u, 26u);
        }
        req.dark_mode = dark;
        req.elevated_hover = true;
        req.allow_clear_passthrough = false;
        req.draw_rim_in_shader = false; // FluentMenu paints its own crisp 1px outer border.

        D2D1_MATRIX_3X2_F cur_xf{};
        dc->GetTransform(&cur_xf);
        const GlassDrawPlan plan = MakeGlassDrawPlan(req, 1.0f, cur_xf.dx, cur_xf.dy);
        menu_glass_pool_.BeginFrame();
        const bool ok = menu_glass_pool_.DrawCard(
            fac1.Get(), dc, sharp_bmp, sharp_w, sharp_h, frost_bmp, frost_w, frost_h,
            logical_w, logical_h, orig_x, orig_y, cur_xf.dx, cur_xf.dy, plan);
        menu_glass_pool_.EndFrame();
        return ok;
    }

    if (wallpaper_pixels_.empty()) return false;
    if (material != menu_material_ || dark != menu_dark_) {
        menu_material_brush_.Reset(); menu_failed_ = false;
        menu_material_ = material; menu_dark_ = dark;
    }
    if (menu_failed_) return false;
    const auto fail = [&](HRESULT hr) {
        menu_failed_ = true;
        ui::LogWindowMaterial("desktop menu wallpaper fallback hr=0x%08lX", static_cast<unsigned long>(hr));
        return false;
    };
    if (!menu_wallpaper_) {
        menu_wallpaper_ = UploadImmutablePbgraBitmap(dc, wallpaper_pixels_.data(), quarter_w_, quarter_h_);
        if (!menu_wallpaper_) return fail(E_FAIL);
        ++menu_uploads_;
    }
    if (!menu_material_brush_) {
        ++menu_material_builds_;
        const float blur = 80.0f * dip_scale_ * quarter_w_ / screen_w_;
        const HRESULT hr = BuildDesktopWallpaperMaterial(dc, menu_wallpaper_.Get(), quarter_w_, quarter_h_,
            blur, material, dark, opacity, menu_material_brush_.GetAddressOf());
        if (FAILED(hr)) return fail(hr);
    }
    // MenuPixelDpiScope supplies a 96-DPI pixel DC, unlike the desktop's DIP DC.
    const auto transform = D2D1::Matrix3x2F::Scale(static_cast<float>(screen_w_) / quarter_w_,
        static_cast<float>(screen_h_) / quarter_h_) * D2D1::Matrix3x2F::Translation(
            static_cast<float>(monitor_screen_origin_.x - popup_screen_px.x),
            static_cast<float>(monitor_screen_origin_.y - popup_screen_px.y));
    menu_material_brush_->SetTransform(transform);
    dc->FillRoundedRectangle(D2D1::RoundedRect(card_px, radius_px, radius_px), menu_material_brush_.Get());
    return true;
}

IWICImagingFactory* WallpaperBackdrop::EnsureWicFactory() {
    if (!wic_factory_) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_factory_));
    }
    return wic_factory_.Get();
}

bool WallpaperBackdrop::LoadForScreen(ID2D1DeviceContext* dc, uint32_t screen_width, uint32_t screen_height,
                                      model::WallpaperPreset preset, bool dark_mode,
                                      const std::wstring& override_wallpaper_path) {
    if (!dc || screen_width == 0 || screen_height == 0) return false;

    uint32_t new_qw = std::max(1u, (screen_width + 3u) / 4u);
    uint32_t new_qh = std::max(1u, (screen_height + 3u) / 4u);
    const uint32_t long_edge = std::max(new_qw, new_qh);
    if (long_edge > 1024u) {
        new_qw = std::max(1u, static_cast<uint32_t>(static_cast<uint64_t>(new_qw) * 1024u / long_edge));
        new_qh = std::max(1u, static_cast<uint32_t>(static_cast<uint64_t>(new_qh) * 1024u / long_edge));
    }

    const std::wstring resolved_sys_path =
        (preset == model::WallpaperPreset::System)
            ? (override_wallpaper_path.empty() ? QueryCurrentWallpaperPath() : override_wallpaper_path)
            : std::wstring{};

    if (owner_dc_.Get() == dc && screen_w_ == screen_width && screen_h_ == screen_height &&
        quarter_w_ == new_qw && quarter_h_ == new_qh && active_preset_ == preset &&
        !wallpaper_pixels_.empty() &&
        (preset == model::WallpaperPreset::System ? (resolved_sys_path == wallpaper_path_)
                                                  : (dark_mode == preset_dark_mode_))) {
        preset_dark_mode_ = dark_mode;
        if (!noise_bitmap_ || !noise_brush_) {
            CreateAcrylicNoiseBrush(dc, noise_bitmap_, noise_brush_);
        }
        ApplyBrushTransforms();
        return noise_brush_ != nullptr;
    }

    owner_dc_ = dc;
    screen_w_ = screen_width;
    screen_h_ = screen_height;
    quarter_w_ = new_qw;
    quarter_h_ = new_qh;
    ReleaseMenuMaterialCache();
    active_preset_ = preset;
    preset_dark_mode_ = dark_mode;

    const size_t pixel_count = static_cast<size_t>(quarter_w_) * quarter_h_;
    const size_t byte_count = pixel_count * 4u;
    std::vector<uint8_t> base_pbgra(byte_count);
    bool have_pixels = false;

    if (preset == model::WallpaperPreset::System) {
        wallpaper_path_ = resolved_sys_path;
        if (!wallpaper_path_.empty()) {
            if (IWICImagingFactory* wic = EnsureWicFactory()) {
                ComPtr<IWICBitmapDecoder> decoder;
                if (SUCCEEDED(wic->CreateDecoderFromFilename(wallpaper_path_.c_str(), nullptr, GENERIC_READ,
                                                             WICDecodeMetadataCacheOnDemand, &decoder)) && decoder) {
                    ComPtr<IWICBitmapFrameDecode> frame;
                    if (SUCCEEDED(decoder->GetFrame(0, &frame)) && frame) {
                        // Fill (Windows' default) and Span cover the screen and crop the overflow centred;
                        // Stretch maps the whole image; the rare Centre/Tile/Fit fall back to Fill.
                        ComPtr<IWICBitmapSource> source = frame;
                        UINT iw = 0, ih = 0;
                        frame->GetSize(&iw, &ih);
                        if (iw && ih && QueryWallpaperPosition() != DWPOS_STRETCH) {
                            const double screen_aspect = static_cast<double>(screen_w_) / screen_h_;
                            WICRect crop{0, 0, static_cast<INT>(iw), static_cast<INT>(ih)};
                            if (static_cast<double>(iw) / ih > screen_aspect) {
                                crop.Width = std::max(1, static_cast<INT>(std::lround(ih * screen_aspect)));
                                crop.X = (static_cast<INT>(iw) - crop.Width) / 2;
                            } else {
                                crop.Height = std::max(1, static_cast<INT>(std::lround(iw / screen_aspect)));
                                crop.Y = (static_cast<INT>(ih) - crop.Height) / 2;
                            }
                            ComPtr<IWICBitmapClipper> clipper;
                            if (SUCCEEDED(wic->CreateBitmapClipper(&clipper)) && clipper &&
                                SUCCEEDED(clipper->Initialize(frame.Get(), &crop))) {
                                source = clipper;
                            }
                        }
                        ComPtr<IWICBitmapScaler> scaler;
                        if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) && scaler &&
                            SUCCEEDED(scaler->Initialize(source.Get(), quarter_w_, quarter_h_,
                                                         WICBitmapInterpolationModeLinear))) {
                            ComPtr<IWICFormatConverter> converter;
                            if (SUCCEEDED(wic->CreateFormatConverter(&converter)) && converter &&
                                SUCCEEDED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                                                                WICBitmapDitherTypeNone, nullptr, 0.0,
                                                                WICBitmapPaletteTypeCustom))) {
                                have_pixels = SUCCEEDED(converter->CopyPixels(
                                    nullptr, quarter_w_ * 4u, static_cast<UINT>(byte_count), base_pbgra.data()));
                            }
                        }
                    }
                }
            }
        }
        if (!have_pixels) {
            SynthesizeFallbackWallpaper(base_pbgra.data(), quarter_w_, quarter_h_);
        }
    } else {
        wallpaper_path_.clear();
        ComPtr<ID2D1Factory> fac;
        dc->GetFactory(&fac);
        if (IWICImagingFactory* wic = EnsureWicFactory(); wic && fac) {
            ComPtr<IWICBitmap> wic_bmp;
            if (SUCCEEDED(wic->CreateBitmap(quarter_w_, quarter_h_, GUID_WICPixelFormat32bppPBGRA,
                                            WICBitmapCacheOnLoad, &wic_bmp)) && wic_bmp) {
                D2D1_RENDER_TARGET_PROPERTIES rt_props = D2D1::RenderTargetProperties(
                    D2D1_RENDER_TARGET_TYPE_DEFAULT,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                    96.0f, 96.0f);
                ComPtr<ID2D1RenderTarget> wic_rt;
                if (SUCCEEDED(fac->CreateWicBitmapRenderTarget(wic_bmp.Get(), rt_props, &wic_rt)) && wic_rt) {
                    wic_rt->BeginDraw();
                    DrawPresetScene(fac.Get(), wic_rt.Get(),
                                    D2D1::RectF(0.0f, 0.0f, static_cast<float>(quarter_w_), static_cast<float>(quarter_h_)),
                                    preset, dark_mode);
                    if (SUCCEEDED(wic_rt->EndDraw())) {
                        have_pixels = SUCCEEDED(wic_bmp->CopyPixels(
                            nullptr, quarter_w_ * 4u, static_cast<UINT>(byte_count), base_pbgra.data()));
                    }
                }
            }
        }
        if (!have_pixels) {
            SynthesizePresetFallbackCpu(base_pbgra.data(), quarter_w_, quarter_h_, preset, dark_mode);
        }
    }

    // Release WIC codec/decoder heap immediately after wallpaper sampling completes.
    wic_factory_.Reset();

    for (size_t i = 0; i < pixel_count; ++i) {
        base_pbgra[i * 4 + 3] = 255u;
    }

    if (preset == model::WallpaperPreset::System) {
        vibrant_accent_argb_ = ExtractVibrantAccentArgb(base_pbgra.data(), quarter_w_, quarter_h_, quarter_w_ * 4u);
    } else {
        vibrant_accent_argb_ = PresetAccentArgb(preset, dark_mode);
    }

    // Retain the single bounded 1/4-res CPU snapshot; GPU wallpaper/Acrylic/Mica bitmaps
    // materialize lazily only if non-HostBackdrop surfaces actually request them.
    wallpaper_pixels_ = std::move(base_pbgra);
    wallpaper_quarter_.Reset();
    acrylic_blur_.Reset();
    mica_blur_.Reset();
    ReleaseGlassSource();
    glass_sharp_failed_ = false;
    acrylic_brush_.Reset();
    mica_brush_.Reset();
    for (auto& brush : sampled_mica_brushes_) brush.Reset();
    for (auto& failed : sampled_mica_failed_) failed = false;

    if (!noise_bitmap_ || !noise_brush_)
        CreateAcrylicNoiseBrush(dc, noise_bitmap_, noise_brush_);
    ApplyBrushTransforms();

    return noise_brush_ != nullptr;
}

void WallpaperBackdrop::ReleaseDeviceResources() noexcept {
    ReleaseMenuMaterialCache();
    ReleaseGlassSource();
    glass_sharp_failed_ = false;
    std::vector<uint8_t>{}.swap(wallpaper_pixels_);
    ClearThumbnailCache();
    noise_brush_.Reset();
    mica_brush_.Reset();
    for (auto& brush : sampled_mica_brushes_) brush.Reset();
    for (auto& failed : sampled_mica_failed_) failed = false;
    acrylic_brush_.Reset();
    noise_bitmap_.Reset();
    mica_blur_.Reset();
    acrylic_blur_.Reset();
    wallpaper_quarter_.Reset();
    owner_dc_.Reset();
    wic_factory_.Reset();
}

void WallpaperBackdrop::ShutdownAsyncWorker() noexcept {
    std::thread to_join;
    {
        std::lock_guard<std::mutex> lk(async_mtx_);
        worker_stop_ = true;
        decode_queue_.clear();
        completed_uploads_.clear();
        pending_keys_.clear();
        async_cv_.notify_all();
        if (worker_thread_.joinable()) {
            to_join = std::move(worker_thread_);
        }
        worker_running_ = false;
    }
    if (to_join.joinable()) {
        to_join.join();
    }
}

void WallpaperBackdrop::SetAsyncReadyCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lk(async_mtx_);
    on_async_ready_ = std::move(cb);
}

void WallpaperBackdrop::EnqueueAsyncDecode(DecodeJob job) {
    if (vector_icons_only_ || job.key.empty() || job.path.empty()) return;
    if (failed_keys_.find(job.key) != failed_keys_.end()) return;

    std::thread retired_worker;
    {
        std::lock_guard<std::mutex> lk(async_mtx_);
        if (pending_keys_.find(job.key) != pending_keys_.end()) return;
        if (decode_queue_.size() >= 48u) return;
        pending_keys_.insert(job.key);
        decode_queue_.push_back(std::move(job));
        if (!worker_running_) {
            if (worker_thread_.joinable()) {
                retired_worker = std::move(worker_thread_);
            }
            worker_stop_ = false;
            worker_running_ = true;
            worker_thread_ = std::thread([this]() { AsyncWorkerLoop(); });
        }
        async_cv_.notify_one();
    }
    if (retired_worker.joinable()) {
        retired_worker.join();
    }
}

void WallpaperBackdrop::AsyncWorkerLoop() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ComPtr<IWICImagingFactory> thread_wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&thread_wic));

    for (;;) {
        DecodeJob job;
        {
            std::unique_lock<std::mutex> lk(async_mtx_);
            if (!async_cv_.wait_for(lk, std::chrono::seconds(4),
                                    [&]() { return worker_stop_ || !decode_queue_.empty(); })) {
                worker_running_ = false;
                break;
            }
            if (worker_stop_) {
                worker_running_ = false;
                break;
            }
            job = std::move(decode_queue_.front());
            decode_queue_.pop_front();
        }

        DecodedUpload upload{};
        upload.key = job.key;
        bool ok = false;
        if (job.is_wallpaper_sharp) {
            ok = DecodePositionedWallpaperPbgra(
                thread_wic.Get(), job.path, QueryWallpaperPosition(),
                screen_w_ > 0 ? screen_w_ : job.max_w * 2u,
                screen_h_ > 0 ? screen_h_ : job.max_h * 2u,
                job.max_w, job.max_h, upload.pbgra);
            if (ok) {
                upload.width = job.max_w;
                upload.height = job.max_h;
            }
        } else if (job.is_photo) {
            ok = DecodeWicFileToPbgra(thread_wic.Get(), job.path, job.max_w, job.max_h,
                                      upload.width, upload.height, upload.pbgra);
        } else {
            const std::wstring ext = job.is_folder ? std::wstring{} : LowerExtension(job.path);
            if (!job.is_folder && IsRasterImageExtension(ext)) {
                ok = DecodeWicFileToPbgra(thread_wic.Get(), job.path, job.max_w, job.max_h,
                                          upload.width, upload.height, upload.pbgra);
            }
            if (!ok) {
                ok = ExtractShellIconToPbgra(thread_wic.Get(), job.path, job.is_folder,
                                             job.shared_type_icon, job.max_w,
                                             upload.width, upload.height, upload.pbgra);
            }
        }
        upload.failed = !ok;

        std::function<void()> notify_cb;
        {
            std::lock_guard<std::mutex> lk(async_mtx_);
            if (worker_stop_) {
                worker_running_ = false;
                break;
            }
            if (job.is_wallpaper_sharp || completed_uploads_.size() < 24u) {
                completed_uploads_.push_back(std::move(upload));
                notify_cb = on_async_ready_;
            } else {
                pending_keys_.erase(job.key);
            }
        }
        if (notify_cb) {
            notify_cb();
        }
    }

    thread_wic.Reset();
    CoUninitialize();
}

void WallpaperBackdrop::DrainCompletedUploads(ID2D1DeviceContext* dc) {
    if (!dc) return;
    std::deque<DecodedUpload> batch;
    {
        std::lock_guard<std::mutex> lk(async_mtx_);
        if (completed_uploads_.empty()) return;
        batch.swap(completed_uploads_);
        for (const auto& u : batch) {
            pending_keys_.erase(u.key);
        }
    }

    for (auto& u : batch) {
        if (u.key == L"__glass_wallpaper_sharp__") {
            glass_sharp_pending_ = false;
            if (!u.failed && !u.pbgra.empty() && u.width > 0 && u.height > 0) {
                glass_sharp_ = UploadImmutablePbgraBitmap(dc, u.pbgra.data(), u.width, u.height);
                if (glass_sharp_) {
                    glass_sharp_w_ = u.width;
                    glass_sharp_h_ = u.height;
                    ++card_material_builds_;
                } else {
                    glass_sharp_failed_ = true;
                }
            } else {
                glass_sharp_failed_ = true;
            }
            std::vector<uint8_t>{}.swap(u.pbgra);
            _heapmin();
            continue;
        }
        if (u.failed || u.pbgra.empty() || u.width == 0 || u.height == 0) {
            failed_keys_.insert(u.key);
            continue;
        }
        const size_t bytes = u.pbgra.size();
        auto bmp = UploadImmutablePbgraBitmap(dc, u.pbgra.data(), u.width, u.height);
        if (bmp) {
            std::unique_ptr<text_contrast::PhotoGrid> photo;
            if (u.key.starts_with(L"photo:")) {
                photo = std::make_unique<text_contrast::PhotoGrid>(
                    text_contrast::MakePhotoGrid(u.pbgra, u.width, u.height));
            }
            InsertIntoLru(u.key, std::move(bmp), bytes, std::move(photo));
        } else {
            failed_keys_.insert(u.key);
        }
    }
}

void WallpaperBackdrop::EvictToBudget(size_t incoming_bytes) noexcept {
    while (!lru_list_.empty() && (cached_bytes_ + incoming_bytes > kMaxCacheBytes)) {
        auto& victim = lru_list_.back();
        cached_bytes_ = (cached_bytes_ >= victim.bytes) ? (cached_bytes_ - victim.bytes) : 0;
        cache_map_.erase(victim.key);
        lru_list_.pop_back();
    }
}

ID2D1Bitmap1* WallpaperBackdrop::InsertIntoLru(const std::wstring& key, ComPtr<ID2D1Bitmap1> bmp, size_t byte_cost,
    std::unique_ptr<text_contrast::PhotoGrid> photo) {
    if (photo) byte_cost += sizeof(text_contrast::PhotoGrid);
    if (!bmp || byte_cost == 0 || byte_cost > kMaxCacheBytes) return nullptr;

    auto existing = cache_map_.find(key);
    if (existing != cache_map_.end()) {
        cached_bytes_ -= existing->second->bytes;
        lru_list_.erase(existing->second);
        cache_map_.erase(existing);
    }

    EvictToBudget(byte_cost);
    lru_list_.push_front(CacheNode{key, std::move(bmp), byte_cost, std::move(photo)});
    cache_map_[key] = lru_list_.begin();
    cached_bytes_ += byte_cost;
    return lru_list_.front().bitmap.Get();
}

ID2D1Bitmap1* WallpaperBackdrop::GetItemBitmap(ID2D1DeviceContext* dc, const std::wstring& path,
                                               bool is_folder, uint32_t target_px, std::wstring_view key_salt) {
    if (!dc || path.empty() || vector_icons_only_) return nullptr;
    DrainCompletedUploads(dc);

    target_px = std::clamp(target_px, 16u, 256u);
    const std::wstring ext = is_folder ? std::wstring{} : LowerExtension(path);
    const bool is_image = !is_folder && IsRasterImageExtension(ext);
    const std::wstring size_tag = L":" + std::to_wstring(target_px) + std::wstring(key_salt);
    const bool shell_namespace = path.starts_with(L"::{");
    const DWORD attrs = shell_namespace ? FILE_ATTRIBUTE_DIRECTORY : GetFileAttributesW(path.c_str());
    if (!shell_namespace && attrs == INVALID_FILE_ATTRIBUTES) {
        return nullptr;
    }

    // Standard folders without desktop.ini customization (READONLY | SYSTEM) share one crisp folder bitmap
    // instead of duplicating identical 256x256 bitmaps for dozens of folders on a busy desktop.
    const bool custom_folder = is_folder &&
        (shell_namespace || (attrs & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM)) != 0);
    const bool is_unique_icon = custom_folder || (!is_folder && (ext.empty() ||
                                ext == L".exe" || ext == L".lnk" || ext == L".ico" || ext == L".url"));
    const bool shared_type_icon = !is_image && !is_unique_icon;

    const std::wstring cache_key = is_image ? (L"thm:" + path + size_tag)
                                 : is_unique_icon ? (L"ico:" + path + size_tag)
                                 : is_folder ? (L"dir:std" + size_tag)
                                             : (L"ext:" + ext + size_tag);

    auto it = cache_map_.find(cache_key);
    if (it != cache_map_.end()) {
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return it->second->bitmap.Get();
    }

    EnqueueAsyncDecode(DecodeJob{cache_key, path, is_folder, false, shared_type_icon, target_px, target_px});
    return nullptr;
}

ID2D1Bitmap1* WallpaperBackdrop::GetPhotoTileBitmap(ID2D1DeviceContext* dc, const std::wstring& image_path,
                                                    uint32_t max_w, uint32_t max_h) {
    if (!dc || image_path.empty() || vector_icons_only_) return nullptr;
    DrainCompletedUploads(dc);

    max_w = std::min(max_w, 424u);
    max_h = std::min(max_h, 208u);
    const std::wstring key = L"photo:" + image_path + L":" + std::to_wstring(max_w) + L"x" + std::to_wstring(max_h);

    auto it = cache_map_.find(key);
    if (it != cache_map_.end()) {
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return it->second->bitmap.Get();
    }

    if (GetFileAttributesW(image_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return nullptr;
    }

    EnqueueAsyncDecode(DecodeJob{key, image_path, false, true, false, max_w, max_h});
    return nullptr;
}

text_contrast::PhotoSamples WallpaperBackdrop::SamplePhotoColors(const std::wstring& image_path,
    const D2D1_RECT_F& tile_rect, const D2D1_RECT_F& text_rect, uint32_t max_w, uint32_t max_h) const {
    if (image_path.empty() || vector_icons_only_) return {};
    max_w = std::min(max_w, 424u);
    max_h = std::min(max_h, 208u);
    const std::wstring key = L"photo:" + image_path + L":" + std::to_wstring(max_w) + L"x" + std::to_wstring(max_h);
    const auto it = cache_map_.find(key);
    if (it == cache_map_.end() || !it->second->photo) return {};
    return text_contrast::SamplePhotoGrid(*it->second->photo,
        {tile_rect.left, tile_rect.top, tile_rect.right, tile_rect.bottom},
        {text_rect.left, text_rect.top, text_rect.right, text_rect.bottom});
}

ID2D1Bitmap1* WallpaperBackdrop::CacheRgbaBitmap(ID2D1DeviceContext* dc, const std::wstring& cache_key,
                                                 const uint8_t* pbgra_pixels, uint32_t width, uint32_t height) {
    if (!dc || cache_key.empty() || !pbgra_pixels || width == 0 || height == 0) return nullptr;
    const size_t bytes = static_cast<size_t>(width) * height * 4u;
    if (bytes > kMaxCacheBytes) return nullptr;
    auto bmp = UploadImmutablePbgraBitmap(dc, pbgra_pixels, width, height);
    if (!bmp) return nullptr;
    return InsertIntoLru(cache_key, std::move(bmp), bytes);
}

void WallpaperBackdrop::ClearThumbnailCache() noexcept {
    {
        std::lock_guard<std::mutex> lk(async_mtx_);
        decode_queue_.clear();
        completed_uploads_.clear();
        pending_keys_.clear();
    }
    failed_keys_.clear();
    cache_map_.clear();
    lru_list_.clear();
    cached_bytes_ = 0;
}

} // namespace pulse::desktop::render

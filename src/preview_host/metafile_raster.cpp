// metafile_raster.cpp — GDI+ rendering of WMF/EMF previews.
//
// Metafiles sit outside the other two preview paths: WIC only decodes them when
// its metafile codec is installed (absent on some machines, which surfaced as an
// "image-decode-failed" preview), and the shell exposes no thumbnail provider
// for them.
//
// They are played through GDI+ rather than plain GDI: Office clip art is usually
// EMF+ stored inside an EMF container, and PlayEnhMetaFile silently skips the
// EMF+ records, which produced a blank page. GDI+ is the one renderer that
// covers plain WMF, plain EMF and EMF+ alike.
#include "metafile_raster.h"

#include "../common/path_utils.h"
#include <objbase.h>

// gdiplus.h expects min/max as callable names, while windows.h brought the
// macros in; std::max stays available through the parenthesised form below.
#undef min
#undef max
#include <algorithm>
using std::max;
using std::min;
#include <gdiplus.h>

#include <cmath>
#include <cstring>

namespace pulse::preview {
namespace {

// Refuse absurd declared extents instead of allocating for them.
constexpr UINT kMaxDeclaredEdge = 32768;

// Started once per process, on first use: the host only needs GDI+ when a
// metafile is actually opened. The token is intentionally never shut down.
bool EnsureGdiplus() {
    static const bool ready = [] {
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    }();
    return ready;
}

} // namespace

bool RasterizeMetaFile(const std::wstring& path, UINT max_edge,
                       std::vector<unsigned char>& pixels,
                       UINT& width, UINT& height, UINT& stride,
                       UINT& source_width, UINT& source_height,
                       std::wstring* error) {
    const auto fail = [error](const wchar_t* text) {
        if (error) *error = text;
        return false;
    };
    pixels.clear();
    if (max_edge == 0) max_edge = 1024;
    if (!EnsureGdiplus()) return fail(L"metafile-no-gdiplus");

    // GDI+ rejects \\?\ paths, which the fs layer hands out for long paths.
    const std::wstring shell_path = path::StripExtendedPathPrefix(path);
    Gdiplus::Bitmap source(shell_path.c_str(), FALSE);
    if (source.GetLastStatus() != Gdiplus::Ok) return fail(L"metafile-load-failed");

    const UINT declared_w = source.GetWidth();
    const UINT declared_h = source.GetHeight();
    if (declared_w == 0 || declared_h == 0 ||
        declared_w > kMaxDeclaredEdge || declared_h > kMaxDeclaredEdge)
        return fail(L"metafile-extent-missing");

    const UINT longest = (std::max)(declared_w, declared_h);
    const double scale = longest > max_edge
        ? static_cast<double>(max_edge) / longest : 1.0;
    const UINT out_width = (std::max)(1u, static_cast<UINT>(declared_w * scale + 0.5));
    const UINT out_height = (std::max)(1u, static_cast<UINT>(declared_h * scale + 0.5));

    // PARGB is what the UI expects: BGRA with premultiplied alpha.
    Gdiplus::Bitmap target(out_width, out_height, PixelFormat32bppPARGB);
    if (target.GetLastStatus() != Gdiplus::Ok) return fail(L"metafile-surface-failed");

    {
        Gdiplus::Graphics graphics(&target);
        if (graphics.GetLastStatus() != Gdiplus::Ok) return fail(L"metafile-surface-failed");
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        // Metafiles assume a page, and Explorer shows them on white.
        graphics.Clear(Gdiplus::Color(255, 255, 255, 255));
        if (graphics.DrawImage(&source, Gdiplus::Rect(0, 0,
                static_cast<INT>(out_width), static_cast<INT>(out_height))) != Gdiplus::Ok)
            return fail(L"metafile-render-failed");
    }

    Gdiplus::BitmapData data{};
    const Gdiplus::Rect area(0, 0, static_cast<INT>(out_width), static_cast<INT>(out_height));
    if (target.LockBits(&area, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) !=
        Gdiplus::Ok)
        return fail(L"metafile-lock-failed");

    stride = out_width * 4;
    pixels.resize(static_cast<size_t>(stride) * out_height);
    const auto* base = static_cast<const unsigned char*>(data.Scan0);
    // A negative stride means bottom-up rows, which some drivers hand out.
    for (UINT row = 0; row < out_height; ++row) {
        const auto* line = data.Stride >= 0
            ? base + static_cast<size_t>(row) * data.Stride
            : base + static_cast<size_t>(out_height - 1 - row) * (-data.Stride);
        std::memcpy(pixels.data() + static_cast<size_t>(row) * stride, line, stride);
    }
    target.UnlockBits(&data);

    width = out_width;
    height = out_height;
    source_width = declared_w;
    source_height = declared_h;
    return true;
}

} // namespace pulse::preview

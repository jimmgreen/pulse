#pragma once

#include "preview_router.h"
#include <utility>

namespace pulse::preview {

// Runs the SVG preview fallback chain:
//   1. Direct2D's built-in SVG subset (`rasterize_svg`).
//   2. System shell thumbnail cache / registered IThumbnailProvider (`shell_thumbnail`),
//      skipped for offline placeholders or unreadable files. When a provider
//      returns a bitmap, clears the Direct2D subset warning and marks
//      `result.decoder = "shell-thumbnail"` so `DescribeIntegrity` reports
//      `Unknown / ProviderDefined` instead of `Partial / SourceFallback`
//      (allowing persistent `.pt` and micro-thumb caching).
//   3. Source text (`text_or_hex`) for Quick Look / Details / non-grid requests,
//      preserving `r.error` (`svg-unsupported:...`, `svg-budget-exceeded`, etc.);
//      explicit grid tile requests (`kPreviewRequestFlagGrid`) fail cleanly
//      without reading or shipping 32 KB of XML text.
template <typename RasterizeFn, typename ShellFn, typename TextFn>
DecodeStep ResolveSvgDecode(const DecodeRequest& q, DecodeResult& r,
                            RasterizeFn&& rasterize_svg,
                            ShellFn&& shell_thumbnail,
                            TextFn&& text_or_hex) {
    if (q.offline) return DecodeStep::Failed;
    if (rasterize_svg(q, r)) {
        r.kind = ipc::PreviewContentKind::Bitmap;
        return DecodeStep::Made;
    }
    const std::wstring svg_error = std::move(r.error);
    r.pixels.clear();
    r.width = r.height = r.stride = r.source_width = r.source_height = 0;
    if (svg_error != L"svg-unreadable" && shell_thumbnail(q, r)) {
        r.error.clear();
        r.decoder = "shell-thumbnail";
        r.kind = ipc::PreviewContentKind::Bitmap;
        return DecodeStep::Made;
    }
    r.pixels.clear();
    r.width = r.height = r.stride = r.source_width = r.source_height = 0;
    r.error = svg_error;
    if ((q.request.flags & ipc::kPreviewRequestFlagGrid) != 0) {
        if (r.error.empty()) r.error = L"svg-render-failed";
        return DecodeStep::Failed;
    }
    if (text_or_hex(q, r)) return DecodeStep::Made;
    r.error = L"svg-render-failed";
    return DecodeStep::Failed;
}

} // namespace pulse::preview

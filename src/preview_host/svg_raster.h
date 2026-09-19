// svg_raster.h — Direct2D rasterization of SVG documents for the preview host.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace pulse::preview {

// Rasterizes an SVG file into 32bpp premultiplied BGRA pixels — the same format
// the WIC path produces, so the UI side needs no special handling. max_edge caps
// the longest edge of the result while keeping the document's aspect ratio;
// source_width/source_height report its intrinsic size. Returns false and fills
// *error when the file is unreadable, larger than the size budget, not an SVG
// document, or when no Direct2D device is available in this process.
bool RasterizeSvgFile(const std::wstring& path, UINT max_edge,
                      std::vector<unsigned char>& pixels,
                      UINT& width, UINT& height, UINT& stride,
                      UINT& source_width, UINT& source_height,
                      std::wstring* error);

} // namespace pulse::preview

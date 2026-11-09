// psd_raster.h — Photoshop PSD / PSB previews for the preview host.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace pulse::preview {

// Decodes a PSD/PSB into 32bpp premultiplied BGRA, longest edge <= max_edge.
//
// The full-size source is the merged composite Photoshop stores after the
// layers (RLE or raw; 1/8/16/32-bit; RGB, grayscale, CMYK, indexed, duotone,
// Lab), area-averaged down while it streams so memory stays bounded. When the
// file was saved without "Maximize compatibility" (no real merged data), or
// the composite cannot be read in time, the JPEG thumbnail Photoshop embeds
// in the image resources is used instead. prefer_embedded asks for that
// thumbnail first — it sits at the start of the file and is cheap, which suits
// grid thumbnails. source_width/source_height report the document size.
bool RasterizePsdFile(const std::wstring& path, UINT max_edge, bool prefer_embedded,
                      std::vector<unsigned char>& pixels, UINT& width, UINT& height,
                      UINT& stride, UINT& source_width, UINT& source_height,
                      std::wstring* error, bool* embedded = nullptr);

} // namespace pulse::preview

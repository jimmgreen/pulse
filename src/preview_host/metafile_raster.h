// metafile_raster.h — GDI rendering of Windows metafile (WMF/EMF) previews.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace pulse::preview {

// Renders a Windows metafile into 32bpp BGRA pixels. WIC only decodes metafiles
// when its codec happens to be installed, and the shell exposes no thumbnail
// provider for them, so the preview host plays them through GDI — their native
// renderer. max_edge caps the longest edge of the result; source_* report the
// size the document declares. Returns false and fills *error when the file
// cannot be loaded or has no usable extent.
bool RasterizeMetaFile(const std::wstring& path, UINT max_edge,
                       std::vector<unsigned char>& pixels,
                       UINT& width, UINT& height, UINT& stride,
                       UINT& source_width, UINT& source_height,
                       std::wstring* error);

} // namespace pulse::preview

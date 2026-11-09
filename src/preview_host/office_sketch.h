// office_sketch.h - text sketch thumbnails for Word / Excel files.
#pragma once

#include <windows.h>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::preview {

// Below this edge the shell icon is clearer than any sketch.
constexpr UINT kOfficeSketchMinEdge = 64;
// At or above this edge Word files get the page layout sketch; below it the
// readable summary card.
constexpr UINT kOfficeSketchPageEdge = 200;

// Formats the sketch can read, all by content rather than extension:
// Word (.docx/.docm/.doc/.rtf and WPS .wps), Excel (.xlsx/.xlsm/.xls and WPS
// .et), PowerPoint
// (.pptx/.pptm/.ppsx/.ppt/.pps and WPS .dps) and OpenDocument (.odt/.ods/
// .odp/.odg, through the thumbnail every ODF writer embeds).
bool IsOfficeSketchExtension(std::wstring_view extension);

// Formats with no Windows thumbnail or preview handler of their own (WPS,
// OpenDocument): the sketch serves the details pane and Quick Look too, not
// only grid thumbnails.
bool IsSketchOnlyExtension(std::wstring_view extension);

// Draws a sketch of the file's first page into 32bpp premultiplied BGRA:
//  - Word, max_edge >= kOfficeSketchPageEdge: the first page at its real paper
//    size, margins and columns; text too small to read becomes grey bars.
//  - Word, smaller: a 3:4 card with the title and the first lines, readable.
//  - Excel: the first sheet's top-left used range with column letters and row
//    numbers.
//  - PowerPoint: the first slide at its aspect ratio - title and text boxes
//    where the slide places them (layout defaults when it does not), text too
//    small to read as grey bars, pictures / charts / tables as grey boxes.
//  - OpenDocument: the embedded thumbnail, scaled.
// Application badges are drawn separately by the UI, without covering the document.
// For files Windows has no thumbnail for; it is not a real
// rendering of the document. False (with *error) for max_edge below
// kOfficeSketchMinEdge or files that are not readable packages.
bool RenderOfficeSketch(const std::wstring& path, std::wstring_view extension, UINT max_edge,
                        std::vector<unsigned char>& pixels, UINT& width, UINT& height,
                        UINT& stride, std::wstring* error);

// True when a 32bpp BGRA thumbnail carries no picture at all: every pixel is
// near-white or fully transparent. Some writers (Mac Word) embed such blank
// thumbnails; for Office files the sketch is then shown instead.
bool IsBlankThumbnail(const std::vector<unsigned char>& pixels, UINT width, UINT height, UINT stride);

} // namespace pulse::preview
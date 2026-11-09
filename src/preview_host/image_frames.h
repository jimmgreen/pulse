// image_frames.h — container parsing for the Quick Look image extras: APNG
// frames (acTL / fcTL / fdAT, which WIC's PNG decoder ignores) and the entry
// directory of .ico / .cur files. Pure byte parsing; decoding and compositing
// stay with WIC in preview_decoders.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::preview {

struct ApngFrame {
    uint32_t x = 0, y = 0, width = 0, height = 0;
    uint32_t delay_ms = 100;
    uint8_t dispose = 0;  // 0 none, 1 clear to transparent, 2 restore previous
    uint8_t blend = 0;    // 0 replace, 1 alpha over
    std::vector<std::pair<size_t, size_t>> data;  // zlib pieces (offset, length) in the file
};

struct ApngInfo {
    uint32_t width = 0, height = 0;
    uint32_t plays = 0;  // 0 loops forever
    uint32_t declared_frames = 0;
    uint32_t observed_frames = 0;
    std::wstring incomplete_reason;
    size_t ihdr = 0;     // offset of the IHDR data (13 bytes)
    std::vector<std::pair<size_t, size_t>> header_chunks;  // whole chunks copied into every frame
    std::vector<ApngFrame> frames;
};

// True for an animated PNG with at least two usable frames.
bool ParseApng(const std::vector<uint8_t>& file, ApngInfo& info);
std::wstring MakeImageFramesPayload(std::wstring_view type, uint32_t loaded, uint32_t declared,
                                   uint32_t selected, std::wstring_view reason = {});
// A standalone PNG holding one frame (its own size, the shared palette and
// colour chunks), for WIC to decode.
std::vector<uint8_t> BuildApngFramePng(const std::vector<uint8_t>& file, const ApngInfo& info, size_t index);

struct IconEntry {
    uint32_t width = 0, height = 0;  // real size (PNG entries read their IHDR)
    uint32_t bits = 0;               // bits per pixel, 0 when unknown
    bool png = false;
    uint32_t frame = 0;              // file directory position, not a WIC frame index
    uint32_t offset = 0, size = 0;
};

// .ico (type 1) and .cur (type 2) directories. Entries keep file order, which
// need not match a decoder's frame order. Returns false for anything else.
bool ParseIconDirectory(const std::vector<uint8_t>& file, std::vector<IconEntry>& entries, bool& cursor);
// Index of the entry shown by default: the largest, then the deepest colour.
size_t DefaultIconEntry(const std::vector<IconEntry>& entries);
// Give WIC exactly one selected entry so codec-specific frame ordering cannot
// substitute a different resolution. Cursor hotspots are not icon bit depths.
bool MakeIconEntryFile(const std::vector<uint8_t>& file, const IconEntry& entry,
                      bool cursor, std::vector<uint8_t>& out);

// Payload sent beside the pixels so the window can offer the sizes:
//   PULSEICO\t1\n
//   S\t<selected index>\n
//   E\t<width>\t<height>\t<bits>\t<png 0|1>\n   (one per entry, file order)
std::wstring MakeIconSizesPayload(const std::vector<IconEntry>& entries, size_t selected);

} // namespace pulse::preview

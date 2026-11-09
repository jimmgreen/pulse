#pragma once

// Ordered decoder table for Content requests in the preview host.
//
// Each DecoderEntry is one format family. The router walks the table in order;
// the first entry whose accepts() matches runs:
//   Made   - it produced the preview (bitmap, text, hex or archive listing);
//   Failed - it owns the file but could not preview it (error text is set);
//   Next   - it could not read the file natively and lets later entries try,
//            e.g. a damaged archive still gets the shell thumbnail / hex view.
// The usual fallback chain is therefore
//   native decoder -> embedded thumbnail -> shell provider -> text / hex.

#include "../ipc/preview_protocol.h"
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::preview {

// Requests at or below this edge, or flagged kPreviewRequestFlagGrid, are
// list/grid thumbnails; the rest are the details pane and Quick Look.
inline constexpr UINT kGridThumbnailEdge = 256;

// The request plus the checks every decoder needs, computed once.
struct DecodeRequest {
    DecodeRequest(const ipc::PreviewRequest& request_in, const std::wstring& path_in,
                  const std::wstring& extension_in, bool is_directory_in, bool offline_in,
                  UINT cap_in, bool grid_in)
        : request(request_in), path(path_in), extension(extension_in),
          is_directory(is_directory_in), offline(offline_in), cap(cap_in), grid(grid_in) {}

    const ipc::PreviewRequest& request;
    const std::wstring& path;
    const std::wstring& extension;  // lower case, with the dot; empty if none
    bool is_directory;
    bool offline;                   // cloud placeholder: never read its data
    UINT cap;                       // ClampPreviewPixelSize(pixel_size, false)
    bool grid;                      // list/grid thumbnail: kPreviewRequestFlagGrid or cap <= kGridThumbnailEdge
};

// Everything a decoder writes. Defaults match a zeroed PreviewResponse.
struct DecodeResult {
    bool embedded_psd = false;
    ipc::PreviewContentKind kind = ipc::PreviewContentKind::None;
    std::vector<uint8_t> pixels;
    UINT width = 0, height = 0, stride = 0;
    UINT source_width = 0, source_height = 0;
    std::wstring text;
    std::wstring error;
    uint32_t bytes_read = 0;
    bool truncated = false;
    uint32_t frame_count = 1;
    uint32_t frame_delay_ms = 0;
    uint32_t loop_count = 0;
    uint32_t duration_ms = 0;       // videos: playing time when the decoder learned it (media pack)
    ipc::PreviewTextEncoding text_encoding = ipc::PreviewTextEncoding::Unknown;  // Text only
    const char* decoder = nullptr;  // name of the entry that finished the request
};

enum class DecodeStep { Made, Failed, Next };

struct DecoderEntry {
    const char* name;
    bool (*accepts)(const DecodeRequest&);
    DecodeStep (*run)(const DecodeRequest&, DecodeResult&);
};

// Returns true when an entry made the preview. Entries that return Next keep
// whatever they wrote in `result`, as the if/else chain this replaced did.
template <size_t N>
bool RunDecoders(const DecoderEntry (&table)[N], const DecodeRequest& request,
                 DecodeResult& result) {
    for (const DecoderEntry& entry : table) {
        if (!entry.accepts(request)) continue;
        const DecodeStep step = entry.run(request, result);
        if (step == DecodeStep::Next) continue;
        result.decoder = entry.name;
        return step == DecodeStep::Made;
    }
    return false;
}

} // namespace pulse::preview
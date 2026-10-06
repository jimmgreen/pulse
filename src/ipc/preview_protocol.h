#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include "../common/preview_integrity.h"

namespace pulse::ipc {
constexpr uint32_t kPreviewMagic = 0x57565250; // PRVW
enum class PreviewContentKind : uint32_t {
    None = 0,
    Bitmap = 1,
    Text = 2,
    Hex = 3,
    Unsupported = 4,
    // Archive contents: a tab-separated tree (see archive_listing.h) that the
    // UI draws itself - summary, type mix and an expandable folder tree.
    Archive = 5,
    // Markdown rendered view: block records plus the source (see
    // preview_host/markdown_document.h), drawn by ui/markdown_view.cpp.
    Markdown = 6,
    // CSV/TSV/XLSX grid: sheet and row records (see
    // preview_host/table_document.h), drawn by ui/table_view.cpp.
    Table = 7,
    // JSON/XML tree: node records plus the source (see
    // preview_host/tree_document.h), drawn by ui/tree_view.cpp.
    Tree = 8,
};
enum class PreviewRequestKind : uint32_t {
    Content = 0,
    Properties = 1,
    // Audio columns: title, artist and album as three label/value pairs in a
    // fixed order, so a row that needs only these never pays for the general
    // property handler sweep in ReadProperties.
    AudioMeta = 2,
};
constexpr uint32_t kAudioMetaFields = 3;
constexpr uint32_t kPreviewFlagTruncated = 1u << 0;
// Source encoding of a Text preview, stored in response flag bits 8..11.
constexpr uint32_t kPreviewFlagEncodingShift = 8;
constexpr uint32_t kPreviewFlagEncodingMask = 0xFu << kPreviewFlagEncodingShift;
enum class PreviewTextEncoding : uint32_t { Unknown = 0, Utf8, Utf8Bom, Utf16Le, Utf16Be, Ansi };
// PreviewRequest::flags. Grid: a list/grid/sidebar thumbnail (as opposed to
// the details pane or Quick Look), whatever its pixel size - extra large icons
// on high-DPI screens ask for more than kGridThumbnailEdge.
constexpr uint32_t kPreviewRequestFlagGrid = 1u << 0;
// Quick Look on a folder: its contents listing (folder_listing.h, as an
// Archive payload) instead of the folder icon. frame_index 0 is a quick pass,
// 1 the full count.
constexpr uint32_t kPreviewRequestFlagFolderListing = 1u << 1;
// Quick Look: rich document payloads (Markdown) instead of plain text.
constexpr uint32_t kPreviewRequestFlagRichText = 1u << 2;
// Isolated, low-priority folder artwork; never the ordinary folder listing.
constexpr uint32_t kPreviewRequestFlagFolderThumbnail = 1u << 3;
constexpr uint32_t kPreviewRequestFlagFolderRefresh = 1u << 4;
constexpr uint32_t kPreviewRequestFlagFolderSingle = 1u << 5;
constexpr uint32_t kPreviewMaxTextChars = 32768;
constexpr uint32_t kPreviewMaxArchiveChars = 512u * 1024u;
constexpr uint32_t kPreviewMaxTableChars = 2u * 1024u * 1024u;
constexpr uint32_t kPreviewMinPixelSize = 32;
constexpr uint32_t kPreviewDefaultPixelSize = 512;
constexpr uint32_t kPreviewMaxPixelSize = 1024;
constexpr uint32_t kPreviewGifMaxPixelSize = 512;
constexpr uint32_t kPreviewPixelBucket = 128;

inline uint32_t ClampPreviewPixelSize(uint32_t requested, bool gif) noexcept {
    const uint32_t cap = gif ? kPreviewGifMaxPixelSize : kPreviewMaxPixelSize;
    if (requested < kPreviewMinPixelSize) return kPreviewMinPixelSize;
    if (requested > cap) return cap;
    return requested;
}

inline uint32_t BucketPreviewPixelSize(uint32_t longest_edge) noexcept {
    if (longest_edge <= kPreviewDefaultPixelSize) return kPreviewDefaultPixelSize;
    const uint32_t bucket = ((longest_edge + kPreviewPixelBucket - 1u) /
                             kPreviewPixelBucket) * kPreviewPixelBucket;
    return bucket > kPreviewMaxPixelSize ? kPreviewMaxPixelSize : bucket;
}
struct PreviewRequest {
    uint32_t magic = kPreviewMagic;
    uint32_t request_id = 0;
    uint64_t generation = 0;
    PreviewRequestKind kind = PreviewRequestKind::Content;
    uint32_t pixel_size = 0;
    uint32_t attrs = 0;
    uint32_t path_chars = 0;
    uint32_t frame_index = 0;
    uint32_t flags = 0;
};
struct PreviewResponse {
    uint32_t magic = kPreviewMagic;
    uint32_t request_id = 0;
    uint64_t generation = 0;
    int32_t status = 0;
    PreviewContentKind kind = PreviewContentKind::None;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t mapping_chars = 0;
    uint32_t text_chars = 0;
    uint32_t error_chars = 0;
    uint32_t property_count = 0;
    uint32_t flags = 0;
    uint32_t bytes_read = 0;
    uint32_t frame_count = 1;
    uint32_t frame_delay_ms = 0;
    uint32_t loop_count = 0;
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    // Grid thumbnails of videos: System.Media.Duration in ms, 0 when unknown.
    uint32_t duration_ms = 0;
    preview::Integrity integrity;
};
inline std::wstring PreviewPipeName(DWORD pid) {
    return L"\\\\.\\pipe\\PulsePreview-" + std::to_wstring(pid);
}
inline bool ReadAll(HANDLE h, void* data, DWORD bytes) {
    auto* p = static_cast<unsigned char*>(data);
    while (bytes) { DWORD n = 0; if (!ReadFile(h, p, bytes, &n, nullptr) || !n) return false; p += n; bytes -= n; }
    return true;
}
inline bool WriteAll(HANDLE h, const void* data, DWORD bytes) {
    auto* p = static_cast<const unsigned char*>(data);
    while (bytes) { DWORD n = 0; if (!WriteFile(h, p, bytes, &n, nullptr) || !n) return false; p += n; bytes -= n; }
    return true;
}
} // namespace pulse::ipc

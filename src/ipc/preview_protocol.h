#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace pulse::ipc {
constexpr uint32_t kPreviewMagic = 0x57565250; // PRVW
enum class PreviewContentKind : uint32_t {
    None = 0,
    Bitmap = 1,
    Text = 2,
    Hex = 3,
    Unsupported = 4,
};
enum class PreviewRequestKind : uint32_t {
    Content = 0,
    Properties = 1,
};
constexpr uint32_t kPreviewFlagTruncated = 1u << 0;
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

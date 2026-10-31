#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pulse::preview {
struct GifFrame {
    std::vector<uint8_t> pixels;
    UINT width = 0, height = 0, stride = 0, source_width = 0, source_height = 0;
    uint32_t count = 0, delay = 100, loops = 0;
    std::wstring error;
};
struct GifStats { uint64_t decoded_frames = 0, cache_loads = 0, resets = 0, reserved_bytes = 0, backup_capacity_bytes = 0; };
// Construct, decode and destroy on one COM-initialized thread. Cancellation is
// checked between bounded decode/composition steps; WIC itself is host-isolated.
class GifDecoder {
public:
    GifDecoder();
    ~GifDecoder();
    GifDecoder(const GifDecoder&) = delete;
    GifDecoder& operator=(const GifDecoder&) = delete;
    bool Decode(const std::wstring& path, UINT edge, uint32_t index, GifFrame& result, HANDLE cancel = nullptr, bool first_frame_only = false);
    void Reset();
    GifStats Stats() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
bool DecodeGifCached(const std::wstring& path, UINT edge, uint32_t index, GifFrame& result);
bool DecodeGifFirstFrameThumbnail(const std::wstring& path, UINT edge, GifFrame& result);
void ResetGifDecodeCache();
}

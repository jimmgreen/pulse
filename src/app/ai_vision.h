// ai_vision.h - Multimodal image encoding (file & clipboard screenshot) via WIC.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace pulse::app::ai {

struct EncodedImage {
    bool ok = false;
    std::string mime;        // "image/jpeg" or "image/png"
    std::string data_url;    // "data:image/jpeg;base64,..."
    int width = 0;
    int height = 0;
    size_t encoded_bytes = 0;
    std::string error_message;
};

std::string Base64Encode(const uint8_t* data, size_t length);

// True when the Windows clipboard holds a bitmap/DIB and no plain text.
bool ClipboardHasImage();

// Encodes an image file (PNG/JPG/WEBP/BMP/GIF/TIFF) downscaled to at most
// `max_edge` on the longer side and capped to `max_bytes` before base64.
EncodedImage EncodeImageFileForVision(const std::wstring& path, int max_edge = 1568, size_t max_bytes = 900 * 1024);

// Captures CF_BITMAP from the clipboard and encodes it as a bounded data URL.
EncodedImage EncodeClipboardImageForVision(HWND hwnd, int max_edge = 1568, size_t max_bytes = 900 * 1024);

} // namespace pulse::app::ai

// background_image_fetch.h — WinHTTP download and JSON Pointer extraction for
// the shared background image source.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

// The service never buffers more than these limits, and WinHTTP aborts a body
// that grows past the request's own limit.
inline constexpr size_t kBackgroundMaxImageBytes = 32ull * 1024ull * 1024ull;
inline constexpr size_t kBackgroundMaxJsonBytes = 1024ull * 1024ull;

struct BackgroundFetchRequest {
    std::wstring url;
    size_t max_bytes = kBackgroundMaxImageBytes;
};

struct BackgroundFetchResult {
    bool ok = false;
    uint32_t error = 0;  // GetLastError()/WinHTTP code when ok is false
    std::vector<uint8_t> body;
};

// The real transport is swapped for a fake by tests; the service only needs
// bytes and an error code, never a live network.
using BackgroundFetchFunction =
    std::function<bool(const BackgroundFetchRequest&, BackgroundFetchResult&)>;

bool FetchBackgroundHttp(const BackgroundFetchRequest& request, BackgroundFetchResult& result);

// RFC 6901 subset: object keys and array indexes, with ~0/~1 escapes. Only a
// string result is accepted. An empty pointer selects the document root; the
// service treats an empty pointer as a raw URL body instead.
enum class JsonPointerStatus : uint8_t { Found, NotFound, InvalidJson, NotString };

JsonPointerStatus JsonPointerString(std::string_view json, std::wstring_view pointer,
                                    std::string& out);

// Magic-only format detection; no decoder is involved and no bytes are trusted.
enum class BackgroundImageFormat : uint8_t { Unknown, Png, Jpeg, Gif, Bmp, Webp, Tiff };

BackgroundImageFormat SniffBackgroundImage(const uint8_t* data, size_t size) noexcept;
const wchar_t* BackgroundImageExtension(BackgroundImageFormat format) noexcept;

bool IsHttpUrl(std::wstring_view url) noexcept;

std::string WideToUtf8(std::wstring_view text);
std::wstring Utf8ToWide(std::string_view text);

}  // namespace pulse::app

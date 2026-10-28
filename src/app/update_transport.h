#pragma once
#include "update_checker.h"
#include <atomic>
#include <functional>
#include <string_view>
#include <vector>

namespace pulse::app {
// Streams an HTTPS response with a size limit, cancellation, and a total deadline.
bool ReadUpdateResponse(std::wstring_view url, uint64_t maximum_bytes,
                        const std::atomic<bool>& cancelled,
                        const std::function<bool(const void*, DWORD)>& consume,
                        UpdateError& category, DWORD& error);

// Called after response headers and after each successfully consumed chunk.
using UpdateDownloadProgress = std::function<void(uint64_t received, uint64_t total)>;
uint64_t ParseUpdateContentLength(std::wstring_view header) noexcept;
bool ReadUpdateResponseWithProgress(std::wstring_view url, uint64_t maximum_bytes,
                                    const std::atomic<bool>& cancelled,
                                    const std::function<bool(const void*, DWORD)>& consume,
                                    UpdateError& category, DWORD& error,
                                    const UpdateDownloadProgress& progress);

using UpdateResponseReader = std::function<decltype(ReadUpdateResponse)>;
std::wstring AcceleratedUpdateUrl(std::wstring_view url);

// Settings > 增强包 > 高级 > 下载源 (packs.json "download_source"), used by
// packs and by Pulse's own update. The values are the stored ones.
enum class UpdateSource : uint32_t {
    Automatic = 0,   // mirror 1, mirror 2, then GitHub
    GitHub = 1,      // github.com only (through the system proxy)
    Mirror1 = 2,     // ghproxy.net only
    Mirror2 = 3,     // gh-proxy.com only
};
inline constexpr uint32_t kUpdateSourceCount = 4;
void SetUpdateSource(UpdateSource source) noexcept;
UpdateSource CurrentUpdateSource() noexcept;
// The URLs tried for `url`, in order. Only public GitHub release assets go
// through a mirror; any other URL is always fetched as is.
std::vector<std::wstring> UpdateSourceUrls(std::wstring_view url, UpdateSource source);
// The URL one source would use for `url` (empty when it cannot serve it).
std::wstring UpdateSourceUrl(std::wstring_view url, UpdateSource source);

// Reset the destination before each attempt so partial responses never mix.
// Under CurrentUpdateSource(): a network/HTTP failure, a response over
// `maximum_bytes`, or a complete response `verify` rejects moves on to the
// next source. When every source fails, a mismatch (too large / rejected)
// is reported over a network failure: the mirrors serve the same file.
bool ReadUpdateWithFallback(std::wstring_view url, uint64_t maximum_bytes,
                            const std::atomic<bool>& cancelled,
                            const std::function<bool()>& reset,
                            const std::function<bool(const void*, DWORD)>& consume,
                            UpdateError& category, DWORD& error,
                            const UpdateResponseReader& read = ReadUpdateResponse,
                            const std::function<bool()>& verify = {});
}

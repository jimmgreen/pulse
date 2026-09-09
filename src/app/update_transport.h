#pragma once
#include "update_checker.h"
#include <atomic>
#include <functional>
#include <string_view>

namespace pulse::app {
// Streams an HTTPS response with a size limit, cancellation, and a total deadline.
bool ReadUpdateResponse(std::wstring_view url, uint64_t maximum_bytes,
                        const std::atomic<bool>& cancelled,
                        const std::function<bool(const void*, DWORD)>& consume,
                        UpdateError& category, DWORD& error);
}

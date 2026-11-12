#pragma once
#include <cstdint>
#include <string>

namespace pulse::preview {
// Bounded, reflowed body content; no Office automation or IFilter fallback.
bool MakeLegacyWordDocument(const std::wstring& path, std::wstring& payload,
                            uint32_t& bytes_read, bool& truncated);
}

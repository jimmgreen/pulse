#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::text {

bool IsOfflinePlaceholder(DWORD attributes) noexcept;
bool IsKnownTextExtension(std::wstring_view extension) noexcept;
bool LooksBinary(const std::vector<uint8_t>& bytes) noexcept;
bool Decode(const std::vector<uint8_t>& bytes, std::wstring& output);
bool ReadFile(const std::wstring& path, uint64_t maximum_bytes, std::wstring& output,
              uint64_t& bytes_read, DWORD* error = nullptr);

} // namespace pulse::text

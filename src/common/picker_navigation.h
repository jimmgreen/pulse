#pragma once
#include <string_view>
namespace pulse::picker_navigation {
inline constexpr wchar_t kComputerPath[] = L"pulse:computer";
inline bool IsComputerPath(std::wstring_view path) noexcept { return path == kComputerPath; }
}

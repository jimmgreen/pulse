#pragma once
#include <string>
#include <string_view>

namespace pulse::preview {

// Empty means an opaque/unknown subtype identifier, unsuitable for display.
std::wstring VideoCodecDisplayName(std::wstring_view value);

} // namespace pulse::preview

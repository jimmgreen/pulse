#pragma once

// Details-pane properties (dimensions, duration, author...) read from the
// shell property store for Properties requests.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct IPropertyStore;

namespace pulse::preview {

struct PreviewPropertyValue { std::wstring label, value; };

// At most six formatted label/value pairs; empty when the store is unavailable.
std::vector<PreviewPropertyValue> ReadPropertiesFromStore(const std::wstring& path, IPropertyStore* store);
std::vector<PreviewPropertyValue> ReadProperties(const std::wstring& path);

// Playing time of a video file in milliseconds (grid thumbnail duration
// chip); 0 for other types or when the property is unavailable.
uint32_t ReadMediaDurationMs(const std::wstring& path);

// Title, artist and album of an audio file, in that order. Every entry is
// empty for a non-audio extension or when the tag is missing, which the
// details columns cache as a negative result.
std::array<std::wstring, 3> ReadAudioMeta(const std::wstring& path);

} // namespace pulse::preview

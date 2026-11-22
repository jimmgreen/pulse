// archive_thumbnail.h — grid cover for archive files, built in the preview host.
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::preview {

// Picks the best-looking picture member of an archive (ZIP central directory
// members and 7z through the system libarchive, archiveint.dll) and decodes it
// into a premultiplied BGRA square thumbnail scaled so its longest edge is at
// most `pixels` (clamped to 32..512). Nothing is extracted to disk.
//
// A member named cover / folder / preview / thumb wins; otherwise the first
// picture. Archives without a usable picture (or a member above 24 MiB) return
// false with *error set so the caller keeps the ordinary type icon:
//   archive-unavailable, archive-open-failed, archive-no-cover,
//   archive-cover-decode-failed, libarchive-unavailable
bool MakeArchiveThumbnail(const std::wstring& path, UINT pixels,
                          std::vector<uint8_t>& bgra, UINT& width, UINT& height, UINT& stride,
                          std::wstring* error);

} // namespace pulse::preview
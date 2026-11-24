// archive_member.h - thumbnails of pictures inside an archive (Quick Look's
// archive thumbnail view).
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::preview {

// Splits "<archive path>|<member path>" (ipc::kPreviewArchiveMemberSeparator).
// False when there is no separator or either side is empty.
bool SplitArchiveMemberPath(std::wstring_view spec, std::wstring& archive, std::wstring& member);

// Reads the member (UTF-16, '\' or '/' separators, a leading "./" ignored) of
// the archive at `archive` into `data` through the system's libarchive
// (archiveint.dll from System32, as archive_listing.cpp and zip_entry.cpp do):
// ZIP seekably, other formats by a sequential scan. Members larger than
// max_bytes are refused. *error: libarchive-unavailable, archive-open-failed,
// entry-not-found, entry-too-large, entry-read-failed.
bool ReadArchiveMember(const std::wstring& archive, std::wstring_view member, size_t max_bytes,
                       std::vector<unsigned char>& data, std::wstring* error);

// Decodes a picture member into 32bpp premultiplied BGRA no larger than
// max_edge on its long side. source_* receive the full picture size.
bool DecodeArchiveMemberThumbnail(std::wstring_view spec, UINT max_edge, std::vector<uint8_t>& pixels,
                                  UINT& width, UINT& height, UINT& stride,
                                  UINT& source_width, UINT& source_height, std::wstring* error);

} // namespace pulse::preview

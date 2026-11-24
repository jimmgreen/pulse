// zip_entry.h - single members of ZIP packages (OOXML, ODF, EPUB).
#pragma once

#include <windows.h>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::preview {

// Reads the member `name` of the ZIP file at `path` into `data`.
//
// `name` is the UTF-8 member path with '/' separators and no leading slash;
// it is compared ASCII case-insensitively, as OPC part names are. Members are
// inflated through the system's libarchive (archiveint.dll, Windows 10 1803+,
// loaded on demand from System32 only) in seekable mode, so only the central
// directory and the wanted member are read. Members larger than max_bytes are
// refused. Returns false with *error set to one of libarchive-unavailable,
// archive-open-failed, entry-not-found, entry-too-large, entry-read-failed.
bool ReadZipEntry(const std::wstring& path, std::string_view name, size_t max_bytes,
                  std::vector<unsigned char>& data, std::wstring* error);

// Visits requested members in archive order while retaining their logical indices.
using ZipEntryVisitor = std::function<bool(size_t, const std::vector<unsigned char>&)>;
bool VisitZipEntries(const std::wstring& path, const std::vector<std::string>& names, size_t max_bytes,
                     const ZipEntryVisitor& visit);

// Like ReadZipEntry, but a member larger than max_bytes is not refused: its
// first max_bytes are returned and *truncated is set. For formats that can be
// read from the start (XML parts whose first elements are enough).
bool ReadZipEntryPrefix(const std::wstring& path, std::string_view name, size_t max_bytes,
                        std::vector<unsigned char>& data, bool* truncated, std::wstring* error);

} // namespace pulse::preview

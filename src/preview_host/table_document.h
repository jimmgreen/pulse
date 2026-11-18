// table_document.h — Quick Look table payload for CSV/TSV and XLSX.
//
// Payload (UTF-16, one record per line, fields separated by tabs; text fields
// escape backslash, tab and newline as \\ \t \n):
//   PULSETBL\t1
//   S\t<kind>\t<name>\t<columns>\t<rows>\t<truncated>\t<detail>\t<header>
//       kind: "csv" or "xlsx"; rows/columns: what follows in R records;
//       truncated: 1 when rows, columns, fields or payload were clipped;
//       detail: CSV delimiter name ("comma", "semicolon", "tab", "pipe");
//       header: 1 when the first row is a header row (CSV heuristic).
//   R\t<cell>\t<cell>...   one per row of the preceding sheet; a cell that
//       starts with "\B" is bold (XLSX styles).
//   F\t<row>\t<column>\t<ARGB> optional solid cell fill, zero-based indices;
//       eight hexadecimal digits, separate from cell text. Older readers ignore it.
//   X\t<source>            CSV only: the decoded text for the source view.
//   W\t<total>\t<read>\t<selected> XLSX: total sheets, read parts (0/1), and
//       the requested zero-based workbook sheet index.
// XLSX S.detail explains unavailable sheets: hidden, missing-relationship,
// not-loaded (request this sheet on selection), time-limit, payload-limit,
// or read-failed. Only the selected sheet contains rows; not-loaded is not
// truncation. Hidden sheets remain listed but are not read.
// S records retain workbook order, including unavailable/hidden sheets; if
// metadata itself exceeds the payload budget W.total exceeds the S count.
#pragma once

#include <cstdint>
#include <string>

#include "../ipc/preview_protocol.h"

namespace pulse::preview {

// Reads up to max_bytes of a text file and decodes it (BOM, UTF-8, else the
// ANSI code page). cut: the file is longer; the text then ends at a line break.
bool ReadTextFile(const std::wstring& path, size_t max_bytes, std::wstring& text, bool& cut,
                  uint32_t& bytes_read, ipc::PreviewTextEncoding& encoding);

bool IsTableExtension(std::wstring_view extension);
bool IsSpreadsheetExtension(std::wstring_view extension);  // .xlsx / .xlsm

bool MakeCsvTable(const std::wstring& path, std::wstring_view extension, std::wstring& payload,
                  uint32_t& bytes_read, bool& truncated, ipc::PreviewTextEncoding& encoding);
// Needs the system libarchive (archiveint.dll, Windows 10 1803+) via ReadZipEntry.
bool MakeXlsxTable(const std::wstring& path, std::wstring& payload, uint32_t& bytes_read,
                   uint32_t sheet_index = 0);

}  // namespace pulse::preview

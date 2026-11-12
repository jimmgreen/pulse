// office_doc_model.h - first-page content of legacy Word files for thumbnails.
#pragma once

#include "office_model.h"
#include <windows.h>
#include <string>

namespace pulse::preview {

enum class WordFileFormat { Unknown, OpenXml, Binary, Rtf };

// Decided by content, not extension: ".doc" files are often really .docx
// (ZIP) or RTF saved under the old name, and the reverse happens too.
WordFileFormat SniffWordFile(const std::wstring& path);

// Word 97-2003 binary document ([MS-DOC] inside an [MS-CFB] compound file;
// the reading order follows Apache POI HWPF): the body text through the piece
// table, tables and alignment from the paragraph properties, sizes and bold
// from the character properties resolved through the style sheet, and the
// first section's page setup. Only the first page's worth of blocks is read.
// Passing truncated enables bounded document-body mode (2M DOC characters,
// 32768 blocks/rows; RTF input is capped at 8 MiB), without IFilter.
// False (with *error) for encrypted, Word 6/95 or damaged files.
bool ReadDocBinaryModel(const std::wstring& path, DocModel& model, std::wstring* error, bool* truncated = nullptr);

// Rich Text Format: paragraphs, tables, alignment, sizes, bold, page setup.
bool ReadRtfModel(const std::wstring& path, DocModel& model, std::wstring* error, bool* truncated = nullptr);

// RTF body as plain text for the details pane and Quick Look (the raw markup
// is unreadable): paragraphs one per line, table cells tab-separated, capped
// like other text previews (*truncated when cut).
bool ReadRtfText(const std::wstring& path, std::wstring& text, bool* truncated, std::wstring* error);

// Plain text through the IFilter Windows Search uses for the file type (the
// Office filter ships with Windows): paragraphs only, default formatting.
bool ReadFilterTextModel(const std::wstring& path, DocModel& model, std::wstring* error);

// Any Word file: dispatches on SniffWordFile, then falls back to the IFilter
// text when the structured reader fails.
bool ReadWordModel(const std::wstring& path, DocModel& model, std::wstring* error);

} // namespace pulse::preview

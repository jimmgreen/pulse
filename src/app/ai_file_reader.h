// ai_file_reader.h - Bounded, explicit-consent local document reader for Pulse 助手.
//
// Reads user-attached or explicitly confirmed local files (plaintext, code,
// Office OOXML, PDF via pulse_document_reader) with strict byte/character caps
// and binary detection. Never called without user attachment or confirmation.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace pulse::app::ai {

struct LocalFileExcerpt {
    bool ok = false;
    bool is_image = false;
    bool is_dir = false;
    bool truncated = false;
    uint64_t size_bytes = 0;
    std::string format;          // lower-case extension or "folder" / "text"
    std::string text;            // UTF-8 excerpt
    std::string error_code;
    std::string error_message;
};

bool IsSupportedImageExtension(std::wstring_view ext);
bool IsReadableDocumentExtension(std::wstring_view ext);
std::wstring LowerExtension(std::wstring_view path);

// Reads a bounded UTF-8 text excerpt from `path` (supports long paths & UNC).
// For images, sets `ok = true, is_image = true` without decoding pixels.
// For directories, lists up to 50 direct items (names and sizes).
LocalFileExcerpt ReadLocalFileExcerpt(const std::wstring& path, size_t max_chars,
                                      const std::atomic<bool>& cancelled);

} // namespace pulse::app::ai

// ai_file_reader.cpp - Bounded local document reading (ai_file_reader.h).
#include "ai_file_reader.h"

#include "ai_tools_fs.h"
#include "../common/json_value.h"
#include "../common/text_decode.h"
#include "../index/document_reader.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <vector>

namespace pulse::app::ai {
namespace {

constexpr uint64_t kMaxDocumentReadBytes = 16ull * 1024ull * 1024ull;
constexpr uint64_t kMaxPlainTextBytes = 512ull * 1024ull;

std::string TruncateUtf8Chars(const std::string& s, size_t max_chars, bool& truncated) {
    size_t chars = 0, i = 0;
    while (i < s.size()) {
        if (chars >= max_chars) {
            truncated = true;
            return s.substr(0, i);
        }
        const unsigned char b = static_cast<unsigned char>(s[i]);
        size_t step = 1;
        if ((b & 0xE0u) == 0xC0u) step = 2;
        else if ((b & 0xF0u) == 0xE0u) step = 3;
        else if ((b & 0xF8u) == 0xF0u) step = 4;
        if (i + step > s.size()) break;
        i += step;
        ++chars;
    }
    truncated = false;
    return s;
}

bool LooksBinary(const uint8_t* data, size_t n) {
    if (!data || n == 0) return false;
    // Allow UTF-16 BOMs (text_decode handles them cleanly).
    if (n >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF))) return false;
    size_t nul = 0, ctrl = 0;
    const size_t check = (std::min)(n, static_cast<size_t>(8192));
    for (size_t i = 0; i < check; ++i) {
        const uint8_t b = data[i];
        if (b == 0) ++nul;
        else if (b < 0x09 || (b > 0x0D && b < 0x20 && b != 0x1B)) ++ctrl;
    }
    return nul > 0 || (ctrl * 20 > check);
}

std::string FormatSizeShort(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 3) {
        v /= 1024.0;
        ++u;
    }
    char buf[32]{};
    if (u == 0 || v >= 100.0) sprintf_s(buf, "%.0f %s", v, units[u]);
    else sprintf_s(buf, "%.1f %s", v, units[u]);
    return buf;
}

} // namespace

std::wstring LowerExtension(std::wstring_view path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.rfind(L'.');
    if (dot == std::wstring_view::npos || (slash != std::wstring_view::npos && dot < slash)) return {};
    std::wstring ext(path.substr(dot + 1));
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext;
}

bool IsSupportedImageExtension(std::wstring_view ext) {
    return ext == L"png" || ext == L"jpg" || ext == L"jpeg" || ext == L"webp" || ext == L"bmp" || ext == L"gif" ||
           ext == L"tif" || ext == L"tiff";
}

bool IsReadableDocumentExtension(std::wstring_view ext) {
    if (!ext.empty()) {
        const std::wstring dotted = ext.front() == L'.' ? std::wstring(ext) : (L"." + std::wstring(ext));
        if (index::IsExtractedDocumentExtension(dotted) || index::IsIndexedContentExtension(dotted)) return true;
    }
    static constexpr const wchar_t* kExtra[] = {
        L"txt", L"md", L"markdown", L"json", L"jsonl", L"yaml", L"yml", L"toml", L"ini", L"cfg", L"conf",
        L"csv", L"tsv", L"xml", L"html", L"htm", L"css", L"scss", L"js", L"mjs", L"cjs", L"ts", L"tsx", L"jsx",
        L"py", L"c", L"cc", L"cpp", L"cxx", L"h", L"hh", L"hpp", L"cs", L"java", L"kt", L"go", L"rs", L"rb",
        L"php", L"sql", L"sh", L"bash", L"zsh", L"ps1", L"psm1", L"bat", L"cmd", L"cmake", L"log", L"svg",
        L"tex", L"rst", L"env", L"gitignore", L"dockerfile", L"makefile",
    };
    for (const wchar_t* e : kExtra)
        if (ext == e) return true;
    return false;
}

LocalFileExcerpt ReadLocalFileExcerpt(const std::wstring& path, size_t max_chars,
                                      const std::atomic<bool>& cancelled) {
    LocalFileExcerpt out;
    if (cancelled.load()) {
        out.error_code = "cancelled";
        out.error_message = "Read cancelled.";
        return out;
    }
    const size_t limit = std::clamp<size_t>(max_chars, 200, 32000);
    const std::wstring win_path = Win32Path(path);
    const DWORD attrs = GetFileAttributesW(win_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        out.error_code = "not_found";
        out.error_message = "File or folder does not exist.";
        return out;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        out.is_dir = true;
        out.format = "folder";
        std::wstring pattern = win_path;
        if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
        pattern += L'*';
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                       FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            out.error_code = "access_denied";
            out.error_message = "Could not list folder.";
            return out;
        }
        std::string listing;
        size_t count = 0;
        do {
            if (cancelled.load()) break;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            if (count >= 60) {
                out.truncated = true;
                break;
            }
            const bool is_sub = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const uint64_t sz = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            listing += is_sub ? "[dir] " : "[file] ";
            listing += json::Utf8(fd.cFileName);
            if (!is_sub) listing += " (" + FormatSizeShort(sz) + ")";
            listing.push_back('\n');
            ++count;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
        if (cancelled.load()) {
            out.error_code = "cancelled";
            out.error_message = "Read cancelled.";
            return out;
        }
        out.text = TruncateUtf8Chars(listing, limit, out.truncated);
        out.ok = true;
        return out;
    }

    const std::wstring ext = LowerExtension(path);
    out.format = ext.empty() ? "text" : json::Utf8(ext);

    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(win_path.c_str(), GetFileExInfoStandard, &info)) {
        out.size_bytes = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    }

    if (IsSupportedImageExtension(ext)) {
        out.is_image = true;
        out.ok = true;
        return out;
    }

    // Extracted documents (PDF, DOCX, XLSX, PPTX, ODT, etc.): use Pulse's isolated reader.
    const std::wstring dotted_ext = ext.empty() ? std::wstring() : (L"." + ext);
    if (!dotted_ext.empty() && index::IsExtractedDocumentExtension(dotted_ext)) {
        if (out.size_bytes > kMaxDocumentReadBytes) {
            out.error_code = "too_large";
            out.error_message = "Document exceeds the 16 MB reading limit.";
            return out;
        }
        index::DocumentReadSession session;
        std::wstring wide_text;
        uint64_t bytes_read = 0;
        DWORD err = ERROR_SUCCESS;
        const bool ok = index::ReadSearchableDocument(
            path, kMaxDocumentReadBytes, wide_text, bytes_read, &err, text::Encoding::Auto,
            [&] { return cancelled.load(); });
        if (cancelled.load()) {
            out.error_code = "cancelled";
            out.error_message = "Read cancelled.";
            return out;
        }
        if (!ok) {
            out.error_code = "extract_failed";
            out.error_message = "Could not extract text from this document (code " + std::to_string(err) + ").";
            return out;
        }
        out.text = TruncateUtf8Chars(json::Utf8(wide_text), limit, out.truncated);
        out.ok = true;
        return out;
    }

    // Plaintext / source code / config files: direct bounded read + encoding decode.
    HANDLE file = CreateFileW(win_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        out.error_code = e == ERROR_ACCESS_DENIED ? "access_denied" : "open_failed";
        out.error_message = "Could not open file for reading (Win32 " + std::to_string(e) + ").";
        return out;
    }
    const size_t to_read = static_cast<size_t>((std::min<uint64_t>)(out.size_bytes, kMaxPlainTextBytes));
    std::vector<uint8_t> raw(to_read);
    DWORD got = 0;
    const BOOL read_ok = to_read == 0 ? TRUE : ReadFile(file, raw.data(), static_cast<DWORD>(to_read), &got, nullptr);
    CloseHandle(file);
    if (!read_ok) {
        out.error_code = "read_failed";
        out.error_message = "Failed to read file bytes.";
        return out;
    }
    raw.resize(got);
    if (LooksBinary(raw.data(), raw.size()) || text::LooksBinary(raw)) {
        out.error_code = "binary_file";
        out.error_message = "This file appears to be binary rather than readable text.";
        return out;
    }
    std::wstring decoded;
    if (!raw.empty()) {
        (void)text::Decode(raw, decoded, text::Encoding::Auto);
    }
    bool trunc = out.size_bytes > kMaxPlainTextBytes;
    out.text = TruncateUtf8Chars(json::Utf8(decoded), limit, out.truncated);
    out.truncated = out.truncated || trunc;
    out.ok = true;
    return out;
}

} // namespace pulse::app::ai

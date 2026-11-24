#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::extensions::vfs {

inline constexpr wchar_t kVfsPathPrefix[] = L"pulse:vfs:";
inline constexpr size_t kMaxVfsRelPathBytes = 4096;
inline constexpr size_t kMaxVfsSegments = 64;

inline bool IsVfsPath(std::wstring_view path) noexcept {
    return path.starts_with(kVfsPathPrefix);
}

inline bool Utf8ToWideStrict(std::string_view utf8, std::wstring& out) {
    out.clear();
    out.reserve(utf8.size());
    size_t i = 0;
    const size_t n = utf8.size();
    while (i < n) {
        const uint8_t b0 = static_cast<uint8_t>(utf8[i]);
        if (b0 == 0) return false;
        uint32_t cp = 0;
        size_t seq_len = 0;
        if (b0 < 0x80u) {
            cp = b0;
            seq_len = 1;
        } else if ((b0 & 0xE0u) == 0xC0u) {
            cp = b0 & 0x1Fu;
            seq_len = 2;
        } else if ((b0 & 0xF0u) == 0xE0u) {
            cp = b0 & 0x0Fu;
            seq_len = 3;
        } else if ((b0 & 0xF8u) == 0xF0u) {
            cp = b0 & 0x07u;
            seq_len = 4;
        } else {
            return false;
        }
        if (i + seq_len > n) return false;
        for (size_t k = 1; k < seq_len; ++k) {
            const uint8_t bk = static_cast<uint8_t>(utf8[i + k]);
            if ((bk & 0xC0u) != 0x80u) return false;
            cp = (cp << 6) | (bk & 0x3Fu);
        }
        if ((seq_len == 2 && cp < 0x80u) ||
            (seq_len == 3 && cp < 0x800u) ||
            (seq_len == 4 && cp < 0x10000u) ||
            cp > 0x10FFFFu ||
            (cp >= 0xD800u && cp <= 0xDFFFu)) {
            return false;
        }
        if (cp <= 0xFFFFu) {
            out.push_back(static_cast<wchar_t>(cp));
        } else {
            const uint32_t v = cp - 0x10000u;
            out.push_back(static_cast<wchar_t>(0xD800u + (v >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00u + (v & 0x3FFu)));
        }
        i += seq_len;
    }
    return true;
}

inline bool WideToUtf8Strict(std::wstring_view wide, std::string& out) {
    out.clear();
    out.reserve(wide.size());
    size_t i = 0;
    const size_t n = wide.size();
    while (i < n) {
        uint32_t cp = static_cast<uint16_t>(wide[i]);
        if (cp == 0) return false;
        if (cp >= 0xD800u && cp <= 0xDBFFu) {
            if (i + 1 >= n) return false;
            const uint32_t lo = static_cast<uint16_t>(wide[i + 1]);
            if (lo < 0xDC00u || lo > 0xDFFFu) return false;
            cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
            i += 2;
        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
            return false;
        } else {
            i += 1;
        }
        if (cp < 0x80u) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800u) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp < 0x10000u) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }
    return true;
}

inline bool IsValidVfsIdentifier(std::string_view id) noexcept {
    if (id.empty() || id.size() > 64 || id == "." || id == "..") return false;
    if (id.find("..") != std::string_view::npos) return false;
    for (const unsigned char ch : id) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
        if (!ok) return false;
    }
    return true;
}

inline bool IsValidVfsEntryName(std::string_view name) noexcept {
    if (name.empty() || name.size() > 255 || name == "." || name == "..") return false;
    for (const unsigned char ch : name) {
        if (ch < 0x20u) return false;
        switch (ch) {
        case '/':
        case '\\':
        case ':':
        case '*':
        case '?':
        case '"':
        case '<':
        case '>':
        case '|':
            return false;
        default:
            break;
        }
    }
    std::wstring dummy;
    return Utf8ToWideStrict(name, dummy);
}

inline bool NormalizeRelPathUtf8(std::string_view input,
                                   std::string& out_normalized,
                                   std::vector<std::wstring>* out_segments = nullptr) {
    out_normalized.clear();
    if (out_segments) out_segments->clear();
    if (input.size() > kMaxVfsRelPathBytes) return false;
    for (const unsigned char ch : input) {
        if (ch < 0x20u) return false;
    }
    if (input.empty()) return true;

    std::string work(input);
    for (char& ch : work) {
        if (ch == '\\') ch = '/';
    }

    size_t begin = 0;
    size_t end = work.size();
    if (work.front() == '/') begin = 1;
    if (end > begin && work.back() == '/') --end;
    if (begin >= end) return true; // "/" or empty

    const std::string_view body(work.data() + begin, end - begin);
    size_t pos = 0;
    size_t count = 0;
    while (pos <= body.size()) {
        const size_t slash = body.find('/', pos);
        const size_t seg_end = (slash == std::string_view::npos) ? body.size() : slash;
        const std::string_view seg = body.substr(pos, seg_end - pos);
        if (seg.empty()) return false; // rejects "//"
        if (!IsValidVfsEntryName(seg)) return false;
        if (++count > kMaxVfsSegments) return false;
        if (!out_normalized.empty()) out_normalized.push_back('/');
        out_normalized.append(seg);
        if (out_segments) {
            std::wstring wseg;
            if (!Utf8ToWideStrict(seg, wseg)) return false;
            out_segments->push_back(std::move(wseg));
        }
        if (slash == std::string_view::npos) break;
        pos = slash + 1;
    }
    return true;
}

struct ParsedVfsPath {
    std::string package_id;
    std::string mount_id;
    std::vector<std::wstring> segments;
    std::string rel_path_utf8;
};

inline std::optional<ParsedVfsPath> ParseVfsPath(std::wstring_view path) {
    if (!IsVfsPath(path)) return std::nullopt;
    std::string utf8;
    if (!WideToUtf8Strict(path.substr(std::char_traits<wchar_t>::length(kVfsPathPrefix)), utf8)) {
        return std::nullopt;
    }
    for (char& ch : utf8) {
        if (ch == '\\') ch = '/';
    }
    while (!utf8.empty() && utf8.front() == '/') utf8.erase(utf8.begin());

    const size_t first_slash = utf8.find('/');
    if (first_slash == std::string::npos || first_slash == 0) return std::nullopt;
    const std::string pkg = utf8.substr(0, first_slash);
    if (!IsValidVfsIdentifier(pkg)) return std::nullopt;

    const std::string_view after_pkg(utf8.data() + first_slash + 1, utf8.size() - first_slash - 1);
    const size_t second_slash = after_pkg.find('/');
    const std::string_view mount_view =
        second_slash == std::string_view::npos ? after_pkg : after_pkg.substr(0, second_slash);
    if (!IsValidVfsIdentifier(mount_view)) return std::nullopt;

    ParsedVfsPath out;
    out.package_id = pkg;
    out.mount_id = std::string(mount_view);
    if (second_slash != std::string_view::npos) {
        const std::string_view rel_view = after_pkg.substr(second_slash + 1);
        if (!NormalizeRelPathUtf8(rel_view, out.rel_path_utf8, &out.segments)) {
            return std::nullopt;
        }
    }
    return out;
}

inline std::wstring FormatVfsPath(std::string_view package_id,
                                  std::string_view mount_id,
                                  std::string_view rel_path_utf8 = {}) {
    if (!IsValidVfsIdentifier(package_id) || !IsValidVfsIdentifier(mount_id)) return {};
    std::string norm_rel;
    if (!NormalizeRelPathUtf8(rel_path_utf8, norm_rel)) return {};

    std::string utf8 = "pulse:vfs:";
    utf8.append(package_id);
    utf8.push_back('/');
    utf8.append(mount_id);
    if (!norm_rel.empty()) {
        utf8.push_back('/');
        utf8.append(norm_rel);
    }
    std::wstring wide;
    if (!Utf8ToWideStrict(utf8, wide)) return {};
    return wide;
}

inline std::wstring JoinVfsChild(const ParsedVfsPath& dir, std::wstring_view child_name) {
    std::string child_utf8;
    if (!WideToUtf8Strict(child_name, child_utf8) || !IsValidVfsEntryName(child_utf8)) return {};
    std::string rel = dir.rel_path_utf8;
    if (!rel.empty()) rel.push_back('/');
    rel.append(child_utf8);
    return FormatVfsPath(dir.package_id, dir.mount_id, rel);
}

inline std::optional<std::wstring> ParentVfsPath(std::wstring_view path) {
    auto parsed = ParseVfsPath(path);
    if (!parsed || parsed->segments.empty()) return std::nullopt;
    std::string parent_rel;
    const size_t slash = parsed->rel_path_utf8.rfind('/');
    if (slash != std::string::npos) {
        parent_rel = parsed->rel_path_utf8.substr(0, slash);
    }
    return FormatVfsPath(parsed->package_id, parsed->mount_id, parent_rel);
}

inline bool IsSameOrDescendantVfsPath(std::wstring_view candidate,
                                      std::string_view package_id,
                                      std::string_view mount_id,
                                      std::string_view rel_path_utf8) {
    const auto parsed = ParseVfsPath(candidate);
    if (!parsed) return false;
    if (parsed->package_id != package_id || parsed->mount_id != mount_id) return false;
    std::string norm_rel;
    if (!NormalizeRelPathUtf8(rel_path_utf8, norm_rel)) return false;
    if (norm_rel.empty()) return true;
    if (parsed->rel_path_utf8 == norm_rel) return true;
    return parsed->rel_path_utf8.size() > norm_rel.size() &&
           parsed->rel_path_utf8.starts_with(norm_rel) &&
           parsed->rel_path_utf8[norm_rel.size()] == '/';
}

} // namespace pulse::extensions::vfs

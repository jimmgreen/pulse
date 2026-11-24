#include "jpeg_embedded_preview.h"
#include <windows.h>
#include <algorithm>
#include <cstring>

namespace pulse::preview {
namespace {

constexpr size_t kHeaderBytes = 128 * 1024;          // Exif is at most 64 KB
constexpr uint32_t kMaxPreviewBytes = 8u * 1024 * 1024;
constexpr uint32_t kMaxPreviewEdge = 2560;            // larger is a second full picture

uint32_t Be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }

// A TIFF structure (Exif and MPF both use one) inside [base, base + size).
struct Tiff {
    const uint8_t* base = nullptr;
    size_t size = 0;
    bool little = false;
    bool Init(const uint8_t* p, size_t n) {
        if (n < 8) return false;
        if (p[0] == 'I' && p[1] == 'I') little = true;
        else if (p[0] == 'M' && p[1] == 'M') little = false;
        else return false;
        base = p; size = n;
        return U16(2) == 42;
    }
    uint32_t U16(size_t at) const {
        if (at + 2 > size) return 0;
        return little ? uint32_t(base[at]) | (uint32_t(base[at + 1]) << 8) : Be16(base + at);
    }
    uint32_t U32(size_t at) const {
        if (at + 4 > size) return 0;
        const uint8_t* p = base + at;
        return little ? uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24)
                      : (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }
    // Calls visit(tag, type, count, value_at) for each entry of the IFD at
    // `at`; returns the next IFD's offset (0 at the end or on bad data).
    template <class Visit> uint32_t Ifd(uint32_t at, Visit visit) const {
        if (!at || at + 2 > size) return 0;
        const uint32_t count = U16(at);
        if (count > 1000 || at + 2 + size_t(count) * 12 + 4 > size) return 0;
        for (uint32_t i = 0; i < count; ++i) {
            const size_t entry = at + 2 + size_t(i) * 12;
            visit(U16(entry), U16(entry + 2), U32(entry + 4), entry + 8);
        }
        return U32(at + 2 + size_t(count) * 12);
    }
};

void ParseExif(const uint8_t* tiff_start, size_t tiff_size, size_t tiff_offset, JpegHeaderInfo& out) {
    Tiff t;
    if (!t.Init(tiff_start, tiff_size)) return;
    uint32_t exif_ifd = 0;
    const uint32_t ifd1 = t.Ifd(t.U32(4), [&](uint32_t tag, uint32_t, uint32_t, size_t value_at) {
        if (tag == 0x0112) { const uint32_t v = t.U16(value_at); if (v >= 1 && v <= 8) out.orientation = v; }
        else if (tag == 0x8769) exif_ifd = t.U32(value_at);
    });
    t.Ifd(exif_ifd, [&](uint32_t tag, uint32_t, uint32_t, size_t value_at) {
        if (tag == 0xA001 && t.U16(value_at) == 0xFFFF) out.colour_profile = true;
    });
    uint32_t thumb_at = 0, thumb_size = 0;
    t.Ifd(ifd1, [&](uint32_t tag, uint32_t, uint32_t, size_t value_at) {
        if (tag == 0x0201) thumb_at = t.U32(value_at);
        else if (tag == 0x0202) thumb_size = t.U32(value_at);
    });
    if (thumb_at && thumb_size && size_t(thumb_at) + thumb_size <= tiff_size) {
        out.exif_thumbnail_offset = tiff_offset + thumb_at;
        out.exif_thumbnail_size = thumb_size;
    }
}

void ParseMpf(const uint8_t* tiff_start, size_t tiff_size, uint64_t tiff_file_offset, JpegHeaderInfo& out) {
    Tiff t;
    if (!t.Init(tiff_start, tiff_size)) return;
    uint32_t entries_at = 0, entries_bytes = 0;
    t.Ifd(t.U32(4), [&](uint32_t tag, uint32_t, uint32_t count, size_t value_at) {
        if (tag == 0xB002) { entries_bytes = count; entries_at = t.U32(value_at); }
    });
    if (!entries_at || entries_bytes % 16 || size_t(entries_at) + entries_bytes > tiff_size) return;
    for (uint32_t at = entries_at; at < entries_at + entries_bytes; at += 16) {
        const uint32_t attribute = t.U32(at), bytes = t.U32(at + 4), offset = t.U32(at + 8);
        // Only "large thumbnail" types; other entries are depth maps, HDR
        // gain maps or further full pictures.
        const uint32_t type = attribute & 0xFFFFFF;
        if (!offset || !bytes || bytes > kMaxPreviewBytes || (type != 0x010001 && type != 0x010002)) continue;
        out.mpf_previews.push_back({tiff_file_offset + offset, bytes});
    }
}

bool ReadAt(HANDLE file, uint64_t offset, std::vector<uint8_t>& bytes, uint32_t size) {
    bytes.resize(size);
    OVERLAPPED at{};
    at.Offset = static_cast<DWORD>(offset);
    at.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD got = 0;
    return ReadFile(file, bytes.data(), size, &got, &at) && got == size;
}

bool SameShape(uint32_t w, uint32_t h, uint32_t main_w, uint32_t main_h) {
    if (!w || !h || !main_w || !main_h) return false;
    const double a = double(w) / h, b = double(main_w) / main_h;
    return a / b > 0.98 && a / b < 1.02;
}

} // namespace

bool JpegFrameSize(const uint8_t* data, size_t size, uint32_t& width, uint32_t& height) {
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;
    size_t at = 2;
    while (at + 4 <= size) {
        if (data[at] != 0xFF) return false;
        const uint8_t marker = data[at + 1];
        if (marker == 0xFF) { ++at; continue; }
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) { at += 2; continue; }
        const uint32_t length = Be16(data + at + 2);
        if (length < 2 || at + 2 + length > size) return false;
        const bool frame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (frame) {
            if (length < 7) return false;
            height = Be16(data + at + 5);
            width = Be16(data + at + 7);
            return width && height;
        }
        if (marker == 0xDA || marker == 0xD9) return false;
        at += 2 + length;
    }
    return false;
}

bool ParseJpegHeader(const uint8_t* data, size_t size, JpegHeaderInfo& out) {
    out = JpegHeaderInfo{};
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;
    size_t at = 2;
    bool exif_seen = false, mpf_seen = false;
    while (at + 4 <= size) {
        if (data[at] != 0xFF) return false;
        const uint8_t marker = data[at + 1];
        if (marker == 0xFF) { ++at; continue; }
        if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) { at += 2; continue; }
        const uint32_t length = Be16(data + at + 2);
        if (length < 2 || at + 2 + length > size) return false;
        const uint8_t* body = data + at + 4;
        const size_t body_size = length - 2;
        if (marker == 0xE1 && !exif_seen && body_size > 6 && std::memcmp(body, "Exif\0\0", 6) == 0) {
            exif_seen = true;
            ParseExif(body + 6, body_size - 6, at + 4 + 6, out);
        } else if (marker == 0xE2 && body_size > 12 && std::memcmp(body, "ICC_PROFILE\0", 12) == 0) {
            out.colour_profile = true;
        } else if (marker == 0xE2 && !mpf_seen && body_size > 4 && std::memcmp(body, "MPF\0", 4) == 0) {
            mpf_seen = true;
            ParseMpf(body + 4, body_size - 4, at + 4 + 4, out);
        } else if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (length < 7) return false;
            out.height = Be16(body + 1);
            out.width = Be16(body + 3);
            out.is_progressive = (marker == 0xC2);
            out.non_baseline_sof = (marker != 0xC0 && marker != 0xC1);
        } else if (marker == 0xDA) {
            return out.width && out.height;
        }
        at += 2 + length;
    }
    return false;
}

bool ReadJpegEmbeddedPreview(const std::wstring& path, uint32_t min_edge, JpegEmbeddedPreview& out,
                             JpegProbeSummary* summary) {
    out = JpegEmbeddedPreview{};
    if (summary) *summary = {};
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER file_size{};
    std::vector<uint8_t> header;
    JpegHeaderInfo info;
    bool found = false;
    if (GetFileSizeEx(file, &file_size) && file_size.QuadPart > 4) {
        if (summary) {
            summary->has_size = true;
            summary->file_size = static_cast<uint64_t>(file_size.QuadPart);
        }
        if (ReadAt(file, 0, header, static_cast<uint32_t>((std::min<int64_t>)(file_size.QuadPart, kHeaderBytes))) &&
            ParseJpegHeader(header.data(), header.size(), info)) {
            if (summary) {
                summary->valid = true;
                summary->is_progressive = info.is_progressive;
                summary->needs_coefficients = info.non_baseline_sof;
            }
            if (!info.colour_profile) {
                const auto take = [&](std::vector<uint8_t>&& bytes) {
                    uint32_t w = 0, h = 0;
                    if (!JpegFrameSize(bytes.data(), bytes.size(), w, h) || (std::max)(w, h) < min_edge ||
                        (std::max)(w, h) > kMaxPreviewEdge || !SameShape(w, h, info.width, info.height)) return false;
                    out.jpeg = std::move(bytes);
                    out.width = w; out.height = h;
                    return true;
                };
                // The Exif thumbnail is already read; most are 160 x 120.
                if (info.exif_thumbnail_size)
                    found = take(std::vector<uint8_t>(header.begin() + info.exif_thumbnail_offset,
                        header.begin() + info.exif_thumbnail_offset + info.exif_thumbnail_size));
                // Smallest first: the 640 px one when it is enough.
                std::sort(info.mpf_previews.begin(), info.mpf_previews.end(),
                          [](const auto& a, const auto& b) { return a.size < b.size; });
                for (const auto& entry : info.mpf_previews) {
                    if (found) break;
                    if (entry.offset + entry.size > static_cast<uint64_t>(file_size.QuadPart)) continue;
                    std::vector<uint8_t> bytes;
                    if (ReadAt(file, entry.offset, bytes, entry.size)) found = take(std::move(bytes));
                }
            }
        }
    }
    CloseHandle(file);
    if (!found) return false;
    out.source_width = info.width;
    out.source_height = info.height;
    out.orientation = info.orientation;
    return true;
}

} // namespace pulse::preview

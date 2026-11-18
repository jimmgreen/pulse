#include "media_matroska.h"
#include <algorithm>
#include <cmath>
#include <span>

namespace pulse::app::media {
namespace {
struct Element {
    uint64_t id = 0;
    size_t begin = 0, end = 0;
};

bool Vint(std::span<const unsigned char> bytes, size_t& pos, uint64_t& value, bool id) {
    if (pos >= bytes.size() || !bytes[pos]) return false;
    unsigned mask = 0x80, length = 1;
    while (!(bytes[pos] & mask)) { mask >>= 1; ++length; }
    if ((id && length > 4) || length > bytes.size() - pos) return false;
    value = id ? bytes[pos] : bytes[pos] & (mask - 1);
    ++pos;
    for (unsigned i = 1; i < length; ++i) value = (value << 8) | bytes[pos++];
    return true;
}

bool Next(std::span<const unsigned char> bytes, size_t& pos, Element& e) {
    uint64_t length = 0;
    if (!Vint(bytes, pos, e.id, true) || !Vint(bytes, pos, length, false)) return false;
    e.begin = pos;
    // Only Segment may extend beyond the bounded header buffer (or be unknown-sized).
    if (length > bytes.size() - pos && e.id != 0x18538067) return false;
    e.end = pos + static_cast<size_t>((std::min)(length, uint64_t(bytes.size() - pos)));
    pos = e.end;
    return true;
}

uint64_t UInt(std::span<const unsigned char> bytes) {
    if (bytes.size() > 8) return 0;
    uint64_t value = 0;
    for (const auto b : bytes) value = (value << 8) | b;
    return value;
}

bool Tracks(std::span<const unsigned char> bytes, Props& out) {
    size_t pos = 0;
    Element entry;
    while (pos < bytes.size() && Next(bytes, pos, entry)) {
        if (entry.id != 0xAE) continue;
        auto track = bytes.subspan(entry.begin, entry.end - entry.begin);
        uint64_t type = 0, duration = 0, width = 0, height = 0, enabled = 1;
        size_t at = 0;
        Element field;
        while (at < track.size()) {
            if (!Next(track, at, field)) return false;
            const auto data = track.subspan(field.begin, field.end - field.begin);
            if (field.id == 0x83) type = UInt(data);
            else if (field.id == 0xB9) enabled = UInt(data);
            else if (field.id == 0x23E383) duration = UInt(data);
            else if (field.id == 0xE0) {
                size_t video_at = 0;
                Element video;
                while (video_at < data.size()) {
                    if (!Next(data, video_at, video)) return false;
                    const auto value = UInt(data.subspan(video.begin, video.end - video.begin));
                    if (video.id == 0xB0) width = value;
                    else if (video.id == 0xBA) height = value;
                }
            }
        }
        if (type != 1 || !enabled || !width || !height || width > UINT32_MAX || height > UINT32_MAX) continue;
        if (!out.width) out.width = static_cast<uint32_t>(width);
        if (!out.height) out.height = static_cast<uint32_t>(height);
        // DefaultDuration is nanoseconds per frame; absent for variable frame rate.
        if (!out.fps_milli && duration) {
            const double rate = 1.0e12 / static_cast<double>(duration);
            if (rate >= 1 && rate <= UINT32_MAX) out.fps_milli = static_cast<uint32_t>(std::llround(rate));
        }
        return true;
    }
    return false;
}
}

void ReadMatroskaProps(const std::wstring& path, Props& out) {
    if (out.width && out.height && out.fps_milli) return;
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return;
    const auto ext = path.substr(dot);
    if (_wcsicmp(ext.c_str(), L".mkv") && _wcsicmp(ext.c_str(), L".webm")) return;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    constexpr DWORD limit = 4 * 1024 * 1024;
    std::vector<unsigned char> buffer(limit);
    DWORD read = 0;
    const bool ok = ReadFile(file, buffer.data(), limit, &read, nullptr) != FALSE;
    CloseHandle(file);
    if (!ok) return;
    const std::span<const unsigned char> bytes(buffer.data(), read);
    size_t pos = 0;
    Element e;
    if (!Next(bytes, pos, e) || e.id != 0x1A45DFA3) return;
    while (pos < bytes.size() && Next(bytes, pos, e)) {
        if (e.id != 0x18538067) continue;
        const auto segment = bytes.subspan(e.begin, e.end - e.begin);
        size_t at = 0;
        Element child;
        while (at < segment.size() && Next(segment, at, child)) {
            if (child.id == 0x1654AE6B) {
                Tracks(segment.subspan(child.begin, child.end - child.begin), out);
                return;
            }
        }
        return;
    }
}
}

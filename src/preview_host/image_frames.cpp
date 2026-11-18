// image_frames.cpp — see image_frames.h.
#include "image_frames.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace pulse::preview {
namespace {

uint32_t Be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
uint16_t Be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p) {
    return p[0] | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

const std::array<uint32_t, 256>& CrcTable() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    return table;
}

uint32_t Crc(const uint8_t* data, size_t size, uint32_t crc = 0xFFFFFFFFu) {
    const auto& table = CrcTable();
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

void PutBe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void PutChunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, size_t size) {
    PutBe32(out, static_cast<uint32_t>(size));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    if (size) out.insert(out.end(), data, data + size);
    PutBe32(out, Crc(out.data() + start, out.size() - start) ^ 0xFFFFFFFFu);
}

constexpr uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
constexpr size_t kMaxFrames = 4096;

bool IsType(const uint8_t* p, const char* type) { return std::memcmp(p, type, 4) == 0; }

} // namespace

bool ParseApng(const std::vector<uint8_t>& file, ApngInfo& info) {
    info = ApngInfo{};
    if (file.size() < 8 + 25 || std::memcmp(file.data(), kPngSignature, 8) != 0) return false;
    bool have_actl = false, seen_idat = false, have_ihdr = false, ended = false;
    const auto malformed = [&info]() { info.incomplete_reason = L"malformed"; return false; };
    ApngFrame* current = nullptr;
    bool current_uses_idat = false;
    size_t pos = 8;
    while (pos + 12 <= file.size()) {
        const uint32_t length = Be32(&file[pos]);
        const uint8_t* type = &file[pos + 4];
        if (length > file.size() - pos - 12) { info.incomplete_reason = L"truncated-data"; break; }
        const size_t data = pos + 8;
        if (IsType(type, "IHDR")) {
            if (length != 13 || have_ihdr) return malformed();
            info.width = Be32(&file[data]);
            info.height = Be32(&file[data + 4]);
            info.ihdr = data;
            have_ihdr = true;
        } else if (IsType(type, "acTL")) {
            if (length != 8 || seen_idat || have_actl) return malformed();
            have_actl = true;
            info.declared_frames = Be32(&file[data]);
            if (!info.declared_frames) return malformed();
            info.plays = Be32(&file[data + 4]);
        } else if (IsType(type, "fcTL")) {
            if (length != 26 || !have_actl || !have_ihdr) return malformed();
            ++info.observed_frames;
            ApngFrame frame;
            frame.width = Be32(&file[data + 4]);
            frame.height = Be32(&file[data + 8]);
            frame.x = Be32(&file[data + 12]);
            frame.y = Be32(&file[data + 16]);
            const uint16_t num = Be16(&file[data + 20]);
            uint16_t den = Be16(&file[data + 22]);
            if (!den) den = 100;
            const uint32_t ms = static_cast<uint32_t>(static_cast<uint64_t>(num) * 1000u / den);
            // Browsers show "as fast as possible" frames at 100 ms.
            frame.delay_ms = ms <= 10 ? 100u : (std::min)(ms, 60000u);
            frame.dispose = file[data + 24] <= 2 ? file[data + 24] : 0;
            frame.blend = file[data + 25] <= 1 ? file[data + 25] : 0;
            if (!frame.width || !frame.height || frame.x > info.width || frame.y > info.height ||
                frame.width > info.width - frame.x || frame.height > info.height - frame.y)
                return malformed();
            if (info.frames.size() >= kMaxFrames) {
                info.incomplete_reason = L"frame-limit";
                current = nullptr;
                pos = data + length + 4;
                continue;
            }
            info.frames.push_back(std::move(frame));
            current = &info.frames.back();
            current_uses_idat = !seen_idat;
        } else if (IsType(type, "IDAT")) {
            seen_idat = true;
            if (current && current_uses_idat) current->data.emplace_back(data, length);
        } else if (IsType(type, "fdAT")) {
            if (current && !current_uses_idat && length > 4) current->data.emplace_back(data + 4, length - 4);
        } else if (IsType(type, "IEND")) {
            ended = true;
            break;
        } else if (!seen_idat && !IsType(type, "tEXt") && !IsType(type, "zTXt") &&
                   !IsType(type, "iTXt") && !IsType(type, "eXIf")) {
            // PLTE, tRNS, gAMA, cHRM, sRGB, iCCP, sBIT ... shared by every frame.
            info.header_chunks.emplace_back(pos, static_cast<size_t>(length) + 12);
        }
        pos = data + length + 4;
    }
    if (!have_ihdr || !have_actl || !info.width || !info.height) return false;
    if (!ended && info.incomplete_reason.empty()) info.incomplete_reason = L"truncated-data";
    if (info.observed_frames != info.declared_frames && info.incomplete_reason.empty())
        info.incomplete_reason = L"frame-count-mismatch";
    const auto empty = std::find_if(info.frames.begin(), info.frames.end(), [](const ApngFrame& f) { return f.data.empty(); });
    if (empty != info.frames.end()) {
        info.frames.erase(empty, info.frames.end());
        if (info.incomplete_reason.empty()) info.incomplete_reason = L"truncated-data";
    }
    if (!info.frames.empty()) {
        // The first frame has no previous picture to restore.
        if (info.frames[0].dispose == 2) info.frames[0].dispose = 1;
    }
    return info.frames.size() >= 2;
}

std::wstring MakeImageFramesPayload(std::wstring_view type, uint32_t loaded, uint32_t declared,
                                   uint32_t selected, std::wstring_view reason) {
    std::wstring result = L"PULSEIMAGE\t1\nT\t" + std::wstring(type) + L"\nF\t" + std::to_wstring(loaded) +
        L"\t" + std::to_wstring(declared) + L"\t" + std::to_wstring(selected) + L"\n";
    if (!reason.empty()) result += L"W\t" + std::wstring(reason) + L"\n";
    return result;
}

std::vector<uint8_t> BuildApngFramePng(const std::vector<uint8_t>& file, const ApngInfo& info, size_t index) {
    std::vector<uint8_t> out;
    if (index >= info.frames.size() || info.ihdr + 13 > file.size()) return out;
    const ApngFrame& frame = info.frames[index];
    size_t total = 8 + 25 + 12;
    for (const auto& c : info.header_chunks) total += c.second;
    for (const auto& d : frame.data) total += d.second + 12;
    out.reserve(total);
    out.insert(out.end(), kPngSignature, kPngSignature + 8);
    uint8_t ihdr[13];
    std::memcpy(ihdr, &file[info.ihdr], 13);
    for (int i = 0; i < 4; ++i) {
        ihdr[i] = static_cast<uint8_t>(frame.width >> (24 - 8 * i));
        ihdr[4 + i] = static_cast<uint8_t>(frame.height >> (24 - 8 * i));
    }
    PutChunk(out, "IHDR", ihdr, 13);
    for (const auto& c : info.header_chunks) out.insert(out.end(), file.begin() + static_cast<std::ptrdiff_t>(c.first),
                                                        file.begin() + static_cast<std::ptrdiff_t>(c.first + c.second));
    for (const auto& d : frame.data) PutChunk(out, "IDAT", &file[d.first], d.second);
    PutChunk(out, "IEND", nullptr, 0);
    return out;
}

bool ParseIconDirectory(const std::vector<uint8_t>& file, std::vector<IconEntry>& entries, bool& cursor) {
    entries.clear();
    cursor = false;
    if (file.size() < 6 || Le16(&file[0]) != 0) return false;
    const uint16_t type = Le16(&file[2]);
    const uint16_t count = Le16(&file[4]);
    if ((type != 1 && type != 2) || !count || file.size() < 6u + 16u * count) return false;
    cursor = type == 2;
    for (uint16_t i = 0; i < count && i < 256; ++i) {
        const uint8_t* e = &file[6 + 16 * i];
        IconEntry entry;
        entry.frame = i;
        entry.width = e[0] ? e[0] : 256;
        entry.height = e[1] ? e[1] : 256;
        entry.bits = cursor ? 0 : Le16(e + 6);
        entry.size = Le32(e + 8);
        entry.offset = Le32(e + 12);
        if (entry.offset >= file.size() || entry.size > file.size() - entry.offset || entry.size < 16) continue;
        const uint8_t* img = &file[entry.offset];
        if (std::memcmp(img, kPngSignature, 8) == 0) {
            entry.png = true;
            if (entry.size >= 8 + 8 + 13 && IsType(img + 12, "IHDR")) {
                entry.width = Be32(img + 16);
                entry.height = Be32(img + 20);
                const uint8_t depth = img[24], colour = img[25];
                const uint32_t channels = colour == 6 ? 4 : colour == 2 ? 3 : colour == 4 ? 2 : 1;
                entry.bits = colour == 3 ? depth : depth * channels;
            }
        } else if (entry.size >= 40 && Le32(img) >= 40) {
            // BITMAPINFOHEADER: height counts the AND mask too.
            const uint16_t bits = Le16(img + 14);
            if (bits) entry.bits = bits;
        }
        if (!entry.width || !entry.height || entry.width > 4096 || entry.height > 4096) continue;
        entries.push_back(entry);
    }
    return !entries.empty();
}

bool MakeIconEntryFile(const std::vector<uint8_t>& file, const IconEntry& entry,
                      bool cursor, std::vector<uint8_t>& out) {
    out.clear();
    const size_t directory = 6ull + 16ull * entry.frame;
    if (file.size() < 6 || entry.frame >= Le16(file.data() + 4) ||
        directory > file.size() || file.size() - directory < 16 ||
        entry.offset > file.size() || entry.size > file.size() - entry.offset || entry.size < 16)
        return false;
    out = {0, 0, 1, 0, 1, 0};
    out.insert(out.end(), file.begin() + directory, file.begin() + directory + 16);
    out[18] = 22; out[19] = out[20] = out[21] = 0;
    if (cursor) {
        out[10] = 1; out[11] = 0;
        out[12] = static_cast<uint8_t>(entry.bits);
        out[13] = static_cast<uint8_t>(entry.bits >> 8);
    }
    out.insert(out.end(), file.begin() + entry.offset, file.begin() + entry.offset + entry.size);
    return true;
}

size_t DefaultIconEntry(const std::vector<IconEntry>& entries) {
    size_t best = 0;
    for (size_t i = 1; i < entries.size(); ++i) {
        const IconEntry& a = entries[i];
        const IconEntry& b = entries[best];
        const uint64_t area_a = static_cast<uint64_t>(a.width) * a.height;
        const uint64_t area_b = static_cast<uint64_t>(b.width) * b.height;
        if (area_a > area_b || (area_a == area_b && a.bits > b.bits)) best = i;
    }
    return best;
}

std::wstring MakeIconSizesPayload(const std::vector<IconEntry>& entries, size_t selected) {
    std::wstring s = L"PULSEICO\t1\nS\t" + std::to_wstring(selected) + L"\n";
    for (const IconEntry& e : entries) {
        s += L"E\t" + std::to_wstring(e.width) + L"\t" + std::to_wstring(e.height) + L"\t" +
             std::to_wstring(e.bits) + L"\t" + (e.png ? L"1" : L"0") + L"\n";
    }
    return s;
}

} // namespace pulse::preview

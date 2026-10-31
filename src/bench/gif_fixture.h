#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace pulse_test::gif_fixture {
using Bytes = std::vector<uint8_t>;
inline void Word(Bytes& out, uint32_t n) { out.push_back(static_cast<uint8_t>(n)); out.push_back(static_cast<uint8_t>(n >> 8)); }
inline void Frame(Bytes& out, uint32_t x, uint32_t width, uint32_t height, uint8_t color, uint8_t disposal, bool transparent = false, uint32_t y = 0) {
    out.insert(out.end(), {0x21, 0xf9, 4, static_cast<uint8_t>((disposal << 2) | (transparent ? 1 : 0)), 10, 0, 3, 0, 0x2c});
    Word(out, x); Word(out, y); Word(out, width); Word(out, height); out.push_back(0);
    Bytes data; uint32_t bits = 0; unsigned used = 0;
    auto code = [&](uint32_t value) {
        bits |= value << used; used += 3;
        while (used >= 8) { data.push_back(static_cast<uint8_t>(bits)); bits >>= 8; used -= 8; }
    };
    for (uint32_t i = 0; i < width * height; ++i) { code(4); code(color); }
    code(5); if (used) data.push_back(static_cast<uint8_t>(bits));
    out.push_back(2);
    for (size_t at = 0; at < data.size();) {
        const auto n = (std::min)(size_t(255), data.size() - at);
        out.push_back(static_cast<uint8_t>(n)); out.insert(out.end(), data.begin() + at, data.begin() + at + n); at += n;
    }
    out.push_back(0);
}
}

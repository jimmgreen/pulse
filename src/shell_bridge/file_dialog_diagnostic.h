#pragma once
#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace pulse::shell_bridge {
struct SetterDiagnostic {
    enum class Kind { Unknown, Extension, Chunk, Filter, Shape, Prominent, Text } kind = Kind::Unknown;
    std::array<uint64_t, 10> values{};
    size_t units = 0;
};
inline SetterDiagnostic ParseSetterDiagnostic(std::string_view text) noexcept {
    SetterDiagnostic out;
    if (text.size() > 127) return out;
    const auto number = [&](std::string_view prefix, uint64_t& value, int base = 10) {
        if (!text.starts_with(prefix)) return false;
        text.remove_prefix(prefix.size());
        if (text.empty() || !((text.front() >= '0' && text.front() <= '9') || (base == 16 && ((text.front() >= 'a' && text.front() <= 'f') || (text.front() >= 'A' && text.front() <= 'F'))))) return false;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
        if (result.ec != std::errc{}) return false;
        text.remove_prefix(static_cast<size_t>(result.ptr - text.data()));
        return true;
    };
    auto& v = out.values;
    if (number("extension seq=", v[0])) {
        if (number(" available=", v[1]) && number(" units=", v[2]) && number(" redacted=", v[3]) && number(" truncated=", v[4]) && text.empty() && v[0] < 256 && v[1] <= 1 && v[3] <= 1 && v[4] <= 1) {
            out.kind = SetterDiagnostic::Kind::Extension;
        } else if (number(" offset=", v[1]) && text.starts_with(" utf16=") && v[0] < 256 && v[1] < 128 && v[1] % 8 == 0) {
            text.remove_prefix(7);
            while (!text.empty() && out.units < 8) {
                if (text.size() < 6 || !text.starts_with("\\u")) return {};
                uint64_t unit = 0;
                const auto parsed = std::from_chars(text.data() + 2, text.data() + 6, unit, 16);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + 6 || unit == '/' || unit == '\\' || unit == ':') return {};
                v[2 + out.units++] = unit;
                text.remove_prefix(6);
            }
            if (text.empty() && out.units) out.kind = SetterDiagnostic::Kind::Chunk;
        }
    } else if (number("filter seq=", v[0]) && number(" index=", v[1]) && number(" units=", v[2]) && number(" star=", v[3]) && number(" all=", v[4]) && number(" semicolon=", v[5]) && text.empty() && v[0] < 256 && v[3] <= 1 && v[4] <= 1 && v[5] <= 1) {
        out.kind = SetterDiagnostic::Kind::Filter;
    } else if (number("filter-shape seq=", v[0]) && number(" index=", v[1]) && number(" extension_stars=", v[2]) && number(" questions=", v[3]) && number(" forbidden=", v[4]) && text.empty() && v[0] < 256) {
        out.kind = SetterDiagnostic::Kind::Shape;
    }
    if (out.kind == SetterDiagnostic::Kind::Unknown && number("prominent id=", v[0]) &&
        number(" kind=", v[1]) && number(" parent=", v[2]) && number(" nested=", v[3]) &&
        number(" capture=", v[4]) && number(" feature=", v[5]) && number(" hr=", v[6]) && text.empty() &&
        v[0] <= UINT32_MAX && v[1] <= UINT32_MAX && v[2] <= UINT32_MAX && v[3] <= 1 && v[4] <= 1 && v[5] <= 1 && v[6] <= UINT32_MAX)
        out.kind = SetterDiagnostic::Kind::Prominent;
    if (out.kind == SetterDiagnostic::Kind::Unknown && number("text-snapshot setting=", v[0]) &&
        number(" null=", v[1]) && number(" units=", v[2]) && number(" captured=", v[3]) &&
        number(" hr=0x", v[4], 16) && text.empty() && v[0] <= UINT32_MAX && v[1] <= 1 && v[2] <= 32769 && v[3] <= 1 && v[4] <= UINT32_MAX)
        out.kind = SetterDiagnostic::Kind::Text;
    return out;
}
}

#pragma once
#include <cstdint>
#include <string_view>

namespace pulse::shell_bridge {

// Structural diagnostics only: never copy caller filenames or paths into logs.
enum DialogParameterShape : uint32_t {
    ParameterSeparator = 1u << 0,
    ParameterColon = 1u << 1,
    ParameterWildcard = 1u << 2,
    ParameterLeadingDot = 1u << 3,
    ParameterStarDot = 1u << 4,
    ParameterSemicolon = 1u << 5,
    ParameterWhitespace = 1u << 6,
    ParameterControl = 1u << 7,
    ParameterQuoteOrReserved = 1u << 8,
    ParameterTrailingDot = 1u << 9,
    ParameterInternalDot = 1u << 10,
    ParameterUnc = 1u << 11,
};

inline uint32_t FileDialogParameterShape(std::u16string_view text) noexcept {
    uint32_t shape = 0;
    if (text.starts_with(u".")) shape |= ParameterLeadingDot;
    if (text.starts_with(u"*.")) shape |= ParameterStarDot;
    if (text.ends_with(u".")) shape |= ParameterTrailingDot;
    if (text.starts_with(u"\\\\")) shape |= ParameterUnc;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = text[i];
        if (c == u'\\' || c == u'/') shape |= ParameterSeparator;
        if (c == u':') shape |= ParameterColon;
        if (c == u'*' || c == u'?') shape |= ParameterWildcard;
        if (c == u';') shape |= ParameterSemicolon;
        if (c == u' ' || c == u'\t' || c == u'\r' || c == u'\n') shape |= ParameterWhitespace;
        if (c < 32) shape |= ParameterControl;
        if (c == u'"' || c == u'<' || c == u'>' || c == u'|') shape |= ParameterQuoteOrReserved;
        if (c == u'.' && i != 0 && i + 1 < text.size()) shape |= ParameterInternalDot;
    }
    return shape;
}

} // namespace pulse::shell_bridge

#pragma once

#include "file_name_breaks.h"
#include "typography.h"
#include "ui_compositor.h"

namespace pulse::ui::icon_name {

using NameLine = FileNameLine;
// Same break rules as the wrapped rename field (file_name_breaks.h).
// Breaks are measured with plain DirectWrite advances, as the rename field
// measures them, so a name keeps its line breaks when editing starts.
inline std::vector<NameLine> WrapIconName(IDWriteFactory2* factory, IDWriteTextFormat* fmt,
                                   const std::wstring& s, float width, size_t limit) {
    return WrapFileName([&](const std::wstring& t) { return typography::MeasureAdvance(factory, fmt, t); },
                        s, width, limit);
}

struct IconNameLayout {
    std::wstring shown;
    struct Piece { size_t shown, source, length; };
    std::vector<Piece> pieces;   // runs of `shown` copied from the name
    bool truncated = false;
};

inline IconNameLayout LayoutIconName(Compositor* compositor, IDWriteFactory2* factory, IDWriteTextFormat* fmt,
                              const std::wstring& name, float width, int max_lines) {
    IconNameLayout out;
    const auto measure = [&](const std::wstring& s) {
        return typography::MeasureLine(compositor, fmt, s);
    };
    const float budget = std::max(1.0f, width - 1.0f);
    const size_t limit = static_cast<size_t>(std::max(1, max_lines));
    const auto lines = WrapIconName(factory, fmt, name, budget, limit);
    const auto add = [&](size_t source, size_t length) {
        out.pieces.push_back({out.shown.size(), source, length});
        out.shown.append(name, source, length);
    };
    const auto trimmed = [&](const NameLine& line) {
        size_t n = line.length;
        while (n > 0 && name[line.start + n - 1] == L' ') --n;
        return n;
    };
    const size_t kept = std::min(lines.size(), lines.size() <= limit ? limit : limit - 1);
    for (size_t i = 0; i < kept; ++i) {
        if (i) out.shown.push_back(L'\u2028');
        add(lines[i].start, trimmed(lines[i]));
    }
    if (lines.size() <= limit) return out;
    // Last line: "head…tail", the tail being the extension and up to six
    // characters before it - where versions, sizes and dates usually are.
    out.truncated = true;
    const size_t rest_at = lines[limit - 1].start;
    const std::wstring rest = name.substr(rest_at);
    size_t ext = 0;
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > rest_at && name.size() - dot <= 9) ext = name.size() - dot;
    const std::wstring ellipsis(1, L'\u2026');
    size_t tail_n = std::min(rest.size() - 1, ext + 6);
    while (tail_n > ext && measure(ellipsis + rest.substr(rest.size() - tail_n)) > budget * 0.6f) --tail_n;
    while (tail_n > 0 && measure(ellipsis + rest.substr(rest.size() - tail_n)) > budget) --tail_n;
    if (tail_n > 0 && IS_LOW_SURROGATE(rest[rest.size() - tail_n])) --tail_n;
    const std::wstring tail = rest.substr(rest.size() - tail_n);
    size_t lo = 0, hi = rest.size() - tail_n;
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (measure(rest.substr(0, mid) + ellipsis + tail) <= budget) lo = mid;
        else hi = mid - 1;
    }
    if (lo > 0 && IS_HIGH_SURROGATE(rest[lo - 1])) --lo;
    if (kept) out.shown.push_back(L'\u2028');
    add(rest_at, lo);
    out.shown += ellipsis;
    add(name.size() - tail_n, tail_n);
    return out;
}

} // namespace pulse::ui::icon_name

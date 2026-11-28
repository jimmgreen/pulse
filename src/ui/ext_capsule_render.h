// ext_capsule_render.h - Direct2D / DirectWrite measurement, multi-line layout,
// and crisp rounded-pill rendering for inline filename extension capsules.
#pragma once

#include "ext_capsule.h"
#include "icon_name_layout.h"
#include "name_highlight.h"
#include "typography.h"
#include "ui_compositor.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace pulse::ui::ext_capsule {

inline D2D1_COLOR_F ArgbToColorF(uint32_t argb, float alpha_scale = 1.0f) noexcept {
    const float a = static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;
    const float r = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
    const float g = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
    const float b = static_cast<float>(argb & 0xFFu) / 255.0f;
    return D2D1::ColorF(r, g, b, std::clamp(a * alpha_scale, 0.0f, 1.0f));
}

inline IDWriteTextFormat* CapsuleTextFormat(IDWriteFactory2* factory, IDWriteTextFormat* base_fmt,
                                            float scale, const Style& style) {
    if (!factory || !base_fmt) return nullptr;
    const float base_size = base_fmt->GetFontSize();
    const float cap_size = std::max(9.0f * scale, base_size + style.font_size_delta_dip * scale);
    const auto weight = static_cast<DWRITE_FONT_WEIGHT>(std::clamp(style.font_weight, 100u, 900u));
    const long size_key = std::lround(cap_size * 100.0f);

    struct Cached {
        ComPtr<IDWriteTextFormat> base;
        ComPtr<IDWriteTextFormat> format;
        long size_key = 0;
        DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
        uint64_t generation = 0;
    };
    static thread_local Cached cache;
    const uint64_t gen = typography::Generation();
    if (cache.generation == gen && cache.base.get() == base_fmt && cache.size_key == size_key &&
        cache.weight == weight && cache.format.get()) {
        return cache.format.get();
    }

    wchar_t family[128]{};
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    if (FAILED(base_fmt->GetFontFamilyName(family, ARRAYSIZE(family))) ||
        FAILED(base_fmt->GetLocaleName(locale, ARRAYSIZE(locale)))) {
        return nullptr;
    }
    ComPtr<IDWriteFontCollection> collection;
    base_fmt->GetFontCollection(&collection);
    ComPtr<IDWriteTextFormat> created;
    if (FAILED(factory->CreateTextFormat(family, collection.get(), weight, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, cap_size, locale, &created)) ||
        !created.get()) {
        return nullptr;
    }
    created->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    created->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    created->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    ComPtr<IDWriteTextFormat1> src1, dst1;
    if (SUCCEEDED(base_fmt->QueryInterface(__uuidof(IDWriteTextFormat1), reinterpret_cast<void**>(&src1))) &&
        SUCCEEDED(created->QueryInterface(__uuidof(IDWriteTextFormat1), reinterpret_cast<void**>(&dst1)))) {
        ComPtr<IDWriteFontFallback> fallback;
        if (SUCCEEDED(src1->GetFontFallback(&fallback)) && fallback.get()) dst1->SetFontFallback(fallback.get());
    }
    cache.base.reset();
    base_fmt->AddRef();
    cache.base.p = base_fmt;
    cache.format = std::move(created);
    cache.size_key = size_key;
    cache.weight = weight;
    cache.generation = gen;
    return cache.format.get();
}

struct CapsuleMetrics {
    float width = 0.0f;
    float height = 0.0f;
    float gap = 0.0f;
    float text_width = 0.0f;
};

inline CapsuleMetrics MeasureCapsule(Compositor* compositor, IDWriteFactory2* factory,
                                     IDWriteTextFormat* base_fmt, const Match& match, float scale) {
    CapsuleMetrics out;
    if (!factory || !base_fmt || match.label.empty()) return out;
    IDWriteTextFormat* cap_fmt = CapsuleTextFormat(factory, base_fmt, scale, match.style);
    if (!cap_fmt) cap_fmt = base_fmt;

    float text_w = 0.0f;
    float text_h = cap_fmt->GetFontSize() * 1.25f;
    ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(factory->CreateTextLayout(match.label.c_str(), static_cast<UINT32>(match.label.size()),
                                            cap_fmt, 4096.0f, 4096.0f, &layout)) && layout.get()) {
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        // Subtle tracking for crisp uppercase labels like "PSD" / "AEP".
        if (match.label.size() > 1 && match.label[0] != L'.') {
            ComPtr<IDWriteTextLayout1> layout1;
            if (SUCCEEDED(layout->QueryInterface(__uuidof(IDWriteTextLayout1), reinterpret_cast<void**>(&layout1)))) {
                const float tracking = cap_fmt->GetFontSize() * 0.03f;
                layout1->SetCharacterSpacing(0.0f, tracking, 0.0f,
                                             {0u, static_cast<UINT32>(match.label.size())});
            }
        }
        DWRITE_TEXT_METRICS m{};
        if (SUCCEEDED(layout->GetMetrics(&m))) {
            text_w = std::max(m.width, m.widthIncludingTrailingWhitespace);
            text_h = m.height;
        }
    } else {
        text_w = typography::MeasureLine(compositor, cap_fmt, match.label);
    }

    const float pad_x = match.style.padding_x_dip * scale;
    const float pad_y = match.style.padding_y_dip * scale;
    out.text_width = text_w;
    out.width = std::ceil(text_w + 2.0f * pad_x);
    out.height = std::ceil(text_h + 2.0f * pad_y);
    out.gap = std::round(match.style.gap_dip * scale);
    return out;
}

inline void DrawCapsule(ID2D1DeviceContext* dc, Compositor* compositor, IDWriteFactory2* factory,
                        IDWriteTextFormat* base_fmt, const Match& match, const CapsuleMetrics& metrics,
                        float x, float center_y, float scale, float alpha = 1.0f) {
    if (!dc || !compositor || !factory || !base_fmt || metrics.width <= 1.0f || metrics.height <= 1.0f) return;
    const float w = std::round(metrics.width);
    const float h = std::round(metrics.height);
    const float left = std::round(x);
    const float top = std::round(center_y - h * 0.5f);
    const D2D1_RECT_F rect = D2D1::RectF(left, top, left + w, top + h);
    const float radius = std::min(match.style.corner_radius_dip * scale, std::min(w, h) * 0.5f);

    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(dc->CreateSolidColorBrush(ArgbToColorF(match.bg_argb, alpha), &brush))) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.get());
    }
    if (match.border_argb != 0 && match.style.border_width_dip > 0.0f) {
        const float bw = std::max(1.0f, std::round(match.style.border_width_dip * scale));
        const float inset = bw * 0.5f;
        const D2D1_RECT_F stroke_rc = D2D1::RectF(rect.left + inset, rect.top + inset,
                                                  rect.right - inset, rect.bottom - inset);
        const float sr = std::max(0.0f, radius - inset);
        ComPtr<ID2D1SolidColorBrush> border_brush;
        if (SUCCEEDED(dc->CreateSolidColorBrush(ArgbToColorF(match.border_argb, alpha), &border_brush))) {
            dc->DrawRoundedRectangle(D2D1::RoundedRect(stroke_rc, sr, sr), border_brush.get(), bw);
        }
    }

    IDWriteTextFormat* cap_fmt = CapsuleTextFormat(factory, base_fmt, scale, match.style);
    if (!cap_fmt) cap_fmt = base_fmt;
    ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(factory->CreateTextLayout(match.label.c_str(), static_cast<UINT32>(match.label.size()),
                                            cap_fmt, w, h, &layout)) && layout.get()) {
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (match.label.size() > 1 && match.label[0] != L'.') {
            ComPtr<IDWriteTextLayout1> layout1;
            if (SUCCEEDED(layout->QueryInterface(__uuidof(IDWriteTextLayout1), reinterpret_cast<void**>(&layout1)))) {
                const float tracking = cap_fmt->GetFontSize() * 0.03f;
                layout1->SetCharacterSpacing(0.0f, tracking, 0.0f,
                                             {0u, static_cast<UINT32>(match.label.size())});
            }
        }
        // Subtle baseline snap for crisp rendering on Windows ClearType/grayscale AA.
        float y_snap = top;
        DWRITE_TEXT_METRICS tm{};
        DWRITE_LINE_METRICS lm{};
        UINT32 line_count = 0;
        if (SUCCEEDED(layout->GetMetrics(&tm)) && SUCCEEDED(layout->GetLineMetrics(&lm, 1, &line_count)) &&
            line_count > 0) {
            const float baseline = top + tm.top + lm.baseline;
            y_snap += std::round(baseline) - baseline;
        }
        ComPtr<ID2D1SolidColorBrush> text_brush;
        if (SUCCEEDED(dc->CreateSolidColorBrush(ArgbToColorF(match.text_argb, alpha), &text_brush))) {
            compositor->DrawTextLayout(D2D1::Point2F(left, y_snap), layout.get(), text_brush.get(),
                                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
}

// Filters full-filename match ranges down to the stem prefix [0, stem_len).
inline std::vector<NameMatchRange> StemMatchRanges(const std::vector<NameMatchRange>& matches, size_t stem_len) {
    std::vector<NameMatchRange> out;
    if (matches.empty() || stem_len == 0) return out;
    out.reserve(matches.size());
    for (const auto& m : matches) {
        if (m.start >= stem_len) continue;
        const size_t end = std::min<size_t>(stem_len, static_cast<size_t>(m.start) + m.length);
        if (end > m.start) out.push_back({m.start, static_cast<UINT32>(end - m.start)});
    }
    return out;
}

// Multi-line icon-grid layout for `[stem]` followed inline by `[capsule]`:
// - Prefix lines (0..N-2) are joined by U+2028 in `prefix_text`.
// - The final line holds `last_line` + `gap` + `[capsule]`, centered as one unit.
// - If `stem` exceeds `max_lines` or its final line leaves no room for the capsule,
//   the final line truncates the stem with an end ellipsis while keeping the capsule intact.
struct IconCapsuleLayout {
    std::wstring prefix_text;
    std::vector<NameMatchRange> prefix_matches;
    std::wstring last_line;
    std::vector<NameMatchRange> last_matches;
    int total_lines = 1;
    bool truncated = false;
};

inline IconCapsuleLayout LayoutIconNameWithCapsule(Compositor* compositor, IDWriteFactory2* factory,
                                                   IDWriteTextFormat* fmt, const std::wstring& stem,
                                                   float width, int max_lines, float capsule_w, float gap,
                                                   const std::vector<NameMatchRange>& stem_matches) {
    IconCapsuleLayout out;
    if (!factory || !fmt || stem.empty()) return out;
    const float budget = std::max(1.0f, width - 1.0f);
    const size_t limit = static_cast<size_t>(std::max(1, max_lines));
    const auto measure = [&](const std::wstring& s) {
        return typography::MeasureLine(compositor, fmt, s);
    };
    const auto lines = icon_name::WrapIconName(factory, fmt, stem, budget, limit);
    const auto trimmed_len = [&](const FileNameLine& line) {
        size_t n = line.length;
        while (n > 0 && stem[line.start + n - 1] == L' ') --n;
        return n;
    };
    const auto map_matches = [&](size_t dst_offset, size_t src_start, size_t src_len,
                                 std::vector<NameMatchRange>& dst) {
        for (const auto& m : stem_matches) {
            const size_t a = std::max<size_t>(m.start, src_start);
            const size_t b = std::min<size_t>(static_cast<size_t>(m.start) + m.length, src_start + src_len);
            if (a < b) dst.push_back({static_cast<UINT32>(dst_offset + a - src_start), static_cast<UINT32>(b - a)});
        }
    };

    auto append_prefix = [&](const FileNameLine& line) {
        if (!out.prefix_text.empty()) out.prefix_text.push_back(L'\u2028');
        const size_t dst_off = out.prefix_text.size();
        const size_t n = trimmed_len(line);
        out.prefix_text.append(stem, line.start, n);
        map_matches(dst_off, line.start, n, out.prefix_matches);
    };

    const float last_budget = std::max(0.0f, budget - capsule_w - gap);

    if (lines.size() <= limit) {
        // Stem itself fits within `limit` lines; check whether the capsule fits on the last line.
        const auto& last = lines.back();
        const size_t last_n = trimmed_len(last);
        const std::wstring last_str = stem.substr(last.start, last_n);
        if (measure(last_str) <= last_budget + 0.5f) {
            for (size_t i = 0; i + 1 < lines.size(); ++i) append_prefix(lines[i]);
            out.last_line = last_str;
            map_matches(0, last.start, last_n, out.last_matches);
            out.total_lines = static_cast<int>(lines.size());
            out.truncated = false;
            return out;
        }
        if (lines.size() < limit) {
            // Room for an extra line: place the capsule centered on the next line, no truncation.
            for (const auto& line : lines) append_prefix(line);
            out.last_line.clear();
            out.total_lines = static_cast<int>(lines.size() + 1);
            out.truncated = false;
            return out;
        }
    }

    // Truncate on line `limit - 1` so `head…` + `gap` + `[capsule]` fits inside `budget`.
    out.truncated = true;
    out.total_lines = static_cast<int>(limit);
    const size_t prefix_count = std::min(lines.size(), limit - 1);
    for (size_t i = 0; i < prefix_count; ++i) append_prefix(lines[i]);

    const size_t rest_at = prefix_count < lines.size() ? lines[prefix_count].start : stem.size();
    const std::wstring rest = rest_at < stem.size() ? stem.substr(rest_at) : std::wstring();
    const std::wstring ellipsis(1, L'\u2026');
    if (last_budget < measure(ellipsis)) {
        // Cell is too narrow even for "… + capsule"; keep capsule alone on the last line.
        out.last_line.clear();
        return out;
    }
    size_t lo = 0, hi = rest.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (measure(rest.substr(0, mid) + ellipsis) <= last_budget) lo = mid;
        else hi = mid - 1;
    }
    if (lo > 0 && IS_HIGH_SURROGATE(rest[lo - 1])) --lo;
    out.last_line = rest.substr(0, lo) + ellipsis;
    map_matches(0, rest_at, lo, out.last_matches);
    return out;
}

}  // namespace pulse::ui::ext_capsule

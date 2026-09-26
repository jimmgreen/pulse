#include "name_highlight.h"
#include "../index/filename_pinyin.h"
#include <algorithm>
#include <cwctype>

namespace pulse::ui {
namespace {
std::vector<NameMatchRange> RangesFromMask(const std::vector<bool>& mask) {
    std::vector<NameMatchRange> ranges;
    for (size_t i = 0; i < mask.size();) {
        if (!mask[i]) { ++i; continue; }
        const size_t start = i;
        while (i < mask.size() && mask[i]) ++i;
        ranges.push_back({static_cast<UINT32>(start), static_cast<UINT32>(i - start)});
    }
    return ranges;
}
}

std::vector<index::Term> NameHighlightTerms(std::wstring_view filter, std::wstring_view search) {
    std::vector<index::Term> terms;
    // Pane filters join non-tag tokens into one filename pattern, unlike search's AND tokens.
    std::wstring filter_name;
    for (size_t i = 0; i < filter.size();) {
        while (i < filter.size() && std::iswspace(filter[i])) ++i;
        const size_t start = i;
        while (i < filter.size() && !std::iswspace(filter[i])) ++i;
        const auto token = filter.substr(start, i - start);
        if (token.empty() || (token.front() == L'#' && token.size() > 1)) continue;
        if (!filter_name.empty()) filter_name += L' ';
        filter_name += token;
    }
    if (!filter_name.empty()) {
        index::Term term;
        term.name = index::Fold(filter_name);
        term.name_how = filter_name.find_first_of(L"*?") == std::wstring::npos
            ? index::NameHow::Substring : index::NameHow::Wildcard;
        terms.push_back(std::move(term));
    }
    const auto query = index::ParseQuery(search);
    for (const auto& group : query.groups) for (const auto& term : group) {
        if (term.name.empty() || term.name_not || term.name_in_path ||
            term.name_how == index::NameHow::Any || term.name.find(L':') != std::wstring::npos) continue;
        terms.push_back(term);
    }
    return terms;
}

std::vector<NameMatchRange> NameMatchRanges(std::wstring_view name, const std::vector<index::Term>& terms) {
    if (name.empty() || terms.empty()) return {};
    std::vector<bool> mask(name.size(), false);
    const auto folded = index::Fold(name);
    for (const auto& term : terms) {
        if (!index::MatchName(name.data(), static_cast<uint32_t>(name.size()), term)) continue;
        if (term.pinyin && term.name_how == index::NameHow::Substring &&
            folded.find(term.name) == std::wstring::npos) {
            const auto match = index::FindPinyinMatch(name, term.name);
            if (match && match.start + match.length <= mask.size())
                std::fill(mask.begin() + match.start, mask.begin() + match.start + match.length, true);
        }
        for (size_t i = 0; i < term.name.size();) {
            const size_t start = i;
            if (term.name_how == index::NameHow::Wildcard) {
                while (i < term.name.size() && term.name[i] != L'*' && term.name[i] != L'?') ++i;
            } else i = term.name.size();
            const auto literal = term.name.substr(start, i - start);
            if (!literal.empty()) for (size_t at = 0; (at = folded.find(literal, at)) != std::wstring::npos; ++at) {
                std::fill(mask.begin() + at, mask.begin() + at + literal.size(), true);
            }
            if (i < term.name.size()) ++i;
        }
    }
    return RangesFromMask(mask);
}

std::vector<NameMatchRange> VisibleNameMatchRanges(std::wstring_view original, std::wstring_view shown,
                                                const std::vector<NameMatchRange>& ranges) {
    if (ranges.empty()) return {};
    if (original == shown) return ranges;
    size_t prefix = 0, suffix = 0;
    while (prefix < original.size() && prefix < shown.size() && original[prefix] == shown[prefix]) ++prefix;
    while (suffix < original.size() - prefix && suffix < shown.size() - prefix &&
           original[original.size() - suffix - 1] == shown[shown.size() - suffix - 1]) ++suffix;
    std::vector<bool> mask(shown.size(), false);
    for (size_t i = 0; i < shown.size(); ++i) {
        const size_t source = i < prefix ? i : i >= shown.size() - suffix
            ? original.size() - (shown.size() - i) : original.size();
        for (const auto& range : ranges) if (source >= range.start && source - range.start < range.length) {
            mask[i] = true;
            break;
        }
    }
    return RangesFromMask(mask);
}

void ApplyNameHighlightPadding(IDWriteTextLayout* layout,
                               const std::vector<NameMatchRange>& ranges, float scale) {
    if (!layout || ranges.empty()) return;
    ComPtr<IDWriteTextLayout1> spaced;
    if (FAILED(layout->QueryInterface(__uuidof(IDWriteTextLayout1), reinterpret_cast<void**>(&spaced.p)))) return;
    const float padding = kNameHighlightPaddingDip * scale;
    for (const auto& range : ranges) {
        if (!range.length) continue;
        if (range.length == 1) {
            spaced->SetCharacterSpacing(padding, padding, 0, {range.start, 1});
        } else {
            spaced->SetCharacterSpacing(padding, 0, 0, {range.start, 1});
            spaced->SetCharacterSpacing(0, padding, 0, {range.start + range.length - 1, 1});
        }
    }
}

void DrawNameHighlightBackground(Compositor* compositor, IDWriteTextLayout* layout,
                                 D2D1_POINT_2F origin, const D2D1_RECT_F& clip,
                                 const std::vector<NameMatchRange>& ranges, const Theme& theme, float scale) {
    if (!compositor || !layout || ranges.empty()) return;
    ApplyNameHighlightPadding(layout, ranges, scale);
    const bool dark = theme.bg.r < 0.5f;
    // Soft accent wash with an accent underline: the match stays readable
    // without the heavy amber box of earlier builds.
    ComPtr<ID2D1SolidColorBrush> background, foreground, underline;
    auto* dc = compositor->Dc();
    auto fill = theme.accent;
    fill.a = dark ? 0.24f : 0.18f;
    auto ink = dark ? HexColor(0xFFFFFF) : theme.text;
    ink.a = 1.0f;
    if (FAILED(dc->CreateSolidColorBrush(fill, &background)) ||
        FAILED(dc->CreateSolidColorBrush(ink, &foreground)) ||
        FAILED(dc->CreateSolidColorBrush(theme.accent, &underline))) return;
    const auto previous_aa = dc->GetAntialiasMode();
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (const auto& range : ranges) {
        UINT32 count = 0;
        layout->HitTestTextRange(range.start, range.length, origin.x, origin.y, nullptr, 0, &count);
        if (!count) continue;
        std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
        if (FAILED(layout->HitTestTextRange(range.start, range.length, origin.x, origin.y,
            metrics.data(), count, &count))) continue;
        for (const auto& hit : metrics) {
            if (!hit.isText || hit.isTrimmed || hit.width <= 0 || hit.height <= 0) continue;
            const float inset = std::min(0.5f * scale, hit.width * 0.1f);
            const auto rect = D2D1::RectF(hit.left + inset, hit.top + 1.0f * scale,
                hit.left + hit.width - inset, hit.top + hit.height - 1.0f * scale);
            const float radius = std::min(3.0f * scale, (rect.right - rect.left) * 0.5f);
            dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), background.get());
            const float line = std::max(1.0f, 2.0f * scale);
            dc->FillRectangle(D2D1::RectF(rect.left, rect.bottom - line, rect.right, rect.bottom), underline.get());
        }
        layout->SetDrawingEffect(foreground.get(), {range.start, range.length});
    }
    dc->PopAxisAlignedClip();
    dc->SetAntialiasMode(previous_aa);
}
}

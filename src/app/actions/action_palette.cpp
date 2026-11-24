// action_palette.cpp - The command palette's rows (action_registry.h).
//
// Matching ranks like the mock: whole title > title start > pinyin initials >
// contains (full pinyin ranks like the characters it spells); a choice's label
// and the id's English words ("view sort") match last. Actions run this session come first within
// a rank; unavailable ones go to the end, under their own heading.
#include "action_registry.h"

#include "../keymap.h"
#include "../../common/localization.h"
#include "../../index/filename_pinyin.h"
#include "../../ui/command_icons.h"

#include <algorithm>
#include <climits>
#include <cwctype>
#include <utility>

namespace pulse::app::actions {

namespace {

using I = l10n::StringId;

std::wstring Text(I id) { return l10n::Get(id).c_str(); }

std::wstring Fold(std::wstring_view text) {
    std::wstring out(text);
    for (wchar_t& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

std::wstring_view Trim(std::wstring_view text) {
    while (!text.empty() && std::iswspace(text.front())) text.remove_prefix(1);
    while (!text.empty() && std::iswspace(text.back())) text.remove_suffix(1);
    return text;
}

struct Match {
    int tier = 0;  // 0: no match
    uint16_t start = 0;
    uint16_t length = 0;
};

Match Make(int tier, size_t start, size_t length) {
    return {tier, static_cast<uint16_t>(start), static_cast<uint16_t>(length)};
}

Match MatchView(const ActionView& v, const std::wstring& needle) {
    const std::wstring title = Fold(v.title);
    if (title == needle) return Make(6, 0, title.size());
    if (title.starts_with(needle)) return Make(5, 0, needle.size());
    Match pinyin;
    if (index::PinyinEligible(needle)) {
        const index::PinyinMatch m = index::FindPinyinMatch(v.title, needle);
        if (m.kind == index::PinyinMatchKind::Full && m.start == 0) return Make(4, m.start, m.length);
        if (m.kind == index::PinyinMatchKind::Initials && m.start == 0) return Make(3, m.start, m.length);
        if (m) pinyin = Make(2, m.start, m.length);
    }
    const size_t at = title.find(needle);
    if (at != std::wstring::npos) return Make(2, at, needle.size());
    if (pinyin.tier) return pinyin;
    // A choice's label finds its action (四宫 → 分栏布局 ›).
    for (const auto& choice : v.choices)
        if (Fold(choice.text).find(needle) != std::wstring::npos) return Make(1, 0, 0);
    std::wstring words;
    for (const char c : v.id) words.push_back(c == '.' || c == '_' ? L' ' : static_cast<wchar_t>(c));
    words.push_back(L' ');
    for (const char c : v.key) words.push_back(c == '_' ? L' ' : static_cast<wchar_t>(c));
    if (words.find(needle) != std::wstring::npos) return Make(1, 0, 0);
    return {};
}

// 常用: fixed, so the list stays where the hand expects it.
constexpr std::pair<std::string_view, std::string_view> kCommon[] = {
    {"tab.new", ""}, {"view.sort", ""}, {"view.mode", ""}, {"window.reopen", ""},
    {"settings.set", "show_hidden"}, {"select.all", ""}, {"app.settings_page", ""},
    {"app.shortcut_help", ""},
};
constexpr size_t kMaxUsed = 5;
constexpr size_t kMaxMixed = 3;
constexpr size_t kMaxRows = 40;
constexpr size_t kMaxAskChars = 40;

ui::FluentMenuItem Heading(std::wstring text) {
    ui::FluentMenuItem row;
    row.heading = true;
    row.enabled = false;
    row.text = std::move(text);
    return row;
}

} // namespace

int ActionMatchTier(const ActionView& view, std::wstring_view needle) {
    const std::wstring folded = Fold(Trim(needle));
    return folded.empty() ? 0 : MatchView(view, folded).tier;
}

ui::FluentMenuItem PaletteRow(const ActionView& view) {
    ui::FluentMenuItem row;
    row.command = PaletteCommand(view.index);
    row.text = view.title;
    row.glyph = view.glyph;
    row.pictogram = view.pictogram;
    row.enabled = view.enabled;
    if (!view.enabled) {
        row.shortcut = view.why;
        return row;
    }
    row.shortcut = view.shortcut.empty() ? view.value : view.shortcut;
    for (size_t i = 0; i < view.choices.size() && i + 1 < static_cast<size_t>(kPaletteStride); ++i) {
        ui::FluentMenuItem choice = view.choices[i];
        if (choice.command == 0) choice.command = PaletteCommand(view.index, static_cast<int>(i));
        choice.children.clear();
        row.children.push_back(std::move(choice));
    }
    if (!row.children.empty()) row.command = 0;  // a flyout header returns no command
    return row;
}

std::vector<ui::FluentMenuItem> PaletteRows(const std::vector<ActionView>& views,
                                            std::wstring_view raw_needle, PaletteMode mode,
                                            const std::vector<int>& used, bool offer_assistant) {
    std::vector<ui::FluentMenuItem> rows;
    const std::wstring_view typed = Trim(raw_needle);
    const std::wstring needle = Fold(typed);
    const auto by_index = [&](int index) -> const ActionView* {
        for (const auto& v : views)
            if (v.index == index) return &v;
        return nullptr;
    };

    if (needle.empty()) {
        if (mode != PaletteMode::Commands) return rows;
        std::vector<int> shown;
        for (const int index : used) {
            const ActionView* v = by_index(index);
            if (!v || shown.size() >= kMaxUsed) continue;
            if (shown.empty()) rows.push_back(Heading(Text(I::ActPalUsed)));
            rows.push_back(PaletteRow(*v));
            shown.push_back(index);
        }
        bool headed = false;
        for (const auto& [id, key] : kCommon) {
            for (const auto& v : views) {
                if (v.id != id || v.key != key) continue;
                if (std::find(shown.begin(), shown.end(), v.index) == shown.end()) {
                    if (!headed) rows.push_back(Heading(Text(I::ActPalCommon)));
                    headed = true;
                    rows.push_back(PaletteRow(v));
                }
                break;
            }
        }
        return rows;
    }

    struct Hit {
        const ActionView* view;
        Match match;
        int used_rank;
    };
    std::vector<Hit> hits;
    for (const auto& v : views) {
        if (!v.palette) continue;
        const Match m = MatchView(v, needle);
        if (!m.tier) continue;
        const auto it = std::find(used.begin(), used.end(), v.index);
        hits.push_back({&v, m, it == used.end() ? INT_MAX : static_cast<int>(it - used.begin())});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.view->enabled != b.view->enabled) return a.view->enabled;
        if (a.match.tier != b.match.tier) return a.match.tier > b.match.tier;
        return a.used_rank < b.used_rank;
    });
    const auto row_of = [](const Hit& h) {
        ui::FluentMenuItem row = PaletteRow(*h.view);
        row.match_start = h.match.start;
        row.match_length = h.match.length;
        return row;
    };

    if (mode == PaletteMode::Mixed) {
        for (const Hit& h : hits) {
            if (!h.view->enabled || rows.size() >= kMaxMixed) break;
            rows.push_back(row_of(h));
        }
        return rows;
    }

    if (hits.empty()) {
        ui::FluentMenuItem none;
        none.text = Text(I::ActPalNoCommand);
        none.enabled = false;
        none.separator_after = offer_assistant;
        rows.push_back(std::move(none));
        if (!offer_assistant) return rows;
        std::wstring said(typed);
        if (said.size() > kMaxAskChars) said = said.substr(0, kMaxAskChars) + L"\x2026";
        ui::FluentMenuItem ask;
        ask.command = kPaletteAskAssistant;
        ask.text = Text(I::ActPalAskAssistant);
        const size_t at = ask.text.find(L"%s");
        if (at != std::wstring::npos) ask.text.replace(at, 2, said);
        ask.glyph = ui::command_icons::kGlyphSparkle;
        ask.accent_glyph = true;
        ask.shortcut = ShownShortcut(L"Ctrl+J");
        rows.push_back(std::move(ask));
        return rows;
    }

    std::vector<std::wstring> domains;  // in the order their best match ranks
    for (const Hit& h : hits)
        if (h.view->enabled && std::find(domains.begin(), domains.end(), h.view->domain) == domains.end())
            domains.push_back(h.view->domain);
    size_t count = 0;
    for (const auto& domain : domains) {
        if (count >= kMaxRows) break;
        rows.push_back(Heading(domain));
        for (const Hit& h : hits) {
            if (!h.view->enabled || h.view->domain != domain || count >= kMaxRows) continue;
            rows.push_back(row_of(h));
            ++count;
        }
    }
    bool headed = false;
    for (const Hit& h : hits) {
        if (h.view->enabled || count >= kMaxRows) continue;
        if (!headed) rows.push_back(Heading(Text(I::ActPalUnavailable)));
        headed = true;
        rows.push_back(row_of(h));
        ++count;
    }
    return rows;
}

} // namespace pulse::app::actions

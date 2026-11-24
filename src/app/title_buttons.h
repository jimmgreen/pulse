// title_buttons.h - The title bar's button group (ui_modes_mockup "pill"):
// a comma list of ids in display order, "-id" hidden, "|" a divider.
#pragma once
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

// win 窗口, layout 界面, settings 设置, theme 切换主题, ai AI 助手.
inline constexpr std::array<std::wstring_view, 5> kTitleButtonIds{L"win", L"layout", L"settings", L"theme", L"ai"};

struct TitleButton {
    std::wstring id;       // empty for a divider
    bool shown = true;
    bool Divider() const noexcept { return id.empty(); }
};

inline bool IsTitleButtonId(std::wstring_view id) noexcept {
    for (const auto known : kTitleButtonIds) if (known == id) return true;
    return false;
}

// Unknown and repeated ids go, dividers are kept (at most one in a row), and
// every known id appears once: missing ones are appended hidden, so a spec
// written by an older build still lists everything the settings page shows.
inline std::vector<TitleButton> ParseTitleButtons(std::wstring_view spec) {
    std::vector<TitleButton> out;
    size_t start = 0;
    while (start <= spec.size()) {
        const size_t comma = spec.find(L',', start);
        std::wstring_view part = spec.substr(start, comma == std::wstring_view::npos ? std::wstring_view::npos : comma - start);
        while (!part.empty() && part.front() == L' ') part.remove_prefix(1);
        while (!part.empty() && part.back() == L' ') part.remove_suffix(1);
        if (part == L"|") {
            if (out.empty() || !out.back().Divider()) out.push_back({});
        } else if (!part.empty()) {
            const bool hidden = part.front() == L'-';
            if (hidden) part.remove_prefix(1);
            bool seen = false;
            for (const auto& b : out) seen = seen || b.id == part;
            if (IsTitleButtonId(part) && !seen) out.push_back({std::wstring(part), !hidden});
        }
        if (comma == std::wstring_view::npos) break;
        start = comma + 1;
    }
    for (const auto known : kTitleButtonIds) {
        bool seen = false;
        for (const auto& b : out) seen = seen || b.id == known;
        if (!seen) out.push_back({std::wstring(known), false});
    }
    return out;
}

inline std::wstring FormatTitleButtons(const std::vector<TitleButton>& buttons) {
    std::wstring out;
    for (const auto& b : buttons) {
        if (!out.empty()) out += L',';
        out += b.Divider() ? std::wstring(L"|") : (b.shown ? L"" : L"-") + b.id;
    }
    return out;
}

inline std::wstring NormalizeTitleButtons(std::wstring_view spec) { return FormatTitleButtons(ParseTitleButtons(spec)); }

// What the title bar draws: shown buttons, dividers only between two of them.
inline std::vector<TitleButton> VisibleTitleButtons(std::wstring_view spec) {
    std::vector<TitleButton> out;
    for (auto& b : ParseTitleButtons(spec)) {
        if (!b.Divider() && !b.shown) continue;
        if (b.Divider() && (out.empty() || out.back().Divider())) continue;
        out.push_back(std::move(b));
    }
    while (!out.empty() && out.back().Divider()) out.pop_back();
    return out;
}

} // namespace pulse::app

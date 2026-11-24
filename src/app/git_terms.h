#pragma once
// Shared wording helpers for Git UI: switches cleanly between plain user-friendly
// terms (default) and technical Git terms using generated StringId entries.
#include "../common/localization.h"
#include <string>
#include <string_view>

namespace pulse::app {

inline const std::wstring& GitStr(bool plain_terms, l10n::StringId plain_id, l10n::StringId git_id) {
    return l10n::Get(plain_terms ? plain_id : git_id);
}

inline std::wstring GitFmt(std::wstring text, std::wstring_view key, const std::wstring& value) {
    size_t pos = 0;
    while ((pos = text.find(key, pos)) != std::wstring::npos) {
        text.replace(pos, key.size(), value);
        pos += value.size();
    }
    return text;
}

inline std::wstring GitFmtCount(std::wstring text, int n) {
    return GitFmt(std::move(text), L"{n}", std::to_wstring(n));
}

}  // namespace pulse::app

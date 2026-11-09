#pragma once
#include "../index/filename_pinyin.h"
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {
// Prepare once for a filter pass. Matching never allocates a folded filename.
class NamePattern {
public:
    explicit NamePattern(std::wstring_view pattern, bool pinyin = false) : pattern_(pattern) {
        for (auto& c : pattern_) c = Fold(c);
        wildcard_ = pattern_.find_first_of(L"*?") != std::wstring::npos;
        pinyin_ = pinyin && !wildcard_ && index::PinyinEligible(pattern_);
        if (!wildcard_) {
            prefix_.resize(pattern_.size());
            for (size_t i = 1, matched = 0; i < pattern_.size(); ++i) {
                while (matched && pattern_[i] != pattern_[matched]) matched = prefix_[matched - 1];
                if (pattern_[i] == pattern_[matched]) ++matched;
                prefix_[i] = matched;
            }
        }
    }

    bool Matches(std::wstring_view name) const {
        if (pattern_.empty()) return true;
        if (!wildcard_) {
            size_t matched = 0;
            for (const wchar_t raw : name) {
                const wchar_t c = Fold(raw);
                while (matched && c != pattern_[matched]) matched = prefix_[matched - 1];
                if (c == pattern_[matched]) ++matched;
                if (matched == pattern_.size()) return true;
            }
            return pinyin_ && static_cast<bool>(index::FindPinyinMatch(name, pattern_));
        }
        size_t si = 0, pi = 0, star = std::wstring::npos, match = 0;
        while (si < name.size()) {
            if (pi < pattern_.size() && pattern_[pi] == L'*') {
                star = pi++;
                match = si;
            } else if (pi < pattern_.size() && (pattern_[pi] == L'?' || pattern_[pi] == Fold(name[si]))) {
                ++si;
                ++pi;
            } else if (star != std::wstring::npos) {
                pi = star + 1;
                si = ++match;
            } else {
                return false;
            }
        }
        while (pi < pattern_.size() && pattern_[pi] == L'*') ++pi;
        return pi == pattern_.size();
    }
private:
    static wchar_t Fold(wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); }
    std::wstring pattern_;
    std::vector<size_t> prefix_;
    bool wildcard_ = false;
    bool pinyin_ = false;
};
}

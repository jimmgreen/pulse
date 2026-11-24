#pragma once
// A set of folder paths answering "is this path one of them or inside one?"
// with the exact rule of StartsWithPath (case-insensitive like _wcsnicmp, a
// boundary at '\', '/' or the end), in O(path length) instead of a scan over
// every folder in the set. Each boundary of the queried path between the
// shortest and longest member length is looked up by its towlower-folded text,
// the folding _wcsnicmp applies. Small sets keep the plain list scan: below
// kLinearLimit members it is cheaper than folding the query.
#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <cwchar>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace pulse::ops {

class PathPrefixSet {
public:
    void Add(const std::wstring& folder) {
        if (list_.size() < kLinearLimit) list_.push_back(folder);
        std::wstring key = Fold(folder, folder.size());
        shortest_ = keys_.empty() ? key.size() : (std::min)(shortest_, key.size());
        longest_ = (std::max)(longest_, key.size());
        keys_.insert(std::move(key));
    }
    bool empty() const { return keys_.empty(); }
    // Case-insensitive equality with a member (_wcsicmp), no descendants.
    bool ContainsExactly(const std::wstring& path) const {
        return !keys_.empty() && keys_.contains(std::wstring_view(Fold(path, path.size())));
    }
    size_t size() const { return keys_.size(); }
    bool Covers(const std::wstring& path) const {
        if (keys_.empty() || path.size() < shortest_) return false;
        if (keys_.size() < kLinearLimit) {
            for (const auto& prefix : list_) {
                if (path.size() >= prefix.size() &&
                    _wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) == 0 &&
                    (path.size() == prefix.size() || path[prefix.size()] == L'\\' || path[prefix.size()] == L'/'))
                    return true;
            }
            return false;
        }
        const size_t limit = (std::min)(path.size(), longest_);
        const std::wstring folded = Fold(path, limit);
        for (size_t length = shortest_; length <= limit; ++length) {
            if (length < path.size() && path[length] != L'\\' && path[length] != L'/') continue;
            if (keys_.contains(std::wstring_view(folded.data(), length))) return true;
        }
        return false;
    }

private:
    static constexpr size_t kLinearLimit = 16;
    struct Hash {
        using is_transparent = void;
        size_t operator()(std::wstring_view text) const { return std::hash<std::wstring_view>{}(text); }
    };
    static std::wstring Fold(const std::wstring& text, size_t count) {
        std::wstring folded(text, 0, count);
        for (auto& ch : folded) ch = static_cast<wchar_t>(towlower(ch));
        return folded;
    }
    std::unordered_set<std::wstring, Hash, std::equal_to<>> keys_;
    std::vector<std::wstring> list_;  // first kLinearLimit members, as added
    size_t shortest_ = 0;
    size_t longest_ = 0;
};

}  // namespace pulse::ops

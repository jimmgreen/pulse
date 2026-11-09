#pragma once
#include <algorithm>
#include <string>
#include <vector>
#include <windows.h>

namespace pulse::tag_rules_detail {
inline bool SameRoot(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
enum class EnqueueResult { Duplicate, Added, Full };
inline EnqueueResult EnqueueHit(std::vector<std::wstring>& hits, const std::wstring& path) {
    if (std::any_of(hits.begin(), hits.end(), [&](const auto& old) { return SameRoot(old, path); }))
        return EnqueueResult::Duplicate;
    if (hits.size() >= 1024) return EnqueueResult::Full;
    hits.push_back(path);
    return EnqueueResult::Added;
}
inline bool CoveredRoot(const std::wstring& path, const std::wstring& root) {
    return SameRoot(path, root) || (path.size() > root.size() &&
        CompareStringOrdinal(path.data(), static_cast<int>(root.size()), root.data(),
                             static_cast<int>(root.size()), TRUE) == CSTR_EQUAL &&
        (root.back() == L'\\' || path[root.size()] == L'\\'));
}
inline std::vector<std::wstring> MinimalRoots(std::vector<std::wstring> paths) {
    std::vector<std::wstring> roots;
    for (auto& path : paths) {
        std::replace(path.begin(), path.end(), L'/', L'\\');
        while (path.size() > 3 && path.back() == L'\\') path.pop_back();
        if (path.empty() || std::any_of(roots.begin(), roots.end(), [&](const auto& root) {
                return CoveredRoot(path, root);
            })) continue;
        std::erase_if(roots, [&](const auto& root) { return CoveredRoot(root, path); });
        roots.push_back(std::move(path));
    }
    return roots;
}
}

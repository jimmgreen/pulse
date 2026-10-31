#pragma once
#include "path_utils.h"
#include <algorithm>
#include <vector>
namespace pulse::path {
inline bool OnVolume(std::wstring_view path, std::wstring_view root) {
    auto p = StripExtendedPathPrefix(path);
    auto r = StripExtendedPathPrefix(root);
    std::replace(p.begin(), p.end(), L'/', L'\\');
    std::replace(r.begin(), r.end(), L'/', L'\\');
    while (!r.empty() && r.back() == L'\\') r.pop_back();
    return !r.empty() && p.size() >= r.size() && EqualInsensitive(p.substr(0, r.size()), r) &&
           (p.size() == r.size() || p[r.size()] == L'\\');
}
inline bool OnAnyVolume(std::wstring_view path, const std::vector<std::wstring>& roots) {
    return std::any_of(roots.begin(), roots.end(), [&](const auto& root) { return OnVolume(path, root); });
}
}

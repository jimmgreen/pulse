#pragma once
#include "../index/index_config.h"
#include "../index/network_index.h"
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

enum class LiveFolderSearchKind : uint8_t {
    None,
    Network,
    Local,
};

inline bool IsVirtualScopePath(std::wstring_view path) {
    return path.starts_with(L"pulse:");
}

inline std::wstring NormalizeLiveScopeKey(std::wstring path) {
    if (path.empty()) return path;
    if (path.size() >= 8 &&
        CompareStringOrdinal(path.data(), 8, L"\\\\?\\UNC\\", 8, TRUE) == CSTR_EQUAL) {
        path = L"\\\\" + path.substr(8);
    } else if (path.starts_with(L"\\\\?\\")) {
        path.erase(0, 4);
    }
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.size() == 2 && iswalpha(path[0]) && path[1] == L':') {
        path.push_back(L'\\');
    }
    const bool unc = path.starts_with(L"\\\\");
    while (path.back() == L'\\' && ((!unc && path.size() > 3) || (unc && path.size() > 2))) {
        path.pop_back();
    }
    for (wchar_t& ch : path) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return path;
}

inline bool LiveScopePathUnder(std::wstring_view path, std::wstring_view root) {
    if (path.empty() || root.empty()) return false;
    return path == root || (path.size() > root.size() && path.starts_with(root) &&
        (root.ends_with(L'\\') || path[root.size()] == L'\\'));
}

inline bool IsPathCoveredByLocalIndex(
    bool index_ready,
    bool scope_known,
    const std::vector<index::VolumeInfo>& volumes,
    const std::vector<std::wstring>& excluded,
    const std::wstring& path,
    bool* explicitly_disabled = nullptr,
    bool* excluded_by_rule = nullptr) {
    if (explicitly_disabled) *explicitly_disabled = false;
    if (excluded_by_rule) *excluded_by_rule = false;
    const std::wstring scope = NormalizeLiveScopeKey(path);
    if (scope.empty()) return false;
    for (const auto& ex : excluded) {
        const std::wstring ex_key = NormalizeLiveScopeKey(ex);
        if (!ex_key.empty() && LiveScopePathUnder(scope, ex_key)) {
            if (excluded_by_rule) *excluded_by_rule = true;
            return false;
        }
    }
    const index::VolumeInfo* best = nullptr;
    size_t best_len = 0;
    for (const auto& vol : volumes) {
        const std::wstring mount = NormalizeLiveScopeKey(vol.mount_point);
        if (mount.empty() || !LiveScopePathUnder(scope, mount)) continue;
        if (mount.size() >= best_len) {
            best = &vol;
            best_len = mount.size();
        }
    }
    if (scope_known && best && best->supported && !best->enabled) {
        if (explicitly_disabled) *explicitly_disabled = true;
        return false;
    }
    if (!index_ready || !scope_known || !best) return false;
    return best->online && best->supported && best->enabled;
}

inline LiveFolderSearchKind ClassifyLiveFolderSearch(
    bool index_ready,
    bool scope_known,
    const std::vector<index::VolumeInfo>& volumes,
    const std::vector<std::wstring>& excluded,
    const std::vector<index::NetworkRootInfo>& network_roots,
    const std::wstring& path_prefix,
    bool isolated_test = false) {
    if (path_prefix.empty() || IsVirtualScopePath(path_prefix)) {
        return LiveFolderSearchKind::None;
    }
    if (index::IsNetworkFolderPath(path_prefix)) {
        return index::NetworkRootsCover(network_roots, path_prefix, true)
            ? LiveFolderSearchKind::None
            : LiveFolderSearchKind::Network;
    }
    if (isolated_test && !scope_known) {
        return LiveFolderSearchKind::None;
    }
    return IsPathCoveredByLocalIndex(index_ready, scope_known, volumes, excluded, path_prefix)
        ? LiveFolderSearchKind::None
        : LiveFolderSearchKind::Local;
}

inline bool ShouldAllowUncoveredContentScope(
    bool index_ready,
    bool scope_known,
    const std::vector<index::VolumeInfo>& volumes,
    const std::vector<std::wstring>& excluded,
    const std::wstring& scope_path,
    bool isolated_test = false) {
    if (scope_path.empty() || IsVirtualScopePath(scope_path) ||
        index::IsNetworkFolderPath(scope_path)) {
        return false;
    }
    if (isolated_test && !scope_known) {
        return false;
    }
    bool explicitly_disabled = false;
    bool excluded_by_rule = false;
    const bool covered = IsPathCoveredByLocalIndex(
        index_ready, scope_known, volumes, excluded, scope_path,
        &explicitly_disabled, &excluded_by_rule);
    if (covered) return true;
    return !explicitly_disabled && !excluded_by_rule;
}

} // namespace pulse::app

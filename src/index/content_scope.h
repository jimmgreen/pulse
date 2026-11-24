#pragma once
#include "content_index.h"
#include "index_config.h"
#include <algorithm>
#include <cwctype>

namespace pulse::index {
inline std::wstring ContentScopeKey(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    std::transform(path.begin(), path.end(), path.begin(), towlower);
    return path;
}
inline bool ContentPathUnder(std::wstring_view path, std::wstring_view root) {
    return path == root || (path.size() > root.size() && path.starts_with(root) &&
        (root.ends_with(L'\\') || path[root.size()] == L'\\'));
}
// Intersect configured coverage with the request before opening any directory.
inline std::vector<std::wstring> ContentTaskRoots(const ContentIndexConfig& config,
                                                 const ContentSearchRequest& request) {
    std::vector<std::wstring> requested = request.roots;
    if (requested.empty() && !request.root.empty()) requested.push_back(request.root);
    if (requested.empty()) for (const auto& root : config.roots) requested.push_back(root.path);
    std::vector<std::wstring> roots;
    for (auto path : requested) {
        path = ContentScopeKey(std::move(path));
        if (!request.root.empty()) {
            const auto single = ContentScopeKey(request.root);
            if (path != single && !request.recursive) continue;
            if (ContentPathUnder(single, path)) path = single;
            else if (!ContentPathUnder(path, single)) continue;
        }
        bool added_from_config = false;
        for (const auto& configured : config.roots) {
            const auto scope = ContentScopeKey(configured.path);
            std::wstring effective;
            if (ContentPathUnder(path, scope)) effective = path;
            else if (request.recursive && ContentPathUnder(scope, path)) effective = scope;
            else continue;
            added_from_config = true;
            if (std::any_of(roots.begin(), roots.end(), [&](const auto& old) {
                    return effective == old || (request.recursive && ContentPathUnder(effective, old)); })) continue;
            if (request.recursive) std::erase_if(roots, [&](const auto& old) { return ContentPathUnder(old, effective); });
            roots.push_back(std::move(effective));
        }
        if (!added_from_config && request.allow_uncovered_root && !request.root.empty()) {
            const auto uncovered = ContentScopeKey(request.root);
            if (ContentPathUnder(path, uncovered)) {
                const auto effective = path;
                if (!std::any_of(roots.begin(), roots.end(), [&](const auto& old) {
                        return effective == old || (request.recursive && ContentPathUnder(effective, old)); })) {
                    if (request.recursive) std::erase_if(roots, [&](const auto& old) { return ContentPathUnder(old, effective); });
                    roots.push_back(effective);
                }
            }
        }
    }
    return roots;
}
inline bool ContentPathExcluded(const ContentIndexConfig& config, const std::wstring& path) {
    const auto key = ContentScopeKey(path);
    for (const auto& name : config.excluded_directories) {
        const auto part = L"\\" + ContentScopeKey(name) + L"\\";
        if ((key + L"\\").find(part) != std::wstring::npos) return true;
    }
    return std::any_of(config.excluded_paths.begin(), config.excluded_paths.end(), [&](const auto& excluded) {
        return ContentPathUnder(key, ContentScopeKey(excluded));
    });
}
inline ContentIndexConfig SharedContentScope(const ContentIndexConfig& previous,
        const std::vector<VolumeInfo>& volumes, const std::vector<std::wstring>& excluded) {
    ContentIndexConfig config = previous;
    config.shared_scope = true;
    config.roots.clear();
    config.excluded_directories.clear();
    config.excluded_paths = excluded;
    auto add_volume = [&](const VolumeInfo& volume) {
        if (!volume.enabled || !volume.supported || volume.mount_point.empty()) return;
        const std::wstring key = ContentScopeKey(volume.mount_point);
        if (std::any_of(config.roots.begin(), config.roots.end(),
                        [&](const ContentIndexRoot& existing) { return existing.path == key; })) {
            return;
        }
        ContentIndexRoot root{key, config.default_encoding};
        for (const auto& old : previous.roots)
            if (ContentScopeKey(old.path) == root.path) root.encoding = old.encoding;
        config.roots.push_back(std::move(root));
    };
    for (const auto& volume : volumes) if (volume.online) add_volume(volume);
    for (const auto& volume : volumes) if (!volume.online) add_volume(volume);
    std::sort(config.roots.begin(), config.roots.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    for (auto& path : config.excluded_paths) path = ContentScopeKey(std::move(path));
    std::sort(config.excluded_paths.begin(), config.excluded_paths.end());
    config.excluded_paths.erase(std::unique(config.excluded_paths.begin(), config.excluded_paths.end()), config.excluded_paths.end());
    return config;
}
} // namespace pulse::index

#pragma once
#include "places.h"
#include <optional>
#include <string_view>

namespace pulse::app {
inline constexpr std::wstring_view kWorkspacePathPrefix=L"pulse:workspace:";

inline std::optional<size_t> WorkspacePathIndex(std::wstring_view path, size_t count) {
    if (!path.starts_with(kWorkspacePathPrefix)) return std::nullopt;
    path.remove_prefix(kWorkspacePathPrefix.size());
    if (path.empty()) return std::nullopt;
    size_t index=0;
    for (wchar_t ch:path) {
        if (ch<L'0' || ch>L'9' || index>=count) return std::nullopt;
        index=index*10+static_cast<size_t>(ch-L'0');
    }
    return index<count ? std::optional<size_t>(index) : std::nullopt;
}

// Workspace links are commands, never persistent pane locations. Old sessions
// may contain them: resolve roots without activating another workspace layout.
inline std::wstring ResolveWorkspaceFolderPath(std::wstring path, const std::vector<Workspace>& workspaces) {
    for (size_t hops=0; path.starts_with(kWorkspacePathPrefix); ++hops) {
        if (hops>=workspaces.size()) return {};
        const auto index=WorkspacePathIndex(path,workspaces.size());
        if (!index) return {};
        path=workspaces[*index].root;
    }
    return path;
}
} // namespace pulse::app

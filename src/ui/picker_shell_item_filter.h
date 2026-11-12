#pragma once
#include <windows.h>
#include <vector>
#include <utility>
namespace pulse::ui {
enum class ShellFilterCandidateRole { EnumeratedView, NavigationShortcut };
inline bool ShellFilterCandidateNeedsCallback(ShellFilterCandidateRole role) noexcept {
    // Only worker-enumerated view candidates have the current view's object role.
    // Navigation shortcuts may be stale/offline; do not bind/probe them on caller STA.
    return role == ShellFilterCandidateRole::EnumeratedView;
}
// Run only on the Show STA, after asynchronous enumeration and before publishing
// rows. Keep the original batch intact if a callback closes or pumps a
// navigation that makes the batch stale. Typed paths never pass through here.
template<class Item, class Path, class Include, class Current>
HRESULT FilterShellItems(std::vector<Item>& items, Path path, Include include, Current current,
    ShellFilterCandidateRole role = ShellFilterCandidateRole::EnumeratedView) {
    if (!ShellFilterCandidateNeedsCallback(role)) return S_OK;
    std::vector<size_t> accepted; accepted.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        if (!current()) return HRESULT_FROM_WIN32(ERROR_RETRY);
        const HRESULT result = include(path(items[i]));
        if (!current()) return HRESULT_FROM_WIN32(ERROR_RETRY);
        // Native IFileDialog oracle: only exact S_FALSE excludes an item.
        // FAILED(hr) and other positive codes still include it; Close wins via current().
        if (result != S_FALSE) accepted.push_back(i);
    }
    if (!current()) return HRESULT_FROM_WIN32(ERROR_RETRY);
    std::vector<Item> filtered; filtered.reserve(accepted.size());
    for (const auto index : accepted) filtered.push_back(std::move(items[index]));
    items = std::move(filtered); return S_OK;
}
}

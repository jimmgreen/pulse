#pragma once
#include "../common/path_utils.h"
#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pulse::app {
inline std::wstring GroupParentKey(std::wstring_view value) {
    auto key = path::StripExtendedPathPrefix(value);
    std::replace(key.begin(), key.end(), L'/', L'\\');
    while (key.size() > 3 && key.back() == L'\\') key.pop_back();
    return key;
}

// Grouping creates a sibling of the selected items, not a child of the
// currently displayed tree root. Different parents have no unambiguous target.
inline std::optional<std::wstring> GroupSelectionParent(std::span<const std::wstring> sources) {
    if (sources.size() < 2) return std::nullopt;
    std::wstring parent, key;
    for (const auto& source : sources) {
        auto full = source;
        std::replace(full.begin(), full.end(), L'/', L'\\');
        while (full.size() > 3 && full.back() == L'\\') full.pop_back();
        const auto ordinary = path::StripExtendedPathPrefix(full);
        const bool absolute_drive = ordinary.size() >= 3 && ordinary[1] == L':' && ordinary[2] == L'\\';
        if (!absolute_drive && !ordinary.starts_with(L"\\\\")) return std::nullopt;
        const auto slash = full.find_last_of(L'\\');
        if (slash == std::wstring::npos || slash + 1 == full.size()) return std::nullopt;
        const auto leaf = full.substr(slash + 1);
        if (leaf == L"." || leaf == L".." || leaf.find(L':') != std::wstring::npos ||
            full.find(L"\\.\\") != std::wstring::npos || full.find(L"\\..\\") != std::wstring::npos)
            return std::nullopt;
        auto candidate = full.substr(0, slash);
        if (!candidate.empty() && candidate.back() == L':') candidate += L'\\';
        const auto candidate_key = GroupParentKey(candidate);
        if (candidate_key.empty()) return std::nullopt;
        if (candidate_key == L"\\" || (candidate_key.starts_with(L"\\\\") &&
            candidate_key.find(L'\\', 2) == std::wstring::npos)) return std::nullopt;
        if (parent.empty()) { parent = std::move(candidate); key = candidate_key; }
        // Exact case is intentional: NTFS directories may be case-sensitive.
        else if (candidate_key != key) return std::nullopt;
    }
    return parent;
}

struct GroupSelectionPlan {
    std::wstring parent;
    std::wstring destination;
    std::vector<std::wstring> sources;
};

template<class UniqueName>
std::optional<GroupSelectionPlan> PlanGroupSelection(std::span<const std::wstring> sources, UniqueName unique_name) {
    auto parent = GroupSelectionParent(sources);
    if (!parent) return std::nullopt;
    const std::wstring name = unique_name(*parent);
    if (name.empty() || name == L"." || name == L".." || name.find_first_of(L"\\/:") != std::wstring::npos)
        return std::nullopt;
    auto destination = *parent;
    if (!destination.ends_with(L'\\')) destination += L'\\';
    destination += name;
    return GroupSelectionPlan{std::move(*parent), std::move(destination), {sources.begin(), sources.end()}};
}
}

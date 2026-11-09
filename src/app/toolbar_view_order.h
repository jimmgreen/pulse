#pragma once
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {
inline constexpr std::array<std::wstring_view, 3> kToolbarViewIds{L"sort", L"filter", L"group"};
inline int ToolbarViewIndex(std::wstring_view id) {
    for (size_t i = 0; i < kToolbarViewIds.size(); ++i)
        if (id == kToolbarViewIds[i]) return static_cast<int>(i);
    return -1;
}
inline std::vector<std::wstring> NormalizeToolbarViewOrder(const std::vector<std::wstring>& ids) {
    std::vector<std::wstring> result;
    std::array<bool, 3> placed{};
    for (const auto& id : ids) {
        const int index = ToolbarViewIndex(id);
        if (index >= 0 && !placed[index]) { result.push_back(id); placed[index] = true; }
    }
    for (size_t i = 0; i < placed.size(); ++i)
        if (!placed[i]) result.emplace_back(kToolbarViewIds[i]);
    return result;
}
inline std::array<int, 3> ToolbarViewLayoutOrder(const std::vector<std::wstring>& ids) {
    std::array<int, 3> result{};
    std::array<bool, 3> placed{};
    size_t count = 0;
    for (const auto& id : ids) {
        const int index = ToolbarViewIndex(id);
        if (index >= 0 && !placed[index]) { result[count++] = index; placed[index] = true; }
    }
    for (int index = 0; index < 3; ++index)
        if (!placed[index]) result[count++] = index;
    return result;
}
inline std::vector<std::wstring> ReorderVisibleToolbarViews(const std::vector<std::wstring>& current,
                                                           const std::vector<std::wstring>& shown) {
    auto result = NormalizeToolbarViewOrder(current);
    std::vector<std::wstring> ordered;
    std::array<bool, 3> present{};
    for (const auto& id : shown) {
        const int index = ToolbarViewIndex(id);
        if (index >= 0 && !present[index]) { ordered.push_back(id); present[index] = true; }
    }
    size_t next = 0;
    for (auto& id : result)
        if (present[ToolbarViewIndex(id)]) id = ordered[next++];
    return result;
}
}

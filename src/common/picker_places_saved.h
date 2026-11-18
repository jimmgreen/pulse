#pragma once
#include "picker_places_snapshot.h"
#include "json_utils.h"
#include "utf8_file.h"
#include <shlobj.h>

namespace pulse::picker_places {
namespace detail {
inline std::vector<std::wstring> SavedObjects(const std::wstring& text, const wchar_t* key) {
    std::vector<std::wstring> result;
    const auto pos = json::ValuePosition(text, key);
    if (pos == std::wstring::npos || text[pos] != L'[') return result;
    int arrays = 0, objects = 0;
    bool quoted = false;
    size_t begin = 0;
    for (size_t i = pos; i < text.size(); ++i) {
        const auto c = text[i];
        if (quoted) { if (c == L'\\') ++i; else if (c == L'"') quoted = false; continue; }
        if (c == L'"') quoted = true;
        else if (c == L'[') ++arrays;
        else if (c == L']') { if (--arrays == 0) break; }
        else if (c == L'{') { if (objects++ == 0) begin = i; }
        else if (c == L'}' && objects > 0 && --objects == 0) result.push_back(text.substr(begin, i - begin + 1));
    }
    return result;
}
}
inline Snapshot ParseSaved(const std::wstring& text) {
    Snapshot result;
    if (!json::ValidConfigObject(text, true)) return result;
    for (const auto& block : detail::SavedObjects(text, L"workspaces")) {
        Workspace w{json::ExtractString(block, L"root"), json::ExtractString(block, L"name"),
            static_cast<uint32_t>(json::ExtractInt(block, L"color_rgb", 0x4CC2FF)) & 0xFFFFFF};
        if (detail::DiskPath(w.root)) result.workspaces.push_back(std::move(w));
    }
    for (const auto& block : detail::SavedObjects(text, L"starred_groups")) {
        StarredGroup group{json::ExtractString(block, L"id"), json::ExtractString(block, L"name")};
        if (!group.id.empty() && !group.name.empty()) result.starred_groups.push_back(std::move(group));
    }
    auto add = [&](StarredItem item) {
        if (!detail::DiskPath(item.path)) return;
        if (std::any_of(result.starred_items.begin(), result.starred_items.end(), [&](const auto& old) {
            return _wcsicmp(old.path.c_str(), item.path.c_str()) == 0;
        })) return;
        if (item.kind == 1 || std::none_of(result.starred_groups.begin(), result.starred_groups.end(),
            [&](const auto& g) { return g.id == item.group; })) item.group.clear();
        result.starred_items.push_back(std::move(item));
    };
    for (const auto& block : detail::SavedObjects(text, L"starred_items")) {
        const auto kind = json::ExtractString(block, L"kind");
        add({json::ExtractString(block, L"path"), kind == L"folder" ? 2u : kind == L"file" ? 1u : 0u,
            json::ExtractString(block, L"group")});
    }
    if (json::ValuePosition(text, L"starred_items") == std::wstring::npos)
        for (const auto& path : json::ExtractStringArray(text, L"starred")) add({path, 0, {}});
    return result;
}
// Worker-only, read-only fallback. Never creates a directory or starts catalog writers.
inline Snapshot ReadSaved(const std::wstring& file = {}) {
    std::wstring path = file;
    if (path.empty()) {
        wchar_t local[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local))) return {};
        path = std::wstring(local) + L"\\Pulse\\places.json";
    }
    std::wstring text;
    return ReadUtf8File(path, text) ? ParseSaved(text) : Snapshot{};
}
}

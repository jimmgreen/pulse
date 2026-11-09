// toolbar_tools.h — the tools pinned to the toolbar (AppPrefs::toolbar_tools),
// in toolbar order, saved by stable id. Labels, glyphs and commands live in
// the catalog in app_commands.cpp (kToolbarTools, same order as the ids here).
//
// Prefs before this list had a 5-bit mask, toolbar_pins: bit i pinned
// kToolbarToolIds[i]. It is still written, from the list, so an older Pulse
// keeps the pins it knows.
#pragma once
#include <cstddef>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

// Saved in prefs: never rename or reorder. Append new tools at the end.
inline constexpr std::wstring_view kToolbarToolIds[] = {
    // The former toolbar_pins bits, in bit order.
    L"new_text", L"copy_path", L"terminal", L"properties", L"undo",
    // Toggles: each flips an existing setting (the toolbar shows it on).
    L"hidden", L"protected", L"compact", L"zebra", L"size_bar", L"smart_date",
    L"outline", L"sidebar", L"theme",
    // Toolbar picker T3d (the v3 prototype's whole catalog): the commands that
    // were fixed buttons (cut .. delete, split / details / columns), then the
    // rest of the prototype's tools Pulse has commands for.
    L"cut", L"copy", L"paste", L"rename", L"delete", L"split", L"details", L"columns",
    L"select_all", L"invert", L"select_wild", L"new_md", L"recent", L"apply_all",
    L"search_all", L"adv_search", L"recycle", L"map_drive", L"shortcuts", L"settings",
    // 压缩 (native zip) and the 文件夹大小 switch.
    L"zip", L"folder_size",
    // The 文件扩展名 switch.
    L"extensions",
};
inline constexpr size_t kToolbarToolCount = std::size(kToolbarToolIds);
inline constexpr size_t kLegacyToolbarPins = 5;
// Every tool may be pinned; the toolbar folds what has no room into More.
// Matches ui::kToolbarCommandPins.
inline constexpr size_t kMaxToolbarTools = kToolbarToolCount;
// toolbar_tools_v in app.json. Lists saved before 2 lack the former fixed
// buttons; UpgradeToolbarTools adds them back where they used to be.
inline constexpr int kToolbarToolsVersion = 2;

// A separator between two tools (自定义工具栏 → 在后面加分隔符). An older
// Pulse drops the id it does not know, so it keeps every tool.
inline bool IsNativeToolbarTool(std::wstring_view id) noexcept {
    if (!id.starts_with(L"native:")) return false;
    id.remove_prefix(7);
    const auto colon = id.find(L':');
    if (colon == std::wstring_view::npos || colon == 0 || colon > 64 || id.size() - colon - 1 == 0 || id.size() - colon - 1 > 64) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        if (i == colon) continue;
        const auto c = id[i];
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
              (c >= L'0' && c <= L'9') || c == L'.' || c == L'_' || c == L'-')) return false;
    }
    return true;
}
inline bool IsToolbarTool(std::wstring_view id) noexcept;

inline int ToolbarToolIndex(std::wstring_view id) noexcept;
inline constexpr std::wstring_view kToolbarRightGroupId = L"::right";
inline int ToolbarRightFrom(const std::vector<std::wstring>& tools) {
    int slot = 0;
    for (const auto& id : tools) {
        if (id == kToolbarRightGroupId) return slot;
        if (IsToolbarTool(id)) ++slot;
    }
    return -1;
}
inline constexpr std::array<std::wstring_view,3> kToolbarViewMarker{L"::sort",L"::filter",L"::group"};
inline std::array<int,3> ToolbarViewSlots(const std::vector<std::wstring>& tools) {
    std::array<int,3> slots{-1,-1,-1};int slot=0;
    for(const auto& id:tools) {
        for(int i=0;i<3;++i) if(id==kToolbarViewMarker[i]) slots[i]=slot;
        if(IsToolbarTool(id))++slot;
    }
    return slots;
}
inline std::vector<std::wstring> SetToolbarViewSlot(std::vector<std::wstring> tools,int view,int slot) {
    if(view<0 || view>=3)return tools;
    for(auto it=tools.begin();it!=tools.end();) {
        if(*it==kToolbarViewMarker[view])it=tools.erase(it);else ++it;
    }
    if(slot<0)return tools;
    int current=0;
    for(auto it=tools.begin();it!=tools.end();++it) {
        if(!IsToolbarTool(*it))continue;
        if(current++==slot){tools.insert(it,std::wstring(kToolbarViewMarker[view]));return tools;}
    }
    tools.emplace_back(kToolbarViewMarker[view]);return tools;
}
inline constexpr std::wstring_view kToolbarSeparatorId = L"|";
inline bool IsToolbarSeparator(std::wstring_view id) noexcept { return id == kToolbarSeparatorId; }

inline int ToolbarToolIndex(std::wstring_view id) noexcept {
    for (size_t i = 0; i < kToolbarToolCount; ++i)
        if (kToolbarToolIds[i] == id) return static_cast<int>(i);
    return -1;
}

inline bool IsToolbarTool(std::wstring_view id) noexcept {
    return ToolbarToolIndex(id) >= 0 || IsNativeToolbarTool(id);
}

// Known ids only, each once, at most kMaxToolbarTools, order kept.
// Separators only between two tools (none first, last or twice in a row);
// they do not count toward the limit.
inline std::vector<std::wstring> NormalizeToolbarTools(const std::vector<std::wstring>& tools) {
    std::vector<std::wstring> out;
    size_t count = 0;
    bool right_seen = false;
    std::array<bool,3> view_seen{};
    for (const auto& id : tools) {
        bool view_marker=false;
        for(int i=0;i<3;++i) if(id==kToolbarViewMarker[i]) {
            if(!view_seen[i])out.emplace_back(id);
            view_seen[i]=true;view_marker=true;
        }
        if(view_marker)continue;
        if (id == kToolbarRightGroupId) {
            if (!right_seen) out.emplace_back(id);
            right_seen = true;
            continue;
        }
        if (IsToolbarSeparator(id)) {
            if (!out.empty() && !IsToolbarSeparator(out.back())) out.emplace_back(kToolbarSeparatorId);
            continue;
        }
        if (count >= kMaxToolbarTools) break;
        if (!IsToolbarTool(id)) continue;
        bool seen = false;
        for (const auto& kept : out) seen = seen || kept == id;
        if (!seen) { out.push_back(id); ++count; }
    }
    while (!out.empty() && IsToolbarSeparator(out.back())) out.pop_back();
    return out;
}

// The tools alone, in order (a toolbar pin's slot indexes this).
inline std::vector<std::wstring> ToolbarToolsOnly(const std::vector<std::wstring>& tools) {
    std::vector<std::wstring> out;
    for (const auto& id : tools)
        if (IsToolbarTool(id)) out.push_back(id);
    return out;
}

// Bit k: a separator sits before the k-th tool.
inline uint64_t ToolbarSeparatorMask(const std::vector<std::wstring>& tools) noexcept {
    uint64_t mask = 0;
    size_t slot = 0;
    bool pending = false;
    for (const auto& id : tools) {
        if (IsToolbarSeparator(id)) { pending = slot > 0; continue; }
        if (!IsToolbarTool(id)) continue;
        if (pending && slot < 64) mask |= uint64_t{1} << slot;
        pending = false;
        ++slot;
    }
    return mask;
}

// The tools with a separator before each tool whose bit is set.
inline std::vector<std::wstring> WithToolbarSeparators(const std::vector<std::wstring>& tools, uint64_t mask) {
    std::vector<std::wstring> out;
    for (size_t i = 0; i < tools.size(); ++i) {
        if (i > 0 && i < 64 && (mask >> i & 1)) out.emplace_back(kToolbarSeparatorId);
        out.push_back(tools[i]);
    }
    return NormalizeToolbarTools(out);
}

inline std::vector<std::wstring> SetToolbarRightFrom(std::vector<std::wstring> tools, int slot) {
    for (auto it = tools.begin(); it != tools.end();) {
        if (*it == kToolbarRightGroupId) it = tools.erase(it); else ++it;
    }
    if (slot < 0) return tools;
    int current = 0;
    for (auto it = tools.begin(); it != tools.end(); ++it) {
        if (!IsToolbarTool(*it)) continue;
        if (current++ == slot) { tools.insert(it, std::wstring(kToolbarRightGroupId)); break; }
    }
    return tools;
}

inline std::vector<std::wstring> KeepToolbarViewSlots(const std::vector<std::wstring>& old,std::vector<std::wstring> changed) {
    const auto slots=ToolbarViewSlots(old);
    for(int i=0;i<3;++i)changed=SetToolbarViewSlot(std::move(changed),i,slots[i]);
    return changed;
}

// Moves the tool in slot `from` to slot `to`; the separators stay between
// the same slots, so the groups keep their place on the toolbar.
inline std::vector<std::wstring> MoveToolbarTool(const std::vector<std::wstring>& tools, size_t from, size_t to) {
    std::vector<std::wstring> only = ToolbarToolsOnly(tools);
    if (from >= only.size() || to >= only.size()) return tools;
    std::wstring id = std::move(only[from]);
    only.erase(only.begin() + static_cast<std::ptrdiff_t>(from));
    only.insert(only.begin() + static_cast<std::ptrdiff_t>(to), std::move(id));
    return KeepToolbarViewSlots(tools, SetToolbarRightFrom(WithToolbarSeparators(only, ToolbarSeparatorMask(tools)), ToolbarRightFrom(tools)));
}

// Takes the tool in `slot` off; a separator on either side of it stays.
inline std::vector<std::wstring> RemoveToolbarTool(const std::vector<std::wstring>& tools, size_t slot) {
    std::vector<std::wstring> only = ToolbarToolsOnly(tools);
    if (slot >= only.size()) return tools;
    const uint64_t mask = ToolbarSeparatorMask(tools);
    const uint64_t below = slot >= 64 ? mask : mask & ((uint64_t{1} << slot) - 1);
    const uint64_t above = slot >= 63 ? 0 : (mask >> 1) & ~((uint64_t{1} << slot) - 1);
    const uint64_t own = slot < 64 ? mask & (uint64_t{1} << slot) : 0;
    only.erase(only.begin() + static_cast<std::ptrdiff_t>(slot));
    const int right = ToolbarRightFrom(tools);
    return KeepToolbarViewSlots(tools, SetToolbarRightFrom(WithToolbarSeparators(only, below | above | own),
        right < 0 ? -1 : right - (static_cast<int>(slot) < right ? 1 : 0)));
}

// Adds (or removes) the separator after the tool in `slot`.
inline std::vector<std::wstring> SetToolbarSeparatorAfter(const std::vector<std::wstring>& tools, size_t slot, bool on) {
    const std::vector<std::wstring> only = ToolbarToolsOnly(tools);
    if (slot + 1 >= only.size() || slot + 1 >= 64) return tools;
    uint64_t mask = ToolbarSeparatorMask(tools);
    const uint64_t bit = uint64_t{1} << (slot + 1);
    mask = on ? mask | bit : mask & ~bit;
    return KeepToolbarViewSlots(tools, SetToolbarRightFrom(WithToolbarSeparators(only, mask), ToolbarRightFrom(tools)));
}

// After the picker hands over a new tool list (checked, unchecked or
// moved): a separator stays before the tool it stood before, or, when that
// tool left the toolbar, before the next one that is still there.
inline std::vector<std::wstring> KeepToolbarSeparators(const std::vector<std::wstring>& old_tools,
                                                       const std::vector<std::wstring>& tools) {
    const auto kept = [&](const std::wstring& id) {
        for (const auto& t : tools) if (t == id) return true;
        return false;
    };
    std::vector<std::wstring> before;
    bool pending = false;
    for (const auto& id : old_tools) {
        if (IsToolbarSeparator(id)) { pending = true; continue; }
        if (!IsToolbarTool(id) || !kept(id)) continue;
        if (pending) before.push_back(id);
        pending = false;
    }
    std::vector<std::wstring> out;
    for (const auto& id : tools) {
        if (IsToolbarSeparator(id)) continue;
        for (const auto& b : before)
            if (b == id) { out.emplace_back(kToolbarSeparatorId); break; }
        out.push_back(id);
    }
    return KeepToolbarViewSlots(old_tools,SetToolbarRightFrom(NormalizeToolbarTools(out), ToolbarRightFrom(old_tools)));
}

// The prototype's default toolbar (a fresh install).
inline std::vector<std::wstring> DefaultToolbarTools() {
    return {L"cut", L"copy", L"paste", L"rename", L"delete", std::wstring(kToolbarSeparatorId),
            L"hidden", L"split", L"details", L"columns"};
}

// A list saved before kToolbarToolsVersion: cut .. delete were fixed buttons
// ahead of the pins and split / details / columns sat after them, so they
// come back there and the toolbar looks as before.
inline std::vector<std::wstring> UpgradeToolbarTools(const std::vector<std::wstring>& tools) {
    const auto has = [&](std::wstring_view id) {
        for (const auto& t : tools) if (t == id) return true;
        return false;
    };
    std::vector<std::wstring> out;
    for (std::wstring_view id : {L"cut", L"copy", L"paste", L"rename", L"delete"})
        if (!has(id)) out.emplace_back(id);
    out.insert(out.end(), tools.begin(), tools.end());
    for (std::wstring_view id : {L"split", L"details", L"columns"})
        if (!has(id)) out.emplace_back(id);
    return NormalizeToolbarTools(out);
}

inline std::vector<std::wstring> ToolbarToolsFromLegacyPins(uint32_t pins) {
    std::vector<std::wstring> out;
    for (size_t i = 0; i < kLegacyToolbarPins; ++i)
        if (pins & (1u << i)) out.emplace_back(kToolbarToolIds[i]);
    return out;
}

inline uint32_t LegacyPinsFromToolbarTools(const std::vector<std::wstring>& tools) noexcept {
    uint32_t pins = 0;
    for (const auto& id : tools) {
        const int i = ToolbarToolIndex(id);
        if (i >= 0 && static_cast<size_t>(i) < kLegacyToolbarPins) pins |= 1u << i;
    }
    return pins;
}

} // namespace pulse::app

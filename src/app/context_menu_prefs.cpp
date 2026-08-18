// context_menu_prefs.cpp — JSON load/save for the Explorer fusion-zone prefs.
#include "context_menu_prefs.h"
#include "session.h"
#include "../common/json_utils.h"
#include <fstream>
#include <sstream>
#include <windows.h>

namespace pulse::app {
namespace {

std::wstring ExtractObject(const std::wstring& json, const std::wstring& key) {
    const std::wstring quoted = L"\"" + key + L"\"";
    size_t pos = json.find(quoted);
    if (pos == std::wstring::npos) return L"";
    pos = json.find(L'{', pos + quoted.size());
    if (pos == std::wstring::npos) return L"";
    int depth = 0;
    for (size_t i = pos; i < json.size(); ++i) {
        if (json[i] == L'{') ++depth;
        else if (json[i] == L'}') {
            --depth;
            if (depth == 0) return json.substr(pos, i - pos + 1);
        }
    }
    return L"";
}

int ClampCap(int v, int lo, int hi, int fallback) {
    if (v < lo || v > hi) return fallback;
    return v;
}

} // namespace

void ContextMenuPrefs::ResetToDefaults() {
    software = true;
    share = false;
    wallpaper = false;
    rotate = false;
    shortcut = false;
    open_with = true;
    open_with_com = false;
    system_extra = false;
    print = true;
    explorer_cap = 12;
    open_with_mru = 2;
    item_enabled.clear();
}

bool ContextMenuPrefs::CategoryEnabled(ipc::CtxMenuCategory c) const {
    switch (c) {
    case ipc::CtxMenuCategory::Share: return share;
    case ipc::CtxMenuCategory::Wallpaper: return wallpaper;
    case ipc::CtxMenuCategory::Rotate: return rotate;
    case ipc::CtxMenuCategory::Shortcut: return shortcut;
    case ipc::CtxMenuCategory::OpenWith: return open_with;
    case ipc::CtxMenuCategory::SystemExtra: return system_extra;
    case ipc::CtxMenuCategory::Print: return print;
    default: return software;
    }
}

void ContextMenuPrefs::SetCategoryEnabled(ipc::CtxMenuCategory c, bool on) {
    switch (c) {
    case ipc::CtxMenuCategory::Share: share = on; break;
    case ipc::CtxMenuCategory::Wallpaper: wallpaper = on; break;
    case ipc::CtxMenuCategory::Rotate: rotate = on; break;
    case ipc::CtxMenuCategory::Shortcut: shortcut = on; break;
    case ipc::CtxMenuCategory::OpenWith: open_with = on; break;
    case ipc::CtxMenuCategory::SystemExtra: system_extra = on; break;
    case ipc::CtxMenuCategory::Print: print = on; break;
    default: software = on; break;
    }
}

bool ContextMenuPrefs::GroupEnabled(ipc::CtxMenuGroup g) const {
    switch (g) {
    case ipc::CtxMenuGroup::OpenWith: return open_with;
    case ipc::CtxMenuGroup::Share: return share;
    case ipc::CtxMenuGroup::System:
        return wallpaper || rotate || shortcut || system_extra;
    case ipc::CtxMenuGroup::Print: return print;
    default: return software;
    }
}

void ContextMenuPrefs::SetGroupEnabled(ipc::CtxMenuGroup g, bool on) {
    switch (g) {
    case ipc::CtxMenuGroup::OpenWith: open_with = on; break;
    case ipc::CtxMenuGroup::Share: share = on; break;
    case ipc::CtxMenuGroup::System:
        wallpaper = rotate = shortcut = system_extra = on;
        break;
    case ipc::CtxMenuGroup::Print: print = on; break;
    default: software = on; break;
    }
}

bool ContextMenuPrefs::ItemEnabled(const std::wstring& key, ipc::CtxMenuCategory c,
                                   bool from_com) const {
    const auto it = item_enabled.find(key);
    if (it != item_enabled.end()) return it->second;
    if (c == ipc::CtxMenuCategory::OpenWith && from_com) return open_with_com;
    return CategoryEnabled(c);
}

void ContextMenuPrefs::SetItemEnabled(const std::wstring& key, bool on) {
    if (key.empty()) return;
    item_enabled[key] = on;
}

bool ContextMenuPrefs::RecordSeen(const std::wstring& key, const std::wstring& text,
                                  bool flyout, ipc::CtxMenuCategory category, bool from_com) {
    if (key.empty() || text.empty()) return false;
    for (const auto& item : seen)
        if (item.key == key) return false;
    if (seen.size() >= 400) return false;
    SeenMenuItem item;
    item.key = key;
    item.text = text;
    item.flyout = flyout;
    item.from_com = from_com;
    item.category = category;
    seen.push_back(std::move(item));
    return true;
}

std::wstring ContextMenuPrefs::ToJson() const {
    std::wstring out;
    out += L"{\n  \"version\":1,\n";
    out += L"  \"explorer_cap\":";
    out += std::to_wstring(explorer_cap);
    out += L",\n  \"open_with_mru\":";
    out += std::to_wstring(open_with_mru);
    out += L",\n  \"categories\":{\n";
    auto cat = [&](const wchar_t* name, bool v, bool last) {
        out += L"    \"";
        out += name;
        out += L"\":";
        out += v ? L"true" : L"false";
        out += last ? L"\n" : L",\n";
    };
    cat(L"software", software, false);
    cat(L"share", share, false);
    cat(L"wallpaper", wallpaper, false);
    cat(L"rotate", rotate, false);
    cat(L"shortcut", shortcut, false);
    cat(L"open_with", open_with, false);
    cat(L"open_with_com", open_with_com, false);
    cat(L"system_extra", system_extra, false);
    cat(L"print", print, true);
    out += L"  },\n  \"items\":{\n";
    size_t n = 0;
    for (const auto& kv : item_enabled) {
        std::wstring key;
        pulse::json::Escape(kv.first, key);
        out += L"    \"";
        out += key;
        out += L"\":{\"enabled\":";
        out += kv.second ? L"true" : L"false";
        out += L"}";
        ++n;
        out += (n == item_enabled.size()) ? L"\n" : L",\n";
    }
    out += L"  },\n  \"seen\":[\n";
    for (size_t i = 0; i < seen.size(); ++i) {
        const auto& item = seen[i];
        std::wstring key, text;
        pulse::json::Escape(item.key, key);
        pulse::json::Escape(item.text, text);
        out += L"    {\"key\":\"";
        out += key;
        out += L"\",\"text\":\"";
        out += text;
        out += L"\",\"kind\":\"";
        out += item.flyout ? L"flyout" : L"verb";
        out += L"\",\"category\":\"";
        out += ipc::CtxMenuCategoryId(item.category);
        out += L"\",\"source\":\"";
        out += item.from_com ? L"com" : L"static";
        out += L"\"}";
        out += (i + 1 == seen.size()) ? L"\n" : L",\n";
    }
    out += L"  ]\n}\n";
    return out;
}

bool ContextMenuPrefs::FromJson(const std::wstring& json) {
    if (json.empty()) return false;
    explorer_cap = ClampCap(pulse::json::ExtractInt(json, L"explorer_cap", 12), 1, 48, 12);
    open_with_mru = ClampCap(pulse::json::ExtractInt(json, L"open_with_mru", 2), 0, 8, 2);

    const std::wstring cats = ExtractObject(json, L"categories");
    const std::wstring& src = cats.empty() ? json : cats;
    software = pulse::json::ExtractBool(src, L"software", true);
    share = pulse::json::ExtractBool(src, L"share", false);
    wallpaper = pulse::json::ExtractBool(src, L"wallpaper", false);
    rotate = pulse::json::ExtractBool(src, L"rotate", false);
    shortcut = pulse::json::ExtractBool(src, L"shortcut", false);
    open_with = pulse::json::ExtractBool(src, L"open_with", true);
    open_with_com = pulse::json::ExtractBool(src, L"open_with_com", false);
    system_extra = pulse::json::ExtractBool(src, L"system_extra", false);
    print = pulse::json::ExtractBool(src, L"print", true);

    item_enabled.clear();
    const std::wstring items = ExtractObject(json, L"items");
    if (!items.empty()) {
        size_t pos = 1;
        while (pos < items.size()) {
            while (pos < items.size() && items[pos] != L'"' && items[pos] != L'}') ++pos;
            if (pos >= items.size() || items[pos] == L'}') break;
            ++pos;
            const std::wstring key = pulse::json::UnescapeString(items, pos);
            size_t obj = items.find(L'{', pos);
            if (obj == std::wstring::npos) break;
            size_t end = items.find(L'}', obj);
            if (end == std::wstring::npos) break;
            const std::wstring block = items.substr(obj, end - obj + 1);
            item_enabled[key] = pulse::json::ExtractBool(block, L"enabled", true);
            pos = end + 1;
        }
    }

    seen.clear();
    size_t pos = json.find(L"\"seen\"");
    if (pos != std::wstring::npos) pos = json.find(L'[', pos);
    if (pos != std::wstring::npos) {
        ++pos;
        while (pos < json.size() && json[pos] != L']') {
            const size_t obj = json.find(L'{', pos);
            if (obj == std::wstring::npos) break;
            const size_t close_arr = json.find(L']', pos);
            if (close_arr != std::wstring::npos && obj > close_arr) break;
            const size_t end = json.find(L'}', obj);
            if (end == std::wstring::npos) break;
            const std::wstring block = json.substr(obj, end - obj + 1);
            SeenMenuItem item;
            item.key = pulse::json::ExtractString(block, L"key");
            item.text = pulse::json::ExtractString(block, L"text");
            item.flyout = pulse::json::ExtractString(block, L"kind") == L"flyout";
            item.from_com = pulse::json::ExtractString(block, L"source") == L"com";
            item.category = ipc::ParseCtxMenuCategory(pulse::json::ExtractString(block, L"category"));
            if (!item.key.empty() && !item.text.empty()) seen.push_back(std::move(item));
            pos = end + 1;
        }
    }
    return true;
}

bool ContextMenuPrefs::Load() {
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    std::wifstream f(dir + L"\\context_menu.json", std::wifstream::binary);
    if (!f) return false;
    std::wstringstream ss;
    ss << f.rdbuf();
    return FromJson(ss.str());
}

bool ContextMenuPrefs::Save() const {
    if (!persist) return true;
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    const std::wstring tmp = dir + L"\\context_menu.tmp";
    const std::wstring final_path = dir + L"\\context_menu.json";
    std::wofstream f(tmp, std::wofstream::out | std::wofstream::trunc);
    if (!f) return false;
    f << ToJson();
    f.close();
    return MoveFileExW(tmp.c_str(), final_path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

} // namespace pulse::app

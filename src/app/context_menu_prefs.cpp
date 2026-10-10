// context_menu_prefs.cpp — JSON load/save for the Explorer fusion-zone prefs.
#include "context_menu_prefs.h"
#include "../common/config_json.h"
#include "session.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <algorithm>
#include <iterator>
#include <unordered_set>
#include <windows.h>

namespace pulse::app {
namespace {

using pulse::json::ExtractObject;

// 2: slow_ext only holds handlers the host proved hung (collect_hung). Files
// written before that carry disables from the old "everyone still running
// after 1 s" rule, which hid every healthy extension after three stalls.
constexpr int kPrefsVersion = 2;

int ClampCap(int v, int lo, int hi, int fallback) {
    if (v < lo || v > hi) return fallback;
    return v;
}

// context_menu.json "pulse_order" keys, indexed by BuiltinMenuSurface.
constexpr const wchar_t* kOrderKeys[] = { L"item", L"background", L"row_buttons" };
static_assert(std::size(kOrderKeys) == static_cast<size_t>(BuiltinMenuSurface::Count));

} // namespace

void ContextMenuPrefs::ResetToDefaults() {
    // Explorer parity for the visible groups; 发送到 and the image / system verbs
    // stay off until the user asks for them from the settings page.
    software = true;
    share = false;
    wallpaper = false;
    rotate = false;
    shortcut = false;
    open_with = true;
    open_with_com = false;
    system_extra = false;
    print = true;
    explorer_cap = ipc::kDefaultExplorerCap;
    open_with_mru = 2;
    item_enabled.clear();
    seen.clear();
    slow_ext.clear();
    builtin_hidden = 0;
    ResetBuiltinOrder();
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

bool ContextMenuPrefs::RowEnabled(const std::wstring& key, ipc::CtxMenuCategory c,
                                  bool from_com) const {
    return ItemEnabled(key, c, from_com) && !ComDisabled(key);
}

void ContextMenuPrefs::SetBuiltinVisible(BuiltinMenuItem item, bool on) {
    if (on) builtin_hidden &= ~BuiltinMenuBit(item);
    else builtin_hidden |= BuiltinMenuBit(item);
}

BuiltinMenuOrder ContextMenuPrefs::BuiltinOrder(BuiltinMenuSurface surface) const {
    if (surface >= BuiltinMenuSurface::Count) return {};
    return NormalizeBuiltinMenuOrder(surface, builtin_order[static_cast<size_t>(surface)]);
}

bool ContextMenuPrefs::BuiltinOrderCustom(BuiltinMenuSurface surface) const {
    return surface < BuiltinMenuSurface::Count && BuiltinOrder(surface) != BuiltinMenuDefaultOrder(surface);
}

void ContextMenuPrefs::SetBuiltinOrder(BuiltinMenuSurface surface, const BuiltinMenuOrder& order) {
    if (surface >= BuiltinMenuSurface::Count) return;
    auto normalized = NormalizeBuiltinMenuOrder(surface, order);
    if (normalized == BuiltinMenuDefaultOrder(surface)) normalized.clear();
    builtin_order[static_cast<size_t>(surface)] = std::move(normalized);
}

void ContextMenuPrefs::ResetBuiltinOrder() {
    for (auto& order : builtin_order) order.clear();
}

void ContextMenuPrefs::SetItemEnabled(const std::wstring& key, bool on) {
    if (key.empty()) return;
    item_enabled[key] = on;
    if (on) SetComDisabled(key, false);
}

bool ContextMenuPrefs::HandlerEnabled(const std::wstring& clsid) const {
    if (clsid.empty()) return true;
    const std::wstring key = ipc::HandlerCatalogKey(clsid);
    if (ComDisabled(key)) return false;
    const auto it = item_enabled.find(key);
    if (it != item_enabled.end()) return it->second;
    return true;
}

std::vector<std::wstring> ContextMenuPrefs::DisabledHandlerClsids() const {
    std::vector<std::wstring> out;
    std::unordered_set<std::wstring> listed;
    auto add = [&](std::wstring clsid) {
        clsid = ipc::ToLowerVerb(clsid);
        if (clsid.empty() || !listed.insert(clsid).second) return;
        out.push_back(std::move(clsid));
    };
    for (const auto& kv : item_enabled) {
        if (!kv.second && ipc::IsHandlerCatalogKey(kv.first))
            add(ipc::HandlerClsidFromKey(kv.first));
    }
    for (const auto& kv : slow_ext) {
        if (kv.second.disabled && ipc::IsHandlerCatalogKey(kv.first))
            add(ipc::HandlerClsidFromKey(kv.first));
    }
    return out;
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

bool ContextMenuPrefs::RecordComTiming(const std::wstring& key, uint32_t elapsed_ms) {
    // SendTo never feeds the slow-extension auto-disable (#77): it serializes
    // the whole default menu, so it is structurally the last worker home and
    // would be permanently killed after three slow right-clicks.
    if (key.empty() || key == ipc::SendToHandlerCatalogKey()) return false;
    SlowComExt& st = slow_ext[key];
    st.last_ms = elapsed_ms;
    bool changed = false;
    if (elapsed_ms >= 1000) {
        ++st.timeout_hits;
        changed = true;
        if (st.timeout_hits >= 3 && !st.disabled) {
            st.disabled = true;
            st.deferred = true;
        }
    } else if (elapsed_ms >= 500) {
        ++st.slow_hits;
        changed = true;
        if (st.slow_hits >= 3 && !st.deferred) st.deferred = true;
    }
    return changed;
}

bool ContextMenuPrefs::ComDeferred(const std::wstring& key) const {
    auto it = slow_ext.find(key);
    return it != slow_ext.end() && (it->second.deferred || it->second.disabled);
}

bool ContextMenuPrefs::ComDisabled(const std::wstring& key) const {
    auto it = slow_ext.find(key);
    return it != slow_ext.end() && it->second.disabled;
}

void ContextMenuPrefs::SetComDisabled(const std::wstring& key, bool on) {
    if (key.empty()) return;
    SlowComExt& st = slow_ext[key];
    st.disabled = on;
    if (!on) st.timeout_hits = 0;
}

std::wstring ContextMenuPrefs::ToJson() const {
    std::wstring out;
    out += L"{\n  \"version\":";
    out += std::to_wstring(kPrefsVersion);
    out += L",\n";
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
    out += L"  },\n  \"pulse_items\":{";
    bool first_builtin = true;
    for (int i = 0; i < kBuiltinMenuItemCount; ++i) {
        const auto item = static_cast<BuiltinMenuItem>(i);
        if (BuiltinVisible(item)) continue;
        out += first_builtin ? L"\n    \"" : L",\n    \"";
        out += BuiltinMenuKey(item);
        out += L"\":false";
        first_builtin = false;
    }
    out += first_builtin ? L"},\n" : L"\n  },\n";
    // "pulse_order": comma-joined keys per surface, only when moved.
    out += L"  \"pulse_order\":{";
    bool first_order = true;
    for (int s = 0; s < static_cast<int>(BuiltinMenuSurface::Count); ++s) {
        const auto surface = static_cast<BuiltinMenuSurface>(s);
        if (!BuiltinOrderCustom(surface)) continue;
        out += first_order ? L"\n    \"" : L",\n    \"";
        out += kOrderKeys[s];
        out += L"\":\"";
        bool first_key = true;
        for (const auto item : BuiltinOrder(surface)) {
            if (!first_key) out += L",";
            out += BuiltinMenuKey(item);
            first_key = false;
        }
        out += L"\"";
        first_order = false;
    }
    out += first_order ? L"},\n  \"items\":{\n" : L"\n  },\n  \"items\":{\n";
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
    out += L"  ],\n  \"slow_ext\":{\n";
    size_t se = 0;
    for (const auto& kv : slow_ext) {
        std::wstring key;
        pulse::json::Escape(kv.first, key);
        out += L"    \"";
        out += key;
        out += L"\":{\"ms\":";
        out += std::to_wstring(kv.second.last_ms);
        out += L",\"slow\":";
        out += std::to_wstring(kv.second.slow_hits);
        out += L",\"timeout\":";
        out += std::to_wstring(kv.second.timeout_hits);
        out += L",\"deferred\":";
        out += kv.second.deferred ? L"true" : L"false";
        out += L",\"disabled\":";
        out += kv.second.disabled ? L"true" : L"false";
        out += L"}";
        ++se;
        out += (se == slow_ext.size()) ? L"\n" : L",\n";
    }
    out += L"  }\n}\n";
    return out;
}

bool ContextMenuPrefs::FromJson(const std::wstring& json) {
    if (!pulse::json::ValidConfigObject(json)) return false;
    explorer_cap = ClampCap(pulse::json::ExtractInt(json, L"explorer_cap", ipc::kDefaultExplorerCap),
                            1, 48, ipc::kDefaultExplorerCap);
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

    builtin_hidden = 0;
    const std::wstring builtin = ExtractObject(json, L"pulse_items");
    for (int i = 0; i < kBuiltinMenuItemCount && !builtin.empty(); ++i) {
        const auto item = static_cast<BuiltinMenuItem>(i);
        if (!pulse::json::ExtractBool(builtin, std::wstring(BuiltinMenuKey(item)), true))
            builtin_hidden |= BuiltinMenuBit(item);
    }
    ResetBuiltinOrder();
    const std::wstring orders = ExtractObject(json, L"pulse_order");
    for (int s = 0; s < static_cast<int>(BuiltinMenuSurface::Count) && !orders.empty(); ++s) {
        const std::wstring keys = pulse::json::ExtractString(orders, kOrderKeys[s]);
        BuiltinMenuOrder order;
        for (size_t at = 0; at < keys.size();) {
            size_t end = keys.find(L',', at);
            if (end == std::wstring::npos) end = keys.size();
            const std::wstring_view key(keys.data() + at, end - at);
            for (int i = 0; i < kBuiltinMenuItemCount; ++i)
                if (BuiltinMenuKey(static_cast<BuiltinMenuItem>(i)) == key) order.push_back(static_cast<BuiltinMenuItem>(i));
            at = end + 1;
        }
        SetBuiltinOrder(static_cast<BuiltinMenuSurface>(s), order);
    }

    // Keys are menu ids such as "h:{GUID}" and texts are whatever the menu
    // shows, so values are walked with the shared string-aware readers.
    item_enabled.clear();
    pulse::json::ForEachMember(ExtractObject(json, L"items"),
        [&](const std::wstring& key, const std::wstring& block) {
            item_enabled[key] = pulse::json::ExtractBool(block, L"enabled", true);
        });

    seen.clear();
    pulse::json::ForEachElement(pulse::json::ExtractArray(json, L"seen"), [&](const std::wstring& block) {
        if (block.empty() || block.front() != L'{') return;
        SeenMenuItem item;
        item.key = pulse::json::ExtractString(block, L"key");
        item.text = pulse::json::ExtractString(block, L"text");
        item.flyout = pulse::json::ExtractString(block, L"kind") == L"flyout";
        item.from_com = pulse::json::ExtractString(block, L"source") == L"com";
        item.category = ipc::ParseCtxMenuCategory(pulse::json::ExtractString(block, L"category"));
        if (!item.key.empty() && !item.text.empty()) seen.push_back(std::move(item));
    });

    MigrateSeenKeys();
    CoalesceCompressCatalog();

    slow_ext.clear();
    pulse::json::ForEachMember(ExtractObject(json, L"slow_ext"),
        [&](const std::wstring& key, const std::wstring& block) {
            if (key.empty() || block.empty() || block.front() != L'{') return;
            SlowComExt st;
            st.last_ms = static_cast<uint32_t>(
                std::max(0, pulse::json::ExtractInt(block, L"ms", 0)));
            st.slow_hits = static_cast<uint32_t>(
                std::max(0, pulse::json::ExtractInt(block, L"slow", 0)));
            st.timeout_hits = static_cast<uint32_t>(
                std::max(0, pulse::json::ExtractInt(block, L"timeout", 0)));
            st.deferred = pulse::json::ExtractBool(block, L"deferred", false);
            st.disabled = pulse::json::ExtractBool(block, L"disabled", false);
            slow_ext[key] = st;
        });
    // Purge disable state older builds persisted for SendTo (#77): the
    // exemption above keeps it clear, so this migration is idempotent.
    slow_ext.erase(ipc::SendToHandlerCatalogKey());
    migrated = false;
    if (pulse::json::ExtractInt(json, L"version", 1) < kPrefsVersion) {
        // Drop the old heuristic's verdicts (every entry there was recorded
        // as a synthetic 1000 ms "timeout") and the per-type timings nothing
        // reads. Explicit switches in "items" are the user's and stay.
        for (auto it = slow_ext.begin(); it != slow_ext.end();) {
            if (!ipc::IsHandlerCatalogKey(it->first)) {
                it = slow_ext.erase(it);
                continue;
            }
            it->second.slow_hits = 0;
            it->second.timeout_hits = 0;
            it->second.deferred = false;
            it->second.disabled = false;
            ++it;
        }
        // Explorer's 打开方式 flyout now stands in for Pulse's static row and
        // follows the 打开方式 switch; older catalogs filed it as a COM row.
        for (auto& item : seen) {
            if (item.flyout && item.category == ipc::CtxMenuCategory::OpenWith)
                item.from_com = false;
        }
        migrated = true;
    }
    return true;
}

// Older builds keyed the catalog by raw display text, so one verb showed up once
// per file type ("新建(N)" / "新建(W)", "用 X 打开" with and without spaces) and
// switching one of them off left its siblings on. Only display-text identities
// are migrated; command and handler identities must remain language independent.
void ContextMenuPrefs::MigrateSeenKeys() {
    constexpr std::wstring_view quick_access = L"pulse:quick-access";
    const auto display_key = [](std::wstring_view key) {
        return key.starts_with(L"v:") || key.starts_with(L"f:");
    };
    const auto old_quick_access = [&](const SeenMenuItem& item) {
        if (!item.key.starts_with(L"v:") || item.flyout || item.from_com ||
            item.category != ipc::CtxMenuCategory::Software) return false;
        const auto normalized = ipc::NormalizeCatalogText(item.text);
        for (const auto label : {L"Pin to Quick access", L"固定到快速访问", L"固定到快速存取"}) {
            if (normalized == ipc::NormalizeCatalogText(label) &&
                ipc::NormalizeCatalogText(item.key.substr(2)) == normalized) return true;
        }
        return false;
    };
    std::unordered_set<std::wstring> repaired_aliases;
    std::unordered_set<std::wstring> observed_keys;
    for (const auto& item : seen) observed_keys.insert(item.key);
    // Older preferences can retain an override after its seen row was evicted.
    // Only infer the legacy Pulse alias when no row identifies it as COM/other.
    for (const auto& [key, enabled] : item_enabled) {
        (void)enabled;
        if (observed_keys.contains(key) || !key.starts_with(L"v:")) continue;
        const auto normalized = ipc::NormalizeCatalogText(key.substr(2));
        for (const auto label : {L"Pin to Quick access", L"固定到快速访问", L"固定到快速存取"})
            if (normalized == ipc::NormalizeCatalogText(label)) repaired_aliases.insert(key);
    }
    std::vector<SeenMenuItem> merged;
    merged.reserve(seen.size());
    for (auto& item : seen) {
        std::wstring canonical = item.key;
        if (old_quick_access(item)) {
            repaired_aliases.insert(item.key);
            canonical = quick_access;
        } else if (display_key(item.key)) canonical = ipc::CatalogKey(item.text, item.flyout);
        if (canonical.empty()) continue;
        bool duplicate = false;
        for (const auto& kept : merged) {
            if (kept.key == canonical) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        item.key = canonical;
        merged.push_back(std::move(item));
    }
    seen = std::move(merged);

    std::unordered_map<std::wstring, bool> moved;
    moved.reserve(item_enabled.size());
    const bool has_stable_choice = item_enabled.contains(std::wstring(quick_access));
    for (const auto& kv : item_enabled) {
        std::wstring key = kv.first;
        if (repaired_aliases.contains(key)) {
            if (has_stable_choice) continue;
            key = quick_access;
        } else if (display_key(key)) {
            key = key.substr(0, 2) + ipc::NormalizeCatalogText(key.substr(2));
        }
        const auto [it, inserted] = moved.emplace(key, kv.second);
        if (!inserted) it->second = it->second && kv.second;
    }
    item_enabled = std::move(moved);
}

void ContextMenuPrefs::CoalesceCompressCatalog() {
    std::vector<SeenMenuItem> kept;
    kept.reserve(seen.size());
    bool compress_seen = false;
    bool compress_forced_on = false;
    bool compress_forced_off = false;
    for (auto& item : seen) {
        if (item.key == ipc::CompressCatalogKey()) {
            compress_seen = true;
            kept.push_back(std::move(item));
            continue;
        }
        if (ipc::IsCompressTopLevel(item.text) && !item.flyout) {
            const auto it = item_enabled.find(item.key);
            if (it != item_enabled.end()) {
                if (it->second) compress_forced_on = true;
                else compress_forced_off = true;
                item_enabled.erase(it);
            }
            continue;
        }
        kept.push_back(std::move(item));
    }
    if (kept.size() != seen.size() && !compress_seen) {
        SeenMenuItem row;
        row.key = ipc::CompressCatalogKey();
        row.text = ipc::CompressCatalogText();
        row.from_com = true;
        row.category = ipc::CtxMenuCategory::Software;
        kept.push_back(std::move(row));
        compress_seen = true;
    }
    seen = std::move(kept);
    if (compress_forced_on && !compress_forced_off)
        item_enabled[ipc::CompressCatalogKey()] = true;
    else if (compress_forced_off && !compress_forced_on)
        item_enabled[ipc::CompressCatalogKey()] = false;
}

bool ContextMenuPrefs::Load() {
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) { load_failed = true; return false; }
    const std::wstring file = dir + L"\\context_menu.json";
    const DWORD attributes = GetFileAttributesW(file.c_str());
    const DWORD code = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    const bool missing = attributes == INVALID_FILE_ATTRIBUTES &&
        (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND);
    std::wstring json;
    if (!missing && (!ReadUtf8File(file, json) || !FromJson(json))) {
        load_failed = true;
        return false;
    }
    load_failed = false;
    if (migrated) {
        Save();
        migrated = false;
    }
    return true;
}

bool ContextMenuPrefs::Save() const {
    if (!persist) return true;
    if (load_failed) return false;
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    return WriteUtf8FileAtomic(dir + L"\\context_menu.json", ToJson());
}

} // namespace pulse::app
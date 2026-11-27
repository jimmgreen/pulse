// context_menu_prefs.cpp — JSON load/save for the Explorer fusion-zone prefs.
#include "context_menu_prefs.h"
#include "../common/config_json.h"
#include "session.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include "../common/async_file_writer.h"
#include <algorithm>
#include <iterator>
#include <unordered_set>
#include <windows.h>

namespace pulse::app {
namespace {

using pulse::json::ExtractObject;
constexpr size_t kSeenCatalogLimit = 4096;

bool RetireOldestSeen(ContextMenuPrefs& prefs, const std::wstring& protected_key = L"") {
    // Retire only unconfigured leaves. A parent is never detached from its
    // children, and a user choice/order never disappears due to discovery.
    const auto oldest = std::find_if(prefs.seen.begin(), prefs.seen.end(), [&](const auto& row) {
        if (row.key == protected_key || prefs.item_enabled.contains(row.key) || prefs.ComDisabled(row.key)) return false;
        if (std::any_of(prefs.seen.begin(), prefs.seen.end(), [&](const auto& child) {
            return child.tree_node && child.parent_key == row.key;
        })) return false;
        for (const auto& [parent, order] : prefs.node_order) {
            if (parent == row.key || std::find(order.begin(), order.end(), row.key) != order.end()) return false;
        }
        return true;
    });
    if (oldest == prefs.seen.end()) return false;
    prefs.seen.erase(oldest);
    return true;
}

void TrimLegacySeen(ContextMenuPrefs& prefs, size_t limit) {
    while (static_cast<size_t>(std::count_if(prefs.seen.begin(), prefs.seen.end(),
        [](const auto& row) { return !row.tree_node; })) > limit) {
        auto oldest = std::find_if(prefs.seen.begin(), prefs.seen.end(), [&](const auto& row) {
            return !row.tree_node && !prefs.item_enabled.contains(row.key) && !prefs.ComDisabled(row.key);
        });
        if (oldest == prefs.seen.end()) oldest = std::find_if(prefs.seen.begin(), prefs.seen.end(),
            [](const auto& row) { return !row.tree_node; });
        if (oldest == prefs.seen.end()) break;
        prefs.seen.erase(oldest); // Discovery only; never erase the user's overrides.
    }
}



// 2: slow_ext only holds handlers the host proved hung (collect_hung). Files
// written before that carry disables from the old "everyone still running
// after 1 s" rule, which hid every healthy extension after three stalls.
// 3: 发送到 / 共享 and the system groups default on (Explorer parity). Older
// files stored the old off defaults as plain values, so they are turned on
// once; per-row switches in "items" are the user's and stay.
// 4: expose COM Open-with entries and remove the old implicit MRU cap.
// 5: ancestry-scoped actual-menu node catalog and sibling ordering.
constexpr int kPrefsVersion = 5;

int ClampCap(int v, int lo, int hi, int fallback) {
    if (v < lo || v > hi) return fallback;
    return v;
}

// context_menu.json "pulse_order" keys, indexed by BuiltinMenuSurface.
constexpr const wchar_t* kOrderKeys[] = { L"item", L"background", L"row_buttons" };
static_assert(std::size(kOrderKeys) == static_cast<size_t>(BuiltinMenuSurface::Count));

std::wstring AsciiWide(std::string_view s) {
    return std::wstring(s.begin(), s.end());
}

bool TryWideAscii(std::wstring_view w, std::string& out) {
    out.clear();
    if (w.empty() || w.size() > 192) return false;
    out.reserve(w.size());
    for (const wchar_t ch : w) {
        if (ch <= 0x20 || ch > 0x7e) return false;
        out.push_back(static_cast<char>(ch));
    }
    return true;
}

const wchar_t* PlacementKey(ExtensionMenuPlacement p) {
    switch (p) {
    case ExtensionMenuPlacement::Flat: return L"flat";
    case ExtensionMenuPlacement::Submenu: return L"submenu";
    case ExtensionMenuPlacement::Hidden: return L"hidden";
    default: return L"default";
    }
}

ExtensionMenuPlacement ParsePlacementKey(std::wstring_view v) {
    if (v == L"flat") return ExtensionMenuPlacement::Flat;
    if (v == L"submenu") return ExtensionMenuPlacement::Submenu;
    if (v == L"hidden") return ExtensionMenuPlacement::Hidden;
    return ExtensionMenuPlacement::Default;
}

} // namespace

void ContextMenuPrefs::ResetToDefaults() {
    // Explorer parity: every group its classic menu shows starts on.
    software = true;
    share = true;
    wallpaper = true;
    rotate = true;
    shortcut = true;
    open_with = true;
    open_with_com = true;
    system_extra = true;
    print = true;
    open_with_mru = -1;
    item_enabled.clear();
    seen.clear();
    node_order.clear();
    slow_ext.clear();
    builtin_hidden = 0;
    ResetBuiltinOrder();
    extension_placement.clear();
    extension_hidden_commands.clear();
}

ExtensionMenuPlacement ContextMenuPrefs::PlacementForExtension(std::string_view package_id) const {
    const auto it = extension_placement.find(std::string(package_id));
    return it == extension_placement.end() ? ExtensionMenuPlacement::Default : it->second;
}

void ContextMenuPrefs::SetExtensionPlacement(std::string_view package_id, ExtensionMenuPlacement placement) {
    if (package_id.empty()) return;
    if (placement == ExtensionMenuPlacement::Default) extension_placement.erase(std::string(package_id));
    else extension_placement[std::string(package_id)] = placement;
}

bool ContextMenuPrefs::ExtensionCommandVisible(std::string_view identity) const {
    return !extension_hidden_commands.contains(std::string(identity));
}

void ContextMenuPrefs::SetExtensionCommandVisible(std::string_view identity, bool on) {
    if (identity.empty()) return;
    if (on) extension_hidden_commands.erase(std::string(identity));
    else extension_hidden_commands.insert(std::string(identity));
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
    if (c == ipc::CtxMenuCategory::OpenWith && from_com) return open_with && open_with_com;
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

bool ContextMenuPrefs::CatalogItemVisible(const std::wstring& key) const {
    for (const auto& item : seen)
        if (item.key == key) return RowEnabled(item.key, item.category, item.from_com);
    const auto it = item_enabled.find(key);
    return (it == item_enabled.end() || it->second) && !ComDisabled(key);
}

std::vector<std::wstring> ContextMenuPrefs::HiddenCatalogKeys() const {
    std::vector<std::wstring> keys;
    for (const auto& item : seen)
        if (!RowEnabled(item.key, item.category, item.from_com)) keys.push_back(item.key);
    for (const auto& [key, on] : item_enabled)
        if (!on) keys.push_back(key);
    for (const auto& [key, state] : slow_ext)
        if (state.disabled) keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
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
    const auto found = std::find_if(seen.begin(), seen.end(), [&](const auto& item) { return item.key == key; });
    if (found != seen.end()) {
        std::rotate(found, std::next(found), seen.end());
        return false;
    }
    TrimLegacySeen(*this, 399);
    while (seen.size() >= kSeenCatalogLimit) if (!RetireOldestSeen(*this)) return false;
    SeenMenuItem item;
    item.key = key;
    item.text = text;
    item.flyout = flyout;
    item.from_com = from_com;
    item.category = category;
    seen.push_back(std::move(item));
    return true;
}

bool ContextMenuPrefs::RecordSeenNode(SeenMenuItem item) {
    if (item.key.empty() || item.text.empty() || item.depth > 16) return false;
    item.tree_node = true;
    if (!item.parent_key.empty()) {
        const auto parent = std::find_if(seen.begin(), seen.end(), [&](const auto& row) { return row.tree_node && row.key == item.parent_key; });
        if (parent == seen.end()) return false;
        item.configurable = item.configurable && parent->configurable;
    }
    auto found = std::find_if(seen.begin(), seen.end(), [&](const auto& row) { return row.key == item.key; });
    if (found != seen.end()) {
        // Once ambiguous, never reactivate an old choice merely because a
        // partial live reply currently contains only one of the twins.
        item.configurable = item.configurable && found->configurable;
        const bool changed = found->text != item.text || found->flyout != item.flyout ||
            found->configurable != item.configurable || found->source != item.source ||
            found->default_order != item.default_order || found->parent_key != item.parent_key ||
            found->root_key != item.root_key || found->depth != item.depth ||
            found->from_com != item.from_com || found->category != item.category || !found->tree_node;
        *found = std::move(item);
        return changed;
    }
    while (seen.size() >= kSeenCatalogLimit) if (!RetireOldestSeen(*this, item.parent_key)) return false;
    seen.push_back(std::move(item));
    return true;
}

bool ContextMenuPrefs::RecordSeenNodes(std::vector<SeenMenuItem> items) {
    // One bounded per-thread lookup accelerator, not a catalog/result cache.
    // Public callers may replace/reorder seen without a revision notification;
    // every hit validates the current key before using its index. Owned keys
    // and numeric indices never dereference an expired prefs/vector address.
    struct LookupCache {
        const ContextMenuPrefs* owner = nullptr;
        const SeenMenuItem* data = nullptr;
        size_t size = 0;
        std::unordered_map<std::wstring, size_t> index;
    };
    thread_local LookupCache cache;
    auto& index = cache.index;
    const auto rebuild = [&] {
        index.clear(); index.reserve(seen.size() + items.size());
        for (size_t i = 0; i < seen.size(); ++i) index.emplace(seen[i].key, i);
        cache.owner = this; cache.data = seen.data(); cache.size = seen.size();
    };
    if (cache.owner != this || cache.data != seen.data() || cache.size != seen.size()) rebuild();
    const auto lookup = [&](const std::wstring& key) {
        auto found = index.find(key);
        if (found != index.end()) {
            if (found->second < seen.size() && seen[found->second].key == key) return found;
            rebuild();
        } else if (std::any_of(seen.begin(), seen.end(), [&](const auto& row) { return row.key == key; })) {
            // A same-size in-place replacement may have introduced this key.
            rebuild();
        }
        return index.find(key);
    };
    bool changed = false;
    std::unordered_set<std::wstring> observed;
    for (auto& item : items) {
        if (item.key.empty() || item.text.empty() || item.depth > 16) continue;
        item.tree_node = true;
        if (!observed.insert(item.key).second) {
            if (const auto duplicate = lookup(item.key); duplicate != index.end()) {
                changed = seen[duplicate->second].configurable || changed;
                seen[duplicate->second].configurable = false;
            }
            continue;
        }
        if (!item.parent_key.empty()) {
            const auto parent = lookup(item.parent_key);
            if (parent == index.end() || !seen[parent->second].tree_node) continue;
            item.configurable = item.configurable && seen[parent->second].configurable;
        }
        const auto found = lookup(item.key);
        if (found != index.end()) {
            auto& old = seen[found->second];
            item.configurable = item.configurable && old.configurable;
            changed = changed || old.text != item.text || old.flyout != item.flyout ||
                old.configurable != item.configurable || old.source != item.source ||
                old.default_order != item.default_order || old.parent_key != item.parent_key ||
                old.root_key != item.root_key || old.depth != item.depth ||
                old.from_com != item.from_com || old.category != item.category || !old.tree_node;
            old = std::move(item);
            continue;
        }
        if (seen.size() >= kSeenCatalogLimit) {
            if (!RetireOldestSeen(*this, item.parent_key)) continue;
            rebuild();
        }
        index.emplace(item.key, seen.size());
        seen.push_back(std::move(item));
        changed = true;
    }
    cache.data = seen.data(); cache.size = seen.size();
    return changed;
}

std::vector<std::wstring> ContextMenuPrefs::NodeOrder(const std::wstring& parent_key) const {
    const auto found = node_order.find(parent_key);
    return found == node_order.end() ? std::vector<std::wstring>{} : found->second;
}

std::vector<SeenMenuItem> ContextMenuPrefs::NodeChildren(const std::wstring& parent_key,
                                                      const std::wstring& root_key) const {
    std::vector<SeenMenuItem> rows;
    for (const auto& row : seen) if (row.tree_node && row.parent_key == parent_key &&
        (root_key.empty() || row.root_key == root_key)) rows.push_back(row);
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.default_order < b.default_order; });
    const auto order = NodeOrder(parent_key);
    if (!order.empty()) {
        std::vector<SeenMenuItem> movable;
        for (const auto& row : rows) if (row.configurable) movable.push_back(row);
        const auto rank = [&](const auto& row) { return std::find(order.begin(), order.end(), row.key) - order.begin(); };
        std::stable_sort(movable.begin(), movable.end(), [&](const auto& a, const auto& b) { return rank(a) < rank(b); });
        size_t next = 0;
        for (auto& row : rows) if (row.configurable) row = std::move(movable[next++]);
    }
    return rows;
}

bool ContextMenuPrefs::NodeEnabled(const std::wstring& key) const {
    const auto found = item_enabled.find(key);
    return found == item_enabled.end() || found->second;
}

bool ContextMenuPrefs::SetNodeEnabled(const std::wstring& key, bool on) {
    const auto row = std::find_if(seen.begin(), seen.end(), [&](const auto& item) {
        return item.tree_node && item.configurable && item.key == key;
    });
    if (row == seen.end() || NodeEnabled(key) == on) return false;
    if (on) item_enabled.erase(key); else item_enabled[key] = false;
    return true;
}

bool ContextMenuPrefs::SetNodeOrder(const std::wstring& parent_key, const std::vector<std::wstring>& keys) {
    std::vector<std::wstring> valid;
    for (const auto& key : keys) {
        if (std::find(valid.begin(), valid.end(), key) != valid.end()) continue;
        if (std::any_of(seen.begin(), seen.end(), [&](const auto& row) {
            return row.tree_node && row.configurable && row.parent_key == parent_key && row.key == key;
        })) valid.push_back(key);
    }
    if (NodeOrder(parent_key) == valid) return false;
    if (valid.empty()) node_order.erase(parent_key); else node_order[parent_key] = std::move(valid);
    return true;
}

bool ContextMenuPrefs::MoveNode(const std::wstring& key, int direction) {
    const auto row = std::find_if(seen.begin(), seen.end(), [&](const auto& item) { return item.tree_node && item.key == key; });
    if (row == seen.end() || !row->configurable || direction == 0) return false;
    auto children = NodeChildren(row->parent_key);
    const auto found = std::find_if(children.begin(), children.end(), [&](const auto& item) { return item.key == key; });
    const auto at = found - children.begin();
    const auto next = at + (direction < 0 ? -1 : 1);
    if (next < 0 || next >= static_cast<ptrdiff_t>(children.size()) || !children[static_cast<size_t>(next)].configurable) return false;
    std::swap(children[static_cast<size_t>(at)], children[static_cast<size_t>(next)]);
    std::vector<std::wstring> keys;
    for (const auto& child : children) keys.push_back(child.key);
    return SetNodeOrder(row->parent_key, keys);
}

bool ContextMenuPrefs::ResetNodeGroup(const std::wstring& parent_key) {
    bool changed = node_order.erase(parent_key) != 0;
    for (const auto& row : seen) if (row.tree_node && row.parent_key == parent_key)
        changed = (item_enabled.erase(row.key) != 0) || changed;
    return changed;
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
    out += L"  \"open_with_mru\":";
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
        out += L"\"";
        if (item.tree_node) {
            std::wstring parent, root, source;
            pulse::json::Escape(item.parent_key, parent);
            pulse::json::Escape(item.root_key, root);
            pulse::json::Escape(item.source, source);
            out += L",\"tree\":true,\"parent\":\"" + parent + L"\",\"root\":\"" + root +
                L"\",\"node_source\":\"" + source + L"\",\"depth\":" + std::to_wstring(item.depth) +
                L",\"default_order\":" + std::to_wstring(item.default_order) +
                L",\"configurable\":" + (item.configurable ? L"true" : L"false");
        }
        out += L"}";
        out += (i + 1 == seen.size()) ? L"\n" : L",\n";
    }
    out += L"  ],\n  \"node_order\":[";
    bool first_node_group = true;
    for (const auto& [parent_key, order] : node_order) {
        std::wstring parent; pulse::json::Escape(parent_key, parent);
        if (!first_node_group) out += L",";
        first_node_group = false;
        out += L"{\"parent\":\"" + parent + L"\",\"keys\":[";
        bool first = true;
        for (const auto& value : order) {
            std::wstring key; pulse::json::Escape(value, key);
            if (!first) out += L",";
            first = false;
            out += L"{\"key\":\"" + key + L"\"}";
        }
        out += L"]}";
    }
    out += L"],\n  \"slow_ext\":{\n";
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
    out += L"  },\n  \"extensions\":{\n    \"placement\":{";
    size_t ep = 0;
    for (const auto& [pkg, mode] : extension_placement) {
        if (mode == ExtensionMenuPlacement::Default) continue;
        std::wstring key;
        pulse::json::Escape(AsciiWide(pkg), key);
        out += (ep == 0 ? L"\n      \"" : L",\n      \"");
        out += key;
        out += L"\":{\"mode\":\"";
        out += PlacementKey(mode);
        out += L"\"}";
        ++ep;
    }
    out += ep == 0 ? L"},\n    \"commands\":{" : L"\n    },\n    \"commands\":{";
    size_t ec = 0;
    for (const auto& id : extension_hidden_commands) {
        std::wstring key;
        pulse::json::Escape(AsciiWide(id), key);
        out += (ec == 0 ? L"\n      \"" : L",\n      \"");
        out += key;
        out += L"\":{\"visible\":false}";
        ++ec;
    }
    out += ec == 0 ? L"}\n  }\n}\n" : L"\n    }\n  }\n}\n";
    return out;
}

bool ContextMenuPrefs::FromJson(const std::wstring& json) {
    if (!pulse::json::ValidConfigObject(json)) return false;
    // "explorer_cap" of older files is ignored: the menu has no row cap.
    open_with_mru = ClampCap(pulse::json::ExtractInt(json, L"open_with_mru", -1), -1, 8, -1);

    const std::wstring cats = ExtractObject(json, L"categories");
    const std::wstring& src = cats.empty() ? json : cats;
    software = pulse::json::ExtractBool(src, L"software", true);
    share = pulse::json::ExtractBool(src, L"share", true);
    wallpaper = pulse::json::ExtractBool(src, L"wallpaper", true);
    rotate = pulse::json::ExtractBool(src, L"rotate", true);
    shortcut = pulse::json::ExtractBool(src, L"shortcut", true);
    open_with = pulse::json::ExtractBool(src, L"open_with", true);
    open_with_com = pulse::json::ExtractBool(src, L"open_with_com", true);
    system_extra = pulse::json::ExtractBool(src, L"system_extra", true);
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
        item.tree_node = pulse::json::ExtractBool(block, L"tree", false);
        item.parent_key = pulse::json::ExtractString(block, L"parent");
        item.root_key = pulse::json::ExtractString(block, L"root");
        item.source = pulse::json::ExtractString(block, L"node_source");
        item.depth = static_cast<uint32_t>(ClampCap(pulse::json::ExtractInt(block, L"depth", 0), 0, 16, 0));
        item.configurable = pulse::json::ExtractBool(block, L"configurable", true);
        item.default_order = static_cast<uint32_t>(ClampCap(pulse::json::ExtractInt(block, L"default_order", 0), 0, 65535, 0));
        if (!item.key.empty() && !item.text.empty()) seen.push_back(std::move(item));
    });

    node_order.clear();
    pulse::json::ForEachElement(pulse::json::ExtractArray(json, L"node_order"), [&](const std::wstring& block) {
        const auto parent = pulse::json::ExtractString(block, L"parent");
        std::vector<std::wstring> keys;
        pulse::json::ForEachElement(pulse::json::ExtractArray(block, L"keys"), [&](const std::wstring& key) {
            const auto value = pulse::json::ExtractString(key, L"key");
            if (!value.empty() && keys.size() < kSeenCatalogLimit) keys.push_back(value);
        });
        SetNodeOrder(parent, keys);
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

    extension_placement.clear();
    extension_hidden_commands.clear();
    const std::wstring ext_obj = ExtractObject(json, L"extensions");
    if (!ext_obj.empty()) {
        pulse::json::ForEachMember(ExtractObject(ext_obj, L"placement"),
            [&](const std::wstring& key, const std::wstring& block) {
                std::string pkg;
                if (!TryWideAscii(key, pkg)) return;
                const auto mode = ParsePlacementKey(pulse::json::ExtractString(block, L"mode"));
                if (mode != ExtensionMenuPlacement::Default) extension_placement[std::move(pkg)] = mode;
            });
        pulse::json::ForEachMember(ExtractObject(ext_obj, L"commands"),
            [&](const std::wstring& key, const std::wstring& block) {
                std::string id;
                if (!TryWideAscii(key, id)) return;
                if (!pulse::json::ExtractBool(block, L"visible", true))
                    extension_hidden_commands.insert(std::move(id));
            });
    }
    const int version = pulse::json::ExtractInt(json, L"version", 1);
    if (version < 2) {
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
    }
    if (version < 3) {
        share = wallpaper = rotate = shortcut = system_extra = true;
    }
    if (version < 4) {
        open_with_com = true;
        if (open_with_mru == 2) open_with_mru = -1;
    }
    // Older tree catalogs merged every MRU application under the empty
    // registry source. Retire that unconfigurable row once at load, not by
    // scanning the historical catalog on every right-click.
    constexpr std::wstring_view obsolete_open_with = L"n:0:11:registry:0:12:v:__openwith:v";
    const bool retired_open_with = std::erase_if(seen, [=](const SeenMenuItem& item) {
        return item.tree_node && !item.configurable && item.key == obsolete_open_with;
    }) != 0;
    TrimLegacySeen(*this, 400);
    while (seen.size() > kSeenCatalogLimit) if (!RetireOldestSeen(*this)) break;
    migrated = version < kPrefsVersion || retired_open_with;
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
        if (!item.tree_node && old_quick_access(item)) {
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
        if (item.tree_node) { kept.push_back(std::move(item)); continue; }
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
    GlobalAsyncFileWriter().Flush();
    return WriteUtf8FileAtomic(dir + L"\\context_menu.json", ToJson());
}

bool ContextMenuPrefs::SaveAsync() const {
    if (!persist) return true;
    if (load_failed) return false;
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    return GlobalAsyncFileWriter().Submit(dir + L"\\context_menu.json", ToJson());
}

} // namespace pulse::app

// context_menu_prefs.cpp — JSON load/save for the Explorer fusion-zone prefs.
#include "context_menu_prefs.h"
#include "session.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <algorithm>
#include <unordered_set>
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

// Every key ToJson() writes (except "version"). A file truncated mid-write still
// parses into a handful of keys, so the count doubles as the completeness test that
// sends ReadDiskState() to the backup. A new key in ToJson() needs a line here too.
constexpr const wchar_t* kStoredKeys[] = {
    L"explorer_cap", L"open_with_mru", L"categories", L"items", L"seen", L"slow_ext"
};

// Half the keys, rounded up: fewer than this and the file is treated as damaged
// rather than as the user's configuration.
constexpr int kMinStoredKeys = (static_cast<int>(ARRAYSIZE(kStoredKeys)) + 1) / 2;

int CountStoredKeys(const std::wstring& json) {
    int found = 0;
    for (const wchar_t* key : kStoredKeys) {
        if (json.find(L"\"" + std::wstring(key) + L"\"") != std::wstring::npos) ++found;
    }
    return found;
}

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
    if (key.empty()) return false;
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
    if (json.empty()) return false;
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

    MigrateSeenKeys();
    CoalesceCompressCatalog();

    slow_ext.clear();
    const std::wstring slow = ExtractObject(json, L"slow_ext");
    if (!slow.empty()) {
        size_t p = 1;
        while (p < slow.size()) {
            while (p < slow.size() && slow[p] != L'"' && slow[p] != L'}') ++p;
            if (p >= slow.size() || slow[p] == L'}') break;
            ++p;
            const std::wstring key = pulse::json::UnescapeString(slow, p);
            size_t obj = slow.find(L'{', p);
            if (obj == std::wstring::npos) break;
            size_t end = slow.find(L'}', obj);
            if (end == std::wstring::npos) break;
            const std::wstring block = slow.substr(obj, end - obj + 1);
            SlowComExt st;
            st.last_ms = static_cast<uint32_t>(
                std::max(0, pulse::json::ExtractInt(block, L"ms", 0)));
            st.slow_hits = static_cast<uint32_t>(
                std::max(0, pulse::json::ExtractInt(block, L"slow", 0)));
            st.timeout_hits = static_cast<uint32_t>(
                std::max(0, pulse::json::ExtractInt(block, L"timeout", 0)));
            st.deferred = pulse::json::ExtractBool(block, L"deferred", false);
            st.disabled = pulse::json::ExtractBool(block, L"disabled", false);
            if (!key.empty()) slow_ext[key] = st;
            p = end + 1;
        }
    }
    return true;
}

// Older builds keyed the catalog by raw display text, so one verb showed up once
// per file type ("新建(N)" / "新建(W)", "用 X 打开" with and without spaces) and
// switching one of them off left its siblings on. Re-key every row through
// CatalogKey, merge the duplicates, and move the stored overrides onto the
// normalized keys so a switch the user already flipped keeps applying.
void ContextMenuPrefs::MigrateSeenKeys() {
    std::vector<SeenMenuItem> merged;
    merged.reserve(seen.size());
    for (auto& item : seen) {
        if (ipc::IsHandlerCatalogKey(item.key)) {
            merged.push_back(std::move(item));
            continue;
        }
        const std::wstring canonical = ipc::CatalogKey(item.text, item.flyout);
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
    for (const auto& kv : item_enabled) {
        std::wstring key = kv.first;
        if (!ipc::IsHandlerCatalogKey(key) && key.size() > 2 && key[1] == L':' &&
            (key[0] == L'v' || key[0] == L'f')) {
            key = key.substr(0, 2) + ipc::NormalizeCatalogText(key.substr(2));
        }
        moved[key] = kv.second;
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

// The main file first, its backup when the main one is missing, unreadable, or too
// incomplete to trust. This file is a setting like app.json, so it gets the same set
// of guarantees: a merge instead of a whole-file overwrite, a backup one step back, a
// completeness gate on the key count, and no write at all when nothing can be read. A
// backed-up file also heals a truncated one: the next Save() writes the merged state
// back over the damaged main file.
bool ContextMenuPrefs::ReadDiskState(ContextMenuPrefsValues& values, bool& main_exists,
                                     bool* used_backup) const {
    main_exists = false;
    if (used_backup) *used_backup = false;
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    const std::wstring main_path = dir + L"\\context_menu.json";
    const std::wstring backup_path = main_path + L".bak";
    main_exists = GetFileAttributesW(main_path.c_str()) != INVALID_FILE_ATTRIBUTES;

    std::wstring main_json;
    std::wstring backup_json;
    const bool main_read = ReadUtf8File(main_path, main_json) && !main_json.empty();
    const bool backup_read = ReadUtf8File(backup_path, backup_json) && !backup_json.empty();
    const int main_keys = main_read ? CountStoredKeys(main_json) : 0;
    const int backup_keys = backup_read ? CountStoredKeys(backup_json) : 0;

    // A complete main file wins outright; anything else is compared key by key, so a
    // half-written file never hides the copy that still has the settings in it.
    const std::wstring* source = nullptr;
    if (main_read && main_keys >= kMinStoredKeys) source = &main_json;
    else if (backup_read && backup_keys >= kMinStoredKeys) source = &backup_json;
    else if (main_read && backup_read)
        source = main_keys >= backup_keys ? &main_json : &backup_json;
    else if (main_read) source = &main_json;
    else if (backup_read) source = &backup_json;
    if (!source) return false;
    if (used_backup) *used_backup = source == &backup_json;

    ContextMenuPrefs parsed;
    parsed.persist = false;
    if (!parsed.FromJson(*source)) return false;
    values = parsed;
    return true;
}

ContextMenuPrefsValues ContextMenuPrefs::MergedWithDisk(
    const ContextMenuPrefsValues& disk) const {
    const ContextMenuPrefsValues& mine = *this;
    const ContextMenuPrefsValues& baseline = disk_state_;
    ContextMenuPrefsValues merged = mine;
    if (mine.software == baseline.software) merged.software = disk.software;
    if (mine.share == baseline.share) merged.share = disk.share;
    if (mine.wallpaper == baseline.wallpaper) merged.wallpaper = disk.wallpaper;
    if (mine.rotate == baseline.rotate) merged.rotate = disk.rotate;
    if (mine.shortcut == baseline.shortcut) merged.shortcut = disk.shortcut;
    if (mine.open_with == baseline.open_with) merged.open_with = disk.open_with;
    if (mine.open_with_com == baseline.open_with_com) merged.open_with_com = disk.open_with_com;
    if (mine.system_extra == baseline.system_extra) merged.system_extra = disk.system_extra;
    if (mine.print == baseline.print) merged.print = disk.print;
    if (mine.explorer_cap == baseline.explorer_cap) merged.explorer_cap = disk.explorer_cap;
    if (mine.open_with_mru == baseline.open_with_mru) merged.open_with_mru = disk.open_with_mru;
    if (mine.item_enabled == baseline.item_enabled) merged.item_enabled = disk.item_enabled;
    if (mine.seen == baseline.seen) merged.seen = disk.seen;
    if (mine.slow_ext == baseline.slow_ext) merged.slow_ext = disk.slow_ext;
    return merged;
}

bool ContextMenuPrefs::Load() {
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) {
        disk_state_ = *this;
        loaded_from_file_ = false;
        return false;
    }
    ContextMenuPrefsValues disk;
    bool main_exists = false;
    loaded_from_file_ = ReadDiskState(disk, main_exists);
    if (loaded_from_file_) static_cast<ContextMenuPrefsValues&>(*this) = disk;
    // What this process runs with is the state the next merge compares against.
    disk_state_ = *this;
    return loaded_from_file_;
}

bool ContextMenuPrefs::Save() const {
    if (!persist) return true;
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    const std::wstring path = dir + L"\\context_menu.json";

    ContextMenuPrefsValues disk;
    bool main_exists = false;
    bool used_backup = false;
    const bool disk_known = ReadDiskState(disk, main_exists, &used_backup);

    bool quarantined = false;
    if (!disk_known && main_exists) {
        // The file is there but nothing in it (or its backup) can be read: keep those
        // bytes as context_menu.json.bad instead of overwriting the only copy. A
        // failing rename means another process holds the file, so leave it alone.
        if (!QuarantineUnreadableFile(path)) return false;
        quarantined = true;
    }

    ContextMenuPrefs out = *this;
    if (disk_known) static_cast<ContextMenuPrefsValues&>(out) = MergedWithDisk(disk);

    // One step back for the next Load(); skipped when that file became the .bad
    // evidence, and when the values came from the backup that just saved us. The
    // policy has a single copy in utf8_file.h (KeepPreviousFileCopy), shared with
    // app.json.
    if (!quarantined && main_exists && !used_backup) KeepPreviousFileCopy(path);

    if (!WriteUtf8FileAtomic(path, out.ToJson())) return false;
    disk_state_ = *this;
    return true;
}

} // namespace pulse::app

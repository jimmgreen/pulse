// places.cpp — Persist workspaces / tags / network pins + NTFS ADS.
#include "places.h"
#include "tag_ads_sync.h"
#include "../common/config_json.h"
#include "session.h"
#include "../fs/fs_enum.h"
#include "../common/json_utils.h"
#include "../common/localization.h"
#include "../common/utf8_file.h"
#include "../common/runtime_log.h"
#include "../common/path_utils.h"
#include <algorithm>
#include <chrono>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <objbase.h>
#include <cwctype>
#include <chrono>

namespace pulse::app {
#ifdef PULSE_PLACES_SAVE_TEST
void PlacesSaveTestStage(unsigned stage);
#endif

namespace {

constexpr const wchar_t* kAdsSuffix = L":Pulse.Tag";

bool WriteCatalogDocument(const std::wstring& path, const std::wstring& document, bool tags) {
    if (!pulse::json::ValidConfigObject(document, true)) {
        SetLastError(ERROR_INVALID_DATA);
    } else if (WriteUtf8FileAtomic(path, document)) {
        return true;
    }
    const DWORD error = GetLastError();
    diagnostics::runtime::Event("places_save_failed", {{"tags", tags ? 1u : 0u},
        {"error", error}, {"characters", document.size()}});
    SetLastError(error);
    return false;
}

static std::wstring FolderTitle(std::wstring path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) path = L"\\\\" + path.substr(8);
    else if (path.starts_with(L"\\\\?\\")) path = path.substr(4);
    while (path.size() > 1 && path.back() == L'\\') path.pop_back();
    auto pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos && pos + 1 < path.size()) return path.substr(pos + 1);
    return path.empty() ? L"Workspace" : path;
}

static uint32_t ExtractRgb(const std::wstring& json) {
    size_t pos = pulse::json::ValuePosition(json, L"rgb");
    if (pos == std::wstring::npos) return 0xEF4444;
    if (pos + 2 < json.size() && json[pos] == L'0' && (json[pos + 1] == L'x' || json[pos + 1] == L'X')) {
        pos += 2;
        uint32_t v = 0;
        while (pos < json.size()) {
            wchar_t c = json[pos];
            int d = -1;
            if (c >= L'0' && c <= L'9') d = c - L'0';
            else if (c >= L'a' && c <= L'f') d = 10 + c - L'a';
            else if (c >= L'A' && c <= L'F') d = 10 + c - L'A';
            if (d < 0) break;
            v = (v << 4) | static_cast<uint32_t>(d);
            ++pos;
        }
        return v;
    }
    return static_cast<uint32_t>(pulse::json::ExtractInt(json, L"rgb"));
}

static bool ReadTagsGeneration(const std::wstring& json, uint64_t& generation) {
    generation = 0;
    if (pulse::json::ValuePosition(json, L"tags_generation") == std::wstring::npos) return true;
    const auto value = pulse::json::ExtractString(json, L"tags_generation");
    if (value.empty()) return false;
    for (wchar_t digit : value) {
        if (digit < L'0' || digit > L'9' || generation > (UINT64_MAX - (digit - L'0')) / 10) return false;
        generation = generation * 10 + (digit - L'0');
    }
    return generation != UINT64_MAX;
}

static std::wstring Norm(const std::wstring& p) {
    if (p.empty() || fs::IsVirtualPath(p)) return p;
    return fs::NormalizePath(p);
}

static std::wstring TagKey(const std::wstring& p) {
    std::wstring key = Norm(p);
    for (auto& c : key) c = static_cast<wchar_t>(std::towlower(c));
    return key;
}

// Comparison form of a quick-access layout key: built-in keys as they are,
// paths normalized and case-folded like the pin list.
static std::wstring QuickAccessKeyId(const std::wstring& key) {
    return IsBuiltinQuickAccessKey(key) ? key : TagKey(key);
}

static TagId NewTagId() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return L"tag-" + std::to_wstring(GetTickCount64());
    wchar_t text[40]{};
    StringFromGUID2(guid, text, ARRAYSIZE(text));
    std::wstring id = text;
    if (!id.empty() && id.front() == L'{') id.erase(id.begin());
    if (!id.empty() && id.back() == L'}') id.pop_back();
    for (auto& c : id) c = static_cast<wchar_t>(std::towlower(c));
    return id;
}

static std::wstring TrimTagName(std::wstring value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

static bool EqualI(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

static bool PathIsOrDescendant(const std::wstring& path, const std::wstring& root) {
    if (EqualI(path, root)) return true;
    if (path.size() <= root.size() || _wcsnicmp(path.c_str(), root.c_str(), root.size()) != 0)
        return false;
    return root.ends_with(L"\\") || path[root.size()] == L'\\' || path[root.size()] == L'/';
}

static std::vector<std::wstring> ExtractObjectArray(const std::wstring& json,
                                                     const wchar_t* key) {
    std::vector<std::wstring> out;
    size_t pos = pulse::json::ValuePosition(json, key);
    if (pos == std::wstring::npos || pos >= json.size() || json[pos] != L'[') return out;
    int array_depth = 0;
    int object_depth = 0;
    bool in_string = false;
    size_t object_start = std::wstring::npos;
    for (size_t i = pos; i < json.size(); ++i) {
        const wchar_t c = json[i];
        if (in_string) {
            if (c == L'\\') ++i;
            else if (c == L'"') in_string = false;
            continue;
        }
        if (c == L'"') in_string = true;
        else if (c == L'[') ++array_depth;
        else if (c == L']') {
            if (--array_depth == 0) break;
        } else if (c == L'{') {
            if (object_depth++ == 0) object_start = i;
        } else if (c == L'}' && object_depth > 0) {
            if (--object_depth == 0 && object_start != std::wstring::npos) {
                out.push_back(json.substr(object_start, i - object_start + 1));
                object_start = std::wstring::npos;
            }
        }
    }
    return out;
}

static const wchar_t* KindName(PlaceItemKind kind) {
    if (kind == PlaceItemKind::Folder) return L"folder";
    if (kind == PlaceItemKind::File) return L"file";
    return L"unknown";
}

static PlaceItemKind ParseKind(const std::wstring& value) {
    if (value == L"folder") return PlaceItemKind::Folder;
    if (value == L"file") return PlaceItemKind::File;
    return PlaceItemKind::Unknown;
}

static uint64_t NowFileTime() {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

static std::wstring TrimBadge(std::wstring value) {
    value = TrimTagName(std::move(value));
    if (value.size() > 12) value.resize(12);
    return value;
}

} // namespace

PlacesCatalog::~PlacesCatalog() {
    StopPlacesWriter();
    FlushPendingSave(true);
    StopTagWriter();
}

bool PlacesCatalog::SaveTagFile(const std::vector<ColorTag>& snapshot, uint64_t generation) {
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    std::wostringstream file;
    auto write_array = [&](const std::vector<std::wstring>& values) {
        file << L"[";
        for (size_t i = 0; i < values.size(); ++i) {
            if (i) file << L",";
            std::wstring escaped;
            pulse::json::Escape(values[i], escaped);
            file << L"\"" << escaped << L"\"";
        }
        file << L"]";
    };
    file << L"{\n  \"version\":3,\n  \"tags_generation\":\"" << generation << L"\",\n  \"tags\":[\n";
    for (size_t i = 0; i < snapshot.size(); ++i) {
        std::wstring id, name;
        pulse::json::Escape(snapshot[i].id, id);
        pulse::json::Escape(snapshot[i].name, name);
        wchar_t rgb[16]{};
        swprintf_s(rgb, L"0x%06X", snapshot[i].rgb & 0xFFFFFFu);
        file << L"    {\"id\":\"" << id << L"\",\"name\":\"" << name
             << L"\",\"rgb\":" << rgb;
        // v3: optional favorite slot; older versions ignore the field.
        if (snapshot[i].favorite > 0) file << L",\"fav\":" << snapshot[i].favorite;
        if (!snapshot[i].group.empty()) {
            std::wstring group;
            pulse::json::Escape(snapshot[i].group, group);
            file << L",\"group\":\"" << group << L"\"";
        }
        file << L",\"paths\":";
        write_array(snapshot[i].paths);
        file << L"}" << (i + 1 < snapshot.size() ? L"," : L"") << L"\n";
    }
    file << L"  ]\n}\n";
    std::wstring document = file.str();
    document.insert(document.find(L'{') + 1, L"\n\"save_generation\":\"" + std::to_wstring(generation) + L"\",");
    if (WriteCatalogDocument(dir + L"\\tags.json", document, true)) {
        DeleteFileW((dir + L"\\tags.pending.json").c_str());
        return true;
    }
    // A failed final rename may still permit a recovery copy. Its generation
    // lets startup distinguish it from an older journal that could not be removed.
    WriteCatalogDocument(dir + L"\\tags.pending.json", document, true);
    return false;
}

void PlacesCatalog::QueueTagSave() const {
    if (!persist || load_failed) return;
    {
        std::lock_guard lock(tag_save_mutex_);
        pending_tag_save_ = std::make_pair(tags, ++tags_generation_);
        if (!tag_save_thread_.joinable()) {
            tag_save_stop_ = false;
            tag_save_thread_ = std::thread([this] {
                uint64_t attempted_generation = 0;
                unsigned attempts = 0;
                for (;;) {
                    std::pair<std::vector<ColorTag>, uint64_t> snapshot;
                    bool stopping;
                    {
                        std::unique_lock lock(tag_save_mutex_);
                        tag_save_cv_.wait(lock, [&] {
                            return tag_save_stop_ || (pending_tag_save_ &&
                                (pending_tag_save_->second != attempted_generation || attempts < 3));
                        });
                        if (!pending_tag_save_) {
                            if (tag_save_stop_) return;
                            continue;
                        }
                        snapshot = *pending_tag_save_;
                        stopping = tag_save_stop_;
                        if (snapshot.second != attempted_generation) {
                            attempted_generation = snapshot.second;
                            attempts = 0;
                        }
                    }
                    bool saved = false, current;
                    DWORD error = ERROR_SUCCESS;
                    {
                        std::lock_guard io_lock(tag_save_io_mutex_);
                        {
                            std::lock_guard state_lock(tag_save_mutex_);
                            current = snapshot.second == tags_generation_;
                        }
                        if (current) {
                            saved = SaveTagFile(snapshot.first, snapshot.second);
                            if (!saved) { error = GetLastError(); if (!error) error = ERROR_WRITE_FAULT; }
                        }
                        std::lock_guard state_lock(tag_save_mutex_);
                        if (current && snapshot.second == tags_generation_) {
                            tag_save_error_ = error;
                            if (saved) pending_tag_save_.reset();
                            else ++attempts;
                        }
                    }
                    std::unique_lock lock(tag_save_mutex_);
                    if (stopping) {
                        if (pending_tag_save_) OutputDebugStringW(L"Pulse: final tag save failed; check the places.json recovery copy.\n");
                        return;
                    }
                    if (current && !saved && attempts >= 3)
                        OutputDebugStringW(L"Pulse: tag save retries exhausted; tag changes remain pending.\n");
                    if (pending_tag_save_ && pending_tag_save_->second == snapshot.second && attempts < 3) {
                        tag_save_cv_.wait_for(lock, std::chrono::milliseconds(250 * attempts), [&] {
                            return tag_save_stop_ || !pending_tag_save_ || pending_tag_save_->second != snapshot.second;
                        });
                    }
                }
            });
        }
    }
    // The existing places file is the recovery copy if the dedicated tag file
    // cannot be replaced. Every tag generation must reach that save channel too.
    MarkPlacesDirty();
    tag_save_cv_.notify_one();
}

DWORD PlacesCatalog::TagSaveError() const {
    std::lock_guard lock(tag_save_mutex_);
    return tag_save_error_;
}

bool PlacesCatalog::FlushTagSave() const {
    if (!persist) return true;
    if (load_failed) return false;
    uint64_t generation;
    const auto snapshot = tags;
    {
        std::lock_guard lock(tag_save_mutex_);
        generation = tags_generation_;
    }
    std::lock_guard io_lock(tag_save_io_mutex_);
    const bool saved = SaveTagFile(snapshot, generation);
    DWORD error = saved ? ERROR_SUCCESS : GetLastError();
    if (!saved && !error) error = ERROR_WRITE_FAULT;
    {
        std::lock_guard lock(tag_save_mutex_);
        if (generation == tags_generation_) {
            tag_save_error_ = error;
            if (saved) pending_tag_save_.reset();
            else pending_tag_save_ = std::make_pair(snapshot, generation);
        }
    }
    tag_save_cv_.notify_all();
    if (!saved) {
        MarkPlacesDirty();
        OutputDebugStringW(L"Pulse: tag flush failed; changes remain pending for recovery.\n");
    }
    return saved;
}

void PlacesCatalog::StopTagWriter() {
    {
        std::lock_guard<std::mutex> lock(tag_save_mutex_);
        tag_save_stop_ = true;
    }
    tag_save_cv_.notify_all();
    if (tag_save_thread_.joinable()) tag_save_thread_.join();
}

void PlacesCatalog::EnsureDefaults() {
    if (!tags.empty()) return;
    struct Def { const wchar_t* id; const wchar_t* name; const wchar_t* english; uint32_t rgb; };
    static const Def kDefs[] = {
        { L"finder-red", L"紧急修补", L"Urgent", 0xEF4444 },
        { L"finder-orange", L"设计审阅", L"Design review", 0xF59E0B },
        { L"finder-green", L"进行中", L"In progress", 0x22C55E },
        { L"finder-yellow", L"待确认", L"Pending", 0xEAB308 },
        { L"finder-purple", L"灵感参考", L"Inspiration", 0xA855F7 },
        { L"finder-blue", L"参考资料", L"Reference", 0x3B82F6 },
        { L"finder-gray", L"归档", L"Archive", 0x94A3B8 },
    };
    for (const auto& d : kDefs) {
        ColorTag t;
        t.id = d.id;
        t.name = l10n::Pick(d.name, d.english);  // seeded once in the UI language, then user data
        t.rgb = d.rgb;
        t.favorite = static_cast<int>(tags.size()) + 1;
        tags.push_back(std::move(t));
    }
    RebuildTagIndex();
}

bool PlacesCatalog::Load() {
    StopPlacesWriter();
    StopTagWriter();
    pending_tag_save_.reset();
    const std::wstring dir = GetPulseDataDir();
    std::wstring json, tag_json;
    auto read = [&](const wchar_t* name, std::wstring& text) {
        const std::wstring file = dir + name;
        if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return true;
        }
        return ReadUtf8File(file, text) && pulse::json::ValidConfigObject(text, true);
    };
    if (dir.empty() || !read(L"\\places.json", json) || !read(L"\\tags.json", tag_json)) {
        load_failed = true;
        return false;
    }
    uint64_t places_generation = 0, tag_generation = 0;
    if (!ReadTagsGeneration(json, places_generation) || !ReadTagsGeneration(tag_json, tag_generation)) {
        load_failed = true;
        return false;
    }
    load_failed = false;
    const auto generation = [](const std::wstring& document) {
        const auto text = pulse::json::ExtractString(document, L"save_generation");
        return text.empty() ? uint64_t{0} : _wcstoui64(text.c_str(), nullptr, 10);
    };
    std::wstring recovery;
    const bool recovery_valid = read(L"\\tags.pending.json", recovery) && !recovery.empty();
    const bool recovered = recovery_valid && generation(recovery) > generation(tag_json);
    if (recovered) tag_json = std::move(recovery);
    if (!ReadTagsGeneration(tag_json, tag_generation)) { load_failed = true; return false; }
    workspaces.clear();
    tags.clear();
    tag_groups.clear();
    tag_sidebar_mode = TagSidebarMode::FavoritesAndGroups;
    networks.clear();
    quick_access_paths.clear();
    quick_access_order.clear();
    starred_items.clear();
    starred_groups.clear();
    quick_access_badges.clear();
    recent_items.clear();
    starred_index_.clear();
    active_workspace = -1;
    const bool places_loaded = !json.empty();

    active_workspace = pulse::json::ExtractInt(json, L"active_workspace");
    if (json.find(L"\"active_workspace\"") == std::wstring::npos) active_workspace = -1;

    for (const auto& block : ExtractObjectArray(json, L"workspaces")) {
        Workspace w;
        w.expanded = pulse::json::ExtractInt(block, L"expanded") != 0;
        if (block.find(L"\"color_rgb\"") != std::wstring::npos)
            w.color_rgb = static_cast<uint32_t>(pulse::json::ExtractInt(block, L"color_rgb")) & 0xFFFFFF;
        w.name = pulse::json::ExtractString(block, L"name");
        w.root = pulse::json::ExtractString(block, L"root");
        w.layout = pulse::json::ExtractInt(block, L"layout");
        w.pane_paths = pulse::json::ExtractStringArray(block, L"panes");
        for (const auto& value : pulse::json::ExtractStringArray(block, L"views"))
            w.pane_views.push_back(ui::ParseViewMode(value));
        while (w.pane_views.size() < w.pane_paths.size())
            w.pane_views.push_back(ui::ViewMode::Details);
        auto freq = pulse::json::ExtractStringArray(block, L"freq");
        for (const auto& row : freq) {
            auto tab = row.find(L'\t');
            std::pair<std::wstring, int> hit;
            if (tab == std::wstring::npos) {
                hit.first = row;
                hit.second = 1;
            } else {
                hit.first = row.substr(0, tab);
                hit.second = _wtoi(row.c_str() + tab + 1);
            }
            if (!hit.first.empty()) w.frequent.push_back(std::move(hit));
        }
        if (!w.root.empty()) workspaces.push_back(std::move(w));
    }

    for (const auto& block : ExtractObjectArray(json, L"tags")) {
        ColorTag t;
        t.id = pulse::json::ExtractString(block, L"id");
        if (t.id.empty()) t.id = NewTagId();
        t.name = pulse::json::ExtractString(block, L"name");
        t.rgb = ExtractRgb(block);
        t.paths = pulse::json::ExtractStringArray(block, L"paths");
        t.favorite = pulse::json::ExtractInt(block, L"fav");
        t.group = pulse::json::ExtractString(block, L"group");
        if (!t.name.empty()) tags.push_back(std::move(t));
    }

    for (const auto& block : ExtractObjectArray(json, L"tag_groups")) {
        TagGroup group;
        group.id = pulse::json::ExtractString(block, L"id");
        group.name = TrimTagName(pulse::json::ExtractString(block, L"name"));
        group.collapsed = pulse::json::ExtractBool(block, L"collapsed", false);
        if (!group.id.empty() && !group.name.empty() && !FindTagGroup(group.id))
            tag_groups.push_back(std::move(group));
    }
    {
        const int mode = pulse::json::ExtractInt(json, L"tag_sidebar_mode");
        tag_sidebar_mode = mode >= 0 && mode <= 2 ? static_cast<TagSidebarMode>(mode)
                                                  : TagSidebarMode::FavoritesAndGroups;
    }

    for (const auto& block : ExtractObjectArray(json, L"networks")) {
        NetworkPlace n;
        n.name = pulse::json::ExtractString(block, L"name");
        n.unc = pulse::json::ExtractString(block, L"unc");
        if (!n.unc.empty()) networks.push_back(std::move(n));
    }

    for (const auto& path : pulse::json::ExtractStringArray(json, L"quick_access_paths")) {
        const auto normalized = Norm(path);
        if (!normalized.empty() && !fs::IsVirtualPath(normalized) &&
            !IsQuickAccessPinned(normalized)) quick_access_paths.push_back(normalized);
    }
    {
        std::unordered_set<std::wstring> order_seen;
        for (const auto& key : pulse::json::ExtractStringArray(json, L"quick_access_order")) {
            const bool builtin = IsBuiltinQuickAccessKey(key);
            std::wstring value = builtin ? key : Norm(key);
            if (value.empty() || (!builtin && fs::IsVirtualPath(value))) continue;
            if (order_seen.insert(QuickAccessKeyId(value)).second)
                quick_access_order.push_back(std::move(value));
        }
    }

    const bool has_starred_items = pulse::json::ValuePosition(
        json, L"starred_items") != std::wstring::npos;
    for (const auto& block : ExtractObjectArray(json, L"starred_items")) {
        StarredItem item;
        item.path = Norm(pulse::json::ExtractString(block, L"path"));
        item.kind = ParseKind(pulse::json::ExtractString(block, L"kind"));
        item.badge = TrimBadge(pulse::json::ExtractString(block, L"badge"));
        item.badge_rgb = ExtractRgb(block);
        item.group = pulse::json::ExtractString(block, L"group");
        item.separator_after = pulse::json::ExtractBool(block, L"sep", false);
        if (item.kind == PlaceItemKind::File) item.group.clear();
        if (!item.path.empty() && !fs::IsVirtualPath(item.path) &&
            std::none_of(starred_items.begin(), starred_items.end(), [&](const StarredItem& old) {
                return EqualI(old.path, item.path);
            })) {
            starred_items.push_back(std::move(item));
        }
    }
    if (!has_starred_items) {
        for (const auto& path : pulse::json::ExtractStringArray(json, L"starred")) {
            const std::wstring normalized = Norm(path);
            if (!normalized.empty() && !fs::IsVirtualPath(normalized))
                starred_items.push_back({ normalized });
        }
    }
    for (const auto& block : ExtractObjectArray(json, L"starred_groups")) {
        StarredGroup group;
        group.id = pulse::json::ExtractString(block, L"id");
        group.name = TrimTagName(pulse::json::ExtractString(block, L"name"));
        group.collapsed = pulse::json::ExtractBool(block, L"collapsed", false);
        group.separator_after = pulse::json::ExtractBool(block, L"sep", false);
        if (!group.id.empty() && !group.name.empty() && !FindStarredGroup(group.id))
            starred_groups.push_back(std::move(group));
    }
    for (auto& item : starred_items)
        if (!item.group.empty() && !FindStarredGroup(item.group)) item.group.clear();
    for (const auto& block : ExtractObjectArray(json, L"quick_access_badges")) {
        QuickAccessBadge item;
        item.path = Norm(pulse::json::ExtractString(block, L"path"));
        item.badge = TrimBadge(pulse::json::ExtractString(block, L"badge"));
        item.badge_rgb = ExtractRgb(block) & 0xFFFFFFu;
        if (!item.path.empty() && !FindQuickAccessBadge(item.path))
            quick_access_badges.push_back(std::move(item));
    }
    for (const auto& block : ExtractObjectArray(json, L"recent_items")) {
        RecentItem item;
        item.path = Norm(pulse::json::ExtractString(block, L"path"));
        item.kind = ParseKind(pulse::json::ExtractString(block, L"kind"));
        const std::wstring opened = pulse::json::ExtractString(block, L"opened_at");
        item.opened_at = opened.empty() ? 0 : _wcstoui64(opened.c_str(), nullptr, 10);
        if (!item.path.empty() && !fs::IsVirtualPath(item.path) &&
            std::none_of(recent_items.begin(), recent_items.end(), [&](const RecentItem& old) {
                return EqualI(old.path, item.path);
            })) {
            recent_items.push_back(std::move(item));
        }
    }
    std::stable_sort(recent_items.begin(), recent_items.end(),
        [](const RecentItem& a, const RecentItem& b) { return a.opened_at > b.opened_at; });
    if (recent_items.size() > 100) recent_items.resize(100);
    std::stable_partition(starred_items.begin(), starred_items.end(),
        [](const StarredItem& item) { return item.kind == PlaceItemKind::Folder; });

    bool tags_loaded = false;
    if (!tag_json.empty()) {
        std::vector<ColorTag> loaded;
        for (const auto& block : ExtractObjectArray(tag_json, L"tags")) {
            ColorTag tag;
            tag.id = pulse::json::ExtractString(block, L"id");
            tag.name = pulse::json::ExtractString(block, L"name");
            tag.rgb = ExtractRgb(block);
            tag.paths = pulse::json::ExtractStringArray(block, L"paths");
            tag.favorite = pulse::json::ExtractInt(block, L"fav");
            tag.group = pulse::json::ExtractString(block, L"group");
            if (!tag.id.empty() && !tag.name.empty()) loaded.push_back(std::move(tag));
        }
        const auto position = pulse::json::ValuePosition(tag_json, L"tags");
        bool empty_array = false;
        if (position != std::wstring::npos && tag_json[position] == L'[') {
            const auto next = tag_json.find_first_not_of(L" \t\r\n", position + 1);
            empty_array = next != std::wstring::npos && tag_json[next] == L']';
        }
        if ((!loaded.empty() || (tag_generation && empty_array)) && tag_generation >= places_generation) {
            tags = std::move(loaded);
            tags_loaded = true;
        }
    }

    {
        std::lock_guard lock(tag_save_mutex_);
        tags_generation_ = (std::max)(places_generation, tag_generation);
        pending_tag_save_.reset();
        tag_save_error_ = ERROR_SUCCESS;
    }
    // Defaults only seed a catalog that has no saved tags yet: a saved empty
    // list means the user deleted them, and they stay deleted.
    if (!tags_loaded) EnsureDefaults();
    // Before tags file v3 Alt+N meant "the Nth tag": the first seven become
    // the favorites, so the keys keep doing what they did.
    const bool favorites_migrated = tags_loaded
        ? pulse::json::ExtractInt(tag_json, L"version") < 3
        : places_loaded && std::none_of(tags.begin(), tags.end(),
                                        [](const ColorTag& tag) { return tag.favorite > 0; });
    if (favorites_migrated) {
        for (size_t i = 0; i < tags.size(); ++i)
            tags[i].favorite = i < 7 ? static_cast<int>(i) + 1 : 0;
    }
    NormalizeFavoriteSlots();
    RebuildTagIndex();
    RebuildStarIndex();
    if (recovered || favorites_migrated || (places_loaded && !tags_loaded)) QueueTagSave();
    return places_loaded || tags_loaded;
}

bool ParsePulsePath(const std::wstring& path, std::wstring* kind, std::wstring* rest) {
    constexpr std::wstring_view kPrefix = L"pulse:";
    if (!path.starts_with(kPrefix)) return false;
    std::wstring_view v = std::wstring_view(path).substr(kPrefix.size());
    const auto colon = v.find(L':');
    if (colon == std::wstring_view::npos) {
        if (kind) *kind = std::wstring(v);
        if (rest) rest->clear();
        return true;
    }
    if (kind) *kind = std::wstring(v.substr(0, colon));
    if (rest) *rest = std::wstring(v.substr(colon + 1));
    return true;
}

PlacesCatalog::SaveSnapshot PlacesCatalog::CaptureSaveSnapshot() const {
    SaveSnapshot snapshot;
    snapshot.workspaces = workspaces;
    snapshot.tags = tags;
    snapshot.tag_groups = tag_groups;
    snapshot.tag_sidebar_mode = tag_sidebar_mode;
    {
        std::lock_guard lock(tag_save_mutex_);
        snapshot.tags_generation = tags_generation_;
    }
    snapshot.networks = networks;
    snapshot.quick_access_paths = quick_access_paths;
    snapshot.quick_access_order = quick_access_order;
    snapshot.starred_items = starred_items;
    // A group outlives its last member in memory (the snack's undo brings
    // the member back into it) but is not saved.
    for (const auto& group : starred_groups)
        if (std::any_of(starred_items.begin(), starred_items.end(),
                        [&](const StarredItem& item) { return item.group == group.id; }))
            snapshot.starred_groups.push_back(group);
    snapshot.quick_access_badges = quick_access_badges;
    snapshot.recent_items = recent_items;
    snapshot.active_workspace = active_workspace;
    snapshot.persist = persist;
    return snapshot;
}

void PlacesCatalog::MarkPlacesDirty() const {
#ifdef PULSE_PLACES_SAVE_TEST
    PlacesSaveTestStage(3);
#endif
    std::lock_guard lock(places_save_mutex_);
    ++places_save_revision_;
    places_save_due_ = GetTickCount64() + 1000;
}

void PlacesCatalog::QueuePlacesSave() const {
    if (!persist || load_failed) return;
    auto snapshot = CaptureSaveSnapshot();
    {
        std::lock_guard<std::mutex> lock(places_save_mutex_);
        pending_places_save_ = std::make_pair(std::move(snapshot), places_save_revision_);
        if (!places_save_thread_.joinable()) {
            places_save_stop_ = false;
            places_save_thread_ = std::thread([this] {
                for (;;) {
                    std::optional<std::pair<SaveSnapshot, uint64_t>> pending;
                    {
                        std::unique_lock<std::mutex> lock(places_save_mutex_);
                        places_save_cv_.wait(lock, [this] {
                            return places_save_stop_ || pending_places_save_.has_value();
                        });
                        if (pending_places_save_) {
                            pending = std::move(pending_places_save_);
                            pending_places_save_.reset();
                        } else if (places_save_stop_) {
                            return;
                        }
                    }
                    if (pending) {
#ifdef PULSE_PLACES_SAVE_TEST
                        PlacesSaveTestStage(1);
#endif
                        bool saved = false;
                        {
                            std::lock_guard io_lock(places_save_io_mutex_);
                            bool current = false;
                            {
                                std::lock_guard state_lock(places_save_mutex_);
                                current = places_save_revision_ == pending->second;
                            }
                            if (current) saved = SaveSnapshotFile(pending->first);
                        }
                        std::lock_guard state_lock(places_save_mutex_);
                        if (saved && places_save_revision_ == pending->second) {
#ifdef PULSE_PLACES_SAVE_TEST
                            PlacesSaveTestStage(2);
#endif
                            places_save_due_ = 0;
                        }
                    }
                    std::lock_guard<std::mutex> lock(places_save_mutex_);
                    if (places_save_stop_ && !pending_places_save_) return;
                }
            });
        }
    }
    places_save_cv_.notify_one();
}

void PlacesCatalog::StopPlacesWriter() {
    {
        std::lock_guard<std::mutex> lock(places_save_mutex_);
        places_save_stop_ = true;
    }
    places_save_cv_.notify_all();
    if (places_save_thread_.joinable()) places_save_thread_.join();
}

bool PlacesCatalog::Save() const {
    if (!persist) return true;
    if (load_failed) return false;
    if (async_save_depth_) {
        MarkPlacesDirty();
        async_save_dirty_ = true;
        return true;
    }
    const SaveSnapshot snapshot = CaptureSaveSnapshot();
    uint64_t revision;
    {
        std::lock_guard state_lock(places_save_mutex_);
        revision = ++places_save_revision_;
        places_save_due_ = GetTickCount64() + 1000;
        pending_places_save_.reset();
    }
    std::lock_guard io_lock(places_save_io_mutex_);
    const bool saved = SaveSnapshotFile(snapshot);
    {
        std::lock_guard state_lock(places_save_mutex_);
        if (saved && places_save_revision_ == revision) places_save_due_ = 0;
    }
    return saved;
}

bool PlacesCatalog::SaveSnapshotFile(const SaveSnapshot& snapshot) {
    std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    std::wostringstream f;
    auto writeArr = [&](const std::vector<std::wstring>& arr) {
        f << L"[";
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i) f << L",";
            std::wstring e;
            pulse::json::Escape(arr[i], e);
            f << L"\"" << e << L"\"";
        }
        f << L"]";
    };
    f << L"{\n  \"places_version\":2,\n  \"tag_version\":2,\n  \"tags_generation\":\"" << snapshot.tags_generation
      << L"\",\n  \"active_workspace\":" << snapshot.active_workspace
      << L",\n  \"workspaces\":[\n";
    for (size_t i = 0; i < snapshot.workspaces.size(); ++i) {
        const auto& w = snapshot.workspaces[i];
        std::wstring name, root;
        pulse::json::Escape(w.name, name);
        pulse::json::Escape(w.root, root);
        f << L"    {\"name\":\"" << name << L"\",\"root\":\"" << root
          << L"\",\"layout\":" << w.layout << L",\"expanded\":" << (w.expanded ? 1 : 0)
          << L",\"color_rgb\":" << w.color_rgb << L",\"panes\":";
        writeArr(w.pane_paths);
        std::vector<std::wstring> views;
        views.reserve(w.pane_views.size());
        for (ui::ViewMode mode : w.pane_views) views.push_back(ui::ViewModeName(mode));
        f << L",\"views\":";
        writeArr(views);
        std::vector<std::wstring> freq;
        freq.reserve(w.frequent.size());
        for (const auto& hit : w.frequent)
            freq.push_back(hit.first + L"\t" + std::to_wstring(hit.second));
        f << L",\"freq\":";
        writeArr(freq);
        f << L"}";
        if (i + 1 < snapshot.workspaces.size()) f << L",";
        f << L"\n";
    }
    f << L"  ],\n  \"tags\":[\n";
    for (size_t i = 0; i < snapshot.tags.size(); ++i) {
        const auto& t = snapshot.tags[i];
        std::wstring id, name;
        pulse::json::Escape(t.id, id);
        pulse::json::Escape(t.name, name);
        wchar_t rgb[16];
        swprintf_s(rgb, L"0x%06X", t.rgb & 0xFFFFFFu);
        f << L"    {\"id\":\"" << id << L"\",\"name\":\"" << name
          << L"\",\"rgb\":" << rgb;
        if (t.favorite > 0) f << L",\"fav\":" << t.favorite;
        if (!t.group.empty()) {
            std::wstring group;
            pulse::json::Escape(t.group, group);
            f << L",\"group\":\"" << group << L"\"";
        }
        f << L",\"paths\":";
        writeArr(t.paths);
        f << L"}";
        if (i + 1 < snapshot.tags.size()) f << L",";
        f << L"\n";
    }
    f << L"  ],\n  \"tag_groups\":[\n";
    for (size_t i = 0; i < snapshot.tag_groups.size(); ++i) {
        const auto& g = snapshot.tag_groups[i];
        std::wstring id, name;
        pulse::json::Escape(g.id, id);
        pulse::json::Escape(g.name, name);
        f << L"    {\"id\":\"" << id << L"\",\"name\":\"" << name << L"\",\"collapsed\":"
          << (g.collapsed ? L"true" : L"false") << L"}";
        if (i + 1 < snapshot.tag_groups.size()) f << L",";
        f << L"\n";
    }
    f << L"  ],\n  \"tag_sidebar_mode\":" << static_cast<int>(snapshot.tag_sidebar_mode);
    f << L",\n  \"networks\":[\n";
    for (size_t i = 0; i < snapshot.networks.size(); ++i) {
        const auto& n = snapshot.networks[i];
        std::wstring name, unc;
        pulse::json::Escape(n.name, name);
        pulse::json::Escape(n.unc, unc);
        f << L"    {\"name\":\"" << name << L"\",\"unc\":\"" << unc << L"\"}";
        if (i + 1 < snapshot.networks.size()) f << L",";
        f << L"\n";
    }
    std::vector<std::wstring> legacy_starred;
    legacy_starred.reserve(snapshot.starred_items.size());
    for (const auto& item : snapshot.starred_items) legacy_starred.push_back(item.path);
    f << L"  ],\n  \"starred\":";
    writeArr(legacy_starred);
    f << L",\n  \"quick_access_paths\":";
    writeArr(snapshot.quick_access_paths);
    f << L",\n  \"quick_access_order\":";
    writeArr(snapshot.quick_access_order);
    f << L",\n  \"starred_items\":[\n";
    for (size_t i = 0; i < snapshot.starred_items.size(); ++i) {
        const auto& item = snapshot.starred_items[i];
        std::wstring path, badge;
        pulse::json::Escape(item.path, path);
        pulse::json::Escape(item.badge, badge);
        wchar_t rgb[16]{};
        swprintf_s(rgb, L"0x%06X", item.badge_rgb & 0xFFFFFFu);
        f << L"    {\"path\":\"" << path << L"\",\"kind\":\""
          << KindName(item.kind) << L"\",\"badge\":\"" << badge
          << L"\",\"rgb\":" << rgb;
        if (!item.group.empty()) {
            std::wstring group;
            pulse::json::Escape(item.group, group);
            f << L",\"group\":\"" << group << L"\"";
        }
        if (item.separator_after) f << L",\"sep\":true";
        f << L"}";
        if (i + 1 < snapshot.starred_items.size()) f << L",";
        f << L"\n";
    }
    f << L"  ],\n  \"starred_groups\":[\n";
    for (size_t i = 0; i < snapshot.starred_groups.size(); ++i) {
        const auto& g = snapshot.starred_groups[i];
        std::wstring id, name;
        pulse::json::Escape(g.id, id);
        pulse::json::Escape(g.name, name);
        f << L"    {\"id\":\"" << id << L"\",\"name\":\"" << name << L"\",\"collapsed\":"
          << (g.collapsed ? L"true" : L"false") << L",\"sep\":" << (g.separator_after ? L"true" : L"false")
          << L"}";
        if (i + 1 < snapshot.starred_groups.size()) f << L",";
        f << L"\n";
    }
    f << L"  ],\n  \"quick_access_badges\":[\n";
    for (size_t i = 0; i < snapshot.quick_access_badges.size(); ++i) {
        const auto& item = snapshot.quick_access_badges[i];
        std::wstring path, badge;
        pulse::json::Escape(item.path, path);
        pulse::json::Escape(item.badge, badge);
        wchar_t rgb[16]{};
        swprintf_s(rgb, L"0x%06X", item.badge_rgb & 0xFFFFFFu);
        f << L"    {\"path\":\"" << path << L"\",\"badge\":\"" << badge
          << L"\",\"rgb\":" << rgb << L"}";
        if (i + 1 < snapshot.quick_access_badges.size()) f << L",";
        f << L"\n";
    }
    f << L"  ],\n  \"recent_items\":[\n";
    for (size_t i = 0; i < snapshot.recent_items.size(); ++i) {
        const auto& item = snapshot.recent_items[i];
        std::wstring path;
        pulse::json::Escape(item.path, path);
        f << L"    {\"path\":\"" << path << L"\",\"kind\":\""
          << KindName(item.kind) << L"\",\"opened_at\":\""
          << item.opened_at << L"\"}";
        if (i + 1 < snapshot.recent_items.size()) f << L",";
        f << L"\n";
    }
    f << L"  ]\n}\n";
    DeleteFileW((dir + L"\\places.tmp").c_str());
    const std::wstring document = f.str();
    const std::wstring path = dir + L"\\places.json";
    std::wstring existing;
    if (ReadUtf8File(path, existing) && existing == document) return true;
    return WriteCatalogDocument(path, document, false);
}

bool PlacesCatalog::FlushPendingSave(bool force) const {
    ULONGLONG due;
    {
        std::lock_guard lock(places_save_mutex_);
        due = places_save_due_;
    }
    if (!due) return true;
    if (!force && GetTickCount64() < due) return true;
    if (force) return Save();
    QueuePlacesSave();
    return true;
}

int PlacesCatalog::FindWorkspace(const std::wstring& root) const {
    const std::wstring n = Norm(root);
    for (int i = 0; i < static_cast<int>(workspaces.size()); ++i)
        if (Norm(workspaces[static_cast<size_t>(i)].root) == n) return i;
    return -1;
}

int PlacesCatalog::PinWorkspace(const std::wstring& root, const std::wstring& name, int layout,
                                const std::vector<std::wstring>& pane_paths,
                                const std::vector<ui::ViewMode>& pane_views) {
    const std::wstring n = Norm(root);
    int existing = FindWorkspace(n);
    if (existing >= 0) {
        UpdateWorkspaceSnapshot(existing, layout, pane_paths, pane_views);
        if (!name.empty()) workspaces[static_cast<size_t>(existing)].name = name;
        active_workspace = existing;
        Save();
        return existing;
    }
    Workspace w;
    w.root = n;
    w.name = name.empty() ? FolderTitle(n) : name;
    w.layout = layout;
    w.pane_paths = pane_paths;
    w.pane_views = pane_views;
    while (w.pane_views.size() < w.pane_paths.size())
        w.pane_views.push_back(ui::ViewMode::Details);
    workspaces.push_back(std::move(w));
    active_workspace = static_cast<int>(workspaces.size()) - 1;
    Save();
    return active_workspace;
}

bool PlacesCatalog::UnpinWorkspace(int index) {
    if (index < 0 || index >= static_cast<int>(workspaces.size())) return false;
    workspaces.erase(workspaces.begin() + index);
    if (active_workspace == index) active_workspace = -1;
    else if (active_workspace > index) --active_workspace;
    Save();
    return true;
}

bool PlacesCatalog::UnpinWorkspace(const std::wstring& root) {
    return UnpinWorkspace(FindWorkspace(root));
}

void PlacesCatalog::UpdateWorkspaceSnapshot(int index, int layout,
                                            const std::vector<std::wstring>& pane_paths,
                                            const std::vector<ui::ViewMode>& pane_views) {
    if (index < 0 || index >= static_cast<int>(workspaces.size())) return;
    auto& w = workspaces[static_cast<size_t>(index)];
    w.layout = layout;
    w.pane_paths = pane_paths;
    w.pane_views = pane_views;
    while (w.pane_views.size() < w.pane_paths.size())
        w.pane_views.push_back(ui::ViewMode::Details);
}

int PlacesCatalog::FindNetwork(const std::wstring& unc) const {
    const std::wstring n = Norm(unc);
    for (int i = 0; i < static_cast<int>(networks.size()); ++i)
        if (Norm(networks[static_cast<size_t>(i)].unc) == n) return i;
    return -1;
}

int PlacesCatalog::PinNetwork(const std::wstring& unc, const std::wstring& name) {
    const std::wstring n = Norm(unc);
    int existing = FindNetwork(n);
    if (existing >= 0) return existing;
    NetworkPlace p;
    p.unc = n;
    p.name = name.empty() ? FolderTitle(n) : name;
    networks.push_back(std::move(p));
    Save();
    return static_cast<int>(networks.size()) - 1;
}

bool PlacesCatalog::UnpinNetwork(const std::wstring& unc) {
    const int index = FindNetwork(unc);
    if (index < 0) return false;
    networks.erase(networks.begin() + index);
    Save();
    return true;
}

bool PlacesCatalog::RenameNetwork(const std::wstring& unc, const std::wstring& name) {
    const int index = FindNetwork(unc);
    if (index < 0) return false;
    auto& place = networks[static_cast<size_t>(index)];
    const std::wstring next = name.empty() ? FolderTitle(place.unc) : name;
    if (next == place.name) return false;
    place.name = next;
    Save();
    return true;
}

void PlacesCatalog::SetNetworkStatus(const std::wstring& unc, fs::NetStatus status, DWORD rtt_ms) {
    const std::wstring n = Norm(unc);
    for (auto& net : networks) {
        if (Norm(net.unc) == n) {
            net.status = status;
            net.rtt_ms = rtt_ms;
            return;
        }
    }
}

void PlacesCatalog::RecordVisit(const std::wstring& path) {
    const std::wstring n = Norm(path);
    if (n.empty() || fs::IsVirtualPath(n)) return;
    for (auto& w : workspaces) {
        const std::wstring root = Norm(w.root);
        if (root.empty()) continue;
        if (n.size() <= root.size()) continue;
        if (!n.starts_with(root)) continue;
        if (n.size() > root.size() && n[root.size()] != L'\\' && root.back() != L'\\') continue;
        bool found = false;
        for (auto& hit : w.frequent) {
            if (hit.first == n) { ++hit.second; found = true; break; }
        }
        if (!found) w.frequent.push_back({ n, 1 });
        std::sort(w.frequent.begin(), w.frequent.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        if (w.frequent.size() > 24) w.frequent.resize(24);
    }
}

std::vector<std::wstring> PlacesCatalog::FrequentChildren(int workspace_index, int limit) const {
    std::vector<std::wstring> out;
    if (workspace_index < 0 || workspace_index >= static_cast<int>(workspaces.size())) return out;
    const auto& freq = workspaces[static_cast<size_t>(workspace_index)].frequent;
    for (size_t i = 0; i < freq.size() && static_cast<int>(out.size()) < limit; ++i)
        out.push_back(freq[i].first);
    return out;
}

bool PlacesCatalog::PathHasTag(const std::wstring& path, int tag_index) const {
    if (tag_index < 0 || tag_index >= static_cast<int>(tags.size())) return false;
    const std::wstring n = TagKey(path);
    const auto it = tag_index_.find(n);
    return it != tag_index_.end() &&
        std::find(it->second.begin(), it->second.end(), tag_index) != it->second.end();
}

std::vector<int> PlacesCatalog::TagsForPath(const std::wstring& path) const {
    const std::wstring n = TagKey(path);
    const auto it = tag_index_.find(n);
    return it == tag_index_.end() ? std::vector<int>{} : it->second;
}

const std::vector<int>* PlacesCatalog::TagIndicesForPath(const std::wstring& path) const {
    const std::wstring n = TagKey(path);
    const auto it = tag_index_.find(n);
    return it == tag_index_.end() ? nullptr : &it->second;
}

const ColorTag* PlacesCatalog::FindTag(const TagId& id) const {
    const int index = FindTagIndex(id);
    return index < 0 ? nullptr : &tags[static_cast<size_t>(index)];
}

int PlacesCatalog::FindTagIndex(const TagId& id) const {
    for (int i = 0; i < static_cast<int>(tags.size()); ++i)
        if (EqualI(tags[static_cast<size_t>(i)].id, id)) return i;
    return -1;
}

TagId PlacesCatalog::ResolveTagRef(const std::wstring& ref) const {
    if (FindTag(ref)) return ref;
    wchar_t* end = nullptr;
    const long legacy = wcstol(ref.c_str(), &end, 10);
    if (end && *end == L'\0' && legacy >= 0 && legacy < static_cast<long>(tags.size()))
        return tags[static_cast<size_t>(legacy)].id;
    return {};
}

std::vector<std::wstring> PlacesCatalog::PathsForTag(const TagId& id) const {
    const ColorTag* tag = FindTag(id);
    return tag ? tag->paths : std::vector<std::wstring>{};
}

TagId PlacesCatalog::CreateTag(const std::wstring& raw_name, uint32_t rgb) {
    const std::wstring name = TrimTagName(raw_name);
    if (name.empty()) return {};
    for (const auto& tag : tags) if (EqualI(tag.name, name)) return {};
    ColorTag tag;
    tag.id = NewTagId();
    tag.name = name;
    tag.rgb = rgb & 0xFFFFFFu;
    // While a favorite slot is free, a new tag gets the next Alt+N key.
    tag.favorite = FreeFavoriteSlot();
    const TagId id = tag.id;
    tags.push_back(std::move(tag));
    RebuildTagIndex();
    QueueTagSave();
    return id;
}

bool PlacesCatalog::RenameTag(const TagId& id, const std::wstring& raw_name) {
    const std::wstring name = TrimTagName(raw_name);
    const int index = FindTagIndex(id);
    if (index < 0 || name.empty()) return false;
    for (int i = 0; i < static_cast<int>(tags.size()); ++i)
        if (i != index && EqualI(tags[static_cast<size_t>(i)].name, name)) return false;
    if (tags[static_cast<size_t>(index)].name == name) return false;
    tags[static_cast<size_t>(index)].name = name;
    ++tag_revision_;
    QueueTagSave();
    return true;
}

bool PlacesCatalog::SetTagColor(const TagId& id, uint32_t rgb) {
    const int index = FindTagIndex(id);
    if (index < 0) return false;
    rgb &= 0xFFFFFFu;
    if (tags[static_cast<size_t>(index)].rgb == rgb) return false;
    tags[static_cast<size_t>(index)].rgb = rgb;
    ++tag_revision_;
    QueueTagSave();
    return true;
}

bool PlacesCatalog::SetTagsBatch(const TagId& id, const std::vector<std::wstring>& paths,
                                 bool tagged, std::vector<TagAdsUpdate>* deferred_ads) {
    const int index = FindTagIndex(id);
    return index >= 0 && SetTaggedBatch(index, paths, tagged, deferred_ads);
}

TagSelectionState PlacesCatalog::GetSelectionState(
    const TagId& id, const std::vector<std::wstring>& paths) const {
    if (paths.empty() || FindTagIndex(id) < 0) return TagSelectionState::None;
    size_t tagged = 0;
    const int index = FindTagIndex(id);
    for (const auto& path : paths) if (PathHasTag(path, index)) ++tagged;
    if (tagged == 0) return TagSelectionState::None;
    return tagged == paths.size() ? TagSelectionState::All : TagSelectionState::Mixed;
}

void PlacesCatalog::RebuildTagIndex() {
    tag_index_.clear();
    for (int i = 0; i < static_cast<int>(tags.size()); ++i) {
        if (tags[static_cast<size_t>(i)].id.empty()) tags[static_cast<size_t>(i)].id = NewTagId();
        for (const auto& path : tags[static_cast<size_t>(i)].paths) {
            const std::wstring n = TagKey(path);
            if (!n.empty() && !fs::IsVirtualPath(n)) tag_index_[n].push_back(i);
        }
    }
    ++tag_revision_;
}

void PlacesCatalog::RebuildStarIndex() {
    starred_index_.clear();
    starred_index_.reserve(starred_items.size());
    for (const auto& item : starred_items) {
        const std::wstring key = TagKey(item.path);
        if (!key.empty()) starred_index_.insert(key);
    }
}

bool PlacesCatalog::IsStarred(const std::wstring& path) const {
    if (path.empty()) return false;
    return starred_index_.contains(TagKey(path));
}

const StarredItem* PlacesCatalog::FindStarred(const std::wstring& path) const {
    const std::wstring key = TagKey(path);
    const auto it = std::find_if(starred_items.begin(), starred_items.end(),
        [&](const StarredItem& item) { return TagKey(item.path) == key; });
    return it == starred_items.end() ? nullptr : &*it;
}

StarredItem* PlacesCatalog::FindStarred(const std::wstring& path) {
    return const_cast<StarredItem*>(std::as_const(*this).FindStarred(path));
}

bool PlacesCatalog::ToggleStarred(const std::wstring& path, PlaceItemKind kind) {
    const std::wstring n = Norm(path);
    if (n.empty() || fs::IsVirtualPath(n)) return false;
    const std::wstring key = TagKey(n);
    const auto it = std::find_if(starred_items.begin(), starred_items.end(),
        [&](const StarredItem& existing) { return TagKey(existing.path) == key; });
    bool now_starred = false;
    if (it != starred_items.end()) {
        starred_items.erase(it);
    } else {
        StarredItem item;
        item.path = n;
        item.kind = kind;
        item.badge_rgb = 0x0078D4;
        if (kind == PlaceItemKind::Folder) {
            const auto first_non_folder = std::find_if(starred_items.begin(), starred_items.end(),
                [](const StarredItem& existing) {
                    return existing.kind != PlaceItemKind::Folder;
                });
            starred_items.insert(first_non_folder, std::move(item));
        } else {
            starred_items.push_back(std::move(item));
        }
        now_starred = true;
    }
    RebuildStarIndex();
    ++tag_revision_;
    Save();
    return now_starred;
}

bool PlacesCatalog::SetStarredBadge(const std::wstring& path, const std::wstring& text,
                                    uint32_t rgb) {
    StarredItem* item = FindStarred(path);
    if (!item) return false;
    const std::wstring badge = TrimBadge(text);
    rgb &= 0xFFFFFFu;
    if (item->badge == badge && item->badge_rgb == rgb) return false;
    item->badge = badge;
    item->badge_rgb = rgb;
    ++tag_revision_;
    Save();
    return true;
}

const QuickAccessBadge* PlacesCatalog::FindQuickAccessBadge(const std::wstring& path) const {
    if (path.empty()) return nullptr;
    const std::wstring key = Norm(path);
    const auto it = std::find_if(quick_access_badges.begin(), quick_access_badges.end(),
        [&](const QuickAccessBadge& item) { return EqualI(item.path, key); });
    return it == quick_access_badges.end() ? nullptr : &*it;
}

bool PlacesCatalog::SetQuickAccessBadge(const std::wstring& path, const std::wstring& text,
                                        uint32_t rgb) {
    const std::wstring key = Norm(path);
    if (key.empty()) return false;
    const std::wstring badge = TrimBadge(text);
    rgb &= 0xFFFFFFu;
    const bool clear = badge.empty() && rgb == kDefaultBadgeRgb;
    const auto it = std::find_if(quick_access_badges.begin(), quick_access_badges.end(),
        [&](const QuickAccessBadge& item) { return EqualI(item.path, key); });
    if (it == quick_access_badges.end()) {
        if (clear) return false;
        quick_access_badges.push_back({ key, badge, rgb });
    } else if (clear) {
        quick_access_badges.erase(it);
    } else {
        if (it->badge == badge && it->badge_rgb == rgb) return false;
        it->badge = badge;
        it->badge_rgb = rgb;
    }
    Save();
    return true;
}

bool PlacesCatalog::SetStarredKind(const std::wstring& path, PlaceItemKind kind) {
    StarredItem* item = FindStarred(path);
    if (!item || kind == PlaceItemKind::Unknown || item->kind == kind) return false;
    item->kind = kind;
    std::stable_partition(starred_items.begin(), starred_items.end(),
        [](const StarredItem& candidate) { return candidate.kind == PlaceItemKind::Folder; });
    ++tag_revision_;
    MarkPlacesDirty();
    return true;
}

bool PlacesCatalog::ReorderStarredFolder(const std::wstring& path, size_t folder_position) {
    const std::wstring key = TagKey(path);
    std::vector<size_t> folders;
    for (size_t i = 0; i < starred_items.size(); ++i)
        if (starred_items[i].kind == PlaceItemKind::Folder) folders.push_back(i);
    const auto found = std::find_if(folders.begin(), folders.end(), [&](size_t index) {
        return TagKey(starred_items[index].path) == key;
    });
    if (found == folders.end() || folders.empty()) return false;
    folder_position = std::min(folder_position, folders.size() - 1);
    const size_t old_position = static_cast<size_t>(found - folders.begin());
    if (old_position == folder_position) return false;
    StarredItem moved = std::move(starred_items[folders[old_position]]);
    starred_items.erase(starred_items.begin() + folders[old_position]);
    const size_t insert_index = folder_position >= starred_items.size()
        ? starred_items.size() : folder_position;
    starred_items.insert(starred_items.begin() + insert_index, std::move(moved));
    ++tag_revision_;
    Save();
    return true;
}

bool PlacesCatalog::MoveStarredItem(size_t from, size_t to) {
    if (from >= starred_items.size() || to >= starred_items.size() || from == to) return false;
    const bool folder = starred_items[from].kind != PlaceItemKind::File;
    if ((starred_items[to].kind != PlaceItemKind::File) != folder) return false;
    StarredItem moved = std::move(starred_items[from]);
    starred_items.erase(starred_items.begin() + static_cast<std::ptrdiff_t>(from));
    starred_items.insert(starred_items.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    ++tag_revision_;
    Save();
    return true;
}

std::vector<std::wstring> PlacesCatalog::StarredPaths() const {
    std::vector<std::wstring> out;
    out.reserve(starred_items.size());
    for (const auto& item : starred_items)
        if (item.kind == PlaceItemKind::Folder) out.push_back(item.path);
    for (const auto& item : starred_items)
        if (item.kind != PlaceItemKind::Folder) out.push_back(item.path);
    return out;
}

const StarredGroup* PlacesCatalog::FindStarredGroup(const std::wstring& id) const {
    if (id.empty()) return nullptr;
    const auto it = std::find_if(starred_groups.begin(), starred_groups.end(),
                                 [&](const StarredGroup& group) { return group.id == id; });
    return it == starred_groups.end() ? nullptr : &*it;
}

std::wstring PlacesCatalog::CreateStarredGroup(const std::wstring& raw_name,
                                               const std::wstring& first_path) {
    const std::wstring name = TrimTagName(raw_name);
    const StarredItem* first = FindStarred(first_path);
    if (name.empty() || !first || first->kind == PlaceItemKind::File) return {};
    StarredGroup group;
    group.id = NewTagId();
    group.name = name;
    starred_groups.push_back(group);
    SetStarredItemGroup(first_path, group.id, false);
    return group.id;
}

bool PlacesCatalog::SetStarredItemGroup(const std::wstring& path, const std::wstring& group_id,
                                        bool gather) {
    StarredItem* item = FindStarred(path);
    if (!item || item->kind == PlaceItemKind::File) return false;
    if (!group_id.empty() && !FindStarredGroup(group_id)) return false;
    if (item->group == group_id) return false;
    item->group = group_id;
    if (gather && !group_id.empty()) {
        // After the group's last member, so the group stays one run.
        const auto self = std::find_if(starred_items.begin(), starred_items.end(),
                                       [&](const StarredItem& candidate) { return &candidate == item; });
        StarredItem moved = std::move(*self);
        starred_items.erase(self);
        size_t at = starred_items.size();
        bool found = false;
        for (size_t i = starred_items.size(); i-- > 0;)
            if (starred_items[i].group == group_id) { at = i + 1; found = true; break; }
        if (!found) {
            at = static_cast<size_t>(std::find_if(starred_items.begin(), starred_items.end(),
                [](const StarredItem& candidate) { return candidate.kind == PlaceItemKind::File; }) -
                starred_items.begin());
        }
        starred_items.insert(starred_items.begin() + static_cast<std::ptrdiff_t>(at), std::move(moved));
    }
    ++tag_revision_;
    Save();
    return true;
}

bool PlacesCatalog::RenameStarredGroup(const std::wstring& id, const std::wstring& raw_name) {
    const std::wstring name = TrimTagName(raw_name);
    if (name.empty()) return false;
    for (auto& group : starred_groups) {
        if (group.id != id || group.name == name) continue;
        group.name = name;
        ++tag_revision_;
        Save();
        return true;
    }
    return false;
}

bool PlacesCatalog::SetStarredGroupCollapsed(const std::wstring& id, bool collapsed) {
    for (auto& group : starred_groups) {
        if (group.id != id || group.collapsed == collapsed) continue;
        group.collapsed = collapsed;
        Save();
        return true;
    }
    return false;
}

bool PlacesCatalog::UngroupStarred(const std::wstring& id) {
    const auto found = std::find_if(starred_groups.begin(), starred_groups.end(),
                                    [&](const StarredGroup& group) { return group.id == id; });
    if (found == starred_groups.end()) return false;
    // The group's separator stays with its last member.
    const bool separator = found->separator_after;
    starred_groups.erase(found);
    StarredItem* last = nullptr;
    for (auto& item : starred_items)
        if (item.group == id) { item.group.clear(); last = &item; }
    if (separator && last) last->separator_after = true;
    ++tag_revision_;
    Save();
    return true;
}

bool PlacesCatalog::SetStarredSeparator(const std::wstring& path_or_group, bool on) {
    for (auto& group : starred_groups) {
        if (group.id != path_or_group) continue;
        if (group.separator_after == on) return false;
        group.separator_after = on;
        ++tag_revision_;
        Save();
        return true;
    }
    StarredItem* item = FindStarred(path_or_group);
    if (!item || item->separator_after == on) return false;
    item->separator_after = on;
    ++tag_revision_;
    Save();
    return true;
}

bool PlacesCatalog::ReorderStarred(const std::vector<size_t>& order) {
    if (order.size() != starred_items.size()) return false;
    std::vector<bool> seen(order.size(), false);
    for (size_t index : order) {
        if (index >= order.size() || seen[index]) return false;
        seen[index] = true;
    }
    bool same = true;
    for (size_t i = 0; i < order.size() && same; ++i) same = order[i] == i;
    if (same) return false;
    std::vector<StarredItem> reordered;
    reordered.reserve(order.size());
    for (size_t index : order) reordered.push_back(std::move(starred_items[index]));
    starred_items = std::move(reordered);
    // Folders stay ahead of files.
    std::stable_partition(starred_items.begin(), starred_items.end(),
        [](const StarredItem& item) { return item.kind != PlaceItemKind::File; });
    ++tag_revision_;
    Save();
    return true;
}

std::vector<StarredRow> MakeStarredRows(const std::vector<StarredItem>& items,
                                        const std::vector<StarredGroup>& groups,
                                        bool folders_only) {
    std::vector<StarredRow> rows;
    rows.reserve(items.size());
    const auto group_of = [&](const StarredItem& item) -> const StarredGroup* {
        if (item.group.empty() || item.kind == PlaceItemKind::File) return nullptr;
        const auto it = std::find_if(groups.begin(), groups.end(),
                                     [&](const StarredGroup& group) { return group.id == item.group; });
        return it == groups.end() ? nullptr : &*it;
    };
    std::vector<std::wstring> placed;
    for (size_t i = 0; i < items.size(); ++i) {
        const StarredItem& item = items[i];
        if (folders_only && item.kind == PlaceItemKind::File) continue;
        bool separator = item.separator_after;
        if (const StarredGroup* group = group_of(item)) {
            if (std::find(placed.begin(), placed.end(), group->id) != placed.end()) continue;
            placed.push_back(group->id);
            StarredRow row;
            row.kind = StarredRow::Kind::Group;
            row.index = i;
            row.group = group->id;
            for (size_t j = i; j < items.size(); ++j)
                if (group_of(items[j]) == group) row.members.push_back(j);
            rows.push_back(std::move(row));
            separator = group->separator_after;
        } else {
            StarredRow row;
            row.index = i;
            rows.push_back(std::move(row));
        }
        if (separator) {
            StarredRow line;
            line.kind = StarredRow::Kind::Separator;
            line.index = i;
            rows.push_back(std::move(line));
        }
    }
    // A trailing line separates nothing.
    if (!rows.empty() && rows.back().kind == StarredRow::Kind::Separator) rows.pop_back();
    return rows;
}

std::vector<std::wstring> PlacesCatalog::StarredFolderPaths() const {
    std::vector<std::wstring> out;
    for (const auto& item : starred_items)
        if (item.kind == PlaceItemKind::Folder) out.push_back(item.path);
    return out;
}

void PlacesCatalog::RecordRecent(const std::wstring& path, PlaceItemKind kind) {
    const std::wstring normalized = Norm(path);
    if (normalized.empty() || fs::IsVirtualPath(normalized)) return;
    const std::wstring key = TagKey(normalized);
    RecentItem item{ normalized, kind, NowFileTime() };
    const auto found = std::find_if(recent_items.begin(), recent_items.end(),
        [&](const RecentItem& old) { return TagKey(old.path) == key; });
    if (found != recent_items.end()) {
        if (kind == PlaceItemKind::Unknown) item.kind = found->kind;
        recent_items.erase(found);
    }
    recent_items.insert(recent_items.begin(), std::move(item));
    if (recent_items.size() > 100) recent_items.resize(100);
    MarkPlacesDirty();
}

const RecentItem* PlacesCatalog::FindRecent(const std::wstring& path) const {
    const std::wstring key = TagKey(path);
    const auto found = std::find_if(recent_items.begin(), recent_items.end(),
        [&](const RecentItem& item) { return TagKey(item.path) == key; });
    return found == recent_items.end() ? nullptr : &*found;
}

bool PlacesCatalog::SetRecentKind(const std::wstring& path, PlaceItemKind kind) {
    if (kind == PlaceItemKind::Unknown) return false;
    const std::wstring key = TagKey(path);
    const auto found = std::find_if(recent_items.begin(), recent_items.end(),
        [&](const RecentItem& item) { return TagKey(item.path) == key; });
    if (found == recent_items.end() || found->kind == kind) return false;
    found->kind = kind;
    MarkPlacesDirty();
    return true;
}

bool PlacesCatalog::RemoveRecent(const std::wstring& path) {
    const std::wstring key = TagKey(path);
    const size_t before = recent_items.size();
    std::erase_if(recent_items, [&](const RecentItem& item) {
        return TagKey(item.path) == key;
    });
    if (before == recent_items.size()) return false;
    Save();
    return true;
}

bool PlacesCatalog::ClearRecent() {
    if (recent_items.empty()) return false;
    recent_items.clear();
    Save();
    return true;
}

std::vector<RecentItem> PlacesCatalog::RecentItems(RecentFilter filter) const {
    std::vector<RecentItem> out;
    for (const auto& item : recent_items) {
        if (filter == RecentFilter::Folders && item.kind != PlaceItemKind::Folder) continue;
        if (filter == RecentFilter::Files && item.kind != PlaceItemKind::File) continue;
        out.push_back(item);
    }
    return out;
}

std::vector<std::wstring> PlacesCatalog::RecentFolderPaths(size_t limit) const {
    std::vector<std::wstring> out;
    for (const auto& item : recent_items) {
        if (item.kind != PlaceItemKind::Folder) continue;
        out.push_back(item.path);
        if (out.size() >= limit) break;
    }
    return out;
}

void PlacesCatalog::TagsReordered() {
    RebuildTagIndex();
    QueueTagSave();
}

int PlacesCatalog::FavoriteTagIndex(int slot) const {
    if (slot < 1 || slot > kMaxFavoriteTags) return -1;
    for (size_t i = 0; i < tags.size(); ++i)
        if (tags[i].favorite == slot) return static_cast<int>(i);
    return -1;
}

int PlacesCatalog::FreeFavoriteSlot() const {
    for (int slot = 1; slot <= kMaxFavoriteTags; ++slot)
        if (FavoriteTagIndex(slot) < 0) return slot;
    return 0;
}

// Out-of-range or duplicate slots (hand edits, merged files) are dropped;
// the first tag keeps a contested slot.
void PlacesCatalog::NormalizeFavoriteSlots() {
    bool used[kMaxFavoriteTags + 1]{};
    for (auto& tag : tags) {
        if (tag.favorite < 1 || tag.favorite > kMaxFavoriteTags || used[tag.favorite]) {
            tag.favorite = 0;
            continue;
        }
        used[tag.favorite] = true;
    }
}

void PlacesCatalog::ReorderFavoriteTags(const std::vector<TagId>& order) {
    std::vector<int> slots;
    std::vector<size_t> members;
    for (const auto& id : order) {
        const int index = FindTagIndex(id);
        if (index < 0 || tags[static_cast<size_t>(index)].favorite <= 0) continue;
        slots.push_back(tags[static_cast<size_t>(index)].favorite);
        members.push_back(static_cast<size_t>(index));
    }
    std::sort(slots.begin(), slots.end());
    bool changed = false;
    for (size_t i = 0; i < members.size(); ++i) {
        changed = changed || tags[members[i]].favorite != slots[i];
        tags[members[i]].favorite = slots[i];
    }
    if (!changed) return;
    ++tag_revision_;
    QueueTagSave();
}

const TagGroup* PlacesCatalog::FindTagGroup(const std::wstring& id) const {
    if (id.empty()) return nullptr;
    for (const auto& group : tag_groups)
        if (group.id == id) return &group;
    return nullptr;
}

std::wstring PlacesCatalog::TagGroupOf(const ColorTag& tag) const {
    return FindTagGroup(tag.group) ? tag.group : std::wstring{};
}

std::wstring PlacesCatalog::CreateTagGroup(const std::wstring& raw_name) {
    const std::wstring name = TrimTagName(raw_name);
    if (name.empty()) return {};
    for (const auto& group : tag_groups)
        if (EqualI(group.name, name)) return group.id;
    TagGroup group;
    group.id = NewTagId();
    group.name = name;
    tag_groups.push_back(group);
    ++tag_revision_;
    Save();
    return group.id;
}

bool PlacesCatalog::RenameTagGroup(const std::wstring& id, const std::wstring& raw_name) {
    const std::wstring name = TrimTagName(raw_name);
    if (name.empty()) return false;
    for (auto& group : tag_groups) {
        if (group.id != id) continue;
        if (group.name == name) return false;
        group.name = name;
        ++tag_revision_;
        Save();
        return true;
    }
    return false;
}

bool PlacesCatalog::DeleteTagGroup(const std::wstring& id) {
    const auto found = std::find_if(tag_groups.begin(), tag_groups.end(),
        [&](const TagGroup& group) { return group.id == id; });
    if (found == tag_groups.end()) return false;
    tag_groups.erase(found);
    bool members = false;
    for (auto& tag : tags) {
        if (tag.group != id) continue;
        tag.group.clear();
        members = true;
    }
    ++tag_revision_;
    Save();
    if (members) QueueTagSave();
    return true;
}

bool PlacesCatalog::SetTagGroup(const TagId& id, const std::wstring& group_id) {
    const int index = FindTagIndex(id);
    if (index < 0 || (!group_id.empty() && !FindTagGroup(group_id))) return false;
    ColorTag& tag = tags[static_cast<size_t>(index)];
    if (TagGroupOf(tag) == group_id) return false;
    tag.group = group_id;
    ++tag_revision_;
    QueueTagSave();
    return true;
}

bool PlacesCatalog::SetTagGroupCollapsed(const std::wstring& id, bool collapsed) {
    for (auto& group : tag_groups) {
        if (group.id != id || group.collapsed == collapsed) continue;
        group.collapsed = collapsed;
        MarkPlacesDirty();
        return true;
    }
    return false;
}

void PlacesCatalog::SetTagSidebarMode(TagSidebarMode mode) {
    if (tag_sidebar_mode == mode) return;
    tag_sidebar_mode = mode;
    ++tag_revision_;
    MarkPlacesDirty();
}

bool PlacesCatalog::SetTagFavorite(const TagId& id, bool favorite) {
    const int index = FindTagIndex(id);
    if (index < 0) return false;
    ColorTag& tag = tags[static_cast<size_t>(index)];
    if ((tag.favorite > 0) == favorite) return false;
    const int slot = favorite ? FreeFavoriteSlot() : 0;
    if (favorite && slot == 0) return false;
    tag.favorite = slot;
    ++tag_revision_;
    QueueTagSave();
    return true;
}

void PlacesCatalog::CommitTagChanges(const std::vector<std::wstring>& paths,
                                     std::vector<TagAdsUpdate>* deferred_ads) {
    RebuildTagIndex();
    for (const auto& path : paths) {
        TagAdsUpdate update;
        update.path = path;
        const std::vector<int> indices = TagsForPath(path);
        for (int index : indices) {
            const auto& tag = tags[static_cast<size_t>(index)];
            update.tag_names.push_back(tag.name);
            update.tags.push_back({ tag.id, tag.name, tag.rgb });
        }
        if (deferred_ads) deferred_ads->push_back(std::move(update));
        else WriteTagAdsV2(update.path, update.tags);
    }
    QueueTagSave();
}

bool PlacesCatalog::SetTagged(int tag_index, const std::wstring& path, bool tagged) {
    return SetTaggedBatch(tag_index, { path }, tagged);
}

bool PlacesCatalog::SetTaggedBatch(int tag_index, const std::vector<std::wstring>& paths,
                                   bool tagged, std::vector<TagAdsUpdate>* deferred_ads) {
    if (tag_index < 0 || tag_index >= static_cast<int>(tags.size())) return false;
    auto& t = tags[static_cast<size_t>(tag_index)];
    std::unordered_set<std::wstring> membership;
    for (const auto& path : t.paths) membership.insert(TagKey(path));
    std::unordered_set<std::wstring> seen;
    std::vector<std::wstring> changed;
    changed.reserve(paths.size());
    for (const auto& path : paths) {
        const std::wstring n = Norm(path);
        const std::wstring key = TagKey(n);
        if (n.empty() || fs::IsVirtualPath(n) || !seen.insert(key).second) continue;
        if (tagged) {
            if (!membership.insert(key).second) continue;
            t.paths.push_back(n);
        } else {
            if (membership.erase(key) == 0) continue;
        }
        changed.push_back(n);
    }
    if (changed.empty()) return false;
    if (!tagged) {
        std::erase_if(t.paths, [&](const std::wstring& path) {
            return !membership.contains(TagKey(path));
        });
    }
    CommitTagChanges(changed, deferred_ads);
    return true;
}

bool PlacesCatalog::ToggleTag(int tag_index, const std::wstring& path) {
    return SetTagged(tag_index, path, !PathHasTag(path, tag_index));
}

bool PlacesCatalog::ToggleTagBatch(int tag_index, const std::vector<std::wstring>& paths,
                                   std::vector<TagAdsUpdate>* deferred_ads) {
    if (tag_index < 0 || tag_index >= static_cast<int>(tags.size())) return false;
    auto& tagged_paths = tags[static_cast<size_t>(tag_index)].paths;
    std::unordered_set<std::wstring> membership;
    for (const auto& path : tagged_paths) membership.insert(TagKey(path));
    std::unordered_set<std::wstring> seen;
    std::vector<std::wstring> changed;
    changed.reserve(paths.size());
    for (const auto& path : paths) {
        const std::wstring n = Norm(path);
        const std::wstring key = TagKey(n);
        if (n.empty() || fs::IsVirtualPath(n) || !seen.insert(key).second) continue;
        if (membership.erase(key) == 0) {
            membership.insert(key);
            tagged_paths.push_back(n);
        }
        changed.push_back(n);
    }
    if (changed.empty()) return false;
    std::erase_if(tagged_paths, [&](const std::wstring& path) {
        return !membership.contains(TagKey(path));
    });
    CommitTagChanges(changed, deferred_ads);
    return true;
}

size_t PlacesCatalog::DeleteTag(const TagId& id, std::vector<TagAdsUpdate>* deferred_ads) {
    const int index = FindTagIndex(id);
    if (index < 0) return 0;
    const std::vector<std::wstring> affected = tags[static_cast<size_t>(index)].paths;
    tags.erase(tags.begin() + index);
    CommitTagChanges(affected, deferred_ads);
    return affected.size();
}

bool PlacesCatalog::IsQuickAccessPinned(const std::wstring& path) const {
    const auto key = TagKey(path);
    return std::any_of(quick_access_paths.begin(), quick_access_paths.end(),
        [&](const auto& value) { return TagKey(value) == key; });
}

bool PlacesCatalog::ReorderQuickAccessPinned(const std::wstring& path, size_t position) {
    const auto key = TagKey(path);
    const auto found = std::find_if(quick_access_paths.begin(), quick_access_paths.end(),
        [&](const std::wstring& candidate) { return TagKey(candidate) == key; });
    if (found == quick_access_paths.end() || quick_access_paths.empty()) return false;
    position = std::min(position, quick_access_paths.size());
    const size_t current = static_cast<size_t>(found - quick_access_paths.begin());
    // The indicator names a gap in the original list, which still includes the
    // dragged row. Removing that row shifts every later gap back by one.
    if (position > current) --position;
    if (current == position) return false;
    std::wstring moved = std::move(*found);
    quick_access_paths.erase(found);
    quick_access_paths.insert(
        quick_access_paths.begin() +
            static_cast<std::ptrdiff_t>(std::min(position, quick_access_paths.size())),
        std::move(moved));
    Save();
    return true;
}

bool IsBuiltinQuickAccessKey(std::wstring_view key) noexcept {
    return key.starts_with(L"builtin:");
}

std::vector<std::wstring> MergeQuickAccessOrder(const std::vector<std::wstring>& order,
                                                const std::vector<std::wstring>& defaults) {
    if (order.empty()) return defaults;
    std::unordered_map<std::wstring, size_t> default_index;
    default_index.reserve(defaults.size());
    for (size_t i = 0; i < defaults.size(); ++i)
        default_index.emplace(QuickAccessKeyId(defaults[i]), i);
    std::vector<bool> placed(defaults.size(), false);
    std::vector<size_t> merged;  // indices into defaults
    merged.reserve(defaults.size());
    for (const auto& key : order) {
        const auto found = default_index.find(QuickAccessKeyId(key));
        if (found == default_index.end() || placed[found->second]) continue;
        placed[found->second] = true;
        merged.push_back(found->second);
    }
    for (size_t i = 0; i < defaults.size(); ++i) {
        if (placed[i]) continue;
        placed[i] = true;
        if (!IsBuiltinQuickAccessKey(defaults[i])) {
            merged.push_back(i);
            continue;
        }
        // Right after the nearest earlier default row, or first.
        size_t at = 0;
        for (size_t j = i; j-- > 0;) {
            const auto pos = std::find(merged.begin(), merged.end(), j);
            if (pos != merged.end()) {
                at = static_cast<size_t>(pos - merged.begin()) + 1;
                break;
            }
        }
        merged.insert(merged.begin() + static_cast<std::ptrdiff_t>(at), i);
    }
    std::vector<std::wstring> keys;
    keys.reserve(merged.size());
    for (size_t index : merged) keys.push_back(defaults[index]);
    return keys;
}

std::vector<std::wstring> MoveQuickAccessKey(std::vector<std::wstring> keys,
                                             const std::vector<std::wstring>& visible,
                                             const std::wstring& key, size_t gap) {
    const std::wstring id = QuickAccessKeyId(key);
    const auto same = [&](const std::wstring& candidate) { return QuickAccessKeyId(candidate) == id; };
    const auto found = std::find_if(keys.begin(), keys.end(), same);
    if (found == keys.end()) return keys;
    std::wstring moved = std::move(*found);
    keys.erase(found);
    const auto index_of = [&](const std::wstring& value) {
        const std::wstring value_id = QuickAccessKeyId(value);
        const auto at = std::find_if(keys.begin(), keys.end(),
            [&](const std::wstring& candidate) { return QuickAccessKeyId(candidate) == value_id; });
        return static_cast<size_t>(at - keys.begin());
    };
    gap = (std::min)(gap, visible.size());
    // Before the first visible row at or after the gap, skipping the dragged
    // row (its own two gaps leave the order as it was); the end gap goes after
    // the last visible row.
    size_t at = keys.size();
    bool placed = false;
    for (size_t i = gap; i < visible.size() && !placed; ++i) {
        if (same(visible[i])) continue;
        at = index_of(visible[i]);
        placed = true;
    }
    for (size_t i = visible.size(); !placed && i-- > 0;) {
        if (same(visible[i])) continue;
        at = (std::min)(index_of(visible[i]) + 1, keys.size());
        placed = true;
    }
    keys.insert(keys.begin() + static_cast<std::ptrdiff_t>((std::min)(at, keys.size())),
                std::move(moved));
    return keys;
}

bool PlacesCatalog::SetQuickAccessLayout(const std::vector<std::wstring>& keys,
                                         const std::vector<std::wstring>& builtin_defaults) {
    std::vector<std::wstring> pins;
    std::vector<std::wstring> builtins;
    std::vector<std::wstring> layout;
    std::unordered_set<std::wstring> seen;
    for (const auto& key : keys) {
        const bool builtin = IsBuiltinQuickAccessKey(key);
        std::wstring value = builtin ? key : Norm(key);
        if (value.empty() || (!builtin && fs::IsVirtualPath(value))) continue;
        if (!seen.insert(QuickAccessKeyId(value)).second) continue;
        (builtin ? builtins : pins).push_back(value);
        layout.push_back(std::move(value));
    }
    std::vector<std::wstring> fallback;
    fallback.reserve(layout.size());
    for (const auto& key : builtin_defaults) {
        if (std::find(builtins.begin(), builtins.end(), key) != builtins.end())
            fallback.push_back(key);
    }
    fallback.insert(fallback.end(), pins.begin(), pins.end());
    if (fallback == layout) layout.clear();
    if (pins == quick_access_paths && layout == quick_access_order) return false;
    quick_access_paths = std::move(pins);
    quick_access_order = std::move(layout);
    Save();
    return true;
}

bool PlacesCatalog::SetQuickAccessPinned(const std::vector<std::wstring>& paths, bool pinned) {
    bool changed = false;
    for (const auto& path : paths) {
        if (path.empty() || fs::IsVirtualPath(path)) continue;
        const auto normalized = Norm(path);
        if (normalized.empty()) continue;
        if (pinned) {
            if (!IsQuickAccessPinned(normalized)) {
                quick_access_paths.push_back(normalized);
                changed = true;
            }
        } else {
            const auto key = TagKey(normalized);
            changed |= std::erase_if(quick_access_paths,
                [&](const auto& value) { return TagKey(value) == key; }) != 0;
            std::erase_if(quick_access_order, [&](const std::wstring& value) {
                return !IsBuiltinQuickAccessKey(value) && TagKey(value) == key;
            });
        }
    }
    if (changed) Save();
    return changed;
}

namespace {
// Roots found from a path's own ancestors: PathIsOrDescendant against every
// root at once instead of one comparison per root.
class PathRoots {
public:
    // False when an equal root is already there.
    bool Add(const std::wstring& root, size_t index) {
        return roots_.emplace(pulse::path::FoldedKey(root), index).second;
    }
    bool Empty() const { return roots_.empty(); }
    bool Contains(const std::wstring& path) const {
        return !roots_.empty() && roots_.contains(pulse::path::FoldedKey(path));
    }
    // A root the path is or lies under, npos when none. Without `self`, only
    // roots strictly above the path count.
    size_t Find(const std::wstring& path, bool self = true) const {
        if (roots_.empty() || path.empty()) return std::wstring::npos;
        const std::wstring key = pulse::path::FoldedKey(path);
        if (self)
            if (const auto found = roots_.find(key); found != roots_.end()) return found->second;
        for (size_t i = key.size(); i-- > 0;) {
            if (key[i] != L'\\' && key[i] != L'/') continue;
            // A root ending in a separator ("C:\") covers whatever follows it;
            // any other root needs a separator right after it.
            if (key[i] == L'\\' && i + 1 < key.size())
                if (const auto found = roots_.find(key.substr(0, i + 1)); found != roots_.end())
                    return found->second;
            if (i)
                if (const auto found = roots_.find(key.substr(0, i)); found != roots_.end())
                    return found->second;
        }
        return std::wstring::npos;
    }

private:
    std::unordered_map<std::wstring, size_t> roots_;
};
}  // namespace

void PlacesCatalog::RemapPaths(const std::wstring& old_path, const std::wstring& new_path) {
    RemapPaths(std::vector<std::pair<std::wstring, std::wstring>>{{old_path, new_path}});
}

void PlacesCatalog::RemapPaths(const std::vector<std::pair<std::wstring, std::wstring>>& moves) {
    std::vector<std::pair<std::wstring, std::wstring>> valid;
    valid.reserve(moves.size());
    for (const auto& [old_path, new_path] : moves) {
        std::wstring old_norm = Norm(old_path);
        std::wstring new_norm = Norm(new_path);
        if (old_norm.empty() || new_norm.empty() || EqualI(old_norm, new_norm)) continue;
        valid.emplace_back(std::move(old_norm), std::move(new_norm));
    }
    if (valid.empty()) return;
    // Operation completion runs on the UI thread. Publish the remapped paths
    // immediately, but never wait there for the catalog writer or storage.
    ScopedAsyncSave queued_save(*this);
    // Every path follows the move whose source it is or lies under.
    const auto apply = [&](const PathRoots& roots) {
        bool changed = false;
        const auto remap = [&](std::wstring& path) {
            const size_t index = roots.Find(path);
            if (index == std::wstring::npos) return;
            path = valid[index].second + path.substr(valid[index].first.size());
            changed = true;
        };
        for (auto& tag : tags) {
            for (auto& path : tag.paths) remap(path);
            std::sort(tag.paths.begin(), tag.paths.end());
            tag.paths.erase(std::unique(tag.paths.begin(), tag.paths.end()), tag.paths.end());
        }
        std::unordered_set<std::wstring> quick_seen;
        for (auto& path : quick_access_paths) remap(path);
        std::erase_if(quick_access_paths, [&](const auto& path) {
            return !quick_seen.insert(TagKey(path)).second;
        });
        for (auto& key : quick_access_order)
            if (!IsBuiltinQuickAccessKey(key)) remap(key);
        std::unordered_set<std::wstring> order_seen;
        std::erase_if(quick_access_order, [&](const std::wstring& key) {
            return !order_seen.insert(QuickAccessKeyId(key)).second;
        });
        for (auto& item : starred_items) remap(item.path);
        std::unordered_set<std::wstring> badge_seen;
        for (auto& item : quick_access_badges) remap(item.path);
        std::erase_if(quick_access_badges, [&](const QuickAccessBadge& item) {
            return !badge_seen.insert(TagKey(item.path)).second;
        });
        for (auto& item : recent_items) remap(item.path);
        return changed;
    };
    // One pass serves every move when no path can meet two of them: no source
    // inside another, and no source and target inside each other (a move of
    // many items into one folder). Otherwise each move applies in turn, as
    // separate calls did. A move of thousands of items sorted every tag and
    // saved the catalog once per item.
    PathRoots sources;
    PathRoots targets;
    bool independent = true;
    for (size_t i = 0; i < valid.size() && independent; ++i)
        independent = sources.Add(valid[i].first, i) && targets.Add(valid[i].second, i);
    for (size_t i = 0; i < valid.size() && independent; ++i)
        independent = sources.Find(valid[i].first, false) == std::wstring::npos &&
                      targets.Find(valid[i].first) == std::wstring::npos &&
                      sources.Find(valid[i].second) == std::wstring::npos;
    bool changed = false;
    if (independent) {
        changed = apply(sources);
    } else {
        for (size_t i = 0; i < valid.size(); ++i) {
            PathRoots one;
            one.Add(valid[i].first, i);
            changed = apply(one) || changed;
        }
    }
    if (changed) {
        RebuildTagIndex();
        RebuildStarIndex();
        QueueTagSave();
        Save();
    }
}

void PlacesCatalog::CloneAssignments(const std::wstring& source,
                                     const std::wstring& destination) {
    const std::wstring src = Norm(source);
    const std::wstring dst = Norm(destination);
    if (src.empty() || dst.empty()) return;
    bool changed = false;
    for (auto& tag : tags) {
        std::vector<std::wstring> clones;
        for (const auto& path : tag.paths) {
            if (PathIsOrDescendant(path, src)) clones.push_back(dst + path.substr(src.size()));
        }
        for (auto& clone : clones) {
            if (std::find(tag.paths.begin(), tag.paths.end(), clone) == tag.paths.end()) {
                tag.paths.push_back(std::move(clone));
                changed = true;
            }
        }
    }
    std::vector<StarredItem> star_clones;
    for (const auto& item : starred_items) {
        if (!PathIsOrDescendant(item.path, src)) continue;
        StarredItem clone = item;
        clone.path = dst + item.path.substr(src.size());
        clone.separator_after = false;
        star_clones.push_back(std::move(clone));
    }
    for (auto& clone : star_clones) {
        if (!FindStarred(clone.path)) {
            starred_items.push_back(std::move(clone));
            changed = true;
        }
    }
    if (changed) {
        RebuildTagIndex();
        RebuildStarIndex();
        QueueTagSave();
        Save();
    }
}

void PlacesCatalog::RemoveAssignments(const std::wstring& path, bool include_descendants) {
    RemoveAssignments(std::vector<std::wstring>{path}, include_descendants);
}

void PlacesCatalog::RemoveAssignments(const std::vector<std::wstring>& paths,
                                      bool include_descendants) {
    PathRoots removed;
    for (size_t i = 0; i < paths.size(); ++i) {
        const std::wstring normalized = Norm(paths[i]);
        if (!normalized.empty()) removed.Add(normalized, i);
    }
    if (removed.Empty()) return;
    const auto gone = [&](const std::wstring& candidate) {
        return include_descendants ? removed.Find(candidate) != std::wstring::npos
                                   : removed.Contains(candidate);
    };
    bool changed = false;
    for (auto& tag : tags) {
        const size_t before = tag.paths.size();
        std::erase_if(tag.paths, gone);
        changed = changed || before != tag.paths.size();
    }
    const size_t starred_before = starred_items.size();
    std::erase_if(starred_items, [&](const StarredItem& item) { return gone(item.path); });
    changed = changed || starred_before != starred_items.size();
    const size_t recent_before = recent_items.size();
    std::erase_if(recent_items, [&](const RecentItem& item) { return gone(item.path); });
    changed = changed || recent_before != recent_items.size();
    if (changed) {
        RebuildTagIndex();
        RebuildStarIndex();
        QueueTagSave();
        Save();
    }
}

void PlacesCatalog::MergeAdsRecords(const std::wstring& path,
                                    const std::vector<TagAdsRecord>& records,
                                    const std::vector<std::wstring>& legacy_names) {
    if (HasPendingTagAds(path)) return;
    const std::wstring n = Norm(path);
    if (n.empty() || fs::IsVirtualPath(n)) return;
    if (!records.empty()) {
        bool changed = false;
        for (const auto& record : records) {
            int index = FindTagIndex(record.id);
            if (index < 0) {
                const std::wstring imported_name = TrimTagName(record.name);
                for (int i = 0; i < static_cast<int>(tags.size()); ++i) {
                    if (!imported_name.empty() &&
                        EqualI(tags[static_cast<size_t>(i)].name, imported_name)) {
                        index = i;
                        break;
                    }
                }
                if (index < 0) {
                    ColorTag tag;
                    tag.id = record.id.empty() ? NewTagId() : record.id;
                    tag.name = imported_name.empty() ? std::wstring(l10n::Get(l10n::StringId::ImportedTag).c_str()) : imported_name;
                    tag.rgb = record.rgb;
                    tags.push_back(std::move(tag));
                    index = static_cast<int>(tags.size()) - 1;
                    changed = true;
                }
            }
            auto& paths = tags[static_cast<size_t>(index)].paths;
            if (std::none_of(paths.begin(), paths.end(),
                             [&](const std::wstring& existing) { return EqualI(existing, n); })) {
                paths.push_back(n);
                changed = true;
            }
        }
        if (changed) {
            RebuildTagIndex();
            QueueTagSave();
        }
        return;
    }
    if (legacy_names.empty()) return;
    bool changed = false;
    for (const auto& name : legacy_names) {
        for (auto& t : tags) {
            if (!EqualI(t.name, TrimTagName(name))) continue;
            if (std::none_of(t.paths.begin(), t.paths.end(),
                             [&](const std::wstring& existing) { return EqualI(existing, n); })) {
                t.paths.push_back(n);
                changed = true;
            }
        }
    }
    if (changed) {
        RebuildTagIndex();
        QueueTagSave();
    }
}

void PlacesCatalog::ReadAdsIntoCatalog(const std::wstring& path) {
    const std::wstring n = Norm(path);
    const auto records = ReadTagAdsV2(n);
    MergeAdsRecords(n, records, records.empty() ? ReadTagAds(n) : std::vector<std::wstring>{});
}

namespace {
// Tags are metadata: writing or deleting the stream must not change the
// file's modified time (NTFS updates it for any stream). Restored when the
// guard goes, after the stream handle is closed; the last error survives.
class KeepFileTimes {
public:
    explicit KeepFileTimes(const std::wstring& path) {
        file_ = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        have_ = file_ != INVALID_HANDLE_VALUE && GetFileTime(file_, nullptr, &access_, &write_);
    }
    ~KeepFileTimes() {
        const DWORD error = GetLastError();
        if (have_) SetFileTime(file_, nullptr, &access_, &write_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
        SetLastError(error);
    }
    KeepFileTimes(const KeepFileTimes&) = delete;
    KeepFileTimes& operator=(const KeepFileTimes&) = delete;
private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    FILETIME access_{}, write_{};
    bool have_ = false;
};
} // namespace

bool WriteTagAdsV2(const std::wstring& path, const std::vector<TagAdsRecord>& tags) {
    std::wstring n = Norm(path);
    if (n.empty() || fs::IsVirtualPath(n)) return false;
    const KeepFileTimes keep_times(n);
    if (tags.empty()) {
        if (DeleteFileW((n + kAdsSuffix).c_str())) return true;
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    std::wstring payload = L"PULSE_TAGS_V2\r\n";
    for (const auto& tag : tags) {
        std::wstring name = tag.name;
        for (auto& c : name) if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
        wchar_t rgb[8]{};
        swprintf_s(rgb, L"%06X", tag.rgb & 0xFFFFFFu);
        payload += tag.id + L"\t" + rgb + L"\t" + name + L"\r\n";
    }
    const std::wstring stream = n + kAdsSuffix;
    HANDLE h = CreateFileW(stream.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, payload.data(),
        static_cast<DWORD>(payload.size() * sizeof(wchar_t)), &written, nullptr);
    const DWORD write_error = ok ? ERROR_WRITE_FAULT : GetLastError();
    CloseHandle(h);
    if (!ok || written != payload.size() * sizeof(wchar_t)) SetLastError(write_error);
    return ok != 0 && written == payload.size() * sizeof(wchar_t);
}

static std::wstring ReadAdsPayload(const std::wstring& path) {
    const std::wstring n = Norm(path);
    if (n.empty() || fs::IsVirtualPath(n)) return {};
    HANDLE h = CreateFileW((n + kAdsSuffix).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 65536 ||
        size.QuadPart % sizeof(wchar_t) != 0) {
        CloseHandle(h);
        return {};
    }
    std::wstring payload(static_cast<size_t>(size.QuadPart / sizeof(wchar_t)), L'\0');
    DWORD read = 0;
    if (!ReadFile(h, payload.data(), static_cast<DWORD>(size.QuadPart), &read, nullptr)) {
        CloseHandle(h);
        return {};
    }
    CloseHandle(h);
    payload.resize(read / sizeof(wchar_t));
    return payload;
}

std::vector<TagAdsRecord> ReadTagAdsV2(const std::wstring& path) {
    std::vector<TagAdsRecord> out;
    const std::wstring payload = ReadAdsPayload(path);
    constexpr std::wstring_view header = L"PULSE_TAGS_V2\r\n";
    if (!payload.starts_with(header)) return out;
    size_t pos = header.size();
    while (pos < payload.size()) {
        const size_t end = payload.find(L"\r\n", pos);
        const std::wstring line = payload.substr(pos,
            (end == std::wstring::npos ? payload.size() : end) - pos);
        const size_t first = line.find(L'\t');
        const size_t second = first == std::wstring::npos ? first : line.find(L'\t', first + 1);
        if (first != std::wstring::npos && second != std::wstring::npos) {
            TagAdsRecord record;
            record.id = line.substr(0, first);
            record.rgb = static_cast<uint32_t>(wcstoul(line.substr(first + 1,
                second - first - 1).c_str(), nullptr, 16));
            record.name = line.substr(second + 1);
            if (!record.id.empty()) out.push_back(std::move(record));
        }
        if (end == std::wstring::npos) break;
        pos = end + 2;
    }
    return out;
}

std::vector<std::wstring> ReadTagAds(const std::wstring& path) {
    std::vector<std::wstring> out;
    const auto records = ReadTagAdsV2(path);
    if (!records.empty()) {
        for (const auto& record : records) out.push_back(record.name);
        return out;
    }
    const std::wstring payload = ReadAdsPayload(path);
    std::wstring cur;
    for (wchar_t c : payload) {
        if (c == L',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else if (c != 0) {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

} // namespace pulse::app

// places.cpp — Persist workspaces / tags / network pins + NTFS ADS.
#include "places.h"
#include "session.h"
#include "../fs/fs_enum.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <algorithm>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <objbase.h>
#include <cwctype>

namespace pulse::app {

namespace {

constexpr const wchar_t* kAdsSuffix = L":Pulse.Tag";

static std::wstring FolderTitle(std::wstring path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) path = L"\\\\" + path.substr(8);
    else if (path.starts_with(L"\\\\?\\")) path = path.substr(4);
    while (path.size() > 1 && path.back() == L'\\') path.pop_back();
    auto pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos && pos + 1 < path.size()) return path.substr(pos + 1);
    return path.empty() ? L"Workspace" : path;
}

static uint32_t ExtractRgb(const std::wstring& json) {
    std::wstring quoted = L"\"rgb\"";
    size_t pos = json.find(quoted);
    if (pos == std::wstring::npos) return 0xEF4444;
    pos = json.find(L':', pos);
    if (pos == std::wstring::npos) return 0xEF4444;
    ++pos;
    while (pos < json.size() && json[pos] == L' ') ++pos;
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

static std::wstring Norm(const std::wstring& p) {
    if (p.empty() || fs::IsVirtualPath(p)) return p;
    return fs::NormalizePath(p);
}

static std::wstring TagKey(const std::wstring& p) {
    std::wstring key = Norm(p);
    for (auto& c : key) c = static_cast<wchar_t>(std::towlower(c));
    return key;
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

bool PlacesCatalog::SaveTagFile(const std::vector<ColorTag>& snapshot) {
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
    file << L"{\n  \"version\":2,\n  \"tags\":[\n";
    for (size_t i = 0; i < snapshot.size(); ++i) {
        std::wstring id, name;
        pulse::json::Escape(snapshot[i].id, id);
        pulse::json::Escape(snapshot[i].name, name);
        wchar_t rgb[16]{};
        swprintf_s(rgb, L"0x%06X", snapshot[i].rgb & 0xFFFFFFu);
        file << L"    {\"id\":\"" << id << L"\",\"name\":\"" << name
             << L"\",\"rgb\":" << rgb << L",\"paths\":";
        write_array(snapshot[i].paths);
        file << L"}" << (i + 1 < snapshot.size() ? L"," : L"") << L"\n";
    }
    file << L"  ]\n}\n";
    return WriteUtf8FileAtomic(dir + L"\\tags.json", file.str());
}

void PlacesCatalog::QueueTagSave() const {
    if (!persist) return;
    {
        std::lock_guard<std::mutex> lock(tag_save_mutex_);
        pending_tag_save_ = tags;
        if (!tag_save_thread_.joinable()) {
            tag_save_stop_ = false;
            tag_save_thread_ = std::thread([this] {
                for (;;) {
                    std::optional<std::vector<ColorTag>> snapshot;
                    {
                        std::unique_lock<std::mutex> lock(tag_save_mutex_);
                        tag_save_cv_.wait(lock, [this] {
                            return tag_save_stop_ || pending_tag_save_.has_value();
                        });
                        if (pending_tag_save_) {
                            snapshot = std::move(pending_tag_save_);
                            pending_tag_save_.reset();
                        } else if (tag_save_stop_) {
                            return;
                        }
                    }
                    if (snapshot) SaveTagFile(*snapshot);
                    std::lock_guard<std::mutex> lock(tag_save_mutex_);
                    if (tag_save_stop_ && !pending_tag_save_) return;
                }
            });
        }
    }
    tag_save_cv_.notify_one();
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
    struct Def { const wchar_t* id; const wchar_t* name; uint32_t rgb; };
    static const Def kDefs[] = {
        { L"finder-red", L"紧急修补", 0xEF4444 },
        { L"finder-orange", L"设计审阅", 0xF59E0B },
        { L"finder-green", L"进行中", 0x22C55E },
        { L"finder-yellow", L"待确认", 0xEAB308 },
        { L"finder-purple", L"灵感参考", 0xA855F7 },
        { L"finder-blue", L"参考资料", 0x3B82F6 },
        { L"finder-gray", L"归档", 0x94A3B8 },
    };
    for (const auto& d : kDefs) {
        ColorTag t;
        t.id = d.id;
        t.name = d.name;
        t.rgb = d.rgb;
        tags.push_back(std::move(t));
    }
    RebuildTagIndex();
}

bool PlacesCatalog::Load() {
    workspaces.clear();
    tags.clear();
    networks.clear();
    quick_access_paths.clear();
    starred_items.clear();
    recent_items.clear();
    starred_index_.clear();
    active_workspace = -1;
    std::wstring dir = GetPulseDataDir();
    if (dir.empty()) { EnsureDefaults(); return false; }
    std::wstring json;
    ReadUtf8File(dir + L"\\places.json", json);
    const bool places_loaded = !json.empty();

    active_workspace = pulse::json::ExtractInt(json, L"active_workspace");
    if (json.find(L"\"active_workspace\"") == std::wstring::npos) active_workspace = -1;

    size_t pos = json.find(L"\"workspaces\"");
    if (pos != std::wstring::npos) {
        pos = json.find(L'[', pos);
        if (pos != std::wstring::npos) {
            ++pos;
            while (pos < json.size() && json[pos] != L']') {
                size_t obj = json.find(L'{', pos);
                if (obj == std::wstring::npos || obj > json.find(L']', pos)) break;
                size_t end = json.find(L'}', obj);
                if (end == std::wstring::npos) break;
                std::wstring block = json.substr(obj, end - obj + 1);
                Workspace w;
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
                pos = end + 1;
            }
        }
    }

    pos = json.find(L"\"tags\"");
    if (pos != std::wstring::npos) {
        pos = json.find(L'[', pos);
        if (pos != std::wstring::npos) {
            ++pos;
            while (pos < json.size() && json[pos] != L']') {
                size_t obj = json.find(L'{', pos);
                if (obj == std::wstring::npos || obj > json.find(L']', pos)) break;
                size_t end = json.find(L'}', obj);
                if (end == std::wstring::npos) break;
                std::wstring block = json.substr(obj, end - obj + 1);
                ColorTag t;
                t.id = pulse::json::ExtractString(block, L"id");
                if (t.id.empty()) t.id = NewTagId();
                t.name = pulse::json::ExtractString(block, L"name");
                t.rgb = ExtractRgb(block);
                t.paths = pulse::json::ExtractStringArray(block, L"paths");
                if (!t.name.empty()) tags.push_back(std::move(t));
                pos = end + 1;
            }
        }
    }

    pos = json.find(L"\"networks\"");
    if (pos != std::wstring::npos) {
        pos = json.find(L'[', pos);
        if (pos != std::wstring::npos) {
            ++pos;
            while (pos < json.size() && json[pos] != L']') {
                size_t obj = json.find(L'{', pos);
                if (obj == std::wstring::npos || obj > json.find(L']', pos)) break;
                size_t end = json.find(L'}', obj);
                if (end == std::wstring::npos) break;
                std::wstring block = json.substr(obj, end - obj + 1);
                NetworkPlace n;
                n.name = pulse::json::ExtractString(block, L"name");
                n.unc = pulse::json::ExtractString(block, L"unc");
                if (!n.unc.empty()) networks.push_back(std::move(n));
                pos = end + 1;
            }
        }
    }

    for (const auto& path : pulse::json::ExtractStringArray(json, L"quick_access_paths")) {
        const auto normalized = Norm(path);
        if (!normalized.empty() && !fs::IsVirtualPath(normalized) &&
            !IsQuickAccessPinned(normalized)) quick_access_paths.push_back(normalized);
    }

    const bool has_starred_items = pulse::json::ValuePosition(
        json, L"starred_items") != std::wstring::npos;
    for (const auto& block : ExtractObjectArray(json, L"starred_items")) {
        StarredItem item;
        item.path = Norm(pulse::json::ExtractString(block, L"path"));
        item.kind = ParseKind(pulse::json::ExtractString(block, L"kind"));
        item.badge = TrimBadge(pulse::json::ExtractString(block, L"badge"));
        item.badge_rgb = ExtractRgb(block);
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
    std::wstring tag_json;
    if (ReadUtf8File(dir + L"\\tags.json", tag_json) && !tag_json.empty()) {
        std::vector<ColorTag> loaded;
        size_t tag_pos = tag_json.find(L"\"tags\"");
        if (tag_pos != std::wstring::npos) tag_pos = tag_json.find(L'[', tag_pos);
        if (tag_pos != std::wstring::npos) {
            ++tag_pos;
            while (tag_pos < tag_json.size() && tag_json[tag_pos] != L']') {
                const size_t object = tag_json.find(L'{', tag_pos);
                if (object == std::wstring::npos || object > tag_json.find(L']', tag_pos)) break;
                const size_t end = tag_json.find(L'}', object);
                if (end == std::wstring::npos) break;
                const std::wstring block = tag_json.substr(object, end - object + 1);
                ColorTag tag;
                tag.id = pulse::json::ExtractString(block, L"id");
                tag.name = pulse::json::ExtractString(block, L"name");
                tag.rgb = ExtractRgb(block);
                tag.paths = pulse::json::ExtractStringArray(block, L"paths");
                if (!tag.id.empty() && !tag.name.empty()) loaded.push_back(std::move(tag));
                tag_pos = end + 1;
            }
        }
        if (!loaded.empty()) {
            tags = std::move(loaded);
            tags_loaded = true;
        }
    }

    EnsureDefaults();
    RebuildTagIndex();
    RebuildStarIndex();
    if (places_loaded && !tags_loaded) QueueTagSave();
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
    snapshot.networks = networks;
    snapshot.quick_access_paths = quick_access_paths;
    snapshot.starred_items = starred_items;
    snapshot.recent_items = recent_items;
    snapshot.active_workspace = active_workspace;
    snapshot.persist = persist;
    return snapshot;
}

void PlacesCatalog::MarkPlacesDirty() const {
    places_save_revision_.fetch_add(1, std::memory_order_relaxed);
    places_save_due_.store(GetTickCount64() + 1000, std::memory_order_release);
}

void PlacesCatalog::QueuePlacesSave() const {
    if (!persist) return;
    auto snapshot = CaptureSaveSnapshot();
    const uint64_t revision = places_save_revision_.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(places_save_mutex_);
        pending_places_save_ = std::make_pair(std::move(snapshot), revision);
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
                        bool saved = false;
                        if (places_save_revision_.load(std::memory_order_acquire)
                                == pending->second) {
                            std::lock_guard<std::mutex> lock(places_save_io_mutex_);
                            // A synchronous Save() may have superseded this
                            // snapshot while it was waiting for the IO lock.
                            if (places_save_revision_.load(std::memory_order_acquire)
                                    == pending->second) {
                                saved = SaveSnapshotFile(pending->first);
                            }
                        }
                        if (saved && places_save_revision_.load(std::memory_order_acquire)
                                == pending->second) {
                            places_save_due_.store(0, std::memory_order_release);
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
    const SaveSnapshot snapshot = CaptureSaveSnapshot();
    std::lock_guard<std::mutex> lock(places_save_io_mutex_);
    const bool saved = SaveSnapshotFile(snapshot);
    if (saved) places_save_due_.store(0, std::memory_order_release);
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
    f << L"{\n  \"places_version\":2,\n  \"tag_version\":2,\n  \"active_workspace\":" << snapshot.active_workspace
      << L",\n  \"workspaces\":[\n";
    for (size_t i = 0; i < snapshot.workspaces.size(); ++i) {
        const auto& w = snapshot.workspaces[i];
        std::wstring name, root;
        pulse::json::Escape(w.name, name);
        pulse::json::Escape(w.root, root);
        f << L"    {\"name\":\"" << name << L"\",\"root\":\"" << root
          << L"\",\"layout\":" << w.layout << L",\"panes\":";
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
          << L"\",\"rgb\":" << rgb << L",\"paths\":";
        writeArr(t.paths);
        f << L"}";
        if (i + 1 < snapshot.tags.size()) f << L",";
        f << L"\n";
    }
    f << L"  ],\n  \"networks\":[\n";
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
          << L"\",\"rgb\":" << rgb << L"}";
        if (i + 1 < snapshot.starred_items.size()) f << L",";
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
    return WriteUtf8FileAtomic(dir + L"\\places.json", f.str());
}

bool PlacesCatalog::FlushPendingSave(bool force) const {
    const ULONGLONG due = places_save_due_.load(std::memory_order_acquire);
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

std::vector<std::wstring> PlacesCatalog::StarredPaths() const {
    std::vector<std::wstring> out;
    out.reserve(starred_items.size());
    for (const auto& item : starred_items)
        if (item.kind == PlaceItemKind::Folder) out.push_back(item.path);
    for (const auto& item : starred_items)
        if (item.kind != PlaceItemKind::Folder) out.push_back(item.path);
    return out;
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
        }
    }
    if (changed) Save();
    return changed;
}

void PlacesCatalog::RemapPaths(const std::wstring& old_path, const std::wstring& new_path) {
    const std::wstring old_norm = Norm(old_path);
    const std::wstring new_norm = Norm(new_path);
    if (old_norm.empty() || new_norm.empty() || EqualI(old_norm, new_norm)) return;
    bool changed = false;
    for (auto& tag : tags) {
        for (auto& path : tag.paths) {
            if (!PathIsOrDescendant(path, old_norm)) continue;
            path = new_norm + path.substr(old_norm.size());
            changed = true;
        }
        std::sort(tag.paths.begin(), tag.paths.end());
        tag.paths.erase(std::unique(tag.paths.begin(), tag.paths.end()), tag.paths.end());
    }
    std::unordered_set<std::wstring> quick_seen;
    for (auto& path : quick_access_paths) {
        if (!PathIsOrDescendant(path, old_norm)) continue;
        path = new_norm + path.substr(old_norm.size());
        changed = true;
    }
    std::erase_if(quick_access_paths, [&](const auto& path) {
        return !quick_seen.insert(TagKey(path)).second;
    });
    for (auto& item : starred_items) {
        if (!PathIsOrDescendant(item.path, old_norm)) continue;
        item.path = new_norm + item.path.substr(old_norm.size());
        changed = true;
    }
    for (auto& item : recent_items) {
        if (!PathIsOrDescendant(item.path, old_norm)) continue;
        item.path = new_norm + item.path.substr(old_norm.size());
        changed = true;
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
    const std::wstring normalized = Norm(path);
    if (normalized.empty()) return;
    bool changed = false;
    for (auto& tag : tags) {
        const size_t before = tag.paths.size();
        std::erase_if(tag.paths, [&](const std::wstring& candidate) {
            return include_descendants ? PathIsOrDescendant(candidate, normalized)
                                       : EqualI(candidate, normalized);
        });
        changed = changed || before != tag.paths.size();
    }
    const size_t starred_before = starred_items.size();
    std::erase_if(starred_items, [&](const StarredItem& item) {
        return include_descendants ? PathIsOrDescendant(item.path, normalized)
                                   : EqualI(item.path, normalized);
    });
    changed = changed || starred_before != starred_items.size();
    const size_t recent_before = recent_items.size();
    std::erase_if(recent_items, [&](const RecentItem& item) {
        return include_descendants ? PathIsOrDescendant(item.path, normalized)
                                   : EqualI(item.path, normalized);
    });
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
                    tag.name = imported_name.empty() ? L"导入的标签" : imported_name;
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

bool WriteTagAdsV2(const std::wstring& path, const std::vector<TagAdsRecord>& tags) {
    std::wstring n = Norm(path);
    if (n.empty() || fs::IsVirtualPath(n)) return false;
    if (tags.empty()) {
        DeleteFileW((n + kAdsSuffix).c_str());
        return true;
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
    CloseHandle(h);
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
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 65536) {
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

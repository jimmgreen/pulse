// places.cpp — Persist workspaces / tags / network pins + NTFS ADS.
#include "places.h"
#include "session.h"
#include "../fs/fs_enum.h"
#include "../common/json_utils.h"
#include <algorithm>
#include <fstream>
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

} // namespace

PlacesCatalog::~PlacesCatalog() {
    StopTagWriter();
}

bool PlacesCatalog::SaveTagFile(const std::vector<ColorTag>& snapshot) {
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    const std::wstring tmp = dir + L"\\tags.tmp";
    const std::wstring final = dir + L"\\tags.json";
    std::wofstream file(tmp, std::wofstream::out | std::wofstream::trunc);
    if (!file) return false;
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
    file.close();
    return MoveFileExW(tmp.c_str(), final.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
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
    starred.clear();
    starred_index_.clear();
    active_workspace = -1;
    std::wstring dir = GetPulseDataDir();
    if (dir.empty()) { EnsureDefaults(); return false; }
    std::wifstream f(dir + L"\\places.json", std::wifstream::binary);
    std::wstringstream ss;
    if (f) ss << f.rdbuf();
    const std::wstring json = ss.str();
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

    starred = pulse::json::ExtractStringArray(json, L"starred");

    bool tags_loaded = false;
    std::wifstream tag_file(dir + L"\\tags.json", std::wifstream::binary);
    if (tag_file) {
        std::wstringstream tag_stream;
        tag_stream << tag_file.rdbuf();
        const std::wstring tag_json = tag_stream.str();
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

bool PlacesCatalog::Save() const {
    if (!persist) return true;
    std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    std::wstring tmp = dir + L"\\places.tmp";
    std::wstring final = dir + L"\\places.json";
    std::wofstream f(tmp, std::wofstream::out | std::wofstream::trunc);
    if (!f) return false;
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
    f << L"{\n  \"tag_version\":2,\n  \"active_workspace\":" << active_workspace
      << L",\n  \"workspaces\":[\n";
    for (size_t i = 0; i < workspaces.size(); ++i) {
        const auto& w = workspaces[i];
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
        if (i + 1 < workspaces.size()) f << L",";
        f << L"\n";
    }
    f << L"  ],\n  \"tags\":[\n";
    for (size_t i = 0; i < tags.size(); ++i) {
        const auto& t = tags[i];
        std::wstring id, name;
        pulse::json::Escape(t.id, id);
        pulse::json::Escape(t.name, name);
        wchar_t rgb[16];
        swprintf_s(rgb, L"0x%06X", t.rgb & 0xFFFFFFu);
        f << L"    {\"id\":\"" << id << L"\",\"name\":\"" << name
          << L"\",\"rgb\":" << rgb << L",\"paths\":";
        writeArr(t.paths);
        f << L"}";
        if (i + 1 < tags.size()) f << L",";
        f << L"\n";
    }
    f << L"  ],\n  \"networks\":[\n";
    for (size_t i = 0; i < networks.size(); ++i) {
        const auto& n = networks[i];
        std::wstring name, unc;
        pulse::json::Escape(n.name, name);
        pulse::json::Escape(n.unc, unc);
        f << L"    {\"name\":\"" << name << L"\",\"unc\":\"" << unc << L"\"}";
        if (i + 1 < networks.size()) f << L",";
        f << L"\n";
    }
    f << L"  ],\n  \"starred\":";
    writeArr(starred);
    f << L"\n}\n";
    f.close();
    return MoveFileExW(tmp.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
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

std::vector<TagId> PlacesCatalog::TagIdsForPath(const std::wstring& path) const {
    std::vector<TagId> out;
    for (int index : TagsForPath(path)) {
        if (index >= 0 && index < static_cast<int>(tags.size()))
            out.push_back(tags[static_cast<size_t>(index)].id);
    }
    return out;
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
    starred_index_.reserve(starred.size());
    for (const auto& path : starred) {
        const std::wstring key = TagKey(path);
        if (!key.empty()) starred_index_.insert(key);
    }
}

bool PlacesCatalog::IsStarred(const std::wstring& path) const {
    if (path.empty()) return false;
    return starred_index_.contains(TagKey(path));
}

bool PlacesCatalog::ToggleStarred(const std::wstring& path) {
    const std::wstring n = Norm(path);
    if (n.empty() || fs::IsVirtualPath(n)) return false;
    const std::wstring key = TagKey(n);
    const auto it = std::find_if(starred.begin(), starred.end(),
        [&](const std::wstring& existing) { return TagKey(existing) == key; });
    bool now_starred = false;
    if (it != starred.end()) {
        starred.erase(it);
    } else {
        starred.push_back(n);
        now_starred = true;
    }
    RebuildStarIndex();
    ++tag_revision_;
    Save();
    return now_starred;
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
    for (auto& path : starred) {
        if (!PathIsOrDescendant(path, old_norm)) continue;
        path = new_norm + path.substr(old_norm.size());
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
    std::vector<std::wstring> star_clones;
    for (const auto& path : starred) {
        if (PathIsOrDescendant(path, src)) star_clones.push_back(dst + path.substr(src.size()));
    }
    for (auto& clone : star_clones) {
        if (std::find(starred.begin(), starred.end(), clone) == starred.end()) {
            starred.push_back(std::move(clone));
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
    const size_t starred_before = starred.size();
    std::erase_if(starred, [&](const std::wstring& candidate) {
        return include_descendants ? PathIsOrDescendant(candidate, normalized)
                                   : EqualI(candidate, normalized);
    });
    changed = changed || starred_before != starred.size();
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

bool WriteTagAds(const std::wstring& path, const std::vector<std::wstring>& tag_names) {
    std::wstring n = Norm(path);
    if (n.empty() || fs::IsVirtualPath(n)) return false;
    std::wstring stream = n + kAdsSuffix;
    HANDLE h = CreateFileW(stream.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::wstring payload;
    for (size_t i = 0; i < tag_names.size(); ++i) {
        if (i) payload += L",";
        payload += tag_names[i];
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, payload.data(),
                        static_cast<DWORD>(payload.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
    if (tag_names.empty()) DeleteFileW(stream.c_str());
    return ok != 0;
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

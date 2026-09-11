#include "change_tracking.h"
#include "../ipc/protocol.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <unordered_set>
namespace pulse::index {
namespace {
constexpr uint64_t kRetention = 7 * 86400;
constexpr size_t kMaxRecords = 100000;
std::wstring Normalize(std::wstring value) {
    for (auto& c : value) { if (c == L'/') c = L'\\'; c = static_cast<wchar_t>(towlower(c)); }
    if (value.rfind(L"\\\\?\\unc\\", 0) == 0) value = L"\\\\" + value.substr(8);
    else if (value.rfind(L"\\\\?\\", 0) == 0) value.erase(0, 4);
    while (value.size() > 3 && value.back() == L'\\') value.pop_back();
    return value;
}
bool Under(const std::wstring& file, const std::wstring& folder) {
    auto a = Normalize(file), b = Normalize(folder);
    if (b.empty()) return false;
    return a == b || (a.size() > b.size() && a.compare(0, b.size(), b) == 0 &&
                     (b.back() == L'\\' || a[b.size()] == L'\\'));
}
bool RecyclePath(const std::wstring& path) {
    const auto value = Normalize(path);
    // Windows owns this directory only at a local volume root, not an arbitrary
    // folder containing the same name or a similarly named network share.
    if (value.size() < 24 || value[1] != L':' || value.substr(2, 14) != L"\\$recycle.bin\\") return false;
    const auto end = value.find(L'\\', 16);
    if (end == std::wstring::npos || end + 1 == value.size() || value.compare(16, 4, L"s-1-") != 0) return false;
    return std::all_of(value.begin() + 20, value.begin() + end, [](wchar_t c) { return (c >= L'0' && c <= L'9') || c == L'-'; });
}
bool NormalizeRecycleEvent(ChangeRecord& event) {
    const bool current = RecyclePath(event.path), old = RecyclePath(event.old_path);
    if (event.kind == ChangeKind::Renamed && current && !old && !event.old_path.empty()) {
        event.path = event.old_path; event.old_path.clear(); event.kind = ChangeKind::Deleted;
    } else if (event.kind == ChangeKind::Renamed && old && !current) {
        event.old_path.clear(); event.kind = ChangeKind::Created;
    } else if (current || old) return false;
    return true;
}
int Priority(ChangeKind kind) {
    switch (kind) {
    case ChangeKind::Deleted: return 5;
    case ChangeKind::Created: return 4;
    case ChangeKind::MovedIn: case ChangeKind::MovedOut: return 3;
    case ChangeKind::Renamed: return 2;
    default: return 1;
    }
}
using Aliases = std::unordered_map<std::wstring, std::wstring>;
std::wstring Identity(const ChangeRecord& e) {
    const auto path = Normalize(e.path);
    return e.file_id ? path.substr(0, (std::min)(path.size(), size_t(2))) + L":" + std::to_wstring(e.file_id) : path;
}
Aliases EventAliases(const std::vector<ChangeRecord>& records, uint64_t since, uint64_t now) {
    Aliases aliases;
    for (const auto& e : records) if (e.source == ChangeSource::Event && e.time >= since && e.time + kRetention >= now) {
        auto key = Identity(e);
        const auto existing = aliases.find(Normalize(e.path));
        const auto old = aliases.find(Normalize(e.old_path));
        if (!e.file_id && existing != aliases.end()) key = existing->second;
        else if (!e.file_id && old != aliases.end()) key = old->second;
        aliases[Normalize(e.path)] = key;
        if (!e.old_path.empty()) aliases[Normalize(e.old_path)] = key;
    }
    return aliases;
}
std::wstring Key(const ChangeRecord& e, const Aliases& aliases) {
    if (e.file_id) return Identity(e);
    auto found = aliases.find(Normalize(e.path));
    return found == aliases.end() ? Identity(e) : found->second;
}
ChangeRecord Relative(ChangeRecord e, const std::wstring& path) {
    if (e.kind == ChangeKind::Renamed && !e.old_path.empty()) {
        const bool current = Under(e.path, path), old = Under(e.old_path, path);
        if (current != old) e.kind = current ? ChangeKind::MovedIn : ChangeKind::MovedOut;
        else if (current) {
            const auto current_path = Normalize(e.path), old_path = Normalize(e.old_path);
            if (current_path.substr(0, current_path.find_last_of(L'\\')) != old_path.substr(0, old_path.find_last_of(L'\\')))
                e.kind = ChangeKind::MovedIn;
        }
    }
    return e;
}
void Merge(ChangeRecord& into, const ChangeRecord& event) {
    const auto kind = into.kind == ChangeKind::Deleted && event.kind == ChangeKind::Created ? event.kind :
        (Priority(into.kind) >= Priority(event.kind) ? into.kind : event.kind);
    const auto time = (std::max)(into.time, event.time);
    const auto old_path = !event.old_path.empty() ? event.old_path : into.old_path;
    if (event.id >= into.id) into = event;
    into.kind = kind; into.time = time; into.old_path = old_path;
}
template <class Visitor>
void VisitAncestors(const ChangeRecord& event, Visitor visit) {
    const auto current = Normalize(event.path);
    auto walk = [&](std::wstring path, bool old) {
        auto emit = [&] {
            // Common ancestors of old and new paths only receive this event once.
            if (old && (current == path || (current.size() > path.size() && current.compare(0, path.size(), path) == 0 &&
                (path.back() == L'\\' || current[path.size()] == L'\\')))) return;
            visit(path);
        };
        if (path.empty()) return;
        if (event.is_dir) emit();
        while (path.size() > 3) {
            const auto slash = path.find_last_of(L'\\');
            if (slash == std::wstring::npos || slash < 2) break;
            path.resize(slash == 2 && path[1] == L':' ? 3 : slash);
            emit();
        }
    };
    walk(current, false);
    if (!event.old_path.empty()) walk(Normalize(event.old_path), true);
}
std::wstring File(const std::wstring& dir, std::wstring owner) {
    for (auto& c : owner) if (!iswalnum(c) && c != L'-') c = L'_';
    return dir + L"\\changes-" + owner + L".bin";
}
}
std::wstring NormalizeChangePath(std::wstring path) { return Normalize(std::move(path)); }
bool IsChangeJournalName(std::wstring_view name) {
    const auto slash = name.find_last_of(L"\\/");
    if (slash != std::wstring_view::npos) name.remove_prefix(slash + 1);
    if (name.size() < 12 || _wcsnicmp(name.data(), L"changes-", 8) != 0) return false;
    const auto dot = name.find(L".bin", 8);
    return dot != std::wstring_view::npos && (dot + 4 == name.size() ||
        (dot + 8 == name.size() && _wcsnicmp(name.data() + dot + 4, L".tmp", 4) == 0));
}
uint64_t ChangeTracker::Now() {
    FILETIME ft{}; GetSystemTimeAsFileTime(&ft);
    return (((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000000) - 11644473600ULL;
}
void ChangeTracker::Open(const std::wstring& directory) {
    std::lock_guard lock(mutex_); directory_ = directory;
}
void ChangeTracker::Load(const std::wstring& owner, Journal& j) {
    if (j.loaded) return;
    j.loaded = true;
    std::ifstream input(std::filesystem::path(File(directory_, owner)), std::ios::binary);
    if (!input) return;
    const auto now = Now();
    auto corrupt = [&] { j.gap = true; j.gap_end = now; j.gap_until = now + kRetention; j.dirty = true; };
    input.seekg(0, std::ios::end); const auto size = input.tellg();
    if (size <= 0 || size > 32 * 1024 * 1024) { corrupt(); return; }
    input.seekg(0); std::vector<uint8_t> data(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(data.data()), size);
    ipc::PayloadReader r(data.data(), data.size()); uint32_t magic = 0, count = 0;
    if (!input || !r.GetU32(magic) || (magic != 0x32484350 && magic != 0x33484350) ||
        !r.GetU32(count) || count > kMaxRecords) { corrupt(); return; }
    uint64_t origin = 0, paused = 0, gap_end = 0, gap_until = 0; uint32_t active = 0;
    if (magic == 0x33484350 && (!r.GetU64(origin) || !r.GetU64(paused) || !r.GetU64(gap_end) ||
        !r.GetU64(gap_until) || !r.GetU32(active) || active > 1)) { corrupt(); return; }
    std::vector<ChangeRecord> records;
    for (uint32_t i = 0; i < count; ++i) {
        ChangeRecord e; uint32_t kind = 0, dir = 0, source = 0;
        if (!r.GetU64(e.id) || !r.GetU64(e.time) || !r.GetU64(e.file_id) || !r.GetU32(kind) || kind > 5 ||
            !r.GetU32(dir) || !r.GetU32(source) || source > 1 || !r.GetString(e.path) || !r.GetString(e.old_path)) { corrupt(); return; }
        if (source != static_cast<uint32_t>(ChangeSource::Event)) continue;
        e.kind = static_cast<ChangeKind>(kind); e.is_dir = dir != 0;
        if (!NormalizeRecycleEvent(e)) continue;
        next_id_ = (std::max)(next_id_, e.id + 1);
        if (magic == 0x32484350 && (!origin || e.time < origin)) origin = e.time;
        if (e.time + kRetention >= now) records.push_back(std::move(e));
    }
    if (r.remaining()) { corrupt(); return; }
    j.tracking_since = origin; j.paused_at = paused; j.active = active != 0;
    j.gap_end = gap_end; j.gap_until = gap_until; j.gap = gap_until != 0;
    // v2 had no lifecycle metadata. Preserve captured events, but do not claim
    // continuity across the upgrade/restart or import filesystem mtime history.
    if (magic == 0x32484350 && origin) {
        j.gap = true; j.gap_end = now; j.gap_until = now + kRetention;
    }
    j.records = std::move(records); ++j.revision; j.dirty = magic == 0x32484350;
}
void ChangeTracker::Prune(Journal& j) {
    const auto old_count = j.records.size();
    const auto now = Now();
    std::erase_if(j.records, [now](const auto& e) { return e.time + kRetention < now; });
    if (j.records.size() > kMaxRecords) {
        j.records.erase(j.records.begin(), j.records.begin() + (j.records.size() - kMaxRecords)); j.gap = true; j.gap_end = Now(); j.gap_until = j.gap_end + kRetention;
    }
    size_t bytes = 44, keep = j.records.size();
    while (keep > 0) {
        const auto& e = j.records[keep - 1];
        const auto cost = 44 + (e.path.size() + e.old_path.size()) * sizeof(wchar_t);
        if (bytes + cost > 32 * 1024 * 1024) break;
        bytes += cost; --keep;
    }
    if (keep) { j.records.erase(j.records.begin(), j.records.begin() + keep); j.gap = true; j.gap_end = Now(); j.gap_until = j.gap_end + kRetention; }
    if (j.records.size() != old_count) { ++j.revision; j.dirty = true; }
}
bool ChangeTracker::Save(const std::wstring& owner, const Journal& j) {
    if (directory_.empty()) return false;
    ipc::PayloadWriter w; w.PutU32(0x33484350); w.PutU32(static_cast<uint32_t>(j.records.size()));
    w.PutU64(j.tracking_since); w.PutU64(j.paused_at); w.PutU64(j.gap_end); w.PutU64(j.gap_until);
    w.PutU32(j.active ? 1u : 0u);
    for (const auto& e : j.records) {
        w.PutU64(e.id); w.PutU64(e.time); w.PutU64(e.file_id); w.PutU32(static_cast<uint32_t>(e.kind));
        w.PutU32(e.is_dir ? 1u : 0u); w.PutU32(static_cast<uint32_t>(e.source)); w.PutString(e.path); w.PutString(e.old_path);
    }
    auto path = File(directory_, owner), temp = path + L".tmp";
    std::ofstream out(std::filesystem::path(temp), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(w.data().data()), static_cast<std::streamsize>(w.data().size()));
    out.close();
    return out.good() && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
bool ChangeTracker::Lease(const std::wstring& owner, bool enabled) {
    if (owner.empty()) return false;
    std::lock_guard lock(mutex_); auto& j = journals_[owner]; Load(owner, j);
    const auto now = Now();
    if (enabled) {
        if (!j.tracking_since) { j.tracking_since = now; ++j.revision; }
        else if (j.expiry < now && (j.active || j.paused_at)) {
            j.gap = true; j.gap_end = now; j.gap_until = now + kRetention;
        }
        if (j.expiry < now) j.start = now;
        j.active = true; j.expiry = now + 90; j.paused_at = 0;
    } else {
        if (j.active) j.paused_at = now;
        j.active = false; j.expiry = 0;
    }
    j.seeded = true; j.dirty = true;
    // Only observed events after tracking starts belong to this journal.
    // Returning false avoids scheduling a full-index mtime baseline scan.
    return false;
}
void ChangeTracker::Seed(const std::wstring& owner, std::vector<ChangeRecord> records) {
    (void)owner; (void)records;
}
void ChangeTracker::Record(ChangeRecord e) {
    if (e.source != ChangeSource::Event || !NormalizeRecycleEvent(e)) return;
    if (IsChangeJournalName(e.path) || IsChangeJournalName(e.old_path)) return;
    if (e.path.empty() || e.path.size() > 32767 || e.old_path.size() > 32767) return;
    std::lock_guard lock(mutex_); const auto now = Now(); if (!e.time) e.time = now;
    for (auto& [owner, j] : journals_) {
        if (j.expiry < now || e.time < j.start || e.time > now + 60) continue;
        bool duplicate = false;
        for (auto it = j.records.rbegin(); it != j.records.rend() && it->time + 2 >= e.time; ++it) {
            if (e.file_id && it->file_id == e.file_id && it->kind != e.kind) break;
            if (it->source == ChangeSource::Event && it->kind == e.kind && (!e.file_id || it->file_id == e.file_id) && Normalize(it->path) == Normalize(e.path) &&
                Normalize(it->old_path) == Normalize(e.old_path)) { duplicate = true; break; }
        }
        if (duplicate) continue;
        e.id = next_id_++; j.records.push_back(e); ++j.revision;
        j.dirty = true;
    }
}
void ChangeTracker::Flush() {
    std::vector<std::pair<std::wstring, Journal>> pending;
    {
        std::lock_guard lock(mutex_);
        for (auto& [owner, journal] : journals_) {
            Prune(journal);
            if (!journal.dirty) continue;
            Journal snapshot; snapshot.records = journal.records;
            snapshot.tracking_since = journal.tracking_since; snapshot.paused_at = journal.paused_at;
            snapshot.gap_end = journal.gap_end; snapshot.gap_until = journal.gap_until; snapshot.active = journal.active;
            pending.emplace_back(owner, std::move(snapshot)); journal.dirty = false;
        }
    }
    // Serialization and disk I/O hold neither the index lock nor the tracker lock.
    for (const auto& [owner, snapshot] : pending) {
        if (Save(owner, snapshot)) continue;
        std::lock_guard lock(mutex_);
        auto& journal = journals_[owner]; journal.gap = true;
        journal.gap_end = Now(); journal.gap_until = journal.gap_end + kRetention; journal.dirty = true;
    }
}
void ChangeTracker::Gap() {
    std::lock_guard lock(mutex_); const auto now = Now();
    for (auto& [owner, j] : journals_) {
        if (!j.active || j.expiry < now) continue;
        j.gap = true; j.gap_end = now; j.gap_until = now + kRetention; j.dirty = true;
    }
}
ChangeResponse ChangeTracker::Details(const std::wstring& owner, const std::wstring& path,
                                     uint64_t since, uint64_t before, uint32_t limit, uint32_t kind_filter) {
    std::lock_guard lock(mutex_); ChangeResponse out;
    if (path.empty()) return out;
    auto& j = journals_[owner]; Load(owner, j); const auto now = Now();
    since = (std::max)(since, j.tracking_since);
    out.state = j.expiry < now ? ChangeState::Unavailable :
        (j.gap && now < j.gap_until && since <= j.gap_end ? ChangeState::Gap : ChangeState::Available);
    limit = (std::clamp)(limit, 1u, 200u);
    const auto aliases = EventAliases(j.records, since, now);
    std::unordered_map<std::wstring, ChangeRecord> collapsed;
    for (const auto& event : j.records) {
        if (event.time < since || event.time + kRetention < now) continue;
        if (event.source == ChangeSource::InitialMtime && aliases.contains(Normalize(event.path))) continue;
        if (!Under(event.path, path) && (event.old_path.empty() || !Under(event.old_path, path))) continue;
        auto e = Relative(event, path);
        const auto key = Key(e, aliases);
        auto [it, added] = collapsed.try_emplace(key, e);
        if (!added) Merge(it->second, e);
    }
    std::vector<ChangeRecord> records;
    for (auto& [key, e] : collapsed) {
        if ((before && e.id >= before) || (kind_filter != UINT32_MAX && static_cast<uint32_t>(e.kind) != kind_filter)) continue;
        records.push_back(std::move(e));
    }
    std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) { return a.id > b.id; });
    if (records.size() > limit) { records.resize(limit); out.next_cursor = records.back().id; }
    out.records = std::move(records); return out;
}
ChangeResponse ChangeTracker::Summaries(const std::wstring& owner, const std::vector<std::wstring>& paths, uint64_t since) {
    std::lock_guard lock(mutex_); ChangeResponse out;
    auto& j = journals_[owner]; Load(owner, j); const auto now = Now();
    since = (std::max)(since, j.tracking_since);
    out.state = j.expiry < now ? ChangeState::Unavailable :
        (j.gap && now < j.gap_until && since <= j.gap_end ? ChangeState::Gap : ChangeState::Available);
    if (j.cache_revision != j.revision || since < j.cache_since || since > j.cache_oldest || now >= j.cache_expires) {
        const auto aliases = EventAliases(j.records, since, now);
        j.cache_expires = now + kRetention; j.cache_oldest = UINT64_MAX;
        struct Rollup { uint64_t time = 0; ChangeKind kind = ChangeKind::Modified; };
        std::unordered_map<std::wstring, std::unordered_map<uint32_t, Rollup>> rollups;
        std::unordered_map<std::wstring, uint32_t> identities;
        std::unordered_set<std::wstring> initial_paths;
        j.summaries.clear();
        for (const auto& event : j.records) {
            if (event.time < since || event.time + kRetention < now) continue;
            if (event.source == ChangeSource::InitialMtime && aliases.contains(Normalize(event.path))) continue;
            j.cache_expires = (std::min)(j.cache_expires, event.time + kRetention + 1);
            j.cache_oldest = (std::min)(j.cache_oldest, event.time);
            if (event.source == ChangeSource::InitialMtime) {
                if (!initial_paths.insert(Normalize(event.path)).second) continue;
                VisitAncestors(event, [&](const std::wstring& ancestor) {
                    auto& summary = j.summaries[ancestor];
                    ++summary.count; ++summary.initial_count;
                    summary.last_change = (std::max)(summary.last_change, event.time);
                });
                continue;
            }
            const auto key = Key(event, aliases);
            const auto [identity, inserted] = identities.try_emplace(key, static_cast<uint32_t>(identities.size()));
            (void)inserted;
            VisitAncestors(event, [&](const std::wstring& ancestor) {
                const auto kind = event.kind == ChangeKind::Renamed ? Relative(event, ancestor).kind : event.kind;
                auto& aggregate = rollups[ancestor][identity->second];
                aggregate.time = (std::max)(aggregate.time, event.time);
                if (Priority(kind) >= Priority(aggregate.kind) ||
                    (aggregate.kind == ChangeKind::Deleted && kind == ChangeKind::Created)) aggregate.kind = kind;
            });
        }
        for (auto& [path, items] : rollups) {
            auto& summary = j.summaries[path];
            for (const auto& [key, item] : items) {
                ++summary.count; summary.last_change = (std::max)(summary.last_change, item.time);
                ++summary.counts[static_cast<uint32_t>(item.kind)];
                summary.has_deleted |= item.kind == ChangeKind::Deleted;
            }
        }
        j.cache_revision = j.revision; j.cache_since = since;
    }
    j.cache_since = since;
    for (const auto& path : paths) {
        ChangeSummary summary;
        const auto found = j.summaries.find(Normalize(path));
        if (found != j.summaries.end()) summary = found->second;
        summary.path = path; summary.state = out.state;
        summary.incomplete = out.state != ChangeState::Available;
        out.summaries.push_back(std::move(summary));
    }
    return out;
}
}

#include "folder_sizes.h"
#include "../fs/drive_kind.h"
#include "../fs/cloud_state.h"
#include "folder_size_scanner.h"
#include "folder_size_store.h"
#include "../fs/fs_enum.h"
#include "../fs/fs_watch.h"
#include "../common/path_utils.h"
#include "../index/index_feed.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <iterator>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>
#include <utility>

namespace pulse::app {
namespace {
using folder_size::Key;
using folder_size::Within;
constexpr uint64_t kFreshMs = 300000;
constexpr uint64_t kFailureBackoffMs = 600000;
constexpr uint64_t kWatchLeaseMs = 60000;
constexpr size_t kScanConcurrency = 2;
// Volume overview rows use the index/explicit calculation, not N full-tree scans.
bool OverviewPath(const std::wstring& key) {
    const auto p = pulse::path::StripExtendedPathPrefix(key);
    return p.size() >= 3 && p[1] == L':' &&
        (p.size() == 3 || p.find(L'\\', 3) == std::wstring::npos);
}
bool ExpensiveAutomaticPath(const std::wstring& key) {
    if (OverviewPath(key)) return true;
    static const auto system_roots = [] {
        std::vector<std::wstring> paths;
        wchar_t buffer[32768]{};
        if (GetWindowsDirectoryW(buffer, ARRAYSIZE(buffer))) paths.push_back(Key(buffer));
        for (const auto name : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramData"}) {
            const auto count = GetEnvironmentVariableW(name, buffer, ARRAYSIZE(buffer));
            if (count && count < ARRAYSIZE(buffer)) paths.push_back(Key(buffer));
        }
        return paths;
    }();
    return std::any_of(system_roots.begin(), system_roots.end(), [&](const auto& root) { return Within(key, root); });
}
bool VolumeRoot(const std::wstring& key) {
    const auto p = pulse::path::StripExtendedPathPrefix(key);
    return p.size() == 3 && p[1] == L':';
}
constexpr size_t kWatchLimit = 16;
// Emptiness probes: a bounded read of the first entries of visible folders.
constexpr size_t kProbeLimit = 768;        // paths the UI may ask for at once
constexpr size_t kProbeBatch = 48;         // probes per worker pass, I/O outside the lock
constexpr size_t kProbeCacheLimit = 4096;  // results kept for folders no longer shown
constexpr unsigned kProbeEntryCap = 64;    // hidden entries read before giving up
constexpr uint64_t kProbeFreshMs = 10000;  // unwatched folders are re-read this often
#ifndef FILE_ATTRIBUTE_RECALL_ON_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif
bool NetworkPath(const std::wstring& path) {
    return fs::IsUncPath(path) || path.starts_with(L"\\\\?\\unc\\");
}
std::wstring ChildPath(const std::wstring& parent, const std::wstring& name) {
    return parent + (parent.back() == L'\\' ? L"" : L"\\") + name;
}
std::vector<std::wstring> MergeRoots(std::vector<std::wstring> roots) {
    std::sort(roots.begin(), roots.end(), [](const auto& a, const auto& b) {
        return a.size() == b.size() ? a < b : a.size() < b.size();
    });
    std::vector<std::wstring> merged;
    for (auto& root : roots) {
        if (root.empty() || fs::IsVirtualPath(root)) continue;
        if (std::any_of(merged.begin(), merged.end(), [&](const auto& parent) { return Within(root, parent); })) continue;
        if (merged.size() == kWatchLimit) break;
        merged.push_back(std::move(root));
    }
    return merged;
}
}
struct FolderSizes::Impl {
    struct ProbeResult {
        uint8_t bits = 0;
        uint64_t at = 0, generation = 0;
        bool stale = false;
    };
    struct Watch {
        std::shared_ptr<fs::DirWatch> handle;
        uint64_t instance = 0, generation = 0;
        bool started = false;
    };
    mutable std::mutex mutex;
    std::condition_variable wake;
    folder_size::Store store;
    std::map<std::wstring, bool> wanted;
    std::set<std::wstring> protected_paths, manual, blocked_auto;
    std::vector<std::wstring> order, roots;
    std::wstring active_scan_path;
    struct Suspension {
        std::promise<void> ready;
        std::shared_future<void> future = ready.get_future().share();
        std::vector<std::shared_future<void>> watches;
        bool drained = false, completed = false;
    };
    std::map<std::wstring, std::shared_ptr<Suspension>> suspended;
    std::vector<std::pair<std::wstring, std::shared_future<void>>> retiring_watches;
    bool Suspended(const std::wstring& path) const {
        return std::any_of(suspended.begin(), suspended.end(), [&](const auto& item) { return Within(path, item.first); });
    }
    std::vector<FolderSizeRequest> last_visible;
    std::vector<std::wstring> last_roots;
    std::map<std::wstring, Watch> watches;
    std::map<std::wstring, uint64_t> watch_used;
    std::map<std::wstring, UINT> drive_types;
    std::map<std::wstring, ProbeResult> probes;
    std::vector<std::wstring> probe_wanted;
    uint64_t probe_events = 0;
    std::function<std::wstring()> cache_path;
    std::thread worker, index_worker;
    // Set by every thread that changes what the window shows. The first change
    // after the window took the last batch also wakes it (SetChangeNotify), so
    // a result does not wait for the window's idle poll.
    struct ChangeFlag {
        std::atomic<bool> value{false}, posted{false};
        std::atomic<HWND> hwnd{nullptr};
        std::atomic<UINT> message{0};
        ChangeFlag& operator=(bool set) noexcept {
            value.store(set);
            if (set && !posted.exchange(true))
                if (const HWND target = hwnd.load()) PostMessageW(target, message.load(), 0, 0);
            return *this;
        }
        bool exchange(bool set) noexcept { posted.store(false); return value.exchange(set); }
    };
    std::atomic<bool> stopping{false};
    ChangeFlag changed;
    bool index_enabled = true;
    // dirty_revision counts changes to what the cache file holds; a result
    // confirmed again only moves its verified time, which waits for the final
    // save (stamps_dirty) instead of rewriting the file every 3 s.
    uint64_t scope = 0, watch_sequence = 0, dirty_revision = 0, saved_revision = 0;
    bool stamps_dirty = false;
    // Set under mutex with every notify: work arrived since Run's last pass,
    // which may have been outside the lock when the notify fired.
    bool kick = false;
    FolderSizeStats stats;
    HANDLE index_cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Impl() { if (index_cancel) CloseHandle(index_cancel); }

    uint64_t Coverage(const std::wstring& path) const {
        for (const auto& [root, watch] : watches)
            if (Within(path, root) && watch.started && watch.handle->Armed() &&
                std::find(roots.begin(), roots.end(), root) != roots.end()) return watch.generation;
        return 0;
    }
    bool Reusable(const std::wstring& path, const folder_size::Store::Entry& entry) const {
        return entry.value.has_value && entry.value.source == FolderSizeSource::Scan &&
            !entry.value.partial && entry.value.verified && entry.value.verified_revision == entry.revision &&
            entry.watch_generation != 0 && Coverage(path) == entry.watch_generation;
    }
    // A current index total is authoritative for automatic work: the service
    // already counted the subtree (including names it hides from search), so
    // rescanning would only re-read it. Clicks still scan; unavailable or
    // unreachable indexes fall back to scanning.
    static bool IndexCurrent(const folder_size::Store::Entry& entry) {
        return entry.index_current && entry.value.has_value && !entry.value.partial &&
            entry.value.source == FolderSizeSource::Index;
    }
    // A watched result stays current until its watch reports a change; others age out.
    bool ProbeFresh(const std::wstring& key, const ProbeResult& probe, uint64_t now) const {
        if (probe.stale) return false;
        if (probe.generation != 0 && Coverage(key) == probe.generation) return true;
        return now - probe.at < kProbeFreshMs;
    }
    void StaleProbeLocked(const std::wstring& key) {
        // A change to an entry changes what its parent contains.
        for (const auto& path : {key, key.substr(0, key.find_last_of(L'\\'))}) {
            const auto found = probes.find(path);
            if (found != probes.end()) found->second.stale = true;
        }
        ++probe_events;
    }
    // Reads at most kProbeEntryCap entries; anything uncertain stays unknown (0).
    uint8_t ProbeFolder(const std::wstring& key) {
        const auto type = DriveType(key);
        if (type != DRIVE_FIXED && type != DRIVE_RAMDISK) return 0;
        constexpr DWORD kSkip = FILE_ATTRIBUTE_REPARSE_POINT | ::pulse::fs::kCloudPlaceholderAttrs;
        const DWORD attrs = GetFileAttributesW(key.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & kSkip)) return 0;
        WIN32_FIND_DATAW data{};
        const HANDLE find = FindFirstFileExW(ChildPath(key, L"*").c_str(), FindExInfoBasic,
                                             &data, FindExSearchNameMatch, nullptr, 0);
        if (find == INVALID_HANDLE_VALUE)
            return GetLastError() == ERROR_FILE_NOT_FOUND ? FolderContentKnown : 0; // root without . entries
        uint8_t bits = 0;
        unsigned hidden = 0;
        bool complete = false;
        for (;;) {
            const wchar_t* name = data.cFileName;
            const bool dots = name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0));
            if (!dots) {
                if (!(data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) { bits |= FolderContentVisible; complete = true; break; }
                bits |= (data.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) ? FolderContentSystem : FolderContentHidden;
                if (++hidden == kProbeEntryCap) break;
            }
            if (!FindNextFileW(find, &data)) { complete = GetLastError() == ERROR_NO_MORE_FILES; break; }
        }
        FindClose(find);
        return complete ? static_cast<uint8_t>(bits | FolderContentKnown) : 0;
    }
    // Returns true when it did work, so the loop checks again without sleeping.
    bool RunProbes() {
        std::vector<std::wstring> batch;
        uint64_t events = 0;
        {
            std::lock_guard lock(mutex);
            const auto now = GetTickCount64();
            for (const auto& key : probe_wanted) {
                if (Suspended(key)) continue;
                const auto found = probes.find(key);
                if (found != probes.end() && ProbeFresh(key, found->second, now)) continue;
                batch.push_back(key);
                if (batch.size() == kProbeBatch) break;
            }
            events = probe_events;
        }
        if (batch.empty()) return false;
        std::vector<uint8_t> bits;
        bits.reserve(batch.size());
        for (const auto& key : batch) {
            if (stopping) return false;
            bits.push_back(ProbeFolder(key));
        }
        std::lock_guard lock(mutex);
        const auto now = GetTickCount64();
        for (size_t i = 0; i < batch.size(); ++i) {
            auto& probe = probes[batch[i]];
            if (probe.at == 0 || probe.bits != bits[i]) changed = true;
            probe.bits = bits[i]; probe.at = now; probe.generation = Coverage(batch[i]);
            // A change reported while reading may not be in this result: read again.
            probe.stale = probe_events != events;
        }
        return true;
    }
    void InvalidateLocked(const std::wstring& key, uint64_t delay = 200, bool known_only = false) {
        if (key.empty()) return;
        // Staleness is not saved: a restored value is unverified until checked.
        if (store.Invalidate(key, GetTickCount64(), delay, known_only)) changed = true;
    }
    void Invalidate(const std::wstring& path) {
        const auto key = Key(path);
        { std::lock_guard lock(mutex); InvalidateLocked(key); kick = true; }
        wake.notify_all();
    }
    UINT DriveType(const std::wstring& path) {
        const auto plain = pulse::path::StripExtendedPathPrefix(path);
        if (plain.size() < 3 || plain[1] != L':') return DRIVE_REMOTE;
        const auto root = plain.substr(0, 3);
        const auto found = drive_types.find(root);
        if (found != drive_types.end()) return found->second;
        // Cloud mounts count as network drives: never walked for sizes.
        return drive_types.emplace(root, ::pulse::fs::IsRemoteLikeDrive(root) ? static_cast<UINT>(DRIVE_REMOTE)
                                                                       : GetDriveTypeW(root.c_str())).first->second;
    }
    void Save(const std::wstring& file, bool final = false) {
        if (file.empty()) return;
        folder_size::SavedValues values;
        uint64_t revision = 0;
        bool stamps = false;
        {
            std::lock_guard lock(mutex);
            if (dirty_revision == saved_revision && !(final && stamps_dirty)) return;
            values = store.Snapshot(); revision = dirty_revision;
            stamps = std::exchange(stamps_dirty, false);
        }
        // The write runs outside the lock: the UI thread reads sizes under it.
        const bool written = folder_size::WriteValues(file, values);
        std::lock_guard lock(mutex);
        if (written) saved_revision = (std::max)(saved_revision, revision);
        else stamps_dirty |= stamps;
    }
    void UpdateWatches(const std::wstring& cache_key) {
        std::vector<std::shared_ptr<fs::DirWatch>> retired;
        std::vector<std::wstring> create;
        {
            std::lock_guard lock(mutex);
            const auto now = GetTickCount64();
            std::erase_if(roots, [&](const auto& root) {
                if (Suspended(root)) return true;
                const bool active = std::any_of(last_roots.begin(), last_roots.end(), [&](const auto& path) {
                    return Within(path, root) && !VolumeRoot(path);
                });
                if (active) watch_used[root] = now;
                return !active && now - watch_used[root] >= kWatchLeaseMs;
            });
            for (auto it = watches.begin(); it != watches.end();) {
                if (std::find(roots.begin(), roots.end(), it->first) == roots.end()) {
                    for (auto& [path, entry] : store.entries) if (Within(path, it->first)) {
                        // Coverage loss revokes authority, not the published bytes or age.
                        entry.value.verified = false;
                        if (entry.value.has_value && entry.value.source != FolderSizeSource::Index)
                            entry.value.state = FolderSizeState::Cached;
                        ++entry.request_epoch; entry.work = {}; changed = true;
                    }
                    watch_used.erase(it->first);
                    retired.push_back(std::move(it->second.handle)); it = watches.erase(it);
                } else ++it;
            }
            for (const auto& root : roots) if (!watches.contains(root)) create.push_back(root);
        }
        for (auto& watch : retired) {
            auto future = watch->StopAsync();
            retiring_watches.emplace_back(Key(watch->path()), std::move(future));
        }
        std::erase_if(retiring_watches, [](const auto& item) {
            return !item.second.valid() || item.second.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
        });
        for (const auto& root : create) {
            if (stopping || DriveType(root) == DRIVE_REMOTE) continue;
            auto watch = std::make_shared<fs::DirWatch>();
            uint64_t instance = 0;
            {
                std::lock_guard lock(mutex);
                if (Suspended(root) || std::find(roots.begin(), roots.end(), root) == roots.end()) continue;
                instance = ++watch_sequence;
                watches.emplace(root, Watch{watch, instance, instance});
            }
            const bool started = watch->Start(root, [this, root, instance, cache_key](bool gap, std::vector<fs::DirNotifyEvent> events) {
                {
                    std::lock_guard lock(mutex);
                    const auto found = watches.find(root);
                    if (stopping || found == watches.end() || found->second.instance != instance ||
                        std::find(roots.begin(), roots.end(), root) == roots.end()) return;
                    if (gap) {
                        ++stats.watch_gaps;
                        const bool initial = found->second.generation == found->second.instance;
                        found->second.generation = ++watch_sequence;
                        // The first notification reconciles a newly armed read;
                        // it is not a write burst that needs a debounce delay.
                        // Rows still waiting for their first result have nothing
                        // to reconcile: keep their in-flight index answer.
                        InvalidateLocked(root, initial ? 0 : 200, initial);
                        for (auto& [path, probe] : probes) if (Within(path, root)) probe.stale = true;
                        ++probe_events;
                    } else {
                        std::set<std::wstring> paths;
                        for (const auto& event : events) {
                            const auto path = Key(ChildPath(root, event.name));
                            if (!cache_key.empty() && (path == cache_key || path == cache_key + L".tmp")) continue;
                            paths.insert(path);
                            if (!event.old_name.empty()) paths.insert(Key(ChildPath(root, event.old_name)));
                        }
                        for (const auto& path : paths) { InvalidateLocked(path); StaleProbeLocked(path); }
                    }
                    kick = true;
                }
                wake.notify_all();
            }, true);
            {
                std::lock_guard lock(mutex);
                const auto found = watches.find(root);
                if (found != watches.end() && found->second.instance == instance) found->second.started = started;
            }
        }
    }
    void RunIndex() {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        index::IndexFeedConnection connection(index_cancel);
        uint64_t seen_scope = 0, next_poll = 0;
        size_t cursor = 0;
        while (!stopping) {
            std::vector<std::wstring> paths;
            std::vector<std::pair<uint64_t, uint64_t>> versions;
            uint64_t request_scope = 0;
            bool more = false;
            {
                std::unique_lock lock(mutex);
                if (seen_scope != scope) { seen_scope = scope; next_poll = 0; cursor = 0; }
                const auto now = GetTickCount64();
                if (now < next_poll) {
                    wake.wait_for(lock, std::chrono::milliseconds(std::min<uint64_t>(next_poll - now, 250)));
                    continue;
                }
                for (const auto& path : order) {
                    const auto demand = wanted.find(path);
                    const auto entry = store.entries.find(path);
                    if (demand == wanted.end() || !demand->second || entry == store.entries.end() ||
                        manual.contains(path) || NetworkPath(path) || now < entry->second.next_index_at ||
                        (entry->second.value.source == FolderSizeSource::Scan && entry->second.value.has_value &&
                         entry->second.value.state != FolderSizeState::Calculating)) continue;
                    paths.push_back(path);
                }
                if (paths.empty()) { next_poll = now + 1000; continue; }
                cursor %= paths.size();
                std::rotate(paths.begin(), paths.begin() + cursor, paths.end());
                cursor = (cursor + index::kFolderSizeBatch) % paths.size();
                // Remaining paths follow at once: automatic scans only wait
                // 150 ms for an index answer before starting.
                more = paths.size() > index::kFolderSizeBatch;
                if (more) paths.resize(index::kFolderSizeBatch);
                for (const auto& path : paths) {
                    const auto& entry = store.entries.at(path);
                    versions.emplace_back(entry.revision, entry.request_epoch);
                    store.entries.at(path).next_index_at = now + 30000;
                }
                if (!stopping && index_cancel) ResetEvent(index_cancel);
                request_scope = scope; ++stats.index_queries;
            }
            std::vector<index::IndexedFolderSize> values;
            const bool received = connection.FolderSizes(paths, values);
            {
                std::lock_guard lock(mutex);
                const auto answered = GetTickCount64();
                next_poll = answered + (received ? (more ? 0 : 1000) : 2000);
                if (stopping) continue;
                kick = true;  // an answer can grant or revoke index authority
                (void)request_scope;
                for (size_t i = 0; i < paths.size(); ++i) {
                    const auto found = store.entries.find(paths[i]);
                    const auto demand = wanted.find(paths[i]);
                    (void)demand;
                    if (found == store.entries.end() || manual.contains(paths[i])) continue;
                    auto& entry = found->second;
                    // Any answer other than an available total revokes index authority,
                    // even when the request itself has since been superseded.
                    if (!received || i >= values.size() || !values[i].available) entry.index_current = false;
                    if (entry.revision != versions[i].first || entry.request_epoch != versions[i].second ||
                        (entry.value.source == FolderSizeSource::Scan && entry.value.has_value &&
                         entry.value.state != FolderSizeState::Calculating)) continue;
                    if (received && i < values.size() && values[i].available) {
                        if (!entry.value.has_value || entry.value.bytes != values[i].bytes ||
                            entry.value.source != FolderSizeSource::Index) {
                            entry.value = {FolderSizeState::Indexed, values[i].bytes, true};
                            entry.value.source = FolderSizeSource::Index;
                            ++dirty_revision; changed = true;
                        }
                        entry.value.verified_at = folder_size::NowUtcMs();
                        stamps_dirty = true;
                        entry.index_current = true;
                    } else if (received && i < values.size()) {
                        // Totals may still be building in the service: ask again soon.
                        entry.next_index_at = (std::min)(entry.next_index_at, answered + 5000);
                    }
                    // An unavailable poll does not turn a retained estimate into a
                    // different list label. Its timestamp remains available in details.
                }
            }
            wake.notify_all();
        }
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    }
    void Publish(folder_size::Scan& job) {
        const auto root = store.entries.find(job.path);
        const auto demand = wanted.find(job.path);
        if (root == store.entries.end() || demand == wanted.end() || !demand->second ||
            root->second.revision != job.revision || root->second.request_epoch != job.request_epoch) {
            ++stats.jobs_cancelled; return;
        }
        const auto now = GetTickCount64();
        const auto utc = folder_size::NowUtcMs();
        bool saved_change = false;
        for (auto& subtree : job.completed) {
            const auto key = Key(subtree.path);
            auto* entry = store.Ensure(key, protected_paths);
            if (!entry) continue;
            if (!subtree.value.has_value && entry->value.has_value) {
                entry->value.state = FolderSizeState::Cached;
                entry->value.verified = false;
            } else {
                saved_change |= SavedFieldsDiffer(entry->value, subtree.value);
                entry->value = subtree.value;
                entry->value.verified_revision = entry->revision;
                entry->value.verified_at = subtree.value.has_value ? utc : 0;
                entry->watch_generation = subtree.watch_generation;
                entry->value.verified = subtree.value.has_value && !subtree.value.partial &&
                    subtree.watch_generation != 0 && Coverage(key) == subtree.watch_generation;
            }
            entry->completed = now;
            entry->work = {};
            entry->work.skipped = subtree.value.skipped; entry->work.issues = subtree.value.issues;
            const bool incomplete = !subtree.value.has_value || subtree.value.partial;
            entry->work.activity = incomplete ? FolderSizeActivity::Failed : FolderSizeActivity::Idle;
            // A containing scan may publish a useful child snapshot, but must
            // not back off an explicit child request still waiting for its turn.
            entry->not_before = incomplete && !(key != job.path && manual.contains(key))
                ? now + kFailureBackoffMs : 0;
            if (key == job.path) {
                manual.erase(key);
                if (std::none_of(last_visible.begin(), last_visible.end(), [&](const auto& r) { return r.path == key; })) {
                    wanted[key] = false;
                    protected_paths.erase(key);
                }
            }
        }
        ++stats.jobs_completed; changed = true;
        // The same totals found again only refresh their verified time.
        if (saved_change) ++dirty_revision;
        else if (!job.completed.empty()) stamps_dirty = true;
    }
    // What WriteValues stores, apart from the verified time.
    static bool SavedFieldsDiffer(const FolderSizeValue& saved, const FolderSizeValue& next) {
        if (saved.has_value != next.has_value) return true;
        if (!next.has_value) return false;
        return saved.bytes != next.bytes || saved.partial != next.partial || saved.source != next.source ||
            saved.skipped != next.skipped || saved.issues != next.issues;
    }
    // The earliest time something Run skipped becomes due without a notify:
    // backoffs, freshness, unwatched probe ageing, watch leases, the pending
    // save. `now` is the scheduling pass's clock, so a backoff that expired
    // since then is still seen. Capped so a case missed here costs a late
    // pass, not a lost one.
    uint64_t IdleDeadline(uint64_t now, uint64_t last_save, bool saving) const {
        uint64_t due = now + kProbeFreshMs;
        // Scheduling skipped rows already past these times for other reasons.
        const auto at = [&](uint64_t when) { if (when > now && when < due) due = when; };
        // The next pass always acts on these once expired, so overdue is due now.
        const auto by = [&](uint64_t when) { due = (std::min)(due, (std::max)(when, now)); };
        for (const auto& path : order) {
            const auto demand = wanted.find(path);
            const auto found = store.entries.find(path);
            if (demand == wanted.end() || !demand->second || found == store.entries.end()) continue;
            at(found->second.not_before);
            if (found->second.completed && !manual.contains(path)) at(found->second.completed + kFreshMs);
        }
        for (const auto& key : probe_wanted) {
            const auto found = probes.find(key);
            if (found == probes.end() || found->second.stale) continue;
            if (found->second.generation != 0 && Coverage(key) == found->second.generation) continue;
            by(found->second.at + kProbeFreshMs);
        }
        for (const auto& root : roots)
            if (const auto used = watch_used.find(root); used != watch_used.end()) by(used->second + kWatchLeaseMs);
        if (saving && dirty_revision != saved_revision) {
            if (now - last_save >= 3000) return now;
            at(last_save + 3000);
        }
        return due;
    }
    void Run() {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        const auto file = cache_path ? cache_path() : std::wstring();
        const auto cache_key = file.empty() ? std::wstring() : Key(file);
        const auto restored = folder_size::ReadValues(file);
        {
            std::lock_guard lock(mutex);
            for (const auto& [path, value] : restored) {
                auto* entry = store.Ensure(path, protected_paths);
                if (!entry || entry->value.source == FolderSizeSource::Scan) continue;
                if (!entry->value.has_value || value.source == FolderSizeSource::Scan) {
                    entry->value = value;
                    entry->completed = GetTickCount64();
                    if (value.partial) entry->not_before = entry->completed + kFailureBackoffMs;
                }
            }
            if (!restored.empty()) changed = true;
        }
        std::deque<std::unique_ptr<folder_size::Scan>> jobs;
        uint64_t last_save = GetTickCount64(), schedule_scope = 0;
        bool manual_turn = true;
        while (!stopping) {
            std::vector<std::pair<std::shared_ptr<Suspension>, std::shared_ptr<fs::DirWatch>>> retiring;
            {
                std::lock_guard lock(mutex);
                for (auto& [root, request] : suspended) {
                    if (!request->drained) {
                        for (const auto& [path, future] : retiring_watches)
                            if (Within(path, root)) request->watches.push_back(future);
                        std::erase_if(jobs, [&](const auto& job) { return Within(job->path, root); });
                        for (auto it = watches.begin(); it != watches.end();) {
                            if (!Within(it->first, root)) { ++it; continue; }
                            retiring.emplace_back(request, std::move(it->second.handle));
                            it = watches.erase(it);
                        }
                        request->drained = true;
                    }
                    if (retiring.empty() && !request->completed && std::all_of(request->watches.begin(), request->watches.end(), [](const auto& future) {
                        return !future.valid() || future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
                    })) { request->completed = true; request->ready.set_value(); }
                }
            }
            for (auto& [request, watch] : retiring) {
                auto completion = watch->StopAsync();
                std::lock_guard lock(mutex);
                retiring_watches.emplace_back(Key(watch->path()), completion);
                request->watches.push_back(std::move(completion));
            }
            { std::lock_guard lock(mutex); kick = false; }
            UpdateWatches(cache_key);
            const bool probing = RunProbes();
            std::vector<std::wstring> classify;
            {
                std::lock_guard lock(mutex);
                for (const auto& path : order)
                    if (wanted.at(path) && !manual.contains(path) && !Suspended(path)) classify.push_back(path);
            }
            for (const auto& path : classify) {
                const auto type = DriveType(path);
                if (type != DRIVE_FIXED && type != DRIVE_RAMDISK) {
                    std::lock_guard lock(mutex);
                    if (wanted.contains(path) && !manual.contains(path)) {
                        blocked_auto.insert(path); wanted[path] = false; changed = true;
                    }
                }
            }
            {
                std::unique_lock lock(mutex);
                std::erase_if(jobs, [&](const auto& job) {
                    const auto demand = wanted.find(job->path);
                    const auto entry = store.entries.find(job->path);
                    const bool cancelled = demand == wanted.end() || !demand->second || entry == store.entries.end() ||
                        entry->second.revision != job->revision || entry->second.request_epoch != job->request_epoch;
                    if (cancelled) {
                        ++stats.jobs_cancelled;
                        if (entry != store.entries.end() && entry->second.request_epoch == job->request_epoch)
                            entry->second.work = {};
                    }
                    return cancelled;
                });
                const auto now = GetTickCount64();
                const bool reprioritize = schedule_scope != scope;
                std::map<std::wstring, size_t> priority;
                if (reprioritize) for (size_t i = 0; i < order.size(); ++i) priority.emplace(order[i], manual.contains(order[i]) ? i : order.size() + i);
                for (const auto& path : order) {
                    if (jobs.size() == kScanConcurrency && !reprioritize) break;
                    if (!wanted.at(path) || Suspended(path)) continue;
                    auto& entry = store.entries.at(path);
                    const bool explicit_request = manual.contains(path);
                    if (!explicit_request && ExpensiveAutomaticPath(path)) {
                        entry.auto_deferred = true;
                        if (!entry.work.Running()) entry.work.activity = FolderSizeActivity::Deferred;
                    }
                    if (!explicit_request && entry.auto_deferred) continue;
                    if (now < entry.not_before || (!explicit_request && (Reusable(path, entry) || IndexCurrent(entry)))) continue;
                    if (!explicit_request && entry.completed && now - entry.completed < kFreshMs) continue;
                    if (std::any_of(jobs.begin(), jobs.end(), [&](const auto& job) {
                        return Within(path, job->path) || Within(job->path, path);
                    })) continue;
                    if (jobs.size() == kScanConcurrency) {
                        const auto lowest = std::max_element(jobs.begin(), jobs.end(), [&](const auto& a, const auto& b) {
                            return priority.at(a->path) < priority.at(b->path);
                        });
                        if (priority.at((*lowest)->path) <= priority.at(path)) continue;
                        jobs.erase(lowest); ++stats.jobs_cancelled;
                    }
                    entry.work = {};
                    entry.work.activity = FolderSizeActivity::Scanning;
                    entry.work.manual = explicit_request;
                    jobs.push_back(std::make_unique<folder_size::Scan>(path, entry.revision, entry.request_epoch, manual.contains(path)));
                    ++stats.jobs_started; changed = true;
                }
                if (reprioritize) {
                    schedule_scope = scope;
                    std::stable_sort(jobs.begin(), jobs.end(), [&](const auto& a, const auto& b) {
                        return priority.at(a->path) < priority.at(b->path);
                    });
                }
                // Give explicit clicks every other slice, without starving other
                // visible folders. Round-robin still applies among manual jobs.
                if (manual_turn) {
                    const auto urgent = std::find_if(jobs.begin(), jobs.end(), [&](const auto& job) {
                        return manual.contains(job->path);
                    });
                    if (urgent != jobs.end()) std::rotate(jobs.begin(), urgent, std::next(urgent));
                }
                if (jobs.empty() && !probing) {
                    const auto due = IdleDeadline(now, last_save, !file.empty());
                    const auto idle_now = GetTickCount64();
                    const bool releasing = std::any_of(suspended.begin(), suspended.end(), [](const auto& item) { return !item.second->completed; });
                    wake.wait_for(lock, std::chrono::milliseconds(releasing ? 10 : (due > idle_now ? due - idle_now : 0)),
                        [&] { return kick || stopping.load(); });
                }
            }
            if (!jobs.empty() && !stopping) {
                manual_turn = !manual_turn;
                auto job = std::move(jobs.front()); jobs.pop_front();
                const auto before_entries = job->entries_scanned, before_hits = job->subtree_hits;
                { std::lock_guard lock(mutex); active_scan_path = job->path; }
                const auto step_start = GetTickCount64();
                job->Step(stopping, [&](const std::wstring& path) -> std::optional<uint64_t> {
                    const auto key = Key(path);
                    std::lock_guard lock(mutex);
                    const auto found = store.entries.find(key);
                    if (found == store.entries.end() || !Reusable(key, found->second)) return std::nullopt;
                    store.Touch(found->second);
                    return found->second.value.bytes;
                }, [&](const std::wstring& path) -> uint64_t {
                    // Only this worker changes the watch map structure.
                    if (watches.empty()) return 0;
                    const auto key = Key(path);
                    std::lock_guard lock(mutex); return Coverage(key);
                });
                job->active_ms += GetTickCount64() - step_start;
                const bool completed = job->done;
                bool deferred = false;
                {
                    std::lock_guard lock(mutex);
                    active_scan_path.clear();
                    stats.entries_scanned += job->entries_scanned - before_entries;
                    stats.subtree_hits += job->subtree_hits - before_hits;
                    if (!completed && !manual.contains(job->path) &&
                        (job->active_ms >= 500 || job->entries_scanned >= 20000)) {
                        const auto found = store.entries.find(job->path);
                        if (found != store.entries.end() && found->second.request_epoch == job->request_epoch) {
                            found->second.auto_deferred = true;
                            found->second.work.activity = FolderSizeActivity::Deferred;
                            found->second.work.bytes = job->Progress().bytes;
                            found->second.work.entries = job->entries_scanned;
                            ++stats.deferred_jobs; changed = true;
                        }
                        deferred = true;
                    }
                    if (completed) Publish(*job);
                    else if (job->entries_scanned != 0 &&
                             GetTickCount64() - job->progress_reported_at >= 200) {
                        const auto found = store.entries.find(job->path);
                        const auto demand = wanted.find(job->path);
                        if (found != store.entries.end() && demand != wanted.end() && demand->second &&
                            found->second.revision == job->revision &&
                            found->second.request_epoch == job->request_epoch &&
                            found->second.work.Running()) {
                            found->second.work.bytes = job->Progress().bytes;
                            found->second.work.entries = job->entries_scanned;
                            changed = true;
                        }
                        job->progress_reported_at = GetTickCount64();
                    }
                }
                if (!completed && !deferred) {
                    jobs.push_back(std::move(job));
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(4), [&] { return stopping.load(); });
                }
            }
            if (GetTickCount64() - last_save >= 3000) { Save(file); last_save = GetTickCount64(); }
        }
        jobs.clear();
        std::vector<std::shared_ptr<fs::DirWatch>> retired;
        {
            std::lock_guard lock(mutex);
            for (auto& [path, watch] : watches) retired.push_back(std::move(watch.handle));
            watches.clear();
        }
        for (auto& watch : retired) watch->Stop();
        Save(file, true);
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    }
};

FolderSizes::FolderSizes() : impl_(std::make_unique<Impl>()) {}
FolderSizes::~FolderSizes() { Stop(); }

std::shared_future<void> FolderSizes::SuspendVolume(const std::wstring& root) {
    auto& state = *impl_;
    const auto key = Key(root);
    std::lock_guard lock(state.mutex);
    if (key.empty() || state.stopping) return {};
    if (const auto found = state.suspended.find(key); found != state.suspended.end()) return found->second->future;
    auto request = std::make_shared<Impl::Suspension>();
    state.suspended.emplace(key, request);
    for (auto& [path, entry] : state.store.entries) if (Within(path, key)) {
        ++entry.request_epoch;
        entry.value.verified = false;
        if (entry.value.has_value && entry.value.source != FolderSizeSource::Index) entry.value.state = FolderSizeState::Cached;
        entry.work = {};
    }
    if (state.worker.joinable() && Within(state.active_scan_path, key)) CancelSynchronousIo(state.worker.native_handle());
    if (!state.worker.joinable()) state.worker = std::thread([&state] { state.Run(); });
    state.changed = true; state.kick = true;
    state.wake.notify_all();
    return request->future;
}

void FolderSizes::ResumeVolume(const std::wstring& root) {
    std::vector<FolderSizeRequest> visible;
    std::vector<std::wstring> roots;
    {
        auto& state = *impl_;
        std::lock_guard lock(state.mutex);
        if (!state.suspended.erase(Key(root))) return;
        visible = state.last_visible; roots = state.last_roots;
        state.last_visible.clear(); state.last_roots.clear();
        state.kick = true;
        state.wake.notify_all();
    }
    Sync(std::move(visible), std::move(roots));
}
void FolderSizes::SetCachePath(std::function<std::wstring()> path) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->worker.joinable()) impl_->cache_path = std::move(path);
}
void FolderSizes::SetIndexEnabled(bool enabled) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->worker.joinable()) impl_->index_enabled = enabled;
}
void FolderSizes::Sync(std::vector<FolderSizeRequest> visible, std::vector<std::wstring> requested_roots) {
    std::vector<FolderSizeRequest> normalized;
    std::map<std::wstring, size_t> positions;
    for (const bool priority : {true, false}) for (const auto& request : visible) {
        if (request.visible != priority || request.path.empty() || fs::IsVirtualPath(request.path)) continue;
        const auto key = Key(request.path);
        const auto found = positions.find(key);
        if (found != positions.end()) { normalized[found->second].automatic |= request.automatic; continue; }
        if (normalized.size() == folder_size::kCacheLimit) continue;
        positions.emplace(key, normalized.size());
        normalized.push_back({key, request.automatic, priority});
    }
    for (auto& root : requested_roots) root = Key(root);
    requested_roots = MergeRoots(std::move(requested_roots));
    auto& state = *impl_;
    std::lock_guard lock(state.mutex);
    if (state.stopping) return;
    const auto same = [](const auto& a, const auto& b) {
        return a.path == b.path && a.automatic == b.automatic && a.visible == b.visible;
    };
    if (requested_roots == state.last_roots &&
        std::equal(normalized.begin(), normalized.end(), state.last_visible.begin(), state.last_visible.end(), same)) return;
    state.last_visible = normalized; state.last_roots = requested_roots;
    // Bounded recent watches survive transient loading/Up/Back subscriptions.
    // Never recursively watch a whole volume just because its overview is visible.
    const auto now = GetTickCount64();
    for (const auto& root : requested_roots) {
        if (state.Suspended(root) || VolumeRoot(root) || NetworkPath(root)) continue;
        const auto covering = std::find_if(state.roots.begin(), state.roots.end(), [&](const auto& r) { return Within(root, r); });
        if (covering != state.roots.end()) { state.watch_used[*covering] = now; continue; }
        if (state.roots.size() >= kWatchLimit) {
            const auto victim = std::min_element(state.roots.begin(), state.roots.end(), [&](const auto& a, const auto& b) {
                return state.watch_used[a] < state.watch_used[b];
            });
            state.watch_used.erase(*victim);
            state.roots.erase(victim);
        }
        state.roots.push_back(root); state.watch_used[root] = now;
    }
    std::set<std::wstring> protected_paths;
    for (const auto& request : normalized) protected_paths.insert(request.path);
    // Explicit tasks finish in the background even if their row scrolls away.
    for (const auto& path : state.manual) protected_paths.insert(path);
    bool cancelled = false;
    for (const auto& [path, automatic] : state.wanted) if (!protected_paths.contains(path)) {
        if (auto found = state.store.entries.find(path); found != state.store.entries.end()) {
            ++found->second.request_epoch;
            found->second.work = {};
        }
        cancelled |= automatic;
    }
    std::erase_if(state.blocked_auto, [&](const auto& path) { return !protected_paths.contains(path); });
    std::map<std::wstring, bool> wanted;
    std::vector<std::wstring> order;
    for (const auto& request : normalized) {
        auto* entry = state.store.Ensure(request.path, protected_paths);
        if (!entry) continue;
        // Visibility is not a data revision. Preserve published bytes and completion age.
        if (!entry->value.has_value && entry->completed == 0 && !entry->work.Running())
            entry->not_before = (std::max)(entry->not_before, now + 150);
        wanted[request.path] = !state.Suspended(request.path) && ((request.automatic && !NetworkPath(request.path) &&
            !state.blocked_auto.contains(request.path)) || state.manual.contains(request.path));
        order.push_back(request.path);
    }
    for (const auto& path : state.manual) if (!wanted.contains(path)) {
        wanted[path] = true; order.insert(order.begin(), path);
    }
    state.protected_paths = std::move(protected_paths);
    state.wanted = std::move(wanted); state.order = std::move(order); ++state.scope;
    // Visibility changes do not cancel a useful in-flight metadata read.
    // The worker may already run for emptiness probes; the index starts with size work.
    if (!state.wanted.empty()) {
        if (!state.worker.joinable()) state.worker = std::thread([&state] { state.Run(); });
        if (state.index_enabled && !state.index_worker.joinable())
            state.index_worker = std::thread([&state] { state.RunIndex(); });
    }
    (void)cancelled; // Worker rejects obsolete auto jobs between bounded slices.
    state.kick = true;
    state.wake.notify_all();
}
void FolderSizes::Calculate(const std::wstring& path) {
    auto& state = *impl_;
    const auto key = Key(path);
    std::lock_guard lock(state.mutex);
    const auto demand = state.wanted.find(key);
    if (demand == state.wanted.end() || state.Suspended(key)) return;
    demand->second = true; state.manual.insert(key);
    state.store.InvalidateAncestors(key, GetTickCount64(), 0);
    if (auto found = state.store.entries.find(key); found != state.store.entries.end()) {
        found->second.not_before = 0; found->second.auto_deferred = false;
        found->second.work = {};
        found->second.work.activity = FolderSizeActivity::Queued;
        found->second.work.manual = true;
    }
    ++state.scope; state.changed = true; state.kick = true;
    if (state.index_cancel) SetEvent(state.index_cancel);
    state.wake.notify_all();
}
void FolderSizes::Cancel(const std::wstring& path) {
    auto& state = *impl_;
    const auto key = Key(path);
    std::lock_guard lock(state.mutex);
    if (auto found = state.store.entries.find(key); found != state.store.entries.end()) {
        ++found->second.request_epoch;
        found->second.work = {};
        found->second.work.activity = FolderSizeActivity::Cancelled;
        found->second.auto_deferred = true;
    }
    state.manual.erase(key); ++state.scope; state.changed = true; state.kick = true;
    if (state.worker.joinable() && state.active_scan_path == key) CancelSynchronousIo(state.worker.native_handle());
    state.wake.notify_all();
}
FolderSizeWork FolderSizes::GetWork(const std::wstring& path) const {
    const auto key = Key(path);
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->store.entries.find(key);
    auto work = found == impl_->store.entries.end() ? FolderSizeWork{} : found->second.work;
    // A watch revision can invalidate the current scan, but not the user's
    // outstanding request. It remains queued until completion/cancellation.
    if (impl_->manual.contains(key) && !work.Running()) {
        work.activity = FolderSizeActivity::Queued; work.manual = true;
    } else if (found != impl_->store.entries.end() && found->second.auto_deferred && work.activity == FolderSizeActivity::Idle) {
        work.activity = FolderSizeActivity::Deferred;
    }
    return work;
}
FolderSizeValue FolderSizes::Get(const std::wstring& path) const {
    const auto key = Key(path);
    auto& state = *impl_;
    std::lock_guard lock(state.mutex);
    const auto found = state.store.entries.find(key);
    auto value = found == state.store.entries.end() ? FolderSizeValue{} : found->second.value;
    if (found != state.store.entries.end() && value.source == FolderSizeSource::Scan &&
        !state.Reusable(key, found->second) &&
        (value.verified || (found->second.completed && GetTickCount64() - found->second.completed >= kFreshMs))) {
        value.verified = false;
        if (value.has_value) value.state = FolderSizeState::Cached;
    }
    return value;
}
std::unordered_map<std::wstring, uint64_t> FolderSizes::KnownChildren(const std::wstring& parent) const {
    std::unordered_map<std::wstring, uint64_t> children;
    auto prefix = Key(parent);
    if (prefix.empty() || fs::IsVirtualPath(prefix)) return children;
    if (prefix.back() != L'\\') prefix += L'\\';
    auto& state = *impl_;
    std::lock_guard lock(state.mutex);
    for (auto found = state.store.entries.lower_bound(prefix); found != state.store.entries.end() && found->first.starts_with(prefix); ++found) {
        if (!found->second.value.has_value) continue;
        std::wstring_view rest(found->first); rest.remove_prefix(prefix.size());
        if (rest.empty() || rest.find(L'\\') != std::wstring_view::npos) continue;
        children.emplace(std::wstring(rest), found->second.value.bytes);
    }
    return children;
}
void FolderSizes::Invalidate(const std::wstring& path) { impl_->Invalidate(path); }
void FolderSizes::Probe(std::vector<std::wstring> paths) {
    std::vector<std::wstring> keys;
    std::set<std::wstring> seen;
    for (const auto& path : paths) {
        if (keys.size() == kProbeLimit) break;
        if (path.empty() || NetworkPath(path)) continue;
        auto key = Key(path);
        if (seen.insert(key).second) keys.push_back(std::move(key));
    }
    auto& state = *impl_;
    std::lock_guard lock(state.mutex);
    if (state.stopping) return;
    const auto now = GetTickCount64();
    const bool work = std::any_of(keys.begin(), keys.end(), [&](const auto& key) {
        const auto found = state.probes.find(key);
        return found == state.probes.end() || !state.ProbeFresh(key, found->second, now);
    });
    state.probe_wanted = std::move(keys);
    if (state.probes.size() > kProbeCacheLimit)
        std::erase_if(state.probes, [&](const auto& item) { return !seen.contains(item.first); });
    if (!work) return;
    if (!state.worker.joinable()) state.worker = std::thread([&state] { state.Run(); });
    state.kick = true;
    state.wake.notify_all();
}
uint8_t FolderSizes::Content(const std::wstring& path) const {
    const auto key = Key(path);
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->probes.find(key);
    return found == impl_->probes.end() ? 0 : found->second.bits;
}
bool FolderLooksEmpty(uint8_t content, bool show_hidden, bool show_protected) {
    if (!(content & FolderContentKnown) || (content & FolderContentVisible)) return false;
    if (show_hidden && (content & FolderContentHidden)) return false;
    return !(show_hidden && show_protected && (content & FolderContentSystem));
}
bool FolderSizes::TakeChanged() { return impl_->changed.exchange(false); }
void FolderSizes::SetChangeNotify(HWND hwnd, UINT message) {
    impl_->changed.message.store(message);
    impl_->changed.hwnd.store(hwnd);
}
FolderSizeStats FolderSizes::ReadStats() const {
    std::lock_guard lock(impl_->mutex);
    auto stats = impl_->stats; stats.cache_items = impl_->store.entries.size();
    stats.active_watches = impl_->watches.size(); return stats;
}
void FolderSizes::Stop() {
    auto& state = *impl_;
    {
        std::lock_guard lock(state.mutex);
        state.stopping = true;
        if (state.index_cancel) SetEvent(state.index_cancel);
        // The worker takes this mutex before its final save. Issue cancellation
        // first so shutdown cannot cancel the cache write it is waiting for.
        if (state.worker.joinable()) CancelSynchronousIo(state.worker.native_handle());
    }
    state.wake.notify_all();
    if (state.worker.joinable()) state.worker.join();
    if (state.index_worker.joinable()) { CancelSynchronousIo(state.index_worker.native_handle()); state.index_worker.join(); }
}
} // namespace pulse::app

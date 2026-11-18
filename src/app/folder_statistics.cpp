#include "folder_statistics.h"
#include "../fs/bounded_enumeration.h"
#include "../fs/cloud_state.h"
#include "../fs/drive_kind.h"
#include "../fs/fs_enum.h"
#include "../fs/fs_watch.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace pulse::app {
namespace {
FolderStatistics ScanFolder(const std::wstring& root, const std::atomic_bool& cancel) {
    FolderStatistics result;
    std::vector<std::wstring> stack{root};
    while (!stack.empty()) {
        if (cancel) throw fs::EnumerationCancelled();
        auto path = std::move(stack.back());
        stack.pop_back();
        const DWORD attrs = GetFileAttributesW(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || fs::IsCloudPlaceholder(attrs)) {
            result.partial = true;
            continue;
        }
        if (!path.empty() && path.back() != L'\\') path += L'\\';
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileExW((path + L"*").c_str(), FindExInfoBasic, &data,
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) result.partial = true;
            continue;
        }
        struct Guard { HANDLE handle; ~Guard() { FindClose(handle); } } guard{find};
        do {
            if (cancel) throw fs::EnumerationCancelled();
            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                ++result.directories;
                if (fs::IsCloudPlaceholder(data.dwFileAttributes)) result.partial = true;
                else if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    stack.push_back(path + data.cFileName);
            } else {
                ++result.files;
                result.bytes += (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                if (fs::IsCloudPlaceholder(data.dwFileAttributes)) result.partial = true;
            }
        } while (FindNextFileW(find, &data));
        if (GetLastError() != ERROR_NO_MORE_FILES) result.partial = true;
    }
    return result;
}
}

struct FolderStatisticsService::Impl {
    struct Signals {
        std::condition_variable changed;
        std::atomic<uint64_t> revision{1};
        std::atomic_bool replaced{false};
        std::atomic_bool root_armed_callback{false}, parent_armed_callback{false};
    };
    struct Entry {
        std::wstring path;
        std::mutex mutex;
        std::shared_ptr<Signals> signals = std::make_shared<Signals>();
        fs::DirWatch watch;
        fs::DirWatch parent_watch;
        bool needs_parent = false;
        std::atomic_bool cancel{false};
        size_t subscribers = 0;
        bool running = false, available = false;
        uint64_t result_revision = 0, completed_at = 0;
        FolderStatistics result;
        ~Entry() { cancel = true; watch.StopAsync(); parent_watch.StopAsync(); }
    };
    std::mutex mutex;
    std::vector<std::shared_ptr<Entry>> entries;
    Scan scan;
    std::atomic<uint64_t> scans{0}, versions{0};
    std::atomic_bool stopping{false};

    static void Arm(const std::shared_ptr<Entry>& entry) {
        std::weak_ptr<Signals> weak = entry->signals;
        entry->watch.StopAsync();
        // StopAsync quiesces callbacks before returning, so an old watch
        // cannot set this acknowledgement after it is reset for the new one.
        entry->signals->root_armed_callback = false;
        entry->watch.Start(entry->path, [weak](bool overflow, std::vector<fs::DirNotifyEvent> events) {
            if (auto current = weak.lock()) {
                ++current->revision;
                if (fs::ArmedAt(overflow, events)) current->root_armed_callback = true;
                current->changed.notify_all();
            }
        }, true, true);
    }
    static bool Covered(const Entry& entry) {
        return entry.watch.Armed() && entry.signals->root_armed_callback &&
            (!entry.needs_parent || (entry.parent_watch.Armed() && entry.signals->parent_armed_callback)) &&
            !entry.signals->replaced;
    }

    std::shared_ptr<Entry> Get(const std::wstring& input) {
        auto path = fs::NormalizePath(input);
        while (path.size() > 7 && path.back() == L'\\') path.pop_back();
        std::lock_guard lock(mutex);
        for (const auto& entry : entries)
            if (entry->path == path)
                return entry;
        if (entries.size() >= 16) {
            const auto removable = std::find_if(entries.begin(), entries.end(), [](const auto& entry) {
                std::lock_guard entry_lock(entry->mutex);
                return !entry->running && entry->subscribers == 0;
            });
            if (removable == entries.end()) throw std::runtime_error("Folder statistics capacity exhausted");
            entries.erase(removable);
        }
        auto entry = std::make_shared<Entry>();
        entry->path = std::move(path);
        Arm(entry);
        const auto slash = entry->path.find_last_of(L'\\');
        if (slash != std::wstring::npos && slash + 1 < entry->path.size()) {
            entry->needs_parent = true;
            const auto leaf = entry->path.substr(slash + 1);
            std::weak_ptr<Signals> weak = entry->signals;
            entry->parent_watch.Start(entry->path.substr(0, slash + 1),
                [weak, leaf](bool overflow, std::vector<fs::DirNotifyEvent> events) {
                    if (auto current = weak.lock()) {
                        const bool armed = fs::ArmedAt(overflow, events) != 0;
                        const bool replacement = !armed && (overflow || std::any_of(events.begin(), events.end(),
                            [&](const auto& event) {
                                return CompareStringOrdinal(event.name.c_str(), -1, leaf.c_str(), -1, TRUE) == CSTR_EQUAL ||
                                    CompareStringOrdinal(event.old_name.c_str(), -1, leaf.c_str(), -1, TRUE) == CSTR_EQUAL;
                            }));
                        if (replacement || armed) {
                            if (replacement) current->replaced = true;
                            ++current->revision;
                            if (armed) current->parent_armed_callback = true;
                            current->changed.notify_all();
                        }
                    }
                }, false, true);
        }
        entries.push_back(entry);
        return entry;
    }

    void Start(const std::shared_ptr<Entry>& entry, const std::shared_ptr<Impl>& self) {
        entry->running = true;
        entry->cancel = false;
        try {
            std::thread([entry, self] {
                FolderStatistics result;
                bool succeeded = false;
                uint64_t revision = 0;
                bool covered = false;
                try {
                    {
                        std::lock_guard lock(entry->mutex);
                        if (entry->signals->replaced.exchange(false)) Arm(entry);
                    }
                    {
                        std::unique_lock lock(entry->mutex);
                        // DirWatch wakes WaitForStartingWatch before its armed
                        // callback. Wait for both callbacks within one bounded
                        // budget so their initial revisions precede the scan.
                        const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(fs::IsUncPath(entry->path) ? 150 : 20);
                        while (!entry->signals->root_armed_callback ||
                               (entry->needs_parent && !entry->signals->parent_armed_callback)) {
                            if (self->stopping || entry->cancel) throw fs::EnumerationCancelled();
                            if (std::chrono::steady_clock::now() >= deadline) break;
                            entry->signals->changed.wait_until(lock, (std::min)(deadline,
                                std::chrono::steady_clock::now() + std::chrono::milliseconds(5)));
                        }
                        if (self->stopping || entry->cancel) throw fs::EnumerationCancelled();
                        covered = Covered(*entry);
                        revision = entry->signals->revision.load();
                    }
                    ++self->scans;
                    result = fs::RunBoundedEnumeration<FolderStatistics>(
                        fs::IsUncPath(entry->path) || fs::IsRemoteLikeDrive(entry->path),
                        [path = entry->path, scan = self->scan](const std::atomic_bool& cancel) {
                            return scan(path, cancel);
                        }, [&] { return self->stopping || entry->cancel.load(); }, std::chrono::hours(24));
                    succeeded = true;
                } catch (...) {
                    result.partial = true;
                    succeeded = !self->stopping && !entry->cancel;
                }
                {
                    std::lock_guard lock(entry->mutex);
                    entry->running = false;
                    if (succeeded && !entry->cancel) {
                        result.verified = covered && Covered(*entry) &&
                            revision == entry->signals->revision.load() && !result.partial;
                        result.version = ++self->versions;
                        entry->result = result;
                        entry->result_revision = revision;
                        entry->completed_at = GetTickCount64();
                        entry->available = true;
                    }
                }
                entry->signals->changed.notify_all();
            }).detach();
        } catch (...) { entry->running = false; throw; }
    }
};

FolderStatisticsService::FolderStatisticsService(Scan scan) : impl_(std::make_shared<Impl>()) {
    impl_->scan = scan ? std::move(scan) : ScanFolder;
}
FolderStatisticsService::~FolderStatisticsService() {
    impl_->stopping = true;
    std::lock_guard lock(impl_->mutex);
    for (const auto& entry : impl_->entries) { entry->cancel = true; entry->signals->changed.notify_all(); }
    impl_->entries.clear();
}
FolderStatistics FolderStatisticsService::Read(const std::wstring& path,
        const std::function<bool()>& cancelled, uint64_t after_version) {
    if (cancelled()) throw fs::EnumerationCancelled();
    const auto self = impl_;
    const auto entry = self->Get(path);
    std::unique_lock lock(entry->mutex);
    ++entry->subscribers;
    const auto observed_version = entry->result.version;
    struct Subscription {
        Impl::Entry& entry;
        ~Subscription() { if (--entry.subscribers == 0 && entry.running) entry.cancel = true; }
    } subscription{*entry};
    for (;;) {
        if (cancelled() || self->stopping) throw fs::EnumerationCancelled();
        const bool current = entry->available && entry->result_revision == entry->signals->revision.load();
        const bool reusable = current && entry->result.verified && Impl::Covered(*entry);
        // A just-completed partial/unwatched scan can be delivered to its
        // subscribers but never reused by a later A -> B -> A request.
        if (current && entry->result.version != after_version &&
            (reusable || entry->result.version != observed_version))
            return entry->result;
        if (reusable && entry->result.version == after_version) {
            entry->signals->changed.wait_for(lock, std::chrono::milliseconds(20));
            continue;
        }
        if (!entry->running) {
            if (after_version != 0 && current && GetTickCount64() - entry->completed_at < 2000) {
                entry->signals->changed.wait_for(lock, std::chrono::milliseconds(20));
                continue;
            }
            const auto before = entry->result.version;
            self->Start(entry, self);
            while (entry->running) {
                if (cancelled() || self->stopping) throw fs::EnumerationCancelled();
                entry->signals->changed.wait_for(lock, std::chrono::milliseconds(20));
            }
            if (entry->available && entry->result.version != before &&
                entry->result_revision == entry->signals->revision.load()) return entry->result;
        } else entry->signals->changed.wait_for(lock, std::chrono::milliseconds(20));
    }
}
uint64_t FolderStatisticsService::ScansStarted() const { return impl_->scans; }
FolderStatisticsService& SharedFolderStatistics() {
    static FolderStatisticsService service;
    return service;
}
}

#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <future>

namespace pulse::app {
enum class FolderSizeState { Manual, Calculating, Ready, Updating, Partial, Unavailable, Cached, Indexed };
enum class FolderSizeSource { Unknown, Scan, Index };
enum class FolderSizeActivity { Idle, Queued, Scanning, Deferred, Failed, Cancelled };
enum FolderSizeIssue : uint32_t { SizeAccessDenied = 1, SizeOffline = 2, SizeLink = 4, SizeIoError = 8 };
struct FolderSizeWork {
    FolderSizeActivity activity = FolderSizeActivity::Idle;
    uint64_t bytes = 0, entries = 0, skipped = 0;
    uint32_t issues = 0;
    bool manual = false;
    bool Running() const { return activity == FolderSizeActivity::Queued || activity == FolderSizeActivity::Scanning; }
};
struct FolderSizeValue {
    FolderSizeState state = FolderSizeState::Manual;
    uint64_t bytes = 0;
    bool has_value = false;
    // A lower bound remains incomplete while updating or restored from cache.
    bool partial = false;
    FolderSizeSource source = FolderSizeSource::Unknown;
    // Only a complete scan protected by an uninterrupted watch is reusable.
    bool verified = false;
    uint64_t verified_revision = 0;
    uint64_t verified_at = 0; // Unix milliseconds of the completed scan.
    uint64_t skipped = 0;
    uint32_t issues = 0;
};
struct FolderSizeRequest {
    std::wstring path;
    bool automatic = true;
    bool visible = true;
};
// What a cheap first-entries probe saw inside a folder, for the empty-folder look.
// It never scans subtrees and is kept apart from size results.
enum FolderContentBits : uint8_t {
    FolderContentKnown = 1,    // probe completed; without it nothing is claimed
    FolderContentVisible = 2,  // a child without FILE_ATTRIBUTE_HIDDEN
    FolderContentHidden = 4,   // a hidden, non-system child
    FolderContentSystem = 8,   // a hidden + system child
};
// True only for a probed folder whose children are all filtered out by the
// listing's visibility rule (Tab::AllowsAttributes). Unknown never looks empty.
bool FolderLooksEmpty(uint8_t content, bool show_hidden, bool show_protected);
struct FolderSizeStats {
    uint64_t jobs_started = 0, jobs_completed = 0, jobs_cancelled = 0;
    uint64_t entries_scanned = 0, subtree_hits = 0, index_queries = 0;
    uint64_t watch_gaps = 0, cache_items = 0;
    uint64_t deferred_jobs = 0, active_watches = 0;
};

// Window-thread methods only exchange in-memory state. Enumeration, watches and
// cache I/O belong to the background worker, including network paths.
class FolderSizes {
public:
    FolderSizes();
    ~FolderSizes();
    FolderSizes(const FolderSizes&) = delete;
    FolderSizes& operator=(const FolderSizes&) = delete;
    void SetCachePath(std::function<std::wstring()> path);
    void SetIndexEnabled(bool enabled);
    void Sync(std::vector<FolderSizeRequest> visible, std::vector<std::wstring> watch_roots);
    void Calculate(const std::wstring& path);
    void Cancel(const std::wstring& path);
    FolderSizeWork GetWork(const std::wstring& path) const;
    FolderSizeValue Get(const std::wstring& path) const;
    // Known totals of the direct subfolders of `parent`, keyed by lower-cased
    // name: what a Size sort of that folder can use right now (#58).
    std::unordered_map<std::wstring, uint64_t> KnownChildren(const std::wstring& parent) const;
    void Invalidate(const std::wstring& path);
    // Replaces the folders whose emptiness the UI wants (visible rows only).
    // Network, removable, offline, cloud and reparse folders stay unknown.
    void Probe(std::vector<std::wstring> paths);
    uint8_t Content(const std::wstring& path) const; // FolderContentBits, 0 = unknown
    bool TakeChanged();
    // The first change after each TakeChanged also posts `message` to `hwnd`.
    void SetChangeNotify(HWND hwnd, UINT message);
    FolderSizeStats ReadStats() const;
    void Stop();
    // UI request only. Wait/get off-thread before eject; timeout must abort eject.
    std::shared_future<void> SuspendVolume(const std::wstring& root);
    void ResumeVolume(const std::wstring& root);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

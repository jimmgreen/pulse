#pragma once
#include "folder_sizes.h"
#include <windows.h>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace pulse::app::folder_size {
struct CompletedSubtree {
    std::wstring path;
    FolderSizeValue value;
    uint64_t watch_generation = 0;
};
// What a listed subdirectory means for a size walk.
enum class Descend { Yes, Link, Offline };
// Name surrogates (junctions, symbolic links, mount points) point elsewhere and
// are never followed. Other reparse tags (OneDrive/cloud folders, dedup, ...)
// are the directory itself and are walked. Folders whose listing is not on this
// device (offline / recall-on-open) are only read for an explicit request.
Descend ClassifyDirectory(DWORD attrs, DWORD reparse_tag, bool manual);
class Scan {
public:
    using Lookup = std::function<std::optional<uint64_t>(const std::wstring&)>;
    using Coverage = std::function<uint64_t(const std::wstring&)>;
    Scan(std::wstring path, uint64_t revision, uint64_t request_epoch, bool manual = false);
    ~Scan();
    Scan(const Scan&) = delete;
    Scan& operator=(const Scan&) = delete;
    void Step(const std::atomic<bool>& stopping, const Lookup& lookup, const Coverage& coverage);
    FolderSizeValue Progress() const;
    uint64_t progress_reported_at = 0;
    std::wstring path;
    uint64_t revision = 0, request_epoch = 0;
    // Position in the coordinator's change log when the walk started: later
    // changes inside a finished subtree keep it from being published.
    uint64_t change_mark = 0;
    uint64_t entries_scanned = 0, subtree_hits = 0, active_ms = 0;
    FolderSizeValue result{FolderSizeState::Calculating};
    std::deque<CompletedSubtree> completed;
    bool done = false;
    bool manual() const { return manual_; }
private:
    struct Frame {
        std::wstring path;
        // Open only while this directory's own listing is read; closed before
        // its subdirectories are visited, so a walk holds one handle at most.
        HANDLE dir = INVALID_HANDLE_VALUE;
        HANDLE find = INVALID_HANDLE_VALUE; // FindFirstFileEx fallback
        WIN32_FIND_DATAW data{};
        std::vector<std::wstring> children; // subdirectory names still to visit
        size_t next_child = 0;
        uint64_t bytes = 0, watch_generation = 0, skipped = 0;
        uint32_t issues = 0;
        bool started = false, listed = false, partial = false, read_any = false, queried = false;
    };
    bool manual_ = false;
    std::vector<Frame> stack_;
    std::unique_ptr<uint64_t[]> buffer_; // 64 KiB, 8-byte aligned directory records
    bool Open(Frame& frame);
    unsigned ReadListing(Frame& frame);
    unsigned ReadFindListing(Frame& frame);
    void AddEntry(Frame& frame, const wchar_t* name, size_t length, DWORD attrs, DWORD tag, uint64_t bytes);
    void Skip(Frame& frame, uint32_t issue);
    void FinishFrame();
};
} // namespace pulse::app::folder_size

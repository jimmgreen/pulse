#pragma once

#include "folder_picker_model.h"
#include "../app/media_props.h"

namespace pulse::ui {
// Reuse the main window's versioned metadata cache; painting never opens files.
// The in-process picker pins its module before any worker can be started.
class PickerImageDimensions {
public:
    ~PickerImageDimensions() { Reset(); }
    void Reset() {
        app::media::Cancel(this);
        snapshot_.reset();
        rows_.clear();
        active_ = false;
    }
    void Bind(const std::shared_ptr<const std::vector<fs::DirEntry>>& snapshot,
              const std::vector<PickerEntry>& entries) {
        if (snapshot_ == snapshot) return;
        Reset();
        snapshot_ = snapshot;
        if (!snapshot || snapshot->size() != entries.size()) return;
        rows_.resize(entries.size());
        std::vector<app::media::Job> pending;
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            const auto& file = (*snapshot)[i];
            if (file.attrs & FILE_ATTRIBUTE_DIRECTORY ||
                app::media::KindOfName(entry.name) != app::media::Kind::Photo) continue;
            active_ = true;
            // As in the main media columns, do not turn a network/Shell listing
            // into extra provider calls. Offline placeholders are skipped by ReadProps.
            const auto& path = entry.path;
            const bool local = (path.size() > 2 && path[1] == L':') ||
                (path.size() > 6 && path.rfind(L"\\\\?\\", 0) == 0 && path[5] == L':');
            if (!local) continue;
            auto& job = rows_[i];
            job.path = path;
            job.mtime = (uint64_t{file.mtime.dwHighDateTime} << 32) | file.mtime.dwLowDateTime;
            job.size = file.size;
            job.attrs = file.attrs;
            job.kind = app::media::Kind::Photo;
            pending.push_back(job);
        }
        revision_ = app::media::Revision();
        if (!pending.empty()) app::media::Request(this, std::move(pending));
    }
    bool Active() const { return active_; }
    bool Changed() {
        const auto revision = app::media::Revision();
        if (!active_ || revision == revision_) return false;
        revision_ = revision;
        return true;
    }
    std::wstring Cell(int source) const {
        if (source < 0 || static_cast<size_t>(source) >= rows_.size()) return {};
        const auto& row = rows_[static_cast<size_t>(source)];
        if (row.path.empty()) return {};
        const auto props = app::media::Find(row.path, row.mtime, row.size);
        return props ? app::media::FormatDimensions(props->width, props->height) : std::wstring();
    }
private:
    std::shared_ptr<const std::vector<fs::DirEntry>> snapshot_;
    std::vector<app::media::Job> rows_;
    uint64_t revision_ = 0;
    bool active_ = false;
};
} // namespace pulse::ui

#pragma once
#include <windows.h>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace pulse::ui {
struct QuickPreviewItem {
    std::wstring path, name;
    DWORD attrs = 0;
    uint64_t modified = 0, size = 0;
    bool starred = false;
    bool read_only = false;
    // 1-based place in the listing and its length; 0 when unknown.
    int position = 0, count = 0;
};

enum class QuickPreviewAction : int {
    None = 0, Open, Cut, Copy, CopyPath, ToggleStar, Rename, Delete, Properties,
    InstallMediaPack,   // start (or cancel) the FFmpeg preview pack download
    InstallImagePack,   // start (or cancel) the image preview pack download
    DownloadCloud,      // 下载并预览 on the cloud-only card
    OpenInNewTab,       // the previewed folder, in a new tab
    SaveOptions,        // the window changed QuickPreviewOptions itself (size menu, L, a drag)
};

// Quick Look behaviour from Settings > 快速预览 (app prefs preview_*). The
// window changes some of it itself and asks the owner to save it with
// QuickPreviewAction::SaveOptions; the owner reads QuickPreviewWindow::options().
struct QuickPreviewOptions {
    int size_mode = 0;              // PreviewSizeMode / app::kPreviewSize*
    bool remember_size = true;      // a dragged size is used next time
    RECT remembered{};              // that size (screen px; empty = none)
    bool space_closes = true;       // false: Space plays / pauses videos and animations
    int loop = 0;                   // app::kPreviewLoop*: 0 animations, 1 all, 2 none
    int key_hint_left = 3;          // how many more videos show the key hint
    bool operator==(const QuickPreviewOptions& other) const {
        return size_mode == other.size_mode && remember_size == other.remember_size &&
               EqualRect(&remembered, &other.remembered) && space_closes == other.space_closes &&
               loop == other.loop && key_hint_left == other.key_hint_left;
    }
};

// 下载并预览 state for the item Quick Look shows (C4b).
struct CloudDownloadOffer {
    std::wstring path;          // the item this describes
    int percent = -1;           // 0..100 while it downloads
    std::wstring provider;      // sidebar label of its cloud root; empty: generic
    bool operator==(const CloudDownloadOffer&) const = default;
};

// A preview pack (FFmpeg or images) as offered on Quick Look's "can't decode"
// cards.
struct MediaPackOffer {
    bool installable = false;   // published for this build and not on this PC
    bool installing = false;
    float progress = 0.0f;      // 0..1 while installing
    uint64_t download_bytes = 0;
    std::wstring notice;        // why the last attempt failed; empty otherwise
    bool operator==(const MediaPackOffer&) const = default;
};

inline bool CanApplyPreviewFileAction(bool folder_listing, bool selected_archive_entry) {
    return folder_listing || !selected_archive_entry;
}

struct QuickPreviewCommand {
    QuickPreviewAction action = QuickPreviewAction::None;
    QuickPreviewItem target;
    bool shift = false;
    bool folder_child = false;
};

// Window messages carry a token, never a borrowed selection or heap pointer.
class QuickPreviewCommands {
public:
    UINT_PTR Push(QuickPreviewCommand command) {
        if (pending_.size() >= 32) return 0;
        do { ++next_; } while (!next_ || pending_.contains(next_));
        pending_.emplace(next_, std::move(command));
        return next_;
    }
    bool Take(UINT_PTR token, QuickPreviewCommand& command) {
        const auto it = pending_.find(token);
        if (it == pending_.end()) return false;
        command = std::move(it->second);
        pending_.erase(it);
        return true;
    }
    void Clear() { pending_.clear(); }
private:
    UINT_PTR next_ = 0;
    std::map<UINT_PTR, QuickPreviewCommand> pending_;
};

inline QuickPreviewItem FolderPreviewTarget(const QuickPreviewItem& folder,
                                          const std::wstring& relative, bool directory) {
    if (relative.empty()) return folder;
    // Only paths assembled from listing components may become file-operation targets.
    size_t start = 0;
    while (start < relative.size()) {
        const size_t end = relative.find_first_of(L"\\/", start);
        const auto part = relative.substr(start, end == std::wstring::npos ? end : end - start);
        if (part.empty() || part == L"." || part == L".." || part.find(L':') != std::wstring::npos)
            return {};
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    QuickPreviewItem target;
    target.path = folder.path;
    if (!target.path.empty() && target.path.back() != L'\\') target.path += L'\\';
    target.path += relative;
    target.name = relative.substr(relative.find_last_of(L"\\/") + 1);
    target.attrs = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    target.read_only = folder.read_only;
    return target;
}
} // namespace pulse::ui

#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <memory>
#include <string_view>
#include <vector>
#include "../common/preview_extensions.h"

namespace pulse::ui {
// 0 other, 1 picture (RAW and PSD included), 2 video: the kinds Quick Look's
// photo viewer and its filmstrip filter know. Works on a name or a path.
inline int QuickPreviewMediaKind(std::wstring_view name) {
    const size_t dot = name.find_last_of(L'.');
    const size_t slash = name.find_last_of(L"\\/");
    if (dot == std::wstring_view::npos || (slash != std::wstring_view::npos && dot < slash) ||
        name.size() - dot > 7)
        return 0;
    wchar_t buffer[8]{};
    size_t n = 0;
    for (size_t i = dot; i < name.size() && n < 7; ++i) {
        wchar_t c = name[i];
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        buffer[n++] = c;
    }
    const std::wstring_view ext(buffer, n);
    using pulse::preview::PreviewFamily;
    if (pulse::preview::IsImageExtension(ext) || pulse::preview::IsFamilyExtension(PreviewFamily::Raw, ext) ||
        pulse::preview::IsFamilyExtension(PreviewFamily::Psd, ext))
        return 1;
    return pulse::preview::IsVideoExtension(ext) ? 2 : 0;
}

// The listing around the previewed item, for the photo viewer's filmstrip and
// its filtered prev / next. kinds has one QuickPreviewMediaKind per view row;
// entries describe rows [first, first + entries.size()).
struct QuickPreviewGalleryEntry {
    std::wstring path;
    DWORD attrs = 0;
    uint64_t modified = 0, size = 0;
};
struct QuickPreviewGallery {
    std::vector<uint8_t> kinds;
    int current = -1;   // view row of the previewed item
    int first = 0;
    std::vector<QuickPreviewGalleryEntry> entries;
};

struct QuickPreviewItem {
    std::wstring path, name;
    DWORD attrs = 0;
    uint64_t modified = 0, size = 0;
    bool starred = false;
    bool read_only = false;
    // 1-based place in the listing and its length; 0 when unknown.
    int position = 0, count = 0;
    // Quick Look from a folder listing: the rows around it (null elsewhere).
    std::shared_ptr<const QuickPreviewGallery> gallery;
};

enum class QuickPreviewAction : int {
    None = 0, Open, Cut, Copy, CopyPath, ToggleStar, Rename, Delete, Properties,
    InstallMediaPack,   // start (or cancel) the FFmpeg preview pack download
    InstallImagePack,   // start (or cancel) the image preview pack download
    DownloadCloud,      // 下载并预览 on the cloud-only card
    OpenInNewTab,       // the previewed folder, in a new tab
    SaveOptions,        // the window changed QuickPreviewOptions itself (size menu, L, a drag)
    SaveSystemPreview,  // commit the explicit Office preview choice
    SavePlaybackAudio,  // commit a volume gesture or mute toggle
    PinDetach,          // pinned always-on-top: move out of the main follow slot into multi-preview pool
    DetachedClosed,     // a detached preview finished closing; reap it asynchronously
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
    bool prefer_system = false;
    int volume_percent = 100;
    bool muted = false;
    bool immersive = true;          // pictures / videos in the photo viewer layout
    bool operator==(const QuickPreviewOptions& other) const {
        return size_mode == other.size_mode && remember_size == other.remember_size &&
               EqualRect(&remembered, &other.remembered) && space_closes == other.space_closes &&
               loop == other.loop && key_hint_left == other.key_hint_left &&
               prefer_system == other.prefer_system && volume_percent == other.volume_percent &&
               muted == other.muted && immersive == other.immersive;
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
        UINT_PTR token = 0;
        do { token = NextGlobalToken(); } while (!token || pending_.contains(token));
        pending_.emplace(token, std::move(command));
        return token;
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
    static UINT_PTR NextGlobalToken() noexcept {
        static std::atomic<UINT_PTR> global_next{0};
        return global_next.fetch_add(1, std::memory_order_relaxed) + 1;
    }
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

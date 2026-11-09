#pragma once
#include "folder_picker_loader.h"
#include "hover_preview_content.h"
#include "ui_renderer.h"

namespace pulse::ui {
// Owns only the picker's candidate/lifetime. Content and painting belong to
// the same renderer and preview cache as the main window.
class PickerHoverPreview {
public:
    HoverPreviewView view;
    void Cancel() {
        loader_.reset();
        ++generation_;
        view = {};
        started_ = 0;
        prefetched_ = false;
    }
    void Candidate(const PickerEntry& entry, DWORD attrs, D2D1_RECT_F anchor,
                   POINT pointer, HWND owner, int delay, int size_step) {
        if (view.path == entry.path) return;
        Cancel();
        view.path = entry.path;
        view.name = entry.name;
        view.attrs = attrs;
        view.size = entry.size;
        view.modified = (uint64_t{entry.modified.dwHighDateTime} << 32) | entry.modified.dwLowDateTime;
        view.folder = entry.kind == PickerEntryKind::Folder || entry.kind == PickerEntryKind::Drive;
        view.drive = entry.kind == PickerEntryKind::Drive;
        view.drive_total = view.drive ? entry.size : 0;
        view.drive_free = view.drive ? entry.free : 0;
        view.type_text = view.folder ? l10n::Get(l10n::StringId::KindFolder)
                                    : hover_preview_content::TypeName(entry.name);
        view.size_text = L"\u2014";
        view.video = !view.folder && hover_preview_content::CardVideo(entry.path);
        view.slow = !view.folder && hover_preview_content::SlowToDecode(entry.name, entry.size, entry.path);
        view.anchor = anchor;
        view.pointer = D2D1::Point2F(static_cast<float>(pointer.x), static_cast<float>(pointer.y));
        view.owner = owner;
        view.size_step = size_step;
        delay_ = static_cast<ULONGLONG>(delay + (view.folder ? 300 : 0));
        started_ = GetTickCount64();
    }
    bool Pending() const { return started_ && !view.visible; }
    void Tick(MainRenderer& renderer, HWND owner, UINT listing_message, const PickerOptions& options) {
        if (!started_) return;
        const auto elapsed = GetTickCount64() - started_;
        if (!view.folder && !prefetched_ && elapsed >= 150) {
            renderer.PrefetchHoverPreview(view.path, view.attrs, view.size, view.modified);
            prefetched_ = true;
        }
        if (!view.visible && elapsed >= delay_) {
            view.visible = true;
            view.shown_ms = GetTickCount64();
            if (view.folder) {
                loader_ = std::make_unique<PickerLoader>(owner, listing_message);
                PickerOptions listing_options;
                listing_options.show_hidden = options.show_hidden;
                listing_options.sort = options.sort;
                listing_options.descending = options.descending;
                loader_->Load(generation_, view.path, PickerMode::File, listing_options);
            }
        }
    }
    void Receive(std::unique_ptr<PickerListing> listing) {
        if (!listing || listing->generation != generation_ || listing->path != view.path || !view.folder) return;
        auto info = std::make_shared<HoverFolderInfo>();
        info->state = listing->error == ERROR_SUCCESS ? HoverFolderInfo::State::Ready
            : listing->error == ERROR_ACCESS_DENIED ? HoverFolderInfo::State::Denied : HoverFolderInfo::State::Failed;
        for (size_t i = 0; i < listing->entries.size(); ++i) {
            const auto& entry = listing->entries[i];
            HoverFolderItem item;
            item.name = entry.name;
            item.is_dir = entry.kind == PickerEntryKind::Folder || entry.kind == PickerEntryKind::Drive;
            item.attrs = listing->snapshot && i < listing->snapshot->size() ? (*listing->snapshot)[i].attrs : 0;
            item.kind = item.is_dir ? kHoverKindOther : hover_preview_content::FolderKind(entry.name);
            item.size = entry.size;
            item.modified = (uint64_t{entry.modified.dwHighDateTime} << 32) | entry.modified.dwLowDateTime;
            if (item.is_dir) ++info->folders;
            else { ++info->files; ++info->kinds[static_cast<size_t>(item.kind)]; }
            if (info->items.size() < kHoverFolderMaxItems) info->items.push_back(std::move(item));
        }
        view.listing = std::move(info);
    }
private:
    std::unique_ptr<PickerLoader> loader_;
    uint64_t generation_ = 0;
    ULONGLONG started_ = 0, delay_ = 500;
    bool prefetched_ = false;
};
}

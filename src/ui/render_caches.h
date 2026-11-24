// render_caches.h - Decoded icon and thumbnail caches shared by every Pulse
// window (multi-window M2; plan §8).
//
// The bitmaps come from the one per-thread D2D device (ui_compositor.cpp), so
// every window on the UI thread can draw from the same cache, and a second
// window does not double the thumbnail budgets. AppCore owns one instance and
// each window's MainRenderer binds to it; a stand-alone MainRenderer (folder
// picker, tests) owns its own.
#pragma once
#include "folder_thumbnail_cache.h"
#include "open_with_icons.h"
#include "shell_icons.h"
#include "thumbnail_cache.h"

namespace pulse::ui {

struct RenderCaches {
    // The grid decodes on several hosts at once, visible tiles first.
    RenderCaches() { thumbnails.SetParallelism(0); folder_thumbnails.ShareUploadBudget(thumbnails); }
    RenderCaches(const RenderCaches&) = delete;
    RenderCaches& operator=(const RenderCaches&) = delete;

    ShellIconCache icons;
    OpenWithIconCache open_with_icons;
    // Grid/list thumbnails and the details-pane preview decode through
    // separate hosts and budgets: a 2048 px selection preview (up to 16 MB)
    // must neither evict the visible grid nor queue ahead of it.
    // Grid + folder artwork remains capped at 112 MiB in total. Reserve
    // 32 MiB full-size + 8 MiB compact folder artwork, without growing the total.
    ThumbnailCache thumbnails{72ull * 1024ull * 1024ull, 1024};
    FolderThumbnailCache folder_thumbnails;
    ThumbnailCache details{4ull * 1024ull * 1024ull, 24};
    // The hover preview card: its own host, so resting on a file never
    // displaces the details-pane preview of the selection.
    ThumbnailCache hover{12ull * 1024ull * 1024ull, 6};
};

// Stops the decode threads and drops every bitmap. Shared caches outlive any
// one window: the last window resets them (multi-window M3f); a renderer that
// owns its caches resets them when it detaches.
inline void ResetRenderCaches(RenderCaches& caches) {
    caches.icons.Reset();
    caches.open_with_icons.Reset();
    caches.thumbnails.Reset();
    caches.folder_thumbnails.Reset();
    caches.details.Reset();
    caches.hover.Reset();
}

inline void TrimRenderCachesForTray(RenderCaches& caches) {
    caches.thumbnails.TrimToBytes(4u * 1024u * 1024u);
    caches.folder_thumbnails.TrimForBackground(2u * 1024u * 1024u);
    caches.details.TrimToBytes(0);
    caches.hover.TrimToBytes(0);
}

inline size_t TrimRenderCachesActiveWorkingSet(RenderCaches& caches, bool details_active, bool hover_active) {
    size_t freed = 0;
    freed += caches.thumbnails.TrimInactiveToSlack(8u * 1024u * 1024u, 0);
    freed += caches.folder_thumbnails.TrimInactiveWorkingSet();
    if (!details_active) freed += caches.details.TrimInactiveToSlack(0, 0);
    if (!hover_active) freed += caches.hover.TrimInactiveToSlack(0, 0);
    return freed;
}

}  // namespace pulse::ui

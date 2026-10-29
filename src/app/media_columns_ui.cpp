#include "media_columns_ui.h"
#include "media_folder.h"
#include "media_props.h"
#include "app_state.h"
#include "app_runtime.h"
#include "entry_group.h"
#include "entry_sort.h"
#include "../common/text_format.h"
#include <algorithm>
#include <atomic>

namespace pulse::app {

// What a tab's media columns were worked out from (Tab::media_state).
struct MediaColumnsState {
    fs::SnapshotPtr snapshot;
    std::wstring prefix;   // the folder, ending in a backslash
    media::Kind kind = media::Kind::None;
};

namespace {

std::atomic<bool> g_enabled{true};

uint64_t Ticks(const FILETIME& t) noexcept {
    return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

std::wstring Prefix(const std::wstring& folder) {
    return !folder.empty() && folder.back() == L'\\' ? folder : folder + L"\\";
}

// Local folders only: on a share every read would cross the network.
bool Eligible(const std::wstring& path) {
    if (path.empty() || fs::IsVirtualPath(path) || fs::IsUncPath(path)) return false;
    return !path.starts_with(L"\\\\") || (path.starts_with(L"\\\\?\\") && !path.starts_with(L"\\\\?\\UNC\\"));
}

std::shared_ptr<const MediaColumnsState> Build(const Tab& tab) {
    auto state = std::make_shared<MediaColumnsState>();
    state->snapshot = tab.snapshot;
    media::Tally tally;
    for (const fs::DirEntry& e : *tab.snapshot) tally.Add(e.name, e.is_dir);
    state->kind = media::Classify(tally);
    if (state->kind == media::Kind::None) return state;
    state->prefix = Prefix(tab.current_path);
    std::vector<media::Job> jobs;
    for (const fs::DirEntry& e : *tab.snapshot) {
        if (e.is_dir) continue;
        const media::Kind kind = media::KindOfName(e.name);
        if (kind == media::Kind::None) continue;
        media::Job job;
        job.path = state->prefix + e.name;
        job.mtime = Ticks(e.mtime);
        job.size = e.size;
        job.attrs = e.attrs;
        job.kind = kind;
        jobs.push_back(std::move(job));
    }
    media::Request(&tab, std::move(jobs));
    return state;
}

std::wstring Cell(const MediaColumnsState& state, int source, ui::SortColumn column) {
    if (!state.snapshot || source < 0 || static_cast<size_t>(source) >= state.snapshot->size()) return {};
    const fs::DirEntry& e = (*state.snapshot)[static_cast<size_t>(source)];
    if (e.is_dir) return {};
    const auto props = media::Find(state.prefix + e.name, Ticks(e.mtime), e.size);
    if (!props) return {};
    switch (column) {
    case ui::SortColumn::MediaTaken: {
        if (!props->taken) return {};
        FILETIME ft{static_cast<DWORD>(props->taken & 0xFFFFFFFFu), static_cast<DWORD>(props->taken >> 32)};
        return format::LocalFileTime(ft);
    }
    case ui::SortColumn::MediaDims: return media::FormatDimensions(props->width, props->height);
    case ui::SortColumn::MediaCamera: return props->camera;
    case ui::SortColumn::MediaTitle: return props->title;
    case ui::SortColumn::MediaArtist: return props->artist;
    case ui::SortColumn::MediaAlbum: return props->album;
    case ui::SortColumn::MediaDuration: return media::FormatDuration(props->duration);
    case ui::SortColumn::MediaFps: return media::FormatFrameRate(props->fps_milli);
    default: return {};
    }
}

void Decorate(const Tab& tab, ui::PaneViewModel& out) {
    if (!g_enabled.load(std::memory_order_relaxed) || tab.view_mode != ui::ViewMode::Details ||
        out.this_pc_columns || out.is_search || out.content_results || out.column_strip.Active() ||
        tab.loading || !tab.snapshot || !Eligible(tab.current_path))
        return;
    if (tab.media_snapshot != tab.snapshot || !tab.media_state) {
        tab.media_snapshot = tab.snapshot;
        tab.media_state = Build(tab);
    }
    const std::shared_ptr<const MediaColumnsState>& state = tab.media_state;
    if (state->kind == media::Kind::None) return;
    out.media_columns = static_cast<uint8_t>(state->kind);
    out.media_cell = [state](int source, ui::SortColumn column) { return Cell(*state, source, column); };
}

void SortKeys(const std::wstring& folder, const std::vector<fs::DirEntry>& entries, ui::SortColumn column,
              std::vector<MediaSortKey>& keys) {
    keys.clear();
    if (!g_enabled.load(std::memory_order_relaxed)) return;   // names, as the header shows
    keys.assign(entries.size(), MediaSortKey{});
    const std::wstring prefix = Prefix(folder);
    std::wstring path;
    for (size_t i = 0; i < entries.size(); ++i) {
        const fs::DirEntry& e = entries[i];
        if (e.is_dir) continue;
        path = prefix;
        path += e.name;
        const auto props = media::Find(path, Ticks(e.mtime), e.size);
        if (!props) continue;
        MediaSortKey& key = keys[i];
        switch (column) {
        case ui::SortColumn::MediaTaken: key.number = props->taken; key.known = key.number != 0; break;
        case ui::SortColumn::MediaDims:
            key.number = uint64_t{props->width} * props->height;
            key.known = key.number != 0;
            break;
        case ui::SortColumn::MediaCamera: key.text = props->camera; key.known = !key.text.empty(); break;
        case ui::SortColumn::MediaTitle: key.text = props->title; key.known = !key.text.empty(); break;
        case ui::SortColumn::MediaArtist:
            // Then by album and track, so each album plays in order.
            key.text = props->artist + L'\x1F' + props->album;
            key.number = props->track;
            key.known = !props->artist.empty();
            break;
        case ui::SortColumn::MediaAlbum:
            key.text = props->album;
            key.number = props->track;
            key.known = !key.text.empty();
            break;
        case ui::SortColumn::MediaDuration: key.number = props->duration; key.known = key.number != 0; break;
        case ui::SortColumn::MediaFps: key.number = props->fps_milli; key.known = key.number != 0; break;
        default: break;
        }
    }
}

}  // namespace

void InstallMediaColumns() {
    SetMediaPaneDecorator(&Decorate);
    SetMediaSortKeySource(&SortKeys);
}

void SetMediaColumnsEnabled(bool on) noexcept { g_enabled.store(on, std::memory_order_relaxed); }

}  // namespace pulse::app

namespace pulse {
namespace {
// Live re-sorts copy the listing on the window thread: keep them small and paced.
constexpr size_t kLiveResortLimit = 20000;
constexpr uint64_t kResortIntervalMs = 400;
}  // namespace

bool ResortForMediaProps(AppState& s) {
    bool waiting = false;
    const uint64_t now = GetTickCount64();
    const uint64_t revision = app::media::Revision();
    // Rows must not move under a press (the click resolves by index), a drag
    // or the rename box.
    const bool busy = s.scrollAnimating || s.scrollbarDragging ||
        s.renameIndex >= 0 || s.marqueeActive || s.dragPending ||
        (GetKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetKeyState(VK_RBUTTON) & 0x8000) != 0;
    ForEachPane(s, [&](app::Pane& pane) {
        app::Tab* tab = pane.ActiveTab();
        if (!tab || tab->sort_column < ui::SortColumn::MediaTaken || !tab->snapshot || tab->loading ||
            tab->pending_generation != 0 || tab->content_results || tab->search_content_active ||
            !tab->media_state || tab->media_state->kind == app::media::Kind::None ||
            tab->snapshot_path != tab->current_path || !tab->held_renames.empty() ||
            tab->snapshot->size() > kLiveResortLimit || tab->media_resort_revision == revision)
            return;
        if (busy || now - tab->media_resorted_at < kResortIntervalMs) {
            waiting = true;
            return;
        }
        tab->media_resort_revision = revision;
        tab->media_resorted_at = now;
        auto sorted = std::make_shared<std::vector<fs::DirEntry>>(*tab->snapshot);
        {
            const app::ScopedEntryGrouping grouping(tab->EffectiveGroup(), tab->current_path);
            const app::ScopedFolderSortMode folder_mode(tab->EffectiveFolderSort());
            const app::ScopedMediaSortFolder media_folder(tab->current_path);
            app::SortEntries(*sorted, tab->sort_column, tab->sort_direction);
        }
        if (std::equal(sorted->begin(), sorted->end(), tab->snapshot->begin(), tab->snapshot->end(),
                       [](const fs::DirEntry& a, const fs::DirEntry& b) { return a.name == b.name; }))
            return;
        // The selection stays on the same items while the rows move.
        const bool all = tab->all_selected;
        std::vector<std::wstring> names;
        std::wstring focus;
        if (!all) {
            const int count = static_cast<int>(tab->EntryCount());
            for (const int index : tab->SelectedIndices())
                if (index >= 0 && index < count) names.push_back(tab->EntryAt(static_cast<size_t>(index)).name);
            if (tab->selected_index >= 0 && tab->selected_index < count)
                focus = tab->EntryAt(static_cast<size_t>(tab->selected_index)).name;
        }
        tab->SetSnapshot(std::move(sorted));
        // Same files in a new order: the cells index the new snapshot, and
        // nothing needs reading again.
        auto moved = std::make_shared<app::MediaColumnsState>(*tab->media_state);
        moved->snapshot = tab->snapshot;
        tab->media_state = std::move(moved);
        tab->media_snapshot = tab->snapshot;
        tab->order_held = false;
        if (!all) {
            if (names.empty()) tab->ClearSelection();
            else tab->RemapSelection(names, focus);
        }
        if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
    });
    return waiting;
}

}  // namespace pulse

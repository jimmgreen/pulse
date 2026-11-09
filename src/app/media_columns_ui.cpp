#include "media_columns_ui.h"
#include "media_folder.h"
#include "media_props.h"
#include "app_state.h"
#include "app_runtime.h"
#include "entry_group.h"
#include "entry_sort.h"
#include "../common/localization.h"
#include "../common/string_ids.h"
#include "../common/text_format.h"
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <unordered_map>

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

// Group titles run as Explorer sorts names: case-insensitive, digits as numbers.
bool LabelLess(const std::wstring& a, const std::wstring& b) {
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS,
                           a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()),
                           nullptr, nullptr, 0) == CSTR_LESS_THAN;
}

// 拍摄月份 / 相机 / 专辑 / 艺术家 / 分辨率档 for the files of `folder` read so far.
std::shared_ptr<const MediaGroupMap> MediaGroups(const std::wstring& folder, GroupBy g) {
    struct Found {
        std::wstring name;
        uint64_t mtime = 0, size = 0;
        int value = 0;   // year * 12 + month - 1, media::Tier, or an index into texts
    };
    std::vector<Found> found;
    std::vector<std::wstring> texts;                   // distinct, as first spelt
    std::unordered_map<std::wstring, int> text_index;  // lower case -> texts
    media::ForEachInFolder(Prefix(folder), [&](std::wstring_view name, uint64_t mtime, uint64_t size,
                                               const media::Props& p) {
        int value = 0;
        if (g == GroupBy::MediaMonth) {
            if (!p.taken) return;
            const FILETIME utc{static_cast<DWORD>(p.taken & 0xFFFFFFFFu), static_cast<DWORD>(p.taken >> 32)};
            FILETIME local{};
            SYSTEMTIME st{};
            if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) return;
            value = st.wYear * 12 + st.wMonth - 1;
        } else if (g == GroupBy::MediaTier) {
            const media::Tier tier = media::TierOf(p.width, p.height);
            if (tier == media::Tier::Unknown) return;
            value = static_cast<int>(tier);
        } else {
            const std::wstring& text = g == GroupBy::MediaCamera ? p.camera
                                     : g == GroupBy::MediaAlbum ? p.album : p.artist;
            if (text.empty()) return;
            std::wstring key = text;
            for (auto& ch : key) ch = static_cast<wchar_t>(std::towlower(ch));
            const auto [it, inserted] = text_index.try_emplace(std::move(key), static_cast<int>(texts.size()));
            if (inserted) texts.push_back(text);
            value = it->second;
        }
        found.push_back({std::wstring(name), mtime, size, value});
    });
    auto map = std::make_shared<MediaGroupMap>();
    std::vector<int> order;   // distinct values in group order
    for (const Found& f : found) order.push_back(f.value);
    std::sort(order.begin(), order.end());
    order.erase(std::unique(order.begin(), order.end()), order.end());
    if (g == GroupBy::MediaMonth) {
        std::reverse(order.begin(), order.end());   // newest first
    } else if (g != GroupBy::MediaTier) {
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return LabelLess(texts[static_cast<size_t>(a)], texts[static_cast<size_t>(b)]);
        });
    }
    std::unordered_map<int, int> rank_of;
    for (const int value : order) {
        rank_of.emplace(value, static_cast<int>(map->labels.size()));
        if (g == GroupBy::MediaMonth) {
            wchar_t buf[64]{};
            swprintf_s(buf, l10n::Get(l10n::StringId::GroupMediaMonth).c_str(), value / 12, value % 12 + 1);
            map->labels.emplace_back(buf);
        } else if (g == GroupBy::MediaTier) {
            static const wchar_t* const kTiers[] = {L"4K", L"1080p", L"720p"};
            map->labels.emplace_back(value >= 0 && value < 3 ? std::wstring(kTiers[value])
                                                             : l10n::Get(l10n::StringId::GroupNameOther));
        } else {
            map->labels.push_back(texts[static_cast<size_t>(value)]);
        }
    }
    map->by_name.reserve(found.size());
    for (Found& f : found)
        map->by_name.emplace(std::move(f.name), MediaGroupMap::Item{f.mtime, f.size, rank_of[f.value]});
    return map;
}

}  // namespace

void InstallMediaColumns() {
    SetMediaPaneDecorator(&Decorate);
    SetMediaSortKeySource(&SortKeys);
    SetMediaGroupSource(&MediaGroups);
}

void SetMediaColumnsEnabled(bool on) noexcept { g_enabled.store(on, std::memory_order_relaxed); }

int MediaFolderKindFor(const Tab& tab) {
    if (!g_enabled.load(std::memory_order_relaxed) || tab.content_results || tab.loading || !tab.snapshot ||
        !Eligible(tab.current_path))
        return 0;
    if (tab.media_state && tab.media_snapshot == tab.snapshot) return static_cast<int>(tab.media_state->kind);
    media::Tally tally;
    for (const fs::DirEntry& e : *tab.snapshot) tally.Add(e.name, e.is_dir);
    return static_cast<int>(media::Classify(tally));
}

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
        s.renameIndex >= 0 || s.listSelection.BandActive() || s.listSelection.PendingFile() ||
        (GetKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetKeyState(VK_RBUTTON) & 0x8000) != 0;
    ForEachPane(s, [&](app::Pane& pane) {
        app::Tab* tab = pane.ActiveTab();
        if (!tab) return;
        const bool grouped = app::IsMediaGroup(app::GroupByFromInt(tab->EffectiveGroup()));
        // Grouping needs the values in any view; the columns only ask in Details.
        if (grouped && tab->snapshot && !tab->loading && !tab->content_results &&
            tab->snapshot_path == tab->current_path && app::Eligible(tab->current_path) &&
            (tab->media_snapshot != tab->snapshot || !tab->media_state)) {
            tab->media_snapshot = tab->snapshot;
            tab->media_state = app::Build(*tab);
        }
        if ((tab->sort_column < ui::SortColumn::MediaTaken && !grouped) || !tab->snapshot || tab->loading ||
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

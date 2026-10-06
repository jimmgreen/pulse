#include "audio_meta_ui.h"
#include "app_state.h"
#include "../common/path_utils.h"
#include "../common/preview_extensions.h"
#include "../fs/fs_enum.h"
#include "../ui/ui_renderer.h"
#include "../ui/view_layout.h"
#include "app_runtime.h"
#include "entry_group.h"
#include <unordered_map>

namespace pulse {
namespace {
// A tag sort needs every row's tags, not only the ones on screen (#92). The
// queue that carries them is shared with thumbnails, so the extra rows are
// capped rather than drained in one go.
constexpr size_t kSortRequestLimit = 2048;
// Live re-sorts copy the listing on the window thread: keep them small and paced.
constexpr size_t kLiveResortLimit = 20000;
constexpr uint64_t kResortIntervalMs = 750;

std::wstring ChildPath(const std::wstring& parent, const std::wstring& name) {
    return !parent.empty() && parent.back() == L'\\' ? parent + name : parent + L"\\" + name;
}

uint64_t ToTicks(const FILETIME& time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

bool SortsByAudioColumn(ui::SortColumn column) noexcept {
    return column == ui::SortColumn::Title || column == ui::SortColumn::Artist ||
           column == ui::SortColumn::Album;
}

bool IsAudioFile(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return false;
    std::wstring extension = path.substr(dot);
    for (auto& c : extension) c = static_cast<wchar_t>(std::towlower(c));
    return preview::IsAudioExtension(extension);
}

// The tags already back from the preview host for every row of `tab`.
std::shared_ptr<const app::AudioMetaLookup> BuildAudioMetaLookup(AppState& s, const app::Tab& tab,
                                                                const std::wstring& path) {
    auto meta = std::make_shared<app::AudioMetaLookup>();
    // The tag cache is keyed by the path the rows are drawn with, which has no
    // \\?\ prefix; a load request carries the prefixed form.
    const std::wstring folder = path::StripExtendedPathPrefix(path);
    for (const auto& entry : *tab.snapshot) {
        if (entry.is_dir || entry.drive_type != 0) continue;
        const std::wstring full = entry.full_path.empty() ? ChildPath(folder, entry.name)
                                                         : entry.full_path;
        ui::AudioMetaValues values;
        if (!s.renderer.CachedAudioMeta(full, ToTicks(entry.mtime), entry.size, values)) continue;
        // A row that came back untagged has nothing to order by, so it stays out
        // of the lookup and keeps name order with the rows still pending.
        if (values[0].empty() && values[1].empty() && values[2].empty()) continue;
        std::wstring lower = entry.name;
        for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
        meta->emplace(std::move(lower), std::move(values));
    }
    return meta;
}
} // namespace

void FillAudioMeta(AppState& s, ui::WindowViewModel& vm) {
    const uint32_t wanted = s.renderer.DetailsColumnsMask() & ui::kDetailsColumnAudio;
    for (auto& slot : vm.pane_slots) {
        auto& pane = slot.pane;
        pane.audio_meta_labels.clear();
        const bool usable = pane.snapshot && pane.is_file_system && !pane.loading &&
            !pane.content_results && !pane.is_search && !pane.is_recycle && !pane.path.empty() &&
            !fs::IsVirtualPath(pane.path);
        // Only the details columns show these, and search results are listed
        // from the index, which carries no audio properties.
        const bool labels = usable && wanted != 0 && pane.view_mode == ui::ViewMode::Details;
        const bool sorted = usable && SortsByAudioColumn(pane.sort_column);
        if (!labels && !sorted) continue;
        const auto request = [&](const fs::DirEntry& entry, int source, bool record) {
            if (entry.is_dir || entry.drive_type != 0) return;
            const std::wstring path = entry.full_path.empty()
                ? ChildPath(pane.path, entry.name) : entry.full_path;
            if (fs::IsVirtualPath(path) || !IsAudioFile(path)) return;
            ui::AudioMetaValues values;
            if (!s.renderer.RequestAudioMeta(path, entry.attrs, pane.view_generation,
                                             ToTicks(entry.mtime), entry.size, values))
                return;
            if (record) pane.audio_meta_labels.emplace(source, std::move(values));
        };
        if (labels) {
            const auto list = s.renderer.PaneListRect(pane, slot.rect);
            ui::ViewLayout layout(pane.view_mode, list, pane.EntryCount(), pane.scroll_x,
                                  pane.scroll_y, s.scale,
                                  s.renderer.ListRowHeightDip(pane, list), pane.Groups());
            const auto [first, last] = layout.VisibleRange();
            for (int i = std::max(0, first); i <= last; ++i) {
                const int source = pane.SourceIndex(i);
                if (source < 0 || static_cast<size_t>(source) >= pane.snapshot->size()) continue;
                request((*pane.snapshot)[static_cast<size_t>(source)], source, true);
            }
        }
        if (sorted) {
            const auto& entries = *pane.snapshot;
            for (size_t i = 0; i < entries.size() && i < kSortRequestLimit; ++i) {
                request(entries[i], static_cast<int>(i), false);
            }
        }
    }
    for (const auto& slot : vm.pane_slots) if (slot.focused)
        vm.pane.audio_meta_labels = slot.pane.audio_meta_labels;
}

std::shared_ptr<const app::AudioMetaLookup> SortAudioMeta(AppState& s, app::Tab& tab,
                                                        const std::wstring& path) {
    if (!SortsByAudioColumn(tab.sort_column) || !tab.snapshot || path.empty() ||
        fs::IsVirtualPath(path))
        return nullptr;
    auto meta = BuildAudioMetaLookup(s, tab, path);
    tab.audio_meta_signature = app::AudioMetaSignature(*meta);
    tab.audio_meta_resorted_at = GetTickCount64();
    return meta;
}

bool ResortForAudioMeta(AppState& s) {
    bool waiting = false;
    const uint64_t now = GetTickCount64();
    // Rows must not move under a press (the click resolves by index), a drag
    // or the rename box.
    const bool busy = s.renameIndex >= 0 || s.marqueeActive || s.dragPending ||
        (GetKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetKeyState(VK_RBUTTON) & 0x8000) != 0;
    ForEachPane(s, [&](app::Pane& pane) {
        app::Tab* tab = pane.ActiveTab();
        if (!tab || !SortsByAudioColumn(tab->sort_column) || !tab->snapshot || tab->loading ||
            tab->pending_generation != 0 || tab->content_results || tab->search_content_active ||
            tab->current_path.empty() || fs::IsVirtualPath(tab->current_path) ||
            tab->snapshot_path != tab->current_path || !tab->held_renames.empty() ||
            tab->snapshot->size() > kLiveResortLimit)
            return;
        const auto meta = BuildAudioMetaLookup(s, *tab, tab->current_path);
        const uint64_t signature = app::AudioMetaSignature(*meta);
        if (signature == tab->audio_meta_signature) return;
        if (busy || now - tab->audio_meta_resorted_at < kResortIntervalMs) {
            waiting = true;
            return;
        }
        tab->audio_meta_signature = signature;
        tab->audio_meta_resorted_at = now;
        auto sorted = std::make_shared<std::vector<fs::DirEntry>>(*tab->snapshot);
        {
            const app::ScopedEntryGrouping grouping(tab->EffectiveGroup(), tab->current_path);
            app::SortEntriesByAudioMeta(*sorted, tab->sort_column, tab->sort_direction, *meta);
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
        tab->order_held = false;
        if (!all) {
            if (names.empty()) tab->ClearSelection();
            else tab->RemapSelection(names, focus);
        }
        if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
    });
    return waiting;
}
}

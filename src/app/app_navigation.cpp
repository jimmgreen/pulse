// app_navigation.cpp — extracted from app_main.cpp.
#include "app_internal.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/fluent_menu.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../ui/quick_preview_window.h"
#include "../ui/typography.h"
#include "../ui/color_picker.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include "../common/diagnostics_exporter.h"
#include "snapshot_patch.h"
#include "session.h"
#include "../fs/fs_net_cache.h"
#include "context_menu.h"
#include "batch_rename.h"
#include "search_query.h"
#include "link_resolve.h"
#include "resource.h"
#include "../ops/clipboard.h"
#include "../ipc/ctx_menu_util.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <thread>
#include <unordered_set>

using namespace pulse;

namespace pulse {
std::vector<std::wstring> CollectPanePaths(const AppState& s) {
    std::vector<std::wstring> out;
    if (!Root(s)) return out;
    std::vector<app::Pane*> vis;
    Root(s)->CollectPanes(vis);
    for (app::Pane* p : vis) {
        app::Tab* t = p ? p->ActiveTab() : nullptr;
        out.push_back(t ? t->current_path : L"");
    }
    return out;
}

std::vector<ui::ViewMode> CollectPaneViews(const AppState& s) {
    std::vector<ui::ViewMode> out;
    if (!Root(s)) return out;
    std::vector<app::Pane*> panes;
    Root(s)->CollectPanes(panes);
    out.reserve(panes.size());
    for (const app::Pane* pane : panes) {
        const app::Tab* tab = pane ? pane->ActiveTab() : nullptr;
        out.push_back(tab ? tab->view_mode : ui::ViewMode::Details);
    }
    return out;
}

bool IsUncPath(const std::wstring& p) {
    return fs::IsUncPath(p);
}

void PumpUncProbe(AppState& s) {
    if (s.probeBusy || s.probeQueue.empty() || !s.hwnd) return;
    s.probeUnc = s.probeQueue.front();
    s.probeQueue.erase(s.probeQueue.begin());
    s.probeBusy = true;
    fs::StartUncProbe(s.hwnd, WM_NET_PROBE, s.probeUnc);
}

void RequestUncProbe(AppState& s, std::wstring unc) {
    if (!fs::IsUncPath(unc) || !s.hwnd) return;
    unc = fs::NormalizePath(unc);
    if (s.probeBusy && s.probeUnc == unc) return;
    for (const auto& q : s.probeQueue) if (q == unc) return;
    s.probeQueue.push_back(std::move(unc));
    PumpUncProbe(s);
}

void ProbePinnedNetworks(AppState& s) {
    for (const auto& n : s.places.networks) RequestUncProbe(s, n.unc);
}

void WarmupUnc(AppState& s, const std::wstring& path) {
    if (!fs::IsUncPath(path)) return;
    RequestUncProbe(s, path);
    uint64_t gen = 0;
    if (s.store.GetOrStart(path, gen)) return;
    if (!s.store.Peek(path)) {
        uint64_t ts = 0;
        if (auto disk = fs::LoadNetSnapshot(path, &ts))
            s.store.Update(path, gen, disk);
    }
    s.worker.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Asc);
}

std::wstring PinCandidate(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return L"";
    if (tab->SelectedCount() == 1 && tab->snapshot && tab->selected_index >= 0 &&
        tab->selected_index < static_cast<int>(tab->snapshot->size()) &&
        (*tab->snapshot)[static_cast<size_t>(tab->selected_index)].is_dir) {
        return EntryFullPath(*tab, tab->selected_index);
    }
    if (fs::IsVirtualPath(tab->current_path)) return L"";
    return tab->current_path;
}

std::wstring ProjectSearchRoot(AppState& s) {
    if (s.places.active_workspace >= 0 &&
        s.places.active_workspace < static_cast<int>(s.places.workspaces.size())) {
        return s.places.workspaces[static_cast<size_t>(s.places.active_workspace)].root;
    }
    app::Tab* tab = ActiveTab(s);
    if (!tab) return L"";
    std::wstring git = app::FindGitRoot(tab->current_path);
    if (!git.empty()) return git;
    return fs::IsVirtualPath(tab->current_path) ? L"" : tab->current_path;
}

void OpenWorkspace(AppState& s, int index) {
    if (index < 0 || index >= static_cast<int>(s.places.workspaces.size())) return;
    const app::Workspace w = s.places.workspaces[static_cast<size_t>(index)];
    s.places.active_workspace = index;
    const std::wstring root = fs::NormalizePath(w.root);
    size_t found = static_cast<size_t>(-1);
    for (size_t i = 0; i < s.window_tabs.items.size(); ++i) {
        const app::Tab* folder = s.window_tabs.items[i]->ActiveFolder();
        if (!folder || folder->current_path.empty()) continue;
        if (_wcsicmp(fs::NormalizePath(folder->current_path).c_str(), root.c_str()) == 0) {
            found = i;
            break;
        }
    }
    if (found != static_cast<size_t>(-1)) SwitchTab(s, found);
    else NewTab(s, w.root.empty() ? L"C:\\" : w.root);
    ApplyLayoutPreset(s, static_cast<app::LayoutPreset>(std::clamp(w.layout, 0, 4)));
    if (w.pane_paths.empty()) {
        NavigateTo(s, w.root);
    } else {
        for (size_t i = 0; i < Panes(s).size() && i < w.pane_paths.size(); ++i) {
            const std::wstring& pth = w.pane_paths[i];
            if (pth.empty()) continue;
            app::Tab* t = Panes(s)[i]->ActiveTab();
            if (!t) {
                Panes(s)[i]->NewTab(pth);
                t = Panes(s)[i]->ActiveTab();
            }
            if (t && i < w.pane_views.size()) t->view_mode = w.pane_views[i];
            if (t) StartLoadingPath(s, *t, pth);
        }
        RememberPath(s, w.root);
    }
    s.places.Save();
    for (const auto& n : s.places.networks) WarmupUnc(s, n.unc);
    for (const auto& pth : w.pane_paths) WarmupUnc(s, pth);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void FillContentNeedles(index::ContentSearchRequest& request, const app::SplitSearchQuery& split) {
    request.needles = split.content.needles;
    request.excluded_needles = split.content.excluded;
    request.match_mode = split.content.mode;
    request.whole_word = split.content.whole_word;
    request.case_sensitive = split.content.case_sensitive;
    request.needle.clear();
    if (split.content.mode == index::ContentMatchMode::Phrase) {
        for (const auto& word : split.content.needles) {
            if (!request.needle.empty()) request.needle.push_back(L' ');
            request.needle.append(word);
        }
    } else if (!split.content.needles.empty()) {
        request.needle = split.content.needles.front();
    }
}

static void SetQuerySearchTitle(app::Tab& tab, const std::wstring& rest, size_t count,
                         size_t loaded = static_cast<size_t>(-1)) {
    const std::wstring needle = app::SearchDisplayNeedle(rest);
    const std::wstring shown = needle.empty() ? l10n::Get(l10n::StringId::Search) : needle;
    wchar_t title[512]{};
    if (loaded != static_cast<size_t>(-1) && count > loaded) {
        swprintf_s(title, l10n::Get(l10n::StringId::SearchResultsPartialFormat).c_str(),
                   shown.c_str(), count, loaded);
    } else {
        swprintf_s(title, l10n::Get(l10n::StringId::SearchResultsFormat).c_str(),
                   shown.c_str(), count);
    }
    tab.virtual_title = title;
}

index::Query MakeSearchPageQuery(const app::Tab& tab, const std::wstring& rest,
                                        size_t offset) {
    const auto split = app::SplitSearchQueryText(rest);
    index::Query q;
    q.needle = app::ApplyContentSearchGuards(split.filename_needle, split);
    q.path_prefix = split.path_prefix;
    q.offset = offset;
    q.limit = index::kSearchUiPageSize;
    q.rank = false;
    q.sort = index::ResultSort::Name;
    q.sort_desc = tab.sort_direction == ui::SortDirection::Desc;
    switch (tab.sort_column) {
    case ui::SortColumn::Size: q.sort = index::ResultSort::Size; break;
    case ui::SortColumn::Mtime: q.sort = index::ResultSort::Mtime; break;
    default: q.sort = index::ResultSort::Name; break;
    }
    if (split.content.present()) {
        q.offset = 0;
        q.limit = index::kContentCandidateCap;
        q.rank = true;
        q.sort = index::ResultSort::Index;
    }
    return q;
}

void DispatchIndexSearch(AppState& s, const index::Query& query, uint32_t id) {
    // Both providers return candidates from the beginning of their own ordering.
    // The UI then merges, globally sorts and applies the requested page offset.
    index::Query provider_query = query;
    provider_query.offset = 0;
    provider_query.limit = (std::min)(index::kSearchPageCap,
        query.offset > index::kSearchPageCap - (std::min)(query.limit, index::kSearchPageCap)
            ? index::kSearchPageCap : query.offset + query.limit);
    s.pendingIndexSearches.clear();
    AppState::PendingIndexSearch pending;
    pending.query = query;
    s.pendingIndexSearches.emplace(id, std::move(pending));
    s.index.SearchAsync(provider_query, id);
    s.networkIndex.SearchAsync(provider_query, id);
}

void RequestSearchPage(AppState& s, app::Tab& tab, const std::wstring& rest,
                              bool reset) {
    const auto split = app::SplitSearchQueryText(rest);
    const size_t offset = reset ? 0 : tab.search_next_offset;
    if (!reset && (tab.search_loading_more || offset >= tab.search_total ||
                   tab.search_awaiting_content || tab.search_content_active)) return;
    if (reset) {
        tab.search_entries = std::make_shared<std::vector<fs::DirEntry>>();
        tab.search_snippets = std::make_shared<std::vector<std::wstring>>();
        tab.search_total = 0;
        tab.search_next_offset = 0;
        tab.search_awaiting_content = split.content.present();
        tab.search_content_active = false;
        tab.SetSnapshot(tab.search_entries);
        tab.loading = true;
        tab.banner_title.clear();
        tab.banner_message.clear();
        if (split.content.present() && app::ContentSearchNeedsScope(split)) {
            tab.loading = false;
            tab.search_awaiting_content = false;
            tab.banner_title = l10n::Get(l10n::StringId::AdvancedSearchNeedScopeTitle);
            tab.banner_message = l10n::Get(l10n::StringId::AdvancedSearchNeedScopeMessage);
            tab.virtual_title = l10n::Get(l10n::StringId::AdvancedSearch);
            return;
        }
    } else {
        tab.search_loading_more = true;
    }
    const uint32_t id = ++s.nextIndexReq;
    tab.pending_generation = id;
    tab.pending_search_offset = offset;
    DispatchIndexSearch(s, MakeSearchPageQuery(tab, rest, offset), id);
}

void ApplySearchHits(app::Tab& tab, const std::wstring& rest,
                            index::SearchResult&& result) {
    tab.search_retaining_results = false;
    const size_t offset = tab.pending_search_offset;
    if (!tab.search_entries || offset == 0) {
        tab.search_entries = std::make_shared<std::vector<fs::DirEntry>>();
    }
    auto& entries = *tab.search_entries;
    if (offset != entries.size()) {
        tab.search_loading_more = false;
        tab.pending_generation = 0;
        return;
    }
    entries.reserve(entries.size() + result.hits.size());
    for (auto& hit : result.hits) {
        fs::DirEntry e;
        e.full_path = std::move(hit.path);
        e.name = std::move(hit.name);
        e.is_dir = hit.is_dir;
        e.size = hit.size;
        e.mtime.dwLowDateTime = static_cast<DWORD>(hit.mtime);
        e.mtime.dwHighDateTime = static_cast<DWORD>(hit.mtime >> 32);
        e.attrs = hit.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        entries.push_back(std::move(e));
    }
    tab.search_total = result.total;
    tab.search_next_offset = entries.size();
    if (tab.search_snippets && tab.search_snippets->size() < entries.size())
        tab.search_snippets->resize(entries.size());
    if (tab.search_total > entries.size())
        SetQuerySearchTitle(tab, rest, tab.search_total, entries.size());
    else
        SetQuerySearchTitle(tab, rest, tab.search_total);
    tab.SetSnapshot(tab.search_entries);
    tab.loading = false;
    tab.search_loading_more = false;
    tab.pending_generation = 0;
    if (offset == 0 && tab.snapshot && !tab.snapshot->empty()) tab.SelectOnly(0);
}

void RequestSavedSearch(AppState& s, app::Tab& tab, size_t saved_index) {
    const auto& saved = s.savedSearches.items();
    if (saved_index >= saved.size()) {
        tab.virtual_title = l10n::Get(l10n::StringId::SavedSearchUnavailable);
        tab.banner_title = l10n::Get(l10n::StringId::CannotOpenSavedSearch);
        tab.banner_message = l10n::Get(l10n::StringId::SavedSearchRemoved);
        tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
        return;
    }
    const app::SavedSearch& search = saved[saved_index];
    tab.virtual_title = search.name + L"…";
    tab.view_mode = ui::ViewMode::Details;
    tab.search_entries = std::make_shared<std::vector<fs::DirEntry>>();
    tab.search_total = 0;
    tab.SetSnapshot(tab.search_entries);
    tab.loading = true;
    const uint64_t generation = ++s.nextIndexReq;
    tab.pending_generation = generation;
    if (search.mode == app::SavedSearchMode::Name) {
        index::Query query = MakeSearchPageQuery(tab, search.query, 0);
        query.path_prefix = search.root;
        DispatchIndexSearch(s, query, static_cast<uint32_t>(generation));
        return;
    }
    tab.search_snippets = std::make_shared<std::vector<std::wstring>>();
    tab.search_content_active = search.mode == app::SavedSearchMode::Content;
    index::ContentSearchRequest request;
    request.generation = generation;
    request.mode = search.mode == app::SavedSearchMode::Duplicates
        ? index::ContentSearchMode::Duplicates : index::ContentSearchMode::Content;
    request.root = search.root;
    request.needle = search.query;
    request.recursive = search.recursive;
    s.contentSearch.SearchAsync(std::move(request));
}

void AppendContentHits(app::Tab& tab, std::vector<index::ContentHit> hits) {
    if (!tab.search_entries)
        tab.search_entries = std::make_shared<std::vector<fs::DirEntry>>();
    if (!tab.search_snippets)
        tab.search_snippets = std::make_shared<std::vector<std::wstring>>();
    auto& entries = *tab.search_entries;
    auto& snippets = *tab.search_snippets;
    entries.reserve(entries.size() + hits.size());
    snippets.reserve(snippets.size() + hits.size());
    for (auto& hit : hits) {
        fs::DirEntry entry;
        entry.full_path = std::move(hit.path);
        entry.name = std::move(hit.name);
        entry.size = hit.size;
        entry.mtime.dwLowDateTime = static_cast<DWORD>(hit.modified);
        entry.mtime.dwHighDateTime = static_cast<DWORD>(hit.modified >> 32);
        entry.attrs = FILE_ATTRIBUTE_NORMAL;
        std::wstring snippet = hit.snippet;
        if (hit.line) {
            snippet = L"L" + std::to_wstring(hit.line);
            if (!hit.snippet.empty()) snippet += L"  " + hit.snippet;
        }
        entries.push_back(std::move(entry));
        snippets.push_back(std::move(snippet));
    }
    tab.search_total = entries.size();
    tab.SetSnapshot(tab.search_entries);
}

void StartContentScanFromHits(AppState& s, app::Tab& tab, const std::wstring& rest,
                              index::SearchResult&& result) {
    const auto split = app::SplitSearchQueryText(rest);
    tab.search_awaiting_content = false;
    tab.search_content_active = true;
    tab.search_entries = std::make_shared<std::vector<fs::DirEntry>>();
    tab.search_snippets = std::make_shared<std::vector<std::wstring>>();
    tab.SetSnapshot(tab.search_entries);
    std::vector<std::wstring> paths;
    paths.reserve((std::min)(result.hits.size(), index::kContentCandidateCap));
    for (auto& hit : result.hits) {
        if (hit.is_dir) continue;
        paths.push_back(std::move(hit.path));
        if (paths.size() >= index::kContentCandidateCap) break;
    }
    const bool capped = result.total > paths.size() ||
                        result.hits.size() >= index::kContentCandidateCap;
    if (paths.empty()) {
        if (split.path_prefix.empty()) {
            tab.loading = false;
            tab.search_content_active = false;
            tab.pending_generation = 0;
            SetQuerySearchTitle(tab, rest, 0);
            return;
        }
        index::ContentSearchRequest request;
        request.generation = tab.pending_generation;
        request.mode = index::ContentSearchMode::Content;
        request.root = split.path_prefix;
        request.recursive = true;
        request.skip_system_locations = true;
        FillContentNeedles(request, split);
        tab.loading = true;
        s.contentSearch.SearchAsync(std::move(request));
        return;
    }
    if (capped) {
        tab.banner_title = l10n::Get(l10n::StringId::ContentSearchCappedTitle);
        tab.banner_message = l10n::Get(l10n::StringId::ContentSearchCappedMessage);
    }
    index::ContentSearchRequest request;
    request.generation = tab.pending_generation;
    request.mode = index::ContentSearchMode::Content;
    request.candidate_paths = std::move(paths);
    FillContentNeedles(request, split);
    tab.loading = true;
    s.contentSearch.SearchAsync(std::move(request));
}

void ApplyContentSearchUpdate(AppState& s, index::ContentSearchUpdate update) {
    app::Tab* target = nullptr;
    size_t saved_index = 0;
    std::wstring search_kind;
    ForEachPane(s, [&](app::Pane& pane) {
        if (target) return;
        app::Tab* tab = pane.ActiveTab();
        if (!tab || tab->pending_generation != update.progress.generation) return;
        std::wstring kind, rest;
        if (!app::ParsePulsePath(tab->current_path, &kind, &rest)) return;
        if (kind != L"saved-search" && kind != L"search") return;
        if (kind == L"saved-search") {
            wchar_t* end = nullptr;
            const unsigned long long parsed = wcstoull(rest.c_str(), &end, 10);
            if (!end || *end != L'\0') return;
            saved_index = static_cast<size_t>(parsed);
        }
        target = tab;
        search_kind = kind;
    });
    if (!target) return;
    if (search_kind == L"saved-search" && saved_index >= s.savedSearches.items().size()) return;
    AppendContentHits(*target, std::move(update.hits));
    wchar_t item_count[64]{};
    swprintf_s(item_count, l10n::Get(l10n::StringId::ItemsCountFormat).c_str(),
               static_cast<int>(target->search_total));
    if (search_kind == L"saved-search") {
        target->virtual_title = s.savedSearches.items()[saved_index].name + L" · " + item_count;
    } else {
        std::wstring rest;
        app::ParsePulsePath(target->current_path, nullptr, &rest);
        SetQuerySearchTitle(*target, rest, target->search_total);
    }
    if (!update.progress.done) {
        wchar_t scanned[96]{};
        swprintf_s(scanned, l10n::Get(l10n::StringId::ScannedFilesFormat).c_str(),
                   static_cast<unsigned long long>(update.progress.scanned_files));
        target->virtual_title += scanned;
    } else {
        target->loading = false;
        target->search_content_active = false;
        target->pending_generation = 0;
        if (update.progress.truncated) {
            target->banner_title = l10n::Get(l10n::StringId::ResultLimitTitle);
            target->banner_message = l10n::Get(l10n::StringId::ResultLimitMessage);
        } else if (update.progress.error != ERROR_SUCCESS &&
                   update.progress.error != ERROR_CANCELLED) {
            target->banner_title = l10n::Get(l10n::StringId::SearchIncomplete);
            wchar_t error[64]{};
            swprintf_s(error, l10n::Get(l10n::StringId::ErrorCodeFormat).c_str(),
                       update.progress.error);
            target->banner_message = error;
        }
        if (target->search_entries && !target->search_entries->empty() &&
            target->selected_index < 0)
            target->SelectOnly(0);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void DeliverIndexSearchResult(AppState& s, uint32_t id,
                                     index::SearchResult&& result) {
    if (id == s.paletteSearchId) {
        s.paletteHits = std::move(result.hits);
        s.paletteTotal = result.total;
        s.paletteSearching = false;
        if (s.menu && s.menu->IsOpen()) s.menu->RequestFilterRefresh();
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    bool applied = false;
    ForEachPane(s, [&](app::Pane& pane) {
        if (applied) return;
        app::Tab* tab = pane.ActiveTab();
        if (!tab || tab->pending_generation != id) return;
        std::wstring kind, rest;
        app::ParsePulsePath(tab->current_path, &kind, &rest);
        if (kind == L"search") {
            if (tab->search_awaiting_content)
                StartContentScanFromHits(s, *tab, rest, std::move(result));
            else
                ApplySearchHits(*tab, rest, std::move(result));
        } else if (kind == L"saved-search") {
            wchar_t* end = nullptr;
            const unsigned long long index = wcstoull(rest.c_str(), &end, 10);
            if (!end || *end != L'\0' || index >= s.savedSearches.items().size()) return;
            ApplySearchHits(*tab, s.savedSearches.items()[static_cast<size_t>(index)].name,
                            std::move(result));
        } else {
            return;
        }
        applied = true;
        InvalidateRect(s.hwnd, nullptr, FALSE);
    });
}

void AcceptIndexProviderResult(AppState& s, uint32_t id,
                                      index::SearchResult&& result, bool network) {
    auto found = s.pendingIndexSearches.find(id);
    if (found == s.pendingIndexSearches.end()) return;
    auto& pending = found->second;
    if (network) {
        pending.network = std::move(result);
        pending.network_ready = true;
    } else {
        pending.local = std::move(result);
        pending.local_ready = true;
    }
    if (!pending.local_ready || !pending.network_ready) return;
    index::SearchResult merged = index::MergeSearchResults(
        pending.query, std::move(pending.local), std::move(pending.network));
    s.pendingIndexSearches.erase(found);
    DeliverIndexSearchResult(s, id, std::move(merged));
}

void MaybePrefetchSearchPage(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->loading || tab->search_loading_more ||
        tab->search_next_offset >= tab->search_total ||
        tab->search_awaiting_content || tab->search_content_active) return;
    std::wstring kind, rest;
    if (!app::ParsePulsePath(tab->current_path, &kind, &rest) || kind != L"search") return;
    const D2D1_RECT_F list = ListRect(s);
    const float view_h = std::max(0.0f, list.bottom - list.top);
    const float max_scroll = MaxScrollForActivePane(s);
    const float scroll_y = std::max(tab->scroll_y, s.scrollTargetY);
    if (scroll_y + view_h * 2.0f >= max_scroll)
        RequestSearchPage(s, *tab, rest, false);
}

void CancelActiveContentSearch(AppState& s, app::Tab& tab) {
    s.contentSearch.Cancel();
    tab.search_content_active = false;
    tab.search_awaiting_content = false;
    tab.loading = false;
    tab.pending_generation = 0;
    std::wstring kind, rest;
    if (app::ParsePulsePath(tab.current_path, &kind, &rest) && kind == L"search")
        SetQuerySearchTitle(tab, rest, tab.search_total);
}

void LoadVirtualView(AppState& s, app::Tab& tab, const std::wstring& path) {
    tab.current_path = path;
    tab.loading = false;
    tab.pending_generation = 0;
    tab.virtual_title.clear();
    SyncVisibleWatches(s);

    std::wstring kind, rest;
    app::ParsePulsePath(path, &kind, &rest);
    if (kind == L"workspace") {
        OpenWorkspace(s, _wtoi(rest.c_str()));
        return;
    }

    if (kind == L"tag") {
        const app::TagId tag_id = s.places.ResolveTagRef(rest);
        const int ti = s.places.FindTagIndex(tag_id);
        if (ti >= 0 && ti < static_cast<int>(s.places.tags.size())) {
            const auto& tag = s.places.tags[static_cast<size_t>(ti)];
            tab.current_path = app::MakeTagPath(tag.id);
            tab.virtual_title = tag.name;
            tab.loading = true;
            tab.SetSnapshot(nullptr);
            tab.pending_generation = s.worker.LoadPaths(
                path, tag.paths, tab.sort_column, tab.sort_direction);
            return;
        }
    } else if (kind == L"search") {
        wchar_t title[512]{};
        swprintf_s(title, l10n::Get(l10n::StringId::SearchLoadingFormat).c_str(),
                   rest.c_str());
        tab.virtual_title = title;
        tab.view_mode = ui::ViewMode::Details;
        RequestSearchPage(s, tab, rest, true);
        return;
    } else if (kind == L"saved-search") {
        wchar_t* end = nullptr;
        const unsigned long long index = wcstoull(rest.c_str(), &end, 10);
        if (end && *end == L'\0') {
            RequestSavedSearch(s, tab, static_cast<size_t>(index));
        } else {
            tab.virtual_title = l10n::Get(l10n::StringId::SavedSearchUnavailable);
            tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
        }
        return;
    } else if (kind == L"starred") {
        tab.virtual_title = l10n::Get(l10n::StringId::StarredItems);
        tab.loading = true;
        tab.view_mode = ui::ViewMode::Details;
        tab.SetSnapshot(nullptr);
        tab.pending_generation = s.worker.LoadPaths(
            path, s.places.StarredPaths(), tab.sort_column, tab.sort_direction, true);
        return;
    } else if (kind == L"recent") {
        tab.virtual_title = l10n::Get(l10n::StringId::Recent);
        tab.loading = true;
        tab.view_mode = ui::ViewMode::Details;
        tab.SetSnapshot(nullptr);
        const auto filter = static_cast<app::RecentFilter>(
            std::clamp(tab.recent_filter, 0, 2));
        const auto recent = s.places.RecentItems(filter);
        std::vector<std::wstring> paths;
        std::vector<uint64_t> opened;
        paths.reserve(recent.size());
        opened.reserve(recent.size());
        for (const auto& item : recent) {
            paths.push_back(item.path);
            opened.push_back(item.opened_at);
        }
        tab.pending_generation = s.worker.LoadPaths(
            path, std::move(paths), tab.sort_column, tab.sort_direction, true,
            std::move(opened));
        return;
    } else if (kind == L"recycle") {
        tab.virtual_title = l10n::Get(l10n::StringId::RecycleBin);
        tab.loading = true;
        tab.view_mode = ui::ViewMode::Details;
        tab.SetSnapshot(nullptr);
        tab.banner_title = l10n::Get(l10n::StringId::RecycleBin);
        tab.banner_message = RecycleOccupancyText(s.recycle_info);
        tab.pending_generation = s.worker.Refresh(
            path, tab.sort_column, tab.sort_direction);
        return;
    } else if (kind == L"settings") {
        tab.virtual_title = l10n::Get(l10n::StringId::Settings);
        tab.loading = false;
        tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
        s.settings.SelectPage(app::SettingsController::PageFromName(rest));
        return;
    }
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    tab.SetSnapshot(std::move(entries));
    if (tab.snapshot && !tab.snapshot->empty()) tab.SelectOnly(0);
}

void StartLoadingPath(AppState& s, app::Tab& tab, const std::wstring& path) {
    tab.search_retaining_results = false;
    std::wstring normalized = fs::NormalizePath(path);
    tab.current_path = normalized;
    if (const auto git = s.gitRoots.find(normalized); git != s.gitRoots.end())
        tab.git_root = git->second;
    else
        tab.git_root.clear();
    tab.loading = true;
    tab.ClearSelection();
    tab.pending_selected_name.clear();
    tab.pending_selected_names.clear();
    tab.pending_ensure_selection_visible = false;
    tab.pending_generation = 0;
    tab.applied_generation = 0;
    tab.scroll_y = 0.0f;
    tab.search_entries.reset();
    tab.search_total = 0;
    tab.search_next_offset = 0;
    tab.pending_search_offset = 0;
    if (tab.search_content_active || tab.search_awaiting_content)
        s.contentSearch.Cancel();
    tab.search_loading_more = false;
    tab.search_awaiting_content = false;
    tab.search_content_active = false;
    tab.search_snippets.reset();
    if (fs::IsVirtualPath(normalized)) {
        LoadVirtualView(s, tab, normalized);
        return;
    }
    tab.virtual_title.clear();
    tab.banner_title.clear();
    tab.banner_message.clear();
    tab.net_readonly = false;
    tab.cache_unix = 0;
    if (tab.snapshot_path != normalized) {
        tab.SetSnapshot(nullptr);
        tab.applied_generation = 0;
    }
    const bool focused = s.pane && s.pane->ActiveTab() == &tab;
    if (focused) {
        s.scrollTargetY = 0.0f;
        s.scrollAnimating = false;
    }

    uint64_t shared_generation = 0;
    fs::SnapshotPtr shared_snapshot;
    ForEachPane(s, [&](app::Pane& pane) {
        if (shared_generation != 0) return;
        const app::Tab* other = pane.ActiveTab();
        if (!other || other == &tab || other->current_path != normalized ||
            other->pending_generation == 0 ||
            other->sort_column != tab.sort_column ||
            other->sort_direction != tab.sort_direction) {
            return;
        }
        shared_generation = other->pending_generation;
        shared_snapshot = other->snapshot;
    });

    uint64_t gen = 0;
    fs::SnapshotPtr snap;
    if (shared_generation != 0) {
        snap = shared_snapshot ? shared_snapshot : s.store.Peek(normalized);
    } else {
        snap = s.store.GetOrStart(normalized, gen);
        (void)gen;
        if (!snap) snap = s.store.Peek(normalized);
    }
    uint64_t disk_ts = 0;
    const bool from_net = !snap && fs::IsUncPath(normalized);
    if (from_net)
        snap = fs::LoadNetSnapshot(normalized, &disk_ts);

    if (snap) {
        tab.SetSnapshot(snap);
        tab.loading = false;
        if (tab.snapshot && !tab.snapshot->empty()) tab.SelectOnly(0);
        if (from_net) {
            tab.cache_unix = disk_ts;
            tab.banner_title = l10n::Get(l10n::StringId::Cache);
            tab.banner_message = disk_ts
                ? fs::FormatCacheAge(disk_ts) + L" · " +
                    l10n::Get(l10n::StringId::Refreshing)
                : l10n::Get(l10n::StringId::Refreshing);
        }
    }
    tab.pending_generation = shared_generation != 0
        ? shared_generation
        : s.worker.Refresh(normalized, tab.sort_column, tab.sort_direction);

    SyncVisibleWatches(s);
    if (fs::IsUncPath(normalized)) RequestUncProbe(s, normalized);
}

void ApplyWorkerResult(AppState& s, app::WorkResult& res) {
    if (Panes(s).empty()) return;
    if (res.error) {
        ForEachPane(s, [&](app::Pane& pane) {
            app::Tab* tab = pane.ActiveTab();
            if (!tab || tab->current_path != res.path) return;
            if (tab->pending_generation != 0 &&
                tab->pending_generation != res.generation) return;
            tab->loading = false;
            tab->pending_generation = 0;
            tab->net_readonly = fs::IsUncPath(res.path);
            tab->banner_title = l10n::Get(tab->net_readonly
                ? l10n::StringId::Offline : l10n::StringId::CannotOpen);
            tab->banner_message = tab->snapshot
                ? l10n::Get(l10n::StringId::CachedReadOnly)
                : l10n::Get(l10n::StringId::DirectoryUnavailable);
        });
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    const bool again = res.snapshot && !fs::IsVirtualPath(res.path) &&
                       s.store.IsDirty(res.path);
    if (res.snapshot && !fs::IsVirtualPath(res.path)) {
        s.store.Update(res.path, res.generation, res.snapshot, res.identity);
    }
    if (!fs::IsVirtualPath(res.path)) s.gitRoots[res.path] = res.git_root;
    bool any = false;
    ForEachPane(s, [&](app::Pane& pane) {
        app::Tab* tab = pane.ActiveTab();
        if (!tab || tab->current_path != res.path) return;
        if (tab->pending_generation != 0 && tab->pending_generation != res.generation) return;
        if (tab->applied_generation != 0 && res.generation < tab->applied_generation) return;
        any = true;

        const bool focusedTab = (&pane == s.pane);
            std::vector<std::wstring> pendingNames = std::move(tab->pending_selected_names);
            std::wstring pendingFocus = std::move(tab->pending_selected_name);
            const bool ensurePendingVisible = tab->pending_ensure_selection_visible;
            tab->pending_selected_names.clear();
            tab->pending_selected_name.clear();
            tab->pending_ensure_selection_visible = false;
            std::wstring renameTarget;
            if (focusedTab) {
                renameTarget = s.pendingRenameName;
                if (renameTarget.empty() && s.renameIndex >= 0 && tab->snapshot &&
                    s.renameIndex < static_cast<int>(tab->snapshot->size())) {
                    renameTarget = (*tab->snapshot)[s.renameIndex].name;
                }
            }
            std::wstring virtual_kind;
            if (res.snapshot && app::ParsePulsePath(
                    tab->current_path, &virtual_kind, nullptr) &&
                (virtual_kind == L"starred" || virtual_kind == L"recent")) {
                for (const auto& entry : *res.snapshot) {
                    if (entry.full_path.empty() || entry.attrs == 0) continue;
                    const auto item_kind = entry.is_dir
                        ? app::PlaceItemKind::Folder : app::PlaceItemKind::File;
                    if (virtual_kind == L"starred")
                        s.places.SetStarredKind(entry.full_path, item_kind);
                    else
                        s.places.SetRecentKind(entry.full_path, item_kind);
                }
            }
            tab->SetSnapshot(res.snapshot);
            tab->git_root = res.git_root;
            tab->loading = false;
            tab->pending_generation = 0;
            tab->applied_generation = res.generation;
            tab->net_readonly = false;
            tab->banner_title.clear();
            tab->banner_message.clear();
            tab->cache_unix = 0;
            if (virtual_kind == L"recycle" || fs::IsRecycleViewPath(res.path)) {
                ApplyQueriedRecycleInfo(s, res.recycle_info);
                ApplyRecycleOccupancy(s);
            }

            bool startedRename = false;
            if (focusedTab && !renameTarget.empty() && tab->snapshot) {
                for (int i = 0; i < static_cast<int>(tab->snapshot->size()); ++i) {
                    if ((*tab->snapshot)[i].name != renameTarget) continue;
                    tab->SelectOnly(i);
                    EnsureRowVisible(s, *tab, i);
                    s.pendingRenameName.clear();
                    if (s.renameIndex >= 0) {
                        s.renameIndex = i;
                        LayoutRenameOverlay(s);
                    } else {
                        ShowRenameOverlay(s);
                    }
                    startedRename = true;
                    break;
                }
                if (!startedRename) {
                    s.pendingRenameName = renameTarget;
                    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
                }
            }
            if (!startedRename) {
                if (focusedTab && !s.pendingRenameName.empty()) {
                    if (!pendingNames.empty()) tab->RemapSelection(pendingNames, pendingFocus);
                    else tab->ClearSelection();
                } else if (!pendingNames.empty()) {
                    tab->RemapSelection(pendingNames, pendingFocus);
                } else if (tab->snapshot && !tab->snapshot->empty()) {
                    tab->SelectOnly(0);
                } else {
                    tab->ClearSelection();
                }
            }
            if (focusedTab && ensurePendingVisible && tab->selected_index >= 0) {
                EnsureRowVisible(s, *tab, tab->selected_index);
                s.scrollTargetY = tab->scroll_y;
                s.scrollAnimating = false;
            }
    });
    if (again) RefreshPath(s, res.path);
    if (!any) return;

    s.timing.enum_ms = res.enum_ms;
    s.timing.sort_ms = res.sort_ms;
    s.timing.sort_done_ms = res.enum_ms + res.sort_ms;

    if (s.shot.active) InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ProcessPendingResults(AppState& s) {
    std::queue<app::WorkResult> batch;
    {
        std::lock_guard<std::mutex> lock(s.resultMutex);
        batch = std::move(s.results);
        s.results = {};
    }
    while (!batch.empty()) {
        ApplyWorkerResult(s, batch.front());
        batch.pop();
    }
}

void CaptureListingSelection(app::Tab& tab) {
    tab.pending_selected_names.clear();
    tab.pending_selected_name.clear();
    tab.pending_ensure_selection_visible = false;
    if (!tab.snapshot || tab.SelectedCount() <= 0) return;
    for (int index : tab.SelectedIndices()) {
        if (index >= 0 && index < static_cast<int>(tab.snapshot->size()))
            tab.pending_selected_names.push_back(
                (*tab.snapshot)[static_cast<size_t>(index)].name);
    }
    if (tab.selected_index >= 0 &&
        tab.selected_index < static_cast<int>(tab.snapshot->size())) {
        tab.pending_selected_name = (*tab.snapshot)[static_cast<size_t>(tab.selected_index)].name;
    }
}

bool PathHasPendingRefresh(AppState& s, const std::wstring& path) {
    bool pending = false;
    ForEachPane(s, [&](app::Pane& pane) {
        const app::Tab* tab = pane.ActiveTab();
        if (tab && tab->current_path == path && tab->pending_generation != 0)
            pending = true;
    });
    return pending;
}

void RefreshPath(AppState& s, const std::wstring& path) {
    const std::wstring normalized = fs::NormalizePath(path);
    std::vector<app::Tab*> tabs;
    ForEachPane(s, [&](app::Pane& pane) {
        app::Tab* tab = pane.ActiveTab();
        if (tab && tab->current_path == normalized) tabs.push_back(tab);
    });
    if (tabs.empty()) return;
    if (fs::IsVirtualPath(normalized) && !fs::IsRecycleViewPath(normalized)) {
        for (app::Tab* tab : tabs) LoadVirtualView(s, *tab, normalized);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    for (app::Tab* tab : tabs) {
        CaptureListingSelection(*tab);
        tab->loading = !tab->snapshot;
        tab->pending_generation = 0;
    }
    for (app::Tab* tab : tabs) {
        if (tab->pending_generation != 0) continue;
        for (app::Tab* other : tabs) {
            if (other == tab || other->pending_generation == 0) continue;
            if (other->sort_column == tab->sort_column &&
                other->sort_direction == tab->sort_direction) {
                tab->pending_generation = other->pending_generation;
                break;
            }
        }
        if (tab->pending_generation == 0) {
            tab->pending_generation = s.worker.Refresh(
                normalized, tab->sort_column, tab->sort_direction);
        }
    }
}

void RevalidateVisibleFolders(AppState& s) {
    std::vector<std::wstring> paths = VisibleFolderPaths(s);
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    for (const std::wstring& path : paths) {
        app::Tab* empty = nullptr;
        for (auto& pane : Panes(s)) {
            app::Tab* tab = pane->ActiveTab();
            if (!tab || tab->current_path != path) continue;
            if (!tab->snapshot && !tab->loading) {
                empty = tab;
                break;
            }
        }
        if (empty) StartLoadingPath(s, *empty, path);
        else RefreshPath(s, path);
    }
}

void RefreshActiveTab(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    RefreshPath(s, tab->current_path);
}

void QueueSnapshotValidation(AppState& s, app::Tab& tab) {
    if (tab.loading || tab.current_path.empty() ||
        fs::IsVirtualPath(tab.current_path) || fs::IsUncPath(tab.current_path) ||
        tab.pending_generation != 0) {
        return;
    }
    if (s.store.IsDirty(tab.current_path)) RefreshPath(s, tab.current_path);
}

bool ApplyNotifyToVisible(AppState& s, const std::wstring& path,
                                 const fs::DirNotifyEvent& event) {
    bool any = false;
    bool need_full = false;
    fs::SnapshotPtr store_snap;
    ForEachPane(s, [&](app::Pane& pane) {
        if (need_full) return;
        app::Tab* tab = pane.ActiveTab();
        if (!tab || tab->current_path != path || !tab->snapshot) return;
        auto copy = std::make_shared<std::vector<fs::DirEntry>>(*tab->snapshot);
        if (app::ApplyDirNotify(*copy, path, event, tab->sort_column, tab->sort_direction) ==
            app::NotifyPatch::NeedFullEnum) {
            need_full = true;
            return;
        }
        std::vector<std::wstring> names;
        std::wstring focus;
        if (tab->SelectedCount() > 0) {
            for (int index : tab->SelectedIndices()) {
                if (index >= 0 && index < static_cast<int>(tab->snapshot->size()))
                    names.push_back((*tab->snapshot)[static_cast<size_t>(index)].name);
            }
            if (tab->selected_index >= 0 &&
                tab->selected_index < static_cast<int>(tab->snapshot->size())) {
                focus = (*tab->snapshot)[static_cast<size_t>(tab->selected_index)].name;
            }
        }
        if (event.action == FILE_ACTION_RENAMED_NEW_NAME && !event.old_name.empty()) {
            for (auto& name : names) {
                if (_wcsicmp(name.c_str(), event.old_name.c_str()) == 0) name = event.name;
            }
            if (_wcsicmp(focus.c_str(), event.old_name.c_str()) == 0) focus = event.name;
        }
        tab->SetSnapshot(std::move(copy));
        if (!names.empty()) tab->RemapSelection(names, focus);
        else if (tab->snapshot && !tab->snapshot->empty() && tab->selected_index < 0)
            tab->SelectOnly(0);
        if (!store_snap) store_snap = tab->snapshot;
        any = true;
    });
    if (need_full) return false;
    if (store_snap) s.store.Put(path, store_snap);
    return any;
}

void DropSizePatches(AppState& s, const std::wstring& path) {
    s.size_patches.erase(
        std::remove_if(s.size_patches.begin(), s.size_patches.end(),
                       [&](const AppState::CoalescedSizePatch& patch) {
                           return patch.path == path;
                       }),
        s.size_patches.end());
}

void QueueSizePatch(AppState& s, const std::wstring& path, const std::wstring& name,
                           ULONGLONG due) {
    for (auto& patch : s.size_patches) {
        if (patch.path == path && _wcsicmp(patch.name.c_str(), name.c_str()) == 0) {
            patch.due = due;
            patch.name = name;
            return;
        }
    }
    s.size_patches.push_back({path, name, due});
}

void DrainDirNotifies(AppState& s) {
    std::vector<AppState::DirNotifyBatch> batch;
    {
        std::lock_guard<std::mutex> lock(s.notify_mu);
        batch.swap(s.notify_queue);
    }
    bool changed = false;
    const ULONGLONG now = GetTickCount64();
    for (auto& item : batch) {
        const std::wstring path = fs::NormalizePath(item.path);
        if (item.overflow) {
            DropSizePatches(s, path);
            s.store.MarkDirty(path);
            RefreshPath(s, path);
            changed = true;
            continue;
        }
        if (PathHasPendingRefresh(s, path)) {
            s.store.MarkDirty(path);
            continue;
        }
        for (const auto& event : item.events) {
            if (event.action == FILE_ACTION_MODIFIED) {
                QueueSizePatch(s, path, event.name, now + 100);
                continue;
            }
            if (!ApplyNotifyToVisible(s, path, event)) {
                DropSizePatches(s, path);
                s.store.MarkDirty(path);
                RefreshPath(s, path);
                changed = true;
                break;
            }
            changed = true;
        }
    }

    std::vector<AppState::CoalescedSizePatch> due;
    std::vector<AppState::CoalescedSizePatch> keep;
    due.reserve(s.size_patches.size());
    keep.reserve(s.size_patches.size());
    for (auto& patch : s.size_patches) {
        if (patch.due <= now) due.push_back(std::move(patch));
        else keep.push_back(std::move(patch));
    }
    s.size_patches = std::move(keep);
    for (const auto& patch : due) {
        if (PathHasPendingRefresh(s, patch.path)) {
            s.store.MarkDirty(patch.path);
            continue;
        }
        fs::DirNotifyEvent event;
        event.action = FILE_ACTION_MODIFIED;
        event.name = patch.name;
        if (!ApplyNotifyToVisible(s, patch.path, event)) {
            s.store.MarkDirty(patch.path);
            RefreshPath(s, patch.path);
        }
        changed = true;
    }

    if (now - s.last_unc_poll >= 1000) {
        s.last_unc_poll = now;
        for (const auto& path : VisibleFolderPaths(s)) {
            if (!fs::IsUncPath(path)) continue;
            if (s.watches.Armed(path) || PathHasPendingRefresh(s, path)) continue;
            RefreshPath(s, path);
            changed = true;
        }
    }
    if (changed && s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

void RestoreNavigationReturnSelection(AppState& s, app::Tab& tab,
                                             const std::wstring& childName) {
    if (childName.empty()) return;
    tab.pending_selected_name = childName;
    tab.pending_selected_names = { childName };
    tab.pending_ensure_selection_visible = true;

    if (!tab.snapshot) return;
    for (int i = 0; i < static_cast<int>(tab.snapshot->size()); ++i) {
        const fs::DirEntry& entry = (*tab.snapshot)[static_cast<size_t>(i)];
        if (!entry.is_dir || _wcsicmp(entry.name.c_str(), childName.c_str()) != 0) continue;
        tab.SelectOnly(i);
        if (ActiveTab(s) == &tab) {
            EnsureRowVisible(s, tab, i);
            s.scrollTargetY = tab.scroll_y;
            s.scrollAnimating = false;
        }
        return;
    }
}

void NavigateTo(AppState& s, const std::wstring& path) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    std::wstring normalized = fs::NormalizePath(path);
    std::wstring next_kind;
    if (!IsAddressSearchResults(tab) && app::ParsePulsePath(normalized, &next_kind, nullptr) &&
        next_kind == L"search") {
        tab->search_origin_path = tab->current_path;
        tab->search_origin_valid = true;
    }
    const std::wstring returnedChild =
        app::NavigationReturnChildName(tab->current_path, normalized);
    tab->NavigateTo(normalized);
    StartLoadingPath(s, *tab, normalized);
    RestoreNavigationReturnSelection(s, *tab, returnedChild);
    RememberPath(s, normalized);
    RecordRecentOpen(s, normalized, app::PlaceItemKind::Folder);
    s.timing.first_frame_recorded = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void FocusPane(AppState& s, app::Pane* p) {
    if (!p || p == s.pane) return;
    if (s.addressSearching) HideAddressEditor(s, false);
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    if (s.filterEditing) HideFilterEditor(s, true);
    for (auto& pane : Panes(s)) pane->focused = (pane.get() == p);
    s.pane = p;
    RememberLayoutFocus(s);
    app::Tab* tab = p->ActiveTab();
    if (tab) {
        s.scrollTargetY = tab->scroll_y;
        s.scrollAnimating = false;
        SyncVisibleWatches(s);
        QueueSnapshotValidation(s, *tab);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ApplyLayoutPreset(AppState& s, app::LayoutPreset preset) {
    if (IsSettingsTab(ActiveTab(s)) && preset != app::LayoutPreset::Single)
        return;
    const size_t n = app::LayoutPresetCount(preset);
    std::wstring clone = L"C:\\";
    ui::ViewMode cloneView = ui::ViewMode::Details;
    std::array<float, 3> cloneColumns{};
    std::array<float, 4> cloneSearchColumns{};
    if (s.pane && s.pane->ActiveTab() && !s.pane->ActiveTab()->current_path.empty())
        clone = s.pane->ActiveTab()->current_path;
    if (s.pane && s.pane->ActiveTab()) {
        cloneView = s.pane->ActiveTab()->view_mode;
        cloneColumns = s.pane->ActiveTab()->details_column_dividers;
        cloneSearchColumns = s.pane->ActiveTab()->search_column_dividers;
    }
    while (Panes(s).size() < n) {
        auto p = std::make_unique<app::Pane>();
        p->focused = false;
        p->NewTab(clone);
        app::Tab* tab = p->ActiveTab();
        if (tab) {
            tab->view_mode = cloneView;
            tab->details_column_dividers = cloneColumns;
            tab->search_column_dividers = cloneSearchColumns;
        }
        Panes(s).push_back(std::move(p));
        if (tab) StartLoadingPath(s, *tab, clone);
    }
    std::vector<app::Pane*> used;
    used.reserve(n);
    for (size_t i = 0; i < n; ++i) used.push_back(Panes(s)[i].get());
    Root(s) = app::MakePresetTree(preset, used);
    LayoutOf(s) = preset;
    bool focusOk = false;
    for (app::Pane* p : used) if (p == s.pane) focusOk = true;
    if (!focusOk && !used.empty()) FocusPane(s, used[0]);
    if (s.targetPane) {
        bool targetOk = false;
        for (app::Pane* p : used) if (p == s.targetPane) targetOk = true;
        if (!targetOk) {
            s.targetPane = nullptr;
            for (auto& p : Panes(s)) p->target = false;
        }
    }
    SyncVisibleWatches(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void TransferToTarget(AppState& s, bool move) {
    app::Tab* src = ActiveTab(s);
    if (!src || !s.targetPane || s.targetPane == s.pane) return;
    app::Tab* dst = s.targetPane->ActiveTab();
    if (!dst || dst->current_path.empty() || fs::IsVirtualPath(dst->current_path) || dst->net_readonly) return;
    std::vector<std::wstring> paths = SelectedFullPaths(*src);
    if (paths.empty()) return;
    ops::OpRequest req;
    req.type = move ? ops::OpType::Move : ops::OpType::Copy;
    req.dest_dir = dst->current_path;
    req.sources = std::move(paths);
    if (move) s.cutPaths.clear();
    SubmitWithConflictResolution(s, std::move(req));
}

void CycleFocus(AppState& s) {
    if (!Root(s)) return;
    std::vector<app::Pane*> vis;
    Root(s)->CollectPanes(vis);
    if (vis.size() < 2) return;
    int i = 0;
    for (; i < static_cast<int>(vis.size()); ++i) if (vis[static_cast<size_t>(i)] == s.pane) break;
    FocusPane(s, vis[static_cast<size_t>((i + 1) % static_cast<int>(vis.size()))]);
}

void MarkTargetPane(AppState& s) {
    if (!s.pane) return;
    s.targetPane = s.pane;
    for (auto& p : Panes(s)) p->target = (p.get() == s.pane);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
void SortBy(AppState& s, ui::SortColumn col) {
    const app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    const auto direction = tab->sort_column == col && tab->sort_direction == ui::SortDirection::Asc
        ? ui::SortDirection::Desc : ui::SortDirection::Asc;
    SetSort(s, col, direction);
}
void SetSort(AppState& s, ui::SortColumn col, ui::SortDirection direction) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    std::wstring kind;
    if (app::ParsePulsePath(tab->current_path, &kind, nullptr) &&
        (kind == L"starred" || kind == L"recent")) {
        return;
    }
    if (tab->sort_column == col && tab->sort_direction == direction) return;
    tab->sort_column = col;
    tab->sort_direction = direction;
    RefreshActiveTab(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
void OpenSelected(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot) return;
    if (IsRecycleTab(tab)) {
        RestoreSelected(s);
        return;
    }
    const auto indices = tab->SelectedIndices();
    if (indices.empty()) return;
    if (indices.size() == 1) {
        const fs::DirEntry& e = (*tab->snapshot)[static_cast<size_t>(indices[0])];
        if (!e.link_target.empty()) {
            if (e.link_target_is_dir) NavigateTo(s, e.link_target);
            else {
                s.ops.OpenWith(e.link_target);
                RecordRecentOpen(s, e.link_target, app::PlaceItemKind::File);
            }
            return;
        }
        std::wstring full = EntryFullPath(*tab, indices[0]);
        if (e.is_dir) NavigateTo(s, full);
        else {
            s.ops.OpenWith(full);
            RecordRecentOpen(s, full, app::PlaceItemKind::File);
        }
        return;
    }
    for (int index : indices) {
        const fs::DirEntry& e = (*tab->snapshot)[static_cast<size_t>(index)];
        if (!e.link_target.empty()) {
            if (!e.link_target_is_dir) {
                s.ops.OpenWith(e.link_target);
                RecordRecentOpen(s, e.link_target, app::PlaceItemKind::File);
            }
            continue;
        }
        if (e.is_dir) continue;
        std::wstring full = EntryFullPath(*tab, index);
        if (!full.empty()) {
            s.ops.OpenWith(full);
            RecordRecentOpen(s, full, app::PlaceItemKind::File);
        }
    }
}

void GoUp(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    if (IsRecycleTab(tab)) {
        NavigateTo(s, L"");
        return;
    }
    if (fs::IsVirtualPath(tab->current_path)) {
        if (tab->CanGoBack()) GoBack(s);
        return;
    }
    const std::wstring up = fs::ParentPath(tab->current_path);
    if (_wcsicmp(up.c_str(), tab->current_path.c_str()) != 0) NavigateTo(s, up);
    else if (!tab->current_path.empty()) NavigateTo(s, L""); // drive root -> This PC
}

void GoBack(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->CanGoBack()) return;
    const std::wstring from = tab->current_path;
    std::wstring path = tab->GoBack();
    StartLoadingPath(s, *tab, path);
    RecordRecentOpen(s, path, app::PlaceItemKind::Folder);
    RestoreNavigationReturnSelection(
        s, *tab, app::NavigationReturnChildName(from, path));
    s.timing.first_frame_recorded = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void GoForward(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->CanGoForward()) return;
    std::wstring path = tab->GoForward();
    StartLoadingPath(s, *tab, path);
    RecordRecentOpen(s, path, app::PlaceItemKind::Folder);
    s.timing.first_frame_recorded = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool IsSettingsTab(const app::Tab* tab) {
    if (!tab) return false;
    std::wstring kind;
    return app::ParsePulsePath(tab->current_path, &kind, nullptr) && kind == L"settings";
}

std::wstring NewTabPath(const AppState& s) {
    const app::Tab* tab = s.pane ? s.pane->ActiveTab() : nullptr;
    if (!tab || IsSettingsTab(tab) || tab->current_path.empty()) {
        const auto recent = s.places.RecentFolderPaths(1);
        return recent.empty() ? L"C:\\" : recent.front();
    }
    return tab->current_path;
}

void NewTab(AppState& s, const std::wstring& path) {
    RememberLayoutFocus(s);
    s.window_tabs.NewTab(path.empty() ? L"C:\\" : path);
    BindCurrentLayout(s);
    if (app::Tab* tab = ActiveTab(s)) {
        StartLoadingPath(s, *tab, tab->current_path);
        RecordRecentOpen(s, tab->current_path, app::PlaceItemKind::Folder);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void OpenSettingsTab(AppState& s, int page) {
    s.settings.SelectPage(page);
    const std::wstring path = app::MakeSettingsPath(
        app::SettingsController::PageName(s.settings.page()));
    if (page == 1) s.index.RefreshVolumesAsync();
    for (size_t i = 0; i < s.window_tabs.items.size(); ++i) {
        app::LayoutTab& layout = *s.window_tabs.items[i];
        bool found = false;
        for (auto& pane : layout.panes) {
            if (IsSettingsTab(pane->ActiveTab())) { found = true; break; }
        }
        if (!found) continue;
        SwitchTab(s, i);
        ApplyLayoutPreset(s, app::LayoutPreset::Single);
        if (app::Tab* tab = ActiveTab(s)) {
            tab->current_path = path;
            tab->virtual_title = l10n::Get(l10n::StringId::Settings);
        }
        s.settings.SelectPage(page);
        if (page == 1) s.index.RefreshVolumesAsync();
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    NewTab(s, path);
    ApplyLayoutPreset(s, app::LayoutPreset::Single);
    if (app::Tab* tab = ActiveTab(s))
        tab->virtual_title = l10n::Get(l10n::StringId::Settings);
}
void CloseLayoutTab(AppState& s, size_t idx) {
    if (idx >= s.window_tabs.items.size()) return;
    RememberLayoutFocus(s);
    s.window_tabs.CloseTab(idx);
    BindCurrentLayout(s);
    RevalidateVisibleFolders(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void CloseActiveTab(AppState& s) {
    CloseLayoutTab(s, s.window_tabs.active);
}

void SwitchTab(AppState& s, size_t idx) {
    if (idx >= s.window_tabs.items.size()) return;
    if (s.addressSearching) HideAddressEditor(s, false);
    RememberLayoutFocus(s);
    s.window_tabs.SwitchTab(idx);
    BindCurrentLayout(s);
    RevalidateVisibleFolders(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool ActivateExistingFolderTab(AppState& s, const std::wstring& path) {
    if (s.window_tabs.items.empty() || path.empty()) return false;
    const std::wstring normalized = fs::NormalizePath(path);
    for (size_t i = 0; i < s.window_tabs.items.size(); ++i) {
        app::LayoutTab& layout = *s.window_tabs.items[i];
        for (auto& pane : layout.panes) {
            const app::Tab* tab = pane ? pane->ActiveTab() : nullptr;
            if (!tab || tab->current_path.empty()) continue;
            const std::wstring tab_path = fs::NormalizePath(tab->current_path);
            if (_wcsicmp(tab_path.c_str(), normalized.c_str()) != 0) continue;
            SwitchTab(s, i);
            FocusPane(s, pane.get());
            RecordRecentOpen(s, normalized, app::PlaceItemKind::Folder);
            return true;
        }
    }
    return false;
}

} // namespace pulse

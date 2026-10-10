#include <thread>
// app_navigation.cpp — extracted from app_main.cpp.
#include "app_internal.h"
#include "shell_window_sync.h"
#include "app_column_view.h"
#include "listing_selection_restore.h"
#include "content_navigation.h"
#include "content_search_snapshot.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/fluent_menu.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../ui/advanced_search_dialog.h"
#include "../ui/quick_preview_window.h"
#include "../ui/typography.h"
#include "../ui/preview_format_catalog.h"
#include "../ui/color_picker.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include "../common/diagnostics_exporter.h"
#include "snapshot_patch.h"
#include "entry_group.h"
#include "entry_order_hold.h"
#include "session.h"
#include "layout_pane_selection.h"
#include "../fs/fs_net_cache.h"
#include "context_menu.h"
#include "batch_rename.h"
#include "search_query.h"
#include "search_snapshot_hint.h"
#include "search_refresh_log.h"
#include "link_resolve.h"
#include "startup_location.h"
#include "last_tab_close.h"
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
#include <unordered_map>
#include <unordered_set>
#include <utility>

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
    if (!s.hwnd) return;
    if (s.probe_scheduler.active_id != 0 || s.probeQueue.empty()) {
        KillTimer(s.hwnd, kTimerUncProbeRetry);
        return;
    }
    const std::wstring& next = s.probeQueue.front();
    const fs::UncProbeId probe_id = s.probe_scheduler.Begin(next);
    if (!fs::StartUncProbe(s.hwnd, WM_NET_PROBE, next, probe_id)) {
        s.probe_scheduler.Cancel(next, probe_id);
        SetTimer(s.hwnd, kTimerUncProbeRetry, 1000, nullptr);
        return;
    }
    KillTimer(s.hwnd, kTimerUncProbeRetry);
    s.probeUnc = next;
    s.probeQueue.erase(s.probeQueue.begin());
}

void RequestUncProbe(AppState& s, std::wstring unc) {
    if (!fs::IsUncPath(unc) || !s.hwnd) return;
    unc = fs::NormalizePath(unc);
    if (s.probe_scheduler.active_id != 0 && s.probeUnc == unc) return;
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
        tab->selected_index < static_cast<int>(tab->EntryCount()) &&
        tab->EntryAt(static_cast<size_t>(tab->selected_index)).is_dir) {
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
        const auto visible = LiveLayout(s).VisiblePanes();
        for (size_t i = 0; i < visible.size() && i < w.pane_paths.size(); ++i) {
            const std::wstring& pth = w.pane_paths[i];
            app::Tab* t = visible[i]->ActiveTab();
            if (!t) {
                visible[i]->NewTab(pth);
                t = visible[i]->ActiveTab();
            }
            if (t && i < w.pane_views.size()) t->view_mode = w.pane_views[i];
            if (t) StartLoadingPath(s, *t, pth, PathLoadReason::RestoreSession);
        }
        RememberPath(s, w.root);
    }
    s.places.Save();
    for (const auto& n : s.places.networks) WarmupUnc(s, n.unc);
    for (const auto& pth : w.pane_paths) WarmupUnc(s, pth);
    InvalidateRect(s.hwnd, nullptr, FALSE);
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
    q.rank = tab.search_relevance;
    q.sort = index::ResultSort::Name;
    q.sort_desc = tab.sort_direction == ui::SortDirection::Desc;
    switch (tab.sort_column) {
    case ui::SortColumn::Size: q.sort = index::ResultSort::Size; break;
    case ui::SortColumn::Mtime: q.sort = index::ResultSort::Mtime; break;
    default: q.sort = index::ResultSort::Name; break;
    }
    if (tab.search_relevance) q.sort = index::ResultSort::Index;
    return q;
}

// #74: the local index never holds SMB paths, and the network index only
// answers for roots added in Settings whose crawl has finished. A name search
// scoped anywhere else on the network walks the folder live, like Explorer.
static bool NeedsLiveNetworkSearch(AppState& s, const index::Query& query) {
    if (query.path_prefix.empty() || fs::IsVirtualPath(query.path_prefix)) return false;
    if (!index::IsNetworkFolderPath(query.path_prefix)) return false;
    return !index::NetworkRootsCover(s.networkIndex.Roots(), query.path_prefix, true);
}

static void PostLiveNetworkProgress(HWND hwnd, const std::shared_ptr<LiveNetworkSearch>& live) {
    auto* holder = new std::shared_ptr<LiveNetworkSearch>(live);
    if (!PostMessageW(hwnd, WM_NETWORK_LIVE_SEARCH, 0, reinterpret_cast<LPARAM>(holder)))
        delete holder;
}

void DropLiveNetworkSearch(AppState& s, uint64_t session_id) {
    if (const auto live = s.liveNetworkSearches.Find(session_id))
        s.pendingIndexSearches.erase(live->latest_id.load());
    s.liveNetworkSearches.Drop(session_id);
}

static void StartLiveNetworkSearch(AppState& s, const index::Query& query, uint32_t id) {
    const std::wstring key = query.needle + L'\n' + query.path_prefix +
                             (query.folders_only ? L"\n1" : L"\n0") + L'\n' +
                             std::to_wstring(static_cast<uint32_t>(query.sort)) +
                             (query.sort_desc ? L"d" : L"a") + (query.rank ? L"r" : L"n");
    if (auto live = s.liveNetworkSearches.Find(query.session_id); live && index::CanReuseLiveNetworkWalk(live->query, query)) {
        // Same query and ordering (next page): answer from the walk so far; a
        // running walk keeps posting progress for the newest request.
        live->latest_id = id;
        PostLiveNetworkProgress(s.hwnd, live);
        return;
    }
    DropLiveNetworkSearch(s, query.session_id);
    auto live = std::make_shared<LiveNetworkSearch>();
    live->query = query;
    live->session_id = query.session_id;
    live->key = key;
    live->folder = query.path_prefix;
    live->latest_id = id;
    s.liveNetworkSearches.Set(live);
    std::thread([hwnd = s.hwnd, live, query] {
        index::LiveNetworkWalk(live->query, live->matches, live->mutex,
            [&live] { return live->cancel.load(); },
            [&] { PostLiveNetworkProgress(hwnd, live); });
        {
            std::lock_guard<std::mutex> lock(live->mutex);
            live->matches.finished = true;
            if (live->cancel.load()) live->matches.error = ERROR_CANCELLED;
        }
        if (!live->cancel.load()) PostLiveNetworkProgress(hwnd, live);
    }).detach();
}

void AcceptLiveNetworkProgress(AppState& s, const std::shared_ptr<LiveNetworkSearch>& live) {
    if (!s.liveNetworkSearches.Owns(live)) return;
    const uint32_t id = live->latest_id.load();
    const auto found = s.pendingIndexSearches.find(id);
    if (found == s.pendingIndexSearches.end() || found->second.network_ready ||
        found->second.query.session_id != live->session_id) return;
    const index::Query& asked = found->second.query;
    index::Query provider = asked;
    provider.offset = 0;
    provider.limit = (std::min)(index::kSearchPageCap,
        asked.offset > index::kSearchPageCap - (std::min)(asked.limit, index::kSearchPageCap)
            ? index::kSearchPageCap : asked.offset + asked.limit);
    index::SearchResult result;
    bool complete = false;
    {
        std::lock_guard<std::mutex> lock(live->mutex);
        result = index::SelectLiveNetworkHits(provider, live->matches);
        complete = live->matches.finished;
    }
    AcceptIndexProviderResult(s, id, std::move(result), true, complete);
}

static void ApplyLiveNetworkBanner(AppState& s, uint32_t id, const std::wstring& root,
                                   bool done, DWORD error) {
    ForEachPane(s, [&](app::Pane& pane) {
        app::Tab* tab = pane.ActiveTab();
        if (!tab || tab->filename_live_generation != id) return;
        if (error) {
            tab->banner_title = l10n::Get(l10n::StringId::SearchIncomplete);
            wchar_t text[128]{};
            swprintf_s(text, l10n::Get(l10n::StringId::ErrorCodeFormat).c_str(), error);
            tab->banner_message = text;
            tab->network_live_root.clear();
        } else {
            // Mapped drives normalize to their share; the button adds that share.
            const std::wstring share = index::NormalizeNetworkRoot(root);
            const bool added = !share.empty() &&
                (tab->network_live_added_root == share ||
                 index::NetworkRootsCover(s.networkIndex.Roots(), share, false));
            const bool drive = !fs::IsUncPath(root);
            if (added) {
                tab->banner_title = l10n::Get(l10n::StringId::NetworkLiveAdded);
                tab->banner_message = l10n::Get(l10n::StringId::NetworkLiveAddedMessage);
                tab->network_live_root.clear();
            } else {
                tab->banner_title = l10n::Get(l10n::StringId::NetworkLiveTitle);
                tab->banner_message = l10n::Get(!done ? l10n::StringId::NetworkLiveRunning
                    : drive ? l10n::StringId::NetworkLiveDoneDrive : l10n::StringId::NetworkLiveDone);
                // A drive-letter scope keeps walking live even once its share
                // is indexed, so only share paths offer the button.
                tab->network_live_root = drive ? std::wstring{} : share;
            }
        }
        InvalidateRect(s.hwnd, nullptr, FALSE);
    });
}

void AddLiveNetworkRoot(AppState& s, app::Tab& tab) {
    if (tab.network_live_root.empty()) return;
    std::wstring error;
    if (s.networkIndex.AddRoot(tab.network_live_root, &error)) {
        tab.network_live_added_root = tab.network_live_root;
        tab.network_live_root.clear();
        tab.banner_title = l10n::Get(l10n::StringId::NetworkLiveAdded);
        tab.banner_message = l10n::Get(l10n::StringId::NetworkLiveAddedMessage);
    } else {
        tab.banner_title = l10n::Get(l10n::StringId::NetworkLiveAddFailed);
        tab.banner_message = error.empty() ? tab.network_live_root : error;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void DispatchIndexSearch(AppState& s, const index::Query& query, uint32_t id) {
    // Both providers return candidates from the beginning of their own ordering.
    // The UI then merges, globally sorts and applies the requested page offset.
    index::Query provider_query = query;
    if (!s.appPrefs.search_pinyin) provider_query.needle = L"nopinyin: " + provider_query.needle;
    provider_query.offset = 0;
    provider_query.limit = (std::min)(index::kSearchPageCap,
        query.offset > index::kSearchPageCap - (std::min)(query.limit, index::kSearchPageCap)
            ? index::kSearchPageCap : query.offset + query.limit);
    std::erase_if(s.pendingIndexSearches, [&](const auto& item) { return item.second.query.session_id == query.session_id; });
    AppState::PendingIndexSearch pending;
    pending.query = query;
    const bool live_network = NeedsLiveNetworkSearch(s, query);
    pending.network_snapshot = app::UsesNetworkSnapshot(s.networkIndex.Roots(), query, live_network);
    pending.network_ready = !live_network && s.networkIndex.Roots().empty();
    if (live_network) pending.live_network_root = query.path_prefix;
    if (!s.appPrefs.search_pinyin) pending.query.needle = L"nopinyin: " + pending.query.needle;
    s.pendingIndexSearches.emplace(id, std::move(pending));
    s.index.SearchAsync(provider_query, id);
    if (live_network) StartLiveNetworkSearch(s, provider_query, id);
    else {
        DropLiveNetworkSearch(s, query.session_id);
        s.networkIndex.SearchAsync(provider_query, id);
    }
}

void RequestSearchPage(AppState& s, app::Tab& tab, const std::wstring& rest,
                              bool reset) {
    if (!tab.search_session_id) tab.search_session_id = ++s.nextIndexReq;
    const auto split = app::SplitSearchQueryText(rest);
    if (split.content.present() && (!reset || tab.search_content_empty)) {
        tab.loading = false;
        if (tab.search_content_empty) {
            tab.banner_title = l10n::Get(l10n::StringId::ContentIndexNoQuery);
            tab.banner_message.clear();
        }
        return;
    }
    if (split.content.present()) {
        // Only snapshots published for this query survive a replacement scan.
        // Virtual navigation can still carry the previous directory's snapshot.
        if (!tab.content_results) {
            tab.search_entries = std::make_shared<std::vector<fs::DirEntry>>();
            tab.search_snippets = std::make_shared<std::vector<std::wstring>>();
            const auto initial = app::PrepareContentSearchSnapshot(tab.snapshot,
                tab.snapshot_path, tab.current_path, tab.search_entries);
            tab.search_retaining_results = initial.retained;
            if (!initial.retained) tab.SetSnapshot(initial.snapshot);
        }
        StartIndexedContentSearch(s, tab, rest, tab.search_allow_scan);
        return;
    }
    const size_t offset = reset ? 0 : tab.search_next_offset;
    if (!reset && (tab.search_loading_more || offset >= tab.search_total ||
                   tab.search_awaiting_content || tab.search_content_active)) return;
    if (reset) {
        CancelContentSelection(s,tab);
    tab.content_results.reset();
    tab.content_selection_restore.reset();
    tab.content_count_final=false;
        tab.search_entries = std::make_shared<std::vector<fs::DirEntry>>();
        tab.search_snippets = std::make_shared<std::vector<std::wstring>>();
        if (!tab.snapshot || tab.snapshot->empty()) tab.search_total = 0;
        tab.search_next_offset = 0;
        tab.search_awaiting_content = split.content.present();
        tab.search_content_active = false;
        if (!tab.snapshot || tab.snapshot->empty()) tab.SetSnapshot(tab.search_entries);
        tab.loading = !tab.snapshot || tab.snapshot->empty();
        tab.banner_title.clear();
        tab.banner_message.clear();
        tab.network_live_root.clear();
    } else {
        tab.search_loading_more = true;
    }
    const uint32_t id = ++s.nextIndexReq;
    tab.pending_generation = id;
    tab.filename_live_generation = id;
    tab.pending_search_offset = 0;
    if (s.appPrefs.search_pinyin && !s.index.PinyinReady() && index::QueryHasPinyin(index::ParseQuery(rest))) {
        tab.banner_title = l10n::Get(l10n::StringId::SearchPinyin);
        tab.banner_message = l10n::Get(l10n::StringId::PinyinPreparing);
    }
    auto query = MakeSearchPageQuery(tab, rest, 0);
    query.limit = (std::min)(index::kSearchPageCap, offset + query.limit);
    query.session_id = tab.search_session_id; query.subscribe = true;
    DispatchIndexSearch(s, query, id);
}

void ApplySearchHits(app::Tab& tab, const std::wstring& rest,
                            index::SearchResult&& result) {
    // No request is pending for a live refresh (the index re-ran the
    // subscribed query); new pages and load-more set pending_generation.
    const bool live_refresh = tab.pending_generation == 0;
    const bool had_selection = tab.SelectedCount() > 0;
    std::vector<std::wstring> keep_paths;
    if (tab.SelectedCount() > 1) {
        for (int i : tab.SelectedIndices())
            if (i >= 0 && static_cast<size_t>(i) < tab.EntryCount())
                keep_paths.push_back(tab.EntryAt(static_cast<size_t>(i)).full_path);
    }
    if (tab.selected_index >= 0 && static_cast<size_t>(tab.selected_index) < tab.EntryCount())
        tab.search_preserve_selection = tab.EntryAt(static_cast<size_t>(tab.selected_index)).full_path;
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
    if (result.error) {
        tab.banner_title = l10n::Get(l10n::StringId::SearchIncomplete);
        wchar_t error[128]{};
        swprintf_s(error, l10n::Get(l10n::StringId::ErrorCodeFormat).c_str(), result.error);
        tab.banner_message = error;
    } else if (tab.banner_title == l10n::Get(l10n::StringId::SearchIncomplete)) {
        tab.banner_title.clear(); tab.banner_message.clear();
    }
    tab.search_total = result.total;
    tab.search_next_offset = entries.size();
    if (tab.search_snippets && tab.search_snippets->size() < entries.size())
        tab.search_snippets->resize(entries.size());
    // Index pages arrive in relevance/sort order; grouping regroups them
    // stably so that order survives inside each group.
    if (const int group = tab.EffectiveGroup(); group != 0)
        app::StableGroupOrder(entries, tab.search_snippets ? tab.search_snippets.get() : nullptr,
                              app::GroupByFromInt(group), tab.sort_column, tab.sort_direction);
    if (tab.search_total > entries.size())
        SetQuerySearchTitle(tab, rest, tab.search_total, entries.size());
    else
        SetQuerySearchTitle(tab, rest, tab.search_total);
    tab.SetSnapshot(tab.search_entries);
    tab.loading = false;
    tab.search_loading_more = false;
    tab.pending_generation = 0;
    if (offset == 0 && tab.snapshot && tab.EntryCount() != 0 && !keep_paths.empty()) {
        const std::unordered_set<std::wstring> want(keep_paths.begin(), keep_paths.end());
        std::vector<int> keep;
        int focus = -1;
        for (size_t i = 0; i < tab.EntryCount(); ++i) {
            const auto& path = tab.EntryAt(i).full_path;
            if (!want.contains(path)) continue;
            keep.push_back(static_cast<int>(i));
            if (path == tab.search_preserve_selection) focus = static_cast<int>(i);
        }
        if (!keep.empty()) {
            tab.SelectIndices(keep);
            if (focus >= 0 && tab.IsSelected(focus)) {
                tab.selected_index = focus;
                tab.selection_anchor = focus;
            }
            tab.search_preserve_selection.clear();
            return;
        }
    }
    if (live_refresh && !had_selection) {
        tab.search_preserve_selection.clear();
        return;
    }
    if (offset == 0 && tab.snapshot && tab.EntryCount() != 0) {
        int selected = 0;
        if (!tab.search_preserve_selection.empty()) {
            for (size_t i = 0; i < tab.EntryCount(); ++i)
                if (tab.EntryAt(i).full_path == tab.search_preserve_selection) { selected = static_cast<int>(i); break; }
        }
        tab.SelectOnly(selected);
        tab.search_preserve_selection.clear();
    }
}

void RequestSavedSearch(AppState& s, app::Tab& tab, size_t saved_index) {
    tab.search_content_stopped = false;
    tab.content_subscription_error = ERROR_SUCCESS;
    tab.content_subscription_failure = index::ContentSubscriptionFailure::None;
    tab.content_scan_error = ERROR_SUCCESS;
    if(!tab.search_session_id) tab.search_session_id=++s.nextIndexReq;
    const auto& saved = s.savedSearches.items();
    if (saved_index >= saved.size()) {
        tab.virtual_title = l10n::Get(l10n::StringId::SavedSearchUnavailable);
        tab.banner_title = l10n::Get(l10n::StringId::CannotOpenSavedSearch);
        tab.banner_message = l10n::Get(l10n::StringId::SavedSearchRemoved);
        tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
        return;
    }
    const app::SavedSearch& search = saved[saved_index];
    CancelContentSelection(s,tab);
    tab.content_results.reset();
    tab.content_selection_restore.reset();
    tab.content_count_final=false;
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
        query.session_id=tab.search_session_id;query.subscribe=true;tab.filename_live_generation=generation;
        query.path_prefix = search.root;
        DispatchIndexSearch(s, query, static_cast<uint32_t>(generation));
        return;
    }
    tab.search_snippets = std::make_shared<std::vector<std::wstring>>();
    tab.search_content_active = search.mode == app::SavedSearchMode::Content;
    tab.content_scanned_files=0; tab.content_total_files=0;
    index::ContentSearchRequest request;
    request.generation = generation;
    request.session_id=tab.search_session_id;
    request.mode = search.mode == app::SavedSearchMode::Duplicates
        ? index::ContentSearchMode::Duplicates : index::ContentSearchMode::Content;
    ConfigureContentSort(tab,request);
    request.paged_results = search.mode == app::SavedSearchMode::Content;
    request.task_scan = search.mode == app::SavedSearchMode::Content;
    if(search.mode == app::SavedSearchMode::Duplicates) request.maximum_hits = 10000;
    request.root = search.root;
    request.needle = search.query;
    request.indexed = search.mode == app::SavedSearchMode::Content;
    request.subscribe=request.indexed;tab.search_live_generation=request.subscribe ? generation : 0;
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
    tab.search_next_offset = entries.size();
    tab.SetSnapshot(tab.search_entries);
}

void ApplyContentSearchUpdate(AppState& s, index::ContentSearchUpdate update) {
    if (!update.progress.generation) return;
    app::Tab* target = nullptr;
    size_t saved_index = 0;
    std::wstring search_kind;
    ForEachPane(s, [&](app::Pane& pane) {
        if (target) return;
        app::Tab* tab = pane.ActiveTab();
        if (!tab || (tab->pending_generation != update.progress.generation &&
            tab->search_live_generation != update.progress.generation)) return;
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
    if (update.progress.subscription_error) {
        target->content_subscription_error = update.progress.subscription_error;
        target->content_subscription_failure = update.progress.subscription_failure;
        target->search_live_generation = 0;
        target->pending_generation = 0;
        target->search_content_active = false;
        target->loading = false;
        const auto scan_message = target->content_scan_error ? target->banner_title + L" · " + target->banner_message : std::wstring{};
        target->banner_title = l10n::Get(l10n::StringId::ContentUpdatesPaused);
        target->banner_message = l10n::Get(l10n::StringId::ContentUpdatesPausedDesc);
        if (!scan_message.empty()) target->banner_message += L" · " + scan_message;
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (!update.progress.delta) {
        target->content_scanned_files=update.progress.scanned_files;
        target->content_total_files=update.progress.total_files;
        if (update.progress.done) target->content_scan_error = update.progress.error;
    }
    if(update.results) {
        const bool fresh=target->content_results!=update.results;
        if (fresh && target->content_results && target->selected_index >= 0 &&
            static_cast<size_t>(target->selected_index) < target->EntryCount())
            target->search_preserve_selection = target->EntryAt(static_cast<size_t>(target->selected_index)).full_path;
        target->content_results=update.results;
        if(!update.progress.delta)
            target->content_count_final=update.progress.done && update.progress.error==ERROR_SUCCESS && !update.progress.truncated;
        target->search_total=update.results->Count();
        target->search_retaining_results=false;
        target->loading=!update.progress.done && target->search_total==0;
        target->file_count=target->search_total;
        if(fresh) {
            if(target->content_sort_override) {
                index::ContentSearchRequest order;
                ConfigureContentSort(*target,order);
                target->content_results->SetSort(order.sort,order.sort_desc);
            }
            target->ClearSelection(); target->content_filter.clear(); target->content_selection_restore.reset();
            target->search_entries.reset(); target->search_snippets.reset();
            target->SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
        }
        if(fresh) target->content_revision=UINT64_MAX;
    } else if (!target->content_results && (!update.hits.empty() ||
               (update.progress.done && !update.progress.error && !update.progress.truncated))) {
        AppendContentHits(*target, std::move(update.hits));
        // These rows belong to the current request, so they are usable even
        // while later batches are still arriving.
        target->search_retaining_results = false;
        target->loading = false;
    }
    if (!target->content_results && !target->search_retaining_results && !target->search_preserve_selection.empty() && target->snapshot) {
        for (size_t i = 0; i < target->EntryCount(); ++i) {
            if (target->EntryAt(i).full_path == target->search_preserve_selection) {
                target->SelectOnly(static_cast<int>(i));
                target->search_preserve_selection.clear();
                break;
            }
        }
        if (update.progress.done) target->search_preserve_selection.clear();
    }
    wchar_t item_count[64]{};
    swprintf_s(item_count, l10n::Get(l10n::StringId::ItemsCountFormat).c_str(),
               static_cast<int>(target->search_total));
    std::wstring rest;
    app::ParsePulsePath(target->current_path, nullptr, &rest);
    if (!update.progress.done && target->search_total == 0) {
        const auto needle = search_kind == L"saved-search"
            ? s.savedSearches.items()[saved_index].name : app::SearchDisplayNeedle(rest);
        wchar_t title[512]{};
        swprintf_s(title, l10n::Get(l10n::StringId::ContentFoundFormat).c_str(), needle.c_str(), target->search_total);
        target->virtual_title = title;
    } else if (search_kind == L"saved-search") {
        target->virtual_title = s.savedSearches.items()[saved_index].name + L" · " + item_count;
    } else {
        SetQuerySearchTitle(*target, rest, target->search_total);
    }
    if (!update.progress.done) {
        wchar_t scanned[96]{};
        swprintf_s(scanned, l10n::Get(l10n::StringId::ScannedFilesFormat).c_str(),
                   static_cast<unsigned long long>(update.progress.scanned_files));
        target->virtual_title += scanned;
    } else {
        target->loading = false;
        target->search_retaining_results = false;
        target->search_content_active = false;
        target->pending_generation = 0;
        if(!update.progress.delta && !update.progress.error && target->banner_title==l10n::Get(l10n::StringId::SearchIncomplete)) {
            target->banner_title.clear();target->banner_message.clear();
        }
        if (target->banner_title == l10n::Get(l10n::StringId::ContentIndexScanRunning)) {
            target->banner_title.clear(); target->banner_message.clear();
        }
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
        // A live delta is a refresh, not a new result list: never invent a
        // selection (and never pre-empt a pending path restore).
        if (!update.progress.delta && target->EntryCount() && target->selected_index < 0 &&
            target->search_preserve_selection.empty() && !target->content_selection_restore)
            target->SelectOnly(0);
    }
    if(target->content_results) RefreshContentResults(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void DeliverIndexSearchResult(AppState& s, uint32_t id,
                                      index::SearchResult&& result, bool network_snapshot) {
    if (id != 0 && id == s.advancedCountId) {
        s.advancedCountId = 0;
        if (IsWindow(s.advancedCountHwnd))
            PostMessageW(s.advancedCountHwnd, ui::kAdvancedSearchCountMessage,
                         static_cast<WPARAM>(result.total), 0);
        return;
    }
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
        if (!tab || (tab->pending_generation != id && tab->filename_live_generation != id)) return;
        std::wstring kind, rest;
        app::ParsePulsePath(tab->current_path, &kind, &rest);
        if (kind == L"search") {
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
        if (network_snapshot && tab->banner_title.empty())
            tab->banner_message = l10n::Get(l10n::StringId::NetworkSearchSnapshot);
        applied = true;
        InvalidateRect(s.hwnd, nullptr, FALSE);
    });
}

void AcceptIndexProviderResult(AppState& s, uint32_t id,
                                      index::SearchResult&& result, bool network,
                                      bool network_final) {
    auto found = s.pendingIndexSearches.find(id);
    if (found == s.pendingIndexSearches.end()) return;
    auto& pending = found->second;
    if (network) {
        pending.network = std::move(result);
        pending.network_ready = network_final;
    } else {
        pending.local = std::move(result);
        pending.local_ready = true;
    }
    if (!pending.local_ready) return;
    index::SearchResult merged = index::MergeSearchResults(pending.query, pending.local, pending.network);
    const std::wstring live_root = pending.live_network_root;
    const bool network_done = pending.network_ready;
    const DWORD live_error = pending.network.error;
    const bool network_snapshot = pending.network_snapshot;
    if (!pending.query.subscribe && pending.network_ready) s.pendingIndexSearches.erase(found);
    DeliverIndexSearchResult(s, id, std::move(merged), network_snapshot);
    if (!live_root.empty()) ApplyLiveNetworkBanner(s, id, live_root, network_done, live_error);
}

void MaybePrefetchSearchPage(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if(tab && tab->content_results) { RefreshContentResults(s); return; }
    if (!tab || tab->search_content_stopped || tab->loading || tab->search_loading_more ||
        tab->search_next_offset >= tab->search_total ||
        tab->search_awaiting_content || tab->search_content_active) return;
    std::wstring kind, rest;
    if (!app::ParsePulsePath(tab->current_path, &kind, &rest) ||
        (kind != L"search" && kind != L"saved-search")) return;
    const D2D1_RECT_F list = ListRect(s);
    const float view_h = std::max(0.0f, list.bottom - list.top);
    const float max_scroll = MaxScrollForActivePane(s);
    const float scroll_y = std::max(tab->scroll_y, s.scrollTargetY);
    if (scroll_y + view_h * 2.0f < max_scroll) return;
    if (kind == L"saved-search") {
        wchar_t* end = nullptr;
        const auto saved_index = wcstoull(rest.c_str(), &end, 10);
        if (!end || *end || saved_index >= s.savedSearches.items().size()) return;
        const auto& saved = s.savedSearches.items()[static_cast<size_t>(saved_index)];
        if (saved.mode != app::SavedSearchMode::Name) return;
        auto query = MakeSearchPageQuery(*tab, saved.query, 0);
        query.path_prefix = saved.root;
        query.limit = (std::min)(index::kSearchPageCap, tab->search_next_offset + index::kSearchUiPageSize);
        if (query.limit <= tab->search_next_offset) return;
        query.session_id = tab->search_session_id; query.subscribe = true;
        const auto id = ++s.nextIndexReq;
        tab->pending_generation = id; tab->filename_live_generation = id;
        tab->pending_search_offset = 0; tab->search_loading_more = true;
        DispatchIndexSearch(s, query, id);
    } else RequestSearchPage(s, *tab, rest, false);
}

void CancelActiveContentSearch(AppState& s, app::Tab& tab) {
    s.addressLiveDue = 0;
    if (tab.search_session_id) s.contentSearch.Cancel(tab.search_session_id);
    tab.search_live_generation = 0;
    tab.filename_live_generation = 0;
    if(tab.search_content_active) tab.content_count_final=false;
    tab.search_content_active = false;
    tab.search_awaiting_content = false;
    tab.loading = false;
    tab.pending_generation = 0;
    MarkContentSearchStopped(tab);
    std::wstring kind, rest;
    if (app::ParsePulsePath(tab.current_path, &kind, &rest) && kind == L"search")
        SetQuerySearchTitle(tab, rest, tab.search_total);
    if(tab.content_results) {tab.content_revision=UINT64_MAX;RefreshContentResults(s);}
}

static void CancelUnsharedDirectoryRequest(AppState& s, app::Tab& tab) {
    const uint64_t generation = tab.pending_generation;
    if (!generation) return;
    if (fs::IsVirtualPath(tab.current_path)) {
        std::wstring kind;
        app::ParsePulsePath(tab.current_path, &kind, nullptr);
        // Search and other virtual providers own separate generation spaces.
        // Only these views dispatch independent WorkerPool::LoadPaths jobs.
        if (kind == L"tag" || kind == L"starred" || kind == L"recent")
            s.worker.CancelGeneration(generation);
        return;
    }
    bool shared = false;
    ForEachPane(s, [&](app::Pane& pane) {
        const auto& other = pane.view;
        if (&other != &tab && !fs::IsVirtualPath(other.current_path) &&
            other.pending_generation == generation) shared = true;
        for (const auto& column : other.column_strip.ancestors)
            if (column.generation == generation) shared = true;
        if (other.column_strip.child.generation == generation) shared = true;
    });
    if (!shared) s.worker.CancelGeneration(generation);
}

void LoadVirtualView(AppState& s, app::Tab& tab, const std::wstring& path, PathLoadReason reason) {
    CancelUnsharedDirectoryRequest(s, tab);
    tab.explorer_handoff.reset();
    if (_wcsicmp(tab.current_path.c_str(), path.c_str()) != 0)
        tab.create_rename_intents.clear();
    tab.current_path = path;
    tab.loading = false;
    tab.pending_generation = 0;
    tab.virtual_title.clear();
    SyncVisibleWatches(s);

    std::wstring kind, rest;
    app::ParsePulsePath(path, &kind, &rest);
    if (kind == L"changes") {
        LoadChangeView(s, tab, rest);
        return;
    }
    if (kind == L"networks") {
        RequestNetworkLocations(s);
        FillNetworkLocationsView(s, tab);
        return;
    }
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
                tab.current_path, tag.paths, tab.sort_column, tab.sort_direction, false, {}, tab.EffectiveGroup());
            return;
        }
        tab.virtual_title = l10n::Pick(L"标签已删除", L"Tag deleted");
    } else if (kind == L"search") {
        wchar_t title[512]{};
        swprintf_s(title, l10n::Get(l10n::StringId::SearchLoadingFormat).c_str(),
                   rest.c_str());
        tab.virtual_title = title;
        tab.view_mode = ui::ViewMode::Details;
        if (reason == PathLoadReason::RestoreSession && app::SplitSearchQueryText(rest).content.present()) {
            tab.search_input_content = true;
            tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
            SetQuerySearchTitle(tab, rest, 0);
            MarkContentSearchStopped(tab);
            return;
        }
        RequestSearchPage(s, tab, rest, true);
        return;
    } else if (kind == L"saved-search") {
        wchar_t* end = nullptr;
        const unsigned long long index = wcstoull(rest.c_str(), &end, 10);
        if (end && *end == L'\0') {
            const auto& saved = s.savedSearches.items();
            if (reason == PathLoadReason::RestoreSession && index < saved.size() &&
                saved[static_cast<size_t>(index)].mode == app::SavedSearchMode::Content) {
                tab.virtual_title = saved[static_cast<size_t>(index)].name;
                tab.view_mode = ui::ViewMode::Details;
                tab.search_input_content = true;
                tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
                MarkContentSearchStopped(tab);
                return;
            }
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
            path, tab.sort_column, tab.sort_direction, tab.EffectiveGroup());
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
    if (tab.snapshot && tab.EntryCount() != 0) tab.SelectOnly(0);
}

int FolderGroupFor(const AppState& s, const std::wstring& path) {
    return static_cast<int>(s.appPrefs.folder_groups.Resolve(path));
}

// A Size sort orders folders by the totals known so far (#58); the UI timer
// re-sorts as more arrive (ResortForFolderSizes).
static std::shared_ptr<const app::FolderSizeLookup> SortFolderSizes(
    AppState& s, app::Tab& tab, const std::wstring& path) {
    if (tab.sort_column != ui::SortColumn::Size || path.empty() || fs::IsVirtualPath(path))
        return nullptr;
    auto sizes = std::make_shared<const app::FolderSizeLookup>(s.folderSizes.KnownChildren(path));
    tab.folder_size_signature = app::FolderSizeSignature(*sizes);
    tab.folder_size_resorted_at = GetTickCount64();
    return sizes;
}

// Reject a failed conversion before changing history, the current folder or
// worker state. Empty is otherwise a legitimate request to show This PC.
static bool NormalizeNavigationTarget(AppState& s, app::Tab& tab,
                                      const std::wstring& path, std::wstring& normalized) {
    normalized = fs::NormalizePath(path);
    if (path.empty() || !normalized.empty()) return true;
    tab.banner_title = l10n::Get(l10n::StringId::CannotOpen);
    tab.banner_message = l10n::Get(l10n::StringId::DirectoryUnavailable);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return false;
}

void StartLoadingPath(AppState& s, app::Tab& tab, const std::wstring& path, PathLoadReason reason) {
    std::wstring normalized;
    if (!NormalizeNavigationTarget(s, tab, path, normalized)) return;
    // Tab history may already have assigned current_path; compare the stored
    // origin instead so navigating away and back cannot revive an old request.
    (void)app::PendingCreateRenameIntent(tab.create_rename_intents, normalized, tab.view_generation);
    CancelUnsharedDirectoryRequest(s, tab);
    tab.explorer_handoff.reset();
    SyncTagGroups(s);
    DropLiveNetworkSearch(s, tab.search_session_id);
    s.index.CancelSession(tab.search_session_id);
    std::erase_if(s.pendingIndexSearches,[&](const auto& item){return item.second.query.session_id==tab.search_session_id;});
    tab.filename_live_generation=0;
    tab.search_retaining_results = false;
    tab.current_path = normalized;
    tab.group_by = FolderGroupFor(s, normalized);
    if (const auto git = s.gitRoots.find(normalized); git != s.gitRoots.end())
        tab.git_root = git->second;
    else
        tab.git_root.clear();
    tab.loading = true;
    tab.ClearSelection();
    tab.pending_selected_name.clear();
    tab.pending_preview_rename.clear();
    tab.pending_selected_names.clear();
    tab.pending_selection_revision = UINT64_MAX;
    tab.pending_ensure_selection_visible = false;
    tab.pending_generation = 0;
    tab.applied_generation = 0;
    tab.scroll_y = 0.0f;
    CancelContentSelection(s,tab);
    tab.content_results.reset();
    tab.content_selection_restore.reset();
    tab.content_count_final=false;
    tab.search_entries.reset();
    tab.search_total = 0;
    tab.search_next_offset = 0;
    tab.pending_search_offset = 0;
    if (tab.search_session_id) s.contentSearch.Cancel(tab.search_session_id);
    tab.search_live_generation = 0;
    tab.search_loading_more = false;
    tab.content_subscription_error = ERROR_SUCCESS;
    tab.content_subscription_failure = index::ContentSubscriptionFailure::None;
    tab.content_scan_error = ERROR_SUCCESS;
    tab.search_awaiting_content = false;
    tab.search_content_active = false;
    tab.search_content_stopped = false;
    tab.search_snippets.reset();
    if (reason == PathLoadReason::History && app::RestoreContentNavigation(tab)) {
        s.scrollTargetY = tab.scroll_y;
        s.scrollAnimating = false;
        SyncVisibleWatches(s);
        return;
    }
    if (fs::IsVirtualPath(normalized)) {
        LoadVirtualView(s, tab, normalized, reason);
        return;
    }
    tab.virtual_title.clear();
    tab.banner_title.clear();
    tab.banner_message.clear();
    if (!s.shot.active) {
        const auto saved = s.appPrefs.folder_views.Find(normalized);
        const auto mode = saved.value_or(reason == PathLoadReason::RestoreSession
            ? tab.view_mode : s.appPrefs.folder_views.Default());
        // Migrate older sessions/workspaces so returning later keeps their mode.
        if (!saved && reason == PathLoadReason::RestoreSession)
            s.appPrefs.folder_views.Set(normalized, mode);
        if (tab.view_mode != mode) {
            tab.view_mode = mode;
            ++tab.view_generation;
        }
        tab.scroll_x = 0.0f;
        // Sort order is remembered per folder (#26); sessions never stored it,
        // so there is nothing to migrate. Set before the load request below.
        const app::FolderSort sort = s.appPrefs.folder_sorts.Find(normalized)
            .value_or(s.appPrefs.folder_sorts.Default());
        tab.sort_column = sort.column;
        tab.sort_direction = sort.direction;
    }
    tab.net_readonly = false;
    tab.cache_unix = 0;
    // Opening a folder shows it in sort order again (#13).
    tab.refresh_keeps_order = false;
    tab.order_held = false;
    tab.held_renames.clear();
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
            other->sort_direction != tab.sort_direction ||
            other->EffectiveGroup() != tab.EffectiveGroup()) {
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
        if (tab.snapshot && tab.EntryCount() != 0) tab.FocusFirstEntry();
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
        : s.worker.Refresh(normalized, tab.sort_column, tab.sort_direction, tab.EffectiveGroup(),
                           SortFolderSizes(s, tab, normalized));

    SyncVisibleWatches(s);
    if (fs::IsUncPath(normalized)) RequestUncProbe(s, normalized);
}

void ApplyWorkerResult(AppState& s, app::WorkResult& res) {
    if (Panes(s).empty()) return;
    NoteColumnStripResult(s, res);
    if (res.error) {
        ForEachPane(s, [&](app::Pane& pane) {
            app::Tab* tab = pane.ActiveTab();
            if (!tab || tab->current_path != res.path) return;
            if (tab->pending_generation != res.generation) return;
            tab->loading = false;
            tab->pending_generation = 0;
            tab->net_readonly = fs::IsUncPath(res.path);
            CompleteExplorerNavigation(*tab, res.generation, false);
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
        if (tab->pending_generation != res.generation) return;
        if (tab->applied_generation != 0 && res.generation < tab->applied_generation) return;
        any = true;

        const bool focusedTab = (&pane == s.pane);
            auto selection_restore = app::TakeListingSelection(*tab, res.path);
            auto& pendingNames = selection_restore.names;
            auto& pendingFocus = selection_restore.focus;
            bool ensurePendingVisible = selection_restore.ensure_visible;
            std::wstring renameTarget;
            const bool hasCreateRename = app::PendingCreateRenameIntent(tab->create_rename_intents,
                tab->current_path, tab->view_generation).has_value();
            const auto previewRenameTarget = std::exchange(tab->pending_preview_rename, {});
            const bool previewRename = focusedTab && !previewRenameTarget.empty();
            if (focusedTab) {
                renameTarget = previewRename ? previewRenameTarget : std::wstring{};
                if (renameTarget.empty() && s.renameIndex >= 0 && tab->snapshot &&
                    s.renameIndex < static_cast<int>(tab->EntryCount())) {
                    renameTarget = tab->EntryAt(s.renameIndex).name;
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
            // A background refresh keeps the rows where they are; F5 and a
            // new sort take the worker's order as is (#13).
            if (res.snapshot && !tab->held_renames.empty())
                app::FollowHeldRenames(tab->held_renames, *res.snapshot, pendingNames, pendingFocus);
            fs::SnapshotPtr listing = res.snapshot;
            const bool keep_order = tab->refresh_keeps_order && res.snapshot && tab->snapshot &&
                tab->snapshot_path == res.path && !fs::IsVirtualPath(res.path);
            if (keep_order) {
                const app::ScopedEntryGrouping grouping(tab->EffectiveGroup(), tab->current_path);
                listing = std::make_shared<std::vector<fs::DirEntry>>(app::KeepEntryOrder(
                    *tab->snapshot, *res.snapshot, tab->held_renames,
                    tab->sort_column, tab->sort_direction));
            }
            tab->order_held = keep_order;
            tab->refresh_keeps_order = false;
            if (keep_order) app::PruneHeldRenames(tab->held_renames, *res.snapshot);
            else tab->held_renames.clear();
            tab->SetSnapshot(listing);
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

            std::optional<app::CreateRenameSelection> createRename;
            if (hasCreateRename && focusedTab && !previewRename && s.renameIndex < 0 && tab->snapshot) {
                for (size_t i = 0; i < tab->EntryCount(); ++i) {
                    const auto& entry = tab->EntryAt(i);
                    const app::CreateRenameEntry candidate{entry.name, entry.full_path};
                    createRename = app::SelectCreateRenameIntent(tab->create_rename_intents,
                        tab->current_path, tab->view_generation, {&candidate, 1}, focusedTab, false);
                    if (createRename) {
                        createRename->entry_index = i;
                        break;
                    }
                }
            }
            const bool createdRename = createRename.has_value();
            bool startedRename = false;
            if (focusedTab && (createdRename || !renameTarget.empty()) && tab->snapshot) {
                for (int i = 0; i < static_cast<int>(tab->EntryCount()); ++i) {
                    const auto& entry = tab->EntryAt(i);
                    if (createdRename ? createRename->entry_index != static_cast<size_t>(i)
                                      : entry.name != renameTarget) continue;
                    tab->SelectOnly(i);
                    EnsureRowVisible(s, *tab, i);
                    if (s.renameIndex >= 0) {
                        s.renameIndex = i;
                        LayoutRenameOverlay(s);
                    } else if (focusedTab) {
                        ShowRenameOverlay(s);
                    }
                    startedRename = s.renameIndex == i;
                    if (createdRename && startedRename)
                        app::ConsumeCreateRenameIntent(tab->create_rename_intents, createRename->task_id);
                    break;
                }
                if (!startedRename) {
                    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
                }
            }
            if (!startedRename) {
                app::RestoreListingSelection(*tab, selection_restore, &pane, &s.places);
            }
            // A launcher's "open file location" (or another app's equivalent)
            // names the entry it wants focused. Apply it once its listing is
            // here, then forget it, so a later refresh does not drag the cursor
            // back to a stale name.
            if (!startedRename && !tab->launch_selected_name.empty()) {
                const std::wstring wanted = std::exchange(tab->launch_selected_name, {});
                for (size_t i = 0; i < tab->EntryCount(); ++i) {
                    if (_wcsicmp(tab->EntryAt(i).name.c_str(), wanted.c_str()) != 0) continue;
                    tab->SelectOnly(static_cast<int>(i));
                    ensurePendingVisible = true;
                    break;
                }
            }
            if (focusedTab && ensurePendingVisible && tab->selected_index >= 0) {
                EnsureRowVisible(s, *tab, tab->selected_index);
                s.scrollTargetY = tab->scroll_y;
                s.scrollAnimating = false;
            }
            CompleteExplorerNavigation(*tab, res.generation, !res.cancelled && res.snapshot != nullptr);
    });
    if (again) RefreshPath(s, res.path);
    if (!any) return;
    SyncQuickPreview(s);

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
    app::CapturePendingListingSelection(tab);
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

void RefreshPath(AppState& s, const std::wstring& path, RefreshReason reason) {
    const std::wstring normalized = fs::NormalizePath(path);
    if (!path.empty() && normalized.empty()) return;
    std::vector<app::Tab*> tabs;
    ForEachPane(s, [&](app::Pane& pane) {
        app::Tab* tab = pane.ActiveTab();
        if (tab && tab->current_path == normalized) tabs.push_back(tab);
    });
    if (tabs.empty()) return;
    if (fs::IsVirtualPath(normalized) && !fs::IsRecycleViewPath(normalized)) {
        for (app::Tab* tab : tabs) {
            if (tab->search_content_stopped && reason != RefreshReason::Explicit) continue;
            // The subscribed session consumes changes since its initial snapshot,
            // including changes committed while its first scan is still running.
            const bool retain = reason != RefreshReason::Explicit &&
                (tab->search_live_generation != 0 || tab->content_subscription_error != ERROR_SUCCESS);
            if (tab->search_live_generation || tab->search_content_active || tab->content_results)
                LogSearchRefresh(reason, retain, tab->search_session_id,
                    tab->search_live_generation ? tab->search_live_generation : tab->pending_generation,
                    tab->search_content_active);
            if (retain) RefreshContentResults(s);
            else LoadVirtualView(s, *tab, normalized);
        }
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    for (app::Tab* tab : tabs) {
        // F5 re-sorts; anything else merges into the rows on screen unless an
        // in-flight load (navigation, F5) already wants sort order (#13).
        if (reason == RefreshReason::Explicit) tab->refresh_keeps_order = false;
        else if (tab->pending_generation == 0) tab->refresh_keeps_order = true;
        const bool reveal = reason == RefreshReason::Explicit && tab->order_held;
        CaptureListingSelection(*tab);
        // The selection usually moved when the held rows re-sort; follow it.
        if (reveal && tab->SelectedCount() > 0) tab->pending_ensure_selection_visible = true;
        tab->loading = !tab->snapshot;
        tab->pending_generation = 0;
    }
    for (app::Tab* tab : tabs) {
        if (tab->pending_generation != 0) continue;
        for (app::Tab* other : tabs) {
            if (other == tab || other->pending_generation == 0) continue;
            if (other->sort_column == tab->sort_column &&
                other->sort_direction == tab->sort_direction &&
                other->EffectiveGroup() == tab->EffectiveGroup()) {
                tab->pending_generation = other->pending_generation;
                break;
            }
        }
        if (tab->pending_generation == 0) {
            // The new pass covers what was marked dirty so far. Without this the
            // mark that queued it survived, and its result queued one more full
            // enumeration (three per folder open with the watch's first report).
            s.store.BeginRefresh(normalized);
            tab->pending_generation = s.worker.Refresh(
                normalized, tab->sort_column, tab->sort_direction, tab->EffectiveGroup(),
                SortFolderSizes(s, *tab, normalized));
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

void RefreshActiveTab(AppState& s, RefreshReason reason) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    // F5 or a finished file operation must see the share as it is now (#74).
    if (reason == RefreshReason::Explicit || reason == RefreshReason::OperationCompleted)
        DropLiveNetworkSearch(s, tab->search_session_id);
    RefreshPath(s, tab->current_path, reason);
}

void QueueSnapshotValidation(AppState& s, app::Tab& tab) {
    if (tab.loading || tab.current_path.empty() ||
        fs::IsVirtualPath(tab.current_path) || fs::IsUncPath(tab.current_path) ||
        tab.pending_generation != 0) {
        return;
    }
    if (s.store.IsDirty(tab.current_path) || app::PendingCreateRenameIntent(
        tab.create_rename_intents, tab.current_path, tab.view_generation))
        RefreshPath(s, tab.current_path);
}

// Applies a run of change events for one folder to every visible tab showing
// it, copying each snapshot once. Returns false when a full enumeration should
// run instead: an unsupported event, no visible tab, or more patch work than a
// background re-enumeration costs.
static bool ApplyNotifiesToVisible(AppState& s, const std::wstring& path,
                                   const std::vector<fs::DirNotifyEvent>& events) {
    if (events.empty()) return true;
    // The batch is merged in one O(entries + events log events) pass, but every
    // touched file is stat'ed on this thread, so huge bursts go async instead.
    constexpr size_t kMaxIncrementalEvents = 512;
    if (events.size() > kMaxIncrementalEvents) return false;
    bool any = false;
    bool need_full = false;
    fs::SnapshotPtr store_snap;
    ForEachPane(s, [&](app::Pane& pane) {
        if (need_full) return;
        app::Tab* tab = pane.ActiveTab();
        if (!tab || tab->current_path != path || !tab->snapshot) return;
        auto copy = std::make_shared<std::vector<fs::DirEntry>>(*tab->snapshot);
        const app::ScopedEntryGrouping grouping(tab->EffectiveGroup(), tab->current_path);
        if (app::ApplyDirNotifyBatch(*copy, path, events, tab->sort_column, tab->sort_direction) ==
            app::NotifyPatch::NeedFullEnum) {
            need_full = true;
            return;
        }
        std::vector<std::wstring> names;
        std::wstring focus;
        if (tab->SelectedCount() > 0) {
            for (int index : tab->SelectedIndices()) {
                if (index >= 0 && index < static_cast<int>(tab->EntryCount()))
                    names.push_back(tab->EntryAt(static_cast<size_t>(index)).name);
            }
            if (tab->selected_index >= 0 &&
                tab->selected_index < static_cast<int>(tab->EntryCount())) {
                focus = tab->EntryAt(static_cast<size_t>(tab->selected_index)).name;
            }
        }
        for (const auto& event : events) {
            if (event.action != FILE_ACTION_RENAMED_NEW_NAME || event.old_name.empty()) continue;
            for (auto& name : names) {
                if (_wcsicmp(name.c_str(), event.old_name.c_str()) == 0) name = event.name;
            }
            if (_wcsicmp(focus.c_str(), event.old_name.c_str()) == 0) focus = event.name;
        }
        app::ListingSelectionRestore selection_restore{
            std::move(names), std::move(focus), false, false, tab->snapshot, tab->selected_index};
        tab->SetSnapshot(std::move(copy));
        tab->order_held = true;
        if (!selection_restore.names.empty()) app::RestoreListingSelection(*tab, selection_restore, &pane, &s.places);
        else if (tab->snapshot && tab->EntryCount() != 0 && tab->selected_index < 0)
            tab->FocusFirstEntry();
        if (!store_snap) store_snap = tab->snapshot;
        any = true;
    });
    if (need_full) return false;
    if (store_snap) s.store.Put(path, store_snap);
    if (any) SyncQuickPreview(s);
    return any;
}

bool ApplyNotifyToVisible(AppState& s, const std::wstring& path,
                                 const fs::DirNotifyEvent& event) {
    return ApplyNotifiesToVisible(s, path, std::vector<fs::DirNotifyEvent>{ event });
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

// Batch form of QueueSizePatch: a bulk write queues thousands of MODIFIED
// names, and the one-by-one scan above is quadratic in that case.
static void QueueSizePatches(AppState& s, const std::wstring& path,
                             const std::vector<std::wstring>& names, ULONGLONG due) {
    constexpr size_t kLinearLimit = 8;
    if (names.size() <= kLinearLimit) {
        for (const auto& name : names) QueueSizePatch(s, path, name, due);
        return;
    }
    const auto fold = [](const std::wstring& name) {
        std::wstring key(name);
        for (auto& c : key) c = static_cast<wchar_t>(std::towlower(c));
        return key;
    };
    std::unordered_map<std::wstring, size_t> index;
    index.reserve(s.size_patches.size() + names.size());
    for (size_t i = 0; i < s.size_patches.size(); ++i) {
        if (s.size_patches[i].path == path) index.emplace(fold(s.size_patches[i].name), i);
    }
    for (const auto& name : names) {
        std::wstring key = fold(name);
        const auto found = index.find(key);
        if (found != index.end()) {
            auto& patch = s.size_patches[found->second];
            patch.due = due;
            patch.name = name;
        } else {
            index.emplace(std::move(key), s.size_patches.size());
            s.size_patches.push_back({path, name, due});
        }
    }
}

template <typename T>
static T& GroupFor(std::vector<std::pair<std::wstring, T>>& groups, const std::wstring& key) {
    for (auto& group : groups) {
        if (group.first == key) return group.second;
    }
    groups.emplace_back(key, T{});
    return groups.back().second;
}

void DrainDirNotifies(AppState& s) {
    std::vector<AppState::DirNotifyBatch> batch;
    {
        std::lock_guard<std::mutex> lock(s.notify_mu);
        batch.swap(s.notify_queue);
    }
    bool changed = false;
    const ULONGLONG now = GetTickCount64();
    // The watcher queues one batch per notification buffer; during a slow bulk
    // copy that is one or two events each, so thousands can pile up behind a
    // single tick. Group the whole drain per folder and patch each folder once.
    std::vector<std::pair<std::wstring, std::vector<fs::DirNotifyEvent>>> structural_by_path;
    std::vector<std::pair<std::wstring, std::vector<std::wstring>>> modified_by_path;
    for (auto& item : batch) {
        const std::wstring path = fs::NormalizePath(item.path);
        if (item.overflow) {
            DropSizePatches(s, path);
            s.store.MarkDirty(path);
            // A scan already in flight (a newly armed watch reports one overflow
            // while the folder is still loading) finishes and is shown first; the
            // dirty mark then runs one more pass instead of discarding its work.
            if (!PathHasPendingRefresh(s, path)) RefreshPath(s, path);
            // The refresh re-enumerates; this drain's earlier events are moot.
            GroupFor(structural_by_path, path).clear();
            GroupFor(modified_by_path, path).clear();
            changed = true;
            continue;
        }
        if (PathHasPendingRefresh(s, path)) {
            s.store.MarkDirty(path);
            continue;
        }
        auto& structural = GroupFor(structural_by_path, path);
        auto& modified = GroupFor(modified_by_path, path);
        for (const auto& event : item.events) {
            if (event.action == FILE_ACTION_MODIFIED) modified.push_back(event.name);
            else structural.push_back(event);
        }
    }
    for (const auto& [folder, names] : modified_by_path)
        QueueSizePatches(s, folder, names, now + 100);
    for (const auto& [folder, events] : structural_by_path) {
        if (events.empty()) continue;
        if (!ApplyNotifiesToVisible(s, folder, events)) {
            DropSizePatches(s, folder);
            s.store.MarkDirty(folder);
            RefreshPath(s, folder);
        }
        changed = true;
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
    // One snapshot copy per folder, not per modified file.
    std::vector<std::pair<std::wstring, std::vector<fs::DirNotifyEvent>>> due_by_path;
    for (const auto& patch : due) {
        if (PathHasPendingRefresh(s, patch.path)) {
            s.store.MarkDirty(patch.path);
            continue;
        }
        fs::DirNotifyEvent event;
        event.action = FILE_ACTION_MODIFIED;
        event.name = patch.name;
        size_t group = 0;
        while (group < due_by_path.size() && due_by_path[group].first != patch.path) ++group;
        if (group == due_by_path.size())
            due_by_path.emplace_back(patch.path, std::vector<fs::DirNotifyEvent>{});
        due_by_path[group].second.push_back(std::move(event));
    }
    for (const auto& [patch_path, events] : due_by_path) {
        if (!ApplyNotifiesToVisible(s, patch_path, events)) {
            s.store.MarkDirty(patch_path);
            RefreshPath(s, patch_path);
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
    tab.pending_selection_revision = tab.selection_revision;
    tab.pending_ensure_selection_visible = true;
    tab.pending_selection_revision = tab.selection_revision;

    if (!tab.snapshot) return;
    for (int i = 0; i < static_cast<int>(tab.EntryCount()); ++i) {
        const fs::DirEntry& entry = tab.EntryAt(static_cast<size_t>(i));
        if (!entry.is_dir || _wcsicmp(entry.name.c_str(), childName.c_str()) != 0) continue;
        tab.SelectOnly(i);
        tab.pending_selection_revision = tab.selection_revision;
        if (ActiveTab(s) == &tab) {
            EnsureRowVisible(s, tab, i);
            s.scrollTargetY = tab.scroll_y;
            s.scrollAnimating = false;
        }
        return;
    }
}

void NavigateTo(AppState& s, const std::wstring& path) {
    if (OpenSystemNetworkShortcut(s, path)) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    std::wstring normalized;
    if (!NormalizeNavigationTarget(s, *tab, path, normalized)) return;
    if (normalized != tab->current_path && !fs::IsVirtualPath(normalized) && tab->content_results) {
        s.addressLiveDue = s.addressHistoryDue = 0;
        if (s.addressSearching) HideAddressEditor(s, false);
        if (tab->search_content_active) CancelActiveContentSearch(s, *tab);
        app::RememberContentNavigation(*tab);
    }
    const std::wstring returnedChild =
        app::NavigationReturnChildName(tab->current_path, normalized);
    tab->NavigateTo(normalized);
    RecordSearchHistory(s, normalized);
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
    ui::DetailsColumnWidths cloneColumns{};
    std::array<float, 4> cloneSearchColumns{};
    // New panes clone the focused folder, This PC included: its path is empty,
    // and it used to fall back to C:\ (#68).
    if (s.pane && s.pane->ActiveTab())
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
    std::vector<app::Pane*> visible;
    if (Root(s)) Root(s)->CollectPanes(visible);
    const auto used = app::SelectLayoutPanes(Panes(s), s.pane, n, visible);
    Root(s) = app::MakePresetTree(preset, used);
    LayoutOf(s) = preset;
    if (n != 2) {
        if (app::LayoutTab* lt = s.window_tabs.Active()) {
            lt->compare = false;
            lt->compare_diff_only = false;
        }
    }
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
    RememberLayoutFocus(s);
    SyncVisibleWatches(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void SwapSplitPanes(AppState& s) {
    app::SplitContainer* root = Root(s).get();
    if (!root || root->is_leaf || !root->first || !root->second ||
        !root->first->is_leaf || !root->second->is_leaf) return;
    app::Pane* a = root->first->pane;
    app::Pane* b = root->second->pane;
    if (!a || !b || a == b) return;
    std::swap(root->first->pane, root->second->pane);
    // Ownership order decides the order when the layout changes later and on
    // restore, so it follows the tree.
    auto& owned = Panes(s);
    const auto find = [&owned](const app::Pane* pane) {
        return std::find_if(owned.begin(), owned.end(),
                            [pane](const std::unique_ptr<app::Pane>& p) { return p.get() == pane; });
    };
    const auto ia = find(a);
    const auto ib = find(b);
    if (ia != owned.end() && ib != owned.end()) std::iter_swap(ia, ib);
    // Compare inputs are recorded per side; swapping keeps the last pass valid.
    if (app::LayoutTab* lt = s.window_tabs.Active())
        std::swap(lt->compare_snap_a, lt->compare_snap_b);
    s.hoverPaneIndex = -1;
    s.dropPaneIndex = -1;
    RememberLayoutFocus(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Two-pane folder compare
// ---------------------------------------------------------------------------
namespace {
bool CompareFolderPath(const app::Tab* tab) {
    return tab && tab->snapshot && !tab->content_results && !tab->current_path.empty() &&
           !fs::IsVirtualPath(tab->current_path);
}

std::vector<app::Pane*> VisiblePanes(AppState& s) {
    std::vector<app::Pane*> visible;
    if (Root(s)) Root(s)->CollectPanes(visible);
    return visible;
}

void ClearCompare(app::Tab& tab) {
    if (!tab.compare_marks && !tab.compare_diff_only) return;
    tab.compare_marks.reset();
    tab.compare_counts = {};
    tab.compare_diff_only = false;
    tab.view_compare_map.reset();
    tab.view_compare_base.reset();
    tab.view_compare_marks.reset();
}
} // namespace

bool FolderCompareAvailable(AppState& s) {
    const auto visible = VisiblePanes(s);
    return visible.size() == 2 && visible[0] && visible[1];
}

// Called at the start of every view-model build: cheap unless one of the two
// listings (or the hidden-file visibility) changed since the last pass.
void UpdateFolderCompare(AppState& s) {
    app::LayoutTab* lt = s.window_tabs.Active();
    if (!lt) return;
    const auto visible = VisiblePanes(s);
    app::Tab* a = nullptr;
    app::Tab* b = nullptr;
    bool active = lt->compare && visible.size() == 2 && visible[0] && visible[1];
    if (active) {
        a = visible[0]->ActiveTab();
        b = visible[1]->ActiveTab();
        active = CompareFolderPath(a) && CompareFolderPath(b);
    }
    for (auto& pane : lt->panes) {
        app::Tab* tab = pane ? pane->ActiveTab() : nullptr;
        if (tab && (!active || (tab != a && tab != b))) ClearCompare(*tab);
    }
    if (!active) {
        lt->compare_snap_a.reset();
        lt->compare_snap_b.reset();
        lt->compare_visibility = -1;
        return;
    }
    a->compare_diff_only = lt->compare_diff_only;
    b->compare_diff_only = lt->compare_diff_only;
    const int visibility = (a->show_hidden_files ? 1 : 0) | (a->show_protected_os_files ? 2 : 0) |
                           (b->show_hidden_files ? 4 : 0) | (b->show_protected_os_files ? 8 : 0);
    if (a->compare_marks && b->compare_marks && lt->compare_snap_a == a->snapshot &&
        lt->compare_snap_b == b->snapshot && lt->compare_visibility == visibility)
        return;
    app::FolderCompareResult result = app::CompareFolderTabs(*a, *b);
    a->compare_marks = std::move(result.marks_a);
    b->compare_marks = std::move(result.marks_b);
    a->compare_counts = result.counts_a;
    b->compare_counts = result.counts_b;
    lt->compare_snap_a = a->snapshot;
    lt->compare_snap_b = b->snapshot;
    lt->compare_visibility = visibility;
}

static void ResetCompareScroll(AppState& s) {
    for (app::Pane* pane : VisiblePanes(s)) {
        if (app::Tab* tab = pane ? pane->ActiveTab() : nullptr) tab->scroll_y = 0.0f;
    }
    s.scrollTargetY = 0.0f;
    s.scrollAnimating = false;
}

void SetFolderCompare(AppState& s, bool on) {
    app::LayoutTab* lt = s.window_tabs.Active();
    if (!lt) return;
    if (on && !FolderCompareAvailable(s)) return;
    if (lt->compare_diff_only) ResetCompareScroll(s);
    lt->compare = on;
    lt->compare_diff_only = false;
    lt->compare_visibility = -1;
    UpdateFolderCompare(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ToggleCompareDiffOnly(AppState& s) {
    app::LayoutTab* lt = s.window_tabs.Active();
    if (!lt || !lt->compare) return;
    lt->compare_diff_only = !lt->compare_diff_only;
    ResetCompareScroll(s);
    UpdateFolderCompare(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Exactly two selected real folders -> their paths (else empty).
std::vector<std::wstring> SideBySideFolders(const app::Tab& tab) {
    std::vector<std::wstring> out;
    if (!tab.snapshot || tab.SelectedCount() != 2) return out;
    for (int index : tab.SelectedIndices()) {
        if (index < 0 || index >= static_cast<int>(tab.EntryCount())) return {};
        const fs::DirEntry& entry = tab.EntryAt(static_cast<size_t>(index));
        if (!entry.is_dir || entry.change_record_only || !entry.recycle_path.empty()) return {};
        std::wstring path = EntryFullPath(tab, index);
        if (path.empty() || fs::IsVirtualPath(path)) return {};
        out.push_back(std::move(path));
    }
    if (out.size() != 2) out.clear();
    return out;
}

// 并排对比: split left | right, A on the left, B on the right, compare on.
void OpenFoldersSideBySide(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || IsSettingsTab(tab)) return;
    const std::vector<std::wstring> folders = SideBySideFolders(*tab);
    if (folders.size() != 2) return;
    ApplyLayoutPreset(s, app::LayoutPreset::TwoVertical);
    const auto visible = VisiblePanes(s);
    if (visible.size() != 2 || !visible[0] || !visible[1]) return;
    FocusPane(s, visible[1]);
    NavigateTo(s, folders[1]);
    FocusPane(s, visible[0]);
    NavigateTo(s, folders[0]);
    if (app::LayoutTab* lt = s.window_tabs.Active()) {
        lt->compare = true;
        lt->compare_diff_only = false;
        lt->compare_visibility = -1;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void TransferToTarget(AppState& s, bool move) {
    if(DeferContentSelection(s,[=](AppState& v){TransferToTarget(v,move);})) return;
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
    const bool was_relevance = tab->search_relevance;
    tab->search_relevance = false;
    if (!was_relevance && tab->sort_column == col && tab->sort_direction == direction) return;
    tab->sort_column = col;
    tab->sort_direction = direction;
    // Real folders remember the choice; virtual views are rejected by the key
    // and search result orders never become a folder preference.
    if (!tab->content_results && !tab->search_content_active &&
        s.appPrefs.folder_sorts.Set(tab->current_path, { col, direction }))
        s.appPrefs.Save();
    if(tab->content_results) {
        if(tab->selected_index>=0 && static_cast<size_t>(tab->selected_index)<tab->EntryCount())
            tab->search_preserve_selection=tab->EntryAt(static_cast<size_t>(tab->selected_index)).full_path;
        tab->ClearSelection();
        index::ContentSearchRequest order;
        ConfigureContentSort(*tab,order);
        tab->content_sort_override=true;
        tab->content_results->SetSort(order.sort,order.sort_desc);
        RefreshContentResults(s);
    } else if (tab->search_content_active) {
        // Apply the chosen order when the first result store arrives.
        tab->content_sort_override = true;
    } else RefreshActiveTab(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
void SetGroupBy(AppState& s, int group_by) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->content_results) return;
    std::wstring kind;
    const bool pulse_view = app::ParsePulsePath(tab->current_path, &kind, nullptr);
    const bool recent = pulse_view && kind == L"recent";
    // Search results and tag views span folders: they group by Location.
    // The recycle bin groups by deleted date (mtime) or original folder.
    const bool multi = pulse_view && (kind == L"search" || kind == L"saved-search" || kind == L"tag" ||
                                      kind == L"recycle");
    if (!recent && !multi && (tab->current_path.empty() || fs::IsVirtualPath(tab->current_path))) return;
    const app::GroupBy by = app::GroupByFromInt(group_by);
    // Tag groups need one parent folder; Recent keeps its opened-time order.
    if ((recent || multi) && by == app::GroupBy::Tag) return;
    if (!multi && by == app::GroupBy::Location) return;
    SyncTagGroups(s);
    if (s.appPrefs.folder_groups.Set(tab->current_path, by)) s.appPrefs.Save();
    group_by = static_cast<int>(by);
    // Groups are drawn in list-like views; bring an icon view to Details.
    if (group_by != 0 && tab->view_mode != ui::ViewMode::Details &&
        tab->view_mode != ui::ViewMode::Content)
        SetViewMode(s, ui::ViewMode::Details);
    const int old_group = tab->EffectiveGroup();
    tab->group_by = group_by;
    if (tab->EffectiveGroup() == old_group) {
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    tab->scroll_y = 0.0f;
    s.scrollTargetY = 0.0f;
    s.scrollAnimating = false;
    // Recent keeps its opened-time order (already date-contiguous); folders re-sort.
    if (!recent) RefreshActiveTab(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void OpenSelected(AppState& s) {
    if(DeferContentSelection(s,[=](AppState& v){OpenSelected(v);})) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot) return;
    if (IsRecycleTab(tab)) {
        RestoreSelected(s);
        return;
    }
    const auto indices = tab->SelectedIndices();
    if (indices.empty()) return;
    if (indices.size() == 1) {
        const fs::DirEntry& e = tab->EntryAt(static_cast<size_t>(indices[0]));
        if (e.change_record_only) return;
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
        const fs::DirEntry& e = tab->EntryAt(static_cast<size_t>(index));
        if (e.change_record_only) continue;
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

void OpenPath(AppState& s, const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        NavigateTo(s, path);
    } else {
        s.ops.OpenWith(path);
        RecordRecentOpen(s, path, app::PlaceItemKind::File);
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
    // ParentPath answers in normalized form; compare like with like so an
    // unnormalized C:\ is still seen as a root (#68).
    const std::wstring current = fs::NormalizePath(tab->current_path);
    const std::wstring up = fs::ParentPath(current);
    if (_wcsicmp(up.c_str(), current.c_str()) != 0) NavigateTo(s, up);
    else if (!current.empty()) NavigateTo(s, L""); // drive root -> This PC
}

void GoBack(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->CanGoBack()) return;
    s.addressLiveDue = s.addressHistoryDue = 0;
    if (s.addressSearching) HideAddressEditor(s, false);
    if (tab->search_content_active) CancelActiveContentSearch(s, *tab);
    app::RememberContentNavigation(*tab);
    const std::wstring from = tab->current_path;
    std::wstring path = tab->GoBack();
    StartLoadingPath(s, *tab, path, PathLoadReason::History);
    RecordRecentOpen(s, path, app::PlaceItemKind::Folder);
    RestoreNavigationReturnSelection(
        s, *tab, app::NavigationReturnChildName(from, path));
    s.timing.first_frame_recorded = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void GoForward(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->CanGoForward()) return;
    s.addressLiveDue = s.addressHistoryDue = 0;
    if (s.addressSearching) HideAddressEditor(s, false);
    if (tab->search_content_active) CancelActiveContentSearch(s, *tab);
    app::RememberContentNavigation(*tab);
    std::wstring path = tab->GoForward();
    StartLoadingPath(s, *tab, path, PathLoadReason::History);
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
    std::wstring current;
    if (!tab || IsSettingsTab(tab) || tab->current_path.empty()) {
        const auto recent = s.places.RecentFolderPaths(1);
        current = recent.empty() ? L"C:\\" : recent.front();
    } else {
        current = tab->current_path;
    }
    return app::NewTabLocation(s.appPrefs, current);
}

void OpenNewTab(AppState& s) {
    OpenTabAt(s, NewTabPath(s));
}

void OpenTabAt(AppState& s, const std::wstring& path) {
    if (!path.empty()) {
        NewTab(s, path);
        return;
    }
    // Preserve the explicit virtual location before binding/loading the tab.
    RememberLayoutFocus(s);
    s.window_tabs.NewTabAtLocation(s.window_tabs.items.size(), path);
    BindCurrentLayout(s);
    if (app::Tab* tab = ActiveTab(s)) StartLoadingPath(s, *tab, std::wstring());
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void NewTab(AppState& s, const std::wstring& path) {
    if (OpenSystemNetworkShortcut(s, path)) return;
    RememberLayoutFocus(s);
    s.window_tabs.NewTab(path.empty() ? L"C:\\" : path);
    BindCurrentLayout(s);
    if (app::Tab* tab = ActiveTab(s)) {
        StartLoadingPath(s, *tab, tab->current_path);
        RecordRecentOpen(s, tab->current_path, app::PlaceItemKind::Folder);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

namespace {
constexpr ULONGLONG kTabFlashMs = 900;

// Window-tab index whose pane currently shows `path`, or -1.
int FindFolderTab(AppState& s, const std::wstring& path, app::Pane** pane_out) {
    if (s.window_tabs.items.empty() || path.empty() || fs::IsVirtualPath(path)) return -1;
    const std::wstring normalized = fs::NormalizePath(path);
    for (size_t i = 0; i < s.window_tabs.items.size(); ++i) {
        app::LayoutTab& layout = *s.window_tabs.items[i];
        for (auto* pane : layout.VisiblePanes()) {
            const app::Tab* tab = pane ? pane->ActiveTab() : nullptr;
            if (!tab || tab->current_path.empty()) continue;
            const std::wstring tab_path = fs::NormalizePath(tab->current_path);
            if (_wcsicmp(tab_path.c_str(), normalized.c_str()) != 0) continue;
            if (pane_out) *pane_out = pane;
            return static_cast<int>(i);
        }
    }
    return -1;
}

void FlashTab(AppState& s, size_t index) {
    if (index >= s.window_tabs.items.size()) return;
    s.tabFlashKey = s.window_tabs.items[index].get();
    s.tabFlashAt = GetTickCount64();
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
} // namespace

bool SyncTagGroups(AppState& s) {
    const uint64_t revision = s.places.TagRevision();
    if (s.tagGroupsRevision == revision && app::CurrentTagCatalog()) return false;
    s.tagGroupsRevision = revision;
    auto snapshot = std::make_shared<app::TagCatalogSnapshot>();
    snapshot->revision = revision;
    std::unordered_map<std::wstring, int> primary;
    for (size_t i = 0; i < s.places.tags.size(); ++i) {
        const app::ColorTag& tag = s.places.tags[i];
        snapshot->tags.push_back({tag.name, tag.rgb});
        for (const auto& path : tag.paths) {
            if (path.empty() || fs::IsVirtualPath(path)) continue;
            std::wstring key = fs::NormalizePath(path);
            for (auto& ch : key) ch = static_cast<wchar_t>(std::towlower(ch));
            primary.try_emplace(std::move(key), static_cast<int>(i));  // first tag wins
        }
    }
    snapshot->paths.assign(primary.begin(), primary.end());
    app::PublishTagCatalog(std::move(snapshot));
    return true;
}

float TabFlashAmount(const AppState& s, const app::LayoutTab* key) {
    if (!key || key != s.tabFlashKey) return 0.0f;
    const ULONGLONG elapsed = GetTickCount64() - s.tabFlashAt;
    if (elapsed >= kTabFlashMs) return 0.0f;
    // Two soft pulses.
    const float x = std::sin(3.14159265f * static_cast<float>(elapsed) / (kTabFlashMs / 2.0f));
    return x * x;
}

bool TickTabFlash(AppState& s) {
    if (!s.tabFlashKey) return false;
    if (GetTickCount64() - s.tabFlashAt >= kTabFlashMs) s.tabFlashKey = nullptr;
    return true;  // one more frame clears the overlay
}

void OpenFolderTab(AppState& s, const std::wstring& path) {
    if (!ActivateExistingFolderTab(s, path)) NewTab(s, path);
}

void SelectNameInTab(AppState& s, app::Tab& tab, const std::wstring& name) {
    tab.pending_selected_names = {name};
    tab.pending_selected_name = name;
    tab.pending_selection_revision = tab.selection_revision;
    tab.pending_ensure_selection_visible = true;
    tab.pending_selection_revision = tab.selection_revision;
    if (!tab.snapshot) return;
    const auto& entries = *tab.snapshot;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (_wcsicmp(entries[i].name.c_str(), name.c_str()) != 0 ||
            !tab.EntryVisible(static_cast<int>(i))) continue;
        tab.SelectOnly(static_cast<int>(i));
        tab.pending_selection_revision = tab.selection_revision;
        EnsureRowVisible(s, tab, static_cast<int>(i));
        break;
    }
}

void NewBackgroundTab(AppState& s, const std::wstring& path) {
    if (OpenSystemNetworkShortcut(s, path)) return;
    const app::LayoutTab* opener = s.window_tabs.Active();
    if (path.empty() || !opener) {
        NewTab(s, path);
        return;
    }
    // Already open: point at it without stealing focus.
    if (const int existing = FindFolderTab(s, path, nullptr); existing >= 0) {
        FlashTab(s, static_cast<size_t>(existing));
        return;
    }
    if (s.backgroundTabOpener != opener) {
        s.backgroundTabOpener = opener;
        s.backgroundTabRun = 0;
    }
    RememberLayoutFocus(s);
    app::LayoutTab& added = s.window_tabs.NewTabAt(s.window_tabs.active + 1 + s.backgroundTabRun, path);
    ++s.backgroundTabRun;
    // Stay on the opener; NewTabAt activated the new tab.
    for (size_t i = 0; i < s.window_tabs.items.size(); ++i) {
        if (s.window_tabs.items[i].get() == opener) {
            s.window_tabs.active = i;
            break;
        }
    }
    if (app::Tab* tab = added.ActiveFolder()) {
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
    if (page == 5) ui::DetectPreviewCodecs(true, s.hwnd);
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
    if (page == 5) ui::DetectPreviewCodecs(true, s.hwnd);
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
    if (app::LastTabClosesWindow(s.window_tabs.items.size(), s.window_tabs.items[idx]->pinned,
                                 s.appPrefs.close_window_with_last_tab)) {
        PostMessageW(s.hwnd, WM_CLOSE, 0, 0);
        return;
    }
    RememberLayoutFocus(s);
    if (s.window_tabs.items.size() > 1 && !s.window_tabs.items[idx]->pinned) {
        for (const auto& pane : s.window_tabs.items[idx]->panes)
            DropLiveNetworkSearch(s, pane->view.search_session_id);
    }
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
    app::Pane* pane = nullptr;
    const int index = FindFolderTab(s, path, &pane);
    if (index < 0) return false;
    SwitchTab(s, static_cast<size_t>(index));
    FocusPane(s, pane);
    RecordRecentOpen(s, fs::NormalizePath(path), app::PlaceItemKind::Folder);
    FlashTab(s, static_cast<size_t>(index));
    return true;
}

} // namespace pulse

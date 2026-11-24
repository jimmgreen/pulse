#include "app_internal.h"
#include "search_query.h"
#include "../index/content_index.h"
#include "../ui/address_search_layout.h"
#include "../common/localization.h"
#include "content_scope_mode.h"
#include "keymap.h"
#include "../ui/ui_renderer.h"
#include <algorithm>

namespace pulse {
namespace {
using I = l10n::StringId;
std::wstring NormalScope(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    std::transform(path.begin(), path.end(), path.begin(), [](wchar_t c) { return towlower(c); });
    return path;
}
bool Within(std::wstring_view path, std::wstring_view parent) {
    return path == parent || (path.size() > parent.size() && path.starts_with(parent) &&
        (parent.ends_with(L'\\') || path[parent.size()] == L'\\'));
}
ui::FluentMenuItem MenuItem(int command, I label, bool checked = false) {
    ui::FluentMenuItem item;
    item.command = command; item.text = l10n::Get(label); item.checked = checked;
    if (checked) item.glyph = L"\xE73E";
    return item;
}
}

std::wstring ContentIndexStatusText(AppState& s) {
    if (s.contentSearch.InstantMode()) return l10n::Get(I::ContentInstantReady);
    const auto config = s.contentSearch.GetConfig();
    const auto status = s.contentSearch.GetStatus();

    if (config.roots.empty()) return l10n::Get(I::ContentIndexEmpty);
    wchar_t counts[256]{};
    swprintf_s(counts, l10n::Get(I::ContentIndexStatusFormat).c_str(),
        static_cast<unsigned long long>(status.indexed_files),
        static_cast<unsigned long long>(status.pending_files),
        static_cast<unsigned long long>(status.skipped_files),
        static_cast<unsigned long long>(status.errors));
    if (status.indexing && !status.paused)
        return l10n::Get(I::ContentIndexBuilding) + L" · " + counts;
    if (status.error == ERROR_SHARING_VIOLATION || status.error == ERROR_LOCK_VIOLATION)
        return l10n::Get(I::ContentFilesBusy) + L" · " + counts;
    if (status.error) return l10n::Get(I::SearchIncomplete) + L" (" + std::to_wstring(status.error) + L") · " + counts;
    return l10n::Get(status.paused ? I::ContentIndexPaused : status.indexing ? I::ContentIndexBuilding : I::ContentIndexReady)
        + L" · " + counts;
}

void ConfigureContentSort(const app::Tab& tab,index::ContentSearchRequest& request) {
    request.sort_desc = tab.sort_direction == ui::SortDirection::Desc;
    request.sort = tab.search_relevance ? index::ContentResultSort::Index :
        tab.sort_column == ui::SortColumn::Size ? index::ContentResultSort::Size :
        tab.sort_column == ui::SortColumn::Mtime ? index::ContentResultSort::Mtime :
        tab.sort_column == ui::SortColumn::Path ? index::ContentResultSort::Path :
        tab.sort_column == ui::SortColumn::Type ? index::ContentResultSort::Type : index::ContentResultSort::Name;
}

void StartIndexedContentSearch(AppState& s, app::Tab& tab, const std::wstring& rest, bool scan) {
    tab.search_content_stopped = false;
    tab.content_subscription_error = ERROR_SUCCESS;
    tab.content_subscription_failure = index::ContentSubscriptionFailure::None;
    tab.content_scan_error = ERROR_SUCCESS;
    if (!tab.search_session_id) tab.search_session_id = ++s.nextIndexReq;
    const auto split = app::SplitSearchQueryText(rest);
    const auto config = s.contentSearch.GetConfig();
    tab.search_awaiting_content = false;
    tab.search_content_active = false;
    tab.search_allow_scan = scan;
    tab.search_index_revision = s.contentSearch.GetStatus().revision;
    tab.pending_generation = 0;
    const auto scope = NormalScope(split.path_prefix);
    bool overlaps = scope.empty() && !config.roots.empty();
    for (const auto& root : config.roots) {
        const auto path = NormalScope(root.path);
        overlaps |= Within(scope, path) || Within(path, scope);
    }
    bool uncovered_allowed = false;
    if (!scan && !overlaps && !split.path_prefix.empty()) {
        std::vector<index::VolumeInfo> volumes;
        std::vector<std::wstring> excluded;
        const bool scope_known = s.index.GetScope(volumes, excluded);
        uncovered_allowed = app::ShouldAllowUncoveredContentScope(
            s.index.Ready(), scope_known, volumes, excluded, split.path_prefix, s.isolatedTest);
    }
    if (!scan && !overlaps && !uncovered_allowed) {
        tab.loading = false;
        tab.banner_title = l10n::Get(scope.empty() ? I::ContentIndexEmpty : I::ContentIndexNotCovered);
        tab.banner_message = l10n::Get(scope.empty() ? I::ContentIndexEmptyDesc : I::ContentIndexNotCoveredDesc);
        tab.virtual_title = l10n::Get(I::SearchModeContent);
        return;
    }
    if (!split.content.present()) {
        tab.loading = false;
        tab.banner_title = l10n::Get(I::ContentIndexNoQuery);
        return;
    }
    index::ContentSearchRequest request;
    request.paged_results = true;
    request.task_scan = !scan;
    request.generation = ++s.nextIndexReq;
    request.indexed = !scan;
    request.allow_uncovered_root = uncovered_allowed;
    request.root = split.path_prefix;
    if (request.root.empty()) request.roots = app::DefaultContentSearchRoots(config.shared_scope);
    request.filename_query = split.filename_needle;
    if (!s.appPrefs.search_pinyin) request.filename_query = L"nopinyin: " + request.filename_query;
    request.needles = split.content.needles;
    request.excluded_needles = split.content.excluded;
    request.match_mode = split.content.mode;
    request.whole_word = split.content.whole_word;
    request.case_sensitive = split.content.case_sensitive;
    ConfigureContentSort(tab,request);
    tab.content_sort_override=false;
    tab.content_scanned_files=0;
    tab.content_total_files=0;
    request.maximum_file_bytes = config.maximum_file_bytes;
    request.maximum_document_bytes = config.maximum_document_bytes;
    request.skip_system_locations = true;
    if (!request.needles.empty()) request.needle = request.needles.front();
    tab.pending_generation = request.generation;
    tab.search_content_active = true;
    tab.loading = !request.previous_results;
    wchar_t title[512]{};
    swprintf_s(title, l10n::Get(I::ContentFoundFormat).c_str(), app::SearchDisplayNeedle(rest).c_str(), size_t{0});
    if (!request.previous_results) tab.virtual_title = title;
    tab.banner_title = request.task_scan ? std::wstring{} : l10n::Get(I::ContentIndexScanRunning);
    tab.banner_message.clear();
    request.session_id = tab.search_session_id;
    request.subscribe = request.indexed && request.paged_results;
    tab.search_live_generation = request.subscribe ? request.generation : 0;
    s.contentSearch.SearchAsync(std::move(request));
}

void ShowSearchOptions(AppState& s, bool management) {
    if (management) { OpenSettingsTab(s, 1); return; }
    if (s.searchHistoryOpen) {
        s.searchOptionsPending = true;
        if (s.menu) s.menu->Dismiss();
        return;
    }
    if (!EnsureMenu(s)) return;
    struct FocusGuard {
        bool& flag;
        bool previous;
        explicit FocusGuard(bool& value) : flag(value), previous(value) { flag = true; }
        ~FocusGuard() { flag = previous; }
    } focus_guard(s.addressIgnoreKillFocus);
    // Keep one focus-protected interaction while the compact menu changes
    // into index management. The search editor remains attached throughout.
    for (;;) {
        auto* tab = ActiveTab(s);
        const bool editing = s.addressSearching && !management;
        std::vector<ui::FluentMenuItem> items;
        items.push_back(MenuItem(3, I::SearchPinyin, s.appPrefs.search_pinyin));
        items.back().toggle = true;
        items.back().glyph = L"\xE8D2";
        if (editing && tab) items.push_back(MenuItem(4, I::SearchRelevance, tab->search_relevance));
        items.back().separator_after = true;
        items.push_back(MenuItem(5, I::ContentIndexManage));
        if (editing) {
            ui::FluentMenuItem manage;
            manage.command = 5;
            manage.text = l10n::Get(I::ContentIndexManage) + L"…";
            manage.glyph = L"\xE713";
            items.resize(tab ? 2 : 1);
            items.push_back(std::move(manage));
            const auto layout = ui::LayoutAddressSearch(s.renderer.SearchBarRect(static_cast<float>(s.compositor.Width())), s.scale);
            if (layout.scope.left == layout.scope.right) {
                ui::FluentMenuItem scope;
                scope.text = l10n::Get(s.addressSearchCurrent ? I::LocationCurrent : I::LocationIndexed);
                scope.children.push_back(MenuItem(20, I::LocationIndexed, !s.addressSearchCurrent));
                scope.children.push_back(MenuItem(21, I::LocationCurrent, s.addressSearchCurrent));
                scope.children.back().enabled = !s.addressSearchRoot.empty();
                scope.separator_after = true;
                items.insert(items.begin(), std::move(scope));
            }
        }
        auto advanced = MenuItem(6, I::AdvancedSearch);
        advanced.text += L"…";
        advanced.shortcut = app::ShownShortcut(L"Ctrl+Shift+F");
        items.insert(items.end() - 1, std::move(advanced));
        POINT anchor{};
        if (s.addressSearching) {
            const auto layout = ui::LayoutAddressSearch(s.renderer.SearchBarRect(static_cast<float>(s.compositor.Width())), s.scale);
            ui::FluentMenuModel popup_layout;
            popup_layout.SetItems(items);
            popup_layout.Layout(s.compositor.DwriteFactory(), s.scale);
            anchor = {static_cast<LONG>(layout.options.right - popup_layout.WidthPx()), static_cast<LONG>(layout.options.bottom + 4*s.scale)};
            ClientToScreen(s.hwnd, &anchor);
        } else GetCursorPos(&anchor);
        anchor.x -= ui::FluentMenu::kShadowMargin;
        anchor.y -= ui::FluentMenu::kShadowMargin;
        wchar_t debug_path[32768]{};
        if (s.shot.active && s.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_OPTIONS_SHOT", debug_path, ARRAYSIZE(debug_path))) {
            s.menu->SetTheme(s.darkMode, s.accentColor);
            s.menu->SaveDebugSnapshot(debug_path, std::move(items));
            return;
        }
        const int command = s.menu->TrackPopup(anchor, std::move(items));
        if (command == 6) { ShowAdvancedSearch(s, false); return; }
        if (command == 5) { OpenSettingsTab(s, 1); return; }
        bool search = false;
        if (command == 20 || command == 21) { s.addressSearchCurrent = command == 21; search = true; }
        else if (command == 1 || command == 2) { s.addressSearchContent = command == 2; search = true; }
        else if (command == 3) { s.appPrefs.search_pinyin = !s.appPrefs.search_pinyin; s.appPrefs.Save(); search = editing; }
        else if (command == 4 && tab) { tab->search_relevance = true; search = editing; }
        if (s.addressSearching && IsWindow(s.hwndAddressEdit)) {
            SetForegroundWindow(GetAncestor(s.hwndAddressEdit, GA_ROOT));
            SetFocus(s.hwndAddressEdit);
            if (search) {
                if ((command == 3 || command == 4) && tab && IsAddressSearchResults(tab)) {
                    std::wstring rest;
                    app::ParsePulsePath(tab->current_path, nullptr, &rest);
                    RequestSearchPage(s, *tab, rest, true);
                } else QueueAddressSearch(s);
            }
        }
        InvalidateRect(s.hwnd, nullptr, FALSE);
        break;
    }
}
} // namespace pulse

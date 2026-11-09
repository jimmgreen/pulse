#include "app_internal.h"
#include "../index/content_scope.h"
#include "../index/content_search_protocol.h"
#include "search_query.h"
#include "../common/localization.h"
#include "../ui/address_search_layout.h"
#include "../index/content_config_storage.h"
#include "../common/text_format.h"
#include "app_prompts.h"
#include "content_scope_mode.h"
#include "content_pack_ui.h"
#include "../ui/ui_renderer.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

namespace pulse {

bool IsAddressSearchResults(const app::Tab* tab) {
    std::wstring kind;
    return tab && app::ParsePulsePath(tab->current_path, &kind, nullptr) && kind == L"search";
}

namespace {
// Code-side Chinese, converted for the zh-TW UI phrase by phrase.
std::wstring Zh(const wchar_t* chinese, const wchar_t* english) {
    return l10n::IsChinese() ? l10n::HantText(chinese) : std::wstring(english);
}

// Removes the pre-Instant content-v1.sqlite once per process
// (app::DeleteLegacyContentDatabase). The delete runs on a worker so a
// multi-GB file never stalls the window; the next tick reports it. A database
// still open elsewhere is left for the next launch.
struct LegacyContentCleanupState {
    enum Stage : int { Idle, Running, Finished, Done };
    std::atomic<int> stage{Idle};
    app::LegacyContentCleanup result;
};
LegacyContentCleanupState& LegacyContentCleanup() {
    static LegacyContentCleanupState state;
    return state;
}
void TickLegacyContentCleanup(AppState& s) {
    auto& state = LegacyContentCleanup();
    const int stage = state.stage.load(std::memory_order_acquire);
    if (stage == LegacyContentCleanupState::Idle) {
        if (!s.contentSearch.ConfigurationReady()) return;
        int expected = LegacyContentCleanupState::Idle;
        if (!state.stage.compare_exchange_strong(expected, LegacyContentCleanupState::Running)) return;
        const auto database = index::config_storage::DatabasePath();
        index::ContentIndexConfig stored;
        // Instant mode keeps its folders in .config beside the database; only
        // delete the database once that file is there.
        if (database.empty() || GetFileAttributesW(database.c_str()) == INVALID_FILE_ATTRIBUTES ||
            !index::config_storage::ReadConfigFile(database, stored)) {
            state.stage.store(LegacyContentCleanupState::Done, std::memory_order_release);
            return;
        }
        std::thread([database] {
            auto& worker = LegacyContentCleanup();
            worker.result = app::DeleteLegacyContentDatabase(database);
            worker.stage.store(LegacyContentCleanupState::Finished, std::memory_order_release);
        }).detach();
        return;
    }
    if (stage != LegacyContentCleanupState::Finished) return;
    state.stage.store(LegacyContentCleanupState::Done, std::memory_order_release);
    if (!state.result.deleted) return;
    const auto size = format::ByteSize(state.result.bytes);
    std::wstring message = Zh(L"已删除旧版本留下的内容索引数据库，释放了 ", L"Deleted the content index database left by an older version and freed ");
    message += size;
    message += Zh(L"。内容搜索现在直接读取文件，不再需要它。", L". Content search now reads files directly and no longer needs it.");
    s.notification_toast.SetKind(ui::fluent::InfoBarKind::Success);
    s.notification_toast.Show(s.hwnd, Zh(L"已清理旧的内容索引", L"Old content index removed"), std::move(message));
}
} // namespace

static std::wstring SearchOrigin(const app::Tab& tab) {
    if (tab.search_origin_valid) return tab.search_origin_path;
    auto history = tab.back_stack;
    while (!history.empty()) {
        const auto path = history.top();
        history.pop();
        std::wstring kind;
        if (!app::ParsePulsePath(path, &kind, nullptr) || kind != L"search") return path;
    }
    return {};
}

static void RestoreSearchDraft(app::Tab& tab) {
    if (tab.search_input_path == tab.current_path) return;
    std::wstring rest;
    app::ParsePulsePath(tab.current_path, nullptr, &rest);
    const auto spec = app::ParseSearchQuery(rest);
    tab.search_input_path = tab.current_path;
    tab.search_input_content = !spec.content.empty();
    tab.search_input_text = tab.search_input_content ? spec.content : app::NameQueryDraft(rest);
    tab.search_input_current = spec.location != app::LocationScope::Indexed;
    tab.search_input_root = spec.location == app::LocationScope::CustomFolder
        ? spec.custom_folder : spec.current_folder;
    if (tab.search_input_root.empty()) {
        const auto origin = SearchOrigin(tab);
        if (!fs::IsVirtualPath(origin)) tab.search_input_root = origin;
    }
}

void SaveAddressSearchDraft(AppState& s) {
    auto* tab = ActiveTab(s);
    if (!s.addressSearching || !tab || !s.hwndAddressEdit) return;
    const int length = GetWindowTextLengthW(s.hwndAddressEdit);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    text.resize(GetWindowTextW(s.hwndAddressEdit, text.data(), length + 1));
    tab->search_input_path = tab->current_path;
    tab->search_input_text = std::move(text);
    tab->search_input_root = s.addressSearchRoot;
    tab->search_input_current = s.addressSearchCurrent;
    tab->search_input_content = s.addressSearchContent;
}

void FillAddressSearchView(AppState& s, ui::WindowViewModel& vm) {
    vm.address_search_content = s.addressSearchContent;
    auto* tab = ActiveTab(s);
    if (!s.addressEditing && IsAddressSearchResults(tab)) {
        RestoreSearchDraft(*tab);
        vm.address_searching = true;
        vm.address_search_current = tab->search_input_current;
        vm.address_search_content = tab->search_input_content;
        vm.address_search_text = tab->search_input_text;
        vm.address_search_has_text = !vm.address_search_text.empty();
    }
    std::wstring root;
    bool current = false;
    if (s.addressSearching) {
        root = s.addressSearchRoot;
        current = s.addressSearchCurrent;
    } else if (tab && tab->search_input_path == tab->current_path) {
        root = tab->search_input_root;
        current = tab->search_input_current;
    } else if (tab && !fs::IsVirtualPath(tab->current_path)) {
        root = tab->current_path;
        current = s.appPrefs.address_search_current;
    }
    const auto scope = current && !root.empty() ? app::TabTitle(root) : l10n::Get(l10n::StringId::SearchScopeAll);
    vm.address_search_scope_label = scope;
    vm.address_search_placeholder = l10n::Get(l10n::StringId::Search) +
        (std::wstring(l10n::LocaleName()).starts_with(L"zh") ? L"" : L" ") + scope;
}

void ExitAddressSearch(AppState& s) {
    auto* tab = ActiveTab(s);
    const bool results = IsAddressSearchResults(tab);
    const auto origin = results ? SearchOrigin(*tab) : std::wstring{};
    // Explicit cancellation must not flush a debounced query while hiding the edit.
    s.addressLiveDue = s.addressHistoryDue = 0;
    s.addressLiveContext.clear();
    s.addressHistoryPath.clear();
    s.addressSearchComposing = false;
    if (results) {
        s.pendingIndexSearches.erase(static_cast<uint32_t>(tab->pending_generation));
        CancelActiveContentSearch(s, *tab);
        tab->search_loading_more = false;
    }
    HideAddressEditor(s, false);
    if (results) NavigateTo(s, origin);
}

void ShowAddressSearch(AppState& s) {
    if (!s.appPrefs.search_enabled) return;   // search off: only the folder filter remains
    if (s.addressSearching && s.hwndAddressEdit) {
        SetForegroundWindow(GetAncestor(s.hwndAddressEdit, GA_ROOT));
        SetFocus(s.hwndAddressEdit);
        SendMessageW(s.hwndAddressEdit, EM_SETSEL, 0, -1);
        return;
    }
    auto* tab = ActiveTab(s);
    if (!tab) return;
    std::wstring query;
    if (IsAddressSearchResults(tab)) RestoreSearchDraft(*tab);
    if (tab->search_input_path == tab->current_path) {
        query = tab->search_input_text;
        s.addressSearchRoot = tab->search_input_root;
        s.addressSearchCurrent = tab->search_input_current;
        s.addressSearchContent = tab->search_input_content;
    } else {
        s.addressSearchRoot = fs::IsVirtualPath(tab->current_path) ? L"" : tab->current_path;
        s.addressSearchCurrent = s.appPrefs.address_search_current && !s.addressSearchRoot.empty();
        s.addressSearchContent = s.appPrefs.address_search_content;
    }
    ShowAddressEditor(s);
    if (!s.addressEditing || !s.hwndAddressEdit) return;
    s.addressSearching = true;
    s.addressAnimationTick = GetTickCount64();
    SetWindowTextW(s.hwndAddressEdit, query.c_str());
    SendMessageW(s.hwndAddressEdit, EM_SETSEL, 0, -1);
    s.addressSearchEntryText = query;
    const auto cue = l10n::Get(s.addressSearchContent ? l10n::StringId::SearchContentHint : l10n::StringId::SearchNameHint);
    SendMessageW(s.hwndAddressEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
    LayoutAddressEditor(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    if (query.empty() && !s.searchHistoryOpen)
        PostMessageW(s.hwnd, WM_SEARCH_HISTORY, 0, 0);
}

void SyncSearchBarWidth(AppState& s) {
    s.renderer.SetNavigationForward(ActiveTab(s) && ActiveTab(s)->CanGoForward());
    // Editing gets room to read the whole query; a results page keeps a wider,
    // clearly active field so the query stays visible and clickable.
    const float dip = s.addressSearching ? 520.0f : IsAddressSearchResults(ActiveTab(s)) ? 360.0f : 0.0f;
    s.renderer.SetSearchBarMinWidth(dip);
}

static std::wstring AddressEditText(const AppState& s) {
    if (!s.hwndAddressEdit) return {};
    const int length = GetWindowTextLengthW(s.hwndAddressEdit);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    text.resize(GetWindowTextW(s.hwndAddressEdit, text.data(), length + 1));
    return text;
}

void EscapeAddressSearch(AppState& s) {
    auto* tab = ActiveTab(s);
    const auto text = AddressEditText(s);
    if (!tab || text == s.addressSearchEntryText) {
        ExitAddressSearch(s);
        return;
    }
    if (s.addressSearchEntryText.empty()) {
        // A new search that already produced live results: undoing means leaving it.
        if (IsAddressSearchResults(tab)) {
            ExitAddressSearch(s);
            return;
        }
        SetWindowTextW(s.hwndAddressEdit, L"");
        s.addressLiveDue = 0;
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    // First Esc undoes this edit and restores the results it replaced.
    SetWindowTextW(s.hwndAddressEdit, s.addressSearchEntryText.c_str());
    const auto end = static_cast<WPARAM>(s.addressSearchEntryText.size());
    SendMessageW(s.hwndAddressEdit, EM_SETSEL, end, static_cast<LPARAM>(end));
    if (IsAddressSearchResults(tab)) SubmitAddressSearch(s, true);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void FocusSearchResults(AppState& s, bool select_first) {
    if (!IsAddressSearchResults(ActiveTab(s))) return;
    HideAddressEditor(s, false);
    if (select_first && s.hwnd) SendMessageW(s.hwnd, WM_KEYDOWN, VK_HOME, 0);
}

void BeginSearchEditAt(AppState& s, int x, int y) {
    if (!s.appPrefs.search_enabled) return;
    ShowAddressSearch(s);
    if (!s.addressSearching || !s.hwndAddressEdit) return;
    POINT point{x, y};
    MapWindowPoints(s.hwnd, s.hwndAddressEdit, &point, 1);
    RECT client{};
    GetClientRect(s.hwndAddressEdit, &client);
    point.x = std::clamp<LONG>(point.x, 0, std::max<LONG>(0, client.right - 1));
    point.y = client.bottom / 2;
    // Put the caret where the user clicked instead of selecting the whole query.
    SendMessageW(s.hwndAddressEdit, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
    SendMessageW(s.hwndAddressEdit, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
    MSG pending{};
    while (PeekMessageW(&pending, s.hwnd, WM_SEARCH_HISTORY, WM_SEARCH_HISTORY, PM_REMOVE)) {}
}

void SwitchAddressSearchMode(AppState& s, bool content) {
    if (!s.addressSearching) ShowAddressSearch(s);
    if (s.addressSearchContent == content) return;
    if (s.searchHistoryOpen && s.menu) s.menu->Dismiss();
    // First switch to content search: where unscoped searches look. Closing
    // the prompt keeps the default (user folders) and asks again next time.
    if (content && !s.isolatedTest && s.appPrefs.content_scope == 0) {
        const int scope = AskContentScope(s);
        if (scope) {
            s.appPrefs.content_scope = scope;
            s.appPrefs.Save();
            app::SetContentScopeMode(scope);
        }
        if (s.hwndAddressEdit) SetFocus(s.hwndAddressEdit);
    }
    // Content search comes in the enhancement pack; offer it once per run here.
    if (content && !s.isolatedTest) OfferContentPack(s, true);
    s.addressSearchContent = content;
    const auto& cue = l10n::Get(content ? l10n::StringId::SearchContentHint : l10n::StringId::SearchNameHint);
    SendMessageW(s.hwndAddressEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
    QueueAddressSearch(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void QueueAddressSearch(AppState& s) {
    auto* tab = ActiveTab(s);
    if (!s.addressSearching || !tab) return;
    s.addressLiveContext = tab->current_path;
    if (s.appPrefs.address_search_current != s.addressSearchCurrent ||
        s.appPrefs.address_search_content != s.addressSearchContent) {
        s.appPrefs.address_search_current = s.addressSearchCurrent;
        s.appPrefs.address_search_content = s.addressSearchContent;
        if (!s.isolatedTest) s.appPrefs.Save();
    }
    s.addressLiveDue = GetTickCount64() + (s.addressSearchContent ? 150 : 100);
    tab->search_allow_scan = false;
    s.addressHistoryDue = 0;
}

void FlushAddressSearch(AppState& s) {
    if (s.addressSearching && s.addressLiveDue && !s.addressSearchComposing &&
        ActiveTab(s) && ActiveTab(s)->current_path == s.addressLiveContext)
        SubmitAddressSearch(s, true);
    if (s.addressHistoryDue && ActiveTab(s) && ActiveTab(s)->current_path == s.addressHistoryPath)
        RecordSearchHistory(s, s.addressHistoryPath);
    s.addressLiveDue = s.addressHistoryDue = 0;
    s.addressSearchComposing = false;
}

void SubmitAddressSearch(AppState& s, bool live) {
    auto* tab = ActiveTab(s);
    if (!tab) return;
    const int length = GetWindowTextLengthW(s.hwndAddressEdit);
    std::wstring query(static_cast<size_t>(length) + 1, L'\0');
    query.resize(GetWindowTextW(s.hwndAddressEdit, query.data(), length + 1));
    const auto first = query.find_first_not_of(L" \t\r\n");
    s.addressLiveDue = 0;
    if (s.addressSearchComposing) return;
    // Enter in content search without the pack: ask (again) for the download.
    if (!live && s.addressSearchContent) OfferContentPack(s);
    if (first == std::wstring::npos) {
        if ((!live || !IsAddressSearchResults(tab)) && !s.addressSearchContent) return;
        query.clear();
    } else query = query.substr(first, query.find_last_not_of(L" \t\r\n") - first + 1);
    const bool continuing = IsAddressSearchResults(tab);
    if (s.addressSearchContent && query.empty()) {
        if (tab->search_session_id) s.contentSearch.Cancel(tab->search_session_id);
        tab->search_live_generation = 0;
        if (continuing) {
            tab->pending_generation = 0;
            tab->loading = tab->search_content_active = tab->search_awaiting_content = false;
            tab->search_input_content = true;
            tab->search_content_empty = true;
            tab->search_input_text.clear();
            CancelContentSelection(s,*tab);
            tab->content_results.reset();
            tab->content_count_final=false;
            tab->content_selection_restore.reset();
            tab->search_entries = std::make_shared<std::vector<fs::DirEntry>>();
            tab->search_snippets = std::make_shared<std::vector<std::wstring>>();
            tab->search_total = 0;
            tab->SetSnapshot(tab->search_entries);
            tab->banner_title = l10n::Get(l10n::StringId::ContentIndexNoQuery);
            tab->banner_message.clear();
            InvalidateRect(s.hwnd, nullptr, FALSE);
        }
        return;
    }
    const bool restore_empty_content = tab->search_content_empty;
    tab->search_content_empty = false;
    std::wstring previous_query;
    if (continuing) app::ParsePulsePath(tab->current_path, nullptr, &previous_query);
    auto spec = continuing ? app::ParseSearchQuery(previous_query) : app::AdvancedSearchSpec{};
    if (s.addressSearchContent) {
        const bool was_content_query = !spec.content.empty();
        spec.content = query;
        if (!was_content_query) spec.name.clear();
    } else {
        spec.name = query;
        spec.name_is_query = true;
        spec.content.clear();
        spec.content_exclude.clear();
    }
    spec.current_folder = s.addressSearchRoot;
    spec.custom_folder.clear();
    spec.location = s.addressSearchCurrent && !spec.current_folder.empty()
        ? app::LocationScope::CurrentFolder : app::LocationScope::Indexed;
    auto compiled_text = s.addressSearchContent ? app::CompileSearchQuery(spec)
        : app::CompileNameQueryInput(query, s.addressSearchCurrent ? s.addressSearchRoot : L"");
    const auto path = app::MakeSearchPath(compiled_text);
    if (tab->current_path == path) {
        if (restore_empty_content || (!live && tab->search_content_stopped)) {
            tab->search_input_text = query;
            RequestSearchPage(s, *tab, compiled_text, true);
        }
        if (!query.empty()) {
            if (live) {
                s.addressHistoryPath = path;
                s.addressHistoryDue = GetTickCount64() + 800;
            } else {
                s.addressHistoryDue = 0;
                RecordSearchHistory(s, path);
            }
        }
        return;
    }
    // Rows from the other mode answer a different question: never keep them on screen
    // under a "Searching…" header for the new mode.
    const bool mode_changed = continuing && tab->search_input_content != s.addressSearchContent;
    const auto previous_results = continuing && !mode_changed ? tab->snapshot : fs::SnapshotPtr{};
    if (continuing) {
        ++tab->view_generation;
        tab->current_path = path;
    } else tab->NavigateTo(path);
    StartLoadingPath(s, *tab, path);
    if (!query.empty()) {
        if (live) {
            s.addressHistoryPath = path;
            s.addressHistoryDue = GetTickCount64() + 800;
        } else {
            s.addressHistoryDue = 0;
            RecordSearchHistory(s, path);
        }
    }
    if (previous_results && !previous_results->empty() && tab->loading) {
        tab->SetSnapshot(previous_results);
        tab->search_retaining_results = true;
    } else if (mode_changed && tab->snapshot && !tab->snapshot->empty()) {
        // The live toggle can land before the tab is marked loading; clear regardless, and
        // never reuse a list that may still hold the other mode's rows.
        if (!tab->search_entries || !tab->search_entries->empty())
            tab->search_entries = std::make_shared<std::vector<fs::DirEntry>>();
        tab->SetSnapshot(tab->search_entries);
        tab->search_retaining_results = false;
        tab->search_total = 0;
        tab->loading = true;
    }
    tab->search_input_path = tab->current_path;
    tab->search_input_text = query;
    tab->search_input_root = s.addressSearchRoot;
    tab->search_input_current = s.addressSearchCurrent;
    tab->search_input_content = s.addressSearchContent;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ShowAddressSearchScope(AppState& s) {
    if (s.searchHistoryOpen) {
        s.searchScopePending = true;
        if (s.menu) s.menu->Dismiss();
        return;
    }
    if (!s.addressSearching || !EnsureMenu(s)) return;
    std::vector<ui::FluentMenuItem> items(3);
    items[0].command = 1;
    items[0].text = l10n::Get(l10n::StringId::LocationCurrent);
    items[0].enabled = !s.addressSearchRoot.empty();
    items[0].checked = s.addressSearchCurrent;
    items[0].radio = s.addressSearchCurrent;
    items[0].radio_group = true;
    items[1].command = 2;
    items[1].text = l10n::Get(l10n::StringId::LocationIndexed);
    items[1].checked = !s.addressSearchCurrent;
    items[1].radio = !s.addressSearchCurrent;
    items[1].radio_group = true;
    items[2].command = 3;
    items[2].text = l10n::Get(l10n::StringId::SearchChooseScope);
    const auto layout = ui::LayoutAddressSearch(
        s.renderer.SearchBarRect(static_cast<float>(s.compositor.Width())), s.scale);
    POINT anchor{static_cast<LONG>(layout.scope.left), static_cast<LONG>(layout.scope.bottom)};
    ClientToScreen(s.hwnd, &anchor);
    anchor.x -= ui::FluentMenu::kShadowMargin;
    anchor.y += static_cast<LONG>(4 * s.scale) - ui::FluentMenu::kShadowMargin;
    s.addressIgnoreKillFocus = true;
    const int command = s.menu->TrackPopup(anchor, std::move(items));
    if (command == 3) {
        std::wstring chosen;
        if (PickFolder(s, chosen, l10n::Get(l10n::StringId::SearchChooseScope).c_str(), L"search_scope")) {
            s.addressSearchRoot = chosen;
            s.addressSearchCurrent = true;
            QueueAddressSearch(s);
        }
    }
    if (command == 1 || command == 2) {
        s.addressSearchCurrent = command == 1;
        s.addressScopeAnimation = 1.0f;
        s.addressAnimationTick = GetTickCount64();
        QueueAddressSearch(s);
    }
    SetForegroundWindow(GetAncestor(s.hwndAddressEdit, GA_ROOT));
    SetFocus(s.hwndAddressEdit);
    s.addressIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool TickAddressSearch(AppState& s, ULONGLONG now) {
    bool searched = false;
    app::SetContentScopeMode(s.appPrefs.content_scope);
    if (s.searchOptionsPending && !s.searchHistoryOpen && (!s.menu || !s.menu->IsOpen())) {
        s.searchOptionsPending = false;
        ShowSearchOptions(s);
        return true;
    }
    if (now - s.contentStatusTick >= 750) {
        s.contentStatusTick = now;
        std::vector<index::VolumeInfo> volumes;
        std::vector<std::wstring> excluded;
        if (!s.isolatedTest && s.contentSearch.ConfigurationReady() && s.index.GetScope(volumes, excluded)) {
            const auto current = s.contentSearch.GetConfig();
            const auto shared = index::SharedContentScope(current, volumes, excluded);
            ipc::PayloadWriter old_scope, new_scope;
            index::content::PutConfig(old_scope, current);
            index::content::PutConfig(new_scope, shared);
            if (old_scope.data() != new_scope.data()) s.contentSearch.Configure(shared);
        }
        if (!s.isolatedTest) TickLegacyContentCleanup(s);
        const auto status = ContentIndexStatusText(s);
        if (status != s.contentStatusText) {
            s.contentStatusText = status;
            searched = true;
        }
        const auto progress = s.contentSearch.GetStatus();
        // The client runs one query at a time. Leave revisions unconsumed while
        // busy so another tick can refresh without cancelling an active query.
        bool content_busy = false;
        ForEachPane(s, [&](app::Pane& pane) {
            if (auto* tab = pane.ActiveTab())
                content_busy |= tab->search_content_active && tab->pending_generation != 0 &&
                    tab->pending_generation == s.contentSearch.CurrentGeneration();
        });
        ForEachPane(s, [&](app::Pane& pane) {
            auto* tab = pane.ActiveTab();
            std::wstring kind, rest;
            if (!tab || !app::ParsePulsePath(tab->current_path, &kind, &rest) ||
                kind != L"search" || !app::SplitSearchQueryText(rest).content.present()) return;
            if (tab->banner_title == l10n::Get(l10n::StringId::ContentIndexPartial) ||
                tab->banner_title == l10n::Get(l10n::StringId::ContentCoverageHint) ||
                tab->banner_title == l10n::Get(l10n::StringId::ContentIndexBuilding) ||
                tab->banner_title == l10n::Get(l10n::StringId::ContentIndexPaused)) {
                if (tab->search_content_active && !tab->search_allow_scan) {
                    // The query title and status bar already show live scan progress.
                    tab->banner_title.clear();
                    tab->banner_message.clear();
                    searched = true;
                } else {
                    const auto title = l10n::Get(progress.paused ? l10n::StringId::ContentIndexPaused :
                        progress.indexing ? l10n::StringId::ContentIndexBuilding :
                        progress.error || progress.skipped_files ? l10n::StringId::ContentIndexPartial : l10n::StringId::ContentCoverageHint);
                    searched |= tab->banner_title != title || tab->banner_message != status;
                    tab->banner_title = title;
                    tab->banner_message = status;
                }
            }
            // Instant mode has no index revision to trigger a retry when a
            // disabled drive becomes part of the shared scope again.
            if (!tab->search_content_stopped && !tab->search_content_active &&
                !s.addressLiveDue && s.renameIndex < 0 &&
                (tab->banner_title == l10n::Get(l10n::StringId::ContentIndexNotCovered) ||
                 tab->banner_title == l10n::Get(l10n::StringId::ContentIndexEmpty))) {
                index::ContentSearchRequest scope_request;
                scope_request.root = app::SplitSearchQueryText(rest).path_prefix;
                if (!index::ContentTaskRoots(s.contentSearch.GetConfig(), scope_request).empty()) {
                    RequestSearchPage(s, *tab, rest, true);
                    content_busy = tab->search_content_active;
                    searched = true;
                    return;
                }
            }
            if (tab->search_content_stopped || tab->content_subscription_error || s.contentSearch.InstantMode() || !progress.revision || tab->search_index_revision == progress.revision ||
                content_busy || tab->search_live_generation || tab->search_content_active || tab->search_content_empty || tab->search_allow_scan ||
                s.addressLiveDue || s.renameIndex >= 0) return;
            if (tab->selected_index >= 0 &&
                static_cast<size_t>(tab->selected_index) < tab->EntryCount())
                tab->search_preserve_selection = tab->EntryAt(static_cast<size_t>(tab->selected_index)).full_path;
            RequestSearchPage(s, *tab, rest, true);
            content_busy = tab->search_content_active;
            searched = true;
        });
    }
    if (s.addressLiveDue && now >= s.addressLiveDue && !s.addressSearchComposing) {
        s.addressLiveDue = 0;
        if (s.addressSearching && ActiveTab(s) && ActiveTab(s)->current_path == s.addressLiveContext) {
            SubmitAddressSearch(s, true);
            searched = true;
        }
    }
    if (s.addressHistoryDue && now >= s.addressHistoryDue) {
        s.addressHistoryDue = 0;
        if (ActiveTab(s) && ActiveTab(s)->current_path == s.addressHistoryPath)
            RecordSearchHistory(s, s.addressHistoryPath);
    }
    const float target = s.addressSearching ? 1.0f : 0.0f;
    if (s.addressSearchAnimation == target && s.addressScopeAnimation == 0.0f) return searched;
    BOOL animate = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
    const float dt = s.addressAnimationTick ? static_cast<float>(now - s.addressAnimationTick) : 16.0f;
    s.addressAnimationTick = now;
    const float blend = animate ? 1.0f - std::exp(-std::min(dt, 100.0f) / 55.0f) : 1.0f;
    s.addressSearchAnimation += (target - s.addressSearchAnimation) * blend;
    if (std::abs(s.addressSearchAnimation - target) < 0.005f) s.addressSearchAnimation = target;
    s.addressScopeAnimation *= 1.0f - blend;
    if (s.addressScopeAnimation < 0.005f) s.addressScopeAnimation = 0.0f;
    return true;
}

} // namespace pulse

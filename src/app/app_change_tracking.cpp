#include "app_internal.h"
#include "change_time.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../ui/fluent_menu.h"
#include <algorithm>
#include <cwctype>
#include <unordered_set>

namespace pulse {
namespace {
using I = l10n::StringId;
using K = index::ChangeKind;
using S = index::ChangeState;
const std::wstring& Text(I id) { return l10n::Get(id); }
std::wstring Key(std::wstring path) {
    path = fs::NormalizePath(std::move(path));
    while (path.size() > 7 && path.back() == L'\\') path.pop_back();
    for (auto& c : path) c = static_cast<wchar_t>(std::towlower(c));
    return path;
}
std::wstring KindText(uint32_t kind) {
    static constexpr I labels[] = {I::ChangeCreated, I::ChangeModified, I::ChangeDeleted,
        I::ChangeRenamed, I::ChangeMovedIn, I::ChangeMovedOut};
    return kind < std::size(labels) ? Text(labels[kind]) : Text(I::ChangeAll);
}
std::wstring DaysText(int days) {
    return Text(days == 1 ? I::ChangeToday : days == 3 ? I::ChangeLast3Days : I::ChangeLast7Days);
}
bool HasResponse(S state) { return state == S::Available || state == S::Gap; }
std::wstring SummaryKey(const std::wstring& path, int days) {
    auto key = Key(path) + L"\n" + std::to_wstring(days);
    if (days == 1) key += L":" + std::to_wstring(app::ChangeLocalMidnight(app::ChangeNow()));
    return key;
}
std::wstring AgeText(uint64_t time) {
    const uint64_t now = app::ChangeNow();
    if (time > now || now - time < 300) return Text(I::ChangeJustNow);
    const auto midnight = app::ChangeLocalMidnight(now);
    if (time >= midnight) return Text(I::ChangeToday);
    if (time >= app::ChangeLocalMidnight(midnight - 1)) return Text(I::ChangeYesterday);
    wchar_t text[80]{};
    swprintf_s(text, Text(I::ChangeDaysAgoFormat).c_str(),
        static_cast<int>(std::max<uint64_t>(2, (now - time) / 86400)));
    return text;
}
ui::ChangeBadge Badge(const index::ChangeSummary& summary, int days) {
    ui::ChangeBadge badge;
    const auto now = app::ChangeNow();
    if (!summary.count || summary.initial_count || !summary.last_change ||
        summary.last_change < app::ChangeSince(days, now) || summary.last_change > now + 60) return badge;
    badge.count = static_cast<int>(summary.count);
    badge.has_deleted = summary.has_deleted;
    badge.status = summary.last_change < app::ChangeLocalMidnight(app::ChangeNow()) ? 1 : 0;
    if (summary.count && summary.last_change >= app::ChangeSince(days, app::ChangeNow()))
        badge.label = AgeText(summary.last_change);
    badge.tooltip = DaysText(days) + L" · " + Text(I::ChangeIncludesChildren);
    if (summary.last_change) badge.tooltip += L"\n" + format::LocalFileTime(app::ChangeFileTime(summary.last_change));
    std::wstring counts;
    for (uint32_t i = 0; i < 6; ++i) {
        if (!summary.counts[i]) continue;
        if (!counts.empty()) counts += L" · ";
        counts += KindText(i) + L" " + std::to_wstring(summary.counts[i]);
    }
    if (!counts.empty()) badge.tooltip += L"\n" + counts;
    return badge;
}
std::wstring RootOf(const app::Tab& tab) {
    std::wstring kind, rest;
    app::ParsePulsePath(tab.current_path, &kind, &rest);
    return kind == L"changes" ? rest : std::wstring{};
}
std::wstring HitPath(AppState& s, const ui::HitTestResult& hit) {
    app::Pane* pane = PaneAtSlot(s, hit.pane_index);
    auto* tab = pane ? pane->ActiveTab() : nullptr;
    if (!tab) return {};
    if (hit.index < 0) return fs::IsVirtualPath(tab->current_path) ? RootOf(*tab) : tab->current_path;
    return EntryFullPath(*tab, hit.index);
}
}

void StartChangeTracking(AppState& s) {
    s.changes.enabled = s.appPrefs.change_tracking_enabled;
    s.changes.days = s.appPrefs.change_tracking_days;
    if (s.isolatedTest || s.shot.active) return;
    s.changes.client.Start(s.hwnd, WM_CHANGE_TRACKING);
    s.changes.client.SetEnabled(s.changes.enabled);
}

bool TickChangeTracking(AppState& s) {
    auto& state = s.changes;
    bool changed = false;
    const bool hovering = s.hoverRegion == static_cast<int>(ui::HitTestResult::ChangeBadge) ||
        s.hoverRegion == static_cast<int>(ui::HitTestResult::ChangeOpen);
    if (state.popover.visible && !hovering && GetTickCount64() > state.hover_deadline) {
        state.popover.visible = false;
        changed = true;
    }
    if (state.popover.visible) {
        const auto found = state.summaries.find(SummaryKey(state.hover_path, state.days));
        const auto badge = found == state.summaries.end() ? ui::ChangeBadge{} : Badge(found->second, state.days);
        if (badge.label.empty()) { state.popover.visible = false; changed = true; }
        else if (state.popover.summary != badge.tooltip) { state.popover.summary = badge.tooltip; changed = true; }
    }
    if (state.enabled != s.appPrefs.change_tracking_enabled || state.days != s.appPrefs.change_tracking_days) {
        state.enabled = s.appPrefs.change_tracking_enabled;
        state.days = s.appPrefs.change_tracking_days;
        state.client.SetEnabled(state.enabled);
        if (!state.enabled) state.summaries.clear();
        state.requested_paths.clear();
        state.popover.visible = false;
        state.summary_request = 0;
        state.summary_pending = false;
        state.details.clear();
        state.last_query = 0;
        ForEachPane(s, [&](app::Pane& pane) {
            auto* tab = pane.ActiveTab();
            if (!tab || RootOf(*tab).empty()) return;
            tab->loading = false;
            tab->change_request = 0;
            tab->change_days = state.days;
            if (state.enabled) LoadChangeView(s, *tab, RootOf(*tab));
            else {
                tab->banner_message = Text(I::ChangeScope);
                tab->change_empty_text = Text(I::ChangeDisabled);
                tab->change_status_text = tab->snapshot && !tab->snapshot->empty() ? Text(I::ChangeDisabled) : L"";
            }
        });
        changed = true;
    }
    if (!state.enabled || s.isolatedTest || s.shot.active) return changed;
    const uint64_t now = GetTickCount64();
    if (now - state.last_detail_refresh >= 5000) {
        state.last_detail_refresh = now;
        for (auto& pane : Panes(s)) {
            auto* tab = pane->ActiveTab();
            if (!tab || tab->loading || tab->SelectedCount() || tab->scroll_y != 0 ||
                tab->change_cursor || RootOf(*tab).empty()) continue;
            LoadChangeView(s, *tab, RootOf(*tab), false, true);
        }
    }
    if (now - state.last_query < 500) return changed;
    auto paths = state.visible_paths;
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    if (paths.size() > 256) paths.resize(256);
    if (paths != state.requested_paths || (!state.summary_pending && now - state.last_query >= 2000)) {
        state.last_query = now;
        state.requested_paths = paths;
        if (!paths.empty()) {
            state.summary_request = ++state.next_request;
            state.summary_pending = true;
            state.client.QuerySummaryAsync(paths, app::ChangeSince(state.days, app::ChangeNow()),
                state.summary_request);
        }
    }
    return changed;
}

void LoadChangeView(AppState& s, app::Tab& tab, const std::wstring& root, bool append, bool refresh) {
    const auto slash = root.find_last_of(L"\\/");
    tab.virtual_title = Text(I::ChangeTitle) + L" · " + root.substr(slash == std::wstring::npos ? 0 : slash + 1);
    tab.view_mode = ui::ViewMode::Details;
    tab.banner_title = Text(I::ChangeTitle);
    tab.banner_message = Text(I::ChangeScope);
    if (!refresh) tab.change_empty_text = Text(s.appPrefs.change_tracking_enabled ? I::ChangeScanning : I::ChangeDisabled);
    if (!append && !refresh) tab.change_status_text.clear();
    if (!append && !refresh) {
        tab.ClearSelection();
        tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
        tab.change_cursor = 0;
        tab.scroll_y = 0;
    }
    if (!s.appPrefs.change_tracking_enabled) { tab.loading = false; return; }
    tab.loading = true;
    const auto request = ++s.changes.next_request;
    std::erase_if(s.changes.details, [&](const auto& item) { return item.second.tab == &tab; });
    tab.change_request = request;
    s.changes.details[request] = {&tab, tab.current_path, tab.view_generation, append};
    s.changes.client.QueryDetailsAsync(root, app::ChangeSince(tab.change_days, app::ChangeNow()),
        append ? tab.change_cursor : 0, 200, request, tab.change_kind);
}

void OpenChangeView(AppState& s, const std::wstring& root) {
    if (root.empty() || fs::IsVirtualPath(root)) return;
    if (auto* tab = ActiveTab(s)) {
        tab->change_days = s.appPrefs.change_tracking_days;
        tab->change_kind = UINT32_MAX;
    }
    s.changes.popover.visible = false;
    NavigateTo(s, L"pulse:changes:" + fs::NormalizePath(root));
}

void ApplyChangeDetails(AppState& s, uint32_t request, const index::ChangeResponse& result) {
    auto& state = s.changes;
    const auto pending = state.details.find(request);
    if (pending == state.details.end()) return;
    const auto wanted = pending->second;
    state.details.erase(pending);
    if (!state.enabled || !s.appPrefs.change_tracking_enabled) return;
    ForEachPane(s, [&](app::Pane& pane) {
        auto* tab = pane.ActiveTab();
        if (!tab || tab != wanted.tab || tab->current_path != wanted.path ||
            tab->view_generation != wanted.generation || tab->change_request != request) return;
        tab->loading = false;
        tab->banner_title = Text(I::ChangeTitle);
        tab->banner_message = Text(I::ChangeScope);
        if (!HasResponse(result.state)) {
            const bool has_entries = tab->snapshot && !tab->snapshot->empty();
            const auto message = result.state == S::NotCovered ? I::ChangeNotCovered :
                has_entries ? I::ChangeRefreshFailed : I::ChangeReadFailed;
            tab->change_empty_text = Text(message);
            tab->change_status_text = has_entries ? Text(message) : L"";
            return;
        }
        auto entries = std::make_shared<std::vector<fs::DirEntry>>();
        if (wanted.append && tab->snapshot) *entries = *tab->snapshot;
        for (const auto& record : result.records) {
            if (record.source != index::ChangeSource::Event) continue;
            fs::DirEntry entry;
            entry.full_path = record.kind == K::MovedOut && !record.old_path.empty()
                ? record.old_path : record.path;
            entry.change_old_path = record.old_path;
            const auto slash = entry.full_path.find_last_of(L"\\/");
            entry.name = entry.full_path.substr(slash == std::wstring::npos ? 0 : slash + 1);
            entry.is_dir = record.is_dir;
            entry.attrs = entry.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            entry.mtime = app::ChangeFileTime(record.time);
            entry.change_type_text = record.source == index::ChangeSource::InitialMtime
                ? Text(I::ChangeInitialMtime) : KindText(static_cast<uint32_t>(record.kind));
            entry.change_record_only = record.kind == K::Deleted || record.kind == K::MovedOut;
            entries->push_back(std::move(entry));
        }
        tab->SetSnapshot(entries);
        tab->loading = false;
        tab->change_cursor = result.next_cursor;
        tab->change_empty_text = Text(I::ChangeNoRecords);
        tab->change_status_text.clear();
    });
}

void ApplyChangeSummaries(AppState& s, uint32_t request, const index::ChangeResponse& result) {
    auto& state = s.changes;
    if (request != state.summary_request || !state.enabled || !s.appPrefs.change_tracking_enabled ||
        state.days != s.appPrefs.change_tracking_days) return;
    state.summary_pending = false;
    std::unordered_set<std::wstring> requested;
    for (const auto& path : state.requested_paths) requested.insert(SummaryKey(path, state.days));
    for (const auto& summary : result.summaries) {
        const auto key = SummaryKey(summary.path, state.days);
        if (!requested.contains(key)) continue;
        // A failed/missing reply is not an authoritative empty result.
        if (HasResponse(summary.state) || summary.state == S::NotCovered || summary.count)
            state.summaries[key] = summary;
    }
    for (auto it = state.summaries.begin(); state.summaries.size() > 2048 && it != state.summaries.end();) {
        if (!requested.contains(it->first)) it = state.summaries.erase(it);
        else ++it;
    }
}

void ReceiveChangeTracking(AppState& s, uint32_t request, bool details) {
    index::ChangeResponse result;
    auto& state = s.changes;
    if (!details) {
        if (!state.client.TakeSummary(request, result)) return;
        ApplyChangeSummaries(s, request, result);
    } else {
        if (!state.client.TakeDetails(request, result)) return;
        ApplyChangeDetails(s, request, result);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void FillChangePane(AppState& s, app::Tab& tab, ui::PaneViewModel& pane, const D2D1_RECT_F& bounds) {
    const auto root = RootOf(tab);
    pane.change_badges.clear();
    pane.title_change_badge = {};
    pane.is_changes = !root.empty();
    if (pane.is_changes) {
        pane.is_search = true;
        pane.search_retaining_results = tab.snapshot && !tab.snapshot->empty();
        pane.curated_order = true;
        pane.change_time_label = DaysText(tab.change_days);
        pane.change_type_label = KindText(tab.change_kind);
        pane.change_has_more = tab.change_cursor != 0 && !tab.loading;
        pane.date_column_label = Text(I::ChangeTime);
        pane.banner_message = Text(I::ChangeScope);
        pane.change_empty_text = tab.change_empty_text.empty() ? Text(I::ChangeScanning) : tab.change_empty_text;
        pane.change_status_text = tab.change_status_text;
    }
    if (!s.appPrefs.change_tracking_enabled) return;
    auto add = [&](const std::wstring& path, int index) {
        if (path.empty() || fs::IsVirtualPath(path)) return;
        const auto key = Key(path);
        s.changes.visible_paths.push_back(key);
        const auto found = s.changes.summaries.find(SummaryKey(path, s.appPrefs.change_tracking_days));
        index::ChangeSummary missing;
        missing.state = S::Scanning;
        const auto badge = Badge(found == s.changes.summaries.end() ? missing : found->second,
            s.appPrefs.change_tracking_days);
        if (index < 0) pane.title_change_badge = badge;
        else if (!badge.label.empty()) pane.change_badges[index] = badge;
    };
    add(root.empty() ? tab.current_path : root, -1);
    if (pane.is_changes || !tab.snapshot) return;
    const auto [first, last] = s.renderer.VisibleRangeInPane(pane, bounds);
    for (int view = first; view < last && view < static_cast<int>(pane.EntryCount()); ++view) {
        const int source = pane.SourceIndex(view);
        if (source < 0 || source >= static_cast<int>(tab.snapshot->size())) continue;
        const auto& entry = (*tab.snapshot)[static_cast<size_t>(source)];
        if (entry.is_dir && !entry.change_record_only) add(EntryFullPath(tab, source), source);
    }
}

void UpdateChangeHover(AppState& s, const ui::HitTestResult& hit, POINT point) {
    auto& state = s.changes;
    if (hit.region == ui::HitTestResult::ChangeOpen) { state.hover_deadline = GetTickCount64() + 350; return; }
    if (hit.region != ui::HitTestResult::ChangeBadge) return;
    const auto path = HitPath(s, hit);
    const auto found = state.summaries.find(SummaryKey(path, s.appPrefs.change_tracking_days));
    if (found == state.summaries.end()) return;
    const auto badge = Badge(found->second, s.appPrefs.change_tracking_days);
    if (badge.label.empty()) { state.popover.visible = false; return; }
    if (state.hover_path != path || !state.popover.visible) {
        state.hover_path = path;
        state.popover.x = static_cast<float>(point.x);
        state.popover.y = static_cast<float>(point.y);
    }
    state.popover.visible = true;
    state.popover.summary = badge.tooltip;
    state.popover.pane_index = hit.pane_index;
    state.popover.row_index = hit.index;
    state.hover_deadline = GetTickCount64() + 350;
    s.tooltipText.clear();
}

void FillChangePopover(AppState& s, ui::WindowViewModel& vm) {
    vm.change_popover = s.changes.popover;
    if (vm.change_popover.visible) vm.tooltip_text.clear();
}

bool HandleChangeClick(AppState& s, const ui::HitTestResult& hit, POINT point) {
    using R = ui::HitTestResult;
    if (hit.region == R::SettingsChangeDays) {
        static constexpr int days[] = {1, 3, 7};
        if (hit.index >= 0 && hit.index < 3) s.settings.ChangeTrackingDays(days[hit.index]);
        return true;
    }
    if (hit.region == R::ChangeBadge || hit.region == R::ChangeOpen) {
        const auto root = hit.region == R::ChangeOpen ? s.changes.hover_path : HitPath(s, hit);
        OpenChangeView(s, root);
        return true;
    }
    auto* tab = ActiveTab(s);
    if (!tab || RootOf(*tab).empty()) return false;
    if (hit.region == R::ChangeMore) { LoadChangeView(s, *tab, RootOf(*tab), true); return true; }
    if (hit.region != R::ChangeTimeFilter && hit.region != R::ChangeTypeFilter) return false;
    if (!EnsureMenu(s)) return true;
    std::vector<ui::FluentMenuItem> items;
    const bool time = hit.region == R::ChangeTimeFilter;
    for (int i = 0; i < (time ? 3 : 7); ++i) {
        ui::FluentMenuItem item;
        item.command = i + 1;
        item.text = time ? DaysText(i == 0 ? 1 : i == 1 ? 3 : 7) : KindText(i == 0 ? UINT32_MAX : i - 1);
        items.push_back(std::move(item));
    }
    ClientToScreen(s.hwnd, &point);
    const int choice = s.menu->TrackPopup(point, std::move(items));
    if (choice > 0) {
        if (time) tab->change_days = choice == 1 ? 1 : choice == 2 ? 3 : 7;
        else tab->change_kind = choice == 1 ? UINT32_MAX : choice - 2;
        LoadChangeView(s, *tab, RootOf(*tab));
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
    return true;
}
}

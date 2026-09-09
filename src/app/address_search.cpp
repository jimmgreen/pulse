#include "app_internal.h"
#include "search_query.h"
#include "../common/localization.h"
#include "../ui/address_search_layout.h"
#include <algorithm>
#include <cmath>

namespace pulse {

bool IsAddressSearchResults(const app::Tab* tab) {
    std::wstring kind;
    return tab && app::ParsePulsePath(tab->current_path, &kind, nullptr) && kind == L"search";
}

static std::wstring SearchOrigin(const app::Tab& tab) {
    if (tab.search_origin_valid) return tab.search_origin_path;
    auto history = tab.back_stack;
    while (!history.empty()) {
        const auto path = history.top();
        history.pop();
        if (!fs::IsVirtualPath(path)) return path;
    }
    return {};
}

static void RestoreSearchDraft(app::Tab& tab) {
    if (tab.search_input_path == tab.current_path) return;
    std::wstring rest;
    app::ParsePulsePath(tab.current_path, nullptr, &rest);
    const auto spec = app::ParseSearchQuery(rest);
    tab.search_input_path = tab.current_path;
    tab.search_input_text = spec.name;
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
}

void FillAddressSearchView(AppState& s, ui::WindowViewModel& vm) {
    auto* tab = ActiveTab(s);
    if (!s.addressEditing && IsAddressSearchResults(tab)) {
        RestoreSearchDraft(*tab);
        vm.address_searching = true;
        vm.address_search_current = tab->search_input_current;
        vm.address_search_text = tab->search_input_text;
        vm.address_search_has_text = !vm.address_search_text.empty();
    }
}

void ExitAddressSearch(AppState& s) {
    const auto* tab = ActiveTab(s);
    const bool results = IsAddressSearchResults(tab);
    const auto origin = results ? SearchOrigin(*tab) : std::wstring{};
    HideAddressEditor(s, false);
    if (results) NavigateTo(s, origin);
}

void ShowAddressSearch(AppState& s) {
    if (s.addressSearching && s.hwndAddressEdit) {
        SetForegroundWindow(s.hwndAddressEdit);
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
    } else {
        s.addressSearchRoot = fs::IsVirtualPath(tab->current_path) ? L"" : tab->current_path;
        s.addressSearchCurrent = false;
    }
    ShowAddressEditor(s);
    if (!s.addressEditing || !s.hwndAddressEdit) return;
    s.addressSearching = true;
    s.addressAnimationTick = GetTickCount64();
    SetWindowTextW(s.hwndAddressEdit, query.c_str());
    SendMessageW(s.hwndAddressEdit, EM_SETSEL, 0, -1);
    const auto cue = l10n::Get(l10n::StringId::Search);
    SendMessageW(s.hwndAddressEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
    LayoutAddressEditor(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void SubmitAddressSearch(AppState& s) {
    auto* tab = ActiveTab(s);
    if (!tab) return;
    const int length = GetWindowTextLengthW(s.hwndAddressEdit);
    std::wstring query(static_cast<size_t>(length) + 1, L'\0');
    query.resize(GetWindowTextW(s.hwndAddressEdit, query.data(), length + 1));
    const auto first = query.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return;
    query = query.substr(first, query.find_last_not_of(L" \t\r\n") - first + 1);
    const bool continuing = IsAddressSearchResults(tab);
    std::wstring previous_query;
    if (continuing) app::ParsePulsePath(tab->current_path, nullptr, &previous_query);
    auto spec = continuing ? app::ParseSearchQuery(previous_query) : app::AdvancedSearchSpec{};
    spec.name = query;
    spec.current_folder = s.addressSearchRoot;
    spec.custom_folder.clear();
    spec.location = s.addressSearchCurrent && !spec.current_folder.empty()
        ? app::LocationScope::CurrentFolder : app::LocationScope::Indexed;
    const auto path = app::MakeSearchPath(app::CompileSearchQuery(spec));
    if (!continuing) {
        tab->search_origin_path = tab->current_path;
        tab->search_origin_valid = true;
    }
    const auto previous_results = continuing ? tab->snapshot : fs::SnapshotPtr{};
    HideAddressEditor(s, false);
    NavigateTo(s, path);
    if (previous_results && !previous_results->empty() && tab->loading) {
        tab->SetSnapshot(previous_results);
        tab->search_retaining_results = true;
    }
    tab->search_input_path = tab->current_path;
    tab->search_input_text = query;
    tab->search_input_root = s.addressSearchRoot;
    tab->search_input_current = s.addressSearchCurrent;
    ShowAddressSearch(s);
}

void ShowAddressSearchScope(AppState& s) {
    if (!s.addressSearching || !EnsureMenu(s)) return;
    std::vector<ui::FluentMenuItem> items(2);
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
    const auto layout = ui::LayoutAddressSearch(
        s.renderer.AddressBarRect(static_cast<float>(s.compositor.Width())), s.scale);
    POINT anchor{static_cast<LONG>(layout.scope.left), static_cast<LONG>(layout.scope.bottom)};
    ClientToScreen(s.hwnd, &anchor);
    s.addressIgnoreKillFocus = true;
    const int command = s.menu->TrackPopup(anchor, std::move(items));
    if (command == 1 || command == 2) {
        s.addressSearchCurrent = command == 1;
        s.addressScopeAnimation = 1.0f;
        s.addressAnimationTick = GetTickCount64();
    }
    SetForegroundWindow(s.hwndAddressEdit);
    SetFocus(s.hwndAddressEdit);
    s.addressIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool TickAddressSearch(AppState& s, ULONGLONG now) {
    const float target = s.addressSearching ? 1.0f : 0.0f;
    if (s.addressSearchAnimation == target && s.addressScopeAnimation == 0.0f) return false;
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

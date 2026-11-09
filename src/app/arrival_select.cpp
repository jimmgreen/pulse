// arrival_select.cpp - see arrival_select.h.
#include "arrival_select.h"
#include "app_internal.h"
#include "../ui/ui_renderer.h"
#include "window_registry.h"
#include "pane_layout.h"
#include "../common/path_utils.h"
#include "../ops/ops_manager.h"
#include <algorithm>
#include <cwctype>

namespace pulse {
namespace {

constexpr uint64_t kAfterTaskMs = 2500;      // the watcher's last events
constexpr uint64_t kAfterArrivalMs = 3000;   // shell verbs: more may follow
constexpr uint64_t kHardCapMs = 10 * 60 * 1000;

std::wstring Lower(std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(std::towlower(c));
    return text;
}

std::wstring Leaf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool SameDir(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(fs::NormalizePath(a).c_str(), fs::NormalizePath(b).c_str()) == 0;
}

std::vector<std::wstring> SelectedNames(const app::Tab& tab) {
    std::vector<std::wstring> names;
    if (tab.SelectedCount() <= 0) return names;
    for (int index : tab.SelectedIndices())
        if (index >= 0 && index < static_cast<int>(tab.EntryCount()))
            names.push_back(Lower(std::wstring(tab.EntryAt(static_cast<size_t>(index)).name)));
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace

void CenterResultInPane(AppState& s, app::Pane& pane, app::Tab& tab, int index) {
    ui::PaneViewModel vm;
    app::FillPaneViewModel(vm, pane, &s.places);
    int row = index;
    if (vm.filter_map || !vm.filter_text.empty()) {
        row = vm.ViewIndex(index);
        if (row < 0) return;
    }
    const D2D1_RECT_F content = s.renderer.ContentRect(
        static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    const D2D1_RECT_F bounds = &pane == s.pane ? FocusedPaneRect(s) :
        app::FocusedWindowPaneRect(Root(s).get(), &pane, content, s.scale);
    const D2D1_RECT_F list = s.renderer.PaneListRect(vm, bounds);
    const D2D1_RECT_F item = s.renderer.ItemRectInPane(vm, bounds, row);
    if (list.bottom <= list.top) return;
    tab.scroll_y += (item.top + item.bottom - list.top - list.bottom) * 0.5f;
    tab.scroll_y = std::clamp(tab.scroll_y, 0.0f, s.renderer.MaxScrollForPane(vm, bounds));
    if (&pane == s.pane) {
        s.scrollTargetY = tab.scroll_y;
        s.scrollAnimating = false;
    }
}

namespace {
void AddExpected(AppState& window, const std::vector<std::wstring>& paths) {
    auto& a = window.arrivalSelect;
    for (const auto& path : paths)
        if (SameDir(fs::ParentPath(path), a.dir)) a.expected.insert(Lower(Leaf(path)));
}

} // namespace

bool ShellVerbMayCreate(const std::wstring& verb) {
    static const wchar_t* const kNoArrival[] = {
        L"open", L"opennew", L"openas", L"openwith", L"edit", L"print", L"printto", L"properties",
        L"runas", L"runasuser", L"find", L"explore", L"copy", L"cut", L"delete", L"rename",
        L"play", L"preview", L"pintohome", L"pintostartscreen", L"cast", L"share", L"refresh",
        L"copyaspath", L"opencontaining", L"opendevice", L"eject", L"format",
    };
    for (const wchar_t* name : kNoArrival)
        if (_wcsicmp(verb.c_str(), name) == 0) return false;
    return true;
}

void ArmArrivalSelection(AppState& s, const app::Pane* pane, uint64_t task_id, uint32_t settle_ms) {
    if (!pane) pane = s.pane;
    const app::Tab* tab = pane ? pane->ActiveTab() : nullptr;
    if (!tab || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path)) return;
    auto& a = s.arrivalSelect;
    const std::wstring dir = fs::NormalizePath(tab->current_path);
    const uint64_t now = GetTickCount64();
    // Two quick pastes into one folder: keep the first one's baseline, so
    // its items stay selected along with the second's.
    const bool again = !a.dir.empty() && a.pane == pane && _wcsicmp(a.dir.c_str(), dir.c_str()) == 0 &&
        (!a.until || now <= a.until);
    if (!again) {
        a = {};
        a.dir = dir;
        a.pane = pane;
        if (tab->snapshot)
            for (size_t i = 0; i < tab->EntryCount(); ++i)
                a.baseline.insert(Lower(std::wstring(tab->EntryAt(i).name)));
    }
    a.armed_at = now;
    a.task_id = task_id;
    a.until = task_id ? 0 : now + settle_ms;
}

void ExpectArrival(AppState& s, const std::wstring& path) {
    if (s.arrivalSelect.dir.empty()) return;
    AddExpected(s, {path});
}

void NoteArrivalCompletion(AppState& s, const ops::CompletedOperation& completed) {
    if (!completed.task_id) return;
    app::ForEachWindowSharingCore(s, [&](AppState& window) {
        if (window.arrivalSelect.task_id == completed.task_id) AddExpected(window, completed.destinations);
    });
}

void NoteArrivalFinished(AppState& s, const ops::FinishedTask& finished) {
    if (!finished.task_id) return;
    app::ForEachWindowSharingCore(s, [&](AppState& window) {
        auto& a = window.arrivalSelect;
        if (a.task_id != finished.task_id) return;
        a.task_id = 0;
        a.until = GetTickCount64() + kAfterTaskMs;
    });
}

bool ApplyArrivalSelection(AppState& s, app::Pane& pane, app::Tab& tab, bool /*focused*/) {
    auto& a = s.arrivalSelect;
    if (a.dir.empty()) return false;
    const uint64_t now = GetTickCount64();
    if ((a.until && now > a.until) || now - a.armed_at > kHardCapMs) {
        a = {};
        return false;
    }
    if (&pane != a.pane || !tab.snapshot || !SameDir(tab.current_path, a.dir)) return false;
    const std::vector<std::wstring> current = SelectedNames(tab);
    if (!a.chosen.empty() && current != a.chosen) {
        // Selected something else by hand: leave it be.
        a = {};
        return false;
    }
    std::vector<std::wstring> pick;
    for (size_t i = 0; i < tab.EntryCount(); ++i) {
        if (!tab.EntryVisible(static_cast<int>(i))) continue;
        std::wstring name(tab.EntryAt(i).name);
        const std::wstring key = Lower(name);
        if ((!a.expected_only && !a.baseline.contains(key)) || a.expected.contains(key))
            pick.push_back(std::move(name));
    }
    if (pick.empty()) return false;
    std::vector<std::wstring> keys;
    keys.reserve(pick.size());
    for (const auto& name : pick) keys.push_back(Lower(name));
    std::sort(keys.begin(), keys.end());
    if (!a.task_id) a.until = std::max(a.until, now + kAfterArrivalMs);
    if (keys == current && !a.chosen.empty()) {
        a.chosen = std::move(keys);
        return false;
    }
    tab.RemapSelection(pick, pick.front(), false);
    a.chosen = SelectedNames(tab);
    if (tab.selected_index >= 0) CenterResultInPane(s, pane, tab, tab.selected_index);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}
}

#include "picker_saved_followup.h"
#include "picker_saved_followup_policy.h"
#include "app_internal.h"
#include "drag_shake_watcher.h"
#include "global_search_controller.h"
#include "pane_layout.h"
#include "picker_places_catalog.h"
#include "shelf_snippets.h"
#include "tray_reveal.h"
#include "../common/picker_save_request.h"
#include "../common/picker_save_watch.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../ops/clipboard.h"
#include "../ui/picker_saved_toast.h"
#include "../ui/ui_renderer.h"
#include <algorithm>
#include <unordered_map>

namespace pulse {
namespace {
struct Target {
    AppState* window = nullptr;
    app::LayoutTab* layout = nullptr;
    app::Pane* pane = nullptr;
    app::Tab* tab = nullptr;
};
struct Followup {
    picker_save::Request request;
    app::Tab* tab = nullptr;
    app::PickerSavedSelection selection;
    uint64_t until = 0;
    uint64_t input_tick = 0;
    bool completed = false;
};
struct Watch {
    picker_save::Observer observer;
    std::unordered_map<uint64_t, Followup> items;
    std::unordered_map<WPARAM, std::wstring> actions;
    WPARAM next_action = 1;
};
std::unordered_map<AppCore*, std::unique_ptr<Watch>> watches;
UINT ShowMessage() { static const UINT value = RegisterWindowMessageW(L"Pulse.PickerSaved.Show.1"); return value; }
UINT CopyMessage() { static const UINT value = RegisterWindowMessageW(L"Pulse.PickerSaved.Copy.1"); return value; }
bool SamePath(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(fs::NormalizePath(a).c_str(), fs::NormalizePath(b).c_str()) == 0;
}
Target FindTarget(AppState& state, const std::wstring& parent, const app::Tab* required = nullptr) {
    Target result;
    state.core.windows.ForEach([&](HWND, AppState& window) {
        if (result.tab) return;
        for (auto& layout : window.window_tabs.items) {
            for (auto& pane : layout->panes) {
                auto& tab = pane->view;
                if ((!required || required == &tab) && SamePath(tab.current_path, parent)) {
                    result = {&window, layout.get(), pane.get(), &tab};
                    return;
                }
            }
        }
    });
    return result;
}
bool Interacting(const AppState& s) {
    return s.renameIndex >= 0 || s.listSelection.BandActive() || s.listSelection.PendingBand() ||
        s.listSelection.PendingFile() || app::IsInternalFileDragActive() || s.tabDragging ||
        s.paneDragPending || s.trayDrag.active || s.trayDragOut || s.dropRow >= 0 ||
        s.dropTray || s.dropSidebar >= 0 || s.dropBreadcrumb >= 0 || s.splitterDragging || s.scrollbarDragging;
}
void CenterTarget(Target target, int index) {
    auto& s = *target.window;
    auto& tab = *target.tab;
    ui::PaneViewModel vm;
    app::FillPaneViewModel(vm, *target.pane, &s.places);
    const int row = vm.ViewIndex(index);
    if (row < 0) return;
    const auto content = s.renderer.ContentRect(static_cast<float>(s.compositor.Width()),
        static_cast<float>(s.compositor.Height()));
    const auto bounds = app::FocusedWindowPaneRect(target.layout->root.get(), target.pane, content, s.scale);
    const auto list = s.renderer.PaneListRect(vm, bounds);
    const auto item = s.renderer.ItemRectInPane(vm, bounds, row);
    if (list.bottom > list.top) {
        tab.scroll_y = std::clamp(tab.scroll_y + (item.top + item.bottom - list.top - list.bottom) * 0.5f,
            0.0f, s.renderer.MaxScrollForPane(vm, bounds));
        if (target.pane == s.pane) { s.scrollTargetY = tab.scroll_y; s.scrollAnimating = false; }
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
void Replace(std::wstring& text, const wchar_t* token, const std::wstring& value) {
    const size_t at = text.find(token);
    if (at != std::wstring::npos) text.replace(at, wcslen(token), value);
}
void Notify(AppState& s, Watch& watch, const picker_save::Completion& completion) {
    using I = l10n::StringId;
    const auto& request = completion.request;
    std::wstring title = l10n::Get(I::PickerSavedObserved);
    Replace(title, L"{name}", BaseName(request.path));
    std::wstring detail = l10n::Get(I::PickerSavedDetail);
    Replace(detail, L"{folder}", fs::ParentPath(request.path));
    Replace(detail, L"{size}", format::ByteSize(completion.after.size));
    Replace(detail, L"{app}", request.app_name);
    const WPARAM token = watch.next_action++;
    if (watch.actions.size() >= 64) watch.actions.clear();
    watch.actions.emplace(token, request.path);
    ui::ShowPickerSavedToast(s.core.host.Hwnd(), s.darkMode, s.accentColor,
        std::move(title), std::move(detail), {
            {l10n::Get(I::AiAsstShowInPulse), ShowMessage(), token},
            {l10n::Get(I::CopyPath), CopyMessage(), token}});
}
}

bool HandlePickerSaveWatch(AppState& s, const COPYDATASTRUCT* data) {
    picker_save::Request request;
    if (!picker_save::Decode(data, request)) return false;
    auto& slot = watches[&s.core];
    if (!slot) slot = std::make_unique<Watch>();
    auto& watch = *slot;
    if (watch.items.contains(request.request_id) || watch.items.size() >= 128) return false;
    Followup item;
    item.request = request;
    item.until = GetTickCount64() + 130000;
    const auto target = FindTarget(s, fs::ParentPath(request.path));
    if (target.tab) {
        item.tab = target.tab;
        item.input_tick = target.window->last_input_tick;
        item.selection = app::ArmPickerSavedSelection(*target.tab, Interacting(*target.window));
    }
    if (!watch.observer.Submit(request)) return false;
    watch.items.emplace(request.request_id, std::move(item));
    SetTimer(s.core.host.Hwnd(), kPickerSaveWatchTimer, 250, nullptr);
    return true;
}

void TickPickerSaveWatch(AppState& s) {
    const auto found = watches.find(&s.core);
    if (found == watches.end()) return;
    auto& watch = *found->second;
    const uint64_t now = GetTickCount64();
    for (auto& [id, item] : watch.items) {
        (void)id;
        if (!item.tab || !item.selection.armed) continue;
        const auto target = FindTarget(s, fs::ParentPath(item.request.path), item.tab);
        if (!target.tab || target.window->last_input_tick != item.input_tick ||
            !app::CanApplyPickerSavedSelection(*target.tab, item.selection, Interacting(*target.window)))
            item.selection.armed = false;
        else {
            item.selection.selection_revision = target.tab->selection_revision;
            item.selection.snapshot = target.tab->snapshot;
        }
    }
    for (const auto& completion : watch.observer.Drain()) {
        const auto it = watch.items.find(completion.request.request_id);
        if (it == watch.items.end()) continue;
        auto& item = it->second;
        item.completed = true;
        item.until = now + 30000;
        const auto parent = fs::ParentPath(item.request.path);
        auto target = FindTarget(s, parent, item.tab);
        if (!target.tab) target = FindTarget(s, parent);
        if (target.tab) {
            if (!item.tab) {
                item.selection = app::ArmPickerSavedSelection(*target.tab, Interacting(*target.window));
                item.input_tick = target.window->last_input_tick;
            }
            else if (target.tab != item.tab) item.selection.armed = false;
            item.tab = target.tab;
            target.window->store.MarkDirty(parent);
            RefreshPath(*target.window, parent, RefreshReason::OperationCompleted);
            InvalidateRect(target.window->hwnd, nullptr, FALSE);
        } else if (item.request.notify_if_no_tab) Notify(s, watch, completion);
        if (item.request.stage_to_tray) {
            const auto shelf_dir = app::DefaultShelfSnippetsDir();
            app::TrayItem staged;
            staged.path = item.request.path;
            staged.display_name = BaseName(item.request.path);
            staged.source_label = item.request.app_name;
            staged.size = completion.after.size;
            staged.attrs = completion.after.attributes;
            staged.exists = completion.after.exists;
            staged.content_kind = 0;
            staged.owned_snippet = !shelf_dir.empty() && SamePath(parent, shelf_dir);
            if (s.tray.CollectRichToShelf(0, {std::move(staged)}, false, nullptr, true) > 0) {
                app::ForEachWindowSharingCore(s, [](AppState& window) {
                    InvalidateRect(window.hwnd, nullptr, FALSE);
                });
                PublishPickerPlaces(s, true);
            }
        }
        if (item.request.copy_path) ops::WriteClipboardText(item.request.path);
    }
    for (auto it = watch.items.begin(); it != watch.items.end();) {
        auto& item = it->second;
        if (now >= item.until) {
            if (item.completed) {
                const auto target = FindTarget(s, fs::ParentPath(item.request.path), item.tab);
                if (target.window) InvalidateRect(target.window->hwnd, nullptr, FALSE);
            }
            watch.observer.Cancel(it->first); it = watch.items.erase(it); continue;
        }
        if (item.completed && item.selection.armed && item.tab) {
            const auto target = FindTarget(s, fs::ParentPath(item.request.path), item.tab);
            if (target.tab) {
                const int index = app::ApplyPickerSavedSelection(*target.tab, item.selection,
                    BaseName(item.request.path), Interacting(*target.window));
                if (index >= 0) CenterTarget(target, index);
            } else item.selection.armed = false;
        }
        ++it;
    }
    if (watch.items.empty()) KillTimer(s.core.host.Hwnd(), kPickerSaveWatchTimer);
}

bool HandlePickerSavedAction(AppState& s, UINT message, WPARAM token) {
    if (message != ShowMessage() && message != CopyMessage()) return false;
    const auto found = watches.find(&s.core);
    if (found == watches.end()) return true;
    auto& actions = found->second->actions;
    const auto action = actions.find(token);
    if (action == actions.end()) return true;
    const std::wstring path = action->second;
    actions.erase(action);
    if (message == CopyMessage()) ops::WriteClipboardText(path);
    else {
        AppState& window = app::CurrentWindow(s);
        OpenFolderTab(window, fs::ParentPath(path));
        if (auto* tab = ActiveTab(window)) SelectNameInTab(window, *tab, BaseName(path));
        RevealWindow(window);
    }
    return true;
}

void FillPickerSavedBadges(AppState& s, app::Tab& tab, ui::PaneViewModel& pane) {
    const auto found = watches.find(&s.core);
    if (found == watches.end() || !tab.snapshot) return;
    for (const auto& [id, item] : found->second->items) {
        (void)id;
        if (!item.completed || item.tab != &tab || GetTickCount64() >= item.until ||
            !SamePath(tab.current_path, fs::ParentPath(item.request.path))) continue;
        const auto name = BaseName(item.request.path);
        for (int i = 0; i < static_cast<int>(tab.EntryCount()); ++i) {
            if (_wcsicmp(tab.EntryAt(static_cast<size_t>(i)).name.c_str(), name.c_str()) != 0) continue;
            ui::ChangeBadge badge;
            badge.label = l10n::Get(l10n::StringId::PickerNewBadge);
            badge.tooltip = item.request.path;
            pane.change_badges[i] = std::move(badge);
            break;
        }
    }
}
void StopPickerSaveWatch(AppState& s) {
    KillTimer(s.core.host.Hwnd(), kPickerSaveWatchTimer);
    watches.erase(&s.core);
}
}
#ifdef PULSE_WITH_TEST_HOOKS
#include <filesystem>
#include <fstream>
#include <thread>
namespace pulse {
int RunPickerSavedFollowupProbe(AppState& s, const wchar_t* report_path) {
    if (!s.isolatedTest) return 2;
    FILE* report = nullptr;
    if (_wfopen_s(&report, report_path, L"w") || !report) return 2;
    int failures = 0;
    auto check = [&](bool value, const char* label) {
        fprintf(report, "[%s] %s\n", value ? "PASS" : "FAIL", label);
        fflush(report); failures += !value;
    };
    auto pump = [&](const auto& done, DWORD timeout) {
        const auto until = GetTickCount64() + timeout;
        do {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); return false; }
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            if (done()) return true;
            Sleep(10);
        } while (GetTickCount64() < until);
        return done();
    };
    const auto fixture = std::filesystem::absolute(std::filesystem::path(report_path).parent_path() /
        (L"save-followup-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(fixture / L"open");
    std::filesystem::create_directories(fixture / L"closed");
    const std::wstring folder = (fixture / L"open").wstring();
    { std::ofstream stream(fixture / L"open" / L"old.txt"); stream << "baseline"; }
    OpenTabAt(s, folder);
    auto* tab = ActiveTab(s);
    check(tab && pump([&] { return !tab->pending_generation && tab->snapshot; }, 10000), "real directory worker initial listing ready");
    if (!tab || !tab->snapshot) { fclose(report); return 3; }
    check(!IsWindowVisible(s.hwnd), "isolated main window remains hidden");
    const HWND foreground = GetForegroundWindow();
    uint64_t request_id = 1000;
    auto send = [&](const std::wstring& path, bool notify) {
        picker_save::Request request;
        request.path = path; request.app_name = L"Pulse hidden save probe";
        request.source_pid = GetCurrentProcessId(); request.request_id = ++request_id;
        request.notify_if_no_tab = notify;
        const auto bytes = picker_save::Encode(request, s.hwnd);
        COPYDATASTRUCT data{picker_save::kMessage, static_cast<DWORD>(bytes.size()), const_cast<BYTE*>(bytes.data())};
        check(SendMessageW(s.core.host.Hwnd(), WM_COPYDATA, reinterpret_cast<WPARAM>(s.hwnd),
            reinterpret_cast<LPARAM>(&data)) == TRUE, "hidden host accepts encoded observer request");
        return request.request_id;
    };
    auto write = [](const std::wstring& path) {
        std::thread worker([path] { Sleep(100); std::ofstream stream(std::filesystem::path(path), std::ios::binary); stream << "real application writer output"; });
        worker.join();
    };
    auto finished = [&](uint64_t id) {
        return pump([&] {
            const auto watch = watches.find(&s.core);
            if (watch == watches.end()) return false;
            const auto item = watch->second->items.find(id);
            return item != watch->second->items.end() && item->second.completed;
        }, 12000);
    };
    auto selected_name = [&] { return tab->selected_index < 0 ? std::wstring{} : tab->EntryAt(static_cast<size_t>(tab->selected_index)).name; };
    auto diagnostics = [&](uint64_t id) {
        const auto& item = watches.at(&s.core)->items.at(id);
        fprintf(report, "STATE id=%llu completed=%d armed=%d rows=%zu selected=%d loading=%d pending=%llu revision=%llu expected_revision=%llu generation=%llu expected_generation=%llu input=%llu expected_input=%llu\n",
            id, item.completed, item.selection.armed, tab->EntryCount(), tab->selected_index,
            tab->loading, tab->pending_generation, tab->selection_revision, item.selection.selection_revision,
            tab->view_generation, item.selection.view_generation, s.last_input_tick, item.input_tick);
        fflush(report);
    };
    auto first = (fixture / L"open" / L"first.txt").wstring();
    tab->ClearSelection();
    const size_t active = s.window_tabs.active;
    auto id = send(first, false);
    check(tab->SelectedCount() == 0, "picker result delivery does not select before write");
    write(first);
    check(finished(id) && pump([&] { return selected_name() == L"first.txt"; }, 5000), "observed real write selects file after refresh");
    diagnostics(id);
    check(s.window_tabs.active == active && GetForegroundWindow() == foreground, "write completion preserves active layout and foreground");
    const auto second = (fixture / L"open" / L"second.txt").wstring();
    const auto kept = selected_name();
    id = send(second, false); write(second);
    check(finished(id) && pump([&] { return !tab->pending_generation; }, 5000) && selected_name() == kept,
        "second real write preserves existing selection");
    ui::PaneViewModel vm;
    FillPickerSavedBadges(s, *tab, vm);
    bool badge = false;
    for (const auto& [row, value] : vm.change_badges)
        if (tab->EntryAt(static_cast<size_t>(row)).name == L"second.txt" && value.label == l10n::Get(l10n::StringId::PickerNewBadge)) badge = true;
    check(badge, "preserved selection receives existing change-badge new label");
    tab->ClearSelection();
    OpenTabAt(s, (fixture / L"closed").wstring());
    const size_t background_active = s.window_tabs.active;
    const auto third = (fixture / L"open" / L"background.txt").wstring();
    id = send(third, false); write(third);
    check(finished(id) && pump([&] { return selected_name() == L"background.txt"; }, 5000), "background layout receives saved selection through actual refresh");
    diagnostics(id);
    check(s.window_tabs.active == background_active && GetForegroundWindow() == foreground, "background save never activates its tab or window");
    tab->ClearSelection();
    const auto fourth = (fixture / L"open" / L"intervened.txt").wstring();
    id = send(fourth, false); tab->SelectOnly(0); const auto user_choice = selected_name(); write(fourth);
    check(finished(id) && pump([&] { return !tab->pending_generation; }, 5000) && selected_name() == user_choice,
        "user selection after submission wins over later observed completion");
    // A real background listing larger than the viewport exercises the same
    // production CenterTarget geometry rather than only checking selection.
    std::thread bulk_writer([&] {
        for (int i = 0; i < 96; ++i) {
            wchar_t name[40]{};
            swprintf_s(name, L"tail-%03d.txt", i);
            std::ofstream stream(fixture / L"open" / name);
            stream << "offscreen fixture";
        }
    });
    bulk_writer.join();
    tab->sort_column = ui::SortColumn::Name;
    tab->sort_direction = ui::SortDirection::Asc;
    tab->group_by = 0;
    tab->view_mode = ui::ViewMode::Details;
    RefreshPath(s, folder, RefreshReason::Explicit);
    check(pump([&] { return !tab->pending_generation && tab->EntryCount() >= 101; }, 10000),
        "96 real extra files load into background production listing");
    tab->ClearSelection();
    tab->scroll_y = 0;
    const float scroll_before = tab->scroll_y;
    const size_t active_before_scroll = s.window_tabs.active;
    const auto offscreen = (fixture / L"open" / L"zz-visible.txt").wstring();
    id = send(offscreen, false); write(offscreen);
    check(finished(id) && pump([&] { return !tab->pending_generation && selected_name() == L"zz-visible.txt"; }, 5000),
        "offscreen saved file selected through observer and actual listing refresh");
    diagnostics(id);
    const auto scroll_target = FindTarget(s, folder, tab);
    bool visible = false;
    if (scroll_target.tab && tab->selected_index >= 0) {
        ui::PaneViewModel measured;
        app::FillPaneViewModel(measured, *scroll_target.pane, &s.places);
        const int row = measured.ViewIndex(tab->selected_index);
        const auto content = s.renderer.ContentRect(static_cast<float>(s.compositor.Width()),
            static_cast<float>(s.compositor.Height()));
        const auto bounds = app::FocusedWindowPaneRect(scroll_target.layout->root.get(), scroll_target.pane, content, s.scale);
        const auto list = s.renderer.PaneListRect(measured, bounds);
        const auto item = s.renderer.ItemRectInPane(measured, bounds, row);
        visible = row >= 0 && item.top >= list.top - 1 && item.bottom <= list.bottom + 1 &&
            item.left >= list.left - 1 && item.right <= list.right + 1;
        fprintf(report, "GEOMETRY row=%d scroll_before=%.2f scroll_after=%.2f item=(%.2f,%.2f,%.2f,%.2f) viewport=(%.2f,%.2f,%.2f,%.2f)\n",
            row, scroll_before, tab->scroll_y, item.left, item.top, item.right, item.bottom,
            list.left, list.top, list.right, list.bottom);
    }
    check(tab->scroll_y > scroll_before && visible,
        "production renderer confirms previously offscreen saved row is fully visible after scroll");
    check(s.window_tabs.active == active_before_scroll && GetForegroundWindow() == foreground,
        "offscreen background save preserves active layout and foreground");
    std::filesystem::create_directories(fixture / L"unopened");
    const auto silent = (fixture / L"unopened" / L"silent.txt").wstring();
    const size_t tabs_before = s.window_tabs.items.size();
    const size_t actions_before = watches.at(&s.core)->actions.size();
    id = send(silent, false); write(silent);
    check(finished(id) && watches.at(&s.core)->actions.size() == actions_before && s.window_tabs.items.size() == tabs_before,
        "no matching tab and notify disabled leaves UI unchanged");
    const auto notified = (fixture / L"unopened" / L"notice.txt").wstring();
    id = send(notified, true); write(notified);
    check(finished(id) && watches.at(&s.core)->actions.size() == actions_before + 1 &&
        GetForegroundWindow() == foreground && s.window_tabs.items.size() == tabs_before,
        "no matching tab and opt-in shows nonactivating notification only");
    HWND toast = FindWindowW(L"PulsePickerSavedToast", nullptr);
    check(toast && IsWindowVisible(toast), "real standalone notification exists while main window stays hidden");
    if (toast) PostMessageW(toast, WM_CLOSE, 0, 0);
    fprintf(report, "LIMIT action buttons are not invoked: Show intentionally reveals window; Copy changes clipboard. Toast transport is tested separately.\n");
    fprintf(report, "RESULT failures=%d\n", failures);
    fclose(report);
    StopPickerSaveWatch(s);
    return failures ? 1 : 0;
}
}
#endif

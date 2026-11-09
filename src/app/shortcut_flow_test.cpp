#include "app_internal.h"
#include "../ui/ui_renderer.h"
#include "app_input.h"
#include "app_hosted_edit.h"
#include "keymap.h"
#include "shortcut_page.h"
#include "shortcut_settings.h"
#include "context_menu.h"
#include "../ui/ui_renderer.h"
#include <filesystem>
#include "../ops/clipboard.h"
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#if defined(PULSE_WITH_SELFTEST) || defined(PULSE_WITH_TEST_HOOKS)
using namespace pulse;
namespace {

#ifdef PULSE_WITH_SELFTEST
std::wstring ClipboardText() {
    std::wstring text;
    for (int attempt = 0; attempt < 10 && !OpenClipboard(nullptr); ++attempt) Sleep(20);
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* chars = static_cast<const wchar_t*>(GlobalLock(data))) {
            text = chars;
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return text;
}
#endif

LPARAM KeyLparam(UINT vk, bool alt) {
    LPARAM lp = 1 | (static_cast<LPARAM>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16);
    if (alt) lp |= LPARAM{1} << 29;
    return lp;
}

}  // namespace

#ifdef PULSE_WITH_SELFTEST
void Render(pulse::AppState&);

// Settings > 快捷键 (page 7) driven like a user: clicks go through the page's
// click handler (and one real hit test), keys through the window procedure.
// The conflict probe is the real one, so the strip shows this machine's state.
// Screenshots land next to the log.
int RunShortcutPageTest(AppState& s, const wchar_t* output) {
    FILE* log = nullptr;
    if (!s.isolatedTest || _wfopen_s(&log, output, L"w") || !log) return 2;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label);
        std::fflush(log);
        failures += !ok;
    };
    const auto base = std::filesystem::path(output).parent_path();
    s.appPrefs.persist = false;
    const auto saved_prefs = s.appPrefs.shortcuts;
    const app::Keymap saved_map = app::ActiveKeymap();
    BYTE old_keys[256]{};
    GetKeyboardState(old_keys);
    auto size_window = [&](int w, int h) {
        SetWindowPos(s.hwnd, nullptr, 0, 0, static_cast<int>(w * s.scale), static_cast<int>(h * s.scale),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    };
    auto shot = [&](const wchar_t* name) {
        Render(s);
        const bool ok = s.compositor.SaveSnapshot((base / name).c_str());
        std::fprintf(log, "[%s] screenshot %ls\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    auto view = [&]() { BuildVm(s, false); return s.settings.Shortcuts().View(); };
    auto press = [&](UINT vk, bool ctrl, bool shift, bool alt) {
        BYTE keys[256]{};
        if (ctrl) keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
        if (shift) keys[VK_SHIFT] = keys[VK_LSHIFT] = 0x80;
        if (alt) keys[VK_MENU] = keys[VK_LMENU] = 0x80;
        SetKeyboardState(keys);
        const UINT msg = alt && !ctrl ? WM_SYSKEYDOWN : WM_KEYDOWN;
        SendMessageW(s.hwnd, msg, vk, KeyLparam(vk, alt));
        BYTE none[256]{};
        SetKeyboardState(none);
    };
    auto type = [&](const wchar_t* text) {
        for (const wchar_t* c = text; *c; ++c) SendMessageW(s.hwnd, WM_CHAR, *c, 1);
    };
    auto click = [&](int index) { ShortcutsAction(s, index); };
    auto row_of = [&](const ui::ShortcutsView& v, app::KeyAction action) -> const ui::ShortcutRowView* {
        for (const auto& g : v.groups)
            for (const auto& r : g.rows)
                if (r.action == static_cast<int>(action)) return &r;
        return nullptr;
    };
    auto key_index = [](app::KeyAction action, int slot) { return ui::kScKeyBase + static_cast<int>(action) * 4 + slot; };

    size_window(1360, 900);
    OpenSettingsTab(s, 7);
    s.settings.SetScroll(0, 0);
    SetFocus(s.hwnd);
    auto v = view();
    for (int i = 0; i < 100 && v && v->probing; ++i) {
        Sleep(50);
        v = view();
    }
    check(v && !v->probing, "page opens and the conflict probe finishes");
    if (!v) { std::fclose(log); return 3; }
    size_t rows = 0;
    for (const auto& g : v->groups) rows += g.rows.size();
    check(v->groups.size() == 7 && rows == static_cast<size_t>(app::kKeyActionCount), "7 groups list every action");
    std::fprintf(log, "machine: %d red, %d amber, ime=%d\n", v->red, v->amber, v->ime_installed ? 1 : 0);
    for (const auto& chip : v->chips) std::fprintf(log, "  chip: %ls\n", chip.text.c_str());
    check(BuildVm(s, false).settings_shortcut_issues == v->red + v->amber, "sidebar badge counts the problems");
    shot(L"sc-wide.png");

    // A real hit test finds 复制路径's first key on screen.
    {
        const auto vm = BuildVm(s, false);
        const auto rect = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
        bool found = false;
        const int want = key_index(app::KeyAction::CopyPath, 0);
        for (float y = 0; y < rect.bottom && !found; y += 8)
            for (float x = rect.right * 0.3f; x < rect.right && !found; x += 8) {
                const auto hit = s.renderer.HitTest(vm, rect, x, y);
                found = hit.region == ui::HitTestResult::SettingsShortcuts && hit.index == want;
            }
        check(found, "hit test reaches the 复制路径 key");
    }

    // Record F8 for 复制路径, save with Enter, undo from the toast.
    click(key_index(app::KeyAction::CopyPath, 0));
    v = view();
    check(v->pop.kind == ui::ShortcutPopView::Record && v->pop.waiting, "clicking a key opens the recorder");
    press(VK_F8, false, false, false);
    v = view();
    check(!v->pop.waiting && v->pop.can_save && v->pop.level == 0, "F8 is free and can be saved");
    shot(L"sc-record.png");
    press(VK_RETURN, false, false, false);
    v = view();
    const auto& copy_path = app::ActiveKeymap().Bindings(app::KeyAction::CopyPath);
    check(v->pop.kind == ui::ShortcutPopView::None && !copy_path.empty() && copy_path[0] == app::KeyChord{0, VK_F8},
          "Enter saves: F8 copies the path everywhere");
    bool in_prefs = false;
    for (const auto& line : s.appPrefs.shortcuts) in_prefs = in_prefs || line.rfind(L"CopyPath=F8", 0) == 0;
    check(in_prefs, "prefs keep CopyPath=F8");
    // Menus, tooltips and the shortcut sheet show the new key.
    check(app::ShownShortcut(L"Ctrl+Shift+C") == L"F8" && app::ShownShortcut(L"Ctrl+C") == L"Ctrl+C" &&
          app::RemapShortcutText(L"(Ctrl+Shift+C) \u00B7 Ctrl+K") == L"(F8) \u00B7 Ctrl+K" &&
          app::CustomizedKeysText({app::KeyAction::CopyPath}) == L"F8" &&
          app::CustomizedKeysText({app::KeyAction::Copy}).empty(),
          "menus and hints show F8 for 复制路径");
    check(!v->toast.empty() && v->toast_undo && v->counts[2] == 1, "toast offers undo; 已修改 counts 1");
    shot(L"sc-toast.png");
    click(ui::kScToastUndo);
    v = view();
    check(app::ActiveKeymap().IsDefault(app::KeyAction::CopyPath) && v->counts[2] == 0, "撤销 restores the default");

    // Ctrl+C for 复制路径: already 复制's key, offered as a replacement.
    click(key_index(app::KeyAction::CopyPath, 0));
    press('C', true, false, false);
    v = view();
    check(v->pop.can_save && v->pop.level == 2 && !v->pop.suggestions.empty(), "Ctrl+C: replace prompt with suggestions");
    shot(L"sc-duplicate.png");
    press(VK_ESCAPE, false, false, false);
    v = view();
    check(v->pop.kind == ui::ShortcutPopView::None && app::ActiveKeymap().IsDefault(app::KeyAction::Copy),
          "Esc cancels; 复制 keeps Ctrl+C");
    // Alt+F4 is reserved.
    click(key_index(app::KeyAction::CopyPath, 0));
    press(VK_F4, false, false, true);
    v = view();
    check(!v->pop.can_save && v->pop.level == 3, "Alt+F4 is blocked");
    press(VK_ESCAPE, false, false, false);

    // Numbered group: 切换第 N 个标签 to Alt.
    click(ui::kScRowBase + static_cast<int>(app::KeyAction::TabNumber));
    v = view();
    check(v->pop.kind == ui::ShortcutPopView::Range && v->pop.digit_labels.size() >= 8, "range row opens the modifier picker");
    click(ui::kScPopModBase + 1);  // + Alt
    v = view();
    shot(L"sc-range.png");
    click(ui::kScPopModBase + 0);  // - Ctrl: Alt alone
    v = view();
    const bool alt_ok = v->pop.can_save;
    click(ui::kScPopSave);
    check(alt_ok && app::ActiveKeymap().RangeMods(app::KeyAction::TabNumber) == app::kKeyAlt, "range saved as Alt+1–9");
    check(app::ShownShortcut(L"Ctrl+3") == L"Alt+3", "menus show Alt+3 for tab 3");
    click(ui::kScToastUndo);
    check(app::ActiveKeymap().IsDefault(app::KeyAction::TabNumber), "range undo");
    click(ui::kScRowBase + static_cast<int>(app::KeyAction::TabNumber));
    click(ui::kScPopModBase + 0);  // no modifier at all
    v = view();
    check(!v->pop.can_save && v->pop.level == 3, "a range without Ctrl/Alt is blocked");
    click(ui::kScPopClose);

    // Problems: the first chip opens its explanation.
    v = view();
    if (!v->chips.empty()) {
        click(ui::kScChipBase);
        v = view();
        check(v->pop.kind == ui::ShortcutPopView::Info && !v->pop.verdict.empty(), "a conflict chip opens its explanation");
        shot(L"sc-info.png");
        click(ui::kScBackdrop);
        click(ui::kScFilterBase + 1);
        v = view();
        size_t shown = 0;
        for (const auto& g : v->groups) shown += g.rows.size();
        check(shown == static_cast<size_t>(v->counts[1]), "有问题 shows only rows with problems");
        shot(L"sc-issues.png");
        click(ui::kScFilterBase + 0);
    }

    // Search box: typed text filters actions and keys.
    click(ui::kScSearch);
    type(L"tab");
    v = view();
    size_t shown = 0;
    for (const auto& g : v->groups) shown += g.rows.size();
    check(v->search_focused && v->query == L"tab" && shown > 0 && shown < rows, "typing filters the list");
    press(VK_BACK, false, false, false);
    v = view();
    check(v->query == L"ta", "Backspace edits the search");
    shot(L"sc-search.png");
    press(VK_ESCAPE, false, false, false);
    press(VK_ESCAPE, false, false, false);
    v = view();
    check(v->query.empty() && !v->search_focused, "Esc clears, then leaves the box");

    // 按键查找: Ctrl+C finds 复制.
    click(ui::kScFind);
    press('C', true, false, false);
    v = view();
    const auto* copy_row = row_of(*v, app::KeyAction::Copy);
    shown = 0;
    for (const auto& g : v->groups) shown += g.rows.size();
    check(copy_row && copy_row->found && shown == 1, "按键查找 Ctrl+C shows only 复制");
    click(ui::kScSearchClear);

    // 按键测试.
    click(ui::kScTester);
    press('C', true, true, false);
    v = view();
    check(v->pop.kind == ui::ShortcutPopView::Tester && v->pop.verdict.find(app::ActionName(app::KeyAction::CopyPath)) != std::wstring::npos,
          "按键测试: Ctrl+Shift+C → 复制路径");
    shot(L"sc-tester.png");
    press(VK_ESCAPE, false, false, false);

    // ⋯ menu, dark theme and a narrow window.
    click(ui::kScMore);
    shot(L"sc-more.png");
    click(ui::kScBackdrop);
    const bool was_dark = s.darkMode;
    s.darkMode = true;
    click(key_index(app::KeyAction::CopyToTarget, 0));
    shot(L"sc-dark.png");
    click(ui::kScBackdrop);
    s.darkMode = was_dark;
    size_window(980, 820);
    shot(L"sc-narrow.png");

    // Settings has no tab of its own: the title-bar button stands in for it.
    size_window(1360, 900);
    {
        const size_t n = s.window_tabs.items.size();
        const ui::WindowViewModel open_vm = BuildVm(s, false);
        check(open_vm.settings_open && s.window_tabs.active < open_vm.tabs.size() &&
              open_vm.tabs[s.window_tabs.active].hidden, "open settings is not drawn as a tab");
        shot(L"sc-titlebar.png");
        CloseSettings(s);
        const ui::WindowViewModel back_vm = BuildVm(s, false);
        bool any_hidden = false;
        for (const auto& t : back_vm.tabs) any_hidden = any_hidden || t.hidden;
        check(n >= 2 && !back_vm.settings_open && !any_hidden && s.window_tabs.items.size() == n - 1,
              "the settings button closes settings and its tab");
        shot(L"sc-closed.png");
        OpenSettingsTab(s, s.settings.page());
        check(IsSettingsTab(ActiveTab(s)) && s.settings.page() == 7 && s.window_tabs.items.size() == n,
              "the settings button reopens on 快捷键");
        NewTab(s, L"C:\\");
        check(s.window_tabs.items.size() == n && !IsSettingsTab(ActiveTab(s)), "a new tab replaces settings");
        OpenSettingsTab(s, 7);
        press(VK_ESCAPE, false, false, false);
        check(!IsSettingsTab(ActiveTab(s)) && s.window_tabs.items.size() == n, "Esc closes settings");
        OpenSettingsTab(s, 7);
        click(ui::kScTester);
        press(VK_ESCAPE, false, false, false);
        check(IsSettingsTab(ActiveTab(s)) && view()->pop.kind == ui::ShortcutPopView::None,
              "Esc closes the open popover first, settings stays");
    }

    s.appPrefs.shortcuts = saved_prefs;
    ApplyKeymapPrefs(s);
    (void)saved_map;
    SetKeyboardState(old_keys);
    std::fprintf(log, "failures=%d\n", failures);
    std::fclose(log);
    return failures ? 1 : 0;
}

#endif

int RunLayoutShortcutTest(AppState& s, const wchar_t* output) {
    FILE* log = nullptr;
    if (!s.isolatedTest || _wfopen_s(&log, output, L"w") || !log) return 2;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label);
        std::fflush(log);
        failures += !ok;
    };
    s.appPrefs.persist = false;
    const auto saved_map = app::ActiveKeymap();
    BYTE saved_keys[256]{};
    GetKeyboardState(saved_keys);
    auto press = [&](HWND target, UINT digit, bool shift) {
        BYTE keys[256]{};
        keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
        keys[VK_MENU] = keys[VK_LMENU] = 0x80;
        if (shift) keys[VK_SHIFT] = keys[VK_LSHIFT] = 0x80;
        SetKeyboardState(keys);
        SendMessageW(target, WM_KEYDOWN, digit, KeyLparam(digit, true));
        BYTE none[256]{};
        SetKeyboardState(none);
    };
    auto is_layout = [&](app::LayoutPreset expected) {
        const auto* layout = s.window_tabs.Active();
        return layout && layout->layout == expected &&
            layout->VisiblePanes().size() == app::LayoutPresetCount(expected);
    };
    const app::LayoutPreset presets[] = {app::LayoutPreset::Single, app::LayoutPreset::TwoVertical,
        app::LayoutPreset::Three, app::LayoutPreset::FourGrid};
    for (bool custom : {false, true}) {
        app::SetActiveKeymap(custom ? app::Keymap::FromPrefs({L"Layout=Ctrl+Alt+Shift"}) : app::Keymap{});
        SetFocus(s.hwnd);
        for (int i : {1, 2, 3, 0}) {
            press(s.hwnd, static_cast<UINT>('1' + i), custom);
            const auto label = std::string(custom ? "custom layout digit " : "default layout digit ") +
                std::to_string(i + 1) + " applies expected preset and pane count";
            check(is_layout(presets[i]), label.c_str());
        }
        if (custom) {
            press(s.hwnd, '2', false);
            check(is_layout(app::LayoutPreset::Single), "old layout binding stays inactive after remapping");
        }
        ShowFilterEditor(s);
        check(s.filterEditing && s.hwndFilterEdit, "filter editor opens for focus boundary");
        if (s.hwndFilterEdit) {
            press(s.hwndFilterEdit, '2', custom);
            check(is_layout(app::LayoutPreset::Single), "layout chord does not split while editing filter text");
        }
        if (s.filterEditing) HideFilterEditor(s, false);
    }
    DispatchMenuCommand(s, app::CmdLayoutTwoHorizontal);
    check(is_layout(app::LayoutPreset::TwoHorizontal), "stacked panes menu command still applies two horizontal panes");
    app::SetActiveKeymap(saved_map);
    SetKeyboardState(saved_keys);
    std::fprintf(log, "failures=%d\n", failures);
    std::fclose(log);
    return failures ? 1 : 0;
}

#ifdef PULSE_WITH_SELFTEST
// The shortcuts users reported (复制路径 Ctrl+Shift+C, 窗口总览 Ctrl+Shift+O)
// and their neighbours, sent as real WM_KEYDOWN messages through the window
// procedure and the hosted edit procedures. The user's clipboard text is put
// back at the end.
int RunShortcutFlowTest(AppState& s, const wchar_t* output) {
    if (GetEnvironmentVariableW(L"PULSE_TEST_LAYOUT_SHORTCUTS", nullptr, 0)) return RunLayoutShortcutTest(s, output);
    if (GetEnvironmentVariableW(L"PULSE_TEST_SHORTCUT_PAGE", nullptr, 0)) return RunShortcutPageTest(s, output);
    FILE* log = nullptr;
    if (!s.isolatedTest || _wfopen_s(&log, output, L"w") || !log) return 2;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label);
        std::fflush(log);
        failures += !ok;
    };
    s.appPrefs.persist = false;
    auto* tab = ActiveTab(s);
    if (!tab) { std::fclose(log); return 3; }
    const std::wstring saved_clipboard = ClipboardText();
    BYTE old_keys[256]{};
    GetKeyboardState(old_keys);
    auto press = [&](HWND target, UINT vk, bool ctrl, bool shift, bool alt) {
        BYTE keys[256]{};
        if (ctrl) keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
        if (shift) keys[VK_SHIFT] = keys[VK_LSHIFT] = 0x80;
        if (alt) keys[VK_MENU] = keys[VK_LMENU] = 0x80;
        SetKeyboardState(keys);
        const UINT msg = alt && !ctrl ? WM_SYSKEYDOWN : WM_KEYDOWN;
        SendMessageW(target, msg, vk, KeyLparam(vk, alt));
        BYTE none[256]{};
        SetKeyboardState(none);
    };

    tab->current_path = L"C:\\PulseShortcutFixture";
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(3);
    for (int i = 0; i < 3; ++i) {
        (*entries)[i].name = i == 0 ? L"a.txt" : i == 1 ? L"b.txt" : L"c.txt";
        (*entries)[i].attrs = FILE_ATTRIBUTE_NORMAL;
        (*entries)[i].size = static_cast<uint64_t>(i + 1);
    }
    tab->SetSnapshot(entries);
    tab->loading = false;
    tab->view_mode = ui::ViewMode::Details;
    tab->filter_text.clear();
    tab->group_by = 0;
    BuildVm(s);  // the rows a user sees before pressing a key
    SetFocus(s.hwnd);

    // 复制路径
    tab->SelectOnly(1);
    ops::WriteClipboardText(L"-");
    press(s.hwnd, 'C', true, true, false);
    const std::wstring copied = ClipboardText();
    check(copied == ClipboardPath(tab->current_path + L"\\b.txt"),
        "Ctrl+Shift+C copies the selected item's path");
    std::fprintf(log, "  clipboard: %ls\n", copied.c_str());
    tab->ClearSelection();
    ops::WriteClipboardText(L"-");
    press(s.hwnd, 'C', true, true, false);
    check(ClipboardText() == ClipboardPath(tab->current_path),
        "Ctrl+Shift+C with nothing selected copies the folder path (as the background menu)");

    // 窗口总览
    press(s.hwnd, 'O', true, true, false);
    check(s.overview.open, "Ctrl+Shift+O opens the window overview");
    press(s.hwnd, VK_ESCAPE, false, false, false);
    check(!s.overview.open, "Esc closes the window overview");
    press(s.hwnd, 'F', true, false, false);
    check(s.filterEditing && s.hwndFilterEdit, "Ctrl+F opens the filter box");
    if (s.hwndFilterEdit) {
        SetFocus(s.hwndFilterEdit);
        press(s.hwndFilterEdit, 'O', true, true, false);
        check(s.overview.open, "Ctrl+Shift+O from the filter box opens the window overview");
        check(GetFocus() == s.hwnd, "the overview takes the keyboard from the filter box");
        press(s.hwnd, VK_ESCAPE, false, false, false);
        check(!s.overview.open, "Esc closes the overview opened from the filter box");
    }
    if (s.filterEditing) HideFilterEditor(s, false);
    SetFocus(s.hwnd);

    // Neighbours in the shortcut table.
    tab = ActiveTab(s);
    press(s.hwnd, 'A', true, false, false);
    check(tab && tab->SelectedCount() == 3, "Ctrl+A selects all");
    if (tab) tab->SelectOnly(0);
    press(s.hwnd, 'I', true, false, false);
    check(tab && tab->SelectedCount() == 2 && !tab->IsSelected(0), "Ctrl+I inverts the selection");
    const bool collapsed = s.appPrefs.sidebar_collapsed;
    press(s.hwnd, 'B', true, false, false);
    check(s.appPrefs.sidebar_collapsed != collapsed, "Ctrl+B folds the sidebar");
    press(s.hwnd, 'B', true, false, false);
    check(s.appPrefs.sidebar_collapsed == collapsed, "Ctrl+B unfolds the sidebar");
    const size_t tabs = s.window_tabs.items.size();
    press(s.hwnd, 'T', true, false, false);
    check(s.window_tabs.items.size() == tabs + 1, "Ctrl+T opens a tab");
    press(s.hwnd, 'W', true, false, false);
    check(s.window_tabs.items.size() == tabs, "Ctrl+W closes it");

    // Settings > 快捷键: keys the user moved, through the same window procedure.
    {
        const app::Keymap saved_map = app::ActiveKeymap();
        app::Keymap map;
        map.SetBindings(app::KeyAction::CopyPath, {app::KeyChord{0, VK_F8}});
        map.SetBindings(app::KeyAction::Overview, {app::KeyChord{app::kKeyAlt, 'O'}});
        map.SetRangeMods(app::KeyAction::TabNumber, app::kKeyAlt);
        app::SetActiveKeymap(map);
        tab = ActiveTab(s);
        if (tab) {
            tab->SelectOnly(2);
            ops::WriteClipboardText(L"-");
            press(s.hwnd, VK_F8, false, false, false);
            check(ClipboardText() == ClipboardPath(tab->current_path + L"\\c.txt"),
                "remapped: F8 copies the path (复制路径 moved to F8)");
            ops::WriteClipboardText(L"-");
            press(s.hwnd, 'C', true, true, false);
            check(ClipboardText() == L"-", "remapped: Ctrl+Shift+C no longer copies the path");
        }
        press(s.hwnd, 'O', false, false, true);
        check(s.overview.open, "remapped: Alt+O opens the window overview");
        press(s.hwnd, 'O', false, false, true);
        check(!s.overview.open, "remapped: Alt+O closes it again");
        press(s.hwnd, 'O', true, true, false);
        check(!s.overview.open, "remapped: Ctrl+Shift+O is free");
        if (s.overview.open) press(s.hwnd, VK_ESCAPE, false, false, false);
        const size_t before = s.window_tabs.items.size();
        press(s.hwnd, 'T', true, false, false);
        const size_t opened = s.window_tabs.active;
        press(s.hwnd, '1', false, false, true);
        check(opened != 0 && s.window_tabs.active == 0, "remapped: Alt+1 goes to the first tab");
        press(s.hwnd, '2', true, false, false);
        check(s.window_tabs.active == 0, "remapped: Ctrl+2 no longer switches tabs");
        press(s.hwnd, static_cast<UINT>('1' + opened), false, false, true);
        check(s.window_tabs.active == opened, "remapped: Alt+N reaches the new tab");
        press(s.hwnd, 'W', true, false, false);
        check(s.window_tabs.items.size() == before, "Ctrl+W still closes it");
        app::SetActiveKeymap(saved_map);
    }

    SetKeyboardState(old_keys);
    ops::WriteClipboardText(saved_clipboard);
    std::fprintf(log, "failures=%d\n", failures);
    std::fclose(log);
    return failures ? 1 : 0;
}
#endif
#endif

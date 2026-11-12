#include "app_internal.h"
#include "app_navigation.h"
#include "app_runtime.h"
#include "app_commands.h"
#include "extensions/native/command_contributions.h"
#include "extensions/native/session.h"
#include "extensions/native/settings_pages_adapter.h"
#include "extensions/runtime.h"
#include "extension_store_ui.h"
#include "extension_store_seven_zip.h"
#include "extension_store_edits.h"
#include "extension_store_actions.h"
#include "../common/json_value.h"
#include "../ui/ui_renderer.h"
#include <commctrl.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <algorithm>

int RunExtensionStoreOperationsTest(pulse::AppState&, const wchar_t*);
int RunExtensionStoreSwitchesTest(pulse::AppState&, const wchar_t*);

namespace {
void PumpStoreFlow() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); break; }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
struct FindStoreEdit {
    std::wstring cue;
    HWND result = nullptr;
};
BOOL CALLBACK MatchStoreEdit(HWND hwnd, LPARAM parameter) {
    auto& find = *reinterpret_cast<FindStoreEdit*>(parameter);
    wchar_t name[32]{}, cue[256]{};
    GetClassNameW(hwnd, name, ARRAYSIZE(name));
    if (_wcsicmp(name, L"Edit") || !(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VISIBLE)) return TRUE;
    SendMessageW(hwnd, EM_GETCUEBANNER, reinterpret_cast<WPARAM>(cue), ARRAYSIZE(cue));
    if (find.cue == cue) { find.result = hwnd; return FALSE; }
    return TRUE;
}
std::wstring MenuSignature(const std::vector<pulse::ui::SevenZipMenuRow>& rows) {
    std::wstring value;
    for (const auto& row : rows) value += row.title + L"|" + row.detail + L"\n";
    return value;
}
}

int RunExtensionStoreFlowTest(pulse::AppState& s, const wchar_t* output_dir) {
    using namespace pulse;
    using A = ui::PackAction;
    using I = l10n::StringId;
    if (!s.isolatedTest || !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0) || !output_dir || !*output_dir) return 2;
    const std::filesystem::path output(output_dir);
    std::error_code error;
    std::filesystem::create_directories(output, error);
    if (error) return 2;
    std::ofstream log(output / L"store-flow.log");
    if (!log) return 2;
    int failures = 0;
    auto check = [&](bool passed, const char* label) {
        log << (passed ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!passed) ++failures;
    };
    auto read = [&] {
        ui::WindowViewModel vm;
        FillExtensionStoreView(s, vm);
        return vm.settings_store_view;
    };
    auto wait = [&](const std::function<bool(const ui::ExtensionStoreView&)>& predicate, DWORD timeout = 3000) {
        const ULONGLONG start = GetTickCount64();
        do {
            PumpStoreFlow();
            TickExtensionStoreUi(s);
            if (predicate(read())) return true;
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } while (GetTickCount64() - start < timeout);
        return false;
    };
    auto action = [&](A base, int offset = 0) {
        return ExtensionStoreAction(s, static_cast<int>(base) + offset);
    };
    OpenSettingsTab(s, 8);
    action(A::StoreBrowse);
    const auto ready_started = GetTickCount64();
    const bool ready = wait([](const auto& view) {
        return view.seven_zip.available && view.seven_zip.item >= 0 && view.seven_zip.installed_index >= 0;
    }, 30000);
    check(ready, "installed registry and real 7-Zip store settings become ready within 30 seconds");
    log << "store_and_plugin_ready_ms=" << GetTickCount64() - ready_started << std::endl;
    if (!ready) {
        const auto view = read();
        log << "packages=" << extensions::PackageCount() << " items=" << view.items.size()
            << " installed=" << view.installed.size() << " installed_error=" << json::Utf8(view.installed_error) << std::endl;
        for (size_t i = 0; i < extensions::PackageCount(); ++i) {
            log << "package=" << i << " native_status=" << extensions::native::PackageLastStatus(s.hwnd, i);
            if (auto* page = extensions::native::PackageSettings(s.hwnd, i))
                log << " ready=" << page->State("ready").value_or("missing") << " status=" << page->State("status").value_or("missing");
            log << std::endl;
        }
        log << "failures=" << failures << std::endl; return 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_STORE_LEGACY", nullptr, 0)) {
        const auto legacy = read().seven_zip;
        check(legacy.available && !legacy.menu_available && !legacy.formats_available,
            "real old plugin is detected by missing menu and format state capabilities");
        check(!SetSevenZipState(s, "cmd0", "false"), "old plugin rejects missing command state rather than saving a false success");
        for (int tab : {1, 2}) {
            s.store_detail = true; s.store_detail_tab = tab;
            auto vm = BuildVm(s, false); vm.settings_open = true; vm.settings_page = 8;
            const auto theme = ui::MakeTheme(s.darkMode, s.accentColor);
            auto* dc = s.compositor.Dc(); dc->BeginDraw(); dc->Clear(theme.bg);
            s.renderer.Render(vm, D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height())), theme);
            check(SUCCEEDED(dc->EndDraw()) && s.compositor.SaveSnapshot(
                (output / (tab == 1 ? L"legacy-menu.png" : L"legacy-formats.png")).c_str()),
                "render explicit extension-update message instead of fake switches or empty formats");
        }
        log << "failures=" << failures << std::endl;
        return failures ? 1 : 0;
    }
    check(read().seven_zip.menu_available && read().seven_zip.formats_available &&
        !read().seven_zip.formats_rw.empty() && !read().seven_zip.formats_ro.empty(),
        "updated real plugin supplies six switches and both nonempty format groups");
    {
        const auto view = read();
        const auto& installed = view.installed[view.seven_zip.installed_index];
        if (installed.version.empty()) check(!view.items[view.seven_zip.item].action_enabled,
            "unlisted upgraded plugin is not offered an older catalog package as an update");
    }
    const auto original = read().seven_zip;
    const auto original_search = s.store_search;
    const int original_category = s.store_category;
    const bool original_detail = s.store_detail;
    const int original_tab = s.store_detail_tab;
    const int original_preview = s.store_preview_kind;
    auto expect = [&](A base, int offset, const std::function<bool(const ui::SevenZipView&)>& predicate, const char* label) {
        const bool handled = action(base, offset);
        // A real mouse press can be down while this isolated probe runs. Finish
        // the same release path rather than leaving a slider preview uncommitted.
        if (base == A::SzLevelBase && ExtensionStoreLevelDragging(s)) {
            log << "level_preview=" << s.store_level_drag << std::endl;
            ExtensionStoreLevelDragEnd(s);
            if (GetCapture() == s.hwnd) ReleaseCapture();
        }
        check(handled && wait([&](const auto& view) { return predicate(view.seven_zip); }), label);
    };
    // All mutations go through production action dispatch and the plugin-owned state bridge.
    for (int mode = 0; mode < 3; ++mode)
        expect(A::SzModeBase, mode, [=](const auto& z) { return z.mode == mode; }, "mode action reaches plugin state");
    const auto here = MenuSignature(read().seven_zip.archive_rows);
    expect(A::SzModeBase, 1, [](const auto& z) { return z.mode == 1; }, "folder extraction mode selected");
    check(wait([&](const auto& view) { return MenuSignature(view.seven_zip.archive_rows) != here; }), "extract menu title or detail changes with mode");
    for (int location = 0; location < 2; ++location)
        expect(A::SzLocationBase, location, [=](const auto& z) { return z.location == location; }, "location action reaches plugin state");
    for (int format = 0; format < 6; ++format)
        expect(A::SzFormatBase, format, [=](const auto& z) { return z.format == format; }, "format action reaches plugin state");
    expect(A::SzFormatBase, 0, [](const auto& z) { return z.format == 0; }, "compressed format enables level selection");
    for (int level = 0; level < 6; ++level)
        expect(A::SzLevelBase, level, [=](const auto& z) { return z.level == level; }, "level action reaches plugin state");
    expect(A::SzFormatBase, 2, [](const auto& z) { return z.format == 2; }, "tar format selected");
    expect(A::SzLevelBase, 0, [](const auto& z) { return z.level == 5; }, "tar ignores disabled level action");
    expect(A::SzFormatBase, 1, [](const auto& z) { return z.format == 1; }, "zip format selected");
    check(wait([](const auto& view) { return MenuSignature(view.seven_zip.file_rows).find(L"zip") != std::wstring::npos; }), "file menu reflects zip format");
    const bool remember = read().seven_zip.remember;
    expect(A::SzRemember, 0, [=](const auto& z) { return z.remember != remember; }, "detail remember switch toggles plugin state");
    expect(A::StoreQuickRemember, 0, [=](const auto& z) { return z.remember == remember; }, "quick remember switch uses same plugin state");
    for (int placement = 0; placement < 2; ++placement)
        expect(A::SzPlacementBase, placement, [=](const auto& z) { return z.submenu == (placement != 0); }, "menu placement action reaches plugin state");
    for (int command = 0; command < 6; ++command) {
        const auto before = read().seven_zip;
        const auto wanted = before.commands ^ (1u << command);
        expect(A::SzCommandBase, command, [=](const auto& z) { return z.commands == wanted; }, "command toggle updates actual mask");
        check(wait([&](const auto& view) {
            return MenuSignature(view.seven_zip.archive_rows) != MenuSignature(before.archive_rows) ||
                MenuSignature(view.seven_zip.file_rows) != MenuSignature(before.file_rows);
        }), "command toggle changes live context-menu preview");
        expect(A::SzCommandBase, command, [=](const auto& z) { return z.commands == before.commands; }, "command toggle restores original command mask");
    }
    for (int tab = 0; tab < 4; ++tab) {
        action(A::StoreDetailTabBase, tab);
        check(read().detail_tab == tab, "detail tab action updates view");
    }
    for (int category = 0; category < 5; ++category) {
        action(A::StoreChipBase, category);
        check(read().category == category, "category action updates view");
    }
    for (int preview = 0; preview < 2; ++preview) {
        action(A::SzPreviewBase, preview);
        check(read().preview_kind == preview, "preview kind action updates view");
    }
    s.store_detail = true;
    action(A::StoreDetailBack);
    check(!s.store_detail && !read().detail, "detail back returns to store");
    auto render = [&](int field) {
        auto vm = BuildVm(s, false);
        vm.settings_open = true; vm.settings_page = 8;
        const auto rect = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
        auto draw = [&] {
            auto* dc = s.compositor.Dc(); dc->BeginDraw();
            const auto theme = ui::MakeTheme(s.darkMode, s.accentColor);
            dc->Clear(theme.bg);
            s.renderer.Render(vm, rect, theme);
            return SUCCEEDED(dc->EndDraw());
        };
        bool ok = draw();
        const auto bounds = s.renderer.SettingsStoreField(field);
        const auto clip = s.renderer.SettingsStoreClip();
        if (field >= 0 && (bounds.top < clip.top || bounds.bottom > clip.bottom)) {
            vm.settings_scroll += bounds.top - clip.top - 24 * s.scale;
            ok = draw() && ok;
        }
        SyncStoreEdits(s, vm);
        return ok;
    };
    auto edit = [&](I cue) {
        FindStoreEdit find{l10n::Get(cue)};
        EnumChildWindows(s.hwnd, MatchStoreEdit, reinterpret_cast<LPARAM>(&find));
        return find.result;
    };
    check(render(0), "store frame rendered for native search edit");
    check(s.compositor.SaveSnapshot((output / L"store-live.png").c_str()), "save actual plugin store frame");
    const HWND search = edit(I::StoreSearchHint);
    check(search != nullptr, "native search EDIT located by real cue banner");
    if (search) {
        SendMessageW(search, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"7-Zip"));
        check(s.store_search == L"7-Zip" && read().search == L"7-Zip", "search EDIT routes text to store view");
        SendMessageW(search, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(original_search.c_str()));
    }
    const bool has_catalog = read().seven_zip.item >= 0;
    check(has_catalog, "real catalog contains installed 7-Zip detail item");
    if (has_catalog) {
        s.store_detail = true; s.store_detail_tab = 0;
        check(render(1), "detail frame rendered for native folder edit");
        check(s.compositor.SaveSnapshot((output / L"detail-live.png").c_str()), "save actual plugin detail frame");
        expect(A::SzLevelBase, 0, [](const auto& z) { return z.level == 0; }, "slider drag starts from committed level");
        const auto track = s.renderer.SettingsStoreField(6);
        s.store_level_drag = 0.0f;
        ExtensionStoreLevelDrag(s, static_cast<int>(std::lround(ui::fluent::StepSliderStopX(track, 6, 5, s.scale))));
        check(ExtensionStoreLevelDragging(s) && std::lround(s.store_level_drag) == 5 && read().seven_zip.level == 0,
            "shared slider drag previews the endpoint without saving intermediate values");
        check(ExtensionStoreLevelDragEnd(s) && wait([](const auto& v) { return v.seven_zip.level == 5; }),
            "slider release commits the chosen level through plugin state");
        const HWND folder = edit(I::SzFolderHint);
        check(folder != nullptr, "native fixed-folder EDIT located by real cue banner");
        if (folder) {
            const auto path = std::filesystem::absolute(output).wstring();
            SendMessageW(folder, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(path.c_str()));
            SendMessageW(folder, WM_KEYDOWN, VK_RETURN, 0);
            check(wait([&](const auto& view) { return view.seven_zip.folder == path; }), "folder EDIT Enter commits through plugin validation");
            DestroyStoreEdits(s);
            check(render(1), "hosted controls rebuilt without resetting plugin values");
            check(read().seven_zip.folder == path, "folder survives native EDIT recreation");
        }
    }
    if (has_catalog) {
        for (int tab : {2, 3}) {
            action(A::StoreDetailTabBase, tab);
            check(read().detail_tab == tab && render(-1), "render actual plugin formats/about tab");
            check(s.compositor.SaveSnapshot((output / (tab == 2 ? L"formats-live.png" : L"about-live.png")).c_str()),
                "save actual plugin formats/about frame");
        }
    }
    // Exercise the actual contribution adapter against a real selected archive,
    // not the store's sample-selection preview or a fabricated CommandInfo list.
    const auto fixture = output / L"menu-fixture.zip";
    {
        constexpr unsigned char empty_zip[22] = {0x50, 0x4b, 0x05, 0x06};
        std::ofstream zip(fixture, std::ios::binary);
        zip.write(reinterpret_cast<const char*>(empty_zip), sizeof(empty_zip));
        check(static_cast<bool>(zip), "write isolated empty ZIP for actual selection context");
    }
    auto select_fixture = [&] {
        NavigateTo(s, std::filesystem::absolute(output).wstring());
        return wait([&](const auto&) {
            auto* tab = ActiveTab(s);
            std::error_code path_error;
            if (!tab || tab->loading || !std::filesystem::equivalent(
                std::filesystem::path(tab->current_path), std::filesystem::absolute(output), path_error)) return false;
            for (size_t i = 0; i < tab->EntryCount(); ++i) {
                if (tab->EntryAt(i).name == fixture.filename().wstring()) {
                    tab->SelectOnly(static_cast<int>(i));
                    return tab->SelectedCount() == 1;
                }
            }
            return false;
        }, 10000);
    };
    const bool selected_archive = select_fixture();
    check(selected_archive, "real directory enumeration selects menu-fixture.zip");
    for (int command = 0; command < 6; ++command)
        SetSevenZipState(s, "cmd" + std::to_string(command), "true");
    expect(A::SzPlacementBase, 0, [](const auto& z) { return !z.submenu && z.commands == 0x3f; },
        "real plugin registers top-level context commands");
    std::vector<ui::FluentMenuItem> contributed;
    const bool contributed_ready = selected_archive && wait([&](const auto&) {
        contributed = extensions::native::ContextMenuCommandRows(s);
        return std::any_of(contributed.begin(), contributed.end(), [](const auto& row) {
            return row.heading && row.group_tint && row.trailing_command && row.trailing_vector_icon == ui::StoreIcon::Gear;
        });
    });
    check(contributed_ready, "actual contribution rows contain tinted group header and settings gear");
    int gear_command = 0;
    if (contributed_ready) {
        bool primary_detail = false, icons = true, tinted_end = false;
        for (const auto& row : contributed) {
            if (row.heading && row.group_tint) gear_command = row.trailing_command;
            if (row.group_tint && !row.heading) {
                icons = icons && row.vector_icon != ui::StoreIcon::None && row.glyph.empty();
                primary_detail = primary_detail || (row.primary && !row.detail.empty());
                tinted_end = tinted_end || row.group_end;
            }
        }
        check(primary_detail, "actual primary extraction command retains its result detail");
        check(icons && tinted_end, "actual contributed commands use vectors and close their tinted group");
        const bool menu_ready = EnsureMenu(s);
        check(menu_ready, "reuse production FluentMenu for real contribution snapshots");
        auto full_menu = BuildFinderItemMenu(s, false, L"");
        AppendNativeCommandRows(s, full_menu);
        const auto group_at = std::find_if(full_menu.begin(), full_menu.end(), [](const auto& row) { return row.group_start; });
        const auto copy_at = std::find_if(full_menu.begin(), full_menu.end(), [](const auto& row) { return row.command == app::CmdCopyPath; });
        check(group_at != full_menu.end() && (copy_at == full_menu.end() || group_at < copy_at),
            "production item menu places archive group before copy-path utilities");
        if (menu_ready) for (bool dark : {false, true}) {
            s.menu->SetTheme(dark, s.accentColor);
            const auto name = dark ? L"context-contributions-dark.png" : L"context-contributions-light.png";
            check(s.menu->SaveDebugSnapshot((output / name).c_str(), contributed),
                "snapshot actual top-level plugin contribution rows");
            check(s.menu->SaveDebugSnapshot((output / (dark ? L"context-full-dark.png" : L"context-full-light.png")).c_str(), full_menu),
                "snapshot production file menu with actual plugin commands");
        }
        s.store_detail = false;
        check(gear_command && extensions::native::HandleContributedCommand(s, gear_command) &&
            s.settings.page() == 8 && s.store_detail && s.store_detail_tab == 0,
            "actual header gear dispatch opens store page 8 detail");
    }
    const bool reselected_archive = select_fixture();
    check(reselected_archive, "restore real archive selection after settings gear navigation");
    expect(A::SzPlacementBase, 1, [](const auto& z) { return z.submenu; },
        "real plugin re-registers commands as submenu");
    std::vector<ui::FluentMenuItem> submenu_rows;
    const bool submenu_ready = reselected_archive && wait([&](const auto&) {
        contributed = extensions::native::ContextMenuCommandRows(s);
        for (const auto& row : contributed) if (!row.children.empty() && row.group_tint) {
            for (const auto& child : row.children) if (child.vector_icon == ui::StoreIcon::Gear && child.command == gear_command) {
                submenu_rows = row.children;
                return true;
            }
        }
        return false;
    });
    check(submenu_ready, "actual submenu contains the same settings command entry");
    if (submenu_ready && EnsureMenu(s)) {
        for (bool dark : {false, true}) {
            s.menu->SetTheme(dark, s.accentColor);
            const auto parent_name = dark ? L"context-submenu-parent-dark.png" : L"context-submenu-parent-light.png";
            const auto child_name = dark ? L"context-submenu-children-dark.png" : L"context-submenu-children-light.png";
            check(s.menu->SaveDebugSnapshot((output / parent_name).c_str(), contributed), "snapshot actual plugin submenu parent");
            check(s.menu->SaveDebugSnapshot((output / child_name).c_str(), submenu_rows), "snapshot actual plugin submenu children and settings");
        }
    }
    if (s.menu) s.menu->SetTheme(s.darkMode, s.accentColor);
    check(RunExtensionStoreOperationsTest(s, output_dir) == 0,
        "real menu clicks complete archive creation, testing, extraction and checksum verification");
    check(RunExtensionStoreSwitchesTest(s, output_dir) == 0,
        "actual mouse clicks change both placement cards and all six command switches");
    // Restore all plugin-owned values, including the mask, even if any assertion failed.
    SetSevenZipState(s, "mode", std::to_string(original.mode));
    SetSevenZipState(s, "location", std::to_string(original.location));
    SetSevenZipState(s, "format", std::to_string(original.format));
    SetSevenZipState(s, "level", std::to_string(original.level));
    SetSevenZipState(s, "remember", original.remember ? "true" : "false");
    SetSevenZipState(s, original.submenu ? "placement1" : "placement0", "true");
    SetSevenZipState(s, "folder", json::Utf8(original.folder));
    for (int command = 0; command < 6; ++command)
        SetSevenZipState(s, "cmd" + std::to_string(command), original.commands & (1u << command) ? "true" : "false");
    check(wait([&](const auto& view) {
        const auto& z = view.seven_zip;
        return z.mode == original.mode && z.location == original.location && z.format == original.format &&
            z.level == original.level && z.remember == original.remember && z.submenu == original.submenu &&
            z.commands == original.commands && z.folder == original.folder;
    }), "all original plugin settings restored");
    // Let the existing asynchronous settings resource writer drain; no private storage format is assumed.
    const auto settle = GetTickCount64();
    while (GetTickCount64() - settle < 500) { PumpStoreFlow(); MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE); }
    log << "plugin_status=" << json::Utf8(read().seven_zip.status) << '\n';
    log << "NOTE: native control recreation is covered; process-restart persistence is not asserted by this in-process test.\n";
    s.store_search = original_search; s.store_category = original_category; s.store_detail = original_detail;
    s.store_detail_tab = original_tab; s.store_preview_kind = original_preview;
    DestroyStoreEdits(s);
    log << "failures=" << failures << std::endl;
    return failures ? 1 : 0;
}

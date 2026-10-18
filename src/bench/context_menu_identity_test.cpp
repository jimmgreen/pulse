#include "../app/context_menu_prefs.h"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <cstdio>

namespace { std::wstring fixture; }
namespace pulse::app { std::wstring GetPulseDataDir() { return fixture; } }

int wmain() {
    using namespace pulse;
    namespace fs = std::filesystem;
    const auto parent = fs::absolute(L"bench_data").lexically_normal();
    const auto base = parent / (L"context-menu-identity-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(parent);
    if (!fs::create_directory(base)) return 1;
    fixture = base.wstring();
    int failures = 0;
    auto check = [&](bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; };
    const std::wstring stable = L"pulse:quick-access";
    const auto category = ipc::CtxMenuCategory::Software;
    for (const auto label : {L"Pin to Quick access", L"固定到快速访问", L"固定到快速存取"}) {
        app::ContextMenuPrefs original;
        original.RecordSeen(stable, label, false, category, false);
        original.SetItemEnabled(stable, false);
        check(original.Save(), "save disabled stable command in private configuration");
        app::ContextMenuPrefs loaded;
        check(loaded.Load() && loaded.seen.size() == 1 && loaded.seen[0].key == stable &&
            !loaded.ItemEnabled(stable, category, false), "restart preserves the actual command choice and settings identity");
        loaded.SetItemEnabled(loaded.seen[0].key, true);
        check(loaded.Save(), "settings enable uses the same stable command key");
        app::ContextMenuPrefs restarted;
        check(restarted.Load() && restarted.ItemEnabled(stable, category, false) &&
            !restarted.RecordSeen(stable, L"固定到快速存取", false, category, false) && restarted.seen.size() == 1,
            "language change and another menu opening keep one functional settings row");
        for (int choice = 0; choice < 3; ++choice) {
            app::ContextMenuPrefs damaged;
            const auto alias = ipc::CatalogKey(label, false);
            damaged.RecordSeen(alias, label, false, category, false);
            damaged.SetItemEnabled(alias, false);
            if (choice != 0) {
                damaged.RecordSeen(stable, label, false, category, false);
                damaged.SetItemEnabled(stable, choice == 2);
                damaged.SetItemEnabled(alias, choice != 2);
            }
            app::ContextMenuPrefs repaired;
            check(repaired.FromJson(damaged.ToJson()) && repaired.seen.size() == 1 &&
                repaired.seen[0].key == stable && !repaired.item_enabled.contains(alias) &&
                repaired.ItemEnabled(stable, category, false) == (choice == 2),
                "repair old display alias; explicit stable override takes precedence");
            app::ContextMenuPrefs repeated;
            check(repeated.FromJson(repaired.ToJson()) && repeated.ToJson() == repaired.ToJson(),
                "repeated migration is idempotent");
        }
    }
    app::ContextMenuPrefs other;
    other.RecordSeen(L"pulse:future-command", L"Future translated label", false, category);
    const auto handler = ipc::HandlerCatalogKey(L"{00000000-0000-0000-0000-000000000001}");
    other.RecordSeen(handler, L"Handler", false, category, true);
    other.RecordSeen(L"v:New (N)", L"New (N)", false, category);
    other.RecordSeen(L"f: Open with ", L"Open with", true, category);
    other.SetItemEnabled(L"v:New (N)", false);
    other.MigrateSeenKeys();
    check(other.seen.size() == 4 && other.seen[0].key == L"pulse:future-command" && other.seen[1].key == handler,
        "other command and handler identities stay unchanged");
    check(other.seen[2].key == L"v:new" && other.seen[3].key == L"f:openwith" &&
        !other.ItemEnabled(L"v:new", category, false), "legacy display verbs and flyouts still normalize with their choices");
    app::ContextMenuPrefs com;
    com.RecordSeen(L"v:pintoquickaccess", L"Pin to Quick access", false, category, true);
    com.MigrateSeenKeys();
    check(com.seen[0].key == L"v:pintoquickaccess", "COM command with similar text is not reassigned to Pulse");
    {
        // Pulse menu card: new switchable rows, presets and preview rows.
        using app::BuiltinMenuItem;
        using app::BuiltinMenuPreset;
        app::ContextMenuPrefs fresh;
        check(fresh.builtin_hidden == 0 && app::BuiltinMenuPresetFor(fresh.builtin_hidden) == BuiltinMenuPreset::Full,
            "CMI-B01 defaults keep every Pulse row (preset 完整)");
        app::ContextMenuPrefs hide;
        hide.SetBuiltinVisible(BuiltinMenuItem::Paste, false);
        hide.SetBuiltinVisible(BuiltinMenuItem::View, false);
        hide.SetBuiltinVisible(BuiltinMenuItem::FolderProperties, false);
        app::ContextMenuPrefs back;
        check(back.FromJson(hide.ToJson()) && !back.BuiltinVisible(BuiltinMenuItem::Paste) &&
            !back.BuiltinVisible(BuiltinMenuItem::View) && !back.BuiltinVisible(BuiltinMenuItem::FolderProperties) &&
            back.BuiltinVisible(BuiltinMenuItem::Properties) && back.builtin_hidden == hide.builtin_hidden,
            "CMI-B02 new rows persist by key");
        check(app::BuiltinMenuPresetFor(back.builtin_hidden) == BuiltinMenuPreset::Count, "CMI-B03 hand-picked mask reads as 自定义");
        bool presets = true;
        for (int i = 0; i < static_cast<int>(BuiltinMenuPreset::Count); ++i) {
            const auto preset = static_cast<BuiltinMenuPreset>(i);
            presets &= app::BuiltinMenuPresetFor(app::BuiltinMenuPresetHidden(preset)) == preset;
        }
        const uint32_t slim = app::BuiltinMenuPresetHidden(BuiltinMenuPreset::Slim);
        presets &= (slim & app::BuiltinMenuBit(BuiltinMenuItem::Paste)) == 0 &&
            (slim & app::BuiltinMenuBit(BuiltinMenuItem::Properties)) == 0 &&
            (slim & app::BuiltinMenuBit(BuiltinMenuItem::PinNetwork)) != 0;
        check(presets, "CMI-B04 presets round-trip; 精简 keeps paste and properties");
        bool fixed_ok = true, separators_ok = true;
        for (int surface = 0; surface < static_cast<int>(app::BuiltinMenuSurface::Count); ++surface) {
            for (const uint32_t hidden : { 0u, slim, 0xffffffffu }) {
                const auto rows = app::BuiltinMenuVisibleRows(static_cast<app::BuiltinMenuSurface>(surface), hidden);
                if (!rows.empty()) separators_ok &= !rows.back().separator_after;
                int fixed = 0;
                for (const auto& r : rows) {
                    if (r.row.fixed != app::BuiltinFixedRow::None) ++fixed;
                    else fixed_ok &= (hidden & app::BuiltinMenuBit(r.row.item)) == 0;
                }
                if (surface == 0) fixed_ok &= fixed == 2;
            }
        }
        check(fixed_ok, "CMI-B05 open and the action strip always stay; hidden rows leave the preview");
        check(separators_ok, "CMI-B06 preview never ends in a separator");
        check(app::BuiltinMenuShared(BuiltinMenuItem::CopyPath) && app::BuiltinMenuShared(BuiltinMenuItem::Undo) &&
            !app::BuiltinMenuShared(BuiltinMenuItem::Paste) && !app::BuiltinMenuShared(BuiltinMenuItem::Properties),
            "CMI-B07 两处共用 marks rows present in both menus");
        // #303: the new row is a real catalog member, not a menu-only extra.
        check(app::BuiltinMenuShared(BuiltinMenuItem::CopyName) &&
            app::BuiltinMenuKey(BuiltinMenuItem::CopyName) == L"copy_name" &&
            (slim & app::BuiltinMenuBit(BuiltinMenuItem::CopyName)) == 0,
            "CMI-B09 复制名称 shares both menus, keeps 精简 and its own key");
        {
            const auto item_rows = app::BuiltinMenuDefaultOrder(app::BuiltinMenuSurface::Item);
            const auto bg_rows = app::BuiltinMenuDefaultOrder(app::BuiltinMenuSurface::Background);
            const auto before = [](const app::BuiltinMenuOrder& rows, BuiltinMenuItem a, BuiltinMenuItem b) {
                return std::find(rows.begin(), rows.end(), a) + 1 == std::find(rows.begin(), rows.end(), b);
            };
            check(before(item_rows, BuiltinMenuItem::CopyName, BuiltinMenuItem::CopyPath) &&
                before(bg_rows, BuiltinMenuItem::CopyName, BuiltinMenuItem::CopyPath),
                "CMI-B10 复制名称 sits right above 复制路径 in both surfaces");
            auto name_first = item_rows;
            name_first.erase(std::find(name_first.begin(), name_first.end(), BuiltinMenuItem::CopyName));
            name_first.insert(name_first.begin(), BuiltinMenuItem::CopyName);
            const auto moved = app::BuiltinMenuOrderedRows(app::BuiltinMenuSurface::Item, name_first);
            const auto item_surface = app::BuiltinMenuSurfaceRows(app::BuiltinMenuSurface::Item);
            check(moved.size() == item_surface.count && moved[2].item == BuiltinMenuItem::CopyName &&
                app::NormalizeBuiltinMenuOrder(app::BuiltinMenuSurface::Item, name_first) == name_first,
                "CMI-B11 the new row drags to the first slot and keeps its slot's separators");
        }
        bool labels = true;
        for (int i = 0; i < app::kBuiltinMenuItemCount; ++i)
            labels &= !app::BuiltinMenuKey(static_cast<BuiltinMenuItem>(i)).empty();
        check(labels, "CMI-B08 every Pulse row has a stable key");

        // Drag-to-reorder on the settings preview (CMI-O*).
        using app::BuiltinMenuSurface;
        auto copy_first = app::BuiltinMenuDefaultOrder(BuiltinMenuSurface::Item);
        copy_first.erase(std::find(copy_first.begin(), copy_first.end(), BuiltinMenuItem::CopyPath));
        copy_first.insert(copy_first.begin(), BuiltinMenuItem::CopyPath);
        auto row_order = app::BuiltinMenuOrder{ BuiltinMenuItem::RowMore, BuiltinMenuItem::RowStar, BuiltinMenuItem::RowNewTab };
        app::ContextMenuPrefs ordered;
        ordered.SetBuiltinOrder(BuiltinMenuSurface::Item, copy_first);
        ordered.SetBuiltinOrder(BuiltinMenuSurface::RowButtons, row_order);
        check(ordered.Save(), "CMI-O01 save a moved order");
        app::ContextMenuPrefs reread;
        check(reread.Load() && reread.BuiltinOrder(BuiltinMenuSurface::Item) == copy_first &&
            reread.BuiltinOrder(BuiltinMenuSurface::RowButtons) == row_order &&
            !reread.BuiltinOrderCustom(BuiltinMenuSurface::Background),
            "CMI-O02 the order survives a restart, per surface");
        app::ContextMenuPrefs table;
        table.SetBuiltinOrder(BuiltinMenuSurface::Item, app::BuiltinMenuDefaultOrder(BuiltinMenuSurface::Item));
        check(table.builtin_order[0].empty() && table.ToJson().find(L"open_new_tab,") == std::wstring::npos,
            "CMI-O03 the table order is stored as no order");
        const auto cleaned = app::NormalizeBuiltinMenuOrder(BuiltinMenuSurface::Background,
            { BuiltinMenuItem::Undo, BuiltinMenuItem::Undo, BuiltinMenuItem::RowStar, BuiltinMenuItem::View });
        check(cleaned.size() == app::BuiltinMenuDefaultOrder(BuiltinMenuSurface::Background).size() &&
            cleaned[0] == BuiltinMenuItem::Undo && cleaned[1] == BuiltinMenuItem::FolderProperties &&
            cleaned[2] == BuiltinMenuItem::View && cleaned[3] == BuiltinMenuItem::Sort &&
            std::count(cleaned.begin(), cleaned.end(), BuiltinMenuItem::Undo) == 1 &&
            std::find(cleaned.begin(), cleaned.end(), BuiltinMenuItem::RowStar) == cleaned.end(),
            "CMI-O04 a saved order drops repeats and foreign rows; missing ones follow their table predecessor");
        auto undo_first = app::BuiltinMenuDefaultOrder(BuiltinMenuSurface::Item);
        undo_first.erase(std::find(undo_first.begin(), undo_first.end(), BuiltinMenuItem::Undo));
        undo_first.insert(undo_first.begin(), BuiltinMenuItem::Undo);
        const auto table_rows = app::BuiltinMenuSurfaceRows(BuiltinMenuSurface::Item);
        const auto moved_rows = app::BuiltinMenuOrderedRows(BuiltinMenuSurface::Item, undo_first);
        bool slots = moved_rows.size() == table_rows.count;
        for (size_t i = 0; slots && i < moved_rows.size(); ++i)
            slots = moved_rows[i].separator_after == table_rows.rows[i].separator_after &&
                moved_rows[i].fixed == table_rows.rows[i].fixed;
        check(slots && moved_rows[2].item == BuiltinMenuItem::Undo && moved_rows[2].shortcut != nullptr &&
            moved_rows.back().item == BuiltinMenuItem::Tags,
            "CMI-O05 moved rows keep their look; separators stay with the slot");
        const auto visible = app::BuiltinMenuVisibleRows(BuiltinMenuSurface::Item, 0, undo_first);
        check(visible.size() == table_rows.count && visible[2].row.item == BuiltinMenuItem::Undo &&
            !visible.back().separator_after,
            "CMI-O06 the preview follows the saved order");
        check(app::RowActionMask(0, row_order) ==
                (app::kRowActionsAll | ((2u | (0u << 2) | (1u << 4)) << app::kRowActionOrderShift)) &&
            app::RowActionMask(0, app::BuiltinMenuDefaultOrder(BuiltinMenuSurface::RowButtons)) == app::kRowActionsAll &&
            app::RowActionMask(app::BuiltinMenuBit(BuiltinMenuItem::RowStar), {}) == (app::kRowActionNewTab | app::kRowActionMore),
            "CMI-O07 the row buttons carry their order to the renderer");
        ordered.ResetToDefaults();
        check(!ordered.BuiltinOrderCustom(BuiltinMenuSurface::Item) && !ordered.BuiltinOrderCustom(BuiltinMenuSurface::RowButtons),
            "CMI-O08 restore defaults also restores the order");
    }
    if (base.parent_path() != parent || !base.filename().wstring().starts_with(L"context-menu-identity-")) return 1;
    std::error_code error; fs::remove_all(base, error);
    check(!error && !fs::exists(base), "private preferences fixture removed");
    return failures ? 1 : 0;
}

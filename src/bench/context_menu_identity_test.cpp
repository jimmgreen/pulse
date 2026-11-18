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
            const auto hidden = app::BuiltinMenuPresetHidden(preset);
            presets &= app::BuiltinMenuPresetFor(hidden) == preset;
            presets &= app::RowActionMask(hidden) == app::kRowActionsAll;
            presets &= app::BuiltinMenuPresetFor(hidden | app::BuiltinMenuBit(BuiltinMenuItem::RowStar)) == preset;
        }
        const uint64_t slim = app::BuiltinMenuPresetHidden(BuiltinMenuPreset::Slim);
        presets &= (slim & app::BuiltinMenuBit(BuiltinMenuItem::Paste)) == 0 &&
            (slim & app::BuiltinMenuBit(BuiltinMenuItem::Properties)) == 0 &&
            (slim & app::BuiltinMenuBit(BuiltinMenuItem::PinNetwork)) != 0;
        check(presets, "CMI-B04 presets round-trip; 精简 keeps paste and properties");
        bool fixed_ok = true, separators_ok = true;
        for (int surface = 0; surface < static_cast<int>(app::BuiltinMenuSurface::Count); ++surface) {
            for (const uint64_t hidden : { uint64_t{0}, slim, ~uint64_t{0} }) {
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
        bool labels = true;
        for (int i = 0; i < app::kBuiltinMenuItemCount; ++i)
            labels &= !app::BuiltinMenuKey(static_cast<BuiltinMenuItem>(i)).empty();
        check(labels, "CMI-B08 every Pulse row has a stable key");


        app::ContextMenuPrefs expanded;
        uint64_t expected_hidden = 0;
        for (int i = 22; i < app::kBuiltinMenuItemCount; ++i) {
            const auto item = static_cast<BuiltinMenuItem>(i);
            expanded.SetBuiltinVisible(item, false);
            expected_hidden |= app::BuiltinMenuBit(item);
        }
        app::ContextMenuPrefs expanded_back;
        check(expanded_back.FromJson(expanded.ToJson()) && expanded_back.builtin_hidden == expected_hidden &&
            (expected_hidden >> 32) != 0 && expanded_back.BuiltinVisible(BuiltinMenuItem::OpenInNewTab) &&
            !expanded_back.BuiltinVisible(BuiltinMenuItem::CloudFree) &&
            !expanded_back.BuiltinVisible(BuiltinMenuItem::MoveToWindow),
            "CMI-B09 all 25 added keys round-trip including bits above 31");
        expanded_back.SetBuiltinVisible(BuiltinMenuItem::MoveToWindow, true);
        check(expanded_back.builtin_hidden == (expected_hidden & ~app::BuiltinMenuBit(BuiltinMenuItem::MoveToWindow)) &&
            app::RowActionMask(expected_hidden) == app::kRowActionsAll,
            "CMI-B10 high-bit switch preserves other choices and the 32-bit row-action mask");
        const wchar_t* legacy_keys[] = {
            L"open_new_tab", L"copy_path", L"terminal", L"quick_access", L"pin_workspace", L"pin_network",
            L"tags", L"recent_changes", L"select", L"undo", L"row_new_tab", L"row_star", L"row_more",
            L"view", L"sort", L"group", L"refresh", L"new_folder", L"new_text", L"paste", L"properties", L"folder_properties"
        };
        bool legacy_compatible = true;
        for (int i = 0; i < 22; ++i)
            legacy_compatible &= app::BuiltinMenuKey(static_cast<BuiltinMenuItem>(i)) == legacy_keys[i];
        app::ContextMenuPrefs legacy;
        legacy_compatible &= legacy.FromJson(LR"({"pulse_items":{"copy_path":false,"row_star":false}})") &&
            legacy.builtin_hidden == (app::BuiltinMenuBit(BuiltinMenuItem::CopyPath) | app::BuiltinMenuBit(BuiltinMenuItem::RowStar));
        check(legacy_compatible, "CMI-B11 original 22 numeric positions and JSON keys stay compatible");
        int coverage[app::kBuiltinMenuItemCount]{};
        bool unique_surfaces = true;
        for (int i = 0; i < static_cast<int>(app::BuiltinMenuSurface::Count); ++i) {
            const auto surface = static_cast<app::BuiltinMenuSurface>(i);
            uint64_t seen = 0;
            for (const auto item : app::BuiltinMenuDefaultOrder(surface)) {
                const auto bit = app::BuiltinMenuBit(item);
                unique_surfaces &= (seen & bit) == 0;
                seen |= bit;
                ++coverage[static_cast<int>(item)];
            }
        }
        for (int i = 0; i < app::kBuiltinMenuItemCount; ++i)
            unique_surfaces &= coverage[i] == (app::BuiltinMenuShared(static_cast<BuiltinMenuItem>(i)) ? 2 : 1);
        check(unique_surfaces && app::BuiltinMenuShared(BuiltinMenuItem::MapNetworkDrive),
            "CMI-B12 every setting has unique surface coverage; map drive is shared");
        const auto item_surface = app::BuiltinMenuDefaultOrder(app::BuiltinMenuSurface::Item);
        const auto background_surface = app::BuiltinMenuDefaultOrder(app::BuiltinMenuSurface::Background);
        bool new_surfaces = true;
        for (int i = 22; i < app::kBuiltinMenuItemCount; ++i) {
            const auto item = static_cast<BuiltinMenuItem>(i);
            const bool background = item == BuiltinMenuItem::FindDuplicates || item == BuiltinMenuItem::AiOrganizeHere ||
                item == BuiltinMenuItem::NewTemplates || item == BuiltinMenuItem::Git || item == BuiltinMenuItem::ConnectServer;
            const bool shared = item == BuiltinMenuItem::MapNetworkDrive;
            new_surfaces &= (std::find(item_surface.begin(), item_surface.end(), item) != item_surface.end()) == !background;
            new_surfaces &= (std::find(background_surface.begin(), background_surface.end(), item) != background_surface.end()) == (background || shared);
        }
        check(new_surfaces, "CMI-B13 each added setting belongs to its actual menu surface");
        bool old_orders_kept = true;
        for (int surface = 0; surface < static_cast<int>(app::BuiltinMenuSurface::Count); ++surface) {
            const auto kind = static_cast<app::BuiltinMenuSurface>(surface);
            auto old_order = app::BuiltinMenuDefaultOrder(kind);
            std::erase_if(old_order, [](auto item) { return static_cast<int>(item) >= 22; });
            std::reverse(old_order.begin(), old_order.end());
            auto normalized = app::NormalizeBuiltinMenuOrder(kind, old_order);
            std::erase_if(normalized, [](auto item) { return static_cast<int>(item) >= 22; });
            old_orders_kept &= normalized == old_order;
        }
        check(old_orders_kept, "CMI-B14 adding rows preserves users' saved relative order of original commands");

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

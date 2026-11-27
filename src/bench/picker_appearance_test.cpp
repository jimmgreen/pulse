#include "../app/picker_appearance.h"
#include "../app/picker_list_order.h"
#include "../app/app_prefs.h"
#include "../ui/folder_picker_dialog.h"
#include "../common/utf8_file.h"
#include <objbase.h>
#include <filesystem>
#include <iostream>

namespace {
int checks = 0, failures = 0;
void Check(bool passed, const char* description) {
    ++checks; if (!passed) ++failures;
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << description << '\n';
}
bool WriteFixture(const std::filesystem::path& path, const std::wstring& json) {
    std::vector<uint8_t> bytes;
    if (!pulse::EncodeUtf8Bytes(json, bytes)) return false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool result = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file); return result;
}
}
int main() {
    using namespace pulse::app;
    {
        PickerAppearanceSnapshot session; session.theme_mode = 2; session.language = L"en";
        PickerAppearanceReadResult latest; latest.status = PickerAppearanceReadStatus::Loaded;
        ParsePickerAppearanceJson(LR"({"show_hidden_files":true,"show_protected_os_files":true,"show_file_extensions":false,"sort_default_column":5})", latest.snapshot);
        RefreshPickerListPreferences(session, latest);
        Check(session.show_hidden_files && session.show_protected_os_files && !session.show_file_extensions &&
            session.list_order && session.list_order->Resolve(L"C:\\fixture").column == 5, "reused session refreshes current visibility and sort preferences");
        Check(session.theme_mode == 2 && session.language == L"en", "list refresh preserves initialized theme and language");
        latest = {}; RefreshPickerListPreferences(session, latest);
        Check(session.show_hidden_files && !session.show_file_extensions && session.list_order,
            "temporary unreadable preferences retain the last valid listing settings");
        latest.status = PickerAppearanceReadStatus::Missing; RefreshPickerListPreferences(session, latest);
        Check(!session.show_hidden_files && !session.show_protected_os_files && session.show_file_extensions && !session.list_order,
            "removed preferences restore list defaults without changing session theme");
    }
    PickerAppearanceSnapshot value;
    Check(ParsePickerAppearanceJson(L"{}", value) && value.ui_font_scale == 100 && value.text_render == 0 &&
        value.row_height == 34 && value.smart_date && value.zebra_rows && value.thumbnail_badges, "empty object has Pulse defaults");
    Check(!value.show_hidden_files && !value.show_protected_os_files && value.show_file_extensions,
        "missing visibility preferences match main window defaults");
    for (int mask = 0; mask < 8; ++mask) {
        AppPrefs visibility_prefs;
        visibility_prefs.show_hidden_files = (mask & 1) != 0;
        visibility_prefs.show_protected_os_files = (mask & 2) != 0;
        visibility_prefs.show_file_extensions = (mask & 4) != 0;
        const std::wstring visibility_json = std::wstring(L"{\"show_hidden_files\":") +
            (visibility_prefs.show_hidden_files ? L"true" : L"false") + L",\"show_protected_os_files\":" +
            (visibility_prefs.show_protected_os_files ? L"true" : L"false") + L",\"show_file_extensions\":" +
            (visibility_prefs.show_file_extensions ? L"true" : L"false") + L"}";
        PickerAppearanceSnapshot parsed_visibility;
        Check(ParsePickerAppearanceJson(visibility_json, parsed_visibility), "visibility preference combination parses");
        for (const auto& visibility : {parsed_visibility, PickerAppearanceFromPrefs(visibility_prefs)}) {
            Check(visibility.show_hidden_files == visibility_prefs.show_hidden_files &&
                visibility.show_protected_os_files == visibility_prefs.show_protected_os_files &&
                visibility.show_file_extensions == visibility_prefs.show_file_extensions,
                "persisted and in-memory visibility snapshots match main window preferences");
            for (bool forced_hidden : {false, true}) {
                pulse::ui::FolderPickerSpec visibility_spec;
                visibility_spec.show_hidden = forced_hidden;
                ApplyPickerAppearanceToSpec(visibility, visibility_spec);
                Check(visibility_spec.show_hidden == forced_hidden &&
                    visibility_spec.show_hidden_files == visibility_prefs.show_hidden_files &&
                    visibility_spec.show_protected_os_files == visibility_prefs.show_protected_os_files &&
                    visibility_spec.show_file_extensions == visibility_prefs.show_file_extensions,
                    "spec receives all visibility preferences without overwriting caller force flag");
            }
        }
    }
    PickerAppearanceSnapshot invalid_visibility;
    Check(ParsePickerAppearanceJson(LR"({"nested":{"show_hidden_files":true,"show_protected_os_files":true,"show_file_extensions":false},"show_hidden_files":"true","show_protected_os_files":null,"show_file_extensions":"false"})", invalid_visibility) &&
        !invalid_visibility.show_hidden_files && !invalid_visibility.show_protected_os_files && invalid_visibility.show_file_extensions,
        "invalid and nested visibility preferences use main window defaults");
    for (int action : {0, 1, 2, 99}) {
        const int expected = action == 1 || action == 2 ? action : 0;
        Check(ParsePickerAppearanceJson(L"{\"blank_click_action\":" + std::to_wstring(action) + L"}", value) &&
            value.blank_click_action == expected, "blank action JSON is normalized");
        pulse::ui::FolderPickerSpec action_spec;
        ApplyPickerAppearanceToSpec(value, action_spec);
        Check(action_spec.blank_click_action == expected, "blank action reaches open/save spec");
        AppPrefs action_prefs; action_prefs.blank_click_action = action;
        Check(PickerAppearanceFromPrefs(action_prefs).blank_click_action == expected, "main prefs retain blank action");
    }
    Check(ParsePickerAppearanceJson(L"{\"blank_click_go_back\":true}", value) && value.blank_click_action == 1,
        "legacy blank back preference remains supported");
    ParsePickerAppearanceJson(L"{}", value);
    auto colors = ResolvePickerAppearanceColors(value, false, 0x123456);
    Check(!colors.dark && colors.accent_rgb == 0x527D70, "light default uses Pulse pine accent");
    colors = ResolvePickerAppearanceColors(value, true, 0x123456);
    Check(colors.dark && colors.accent_rgb == 0x123456, "legacy without session follows system");
    colors = ResolvePickerAppearanceColors(value, true, 0x123456, false);
    Check(!colors.dark && colors.accent_rgb == 0x527D70, "main window can retain legacy session theme");
    const std::wstring json = LR"({"theme_mode":2,"accent_rgb":"#aBcD12","accent_follow_system":1,
        "language":"zh-TW","ui_font_scale":125,"text_render":2,"row_height":24,
        "list_selection_outline":true,"list_smart_date":false,"list_zebra_rows":false,
        "list_size_bar":true,"list_thumbnail_badges":false,"details_columns":0,"details_columns_v":2,
        "show_hidden_files":true,"show_protected_os_files":true,"show_file_extensions":false,
        "nested":{"theme_mode":1},"unrelated":"synthetic-only"})";
    Check(ParsePickerAppearanceJson(json, value) && value.theme_mode == 2 && value.language == L"zh-TW" &&
        value.accent_rgb == 0xABCD12u && value.ui_font_scale == 125 && value.text_render == 2 && value.details_columns == 0,
        "whitelist retains normalized appearance and explicit zero columns");
    colors = ResolvePickerAppearanceColors(value, false, 0x654321);
    Check(colors.dark && colors.accent_rgb == 0xABCD12, "custom accent takes precedence over system");
    Check(PickerAppearanceRowHeightDip(value) == 35, "125 percent text raises compact row minimum");
    pulse::ui::FolderPickerSpec spec;
    Check(!spec.explicit_details_columns, "ordinary spec retains implicit default columns");
    spec.initial_path = L"C:\\fixture"; spec.filename = L"keep.txt"; spec.show_hidden = true;
    spec.advisory_file_filters = true; spec.skip_access_validation = true;
    spec.allow_multiselect = true; spec.filters = {{L"Text", L"*.txt"}};
    ApplyPickerAppearanceToSpec(value, spec);
    Check(spec.selection_outline && spec.row_height_dip == 35 && !spec.list_smart_date && !spec.list_zebra_rows &&
        spec.list_size_bar && !spec.thumbnail_badges && spec.details_columns == 0 && spec.explicit_details_columns,
        "snapshot marks zero columns explicit rather than renderer defaults");
    Check(spec.initial_path == L"C:\\fixture" && spec.filename == L"keep.txt" && spec.show_hidden &&
        spec.advisory_file_filters && spec.skip_access_validation && spec.allow_multiselect && spec.filters[0].pattern == L"*.txt",
        "appearance does not overwrite dialog request semantics");
    {
        pulse::ui::FolderPickerSpec plain;
        Check(plain.window_effect == pulse::ui::WindowEffect::Acrylic, "a host without preferences keeps the acrylic dialog");
        PickerAppearanceSnapshot effect;
        Check(ParsePickerAppearanceJson(L"{}", effect) && effect.window_effect == L"mica-alt", "missing window effect matches the main window default");
        for (const auto& [id, expected] : {std::pair{L"mica-alt", pulse::ui::WindowEffect::MicaAlt}, std::pair{L"mica", pulse::ui::WindowEffect::Mica},
                                           std::pair{L"acrylic-material", pulse::ui::WindowEffect::Acrylic}, std::pair{L"none", pulse::ui::WindowEffect::None}}) {
            Check(ParsePickerAppearanceJson(std::wstring(L"{\"window_effect\":\"") + id + L"\"}", effect), "window effect parses");
            pulse::ui::FolderPickerSpec effect_spec;
            ApplyPickerAppearanceToSpec(effect, effect_spec);
            Check(effect_spec.window_effect == expected, "the dialog follows the main window effect");
        }
        pulse::app::AppPrefs effect_prefs;
        effect_prefs.window_effect = L"mica";
        Check(PickerAppearanceFromPrefs(effect_prefs).window_effect == L"mica", "main prefs carry the window effect");
        Check(ParsePickerAppearanceJson(L"{}", effect) && effect.panel_transparency == 50 && effect.frame_transparency == 10,
            "missing transparency matches the main window defaults");
        Check(ParsePickerAppearanceJson(L"{\"wallpaper_look\":1,\"panel_transparency\":30,\"frame_transparency\":100}", effect) &&
            effect.panel_transparency == 30 && effect.frame_transparency == 100, "saved transparency is read");
        Check(ParsePickerAppearanceJson(L"{\"wallpaper_look\":2}", effect) && effect.panel_transparency == 75, "legacy transparency level migrates");
        Check(ParsePickerAppearanceJson(L"{\"panel_transparency\":120,\"frame_transparency\":-1}", effect) &&
            effect.panel_transparency == 50 && effect.frame_transparency == 10, "out-of-range transparency falls back");
        pulse::ui::FolderPickerSpec layered;
        effect.panel_transparency = 20; effect.frame_transparency = 100;
        ApplyPickerAppearanceToSpec(effect, layered);
        Check(layered.panel_transparency == 20 && layered.frame_transparency == 100, "the dialog takes the main window transparency");
    }
    Check(!ParsePickerAppearanceJson(L"{broken}", value) && value.accent_rgb == 0xABCD12u, "malformed input preserves output");
    Check(ParsePickerAppearanceJson(LR"({"nested":{"theme_mode":2,"ui_font_scale":125},"language":"unsupported","theme_mode":99,"text_render":99,"row_height":4,"ui_font_scale":111,"accent_rgb":"oops"})", value) &&
        value.theme_mode == -1 && value.language == L"system" && value.ui_font_scale == 100 && value.text_render == 0 &&
        value.row_height == 34 && !value.accent_rgb, "invalid values normalize and nested values never substitute");
    Check(ParsePickerAppearanceJson(LR"({"details_columns":0})", value) && value.details_columns == pulse::ui::kDetailsColumnStatus,
        "legacy columns add status just as AppPrefs does");
    Check(ParsePickerAppearanceJson(LR"({"ui_font_scale":999999999999999999999999,"row_height":34.5,"text_render":-9999999999999999999999})", value) &&
        value.ui_font_scale == 100 && value.row_height == 34 && value.text_render == 0,
        "oversized and fractional numeric fields safely fall back without integer overflow");
    AppPrefs prefs;
    prefs.persist = false; prefs.theme_mode = 1; prefs.accent_rgb = L"#123aBc"; prefs.language = L"en-US";
    prefs.ui_font_scale = 112; prefs.text_render = 1; prefs.row_height = 24; prefs.details_columns = 0;
    value = PickerAppearanceFromPrefs(prefs);
    Check(value.theme_mode == 1 && value.accent_rgb == 0x123ABCu && value.language == L"en-US" &&
        value.text_render == 1 && PickerAppearanceRowHeightDip(value) == 32 && value.details_columns == 0,
        "existing in-memory preferences need no Load or persistence");

    Check(ParsePickerAppearanceJson(L"{}", value) && value.preview_trigger == kPreviewTriggerHover &&
        value.hover_preview_delay_ms == 500 && value.hover_card_size == kHoverCardMedium,
        "missing hover preferences use main window defaults");
    Check(ParsePickerAppearanceJson(LR"({"preview_trigger":1,"hover_preview_delay_ms":800,"hover_card_size":3})", value) &&
        value.preview_trigger == kPreviewTriggerSpace && value.hover_preview_delay_ms == 800 && value.hover_card_size == kHoverCardXLarge,
        "hover preferences retain disabled trigger and supported delay and size");
    ApplyPickerAppearanceToSpec(value, spec);
    Check(spec.preview_trigger == kPreviewTriggerSpace && spec.hover_preview_delay_ms == 800 && spec.hover_card_size == kHoverCardXLarge,
        "hover preferences reach picker spec");
    Check(ParsePickerAppearanceJson(LR"({"preview_trigger":9,"hover_preview_delay_ms":1,"hover_card_size":9})", value) &&
        value.preview_trigger == kPreviewTriggerHover && value.hover_preview_delay_ms == 500 && value.hover_card_size == kHoverCardMedium,
        "invalid hover preferences normalize like main window");
    prefs.preview_trigger = kPreviewTriggerSpace; prefs.hover_preview_delay_ms = 300; prefs.hover_card_size = kHoverCardSmall;
    value = PickerAppearanceFromPrefs(prefs);
    Check(value.preview_trigger == kPreviewTriggerSpace && value.hover_preview_delay_ms == 300 && value.hover_card_size == kHoverCardSmall,
        "in-memory hover preferences use the same snapshot path");

    // All I/O uses a newly-owned fixture. Never call the real-user resolver here.
    GUID guid{}; wchar_t nonce[40]{};
    const bool named = SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(guid, nonce, ARRAYSIZE(nonce));
    const auto base = std::filesystem::current_path() / L"bench_data" / (L"picker-appearance-" + std::wstring(nonce));
    const bool created = named && CreateDirectoryW(base.c_str(), nullptr);
    Check(created, "create unique synthetic fixture");
    if (!created) return 1;
    const auto file = base / L"appearance.json";
    const bool wrote = WriteFixture(file, json);
    Check(wrote, "CREATE_NEW synthetic appearance file");
    const auto loaded = ReadPickerAppearanceFileReadOnly(file.wstring());
    Check(wrote && loaded.status == PickerAppearanceReadStatus::Loaded && loaded.error == ERROR_SUCCESS &&
        loaded.snapshot.language == L"zh-TW" && loaded.snapshot.accent_rgb == 0xABCD12u, "read-only snapshot loads whitelist");
    Check(loaded.snapshot.show_hidden_files && loaded.snapshot.show_protected_os_files && !loaded.snapshot.show_file_extensions,
        "standalone read-only file path carries file visibility preferences");
    std::wstring after;
    Check(pulse::ReadUtf8File(file.wstring(), after) && after == json, "read does not rewrite fixture content");
    const auto missing_dir = base / L"absent";
    const auto missing = ReadPickerAppearanceFileReadOnly((missing_dir / L"app.json").wstring());
    Check(missing.status == PickerAppearanceReadStatus::Missing && missing.error == ERROR_PATH_NOT_FOUND &&
        GetFileAttributesW(missing_dir.c_str()) == INVALID_FILE_ATTRIBUTES, "missing read never creates directories");
    HANDLE exclusive = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(exclusive != INVALID_HANDLE_VALUE, "snapshot read closed its handle");
    const auto locked = ReadPickerAppearanceFileReadOnly(file.wstring());
    Check(exclusive != INVALID_HANDLE_VALUE && locked.status == PickerAppearanceReadStatus::Failed &&
        locked.error == ERROR_SHARING_VIOLATION && locked.snapshot.ui_font_scale == 100, "locked read returns exact failure and safe defaults");
    if (exclusive != INVALID_HANDLE_VALUE) CloseHandle(exclusive);
    const auto invalid = base / L"invalid.json";
    const bool bad_written = WriteFixture(invalid, L"{bad}");
    const auto bad = ReadPickerAppearanceFileReadOnly(invalid.wstring());
    Check(bad_written && bad.status == PickerAppearanceReadStatus::Failed && bad.error == ERROR_INVALID_DATA,
        "invalid persisted JSON is failure rather than successful defaults");
    Check(DeleteFileW(file.c_str()) && DeleteFileW(invalid.c_str()) && RemoveDirectoryW(base.c_str()), "exact owned fixtures cleaned");
    {
        // The dialog lists a folder in the main window's remembered order.
        AppPrefs order_prefs;
        order_prefs.folder_sorts.Set(L"D:\\Work", {pulse::ui::SortColumn::Mtime, pulse::ui::SortDirection::Desc});
        order_prefs.folder_sorts.SetFolderMode(L"D:\\Work", 2);
        order_prefs.folder_groups.Set(L"D:\\Work", GroupBy::Date);
        order_prefs.folder_groups.Set(L"D:\\Tagged", GroupBy::Tag);
        order_prefs.folder_sort_mode = 1;
        std::wstring order_json = L"{\"theme_mode\":0";
        order_prefs.folder_sorts.AppendJson(order_json);
        order_prefs.folder_groups.AppendJson(order_json);
        order_json += L",\n  \"folder_sort_mode\":1}";
        PickerAppearanceSnapshot parsed;
        Check(ParsePickerAppearanceJson(order_json, parsed) && parsed.list_order, "app.json carries the per-folder list order");
        for (const auto& source : {parsed.list_order, PickerListOrderPrefsFromPrefs(order_prefs)}) {
            if (!source) continue;
            const auto work = source->Resolve(L"d:\\work\\");
            Check(work.column == static_cast<int>(pulse::ui::SortColumn::Mtime) && work.descending &&
                  work.folder_mode == 2 && work.group_by == static_cast<int>(GroupBy::Date),
                  "a remembered folder resolves its sort, folder mode and group-by");
            const auto other = source->Resolve(L"D:\\Other");
            Check(other.column == 0 && !other.descending && other.folder_mode == 1 && other.group_by == 0,
                  "other folders use the default sort and the global folder mode");
            Check(source->Resolve(L"D:\\Tagged").group_by == 0, "groups the dialog cannot compute list flat");
        }
        pulse::ui::FolderPickerSpec order_spec;
        ApplyPickerAppearanceToSpec(parsed, order_spec);
        Check(order_spec.folder_order && order_spec.folder_order(L"D:\\Work").descending, "the spec resolves folders through the snapshot");

        // Rows: a folder, a file from today, an old file, a second file from today.
        SYSTEMTIME now_st{}; GetSystemTime(&now_st);
        FILETIME now{}; SystemTimeToFileTime(&now_st, &now);
        FILETIME old{}; SYSTEMTIME old_st{2019, 3, 0, 5, 12, 0, 0, 0}; SystemTimeToFileTime(&old_st, &old);
        std::vector<pulse::fs::DirEntry> rows(4);
        rows[0].name = L"sub"; rows[0].is_dir = true; rows[0].attrs = FILE_ATTRIBUTE_DIRECTORY; rows[0].mtime = old;
        rows[1].name = L"new-a.txt"; rows[1].mtime = now; rows[1].size = 10;
        rows[2].name = L"old.txt"; rows[2].mtime = old; rows[2].size = 20;
        rows[3].name = L"new-b.txt"; rows[3].mtime = now; rows[3].size = 30;
        std::vector<pulse::ui::PickerEntry> entries(4);
        for (size_t i = 0; i < entries.size(); ++i) { entries[i].name = rows[i].name; entries[i].source = i; }
        pulse::ui::PickerListOrder by_date;
        by_date.column = static_cast<int>(pulse::ui::SortColumn::Name);
        by_date.group_by = static_cast<int>(GroupBy::Date);
        OrderPickerEntries(entries, rows, L"D:\\Work", by_date);
        Check(entries[0].name == L"sub" && entries[1].name == L"new-a.txt" && entries[2].name == L"new-b.txt" &&
              entries[3].name == L"old.txt", "rows are ordered by group, then by the sort column");
        std::vector<pulse::fs::DirEntry> ordered;
        for (const auto& entry : entries) ordered.push_back(rows[entry.source]);
        const auto groups = BuildPickerListGroups(ordered, by_date, {});
        Check(groups && groups->size() == 3 && (*groups)[0].count == 1 && (*groups)[1].count == 2 &&
              (*groups)[2].count == 1 && (*groups)[1].bytes == 40, "group spans cover every row in order");
        const auto folded = BuildPickerListGroups(ordered, by_date, {groups ? (*groups)[2].key : std::wstring()});
        Check(folded && folded->size() == 3 && (*folded)[2].collapsed && !(*folded)[1].collapsed, "collapsed keys fold their group");
        std::swap(ordered[2], ordered[3]);  // today, old, today: not one run per group
        Check(!BuildPickerListGroups(ordered, by_date, {}), "rows out of grouped order are drawn flat");
        pulse::ui::PickerListOrder flat = by_date;
        flat.group_by = 0;
        Check(!BuildPickerListGroups(ordered, flat, {}), "no group-by, no spans");
    }
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures ? 1 : 0;
}

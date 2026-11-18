#include "../app/picker_appearance.h"
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
    PickerAppearanceSnapshot value;
    Check(ParsePickerAppearanceJson(L"{}", value) && value.ui_font_scale == 100 && value.text_render == 0 &&
        value.row_height == 34 && value.smart_date && value.zebra_rows && value.thumbnail_badges, "empty object has Pulse defaults");
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
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures ? 1 : 0;
}

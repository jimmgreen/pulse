// app_prefs.cpp — Persist general settings; sync 开机自启 with the Run key.
#include "app_prefs.h"
#include "blank_pane_click.h"
#include "startup_launch.h"
#include "default_file_manager.h"
#include "shell_integration_registry.h"
#include "session.h"
#include "../ui/panel_metrics.h"
#include "../common/json_utils.h"
#include "../common/config_json.h"
#include "../common/utf8_file.h"
#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cwctype>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

namespace pulse::app {
namespace {

constexpr const wchar_t* kRunKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValue = L"Pulse";

// A self-test run may point every registry path below into a sandbox key, so the
// reconciliation can be exercised without touching the machine's real settings.
// Production, and any run that does not ask for it, gets the real paths.
std::wstring RegistrySandboxPrefix() {
#ifdef PULSE_WITH_SELFTEST
    wchar_t base[512]{};
    if (GetEnvironmentVariableW(L"PULSE_TEST_REGISTRY_BASE", base, ARRAYSIZE(base)) > 0)
        return base;
#endif
    return {};
}

std::wstring RegPath(const std::wstring& path) {
    const std::wstring prefix = RegistrySandboxPrefix();
    return prefix.empty() ? path : prefix + L"\\" + path;
}

std::wstring ExePath() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return n ? std::wstring(path, n) : L"";
}

// The exact Run value this executable installs; one place so the comparison in
// ReconcileRegistryWithFile() cannot drift from what ApplyLaunchOnStartup writes.
std::wstring LaunchOnStartupCommand() {
    const std::wstring exe = ExePath();
    return exe.empty() ? std::wstring() : L"\"" + exe + L"\"";
}

} // namespace

namespace {

bool IsStoredWallpaper(const std::wstring& path, const std::wstring& dir) {
    const std::wstring prefix = dir + L"\\";
    if (!path.starts_with(prefix)) return false;
    const std::wstring name = path.substr(prefix.size());
    return name.find_first_of(L"\\/") == std::wstring::npos &&
        (name.starts_with(L"wallpaper.") ||
         (name.starts_with(L"pwp") && name.find(L".tmp.") != std::wstring::npos));
}

} // namespace

namespace {

// Every key ToJson() writes (except "version"), in the order it writes them. A
// file truncated mid-write still parses into a handful of keys, so the count
// doubles as the completeness test that sends Load() to the backup.
constexpr const wchar_t* kStoredKeys[] = {
    L"launch_on_startup", L"keep_running_on_close", L"open_folders_in_pulse",
    L"verify_copies", L"show_status_performance", L"show_pinned_tab_names",
    L"multi_instance_mode",
    L"show_hidden_files", L"show_protected_os_files", L"search_pinyin",
    L"global_search_enabled", L"global_search_modifiers", L"global_search_key",
    L"blank_click_action", L"change_tracking_enabled", L"change_tracking_days",
    L"theme_mode", L"language", L"window_effect", L"background_image",
    L"row_height", L"sidebar_width", L"address_search_current",
    L"address_search_content", L"tray_icon_size", L"accent_rgb",
    L"custom_tag_colors", L"duplicate_scan_scope", L"duplicate_scan_folder",
    L"duplicate_scan_drive"
};

// Half the keys, rounded up: fewer than this and the file is treated as damaged
// rather than as the user's configuration.
constexpr int kMinStoredKeys = (static_cast<int>(ARRAYSIZE(kStoredKeys)) + 1) / 2;

int CountStoredKeys(const std::wstring& json) {
    int found = 0;
    for (const wchar_t* key : kStoredKeys) {
        if (json.find(L"\"" + std::wstring(key) + L"\"") != std::wstring::npos) ++found;
    }
    return found;
}

} // namespace

void AppPrefs::ResetToDefaults() {
    launch_on_startup = false;
    keep_running_on_close = false;
    notify_icon_mode = 0;
    open_folders_in_pulse = false;
    take_over_win_e = false;
    take_over_this_pc = false;
    integration_enabled = false;
    integration_folders = integration_win_e = integration_this_pc = true;
    integration_configured = false;
    integration_residual = false;
    integration_incomplete = false;
    take_over_explorer_windows = false;
    shell_tag_menu = false;
    verify_copies = false;
    show_status_performance = false;
    show_pinned_tab_names = true;
    multi_instance_mode = false;
    list_smart_date = true;
    list_zebra_rows = true;
    list_size_bar = false;
    list_selection_outline = false;
    folder_sort_mode = 0;
    details_columns = ui::kDetailsColumnsDefault;
    startup_open = 0;
    new_tab_open = 0;
    close_window_with_last_tab = false;
    confirm_recycle_delete = false;
    start_in_tray = false;
    home_folder.clear();
    text_render = 0;
    ui_font_scale = 100;
    folder_views.Clear();
    folder_sorts.Clear();
    folder_groups.Clear();
    search_pinyin = true;
    global_search_enabled = false;
    global_search_modifiers = 1;
    global_search_key = 32;
    show_hidden_files = false;
    show_protected_os_files = false;
    blank_click_action = kBlankClickOff;
    change_tracking_enabled = false;
    change_tracking_days = 7;
    theme_mode = -1;
    language = L"system";
    window_effect = L"mica-alt";
    background_image.clear();
    wallpaper_look = 50;
    wallpaper_blur = 14;
    row_height = 34;
    sidebar_width = 224;
    address_search_current = false;
    address_search_content = false;
    tray_icon_size = 48;
    show_hints = true;
    auto_check_updates = true;
    tips_seen = 0;
    accent_rgb.clear();
    accent_follow_system = false;
    custom_tag_colors.clear();
    duplicate_scan_scope = 0;
    duplicate_scan_folder.clear();
    duplicate_scan_drive.clear();
}

std::wstring AppPrefs::ToJson() const {
    std::wstring escaped_effect;
    std::wstring escaped_image;
    std::wstring escaped_language;
    std::wstring escaped_home;
    pulse::json::Escape(window_effect, escaped_effect);
    pulse::json::Escape(home_folder, escaped_home);
    pulse::json::Escape(background_image, escaped_image);
    pulse::json::Escape(language, escaped_language);
    std::wstring out = L"{\n  \"version\":4,\n  \"launch_on_startup\":";
    out += launch_on_startup ? L"true" : L"false";
    out += L",\n  \"keep_running_on_close\":";
    out += keep_running_on_close ? L"true" : L"false";
    out += L",\n  \"notify_icon_mode\":";
    out += std::to_wstring(notify_icon_mode >= 0 && notify_icon_mode <= 2 ? notify_icon_mode : 0);
    out += L",\n  \"open_folders_in_pulse\":";
    out += open_folders_in_pulse ? L"true" : L"false";
    out += L",\n  \"integration_enabled\":";
    out += integration_enabled ? L"true" : L"false";
    out += L",\n  \"integration_folders\":";
    out += integration_folders ? L"true" : L"false";
    out += L",\n  \"integration_win_e\":";
    out += integration_win_e ? L"true" : L"false";
    out += L",\n  \"integration_this_pc\":";
    out += integration_this_pc ? L"true" : L"false";
    out += L",\n  \"take_over_explorer_windows\":";
    out += take_over_explorer_windows ? L"true" : L"false";
    out += L",\n  \"shell_tag_menu\":";
    out += shell_tag_menu ? L"true" : L"false";
    out += L",\n  \"verify_copies\":";
    out += verify_copies ? L"true" : L"false";
    out += L",\n  \"show_status_performance\":";
    out += show_status_performance ? L"true" : L"false";
    out += L",\n  \"show_pinned_tab_names\":";
    out += show_pinned_tab_names ? L"true" : L"false";
    out += L",\n  \"multi_instance_mode\":";
    out += multi_instance_mode ? L"true" : L"false";
    out += L",\n  \"list_smart_date\":";
    out += list_smart_date ? L"true" : L"false";
    out += L",\n  \"list_zebra_rows\":";
    out += list_zebra_rows ? L"true" : L"false";
    out += L",\n  \"list_size_bar\":";
    out += list_size_bar ? L"true" : L"false";
    out += L",\n  \"list_tag_name_color\":";
    out += list_tag_name_color ? L"true" : L"false";
    out += L",\n  \"list_selection_outline\":";
    out += list_selection_outline ? L"true" : L"false";
    out += L",\n  \"vertical_tabs\":";
    out += vertical_tabs ? L"true" : L"false";
    out += L",\n  \"sidebar_collapsed\":";
    out += sidebar_collapsed ? L"true" : L"false";
    out += L",\n  \"folder_sort_mode\":";
    out += std::to_wstring(folder_sort_mode >= 0 && folder_sort_mode <= 2 ? folder_sort_mode : 0);
    out += L",\n  \"details_columns\":";
    out += std::to_wstring(ui::NormalizeDetailsColumns(details_columns));
    out += L",\n  \"startup_open\":";
    out += startup_open == 1 ? L"1" : L"0";
    out += L",\n  \"new_tab_open\":";
    out += new_tab_open == 1 ? L"1" : L"0";
    out += L",\n  \"close_window_with_last_tab\":";
    out += close_window_with_last_tab ? L"true" : L"false";
    out += L",\n  \"confirm_recycle_delete\":";
    out += confirm_recycle_delete ? L"true" : L"false";
    out += L",\n  \"start_in_tray\":";
    out += start_in_tray ? L"true" : L"false";
    out += L",\n  \"home_folder\":\"";
    out += escaped_home;
    out += L"\"";
    out += L",\n  \"text_render\":";
    out += std::to_wstring(text_render >= 0 && text_render <= 2 ? text_render : 0);
    out += L",\n  \"ui_font_scale\":";
    out += std::to_wstring(NormalizeUiFontScale(ui_font_scale));
    out += L",\n  \"show_hidden_files\":";
    out += show_hidden_files ? L"true" : L"false";
    out += L",\n  \"show_protected_os_files\":";
    out += show_protected_os_files ? L"true" : L"false";
    out += L",\n  \"search_pinyin\":";
    out += search_pinyin ? L"true" : L"false";
    out += L",\n  \"global_search_enabled\":";
    out += global_search_enabled ? L"true" : L"false";
    out += L",\n  \"global_search_modifiers\":" + std::to_wstring(global_search_modifiers);
    out += L",\n  \"global_search_key\":" + std::to_wstring(global_search_key);
    out += L",\n  \"blank_click_action\":";
    out += std::to_wstring(blank_click_action);
    // Kept for older builds, which only know on/off.
    out += L",\n  \"blank_click_go_back\":";
    out += blank_click_action != kBlankClickOff ? L"true" : L"false";
    out += L",\n  \"change_tracking_enabled\":";
    out += change_tracking_enabled ? L"true" : L"false";
    out += L",\n  \"change_tracking_days\":";
    out += std::to_wstring(change_tracking_days == 1 || change_tracking_days == 3 ? change_tracking_days : 7);
    out += L",\n  \"theme_mode\":";
    out += std::to_wstring(theme_mode);
    out += L",\n  \"language\":\"";
    out += escaped_language;
    out += L"\"";
    out += L",\n  \"window_effect\":\"";
    out += escaped_effect;
    out += L"\",\n  \"background_image\":\"";
    out += escaped_image;
    out += L"\",\n  \"row_height\":";
    out += std::to_wstring(row_height);
    out += L",\n  \"sidebar_width\":" + std::to_wstring(sidebar_width);
    out += L",\n  \"address_search_current\":" + std::to_wstring(address_search_current);
    out += L",\n  \"address_search_content\":" + std::to_wstring(address_search_content);
    out += L",\n  \"tray_icon_size\":";
    out += std::to_wstring(tray_icon_size);
    out += L",\n  \"show_hints\":";
    out += show_hints ? L"true" : L"false";
    out += L",\n  \"auto_check_updates\":";
    out += auto_check_updates ? L"true" : L"false";
    out += L",\n  \"tips_seen\":" + std::to_wstring(tips_seen);
    // Legacy three-level keys stay for older builds reading the same app.json.
    out += L",\n  \"wallpaper_look\":";
    out += std::to_wstring(wallpaper_look < 38 ? 0 : (wallpaper_look < 63 ? 1 : 2));
    out += L",\n  \"wallpaper_blur\":";
    out += std::to_wstring(wallpaper_blur <= 0 ? 0 : (wallpaper_blur <= 21 ? 1 : 2));
    out += L",\n  \"panel_transparency\":" + std::to_wstring(wallpaper_look);
    out += L",\n  \"wallpaper_blur_px\":" + std::to_wstring(wallpaper_blur);
    out += L",\n  \"accent_rgb\":\"";
    {
        std::wstring escaped_accent;
        pulse::json::Escape(accent_rgb, escaped_accent);
        out += escaped_accent;
    }
    out += L"\",\n  \"accent_follow_system\":" + std::to_wstring(accent_follow_system);
    out += L",\n  \"custom_tag_colors\":[";
    for (size_t i = 0; i < custom_tag_colors.size(); ++i) {
        wchar_t hex[8]{};
        swprintf_s(hex, L"%06X", custom_tag_colors[i] & 0x00FFFFFFu);
        if (i > 0) out += L",";
        out += L"\"";
        out += hex;
        out += L"\"";
    }
    out += L"],\n  \"duplicate_scan_scope\":";
    out += std::to_wstring(duplicate_scan_scope);
    out += L",\n  \"duplicate_scan_folder\":\"";
    {
        std::wstring escaped;
        pulse::json::Escape(duplicate_scan_folder, escaped);
        out += escaped;
    }
    out += L"\",\n  \"duplicate_scan_drive\":\"";
    {
        std::wstring escaped;
        pulse::json::Escape(duplicate_scan_drive, escaped);
        out += escaped;
    }
    out += L"\",\n  \"last_seen_version\":\"";
    {
        std::wstring escaped;
        pulse::json::Escape(last_seen_version, escaped);
        out += escaped;
    }
    out += L"\",\n  \"tray_dests\":\"";
    {
        std::wstring escaped;
        pulse::json::Escape(tray_dests, escaped);
        out += escaped;
    }
    out += L"\"";
    folder_views.AppendJson(out);
    folder_sorts.AppendJson(out);
    folder_groups.AppendJson(out);
    out += L"\n}\n";
    return out;
}

bool AppPrefs::FromJson(const std::wstring& json) {
    if (!pulse::json::ValidConfigObject(json)) return false;
    folder_views.ReadJson(json);
    folder_sorts.ReadJson(json);
    folder_groups.ReadJson(json);
    launch_on_startup = pulse::json::ExtractBool(json, L"launch_on_startup", false);
    keep_running_on_close = pulse::json::ExtractBool(json, L"keep_running_on_close", false);
    notify_icon_mode = pulse::json::ExtractInt(json, L"notify_icon_mode", 0);
    if (notify_icon_mode < 0 || notify_icon_mode > 2) notify_icon_mode = 0;
    open_folders_in_pulse = pulse::json::ExtractBool(json, L"open_folders_in_pulse", false);
    take_over_explorer_windows = pulse::json::ExtractBool(json, L"take_over_explorer_windows", false);
    integration_configured = json.find(L"\"integration_enabled\"") != std::wstring::npos;
    integration_enabled = pulse::json::ExtractBool(json, L"integration_enabled", false);
    integration_folders = pulse::json::ExtractBool(json, L"integration_folders", true);
    integration_win_e = pulse::json::ExtractBool(json, L"integration_win_e", true);
    integration_this_pc = pulse::json::ExtractBool(json, L"integration_this_pc", true);
    shell_tag_menu = pulse::json::ExtractBool(json, L"shell_tag_menu", false);
    verify_copies = pulse::json::ExtractBool(json, L"verify_copies", false);
    show_status_performance = pulse::json::ExtractBool(json, L"show_status_performance", false);
    show_pinned_tab_names = pulse::json::ExtractBool(json, L"show_pinned_tab_names", true);
    multi_instance_mode = pulse::json::ExtractBool(json, L"multi_instance_mode", false);
    list_smart_date = pulse::json::ExtractBool(json, L"list_smart_date", true);
    list_zebra_rows = pulse::json::ExtractBool(json, L"list_zebra_rows", true);
    list_size_bar = pulse::json::ExtractBool(json, L"list_size_bar", false);
    list_tag_name_color = pulse::json::ExtractBool(json, L"list_tag_name_color", false);
    list_selection_outline = pulse::json::ExtractBool(json, L"list_selection_outline", false);
    vertical_tabs = pulse::json::ExtractBool(json, L"vertical_tabs", false);
    sidebar_collapsed = pulse::json::ExtractBool(json, L"sidebar_collapsed", false);
    folder_sort_mode = pulse::json::ExtractInt(json, L"folder_sort_mode", 0);
    if (folder_sort_mode < 0 || folder_sort_mode > 2) folder_sort_mode = 0;
    details_columns = ui::NormalizeDetailsColumns(static_cast<uint32_t>(pulse::json::ExtractInt(
        json, L"details_columns", static_cast<int>(ui::kDetailsColumnsDefault))));
    startup_open = pulse::json::ExtractInt(json, L"startup_open", 0) == 1 ? 1 : 0;
    new_tab_open = pulse::json::ExtractInt(json, L"new_tab_open", 0) == 1 ? 1 : 0;
    close_window_with_last_tab = pulse::json::ExtractBool(json, L"close_window_with_last_tab", false);
    confirm_recycle_delete = pulse::json::ExtractBool(json, L"confirm_recycle_delete", false);
    start_in_tray = pulse::json::ExtractBool(json, L"start_in_tray", false);
    home_folder = pulse::json::ExtractString(json, L"home_folder");
    text_render = pulse::json::ExtractInt(json, L"text_render", 0);
    if (text_render < 0 || text_render > 2) text_render = 0;
    ui_font_scale = NormalizeUiFontScale(pulse::json::ExtractInt(json, L"ui_font_scale", 100));
    search_pinyin = pulse::json::ExtractBool(json, L"search_pinyin", true);
    global_search_enabled = pulse::json::ExtractBool(json, L"global_search_enabled", false);
    const int modifiers = pulse::json::ExtractInt(json, L"global_search_modifiers", 1);
    const int key = pulse::json::ExtractInt(json, L"global_search_key", 32);
    global_search_modifiers = modifiers > 0 && modifiers <= 15 ? static_cast<uint32_t>(modifiers) : 1;
    global_search_key = key > 0 && key <= 254 ? static_cast<uint32_t>(key) : 32;
    show_hidden_files = pulse::json::ExtractBool(json, L"show_hidden_files", false);
    show_protected_os_files = pulse::json::ExtractBool(json, L"show_protected_os_files", false);
    // Files from before blank_click_action carry only the on/off flag (= back).
    blank_click_action = NormalizeBlankClickAction(pulse::json::ExtractInt(json, L"blank_click_action",
        pulse::json::ExtractBool(json, L"blank_click_go_back", false) ? kBlankClickBack : kBlankClickOff));
    change_tracking_enabled = pulse::json::ExtractBool(json, L"change_tracking_enabled", false);
    change_tracking_days = pulse::json::ExtractInt(json, L"change_tracking_days", 7);
    if (change_tracking_days != 1 && change_tracking_days != 3 && change_tracking_days != 7)
        change_tracking_days = 7;
    theme_mode = pulse::json::ExtractInt(json, L"theme_mode", -1);
    if (theme_mode < -1 || theme_mode > 2) theme_mode = -1;
    language = pulse::json::ExtractString(json, L"language", L"system");
    if (language != L"system" && language != L"zh-CN" && language != L"zh-TW" &&
        language != L"en-US")
        language = L"system";
    window_effect = pulse::json::ExtractString(json, L"window_effect", L"mica-alt");
    if (window_effect == L"dwm-blur") window_effect = L"acrylic-material";
    else if (window_effect.empty()) window_effect = L"mica-alt";
    background_image = pulse::json::ExtractString(json, L"background_image");
    row_height = pulse::json::ExtractInt(json, L"row_height", 34);
    sidebar_width = pulse::json::ExtractInt(json, L"sidebar_width", 224);
    // The stored value is the user's intent; the window caps it while drawing.
    if (sidebar_width < static_cast<int>(ui::kSidebarMinWidthDip) ||
        sidebar_width > static_cast<int>(ui::kPanelWidthMaxDip)) sidebar_width = 224;
    address_search_current = pulse::json::ExtractInt(json, L"address_search_current", 0) != 0;
    address_search_content = pulse::json::ExtractInt(json, L"address_search_content", 0) != 0;
    if (row_height < 24 || row_height > 48) row_height = 34;
    tray_icon_size = pulse::json::ExtractInt(json, L"tray_icon_size", 48);
    show_hints = pulse::json::ExtractBool(json, L"show_hints", true);
    auto_check_updates = pulse::json::ExtractBool(json, L"auto_check_updates", true);
    const int seen = pulse::json::ExtractInt(json, L"tips_seen", 0);
    tips_seen = seen > 0 ? static_cast<uint32_t>(seen) : 0u;
    if (tray_icon_size < 32 || tray_icon_size > 64) tray_icon_size = 48;
    {
        // Continuous values; migrate the former 0/1/2 levels when absent.
        static constexpr int kLookLevels[] = {25, 50, 75};
        static constexpr int kBlurLevels[] = {0, 14, 28};
        int legacy = pulse::json::ExtractInt(json, L"wallpaper_look", 1);
        if (legacy < 0 || legacy > 2) legacy = 1;
        wallpaper_look = pulse::json::ExtractInt(json, L"panel_transparency", kLookLevels[legacy]);
        if (wallpaper_look < 0 || wallpaper_look > 90) wallpaper_look = kLookLevels[legacy];
        legacy = pulse::json::ExtractInt(json, L"wallpaper_blur", 1);
        if (legacy < 0 || legacy > 2) legacy = 1;
        wallpaper_blur = pulse::json::ExtractInt(json, L"wallpaper_blur_px", kBlurLevels[legacy]);
        if (wallpaper_blur < 0 || wallpaper_blur > 40) wallpaper_blur = kBlurLevels[legacy];
    }
    accent_rgb = pulse::json::ExtractString(json, L"accent_rgb");
    accent_follow_system = pulse::json::ExtractInt(json, L"accent_follow_system", 0) != 0;
    uint32_t accent_parsed = 0;
    if (!accent_rgb.empty() && ParseAccentRgb(accent_rgb, accent_parsed)) {
        wchar_t hex[8]{};
        swprintf_s(hex, L"%06X", accent_parsed);
        accent_rgb = hex;
    } else {
        accent_rgb.clear();
    }
    custom_tag_colors.clear();
    for (const std::wstring& entry :
         pulse::json::ExtractStringArray(json, L"custom_tag_colors")) {
        const wchar_t* text = entry.c_str();
        if (*text == L'#') ++text;
        wchar_t* end = nullptr;
        const unsigned long v = wcstoul(text, &end, 16);
        if (end && *end == L'\0' && v <= 0xFFFFFFul && wcslen(text) == 6)
            custom_tag_colors.push_back(static_cast<uint32_t>(v));
    }
    duplicate_scan_scope = pulse::json::ExtractInt(json, L"duplicate_scan_scope", 0);
    if (duplicate_scan_scope < 0 || duplicate_scan_scope > 2) duplicate_scan_scope = 0;
    duplicate_scan_folder = pulse::json::ExtractString(json, L"duplicate_scan_folder");
    duplicate_scan_drive = pulse::json::ExtractString(json, L"duplicate_scan_drive");
    last_seen_version = pulse::json::ExtractString(json, L"last_seen_version");
    tray_dests = pulse::json::ExtractString(json, L"tray_dests");
    return true;
}

bool AppPrefs::StoreBackgroundImage(const std::wstring& source_path) {
    if (source_path.empty()) return false;
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty() || load_failed) return false;
    const wchar_t* ext = PathFindExtensionW(source_path.c_str());
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempFileNameW(dir.c_str(), L"pwp", 0, temporary)) return false;
    const std::wstring staged = temporary;
    const std::wstring dest = staged + ((ext && ext[0]) ? ext : L".img");
    if (!CopyFileW(source_path.c_str(), staged.c_str(), FALSE) ||
        !MoveFileExW(staged.c_str(), dest.c_str(), MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(staged.c_str());
        return false;
    }
    const std::wstring previous = background_image;
    background_image = dest;
    if (!Save()) {
        background_image = previous;
        DeleteFileW(dest.c_str());
        return false;
    }
    if (IsStoredWallpaper(previous, dir)) DeleteFileW(previous.c_str());
    return true;
}

void AppPrefs::ClearBackgroundImage() {
    const std::wstring dir = GetPulseDataDir();
    if (IsStoredWallpaper(background_image, dir)) DeleteFileW(background_image.c_str());
    background_image.clear();
}

namespace {
// The HKCU Run command for Pulse, or empty when there is none.
std::wstring ReadRunCommand() {
    HKEY key = nullptr;
    const std::wstring run_key = RegPath(kRunKey);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, run_key.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return {};
    wchar_t value[1024] = {};
    DWORD bytes = sizeof(value) - sizeof(wchar_t);
    DWORD type = 0;
    const LONG st = RegQueryValueExW(key, kRunValue, nullptr, &type,
                                     reinterpret_cast<LPBYTE>(value), &bytes);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
    return value;
}
} // namespace

bool AppPrefs::ReadLaunchOnStartup() const {
    return !ReadRunCommand().empty();
}

bool AppPrefs::ApplyLaunchOnStartup(bool on) {
    launch_on_startup = on;
    if (!persist) return true;
    HKEY key = nullptr;
    const std::wstring run_key = RegPath(kRunKey);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, run_key.c_str(), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    LONG st = ERROR_SUCCESS;
    if (on) {
        const std::wstring exe = ExePath();
        if (exe.empty()) { RegCloseKey(key); return false; }
        // --startup tells a sign-in launch apart (start_in_tray).
        const std::wstring cmd = StartupCommandLine(exe);
        st = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(key, kRunValue);
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

std::wstring FolderOpenCommandLine(const std::wstring& exe) {
    if (exe.empty()) return {};
    return L"\"" + exe + L"\" \"%1\"";
}

namespace {

// Executable token of a shell command line ("C:\dir with space\pulse.exe" "%1").
std::wstring CommandToken(const std::wstring& command) {
    size_t i = 0;
    while (i < command.size() && iswspace(command[i])) ++i;
    if (i < command.size() && command[i] == L'"') {
        ++i;
        const size_t start = i;
        while (i < command.size() && command[i] != L'"') ++i;
        return command.substr(start, i - start);
    }
    const size_t start = i;
    while (i < command.size() && !iswspace(command[i])) ++i;
    return command.substr(start, i - start);
}

} // namespace

bool FolderOpenCommandIsOurs(const std::wstring& command, const std::wstring& exe) {
    if (command.empty() || exe.empty()) return false;
    const std::wstring token = CommandToken(command);
    return !token.empty() &&
           CompareStringOrdinal(token.c_str(), -1, exe.c_str(), -1, TRUE) == CSTR_EQUAL;
}

namespace {

constexpr const wchar_t* kFolderOpenClasses[] = { L"Directory", L"Drive" };

// "Ours" in the loose sense: the first token names pulse.exe, wherever that copy
// lives. A value left behind by an install that moved (or by an older version) is
// still ours and gets repointed; a verb another program owns is left alone.
bool CommandIsPulse(const std::wstring& command) {
    const std::wstring token = CommandToken(command);
    if (token.empty()) return false;
    const size_t separator = token.find_last_of(L"\\/");
    const std::wstring name = separator == std::wstring::npos
        ? token : token.substr(separator + 1);
    return _wcsicmp(name.c_str(), L"pulse.exe") == 0;
}

std::wstring FolderOpenKey(const wchar_t* cls) {
    return RegPath(std::wstring(L"Software\\Classes\\") + cls + L"\\shell\\open");
}

std::wstring FolderShellKey(const wchar_t* cls) {
    return RegPath(std::wstring(L"Software\\Classes\\") + cls + L"\\shell");
}

// Reads one string value; |name| null means the key's default value.
std::wstring ReadRegString(const std::wstring& key, const wchar_t* name = nullptr) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS)
        return {};
    wchar_t value[1024] = {};
    DWORD bytes = sizeof(value);
    DWORD type = 0;
    const LONG st = RegQueryValueExW(h, name, nullptr, &type,
                                     reinterpret_cast<LPBYTE>(value), &bytes);
    RegCloseKey(h);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
    return value;
}

bool WriteFolderOpenClass(const wchar_t* cls, const std::wstring& exe) {
    const std::wstring open = FolderOpenKey(cls);
    const std::wstring command = open + L"\\command";
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, command.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    const std::wstring line = FolderOpenCommandLine(exe);
    const LONG st = RegSetValueExW(h, nullptr, 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(line.c_str()),
                                   static_cast<DWORD>((line.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    if (st != ERROR_SUCCESS) return false;
    h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, open.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    const wchar_t empty[] = L"";
    const LONG de = RegSetValueExW(h, L"DelegateExecute", 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(empty), sizeof(wchar_t));
    RegCloseKey(h);
    if (de != ERROR_SUCCESS) return false;

    // HKLM Directory/Drive shell default is "none", so double-click never uses
    // the open verb and falls through to Folder → Explorer. Point HKCU at open.
    h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, FolderShellKey(cls).c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    const wchar_t open_verb[] = L"open";
    const LONG def = RegSetValueExW(h, nullptr, 0, REG_SZ,
                                    reinterpret_cast<const BYTE*>(open_verb),
                                    sizeof(open_verb));
    RegCloseKey(h);
    return def == ERROR_SUCCESS;
}

// Ownership is judged loosely here (any command naming a pulse.exe, whatever the
// path), unlike FolderOpenClassIsConfigured: a value left by an install that moved
// still hijacks double-click with an executable that is not there any more, and a
// user who turns the setting off has to be able to turn it off. A command another
// program owns is refused, which is the only line this must not cross.
bool ClearFolderOpenClass(const wchar_t* cls) {
    const std::wstring command = ReadRegString(FolderOpenKey(cls) + L"\\command");
    if (!command.empty() && !CommandIsPulse(command)) return true;
    SHDeleteKeyW(HKEY_CURRENT_USER, FolderOpenKey(cls).c_str());
    const std::wstring shell = FolderShellKey(cls);
    if (_wcsicmp(ReadRegString(shell).c_str(), L"open") == 0) {
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, shell.c_str(), 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
            RegDeleteValueW(h, nullptr);
            RegCloseKey(h);
        }
    }
    return true;
}

void NotifyAssocChanged() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

bool FolderOpenClassIsConfigured(const wchar_t* cls, const std::wstring& exe) {
    const std::wstring command = ReadRegString(FolderOpenKey(cls) + L"\\command");
    if (!FolderOpenCommandIsOurs(command, exe)) return false;
    // Keep the repair path for older installs that wrote the command but left
    // the class default verb as "none" (the HKLM default).
    return _wcsicmp(ReadRegString(FolderShellKey(cls)).c_str(), L"open") == 0;
}

bool FolderOpenClassNeedsClear(const wchar_t* cls) {
    const std::wstring command = ReadRegString(FolderOpenKey(cls) + L"\\command");
    // Loose, like the cleanup itself: residue from an install that moved is ours and
    // has to go, otherwise "off" never clears it and it keeps pointing at a path that
    // no longer exists.
    if (!command.empty()) return CommandIsPulse(command);
    // A previous cleanup or an interrupted registration can leave only the
    // HKCU shell default behind; ClearFolderOpenClass removes that residue.
    return _wcsicmp(ReadRegString(FolderShellKey(cls)).c_str(), L"open") == 0;
}

} // namespace

bool AppPrefs::ReadFolderOpen() const {
    return ReadShellIntegration(ShellIntegrationKind::Folders, ExePath());
}

bool AppPrefs::ReadIntegrationResidual() const {
    const std::wstring exe = ExePath();
    return HasShellIntegrationOwnership(ShellIntegrationKind::Folders, exe) ||
           HasShellIntegrationOwnership(ShellIntegrationKind::WinE, exe) ||
           HasShellIntegrationOwnership(ShellIntegrationKind::ThisPc, exe);
}

bool AppPrefs::ReadIntegrationIncomplete() const {
    const std::wstring exe = ExePath();
    for (const auto kind : {ShellIntegrationKind::Folders, ShellIntegrationKind::WinE, ShellIntegrationKind::ThisPc})
        if (HasShellIntegrationOwnership(kind, exe) && !ReadShellIntegration(kind, exe)) return true;
    return false;
}

bool AppPrefs::ApplyFolderOpen(bool on) {
    if (!persist) { open_folders_in_pulse = on; return true; }
    const bool ok = ApplyShellIntegration(ShellIntegrationKind::Folders, ExePath(), on);
    open_folders_in_pulse = ReadFolderOpen();
    integration_residual = ReadIntegrationResidual();
    integration_incomplete = ReadIntegrationIncomplete();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok && open_folders_in_pulse == on;
}

bool AppPrefs::ReadWinE() const {
    return ReadShellIntegration(ShellIntegrationKind::WinE, ExePath());
}

bool AppPrefs::ApplyWinE(bool on) {
    if (!persist) { take_over_win_e = on; return true; }
    const bool ok = ApplyShellIntegration(ShellIntegrationKind::WinE, ExePath(), on);
    take_over_win_e = ReadWinE();
    integration_residual = ReadIntegrationResidual();
    integration_incomplete = ReadIntegrationIncomplete();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok && take_over_win_e == on;
}
int MenuRowHeightDip(int list_row_height) noexcept {
    return std::clamp(list_row_height + 2, 28, 40);
}
int NormalizeUiFontScale(int percent) noexcept {
    return percent == 90 || percent == 112 || percent == 125 ? percent : 100;
}
int EffectiveRowHeightDip(int list_row_height, int ui_font_scale) noexcept {
    const int scale = NormalizeUiFontScale(ui_font_scale);
    // 28 DIPs is the compact row that still fits 100% text; grow it with the font.
    const int minimum = scale > 100 ? (28 * scale + 99) / 100 : 0;
    return std::max(list_row_height, minimum);
}

bool ParseAccentRgb(const std::wstring& text, uint32_t& rgb) noexcept {
    const wchar_t* p = text.c_str();
    if (!p || !*p) return false;
    if (*p == L'#') ++p;
    if (wcslen(p) != 6) return false;
    for (int i = 0; i < 6; ++i) {
        if (!iswxdigit(p[i])) return false;
    }
    wchar_t* end = nullptr;
    const unsigned long v = wcstoul(p, &end, 16);
    if (!end || *end != L'\0' || v > 0xFFFFFFul) return false;
    rgb = static_cast<uint32_t>(v);
    return true;
}

void AppPrefs::MigrateIntegration() {
    if (integration_configured) return;
    // Legacy registrations can still own an entry without satisfying the new
    // complete-value checks. Preserve that selected scope for repair.
    const std::wstring exe = integration_residual ? ExePath() : std::wstring();
    const bool folders = open_folders_in_pulse || (integration_residual &&
        HasShellIntegrationOwnership(ShellIntegrationKind::Folders, exe));
    const bool win_e = take_over_win_e || (integration_residual &&
        HasShellIntegrationOwnership(ShellIntegrationKind::WinE, exe));
    const bool this_pc = take_over_this_pc || (integration_residual &&
        HasShellIntegrationOwnership(ShellIntegrationKind::ThisPc, exe));
    const bool registered = folders || win_e || this_pc;
    integration_enabled = registered || take_over_explorer_windows;
    if (registered || take_over_explorer_windows) {
        integration_folders = folders;
        integration_win_e = win_e;
        integration_this_pc = this_pc;
    }
    integration_configured = true;
}

// The main file first, its backup when the main one is missing, unreadable, or
// too incomplete to trust. A backed-up file also heals a truncated one: the next
// Save() writes the merged state back over the damaged main file.
bool AppPrefs::ReadDiskState(AppPrefsValues& values, bool& main_exists,
                             bool* used_backup) const {
    main_exists = false;
    if (used_backup) *used_backup = false;
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    const std::wstring main_path = dir + L"\\app.json";
    const std::wstring backup_path = main_path + L".bak";
    main_exists = GetFileAttributesW(main_path.c_str()) != INVALID_FILE_ATTRIBUTES;

    std::wstring main_json;
    std::wstring backup_json;
    const bool main_read = ReadUtf8File(main_path, main_json) && !main_json.empty();
    const bool backup_read = ReadUtf8File(backup_path, backup_json) && !backup_json.empty();
    const int main_keys = main_read ? CountStoredKeys(main_json) : 0;
    const int backup_keys = backup_read ? CountStoredKeys(backup_json) : 0;

    // A complete main file wins outright; anything else is compared key by key, so a
    // half-written file never hides the copy that still has the settings in it.
    const std::wstring* source = nullptr;
    if (main_read && main_keys >= kMinStoredKeys) source = &main_json;
    else if (backup_read && backup_keys >= kMinStoredKeys) source = &backup_json;
    else if (main_read && backup_read)
        source = main_keys >= backup_keys ? &main_json : &backup_json;
    else if (main_read) source = &main_json;
    else if (backup_read) source = &backup_json;
    if (!source) return false;
    if (used_backup) *used_backup = source == &backup_json;

    AppPrefs parsed;
    parsed.persist = false;
    if (!parsed.FromJson(*source)) return false;
    values = parsed;
    return true;
}

// A field this process left alone since the last Load()/Save() takes the value the
// file holds now; a field it changed keeps the local value. Notes:
//   - "left alone" is measured against disk_state_, not against the file: a field
//     the disk changed under us must not be mistaken for a local change.
//   - a field missing from this list keeps the local value, so forgetting one is
//     never a lost setting and never a new failure mode.
AppPrefsValues AppPrefs::MergedWithDisk(const AppPrefsValues& disk) const {
    const AppPrefsValues& mine = *this;
    const AppPrefsValues& baseline = disk_state_;
    AppPrefsValues merged = mine;
    if (mine.launch_on_startup == baseline.launch_on_startup)
        merged.launch_on_startup = disk.launch_on_startup;
    if (mine.keep_running_on_close == baseline.keep_running_on_close)
        merged.keep_running_on_close = disk.keep_running_on_close;
    if (mine.open_folders_in_pulse == baseline.open_folders_in_pulse)
        merged.open_folders_in_pulse = disk.open_folders_in_pulse;
    if (mine.verify_copies == baseline.verify_copies)
        merged.verify_copies = disk.verify_copies;
    if (mine.show_status_performance == baseline.show_status_performance)
        merged.show_status_performance = disk.show_status_performance;
    if (mine.show_pinned_tab_names == baseline.show_pinned_tab_names)
        merged.show_pinned_tab_names = disk.show_pinned_tab_names;
    if (mine.multi_instance_mode == baseline.multi_instance_mode)
        merged.multi_instance_mode = disk.multi_instance_mode;
    if (mine.search_pinyin == baseline.search_pinyin)
        merged.search_pinyin = disk.search_pinyin;
    if (mine.global_search_enabled == baseline.global_search_enabled)
        merged.global_search_enabled = disk.global_search_enabled;
    if (mine.global_search_modifiers == baseline.global_search_modifiers)
        merged.global_search_modifiers = disk.global_search_modifiers;
    if (mine.global_search_key == baseline.global_search_key)
        merged.global_search_key = disk.global_search_key;
    if (mine.show_hidden_files == baseline.show_hidden_files)
        merged.show_hidden_files = disk.show_hidden_files;
    if (mine.show_protected_os_files == baseline.show_protected_os_files)
        merged.show_protected_os_files = disk.show_protected_os_files;
    if (mine.blank_click_action == baseline.blank_click_action)
        merged.blank_click_action = disk.blank_click_action;
    if (mine.change_tracking_enabled == baseline.change_tracking_enabled)
        merged.change_tracking_enabled = disk.change_tracking_enabled;
    if (mine.change_tracking_days == baseline.change_tracking_days)
        merged.change_tracking_days = disk.change_tracking_days;
    if (mine.theme_mode == baseline.theme_mode)
        merged.theme_mode = disk.theme_mode;
    if (mine.language == baseline.language)
        merged.language = disk.language;
    if (mine.window_effect == baseline.window_effect)
        merged.window_effect = disk.window_effect;
    if (mine.background_image == baseline.background_image)
        merged.background_image = disk.background_image;
    if (mine.row_height == baseline.row_height)
        merged.row_height = disk.row_height;
    if (mine.sidebar_width == baseline.sidebar_width)
        merged.sidebar_width = disk.sidebar_width;
    if (mine.address_search_current == baseline.address_search_current)
        merged.address_search_current = disk.address_search_current;
    if (mine.address_search_content == baseline.address_search_content)
        merged.address_search_content = disk.address_search_content;
    if (mine.tray_icon_size == baseline.tray_icon_size)
        merged.tray_icon_size = disk.tray_icon_size;
    if (mine.accent_rgb == baseline.accent_rgb)
        merged.accent_rgb = disk.accent_rgb;
    if (mine.custom_tag_colors == baseline.custom_tag_colors)
        merged.custom_tag_colors = disk.custom_tag_colors;
    if (mine.duplicate_scan_scope == baseline.duplicate_scan_scope)
        merged.duplicate_scan_scope = disk.duplicate_scan_scope;
    if (mine.duplicate_scan_folder == baseline.duplicate_scan_folder)
        merged.duplicate_scan_folder = disk.duplicate_scan_folder;
    if (mine.duplicate_scan_drive == baseline.duplicate_scan_drive)
        merged.duplicate_scan_drive = disk.duplicate_scan_drive;
    return merged;
}

namespace {

// The data directory was redirected, which only a self-test does: that run must not
// touch the machine's registry (see Load()). A registry sandbox lifts that ban for
// the copies it redirects, because those live under a key the test owns.
bool DataDirRedirected() {
#ifdef PULSE_WITH_SELFTEST
    wchar_t probe[2]{};
    return GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", probe, ARRAYSIZE(probe)) > 0;
#else
    return false;
#endif
}

} // namespace

// The file holds the user's intent; the registry is a projection of it that has to
// be kept in step. An uninstaller that deleted the Run key, or an install that moved
// to another folder, leaves the projection missing or stale, and reading that as
// "off" is what used to freeze "off" into the file on the next save. A verb that
// belongs to another program is never taken over.
bool AppPrefs::ReconcileRegistryWithFile() {
    bool adopted = false;

    const std::wstring run_value = ReadRegString(RegPath(kRunKey), kRunValue);
    const bool run_is_ours = CommandIsPulse(run_value);
    if (launch_on_startup) {
        // Missing, or naming a Pulse that is no longer this executable.
        if (run_value.empty() || (run_is_ours && run_value != LaunchOnStartupCommand()))
            ApplyLaunchOnStartup(true);
    } else if (run_is_ours) {
        // The Run key has no "present but off" state: a value that is ours means the
        // user wanted auto-start on and the file lost it (restored profile, another
        // install copy). Adopt it instead of switching the setting off silently.
        launch_on_startup = true;
        adopted = true;
    }

    const std::wstring exe = ExePath();
    bool associations_changed = false;
    for (const wchar_t* cls : kFolderOpenClasses) {
        const std::wstring command = ReadRegString(FolderOpenKey(cls) + L"\\command");
        const bool ours = CommandIsPulse(command);
        const bool another_owner = !ours && !command.empty();
        if (open_folders_in_pulse) {
            // Strict here: "configured" means this executable answers the verb right
            // now, so a value naming an older path is rewritten rather than kept.
            if (!FolderOpenClassIsConfigured(cls, exe) && !another_owner) {
                WriteFolderOpenClass(cls, exe);
                associations_changed = true;
            }
        } else if (FolderOpenClassIsConfigured(cls, exe)) {
            // Reverse protection, same as the Run key: the association works, so the
            // file lost the "on" and must not turn it off behind the user's back.
            open_folders_in_pulse = true;
            adopted = true;
        } else if (FolderOpenClassNeedsClear(cls)) {
            // Ours but unusable: a command without the open verb, a path left by an
            // install that moved, or only the verb an interrupted cleanup left behind.
            // ClearFolderOpenClass refuses anything another program owns, so this never
            // reaches past our own residue.
            ClearFolderOpenClass(cls);
            associations_changed = true;
        }
    }
    if (associations_changed) NotifyAssocChanged();
    return adopted;
}

bool AppPrefs::Load() {
    // A redirected data directory means the run must not touch the user's real state:
    // these toggles live in HKCU, and a repaired verb would point the machine at
    // whatever executable is asking. A sandboxed copy of those keys is fair game.
    const std::wstring sandbox = RegistrySandboxPrefix();
    const bool allow_registry = !sandbox.empty() || !DataDirRedirected();
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) {
        if (allow_registry) {
            launch_on_startup = ReadLaunchOnStartup();
            open_folders_in_pulse = ReadFolderOpen();
            take_over_win_e = ReadWinE();
            take_over_this_pc = ReadThisPcOpen(ExePath());
            integration_residual = ReadIntegrationResidual();
            integration_incomplete = ReadIntegrationIncomplete();
            MigrateIntegration();
        }
        // Nothing was read, but these are the values this process runs with: the
        // next Save() must treat them as the file's state, not as a change.
        disk_state_ = *this;
        loaded_from_file_ = false;
        return false;
    }
    AppPrefsValues disk;
    bool main_exists = false;
    loaded_from_file_ = ReadDiskState(disk, main_exists);
    // main's "the file was there" flag: same fact our read reports, for the toast
    // that explains what changed after an update.
    had_file = main_exists;
    // A file that is there but cannot be read or parsed must not be overwritten
    // with defaults later.
    load_failed = !loaded_from_file_ && main_exists;
    if (loaded_from_file_) static_cast<AppPrefsValues&>(*this) = disk;
    if (persist && allow_registry) {
        if (loaded_from_file_) {
            // Baseline for the merge below and for the write the reconciliation may
            // ask for: the file's own values, before anything is adopted back.
            disk_state_ = *this;
            if (ReconcileRegistryWithFile()) Save();
        } else {
            // First migration: no usable file, so the machine's current state is all
            // there is to inherit.
            launch_on_startup = ReadLaunchOnStartup();
            open_folders_in_pulse = ReadFolderOpen();
        }
        take_over_win_e = ReadWinE();
        // Older builds registered the Run command without --startup.
        if (launch_on_startup && StartupCommandNeedsRepair(ReadRunCommand(), ExePath()))
            ApplyLaunchOnStartup(true);
        take_over_this_pc = ReadThisPcOpen(ExePath());
        integration_residual = ReadIntegrationResidual();
        integration_incomplete = ReadIntegrationIncomplete();
        MigrateIntegration();
    }
    // The state to merge against is what this process runs with, not what the file
    // happened to hold: a repair above must never read as a local change later.
    disk_state_ = *this;
    return true;
}

bool AppPrefs::Save() const {
    if (!persist) return true;
    if (load_failed) return false;
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    const std::wstring path = dir + L"\\app.json";

    AppPrefsValues disk;
    bool main_exists = false;
    bool used_backup = false;
    const bool disk_known = ReadDiskState(disk, main_exists, &used_backup);

    bool quarantined = false;
    if (!disk_known && main_exists) {
        // The file is there but neither it nor its backup can be read: writing over
        // it would destroy the only copy of whatever it holds. Keep those bytes as
        // app.json.bad and start over. When even the rename fails the file is held
        // open by another process, and leaving it untouched is the safe outcome.
        if (!QuarantineUnreadableFile(path)) return false;
        quarantined = true;
    }

    // Nothing readable in either file: every local value is written as it stands.
    AppPrefs out = *this;
    if (disk_known) static_cast<AppPrefsValues&>(out) = MergedWithDisk(disk);

    // Keep the previous file one step back: a later Load() falls back to it when the
    // main file is unreadable or truncated. The file that just became the .bad
    // evidence is gone already, and a main file we could not trust must not replace
    // the backup that just saved the settings. The policy has a single copy in
    // utf8_file.h (KeepPreviousFileCopy), shared with context_menu.json.
    if (!quarantined && main_exists && !used_backup) KeepPreviousFileCopy(path);

    if (!WriteUtf8FileAtomic(path, out.ToJson())) return false;
    // The next merge compares against what this process holds, not against what was
    // just written: a field adopted from the file must not look like a local change.
    disk_state_ = *this;
    return true;
}

} // namespace pulse::app

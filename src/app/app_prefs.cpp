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
#include "../common/runtime_log.h"
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

std::wstring ExePath() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return n ? std::wstring(path, n) : L"";
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
    list_smart_date = true;
    list_zebra_rows = true;
    list_size_bar = false;
    list_selection_outline = false;
    list_thumbnail_badges = true;
    folder_sort_mode = 0;
    details_columns = ui::kDetailsColumnsDefault;
    startup_open = 0;
    new_tab_open = 0;
    close_window_with_last_tab = false;
    close_tab_on_double_click = false;
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
    out += L",\n  \"list_thumbnail_badges\":";
    out += list_thumbnail_badges ? L"true" : L"false";
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
    out += L",\n  \"close_tab_on_double_click\":";
    out += close_tab_on_double_click ? L"true" : L"false";
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
    list_smart_date = pulse::json::ExtractBool(json, L"list_smart_date", true);
    list_zebra_rows = pulse::json::ExtractBool(json, L"list_zebra_rows", true);
    list_size_bar = pulse::json::ExtractBool(json, L"list_size_bar", false);
    list_tag_name_color = pulse::json::ExtractBool(json, L"list_tag_name_color", false);
    list_selection_outline = pulse::json::ExtractBool(json, L"list_selection_outline", false);
    list_thumbnail_badges = pulse::json::ExtractBool(json, L"list_thumbnail_badges", true);
    vertical_tabs = pulse::json::ExtractBool(json, L"vertical_tabs", false);
    sidebar_collapsed = pulse::json::ExtractBool(json, L"sidebar_collapsed", false);
    folder_sort_mode = pulse::json::ExtractInt(json, L"folder_sort_mode", 0);
    if (folder_sort_mode < 0 || folder_sort_mode > 2) folder_sort_mode = 0;
    details_columns = ui::NormalizeDetailsColumns(static_cast<uint32_t>(pulse::json::ExtractInt(
        json, L"details_columns", static_cast<int>(ui::kDetailsColumnsDefault))));
    startup_open = pulse::json::ExtractInt(json, L"startup_open", 0) == 1 ? 1 : 0;
    new_tab_open = pulse::json::ExtractInt(json, L"new_tab_open", 0) == 1 ? 1 : 0;
    close_window_with_last_tab = pulse::json::ExtractBool(json, L"close_window_with_last_tab", false);
    close_tab_on_double_click = pulse::json::ExtractBool(json, L"close_tab_on_double_click", false);
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
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
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
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
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

bool FolderOpenCommandIsOurs(const std::wstring& command, const std::wstring& exe) {
    return ShellCommandTargetsExecutable(command, exe);
}

bool AppPrefs::ReadFolderOpen() const {
    return ReadShellIntegration(ShellIntegrationKind::Folders, ExePath());
}

bool AppPrefs::ReadIntegrationResidual() const {
    const std::wstring exe = ExePath();
    return HasLegacyShellIntegrationResidue() ||
           HasShellIntegrationOwnership(ShellIntegrationKind::Folders, exe) ||
           HasShellIntegrationOwnership(ShellIntegrationKind::WinE, exe) ||
           HasShellIntegrationOwnership(ShellIntegrationKind::ThisPc, exe);
}

bool AppPrefs::ReadIntegrationIncomplete() const {
    if (HasLegacyShellIntegrationResidue()) return true;
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

bool AppPrefs::Load() {
    if (persist && HasLegacyShellIntegrationResidue()) {
        const bool repaired = RepairLegacyShellIntegrationResidue();
        pulse::diagnostics::runtime::Event("shell_integration_legacy_repair", {{"success", repaired ? 1u : 0u}});
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) {
        launch_on_startup = ReadLaunchOnStartup();
        open_folders_in_pulse = ReadFolderOpen();
        take_over_win_e = ReadWinE();
        take_over_this_pc = ReadThisPcOpen(ExePath());
        integration_residual = ReadIntegrationResidual();
        integration_incomplete = ReadIntegrationIncomplete();
        MigrateIntegration();
        return false;
    }
    std::wstring json;
    const std::wstring file = dir + L"\\app.json";
    const DWORD attributes = GetFileAttributesW(file.c_str());
    const DWORD attribute_error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    const bool missing = attributes == INVALID_FILE_ATTRIBUTES &&
        (attribute_error == ERROR_FILE_NOT_FOUND || attribute_error == ERROR_PATH_NOT_FOUND);
    if (!missing && (!ReadUtf8File(file, json) || !FromJson(json))) {
        load_failed = true;
        return false;
    }
    load_failed = false;
    had_file = !missing;
    launch_on_startup = ReadLaunchOnStartup();
    open_folders_in_pulse = ReadFolderOpen();
    take_over_win_e = ReadWinE();
    // Older builds registered the Run command without --startup.
    if (persist && launch_on_startup && StartupCommandNeedsRepair(ReadRunCommand(), ExePath()))
        ApplyLaunchOnStartup(true);
    take_over_this_pc = ReadThisPcOpen(ExePath());
    integration_residual = ReadIntegrationResidual();
    integration_incomplete = ReadIntegrationIncomplete();
    MigrateIntegration();
    return true;
}

bool AppPrefs::Save() const {
    if (!persist) return true;
    if (load_failed) return false;
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    return WriteUtf8FileAtomic(dir + L"\\app.json", ToJson());
}

} // namespace pulse::app

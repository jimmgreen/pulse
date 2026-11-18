#include "picker_appearance.h"
#include "app_prefs.h"
#include "blank_pane_click.h"
#include "../ui/folder_picker_dialog.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <shlobj.h>
#include <algorithm>
#include <vector>

namespace pulse::app {
namespace {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct CoMemory { PWSTR value = nullptr; ~CoMemory() { CoTaskMemFree(value); } };
int FontScale(int percent) noexcept { return percent == 90 || percent == 112 || percent == 125 ? percent : 100; }
int Integer(const std::wstring& json, const std::wstring& key, int fallback) {
    size_t pos = pulse::json::ValuePosition(json, key);
    if (pos == std::wstring::npos || pos == json.size()) return fallback;
    const bool negative = json[pos] == L'-';
    if (negative) ++pos;
    if (pos == json.size() || json[pos] < L'0' || json[pos] > L'9') return fallback;
    uint32_t value = 0;
    const uint32_t limit = negative ? 2147483648u : 2147483647u;
    while (pos < json.size() && json[pos] >= L'0' && json[pos] <= L'9') {
        const auto digit = static_cast<uint32_t>(json[pos++] - L'0');
        if (value > (limit - digit) / 10) return fallback;
        value = value * 10 + digit;
    }
    if (pos < json.size() && json[pos] != L',' && json[pos] != L'}' &&
        json[pos] != L' ' && json[pos] != L'\t' && json[pos] != L'\r' && json[pos] != L'\n') return fallback;
    return static_cast<int>(negative ? -static_cast<int64_t>(value) : static_cast<int64_t>(value));
}
std::optional<uint32_t> Accent(std::wstring_view text) noexcept {
    if (text.starts_with(L'#')) text.remove_prefix(1);
    if (text.size() != 6) return {};
    uint32_t value = 0;
    for (const wchar_t c : text) {
        const int digit = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 :
            c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;
        if (digit < 0) return {};
        value = (value << 4) | static_cast<uint32_t>(digit);
    }
    return value;
}
void Normalize(PickerAppearanceSnapshot& value) {
    if (value.theme_mode < -1 || value.theme_mode > 2) value.theme_mode = -1;
    if (value.language != L"system" && value.language != L"en-US" && value.language != L"zh-CN" && value.language != L"zh-TW")
        value.language = L"system";
    value.ui_font_scale = FontScale(value.ui_font_scale);
    if (value.text_render < 0 || value.text_render > 2) value.text_render = 0;
    if (value.row_height < 24 || value.row_height > 48) value.row_height = 34;
    value.details_columns = ui::NormalizeDetailsColumns(value.details_columns);
    value.blank_click_action = NormalizeBlankClickAction(value.blank_click_action);
    value.preview_trigger = NormalizePreviewTrigger(value.preview_trigger);
    value.hover_preview_delay_ms = NormalizeHoverPreviewDelay(value.hover_preview_delay_ms);
    value.hover_card_size = NormalizeHoverCardSize(value.hover_card_size);
}
PickerAppearanceReadResult Failure(DWORD error) {
    PickerAppearanceReadResult result;
    result.error = error ? error : ERROR_GEN_FAILURE;
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) result.status = PickerAppearanceReadStatus::Missing;
    return result;
}
}
PickerAppearanceSnapshot PickerAppearanceFromPrefs(const AppPrefs& prefs) {
    PickerAppearanceSnapshot value;
    value.theme_mode = prefs.theme_mode;
    value.accent_rgb = Accent(prefs.accent_rgb);
    value.accent_follow_system = prefs.accent_follow_system;
    value.language = prefs.language;
    value.ui_font_scale = prefs.ui_font_scale;
    value.text_render = prefs.text_render;
    value.row_height = prefs.row_height;
    value.selection_outline = prefs.list_selection_outline;
    value.smart_date = prefs.list_smart_date;
    value.zebra_rows = prefs.list_zebra_rows;
    value.size_bar = prefs.list_size_bar;
    value.thumbnail_badges = prefs.list_thumbnail_badges;
    value.details_columns = prefs.details_columns;
    value.blank_click_action = prefs.blank_click_action;
    value.preview_trigger = prefs.preview_trigger;
    value.hover_preview_delay_ms = prefs.hover_preview_delay_ms;
    value.hover_card_size = prefs.hover_card_size;
    Normalize(value);
    return value;
}
bool ParsePickerAppearanceJson(const std::wstring& json, PickerAppearanceSnapshot& output) {
    if (!pulse::json::ValidConfigObject(json)) return false;
    PickerAppearanceSnapshot value;
    value.theme_mode = Integer(json, L"theme_mode", -1);
    value.accent_rgb = Accent(pulse::json::ExtractString(json, L"accent_rgb"));
    value.accent_follow_system = Integer(json, L"accent_follow_system", 0) != 0;
    value.language = pulse::json::ExtractString(json, L"language", L"system");
    value.ui_font_scale = Integer(json, L"ui_font_scale", 100);
    value.text_render = Integer(json, L"text_render", 0);
    value.row_height = Integer(json, L"row_height", 34);
    value.selection_outline = pulse::json::ExtractBool(json, L"list_selection_outline", false);
    value.smart_date = pulse::json::ExtractBool(json, L"list_smart_date", true);
    value.zebra_rows = pulse::json::ExtractBool(json, L"list_zebra_rows", true);
    value.size_bar = pulse::json::ExtractBool(json, L"list_size_bar", false);
    value.thumbnail_badges = pulse::json::ExtractBool(json, L"list_thumbnail_badges", true);
    value.details_columns = static_cast<uint32_t>(Integer(json, L"details_columns", static_cast<int>(ui::kDetailsColumnsDefault)));
    value.blank_click_action = Integer(json, L"blank_click_action",
        pulse::json::ExtractBool(json, L"blank_click_go_back", false) ? kBlankClickBack : kBlankClickOff);
    value.preview_trigger = Integer(json, L"preview_trigger", kPreviewTriggerHover);
    value.hover_preview_delay_ms = Integer(json, L"hover_preview_delay_ms", 500);
    value.hover_card_size = Integer(json, L"hover_card_size", kHoverCardMedium);
    Normalize(value);
    if (Integer(json, L"details_columns_v", 1) < 2) value.details_columns |= ui::kDetailsColumnStatus;
    output = std::move(value);
    return true;
}
PickerAppearanceReadResult ReadPickerAppearanceFileReadOnly(const std::wstring& path) noexcept {
    try {
        Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) return Failure(GetLastError());
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.value, &size)) return Failure(GetLastError());
        if (size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > pulse::kMaxUtf8FileBytes) return Failure(ERROR_FILE_TOO_LARGE);
        std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
        size_t offset = 0;
        while (offset < bytes.size()) {
            DWORD read = 0;
            const auto count = static_cast<DWORD>((std::min)(bytes.size() - offset, size_t{65536}));
            if (!ReadFile(file.value, bytes.data() + offset, count, &read, nullptr)) return Failure(GetLastError());
            if (!read) return Failure(ERROR_HANDLE_EOF);
            offset += read;
        }
        std::wstring json;
        if (!pulse::DecodeUtf8Bytes(bytes, json)) return Failure(ERROR_NO_UNICODE_TRANSLATION);
        PickerAppearanceReadResult result;
        if (!ParsePickerAppearanceJson(json, result.snapshot)) return Failure(ERROR_INVALID_DATA);
        result.status = PickerAppearanceReadStatus::Loaded;
        return result;
    } catch (const std::bad_alloc&) { return Failure(ERROR_NOT_ENOUGH_MEMORY); }
    catch (...) { return Failure(ERROR_INVALID_DATA); }
}
PickerAppearanceReadResult ReadUserPickerAppearanceReadOnly() noexcept {
    try {
        CoMemory directory;
        const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &directory.value);
        if (FAILED(hr)) return Failure(HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : ERROR_GEN_FAILURE);
        if (!directory.value || !*directory.value) return Failure(ERROR_PATH_NOT_FOUND);
        return ReadPickerAppearanceFileReadOnly(std::wstring(directory.value) + L"\\Pulse\\app.json");
    } catch (const std::bad_alloc&) { return Failure(ERROR_NOT_ENOUGH_MEMORY); }
    catch (...) { return Failure(ERROR_INVALID_DATA); }
}
PickerAppearanceColors ResolvePickerAppearanceColors(const PickerAppearanceSnapshot& snapshot,
    bool system_dark, uint32_t system_accent, std::optional<bool> legacy_dark) noexcept {
    const bool dark = snapshot.theme_mode == 1 ? false : snapshot.theme_mode == 2 ? true :
        snapshot.theme_mode == -1 ? legacy_dark.value_or(system_dark) : system_dark;
    return {dark, snapshot.accent_rgb.value_or((dark || snapshot.accent_follow_system) ? system_accent : 0x527D70u)};
}
int PickerAppearanceRowHeightDip(const PickerAppearanceSnapshot& snapshot) noexcept {
    const int scale = FontScale(snapshot.ui_font_scale);
    const int row = snapshot.row_height >= 24 && snapshot.row_height <= 48 ? snapshot.row_height : 34;
    return (std::max)(row, scale > 100 ? (28 * scale + 99) / 100 : 0);
}
void ApplyPickerAppearanceToSpec(const PickerAppearanceSnapshot& snapshot, ui::FolderPickerSpec& spec) noexcept {
    spec.selection_outline = snapshot.selection_outline;
    spec.row_height_dip = static_cast<float>(PickerAppearanceRowHeightDip(snapshot));
    spec.list_smart_date = snapshot.smart_date;
    spec.list_zebra_rows = snapshot.zebra_rows;
    spec.list_size_bar = snapshot.size_bar;
    spec.thumbnail_badges = snapshot.thumbnail_badges;
    spec.blank_click_action = NormalizeBlankClickAction(snapshot.blank_click_action);
    spec.preview_trigger = snapshot.preview_trigger;
    spec.hover_preview_delay_ms = snapshot.hover_preview_delay_ms;
    spec.hover_card_size = snapshot.hover_card_size;
    spec.details_columns = snapshot.details_columns;
    spec.explicit_details_columns = true;
}
}

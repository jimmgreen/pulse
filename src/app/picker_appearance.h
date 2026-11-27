#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include "../ui/details_column_set.h"

namespace pulse::ui { struct FolderPickerSpec; }
namespace pulse::app {
struct AppPrefs;
struct PickerListOrderPrefs;
// Presentation preferences: never carries credentials, integration or selection
// policy. Defaults and normalization match AppPrefs and MakePickerSpec. The only
// paths are the folders whose list order the main window remembers (list_order).
struct PickerAppearanceSnapshot {
    int theme_mode = -1; // -1 legacy, 0 system, 1 light, 2 dark
    std::optional<uint32_t> accent_rgb;
    bool accent_follow_system = false;
    std::wstring language = L"system";
    int ui_font_scale = 100;
    int text_render = 0; // 0 Luma Auto, 1 DirectWrite Sharp, 2 Smooth
    int row_height = 34;
    bool selection_outline = false;
    bool smart_date = true;
    bool zebra_rows = true;
    bool size_bar = false;
    bool thumbnail_badges = true;
    bool show_hidden_files = false;
    bool show_protected_os_files = false;
    bool show_file_extensions = true;
    int blank_click_action = 0;
    int preview_trigger = 0;
    int hover_preview_delay_ms = 500;
    int hover_card_size = 1;
    uint32_t details_columns = ui::kDetailsColumnsDefault;
    std::wstring window_effect = L"mica-alt"; // AppPrefs::window_effect id
    int panel_transparency = 50; // AppPrefs::wallpaper_look, 0..90
    int frame_transparency = 10; // AppPrefs::frame_transparency, 0..100
    // The main window's per-folder sort and group-by; null: the dialog's own order.
    std::shared_ptr<const PickerListOrderPrefs> list_order;
};
struct PickerAppearanceColors { bool dark = false; uint32_t accent_rgb = 0x527D70; };
enum class PickerAppearanceReadStatus { Loaded, Missing, Failed };
struct PickerAppearanceReadResult {
    PickerAppearanceReadStatus status = PickerAppearanceReadStatus::Failed;
    DWORD error = ERROR_SUCCESS;
    PickerAppearanceSnapshot snapshot; // Defaults on missing/invalid/unreadable input.
};

PickerAppearanceSnapshot PickerAppearanceFromPrefs(const AppPrefs& prefs);
// Invalid JSON leaves output unchanged. Unknown and nested members are ignored.
bool ParsePickerAppearanceJson(const std::wstring& json, PickerAppearanceSnapshot& output);
// Startup/worker-side only: bounded read, OPEN_EXISTING, no creation, repair,
// registry integration, persistence or logging. No AppPrefs::Load call.
PickerAppearanceReadResult ReadPickerAppearanceFileReadOnly(const std::wstring& path) noexcept;
PickerAppearanceReadResult ReadUserPickerAppearanceReadOnly() noexcept;
// Refresh list-only settings between Show calls; preserve session theme/language
// and last-known preferences if a concurrent save is temporarily unreadable.
void RefreshPickerListPreferences(PickerAppearanceSnapshot& session, const PickerAppearanceReadResult& latest) noexcept;
// Main windows may supply their legacy-session dark value. Standalone surfaces
// lacking a session follow system when theme_mode is legacy, without reading MRU.
PickerAppearanceColors ResolvePickerAppearanceColors(const PickerAppearanceSnapshot& snapshot,
    bool system_dark, uint32_t system_accent, std::optional<bool> legacy_dark = {}) noexcept;
int PickerAppearanceRowHeightDip(const PickerAppearanceSnapshot& snapshot) noexcept;
// Does not overwrite paths, filters, caller-forced hidden visibility, cancellation or hooks.
// Callers initialize l10n + typography from the snapshot before making formats.
void ApplyPickerAppearanceToSpec(const PickerAppearanceSnapshot& snapshot, ui::FolderPickerSpec& spec) noexcept;
}

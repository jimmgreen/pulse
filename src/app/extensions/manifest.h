#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::extensions {

inline constexpr size_t kMaxManifestBytes = 64 * 1024;
inline constexpr size_t kMaxIdBytes = 64;
inline constexpr size_t kMaxLabelBytes = 128;
inline constexpr size_t kMaxResources = 15;
inline constexpr size_t kMaxContributedCommands = 32;
inline constexpr size_t kMaxContributedIcons = 32;
inline constexpr size_t kMaxContributedFileHandlers = 32;
inline constexpr size_t kMaxShowcaseSummaryBytes = 256;
inline constexpr size_t kMaxShowcaseDescriptionBytes = 4096;
inline constexpr size_t kMaxShowcaseHighlightBytes = 160;
inline constexpr size_t kMaxShowcaseTags = 12;
inline constexpr size_t kMaxShowcaseHighlights = 8;

struct NativeFile {
    std::string path;
    std::string sha256;
};

struct NativeRuntime {
    int version = 1;
    std::string architecture;
    std::string library;
    std::string sha256;
    std::vector<NativeFile> files;
};

struct ManifestLocalizedText {
    std::string zh;
    std::string en;
    std::string tw;
};

struct ManifestIconLayer {
    std::string path_data;
    uint32_t draw_flags = 1u;
    uint32_t color_role = 0u;
    uint32_t argb = 0u;
    uint32_t stroke_width_milli = 0u;
    uint32_t opacity_milli = 1000u;
};

struct ManifestIconContribution {
    std::string id;
    uint32_t view_box_milli = 16000u;
    std::vector<ManifestIconLayer> layers;
};

struct ManifestCommandContribution {
    std::string id;
    ManifestLocalizedText title;
    ManifestLocalizedText detail;
    ManifestLocalizedText group;
    bool submenu = false;
    uint32_t flags = 0;       // PULSE_COMMAND_* presentation/visibility bitmask
    uint32_t icon = 0;        // built-in PULSE_ICON_* (0..67) when icon_name.empty()
    std::string icon_name;    // package custom icon id when non-empty
    uint32_t group_icon = 0;  // built-in PULSE_ICON_* for submenu header when group_icon_name.empty()
    std::string group_icon_name;
    uint32_t kinds = 0;       // bit 1 = files, bit 2 = directories, bit 4 = background
    uint32_t min_selection = 0;
    uint32_t max_selection = 0;
    std::vector<std::string> extensions;
    bool exclude_extensions = false;
    uint32_t virtual_key = 0;
    uint32_t modifiers = 0; // bit 1 = ctrl, bit 2 = shift, bit 4 = alt
};

enum class QuickSettingControlKind : uint8_t { Switch, Select, Button };

struct ManifestQuickSettingOption {
    std::string value;
    ManifestLocalizedText label;
};

struct ManifestQuickSettingCondition {
    std::string state;
    std::string equals;
    bool present = false;
};

struct ManifestQuickSettingControl {
    QuickSettingControlKind kind = QuickSettingControlKind::Switch;
    std::string state;
    std::string command;
    ManifestLocalizedText label;
    uint32_t width_dip = 0;
    std::vector<ManifestQuickSettingOption> options;
    ManifestQuickSettingCondition disabled_when;
    ManifestLocalizedText disabled_label;
};

struct ManifestQuickSettingRow {
    ManifestLocalizedText title;
    ManifestLocalizedText description;
    std::vector<ManifestQuickSettingControl> controls;
};

struct ManifestQuickMenuSample {
    std::string name;
    bool directory = false;
};

struct ManifestQuickMenuPreview {
    ManifestLocalizedText caption;
    std::string submenu_state;
    std::vector<ManifestQuickMenuSample> sample_entries;
};

struct ManifestQuickSettings {
    ManifestLocalizedText title;
    std::string ready_state;
    std::string status_state;
    std::vector<ManifestQuickSettingRow> rows;
    std::optional<ManifestQuickMenuPreview> menu_preview;
};

struct ManifestShowcase {
    std::string version;
    ManifestLocalizedText summary;
    ManifestLocalizedText description;
    std::string category;
    std::string publisher;
    ManifestLocalizedText engine;
    std::string license;
    std::string homepage;
    std::string source;
    uint32_t builtin_icon = 0;
    std::string icon_name;
    uint32_t icon_color = 0;
    bool has_icon_color = false;
    std::vector<std::string> tags;
    std::vector<ManifestLocalizedText> highlights;
    std::optional<ManifestQuickSettings> quick_settings;
};

enum class FileHandlerAction : uint8_t { Command, Preview };

struct ManifestFileHandlerContribution {
    std::string id;
    ManifestLocalizedText display_name;
    ManifestLocalizedText verb_label;
    std::vector<std::string> extensions;
    std::string command;
    FileHandlerAction action = FileHandlerAction::Command;
    std::string icon_resource;
    bool fallback_on_unassociated = false;
};

// When a native package is activated (manifest "activation"). Default: Startup when
// the manifest declares no contributes.commands (nothing else would start it for its
// status items, columns or hooks) or declares the context.events resource (it wants
// folder/selection/file-operation events from the start), otherwise OnDemand.
// See StartsWithWindow().
enum class ManifestActivation { Default, OnDemand, Startup };

struct Manifest {
    int api_version = 1;
    ManifestActivation activation = ManifestActivation::Default;
    std::optional<NativeRuntime> native;
    std::string id;
    std::string name;
    std::vector<std::string> resources;
    std::vector<ManifestIconContribution> icon_contributions;
    bool command_contributions_declared = false;
    std::vector<ManifestCommandContribution> command_contributions;
    std::vector<ManifestFileHandlerContribution> file_handlers;
    std::optional<ManifestShowcase> showcase;
    bool StartsWithWindow() const noexcept {
        if (activation != ManifestActivation::Default) return activation == ManifestActivation::Startup;
        if (!command_contributions_declared || command_contributions.empty()) return true;
        for (const auto& resource : resources)
            if (resource == "context.events") return true;
        return false;
    }
};

struct ManifestResult {
    std::optional<Manifest> manifest;
    std::string error;
    explicit operator bool() const noexcept { return manifest.has_value(); }
};

bool IsForbiddenAssociationExtension(std::string_view ext) noexcept;
// Labels are UTF-8; limits apply to bytes. IDs use ASCII letters, digits, ._-.
ManifestResult ParseManifest(std::string_view utf8);
void SyncManifestIcons(const Manifest& manifest);

} // namespace pulse::extensions
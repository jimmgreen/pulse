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

struct Manifest {
    int api_version = 1;
    std::optional<NativeRuntime> native;
    std::string id;
    std::string name;
    std::vector<std::string> resources;
    std::vector<ManifestIconContribution> icon_contributions;
    bool command_contributions_declared = false;
    std::vector<ManifestCommandContribution> command_contributions;
};

struct ManifestResult {
    std::optional<Manifest> manifest;
    std::string error;
    explicit operator bool() const noexcept { return manifest.has_value(); }
};

// Labels are UTF-8; limits apply to bytes. IDs use ASCII letters, digits, ._-.
ManifestResult ParseManifest(std::string_view utf8);
void SyncManifestIcons(const Manifest& manifest);

} // namespace pulse::extensions

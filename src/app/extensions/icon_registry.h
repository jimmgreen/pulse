#pragma once

#include "../../../sdk/native/include/pulse/icons.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulse::extensions::icons {

inline constexpr size_t kMaxIconNameBytes = 64;
inline constexpr size_t kMaxIconLayers = 32;
inline constexpr size_t kMaxLayerPathBytes = 2048;
inline constexpr size_t kMaxTotalPathBytes = 8192;
inline constexpr size_t kMaxDynamicIconsPerSession = 64;
inline constexpr uint32_t kDefaultViewBoxMilli = 16000u;
inline constexpr uint32_t kMinViewBoxMilli = 4000u;
inline constexpr uint32_t kMaxViewBoxMilli = 1024000u;
inline constexpr uint32_t kMinStrokeWidthMilli = 250u;
inline constexpr uint32_t kMaxStrokeWidthMilli = 8000u;

inline constexpr std::array<std::string_view, PULSE_ICON_BUILTIN_MAX + 1> kBuiltinIconNames = {
    "default",       // 0
    "archive",       // 1
    "extract",       // 2
    "search",        // 3
    "box",           // 4
    "extract-to",    // 5
    "add-archive",   // 6
    "test",          // 7
    "hash",          // 8
    "gear",          // 9
    "folder",        // 10
    "file",          // 11
    "chevron-right", // 12
    "chevron-down",  // 13
    "chevron-up",    // 14
    "more",          // 15
    "check",         // 16
    "shield",        // 17
    "badge",         // 18
    "lock",          // 19
    "eye",           // 20
    "copy",          // 21
    "cut",           // 22
    "rename",        // 23
    "trash",         // 24
    "properties",    // 25
    "open",          // 26
    "open-with",     // 27
    "path",          // 28
    "undo",          // 29
    "close",         // 30
    "plus",          // 31
    "minus",         // 32
    "square",        // 33
    "back",          // 34
    "forward",       // 35
    "up",            // 36
    "refresh",       // 37
    "image",         // 38
    "camera",        // 39
    "film",          // 40
    "text-search",   // 41
    "sparkle",       // 42
    "puzzle",        // 43
    "palette",       // 44
    "list-view",     // 45
    "tabs",          // 46
    "power",         // 47
    "duplicate",     // 48
    "cube",          // 49
    "lines",         // 50
    "keyboard",      // 51
    "info",          // 52
    "warn",          // 53
    "sidebar",       // 54
    "panes",         // 55
    "contrast",      // 56
    "download",      // 57
    "cloud-off",     // 58
    "drag",          // 59
    "sort",          // 60
    "filter",        // 61
    "new-folder",    // 62
    "star",          // 63
    "pc",            // 64
    "disk",          // 65
    "chevron-left",  // 66
    "key"            // 67
};

inline constexpr bool IsBuiltinIconId(uint32_t id) noexcept {
    return id <= PULSE_ICON_BUILTIN_MAX;
}

inline constexpr bool IsCustomIconId(uint32_t id) noexcept {
    return id >= PULSE_ICON_CUSTOM_MIN;
}

inline constexpr std::string_view BuiltinIconName(uint32_t id) noexcept {
    return id < kBuiltinIconNames.size() ? kBuiltinIconNames[id] : std::string_view{};
}

inline std::optional<uint32_t> ResolveBuiltinIconName(std::string_view name) noexcept {
    for (uint32_t i = 0; i < kBuiltinIconNames.size(); ++i) {
        if (kBuiltinIconNames[i] == name) return i;
    }
    if (name == "compress") return PULSE_ICON_ARCHIVE;
    if (name == "settings") return PULSE_ICON_GEAR;
    if (name == "delete") return PULSE_ICON_TRASH;
    if (name == "link") return PULSE_ICON_PATH;
    if (name == "add") return PULSE_ICON_PLUS;
    if (name == "remove") return PULSE_ICON_MINUS;
    if (name == "video") return PULSE_ICON_FILM;
    if (name == "extension") return PULSE_ICON_PUZZLE;
    if (name == "warning") return PULSE_ICON_WARN;
    return std::nullopt;
}

inline bool ValidIconIdentifier(std::string_view id) noexcept {
    if (id.empty() || id.size() > kMaxIconNameBytes) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}

// Validates the exact SVG path grammar accepted by command_icons::AppendSvgPath:
// commands M L H V C S A Z (both cases) with implicit repeats, finite coordinates,
// no unsupported commands or trailing garbage.
inline bool ValidateSvgPathSyntax(std::string_view data) noexcept {
    if (data.empty() || data.size() > kMaxLayerPathBytes) return false;
    for (const unsigned char ch : data) {
        if (ch == '\t' || ch == '\n' || ch == '\r') continue;
        if (ch < 0x20 || ch > 0x7e) return false;
    }
    const char* s = data.data();
    const char* const end = data.data() + data.size();
    const auto skip = [&]() noexcept {
        while (s < end && (*s == ' ' || *s == ',' || *s == '\t' || *s == '\n' || *s == '\r')) ++s;
    };
    const auto number = [&](float& out) noexcept -> bool {
        skip();
        if (s >= end) return false;
        const char* start = s;
        if (*s == '-' || *s == '+') ++s;
        bool digits = false;
        bool point = false;
        while (s < end && ((*s >= '0' && *s <= '9') || (*s == '.' && !point))) {
            if (*s == '.') point = true;
            else digits = true;
            ++s;
        }
        if (!digits) {
            s = start;
            return false;
        }
        if (s < end && (*s == 'e' || *s == 'E')) {
            const char* exp = s + 1;
            if (exp < end && (*exp == '-' || *exp == '+')) ++exp;
            if (exp >= end || *exp < '0' || *exp > '9') return false;
            s = exp;
            while (s < end && *s >= '0' && *s <= '9') ++s;
        }
        const auto parsed = std::from_chars(*start == '+' ? start + 1 : start, s, out);
        return parsed.ec == std::errc{} && std::isfinite(out) && std::fabs(out) <= 16384.0f;
    };
    const auto flag = [&](bool& out) noexcept -> bool {
        skip();
        if (s >= end || (*s != '0' && *s != '1')) return false;
        out = (*s++ == '1');
        return true;
    };

    char command = 0;
    bool has_command = false;
    size_t segments = 0;
    for (;;) {
        skip();
        if (s >= end) break;
        if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) {
            command = *s++;
            if (!has_command && command != 'M' && command != 'm') return false;
        } else if (!command) {
            return false;
        }
        has_command = true;
        if (++segments > 512) return false;
        const bool relative = command >= 'a';
        const char kind = relative ? static_cast<char>(command - 32) : command;
        float a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
        bool large = false, sweep = false;
        if (kind == 'M') {
            if (!number(a) || !number(b)) return false;
            command = relative ? 'l' : 'L';
        } else if (kind == 'L') {
            if (!number(a) || !number(b)) return false;
        } else if (kind == 'H' || kind == 'V') {
            if (!number(a)) return false;
        } else if (kind == 'C') {
            if (!number(a) || !number(b) || !number(c) || !number(d) || !number(e) || !number(f)) return false;
        } else if (kind == 'S') {
            if (!number(c) || !number(d) || !number(e) || !number(f)) return false;
        } else if (kind == 'A') {
            float rotation = 0;
            if (!number(a) || !number(b) || !number(rotation) || !flag(large) || !flag(sweep) ||
                !number(e) || !number(f)) return false;
        } else if (kind == 'Z') {
            command = 0;
        } else {
            return false;
        }
    }
    return has_command;
}

struct CustomIconLayer {
    std::string path_data;
    uint32_t draw_flags = PULSE_ICON_DRAW_STROKE;
    uint32_t color_role = PULSE_ICON_COLOR_INHERIT;
    uint32_t argb = 0;
    uint32_t stroke_width_milli = 0;
    uint32_t opacity_milli = 1000u;
};

struct CustomIconSnapshot {
    PulseIconId id = 0;
    uint64_t revision = 0;
    uint32_t view_box_milli = kDefaultViewBoxMilli;
    std::vector<CustomIconLayer> layers;
};

struct CustomIconEntry {
    PulseIconId id = 0;
    uint64_t revision = 1;
    std::string package_id;
    std::string name;
    bool from_manifest = false;
    uint32_t view_box_milli = kDefaultViewBoxMilli;
    std::vector<CustomIconLayer> layers;
};

inline PulseStatus ValidateDescriptor(const PulseIconDescriptor* desc,
                                      uint32_t& out_view_box_milli,
                                      std::string& out_name,
                                      std::vector<CustomIconLayer>& out_layers) {
    if (!desc || desc->size < sizeof(PulseIconDescriptor) || desc->reserved != 0) return PULSE_ERROR_ARGUMENT;
    if (!desc->layers || desc->layer_count == 0 || desc->layer_count > kMaxIconLayers) return PULSE_ERROR_ARGUMENT;

    const uint32_t vb = desc->view_box_milli == 0 ? kDefaultViewBoxMilli : desc->view_box_milli;
    if (vb < kMinViewBoxMilli || vb > kMaxViewBoxMilli) return PULSE_ERROR_ARGUMENT;

    std::string name;
    if (desc->name.length > 0) {
        if (!desc->name.data) return PULSE_ERROR_ARGUMENT;
        const std::string_view raw(desc->name.data, desc->name.length);
        if (!ValidIconIdentifier(raw)) return PULSE_ERROR_ARGUMENT;
        name.assign(raw);
    }

    std::vector<CustomIconLayer> layers;
    layers.reserve(desc->layer_count);
    size_t total_bytes = 0;
    for (uint32_t i = 0; i < desc->layer_count; ++i) {
        const PulseIconLayer& raw = desc->layers[i];
        if (raw.size < sizeof(PulseIconLayer)) return PULSE_ERROR_ARGUMENT;
        const uint32_t allowed_flags = PULSE_ICON_DRAW_STROKE | PULSE_ICON_DRAW_FILL | PULSE_ICON_DRAW_CLOSED;
        if ((raw.draw_flags & ~allowed_flags) != 0 ||
            (raw.draw_flags & (PULSE_ICON_DRAW_STROKE | PULSE_ICON_DRAW_FILL)) == 0) {
            return PULSE_ERROR_ARGUMENT;
        }
        if (raw.color_role > PULSE_ICON_COLOR_RGB) return PULSE_ERROR_ARGUMENT;
        if (raw.color_role != PULSE_ICON_COLOR_RGB && raw.argb != 0) return PULSE_ERROR_ARGUMENT;
        if (raw.stroke_width_milli != 0 &&
            (raw.stroke_width_milli < kMinStrokeWidthMilli || raw.stroke_width_milli > kMaxStrokeWidthMilli)) {
            return PULSE_ERROR_ARGUMENT;
        }
        const uint32_t opacity = raw.opacity_milli == 0 ? 1000u : raw.opacity_milli;
        if (opacity > 1000u) return PULSE_ERROR_ARGUMENT;
        if (!raw.path_data.data || raw.path_data.length == 0 || raw.path_data.length > kMaxLayerPathBytes) {
            return PULSE_ERROR_ARGUMENT;
        }
        const std::string_view path_sv(raw.path_data.data, raw.path_data.length);
        total_bytes += path_sv.size();
        if (total_bytes > kMaxTotalPathBytes || !ValidateSvgPathSyntax(path_sv)) return PULSE_ERROR_ARGUMENT;

        CustomIconLayer layer;
        layer.path_data.assign(path_sv);
        layer.draw_flags = raw.draw_flags;
        layer.color_role = raw.color_role;
        layer.argb = raw.argb;
        layer.stroke_width_milli = raw.stroke_width_milli;
        layer.opacity_milli = opacity;
        layers.push_back(std::move(layer));
    }

    out_view_box_milli = vb;
    out_name = std::move(name);
    out_layers = std::move(layers);
    return PULSE_OK;
}

class Registry {
public:
    PulseStatus RegisterDynamic(std::string_view package_id,
                                const PulseIconDescriptor* desc,
                                PulseIconId* out_id) {
        if (out_id) *out_id = 0;
        if (!out_id || package_id.empty()) return PULSE_ERROR_ARGUMENT;
        uint32_t vb = 0;
        std::string name;
        std::vector<CustomIconLayer> layers;
        const PulseStatus valid = ValidateDescriptor(desc, vb, name, layers);
        if (valid != PULSE_OK) return valid;

        std::lock_guard lock(mutex_);
        if (!name.empty()) {
            for (const auto& entry : entries_) {
                if (entry.package_id == package_id && entry.name == name) return PULSE_ERROR_STATE;
            }
        }
        if (next_id_ >= 0x3FFFFFFFu) return PULSE_ERROR_UNAVAILABLE;
        const PulseIconId id = next_id_++;
        entries_.push_back(CustomIconEntry{
            id,
            next_revision_++,
            std::string(package_id),
            std::move(name),
            false,
            vb,
            std::move(layers),
        });
        *out_id = id;
        return PULSE_OK;
    }

    PulseStatus UpdateDynamic(std::string_view package_id,
                              PulseIconId id,
                              const PulseIconDescriptor* desc) {
        if (!IsCustomIconId(id) || package_id.empty()) return PULSE_ERROR_ARGUMENT;
        uint32_t vb = 0;
        std::string name;
        std::vector<CustomIconLayer> layers;
        const PulseStatus valid = ValidateDescriptor(desc, vb, name, layers);
        if (valid != PULSE_OK) return valid;

        std::lock_guard lock(mutex_);
        auto it = std::find_if(entries_.begin(), entries_.end(), [&](const CustomIconEntry& e) {
            return e.id == id && !e.from_manifest && e.package_id == package_id;
        });
        if (it == entries_.end()) return PULSE_ERROR_STATE;
        if (!name.empty() && name != it->name) {
            for (const auto& entry : entries_) {
                if (entry.id != id && entry.package_id == package_id && entry.name == name) {
                    return PULSE_ERROR_STATE;
                }
            }
        }
        it->name = std::move(name);
        it->view_box_milli = vb;
        it->layers = std::move(layers);
        it->revision = next_revision_++;
        return PULSE_OK;
    }

    PulseStatus UnregisterDynamic(std::string_view package_id, PulseIconId id) {
        if (!IsCustomIconId(id) || package_id.empty()) return PULSE_ERROR_ARGUMENT;
        std::lock_guard lock(mutex_);
        auto it = std::find_if(entries_.begin(), entries_.end(), [&](const CustomIconEntry& e) {
            return e.id == id && !e.from_manifest && e.package_id == package_id;
        });
        if (it == entries_.end()) return PULSE_ERROR_STATE;
        entries_.erase(it);
        return PULSE_OK;
    }

    void UnregisterDynamicBatch(std::string_view package_id, const std::vector<PulseIconId>& ids) {
        if (ids.empty()) return;
        std::lock_guard lock(mutex_);
        std::erase_if(entries_, [&](const CustomIconEntry& e) {
            return !e.from_manifest && e.package_id == package_id &&
                std::find(ids.begin(), ids.end(), e.id) != ids.end();
        });
    }

    PulseIconId UpsertManifestIcon(std::string_view package_id,
                                   std::string_view name,
                                   uint32_t view_box_milli,
                                   std::vector<CustomIconLayer> layers) {
        if (package_id.empty() || name.empty() || layers.empty()) return 0;
        std::lock_guard lock(mutex_);
        for (auto& entry : entries_) {
            if (entry.from_manifest && entry.package_id == package_id && entry.name == name) {
                entry.view_box_milli = view_box_milli;
                entry.layers = std::move(layers);
                entry.revision = next_revision_++;
                return entry.id;
            }
        }
        if (next_id_ >= 0x3FFFFFFFu && entries_.size() > 4096) return 0;
        const PulseIconId id = next_id_++;
        entries_.push_back(CustomIconEntry{
            id,
            next_revision_++,
            std::string(package_id),
            std::string(name),
            true,
            view_box_milli,
            std::move(layers),
        });
        return id;
    }

    void ClearManifestIconsForMissingPackages(const std::vector<std::string>& active_packages) {
        std::lock_guard lock(mutex_);
        std::erase_if(entries_, [&](const CustomIconEntry& e) {
            if (!e.from_manifest) return false;
            return std::find(active_packages.begin(), active_packages.end(), e.package_id) == active_packages.end();
        });
    }

    void PruneManifestIconsForPackage(std::string_view package_id, const std::vector<std::string>& keep_names) {
        std::lock_guard lock(mutex_);
        std::erase_if(entries_, [&](const CustomIconEntry& e) {
            if (!e.from_manifest || e.package_id != package_id) return false;
            return std::find(keep_names.begin(), keep_names.end(), e.name) == keep_names.end();
        });
    }

    bool Exists(PulseIconId id) const {
        if (IsBuiltinIconId(id)) return true;
        if (!IsCustomIconId(id)) return false;
        std::lock_guard lock(mutex_);
        return std::any_of(entries_.begin(), entries_.end(), [id](const CustomIconEntry& e) {
            return e.id == id;
        });
    }

    std::optional<CustomIconSnapshot> Snapshot(PulseIconId id) const {
        if (!IsCustomIconId(id)) return std::nullopt;
        std::lock_guard lock(mutex_);
        for (const auto& entry : entries_) {
            if (entry.id == id) {
                return CustomIconSnapshot{entry.id, entry.revision, entry.view_box_milli, entry.layers};
            }
        }
        return std::nullopt;
    }

    std::optional<PulseIconId> ResolveName(std::string_view package_id, std::string_view token) const {
        if (token.empty()) return std::nullopt;
        if (token.front() == '#') {
            uint32_t numeric = 0;
            const auto num_sv = token.substr(1);
            if (num_sv.empty()) return std::nullopt;
            const auto parsed = std::from_chars(num_sv.data(), num_sv.data() + num_sv.size(), numeric);
            if (parsed.ec != std::errc{} || parsed.ptr != num_sv.data() + num_sv.size()) return std::nullopt;
            return Exists(numeric) ? std::optional<PulseIconId>(numeric) : std::nullopt;
        }
        if (const auto slash = token.find('/'); slash != std::string_view::npos) {
            const auto pkg = token.substr(0, slash);
            const auto local = token.substr(slash + 1);
            if (pkg.empty() || local.empty()) return std::nullopt;
            std::lock_guard lock(mutex_);
            for (const auto& entry : entries_) {
                if (entry.package_id == pkg && entry.name == local) return entry.id;
            }
            return std::nullopt;
        }
        if (!package_id.empty()) {
            std::lock_guard lock(mutex_);
            for (const auto& entry : entries_) {
                if (entry.package_id == package_id && entry.name == token) return entry.id;
            }
        }
        if (const auto builtin = ResolveBuiltinIconName(token)) return *builtin;
        if (package_id.empty()) {
            std::lock_guard lock(mutex_);
            std::optional<PulseIconId> unique;
            for (const auto& entry : entries_) {
                if (entry.name != token) continue;
                if (unique.has_value()) return std::nullopt; // Ambiguous bare name across packages.
                unique = entry.id;
            }
            return unique;
        }
        return std::nullopt;
    }

private:
    mutable std::mutex mutex_;
    PulseIconId next_id_ = PULSE_ICON_CUSTOM_MIN;
    uint64_t next_revision_ = 1;
    std::vector<CustomIconEntry> entries_;
};

inline Registry& GlobalRegistry() {
    static auto& instance = *new Registry();
    return instance;
}

inline std::optional<PulseIconId> ResolveWideIconGlyph(std::wstring_view glyph) {
    if (!glyph.starts_with(L"icon:")) return std::nullopt;
    const auto rest = glyph.substr(5);
    if (rest.empty() || rest.size() > 136) return std::nullopt;
    std::string ascii;
    ascii.reserve(rest.size());
    for (const wchar_t ch : rest) {
        if (ch <= 0x20 || ch > 0x7e) return std::nullopt;
        ascii.push_back(static_cast<char>(ch));
    }
    return GlobalRegistry().ResolveName({}, ascii);
}

} // namespace pulse::extensions::icons

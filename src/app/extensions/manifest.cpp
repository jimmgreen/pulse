#include "manifest.h"
#include "icon_registry.h"
#include "manifest_schema.generated.h"
#include "../../common/json_value.h"
#include "../../../sdk/native/include/pulse/commands.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>
#include <utility>

namespace pulse::extensions {
namespace {

bool HasOnlyFields(const json::Value& value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) return false;
    for (const auto& key : value.keys()) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) return false;
    }
    return true;
}

bool ReadText(const json::Value& object, std::string_view key, size_t limit, std::string& out) {
    const auto* value = object.find(key);
    if (!value || !value->is_string()) return false;
    const auto& text = value->as_string();
    if (text.empty() || text.size() > limit) return false;
    bool visible = false;
    for (const unsigned char c : text) {
        if (c < 0x20 || c == 0x7f) return false;
        visible = visible || c != ' ';
    }
    if (!visible) return false;
    out = text;
    return true;
}

bool ReadId(const json::Value& object, std::string& out, std::string_view key = "id") {
    if (!ReadText(object, key, kMaxIdBytes, out)) return false;
    return std::all_of(out.begin(), out.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}

bool NativeLibraryName(const json::Value& value, std::string& output) {
    if (!ReadText(value, "library", 128, output) || output.size() <= 4 || !output.ends_with(".dll") ||
        output.find_first_of("<>:\"/\\|?*") != std::string::npos) return false;
    const auto alnum = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (!alnum(output.front()) || !std::all_of(output.begin(), output.end(), [&](char c) {
        return alnum(c) || c == '.' || c == '_' || c == '-';
    })) return false;
    auto stem = output.substr(0, output.find('.'));
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) stem.pop_back();
    if (stem.empty()) return false;
    for (auto& c : stem) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
        stem == "CLOCK$" || stem == "CONIN$" || stem == "CONOUT$") return false;
    if (stem.starts_with("COM") || stem.starts_with("LPT")) {
        const auto number = stem.substr(3);
        if ((number.size() == 1 && number[0] >= '1' && number[0] <= '9') ||
            number == "\xc2\xb9" || number == "\xc2\xb2" || number == "\xc2\xb3") return false;
    }
    return true;
}

std::string LowerAscii(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}

bool SidecarName(const json::Value& value, std::string& output) {
    if (!ReadText(value, "path", 128, output) || output.back() == '.') return false;
    const auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (!alnum(output.front()) || !std::all_of(output.begin(), output.end(), [&](char c) {
        return alnum(c) || c == '.' || c == '_' || c == '-';
    })) return false;
    const auto lower = LowerAscii(output);
    if (lower == "manifest.json" || lower == "package.json") return false;
    const auto stem = lower.substr(0, lower.find('.'));
    return stem != "con" && stem != "prn" && stem != "aux" && stem != "nul" &&
        !(stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '1' && stem[3] <= '9');
}

bool Sha256(const json::Value* value) {
    return value && value->is_string() && value->as_string().size() == 64 &&
        std::all_of(value->as_string().begin(), value->as_string().end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
}

ManifestResult Failure(std::string error) {
    return {std::nullopt, std::move(error)};
}

bool ReadLocalizedOrPlain(const json::Value* value, size_t limit, bool allow_empty, ManifestLocalizedText& out) {
    if (!value) return allow_empty;
    auto valid_str = [&](const json::Value* v, std::string& dst) -> bool {
        if (!v || !v->is_string()) return false;
        const auto& s = v->as_string();
        if (s.empty()) {
            if (!allow_empty) return false;
            dst.clear();
            return true;
        }
        if (s.size() > limit) return false;
        bool visible = false;
        for (const unsigned char c : s) {
            if (c < 0x20 || c == 0x7f) return false;
            visible = visible || c != ' ';
        }
        if (!visible) return false;
        dst = s;
        return true;
    };
    if (value->is_string()) {
        std::string text;
        if (!valid_str(value, text)) return false;
        out = {text, text, text};
        return true;
    }
    if (!value->is_object() || !HasOnlyFields(*value, {"zh", "en", "tw"})) return false;
    if (!valid_str(value->find("zh"), out.zh) || !valid_str(value->find("en"), out.en)) return false;
    if (const auto* tw = value->find("tw")) {
        if (!valid_str(tw, out.tw)) return false;
    } else {
        out.tw.clear();
    }
    return true;
}

bool ParseShortcutKey(std::string_view key, uint32_t& vk) {
    if (key.size() == 1) {
        const char c = key[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            vk = static_cast<uint32_t>(c);
            return true;
        }
        return false;
    }
    if (key.size() >= 2 && key.size() <= 3 && key[0] == 'F') {
        int num = 0;
        for (size_t i = 1; i < key.size(); ++i) {
            if (key[i] < '0' || key[i] > '9') return false;
            if (i == 1 && key[i] == '0') return false;
            num = num * 10 + (key[i] - '0');
        }
        if (num >= 1 && num <= 24) {
            vk = static_cast<uint32_t>(0x70 + num - 1); // VK_F1..VK_F24
            return true;
        }
    }
    return false;
}

bool ParseHexColorArgb(std::string_view text, uint32_t& argb) {
    if (text.empty() || text.front() != '#') return false;
    const auto hex = text.substr(1);
    if (hex.size() != 6 && hex.size() != 8) return false;
    uint32_t value = 0;
    for (const char ch : hex) {
        uint32_t d = 0;
        if (ch >= '0' && ch <= '9') d = static_cast<uint32_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = static_cast<uint32_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') d = static_cast<uint32_t>(ch - 'A' + 10);
        else return false;
        value = (value << 4) | d;
    }
    if (hex.size() == 6) value |= 0xFF000000u;
    else if ((value >> 24) == 0u) return false;
    argb = value;
    return true;
}

bool ParseContributedIcons(const json::Value& icons_val, Manifest& manifest,
                           std::set<std::string>& declared_icons, std::string& error) {
    if (!icons_val.is_array() || icons_val.size() > kMaxContributedIcons) {
        error = "contributes.icons: expected an array of at most 32 icons";
        return false;
    }
    for (size_t i = 0; i < icons_val.size(); ++i) {
        const auto& item = icons_val.at(i);
        if (!HasOnlyFields(item, {"id", "viewBox", "layers"})) {
            error = "contributes.icons[" + std::to_string(i) + "]: unexpected or malformed field";
            return false;
        }
        ManifestIconContribution icon;
        if (!ReadId(item, icon.id) || !declared_icons.insert(icon.id).second) {
            error = "contributes.icons[" + std::to_string(i) + "]: invalid or duplicate icon id";
            return false;
        }
        if (const auto* vb = item.find("viewBox")) {
            if (!vb->is_number() || !std::isfinite(vb->as_number()) ||
                vb->as_number() < 4.0 || vb->as_number() > 1024.0) {
                error = "contributes.icons: viewBox must be a number in [4, 1024]";
                return false;
            }
            icon.view_box_milli = static_cast<uint32_t>(std::lround(vb->as_number() * 1000.0));
        }
        const auto* layers = item.find("layers");
        if (!layers || !layers->is_array() || layers->size() == 0 || layers->size() > icons::kMaxIconLayers) {
            error = "contributes.icons: layers must contain 1..32 layers";
            return false;
        }
        size_t total_path_bytes = 0;
        for (size_t j = 0; j < layers->size(); ++j) {
            const auto& raw = layers->at(j);
            if (!HasOnlyFields(raw, {"path", "stroke", "fill", "closed", "color", "strokeWidth", "opacity"})) {
                error = "contributes.icons: unexpected layer field";
                return false;
            }
            ManifestIconLayer layer;
            if (!ReadText(raw, "path", icons::kMaxLayerPathBytes, layer.path_data) ||
                !icons::ValidateSvgPathSyntax(layer.path_data)) {
                error = "contributes.icons: invalid SVG path syntax";
                return false;
            }
            total_path_bytes += layer.path_data.size();
            if (total_path_bytes > icons::kMaxTotalPathBytes) {
                error = "contributes.icons: icon path data exceeds 8192 bytes";
                return false;
            }
            const auto* stroke_v = raw.find("stroke");
            const auto* fill_v = raw.find("fill");
            if ((stroke_v && !stroke_v->is_bool()) || (fill_v && !fill_v->is_bool())) {
                error = "contributes.icons: stroke and fill must be boolean";
                return false;
            }
            const bool do_stroke = stroke_v ? stroke_v->as_bool() : (fill_v ? false : true);
            const bool do_fill = fill_v ? fill_v->as_bool() : false;
            if (!do_stroke && !do_fill) {
                error = "contributes.icons: layer must enable stroke and/or fill";
                return false;
            }
            bool closed = false;
            if (const auto* closed_v = raw.find("closed")) {
                if (!closed_v->is_bool()) { error = "contributes.icons: closed must be boolean"; return false; }
                closed = closed_v->as_bool();
            }
            layer.draw_flags = (do_stroke ? PULSE_ICON_DRAW_STROKE : 0u) |
                               (do_fill ? PULSE_ICON_DRAW_FILL : 0u) |
                               (closed ? PULSE_ICON_DRAW_CLOSED : 0u);
            if (const auto* color_v = raw.find("color")) {
                if (!color_v->is_string()) { error = "contributes.icons: color must be a string"; return false; }
                const auto& c = color_v->as_string();
                if (c == "inherit") layer.color_role = PULSE_ICON_COLOR_INHERIT;
                else if (c == "accent") layer.color_role = PULSE_ICON_COLOR_ACCENT;
                else if (c == "success") layer.color_role = PULSE_ICON_COLOR_SUCCESS;
                else if (c == "warning") layer.color_role = PULSE_ICON_COLOR_WARNING;
                else if (c == "danger") layer.color_role = PULSE_ICON_COLOR_DANGER;
                else if (ParseHexColorArgb(c, layer.argb)) layer.color_role = PULSE_ICON_COLOR_RGB;
                else { error = "contributes.icons: unknown color role or hex color"; return false; }
            }
            if (const auto* sw = raw.find("strokeWidth")) {
                if (!sw->is_number() || !std::isfinite(sw->as_number()) ||
                    sw->as_number() < 0.25 || sw->as_number() > 8.0) {
                    error = "contributes.icons: strokeWidth must be in [0.25, 8.0]";
                    return false;
                }
                layer.stroke_width_milli = static_cast<uint32_t>(std::lround(sw->as_number() * 1000.0));
            }
            if (const auto* op = raw.find("opacity")) {
                if (!op->is_number() || !std::isfinite(op->as_number()) ||
                    op->as_number() < 0.01 || op->as_number() > 1.0) {
                    error = "contributes.icons: opacity must be in [0.01, 1.0]";
                    return false;
                }
                layer.opacity_milli = static_cast<uint32_t>(std::lround(op->as_number() * 1000.0));
            }
            icon.layers.push_back(std::move(layer));
        }
        manifest.icon_contributions.push_back(std::move(icon));
    }
    return true;
}

bool ParseContributedCommands(const json::Value& contributes, Manifest& manifest, std::string& error) {
    if (!HasOnlyFields(contributes, {"icons", "commands"})) {
        error = "contributes: expected only icons and commands";
        return false;
    }
    std::set<std::string> declared_icons;
    if (const auto* icons_val = contributes.find("icons")) {
        if (!ParseContributedIcons(*icons_val, manifest, declared_icons, error)) return false;
    }
    const auto* commands = contributes.find("commands");
    if (!commands) return true;
    if (!commands->is_array() || commands->size() > kMaxContributedCommands) {
        error = "contributes.commands: expected an array of at most 32 commands";
        return false;
    }
    manifest.command_contributions_declared = true;
    std::set<std::string> seen_ids;
    std::set<std::pair<uint32_t, uint32_t>> seen_shortcuts;
    auto resolve_manifest_icon = [&](std::string_view raw_name, uint32_t& out_builtin, std::string& out_custom) -> bool {
        out_builtin = 0;
        out_custom.clear();
        if (declared_icons.contains(std::string(raw_name))) {
            out_custom.assign(raw_name);
            return true;
        }
        const std::string prefix = manifest.id + "/";
        if (raw_name.starts_with(prefix)) {
            const auto local = raw_name.substr(prefix.size());
            if (declared_icons.contains(std::string(local))) {
                out_custom.assign(local);
                return true;
            }
        }
        if (const auto builtin = icons::ResolveBuiltinIconName(raw_name)) {
            out_builtin = *builtin;
            return true;
        }
        return false;
    };
    for (size_t i = 0; i < commands->size(); ++i) {
        const auto& item = commands->at(i);
        if (!HasOnlyFields(item, {"id", "title", "detail", "group", "groupIcon", "submenu", "primary",
                                  "separatorBefore", "separatorAfter", "hidden", "checked",
                                  "icon", "kinds", "minSelection", "maxSelection", "extensions", "excludeExtensions", "shortcut"})) {
            error = "contributes.commands[" + std::to_string(i) + "]: unexpected or malformed field";
            return false;
        }
        ManifestCommandContribution cmd;
        if (!ReadId(item, cmd.id) || !seen_ids.insert(cmd.id).second) {
            error = "contributes.commands[" + std::to_string(i) + "]: invalid or duplicate command id";
            return false;
        }
        if (!ReadLocalizedOrPlain(item.find("title"), kMaxLabelBytes, false, cmd.title)) {
            error = "contributes.commands[" + std::to_string(i) + "]: invalid title";
            return false;
        }
        if (!ReadLocalizedOrPlain(item.find("detail"), 160, true, cmd.detail)) {
            error = "contributes.commands[" + std::to_string(i) + "]: invalid detail";
            return false;
        }
        if (!ReadLocalizedOrPlain(item.find("group"), 32, true, cmd.group)) {
            error = "contributes.commands[" + std::to_string(i) + "]: invalid group";
            return false;
        }
        auto parse_bool_flag = [&](const char* key, uint32_t bit, bool* mirror = nullptr) -> bool {
            const auto* v = item.find(key);
            if (!v) return true;
            if (!v->is_bool()) {
                error = std::string("contributes.commands: ") + key + " must be boolean";
                return false;
            }
            if (mirror) *mirror = v->as_bool();
            if (v->as_bool()) cmd.flags |= bit;
            return true;
        };
        if (!parse_bool_flag("submenu", PULSE_COMMAND_GROUP_SUBMENU, &cmd.submenu) ||
            !parse_bool_flag("primary", PULSE_COMMAND_PRIMARY) ||
            !parse_bool_flag("separatorBefore", PULSE_COMMAND_SEPARATOR_BEFORE) ||
            !parse_bool_flag("separatorAfter", PULSE_COMMAND_SEPARATOR_AFTER) ||
            !parse_bool_flag("hidden", PULSE_COMMAND_HIDDEN) ||
            !parse_bool_flag("checked", PULSE_COMMAND_CHECKED)) {
            return false;
        }
        if (const auto* icon = item.find("icon")) {
            if (!icon->is_string() || !resolve_manifest_icon(icon->as_string(), cmd.icon, cmd.icon_name)) {
                error = "contributes.commands: unknown icon";
                return false;
            }
        }
        if (const auto* gicon = item.find("groupIcon")) {
            if (!gicon->is_string() || !resolve_manifest_icon(gicon->as_string(), cmd.group_icon, cmd.group_icon_name)) {
                error = "contributes.commands: unknown groupIcon";
                return false;
            }
        }
        const auto* kinds = item.find("kinds");
        if (!kinds || !kinds->is_array() || kinds->size() < 1 || kinds->size() > 3) {
            error = "contributes.commands: kinds must contain files, directories, and/or background";
            return false;
        }
        for (const auto& k : kinds->items()) {
            if (!k.is_string()) { error = "contributes.commands: kinds item must be a string"; return false; }
            const auto& ks = k.as_string();
            const uint32_t bit = ks == "files" ? PULSE_COMMAND_FILES
                : (ks == "directories" ? PULSE_COMMAND_DIRECTORIES
                   : (ks == "background" ? PULSE_COMMAND_BACKGROUND : 0u));
            if (!bit || (cmd.kinds & bit)) { error = "contributes.commands: invalid or duplicate kind"; return false; }
            cmd.kinds |= bit;
        }
        int64_t min_sel = -1, max_sel = -1;
        const auto* min_val = item.find("minSelection");
        const auto* max_val = item.find("maxSelection");
        if (!min_val || !max_val || !min_val->get_int(min_sel, 0, 4096) || !max_val->get_int(max_sel, 0, 4096) || min_sel > max_sel) {
            error = "contributes.commands: invalid minSelection/maxSelection range";
            return false;
        }
        if (cmd.kinds == PULSE_COMMAND_BACKGROUND && min_sel > 0) {
            error = "contributes.commands: background-only command requires minSelection 0";
            return false;
        }
        cmd.min_selection = static_cast<uint32_t>(min_sel);
        cmd.max_selection = static_cast<uint32_t>(max_sel);
        if (const auto* exclude = item.find("excludeExtensions")) {
            if (!exclude->is_bool()) { error = "contributes.commands: excludeExtensions must be boolean"; return false; }
            cmd.exclude_extensions = exclude->as_bool();
        }
        if (const auto* exts = item.find("extensions")) {
            if (!exts->is_array() || exts->size() > 32) {
                error = "contributes.commands: extensions must be an array of at most 32 extensions";
                return false;
            }
            std::set<std::string> seen_ext;
            for (const auto& ext_val : exts->items()) {
                if (!ext_val.is_string()) { error = "contributes.commands: extension must be a string"; return false; }
                const auto& ext = ext_val.as_string();
                if (ext.size() < 2 || ext.size() > 32 || ext[0] != '.' ||
                    !std::all_of(ext.begin() + 1, ext.end(), [](char c) {
                        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
                    }) || !seen_ext.insert(ext).second) {
                    error = "contributes.commands: invalid or duplicate lowercase dotted extension";
                    return false;
                }
                cmd.extensions.push_back(ext);
            }
        }
        if (const auto* shortcut = item.find("shortcut")) {
            if (!HasOnlyFields(*shortcut, {"key", "ctrl", "shift", "alt"})) {
                error = "contributes.commands: invalid shortcut fields";
                return false;
            }
            const auto* key_val = shortcut->find("key");
            if (!key_val || !key_val->is_string() || !ParseShortcutKey(key_val->as_string(), cmd.virtual_key)) {
                error = "contributes.commands: invalid shortcut key";
                return false;
            }
            auto flag = [&](const char* field, uint32_t bit) -> bool {
                const auto* f = shortcut->find(field);
                if (!f) return true;
                if (!f->is_bool()) return false;
                if (f->as_bool()) cmd.modifiers |= bit;
                return true;
            };
            if (!flag("ctrl", 1u) || !flag("shift", 2u) || !flag("alt", 4u)) {
                error = "contributes.commands: shortcut modifiers must be boolean";
                return false;
            }
            if (cmd.virtual_key < 0x70 && !(cmd.modifiers & (1u | 4u))) {
                error = "contributes.commands: non-function shortcut requires ctrl or alt";
                return false;
            }
            if (!seen_shortcuts.insert({cmd.virtual_key, cmd.modifiers}).second) {
                error = "contributes.commands: duplicate shortcut";
                return false;
            }
        }
        manifest.command_contributions.push_back(std::move(cmd));
    }
    return true;
}

} // namespace

ManifestResult ParseManifest(std::string_view utf8) {
    json::Value root;
    std::string error;
    // Eight UI levels can contain eight data-row levels (object/array pairs).
    // ParseUiTree separately enforces each semantic depth and item budget.
    if (!json::Parse(utf8, root, &error, {kMaxManifestBytes, 40})) return Failure(std::move(error));
    for (const auto* field : {"runtime", "commands", "toolbar", "palette", "workflows", "panel"}) {
        if (root.find(field)) return Failure(std::string(field) + " is no longer supported; use a native C++ DLL package");
    }
    if (!HasOnlyFields(root, {"apiVersion", "id", "name", "resources", "native", "contributes"})) {
        return Failure("Manifest must be an object containing only apiVersion, id, name, resources, native and contributes");
    }
    const auto* version = root.find("apiVersion");
    int64_t api_version = 0;
    if (!version || !version->get_int(api_version, 1, 1)) return Failure("apiVersion must be 1");
    Manifest manifest;
    if (!ReadId(root, manifest.id)) return Failure("Invalid manifest id (1-64 ASCII letters, digits, ._-)");
    if (!ReadText(root, "name", kMaxLabelBytes, manifest.name)) return Failure("Invalid manifest name (1-128 UTF-8 bytes, no controls)");
    if (const auto* resources = root.find("resources")) {
        if (!resources->is_array() || resources->size() > kMaxResources) return Failure("resources must be an array of at most 15 resource permissions");
        static_assert(generated::kResourceDeclarations.size() == kMaxResources);
        for (size_t i = 0; i < resources->size(); ++i) {
            const auto& resource = resources->at(i);
            if (!resource.is_string() || std::find(generated::kResourceDeclarations.begin(), generated::kResourceDeclarations.end(), resource.as_string()) == generated::kResourceDeclarations.end()) return Failure("resources[" + std::to_string(i) + "]: unknown resource permission");
            if (std::find(manifest.resources.begin(), manifest.resources.end(), resource.as_string()) != manifest.resources.end()) return Failure("resources contains a duplicate permission");
            manifest.resources.push_back(resource.as_string());
        }
    }
    if (const auto* contributes = root.find("contributes")) {
        if (!ParseContributedCommands(*contributes, manifest, error)) return Failure(std::move(error));
    }
    if (const auto* value = root.find("native")) {
        if (!HasOnlyFields(*value, {"version", "architecture", "library", "sha256", "files"}))
            return Failure("native: expected only version, architecture, library, sha256 and files");
        NativeRuntime native;
        int64_t native_version = 0;
        const auto* version_value = value->find("version");
        if (!version_value || !version_value->get_int(native_version, 1, 1)) return Failure("native.version: expected 1");
        if (value->get_string("architecture") != "windows-x64") return Failure("native.architecture: expected windows-x64");
        native.architecture = "windows-x64";
        if (!NativeLibraryName(*value, native.library)) return Failure("native.library: expected a non-reserved single filename ending in .dll");
        const auto* hash = value->find("sha256");
        if (!Sha256(hash)) return Failure("native.sha256: expected 64 lowercase hexadecimal characters");
        native.sha256 = hash->as_string();
        if (const auto* files = value->find("files")) {
            if (!files->is_array() || files->size() == 0 || files->size() > 63)
                return Failure("native.files: expected 1-63 sidecar files; omit files for a single DLL");
            std::vector<std::string> names{LowerAscii(native.library)};
            for (const auto& entry : files->items()) {
                NativeFile file;
                if (!HasOnlyFields(entry, {"path", "sha256"}) || !SidecarName(entry, file.path) || !Sha256(entry.find("sha256")))
                    return Failure("native.files: expected a non-reserved flat ASCII path and lowercase SHA-256");
                const auto name = LowerAscii(file.path);
                if (std::find(names.begin(), names.end(), name) != names.end())
                    return Failure("native.files: duplicate filename (case-insensitive)");
                names.push_back(name);
                file.sha256 = entry.get_string("sha256");
                native.files.push_back(std::move(file));
            }
        }
        manifest.native = std::move(native);
    }
    return {std::move(manifest), {}};
}

void SyncManifestIcons(const Manifest& manifest) {
    if (manifest.id.empty()) return;
    std::vector<std::string> keep;
    keep.reserve(manifest.icon_contributions.size());
    for (const auto& icon : manifest.icon_contributions) {
        std::vector<icons::CustomIconLayer> layers;
        layers.reserve(icon.layers.size());
        for (const auto& src : icon.layers) {
            layers.push_back(icons::CustomIconLayer{
                src.path_data,
                src.draw_flags,
                src.color_role,
                src.argb,
                src.stroke_width_milli,
                src.opacity_milli,
            });
        }
        icons::GlobalRegistry().UpsertManifestIcon(manifest.id, icon.id, icon.view_box_milli, std::move(layers));
        keep.push_back(icon.id);
    }
    icons::GlobalRegistry().PruneManifestIconsForPackage(manifest.id, keep);
}

} // namespace pulse::extensions

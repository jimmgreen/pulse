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

bool ValidateStringText(std::string_view text, size_t limit, bool allow_empty, bool allow_newlines, std::string& dst) {
    if (text.empty()) {
        if (!allow_empty) return false;
        dst.clear();
        return true;
    }
    if (text.size() > limit) return false;
    std::string normalized;
    if (allow_newlines) normalized.reserve(text.size());
    bool visible = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (allow_newlines && c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            normalized.push_back('\n');
            ++i;
            continue;
        }
        if (allow_newlines && c == '\n') {
            normalized.push_back('\n');
            continue;
        }
        if (c < 0x20 || c == 0x7f) return false;
        if (c != ' ') visible = true;
        if (allow_newlines) normalized.push_back(static_cast<char>(c));
    }
    if (!visible) return false;
    dst = allow_newlines ? std::move(normalized) : std::string(text);
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

bool IsReservedWindowsStem(std::string_view lower_stem) noexcept {
    if (lower_stem == "con" || lower_stem == "prn" || lower_stem == "aux" || lower_stem == "nul" ||
        lower_stem == "clock$" || lower_stem == "conin$" || lower_stem == "conout$") {
        return true;
    }
    return lower_stem.size() == 4 && (lower_stem.starts_with("com") || lower_stem.starts_with("lpt")) &&
           lower_stem[3] >= '1' && lower_stem[3] <= '9';
}

bool ValidFlatAsciiFilename(std::string_view name) {
    if (name.empty() || name.size() > 128 || name.back() == '.') return false;
    if (name.find("..") != std::string_view::npos) return false;
    const auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (!alnum(name.front()) || !std::all_of(name.begin(), name.end(), [&](char c) {
        return alnum(c) || c == '.' || c == '_' || c == '-';
    })) {
        return false;
    }
    const auto lower = LowerAscii(std::string(name));
    if (lower == "manifest.json" || lower == "package.json") return false;
    return !IsReservedWindowsStem(std::string_view(lower).substr(0, lower.find('.')));
}

bool SidecarName(const json::Value& value, std::string& output) {
    if (!ReadText(value, "path", 128, output)) return false;
    return ValidFlatAsciiFilename(output);
}

bool ValidIconResourceSpec(std::string_view value) {
    if (value.size() < 5 || value.size() > 140) return false;
    const size_t comma = value.find(',');
    if (comma == std::string_view::npos) {
        if (!ValidFlatAsciiFilename(value)) return false;
        return LowerAscii(std::string(value)).ends_with(".ico");
    }
    if (comma == 0 || comma + 1 >= value.size()) return false;
    const std::string_view file = value.substr(0, comma);
    const std::string_view index = value.substr(comma + 1);
    if (!ValidFlatAsciiFilename(file)) return false;
    const std::string lower = LowerAscii(std::string(file));
    if (!lower.ends_with(".dll") && !lower.ends_with(".exe") && !lower.ends_with(".ico")) return false;
    size_t pos = index.front() == '-' ? 1 : 0;
    if (pos >= index.size() || index.size() - pos > 6) return false;
    return std::all_of(index.begin() + static_cast<std::ptrdiff_t>(pos), index.end(),
                       [](char c) { return c >= '0' && c <= '9'; });
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

bool ReadLocalizedOrPlain(const json::Value* value, size_t limit, bool allow_empty, ManifestLocalizedText& out,
                          bool allow_newlines = false) {
    if (!value) return allow_empty;
    auto valid_str = [&](const json::Value* v, std::string& dst) -> bool {
        return v && v->is_string() && ValidateStringText(v->as_string(), limit, allow_empty, allow_newlines, dst);
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

bool ParseHexColorRgb(std::string_view text, uint32_t& rgb) {
    if (text.size() != 7 || text.front() != '#') return false;
    uint32_t value = 0;
    for (size_t i = 1; i < 7; ++i) {
        const char ch = text[i];
        uint32_t d = 0;
        if (ch >= '0' && ch <= '9') d = static_cast<uint32_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = static_cast<uint32_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') d = static_cast<uint32_t>(ch - 'A' + 10);
        else return false;
        value = (value << 4) | d;
    }
    rgb = value;
    return true;
}

bool ReadHttpUrl(const json::Value& object, std::string_view key, std::string& out) {
    if (!ReadText(object, key, 512, out)) return false;
    return out.starts_with("https://") || out.starts_with("http://");
}

bool ResolveManifestIcon(const Manifest& manifest, const std::set<std::string>& declared_icons,
                         std::string_view raw_name, uint32_t& out_builtin, std::string& out_custom) {
    out_builtin = 0;
    out_custom.clear();
    if (raw_name.empty() || raw_name.size() > 130) return false;
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

bool ParseContributedFileHandlers(const json::Value& handlers_val, Manifest& manifest, std::string& error) {
    if (!handlers_val.is_array() || handlers_val.size() > kMaxContributedFileHandlers) {
        error = "contributes.fileHandlers: expected an array of at most 32 handlers";
        return false;
    }
    std::set<std::string> seen_ids;
    std::set<std::string> seen_extensions;
    for (size_t i = 0; i < handlers_val.size(); ++i) {
        const auto& item = handlers_val.at(i);
        if (!HasOnlyFields(item, {"id", "displayName", "verbLabel", "extensions", "command",
                                  "action", "iconResource", "fallbackOnUnassociated"})) {
            error = "contributes.fileHandlers[" + std::to_string(i) + "]: unexpected or malformed field";
            return false;
        }
        ManifestFileHandlerContribution handler;
        if (!ReadId(item, handler.id) || !seen_ids.insert(handler.id).second) {
            error = "contributes.fileHandlers[" + std::to_string(i) + "]: invalid or duplicate handler id";
            return false;
        }
        if (!ReadLocalizedOrPlain(item.find("displayName"), kMaxLabelBytes, false, handler.display_name)) {
            error = "contributes.fileHandlers[" + std::to_string(i) + "]: invalid displayName";
            return false;
        }
        if (item.find("verbLabel") && !ReadLocalizedOrPlain(item.find("verbLabel"), kMaxLabelBytes, false, handler.verb_label)) {
            error = "contributes.fileHandlers[" + std::to_string(i) + "]: invalid verbLabel";
            return false;
        }
        if (!ReadId(item, handler.command, "command")) {
            error = "contributes.fileHandlers[" + std::to_string(i) + "]: invalid command identifier";
            return false;
        }
        if (const auto* act = item.find("action")) {
            if (!act->is_string()) {
                error = "contributes.fileHandlers[" + std::to_string(i) + "]: action must be a string";
                return false;
            }
            if (act->as_string() == "command") handler.action = FileHandlerAction::Command;
            else if (act->as_string() == "preview") handler.action = FileHandlerAction::Preview;
            else {
                error = "contributes.fileHandlers[" + std::to_string(i) + "]: unknown action";
                return false;
            }
        }
        if (const auto* icon_res = item.find("iconResource")) {
            if (!icon_res->is_string() || !ValidIconResourceSpec(icon_res->as_string())) {
                error = "contributes.fileHandlers[" + std::to_string(i) + "]: invalid iconResource";
                return false;
            }
            handler.icon_resource = icon_res->as_string();
        }
        if (const auto* fallback = item.find("fallbackOnUnassociated")) {
            if (!fallback->is_bool()) {
                error = "contributes.fileHandlers[" + std::to_string(i) + "]: fallbackOnUnassociated must be boolean";
                return false;
            }
            handler.fallback_on_unassociated = fallback->as_bool();
        }
        const auto* exts = item.find("extensions");
        if (!exts || !exts->is_array() || exts->size() == 0 || exts->size() > 32) {
            error = "contributes.fileHandlers[" + std::to_string(i) + "]: extensions must contain 1..32 extensions";
            return false;
        }
        for (const auto& ext_val : exts->items()) {
            if (!ext_val.is_string()) {
                error = "contributes.fileHandlers[" + std::to_string(i) + "]: extension must be a string";
                return false;
            }
            const auto& ext = ext_val.as_string();
            const bool valid_shape = ext.size() >= 2 && ext.size() <= 32 && ext[0] == '.' &&
                ext.back() != '.' && ext.find("..") == std::string::npos &&
                ((ext[1] >= 'a' && ext[1] <= 'z') || (ext[1] >= '0' && ext[1] <= '9')) &&
                std::all_of(ext.begin() + 1, ext.end(), [](char c) {
                    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
                });
            if (!valid_shape || IsForbiddenAssociationExtension(ext)) {
                error = "contributes.fileHandlers[" + std::to_string(i) + "]: invalid or forbidden extension";
                return false;
            }
            if (!seen_extensions.insert(ext).second) {
                error = "contributes.fileHandlers: duplicate extension across file handlers";
                return false;
            }
            handler.extensions.push_back(ext);
        }
        manifest.file_handlers.push_back(std::move(handler));
    }
    return true;
}

bool ParseContributedCommands(const json::Value& contributes, Manifest& manifest,
                              std::set<std::string>& declared_icons, std::string& error) {
    if (!HasOnlyFields(contributes, {"icons", "commands", "fileHandlers"})) {
        error = "contributes: expected only icons, commands, and fileHandlers";
        return false;
    }
    if (const auto* icons_val = contributes.find("icons")) {
        if (!ParseContributedIcons(*icons_val, manifest, declared_icons, error)) return false;
    }
    if (const auto* handlers_val = contributes.find("fileHandlers")) {
        if (!ParseContributedFileHandlers(*handlers_val, manifest, error)) return false;
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
    for (size_t i = 0; i < commands->size(); ++i) {
        const auto& item = commands->at(i);
        if (!HasOnlyFields(item, {"id", "title", "detail", "group", "groupIcon", "submenu", "primary",
                                  "separatorBefore", "separatorAfter", "hidden", "checked", "toolbar",
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
            !parse_bool_flag("checked", PULSE_COMMAND_CHECKED) ||
            !parse_bool_flag("toolbar", PULSE_COMMAND_TOOLBAR)) {
            return false;
        }
        if (const auto* icon = item.find("icon")) {
            if (!icon->is_string() || !ResolveManifestIcon(manifest, declared_icons, icon->as_string(), cmd.icon, cmd.icon_name)) {
                error = "contributes.commands: unknown icon";
                return false;
            }
        }
        if (const auto* gicon = item.find("groupIcon")) {
            if (!gicon->is_string() || !ResolveManifestIcon(manifest, declared_icons, gicon->as_string(), cmd.group_icon, cmd.group_icon_name)) {
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

bool ParseQuickSettingControl(const json::Value& item, size_t row_index, size_t ctrl_index,
                              ManifestQuickSettingControl& out, std::string& error) {
    const std::string loc = "showcase.quickSettings.rows[" + std::to_string(row_index) + "].controls[" +
                            std::to_string(ctrl_index) + "]";
    if (!HasOnlyFields(item, {"kind", "state", "command", "label", "width", "options",
                              "disabledWhen", "disabledLabel"})) {
        error = loc + ": unexpected or malformed field";
        return false;
    }
    const auto* kind_val = item.find("kind");
    if (!kind_val || !kind_val->is_string()) {
        error = loc + ": kind must be a string";
        return false;
    }
    const auto& kind = kind_val->as_string();
    if (kind == "switch") out.kind = QuickSettingControlKind::Switch;
    else if (kind == "select") out.kind = QuickSettingControlKind::Select;
    else if (kind == "button") out.kind = QuickSettingControlKind::Button;
    else {
        error = loc + ": unknown kind";
        return false;
    }

    if (const auto* dw = item.find("disabledWhen")) {
        if (!HasOnlyFields(*dw, {"state", "equals"})) {
            error = loc + ".disabledWhen: unexpected or malformed field";
            return false;
        }
        if (!ReadId(*dw, out.disabled_when.state, "state")) {
            error = loc + ".disabledWhen: invalid state identifier";
            return false;
        }
        const auto* eq = dw->find("equals");
        if (!eq || !eq->is_string() || eq->as_string().size() > 64) {
            error = loc + ".disabledWhen: equals must be a string (0..64 bytes)";
            return false;
        }
        for (const unsigned char ch : eq->as_string()) {
            if (ch < 0x20 || ch == 0x7f) {
                error = loc + ".disabledWhen: equals must not contain control characters";
                return false;
            }
        }
        out.disabled_when.equals = eq->as_string();
        out.disabled_when.present = true;
    }

    if (out.kind == QuickSettingControlKind::Switch) {
        if (item.find("command") || item.find("label") || item.find("options") ||
            item.find("disabledLabel") || item.find("width")) {
            error = loc + ": switch supports only state and disabledWhen";
            return false;
        }
        if (!ReadId(item, out.state, "state")) {
            error = loc + ": switch requires valid state identifier";
            return false;
        }
        return true;
    }

    if (const auto* w = item.find("width")) {
        int64_t width = 0;
        if (!w->get_int(width, 72, 240)) {
            error = loc + ": width must be an integer in [72, 240]";
            return false;
        }
        out.width_dip = static_cast<uint32_t>(width);
    }

    if (out.kind == QuickSettingControlKind::Select) {
        if (item.find("command") || item.find("label")) {
            error = loc + ": select does not accept command or label";
            return false;
        }
        if (!ReadId(item, out.state, "state")) {
            error = loc + ": select requires valid state identifier";
            return false;
        }
        if (item.find("disabledLabel") &&
            !ReadLocalizedOrPlain(item.find("disabledLabel"), kMaxLabelBytes, false, out.disabled_label)) {
            error = loc + ": invalid disabledLabel";
            return false;
        }
        const auto* opts = item.find("options");
        if (!opts || !opts->is_array() || opts->size() == 0 || opts->size() > 16) {
            error = loc + ": select options must contain 1..16 items";
            return false;
        }
        std::set<std::string> seen_values;
        for (size_t oi = 0; oi < opts->size(); ++oi) {
            const auto& ov = opts->at(oi);
            if (!HasOnlyFields(ov, {"value", "label"})) {
                error = loc + ".options[" + std::to_string(oi) + "]: unexpected or malformed field";
                return false;
            }
            const auto* val = ov.find("value");
            if (!val || !val->is_string() || val->as_string().empty() || val->as_string().size() > 64) {
                error = loc + ".options[" + std::to_string(oi) + "]: value must be 1..64 bytes";
                return false;
            }
            std::string normalized_val;
            if (!ValidateStringText(val->as_string(), 64, false, false, normalized_val) ||
                !seen_values.insert(normalized_val).second) {
                error = loc + ".options[" + std::to_string(oi) + "]: invalid or duplicate value";
                return false;
            }
            ManifestQuickSettingOption opt;
            opt.value = std::move(normalized_val);
            if (!ReadLocalizedOrPlain(ov.find("label"), kMaxLabelBytes, false, opt.label)) {
                error = loc + ".options[" + std::to_string(oi) + "]: invalid label";
                return false;
            }
            out.options.push_back(std::move(opt));
        }
        return true;
    }

    // Button
    if (item.find("state") || item.find("options") || item.find("disabledLabel")) {
        error = loc + ": button does not accept state, options, or disabledLabel";
        return false;
    }
    if (!ReadId(item, out.command, "command")) {
        error = loc + ": button requires valid command identifier";
        return false;
    }
    if (!ReadLocalizedOrPlain(item.find("label"), kMaxLabelBytes, false, out.label)) {
        error = loc + ": button requires valid label";
        return false;
    }
    return true;
}

bool ParseShowcaseQuickSettings(const json::Value& value, ManifestQuickSettings& out, std::string& error) {
    if (!HasOnlyFields(value, {"title", "readyState", "statusState", "rows", "menuPreview"})) {
        error = "showcase.quickSettings: unexpected field";
        return false;
    }
    if (value.find("title") && !ReadLocalizedOrPlain(value.find("title"), kMaxLabelBytes, false, out.title)) {
        error = "showcase.quickSettings.title: invalid localized text";
        return false;
    }
    if (value.find("readyState") && !ReadId(value, out.ready_state, "readyState")) {
        error = "showcase.quickSettings.readyState: invalid identifier";
        return false;
    }
    if (value.find("statusState") && !ReadId(value, out.status_state, "statusState")) {
        error = "showcase.quickSettings.statusState: invalid identifier";
        return false;
    }
    const auto* rows_val = value.find("rows");
    if (!rows_val || !rows_val->is_array() || rows_val->size() == 0 || rows_val->size() > 4) {
        error = "showcase.quickSettings.rows: expected 1..4 rows";
        return false;
    }
    size_t total_controls = 0;
    for (size_t ri = 0; ri < rows_val->size(); ++ri) {
        const auto& rv = rows_val->at(ri);
        if (!HasOnlyFields(rv, {"title", "description", "controls"})) {
            error = "showcase.quickSettings.rows[" + std::to_string(ri) + "]: unexpected or malformed field";
            return false;
        }
        ManifestQuickSettingRow row;
        if (!ReadLocalizedOrPlain(rv.find("title"), kMaxLabelBytes, false, row.title)) {
            error = "showcase.quickSettings.rows[" + std::to_string(ri) + "]: invalid title";
            return false;
        }
        if (rv.find("description") &&
            !ReadLocalizedOrPlain(rv.find("description"), kMaxShowcaseHighlightBytes, false, row.description)) {
            error = "showcase.quickSettings.rows[" + std::to_string(ri) + "]: invalid description";
            return false;
        }
        const auto* ctrls = rv.find("controls");
        if (!ctrls || !ctrls->is_array() || ctrls->size() == 0 || ctrls->size() > 2) {
            error = "showcase.quickSettings.rows[" + std::to_string(ri) + "]: controls must contain 1..2 items";
            return false;
        }
        total_controls += ctrls->size();
        if (total_controls > 8) {
            error = "showcase.quickSettings: at most 8 total controls allowed";
            return false;
        }
        for (size_t ci = 0; ci < ctrls->size(); ++ci) {
            ManifestQuickSettingControl ctrl;
            if (!ParseQuickSettingControl(ctrls->at(ci), ri, ci, ctrl, error)) return false;
            row.controls.push_back(std::move(ctrl));
        }
        out.rows.push_back(std::move(row));
    }

    if (const auto* mp = value.find("menuPreview")) {
        if (!HasOnlyFields(*mp, {"caption", "submenuState", "sampleEntries"})) {
            error = "showcase.quickSettings.menuPreview: unexpected or malformed field";
            return false;
        }
        ManifestQuickMenuPreview preview;
        if (mp->find("caption") &&
            !ReadLocalizedOrPlain(mp->find("caption"), kMaxLabelBytes, false, preview.caption)) {
            error = "showcase.quickSettings.menuPreview.caption: invalid localized text";
            return false;
        }
        if (mp->find("submenuState") && !ReadId(*mp, preview.submenu_state, "submenuState")) {
            error = "showcase.quickSettings.menuPreview.submenuState: invalid identifier";
            return false;
        }
        const auto* samples = mp->find("sampleEntries");
        if (!samples || !samples->is_array() || samples->size() == 0 || samples->size() > 4) {
            error = "showcase.quickSettings.menuPreview.sampleEntries: expected 1..4 entries";
            return false;
        }
        for (size_t si = 0; si < samples->size(); ++si) {
            const auto& sv = samples->at(si);
            if (!HasOnlyFields(sv, {"name", "directory"})) {
                error = "showcase.quickSettings.menuPreview.sampleEntries[" + std::to_string(si) + "]: unexpected field";
                return false;
            }
            const auto* name_v = sv.find("name");
            std::string name;
            if (!name_v || !name_v->is_string() ||
                !ValidateStringText(name_v->as_string(), 128, false, false, name) ||
                name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
                name.find("..") != std::string::npos || name == ".") {
                error = "showcase.quickSettings.menuPreview.sampleEntries[" + std::to_string(si) + "]: invalid sample name";
                return false;
            }
            ManifestQuickMenuSample entry;
            entry.name = std::move(name);
            if (const auto* dir_v = sv.find("directory")) {
                if (!dir_v->is_bool()) {
                    error = "showcase.quickSettings.menuPreview.sampleEntries[" + std::to_string(si) + "]: directory must be boolean";
                    return false;
                }
                entry.directory = dir_v->as_bool();
            }
            preview.sample_entries.push_back(std::move(entry));
        }
        out.menu_preview = std::move(preview);
    }
    return true;
}

bool ParseShowcase(const json::Value& value, Manifest& manifest,
                   const std::set<std::string>& declared_icons, std::string& error) {
    if (!HasOnlyFields(value, {"version", "summary", "description", "category", "publisher", "engine",
                               "license", "homepage", "source", "icon", "iconColor", "tags", "highlights",
                               "quickSettings"})) {
        error = "showcase: unexpected field";
        return false;
    }
    ManifestShowcase sc;
    if (value.find("version") && !ReadText(value, "version", 64, sc.version)) {
        error = "showcase.version: invalid string";
        return false;
    }
    if (value.find("summary") && !ReadLocalizedOrPlain(value.find("summary"), kMaxShowcaseSummaryBytes, false, sc.summary)) {
        error = "showcase.summary: invalid localized text";
        return false;
    }
    if (value.find("description") && !ReadLocalizedOrPlain(value.find("description"), kMaxShowcaseDescriptionBytes, false, sc.description, true)) {
        error = "showcase.description: invalid localized text";
        return false;
    }
    if (const auto* cat = value.find("category")) {
        if (!cat->is_string()) { error = "showcase.category: expected a string"; return false; }
        const auto& c = cat->as_string();
        if (c != "archive" && c != "preview" && c != "search" && c != "ai" && c != "files" && c != "utility") {
            error = "showcase.category: unknown category";
            return false;
        }
        sc.category = c;
    }
    if (value.find("publisher") && !ReadText(value, "publisher", kMaxLabelBytes, sc.publisher)) {
        error = "showcase.publisher: invalid string";
        return false;
    }
    if (value.find("engine") && !ReadLocalizedOrPlain(value.find("engine"), kMaxLabelBytes, false, sc.engine)) {
        error = "showcase.engine: invalid localized text";
        return false;
    }
    if (value.find("license") && !ReadText(value, "license", kMaxLabelBytes, sc.license)) {
        error = "showcase.license: invalid string";
        return false;
    }
    if (value.find("homepage") && !ReadHttpUrl(value, "homepage", sc.homepage)) {
        error = "showcase.homepage: expected http(s) URL";
        return false;
    }
    if (value.find("source") && !ReadHttpUrl(value, "source", sc.source)) {
        error = "showcase.source: expected http(s) URL";
        return false;
    }
    if (const auto* icon = value.find("icon")) {
        if (!icon->is_string() || !ResolveManifestIcon(manifest, declared_icons, icon->as_string(), sc.builtin_icon, sc.icon_name)) {
            error = "showcase.icon: unknown icon";
            return false;
        }
    }
    if (const auto* color = value.find("iconColor")) {
        if (!color->is_string() || !ParseHexColorRgb(color->as_string(), sc.icon_color)) {
            error = "showcase.iconColor: expected #RRGGBB";
            return false;
        }
        sc.has_icon_color = true;
    }
    if (const auto* tags = value.find("tags")) {
        if (!tags->is_array() || tags->size() > kMaxShowcaseTags) {
            error = "showcase.tags: expected an array of at most 12 tags";
            return false;
        }
        std::set<std::string> seen;
        for (const auto& entry : tags->items()) {
            std::string tag;
            if (!entry.is_string() || !ValidateStringText(entry.as_string(), 32, false, false, tag) || !seen.insert(tag).second) {
                error = "showcase.tags: invalid or duplicate tag";
                return false;
            }
            sc.tags.push_back(std::move(tag));
        }
    }
    if (const auto* highlights = value.find("highlights")) {
        if (!highlights->is_array() || highlights->size() > kMaxShowcaseHighlights) {
            error = "showcase.highlights: expected an array of at most 8 highlights";
            return false;
        }
        for (const auto& entry : highlights->items()) {
            ManifestLocalizedText item;
            if (!ReadLocalizedOrPlain(&entry, kMaxShowcaseHighlightBytes, false, item)) {
                error = "showcase.highlights: invalid localized highlight";
                return false;
            }
            sc.highlights.push_back(std::move(item));
        }
    }
    if (const auto* qs_val = value.find("quickSettings")) {
        ManifestQuickSettings qs;
        if (!ParseShowcaseQuickSettings(*qs_val, qs, error)) return false;
        sc.quick_settings = std::move(qs);
    }
    manifest.showcase = std::move(sc);
    return true;
}

} // namespace

bool IsForbiddenAssociationExtension(std::string_view ext) noexcept {
    if (ext.empty() || ext.size() > 32) return true;
    const std::string lower = LowerAscii(std::string(ext));
    static constexpr std::string_view kForbidden[] = {
        ".exe", ".com", ".bat", ".cmd", ".msi", ".msp", ".mst", ".scr", ".pif",
        ".vbs", ".vbe", ".js", ".jse", ".wsf", ".wsh", ".ps1", ".psm1", ".psd1",
        ".lnk", ".url", ".reg", ".inf", ".cpl", ".dll", ".sys", ".drv", ".ocx",
        ".hta", ".jar", ".appref-ms", ".scf", ".shs", ".shb", ".pulse-ext"
    };
    return std::find(std::begin(kForbidden), std::end(kForbidden), std::string_view(lower)) != std::end(kForbidden);
}

ManifestResult ParseManifest(std::string_view utf8) {
    json::Value root;
    std::string error;
    // Eight UI levels can contain eight data-row levels (object/array pairs).
    // ParseUiTree separately enforces each semantic depth and item budget.
    if (!json::Parse(utf8, root, &error, {kMaxManifestBytes, 40})) return Failure(std::move(error));
    for (const auto* field : {"runtime", "commands", "toolbar", "palette", "workflows", "panel"}) {
        if (root.find(field)) return Failure(std::string(field) + " is no longer supported; use a native C++ DLL package");
    }
    if (!HasOnlyFields(root, {"apiVersion", "id", "name", "resources", "native", "activation", "contributes", "showcase"})) {
        return Failure("Manifest must be an object containing only apiVersion, id, name, resources, native, activation, contributes and showcase");
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
    if (const auto* activation = root.find("activation")) {
        const std::string value = activation->is_string() ? activation->as_string() : std::string();
        if (value == "onDemand") manifest.activation = ManifestActivation::OnDemand;
        else if (value == "startup") manifest.activation = ManifestActivation::Startup;
        else return Failure("activation: expected \"onDemand\" or \"startup\"");
    }
    std::set<std::string> declared_icons;
    if (const auto* contributes = root.find("contributes")) {
        if (!ParseContributedCommands(*contributes, manifest, declared_icons, error)) return Failure(std::move(error));
    }
    if (const auto* showcase = root.find("showcase")) {
        if (!ParseShowcase(*showcase, manifest, declared_icons, error)) return Failure(std::move(error));
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
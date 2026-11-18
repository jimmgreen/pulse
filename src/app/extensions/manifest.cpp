#include "manifest.h"
#include "../../common/json_value.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
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
    if (!HasOnlyFields(root, {"apiVersion", "id", "name", "resources", "native"})) {
        return Failure("Manifest must be an object containing only apiVersion, id, name, resources and native");
    }
    const auto* version = root.find("apiVersion");
    int64_t api_version = 0;
    if (!version || !version->get_int(api_version, 1, 1)) return Failure("apiVersion must be 1");
    Manifest manifest;
    if (!ReadId(root, manifest.id)) return Failure("Invalid manifest id (1-64 ASCII letters, digits, ._-)");
    if (!ReadText(root, "name", kMaxLabelBytes, manifest.name)) return Failure("Invalid manifest name (1-128 UTF-8 bytes, no controls)");
    if (const auto* resources = root.find("resources")) {
        if (!resources->is_array() || resources->size() > kMaxResources) return Failure("resources must be an array of at most 15 resource permissions");
        constexpr std::string_view allowed[] = {"context.read", "files.readCurrentDirectory", "operations.own", "settings.own", "audit.own", "files.renameCurrentDirectory", "files.readDirectory", "files.watchDirectory", "files.open", "context.events", "files.transferCurrentSelection", "files.deleteCurrentSelection", "files.transferToDirectory", "files.transferFromDirectory", "files.transferDirectoryTree"};
        static_assert(sizeof(allowed) / sizeof(*allowed) == kMaxResources);
        for (size_t i = 0; i < resources->size(); ++i) {
            const auto& resource = resources->at(i);
            if (!resource.is_string() || std::find(std::begin(allowed), std::end(allowed), resource.as_string()) == std::end(allowed)) return Failure("resources[" + std::to_string(i) + "]: unknown resource permission");
            if (std::find(manifest.resources.begin(), manifest.resources.end(), resource.as_string()) != manifest.resources.end()) return Failure("resources contains a duplicate permission");
            manifest.resources.push_back(resource.as_string());
        }
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

} // namespace pulse::extensions

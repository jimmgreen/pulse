#include "package_store.h"
#include "package_signature.h"
#include "native_image_verify.h"
#include "../../common/json_value.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <set>

#pragma comment(lib, "bcrypt.lib")

namespace pulse::extensions {
namespace {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
bool Only(const json::Value& value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) return false;
    for (const auto& key : value.keys())
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) return false;
    return true;
}
bool Revision(const json::Value* value) {
    if (!value || !value->is_string() || value->as_string().size() != 64) return false;
    return std::all_of(value->as_string().begin(), value->as_string().end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool Directory(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool Read(const std::filesystem::path& path, size_t limit, std::string& bytes, std::string& error) {
    Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    BY_HANDLE_FILE_INFORMATION information{};
    if (file.value == INVALID_HANDLE_VALUE || GetFileType(file.value) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(file.value, &information) ||
        (information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        error = "Cannot read a regular, non-reparse package store file.";
        return false;
    }
    bytes.assign(limit + 1, '\0');
    size_t used = 0;
    while (used < bytes.size()) {
        DWORD count = 0;
        if (!ReadFile(file.value, bytes.data() + used, static_cast<DWORD>(bytes.size() - used), &count, nullptr)) {
            error = "Cannot read the complete package store file.";
            return false;
        }
        if (!count) break;
        used += count;
    }
    if (used > limit) { error = "Package store file exceeds its 65536-byte limit."; return false; }
    bytes.resize(used);
    return true;
}
struct Entry {
    std::string id;
    std::string name;
    std::string revision;
    bool enabled = false;
};
PackageStoreResult Failure(std::string error) {
    PackageStoreResult result;
    result.error = std::move(error);
    return result;
}
}

PackageStoreResult LoadPackageStore(const std::wstring& directory) {
    try {
        const auto root = std::filesystem::path(directory).lexically_normal();
        const auto drive = root.root_name().wstring();
        if (!root.is_absolute() || drive.size() != 2 || drive[1] != L':')
            return Failure("The extension store must use an absolute local drive path.");
        const UINT drive_type = GetDriveTypeW(root.root_path().c_str());
        if (drive_type != DRIVE_FIXED && drive_type != DRIVE_REMOVABLE && drive_type != DRIVE_RAMDISK)
            return Failure("Network extension stores are not supported.");
        for (auto ancestor = root; !ancestor.empty(); ancestor = ancestor.parent_path()) {
            if (!Directory(ancestor)) return Failure("The store and its ancestors must be existing directories without reparse points.");
            if (ancestor == ancestor.root_path()) break;
        }
        std::string bytes;
        std::string error;
        if (!Read(root / "registry.json", kMaxStoreRegistryBytes, bytes, error)) return Failure(error);
        json::Value registry;
        if (!json::Parse(bytes, registry, &error, {kMaxStoreRegistryBytes, 4})) return Failure(error);
        if (!Only(registry, {"formatVersion", "packages"})) return Failure("Invalid package registry fields.");
        const auto* version = registry.find("formatVersion");
        int64_t format = 0;
        const auto* packages = registry.find("packages");
        if (!version || !version->get_int(format, 1, 1) || !packages || !packages->is_array() || packages->size() > kMaxStorePackages)
            return Failure("Registry requires formatVersion 1 and at most 32 packages.");
        std::vector<Entry> entries;
        std::set<std::string> ids;
        for (const auto& item : packages->items()) {
            if (!Only(item, {"id", "name", "enabled", "activeRevision", "previousRevision"}))
                return Failure("Package registry entry contains unknown fields.");
            const auto* enabled = item.find("enabled");
            if (!enabled || !enabled->is_bool() || !Revision(item.find("activeRevision")) ||
                (item.find("previousRevision") && !Revision(item.find("previousRevision"))))
                return Failure("Invalid enabled flag or revision hash in package registry.");
            auto identity = json::Value::Object();
            identity.set("apiVersion", 1);
            identity.set("id", item.find("id") ? *item.find("id") : json::Value());
            identity.set("name", item.find("name") ? *item.find("name") : json::Value());
            const auto parsed_identity = ParseManifest(json::Serialize(identity));
            if (!parsed_identity) return Failure("Invalid registry package identity: " + parsed_identity.error);
            if (!ids.insert(parsed_identity.manifest->id).second) return Failure("Duplicate package id in registry.");
            entries.push_back({parsed_identity.manifest->id, parsed_identity.manifest->name,
                item.get_string("activeRevision"), enabled->as_bool()});
        }
        std::string signature_policy;
        const auto policy_path = root / "signature-policy.json";
        if (GetFileAttributesW(policy_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (!Read(policy_path, kMaxStoreRegistryBytes, signature_policy, error)) return Failure(error);
            if (signature_policy.empty()) return Failure("Invalid empty signature policy.");
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) {
            return Failure("Cannot inspect signature policy.");
        }
        PackageStoreResult result;
        bool versions_checked = false;
        for (const auto& entry : entries) {
            if (!entry.enabled) continue;
            if (!versions_checked) {
                if (!Directory(root / "versions")) return Failure("Enabled packages require a non-reparse versions directory.");
                versions_checked = true;
            }
            if (!Read(root / "versions" / (entry.revision + ".json"), kMaxManifestBytes, bytes, error))
                return Failure(entry.id + ": " + error);
            if (PackageRevision(bytes) != entry.revision) return Failure(entry.id + ": manifest SHA-256 does not match activeRevision.");
            std::string signature;
            const auto signature_path = root / "versions" / (entry.revision + ".sig.json");
            if (GetFileAttributesW(signature_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                if (!Read(signature_path, 16384, signature, error)) return Failure(entry.id + ": " + error);
                if (signature.empty()) return Failure(entry.id + ": invalid empty signature.");
            } else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) {
                return Failure("Cannot inspect package signature.");
            }
            if (!VerifyPackageSignature(bytes, signature, signature_policy, error)) return Failure(entry.id + ": " + error);
            auto parsed = ParseManifest(bytes);
            if (!parsed) return Failure(entry.id + ": " + parsed.error);
            if (parsed.manifest->id != entry.id || parsed.manifest->name != entry.name)
                return Failure(entry.id + ": manifest id or name does not match registry.");
            std::wstring native_path;
            if (parsed.manifest->native) {
                const auto& native = *parsed.manifest->native;
                native_path = native.files.empty()
                    ? (root / "binaries" / (native.sha256 + ".dll")).wstring()
                    : (root / "packages" / entry.revision / native.library).wstring();
                std::vector<HANDLE> locks;
                const bool valid = OpenVerifiedNativePackage(native_path, native, locks, error);
                for (HANDLE handle : locks) CloseHandle(handle);
                if (!valid) return Failure(entry.id + ": " + error);
            }
            result.packages.push_back(std::move(*parsed.manifest));
            result.revisions.push_back(entry.revision);
            result.manifest_bytes.push_back(bytes);
            result.signatures.push_back(signature);
            result.native_paths.push_back(std::move(native_path));
        }
        return result;
    } catch (const std::exception& error) { return Failure(error.what()); }
}
}

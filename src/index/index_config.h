#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace pulse::index {

enum class VolumeKind : uint32_t {
    Fixed = 0,
    Removable = 1,
    Other = 2,
};

enum class VolumeBackend : uint8_t {
    Unsupported = 0,
    NtfsMftUsn = 1,
    DirectoryWalk = 2,
};

inline VolumeBackend SelectVolumeBackend(std::wstring_view file_system) {
    auto ascii_eq = [](std::wstring_view a, std::wstring_view upper) {
        if (a.size() != upper.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            const wchar_t c = (a[i] >= L'a' && a[i] <= L'z')
                ? static_cast<wchar_t>(a[i] - L'a' + L'A')
                : a[i];
            if (c != upper[i]) return false;
        }
        return true;
    };
    if (ascii_eq(file_system, L"NTFS")) return VolumeBackend::NtfsMftUsn;
    if (ascii_eq(file_system, L"FAT32") || ascii_eq(file_system, L"EXFAT") ||
        ascii_eq(file_system, L"FAT") || ascii_eq(file_system, L"FAT16") ||
        ascii_eq(file_system, L"FAT12")) {
        return VolumeBackend::DirectoryWalk;
    }
    return VolumeBackend::Unsupported;
}

struct VolumeInfo {
    std::wstring id;
    std::wstring label;
    std::wstring mount_point;
    std::wstring file_system;
    VolumeKind kind = VolumeKind::Other;
    VolumeBackend backend = VolumeBackend::Unsupported;
    uint32_t serial = 0;
    bool online = false;
    bool supported = false;
    bool enabled = false;
    uint64_t indexed_items = 0;
    uint32_t progress = 0;
    std::wstring state;
    std::wstring error;
};

struct IndexConfig {
    bool load_failed = false;
    uint32_t version = 1;
    uint64_t generation = 1;
    bool include_fixed_ntfs = true;
    bool include_removable_ntfs = true;
    std::wstring index_path;
    std::unordered_set<std::wstring> excluded_volume_ids;
    std::vector<std::wstring> excluded_paths;
    // Built-in system folder groups (kSystemExclusionGroups). Configs written
    // before this setting existed leave system_groups_saved false.
    bool exclude_system = true;
    std::vector<std::wstring> system_groups{L"windows", L"temp", L"old", L"node_modules"};
    bool system_groups_saved = false;
    // Master switch from the app's search setting. Off: the service exits at
    // start (it is still installed) until Pulse starts it with --enable-search.
    bool search_enabled = true;

    bool IsExcluded(const std::wstring& id) const;
    bool IsPathExcluded(std::wstring_view path) const;
};

inline constexpr const wchar_t* kSystemExclusionGroups[] = {
    L"windows", L"temp", L"old", L"programdata", L"node_modules"};
bool IsSystemExclusionGroup(std::wstring_view group);
// "node_modules" hides dependency folders by name on every volume.
bool HidesNodeModules(const IndexConfig& config);
// Folders hidden from the index by the enabled system groups, expanded for
// the system drive and every local user profile.
std::vector<std::wstring> SystemExclusionPaths(const IndexConfig& config);

std::wstring NormalizeVolumeId(std::wstring id);
std::vector<VolumeInfo> EnumerateLocalVolumes(const IndexConfig& config);

std::wstring MachineDataRoot();
std::wstring MachineIndexRoot();
// Protect an empty folder or validate an already-private tree; fail closed.
bool ProtectIndexDirectory(const std::wstring& path);
// Service exit code when the configured index folder is readable by other users
// and holds entries the host does not own, so it cannot be made private.
inline constexpr unsigned long kIndexDirectoryNotPrivate = 1338; // ERROR_INVALID_SECURITY_DESCR
std::wstring UserIndexRoot();
std::wstring MachineConfigPath();

bool LoadIndexConfigFrom(const std::wstring& path, const std::wstring& default_index_path,
                         IndexConfig& config, std::wstring* error = nullptr);
bool LoadMachineConfig(IndexConfig& config, std::wstring* error = nullptr);
bool SaveMachineConfig(const IndexConfig& config, std::wstring* error = nullptr);
bool ConfigureVolume(const std::wstring& id, bool enabled, std::wstring* error = nullptr);
bool ConfigureIndexPath(const std::wstring& path, std::wstring* error = nullptr);
bool ConfigureExcludePath(const std::wstring& path, bool enabled, std::wstring* error = nullptr);
// group is "all" for the master switch or one of kSystemExclusionGroups.
bool ConfigureSystemExclusion(const std::wstring& group, bool enabled, std::wstring* error = nullptr);

} // namespace pulse::index

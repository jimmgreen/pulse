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

struct VolumeInfo {
    std::wstring id;
    std::wstring label;
    std::wstring mount_point;
    std::wstring file_system;
    VolumeKind kind = VolumeKind::Other;
    bool online = false;
    bool supported = false;
    bool enabled = false;
    uint64_t indexed_items = 0;
    uint32_t progress = 0;
    std::wstring state;
    std::wstring error;
};

struct IndexConfig {
    uint32_t version = 1;
    uint64_t generation = 1;
    bool include_fixed_ntfs = true;
    bool include_removable_ntfs = true;
    std::wstring index_path;
    std::unordered_set<std::wstring> excluded_volume_ids;
    std::vector<std::wstring> excluded_paths;

    bool IsExcluded(const std::wstring& id) const;
    bool IsPathExcluded(std::wstring_view path) const;
};

std::wstring NormalizeVolumeId(std::wstring id);
std::vector<VolumeInfo> EnumerateLocalVolumes(const IndexConfig& config);

std::wstring MachineDataRoot();
std::wstring MachineIndexRoot();
std::wstring UserIndexRoot();
std::wstring MachineConfigPath();

bool LoadMachineConfig(IndexConfig& config, std::wstring* error = nullptr);
bool SaveMachineConfig(const IndexConfig& config, std::wstring* error = nullptr);
bool ConfigureVolume(const std::wstring& id, bool enabled, std::wstring* error = nullptr);
bool ConfigureIndexPath(const std::wstring& path, std::wstring* error = nullptr);
bool ConfigureExcludePath(const std::wstring& path, bool enabled, std::wstring* error = nullptr);

} // namespace pulse::index

#pragma once

#include "../index/content_search.h"
#include "../index/index_config.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse {

struct AppState;

namespace app {

enum class DuplicateScanScope : int {
    Folder = 0,
    Drive = 1,
    AllFixed = 2,
};

struct DuplicateFile {
    std::wstring path;
    std::wstring name;
    uint64_t size = 0;
    uint64_t modified = 0;
};

struct DuplicateGroup {
    uint32_t id = 0;
    uint64_t size = 0;
    std::vector<DuplicateFile> files;
    size_t keep_index = 0;
};

struct DuplicateScanSession {
    DuplicateScanScope scope = DuplicateScanScope::Folder;
    std::wstring folder_path;
    std::wstring drive_root;
    uint64_t minimum_file_bytes = 1024;
    uint64_t generation = 0;
    bool scanning = false;
    bool completed = false;
    bool truncated = false;
    DWORD error = ERROR_SUCCESS;
    index::ContentSearchPhase phase = index::ContentSearchPhase::Enumerating;
    uint64_t scanned_files = 0;
    uint64_t total_files = 0;
    uint64_t scanned_bytes = 0;
    std::wstring current_root;
    double files_per_second = 0;
    double megabytes_per_second = 0;
    std::vector<DuplicateGroup> groups;
    uint64_t result_epoch = 0;

    static uint64_t DefaultMinimumBytes(DuplicateScanScope scope) noexcept;
    static std::wstring NormalizeDriveRoot(std::wstring root);
    static std::vector<std::wstring> ResolveRoots(
        DuplicateScanScope scope, const std::wstring& folder, const std::wstring& drive,
        const std::vector<index::VolumeInfo>& volumes);

    void ResetResults();
    void ApplyUpdate(const index::ContentSearchProgress& progress,
                     const std::vector<index::ContentHit>& hits);
    void SetKeep(size_t group, size_t file);
    std::vector<std::wstring> FilesToDelete(size_t group) const;
    std::vector<std::wstring> AllFilesToDelete() const;
    void RemoveDeleted(const std::vector<std::wstring>& paths);
    size_t ExtraCount() const;

private:
    void RebuildGroups();
    void UpdateSpeed(const index::ContentSearchProgress& progress);

    std::vector<index::ContentHit> hits_;
    std::chrono::steady_clock::time_point speed_tick_{};
    uint64_t speed_files_ = 0;
    uint64_t speed_bytes_ = 0;
};

} // namespace app

void StartDuplicateScan(AppState& s);
void CancelDuplicateScan(AppState& s);
void RecycleDuplicateGroup(AppState& s, size_t group);
void RecycleAllDuplicateExtras(AppState& s);
void OpenDuplicateLocation(AppState& s, size_t group, size_t file);
void PersistDuplicateScanPrefs(AppState& s);
void RequestDuplicateVolumeCache(AppState& s, bool force = false);
void ApplyDuplicateVolumeCache(AppState& s, std::vector<index::VolumeInfo> volumes);

} // namespace pulse

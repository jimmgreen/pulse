#include "duplicate_scan.h"

#include <algorithm>
#include <unordered_map>

namespace pulse::app {
namespace {

bool SamePath(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

} // namespace

uint64_t DuplicateScanSession::DefaultMinimumBytes(DuplicateScanScope scope) noexcept {
    return scope == DuplicateScanScope::Folder ? 1024ull : 1024ull * 1024ull;
}

std::wstring DuplicateScanSession::NormalizeDriveRoot(std::wstring root) {
    if (root.size() >= 2 && root[1] == L':') {
        wchar_t letter = root[0];
        if (letter >= L'a' && letter <= L'z')
            letter = static_cast<wchar_t>(letter - L'a' + L'A');
        root.assign(1, letter);
        root += L":\\";
    }
    return root;
}

std::vector<std::wstring> DuplicateScanSession::ResolveRoots(
    DuplicateScanScope scope, const std::wstring& folder, const std::wstring& drive,
    const std::vector<index::VolumeInfo>& volumes) {
    std::vector<std::wstring> roots;
    if (scope == DuplicateScanScope::Folder) {
        if (!folder.empty()) roots.push_back(folder);
        return roots;
    }
    if (scope == DuplicateScanScope::Drive) {
        const std::wstring root = NormalizeDriveRoot(drive);
        if (!root.empty()) roots.push_back(root);
        return roots;
    }
    for (const auto& volume : volumes) {
        if (volume.kind != index::VolumeKind::Fixed || volume.mount_point.empty()) continue;
        roots.push_back(NormalizeDriveRoot(volume.mount_point));
    }
    return roots;
}

void DuplicateScanSession::ResetResults() {
    ++result_epoch;
    scanning = false;
    completed = false;
    truncated = false;
    error = ERROR_SUCCESS;
    phase = index::ContentSearchPhase::Enumerating;
    scanned_files = 0;
    total_files = 0;
    scanned_bytes = 0;
    current_root.clear();
    files_per_second = 0;
    megabytes_per_second = 0;
    groups.clear();
    hits_.clear();
    speed_tick_ = {};
    speed_files_ = 0;
    speed_bytes_ = 0;
}

void DuplicateScanSession::UpdateSpeed(const index::ContentSearchProgress& progress) {
    const auto now = std::chrono::steady_clock::now();
    if (speed_tick_.time_since_epoch().count() == 0 || phase != progress.phase) {
        speed_tick_ = now;
        speed_files_ = progress.scanned_files;
        speed_bytes_ = progress.scanned_bytes;
        if (phase != progress.phase) {
            files_per_second = 0;
            megabytes_per_second = 0;
        }
        return;
    }
    const double elapsed = std::chrono::duration<double>(now - speed_tick_).count();
    if (elapsed < 0.35) return;
    const double files = static_cast<double>(progress.scanned_files - speed_files_) / elapsed;
    const double megabytes =
        static_cast<double>(progress.scanned_bytes - speed_bytes_) / elapsed / (1024.0 * 1024.0);
    const double alpha = (std::min)(1.0, elapsed);
    files_per_second = files_per_second * (1.0 - 0.55 * alpha) + files * (0.55 * alpha);
    megabytes_per_second =
        megabytes_per_second * (1.0 - 0.55 * alpha) + megabytes * (0.55 * alpha);
    speed_tick_ = now;
    speed_files_ = progress.scanned_files;
    speed_bytes_ = progress.scanned_bytes;
}

void DuplicateScanSession::RebuildGroups() {
    std::unordered_map<uint32_t, std::wstring> kept;
    kept.reserve(groups.size());
    for (const auto& group : groups) {
        if (group.keep_index < group.files.size())
            kept[group.id] = group.files[group.keep_index].path;
    }
    std::unordered_map<uint32_t, DuplicateGroup> by_id;
    for (const auto& hit : hits_) {
        if (hit.group == 0) continue;
        DuplicateFile file;
        file.path = hit.path;
        file.name = hit.name.empty() ? hit.path : hit.name;
        file.size = hit.size;
        file.modified = hit.modified;
        auto& group = by_id[hit.group];
        group.id = hit.group;
        group.size = hit.size;
        group.files.push_back(std::move(file));
    }
    groups.clear();
    groups.reserve(by_id.size());
    for (auto& [id, group] : by_id) {
        std::sort(group.files.begin(), group.files.end(),
                  [](const DuplicateFile& left, const DuplicateFile& right) {
                      if (left.modified != right.modified) return left.modified > right.modified;
                      return left.path < right.path;
                  });
        group.keep_index = 0;
        const auto found = kept.find(id);
        if (found != kept.end()) {
            for (size_t i = 0; i < group.files.size(); ++i) {
                if (SamePath(group.files[i].path, found->second)) {
                    group.keep_index = i;
                    break;
                }
            }
        }
        if (group.files.size() >= 2) groups.push_back(std::move(group));
    }
    std::sort(groups.begin(), groups.end(),
              [](const DuplicateGroup& left, const DuplicateGroup& right) {
                  if (left.size != right.size) return left.size > right.size;
                  return left.files.size() > right.files.size();
              });
    ++result_epoch;
}

void DuplicateScanSession::ApplyUpdate(const index::ContentSearchProgress& progress,
                                       const std::vector<index::ContentHit>& hits) {
    if (progress.generation != generation) return;
    UpdateSpeed(progress);
    phase = progress.phase;
    scanned_files = progress.scanned_files;
    total_files = progress.total_files;
    scanned_bytes = progress.scanned_bytes;
    current_root = progress.current_root;
    truncated = progress.truncated;
    error = progress.error;
    if (!hits.empty()) {
        hits_.insert(hits_.end(), hits.begin(), hits.end());
        RebuildGroups();
    }
    if (progress.done) {
        scanning = false;
        completed = true;
        if (!hits_.empty()) RebuildGroups();
    }
}

void DuplicateScanSession::SetKeep(size_t group, size_t file) {
    if (group >= groups.size() || file >= groups[group].files.size()) return;
    if (groups[group].keep_index == file) return;
    groups[group].keep_index = file;
    ++result_epoch;
}

std::vector<std::wstring> DuplicateScanSession::FilesToDelete(size_t group) const {
    std::vector<std::wstring> paths;
    if (group >= groups.size()) return paths;
    const auto& item = groups[group];
    paths.reserve(item.files.size());
    for (size_t i = 0; i < item.files.size(); ++i) {
        if (i == item.keep_index) continue;
        paths.push_back(item.files[i].path);
    }
    return paths;
}

std::vector<std::wstring> DuplicateScanSession::AllFilesToDelete() const {
    std::vector<std::wstring> paths;
    for (size_t i = 0; i < groups.size(); ++i) {
        auto extra = FilesToDelete(i);
        paths.insert(paths.end(), extra.begin(), extra.end());
    }
    return paths;
}

void DuplicateScanSession::RemoveDeleted(const std::vector<std::wstring>& paths) {
    if (paths.empty() || groups.empty()) return;
    ++result_epoch;
    for (auto& group : groups) {
        group.files.erase(std::remove_if(group.files.begin(), group.files.end(),
                                         [&](const DuplicateFile& file) {
                                             return std::any_of(paths.begin(), paths.end(),
                                                                [&](const std::wstring& path) {
                                                                    return SamePath(file.path, path);
                                                                });
                                         }),
                          group.files.end());
        if (group.keep_index >= group.files.size()) group.keep_index = 0;
    }
    groups.erase(std::remove_if(groups.begin(), groups.end(),
                                [](const DuplicateGroup& group) { return group.files.size() < 2; }),
                 groups.end());
    hits_.erase(std::remove_if(hits_.begin(), hits_.end(),
                               [&](const index::ContentHit& hit) {
                                   return std::any_of(paths.begin(), paths.end(),
                                                      [&](const std::wstring& path) {
                                                          return SamePath(hit.path, path);
                                                      });
                               }),
                hits_.end());
}

size_t DuplicateScanSession::ExtraCount() const {
    size_t count = 0;
    for (const auto& group : groups) {
        if (group.files.size() > 1) count += group.files.size() - 1;
    }
    return count;
}

} // namespace pulse::app

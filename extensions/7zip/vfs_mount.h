#pragma once
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "pulse/client.hpp"

class ArchiveVfsManager {
public:
    ArchiveVfsManager() = default;
    ~ArchiveVfsManager();

    ArchiveVfsManager(const ArchiveVfsManager&) = delete;
    ArchiveVfsManager& operator=(const ArchiveVfsManager&) = delete;

    bool Initialize(const pulse::sdk::Host& host);
    bool Initialize(const PulseVfsV1& table);
    void Clear() noexcept;

    bool Available() const noexcept;
    size_t MountCount() const noexcept;

    PulseStatus OpenArchive(const std::wstring& archive_path, bool new_tab = false);

private:
    struct MountData;
    struct MountRecord {
        std::wstring canonical_key;
        std::string mount_id;
        std::shared_ptr<MountData> data;
        pulse::sdk::VfsClient::MountHandle handle;
    };

    static constexpr size_t kMaxMounts = 8;

    mutable std::mutex mutex_;
    pulse::sdk::VfsClient vfs_;
    std::vector<MountRecord> mounts_;
};

#pragma once
#include "../fs/fs_enum.h"
#include <functional>
#include <memory>
#include <string_view>

namespace pulse::ui {

// An uncommitted delivery owns only its newly-created staging directory.
// Destruction queues cleanup; Cleanup is the explicit worker-thread alternative.
class PickerArchiveDelivery {
public:
    ~PickerArchiveDelivery();
    void Commit() noexcept;
    void Cleanup() noexcept;
    PickerArchiveDelivery(const PickerArchiveDelivery&) = delete;
    PickerArchiveDelivery& operator=(const PickerArchiveDelivery&) = delete;
private:
    friend class PickerArchiveSession;
    explicit PickerArchiveDelivery(std::wstring root);
    struct State;
    std::unique_ptr<State> state_;
};

struct PickerArchiveMaterialization {
    DWORD error = ERROR_SUCCESS;
    std::vector<std::wstring> paths;
    std::shared_ptr<PickerArchiveDelivery> lease;
};

class PickerArchiveSession {
public:
    explicit PickerArchiveSession(std::function<std::wstring()> runtime_provider,
                                  std::wstring staging_root = {});
    ~PickerArchiveSession();
    PickerArchiveSession(const PickerArchiveSession&) = delete;
    PickerArchiveSession& operator=(const PickerArchiveSession&) = delete;
    // These three methods are in-memory only and may run on the UI thread.
    void Register(std::wstring archive_path);
    bool Contains(std::wstring_view path) const;
    std::wstring ArchiveFor(std::wstring_view path) const;
    // Blocking operations: call only from a worker. Failures return no partial rows/paths.
    DWORD List(const std::wstring& path, std::vector<fs::DirEntry>& rows,
               const std::function<bool()>& cancelled = {});
    PickerArchiveMaterialization Materialize(const std::vector<std::wstring>& virtual_paths,
                                             const std::function<bool()>& cancelled = {});
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulse::ui

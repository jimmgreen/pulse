#pragma once
#include "../../../../sdk/native/include/pulse/fileops.h"
#include "../runtime.h"
#include <functional>
#include <memory>

namespace pulse { struct AppState; }
namespace pulse::extensions::native {
class DirectoriesAdapter;
struct FileOpsBackend {
    std::function<bool(const CallerBinding&)> live;
    std::function<PulseStatus(const CallerBinding&, uint32_t, PulseFileOperationRequestId*)> submit;
    std::function<PulseStatus(const CallerBinding&, PulseFileOperationRequestId, PulseFileOperation*)> get;
    std::function<PulseStatus(const CallerBinding&, PulseFileOperationRequestId)> cancel;
    std::function<void(const CallerBinding&)> release;
    std::function<PulseStatus(const CallerBinding&, uint32_t, PulseDirectoryId, PulseFileOperationRequestId*)> submit_to_directory;
    std::function<void(const CallerBinding&, PulseFileOperationRequestId)> consume;
    std::function<PulseStatus(const CallerBinding&, uint32_t, PulseDirectoryId, uint64_t,
        const uint64_t*, uint32_t, PulseDirectoryId, PulseFileOperationRequestId*)> submit_entries;
    std::function<PulseStatus(const CallerBinding&, PulseDirectoryId, uint64_t, uint64_t,
        PulseDirectoryId, PulseFileOperationRequestId*)> submit_directory_tree;
    std::function<PulseStatus(const CallerBinding&, PulseFileOperationRequestId, uint32_t,
        char*, uint32_t, PulseFileOperationItem*)> get_item;
};
class FileOpsAdapter final {
public:
    FileOpsAdapter(AppState& owner, CallerBinding caller);
    FileOpsAdapter(AppState& owner, CallerBinding caller, DirectoriesAdapter& directories);
    FileOpsAdapter(CallerBinding caller, FileOpsBackend backend);
    ~FileOpsAdapter();
    FileOpsAdapter(const FileOpsAdapter&) = delete;
    FileOpsAdapter& operator=(const FileOpsAdapter&) = delete;
    PulseFileOpsV1 Table() const noexcept;
    PulseFileOpsV2 TableV2() const noexcept;
    PulseFileOpsV3 TableV3() const noexcept;
    PulseFileOpsV4 TableV4() const noexcept;
    PulseStatus Query(uint32_t version, void* out_table, uint32_t out_size) const noexcept;
    PulseStatus Close() noexcept;
    struct Impl;
private:
    std::shared_ptr<Impl> impl_;
};
} // namespace pulse::extensions::native

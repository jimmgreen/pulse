#include "fileops_adapter.h"
#include <windows.h>
#include <cstring>
#include <unordered_set>
#include <unordered_set>

namespace pulse::extensions::native {
struct FileOpsAdapter::Impl {
    CallerBinding caller;
    FileOpsBackend backend;
    DWORD thread = GetCurrentThreadId();
    bool closed = false;
    std::unordered_set<uint64_t> requests;
    std::unordered_set<uint64_t> tree_requests;
    explicit Impl(CallerBinding value, FileOpsBackend functions) : caller(std::move(value)), backend(std::move(functions)) {}
    PulseStatus Ready() const noexcept {
        if (closed || GetCurrentThreadId() != thread || !backend.live) return PULSE_ERROR_STATE;
        try { return backend.live(caller) ? PULSE_OK : PULSE_ERROR_STATE; }
        catch (...) { return PULSE_ERROR_STATE; }
    }
};

namespace {
PulseStatus Submit(void* context, uint32_t operation, PulseFileOperationRequestId* id) noexcept {
    auto* self = static_cast<FileOpsAdapter::Impl*>(context); if (!self || !id) return PULSE_ERROR_ARGUMENT;
    *id = 0;
    if (self->Ready() != PULSE_OK) return PULSE_ERROR_STATE;
    try {
        if (operation != PULSE_FILEOPS_COPY_SELECTION_TO_OTHER_PANE &&
            operation != PULSE_FILEOPS_MOVE_SELECTION_TO_OTHER_PANE &&
            operation != PULSE_FILEOPS_DELETE_SELECTION_TO_RECYCLE_BIN)
            return PULSE_ERROR_ARGUMENT;
        *id = 0;
        if (self->requests.size() >= 32) return PULSE_ERROR_STATE;
        if (!self->backend.submit) return PULSE_ERROR_UNAVAILABLE;
        const auto status = self->backend.submit(self->caller, operation, id);
        if (status == PULSE_OK) {
            if (!*id || !self->requests.insert(*id).second) { *id = 0; return PULSE_ERROR_STATE; }
        } else *id = 0;
        return status;
    } catch (...) { return PULSE_ERROR_UNAVAILABLE; }
}
PulseStatus Get(void* context, PulseFileOperationRequestId id, PulseFileOperation* out) noexcept {
    auto* self = static_cast<FileOpsAdapter::Impl*>(context); if (!self || !out || out->size < sizeof(PulseFileOperation)) return PULSE_ERROR_ARGUMENT;
    if (self->Ready() != PULSE_OK || !self->backend.get) return PULSE_ERROR_STATE;
    try {
        if (!self->requests.contains(id)) return PULSE_ERROR_STATE;
        const auto status = self->backend.get(self->caller, id, out);
        if (status == PULSE_OK && out->terminal &&
            (!self->tree_requests.contains(id) || out->total_items == 0)) {
            if (self->backend.consume) self->backend.consume(self->caller, id);
            self->requests.erase(id);
        }
        return status;
    } catch (...) { return PULSE_ERROR_UNAVAILABLE; }
}
PulseStatus SubmitToDirectory(void* context, uint32_t operation, PulseDirectoryId directory,
    PulseFileOperationRequestId* id) noexcept {
    auto* self = static_cast<FileOpsAdapter::Impl*>(context);
    if (!self || !id) return PULSE_ERROR_ARGUMENT;
    *id = 0;
    if (self->Ready() != PULSE_OK) return PULSE_ERROR_STATE;
    if (!directory || (operation != PULSE_FILEOPS_COPY_SELECTION_TO_DIRECTORY &&
        operation != PULSE_FILEOPS_MOVE_SELECTION_TO_DIRECTORY)) return PULSE_ERROR_ARGUMENT;
    try {
        if (self->requests.size() >= 32) return PULSE_ERROR_STATE;
        if (!self->backend.submit_to_directory) return PULSE_ERROR_UNAVAILABLE;
        const auto status = self->backend.submit_to_directory(self->caller, operation, directory, id);
        if (status == PULSE_OK) {
            if (!*id || !self->requests.insert(*id).second) { *id = 0; return PULSE_ERROR_STATE; }
        } else *id = 0;
        return status;
    } catch (...) { *id = 0; return PULSE_ERROR_UNAVAILABLE; }
}
PulseStatus SubmitEntries(void* context, uint32_t operation, PulseDirectoryId source, uint64_t snapshot,
    const uint64_t* entries, uint32_t count, PulseDirectoryId destination, PulseFileOperationRequestId* id) noexcept {
    auto* self = static_cast<FileOpsAdapter::Impl*>(context);
    if (!self || !id) return PULSE_ERROR_ARGUMENT;
    *id = 0;
    if (self->Ready() != PULSE_OK) return PULSE_ERROR_STATE;
    if (!source || !snapshot || !destination || !entries || !count || count > 64 ||
        (operation != PULSE_FILEOPS_COPY_ENTRIES_TO_DIRECTORY && operation != PULSE_FILEOPS_MOVE_ENTRIES_TO_DIRECTORY))
        return PULSE_ERROR_ARGUMENT;
    try {
        if (self->requests.size() >= 32) return PULSE_ERROR_STATE;
        if (!self->backend.submit_entries) return PULSE_ERROR_UNAVAILABLE;
        const auto status = self->backend.submit_entries(self->caller, operation, source, snapshot,
            entries, count, destination, id);
        if (status == PULSE_OK) {
            if (!*id || !self->requests.insert(*id).second) { *id = 0; return PULSE_ERROR_STATE; }
        } else *id = 0;
        return status;
    } catch (...) { *id = 0; return PULSE_ERROR_UNAVAILABLE; }
}
PulseStatus SubmitDirectoryTree(void* context, PulseDirectoryId source, uint64_t snapshot, uint64_t entry,
    PulseDirectoryId destination, PulseFileOperationRequestId* id) noexcept {
    auto* self = static_cast<FileOpsAdapter::Impl*>(context);
    if (!self || !id) return PULSE_ERROR_ARGUMENT;
    *id = 0;
    if (self->Ready() != PULSE_OK) return PULSE_ERROR_STATE;
    if (!source || !snapshot || !entry || !destination) return PULSE_ERROR_ARGUMENT;
    try {
        if (self->requests.size() >= 32) return PULSE_ERROR_STATE;
        if (!self->backend.submit_directory_tree) return PULSE_ERROR_UNAVAILABLE;
        const auto status = self->backend.submit_directory_tree(self->caller, source, snapshot, entry, destination, id);
        if (status == PULSE_OK) {
            if (!*id || !self->requests.insert(*id).second) { *id = 0; return PULSE_ERROR_STATE; }
            self->tree_requests.insert(*id);
        } else *id = 0;
        return status;
    } catch (...) { *id = 0; return PULSE_ERROR_UNAVAILABLE; }
}
PulseStatus GetItem(void* context, PulseFileOperationRequestId id, uint32_t index,
    char* path, uint32_t capacity, PulseFileOperationItem* out) noexcept {
    auto* self = static_cast<FileOpsAdapter::Impl*>(context);
    if (!self || !id || !out || out->size < sizeof(PulseFileOperationItem) || (!path && capacity))
        return PULSE_ERROR_ARGUMENT;
    if (self->Ready() != PULSE_OK) return PULSE_ERROR_STATE;
    try {
        if (!self->requests.contains(id)) return PULSE_ERROR_STATE;
        if (!self->backend.get_item) return PULSE_ERROR_UNAVAILABLE;
        const auto status = self->backend.get_item(self->caller, id, index, path, capacity, out);
        if (status == PULSE_OK) {
            PulseFileOperation operation{}; operation.size = sizeof(operation);
            if (self->backend.get && self->backend.get(self->caller, id, &operation) == PULSE_OK &&
                operation.terminal && operation.total_items && index >= operation.total_items - 1) {
                if (self->backend.consume) self->backend.consume(self->caller, id);
                self->tree_requests.erase(id);
                self->requests.erase(id);
            }
        }
        return status;
    } catch (...) { return PULSE_ERROR_UNAVAILABLE; }
}
PulseStatus Cancel(void* context, PulseFileOperationRequestId id) noexcept {
    auto* self = static_cast<FileOpsAdapter::Impl*>(context); if (!self || !id) return PULSE_ERROR_ARGUMENT;
    if (self->Ready() != PULSE_OK || !self->backend.cancel) return PULSE_ERROR_STATE;
    try {
        if (!self->requests.contains(id)) return PULSE_ERROR_STATE;
        return self->backend.cancel(self->caller, id);
    }
    catch (...) { return PULSE_ERROR_UNAVAILABLE; }
}
}
FileOpsAdapter::FileOpsAdapter(CallerBinding caller, FileOpsBackend backend) : impl_(std::make_shared<Impl>(std::move(caller), std::move(backend))) {}
FileOpsAdapter::~FileOpsAdapter() { Close(); }
PulseFileOpsV1 FileOpsAdapter::Table() const noexcept { return {sizeof(PulseFileOpsV1), PULSE_FILEOPS_VERSION_1, impl_.get(), Submit, Get, Cancel}; }
PulseFileOpsV2 FileOpsAdapter::TableV2() const noexcept {
    return {sizeof(PulseFileOpsV2), PULSE_FILEOPS_VERSION_2, impl_.get(), Submit, Get, Cancel, SubmitToDirectory};
}
PulseFileOpsV3 FileOpsAdapter::TableV3() const noexcept {
    return {sizeof(PulseFileOpsV3), PULSE_FILEOPS_VERSION_3, impl_.get(), Submit, Get, Cancel, SubmitToDirectory, SubmitEntries};
}
PulseFileOpsV4 FileOpsAdapter::TableV4() const noexcept {
    return {sizeof(PulseFileOpsV4), PULSE_FILEOPS_VERSION_4, impl_.get(), Submit, Get, Cancel,
        SubmitToDirectory, SubmitEntries, SubmitDirectoryTree, GetItem};
}
PulseStatus FileOpsAdapter::Query(uint32_t version, void* output, uint32_t size) const noexcept {
    if (!output) return PULSE_ERROR_ARGUMENT;
    if (version < PULSE_FILEOPS_VERSION_1 || version > PULSE_FILEOPS_VERSION_4) return PULSE_ERROR_ABI;
    if (size < (version == PULSE_FILEOPS_VERSION_1 ? sizeof(PulseFileOpsV1) :
        version == PULSE_FILEOPS_VERSION_2 ? sizeof(PulseFileOpsV2) :
        version == PULSE_FILEOPS_VERSION_3 ? sizeof(PulseFileOpsV3) : sizeof(PulseFileOpsV4)))
        return PULSE_ERROR_ARGUMENT;
    if (impl_->Ready() != PULSE_OK) return PULSE_ERROR_STATE;
    if (version == PULSE_FILEOPS_VERSION_1) {
        const auto table = Table(); std::memcpy(output, &table, sizeof(table));
    } else if (version == PULSE_FILEOPS_VERSION_2) {
        const auto table = TableV2(); std::memcpy(output, &table, sizeof(table));
    } else if (version == PULSE_FILEOPS_VERSION_3) {
        const auto table = TableV3(); std::memcpy(output, &table, sizeof(table));
    } else {
        const auto table = TableV4(); std::memcpy(output, &table, sizeof(table));
    }
    return PULSE_OK;
}
PulseStatus FileOpsAdapter::Close() noexcept {
    if (!impl_) return PULSE_OK;
    if (GetCurrentThreadId() != impl_->thread) return PULSE_ERROR_STATE;
    if (!impl_->closed) {
        impl_->closed = true;
        for (const auto id : impl_->requests) {
            try { if (impl_->backend.cancel) impl_->backend.cancel(impl_->caller, id); } catch (...) {}
        }
        impl_->requests.clear();
        impl_->tree_requests.clear();
        try { if (impl_->backend.release) impl_->backend.release(impl_->caller); } catch (...) {}
    }
    return PULSE_OK;
}
} // namespace pulse::extensions::native

#pragma once
#include "../app/extensions/transfer_plan.h"
#include <atomic>
#include <functional>
#include <windows.h>
namespace pulse::ops {
enum class ControlledTransferState { NotStarted, Copied, Moved, Failed, Cancelled, DestinationCommittedSourceRetained };
struct ControlledTransferItem {
    std::wstring source;
    std::wstring destination;
    std::wstring relative_destination;
    ControlledTransferState state = ControlledTransferState::NotStarted;
    uint64_t bytes = 0;
    DWORD error = ERROR_SUCCESS;
    DWORD cleanup_error = ERROR_SUCCESS;
    std::wstring remaining_staging_name;
};
struct ControlledTransferResult {
    extensions::transfer_plan::Error permit_error = extensions::transfer_plan::Error::None;
    bool directory_tree = false;
    std::vector<ControlledTransferItem> items;
    bool complete = false;
    DWORD error = ERROR_SUCCESS; // Infrastructure exception; per-item outcomes remain authoritative.
};
struct ControlledTransferProgress { size_t item; uint64_t bytes; uint64_t total; bool committed; };
// Worker-only. No path-based fallback, overwrite, recursive operation, or rollback.
// A crash may leave a staging file or a committed destination alongside its source.
// Preserves ordinary timestamps/attributes, inherits target ACL; no security/EA copy.
// Named streams, EFS, offline, compressed and sparse files are unsupported.
// Caller owns Registry/cancel for the entire invocation; progress runs on this worker.
ControlledTransferResult ExecuteControlledTransfer(
    const extensions::transfer_plan::ExecutionPermit& permit,
    extensions::authorization::Registry& registry,
    const extensions::authorization::PrincipalBinding& expected,
    const std::atomic<bool>& cancelled,
    const std::function<void(const ControlledTransferProgress&)>& progress = {});
}

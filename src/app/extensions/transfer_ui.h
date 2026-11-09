#pragma once
#include "runtime.h"
#include "transfer_plan.h"
#include "transfer_status.h"
#include <windows.h>
namespace pulse {
struct AppState;
}
namespace pulse::extensions {
// UI-thread host entry. Captures selection/target now, never on the worker.
bool BeginTransferToOtherPane(AppState &state, const CallerBinding &principal, transfer_plan::Operation operation);
// The destination lease is checked only on the UI thread until task submission.
bool BeginTransferToDirectory(AppState &state, const CallerBinding &principal, transfer_plan::Operation operation,
    std::wstring target, std::function<bool()> target_valid);
bool BeginTransferEntriesToDirectory(AppState &state, const CallerBinding &principal, transfer_plan::Operation operation,
    std::vector<std::wstring> sources, std::vector<uint64_t> sizes, std::vector<uint64_t> modified,
    std::wstring target, std::function<bool()> sources_valid, std::function<bool()> target_valid);
bool BeginTransferDirectoryTree(AppState &state, const CallerBinding &principal, std::wstring source,
    uint64_t source_modified, std::function<bool()> source_valid, std::wstring target,
    std::function<bool()> target_valid);
// In-memory UI preflight only; filesystem identity is checked asynchronously at execution.
bool CanTransferToOtherPane(AppState &state);
TransferDisplayState ReadTransferDisplayState(AppState &state, const CallerBinding &principal);
std::wstring TransferDisplayLabel(TransferDisplayState state);
// Cancels only pending/controlled work belonging to this live principal.
bool CancelOwnedTransferUi(AppState &state, const CallerBinding &principal);
// Exact request identity for sequential workflows; never adopts a later request on the same owner.
uint64_t CurrentTransferRequest(AppState &state, const CallerBinding &principal);
TransferDisplayState ReadTransferRequestState(AppState &state, const CallerBinding &principal, uint64_t request);
struct TransferRequestSnapshot {
    TransferDisplayState state = TransferDisplayState::Unavailable;
    uint64_t task_id = 0;
    uint64_t transferred_bytes = 0;
    uint64_t total_bytes = 0;
    uint64_t completed_items = 0;
    uint64_t total_items = 0;
    float percent = -1.0f;
    bool cancel_requested = false;
    uint32_t system_error = 0;
};
TransferRequestSnapshot GetTransferRequestSnapshot(AppState &state, const CallerBinding &principal, uint64_t request);
enum class TransferRequestItemState { Pending, Completed, Cancelled, Failed };
struct TransferRequestItemSnapshot {
    std::wstring relative_path;
    uint64_t bytes = 0;
    TransferRequestItemState state = TransferRequestItemState::Pending;
    uint32_t system_error = 0;
};
std::optional<TransferRequestItemSnapshot> GetTransferRequestItem(AppState &state,
    const CallerBinding &principal, uint64_t request, uint32_t index);
bool CancelTransferRequest(AppState &state, const CallerBinding &principal, uint64_t request);
// Native requests hold their exact terminal result until consumed or detached.
bool RetainTransferRequest(AppState &state, const CallerBinding &principal, uint64_t request);
bool ConsumeTransferRequest(AppState &state, const CallerBinding &principal, uint64_t request);
void ReleaseTransferRequests(AppState &state, const CallerBinding &principal);

void CancelTransferUi(HWND owner);
bool TransferUiPending(HWND owner);
// Internal diagnostics; observer is invoked only for an isolated test window.
void SetTransferUiProbeObserver(std::function<void(HWND)> observer);
// One-shot isolated-test hook after immutable source capture, before worker preparation.
void SetTransferPreparationProbeObserver(std::function<void(const std::vector<std::wstring> &)> observer);
uint64_t LastTransferTask(HWND owner);
} // namespace pulse::extensions

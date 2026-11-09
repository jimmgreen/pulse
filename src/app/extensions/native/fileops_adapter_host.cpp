#include "fileops_adapter.h"
#include "directories_adapter.h"
#include "../transfer_ui.h"
#include "../delete_ui.h"
#include "../../app_runtime.h"
#include <algorithm>
#include <cstring>
#include <string>

namespace pulse::extensions::native {
namespace {
FileOpsBackend Backend(AppState& owner, DirectoriesAdapter* directories = nullptr) {
    const HWND window = owner.hwnd;
    FileOpsBackend backend;
    backend.live = [window](const CallerBinding& caller) {
        return IsWindow(window) && caller.Owner().key == reinterpret_cast<uintptr_t>(window) && ValidateCallerBinding(caller);
    };
    backend.submit = [window](const CallerBinding& caller, uint32_t operation, PulseFileOperationRequestId* id) {
        if (!id || !IsWindow(window)) return PULSE_ERROR_ARGUMENT;
        auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!state || caller.Owner().key != reinterpret_cast<uintptr_t>(window) || !ValidateCallerBinding(caller)) return PULSE_ERROR_STATE;
        *id = 0;
        if (operation == PULSE_FILEOPS_DELETE_SELECTION_TO_RECYCLE_BIN)
            return BeginRecycleDeleteSelection(*state, caller, id) ? PULSE_OK : PULSE_ERROR_STATE;
        const auto ok = BeginTransferToOtherPane(*state, caller, operation == PULSE_FILEOPS_COPY_SELECTION_TO_OTHER_PANE
            ? transfer_plan::Operation::Copy : transfer_plan::Operation::Move);
        if (!ok) return PULSE_ERROR_STATE;
        *id = CurrentTransferRequest(*state, caller);
        if (*id && RetainTransferRequest(*state, caller, *id)) return PULSE_OK;
        if (*id) CancelTransferRequest(*state, caller, *id);
        *id = 0;
        return PULSE_ERROR_STATE;
    };
    backend.get = [window](const CallerBinding& caller, PulseFileOperationRequestId id, PulseFileOperation* out) {
        if (!out || !id || !IsWindow(window)) return PULSE_ERROR_ARGUMENT;
        auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!state || caller.Owner().key != reinterpret_cast<uintptr_t>(window) || !ValidateCallerBinding(caller)) return PULSE_ERROR_STATE;
        auto snapshot = GetTransferRequestSnapshot(*state, caller, id);
        if (snapshot.state == TransferDisplayState::Unavailable)
            snapshot = GetDeleteRequestSnapshot(*state, caller, id);
        const auto display = snapshot.state;
        if (display == TransferDisplayState::Unavailable) return PULSE_ERROR_STATE;
        std::memset(out, 0, sizeof(*out)); out->size = sizeof(*out); out->request_id = id;
        switch (display) {
        case TransferDisplayState::Preparing: out->state = PULSE_FILEOPS_PREPARING; break;
        case TransferDisplayState::WaitingConfirmation: out->state = PULSE_FILEOPS_WAITING_CONFIRMATION; break;
        case TransferDisplayState::Queued: out->state = PULSE_FILEOPS_QUEUED; break;
        case TransferDisplayState::Running: out->state = PULSE_FILEOPS_RUNNING; break;
        case TransferDisplayState::Cancelling: out->state = PULSE_FILEOPS_CANCELLING; break;
        case TransferDisplayState::Completed: out->state = PULSE_FILEOPS_COMPLETED; break;
        case TransferDisplayState::Partial: out->state = PULSE_FILEOPS_PARTIAL; break;
        case TransferDisplayState::Cancelled: out->state = PULSE_FILEOPS_CANCELLED; break;
        case TransferDisplayState::Failed: out->state = PULSE_FILEOPS_FAILED; break;
        default: return PULSE_ERROR_STATE;
        }
        out->task_id = snapshot.task_id;
        out->transferred_bytes = snapshot.transferred_bytes;
        out->total_bytes = snapshot.total_bytes;
        out->completed_items = snapshot.completed_items;
        out->total_items = snapshot.total_items;
        if (snapshot.percent >= 0.0f)
            out->percent = static_cast<uint32_t>((std::clamp)(snapshot.percent, 0.0f, 100.0f));
        out->cancel_requested = snapshot.cancel_requested ? 1u : 0u;
        out->system_error = snapshot.system_error;
        out->terminal = display == TransferDisplayState::Completed || display == TransferDisplayState::Partial ||
            display == TransferDisplayState::Cancelled || display == TransferDisplayState::Failed;
        out->succeeded = display == TransferDisplayState::Completed;
        return PULSE_OK;
    };
    backend.cancel = [window](const CallerBinding& caller, PulseFileOperationRequestId id) {
        if (!id || !IsWindow(window)) return PULSE_ERROR_ARGUMENT;
        auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        return state && (CancelTransferRequest(*state, caller, id) || CancelDeleteRequest(*state, caller, id))
            ? PULSE_OK : PULSE_ERROR_STATE;
    };
    backend.release = [window](const CallerBinding& caller) {
        if (!IsWindow(window)) return;
        auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (state && caller.Owner().key == reinterpret_cast<uintptr_t>(window)) {
            ReleaseDeleteRequests(*state, caller);
            ReleaseTransferRequests(*state, caller);
        }
    };
    backend.consume = [window](const CallerBinding& caller, PulseFileOperationRequestId id) {
        if (!IsWindow(window)) return;
        auto* state = GetAppState(window);
        if (state && caller.Owner().key == reinterpret_cast<uintptr_t>(window)) ConsumeTransferRequest(*state, caller, id);
    };
    if (directories) backend.submit_to_directory = [window, directories](const CallerBinding& caller, uint32_t operation,
        PulseDirectoryId destination, PulseFileOperationRequestId* id) {
        if (!id) return PULSE_ERROR_ARGUMENT;
        *id = 0;
        if (!IsWindow(window) || !ValidateCallerBinding(caller) ||
            caller.Owner().key != reinterpret_cast<uintptr_t>(window)) return PULSE_ERROR_STATE;
        auto* state = GetAppState(window);
        if (!state) return PULSE_ERROR_STATE;
        auto target = directories->ResolveTransferTarget(destination);
        if (!target) return PULSE_ERROR_STATE;
        const auto action = operation == PULSE_FILEOPS_COPY_SELECTION_TO_DIRECTORY
            ? transfer_plan::Operation::Copy : transfer_plan::Operation::Move;
        if (!BeginTransferToDirectory(*state, caller, action, std::move(target->path), std::move(target->valid)))
            return PULSE_ERROR_STATE;
        *id = CurrentTransferRequest(*state, caller);
        if (*id && RetainTransferRequest(*state, caller, *id)) return PULSE_OK;
        if (*id) CancelTransferRequest(*state, caller, *id);
        *id = 0;
        return PULSE_ERROR_STATE;
    };
    if (directories) backend.submit_entries = [window, directories](const CallerBinding& caller, uint32_t operation,
        PulseDirectoryId source, uint64_t snapshot, const uint64_t* entries, uint32_t count,
        PulseDirectoryId destination, PulseFileOperationRequestId* id) {
        if (!id) return PULSE_ERROR_ARGUMENT;
        *id = 0;
        if (!IsWindow(window) || !ValidateCallerBinding(caller) ||
            caller.Owner().key != reinterpret_cast<uintptr_t>(window)) return PULSE_ERROR_STATE;
        auto* state = GetAppState(window);
        if (!state) return PULSE_ERROR_STATE;
        const auto sources = directories->ResolveTransferSources(source, snapshot, entries, count);
        const auto target = directories->ResolveTransferTarget(destination);
        if (!sources || !target) return PULSE_ERROR_STATE;
        const auto action = operation == PULSE_FILEOPS_COPY_ENTRIES_TO_DIRECTORY
            ? transfer_plan::Operation::Copy : transfer_plan::Operation::Move;
        if (!BeginTransferEntriesToDirectory(*state, caller, action, sources->paths, sources->sizes,
            sources->modified, target->path, sources->valid, target->valid)) return PULSE_ERROR_STATE;
        *id = CurrentTransferRequest(*state, caller);
        if (*id && RetainTransferRequest(*state, caller, *id)) return PULSE_OK;
        if (*id) CancelTransferRequest(*state, caller, *id);
        *id = 0;
        return PULSE_ERROR_STATE;
    };
    if (directories) backend.submit_directory_tree = [window, directories](const CallerBinding& caller,
        PulseDirectoryId source, uint64_t snapshot, uint64_t entry, PulseDirectoryId destination,
        PulseFileOperationRequestId* id) {
        if (!id) return PULSE_ERROR_ARGUMENT;
        *id = 0;
        if (!IsWindow(window) || !ValidateCallerBinding(caller) ||
            caller.Owner().key != reinterpret_cast<uintptr_t>(window)) return PULSE_ERROR_STATE;
        auto* state = GetAppState(window);
        if (!state) return PULSE_ERROR_STATE;
        auto source_tree = directories->ResolveTreeTransferSource(source, snapshot, entry);
        auto target = directories->ResolveTransferTarget(destination);
        if (!source_tree || !target) return PULSE_ERROR_STATE;
        if (!BeginTransferDirectoryTree(*state, caller, std::move(source_tree->path), source_tree->modified,
            source_tree->valid, std::move(target->path), target->valid)) return PULSE_ERROR_STATE;
        *id = CurrentTransferRequest(*state, caller);
        if (*id && RetainTransferRequest(*state, caller, *id)) return PULSE_OK;
        if (*id) CancelTransferRequest(*state, caller, *id);
        *id = 0;
        return PULSE_ERROR_STATE;
    };
    backend.get_item = [window](const CallerBinding& caller, PulseFileOperationRequestId request,
        uint32_t index, char* path, uint32_t capacity, PulseFileOperationItem* output) {
        if (!output || !request || !IsWindow(window)) return PULSE_ERROR_ARGUMENT;
        auto* state = GetAppState(window);
        if (!state || caller.Owner().key != reinterpret_cast<uintptr_t>(window) || !ValidateCallerBinding(caller))
            return PULSE_ERROR_STATE;
        const auto item = GetTransferRequestItem(*state, caller, request, index);
        if (!item) return PULSE_ERROR_STATE;
        const auto& value = item->relative_path;
        const int bytes = value.empty() ? 0 : WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (!value.empty() && bytes <= 0) return PULSE_ERROR_STATE;
        const uint32_t required = static_cast<uint32_t>(bytes + 1);
        std::memset(output, 0, sizeof(*output));
        output->size = sizeof(*output); output->index = index; output->path_bytes = required;
        output->transferred_bytes = item->bytes; output->system_error = item->system_error;
        switch (item->state) {
        case TransferRequestItemState::Pending: output->state = PULSE_FILEOPS_ITEM_PENDING; break;
        case TransferRequestItemState::Completed: output->state = PULSE_FILEOPS_ITEM_COMPLETED; break;
        case TransferRequestItemState::Cancelled: output->state = PULSE_FILEOPS_ITEM_CANCELLED; break;
        case TransferRequestItemState::Failed: output->state = PULSE_FILEOPS_ITEM_FAILED; break;
        }
        if (capacity < required || !path) return PULSE_ERROR_ARGUMENT;
        if (bytes && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                path, bytes, nullptr, nullptr) != bytes) return PULSE_ERROR_STATE;
        path[bytes] = '\0';
        return PULSE_OK;
    };
    return backend;
}
}
FileOpsAdapter::FileOpsAdapter(AppState& owner, CallerBinding caller)
    : FileOpsAdapter(std::move(caller), Backend(owner)) {}
FileOpsAdapter::FileOpsAdapter(AppState& owner, CallerBinding caller, DirectoriesAdapter& directories)
    : FileOpsAdapter(std::move(caller), Backend(owner, &directories)) {}
} // namespace pulse::extensions::native

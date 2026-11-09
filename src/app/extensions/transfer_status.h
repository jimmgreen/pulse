#pragma once
#include "../../ops/controlled_transfer.h"

namespace pulse::extensions {
enum class TransferDisplayState { Unavailable, Preparing, WaitingConfirmation, Queued, Running,
    Cancelling, Completed, Partial, Cancelled, Failed };

inline TransferDisplayState ClassifyTransferResult(const ops::ControlledTransferResult& result) {
    bool committed = false;
    bool failed = (result.error != ERROR_SUCCESS && result.error != ERROR_CANCELLED) ||
        (result.permit_error != transfer_plan::Error::None && result.permit_error != transfer_plan::Error::Cancelled);
    bool all_completed = !result.items.empty();
    bool cancelled = result.error == ERROR_CANCELLED || result.permit_error == transfer_plan::Error::Cancelled;
    for (const auto& item : result.items) {
        const bool completed = item.state == ops::ControlledTransferState::Copied || item.state == ops::ControlledTransferState::Moved;
        all_completed &= completed;
        committed |= completed || item.state == ops::ControlledTransferState::DestinationCommittedSourceRetained;
        failed |= item.state == ops::ControlledTransferState::Failed;
        cancelled |= item.state == ops::ControlledTransferState::Cancelled;
    }
    if (result.complete && all_completed && !failed && !cancelled) return TransferDisplayState::Completed;
    if (committed) return TransferDisplayState::Partial;
    if (failed) return TransferDisplayState::Failed;
    if (cancelled) return TransferDisplayState::Cancelled;
    return TransferDisplayState::Failed;
}
}

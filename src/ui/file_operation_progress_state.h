#pragma once
#include "../ops/ops_manager.h"

namespace pulse::ui {
inline bool TransferTerminal(const ops::OpStatus& status) {
    return status.phase == ops::OpPhase::Completed || status.phase == ops::OpPhase::Failed;
}
inline bool TransferIndeterminate(const ops::OpStatus& status) {
    return !TransferTerminal(status) && (status.phase == ops::OpPhase::Scanning ||
        !status.enumeration_complete || status.percent < 0.0f);
}
inline bool TransferShowsEmptyingSubtitle(const ops::OpStatus& status) {
    return status.type == ops::OpType::EmptyRecycle && !TransferTerminal(status);
}
inline bool TransferCanAutoClose(const ops::OpStatus& status) {
    return (!status.external || status.external_outcome == ops::ExternalOutcome::Completed) &&
        !status.active && status.phase == ops::OpPhase::Completed &&
        status.failed_items == 0 && status.last_error.empty();
}
}

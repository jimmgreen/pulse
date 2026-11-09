#pragma once

#include "ops_manager.h"
#include "../common/display_path.h"

namespace pulse::ops {

inline OpStatus PresentOperationStatus(OpStatus status) {
    if (status.phase == OpPhase::Completed && (!status.external || status.external_outcome == ExternalOutcome::Completed)) status.percent = 100.0f;
    status.summary = path::FriendlyPathText(status.summary);
    status.last_error = path::FriendlyPathText(status.last_error);
    status.source_label = path::FriendlyPathText(status.source_label);
    status.destination_label = path::FriendlyPathText(status.destination_label);
    status.current_item = path::FriendlyPathText(status.current_item);
    return status;
}

} // namespace pulse::ops

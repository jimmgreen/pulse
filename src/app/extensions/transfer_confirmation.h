#pragma once
#include "transfer_plan.h"
#include "../../ui/confirm_dialog.h"

namespace pulse::extensions {

enum class TransferConfirmationError { None, InvalidIdentity, InvalidPlan, SizeOverflow };
struct TransferConfirmation {
    TransferConfirmationError error = TransferConfirmationError::None;
    std::optional<ui::ConfirmDialogSpec> spec;
    explicit operator bool() const noexcept { return spec.has_value(); }
};

// Presentation only. The caller must separately validate the current principal,
// plan lifetime and receipt before execution. Neither this builder nor displaying
// its result confirms/consumes a Store record or authorizes filesystem access.
// Long rows are shortened by the existing dialog; Ctrl+C retains every row.
TransferConfirmation BuildTransferConfirmation(const transfer_plan::Plan& plan,
    const authorization::PackageIdentity& package, std::wstring_view display_name);

} // namespace pulse::extensions

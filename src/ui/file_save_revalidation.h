#pragma once

#include "file_save_model.h"
#include <array>
#include <functional>
#include <optional>

namespace pulse::ui {

struct SaveObjectIdentity {
    ULONGLONG volume = 0;
    std::array<BYTE, 16> file{};
    LONGLONG creation_time = 0;
    bool operator==(const SaveObjectIdentity&) const = default;
};

enum class SaveRevalidationState { Failed, Ready, ConfirmOverwrite };
struct SaveRevalidationResult;
class SaveConfirmationSnapshot {
public:
    SaveConfirmationSnapshot(const SaveConfirmationSnapshot&) = default;
    SaveConfirmationSnapshot(SaveConfirmationSnapshot&&) = default;
    SaveConfirmationSnapshot& operator=(const SaveConfirmationSnapshot&) = default;
    SaveConfirmationSnapshot& operator=(SaveConfirmationSnapshot&&) = default;
    const std::wstring& Path() const { return path_; }
    const std::wstring& Parent() const { return parent_; }
    size_t FilterIndex() const { return filter_index_; }
    bool NoTestFileCreate() const { return no_test_file_create_; }
    bool TargetExists() const { return target_.has_value(); }
    const SaveObjectIdentity& ParentIdentity() const { return parent_identity_; }
    const std::optional<SaveObjectIdentity>& TargetIdentity() const { return target_; }
private:
    SaveConfirmationSnapshot() = default;
    std::wstring path_, parent_;
    size_t filter_index_ = 0;
    bool caller_validates_access_ = false;
    bool no_test_file_create_ = false, require_existing_parent_ = false;
    bool parent_exists_ = true;
    SaveObjectIdentity parent_identity_;
    std::optional<SaveObjectIdentity> target_;
    friend SaveRevalidationResult CaptureSaveConfirmation(const SaveTargetRequest&, const std::function<bool()>&);
    friend SaveRevalidationResult RevalidateSaveConfirmation(const SaveConfirmationSnapshot&, const std::function<bool()>&);
};

struct SaveRevalidationResult {
    SaveRevalidationState state = SaveRevalidationState::Failed;
    DWORD error = ERROR_INVALID_PARAMETER;
    std::optional<SaveConfirmationSnapshot> snapshot; // Never present on failure.
};

// Worker-only metadata operations. No data access, write/create/truncate, or native
// validation parity promise. Final-component reparse points are unsupported.
SaveRevalidationResult CaptureSaveConfirmation(const SaveTargetRequest& request,
    const std::function<bool()>& cancelled = {});
// Call only after confirmation of this snapshot. Replacement returns a fresh
// ConfirmOverwrite snapshot; do not loop internally or silently reuse consent.
// Missing target + unchanged parent is Ready. Queries are observations, not a
// reservation: replacement, permission changes and ID reuse can race afterwards.
SaveRevalidationResult RevalidateSaveConfirmation(const SaveConfirmationSnapshot& snapshot,
    const std::function<bool()>& cancelled = {});

} // namespace pulse::ui


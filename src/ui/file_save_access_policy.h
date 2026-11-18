#pragma once
#include "file_save_revalidation.h"

namespace pulse::ui {
enum class SaveAccessState { Failed, Ready, NeedsConfirmation };
// Tracks removal of a newly-created probe, not persistence of arbitrary handles.
enum class SaveAccessCleanup { NotRequired, Confirmed, Unconfirmed };
enum class SaveAccessStage { Scope, Revalidation, BeforeOpen, OpenExisting, CreateNew,
    Created, Identity, Close, AfterClose, CleanupQuery, ParentCheck, Complete };
struct SaveAccessResult {
    SaveAccessState state = SaveAccessState::Failed;
    SaveAccessCleanup cleanup = SaveAccessCleanup::NotRequired;
    SaveAccessStage stage = SaveAccessStage::Scope;
    DWORD error = ERROR_INVALID_PARAMETER;
    SaveAccessStage primary_stage = SaveAccessStage::Scope;
    DWORD primary_error = ERROR_SUCCESS;
    DWORD cleanup_error = ERROR_SUCCESS;
    std::wstring path;
    std::optional<SaveObjectIdentity> parent_identity;
    std::optional<SaveObjectIdentity> probe_identity;
    std::optional<SaveConfirmationSnapshot> confirmation;
};
// Worker-only restricted experiment, not native parity. Existing snapshots must
// have been confirmed by the caller. New targets are briefly created/deleted.
// No retries and no path-based deletion. Failure/cleanup evidence must be kept.
// Observer is a deterministic testing seam; exceptions become failures. Never
// call UI or reenter this model from an observer. No callback occurs in DllMain.
// Shared by target and every retained ancestor metadata check.
DWORD ValidateSaveAccessAttributes(DWORD attributes, bool require_unencrypted) noexcept;
using SaveAccessObserver = std::function<void(SaveAccessStage)>;
SaveAccessResult ValidateLocalSaveAccessV1(const SaveConfirmationSnapshot& confirmed,
    const std::function<bool()>& cancelled = {}, const SaveAccessObserver& observer = {}, bool require_unencrypted = false) noexcept;
}

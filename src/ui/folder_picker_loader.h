#pragma once

#include "folder_picker_model.h"
#include "file_save_model.h"
#include "file_save_revalidation.h"
#include "file_save_access_policy.h"
#include <memory>

namespace pulse::ui {

// Admission is independent of enumeration: no entries or snapshot are copied.
struct PickerFolderCommit {
    uint64_t generation = 0;
    std::wstring path;
};

struct PickerValidation {
    uint64_t generation = 0;
    std::vector<std::wstring> paths;
    std::vector<std::wstring> missing_paths;
    std::wstring navigate;
    DWORD error = ERROR_SUCCESS;
    std::wstring failed_path;
    bool overwrite_required = false;
    bool save_access_checked = false;
    bool save_file_ok_checked = false;
    uint64_t save_ui_generation = 0;
    std::optional<SaveAccessResult> save_access_diagnostic;
    std::optional<SaveConfirmationSnapshot> save_snapshot;
};

// Two bounded workers share a latest-only queue; closing never waits on a network request.
class PickerLoader {
public:
    PickerLoader(HWND hwnd, UINT message, UINT validation_message = 0, UINT folder_commit_message = 0);
    ~PickerLoader();
    PickerLoader(const PickerLoader&) = delete;
    PickerLoader& operator=(const PickerLoader&) = delete;
    void Load(uint64_t generation, std::wstring path, PickerMode mode, PickerOptions options = {});
    void Validate(uint64_t generation, std::wstring current, std::vector<std::wstring> names,
                  PickerMode mode, PickerOptions options = {});
    void CreateFolder(uint64_t generation, std::wstring current, std::wstring name);
    void ValidateSave(uint64_t generation, SaveTargetRequest request);
    void RevalidateSave(uint64_t generation, SaveConfirmationSnapshot snapshot);
    void CheckSaveAccess(uint64_t generation, SaveConfirmationSnapshot snapshot,
                         bool after_file_ok, uint64_t ui_generation, bool require_unencrypted = false, bool skip_access_validation = false, uint32_t native_options = 0);
    static std::unique_ptr<PickerListing> Take(LPARAM lparam);
    static std::unique_ptr<PickerFolderCommit> TakeFolderCommit(LPARAM lparam);
    static std::unique_ptr<PickerValidation> TakeValidation(LPARAM lparam);
private:
    struct Target;
    std::shared_ptr<Target> target_;
};

PickerListing ReadPickerListing(const std::wstring& path, PickerMode mode, const PickerOptions& options = {});

} // namespace pulse::ui

#pragma once

#include "folder_picker_model.h"
#include <functional>
#include <optional>

namespace pulse::ui {

enum class SaveTargetState {
    Resolved, Ready, ConfirmOverwrite, InvalidName, InvalidDirectory, InvalidFilter,
    Unsupported, ParentMissing, ParentNotDirectory, TargetDirectory, IoError
};

struct SaveTargetRequest {
    std::wstring directory; // absolute filesystem directory, never the process CWD
    std::wstring filename;  // leaf or strictly parsed path; relative paths use directory
    std::vector<PickerFilter> filters;
    size_t filter_index = 0; // zero-based, like the existing picker
    std::wstring default_extension; // original SetDefaultExtension value, without a dot
    bool caller_validates_access = false; // explicit in-process policy, never inferred from filename
    bool filter_changed = false; // user changed type after the initial default was set
    bool no_test_file_create = false; // metadata/overwrite only; never create or test write access
    bool require_existing_save_parent = false;
    bool native_save_defaults = false; // negotiated current-filter extension precedence
};

struct SaveTargetResult {
    SaveTargetState state = SaveTargetState::InvalidName;
    std::wstring path;
    size_t filter_index = 0;
    DWORD error = ERROR_SUCCESS;
    bool target_exists = false;
    bool read_only = false; // independent of overwrite confirmation and caller policy
};

// No filesystem access. Supports ordinary drive/UNC paths and explicit extensions.
// Complex wildcard patterns and trailing dot/space semantics require native fallback.
SaveTargetResult ResolveSaveTarget(const SaveTargetRequest& request);

// Pure filename inference for user edits only. Never rewrites the filename or
// guesses between caller formats sharing an extension. Wildcard filters are not candidates.
std::optional<size_t> InferSaveFilterFromFilename(std::wstring_view filename,
    const std::vector<PickerFilter>& filters, size_t current_filter);

// Attribute reads only; run off the UI thread. Never creates/truncates a file.
// Ready is not a guarantee of write access. Confirmation cannot prevent later races.
SaveTargetResult ValidateSaveTarget(const SaveTargetRequest& request);

struct SaveDirectoryPlan {
    DWORD error = ERROR_SUCCESS;
    std::wstring target;
    std::vector<std::wstring> missing; // Ordered ancestors; display all before explicit create consent.
};
struct SaveDirectoryCreation {
    DWORD error = ERROR_SUCCESS;
    std::wstring failed_path;
    std::vector<std::wstring> created; // Explicitly created directories remain even if later FileOk is cancelled.
};
// Worker-only, opt-in independent directory creation. Never called by Save validation.
// Native PATHMUSTEXIST and NOTESTFILECREATE prohibit this operation.
SaveDirectoryPlan PlanSaveDirectories(const SaveTargetRequest& request, uint32_t native_options = 0,
    const std::function<bool()>& cancelled = {});
// Call only after separate user consent to approved.missing. A changed plan requires
// renewed consent. No target file is created and no existing object is removed.
SaveDirectoryCreation CreateSaveDirectories(const SaveTargetRequest& request, const SaveDirectoryPlan& approved,
    uint32_t native_options = 0, const std::function<bool()>& cancelled = {});

} // namespace pulse::ui


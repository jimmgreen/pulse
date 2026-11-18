#pragma once
#include <windows.h>
#include <cstdint>

// Read-only diagnostics. No unload or process-termination API is exposed.
// installed_entries uses runtime_config.h coverage bits, not dialog acceptance.
struct PulseFileDialogRuntimeStatus {
    uint32_t size = sizeof(PulseFileDialogRuntimeStatus);
    uint32_t version = 1;
    uint32_t process_id = 0;
    uint32_t installed_entries = 0;
    HRESULT initialization = E_PENDING;
    uint32_t ready = 0;
    uint64_t activations = 0;
    uint64_t admitted = 0;
    uint64_t rejected = 0;
};
extern "C" HRESULT WINAPI PulseFileDialogRuntimeQuery(PulseFileDialogRuntimeStatus* output, uint32_t size) noexcept;

#include "file_dialog_contract.h"
namespace pulse::shell_bridge {
// Separate opt-in for the in-process proof. The ordinary runtime never calls this.
inline bool RuntimeLocalSaveContractCertified(const ContractDecision& decision) noexcept {
    if (decision.disposition != Disposition::Prepared || !decision.request || !decision.effective_options ||
        decision.validation_category != ValidationCategory::LocalSaveAccessV1 ||
        decision.supported_access_policy != AccessPolicy::LocalSaveV1 ||
        decision.required_runtime_checks != (SupportedAccessPolicy | ShellResultContract)) return false;
    namespace wire = ipc::picker;
    const auto* request = std::get_if<wire::Request>(&decision.request->body);
    const bool caller = decision.skip_save_access_validation;
    const uint32_t required = caller ? (FOS_OVERWRITEPROMPT | FOS_NOVALIDATE) : decision.native_save_defaults ? FOS_OVERWRITEPROMPT : (FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    const uint32_t optional = (decision.no_test_file_create ? FOS_NOTESTFILECREATE : 0u) | (decision.native_save_defaults ? FOS_NOREADONLYRETURN : 0u) | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT | FOS_FORCESHOWHIDDEN | FOS_DEFAULTNOMINIMODE | kExtendedFileDialogOptions;
    const auto native = *decision.effective_options;
    if ((native & FOS_ALLNONSTORAGEITEMS) && (native & FOS_FORCEFILESYSTEM)) return false;
    if (decision.native_options != (native & kExtendedFileDialogOptions)) return false;
    if (decision.require_existing_save_parent != (decision.no_test_file_create && (native & FOS_PATHMUSTEXIST))) return false;
    if (decision.no_test_file_create && decision.require_unencrypted_save) return false;
    if (caller && decision.require_unencrypted_save) return false;
    if (decision.no_test_file_create != ((native & FOS_NOTESTFILECREATE) != 0) || (caller && decision.no_test_file_create)) return false;
    if (!request || request->operation != wire::Operation::SaveFile ||
        (native & required) != required || (native & ~(required | optional))) return false;
    const uint32_t capabilities = wire::Save | (request->filters.empty() ? 0 : wire::Filters);
    const uint32_t options = wire::OverwritePrompt | ((native & FOS_FORCESHOWHIDDEN) ? wire::ShowHidden : 0);
    return request->required_capabilities == capabilities && request->options == options;
}
inline bool RuntimeContractCertified(const ContractDecision& decision) noexcept {
    if (decision.disposition != Disposition::Prepared || !decision.request || !decision.effective_options) return false;
    if (decision.native_options != (*decision.effective_options & kExtendedFileDialogOptions)) return false;
    if ((*decision.effective_options & FOS_ALLNONSTORAGEITEMS) && (*decision.effective_options & FOS_FORCEFILESYSTEM)) return false;
    const auto* request = std::get_if<ipc::picker::Request>(&decision.request->body);
    if (!request) return false;
    namespace wire = ipc::picker;
    if (decision.validation_category == ValidationCategory::LocalOpenAccessV1 && request->operation == wire::Operation::PickFolder) {
        constexpr uint32_t required = FOS_PICKFOLDERS;
        constexpr uint32_t optional = FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR | FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT | FOS_FORCESHOWHIDDEN | FOS_ALLOWMULTISELECT | FOS_DEFAULTNOMINIMODE | FOS_NODEREFERENCELINKS | FOS_OVERWRITEPROMPT | kExtendedFileDialogOptions;
        const auto native = *decision.effective_options;
        const bool multiple = (native & FOS_ALLOWMULTISELECT) != 0;
        return decision.supported_access_policy == AccessPolicy::LocalOpenV1 &&
            decision.required_runtime_checks == (SupportedAccessPolicy | ShellResultContract) &&
            (native & required) == required && !(native & ~(required | optional)) &&
            !decision.skip_open_access_validation && request->filters.empty() && request->default_extension.empty() &&
            request->required_capabilities == (wire::Folder | (multiple ? wire::MultiSelect : 0u) | ((native & FOS_NODEREFERENCELINKS) ? wire::PreserveLinks : 0u)) &&
            request->options == (((native & FOS_FORCESHOWHIDDEN) ? wire::ShowHidden : 0u) | (multiple ? wire::AllowMultiSelect : 0u) | ((native & FOS_NODEREFERENCELINKS) ? wire::NoDereferenceLinks : 0u));
    }
    if (decision.validation_category == ValidationCategory::LocalOpenAccessV1) {
        const uint32_t required = decision.allow_missing_open ? ((*decision.effective_options & FOS_CREATEPROMPT) ? FOS_CREATEPROMPT : FOS_NOVALIDATE) : FOS_FILEMUSTEXIST | ((*decision.effective_options & FOS_NOVALIDATE) ? FOS_PATHMUSTEXIST : 0u);
        constexpr uint32_t optional = FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT | FOS_FORCESHOWHIDDEN | FOS_NODEREFERENCELINKS | FOS_ALLOWMULTISELECT | FOS_DEFAULTNOMINIMODE | FOS_NOVALIDATE | FOS_OVERWRITEPROMPT | kExtendedFileDialogOptions;
        const auto native = *decision.effective_options;

        if (decision.supported_access_policy != AccessPolicy::LocalOpenV1 ||
            decision.required_runtime_checks != (SupportedAccessPolicy | ShellResultContract) ||
            (native & required) != required || (native & ~(required | optional)) ||
            request->operation != wire::Operation::OpenFiles || !request->default_extension.empty()) return false;
        const bool filters = !request->filters.empty();
        const bool links = (native & FOS_NODEREFERENCELINKS) != 0;
        const bool multiple = (native & FOS_ALLOWMULTISELECT) != 0;
        const bool skip = (native & FOS_NOVALIDATE) != 0;
        if (skip != decision.skip_open_access_validation) return false;
        if (decision.allow_missing_open && ((!skip && !(native & FOS_CREATEPROMPT)) || (native & FOS_FILEMUSTEXIST) || (decision.open_default_extension && !decision.open_default_extension->empty()))) return false;
        if (decision.require_existing_open_parent != (decision.allow_missing_open && (native & FOS_PATHMUSTEXIST))) return false;
        const uint32_t capabilities = wire::Open | (skip ? wire::ExplicitAccessPolicy : wire::LocalOpenAccessV1) |
            (filters ? wire::Filters | wire::AdvisoryFilters : 0) | (links ? wire::PreserveLinks : 0) |
            (multiple ? wire::MultiSelect : 0);
        const uint32_t options = (skip ? wire::SkipAccessValidation : wire::ValidateLocalOpenAccessV1) |
            (filters ? wire::AdvisoryFileFilters : 0) | (links ? wire::NoDereferenceLinks : 0) |
            ((native & FOS_FORCESHOWHIDDEN) ? wire::ShowHidden : 0) | (multiple ? wire::AllowMultiSelect : 0);
        return request->required_capabilities == capabilities && request->options == options;
    }
    if (decision.supported_access_policy != AccessPolicy::None ||
        decision.required_runtime_checks != (NativeValidationParity | ShellResultContract)) return false;
    constexpr uint32_t folder = FOS_FORCEFILESYSTEM | FOS_PICKFOLDERS | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT;
    if (decision.validation_category == ValidationCategory::Folder)
        return *decision.effective_options == folder && request->operation == wire::Operation::PickFolder &&
            request->required_capabilities == wire::Folder && request->options == 0 &&
            request->filters.empty() && request->filename.empty() && request->default_extension.empty();
    if (decision.validation_category != ValidationCategory::OpenSkipAccessValidation ||
        *decision.effective_options != 0x02001948u || request->operation != wire::Operation::OpenFiles ||
        !request->default_extension.empty()) return false;
    const bool filters = !request->filters.empty();
    return request->required_capabilities == static_cast<uint32_t>(wire::Open | wire::ExplicitAccessPolicy |
               (filters ? wire::Filters | wire::AdvisoryFilters : 0)) &&
        request->options == static_cast<uint32_t>(wire::SkipAccessValidation | (filters ? wire::AdvisoryFileFilters : 0));
}
}




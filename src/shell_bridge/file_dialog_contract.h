#pragma once

#include "../ipc/picker_protocol.h"
#include <windows.h>
#include <shobjidl_core.h>
#include <optional>

namespace pulse::shell_bridge {
enum class DialogFactory { Open, Save };
enum class Setting {
    Options, FileTypes, FileTypeIndex, Folder, DefaultFolder, FileName, DefaultExtension,
    Title, OkButtonLabel, FileNameLabel, Place, ClientGuid, ClearClientData,
    Events, Customize, SaveProperties, SaveAsItem, ShellFilter, Unknown, ReadonlyOwnershipCollection
};
// A bounded snapshot of a real native setter call, in original call order.
// native_result is observed evidence, not a result invented by this model.
struct SetterCall {
    Setting setting = Setting::Unknown;
    HRESULT native_result = E_FAIL;
    uint32_t value = 0; // Original caller Options or FileTypeIndex argument, including zero.
    std::u16string text;
    std::vector<ipc::picker::Filter> filters;
    bool filesystem_folder = false; // Folder item resolved/validated by caller.
    // Independent successful native GetFileTypeIndex evidence immediately after
    // this setter. Never overwrite value or native_result with normalized data.
    std::optional<uint32_t> observed_filter_index;
};
enum class Disposition { Prepared, NativeOnly, InvalidArgument };
enum class NativeReason {
    None, SetterFailed, UnrecognizedSuccess, UnknownSetting, UnsupportedOptions,
    FilesystemNotForced, RecentItemsPolicy, MissingValidationOptions, DefaultDirectoryPolicy,
    NonFilesystemFolder, UnsupportedFilename, UnsupportedExtension, UnsupportedFilter,
    UnsupportedSettingOrder, FolderFileSettings, SaveContract, Events, Customization,
    Labels, Places, Persistence, SaveMetadata, ShellFilter, InvalidData, Limit
};
enum RuntimeCheck : uint32_t {
    NativeValidationParity = 1, // Category-specific validation and native error behavior; never waived by NOVALIDATE.
    SupportedAccessPolicy = 4, // Explicit restricted policy, never a claim of native parity.
    ShellResultContract = 2,    // IShellItem/array reconstruction belongs to wrapper.
};
enum class AccessPolicy { None, LocalOpenV1, LocalSaveV1 };
inline constexpr uint32_t kExtendedFileDialogOptions = static_cast<uint32_t>(FOS_STRICTFILETYPES | FOS_ALLNONSTORAGEITEMS |
    FOS_CREATEPROMPT | FOS_SHAREAWARE | FOS_HIDEMRUPLACES | FOS_HIDEPINNEDPLACES |
    FOS_OKBUTTONNEEDSINTERACTION | FOS_FORCEPREVIEWPANEON | FOS_SUPPORTSTREAMABLEITEMS);
// Separately observed native getter evidence, never an invented setter call.
struct PreparationContext {
    std::optional<std::u16string> resolved_initial_directory;
    bool client_state_owned = false;
    bool filesystem_places_owned = false;
    bool caller_validation_supported = false;
    bool open_missing_supported = false;
    bool save_caller_validation_supported = false;
    bool native_save_defaults_supported = false;
    bool save_no_test_create_supported = false;
    bool save_properties_owned = false;
    bool native_options_supported = false;
};
enum class ValidationCategory { Unknown, DefaultOpen, DefaultSave, Folder, OpenSkipAccessValidation, LocalOpenAccessV1, LocalSaveAccessV1 };
struct ContractDecision {
    // Settings absent from the host wire require an explicitly capable in-process provider.
    std::optional<std::u16string> title, ok_button_label, file_name_label;
    std::optional<std::u16string> open_default_extension;
    uint32_t native_options = 0; // Negotiated extended bits, bound to effective_options at runtime.
    bool require_unencrypted_save = false;
    bool skip_open_access_validation = false;
    bool allow_missing_open = false;
    bool require_existing_open_parent = false;
    bool skip_save_access_validation = false;
    bool native_save_defaults = false;
    bool no_test_file_create = false;
    bool require_existing_save_parent = false;
    AccessPolicy supported_access_policy = AccessPolicy::None;
    Disposition disposition = Disposition::NativeOnly;
    NativeReason reason = NativeReason::None;
    HRESULT observed_result = S_OK;
    size_t call_index = 0;
    std::optional<ipc::picker::Message> request;
    // Final known native options over the complete bounded setter trace, even
    // when an earlier call makes the mapping NativeOnly. Start with the factory
    // defaults; S_OK SetOptions replaces all bits, failure leaves them alone,
    // and another nonnegative HRESULT makes them unknown until a later S_OK.
    // Invalid factory or over-limit trace has no trustworthy snapshot.
    std::optional<uint32_t> effective_options;
    // Only Prepared mappings receive a category. It describes required native
    // validation, never certification that the host implements that validation.
    ValidationCategory validation_category = ValidationCategory::Unknown;
    // Prepared is a settings mapping, not permission to bypass runtime contracts.
    // Until these checks are implemented, a real wrapper must remain native.
    uint32_t required_runtime_checks = NativeValidationParity | ShellResultContract;
};
uint32_t DefaultDialogOptions(DialogFactory factory);
ContractDecision PrepareFileDialogContract(DialogFactory factory, std::span<const SetterCall> calls,
                                          const ipc::picker::RequestId& id, AccessPolicy policy = AccessPolicy::None,
                                          const PreparationContext& context = {});
// The out-of-process request for a prepared decision: decision.request plus
// local-save access and caller labels (pulse.exe applies them exactly as the
// in-process provider does). nullopt when the decision relies on something the
// pulse.exe host does not carry yet (native extended options, share-aware
// saves, Open default extension, missing-file Open), so the caller keeps its
// in-process or native route.
std::optional<ipc::picker::Message> BridgeRequestFor(const ContractDecision& decision);
}


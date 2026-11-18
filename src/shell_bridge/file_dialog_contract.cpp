#include "file_dialog_contract.h"
#include "../common/picker_open_filter.h"
#include "../common/picker_shell_namespace.h"

namespace pulse::shell_bridge {
// Native contract references:
// https://learn.microsoft.com/windows/win32/api/shobjidl_core/ne-shobjidl_core-_fileopendialogoptions
// https://learn.microsoft.com/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifiledialog-setfiletypes
// https://learn.microsoft.com/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifiledialog-setdefaultfolder
namespace {
namespace wire = ipc::picker;
ContractDecision Native(NativeReason reason, size_t index = 0, HRESULT result = S_OK) {
    ContractDecision decision;
    decision.reason = reason; decision.call_index = index; decision.observed_result = result;
    return decision;
}
ContractDecision Invalid(size_t index = 0) {
    auto decision = Native(NativeReason::InvalidData, index);
    decision.disposition = Disposition::InvalidArgument;
    return decision;
}
bool TextValid(const std::u16string& text) {
    if (text.size() > wire::kMaxStringUnits) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = text[i];
        if (!c) return false;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == text.size() || text[i] < 0xdc00 || text[i] > 0xdfff) return false;
        } else if (c >= 0xdc00 && c <= 0xdfff) return false;
    }
    return true;
}
bool AbsoluteDirectory(std::u16string_view path) {
    if (path.starts_with(u"\\\\?\\UNC\\")) return AbsoluteDirectory(u"\\\\" + std::u16string(path.substr(8)));
    if (path.starts_with(u"\\\\?\\")) path.remove_prefix(4);
    if (path.size() >= 3 && ((path[0] >= u'A' && path[0] <= u'Z') || (path[0] >= u'a' && path[0] <= u'z')) && path[1] == u':' && path[2] == u'\\') return true;
    if (!path.starts_with(u"\\\\") || path.size() < 5 || path[2] == u'.' || path[2] == u'?') return false;
    const auto end = path.find(u'\\', 2);
    return end != path.npos && end > 2 && end + 1 < path.size() && path[end + 1] != u'\\';
}
bool Pattern(std::u16string_view pattern) {
    if (pattern == u"*.*") return true;
    size_t start = 0;
    while (start < pattern.size()) {
        const auto end = pattern.find(u';', start);
        const auto part = pattern.substr(start, end == pattern.npos ? pattern.size() - start : end - start);
        if (!part.starts_with(u"*.") || part.size() <= 2 || part.back() == u'.' ||
            part.substr(2).find_first_of(u"*?<>:\"/\\|; \t\r\n") != part.npos) return false;
        if (end == pattern.npos) return true;
        start = end + 1;
    }
    return false;
}
}

uint32_t DefaultDialogOptions(DialogFactory factory) {
    // Common Item Dialog defaults, before any replacing SetOptions call.
    return factory == DialogFactory::Save ? FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR
        : FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR;
}

static ContractDecision PrepareMapping(DialogFactory factory, std::span<const SetterCall> calls, const wire::RequestId& id, AccessPolicy policy, const PreparationContext& context) {
    if (factory != DialogFactory::Open && factory != DialogFactory::Save) return Invalid();
    if (calls.size() > 256) return Native(NativeReason::Limit);
    if (policy != AccessPolicy::None && policy != AccessPolicy::LocalOpenV1 && policy != AccessPolicy::LocalSaveV1) return Invalid();
    uint32_t options = DefaultDialogOptions(factory);
    uint32_t final_options = options;
    for (const auto& call : calls)
        if (call.setting == Setting::Options && call.native_result == S_OK) final_options = call.value;
    uint32_t index = 1;
    bool types_set = false, folder_set = false, extension_set = false;
    wire::Request body;
    bool readonly_ownership = false;
    std::optional<std::u16string> title, ok_label, filename_label, open_extension;
    for (size_t i = 0; i < calls.size(); ++i) {
        const auto& call = calls[i];
        // Native folder dialogs reject both filter setters without changing state.
        if ((call.setting == Setting::FileTypes || call.setting == Setting::FileTypeIndex) &&
            call.native_result == E_UNEXPECTED && (options & FOS_PICKFOLDERS) && !types_set) continue;
        if (FAILED(call.native_result)) return Native(NativeReason::SetterFailed, i, call.native_result);
        if (call.native_result != S_OK) return Native(NativeReason::UnrecognizedSuccess, i, call.native_result);
        switch (call.setting) {
        case Setting::Options:
            options = call.value; // SetOptions replaces flags, it does not OR them.
            break;
        case Setting::FileTypes:
            if (types_set || (options & FOS_PICKFOLDERS)) return Native(NativeReason::UnsupportedSettingOrder, i);
            // Native accepts an empty list (WinForms without a Filter) and keeps
            // listing every file, exactly as if SetFileTypes was never called.
            if (call.filters.empty()) break;
            if (call.filters.size() > wire::kMaxFilters) return Invalid(i);
            for (const auto& filter : call.filters) {
                if (filter.label.empty() || !TextValid(filter.label) || !TextValid(filter.pattern)) return Invalid(i);
                if (factory == DialogFactory::Open ? !picker_filter::OpenPattern(std::u16string_view(filter.pattern)) : !Pattern(filter.pattern))
                    return Native(NativeReason::UnsupportedFilter, i);
            }
            body.filters = call.filters;
            // Both all-file spellings include extensionless names in Open.
            // Canonicalize only this exact form; do not approximate other globs.
            if (factory == DialogFactory::Open)
                for (auto& filter : body.filters) if (filter.pattern == u"*") filter.pattern = u"*.*";
            types_set = true;
            break;
        case Setting::FileTypeIndex: {
            if (!types_set) return Native(NativeReason::UnsupportedSettingOrder, i);
            const uint32_t effective = call.observed_filter_index.value_or(call.value);
            if (!effective || effective > body.filters.size()) return Invalid(i);
            index = effective;
            break;
        }
        case Setting::Folder:
            if (!TextValid(call.text)) return Invalid(i);
            if ((!call.filesystem_folder || !AbsoluteDirectory(call.text)) &&
                !(context.native_options_supported && picker_shell::Enabled(final_options) &&
                  picker_shell::IsParsingName(std::wstring_view(reinterpret_cast<const wchar_t*>(call.text.data()), call.text.size()))))
                return Native(NativeReason::NonFilesystemFolder, i);
            body.initial_directory = call.text;
            folder_set = true;
            break;
        case Setting::FileName:
            if (!TextValid(call.text)) return Invalid(i);
            if (call.text.find_first_of(u"\\/:*?\"<>|") != call.text.npos || call.text == u"." || call.text == u"..")
                return Native(NativeReason::UnsupportedFilename, i);
            body.filename = call.text;
            break;
        case Setting::DefaultExtension:
            if (!TextValid(call.text)) return Invalid(i);
            {
                auto extension = std::u16string_view(call.text);
                if (factory == DialogFactory::Save && policy == AccessPolicy::LocalSaveV1 &&
                    context.native_save_defaults_supported && extension.starts_with(u"*.")) extension.remove_prefix(2);
                if (call.text.size() > 255 || extension.find_first_of(factory == DialogFactory::Save ? u".\\/:*?\"<>|; \t\r\n" : u"\\/:\"<>\t\r\n") != extension.npos ||
                    (!call.text.empty() && extension.empty())) return Native(NativeReason::UnsupportedExtension, i);
            }
            if (factory == DialogFactory::Open) {
                for (const auto unit : call.text) if (unit < 32) return Native(NativeReason::UnsupportedExtension, i);
            }
            if (factory == DialogFactory::Open && !call.text.empty() &&
                (call.text.back() == u'.' || call.text.back() == u' '))
                return Native(NativeReason::UnsupportedExtension, i);
            if (factory == DialogFactory::Save) body.default_extension = call.text;
            else open_extension = call.text;
            extension_set = true;
            break;
        case Setting::DefaultFolder:
            if ((policy != AccessPolicy::LocalOpenV1 && policy != AccessPolicy::LocalSaveV1) || !context.resolved_initial_directory)
                return Native(NativeReason::DefaultDirectoryPolicy, i);
            break;
        case Setting::Title:
        case Setting::OkButtonLabel:
        case Setting::FileNameLabel:
            if (!TextValid(call.text) || call.text.size() > 256 || call.text.find_first_of(u"\r\n\t") != call.text.npos) return Invalid(i);
            if (call.setting == Setting::Title) title = call.text;
            else if (call.setting == Setting::OkButtonLabel) ok_label = call.text;
            else filename_label = call.text;
            break;
        case Setting::Place:
            if (!context.filesystem_places_owned) return Native(NativeReason::Places, i);
            break;
        case Setting::ClientGuid:
        case Setting::ClearClientData:
            if (!context.client_state_owned) return Native(NativeReason::Persistence, i);
            break;
        case Setting::Events: return Native(NativeReason::Events, i);
        case Setting::Customize: return Native(NativeReason::Customization, i);
        case Setting::ReadonlyOwnershipCollection:
            if (factory != DialogFactory::Save || call.value) return Native(NativeReason::SaveMetadata, i);
            readonly_ownership = true;
            break;
        case Setting::SaveProperties:
            if (factory != DialogFactory::Save || !context.save_properties_owned) return Native(NativeReason::SaveMetadata, i);
            break;
        case Setting::SaveAsItem: return Native(NativeReason::SaveMetadata, i);
        case Setting::ShellFilter: return Native(NativeReason::ShellFilter, i);
        default: return Native(NativeReason::UnknownSetting, i);
        }
    }
    constexpr uint32_t common = FOS_NOCHANGEDIR | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT | FOS_FORCESHOWHIDDEN | FOS_DEFAULTNOMINIMODE;
    const uint32_t allowed = common | (context.native_options_supported ? kExtendedFileDialogOptions : 0u) | (factory == DialogFactory::Save ? FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN | FOS_NOVALIDATE | FOS_NOTESTFILECREATE :
        FOS_PICKFOLDERS | FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT | FOS_NODEREFERENCELINKS | FOS_NOVALIDATE | FOS_OVERWRITEPROMPT);
    if ((options & ~allowed) || ((options & FOS_NOVALIDATE) && (options & FOS_PICKFOLDERS)))
        return Native(NativeReason::UnsupportedOptions);
    if (FAILED(picker_shell::ValidateOptions(options))) return Invalid();
    const bool local_open = policy == AccessPolicy::LocalOpenV1 && factory == DialogFactory::Open &&
        (!(options & FOS_NOVALIDATE) || context.caller_validation_supported);
    const bool allow_missing_open = local_open && ((options & FOS_NOVALIDATE) || (context.native_options_supported && (options & FOS_CREATEPROMPT))) && !(options & FOS_FILEMUSTEXIST) && context.open_missing_supported;
    if (allow_missing_open && open_extension && !open_extension->empty()) return Native(NativeReason::UnsupportedExtension);
    const bool local_save = policy == AccessPolicy::LocalSaveV1 && factory == DialogFactory::Save;
    const bool no_test_create = local_save && (options & FOS_NOTESTFILECREATE) && context.save_no_test_create_supported && !(options & FOS_NOVALIDATE);
    if (no_test_create && readonly_ownership) return Native(NativeReason::SaveContract);
    if ((options & FOS_NOTESTFILECREATE) && !no_test_create) return Native(NativeReason::UnsupportedOptions);
    const bool native_save = local_save && context.native_save_defaults_supported && !(options & FOS_NOVALIDATE);
    const bool caller_save = local_save && context.save_caller_validation_supported && (options & FOS_NOVALIDATE) && !(options & FOS_NOREADONLYRETURN);
    if (factory == DialogFactory::Save && (options & FOS_NOVALIDATE) && !caller_save) return Native(NativeReason::UnsupportedOptions);
    if (caller_save && readonly_ownership) return Native(NativeReason::SaveContract);
    if (!local_open && !local_save && !(options & FOS_FORCEFILESYSTEM)) return Native(NativeReason::FilesystemNotForced);
    if (!local_open && !local_save && !(options & FOS_DONTADDTORECENT)) return Native(NativeReason::RecentItemsPolicy);
    // Local COM Open preserves the process CWD with or without NOCHANGEDIR;
    // the picker resolves absolute paths and must never call SetCurrentDirectory.
    // Other policies retain their existing explicit requirement.
    const bool local_existing_file = local_open && (options & FOS_FILEMUSTEXIST) && !(options & (FOS_PICKFOLDERS | FOS_NOVALIDATE));
    if (!caller_save && !native_save && !allow_missing_open && ((!local_open && !(options & FOS_NOCHANGEDIR)) || (!(options & FOS_PATHMUSTEXIST) && !(local_open && (options & FOS_PICKFOLDERS)) && !local_existing_file))) return Native(NativeReason::MissingValidationOptions);
    if ((local_open || local_save) && context.resolved_initial_directory) {
        if (!TextValid(*context.resolved_initial_directory)) return Invalid();
        if (!AbsoluteDirectory(*context.resolved_initial_directory) &&
            !(context.native_options_supported && picker_shell::Enabled(options) &&
              picker_shell::IsParsingName(std::wstring_view(reinterpret_cast<const wchar_t*>(context.resolved_initial_directory->data()), context.resolved_initial_directory->size()))))
            return Native(NativeReason::NonFilesystemFolder);
        body.initial_directory = *context.resolved_initial_directory;
        folder_set = true;
    }
    if (!folder_set) return Native(NativeReason::DefaultDirectoryPolicy);
    if (factory == DialogFactory::Save) {
        if (!(options & FOS_OVERWRITEPROMPT) || (!caller_save && !native_save && !(options & FOS_NOREADONLYRETURN))) return Native(NativeReason::SaveContract);
        body.operation = wire::Operation::SaveFile;
        body.required_capabilities = wire::Save;
        body.options = wire::OverwritePrompt;
    } else if (options & FOS_PICKFOLDERS) {
        if (types_set || extension_set) return Native(NativeReason::FolderFileSettings);
        body.operation = wire::Operation::PickFolder;
        body.required_capabilities = wire::Folder;
        if (options & FOS_ALLOWMULTISELECT) { body.options |= wire::AllowMultiSelect; body.required_capabilities |= wire::MultiSelect; }
    } else {
        if (!(options & FOS_FILEMUSTEXIST) && !allow_missing_open) return Native(NativeReason::MissingValidationOptions);
        body.operation = wire::Operation::OpenFiles;
        body.required_capabilities = wire::Open;
        if (options & FOS_ALLOWMULTISELECT) { body.options |= wire::AllowMultiSelect; body.required_capabilities |= wire::MultiSelect; }
    }
    if ((options & FOS_NOVALIDATE) && !caller_save) {
        body.options |= wire::SkipAccessValidation;
        body.required_capabilities |= wire::ExplicitAccessPolicy;
    }
    if (options & FOS_FORCESHOWHIDDEN) body.options |= wire::ShowHidden;
    // FOS_NODEREFERENCELINKS selects the shortcut itself, not its target. Only
    // Folder mode still requires a directory and rejects shortcut files.
    if (options & FOS_NODEREFERENCELINKS) {
        body.options |= wire::NoDereferenceLinks;
        body.required_capabilities |= wire::PreserveLinks;
    }
    if (types_set) {
        body.required_capabilities |= wire::Filters;
        body.filter_index = index - 1;
        // Default native Open filters control listing, not acceptance of typed paths.
        // STRICTFILETYPES remains outside the admitted options; Save is unchanged.
        if (body.operation == wire::Operation::OpenFiles) {
            body.options |= wire::AdvisoryFileFilters;
            body.required_capabilities |= wire::AdvisoryFilters;
        }
    }
    if (local_open && !(options & (FOS_NOVALIDATE | FOS_PICKFOLDERS))) {
        body.options |= wire::ValidateLocalOpenAccessV1;
        body.required_capabilities |= wire::LocalOpenAccessV1;
    }
    wire::Message request{id, std::move(body)};
    std::vector<uint8_t> encoded;
    if (wire::Encode(request, encoded) != wire::CodecError::None) return Invalid();
    ContractDecision decision;
    decision.native_options = options & kExtendedFileDialogOptions;
    decision.require_unencrypted_save = readonly_ownership;
    decision.skip_save_access_validation = caller_save;
    decision.native_save_defaults = native_save;
    decision.no_test_file_create = no_test_create;
    decision.require_existing_save_parent = no_test_create && (options & FOS_PATHMUSTEXIST);
    decision.skip_open_access_validation = local_open && (options & FOS_NOVALIDATE);
    decision.allow_missing_open = allow_missing_open;
    decision.require_existing_open_parent = allow_missing_open && (options & FOS_PATHMUSTEXIST);
    decision.open_default_extension = std::move(open_extension);
    decision.title = std::move(title); decision.ok_button_label = std::move(ok_label); decision.file_name_label = std::move(filename_label);
    decision.disposition = Disposition::Prepared;
    decision.request = std::move(request);
    if (local_open || local_save) {
        decision.supported_access_policy = local_save ? AccessPolicy::LocalSaveV1 : AccessPolicy::LocalOpenV1;
        decision.required_runtime_checks = SupportedAccessPolicy | ShellResultContract;
    }
    return decision;
}

ContractDecision PrepareFileDialogContract(DialogFactory factory, std::span<const SetterCall> calls, const wire::RequestId& id, AccessPolicy policy, const PreparationContext& context) {
    auto decision = PrepareMapping(factory, calls, id, policy, context);
    if ((factory != DialogFactory::Open && factory != DialogFactory::Save) || calls.size() > 256) return decision;
    decision.effective_options = DefaultDialogOptions(factory);
    for (const auto& call : calls) {
        if (call.setting != Setting::Options) continue;
        if (call.native_result == S_OK) decision.effective_options = call.value;
        else if (SUCCEEDED(call.native_result)) decision.effective_options.reset();
    }
    if (decision.disposition == Disposition::Prepared && decision.effective_options) {
        decision.validation_category = decision.supported_access_policy == AccessPolicy::LocalOpenV1 ? ValidationCategory::LocalOpenAccessV1 :
            decision.supported_access_policy == AccessPolicy::LocalSaveV1 ? ValidationCategory::LocalSaveAccessV1 : factory == DialogFactory::Save ? ValidationCategory::DefaultSave :
            (*decision.effective_options & FOS_PICKFOLDERS) ? ValidationCategory::Folder :
            (*decision.effective_options & FOS_NOVALIDATE) ? ValidationCategory::OpenSkipAccessValidation : ValidationCategory::DefaultOpen;
    }
    return decision;
}

std::optional<wire::Message> BridgeRequestFor(const ContractDecision& decision) {
    if (decision.disposition != Disposition::Prepared || !decision.request || !decision.effective_options) return std::nullopt;
    if (decision.native_options || (*decision.effective_options & FOS_SHAREAWARE) || decision.allow_missing_open ||
        decision.require_existing_open_parent || decision.open_default_extension) return std::nullopt;
    auto message = *decision.request;
    auto* body = std::get_if<wire::Request>(&message.body);
    if (!body) return std::nullopt;
    if (decision.supported_access_policy == AccessPolicy::LocalSaveV1) {
        body->required_capabilities |= wire::LocalSaveAccessV1;
        body->options |= wire::ValidateLocalSaveAccessV1;
        if (decision.native_save_defaults) body->options |= wire::SaveNativeDefaults;
        if (decision.no_test_file_create) body->options |= wire::SaveNoTestFileCreate;
        if (decision.require_existing_save_parent) body->options |= wire::SaveRequireExistingParent;
        if (decision.require_unencrypted_save) body->options |= wire::SaveRequireUnencrypted;
        if (decision.skip_save_access_validation) body->options |= wire::SaveSkipAccessValidation;
    } else if (decision.native_save_defaults || decision.no_test_file_create || decision.require_existing_save_parent ||
               decision.require_unencrypted_save || decision.skip_save_access_validation) {
        return std::nullopt;
    }
    if (decision.title) { body->label_flags |= wire::LabelTitle; body->title = *decision.title; }
    if (decision.ok_button_label) { body->label_flags |= wire::LabelOkButton; body->ok_button_label = *decision.ok_button_label; }
    if (decision.file_name_label) { body->label_flags |= wire::LabelFileName; body->file_name_label = *decision.file_name_label; }
    if (body->label_flags) body->required_capabilities |= wire::DialogLabels;
    std::vector<uint8_t> encoded;
    if (wire::Encode(message, encoded) != wire::CodecError::None) return std::nullopt;
    return message;
}
}


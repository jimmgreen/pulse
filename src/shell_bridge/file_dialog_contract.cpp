#include "file_dialog_contract.h"
#include "../common/picker_save_filter.h"
#include "../common/picker_open_filter.h"
#include "../common/picker_shell_namespace.h"
#include "../common/picker_user_storage.h"

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
std::u16string NormalizeFilterSpacesAfterSemicolon(std::u16string_view pattern) {
    // Native Open/Save ignore ASCII spaces after ';', not spaces before it.
    // Keep whitespace-only segments unsupported and preserve the caller trace.
    std::u16string result;
    size_t start = 0;
    while (start < pattern.size()) {
        const auto end = pattern.find(u';', start);
        auto part = pattern.substr(start, end == pattern.npos ? pattern.size() - start : end - start);
        if (start && !part.empty()) {
            const auto first = part.find_first_not_of(u' ');
            if (first == part.npos) return std::u16string(pattern);
            part.remove_prefix(first);
        }
        result.append(part);
        if (end == pattern.npos) break;
        result.push_back(u';');
        start = end + 1;
    }
    return result;
}
bool IsComputerInitialFolder(std::u16string_view text) {
    if (text == u"pulse:computer") return true;
    constexpr std::u16string_view guid = u"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
    if (text.size() != guid.size()) return false;
    for (size_t i = 0; i < guid.size(); ++i) {
        char16_t a = text[i];
        if (a >= u'a' && a <= u'z') a = static_cast<char16_t>(a - u'a' + u'A');
        if (a != guid[i]) return false;
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
bool Filename(std::u16string_view name, const PreparationContext& context, bool local) {
    if (name.find_first_of(u"\\/:*?\"<>|") == name.npos && name != u"." && name != u"..") return true;
    // A native snapshot is essential: SetFolder and SaveAsItem can change the
    // effective state. Keep the filename intact, never invent a folder split.
    if (!local || !context.resolved_filename || !context.resolved_initial_directory || name.empty() ||
        name.back() == u'\\' || name.back() == u'/') return false;
    std::u16string path(name);
    if (!AbsoluteDirectory(path)) {
        if (path.front() == u'\\' || path.find(u':') != path.npos) return false;
        path = *context.resolved_initial_directory + u"\\" + path;
    }
    picker_user_storage::Path parsed;
    return picker_user_storage::Parse(std::wstring_view(reinterpret_cast<const wchar_t*>(path.data()), path.size()), parsed);
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
bool ShadowedSaveDefaultExtension(std::u16string_view text) {
    // Some MFC/CAD Save callers (e.g. YJK) pass pipe/semicolon-delimited extension
    // lists such as "dwg|dxf||" to SetDefaultExtension. Native IFileSaveDialog
    // accepts S_OK and never substitutes it when every filter has a concrete suffix.
    if (text.empty() || text.size() > 255) return false;
    if (text.find_first_of(u"|;") == text.npos) return false;
    for (const auto unit : text) if (unit < 32) return false;
    size_t start = 0;
    bool has_segment = false;
    while (start <= text.size()) {
        const auto end = text.find_first_of(u"|;", start);
        auto part = text.substr(start, end == text.npos ? text.size() - start : end - start);
        if (!part.empty()) {
            if (part.starts_with(u"*.")) part.remove_prefix(2);
            else if (part.starts_with(u".")) part.remove_prefix(1);
            if (part.empty() || part.back() == u'.' || part.find(u"..") != part.npos ||
                part.find_first_of(u"\\/:*?\"<>|; \t\r\n") != part.npos) {
                return false;
            }
            has_segment = true;
        }
        if (end == text.npos) break;
        start = end + 1;
    }
    return has_segment;
}
bool AllSaveFiltersConcrete(const std::vector<wire::Filter>& filters) {
    if (filters.empty()) return false;
    for (const auto& filter : filters) {
        if (filter.pattern == u"*.*" || filter.pattern == u"*" || !filter.pattern.starts_with(u"*.")) {
            return false;
        }
    }
    return true;
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
    bool save_name_masks = false;
    bool types_set = false, folder_set = false, extension_set = false;
    bool shadowed_save_extension = false;
    size_t shadowed_save_extension_index = 0;
    wire::Request body;
    bool readonly_ownership = false, shell_item_filter = false, save_as_item = false, save_metadata = false;
    size_t shell_filter_index = 0;
    std::optional<std::u16string> title, ok_label, filename_label, cancel_label, open_extension;
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
        case Setting::FileTypes: {
            if (types_set || (options & FOS_PICKFOLDERS)) return Native(NativeReason::UnsupportedSettingOrder, i);
            // Native accepts an empty list (WinForms without a Filter) and keeps
            // listing every file, exactly as if SetFileTypes was never called.
            if (call.filters.empty()) break;
            if (call.filters.size() > wire::kMaxFilters) return Invalid(i);
            body.filters = call.filters;
            const bool local_policy = (factory == DialogFactory::Save && policy == AccessPolicy::LocalSaveV1) ||
                (factory == DialogFactory::Open && policy == AccessPolicy::LocalOpenV1);
            for (auto& filter : body.filters) {
                if (!TextValid(filter.label) || !TextValid(filter.pattern)) return Invalid(i);
                // Native SetFileTypes returns S_OK for blank separator slots like {L"", L""}
                // (used by CorelDRAW in multi-filter lists). Keep the 1-based filter index
                // intact by representing the blank slot as a non-empty separator matching *.*.
                if (local_policy && body.filters.size() > 1 && filter.label.empty() && filter.pattern.empty()) {
                    filter.label = u" ";
                    filter.pattern = u"*.*";
                } else if (factory == DialogFactory::Save && policy == AccessPolicy::LocalSaveV1 &&
                           context.native_save_defaults_supported && body.filters.size() > 1) {
                    if (filter.pattern.empty() && !filter.label.empty()) filter.pattern = u"*.*";
                    else if (filter.label.empty() && !filter.pattern.empty()) filter.label = filter.pattern;
                }
                if (filter.label.empty()) return Invalid(i);
                filter.pattern = NormalizeFilterSpacesAfterSemicolon(filter.pattern);
                if (factory == DialogFactory::Open ? !picker_filter::OpenPattern(std::u16string_view(filter.pattern)) : !Pattern(filter.pattern)) {
                    const auto save_mask = picker_filter::ParseSaveMask(std::u16string_view(filter.pattern));
                    if (factory == DialogFactory::Save && policy == AccessPolicy::LocalSaveV1 &&
                        context.native_save_defaults_supported && !(final_options & FOS_NOVALIDATE) &&
                        save_mask.valid && save_mask.name_mask) { save_name_masks = true; continue; }
                    return Native(NativeReason::UnsupportedFilter, i);
                }
            }
            // Both all-file spellings include extensionless names in Open.
            // Canonicalize only this exact form; do not approximate other globs.
            if (factory == DialogFactory::Open)
                for (auto& filter : body.filters) if (filter.pattern == u"*") filter.pattern = u"*.*";
            types_set = true;
            break;
        }
        case Setting::FileTypeIndex: {
            if (!types_set) return Native(NativeReason::UnsupportedSettingOrder, i);
            const uint32_t effective = call.observed_filter_index.value_or(call.value);
            if (!effective || effective > body.filters.size()) return Invalid(i);
            index = effective;
            break;
        }
        case Setting::Folder: {
            if (!TextValid(call.text)) return Invalid(i);
            const bool computer_folder = factory == DialogFactory::Open && policy == AccessPolicy::LocalOpenV1 &&
                context.computer_initial_folder_supported && !picker_shell::Enabled(final_options) &&
                IsComputerInitialFolder(call.text);
            if ((!call.filesystem_folder || !AbsoluteDirectory(call.text)) && !computer_folder &&
                !(context.native_options_supported && picker_shell::Enabled(final_options) &&
                  picker_shell::IsParsingName(std::wstring_view(reinterpret_cast<const wchar_t*>(call.text.data()), call.text.size()))))
                return Native(NativeReason::NonFilesystemFolder, i);
            body.initial_directory = computer_folder ? std::u16string(u"pulse:computer") : call.text;
            folder_set = true;
            break;
        }
        case Setting::FileName:
            if (!TextValid(call.text)) return Invalid(i);
            {
                const bool local = policy == AccessPolicy::LocalOpenV1 || policy == AccessPolicy::LocalSaveV1;
                const auto& filename = local && context.resolved_filename ? *context.resolved_filename : call.text;
                if (!TextValid(filename)) return Invalid(i);
                if (!Filename(filename, context, local)) return Native(NativeReason::UnsupportedFilename, i);
                body.filename = filename;
            }
            break;
        case Setting::DefaultExtension:
            if (!TextValid(call.text)) return Invalid(i);
            {
                auto extension = std::u16string_view(call.text);
                if (factory == DialogFactory::Save && policy == AccessPolicy::LocalSaveV1 &&
                    context.native_save_defaults_supported && extension.starts_with(u"*.")) extension.remove_prefix(2);
                const bool compound_save = factory == DialogFactory::Save && policy == AccessPolicy::LocalSaveV1 &&
                    context.native_save_defaults_supported && !(final_options & FOS_NOVALIDATE) && !call.text.starts_with(u"*.");
                const bool unsupported_ext = call.text.size() > 255 || extension.find_first_of(factory == DialogFactory::Save ?
                    (compound_save ? u"\\/:*?\"<>|; \t\r\n" : u".\\/:*?\"<>|; \t\r\n") : u"\\/:\"<>\t\r\n") != extension.npos ||
                    (!call.text.empty() && extension.empty()) || (compound_save && !extension.empty() &&
                    (extension.back() == u'.' || extension.starts_with(u"..")));
                if (unsupported_ext) {
                    if (factory == DialogFactory::Save && policy == AccessPolicy::LocalSaveV1 &&
                        context.native_save_defaults_supported && !(final_options & FOS_NOVALIDATE) &&
                        ShadowedSaveDefaultExtension(call.text)) {
                        shadowed_save_extension = true;
                        shadowed_save_extension_index = i;
                        body.default_extension.clear();
                        extension_set = true;
                        break;
                    }
                    return Native(NativeReason::UnsupportedExtension, i);
                }
            }
            if (factory == DialogFactory::Open) {
                for (const auto unit : call.text) if (unit < 32) return Native(NativeReason::UnsupportedExtension, i);
            }
            if (factory == DialogFactory::Open && !call.text.empty() &&
                (call.text.back() == u'.' || call.text.back() == u' '))
                return Native(NativeReason::UnsupportedExtension, i);
            if (factory == DialogFactory::Save) {
                shadowed_save_extension = false;
                body.default_extension = call.text;
            }
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
        case Setting::CancelButtonLabel:
            if (!TextValid(call.text) || call.text.size() > 256 || call.text.find_first_of(u"\r\n\t") != call.text.npos) return Invalid(i);
            if (call.setting == Setting::Title) title = call.text;
            else if (call.setting == Setting::OkButtonLabel) ok_label = call.text;
            else if (call.setting == Setting::FileNameLabel) filename_label = call.text;
            else cancel_label = call.text;
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
            save_metadata = true;
            break;
        case Setting::SaveProperties:
            if (factory != DialogFactory::Save || !context.save_properties_owned) return Native(NativeReason::SaveMetadata, i);
            save_metadata = true;
            break;
        case Setting::SaveAsItem:
            if (call.value > 1) return Invalid(i);
            if (factory != DialogFactory::Save || policy != AccessPolicy::LocalSaveV1 ||
                !context.filesystem_save_as_item_owned || !context.resolved_initial_directory || !context.resolved_save_as_filename)
                return Native(NativeReason::SaveMetadata, i);
            save_as_item = true;
            break;
        case Setting::ShellFilter:
            if (call.value > 1) return Invalid(i);
            shell_item_filter = call.value != 0; shell_filter_index = i;
            break;
        default: return Native(NativeReason::UnknownSetting, i);
        }
    }
    if (save_as_item && save_metadata && !context.save_properties_owned) return Native(NativeReason::SaveMetadata);
    // SetFilter(nullptr) clears only this requirement. Earlier failed setters
    // and independent unsupported settings still keep their original gates.
    if (shell_item_filter && (!context.shell_item_filter_supported || pulse::picker_shell::ShellFilterNeedsNative(options)))
        return Native(NativeReason::ShellFilter, shell_filter_index);
    constexpr uint32_t common = FOS_NOCHANGEDIR | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT | FOS_FORCESHOWHIDDEN | FOS_DEFAULTNOMINIMODE;
    // Empirically verified on Windows 11 comdlg32!CFileSaveDialog: FOS_FILEMUSTEXIST
    // is ignored on Save (non-existent filenames are accepted with S_OK; e.g. PDFXEdit 0x11842).
    const uint32_t allowed = common | (context.native_options_supported ? kExtendedFileDialogOptions : 0u) | (factory == DialogFactory::Save ? FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN | FOS_NOVALIDATE | FOS_NOTESTFILECREATE | FOS_FILEMUSTEXIST :
        FOS_PICKFOLDERS | FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT | FOS_NODEREFERENCELINKS | FOS_NOVALIDATE | FOS_OVERWRITEPROMPT | FOS_NOTESTFILECREATE);
    if (options & ~allowed)
        return Native(NativeReason::UnsupportedOptions);
    if ((options & FOS_NOVALIDATE) && (options & FOS_PICKFOLDERS) &&
        !(policy == AccessPolicy::LocalOpenV1 && context.caller_validation_supported))
        return Native(NativeReason::UnsupportedOptions);
    if (FAILED(picker_shell::ValidateOptions(options))) return Invalid();
    const bool local_open = policy == AccessPolicy::LocalOpenV1 && factory == DialogFactory::Open &&
        (!(options & FOS_NOVALIDATE) || context.caller_validation_supported);
    const bool validated_missing_open = context.open_missing_validated_supported &&
        !(options & (FOS_NOVALIDATE | FOS_CREATEPROMPT | FOS_PICKFOLDERS));
    const bool allow_missing_open = local_open && ((options & FOS_NOVALIDATE) ||
        (context.native_options_supported && (options & FOS_CREATEPROMPT)) || validated_missing_open) &&
        !(options & FOS_FILEMUSTEXIST) && context.open_missing_supported;
    if (allow_missing_open && open_extension && !open_extension->empty() &&
        open_extension->find_first_of(u"*?|") != open_extension->npos)
        return Native(NativeReason::UnsupportedExtension);
    const bool local_save = policy == AccessPolicy::LocalSaveV1 && factory == DialogFactory::Save;
    const bool caller_save = local_save && context.save_caller_validation_supported && (options & FOS_NOVALIDATE) &&
        (!(options & FOS_NOREADONLYRETURN) || context.save_caller_readonly_supported);
    const bool no_test_create = local_save && (options & FOS_NOTESTFILECREATE) && context.save_no_test_create_supported && !(options & FOS_NOVALIDATE);
    if (no_test_create && readonly_ownership) return Native(NativeReason::SaveContract);
    // FOS_NOTESTFILECREATE is a documented no-op on Open/PickFolders and redundant
    // when Save already skips validation via FOS_NOVALIDATE.
    if (factory == DialogFactory::Save && (options & FOS_NOTESTFILECREATE) && !no_test_create && !caller_save)
        return Native(NativeReason::UnsupportedOptions);
    const bool native_save = local_save && context.native_save_defaults_supported && !(options & FOS_NOVALIDATE);
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
        const bool computer_folder = local_open && context.computer_initial_folder_supported &&
            !picker_shell::Enabled(options) && IsComputerInitialFolder(*context.resolved_initial_directory);
        if (!AbsoluteDirectory(*context.resolved_initial_directory) && !computer_folder &&
            !(context.native_options_supported && picker_shell::Enabled(options) &&
              picker_shell::IsParsingName(std::wstring_view(reinterpret_cast<const wchar_t*>(context.resolved_initial_directory->data()), context.resolved_initial_directory->size()))))
            return Native(NativeReason::NonFilesystemFolder);
        body.initial_directory = computer_folder ? std::u16string(u"pulse:computer") : *context.resolved_initial_directory;
        folder_set = true;
    }
    if (save_as_item) {
        const auto& filename = *context.resolved_save_as_filename;
        if (!TextValid(filename)) return Invalid();
        if (filename.find_first_of(u"\\/:*?\"<>|") != filename.npos || filename == u"." || filename == u"..")
            return Native(NativeReason::UnsupportedFilename);
        // Getter evidence after the complete setter sequence also represents
        // replacement/null clearing; it is not a fabricated FileName call.
        body.filename = filename;
    }
    if (!folder_set) return Native(NativeReason::DefaultDirectoryPolicy);
    if (factory == DialogFactory::Save) {
        if (shadowed_save_extension) {
            if (!native_save || !types_set || save_name_masks || !AllSaveFiltersConcrete(body.filters))
                return Native(NativeReason::UnsupportedExtension, shadowed_save_extension_index);
            body.default_extension.clear();
        }
        // The old wire format does not distinguish an explicitly empty default
        // from an absent setter. Native enables type suffixes for the former.
        if (save_name_masks && extension_set && (body.default_extension.empty() ||
            body.default_extension.starts_with(u"*."))) return Native(NativeReason::UnsupportedExtension);
        if ((!(options & FOS_OVERWRITEPROMPT) && !(local_save && context.save_no_overwrite_supported)) ||
            (!caller_save && !native_save && !(options & FOS_NOREADONLYRETURN))) return Native(NativeReason::SaveContract);
        body.operation = wire::Operation::SaveFile;
        body.required_capabilities = wire::Save;
        body.options = (options & FOS_OVERWRITEPROMPT) ? wire::OverwritePrompt : 0u;
    } else if (options & FOS_PICKFOLDERS) {
        const bool nonempty_extension = extension_set && open_extension && !open_extension->empty();
        if (types_set || nonempty_extension) return Native(NativeReason::FolderFileSettings);
        open_extension.reset();
        body.operation = wire::Operation::PickFolder;
        body.required_capabilities = wire::Folder;
        if (options & FOS_ALLOWMULTISELECT) { body.options |= wire::AllowMultiSelect; body.required_capabilities |= wire::MultiSelect; }
    } else {
        if (!(options & FOS_FILEMUSTEXIST) && !allow_missing_open) return Native(NativeReason::MissingValidationOptions);
        body.operation = wire::Operation::OpenFiles;
        body.required_capabilities = wire::Open;
        if (options & FOS_ALLOWMULTISELECT) { body.options |= wire::AllowMultiSelect; body.required_capabilities |= wire::MultiSelect; }
    }
    if ((options & FOS_NOVALIDATE) && !caller_save && !(options & FOS_PICKFOLDERS)) {
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
    decision.reject_readonly_save = caller_save && (options & FOS_NOREADONLYRETURN);
    decision.native_save_defaults = native_save;
    decision.no_test_file_create = no_test_create;
    decision.shell_item_filter = shell_item_filter;
    decision.no_overwrite_prompt = factory == DialogFactory::Save && !(options & FOS_OVERWRITEPROMPT);
    decision.require_existing_save_parent = no_test_create && (options & FOS_PATHMUSTEXIST);
    decision.skip_open_access_validation = local_open && (options & FOS_NOVALIDATE);
    decision.allow_missing_open = allow_missing_open;
    decision.require_existing_open_parent = allow_missing_open && (options & FOS_PATHMUSTEXIST);
    decision.open_default_extension = std::move(open_extension);
    decision.title = std::move(title); decision.ok_button_label = std::move(ok_label); decision.file_name_label = std::move(filename_label); decision.cancel_button_label = std::move(cancel_label);
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
    if (decision.shell_item_filter || decision.native_options || (*decision.effective_options & FOS_SHAREAWARE) || decision.allow_missing_open ||
        decision.require_existing_open_parent || decision.open_default_extension || decision.cancel_button_label || decision.no_overwrite_prompt ||
        decision.reject_readonly_save || ((*decision.effective_options & FOS_PICKFOLDERS) && decision.skip_open_access_validation)) return std::nullopt;
    auto message = *decision.request;
    auto* body = std::get_if<wire::Request>(&message.body);
    if (!body || body->initial_directory == u"pulse:computer") return std::nullopt;
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


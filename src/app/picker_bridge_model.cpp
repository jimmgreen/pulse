#include "picker_bridge_model.h"

namespace pulse::app {
namespace {
namespace wire = ipc::picker;
static_assert(sizeof(wchar_t) == sizeof(char16_t));

wire::Message Terminal(const wire::Message& request, wire::Status status,
                       wire::ErrorSource source = wire::ErrorSource::None, uint32_t error = 0) {
    wire::Response response;
    if (const auto* body = std::get_if<wire::Request>(&request.body)) response.operation = body->operation;
    response.status = status;
    response.error_source = source;
    response.error_code = error;
    return {request.request_id, std::move(response)};
}

bool SupportedPattern(std::u16string_view patterns) {
    if (patterns == u"*.*") return true;
    size_t start = 0;
    while (start < patterns.size()) {
        const auto end = patterns.find(u';', start);
        const auto part = patterns.substr(start, end == patterns.npos ? patterns.size() - start : end - start);
        // The picker uses glob matching, not the full native DOS wildcard rules.
        // Restrict the bridge to conventional extension lists until parity is proven.
        if (!part.starts_with(u"*.") || part.size() <= 2 || part[2] == u'.' || part.back() == u'.' ||
            part.substr(2).find_first_of(u"*?<>:\"/\\|; \t\r\n") != part.npos) return false;
        if (end == patterns.npos) return true;
        start = end + 1;
    }
    return false;
}

std::wstring Wide(const std::u16string& text) { return {text.begin(), text.end()}; }

bool AbsoluteFilesystemPath(std::wstring_view path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) return AbsoluteFilesystemPath(L"\\\\" + std::wstring(path.substr(8)));
    else if (path.starts_with(L"\\\\?\\")) path.remove_prefix(4);
    if (path.size() >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') ||
        (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' && path[2] == L'\\') return true;
    if (!path.starts_with(L"\\\\") || path.size() < 5 || path[2] == L'.' || path[2] == L'?') return false;
    const auto server_end = path.find(L'\\', 2);
    return server_end != path.npos && server_end > 2 && server_end + 1 < path.size() && path[server_end + 1] != L'\\';
}

} // namespace

PickerBridgePreparation PreparePickerBridge(const wire::Message& request) {
    const auto* body = std::get_if<wire::Request>(&request.body);
    if (!body) return Terminal(request, wire::Status::Failed, wire::ErrorSource::Protocol, ERROR_INVALID_DATA);
    std::vector<uint8_t> encoded;
    const auto validation = wire::Encode(request, encoded);
    if (validation == wire::CodecError::UnsupportedCapability)
        return Terminal(request, wire::Status::Unsupported, wire::ErrorSource::Win32, ERROR_NOT_SUPPORTED);
    if (validation != wire::CodecError::None)
        return Terminal(request, wire::Status::Failed, wire::ErrorSource::Protocol, ERROR_INVALID_DATA);
    constexpr uint32_t supported = wire::Open | wire::Folder | wire::Save | wire::MultiSelect | wire::Filters | wire::PreserveLinks | wire::AdvisoryFilters | wire::ExplicitAccessPolicy | wire::LocalOpenAccessV1 |
        wire::LocalSaveAccessV1 | wire::DialogLabels | wire::CallerWindow;
    if ((body->required_capabilities & ~supported) ||
        (body->operation == wire::Operation::PickFolder &&
            (!body->filename.empty() || (body->required_capabilities & (wire::MultiSelect | wire::Filters)))))
        return Terminal(request, wire::Status::Unsupported, wire::ErrorSource::Win32, ERROR_NOT_SUPPORTED);
    if (body->operation == wire::Operation::SaveFile) {
        const auto& extension = body->default_extension;
        const auto& filename = body->filename;
        // Save UI currently always asks before overwrite, rejects read-only files,
        // and accepts a single leaf name. Do not claim unsupported native options.
        if (!(body->options & wire::OverwritePrompt) || (body->required_capabilities & wire::MultiSelect) ||
            (!extension.empty() && (extension.front() == u'.' || extension.back() == u'.' ||
                extension.find_first_of(u"<>:\"/\\|?*; \t\r\n") != extension.npos)) ||
            (!filename.empty() && (filename.back() == u'.' || filename.back() == u' ' ||
                filename.find_first_of(u"<>:\"/\\|?*") != filename.npos)))
            return Terminal(request, wire::Status::Unsupported, wire::ErrorSource::Win32, ERROR_NOT_SUPPORTED);
    }
    for (const auto& filter : body->filters) {
        if (!SupportedPattern(filter.pattern))
            return Terminal(request, wire::Status::Unsupported, wire::ErrorSource::Win32, ERROR_NOT_SUPPORTED);
    }
    ui::FolderPickerSpec spec;
    spec.mode = body->operation == wire::Operation::PickFolder ? ui::PickerMode::Folder :
        body->operation == wire::Operation::SaveFile ? ui::PickerMode::Save : ui::PickerMode::File;
    spec.initial_path = Wide(body->initial_directory);
    spec.filename = Wide(body->filename);
    spec.default_extension = Wide(body->default_extension);
    spec.filter_index = body->filter_index;
    spec.show_hidden = (body->options & wire::ShowHidden) != 0;
    spec.skip_access_validation = (body->options & wire::SkipAccessValidation) != 0;
    spec.validate_local_open_access_v1 = (body->options & wire::ValidateLocalOpenAccessV1) != 0;
    spec.advisory_file_filters = (body->options & wire::AdvisoryFileFilters) != 0;
    spec.no_dereference_links = (body->options & wire::NoDereferenceLinks) != 0;
    spec.allow_multiselect = (body->options & wire::AllowMultiSelect) != 0;
    // Local save: the same spec the in-process provider builds from its V2
    // save options (picker_ui_bootstrap_v2.cpp), so both paths validate alike.
    if (body->options & wire::ValidateLocalSaveAccessV1) {
        spec.validate_local_save_access_v1 = true;
        spec.skip_access_validation = (body->options & wire::SaveSkipAccessValidation) != 0;
        spec.native_save_defaults = (body->options & wire::SaveNativeDefaults) != 0;
        spec.no_test_file_create = (body->options & wire::SaveNoTestFileCreate) != 0;
        spec.require_existing_save_parent = (body->options & wire::SaveRequireExistingParent) != 0;
        spec.require_unencrypted_save = (body->options & wire::SaveRequireUnencrypted) != 0;
    }
    spec.explicit_title = (body->label_flags & wire::LabelTitle) != 0;
    spec.has_ok_button_label = (body->label_flags & wire::LabelOkButton) != 0;
    spec.has_file_name_label = (body->label_flags & wire::LabelFileName) != 0;
    spec.title = Wide(body->title);
    spec.ok_button_label = Wide(body->ok_button_label);
    spec.file_name_label = Wide(body->file_name_label);
    for (const auto& filter : body->filters) spec.filters.push_back({Wide(filter.label), Wide(filter.pattern)});
    return spec;
}

wire::Message CompletePickerBridge(const wire::Message& request, const ui::FilePickerOutcome& outcome,
                                  wire::ErrorSource failure_source) {
    const auto prepared = PreparePickerBridge(request);
    if (const auto* rejected = std::get_if<wire::Message>(&prepared)) return *rejected;
    if (outcome.status == ui::FilePickerStatus::Cancelled) return Terminal(request, wire::Status::Cancelled);
    if (outcome.status == ui::FilePickerStatus::Failed) {
        if (failure_source != wire::ErrorSource::Win32 && failure_source != wire::ErrorSource::HResult)
            return Terminal(request, wire::Status::Failed, wire::ErrorSource::Protocol, ERROR_INVALID_DATA);
        return Terminal(request, wire::Status::Failed, failure_source, outcome.error ? outcome.error :
            (failure_source == wire::ErrorSource::HResult ? static_cast<uint32_t>(E_FAIL) : ERROR_GEN_FAILURE));
    }
    if (outcome.status != ui::FilePickerStatus::Accepted || outcome.error != ERROR_SUCCESS ||
        outcome.result.filter_index >= wire::kMaxFilters || outcome.result.paths.size() > wire::kMaxPaths)
        return Terminal(request, wire::Status::Failed, wire::ErrorSource::Protocol, ERROR_INVALID_DATA);
    auto message = Terminal(request, wire::Status::Accepted);
    auto& response = std::get<wire::Response>(message.body);
    response.filter_index = static_cast<uint32_t>(outcome.result.filter_index);
    for (const auto& path : outcome.result.paths) {
        if (path.size() > wire::kMaxStringUnits || !AbsoluteFilesystemPath(path))
            return Terminal(request, wire::Status::Failed, wire::ErrorSource::Protocol, ERROR_INVALID_DATA);
        response.paths.emplace_back(path.begin(), path.end());
    }
    if (!wire::ValidateResponse(request, message))
        return Terminal(request, wire::Status::Failed, wire::ErrorSource::Protocol, ERROR_INVALID_DATA);
    return message;
}

} // namespace pulse::app

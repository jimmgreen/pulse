#include "../common/picker_navigation.h"
#include "file_dialog_inprocess_session.h"
#include <new>
#include <cstdio>
#include <algorithm>

namespace pulse::shell_bridge {
namespace {
namespace wire = ipc::picker;
constexpr HRESULT Unsupported() { return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); }
bool ProminenceSupported(const FileDialogCustomControl& control, uint64_t features) {
    if (control.state & ~(PULSE_PICKER_UI_V2_CONTROL_ENABLED | PULSE_PICKER_UI_V2_CONTROL_VISIBLE | PULSE_PICKER_UI_V2_CONTROL_PROMINENT)) return false;
    if (!(control.state & PULSE_PICKER_UI_V2_CONTROL_PROMINENT)) return true;
    if (control.kind == PULSE_PICKER_UI_V2_CONTROL_GROUP)
        return !control.has_parent && (features & PULSE_PICKER_UI_V2_FEATURE_CUSTOM_PROMINENT_GROUP) != 0;
    const bool supported = control.kind == PULSE_PICKER_UI_V2_CONTROL_BUTTON || control.kind == PULSE_PICKER_UI_V2_CONTROL_COMBO ||
        control.kind == PULSE_PICKER_UI_V2_CONTROL_MENU || control.kind == PULSE_PICKER_UI_V2_CONTROL_CHECK;
    return supported && (features & PULSE_PICKER_UI_V2_FEATURE_CUSTOM_PROMINENT_CONTROL);
}
PulsePickerUiV2Text Text(const std::wstring& value) { return {value.data(), static_cast<uint32_t>(value.size())}; }
bool Paths(const PulsePickerUiV2Buffer& buffer, uint32_t count, const std::vector<wchar_t>& storage, std::vector<std::wstring>& paths) {
    if (count > PULSE_PICKER_UI_V2_MAX_PATHS || buffer.units > PULSE_PICKER_UI_V2_MAX_PACKED_UNITS || buffer.units >= storage.size() || storage[buffer.units]) return false;
    size_t at = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t start = at;
        while (at < buffer.units && storage[at]) ++at;
        if (at == start || at >= buffer.units || at - start > PULSE_PICKER_UI_V2_MAX_UNITS) return false;
        std::wstring path(storage.data() + start, at - start);
        for (size_t n = 0; n < path.size(); ++n) {
            const auto c = static_cast<uint16_t>(path[n]);
            if (c >= 0xD800 && c <= 0xDBFF) { if (++n >= path.size() || path[n] < 0xDC00 || path[n] > 0xDFFF) return false; }
            else if (c >= 0xDC00 && c <= 0xDFFF) return false;
        }
        paths.push_back(std::move(path)); ++at;
    }
    return at == buffer.units;
}
bool Table(const PulsePickerUiV2Api& api) {
    return api.size == sizeof(api) && api.version == 2 && !api.reserved && api.Initialize && api.ValidateConfig &&
        api.Show && api.Query && api.GetWindow && api.Close && api.Shutdown;
}
}
FileDialogInprocessSession::FileDialogInprocessSession(std::shared_ptr<const FileDialogInprocessProvider> provider, std::function<void(const char*)> diagnostic)
    : diagnostic_(std::move(diagnostic)), provider_(std::move(provider)) {}
void FileDialogInprocessSession::Report(const char* reason) const noexcept {
    try { if (diagnostic_) diagnostic_(reason); } catch (...) {}
}
FileDialogInprocessSession::~FileDialogInprocessSession() {
    if (session_) {
        try { provider_->api.Shutdown(session_); } catch (...) {} // Destructor never crosses COM with an exception.
    }
}
HRESULT FileDialogInprocessSession::Prepare(const ContractDecision& decision, HWND owner, const std::vector<FileDialogCustomControl>& controls, const std::vector<FileDialogPlace>& places, const std::vector<FileDialogPropertyView>& properties) {
    if (!provider_ || !provider_->module_lifetime || !Table(provider_->api) || session_) return Unsupported();
    if (!decision.effective_options || decision.native_options != (*decision.effective_options & kExtendedFileDialogOptions)) return Unsupported();
    const bool save = decision.validation_category == ValidationCategory::LocalSaveAccessV1 && decision.supported_access_policy == AccessPolicy::LocalSaveV1;
    const bool open = decision.validation_category == ValidationCategory::LocalOpenAccessV1 && decision.supported_access_policy == AccessPolicy::LocalOpenV1;
    const bool folder = open && decision.effective_options && (*decision.effective_options & FOS_PICKFOLDERS);
    const uint32_t required = (folder ? PULSE_PICKER_UI_V2_CAP_FOLDER_V1 : save ? PULSE_PICKER_UI_V2_CAP_SAVE_LOCAL_V1 : PULSE_PICKER_UI_V2_CAP_OPEN_LOCAL_V1) |
        PULSE_PICKER_UI_V2_CAP_STANDARD_EVENTS | PULSE_PICKER_UI_V2_CAP_STATE_QUERY | (controls.empty() && properties.empty() ? 0u : PULSE_PICKER_UI_V2_CAP_CUSTOM_BUTTONS);
    if (decision.native_options && !(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_NATIVE_OPTIONS)) return Unsupported();
    if (!provider_->allow_unverified_proof && (provider_->api.capabilities & required) != required) return Unsupported();
    if (decision.require_unencrypted_save && (!save || !(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_SAVE_UNENCRYPTED))) return Unsupported();
    if (!controls.empty() && !provider_->api.UpdateControl) return Unsupported();
    if ((decision.title || decision.ok_button_label || decision.file_name_label) &&
        !(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_CUSTOM_LABELS)) return Unsupported();
    if (decision.open_default_extension && !(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_OPEN_DEFAULT_EXTENSION)) return Unsupported();
    for (const auto& control : controls) if (control.kind == PULSE_PICKER_UI_V2_CONTROL_COMBO &&
        !(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_CUSTOM_COMBOBOX)) return Unsupported();
    for (const auto& control : controls) {
        if (!ProminenceSupported(control, provider_->api.implemented_features)) return Unsupported();
        if (control.kind == PULSE_PICKER_UI_V2_CONTROL_SEPARATOR &&
            !(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_CUSTOM_SEPARATOR)) return Unsupported();
        if(control.kind==6 && !(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_CUSTOM_RADIO))return Unsupported();
        if(control.kind==7 && !(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_CUSTOM_CHECK))return Unsupported();
        if(control.kind==8 && !(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_CUSTOM_EDIT))return Unsupported();
        if (control.kind == PULSE_PICKER_UI_V2_CONTROL_MENU && !(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_CUSTOM_MENU)) return Unsupported();
        if (control.kind == PULSE_PICKER_UI_V2_CONTROL_OPEN_DROPDOWN && !(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_OPEN_DROPDOWN)) return Unsupported();
    }
    if (decision.disposition != Disposition::Prepared || (!save && !open) ||
        decision.required_runtime_checks != (SupportedAccessPolicy | ShellResultContract) || !decision.request || !decision.effective_options) return Unsupported();
    const auto* request = std::get_if<wire::Request>(&decision.request->body);
    if (!request || request->operation != (folder ? wire::Operation::PickFolder : save ? wire::Operation::SaveFile : wire::Operation::OpenFiles)) return Unsupported();
    uint32_t expected = 0;
    if (save) {
        const bool caller = decision.skip_save_access_validation;
        if (decision.require_existing_save_parent != (decision.no_test_file_create && (*decision.effective_options & FOS_PATHMUSTEXIST))) return Unsupported();
        if (decision.no_test_file_create && (caller || decision.require_unencrypted_save || !(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_SAVE_NO_TEST_CREATE))) return Unsupported();
        if (decision.no_test_file_create != ((*decision.effective_options & FOS_NOTESTFILECREATE) != 0)) return Unsupported();
        if (decision.native_save_defaults && (caller || !(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_SAVE_NATIVE_DEFAULTS))) return Unsupported();
        if (caller && (!(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_SAVE_SKIP_ACCESS) || decision.require_unencrypted_save)) return Unsupported();
        const uint32_t required_native = caller ? (FOS_OVERWRITEPROMPT | FOS_NOVALIDATE) : decision.native_save_defaults ? FOS_OVERWRITEPROMPT : (FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
        const uint32_t optional_native = (decision.no_test_file_create ? FOS_NOTESTFILECREATE : 0u) | (decision.native_save_defaults ? FOS_NOREADONLYRETURN : 0u) | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT | FOS_FORCESHOWHIDDEN | FOS_DEFAULTNOMINIMODE | kExtendedFileDialogOptions;
        if ((*decision.effective_options & required_native) != required_native || (*decision.effective_options & ~(required_native | optional_native))) return Unsupported();
        const uint32_t options = wire::OverwritePrompt | ((*decision.effective_options & FOS_FORCESHOWHIDDEN) ? wire::ShowHidden : 0u);
        if (request->options != options) return Unsupported();
        expected = wire::Save | (request->filters.empty() ? 0u : wire::Filters);
    } else if (folder) {
        if (!request->filters.empty() || !request->default_extension.empty() || (request->options & ~(wire::ShowHidden | wire::AllowMultiSelect | wire::NoDereferenceLinks))) return Unsupported();
        expected = wire::Folder | ((request->options & wire::AllowMultiSelect) ? wire::MultiSelect : 0u) |
            ((request->options & wire::NoDereferenceLinks) ? wire::PreserveLinks : 0u);
    } else {
        // Open remains advisory; Save has its independent extension contract.
        const bool skip = decision.skip_open_access_validation;
        if (decision.allow_missing_open && ((!skip && !(*decision.effective_options & FOS_CREATEPROMPT)) || !(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_OPEN_MISSING))) return Unsupported();
        if (decision.require_existing_open_parent && !decision.allow_missing_open) return Unsupported();
        if (skip && !(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_OPEN_SKIP_ACCESS)) return Unsupported();
        const uint32_t access = skip ? wire::SkipAccessValidation : wire::ValidateLocalOpenAccessV1;
        const uint32_t supported_options = access | wire::ShowHidden | wire::NoDereferenceLinks | wire::AdvisoryFileFilters | wire::AllowMultiSelect;
        if (!request->default_extension.empty() || (request->options & ~supported_options) || !(request->options & access)) return Unsupported();
        if (((request->options & wire::AdvisoryFileFilters) != 0) != !request->filters.empty()) return Unsupported();
        expected = wire::Open | (skip ? wire::ExplicitAccessPolicy : wire::LocalOpenAccessV1) | (request->filters.empty() ? 0u : (wire::Filters | wire::AdvisoryFilters)) |
            ((request->options & wire::NoDereferenceLinks) ? wire::PreserveLinks : 0u) |
            ((request->options & wire::AllowMultiSelect) ? wire::MultiSelect : 0u);
    }
    if ((request->options & wire::AllowMultiSelect) && !(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_MULTI_SELECT)) return Unsupported();
    if (request->required_capabilities != expected || request->initial_directory.empty()) return Unsupported();
    std::vector<uint8_t> encoded;
    if (wire::Encode(*decision.request, encoded) != wire::CodecError::None) return Unsupported();
    folder_.assign(request->initial_directory.begin(), request->initial_directory.end());
    filename_.assign(request->filename.begin(), request->filename.end());
    labels_.reserve(request->filters.size()); patterns_.reserve(request->filters.size()); filters_.reserve(request->filters.size());
    for (const auto& filter : request->filters) {
        labels_.emplace_back(filter.label.begin(), filter.label.end()); patterns_.emplace_back(filter.pattern.begin(), filter.pattern.end());
    }
    for (size_t i = 0; i < labels_.size(); ++i) filters_.push_back({sizeof(PulsePickerUiV2Filter), 0, Text(labels_[i]), Text(patterns_[i])});
    if (!places.empty() && !(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_FILESYSTEM_PLACES)) return Unsupported();
    if (places.size() > PULSE_PICKER_UI_V2_MAX_PLACES) return Unsupported();
    for (const auto& place : places) if (picker_navigation::IsComputerPath(place.path) &&
        !(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_COMPUTER_PLACE)) return Unsupported();
    places_ = places;
    place_abi_.clear(); place_abi_.reserve(places_.size());
    for (const auto& place : places_) place_abi_.push_back({sizeof(PulsePickerUiV2Place), place.location,
        {place.label.data(), static_cast<uint32_t>(place.label.size())}, {place.path.data(), static_cast<uint32_t>(place.path.size())}});
    config_.places = place_abi_.data(); config_.place_count = static_cast<uint32_t>(place_abi_.size());
    if (!properties.empty() && (!save || !(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_PROPERTIES))) return Unsupported();
    if (properties.size() > PULSE_PICKER_UI_V2_MAX_PROPERTIES) return Unsupported();
    properties_ = properties;
    property_values_.resize(properties_.size()); property_abi_.clear(); property_abi_.reserve(properties_.size());
    for (size_t i = 0; i < properties_.size(); ++i) {
        const auto& field = properties_[i];
        if ((field.kind != PULSE_PICKER_UI_V2_PROPERTY_SCALAR && field.kind != PULSE_PICKER_UI_V2_PROPERTY_LIST) ||
            field.values.size() > PULSE_PICKER_UI_V2_MAX_PROPERTY_VALUES ||
            (field.kind == PULSE_PICKER_UI_V2_PROPERTY_SCALAR && field.values.size() != 1) ||
            field.label.empty() || field.label.size() > PULSE_PICKER_UI_V2_MAX_UNITS) return Unsupported();
        for (size_t j = 0; j < i; ++j) if (properties_[j].index == field.index) return Unsupported();
        for (const auto& value : field.values) if (value.size() > PULSE_PICKER_UI_V2_MAX_UNITS) return Unsupported();
        auto& values = property_values_[i]; values.clear();
        for (const auto& value : field.values) values.push_back({value.data(), static_cast<uint32_t>(value.size())});
        property_abi_.push_back({sizeof(PulsePickerUiV2Property), field.index, field.kind,
            {field.label.data(), static_cast<uint32_t>(field.label.size())}, values.data(), static_cast<uint32_t>(values.size())});
    }
    config_.properties = property_abi_.data(); config_.property_count = static_cast<uint32_t>(property_abi_.size());
    custom_ = controls;
    control_items_.resize(custom_.size()); controls_.reserve(custom_.size());
    for (size_t i = 0; i < custom_.size(); ++i) controls_.push_back(custom_[i].Abi(control_items_[i]));
    config_.controls = controls_.data(); config_.control_count = static_cast<uint32_t>(controls_.size());
    config_.native_options = (provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_NATIVE_OPTIONS) ? decision.effective_options.value_or(0) : 0u;
    config_.size = sizeof(config_); config_.version = 2; config_.owner = owner;
    config_.required_capabilities = provider_->allow_unverified_proof ? 0 : required;
    if (decision.skip_open_access_validation) config_.open_options |= PULSE_PICKER_UI_V2_OPEN_SKIP_ACCESS_VALIDATION;
    if (decision.allow_missing_open) config_.open_options |= PULSE_PICKER_UI_V2_OPEN_ALLOW_MISSING;
    if (decision.require_existing_open_parent) config_.open_options |= PULSE_PICKER_UI_V2_OPEN_REQUIRE_EXISTING_PARENT;
    config_.access_policy = folder ? PULSE_PICKER_UI_V2_ACCESS_FOLDER_V1 : save ? PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1 : PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
    if (decision.open_default_extension) {
        if (!open || save) return Unsupported();
        extension_.assign(decision.open_default_extension->begin(), decision.open_default_extension->end());
        config_.open_options |= PULSE_PICKER_UI_V2_OPEN_USE_DEFAULT_EXTENSION;
    } else extension_.assign(request->default_extension.begin(), request->default_extension.end());
    config_.default_extension = Text(extension_);
    if (save) config_.save_options = PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT | (decision.no_test_file_create ? PULSE_PICKER_UI_V2_SAVE_NO_TEST_CREATE : 0u) | (decision.require_existing_save_parent ? PULSE_PICKER_UI_V2_SAVE_REQUIRE_EXISTING_PARENT : 0u) | (decision.native_save_defaults ? PULSE_PICKER_UI_V2_SAVE_NATIVE_DEFAULTS : 0u) | (decision.skip_save_access_validation ? PULSE_PICKER_UI_V2_SAVE_SKIP_ACCESS_VALIDATION : 0u) | (decision.require_unencrypted_save ? PULSE_PICKER_UI_V2_SAVE_REQUIRE_UNENCRYPTED : 0u);
    config_.initial_folder = Text(folder_); config_.filename = Text(filename_);
    if (decision.title) { title_.assign(decision.title->begin(), decision.title->end()); config_.title = Text(title_); config_.label_flags |= PULSE_PICKER_UI_V2_LABEL_TITLE; }
    if (decision.ok_button_label) { ok_label_.assign(decision.ok_button_label->begin(), decision.ok_button_label->end()); config_.ok_button_label = Text(ok_label_); config_.label_flags |= PULSE_PICKER_UI_V2_LABEL_OK; }
    if (decision.file_name_label) { filename_label_.assign(decision.file_name_label->begin(), decision.file_name_label->end()); config_.file_name_label = Text(filename_label_); config_.label_flags |= PULSE_PICKER_UI_V2_LABEL_FILE_NAME; }
    if (request->options & wire::AllowMultiSelect) config_.open_options |= PULSE_PICKER_UI_V2_OPEN_ALLOW_MULTI_SELECT;
    config_.filters = filters_.data(); config_.filter_count = static_cast<uint32_t>(filters_.size()); config_.filter_index = request->filter_index;
    if (request->options & wire::ShowHidden) config_.open_options |= PULSE_PICKER_UI_V2_OPEN_SHOW_HIDDEN;
    if (request->options & wire::NoDereferenceLinks) config_.open_options |= PULSE_PICKER_UI_V2_OPEN_NO_DEREFERENCE_LINKS;
    // Like the UI ABI itself, callbacks/workers may outlive a failed Shutdown.
    // Pin the actual implementation module; never unload code on an error path.
    HMODULE implementation = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(provider_->api.Show), &implementation)) return HRESULT_FROM_WIN32(GetLastError());
    PulsePickerUiV2Initialize initialize{}; initialize.size = sizeof(initialize); initialize.version = 2;
    HRESULT hr = provider_->api.Initialize(&initialize, &session_);
    if (hr != S_OK || !session_) return FAILED(hr) ? hr : E_UNEXPECTED;
    const HRESULT validated = provider_->api.ValidateConfig(session_, &config_);
    if (FAILED(validated)) {
        // Read-only diagnostic probes. Never use a reduced config for Show.
        const auto report_probe = [&](const char* part, uint32_t index, const PulsePickerUiV2Config& probe) {
            const HRESULT probe_hr = provider_->api.ValidateConfig(session_, &probe);
            char detail[176]{};
            std::snprintf(detail, sizeof(detail), "config-probe part=%s index=%u hr=0x%08lX controls=%u properties=%u filters=%u", part, index,
                static_cast<unsigned long>(probe_hr), probe.control_count, probe.property_count, probe.filter_count);
            Report(detail);
            return probe_hr;
        };
        auto base = config_;
        base.controls = nullptr; base.control_count = 0;
        base.properties = nullptr; base.property_count = 0;
        base.places = nullptr; base.place_count = 0;
        base.filters = nullptr; base.filter_count = 0; base.filter_index = 0;
        base.label_flags = 0; base.title = {}; base.ok_button_label = {}; base.file_name_label = {};
        report_probe("base", 0, base);
        auto probe = base; probe.label_flags = config_.label_flags; probe.title = config_.title;
        probe.ok_button_label = config_.ok_button_label; probe.file_name_label = config_.file_name_label;
        report_probe("labels", 0, probe);
        probe = base; probe.filters = config_.filters; probe.filter_count = config_.filter_count; probe.filter_index = config_.filter_index;
        report_probe("filters", config_.filter_index, probe);
        probe = base; probe.places = config_.places; probe.place_count = config_.place_count;
        report_probe("places", 0, probe);
        probe = base; probe.controls = config_.controls; probe.control_count = config_.control_count;
        if (FAILED(report_probe("controls", 0, probe))) {
            for (uint32_t i = 0; i < config_.control_count; ++i) {
                probe.control_count = i + 1;
                report_probe("control-prefix", config_.controls[i].id, probe);
            }
        }
        probe = base; probe.properties = config_.properties; probe.property_count = config_.property_count;
        if (FAILED(report_probe("properties", 0, probe))) {
            for (uint32_t i = 0; i < config_.property_count; ++i) {
                probe.properties = config_.properties + i; probe.property_count = 1;
                report_probe("property", config_.properties[i].index, probe);
            }
        }
    }
    return validated;
}
HRESULT FileDialogInprocessSession::Observe(const PulsePickerUiV2Event& event) {
    char diagnostic[256]{};
    std::snprintf(diagnostic, sizeof(diagnostic), "observe-header kind=%lu size_ok=%u reserved=%u show=%llu generation=%llu active_kind=%lu",
        static_cast<unsigned long>(event.kind), event.size == sizeof(event) ? 1u : 0u,
        (event.reserved[0] || event.reserved[1]) ? 1u : 0u, event.show_id, event.generation, static_cast<unsigned long>(event_kind_));
    Report(diagnostic);
    if (event.size != sizeof(event) || event.reserved[0] || event.reserved[1] || !event.show_id || !event.generation ||
        event.kind < PULSE_PICKER_UI_V2_EVENT_READY || event.kind > PULSE_PICKER_UI_V2_EVENT_NAVIGATION_COMPLETE || event_kind_ ||
        (event.kind != PULSE_PICKER_UI_V2_EVENT_NAVIGATION_COMPLETE && event.navigation_result != S_OK)) {
        Report("observe-failed stage=header"); return E_UNEXPECTED;
    }
    if (event.kind == PULSE_PICKER_UI_V2_EVENT_PROPERTY_CHANGED) {
        const auto field = std::find_if(properties_.begin(), properties_.end(), [&](const auto& value) { return value.index == event.control_id; });
        if (!(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_PROPERTIES) || field == properties_.end() ||
            event.property_value_count > PULSE_PICKER_UI_V2_MAX_PROPERTY_VALUES ||
            (event.property_value_count && !event.property_values) ||
            (field->kind == PULSE_PICKER_UI_V2_PROPERTY_SCALAR && event.property_value_count != 1)) return E_INVALIDARG;
        uint64_t total = 0;
        for (uint32_t i = 0; i < event.property_value_count; ++i) {
            const auto& value = event.property_values[i];
            total += value.units;
            if (value.units > PULSE_PICKER_UI_V2_MAX_UNITS || total > PULSE_PICKER_UI_V2_MAX_INPUT_UNITS || (value.units && !value.data)) return E_INVALIDARG;
            for (uint32_t j = 0; j < value.units; ++j) {
                const auto ch = static_cast<uint16_t>(value.data[j]);
                if (!ch || (ch >= 0xDC00 && ch <= 0xDFFF)) return E_INVALIDARG;
                if (ch >= 0xD800 && ch <= 0xDBFF && (++j >= value.units || value.data[j] < 0xDC00 || value.data[j] > 0xDFFF)) return E_INVALIDARG;
            }
        }
    } else if (event.property_value_count || event.property_values) return E_INVALIDARG;
    if (show_ && show_ != event.show_id) { Report("observe-failed stage=show-id"); return E_UNEXPECTED; }
    HWND actual = nullptr;
    const HRESULT hr = provider_->api.GetWindow(session_, event.show_id, &actual);
    DWORD pid = 0;
    const DWORD thread = actual ? GetWindowThreadProcessId(actual, &pid) : 0;
    std::snprintf(diagnostic, sizeof(diagnostic), "observe-window hr=0x%08lX present=%u equal=%u samepid=%u samethread=%u",
        static_cast<unsigned long>(hr), actual ? 1u : 0u, actual == event.window ? 1u : 0u,
        pid == GetCurrentProcessId() ? 1u : 0u, thread == GetCurrentThreadId() ? 1u : 0u);
    Report(diagnostic);
    const bool identity = provider_->validate_remote_window ? provider_->validate_remote_window(session_, actual) :
        thread == GetCurrentThreadId() && pid == GetCurrentProcessId();
    if (hr != S_OK || actual != event.window || !actual || !identity) {
        Report("observe-failed stage=window"); return E_UNEXPECTED;
    }
    show_ = event.show_id; event_kind_ = event.kind; event_generation_ = event.generation;
    return S_OK;
}
HRESULT FileDialogInprocessSession::ObserveOverwrite(const PulsePickerUiV2OverwriteEvent& event) {
    if (config_.access_policy != PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1 || event.size != sizeof(event) || event.reserved ||
        !event.target.data || !event.target.units || event.target.units > PULSE_PICKER_UI_V2_MAX_UNITS) return E_INVALIDARG;
    PulsePickerUiV2Event common{}; common.size = sizeof(common); common.kind = PULSE_PICKER_UI_V2_EVENT_READY;
    common.show_id = event.show_id; common.generation = event.generation; common.window = event.window;
    const HRESULT hr = Observe(common);
    if (hr == S_OK) event_kind_ = 0xFFFFFFFFu; // Internal overwrite scope, never FileOk/GetResult eligibility.
    return hr;
}
HRESULT FileDialogInprocessSession::ObserveShareViolation(const PulsePickerUiV2OverwriteEvent& event) {
    if (!(config_.native_options & FOS_SHAREAWARE) || event.size != sizeof(event) || event.reserved ||
        !event.target.data || !event.target.units || event.target.units > PULSE_PICKER_UI_V2_MAX_UNITS) return E_INVALIDARG;
    PulsePickerUiV2Event common{}; common.size = sizeof(common); common.kind = PULSE_PICKER_UI_V2_EVENT_READY;
    common.show_id = event.show_id; common.generation = event.generation; common.window = event.window;
    const HRESULT hr = Observe(common);
    if (hr == S_OK) event_kind_ = 0xFFFFFFFEu; // Internal share-violation scope, never FileOk/GetResult eligibility.
    return hr;
}
HRESULT FileDialogInprocessSession::Read(FileDialogInprocessSnapshot& output) const noexcept {
    try {
        if (!session_ || !show_) { Report("snapshot-failed stage=session-show"); return E_UNEXPECTED; }
        const auto failed = [&](const char* stage, HRESULT hr) {
            char detail[128]{}; std::snprintf(detail, sizeof(detail), "snapshot-failed stage=%s hr=0x%08lX", stage, static_cast<unsigned long>(hr)); Report(detail); return hr;
        };
        PulsePickerUiV2Snapshot size{}; size.size = sizeof(size);
        HRESULT hr = provider_->api.Query(session_, show_, event_generation_, &size);
        if (hr != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && hr != S_OK) return failed("query-size", hr);
        if (size.folder.units > PULSE_PICKER_UI_V2_MAX_UNITS || size.filename.units > PULSE_PICKER_UI_V2_MAX_UNITS ||
            size.selection.units > PULSE_PICKER_UI_V2_MAX_UNITS || size.candidate.units > PULSE_PICKER_UI_V2_MAX_UNITS ||
            size.selection_paths.units > PULSE_PICKER_UI_V2_MAX_PACKED_UNITS || size.candidate_paths.units > PULSE_PICKER_UI_V2_MAX_PACKED_UNITS || size.selection_count > PULSE_PICKER_UI_V2_MAX_PATHS || size.candidate_count > PULSE_PICKER_UI_V2_MAX_PATHS ||
            size.show_id != show_ || !size.generation) return failed("size-metadata", E_UNEXPECTED);
        std::vector<wchar_t> folder(size.folder.units + 1), filename(size.filename.units + 1), selection(size.selection.units + 1), candidate(size.candidate.units + 1);
        std::vector<wchar_t> selections(size.selection_paths.units + 1), candidates(size.candidate_paths.units + 1);
        PulsePickerUiV2Snapshot full{}; full.size = sizeof(full);
        full.folder = {folder.data(), static_cast<uint32_t>(folder.size()), 0}; full.filename = {filename.data(), static_cast<uint32_t>(filename.size()), 0};
        full.selection = {selection.data(), static_cast<uint32_t>(selection.size()), 0}; full.candidate = {candidate.data(), static_cast<uint32_t>(candidate.size()), 0};
        full.selection_paths = {selections.data(), static_cast<uint32_t>(selections.size()), 0};
        full.candidate_paths = {candidates.data(), static_cast<uint32_t>(candidates.size()), 0};
        hr = provider_->api.Query(session_, show_, size.generation, &full);
        if (hr != S_OK) return failed("query-data", hr);
        constexpr uint32_t flags = PULSE_PICKER_UI_V2_STATE_VALIDATING | PULSE_PICKER_UI_V2_STATE_CANDIDATE_READY | PULSE_PICKER_UI_V2_STATE_CLOSING;
        if (full.show_id != show_ || full.generation != size.generation || full.reserved || (full.flags & ~flags) ||
            full.folder.units >= folder.size() || full.filename.units >= filename.size() ||
            full.selection.units >= selection.size() || full.candidate.units >= candidate.size() ||
            folder[full.folder.units] || filename[full.filename.units] || selection[full.selection.units] || candidate[full.candidate.units]) return failed("data-metadata", E_UNEXPECTED);
        HWND actual = nullptr; hr = GetWindow(&actual);
        if (hr != S_OK || actual != full.window) return failed("window", E_UNEXPECTED);
        output = {full.show_id, full.generation, full.window, full.flags, full.filter_index,
            std::wstring(folder.data(), full.folder.units), std::wstring(filename.data(), full.filename.units),
            std::wstring(selection.data(), full.selection.units), std::wstring(candidate.data(), full.candidate.units)};
        if (!Paths(full.selection_paths, full.selection_count, selections, output.selections) ||
            !Paths(full.candidate_paths, full.candidate_count, candidates, output.candidates)) return failed("packed-paths", E_UNEXPECTED);
        const bool packed = (provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_MULTI_SELECT) != 0;
        if (!packed) { if (output.selections.empty() && !output.selection.empty()) output.selections.push_back(output.selection); if (output.candidates.empty() && !output.candidate.empty()) output.candidates.push_back(output.candidate); }
        if ((output.selections.empty() ? std::wstring{} : output.selections.front()) != output.selection ||
            (output.candidates.empty() ? std::wstring{} : output.candidates.front()) != output.candidate ||
            (!(config_.open_options & PULSE_PICKER_UI_V2_OPEN_ALLOW_MULTI_SELECT) && (output.selections.size() > 1 || output.candidates.size() > 1))) return failed("packed-first", E_UNEXPECTED);
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; } catch (...) { return E_FAIL; }
}
HRESULT FileDialogInprocessSession::CompleteFileOk(HRESULT result) {
    accepted_candidate_.clear(); accepted_paths_.clear(); accepted_generation_ = 0;
    if (result != S_OK || close_requested_) return result;
    FileDialogInprocessSnapshot snapshot; const HRESULT hr = Read(snapshot);
    if (FAILED(hr)) return hr;
    if (!(snapshot.flags & PULSE_PICKER_UI_V2_STATE_CANDIDATE_READY) || snapshot.candidate.empty()) return E_UNEXPECTED;
    accepted_paths_ = snapshot.candidates;
    accepted_candidate_ = snapshot.candidate; accepted_generation_ = snapshot.generation; accepted_index_ = snapshot.filter_index;
    return S_OK;
}
HRESULT FileDialogInprocessSession::GetWindow(HWND* output) const {
    if (!output) return E_POINTER; *output = nullptr;
    return session_ && show_ ? provider_->api.GetWindow(session_, show_, output) : E_UNEXPECTED;
}
HRESULT FileDialogInprocessSession::UpdateControl(const FileDialogCustomControl& control) {
    if (!session_ || !show_ || close_requested_ || !provider_->api.UpdateControl) return E_UNEXPECTED;
    if (!ProminenceSupported(control, provider_->api.implemented_features)) return Unsupported();
    std::vector<PulsePickerUiV2ControlItem> items;
    const auto abi = control.Abi(items);
    return provider_->api.UpdateControl(session_, show_, &abi);
}
HRESULT FileDialogInprocessSession::Navigate(const std::wstring& folder) {
    if (!session_ || !show_ || close_requested_) return E_UNEXPECTED;
    if (!(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_NAVIGATE) || !provider_->api.Navigate) return Unsupported();
    if (folder.empty() || folder.size() > PULSE_PICKER_UI_V2_MAX_UNITS) return E_INVALIDARG;
    return provider_->api.Navigate(session_, show_, {folder.data(), static_cast<uint32_t>(folder.size())});
}
HRESULT FileDialogInprocessSession::Close(HRESULT result) {
    if (close_requested_) return S_OK;
    if (!session_ || !show_) return E_UNEXPECTED;
    const HRESULT hr = provider_->api.Close(session_, show_, result);
    if (SUCCEEDED(hr)) { close_requested_ = true; close_result_ = result; }
    return hr;
}
HRESULT FileDialogInprocessSession::Show(const PulsePickerUiV2Callbacks& callbacks, PulsePickerUiV2Result& result, std::wstring& path) {
    std::vector<wchar_t> buffer(PULSE_PICKER_UI_V2_MAX_UNITS + 1);
    result = {}; result.size = sizeof(result); result.path = {buffer.data(), static_cast<uint32_t>(buffer.size()), 0};
    std::vector<wchar_t> packed(PULSE_PICKER_UI_V2_MAX_PACKED_UNITS + 1);
    result.paths = {packed.data(), static_cast<uint32_t>(packed.size()), 0};
    Report("inprocess-api-show-enter");
    HRESULT hr = E_FAIL;
    try { hr = provider_->api.Show(session_, &config_, &callbacks, &result); }
    catch (...) { Report("inprocess-api-show-exception"); throw; }
    char diagnostic[224]{};
    std::snprintf(diagnostic, sizeof(diagnostic), "inprocess-api-show-return hr=0x%08lX status=%lu flags=%lu win32=%lu terminal=0x%08lX",
        static_cast<unsigned long>(hr), static_cast<unsigned long>(result.status), static_cast<unsigned long>(result.flags),
        static_cast<unsigned long>(result.win32_error), static_cast<unsigned long>(result.terminal_hr));
    Report(diagnostic);
    if (result.path.units >= buffer.size() || buffer[result.path.units] || result.terminal_hr != hr ||
        (result.flags & ~PULSE_PICKER_UI_V2_UI_SHOWN)) return E_UNEXPECTED;
    if (result.status == PULSE_PICKER_UI_V2_ACCEPTED) {
        if (hr != S_OK || close_requested_ || !show_ || result.show_id != show_ || !result.path.units || !(result.flags & PULSE_PICKER_UI_V2_UI_SHOWN)) return E_UNEXPECTED;
        path.assign(buffer.data(), result.path.units);
        std::vector<std::wstring> paths;
        if (!Paths(result.paths, result.path_count, packed, paths)) return E_UNEXPECTED;
        if (paths.empty() && !(provider_->api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_MULTI_SELECT)) paths.push_back(path);
        if (paths != accepted_paths_) return E_UNEXPECTED;
        if (path != accepted_candidate_ || result.generation != accepted_generation_ || result.filter_index != accepted_index_) return E_UNEXPECTED;
    } else {
        if (result.path.units || buffer[0] || result.paths.units || result.path_count || packed[0]) return E_UNEXPECTED;
        if (result.status == PULSE_PICKER_UI_V2_CANCELLED && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) return E_UNEXPECTED;
        else if (result.status == PULSE_PICKER_UI_V2_FAILED && !FAILED(hr)) return E_UNEXPECTED;
        else if (result.status == PULSE_PICKER_UI_V2_CLOSED && (!close_requested_ || hr != close_result_)) return E_UNEXPECTED;
        else if (result.status != PULSE_PICKER_UI_V2_CANCELLED && result.status != PULSE_PICKER_UI_V2_FAILED && result.status != PULSE_PICKER_UI_V2_CLOSED) return E_UNEXPECTED;
    }
    result.path.data = nullptr; result.path.capacity = 0; result.paths.data = nullptr; result.paths.capacity = 0;
    return hr;
}
}


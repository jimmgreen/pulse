#include "../common/runtime_log.h"
#include <shlobj.h>
#include <mutex>
#include "../common/picker_user_storage.h"
#include "../common/picker_navigation.h"
#include "../common/picker_shell_namespace.h"
#include "../common/picker_open_filter.h"
#include "picker_ui_api_v2.h"
#include "picker_property_panel.h"
#include "picker_ui_environment.h"
#include "caller_activation_scope.h"
#include "../common/picker_owner_guard.h"
#include "../ui/folder_picker_dialog.h"
#include "../ui/file_save_model.h"
#include "../ui/typography.h"
#include "../app/picker_appearance_ui.h"
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#if defined(PULSE_PICKER_INPROCESS_PROOF)
#include <cstdio>
#endif

namespace {
thread_local DWORD remote_parent_pid = 0;
void LogOwnerContext(const pulse::PickerOwnerGuard& guard, HWND owner) noexcept {
    const auto identity = guard.Diagnostics();
    if (!guard.Error() && (!identity.identity_available || identity.caller_integrity == identity.owner_integrity)) return;
    const DWORD saved = GetLastError();
    try {
        static std::once_flag once;
        std::call_once(once, [] {
            PWSTR root = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &root))) {
                pulse::diagnostics::runtime::Initialize(std::wstring(root) + L"\\Pulse", "picker_ui");
                CoTaskMemFree(root);
            }
        });
        DWORD pid = 0;
        const DWORD thread = owner ? GetWindowThreadProcessId(owner, &pid) : 0;
        pulse::diagnostics::runtime::Event(guard.Error() ? "owner_guard_failed" : "owner_guard_observed", {
            {"stage", static_cast<uint32_t>(guard.FailureStage())}, {"error", guard.Error()},
            {"identity_available", identity.identity_available ? 1u : 0u},
            {"session_available", identity.session_available ? 1u : 0u},
            {"same_user", identity.same_user ? 1u : 0u}, {"same_session", identity.same_session ? 1u : 0u},
            {"caller_integrity", identity.caller_integrity}, {"owner_integrity", identity.owner_integrity},
            {"valid", owner && IsWindow(owner) ? 1u : 0u},
            {"same_process", pid == GetCurrentProcessId() ? 1u : 0u},
            {"same_thread", thread == GetCurrentThreadId() ? 1u : 0u},
            {"enabled", owner && IsWindowEnabled(owner) ? 1u : 0u}});
    } catch (...) { }
    SetLastError(saved);
}
void ProofContextFailure(const char* step, DWORD error) {
#if defined(PULSE_PICKER_INPROCESS_PROOF)
    const DWORD previous_error = GetLastError();
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"PULSE_PICKER_UI_DIAGNOSTICS", enabled, 2) == 1 && enabled[0] == L'1') {
        std::fprintf(stderr, "picker-context step=%s last_error=%lu\n", step, static_cast<unsigned long>(error));
        std::fflush(stderr);
    }
    SetLastError(previous_error);
#else
    (void)step; (void)error;
#endif
}
using pulse::ui::PickerDialogEvent;
using pulse::ui::PickerDialogSnapshot;
// No verified compatibility advertisement until targeted tests and native oracle.
constexpr uint32_t capabilities = 0;
constexpr uint32_t known_capabilities = PULSE_PICKER_UI_V2_CAP_OPEN_LOCAL_V1 |
    PULSE_PICKER_UI_V2_CAP_STANDARD_EVENTS | PULSE_PICKER_UI_V2_CAP_STATE_QUERY | PULSE_PICKER_UI_V2_CAP_CUSTOM_BUTTONS | PULSE_PICKER_UI_V2_CAP_SAVE_LOCAL_V1 | PULSE_PICKER_UI_V2_CAP_FOLDER_V1;
std::atomic<uint64_t> next_session{1};
std::atomic<uint64_t> live_session{0};
std::atomic<DWORD> live_thread{0};
uint64_t ReserveSessionId() {
    uint64_t value = next_session.load(std::memory_order_relaxed);
    while (value && !next_session.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) {}
    return value; // Zero permanently exhausts the token space; never recycle.
}
struct CallbackContext {
    using SetDpi = DPI_AWARENESS_CONTEXT (WINAPI*)(DPI_AWARENESS_CONTEXT);
    SetDpi set_dpi = nullptr;
    DPI_AWARENESS_CONTEXT caller_dpi = nullptr;
    HANDLE activation = INVALID_HANDLE_VALUE;
    pulse::picker_inprocess::CallerActivation caller_activation;
    ULONG_PTR cookie = 0;
    LANGID caller_language = 0, ui_language = 0;
    bool language_entered = false;
    HRESULT Enter(const pulse::picker_inprocess::Environment& env) {
        if (!caller_activation.Capture()) return HRESULT_FROM_WIN32(GetLastError());
        const auto user = GetModuleHandleW(L"user32.dll");
        set_dpi = user ? reinterpret_cast<SetDpi>(GetProcAddress(user, "SetThreadDpiAwarenessContext")) : nullptr;
        if (!set_dpi) { ProofContextFailure("enter-dpi-api-unavailable", GetLastError()); return HRESULT_FROM_WIN32(ERROR_CALL_NOT_IMPLEMENTED); }
        caller_dpi = set_dpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (!caller_dpi) { ProofContextFailure("enter-ui-dpi", GetLastError()); return HRESULT_FROM_WIN32(ERROR_FUNCTION_FAILED); }
        caller_language = GetThreadUILanguage(); ui_language = env.ui_language;
        if (!SetThreadUILanguage(ui_language)) { ProofContextFailure("enter-ui-language", GetLastError()); return HRESULT_FROM_WIN32(ERROR_FUNCTION_FAILED); }
        language_entered = true; activation = env.activation;
        if (!ActivateActCtx(activation, &cookie)) { ProofContextFailure("enter-ui-activation", GetLastError()); return HRESULT_FROM_WIN32(GetLastError()); }
        return S_OK;
    }
    HRESULT Invoke(PulsePickerUiV2Callback callback, void* context, const PulsePickerUiV2Event& event) {
        // WndProc dispatch may have automatic activation frames above our Show
        // frame. Never pop the outer frame from a nested callback. Overlay the
        // captured caller context and unwind only this callback-owned top frame.
        pulse::picker_inprocess::CallerActivationFrame callback_frame(caller_activation);
        if (!callback_frame.Entered()) {
            ProofContextFailure("invoke-push-caller-activation", GetLastError());
            return HRESULT_FROM_WIN32(ERROR_FUNCTION_FAILED);
        }
        const bool caller_dpi_entered = set_dpi(caller_dpi) != nullptr;
        if (!caller_dpi_entered) ProofContextFailure("invoke-enter-caller-dpi", GetLastError());
        const bool caller_entered = caller_dpi_entered && SetThreadUILanguage(caller_language) != 0;
        if (caller_dpi_entered && !caller_entered) ProofContextFailure("invoke-enter-caller-language", GetLastError());
        HRESULT hr = HRESULT_FROM_WIN32(ERROR_FUNCTION_FAILED);
        if (caller_entered) {
            try { hr = callback(context, &event);
                if (event.kind == PULSE_PICKER_UI_V2_EVENT_BUTTON_CLICKED || event.kind == PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED || event.kind == PULSE_PICKER_UI_V2_EVENT_CONTROL_ACTIVATING || event.kind == PULSE_PICKER_UI_V2_EVENT_CHECK_TOGGLED) hr = S_OK;
            } catch (...) { hr = E_FAIL; }
            caller_language = GetThreadUILanguage();
        }
        const auto outgoing = set_dpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (!outgoing) {
            ProofContextFailure("invoke-restore-ui-dpi", GetLastError());
            return HRESULT_FROM_WIN32(ERROR_FUNCTION_FAILED);
        }
        caller_dpi = outgoing;
        if (!SetThreadUILanguage(ui_language)) {
            ProofContextFailure("invoke-restore-ui-language", GetLastError());
            return HRESULT_FROM_WIN32(ERROR_FUNCTION_FAILED);
        }
        return hr;
    }
    ~CallbackContext() {
        if (cookie) DeactivateActCtx(0, cookie);
        if (language_entered) SetThreadUILanguage(caller_language);
        if (caller_dpi) set_dpi(caller_dpi);
    }
};
struct PendingEvent { PickerDialogEvent kind; std::wstring proposed; uint64_t generation; uint32_t control_id; uint32_t item_id; std::wstring control_text; };
struct ActiveShow {
    pulse::picker_inprocess::PropertyPanel* properties = nullptr;
    uint64_t id = 0;
    uint32_t native_options = 0;
    HWND window = nullptr;
    HANDLE cancel = nullptr;
    bool shown = false, closed = false, in_callback = false, multi_select = false, local_save = false;
    HRESULT close_hr = S_OK, callback_error = S_OK;
    PulsePickerUiV2Callbacks callbacks{};
    PickerDialogSnapshot snapshot;
    std::deque<PendingEvent> pending;
    CallbackContext* context = nullptr;
    PulsePickerUiV2Result* output = nullptr;
    ~ActiveShow() { if (cancel) CloseHandle(cancel); }
};
struct Session {
    PulsePickerUiV2Session id = 0;
    uint64_t next_show = 1;
    pulse::picker_inprocess::Environment environment;
    ActiveShow* active = nullptr;
    bool show_in_progress = false;
};
thread_local std::unique_ptr<Session> session;
HRESULT CheckSession(PulsePickerUiV2Session id) {
    if (!id || live_session.load(std::memory_order_acquire) != id) return E_INVALIDARG;
    if (live_thread.load(std::memory_order_acquire) != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    return session && session->id == id ? S_OK : E_UNEXPECTED;
}
HRESULT CheckShow(PulsePickerUiV2Session id, uint64_t show) {
    const HRESULT hr = CheckSession(id);
    if (FAILED(hr)) return hr;
    if (!session->active || session->active->id != show) return HRESULT_FROM_WIN32(ERROR_RETRY);
    return S_OK;
}
bool CopyText(PulsePickerUiV2Text input, std::wstring& value, uint64_t& total) {
    if (input.units > PULSE_PICKER_UI_V2_MAX_UNITS || (input.units && !input.data)) return false;
    total += input.units;
    if (total > PULSE_PICKER_UI_V2_MAX_INPUT_UNITS) return false;
    for (uint32_t i = 0; i < input.units; ++i) {
        const uint16_t c = static_cast<uint16_t>(input.data[i]);
        if (!c) return false;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == input.units || input.data[i] < 0xdc00 || input.data[i] > 0xdfff) return false;
        } else if (c >= 0xdc00 && c <= 0xdfff) return false;
    }
    if (input.units) value.assign(input.data, input.units); else value.clear();
    return true;
}
bool UserFolderSyntax(const std::wstring& folder, uint32_t native_options = 0) {
    if (pulse::picker_shell::Enabled(native_options) && pulse::picker_shell::IsParsingName(folder)) return true;
    pulse::picker_user_storage::Path path;
    return pulse::picker_user_storage::Parse(folder, path);
}
bool LocalFolderSyntax(const std::wstring& folder) {
    if (folder.size() < 3 || folder[1] != L':' || folder[2] != L'\\' ||
        !((folder[0] >= L'A' && folder[0] <= L'Z') || (folder[0] >= L'a' && folder[0] <= L'z'))) return false;
    return folder.find_first_of(L"/*?\"<>|") == folder.npos;
}
bool FilterSyntax(const std::wstring& pattern, bool save) {
    if (!save) return pulse::picker_filter::OpenPattern(std::wstring_view(pattern));
    if (pattern.empty()) return false;
    size_t begin = 0;
    while (begin < pattern.size()) {
        const size_t end = pattern.find(L';', begin);
        const auto item = pattern.substr(begin, end == pattern.npos ? end : end - begin);
        if (item != L"*" && item != L"*.*") {
            if (item.size() < 3 || item[0] != L'*' || item[1] != L'.' ||
                item.find_first_of(L"*?\\/:\"<>| ", 2) != item.npos) return false;
        }
        if (end == pattern.npos) return true;
        begin = end + 1;
    }
    return false;
}
HRESULT CopyControl(const PulsePickerUiV2Control* input, pulse::ui::PickerCustomControl& copied, uint64_t& total) {
    if (!input) return E_POINTER;
    if (input->size != sizeof(*input)) return E_INVALIDARG;
    const bool supports_prominent =
        (input->kind == PULSE_PICKER_UI_V2_CONTROL_GROUP || input->kind == PULSE_PICKER_UI_V2_CONTROL_BUTTON ||
         input->kind == PULSE_PICKER_UI_V2_CONTROL_COMBO || input->kind == PULSE_PICKER_UI_V2_CONTROL_MENU ||
         input->kind == PULSE_PICKER_UI_V2_CONTROL_CHECK);
    if ((input->kind != PULSE_PICKER_UI_V2_CONTROL_GROUP && input->kind != PULSE_PICKER_UI_V2_CONTROL_BUTTON && input->kind != PULSE_PICKER_UI_V2_CONTROL_COMBO && input->kind != PULSE_PICKER_UI_V2_CONTROL_MENU && input->kind != PULSE_PICKER_UI_V2_CONTROL_OPEN_DROPDOWN && input->kind != PULSE_PICKER_UI_V2_CONTROL_RADIO && input->kind != PULSE_PICKER_UI_V2_CONTROL_CHECK && input->kind != PULSE_PICKER_UI_V2_CONTROL_EDIT && input->kind != PULSE_PICKER_UI_V2_CONTROL_SEPARATOR) ||
        input->has_parent > 1 || (input->state & ~(supports_prominent ? 7u : 3u)) || input->reserved[0] || input->reserved[1] ||
        (!input->has_parent && input->parent_id) || (input->kind == PULSE_PICKER_UI_V2_CONTROL_GROUP && input->has_parent)) return E_INVALIDARG;
    if (input->label.units > 256 || !CopyText(input->label, copied.label, total) ||
        copied.label.find_first_of(L"\r\n\t") != copied.label.npos) return E_INVALIDARG;
    if(input->has_selected>1 || input->item_count>PULSE_PICKER_UI_V2_MAX_CONTROL_ITEMS || (input->item_count && !input->items) ||
        (!input->has_selected && input->selected_item_id) || (((input->kind<3 || input->kind>6) && (input->item_count || input->has_selected)) || (input->kind==4 && input->has_selected))) return E_INVALIDARG;
    if(input->checked>1 || (input->kind!=7 && input->checked) || (input->kind!=8 && input->text.units) || !CopyText(input->text,copied.text,total)) return E_INVALIDARG;
    copied.checked=input->checked!=0;
    bool selected_found=!input->has_selected;
    for(uint32_t i=0;i<input->item_count;++i) {
        const auto& item=input->items[i];
        if(item.size!=sizeof(item)) return E_INVALIDARG;
        pulse::ui::PickerCustomItem value;value.id=item.id;value.state=item.state;
        if(item.reserved || (item.state&~3u) || item.label.units>256 || !CopyText(item.label,value.label,total) || value.label.find_first_of(L"\r\n\t")!=value.label.npos) return E_INVALIDARG;
        for(const auto& previous:copied.items) if(previous.id==value.id) return E_INVALIDARG;
        selected_found|=input->has_selected && input->selected_item_id==value.id;
        copied.items.push_back(std::move(value));
    }
    if(!selected_found) return E_INVALIDARG;
    copied.has_selected=input->has_selected!=0;copied.selected_item_id=input->selected_item_id;
    copied.kind = input->kind; copied.id = input->id; copied.parent_id = input->parent_id;
    copied.has_parent = input->has_parent != 0; copied.state = input->state & 3u;
    copied.prominent = (input->state & PULSE_PICKER_UI_V2_CONTROL_PROMINENT) != 0;
    return S_OK;
}
HRESULT Prepare(const PulsePickerUiV2Config* input, pulse::ui::FolderPickerSpec& spec, pulse::picker_inprocess::PropertyPanel* properties = nullptr) {
    if (!input) return E_POINTER;
    if (input->size != sizeof(*input)) return E_INVALIDARG;
    if (input->version != PULSE_PICKER_UI_V2_VERSION || input->reserved[0] || input->reserved[1] ||
        (input->appearance & ~PULSE_PICKER_UI_V2_DARK)) return E_INVALIDARG;
    if (input->native_options & ~PULSE_PICKER_UI_V2_NATIVE_OPTIONS_MASK) return E_INVALIDARG;
    spec.native_options = input->native_options;
    if (FAILED(pulse::picker_shell::ValidateOptions(spec.native_options))) return E_INVALIDARG;
    const bool folder = input->access_policy == PULSE_PICKER_UI_V2_ACCESS_FOLDER_V1;
    if (folder && (input->filter_count || input->default_extension.units || (input->open_options & ~(PULSE_PICKER_UI_V2_OPEN_SHOW_HIDDEN | PULSE_PICKER_UI_V2_OPEN_ALLOW_MULTI_SELECT | PULSE_PICKER_UI_V2_OPEN_NO_DEREFERENCE_LINKS)))) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    const bool save = input->access_policy == PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;
    spec.allow_missing_open = (input->open_options & PULSE_PICKER_UI_V2_OPEN_ALLOW_MISSING) != 0;
    spec.require_existing_open_parent = (input->open_options & PULSE_PICKER_UI_V2_OPEN_REQUIRE_EXISTING_PARENT) != 0;
    if ((spec.allow_missing_open && (folder || save || (!(input->open_options & PULSE_PICKER_UI_V2_OPEN_SKIP_ACCESS_VALIDATION) && !(input->native_options & FOS_CREATEPROMPT)))) ||
        (spec.require_existing_open_parent && !spec.allow_missing_open)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    spec.allow_multiselect=(input->open_options&PULSE_PICKER_UI_V2_OPEN_ALLOW_MULTI_SELECT)!=0;
    spec.use_open_default_extension=(input->open_options&PULSE_PICKER_UI_V2_OPEN_USE_DEFAULT_EXTENSION)!=0;
    if(save && (spec.allow_multiselect || spec.use_open_default_extension || (input->open_options & PULSE_PICKER_UI_V2_OPEN_SKIP_ACCESS_VALIDATION))) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if ((input->required_capabilities & ~known_capabilities) ||
        (input->required_capabilities & ~capabilities) ||
        (!folder && !save && input->access_policy != PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1) ||
        (input->open_options & ~(PULSE_PICKER_UI_V2_OPEN_SHOW_HIDDEN | PULSE_PICKER_UI_V2_OPEN_NO_DEREFERENCE_LINKS | PULSE_PICKER_UI_V2_OPEN_ALLOW_MULTI_SELECT | PULSE_PICKER_UI_V2_OPEN_USE_DEFAULT_EXTENSION | PULSE_PICKER_UI_V2_OPEN_SKIP_ACCESS_VALIDATION | PULSE_PICKER_UI_V2_OPEN_ALLOW_MISSING | PULSE_PICKER_UI_V2_OPEN_REQUIRE_EXISTING_PARENT)))
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    const bool caller_save = save && (input->save_options & PULSE_PICKER_UI_V2_SAVE_SKIP_ACCESS_VALIDATION);
    if (caller_save && (input->save_options & PULSE_PICKER_UI_V2_SAVE_REQUIRE_UNENCRYPTED)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    spec.no_test_file_create = (input->save_options & PULSE_PICKER_UI_V2_SAVE_NO_TEST_CREATE) != 0;
    spec.require_existing_save_parent = (input->save_options & PULSE_PICKER_UI_V2_SAVE_REQUIRE_EXISTING_PARENT) != 0;
    if ((spec.no_test_file_create && (!save || caller_save || (input->save_options & PULSE_PICKER_UI_V2_SAVE_REQUIRE_UNENCRYPTED))) ||
        (spec.require_existing_save_parent && !spec.no_test_file_create)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    spec.require_unencrypted_save=(input->save_options&PULSE_PICKER_UI_V2_SAVE_REQUIRE_UNENCRYPTED)!=0;
    spec.native_save_defaults=(input->save_options&PULSE_PICKER_UI_V2_SAVE_NATIVE_DEFAULTS)!=0;
    if (caller_save && spec.native_save_defaults) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (save ? ((input->save_options & ~(PULSE_PICKER_UI_V2_SAVE_REQUIRE_UNENCRYPTED | PULSE_PICKER_UI_V2_SAVE_SKIP_ACCESS_VALIDATION | PULSE_PICKER_UI_V2_SAVE_NATIVE_DEFAULTS | PULSE_PICKER_UI_V2_SAVE_NO_TEST_CREATE | PULSE_PICKER_UI_V2_SAVE_REQUIRE_EXISTING_PARENT)) != PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT ||
        (input->open_options & PULSE_PICKER_UI_V2_OPEN_NO_DEREFERENCE_LINKS)) :
        (input->save_options != 0 || (!spec.use_open_default_extension && input->default_extension.units != 0))) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (input->filter_count > PULSE_PICKER_UI_V2_MAX_FILTERS ||
        (input->filter_count && !input->filters) ||
        (input->filter_count ? input->filter_index >= input->filter_count : input->filter_index != 0)) return E_INVALIDARG;
    uint64_t total = 0;
    if (!CopyText(input->initial_folder, spec.initial_path, total) ||
        !CopyText(input->filename, spec.filename, total) || !CopyText(input->title, spec.title, total) ||
        !CopyText(input->default_extension, spec.default_extension, total)) return E_INVALIDARG;
    if(input->label_flags&~7u) return E_INVALIDARG;
    if((!(input->label_flags&PULSE_PICKER_UI_V2_LABEL_OK) && input->ok_button_label.units) ||
       (!(input->label_flags&PULSE_PICKER_UI_V2_LABEL_FILE_NAME) && input->file_name_label.units)) return E_INVALIDARG;
    if(input->ok_button_label.units>1024 || input->file_name_label.units>1024 ||
        !CopyText(input->ok_button_label,spec.ok_button_label,total) || !CopyText(input->file_name_label,spec.file_name_label,total) ||
        spec.ok_button_label.find_first_of(L"\r\n\t")!=spec.ok_button_label.npos || spec.file_name_label.find_first_of(L"\r\n\t")!=spec.file_name_label.npos) return E_INVALIDARG;
    if (input->place_count > PULSE_PICKER_UI_V2_MAX_PLACES || (input->place_count && !input->places)) return E_INVALIDARG;
    for (uint32_t i = 0; i < input->place_count; ++i) {
        const auto& place = input->places[i];
        if (place.size != sizeof(place)) return E_INVALIDARG;
        pulse::ui::PickerPlace copied;
        if ((place.location != PULSE_PICKER_UI_V2_PLACE_BOTTOM && place.location != PULSE_PICKER_UI_V2_PLACE_TOP) ||
            !CopyText(place.label, copied.label, total) || !CopyText(place.path, copied.path, total) ||
            copied.label.empty() || copied.label.find_first_of(L"\r\n\t") != copied.label.npos) return E_INVALIDARG;
        if (!(save ? LocalFolderSyntax(copied.path) : UserFolderSyntax(copied.path, spec.native_options)) && !pulse::picker_navigation::IsComputerPath(copied.path)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        if (place.location == PULSE_PICKER_UI_V2_PLACE_TOP) spec.places.insert(spec.places.begin(), std::move(copied));
        else spec.bottom_places.push_back(std::move(copied));
    }
    spec.explicit_title=(input->label_flags&PULSE_PICKER_UI_V2_LABEL_TITLE)!=0;
    spec.has_ok_button_label=(input->label_flags&PULSE_PICKER_UI_V2_LABEL_OK)!=0;
    spec.has_file_name_label=(input->label_flags&PULSE_PICKER_UI_V2_LABEL_FILE_NAME)!=0;
    if (!(save ? LocalFolderSyntax(spec.initial_path) : UserFolderSyntax(spec.initial_path, spec.native_options)) || (!spec.no_test_file_create && spec.filename.find_first_of(L"\\/:*?\"<>|") != spec.filename.npos))
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    for (uint32_t i = 0; i < input->filter_count; ++i) {
        const auto& filter = input->filters[i];
        if (filter.size != sizeof(filter)) return E_INVALIDARG;
        if (filter.reserved) return E_INVALIDARG;
        pulse::ui::PickerFilter copied;
        if (!CopyText(filter.label, copied.label, total) || !CopyText(filter.pattern, copied.pattern, total)) return E_INVALIDARG;
        if (copied.label.empty() || !FilterSyntax(copied.pattern, save)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        spec.filters.push_back(std::move(copied));
    }
    if (input->control_count > 16 || (input->control_count && !input->controls)) return E_INVALIDARG;
    for (uint32_t i = 0; i < input->control_count; ++i) {
        pulse::ui::PickerCustomControl copied;
        const HRESULT hr = CopyControl(&input->controls[i], copied, total);
        if (FAILED(hr)) return hr;
        bool parent_found = !copied.has_parent;
        for (const auto& previous : spec.custom_controls) {
            if (previous.id == copied.id) return E_INVALIDARG;
            if (copied.has_parent && previous.id == copied.parent_id && previous.kind == 1) parent_found = true;
        }
        if (!parent_found) return E_INVALIDARG;
        spec.custom_controls.push_back(std::move(copied));
    }
    if (save) {
        pulse::ui::SaveTargetRequest request;
        request.directory = spec.initial_path; request.filename = spec.filename.empty() ? L"pulse-validation" : spec.filename;
        request.filters = spec.filters; request.filter_index = input->filter_index; request.default_extension = spec.default_extension;
        request.native_save_defaults = spec.native_save_defaults;
        request.no_test_file_create = spec.no_test_file_create;
        request.require_existing_save_parent = spec.require_existing_save_parent;
        const auto resolved = pulse::ui::ResolveSaveTarget(request);
        if (resolved.state != pulse::ui::SaveTargetState::Resolved) return HRESULT_FROM_WIN32(resolved.error ? resolved.error : ERROR_NOT_SUPPORTED);
    }
    if (input->property_count > PULSE_PICKER_UI_V2_MAX_PROPERTIES || (input->property_count && !input->properties)) return E_INVALIDARG;
    if (input->property_count && !save) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    std::vector<pulse::picker_inprocess::PropertyField> fields;
    for (uint32_t i = 0; i < input->property_count; ++i) {
        const auto& source = input->properties[i];
        if (source.size != sizeof(source)) return E_INVALIDARG;
        if ((source.kind != 1 && source.kind != 2) || source.value_count > PULSE_PICKER_UI_V2_MAX_PROPERTY_VALUES ||
            (source.value_count && !source.values) || (source.kind == 1 && source.value_count != 1)) return E_INVALIDARG;
        if (std::any_of(fields.begin(), fields.end(), [&](const auto& f) { return f.index == source.index; })) return E_INVALIDARG;
        pulse::picker_inprocess::PropertyField field; field.index = source.index; field.kind = source.kind;
        if (!CopyText(source.label, field.label, total) || field.label.empty()) return E_INVALIDARG;
        for (uint32_t j = 0; j < source.value_count; ++j) {
            std::wstring value; if (!CopyText(source.values[j], value, total)) return E_INVALIDARG;
            field.values.push_back(std::move(value));
        }
        fields.push_back(std::move(field));
    }
    if (properties) { properties->fields = std::move(fields); properties->Build(spec.custom_controls); }
    spec.mode = folder ? pulse::ui::PickerMode::Folder : save ? pulse::ui::PickerMode::Save : pulse::ui::PickerMode::File;
    spec.persist_layout = true;
    spec.filter_index = input->filter_index;
    spec.skip_access_validation = caller_save || (!save && (input->open_options & PULSE_PICKER_UI_V2_OPEN_SKIP_ACCESS_VALIDATION));
    spec.validate_local_open_access_v1 = !folder && !save && !spec.skip_access_validation;
    spec.validate_local_save_access_v1 = save;
    spec.advisory_file_filters = true;
    spec.show_hidden = (input->open_options & PULSE_PICKER_UI_V2_OPEN_SHOW_HIDDEN) != 0;
    spec.no_dereference_links = (input->open_options & PULSE_PICKER_UI_V2_OPEN_NO_DEREFERENCE_LINKS) != 0;
    return S_OK;
}
uint32_t EventKind(PickerDialogEvent kind) {
    switch (kind) {
    case PickerDialogEvent::Ready: return PULSE_PICKER_UI_V2_EVENT_READY;
    case PickerDialogEvent::FolderChanging: return PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGING;
    case PickerDialogEvent::FolderChanged: return PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGED;
    case PickerDialogEvent::SelectionChanged: return PULSE_PICKER_UI_V2_EVENT_SELECTION_CHANGED;
    case PickerDialogEvent::TypeChanged: return PULSE_PICKER_UI_V2_EVENT_TYPE_CHANGED;
    case PickerDialogEvent::FileOk: return PULSE_PICKER_UI_V2_EVENT_FILE_OK;
    case PickerDialogEvent::ButtonClicked: return PULSE_PICKER_UI_V2_EVENT_BUTTON_CLICKED;
    case PickerDialogEvent::ItemSelected: return PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED;
    case PickerDialogEvent::ControlActivating: return PULSE_PICKER_UI_V2_EVENT_CONTROL_ACTIVATING;
    case PickerDialogEvent::CheckToggled: return PULSE_PICKER_UI_V2_EVENT_CHECK_TOGGLED;
    case PickerDialogEvent::EditChanged: return PULSE_PICKER_UI_V2_EVENT_EDIT_CHANGED;
    case PickerDialogEvent::OpenDropDownSelected: return PULSE_PICKER_UI_V2_EVENT_OPEN_DROPDOWN_SELECTED;
    default: return 0;
    }
}
HRESULT Dispatch(ActiveShow& frame, PickerDialogEvent kind, const std::wstring& proposed) {
    if (frame.closed || FAILED(frame.callback_error) || !frame.callbacks.callback) return frame.callback_error;
    PulsePickerUiV2Event event{}; event.size = sizeof(event); event.kind = EventKind(kind);
    event.control_id = (kind == PickerDialogEvent::ButtonClicked || kind == PickerDialogEvent::ItemSelected || kind == PickerDialogEvent::ControlActivating || kind == PickerDialogEvent::CheckToggled || kind == PickerDialogEvent::EditChanged || kind == PickerDialogEvent::OpenDropDownSelected) ? frame.snapshot.control_id : 0;
    event.item_id = (kind == PickerDialogEvent::ItemSelected || kind == PickerDialogEvent::CheckToggled || kind == PickerDialogEvent::OpenDropDownSelected) ? frame.snapshot.item_id : 0;
    const std::wstring control_text = kind==PickerDialogEvent::EditChanged ? frame.snapshot.control_text : std::wstring();
    if(kind==PickerDialogEvent::EditChanged) event.control_text={control_text.data(),static_cast<uint32_t>(control_text.size())};
    event.show_id = frame.id; event.generation = frame.snapshot.generation; event.window = frame.window;
    event.proposed_folder = {proposed.empty() ? nullptr : proposed.c_str(), static_cast<uint32_t>(proposed.size())};
    if (frame.properties) {
        bool owned = false;
        const HRESULT translated = frame.properties->Translate(frame.window, kind, frame.snapshot, event, owned);
        if (FAILED(translated)) { frame.callback_error = translated; return translated; }
        if (owned && !event.kind) return S_OK;
        event.generation = frame.snapshot.generation;
    }
    frame.in_callback = true;
    const HRESULT hr = frame.context->Invoke(frame.callbacks.callback, frame.callbacks.context, event);
    frame.in_callback = false;
    if (frame.closed) return S_OK;
    const bool vetoable = kind == PickerDialogEvent::FolderChanging || kind == PickerDialogEvent::FileOk;
    if (hr != S_OK && !(vetoable && hr == S_FALSE)) frame.callback_error = FAILED(hr) ? hr : E_UNEXPECTED;
    return FAILED(frame.callback_error) ? frame.callback_error : hr;
}
HRESULT Notify(ActiveShow& frame, HWND window, PickerDialogEvent kind,
    const PickerDialogSnapshot& snapshot, const std::wstring& proposed) {
    frame.window = window;
    if (kind == PickerDialogEvent::Shown) {
        frame.shown = true;
        if (frame.output) frame.output->flags |= PULSE_PICKER_UI_V2_UI_SHOWN;
        return S_OK;
    }
    frame.snapshot = snapshot;
    if (kind == PickerDialogEvent::Snapshot) return S_OK;
    if (kind == PickerDialogEvent::Ready) frame.shown = true;
    if (frame.in_callback) {
        // The UI defers navigation before FolderChanging. Never grant a nested vetoable action.
        if (kind == PickerDialogEvent::FolderChanging || kind == PickerDialogEvent::FileOk) return S_FALSE;
        if (frame.pending.size() >= 64) {
            frame.callback_error = E_OUTOFMEMORY; SetEvent(frame.cancel); return E_OUTOFMEMORY;
        }
        frame.pending.push_back({kind, proposed, snapshot.generation, snapshot.control_id, snapshot.item_id, snapshot.control_text}); return S_OK;
    }
    const HRESULT hr = Dispatch(frame, kind, proposed);
    while (!frame.pending.empty() && !frame.closed && SUCCEEDED(frame.callback_error)) {
        auto pending = std::move(frame.pending.front()); frame.pending.pop_front();
        if (pending.generation != frame.snapshot.generation) {
            // Do not deliver a historical transition with a newer query snapshot,
            // or roll Query backward. This unsupported reentry sequence fails closed.
            frame.callback_error = HRESULT_FROM_WIN32(ERROR_RETRY);
            SetEvent(frame.cancel); break;
        }
        frame.snapshot.control_id = pending.control_id;
        frame.snapshot.item_id = pending.item_id;
        frame.snapshot.control_text = std::move(pending.control_text);
        Dispatch(frame, pending.kind, pending.proposed);
    }
    return FAILED(frame.callback_error) ? frame.callback_error : hr;
}
struct OverwriteInvocation {
    ActiveShow* frame;
    PulsePickerUiV2OverwriteEvent event;
    uint32_t response = PULSE_PICKER_UI_V2_OVERWRITE_DEFAULT;
    bool sharing = false;
};
HRESULT WINAPI InvokeOverwrite(void* opaque, const PulsePickerUiV2Event*) {
    auto& invocation = *static_cast<OverwriteInvocation*>(opaque);
    const auto callback = invocation.sharing ? invocation.frame->callbacks.share_violation : invocation.frame->callbacks.overwrite;
    const HRESULT hr = callback(invocation.frame->callbacks.context, &invocation.event, &invocation.response);
    if (hr == E_NOTIMPL) { invocation.response = PULSE_PICKER_UI_V2_OVERWRITE_DEFAULT; return S_OK; }
    return hr;
}
HRESULT NotifyOverwrite(ActiveShow& frame, HWND window, const PickerDialogSnapshot& snapshot,
                         const std::wstring& target, uint32_t& response, bool sharing = false) {
    response = PULSE_PICKER_UI_V2_OVERWRITE_DEFAULT;
    frame.window = window; frame.snapshot = snapshot;
    if (frame.closed) return S_OK;
    if (FAILED(frame.callback_error)) return frame.callback_error;
    if (!(sharing ? frame.callbacks.share_violation : frame.callbacks.overwrite)) return S_OK;
    if (frame.in_callback) return HRESULT_FROM_WIN32(ERROR_BUSY);
    OverwriteInvocation invocation{&frame, {sizeof(PulsePickerUiV2OverwriteEvent), 0, frame.id,
        snapshot.generation, window, {target.c_str(), static_cast<uint32_t>(target.size())}}};
    invocation.sharing = sharing;
    PulsePickerUiV2Event context_event{}; context_event.size = sizeof(context_event);
    frame.in_callback = true;
    const HRESULT hr = frame.context->Invoke(InvokeOverwrite, &invocation, context_event);
    frame.in_callback = false;
    if (frame.closed) return S_OK;
    if (hr != S_OK || invocation.response > PULSE_PICKER_UI_V2_OVERWRITE_REFUSE) {
        frame.callback_error = FAILED(hr) ? hr : E_UNEXPECTED;
        SetEvent(frame.cancel); return frame.callback_error;
    }
    response = invocation.response;
    // Reentry notifications must never be paired with an unrelated later state.
    while (!frame.pending.empty() && !frame.closed && SUCCEEDED(frame.callback_error)) {
        auto pending = std::move(frame.pending.front()); frame.pending.pop_front();
        if (pending.generation != frame.snapshot.generation) {
            frame.callback_error = HRESULT_FROM_WIN32(ERROR_RETRY); SetEvent(frame.cancel); break;
        }
        frame.snapshot.control_id = pending.control_id;
        frame.snapshot.item_id = pending.item_id;
        frame.snapshot.control_text = std::move(pending.control_text);
        Dispatch(frame, pending.kind, pending.proposed);
    }
    return frame.callback_error;
}
HRESULT WINAPI Initialize(const PulsePickerUiV2Initialize* input, PulsePickerUiV2Session* output) {
    if (!output) return E_POINTER;
    *output = 0;
    if (!input) return E_POINTER;
    if (input->size != sizeof(*input)) return E_INVALIDARG;
    if (input->version != PULSE_PICKER_UI_V2_VERSION || input->reserved[0] || input->reserved[1]) return E_INVALIDARG;
    if (session) return HRESULT_FROM_WIN32(ERROR_BUSY);
    try {
        auto created = std::make_unique<Session>();
        const HRESULT hr = pulse::picker_inprocess::AcquireEnvironment(created->environment);
        if (FAILED(hr)) return hr;
        created->id = ReserveSessionId();
        if (!created->id) { pulse::picker_inprocess::ReleaseEnvironment(); return E_UNEXPECTED; }
        *output = created->id; session = std::move(created);
        live_thread.store(GetCurrentThreadId(), std::memory_order_release);
        live_session.store(session->id, std::memory_order_release);
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}
HRESULT WINAPI ValidateConfig(PulsePickerUiV2Session id, const PulsePickerUiV2Config* input) {
    const HRESULT hr = CheckSession(id); if (FAILED(hr)) return hr;
    if (session->active || session->show_in_progress) return HRESULT_FROM_WIN32(ERROR_BUSY);
    try { pulse::ui::FolderPickerSpec spec; return Prepare(input, spec); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; } catch (...) { return E_FAIL; }
}
HRESULT WINAPI GetWindow(PulsePickerUiV2Session id, uint64_t show, HWND* window) {
    if (!window) return E_POINTER;
    *window = nullptr;
    const HRESULT hr = CheckShow(id, show); if (FAILED(hr)) return hr;
    DWORD pid = 0; const HWND hwnd = session->active->window;
    if (!hwnd || GetWindowThreadProcessId(hwnd, &pid) != GetCurrentThreadId() || pid != GetCurrentProcessId()) return E_UNEXPECTED;
    *window = hwnd; return S_OK;
}
HRESULT PackPaths(const std::vector<std::wstring>& paths,std::wstring& packed) noexcept {
    try {
        if(paths.size()>PULSE_PICKER_UI_V2_MAX_PATHS) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        for(const auto& path:paths) {
            if(path.empty() || path.size()>PULSE_PICKER_UI_V2_MAX_UNITS || path.find(L'\0')!=path.npos ||
                packed.size()+path.size()+1>PULSE_PICKER_UI_V2_MAX_PACKED_UNITS) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            packed+=path;packed.push_back(L'\0');
        }
        return S_OK;
    } catch(const std::bad_alloc&) { return E_OUTOFMEMORY; } catch(...) { return E_FAIL; }
}
HRESULT WINAPI Query(PulsePickerUiV2Session id, uint64_t show, uint64_t expected, PulsePickerUiV2Snapshot* output) {
    const HRESULT hr = CheckShow(id, show); if (FAILED(hr)) return hr;
    if (!output) return E_POINTER;
    if (output->size != sizeof(*output)) return E_INVALIDARG;
    const auto& frame = *session->active; const auto& state = frame.snapshot;
    if (expected && expected != state.generation) return HRESULT_FROM_WIN32(ERROR_RETRY);
    std::wstring selections,candidates;
    HRESULT packed_hr=PackPaths(state.selections,selections);if(FAILED(packed_hr)) return packed_hr;
    packed_hr=PackPaths(state.candidates,candidates);if(FAILED(packed_hr)) return packed_hr;
    const bool include_paths=frame.multi_select || output->selection_paths.data || output->selection_paths.capacity || output->candidate_paths.data || output->candidate_paths.capacity;
    output->selection_paths.units=static_cast<uint32_t>(selections.size());output->candidate_paths.units=static_cast<uint32_t>(candidates.size());
    output->selection_count=static_cast<uint32_t>(state.selections.size());output->candidate_count=static_cast<uint32_t>(state.candidates.size());
    PulsePickerUiV2Buffer* buffers[] = {&output->folder, &output->filename, &output->selection, &output->candidate,&output->selection_paths,&output->candidate_paths};
    const std::wstring* texts[] = {&state.folder, &state.filename, &state.selection, &state.candidate,&selections,&candidates};
    const unsigned count=include_paths ? 6u : 4u;
    for (auto* buffer : buffers) if (buffer->capacity && !buffer->data) return E_POINTER;
    bool fits = true;
    for (unsigned i = 0; i != count; ++i) {
        buffers[i]->units = static_cast<uint32_t>(texts[i]->size());
        fits = fits && buffers[i]->data && buffers[i]->capacity > texts[i]->size();
    }
    output->flags = (state.validating ? PULSE_PICKER_UI_V2_STATE_VALIDATING : 0u) |
        (!state.candidate.empty() ? PULSE_PICKER_UI_V2_STATE_CANDIDATE_READY : 0u) |
        (frame.closed ? PULSE_PICKER_UI_V2_STATE_CLOSING : 0u);
    output->show_id = show; output->generation = state.generation; output->window = frame.window;
    output->filter_index = static_cast<uint32_t>(state.filter_index); output->reserved = 0;
    if (!fits) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    uintptr_t starts[6]{}, ends[6]{};
    const uintptr_t structure = reinterpret_cast<uintptr_t>(output);
    for (unsigned i = 0; i != count; ++i) {
        starts[i] = reinterpret_cast<uintptr_t>(buffers[i]->data);
        const uint64_t bytes = (static_cast<uint64_t>(texts[i]->size()) + 1) * sizeof(wchar_t);
        if (bytes > UINTPTR_MAX || starts[i] > UINTPTR_MAX - bytes) return E_INVALIDARG;
        ends[i] = starts[i] + static_cast<uintptr_t>(bytes);
        if (starts[i] < structure + sizeof(*output) && structure < ends[i]) return E_INVALIDARG;
        for (unsigned j = 0; j < i; ++j)
            if (starts[i] < ends[j] && starts[j] < ends[i]) return E_INVALIDARG;
    }
    for (unsigned i = 0; i != count; ++i) {
        std::copy(texts[i]->begin(), texts[i]->end(), buffers[i]->data);
        buffers[i]->data[texts[i]->size()] = 0;
    }
    return S_OK;
}
HRESULT WINAPI Close(PulsePickerUiV2Session id, uint64_t show, HRESULT reason) {
    const HRESULT hr = CheckSession(id); if (FAILED(hr)) return hr;
    if (!session->active) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    if (session->active->id != show) return HRESULT_FROM_WIN32(ERROR_RETRY);
    auto& frame = *session->active;
    if (frame.closed) return S_FALSE;
    if (!SetEvent(frame.cancel)) return HRESULT_FROM_WIN32(GetLastError());
    wchar_t trace_enabled[2]{};
    if(GetEnvironmentVariableW(L"PULSE_PICKER_UI_DIAGNOSTICS",trace_enabled,2)==1 && trace_enabled[0]==L'1') {
        std::fprintf(stderr,"picker-close handle=%p signaled=%lu\n",frame.cancel,WaitForSingleObject(frame.cancel,0));std::fflush(stderr);
    }
    frame.closed = true; frame.close_hr = reason; return S_OK;
}
HRESULT ShowCore(PulsePickerUiV2Session id, const PulsePickerUiV2Config* input,
    const PulsePickerUiV2Callbacks* callbacks, PulsePickerUiV2Result* result) {
    const HRESULT session_hr = CheckSession(id); if (FAILED(session_hr)) return session_hr;
    if (session->active || session->show_in_progress) return HRESULT_FROM_WIN32(ERROR_BUSY);
    // Covers owner EnableWindow, typography changes AND their destructors.
    // Those operations can synchronously execute arbitrary caller window code.
    struct ShowBusyScope {
        Session& owner;
        explicit ShowBusyScope(Session& value) : owner(value) { owner.show_in_progress = true; }
        ~ShowBusyScope() { owner.show_in_progress = false; }
    } show_busy(*session);
    if (!callbacks) return E_POINTER;
    if (callbacks->size != sizeof(*callbacks)) return E_INVALIDARG;
    if (callbacks->reserved) return E_INVALIDARG;
    pulse::ui::FolderPickerSpec spec;
    pulse::picker_inprocess::PropertyPanel properties;
    HRESULT hr = Prepare(input, spec, &properties); if (FAILED(hr)) return hr;
    if(spec.allow_multiselect && (!result->paths.data || result->paths.capacity<PULSE_PICKER_UI_V2_MAX_PACKED_UNITS+1)) return E_INVALIDARG;
    const HWND owner = input->owner;
    const uint32_t requested_appearance = input->appearance;
    const auto owned_callbacks = *callbacks;
    std::unique_ptr<pulse::PickerOwnerGuard> owned_guard;
    if (remote_parent_pid) owned_guard = std::make_unique<pulse::PickerOwnerGuard>(owner, pulse::PickerOwnerGuard::CallerManagedRemoteOwner{remote_parent_pid});
    else owned_guard = std::make_unique<pulse::PickerOwnerGuard>(owner);
    auto& owner_guard = *owned_guard;
    LogOwnerContext(owner_guard, owner);
    if (owner_guard.Error()) { result->win32_error = owner_guard.Error(); return HRESULT_FROM_WIN32(owner_guard.Error()); }
    auto& env = session->environment;
    pulse::app::ApplyPickerAppearanceToSpec(env.appearance, spec);
    struct TypographyScope {
        int scale = pulse::ui::typography::UiFontScalePercent();
        pulse::ui::typography::TextRenderMode mode = pulse::ui::typography::CurrentTextRenderMode();
        ~TypographyScope() { pulse::ui::typography::SetUiFontScale(scale); pulse::ui::typography::SetTextRenderMode(mode); }
    } typography;
    pulse::ui::typography::SetUiFontScale(env.appearance.ui_font_scale);
    pulse::ui::typography::SetTextRenderMode(static_cast<pulse::ui::typography::TextRenderMode>(env.appearance.text_render));
    auto appearance = env.appearance;
    if (requested_appearance & PULSE_PICKER_UI_V2_DARK) appearance.theme_mode = 2;
    const auto colors = pulse::app::ResolveSystemPickerAppearanceColors(appearance);
    CallbackContext context;
    hr = context.Enter(env); if (FAILED(hr)) return hr;
    ActiveShow frame; frame.native_options = spec.native_options; frame.properties = &properties; frame.multi_select=spec.allow_multiselect; frame.local_save = spec.mode == pulse::ui::PickerMode::Save; frame.context = &context; frame.callbacks = owned_callbacks; frame.output = result;
    if (!session->next_show) return E_UNEXPECTED;
    frame.id = session->next_show++;
    frame.cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!frame.cancel) return HRESULT_FROM_WIN32(GetLastError());
    result->show_id = frame.id;
    spec.owner_guard = &owner_guard; spec.cancel_event = frame.cancel;
    spec.module_instance = env.module; spec.pulse_painted_plain_child = true;
    spec.on_dialog_event = [&](HWND window, PickerDialogEvent kind, const PickerDialogSnapshot& snapshot, const std::wstring& proposed) {
        return Notify(frame, window, kind, snapshot, proposed);
    };
    spec.on_navigation_complete = [&](HWND window, const std::wstring& path, HRESULT result_hr) {
        if (frame.closed || FAILED(frame.callback_error)) return;
        PulsePickerUiV2Event event{}; event.size = sizeof(event);
        event.kind = PULSE_PICKER_UI_V2_EVENT_NAVIGATION_COMPLETE;
        event.show_id = frame.id; event.generation = frame.snapshot.generation; event.window = window;
        event.proposed_folder = {path.data(), static_cast<uint32_t>(path.size())}; event.navigation_result = result_hr;
        frame.in_callback = true;
        const HRESULT delivered = frame.context->Invoke(frame.callbacks.callback, frame.callbacks.context, event);
        frame.in_callback = false;
        if (delivered != S_OK) { frame.callback_error = FAILED(delivered) ? delivered : E_UNEXPECTED; SetEvent(frame.cancel); }
    };
    if (spec.mode == pulse::ui::PickerMode::Save) spec.on_overwrite = [&](HWND window, const PickerDialogSnapshot& snapshot,
        const std::wstring& target, uint32_t& response) { return NotifyOverwrite(frame, window, snapshot, target, response); };
    if (spec.native_options & FOS_SHAREAWARE) spec.on_share_violation = [&](HWND window, const PickerDialogSnapshot& snapshot,
        const std::wstring& target, uint32_t& response) { return NotifyOverwrite(frame, window, snapshot, target, response, true); };
    std::jthread monitor([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (!owner_guard.Alive()) { SetEvent(frame.cancel); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    });
    session->active = &frame;
    struct ActiveScope { ~ActiveScope() { session->active = nullptr; } } active_scope;
    const auto outcome = pulse::ui::ShowFilePickerDetailed(owner, spec, colors.dark, pulse::ui::HexColor(colors.accent_rgb));
    frame.window = nullptr;
    result->flags = frame.shown ? PULSE_PICKER_UI_V2_UI_SHOWN : 0u;
    result->generation = frame.snapshot.generation;
    result->filter_index = static_cast<uint32_t>(frame.snapshot.filter_index);
    if (frame.closed) { result->status = PULSE_PICKER_UI_V2_CLOSED; result->win32_error = 0; return frame.close_hr; }
    if (FAILED(frame.callback_error)) { result->win32_error = ERROR_FUNCTION_FAILED; return frame.callback_error; }
    if (!owner_guard.Alive() || outcome.status == pulse::ui::FilePickerStatus::Cancelled) {
        result->status = PULSE_PICKER_UI_V2_CANCELLED; result->win32_error = 0; return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    if (outcome.status != pulse::ui::FilePickerStatus::Accepted || (outcome.result.paths.empty() || (!spec.allow_multiselect && outcome.result.paths.size()!=1))) {
        result->win32_error = outcome.error ? outcome.error : ERROR_INVALID_DATA;
        return HRESULT_FROM_WIN32(result->win32_error);
    }
    std::wstring packed;
    const HRESULT packed_hr=PackPaths(outcome.result.paths,packed);if(FAILED(packed_hr)) return packed_hr;
    if(result->paths.data && result->paths.capacity<=packed.size()) { result->win32_error=ERROR_INSUFFICIENT_BUFFER;return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER); }
    const auto& path = outcome.result.paths.front();
    if (path.size() >= result->path.capacity) { result->win32_error = ERROR_INSUFFICIENT_BUFFER; return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER); }
    std::copy(path.begin(), path.end(), result->path.data); result->path.data[path.size()] = 0;
    if(result->paths.data) { std::copy(packed.begin(),packed.end(),result->paths.data);result->paths.data[packed.size()]=0; }
    result->paths.units=static_cast<uint32_t>(packed.size());result->path_count=static_cast<uint32_t>(outcome.result.paths.size());
    result->path.units = static_cast<uint32_t>(path.size()); result->status = PULSE_PICKER_UI_V2_ACCEPTED;
    result->win32_error = 0; return S_OK;
}
HRESULT WINAPI Show(PulsePickerUiV2Session id, const PulsePickerUiV2Config* input,
    const PulsePickerUiV2Callbacks* callbacks, PulsePickerUiV2Result* result) {
    if (!result) return E_POINTER;
    if (result->size != sizeof(*result)) return E_INVALIDARG;
    if (!result->path.data || result->path.capacity < PULSE_PICKER_UI_V2_MAX_UNITS + 1) return E_INVALIDARG;
    const auto result_address=reinterpret_cast<uintptr_t>(result);
    const auto path_address=reinterpret_cast<uintptr_t>(result->path.data);
    const uint64_t path_bytes=static_cast<uint64_t>(result->path.capacity)*sizeof(wchar_t);
    if(path_bytes>UINTPTR_MAX || path_address>UINTPTR_MAX-path_bytes ||
        (path_address<result_address+sizeof(*result) && result_address<path_address+path_bytes)) return E_INVALIDARG;
    if(result->paths.capacity && !result->paths.data) return E_POINTER;
    if(result->paths.data) {
        const auto packed_address=reinterpret_cast<uintptr_t>(result->paths.data);
        const uint64_t packed_bytes=static_cast<uint64_t>(result->paths.capacity)*sizeof(wchar_t);
        if(packed_bytes>UINTPTR_MAX || packed_address>UINTPTR_MAX-packed_bytes ||
            (packed_address<result_address+sizeof(*result) && result_address<packed_address+packed_bytes) ||
            (packed_address<path_address+path_bytes && path_address<packed_address+packed_bytes)) return E_INVALIDARG;
    }
    result->status = PULSE_PICKER_UI_V2_FAILED; result->flags = 0; result->win32_error = ERROR_FUNCTION_FAILED;
    result->show_id = 0; result->generation = 0; result->filter_index = 0;
    result->path.units = 0; result->path.data[0] = 0;
    if(result->paths.capacity && !result->paths.data) return E_POINTER;
    result->path_count=0;result->paths.units=0;if(result->paths.data && result->paths.capacity) result->paths.data[0]=0;
    HRESULT hr = E_FAIL;
    try { hr = ShowCore(id, input, callbacks, result); }
    catch (const std::bad_alloc&) { hr = E_OUTOFMEMORY; }
    catch (...) { hr = E_FAIL; }
    result->terminal_hr = hr;
    return hr;
}
HRESULT WINAPI UpdateControl(PulsePickerUiV2Session id, uint64_t show, const PulsePickerUiV2Control* input) {
    const HRESULT checked = CheckShow(id, show); if (FAILED(checked)) return checked;
    auto& frame = *session->active;
    if (frame.closed || FAILED(frame.callback_error) || !frame.window) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    try {
        pulse::ui::PickerCustomControl copied; uint64_t total = 0;
        const HRESULT hr = CopyControl(input, copied, total); if (FAILED(hr)) return hr;
        return pulse::ui::UpdatePickerCustomControl(frame.window, copied);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; } catch (...) { return E_FAIL; }
}
HRESULT WINAPI Navigate(PulsePickerUiV2Session id, uint64_t show, PulsePickerUiV2Text input) {
    const HRESULT checked = CheckShow(id, show); if (FAILED(checked)) return checked;
    auto& frame = *session->active;
    if (frame.closed || FAILED(frame.callback_error) || !frame.window) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    try {
        std::wstring path; uint64_t total = 0;
        if (!CopyText(input, path, total) || !UserFolderSyntax(path, frame.native_options)) return E_INVALIDARG;
        if (frame.local_save && !LocalFolderSyntax(path)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        return pulse::ui::NavigatePickerFolder(frame.window, path);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; } catch (...) { return E_FAIL; }
}
HRESULT WINAPI Shutdown(PulsePickerUiV2Session id) {
    const HRESULT hr = CheckSession(id); if (FAILED(hr)) return hr;
    if (session->active || session->show_in_progress) return HRESULT_FROM_WIN32(ERROR_BUSY);
    live_session.store(0, std::memory_order_release); live_thread.store(0, std::memory_order_release);
    session.reset();
    return pulse::picker_inprocess::ReleaseEnvironment();
}
}
extern "C" HRESULT WINAPI PulsePickerUiGetApiV2(uint32_t version, uint32_t size, PulsePickerUiV2Api* api) {
    if (!api) return E_POINTER;
    if (version != PULSE_PICKER_UI_V2_VERSION || size != sizeof(*api)) return E_INVALIDARG;
    *api = {sizeof(*api), PULSE_PICKER_UI_V2_VERSION, capabilities, 0,
        Initialize, ValidateConfig, Show, GetWindow, Query, Close, Shutdown, UpdateControl,
        PULSE_PICKER_UI_V2_FEATURE_CUSTOM_RADIO | PULSE_PICKER_UI_V2_FEATURE_CUSTOM_CHECK | PULSE_PICKER_UI_V2_FEATURE_CUSTOM_EDIT | PULSE_PICKER_UI_V2_FEATURE_CUSTOM_LABELS | PULSE_PICKER_UI_V2_FEATURE_CUSTOM_MENU | PULSE_PICKER_UI_V2_FEATURE_OPEN_DROPDOWN | PULSE_PICKER_UI_V2_FEATURE_CUSTOM_COMBOBOX | PULSE_PICKER_UI_V2_FEATURE_MULTI_SELECT | PULSE_PICKER_UI_V2_FEATURE_OPEN_DEFAULT_EXTENSION | PULSE_PICKER_UI_V2_FEATURE_SAVE_UNENCRYPTED | PULSE_PICKER_UI_V2_FEATURE_OPEN_SKIP_ACCESS | PULSE_PICKER_UI_V2_FEATURE_FILESYSTEM_PLACES | PULSE_PICKER_UI_V2_FEATURE_COMPUTER_PLACE | PULSE_PICKER_UI_V2_FEATURE_PROPERTIES | PULSE_PICKER_UI_V2_FEATURE_SAVE_SKIP_ACCESS | PULSE_PICKER_UI_V2_FEATURE_NAVIGATE | PULSE_PICKER_UI_V2_FEATURE_SAVE_NATIVE_DEFAULTS | PULSE_PICKER_UI_V2_FEATURE_OPEN_MISSING | PULSE_PICKER_UI_V2_FEATURE_SAVE_NO_TEST_CREATE | PULSE_PICKER_UI_V2_FEATURE_NATIVE_OPTIONS | PULSE_PICKER_UI_V2_FEATURE_CUSTOM_PROMINENT_GROUP | PULSE_PICKER_UI_V2_FEATURE_CUSTOM_PROMINENT_CONTROL | PULSE_PICKER_UI_V2_FEATURE_CUSTOM_SEPARATOR, Navigate};
    return S_OK;
}


namespace pulse::picker_inprocess {
HRESULT ConfigureRemoteParent(DWORD pid) noexcept {
    if (session || !pid || pid == GetCurrentProcessId() || remote_parent_pid) return E_INVALIDARG;
    remote_parent_pid = pid;
    return S_OK;
}
}

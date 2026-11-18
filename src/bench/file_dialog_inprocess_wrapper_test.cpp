#include "../shell_bridge/file_dialog_wrapper.h"
#include "../shell_bridge/file_dialog_runtime.h"
#include "../shell_bridge/file_dialog_activation.h"
#include "../shell_bridge/file_dialog_class_factory.h"
#include "../shell_bridge/file_dialog_inprocess_session.h"
#include "../shell_bridge/file_dialog_client_state.h"
#include "../shell_bridge/file_dialog_runtime.h"
#include <shlobj.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <atomic>
#include <algorithm>
#include <memory>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

// The v2 UI ABI is a deterministic script; the wrapper, native COM mirror,
// event proxy, HWND, Shell items and owned filesystem fixture are real.
// This is not a UI/input or production STANDARD_EVENTS certification.
namespace {
using namespace pulse::shell_bridge;
using Microsoft::WRL::ComPtr;
unsigned checks = 0, failures = 0;
bool isolated_native = false;
HRESULT CreateIsolatedDialog(DialogFactory kind, const FileDialogBridgeServices& services, REFIID iid, void** output) {
    using Entry = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
    static const auto entry = []() -> Entry {
        const auto module = LoadLibraryExW(L"comdlg32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module ? reinterpret_cast<Entry>(GetProcAddress(module, "DllGetClassObject")) : nullptr;
    }();
    if (!entry) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    ComPtr<IClassFactory> factory;
    HRESULT hr = entry(kind == DialogFactory::Save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, IID_PPV_ARGS(&factory));
    ComPtr<IFileDialog> native;
    if (SUCCEEDED(hr)) hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&native));
    return FAILED(hr) ? hr : WrapNativeFileDialog(kind, native.Get(), services, iid, output);
}
void Check(bool ok, const char* label) { ++checks; if (!ok) ++failures; std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); }
constexpr HRESULT kClose = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x543);
struct Script {
    std::wstring folder, file;
    HWND window = nullptr;
    uint64_t generation = 1;
    unsigned initialized = 0, shown = 0, stopped = 0;
    bool readonly_properties = false, property_close = false;
    bool folder_mode = false, folder_multi = false;
    bool combo = false, menus = false;
    unsigned activating = 0;
    bool caller_validation = false;
    bool missing_open = false, missing_veto = false;
    bool prominent_group = false;
    uint32_t prominent_kind = 0;
    bool prominent_nested = false;
    bool no_test_save = false;
    bool separators = false;
    bool client_places = false, cancel_before_accept = false, default_folder = false;
    bool metadata=false,metadata_cancel=false,metadata_caller=false,metadata_full=false;
    uint32_t title_index=0,keywords_index=0;
    bool value_controls = false;
    unsigned check_events=0;
    bool computer_place = false;
    unsigned computer_changing = 0, computer_changed = 0;
    ComPtr<IShellItem> computer;
    unsigned item_calls = 0, corrupt_packed = 0;
    std::wstring second;
    bool custom = false, button_close = false, save = false, overwrite = false;
    bool native_options_case = false, share = false;
    uint32_t expected_native_options = 0;
    unsigned share_mode = 0, share_calls = 0;
    uint32_t overwrite_response = PULSE_PICKER_UI_V2_OVERWRITE_DEFAULT;
    unsigned overwrite_calls = 0, overwrite_mode = 0;
    std::vector<unsigned> overwrite_order;
    unsigned updates = 0, button_calls = 0;
    bool fail_update = false;
    std::wstring button_label;
    uint32_t button_state = 3;
    bool remove_button_sink = false;
    bool candidate = false, closed = false, corrupt_result = false, fail_after_callback = false, filtered = false;
    HRESULT close_hr = S_OK;
};
Script* script = nullptr;
PulsePickerUiV2Api real_config_api{};
std::function<void()> show_action;
const PulsePickerUiV2Callbacks* navigation_callbacks = nullptr;
HRESULT navigation_result = S_OK;
bool navigation_case = false;
HRESULT WINAPI NavigateFixture(PulsePickerUiV2Session, uint64_t, PulsePickerUiV2Text path) {
    if (navigation_result == S_OK) { script->folder.assign(path.data, path.units); ++script->generation; }
    return navigation_result;
}
HRESULT WINAPI Initialize(const PulsePickerUiV2Initialize*, PulsePickerUiV2Session* out) { ++script->initialized; *out = 1; return S_OK; }
HRESULT WINAPI Validate(PulsePickerUiV2Session, const PulsePickerUiV2Config* config) {
    if (script->native_options_case)
        Check(config->native_options == script->expected_native_options, "complete original native options reach the UI config");
    if(script->prominent_kind){const auto& control=config->controls[script->prominent_nested?1:0];
        Check(control.kind==script->prominent_kind&&bool(control.state&PULSE_PICKER_UI_V2_CONTROL_PROMINENT)==(script->prominent_kind!=6&&script->prominent_kind!=8)&&bool(control.has_parent)==script->prominent_nested,"generic prominence preserves native layout policy and parent identity");}
    if (script->separators) {
        Check(config->control_count == 3 && config->controls[0].kind == PULSE_PICKER_UI_V2_CONTROL_SEPARATOR && config->controls[0].id == 2028 &&
            config->controls[1].kind == PULSE_PICKER_UI_V2_CONTROL_SEPARATOR && config->controls[1].id == 2029 && config->controls[1].state == 2 &&
            config->controls[2].id == 2024 && (config->controls[2].state & PULSE_PICKER_UI_V2_CONTROL_PROMINENT),
            "save separators retain order, IDs, state and prominent button");
    }
    if(script->no_test_save) Check((config->save_options&PULSE_PICKER_UI_V2_SAVE_NO_TEST_CREATE) &&
        !(config->save_options&PULSE_PICKER_UI_V2_SAVE_SKIP_ACCESS_VALIDATION) && config->default_extension.units==3 &&
        std::wstring(config->default_extension.data,config->default_extension.units)==L"pdf", "Save no-test-create keeps separate access policy and PDF extension");
    if(script->default_folder) Check(std::wstring(config->initial_folder.data,config->initial_folder.units)==script->folder,"resolved initial folder respects default/history/explicit precedence");
    if(script->metadata) {
        Check(bool(config->save_options & PULSE_PICKER_UI_V2_SAVE_SKIP_ACCESS_VALIDATION) == script->metadata_caller,"metadata Save carries exact caller access obligation");
        Check(config->property_count==(script->metadata_full?8u:2u) && config->properties,"metadata config preserves requested fields");
        for(uint32_t i=0;i<config->property_count;++i) {
            const auto& field=config->properties[i];
            if(script->metadata_full ? field.index==2 : field.kind==PULSE_PICKER_UI_V2_PROPERTY_SCALAR)script->title_index=field.index;
            if(script->metadata_full ? field.index==1 : field.kind==PULSE_PICKER_UI_V2_PROPERTY_LIST)script->keywords_index=field.index;
        }
    }
    if(script->value_controls) Check(config->control_count==7 && config->controls[0].kind==6 && config->controls[1].kind==7 && config->controls[2].kind==8 && !config->controls[0].state && !config->controls[1].state && !config->controls[2].state,"radio/check/edit initially hidden state captured");
    if(script->value_controls && config->control_count==7) Check(config->controls[3].kind==1 && config->controls[4].has_parent && config->controls[4].parent_id==config->controls[3].id && !config->controls[5].has_parent && config->controls[5].parent_id==0 && !config->controls[6].has_parent && config->controls[6].parent_id==0,"ended groups do not leak parent IDs into standalone controls");
    if(script->computer_place) Check(config->place_count==1 && config->places && std::wstring(config->places[0].path.data,config->places[0].path.units)==L"pulse:computer", "Computer AddPlace carries dedicated navigation token");
    if(script->menus) Check(config->control_count==2 && config->controls[0].kind==PULSE_PICKER_UI_V2_CONTROL_OPEN_DROPDOWN &&
        config->controls[1].kind==PULSE_PICKER_UI_V2_CONTROL_MENU && (config->controls[1].state&PULSE_PICKER_UI_V2_CONTROL_PROMINENT) &&
        config->controls[0].item_count==4 && config->controls[0].items[0].id==0 && config->controls[0].items[1].id==2 &&
        config->controls[0].items[2].id==201 && config->controls[0].items[3].id==202,
        "menu prominence is explicit while creation and dropdown item order stay unchanged");
    if (script->client_places) Check(config->place_count == 1 && config->places && config->places[0].location == PULSE_PICKER_UI_V2_PLACE_TOP &&
        std::wstring(config->places[0].path.data, config->places[0].path.units) == script->folder, "native AddPlace is represented with correct folder and placement");
    if(script->prominent_group) Check(config->control_count==2 && config->controls[0].kind==PULSE_PICKER_UI_V2_CONTROL_GROUP &&
        config->controls[0].state==(3u|PULSE_PICKER_UI_V2_CONTROL_PROMINENT) && config->controls[1].has_parent && config->controls[1].parent_id==1047,
        "prominent group state and child relationship preserved in UI config");
    if (script->combo) {
        Check(bool(config->open_options & PULSE_PICKER_UI_V2_OPEN_SKIP_ACCESS_VALIDATION) == script->caller_validation, "caller validation obligation is carried explicitly to UI");
        Check(config->open_options & PULSE_PICKER_UI_V2_OPEN_ALLOW_MULTI_SELECT, "full trace retains multi-select option");
        Check((config->open_options & PULSE_PICKER_UI_V2_OPEN_USE_DEFAULT_EXTENSION) && config->default_extension.units == 3 &&
            std::wstring(config->default_extension.data, config->default_extension.units) == L"txt", "full trace preserves explicit Open extension and provider obligation");
        Check(config->label_flags == (PULSE_PICKER_UI_V2_LABEL_TITLE | PULSE_PICKER_UI_V2_LABEL_OK | PULSE_PICKER_UI_V2_LABEL_FILE_NAME) &&
            std::wstring(config->title.data, config->title.units) == L"Generic Open" && config->ok_button_label.units == 0 &&
            std::wstring(config->file_name_label.data, config->file_name_label.units) == L"File", "full trace preserves all label presence including explicit empty");
        Check(config->control_count == 1 && config->controls[0].kind == PULSE_PICKER_UI_V2_CONTROL_COMBO &&
            config->controls[0].item_count == 7 && config->controls[0].has_selected && config->controls[0].selected_item_id == 0 &&
            config->controls[0].items[4].id == 5 && config->controls[0].items[5].id == 4, "full trace preserves ordered items and selected ID zero");
    }
    if (script->filtered) Check(config->filter_count == 1 && config->filters && config->filter_index == 0 &&
        config->filters[0].pattern.units == 3 && std::wstring(config->filters[0].pattern.data, 3) == L"*.*",
        "actual native SetFileTypes plus observed SetIndex0 map to advisory v2 filter index zero");
    if (script->custom) Check(config->control_count == 2 && config->controls[0].kind == PULSE_PICKER_UI_V2_CONTROL_GROUP &&
        config->controls[1].kind == PULSE_PICKER_UI_V2_CONTROL_BUTTON && config->controls[1].has_parent &&
        config->controls[1].parent_id == config->controls[0].id, "real native group/button/end is represented in ordered UI config");
    if (script->save) Check(config->save_options == (PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT | (script->metadata_caller ? PULSE_PICKER_UI_V2_SAVE_SKIP_ACCESS_VALIDATION : 0u) | (script->readonly_properties ? PULSE_PICKER_UI_V2_SAVE_REQUIRE_UNENCRYPTED : 0u)) && config->default_extension.units == 3 &&
        std::wstring(config->default_extension.data, 3) == L"txt", "Save config preserves explicit extension and overwrite policy");
    if (real_config_api.ValidateConfig) {
        PulsePickerUiV2Initialize init{}; init.size = sizeof(init); init.version = PULSE_PICKER_UI_V2_VERSION;
        PulsePickerUiV2Session validation_session = 0;
        HRESULT hr = real_config_api.Initialize(&init, &validation_session);
        Check(hr == S_OK, "initialize real UI config validator");
        if (hr == S_OK) {
            hr = real_config_api.ValidateConfig(validation_session, config);
            std::printf("REAL_CONFIG hr=%08lX controls=%u properties=%u filters=%u\n", static_cast<unsigned long>(hr), config->control_count, config->property_count, config->filter_count);
            Check(hr == S_OK, "actual UI accepts wrapper-generated complete config");
            Check(real_config_api.Shutdown(validation_session) == S_OK, "shutdown real validator without showing UI");
        }
        if (hr != S_OK) return hr;
    }
    return config->required_capabilities == 0 && config->access_policy == (script->folder_mode ? PULSE_PICKER_UI_V2_ACCESS_FOLDER_V1 : (script->save || script->no_test_save || script->separators) ? PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1 : PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1) ? S_OK : E_UNEXPECTED;
}
HRESULT WINAPI Window(PulsePickerUiV2Session, uint64_t show, HWND* out) { *out = show == 1 ? script->window : nullptr; return *out ? S_OK : E_UNEXPECTED; }
HRESULT WINAPI Close(PulsePickerUiV2Session, uint64_t show, HRESULT hr) {
    if (show != 1) return E_INVALIDARG;
    if (!script->closed) { script->closed = true; script->close_hr = hr; } return S_OK;
}
HRESULT WINAPI UpdateControl(PulsePickerUiV2Session, uint64_t show, const PulsePickerUiV2Control* control) {
    Check(show == 1 && control && (control->id == 71 || control->id == 72 || (script->combo && control->id == 1) || (script->menus && control->id==50) || (script->separators && control->id == 2029) || (script->value_controls && control->id>=206 && control->id<=213)), "live update targets actual UI session/control");
    if (control->id == 72) { script->button_label.assign(control->label.data, control->label.units); script->button_state = control->state; }
    ++script->updates;
    if (script->fail_update) { script->fail_update = false; return E_ACCESSDENIED; }
    return S_OK;
}
HRESULT WINAPI Shutdown(PulsePickerUiV2Session) { ++script->stopped; return S_OK; }
HRESULT WINAPI Query(PulsePickerUiV2Session, uint64_t show, uint64_t generation, PulsePickerUiV2Snapshot* output) {
    if (show != 1 || (generation && generation != script->generation)) return HRESULT_FROM_WIN32(ERROR_RETRY);
    const std::wstring name = std::filesystem::path(script->file).filename().wstring(), candidate = script->candidate ? script->file : L"";
    const std::wstring* values[]{&script->folder, &name, &script->file, &candidate};
    PulsePickerUiV2Buffer* buffers[]{&output->folder, &output->filename, &output->selection, &output->candidate};
    output->show_id = 1; output->generation = script->generation; output->window = script->window;
    output->filter_index = 0; output->flags = script->candidate ? PULSE_PICKER_UI_V2_STATE_CANDIDATE_READY : 0;
    std::wstring packed_selection, packed_candidate;
    if (script->combo || script->folder_multi || script->missing_open) {
        const unsigned count=(script->combo||script->folder_multi)?2u:1u;
        packed_selection = script->file + std::wstring(1, L'\0');
        if(count==2)packed_selection+=script->second+std::wstring(1,L'\0');
        if (script->candidate) packed_candidate = packed_selection;
        output->selection_paths.units = static_cast<uint32_t>(packed_selection.size()); output->selection_count = count;
        output->candidate_paths.units = static_cast<uint32_t>(packed_candidate.size()); output->candidate_count = script->candidate ? count : 0u;
    }
    bool fits = !(script->combo || script->folder_multi || script->missing_open) || (output->selection_paths.data && output->selection_paths.capacity > packed_selection.size() &&
        output->candidate_paths.data && output->candidate_paths.capacity > packed_candidate.size());
    for (size_t i = 0; i < 4; ++i) { buffers[i]->units = static_cast<uint32_t>(values[i]->size()); fits &= buffers[i]->data && buffers[i]->capacity > values[i]->size(); }
    if (!fits) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    for (size_t i = 0; i < 4; ++i) wcscpy_s(buffers[i]->data, buffers[i]->capacity, values[i]->c_str());
    if (script->combo || script->folder_multi || script->missing_open) {
        std::copy(packed_selection.begin(), packed_selection.end(), output->selection_paths.data); output->selection_paths.data[packed_selection.size()] = 0;
        std::copy(packed_candidate.begin(), packed_candidate.end(), output->candidate_paths.data); output->candidate_paths.data[packed_candidate.size()] = 0;
    }
    return S_OK;
}
HRESULT WINAPI Show(PulsePickerUiV2Session, const PulsePickerUiV2Config* config, const PulsePickerUiV2Callbacks* callbacks, PulsePickerUiV2Result* result) {
    ++script->shown;
    script->window = CreateWindowExW(0, L"STATIC", L"owned wrapper ABI fixture", WS_OVERLAPPED,
        0, 0, 100, 100, config->owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!script->window) return E_FAIL;
    result->path.units = 0; result->path.data[0] = 0; result->flags = PULSE_PICKER_UI_V2_UI_SHOWN;
    result->show_id = 1; result->filter_index = 0;
    auto emit = [&](uint32_t kind) {
        PulsePickerUiV2Event event{}; event.size = sizeof(event); event.kind = kind; event.show_id = 1;
        event.generation = script->generation; event.window = script->window;
        if (kind == PULSE_PICKER_UI_V2_EVENT_BUTTON_CLICKED) event.control_id = 72;
        if (kind == PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED) { event.control_id = 1; event.item_id = 5; }
        if (kind == PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGING) event.proposed_folder = {script->folder.data(), static_cast<uint32_t>(script->folder.size())};
        return callbacks->callback(callbacks->context, &event);
    };
    HRESULT hr = emit(PULSE_PICKER_UI_V2_EVENT_READY);
    navigation_callbacks = callbacks;
    if (hr == S_OK && show_action) show_action();
    if(hr==S_OK && script->computer_place) {
        const auto local_folder=script->folder, local_file=script->file;
        script->folder=L"pulse:computer";script->file.clear();++script->generation;
        hr=emit(PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGING);
        if(hr==S_OK)hr=emit(PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGED);
        script->folder=local_folder;script->file=local_file;++script->generation;
    }
    if (script->cancel_before_accept && hr == S_OK) {
        result->status = PULSE_PICKER_UI_V2_CANCELLED; result->terminal_hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        result->generation = script->generation;
        DestroyWindow(script->window); script->window = nullptr;
        return result->terminal_hr;
    }
    if (hr == S_OK) hr = emit(PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGING);
    Check(hr == S_OK, "COM FolderChanging E_NOTIMPL translates to allow");
    if (hr == S_OK) hr = emit(PULSE_PICKER_UI_V2_EVENT_TYPE_CHANGED);
    if (hr == S_OK) hr = emit(PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGED);
    if (hr == S_OK) hr = emit(PULSE_PICKER_UI_V2_EVENT_SELECTION_CHANGED);
    if (hr == S_OK && script->combo) hr = emit(PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED);
    if (hr == S_OK && script->custom) hr = emit(PULSE_PICKER_UI_V2_EVENT_BUTTON_CLICKED);
    if(hr==S_OK && script->menus) {
        auto control_event=[&](uint32_t kind,uint32_t id,uint32_t item) {
            PulsePickerUiV2Event event{};event.size=sizeof(event);event.kind=kind;event.show_id=1;
            event.generation=script->generation;event.window=script->window;event.control_id=id;event.item_id=item;
            return callbacks->callback(callbacks->context,&event);
        };
        Check(control_event(PULSE_PICKER_UI_V2_EVENT_OPEN_DROPDOWN_SELECTED,1,17)==E_INVALIDARG,"internal dropdown selection rejects menu kind");
        Check(control_event(PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED,50,2)==E_INVALIDARG,"public item event rejects dropdown kind");
        Check(control_event(PULSE_PICKER_UI_V2_EVENT_CONTROL_ACTIVATING,999,0)==E_INVALIDARG,"unknown activating control rejected");
        Check(control_event(PULSE_PICKER_UI_V2_EVENT_CONTROL_ACTIVATING,1,0)==S_OK,"menu activating callback dispatched");
        Check(control_event(PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED,1,17)==S_OK,"menu click notification dispatched");
        Check(control_event(PULSE_PICKER_UI_V2_EVENT_OPEN_DROPDOWN_SELECTED,50,201)==E_INVALIDARG,"disabled dropdown item rejected");
        Check(control_event(PULSE_PICKER_UI_V2_EVENT_OPEN_DROPDOWN_SELECTED,50,202)==E_INVALIDARG,"hidden dropdown item rejected");
        Check(control_event(PULSE_PICKER_UI_V2_EVENT_CONTROL_ACTIVATING,50,0)==S_OK,"dropdown activation permits live state update");
        hr=control_event(PULSE_PICKER_UI_V2_EVENT_OPEN_DROPDOWN_SELECTED,50,2);
        Check(hr==S_OK && script->item_calls==1,"dropdown synchronization emits no OnItemSelected");
    }
    if(hr==S_OK && script->value_controls) {
        auto value_event=[&](uint32_t kind,uint32_t id,uint32_t item,const wchar_t* text=L"") {
            PulsePickerUiV2Event event{};event.size=sizeof(event);event.kind=kind;event.show_id=1;event.generation=script->generation;event.window=script->window;event.control_id=id;event.item_id=item;
            event.control_text={text,static_cast<uint32_t>(wcslen(text))};return callbacks->callback(callbacks->context,&event);
        };
        Check(value_event(PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED,206,208)==E_INVALIDARG,"disabled radio option rejects selection");
        Check(value_event(PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED,206,207)==S_OK,"radio selection reaches real control sink");
        Check(value_event(PULSE_PICKER_UI_V2_EVENT_CHECK_TOGGLED,210,2)==E_INVALIDARG,"invalid checkbox boolean rejected");
        Check(value_event(PULSE_PICKER_UI_V2_EVENT_CHECK_TOGGLED,210,1)==S_OK,"checkbox toggle reaches real control sink");
        hr=value_event(PULSE_PICKER_UI_V2_EVENT_EDIT_CHANGED,213,0,L"typed value");
        Check(hr==S_OK && script->item_calls==1 && script->check_events==1,"edit synchronization does not invent public control events");
    }
    if(hr==S_OK && script->metadata) {
        const wchar_t* title=L"Edited title";const wchar_t* first=L"semi;colon";const wchar_t* second=L"second tag";
        PulsePickerUiV2Text title_value{title,static_cast<uint32_t>(wcslen(title))};
        PulsePickerUiV2Text keywords[]={{first,static_cast<uint32_t>(wcslen(first))},{second,static_cast<uint32_t>(wcslen(second))}};
        auto changed=[&](uint32_t index,const PulsePickerUiV2Text* values,uint32_t count) {
            PulsePickerUiV2Event event{};event.size=sizeof(event);event.kind=PULSE_PICKER_UI_V2_EVENT_PROPERTY_CHANGED;event.show_id=1;event.generation=script->generation;event.window=script->window;
            event.control_id=index;event.property_values=values;event.property_value_count=count;return callbacks->callback(callbacks->context,&event);
        };
        Check(changed(script->title_index,&title_value,1)==S_OK,"metadata scalar edit synchronizes");
        hr=changed(script->keywords_index,keywords,2);Check(hr==S_OK,"metadata vector values remain distinct without semicolon parsing");
    }
    bool refused = false;
    if (hr == S_OK && script->share) {
        script->candidate = true;
        PulsePickerUiV2OverwriteEvent event{}; event.size = sizeof(event); event.show_id = 1; event.generation = script->generation;
        event.window = script->window; event.target = {script->file.data(), static_cast<uint32_t>(script->file.size())};
        uint32_t response = 99;
        if (script->share_mode == 5) ++event.generation;
        hr = callbacks->share_violation ? callbacks->share_violation(callbacks->context, &event, &response) : E_UNEXPECTED;
        if (script->share_mode == 5) Check(FAILED(hr) && script->share_calls == 0, "stale sharing event never reaches caller");
        else Check(hr == S_OK && response == script->overwrite_response, "sharing response maps through actual COM event proxy");
        refused = response == PULSE_PICKER_UI_V2_OVERWRITE_REFUSE;
    }
    if (hr == S_OK && script->overwrite) {
        script->candidate = true;
        PulsePickerUiV2OverwriteEvent event{}; event.size = sizeof(event); event.show_id = 1; event.generation = script->generation;
        event.window = script->window; event.target = {script->file.data(), static_cast<uint32_t>(script->file.size())};
        uint32_t response = 99;
        hr = callbacks->overwrite ? callbacks->overwrite(callbacks->context, &event, &response) : E_UNEXPECTED;
        Check(hr == S_OK && response == script->overwrite_response && (script->overwrite_mode || script->overwrite_calls == 1), "independent overwrite response mapped through actual COM proxy");
        refused = response == PULSE_PICKER_UI_V2_OVERWRITE_REFUSE;
    }
    if (hr == S_OK && !script->closed && !refused) {
        script->candidate = true; hr = emit(PULSE_PICKER_UI_V2_EVENT_FILE_OK);
        if (hr == S_FALSE && !script->closed) {
            Check(IsWindow(script->window) != FALSE, "veto script retains same real HWND");
            script->candidate = false; ++script->generation;
            script->candidate = true; hr = emit(PULSE_PICKER_UI_V2_EVENT_FILE_OK);
        }
    }
    if (script->fail_after_callback && !script->closed) hr = E_ACCESSDENIED;
    if (refused && !script->closed) { result->status = PULSE_PICKER_UI_V2_CANCELLED; hr = HRESULT_FROM_WIN32(ERROR_CANCELLED); }
    else if (script->closed) { result->status = PULSE_PICKER_UI_V2_CLOSED; hr = script->close_hr; }
    else if (hr == S_OK) {
        result->status = PULSE_PICKER_UI_V2_ACCEPTED;
        if (script->combo || script->folder_multi || script->missing_open) {
            const unsigned count=(script->combo||script->folder_multi)?2u:1u;
            auto packed = script->file + std::wstring(1, L'\0');
            if(count==2)packed+=(script->corrupt_packed == 1 ? script->file : script->second) + std::wstring(1, L'\0');
            if (script->corrupt_packed == 2) packed.pop_back();
            Check(result->paths.data && result->paths.capacity > packed.size(), "full result has caller-owned bounded array storage");
            std::copy(packed.begin(), packed.end(), result->paths.data); result->paths.data[packed.size()] = 0;
            result->paths.units = static_cast<uint32_t>(packed.size()); result->path_count = count;
        }
        wcscpy_s(result->path.data, result->path.capacity, script->file.c_str()); result->path.units = static_cast<uint32_t>(script->file.size());
    } else result->status = PULSE_PICKER_UI_V2_FAILED;
    result->generation = script->generation + (script->corrupt_result ? 1 : 0); result->terminal_hr = hr;
    DestroyWindow(script->window); script->window = nullptr; return hr;
}
void CheckMetadata(IPropertyStore* store) {
    PROPVARIANT title{},keywords{};PropVariantInit(&title);PropVariantInit(&keywords);
    const HRESULT title_hr=store?store->GetValue(PKEY_Title,&title):E_POINTER;
    const HRESULT keywords_hr=store?store->GetValue(PKEY_Keywords,&keywords):E_POINTER;
    Check(title_hr==S_OK && title.vt==VT_LPWSTR && title.pwszVal && std::wstring(title.pwszVal)==L"Edited title","GetProperties returns edited scalar");
    Check(keywords_hr==S_OK && keywords.vt==(VT_VECTOR|VT_LPWSTR) && keywords.calpwstr.cElems==2 && keywords.calpwstr.pElems && std::wstring(keywords.calpwstr.pElems[0])==L"semi;colon" && std::wstring(keywords.calpwstr.pElems[1])==L"second tag","GetProperties preserves two exact vector entries");
    PropVariantClear(&title);PropVariantClear(&keywords);
}
struct SinkState {
    unsigned calls = 0;
    bool self_unadvise = false, destroyed = false;
    DWORD cookie = 0;
    ComPtr<IFileDialogEvents>* external = nullptr;
    std::wstring expected;
    unsigned role = 0, multi_mode = 0;
    DWORD remove_cookie = 0, added_cookie = 0;
    std::vector<unsigned>* order = nullptr;
    std::vector<unsigned>* notifications = nullptr;
    bool notimpl_case = false, notimpl_sink = false;
};
class Sink final : public IFileDialogEvents, public IFileDialogControlEvents {
public:
    explicit Sink(SinkState& state) : state_(state) {}
    ~Sink() { state_.destroyed = true; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid == IID_IFileDialogControlEvents) { *out = static_cast<IFileDialogControlEvents*>(this); AddRef(); return S_OK; }
        if (iid != IID_IUnknown && iid != IID_IFileDialogEvents) return E_NOINTERFACE;
        *out = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG count = --refs_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE OnButtonClicked(IFileDialogCustomize* customize, DWORD id) override {
        ++script->button_calls;
        Check(id == 72, "generic button ID reaches real control event proxy");
        ComPtr<IFileDialog> dialog; customize->QueryInterface(IID_PPV_ARGS(&dialog));
        ComPtr<IUnknown> a, b; customize->QueryInterface(IID_PPV_ARGS(&a)); dialog.As(&b);
        Check(a.Get() == b.Get(), "button callback retains wrapper controlling identity");
        CDCONTROLSTATEF own{};
        Check(customize->SetControlState(71, CDCS_VISIBLE) == S_OK && customize->GetControlState(72, &own) == S_OK && own == (CDCS_ENABLED | CDCS_VISIBLE), "group state update preserves child ownstate");
        Check(customize->SetControlLabel(72, L"Updated") == S_OK && customize->SetControlState(71, static_cast<CDCONTROLSTATEF>(3)) == S_OK && script->updates == 3 * script->button_calls, "live labels/states reach UI update provider");
        Check(customize->GetControlState(999, &own) == E_INVALIDARG, "unknown live control rejected");
        if (script->remove_button_sink) {
            Check(dialog->Unadvise(state_.cookie) == S_OK, "button callback safely Unadvises self");
            state_.external->Reset(); Check(!state_.destroyed, "button proxy keeps released sink alive through callback");
        }
        if (script->button_close) {
            Check(customize->SetControlState(72, CDCS_VISIBLE) == S_OK, "live persistent state commits before closing");
            script->fail_update = true;
            Check(customize->SetControlState(72, CDCS_ENABLED) == E_ACCESSDENIED && customize->GetControlState(72, &own) == S_OK && own == CDCS_VISIBLE,
                "failed UI update restores native mirror and retains committed ownstate");
            script->fail_update = true;
            Check(customize->SetControlLabel(72, L"Rejected label") == E_ACCESSDENIED, "failed UI label update rolls native label back");
            Check(script->button_label == L"Updated" && script->button_state == CDCS_VISIBLE, "post-commit UI failure restores both UI label and state");
            Check(dialog->Close(kClose) == S_OK, "button Close is explicit terminal action");
        }
        return E_FAIL; // Control events have no veto semantics.
    }
    HRESULT STDMETHODCALLTYPE OnItemSelected(IFileDialogCustomize* custom, DWORD id, DWORD item) override {
        ++script->item_calls; DWORD selected = 99;
        if(script->value_controls) { Check(id==206 && item==207 && custom->GetSelectedControlItem(id,&selected)==S_OK && selected==207,"radio callback sees native mirrored selection"); return S_OK; }
        if(script->menus) { Check(id==1 && item==17 && FAILED(custom->GetSelectedControlItem(id,&selected)),"menu click has no persistent selection"); return S_OK; }
        ComPtr<IUnknown> a, b; custom->QueryInterface(IID_PPV_ARGS(&a)); ComPtr<IFileDialog> dialog; custom->QueryInterface(IID_PPV_ARGS(&dialog)); dialog.As(&b);
        Check(script->combo && id == 1 && item == 5 && a.Get() == b.Get(), "combo event retains exact IDs and wrapper identity");
        Check(custom->GetSelectedControlItem(id, &selected) == S_OK && selected == item, "combo callback observes native-mirrored new selection");
        Check(custom->SetSelectedControlItem(id, 0) == S_OK && custom->GetSelectedControlItem(id, &selected) == S_OK && selected == 0 &&
            custom->SetSelectedControlItem(id, 5) == S_OK, "callback selection update round-trips ID zero without recursive notifications");
        CDCONTROLSTATEF prior{}, after{}; custom->GetControlItemState(id, 6, &prior); script->fail_update = true;
        Check(custom->SetControlItemState(id, 6, CDCS_ENABLED) == E_ACCESSDENIED && custom->GetControlItemState(id, 6, &after) == S_OK && after == prior,
            "failed live combo update rolls native and UI back to old ownstate");
        Check(custom->AddControlItem(id, 99, L"late") == E_NOTIMPL, "live structural mutation explicitly rejected without native change");
        return E_ACCESSDENIED; // Notification HRESULT must not veto FileOk.
    }
    HRESULT STDMETHODCALLTYPE OnCheckButtonToggled(IFileDialogCustomize* custom, DWORD id, BOOL checked) override {
        if(script->value_controls){++script->check_events;BOOL actual=FALSE;Check(id==210 && checked && custom->GetCheckButtonState(id,&actual)==S_OK && actual,"checkbox callback sees native mirrored value");}
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE OnControlActivating(IFileDialogCustomize* custom, DWORD id) override { if(script->menus) { ++script->activating; if(id==50) Check(custom->SetControlItemState(50,201,static_cast<CDCONTROLSTATEF>(3))==S_OK,"activation updates disabled item state"); } return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        ++state_.calls;
        if(script->no_test_save){
            ComPtr<IShellItem> item;PWSTR path=nullptr;
            Check(dialog->GetResult(&item)==S_OK&&item&&item->GetDisplayName(SIGDN_FILESYSPATH,&path)==S_OK&&path&&script->file==path,"Save FileOk exposes exact missing-parent Shell result");CoTaskMemFree(path);
            if(script->missing_veto){if(state_.calls>1)Check(dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED))==S_OK,"cancel Save after veto");return S_FALSE;}
            return S_OK;
        }
        if (script->missing_open || script->folder_mode) {
            ComPtr<IShellItem> single;const HRESULT one=dialog->GetResult(&single);
            Check(script->folder_multi ? one==E_UNEXPECTED&&!single : one==S_OK&&single,"missing FileOk single-result semantics");
            ComPtr<IFileOpenDialog> open;dialog->QueryInterface(IID_PPV_ARGS(&open));
            ComPtr<IShellItemArray> items;DWORD count=0;
            Check(open&&open->GetResults(&items)==S_OK&&items&&items->GetCount(&count)==S_OK&&count==(script->folder_multi?2u:1u),"missing FileOk candidate count");
            for(DWORD i=0;i<count;++i){ComPtr<IShellItem> item;PWSTR path=nullptr;
                const auto expected=i?script->second:script->file;
                Check(items->GetItemAt(i,&item)==S_OK&&item->GetDisplayName(SIGDN_FILESYSPATH,&path)==S_OK&&path&&expected==path,"missing FileOk paths preserve order");CoTaskMemFree(path);}
            if(script->missing_veto){if(state_.calls>1)Check(dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED))==S_OK,"cancel after missing candidate veto");return S_FALSE;}
            return S_OK;
        }
        if(script->metadata) {
            ComPtr<IFileSaveDialog> save;dialog->QueryInterface(IID_PPV_ARGS(&save));ComPtr<IPropertyStore> properties;
            Check(save && save->GetProperties(&properties)==S_OK,"FileOk exposes edited metadata snapshot");CheckMetadata(properties.Get());
            if(script->metadata_cancel){Check(dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED))==S_OK,"metadata cancel closes exact dialog");return S_OK;}
        }
        if(script->value_controls) {
            ComPtr<IFileDialogCustomize> custom;dialog->QueryInterface(IID_PPV_ARGS(&custom));PWSTR text=nullptr;
            Check(custom && custom->GetEditBoxText(213,&text)==S_OK && text && std::wstring(text)==L"typed value","FileOk reads edited native mirrored text");CoTaskMemFree(text);
        }
        if(script->menus) { ComPtr<IFileDialogCustomize> custom;dialog->QueryInterface(IID_PPV_ARGS(&custom));DWORD selected=99;Check(custom && custom->GetSelectedControlItem(50,&selected)==S_OK && selected==2,"FileOk sees dropdown selection before acceptance"); }
        if (state_.order) state_.order->push_back(state_.role);
        ComPtr<IOleWindow> ole; Check(SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&ole))), "callback IOleWindow");
        ComPtr<IUnknown> a, b; dialog->QueryInterface(IID_PPV_ARGS(&a)); if (ole) ole.As(&b);
        Check(a && a.Get() == b.Get(), "IOleWindow shares controlling IUnknown identity");
        HWND window = nullptr; Check(ole && ole->GetWindow(&window) == S_OK && window == script->window, "actual current HWND");
        ComPtr<IShellItem> item; const HRESULT hr = dialog->GetResult(&item); PWSTR path = nullptr;
        if (item) item->GetDisplayName(SIGDN_FILESYSPATH, &path);
        Check(script->combo ? (FAILED(hr) && !path && !item) : (hr == S_OK && path && state_.expected == path), "FileOk GetResult is exact single item or rejects multiple choices"); CoTaskMemFree(path);
        ComPtr<IShellItem> selection, folder; UINT index = 0; PWSTR name = nullptr;
        Check(dialog->GetCurrentSelection(&selection) == S_OK && dialog->GetFolder(&folder) == S_OK &&
            dialog->GetFileName(&name) == S_OK && name && std::filesystem::path(script->file).filename().wstring() == name &&
            dialog->GetFileTypeIndex(&index) == S_OK && index == 1, "active coherent snapshot getters"); CoTaskMemFree(name);
        Check(dialog->SetFileName(L"must-not-reach-native.txt") == E_UNEXPECTED, "live setter fails without mutating hidden native dialog");
        if (script->combo) {
            ComPtr<IFileOpenDialog> open; dialog->QueryInterface(IID_PPV_ARGS(&open));
            ComPtr<IShellItemArray> candidates, selections; DWORD count = 0;
            Check(open->GetResults(&candidates) == S_OK && candidates->GetCount(&count) == S_OK && count == 2,
                "FileOk GetResults materializes complete candidate array");
            Check(open->GetSelectedItems(&selections) == S_OK && selections->GetCount(&count) == S_OK && count == 2,
                "active GetSelectedItems materializes complete selection array");
            return S_OK;
        }
        if (script->readonly_properties) {
            ComPtr<IFileSaveDialog> save; dialog->QueryInterface(IID_PPV_ARGS(&save));
            ComPtr<IPropertyStore> store; DWORD count = MAXDWORD;
            Check(save && save->GetProperties(&store) == S_OK && store && store->GetCount(&count) == S_OK && count == 0,
                "FileOk returns actual standard empty property store for guarded readonly collection");
            if (script->property_close) Check(dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED)) == S_OK, "property FileOk Close cancellation remains exact");
            return S_OK;
        }
        if (script->overwrite_mode || script->native_options_case) return S_OK;
        if (state_.notimpl_case) return state_.notimpl_sink ? E_NOTIMPL : S_OK;
        if (state_.multi_mode) {
            if (state_.role == 1 && state_.calls == 1 && state_.multi_mode >= 3)
                Check(dialog->Unadvise(state_.multi_mode == 3 ? state_.cookie : state_.remove_cookie) == S_OK,
                    "multi-sink callback removes self or later sink");
            const unsigned veto_role = state_.multi_mode == 2 ? 2u : 1u;
            return state_.role == veto_role && state_.calls == 1 ? S_FALSE : S_OK;
        }
        if (state_.self_unadvise) {
            Check(dialog->Unadvise(state_.cookie) == S_OK, "Unadvise inside callback");
            state_.external->Reset(); Check(!state_.destroyed, "proxy holds sink alive after external release");
            Check(dialog->Close(kClose) == S_OK && dialog->Close(E_FAIL) == S_OK, "callback Close first result wins");
            return S_OK;
        }
        return state_.calls == 1 ? S_FALSE : S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem* proposed) override {
        if(script->computer_place && script->folder==L"pulse:computer") {
            int order=1;Check(proposed && proposed->Compare(script->computer.Get(),SICHINT_CANONICAL,&order)==S_OK && order==0,"FolderChanging exposes actual Computer Shell identity");
            ++script->computer_changing;
        }
        return E_NOTIMPL;
    }
    HRESULT Notification(unsigned kind) {
        if (state_.notifications) state_.notifications->push_back(kind * 10 + state_.role);
        return state_.notimpl_sink ? E_NOTIMPL : S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog* dialog) override {
        if(script->computer_place && script->folder==L"pulse:computer") {
            ComPtr<IShellItem> folder,result;int order=1;
            Check(dialog->GetFolder(&folder)==S_OK && folder && folder->Compare(script->computer.Get(),SICHINT_CANONICAL,&order)==S_OK && order==0,"GetFolder at Computer returns actual virtual Shell item");
            Check(FAILED(dialog->GetResult(&result)) && !result,"virtual navigation root never becomes accepted file");
            ++script->computer_changed;
        }
        return Notification(2);
    }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return Notification(3); }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog* dialog) override { if(script->value_controls) {
            ComPtr<IFileDialogCustomize> custom;dialog->QueryInterface(IID_PPV_ARGS(&custom));
            for(DWORD id:{206u,210u,213u})Check(custom->SetControlState(id,static_cast<CDCONTROLSTATEF>(3))==S_OK,"type change reveals initially hidden control");
            Check(custom->SetEditBoxText(213,L"live text")==S_OK && custom->SetCheckButtonState(210,FALSE)==S_OK,"live edit/check setter publishes UI state");
            script->fail_update=true;Check(custom->SetEditBoxText(213,L"rejected")==E_ACCESSDENIED,"failed edit publish reports failure");
            PWSTR text=nullptr;Check(custom->GetEditBoxText(213,&text)==S_OK && text && std::wstring(text)==L"live text","failed edit publish restores native value");CoTaskMemFree(text);
        } ComPtr<IShellItem> item; Check(FAILED(dialog->GetResult(&item)) && !item, "result unavailable outside FileOk before acceptance"); return Notification(1); }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog* dialog, IShellItem* item, FDE_SHAREVIOLATION_RESPONSE* response) override {
        *response = FDESVR_DEFAULT;
        if (!script->share) return S_OK;
        ++script->share_calls;
        PWSTR path = nullptr;
        Check(item && item->GetDisplayName(SIGDN_FILESYSPATH, &path) == S_OK && path && script->file == path,
            "share event exposes exact existing Shell item");
        CoTaskMemFree(path);
        ComPtr<IShellItem> result;
        Check(FAILED(dialog->GetResult(&result)) && !result, "share event does not authorize provisional results");
        if (script->share_mode == 3) {
            Check(dialog->Close(kClose) == S_OK, "Close during share event wins");
            *response = FDESVR_ACCEPT; return E_FAIL;
        }
        if (script->share_mode == 4 && state_.role == 1) return E_NOTIMPL;
        *response = script->overwrite_response == PULSE_PICKER_UI_V2_OVERWRITE_ACCEPT ? FDESVR_ACCEPT :
            script->overwrite_response == PULSE_PICKER_UI_V2_OVERWRITE_REFUSE ? FDESVR_REFUSE : FDESVR_DEFAULT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog* dialog, IShellItem* item, FDE_OVERWRITE_RESPONSE* response) override {
        ++script->overwrite_calls;
        PWSTR path = nullptr; const HRESULT named = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
        Check(named == S_OK && path && script->file == path && !state_.calls, "OnOverwrite has exact existing ShellItem before FileOk"); CoTaskMemFree(path);
        ComPtr<IShellItem> provisional; Check(FAILED(dialog->GetResult(&provisional)) && !provisional, "overwrite is not FileOk result authorization");
        if (script->overwrite_mode) {
            script->overwrite_order.push_back(state_.role);
            const unsigned mode = script->overwrite_mode;
            if (state_.role == 1 && (mode == 4 || mode == 5)) Check(dialog->Unadvise(mode == 4 ? state_.cookie : state_.remove_cookie) == S_OK, "Overwrite callback unsubscription succeeds");
            if (state_.role == 1 && mode == 8) Check(dialog->Advise(this, &state_.added_cookie) == S_OK, "Overwrite can add subscription for later rounds only");
            if (mode == 6 || mode == 8 || (state_.role == 1 && mode >= 3 && mode <= 5)) { *response = FDEOR_DEFAULT; return E_NOTIMPL; }
            if (state_.role == 1 && mode == 9) { Check(dialog->Close(kClose) == S_OK, "Overwrite Close wins before later sinks"); *response = FDEOR_ACCEPT; return E_FAIL; }
            *response = state_.role == 1 ? (mode == 2 ? FDEOR_REFUSE : mode == 7 ? FDEOR_DEFAULT : FDEOR_ACCEPT) :
                (mode == 1 || (mode == 3 && state_.role == 3)) ? FDEOR_REFUSE : FDEOR_ACCEPT;
            return S_OK;
        }
        *response = script->overwrite_response == PULSE_PICKER_UI_V2_OVERWRITE_ACCEPT ? FDEOR_ACCEPT :
            script->overwrite_response == PULSE_PICKER_UI_V2_OVERWRITE_REFUSE ? FDEOR_REFUSE : FDEOR_DEFAULT;
        return S_OK;
    }
private:
    std::atomic<ULONG> refs_{1}; SinkState& state_;
};
void FolderCase(const std::wstring& folder, bool multi) {
    Script state; script = &state; state.folder = folder; state.file = folder + L"\\named-folder"; state.folder_mode = true; state.folder_multi = multi; state.second = folder;
    Check(CreateDirectoryW(state.file.c_str(), nullptr) != FALSE, "create named folder fixture");
    auto provider = std::make_shared<FileDialogInprocessProvider>();
    provider->module_lifetime = std::make_shared<int>(1); provider->allow_unverified_proof = true;
    auto& api = provider->api; api.size = sizeof(api); api.version = 2;
    api.implemented_features = PULSE_PICKER_UI_V2_FEATURE_CUSTOM_LABELS | (multi ? PULSE_PICKER_UI_V2_FEATURE_MULTI_SELECT : 0u);
    api.Initialize = Initialize; api.ValidateConfig = Validate; api.Show = Show; api.Query = Query;
    api.GetWindow = Window; api.Close = Close; api.Shutdown = Shutdown;
    FileDialogBridgeServices services; services.access_policy = AccessPolicy::LocalOpenV1;
    services.inprocess_provider = provider; services.validation_verified = RuntimeContractCertified;
    services.on_native_reason = [](const char* reason, const IID*) { std::printf("folder diagnostic: %s\n", reason); };
    unsigned native = 0; services.show_native = [&](IFileDialog*, HWND) { ++native; return E_FAIL; };
    ComPtr<IFileOpenDialog> dialog;
    Check(CreateIsolatedDialog(DialogFactory::Open, services, IID_PPV_ARGS(&dialog)) == S_OK, "isolated folder factory");
    if (!dialog) return;
    ComPtr<IShellItem> item; Check(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item)) == S_OK, "folder fixture item");
    Check(dialog->SetOptions(static_cast<FILEOPENDIALOGOPTIONS>(multi ? 0x1260 : 0x101828)) == S_OK, "folder options");
    Check(dialog->SetFileName(L"named-folder") == S_OK && dialog->SetTitle(L"Select") == S_OK, "folder text setters");
    const COMDLG_FILTERSPEC filter{L"All", L"*.*"};
    Check(dialog->SetFileTypes(1, &filter) == E_UNEXPECTED, "native folder filter HRESULT preserved");
    Check(dialog->SetFileTypeIndex(1) == E_UNEXPECTED, "native folder filter index HRESULT preserved without blocking folder replacement");
    Check(dialog->SetFolder(item.Get()) == S_OK, "folder explicit location");
    SinkState observer; observer.expected = state.file;
    ComPtr<IFileDialogEvents> sink; sink.Attach(new Sink(observer));
    Check(dialog->Advise(sink.Get(), &observer.cookie) == S_OK, "folder event subscription");
    if (navigation_case) {
        api.implemented_features |= PULSE_PICKER_UI_V2_FEATURE_NAVIGATE; api.Navigate = NavigateFixture;
        show_action = [&] {
            const auto prior = state.folder;
            ComPtr<IShellItem> target;
            Check(SHCreateItemFromParsingName(state.second.c_str(), nullptr, IID_PPV_ARGS(&target)) == S_OK, "navigation target item");
            navigation_result = S_FALSE;
            Check(dialog->SetFolder(target.Get()) == S_FALSE && state.folder == prior, "live navigation veto retains folder");
            navigation_result = HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
            Check(dialog->SetFolder(target.Get()) == navigation_result && state.folder == prior, "live navigation failure retains folder");
            navigation_result = E_PENDING;
            Check(dialog->SetFolder(target.Get()) == S_OK && state.folder == prior, "queued SetFolder returns native success before commit");
            auto complete = [&](HRESULT result) {
                PulsePickerUiV2Event event{}; event.size = sizeof(event); event.kind = PULSE_PICKER_UI_V2_EVENT_NAVIGATION_COMPLETE;
                event.show_id = 1; event.generation = state.generation; event.window = state.window;
                event.proposed_folder = {state.second.data(), static_cast<uint32_t>(state.second.size())}; event.navigation_result = result;
                return navigation_callbacks->callback(navigation_callbacks->context, &event);
            };
            Check(complete(S_FALSE) == S_OK && state.folder == prior, "queued veto clears request without committing folder");
            Check(dialog->SetFolder(target.Get()) == S_OK && state.folder == prior, "queued retry remains uncommitted");
            state.folder = state.second; ++state.generation;
            Check(complete(S_OK) == S_OK, "queued success commits native mirror after UI completion");
            navigation_result = S_OK;
            Check(dialog->SetFolder(target.Get()) == S_OK && state.folder == state.second, "live SetFolder reaches provider");
            ComPtr<IShellItem> actual; PWSTR text = nullptr;
            const HRESULT got = dialog->GetFolder(&actual);
            Check(got == S_OK && actual && actual->GetDisplayName(SIGDN_FILESYSPATH, &text) == S_OK && text && state.folder == text, "GetFolder sees committed navigation");
            CoTaskMemFree(text);
            Check(dialog->SetFolder(nullptr) == E_INVALIDARG, "live null folder rejected");
        };
    }
    Check(dialog->Show(nullptr) == S_OK && !native, "folder accepted without native fallback");
    show_action = {};
    ComPtr<IShellItemArray> results; DWORD count = 0;
    Check(dialog->GetResults(&results) == S_OK && results && results->GetCount(&count) == S_OK && count == (multi ? 2u : 1u), "folder result array");
    state.candidate = false; state.cancel_before_accept = true;
    Check(dialog->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && !native, "folder cancellation terminal");
    item.Reset(); Check(FAILED(dialog->GetResult(&item)) && !item, "folder cancellation clears result");
    Check(RemoveDirectoryW(state.file.c_str()) != FALSE, "remove named folder fixture");
}
void Case(const std::wstring& folder, int mode) {
    Script state; state.folder = folder; state.file = folder + L"\\owned.txt";
    const bool save_case = mode >= 33;
    state.overwrite_mode = mode == 38 ? 1u : mode >= 41 ? static_cast<unsigned>(mode - 39) : 0u;
    state.save = save_case; state.overwrite = mode == 34 || mode == 35 || state.overwrite_mode;
    if (save_case && !state.overwrite) state.file = folder + L"\\new-save.txt";
    state.overwrite_response = mode == 35 || state.overwrite_mode == 2 || state.overwrite_mode == 9 ? PULSE_PICKER_UI_V2_OVERWRITE_REFUSE :
        state.overwrite_mode >= 6 && state.overwrite_mode <= 8 ? PULSE_PICKER_UI_V2_OVERWRITE_DEFAULT : PULSE_PICKER_UI_V2_OVERWRITE_ACCEPT;
    state.custom = mode == 18 || mode == 19 || mode == 26 || mode == 27 || mode == 28 || mode == 31; state.button_close = mode == 19 || mode == 26; state.remove_button_sink = mode == 26;
    state.corrupt_result = mode == 3; state.fail_after_callback = mode == 4; state.filtered = mode == 13; script = &state;
    auto provider = std::make_shared<FileDialogInprocessProvider>(); provider->module_lifetime = std::make_shared<int>(1);
    provider->allow_unverified_proof = mode != 2;
    auto& api = provider->api; api.size = sizeof(api); api.version = 2;
    api.Initialize = Initialize; api.ValidateConfig = Validate; api.Show = Show; api.Query = Query; api.GetWindow = Window; api.Close = Close; api.Shutdown = Shutdown; api.UpdateControl = mode == 21 ? nullptr : UpdateControl;
    unsigned native_calls = 0;
    bool unknown_probe = false, customize_probe = false, customize_gate = false;
    std::vector<std::string> customize_diagnostics, stage_diagnostics;
    FileDialogBridgeServices services; services.access_policy = AccessPolicy::LocalOpenV1;
    if (save_case && mode != 40) services.save_access_policy = AccessPolicy::LocalSaveV1;
    services.validation_verified = [](const ContractDecision& decision) { return decision.supported_access_policy == AccessPolicy::LocalOpenV1 || decision.supported_access_policy == AccessPolicy::LocalSaveV1; };
    services.show_native = [&](IFileDialog* native, HWND) {
        ++native_calls;
        if (mode == 19 || mode == 26) {
            ComPtr<IFileDialogCustomize> custom; native->QueryInterface(IID_PPV_ARGS(&custom)); CDCONTROLSTATEF value{};
            Check(custom && custom->GetControlState(72, &value) == S_OK && value == CDCS_VISIBLE,
                "second Show native seam observes committed state, not stale or failed update");
        }
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    };
    services.on_native_reason = [&](const char* reason, const IID* iid) {
        if (std::strstr(reason, "inprocess-") == reason || std::strstr(reason, "observe-") == reason || std::strstr(reason, "getter-") == reason) stage_diagnostics.emplace_back(reason);
        if (std::strcmp(reason, "query-interface-not-supported") == 0 && iid && *iid == IID_IStream) unknown_probe = true;
        if (std::strcmp(reason, "query-interface-customize-only") == 0 && iid && *iid == IID_IFileDialogCustomize) customize_probe = true;
        if (std::strstr(reason, "customize-call method=") == reason && iid && *iid == IID_IFileDialogCustomize) customize_gate = true;
        if (iid && *iid == IID_IFileDialogCustomize && std::strstr(reason, "customize-result method=") == reason)
            customize_diagnostics.emplace_back(reason);
        if (mode == 17) throw 1; // Diagnostic failure must not change the route or HRESULT.
    };
    if (mode != 6 && mode != 37) services.inprocess_provider = provider;
    bool published = false, disabled = mode == 32;
    unsigned resolutions = 0;
    const bool deferred_case = mode >= 28 && mode <= 32;
    if (deferred_case) {
        auto ready_services = services;
        services.inprocess_provider.reset(); services.validation_verified = {};
        services.resolve_on_show = [&, ready_services](FileDialogBridgeServices& out) {
            ++resolutions;
            if (mode == 30) throw 1;
            if (!published || disabled) return false;
            out = ready_services; return true;
        };
    }
    ComPtr<IFileDialog> dialog;
    HRESULT activated = E_FAIL;
    if (mode == 31) {
        FileDialogClassActivation activation; activation.enabled = true; activation.original = &::CoGetClassObject; activation.services = services;
        ComPtr<IClassFactory> factory;
        activated = ActivateFileDialogClass(activation, CLSID_FileOpenDialog, CLSCTX_INPROC_SERVER, nullptr, IID_PPV_ARGS(&factory));
        Check(activated == S_OK && factory && resolutions == 0, "early real native class factory retains deferred resolver without invoking it");
        published = true;
        if (factory) activated = factory->CreateInstance(nullptr, IID_PPV_ARGS(&dialog));
    } else if (deferred_case) {
        FileDialogActivation activation; activation.enabled = true; activation.original = &::CoCreateInstance; activation.services = services;
        activated = ActivateFileDialog(activation, CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    } else activated = (isolated_native ? CreateIsolatedDialog : CreatePulseFileDialog)(mode == 7 || save_case ? DialogFactory::Save : DialogFactory::Open, services, IID_PPV_ARGS(&dialog));
    if (FAILED(activated)) std::printf("ACTIVATION mode=%d hr=0x%08lX\n", mode, static_cast<unsigned long>(activated));
    Check(SUCCEEDED(activated) && dialog, "real native COM activation wrapped");
    if (!dialog) return;
    ComPtr<IShellItem> directory; Check(SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&directory))), "real fixture Shell folder");
    const FILEOPENDIALOGOPTIONS options = FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT |
        (save_case ? FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN : mode == 7 ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST);
    Check(dialog->SetOptions(options) == S_OK &&
        dialog->SetFolder(directory.Get()) == S_OK && dialog->SetFileName(std::filesystem::path(state.file).filename().c_str()) == S_OK, "real native setters prepare LocalOpenV1");
    if (save_case) Check(dialog->SetDefaultExtension(L"txt") == S_OK, "Save default extension mirrored natively");
    if (mode == 13) {
        const COMDLG_FILTERSPEC all{L"All files", L"*.*"};
        Check(dialog->SetFileTypes(1, &all) == S_OK && dialog->SetFileTypeIndex(0) == S_OK, "real native filter and zero-index setters");
    }
    const bool notimpl_case = mode == 14 || mode == 15 || mode == 27;
    const bool multiple = state.overwrite_mode || mode == 5 || (mode >= 10 && mode <= 12) || notimpl_case;
    std::vector<unsigned> callback_order, notification_order;
    SinkState observer; observer.expected = state.file; observer.self_unadvise = mode == 1 || mode == 36;
    observer.multi_mode = mode == 5 ? 1u : multiple && mode >= 10 ? static_cast<unsigned>(mode - 8) : 0u;
    observer.role = 1; observer.order = multiple ? &callback_order : nullptr;
    observer.notimpl_case = notimpl_case; observer.notimpl_sink = mode == 14;
    observer.notifications = notimpl_case ? &notification_order : nullptr;
    ComPtr<IFileDialogEvents> sink; sink.Attach(new Sink(observer)); observer.external = std::addressof(sink);
    Check(sink && !observer.destroyed, "retained external ComPtr address does not release sink");
    const HRESULT advised = dialog->Advise(sink.Get(), &observer.cookie);
    std::printf("ADVISE case=%d sink=%p hr=%08lX cookie=%lu\n", mode, static_cast<void*>(sink.Get()), static_cast<unsigned long>(advised), observer.cookie);
    Check(advised == S_OK, "real native Advise retains proxy");
    SinkState second; ComPtr<IFileDialogEvents> extra; DWORD extra_cookie = 0;
    if (multiple) {
        second.expected = state.file; second.multi_mode = observer.multi_mode; second.role = 2; second.order = &callback_order;
        second.notimpl_case = notimpl_case; second.notimpl_sink = mode == 15;
        second.notifications = notimpl_case ? &notification_order : nullptr;
        extra.Attach(new Sink(second)); Check(dialog->Advise(extra.Get(), &extra_cookie) == S_OK, "second native subscription");
        second.cookie = extra_cookie; observer.remove_cookie = extra_cookie;
    }
    SinkState third; ComPtr<IFileDialogEvents> third_sink; DWORD third_cookie = 0;
    if (state.overwrite_mode) {
        third.expected = state.file; third.role = 3; third.order = &callback_order;
        third_sink.Attach(new Sink(third)); Check(dialog->Advise(third_sink.Get(), &third_cookie) == S_OK, "third native subscription for overwrite arbitration");
        third.cookie = third_cookie;
    }
    if (mode == 8) {
        ComPtr<IFileDialogCustomize> customize;
        const HRESULT custom_query = dialog.As(&customize);
        const HRESULT added = customize ? customize->AddText(9001, L"Unsupported static text") : E_NOINTERFACE;
        Check(SUCCEEDED(custom_query) && customize && SUCCEEDED(added), "actual customization remains native");
        if (customize) {
            CDCONTROLSTATEF control_state{};
            const HRESULT read = customize->GetControlState(9001, &control_state);
            const HRESULT failed = customize->SetSelectedControlItem(0xFFFFu, 0xEEEEu);
            Check(SUCCEEDED(read) && FAILED(failed), "native successful Customize read and invalid-control mutation observed");
            const auto has = [&](const char* method, const char* kind, HRESULT hr, const char* outcome) {
                char result[32]{}; std::snprintf(result, sizeof(result), "hr=0x%08lX", static_cast<unsigned long>(hr));
                for (const auto& line : customize_diagnostics)
                    if (line.find(method) != std::string::npos && line.find(kind) != std::string::npos &&
                        line.find(result) != std::string::npos && line.find(outcome) != std::string::npos) return true;
                return false;
            };
            Check(has("method=AddText ", "kind=mutation", added, added == S_OK ? "outcome=mutation-ok" : "outcome=other-success") &&
                has("method=GetControlState ", "kind=read", read, read == S_OK ? "outcome=read-ok" : "outcome=other-success") &&
                has("method=SetSelectedControlItem ", "kind=mutation", failed, "outcome=failed"),
                "Customize diagnostics preserve exact static method, read/mutation and actual HRESULT outcome");
            for (const auto& detail : customize_diagnostics) std::printf("CUSTOMIZE_DIAGNOSTIC %s\n", detail.c_str());
        }
    }
    if (mode == 16 || mode == 17) {
        ComPtr<IStream> unsupported;
        Check(dialog.As(&unsupported) == E_NOINTERFACE && !unsupported, "optional unsupported QI keeps original HRESULT and null output");
        ComPtr<IFileDialogCustomize> customize;
        Check(SUCCEEDED(dialog.As(&customize)) && customize, "Customize capability QI without method call");
    }
    if (mode >= 18 && mode <= 32) {
        ComPtr<IFileDialogCustomize> custom; dialog.As(&custom);
        Check(custom->StartVisualGroup(71, L"Tools") == S_OK && custom->AddPushButton(72, L"Action") == S_OK && custom->EndVisualGroup() == S_OK, "native group/button/end all succeed before mirroring");
        if (mode == 22) Check(FAILED(custom->AddPushButton(72, L"Duplicate")), "duplicate native ID failure is not hidden");
        if (mode == 23) Check(custom->StartVisualGroup(73, L"Outer") == S_OK && FAILED(custom->StartVisualGroup(74, L"Nested")), "nested native group failure is not hidden");
        if (mode == 24) Check(FAILED(custom->EndVisualGroup()), "unbalanced native End failure is not hidden");
        if (mode == 25) for (DWORD id = 100; id < 115; ++id) Check(custom->AddPushButton(id, L"Extra") == S_OK, "native extra controls keep original successful HRESULT despite model limit");
        if (mode == 20) Check(FAILED(custom->SetControlState(999, CDCS_VISIBLE)), "unknown pre-Show control preserves native failure and fallback");
    }
    if (mode == 39) Check(dialog->SetOptions(options | FOS_NOVALIDATE) == S_OK, "Save unsupported option retains native setter success");
    if (mode == 9) Check(dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT | FOS_ALLOWMULTISELECT) == S_OK, "multiselect without provider feature remains native");
    if (mode == 28 || mode == 32) published = true; // Deterministic publication, no sleeps.
    if (deferred_case) Check(resolutions == 0, "early setters/custom/Advise do not resolve or wait for startup");
    const HRESULT shown = dialog->Show(nullptr);
    if (deferred_case) Check(resolutions == 1, "Show resolves services exactly once");
    if (mode == 0) {
        const auto has = [&](const char* value) { return std::find(stage_diagnostics.begin(), stage_diagnostics.end(), value) != stage_diagnostics.end(); };
        Check(has("inprocess-owner provided=0 valid=0 samepid=0 samethread=0 enabled=0") &&
            has("inprocess-prepare-enter") && has("inprocess-prepare-return hr=0x00000000") &&
            has("inprocess-api-show-enter") && has("inprocess-observe-return hr=0x00000000") &&
            has("inprocess-sink-return kind=5 order=1 rawhr=0x00000000") && has("getter-GetWindow hr=0x00000000") &&
            has("getter-GetFileTypeIndex hr=0x00000000") && has("inprocess-event-return kind=6 hr=0x00000001") &&
            has("inprocess-event-return kind=6 hr=0x00000000"), "stage diagnostics distinguish prepare, owner, Show, veto and acceptance");
        bool returned = false;
        for (const auto& line : stage_diagnostics) {
            if (line.find("inprocess-api-show-return hr=0x00000000 status=") == 0 && line.find(" win32=0 terminal=0x00000000") != std::string::npos) returned = true;
            std::printf("STAGE_DIAGNOSTIC %s\n", line.c_str());
        }
        Check(returned, "actual API Show return logs status/flags/win32/terminal HRESULT before validation");
    }
    if (mode == 27) Check(state.button_calls == 2, "button E_FAIL is ignored and later subscribed sink still runs");
    if (mode == 8) Check(customize_probe && customize_gate, "diagnostics distinguish Customize probe from actual method gate");
    if (mode == 16 || mode == 17) Check(unknown_probe && customize_probe && !customize_gate && native_calls == 0,
        "pure QI probes record exact IID without poisoning inprocess routing, even if observer throws");
    if (mode == 2 || mode == 6 || (mode >= 20 && mode <= 25) || mode == 29 || mode == 30 || mode == 32 || (mode == 37 || mode == 39 || mode == 40) || (mode >= 7 && mode <= 9)) Check(native_calls == 1 && state.initialized == 0 && !observer.calls, "uncertified/default/unsupported contracts stay native exactly once");
    else {
        Check(native_calls == 0 && state.shown == 1 && state.stopped == 1, "explicit v2 executes once and shuts down; no native replay");
        if (mode == 19 || mode == 26) Check(shown == kClose && observer.calls == 0, "button Close returns exact HRESULT without FileOk/native replay");
        else if (mode == 1 || mode == 36) Check(shown == kClose && observer.calls == 1 && observer.destroyed, "self-Unadvise Close keeps sink alive until callback unwinds");
        else if (state.overwrite_mode) {
            const unsigned m = state.overwrite_mode;
            const std::vector<unsigned> overwrite_expected = m == 3 || m == 4 ? std::vector<unsigned>{1, 2} :
                m == 5 ? std::vector<unsigned>{1, 3} : m == 6 || m == 8 ? std::vector<unsigned>{1, 2, 3} : std::vector<unsigned>{1};
            const std::vector<unsigned> fileok_expected = m == 2 || m == 9 ? std::vector<unsigned>{} : m == 4 ? std::vector<unsigned>{2, 3} :
                m == 5 ? std::vector<unsigned>{1, 3} : m == 8 ? std::vector<unsigned>{1, 2, 3, 1} : std::vector<unsigned>{1, 2, 3};
            Check(state.overwrite_order == overwrite_expected && callback_order == fileok_expected,
                "three-sink overwrite exact order, first handled response, and subscription snapshot match evidence");
            Check(shown == (m == 2 ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : m == 9 ? kClose : S_OK), "multi-sink overwrite exact terminal status");
        }
        else if (mode == 35) Check(shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) && observer.calls == 0, "REFUSE does not emit FileOk and later cancellation is exact");
        else if (mode == 3) Check(shown == E_UNEXPECTED, "mismatched result generation fails terminally");
        else if (mode == 4) Check(shown == E_ACCESSDENIED, "failure after sink side effects never reopens native");
        else if (notimpl_case) {
            Check(shown == S_OK && callback_order == std::vector<unsigned>{1, 2}, "E_NOTIMPL FileOk continues in both sink orders");
            Check(notification_order == std::vector<unsigned>{11, 12, 21, 22, 31, 32}, "E_NOTIMPL Type/Folder/Selection notification continues in Advise order");
        } else if (multiple) {
            const std::vector<unsigned> expected = mode == 5 ? std::vector<unsigned>{1, 1, 2} :
                mode == 10 ? std::vector<unsigned>{1, 2, 1, 2} : mode == 11 ? std::vector<unsigned>{1, 2} : std::vector<unsigned>{1, 1};
            Check(shown == S_OK && callback_order == expected, "native-oracle FileOk order, first-veto short circuit and unsubscription");
        } else Check(shown == S_OK && observer.calls == 2, "FileOk veto then accepts same candidate");
        ComPtr<IShellItem> result;
        const bool accepted = mode == 0 || mode == 13 || mode == 16 || mode == 17 || mode == 18 || mode == 28 || mode == 31 || mode == 33 || mode == 34 || (multiple && state.overwrite_mode != 2 && state.overwrite_mode != 9);
        Check((accepted ? dialog->GetResult(&result) == S_OK : FAILED(dialog->GetResult(&result))) && (accepted || !result), "only final accepted Show publishes result");
    }
    if (mode == 0) {
        state.candidate = false; state.cancel_before_accept = true;
        Check(dialog->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && native_calls == 0 && state.shown == 2,
            "successful object can show again and cancel without native fallback");
        ComPtr<IShellItem> previous;
        Check(FAILED(dialog->GetResult(&previous)) && !previous, "cancelled reuse clears prior accepted result");
        state.candidate = false; state.cancel_before_accept = false;
        Check(dialog->Show(nullptr) == S_OK && native_calls == 0 && state.shown == 3,
            "cancelled object can show again and accept");
        Check(dialog->GetResult(&previous) == S_OK && previous, "third session publishes its own accepted result");
    }
    if (mode == 29 || mode == 30 || mode == 32) {
        published = true; disabled = false;
        Check(dialog->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && native_calls == 2 && resolutions == 1 && state.shown == 0,
            "not-ready/throw Show stays native after later publication without replay or re-resolve");
    }
    if (mode == 19 || mode == 26) {
        ComPtr<IFileDialogCustomize> custom; dialog.As(&custom); CDCONTROLSTATEF value{};
        Check(custom->GetControlState(72, &value) == S_OK && value == CDCS_VISIBLE, "post-Show native getter retains live successful state");
        state.closed = false; state.candidate = false; state.cancel_before_accept = true;
        Check(dialog->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && native_calls == 0 && state.shown == 2,
            "second Show retains custom configuration and remains bridged");
    }
    if (save_case && !state.overwrite) Check(GetFileAttributesW(state.file.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND,
        "Save candidate/result ShellItem never creates owned nonexistent target");
    if (!observer.self_unadvise || observer.calls == 0) dialog->Unadvise(observer.cookie);
    if (extra_cookie) dialog->Unadvise(extra_cookie);
    if (third_cookie) dialog->Unadvise(third_cookie);
    if (observer.added_cookie) dialog->Unadvise(observer.added_cookie);
}
void OpenTrace(const std::wstring& folder, unsigned variant = 0) {
    const auto cwd_before = std::filesystem::current_path();
    Script state; script = &state; state.combo = true; state.folder = folder; state.file = folder + L"\\owned.txt"; state.second = folder + L"\\second.txt";
    state.caller_validation = variant >= 9;
    state.client_places = variant >= 10;
    state.cancel_before_accept = variant == 11;
    state.corrupt_packed = variant >= 4 && variant <= 5 ? variant - 3 : 0;
    HANDLE h = CreateFileW(state.second.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(h != INVALID_HANDLE_VALUE, "create second owned multi-select fixture"); if (h == INVALID_HANDLE_VALUE) return; CloseHandle(h);
    auto provider = std::make_shared<FileDialogInprocessProvider>(); provider->module_lifetime = std::make_shared<int>(1); provider->allow_unverified_proof = true;
    auto& api = provider->api; api.size = sizeof(api); api.version = 2; api.Initialize = Initialize; api.ValidateConfig = Validate;
    api.Show = Show; api.Query = Query; api.GetWindow = Window; api.Close = Close; api.Shutdown = Shutdown; api.UpdateControl = UpdateControl;
    api.implemented_features = PULSE_PICKER_UI_V2_FEATURE_CUSTOM_LABELS | PULSE_PICKER_UI_V2_FEATURE_CUSTOM_COMBOBOX | PULSE_PICKER_UI_V2_FEATURE_MULTI_SELECT | PULSE_PICKER_UI_V2_FEATURE_OPEN_DEFAULT_EXTENSION | PULSE_PICKER_UI_V2_FEATURE_OPEN_SKIP_ACCESS | PULSE_PICKER_UI_V2_FEATURE_FILESYSTEM_PLACES;
    if (variant >= 1 && variant <= 3) api.implemented_features &= ~(1u << (variant - 1));
    if (variant == 6) api.size -= static_cast<uint32_t>(sizeof(uint32_t));
    if (variant == 7) api.implemented_features &= ~PULSE_PICKER_UI_V2_FEATURE_OPEN_DEFAULT_EXTENSION;
    unsigned native_calls = 0;
    FileDialogBridgeServices services; services.access_policy = AccessPolicy::LocalOpenV1; services.inprocess_provider = provider;
    services.validation_verified = [](const ContractDecision& d) { return d.validation_category == ValidationCategory::LocalOpenAccessV1; };
    services.show_native = [&](IFileDialog*, HWND) { ++native_calls; return E_UNEXPECTED; };
    std::vector<std::string> setter_diagnostics;
    services.on_native_reason = [&](const char* reason, const IID*) {
        if (std::strncmp(reason, "setter ", 7) == 0 || std::strncmp(reason, "extension ", 10) == 0) setter_diagnostics.emplace_back(reason);
    };
    const std::wstring state_root = L"Software\\PulseTestFixtures\\ClientTrace-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    if (state.client_places) services.client_state = std::make_shared<FileDialogClientState>(L"isolated-test", state_root);
    const GUID client = {0x9184d113, 0x8121, 0x4463, {0x82,0x21,0x34,0x51,0x14,0x53,0x62,0x99}};
    ComPtr<IFileOpenDialog> open; Check(CreatePulseFileDialog(DialogFactory::Open, services, IID_PPV_ARGS(&open)) == S_OK, "create generic multi/labels/combo wrapper");
    if (state.client_places) Check(open->SetClientGuid(client) == S_OK, "native client GUID captured with isolated Pulse storage");
    ComPtr<IShellItem> initial; SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&initial));
    const COMDLG_FILTERSPEC types[]{{L"Text", variant == 8 ? L"*" : L"*.txt"}, {L"All", L"*.*"}};
    Check(open && open->SetTitle(L"Generic Open") == S_OK && open->SetDefaultExtension(L"txt") == S_OK &&
        open->SetFileTypes(2, types) == S_OK && open->SetOptions(static_cast<FILEOPENDIALOGOPTIONS>(variant >= 9 ? 0x20001B00 : variant == 8 ? 0x10001A00 : 0x1A48)) == S_OK &&
        open->SetFolder(initial.Get()) == S_OK && open->SetOkButtonLabel(L"") == S_OK && open->SetFileNameLabel(L"File") == S_OK,
        "native accepts observed Title/DefaultExtension/FileTypes/Options order plus fixture folder and labels");
    if (state.client_places) Check(open->AddPlace(initial.Get(), FDAP_TOP) == S_OK, "native filesystem place accepted");
    bool extension_sequence = false, extension_text = false, bounded = true;
    for (const auto& diagnostic : setter_diagnostics) {
        bounded = bounded && diagnostic.size() < 128;
        extension_sequence |= diagnostic.find(state.client_places ? "setter seq=2 method=SetDefaultExtension hr=0x00000000" : "setter seq=1 method=SetDefaultExtension hr=0x00000000") == 0;
        extension_text |= diagnostic.find(state.client_places ? "extension seq=2 offset=0 utf16=\\u0074\\u0078\\u0074" : "extension seq=1 offset=0 utf16=\\u0074\\u0078\\u0074") == 0;
    }
    Check(extension_sequence && extension_text && bounded, "extension setter sequence and escaped text survive bounded runtime log records");
    if (!open) { DeleteFileW(state.second.c_str()); return; }
    ComPtr<IFileDialogCustomize> custom; open.As(&custom);
    Check(custom->AddComboBox(1) == S_OK, "add generic combo");
    for (DWORD id : {0u, 1u, 2u, 3u, 5u, 4u, 6u}) Check(custom->AddControlItem(1, id, (L"Item " + std::to_wstring(id)).c_str()) == S_OK, "ordered generic item accepted");
    Check(custom->SetSelectedControlItem(1, 0) == S_OK, "explicit selected ID zero retained");
    CDCONTROLSTATEF own{}; Check(custom->SetControlItemState(1, 6, CDCS_VISIBLE) == S_OK &&
        custom->GetControlItemState(1, 6, &own) == S_OK && own == CDCS_VISIBLE, "item getter reflects own native state");
    SinkState sink_state; sink_state.expected = state.file; ComPtr<IFileDialogEvents> sink; sink.Attach(new Sink(sink_state));
    Check(open->Advise(sink.Get(), &sink_state.cookie) == S_OK, "advise generic combo events");
    const HRESULT shown = open->Show(nullptr);
    if (state.client_places) {
        const auto remembered = services.client_state->Read(client);
        Check(state.cancel_before_accept ? !remembered : remembered == folder, "accepted folder persisted while cancellation does not write client state");
        Check(RegDeleteTreeW(HKEY_CURRENT_USER, state_root.c_str()) == ERROR_SUCCESS || state.cancel_before_accept, "remove isolated client state fixture");
    }
    if (state.cancel_before_accept) Check(shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) && native_calls == 0, "cancel caller-validation dialog without native replay");
    else if ((variant >= 1 && variant <= 3) || variant == 6 || variant == 7) {
        Check(shown == E_UNEXPECTED && native_calls == 1 && !state.initialized && !state.shown,
            "missing exact labels/combo/multi/extension feature or old API size falls back before UI initialization");
    } else if (variant >= 4 && variant <= 5) {
        Check(shown == E_UNEXPECTED && native_calls == 0 && state.shown == 1,
            "same-generation changed or malformed full result is terminal failure with no native replay");
        ComPtr<IShellItemArray> absent; Check(FAILED(open->GetResults(&absent)) && !absent, "failed result never exposes partial accepted items");
    } else {
    Check(shown == S_OK && native_calls == 0 && state.shown == 1 && state.item_calls == 1 && sink_state.calls == 1,
        "observed Open contract reaches UI once and accepts without native fallback");
    ComPtr<IShellItemArray> results; DWORD count = 0, selected = 0;
    Check(open->GetResults(&results) == S_OK && results->GetCount(&count) == S_OK && count == 2, "final results contain both selected files");
    Check(custom->GetSelectedControlItem(1, &selected) == S_OK && selected == 5, "selected item persists in native mirror after Show");
    if (results) { ComPtr<IShellItem> second; PWSTR actual = nullptr; results->GetItemAt(1, &second); if (second) second->GetDisplayName(SIGDN_FILESYSPATH, &actual);
        Check(actual && state.second == actual, "second result exact identity path preserved"); CoTaskMemFree(actual); }
    }
    Check(std::filesystem::current_path() == cwd_before, "COM Open preserves caller CWD with or without explicit NOCHANGEDIR");
    open->Unadvise(sink_state.cookie); open.Reset(); custom.Reset(); sink.Reset();
    Check(DeleteFileW(state.second.c_str()) != FALSE, "delete only second owned fixture");
}

#include "file_dialog_deferred_history_test.inc"

void DefaultFolderTrace(const std::wstring& folder, unsigned variant) {
    const auto remembered_folder=folder+L"\\remembered";
    Check(CreateDirectoryW(remembered_folder.c_str(),nullptr)!=FALSE,"create owned remembered folder");
    Script state;script=&state;state.default_folder=true;state.cancel_before_accept=true;
    state.folder=variant==1?remembered_folder:folder;state.file=state.folder+L"\\owned.txt";
    auto provider=std::make_shared<FileDialogInprocessProvider>();provider->module_lifetime=std::make_shared<int>(1);provider->allow_unverified_proof=true;
    auto& api=provider->api;api.size=sizeof(api);api.version=2;api.Initialize=Initialize;api.ValidateConfig=Validate;api.Show=Show;api.Query=Query;api.GetWindow=Window;api.Close=Close;api.Shutdown=Shutdown;api.UpdateControl=UpdateControl;
    api.implemented_features=PULSE_PICKER_UI_V2_FEATURE_MULTI_SELECT|PULSE_PICKER_UI_V2_FEATURE_CUSTOM_LABELS;
    unsigned native_calls=0;FileDialogBridgeServices services;services.access_policy=AccessPolicy::LocalOpenV1;services.inprocess_provider=provider;
    services.validation_verified=[](const ContractDecision& d){return RuntimeContractCertified(d);};
    services.show_native=[&](IFileDialog*,HWND){++native_calls;return E_UNEXPECTED;};
    const std::wstring registry_root=L"Software\\PulseTestFixtures\\DefaultFolder-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(variant);
    services.client_state=std::make_shared<FileDialogClientState>(L"isolated-default-test",registry_root);
    GUID client{};CoCreateGuid(&client);
    if(variant)Check(services.client_state->Write(client,remembered_folder),"seed isolated Pulse history");
    ComPtr<IFileOpenDialog> open;Check(CreatePulseFileDialog(DialogFactory::Open,services,IID_PPV_ARGS(&open))==S_OK,"create default-folder wrapper");
    ComPtr<IShellItem> initial;SHCreateItemFromParsingName(folder.c_str(),nullptr,IID_PPV_ARGS(&initial));
    const COMDLG_FILTERSPEC filter{L"All",L"*.*"};
    Check(open->SetClientGuid(client)==S_OK && open->SetOptions(static_cast<FILEOPENDIALOGOPTIONS>(0x1A48))==S_OK && open->SetTitle(L"Choose") == S_OK && open->SetOkButtonLabel(L"Confirm")==S_OK && open->SetFileTypes(1,&filter)==S_OK && open->SetFileTypeIndex(1)==S_OK,"capture observed generic default-folder Open setup");
    if(variant==2)Check(open->SetFolder(initial.Get())==S_OK,"explicit folder before later default");
    Check(open->SetDefaultFolder(initial.Get())==S_OK,"native default folder setter accepted");
    Check(open->Show(nullptr)==HRESULT_FROM_WIN32(ERROR_CANCELLED) && !native_calls && state.shown==1,"default-folder contract reaches Pulse and cancels without replay");
    Check(variant?services.client_state->Read(client)==remembered_folder:!services.client_state->Read(client),"cancel does not replace client directory history");
    open.Reset();
    const auto removed=RegDeleteTreeW(HKEY_CURRENT_USER,registry_root.c_str());Check(removed==ERROR_SUCCESS || removed==ERROR_FILE_NOT_FOUND,"remove isolated registry fixture");
    Check(RemoveDirectoryW(remembered_folder.c_str())!=FALSE,"remove owned remembered folder");
}

void MetadataTrace(const std::wstring& folder,unsigned variant) {
    Script state;script=&state;state.metadata=true;state.metadata_full=variant==6;state.metadata_cancel=variant==1 || variant==5 || variant==8;state.metadata_caller=variant>=3;state.save=true;state.folder=folder;state.file=folder+L"\\new-metadata.txt";
    auto provider=std::make_shared<FileDialogInprocessProvider>();provider->module_lifetime=std::make_shared<int>(1);provider->allow_unverified_proof=true;
    auto& api=provider->api;api.size=sizeof(api);api.version=2;api.Initialize=Initialize;api.ValidateConfig=Validate;api.Show=Show;api.Query=Query;api.GetWindow=Window;api.Close=Close;api.Shutdown=Shutdown;api.UpdateControl=UpdateControl;
    api.implemented_features=variant==2?0u:PULSE_PICKER_UI_V2_FEATURE_PROPERTIES | (variant==6?(PULSE_PICKER_UI_V2_FEATURE_CUSTOM_RADIO|PULSE_PICKER_UI_V2_FEATURE_CUSTOM_CHECK|PULSE_PICKER_UI_V2_FEATURE_CUSTOM_EDIT|PULSE_PICKER_UI_V2_FEATURE_FILESYSTEM_PLACES|PULSE_PICKER_UI_V2_FEATURE_COMPUTER_PLACE|PULSE_PICKER_UI_V2_FEATURE_CUSTOM_LABELS):0u) | (variant>=3?PULSE_PICKER_UI_V2_FEATURE_SAVE_SKIP_ACCESS:0u);
    unsigned native_calls=0;FileDialogBridgeServices services;services.save_access_policy=AccessPolicy::LocalSaveV1;services.inprocess_provider=provider;
    services.validation_verified=[](const ContractDecision& d){return RuntimeLocalSaveContractCertified(d);};services.show_native=[&](IFileDialog*,HWND){++native_calls;return E_UNEXPECTED;};
    services.on_native_reason=[](const char* reason,const IID*){std::printf("METADATA_DIAGNOSTIC %s\n",reason);};
    ComPtr<IFileSaveDialog> save;Check(CreatePulseFileDialog(DialogFactory::Save,services,IID_PPV_ARGS(&save))==S_OK,"create metadata Save wrapper");if(!save)return;
    ComPtr<IPropertyStore> store;Check(PSCreateMemoryPropertyStore(IID_PPV_ARGS(&store))==S_OK,"create owned memory property store");if(!store)return;
    PROPVARIANT title{},keywords{};PropVariantInit(&title);PropVariantInit(&keywords);const wchar_t* initial_keywords[]={L"initial tag"};
    Check(InitPropVariantFromString(L"Initial title",&title)==S_OK && InitPropVariantFromStringVector(initial_keywords,1,&keywords)==S_OK,"create scalar and vector metadata fixtures");
    if (variant == 4 || variant == 5) {
        PropVariantClear(&title); title.vt = VT_NULL;
        PropVariantClear(&keywords); InitPropVariantFromString(L"first; second", &keywords);
    }
    Check(store->SetValue(PKEY_Title,title)==S_OK && store->SetValue(PKEY_Keywords,keywords)==S_OK && store->Commit()==S_OK,"populate owned metadata store");PropVariantClear(&title);PropVariantClear(&keywords);
    if (variant >= 7) {
        PROPVARIANT extra{};
        bool seeded = true;
        for (DWORD i = 0; i < 256; ++i) {
            PROPERTYKEY key = PKEY_Rating; key.pid = 10000 + i;
            extra.vt = VT_UI4; extra.ulVal = i;
            seeded = store->SetValue(key, extra) == S_OK && seeded;
        }
        Check(seeded, "seed 256 additional document properties");
        BYTE thumbnail[] = {0, 1, 127, 255};
        PROPERTYKEY key = PKEY_Rating; key.pid = 11000;
        extra = {}; extra.vt = VT_BLOB; extra.blob.cbSize = sizeof(thumbnail); extra.blob.pBlobData = thumbnail;
        Check(store->SetValue(key, extra) == S_OK, "seed opaque document thumbnail");
    }
    ComPtr<IPropertySystem> system;CoCreateInstance(__uuidof(PropertySystem),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&system));ComPtr<IPropertyDescriptionList> list;
    Check(system && system->GetPropertyDescriptionListFromString(variant==6?L"prop:System.Author;System.Keywords;System.Title;System.Subject;System.Document.Manager;System.Company;System.Category;System.Comment":L"prop:System.Title;System.Keywords",IID_PPV_ARGS(&list))==S_OK,"resolve requested metadata descriptions");if(!list)return;
    Check(save->SetProperties(store.Get())==S_OK && save->SetCollectedProperties(list.Get(),FALSE)==S_OK,"native store and collected properties accepted");
    ComPtr<IShellItem> initial;SHCreateItemFromParsingName(folder.c_str(),nullptr,IID_PPV_ARGS(&initial));
    Check(save->SetFolder(initial.Get())==S_OK && save->SetFileName(L"new-metadata.txt")==S_OK && save->SetDefaultExtension(L"txt")==S_OK && save->SetOptions(static_cast<FILEOPENDIALOGOPTIONS>(variant>=3?0x20000102:0x0200884A))==S_OK,"configure explicit metadata Save contract");
    if (variant == 6) {
        std::vector<std::wstring> labels, patterns; labels.reserve(17); patterns.reserve(17);
        for(unsigned i=0;i<17;++i) { labels.push_back(L"Format " + std::to_wstring(i)); patterns.push_back(i==11?L"*.txt":L"*.f"+std::to_wstring(i)); }
        patterns[8]=L"*.htm;*.html"; patterns[9]=L"*.mht;*.mhtml"; patterns[10]=L"*.doc;*.dot";
        std::vector<COMDLG_FILTERSPEC> filters; for(unsigned i=0;i<17;++i)filters.push_back({labels[i].c_str(),patterns[i].c_str()});
        Check(save->SetFileTypes(static_cast<UINT>(filters.size()),filters.data())==S_OK && save->SetFileTypeIndex(12)==S_OK,"full Save filter list and selected index");
        Check(save->SetTitle(L"Save As") == S_OK && save->SetOkButtonLabel(L"Save") == S_OK && save->SetFileNameLabel(L"File name") == S_OK,"full Save labels");
        ComPtr<IShellItem> computer; SHGetKnownFolderItem(FOLDERID_ComputerFolder,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(&computer));
        Check(save->AddPlace(initial.Get(),FDAP_BOTTOM)==S_OK && save->AddPlace(initial.Get(),FDAP_BOTTOM)==S_OK && save->AddPlace(computer.Get(),FDAP_BOTTOM)==S_OK,"full Save filesystem and Computer places");
        ComPtr<IFileDialogCustomize> custom;save.As(&custom);
        Check(custom->AddRadioButtonList(206)==S_OK && custom->AddControlItem(206,207,L"Choice one")==S_OK && custom->AddControlItem(206,208,L"Choice two")==S_OK && custom->SetSelectedControlItem(206,207)==S_OK,"full Save radio selection");
        Check(custom->AddCheckButton(210,L"Option one",FALSE)==S_OK && custom->AddCheckButton(211,L"Option two",FALSE)==S_OK && custom->AddEditBox(213,L"")==S_OK,"full Save empty edit and checks");
        for(DWORD id:{206u,210u,211u,213u}) Check(custom->SetControlState(id,static_cast<CDCONTROLSTATEF>(0))==S_OK,"full Save hidden control");
    }
    SinkState observed;observed.expected=state.file;ComPtr<IFileDialogEvents> sink;sink.Attach(new Sink(observed));save->Advise(sink.Get(),&observed.cookie);
    const auto shown=save->Show(nullptr);ComPtr<IPropertyStore> result;
    if(variant==2)Check(shown==E_UNEXPECTED && native_calls==1 && !state.initialized,"missing metadata feature falls back before UI initialization");
    else if(variant==1 || variant==5 || variant==8)Check(shown==HRESULT_FROM_WIN32(ERROR_CANCELLED) && !native_calls && FAILED(save->GetProperties(&result)) && !result,"cancel never returns edited metadata as accepted result");
    else {Check(shown==S_OK && !native_calls && save->GetProperties(&result)==S_OK,"accepted Save exposes metadata snapshot");CheckMetadata(result.Get());}
    if (variant == 7 && result) {
        PROPERTYKEY key = PKEY_Rating; key.pid = 11000;
        PROPVARIANT retained{}; result->GetValue(key, &retained);
        const BYTE expected[] = {0, 1, 127, 255};
        Check(retained.vt == VT_BLOB && retained.blob.cbSize == sizeof(expected) &&
            memcmp(retained.blob.pBlobData, expected, sizeof(expected)) == 0,
            "accepted Save preserves opaque document metadata exactly");
        PropVariantClear(&retained);
    }
    Check(GetFileAttributesW(state.file.c_str())==INVALID_FILE_ATTRIBUTES,"metadata fixture does not write caller target");
    save->Unadvise(observed.cookie);
}

void ValueControlsTrace(const std::wstring& folder,bool missing_feature) {
    Script state;script=&state;state.value_controls=true;state.folder=folder;state.file=folder+L"\\owned.txt";
    auto provider=std::make_shared<FileDialogInprocessProvider>();provider->module_lifetime=std::make_shared<int>(1);provider->allow_unverified_proof=true;
    auto& api=provider->api;api.size=sizeof(api);api.version=2;api.Initialize=Initialize;api.ValidateConfig=Validate;api.Show=Show;api.Query=Query;api.GetWindow=Window;api.Close=Close;api.Shutdown=Shutdown;api.UpdateControl=UpdateControl;
    api.implemented_features=PULSE_PICKER_UI_V2_FEATURE_CUSTOM_COMBOBOX|PULSE_PICKER_UI_V2_FEATURE_CUSTOM_RADIO|PULSE_PICKER_UI_V2_FEATURE_CUSTOM_CHECK|(missing_feature?0u:PULSE_PICKER_UI_V2_FEATURE_CUSTOM_EDIT);
    unsigned native_calls=0;FileDialogBridgeServices services;services.access_policy=AccessPolicy::LocalOpenV1;services.inprocess_provider=provider;
    services.validation_verified=[](const ContractDecision& d){return d.validation_category==ValidationCategory::LocalOpenAccessV1;};services.show_native=[&](IFileDialog*,HWND){++native_calls;return E_UNEXPECTED;};
    ComPtr<IFileOpenDialog> open;Check(CreatePulseFileDialog(DialogFactory::Open,services,IID_PPV_ARGS(&open))==S_OK,"create value controls wrapper");if(!open)return;
    ComPtr<IShellItem> initial;SHCreateItemFromParsingName(folder.c_str(),nullptr,IID_PPV_ARGS(&initial));
    Check(open->SetFolder(initial.Get())==S_OK && open->SetOptions(static_cast<FILEOPENDIALOGOPTIONS>(FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_FILEMUSTEXIST|FOS_NOCHANGEDIR))==S_OK,"configure value controls fixture");
    ComPtr<IFileDialogCustomize> custom;open.As(&custom);
    Check(custom->AddRadioButtonList(206)==S_OK && custom->AddControlItem(206,207,L"First")==S_OK && custom->AddControlItem(206,208,L"Disabled")==S_OK,"native radio list captured");
    Check(custom->SetControlItemState(206,208,CDCS_VISIBLE)==S_OK,"disable radio option");
    Check(custom->AddCheckButton(210,L"Option",FALSE)==S_OK && custom->AddEditBox(213,L"initial")==S_OK,"native checkbox and edit captured");
    for(DWORD id:{206u,210u,213u})Check(custom->SetControlState(id,static_cast<CDCONTROLSTATEF>(0))==S_OK,"initially hide value control");
    Check(custom->StartVisualGroup(300,L"Group")==S_OK && custom->AddPushButton(301,L"Inside")==S_OK && custom->EndVisualGroup()==S_OK,"create and close custom group");
    Check(custom->AddPushButton(302,L"Outside")==S_OK && custom->AddComboBox(303)==S_OK && custom->AddControlItem(303,0,L"Choice")==S_OK,"independent button and combo after EndVisualGroup retain no parent");
    SinkState observed;observed.expected=state.file;ComPtr<IFileDialogEvents> sink;sink.Attach(new Sink(observed));open->Advise(sink.Get(),&observed.cookie);
    const auto shown=open->Show(nullptr);
    if(missing_feature)Check(shown==E_UNEXPECTED && native_calls==1 && !state.initialized,"unsupported edit capability falls back before UI");
    else Check(shown==S_OK && !native_calls && state.item_calls==1 && state.check_events==1,"value controls survive hidden/live/input/callback lifecycle");
    open->Unadvise(observed.cookie);
}

void ComputerPlaceTrace(const std::wstring& folder,bool missing_feature) {
    Script state;script=&state;state.computer_place=true;state.folder=folder;state.file=folder+L"\\owned.txt";
    Check(SHGetKnownFolderItem(FOLDERID_ComputerFolder,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(&state.computer))==S_OK,"obtain actual Computer known folder");
    if(!state.computer)return;
    auto provider=std::make_shared<FileDialogInprocessProvider>();provider->module_lifetime=std::make_shared<int>(1);provider->allow_unverified_proof=true;
    auto& api=provider->api;api.size=sizeof(api);api.version=2;api.Initialize=Initialize;api.ValidateConfig=Validate;api.Show=Show;api.Query=Query;api.GetWindow=Window;api.Close=Close;api.Shutdown=Shutdown;
    api.implemented_features=PULSE_PICKER_UI_V2_FEATURE_FILESYSTEM_PLACES | (missing_feature ? 0u : PULSE_PICKER_UI_V2_FEATURE_COMPUTER_PLACE);
    unsigned native_calls=0;FileDialogBridgeServices services;services.access_policy=AccessPolicy::LocalOpenV1;services.inprocess_provider=provider;
    services.validation_verified=[](const ContractDecision& d){return d.validation_category==ValidationCategory::LocalOpenAccessV1;};
    services.show_native=[&](IFileDialog*,HWND){++native_calls;return E_UNEXPECTED;};
    services.on_native_reason=[](const char* reason,const IID*){std::printf("COMPUTER_DIAGNOSTIC %s\n",reason);};
    ComPtr<IFileOpenDialog> open;Check(CreatePulseFileDialog(DialogFactory::Open,services,IID_PPV_ARGS(&open))==S_OK,"create Computer place wrapper");if(!open)return;
    ComPtr<IShellItem> initial;SHCreateItemFromParsingName(folder.c_str(),nullptr,IID_PPV_ARGS(&initial));
    Check(open->SetFolder(initial.Get())==S_OK && open->SetOptions(static_cast<FILEOPENDIALOGOPTIONS>(FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_FILEMUSTEXIST|FOS_NOCHANGEDIR))==S_OK,"configure local file contract with virtual navigation place");
    Check(open->AddPlace(state.computer.Get(),FDAP_TOP)==S_OK,"real native Computer AddPlace accepted");
    SinkState observed;observed.expected=state.file;ComPtr<IFileDialogEvents> sink;sink.Attach(new Sink(observed));open->Advise(sink.Get(),&observed.cookie);
    const auto shown=open->Show(nullptr);
    if(missing_feature)Check(shown==E_UNEXPECTED && native_calls==1 && state.initialized==0 && state.shown==0,"missing Computer feature falls back before UI initialization");
    else {
        Check(shown==S_OK && native_calls==0 && state.computer_changing==1 && state.computer_changed==1,"Computer navigation returns to local folder and accepts file");
        ComPtr<IShellItem> result;PWSTR path=nullptr;
        const auto got=open->GetResult(&result);if(result)result->GetDisplayName(SIGDN_FILESYSPATH,&path);
        Check(got==S_OK && path && state.file==path,"accepted result is exact ordinary file, never virtual root");CoTaskMemFree(path);
    }
    open->Unadvise(observed.cookie);
}

void MenusTrace(const std::wstring& folder) {
    Script state;script=&state;state.menus=true;state.folder=folder;state.file=folder+L"\\owned.txt";
    auto provider=std::make_shared<FileDialogInprocessProvider>();provider->module_lifetime=std::make_shared<int>(1);provider->allow_unverified_proof=true;
    auto& api=provider->api;api.size=sizeof(api);api.version=2;api.Initialize=Initialize;api.ValidateConfig=Validate;api.Show=Show;api.Query=Query;api.GetWindow=Window;api.Close=Close;api.Shutdown=Shutdown;api.UpdateControl=UpdateControl;
    api.implemented_features=PULSE_PICKER_UI_V2_FEATURE_CUSTOM_MENU|PULSE_PICKER_UI_V2_FEATURE_OPEN_DROPDOWN|PULSE_PICKER_UI_V2_FEATURE_CUSTOM_PROMINENT_CONTROL;
    unsigned native_calls=0;FileDialogBridgeServices services;services.access_policy=AccessPolicy::LocalOpenV1;services.inprocess_provider=provider;
    services.validation_verified=[](const ContractDecision& d){return d.validation_category==ValidationCategory::LocalOpenAccessV1;};
    services.show_native=[&](IFileDialog*,HWND){++native_calls;return E_UNEXPECTED;};
    services.on_native_reason=[](const char* reason,const IID*) { std::printf("MENU_DIAGNOSTIC %s\n",reason); };
    ComPtr<IFileOpenDialog> open;Check(CreateIsolatedDialog(DialogFactory::Open,services,IID_PPV_ARGS(&open))==S_OK,"create menu/dropdown wrapper from isolated system factory");if(!open)return;
    ComPtr<IShellItem> initial;SHCreateItemFromParsingName(folder.c_str(),nullptr,IID_PPV_ARGS(&initial));
    Check(open->SetFolder(initial.Get())==S_OK && open->SetOptions(static_cast<FILEOPENDIALOGOPTIONS>(FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_FILEMUSTEXIST|FOS_NOCHANGEDIR))==S_OK,"configure ordinary Open fixture");
    ComPtr<IFileDialogCustomize> custom;open.As(&custom);
    Check(custom->EnableOpenDropDown(50)==S_OK,"enable Open dropdown");
    for(DWORD id:{0u,2u,201u,202u}) Check(custom->AddControlItem(50,id,L"Choice")==S_OK,"add dropdown item");
    Check(custom->SetControlItemState(50,201,CDCS_VISIBLE)==S_OK && custom->SetControlItemState(50,202,CDCS_ENABLED)==S_OK,"configure disabled and hidden items");
    Check(custom->AddMenu(1,L"Tools")==S_OK && custom->AddControlItem(1,17,L"Action")==S_OK && custom->MakeProminent(1)==S_OK,"add prominent Tools menu");
    SinkState observed;observed.expected=state.file;ComPtr<IFileDialogEvents> sink;sink.Attach(new Sink(observed));open->Advise(sink.Get(),&observed.cookie);
    Check(open->Show(nullptr)==S_OK && native_calls==0 && state.activating==2 && state.item_calls==1,"menu/dropdown trace accepts without fallback");
    DWORD selected=99;Check(custom->GetSelectedControlItem(50,&selected)==S_OK && selected==2,"dropdown choice survives Show");
    open->Unadvise(observed.cookie);
}

void ReadonlyProperties(const std::wstring& folder, unsigned variant) {
    Script state; script = &state; state.save = true; state.readonly_properties = true; state.property_close = variant == 1;
    state.folder = folder; state.file = folder + L"\\new-properties.txt";
    auto provider = std::make_shared<FileDialogInprocessProvider>(); provider->module_lifetime = std::make_shared<int>(1); provider->allow_unverified_proof = true;
    auto& api = provider->api; api.size = sizeof(api); api.version = 2; api.Initialize = Initialize; api.ValidateConfig = Validate;
    api.Show = Show; api.Query = Query; api.GetWindow = Window; api.Close = Close; api.Shutdown = Shutdown;
    api.implemented_features = variant == 2 ? 0u : PULSE_PICKER_UI_V2_FEATURE_SAVE_UNENCRYPTED;
    unsigned native_calls = 0;
    FileDialogBridgeServices services; services.save_access_policy = AccessPolicy::LocalSaveV1; services.inprocess_provider = provider;
    services.validation_verified = [](const ContractDecision& value) { return value.supported_access_policy == AccessPolicy::LocalSaveV1; };
    services.show_native = [&](IFileDialog*, HWND) { ++native_calls; return HRESULT_FROM_WIN32(ERROR_CANCELLED); };
    ComPtr<IFileSaveDialog> save; Check(CreatePulseFileDialog(DialogFactory::Save, services, IID_PPV_ARGS(&save)) == S_OK, "create readonly-collection Save wrapper");
    if (!save) return;
    ComPtr<IPropertySystem> system; CoCreateInstance(__uuidof(PropertySystem), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&system));
    ComPtr<IPropertyDescriptionList> list;
    const HRESULT parsed = system ? system->GetPropertyDescriptionListFromString(variant == 4 ? L"prop:System.Title" : L"prop:System.Security.EncryptionOwners", IID_PPV_ARGS(&list)) : E_UNEXPECTED;
    Check(parsed == S_OK && list, "obtain actual Windows collection descriptor"); if (!list) return;
    if (variant == 5) {
        ComPtr<IFileSaveDialog> baseline;
        const HRESULT made = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&baseline));
        const HRESULT expected = made == S_OK ? baseline->SetProperties(nullptr) : made;
        Check(made == S_OK && save->SetProperties(nullptr) == expected, "prior metadata setter preserves actual native HRESULT while retaining fallback");
    }
    Check(save->SetCollectedProperties(list.Get(), variant == 3 ? TRUE : FALSE) == S_OK, "native collection setter result preserved");
    ComPtr<IShellItem> initial; SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&initial));
    Check(save->SetFolder(initial.Get()) == S_OK && save->SetFileName(L"new-properties.txt") == S_OK && save->SetDefaultExtension(L"txt") == S_OK &&
        save->SetOptions(static_cast<FILEOPENDIALOGOPTIONS>(0x0200884A)) == S_OK, "configure exact ordinary Save contract");
    SinkState observed; observed.expected = state.file; ComPtr<IFileDialogEvents> sink; sink.Attach(new Sink(observed));
    Check(save->Advise(sink.Get(), &observed.cookie) == S_OK, "advise actual property FileOk sink");
    const HRESULT shown = save->Show(nullptr);
    if (variant >= 2) Check(shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) && native_calls == 1 && !state.shown,
        "missing guard capability, append defaults, editable metadata, or prior metadata stays native");
    else {
        Check(native_calls == 0 && state.shown == 1 && observed.calls == 1 && shown == (variant == 1 ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK),
            "guarded readonly collection reaches UI once and retains accept/Close result");
        ComPtr<IPropertyStore> store; DWORD count = MAXDWORD;
        const HRESULT got = save->GetProperties(&store);
        Check(variant == 1 ? (FAILED(got) && !store) : (got == S_OK && store && store->GetCount(&count) == S_OK && count == 0),
            "post-Show accepted result provides empty store; Close does not pretend success");
        Check(GetFileAttributesW(state.file.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND,
            "property collection never creates target or applies ownership");
    }
    save->Unadvise(observed.cookie);
}

}
void PreUiFallback(const std::wstring& folder) {
    static unsigned variant = 0;
    for (variant=0;variant<6;++variant) {
        Script state;script=&state;state.folder=folder;state.file=folder+L"\\owned.txt";
        auto provider=std::make_shared<FileDialogInprocessProvider>();provider->module_lifetime=std::make_shared<int>(1);provider->allow_unverified_proof=true;
        auto& api=provider->api;api.size=sizeof(api);api.version=2;api.Initialize=Initialize;api.ValidateConfig=Validate;api.Query=Query;api.GetWindow=Window;api.Close=Close;api.Shutdown=Shutdown;
        api.Show=[](PulsePickerUiV2Session,const PulsePickerUiV2Config*,const PulsePickerUiV2Callbacks* callbacks,PulsePickerUiV2Result* result)->HRESULT {
            result->status=variant==3?PULSE_PICKER_UI_V2_CANCELLED:PULSE_PICKER_UI_V2_FAILED;
            result->terminal_hr=variant==3?HRESULT_FROM_WIN32(ERROR_CANCELLED):E_ACCESSDENIED;
            if(variant==1)result->flags=PULSE_PICKER_UI_V2_UI_SHOWN;
            if(variant==2)result->show_id=1;
            if(variant==4){PulsePickerUiV2Event event{};event.size=sizeof(event);callbacks->callback(callbacks->context,&event);}
            if(variant==5)result->terminal_hr=E_FAIL;
            return variant==5?E_UNEXPECTED:result->terminal_hr;
        };
        unsigned native_calls=0;FileDialogBridgeServices services;services.access_policy=AccessPolicy::LocalOpenV1;services.inprocess_provider=provider;
        services.validation_verified=[](const ContractDecision& d){return d.validation_category==ValidationCategory::LocalOpenAccessV1;};
        services.show_native=[&](IFileDialog*,HWND){++native_calls;return HRESULT_FROM_WIN32(ERROR_CANCELLED);};
        ComPtr<IFileOpenDialog> open;
        Check(CreateIsolatedDialog(DialogFactory::Open,services,IID_PPV_ARGS(&open))==S_OK,"isolated system factory for startup fallback");if(!open)continue;
        ComPtr<IShellItem> initial;SHCreateItemFromParsingName(folder.c_str(),nullptr,IID_PPV_ARGS(&initial));
        Check(open->SetFolder(initial.Get())==S_OK && open->SetOptions(FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_FILEMUSTEXIST|FOS_NOCHANGEDIR)==S_OK,"configure startup fallback contract");
        const auto hr=open->Show(nullptr);
        Check(native_calls==(variant==0?1u:0u),"only validated callback-free pre-UI failure falls back once");
        Check(variant==0||variant==3?hr==HRESULT_FROM_WIN32(ERROR_CANCELLED):FAILED(hr),"failure and cancellation preserve terminal outcome");
        ComPtr<IShellItem> result;Check(FAILED(open->GetResult(&result))&&!result,"failed or cancelled show exposes no result");
    }
}
void NullExtension(const std::wstring& folder) {
    static bool captured = false;
    for (unsigned variant = 0; variant < 4; ++variant) {
        captured = false; unsigned native_calls = 0; bool invalid_snapshot = false;
        Script state; script = &state; state.folder = folder; state.file = folder + L"\\owned.txt";
        auto provider = std::make_shared<FileDialogInprocessProvider>();
        provider->module_lifetime = std::make_shared<int>(1); provider->allow_unverified_proof = true;
        auto& api = provider->api; api.size = sizeof(api); api.version = 2; api.Initialize = Initialize; api.Shutdown = Shutdown;
        api.ValidateConfig = [](PulsePickerUiV2Session, const PulsePickerUiV2Config* config)->HRESULT {
            captured = config && config->default_extension.units == 0;
            return captured ? S_OK : E_INVALIDARG;
        };
        api.Show = [](PulsePickerUiV2Session, const PulsePickerUiV2Config*, const PulsePickerUiV2Callbacks*, PulsePickerUiV2Result* result)->HRESULT {
            result->status = PULSE_PICKER_UI_V2_CANCELLED; result->flags = PULSE_PICKER_UI_V2_UI_SHOWN;
            result->terminal_hr = HRESULT_FROM_WIN32(ERROR_CANCELLED); return result->terminal_hr;
        };
        api.Query = Query; api.GetWindow = Window; api.Close = Close;
        api.implemented_features = PULSE_PICKER_UI_V2_FEATURE_OPEN_DEFAULT_EXTENSION;
        FileDialogBridgeServices services; services.access_policy = AccessPolicy::LocalOpenV1; services.inprocess_provider = provider;
        services.validation_verified = [](const ContractDecision& d) { return d.validation_category == ValidationCategory::LocalOpenAccessV1; };
        services.show_native = [&](IFileDialog*, HWND) { ++native_calls; return HRESULT_FROM_WIN32(ERROR_CANCELLED); };
        services.on_native_reason = [&](const char* reason, const IID*) { std::printf("NULL_EXTENSION_DIAGNOSTIC %s\n", reason ? reason : ""); if (reason && std::string_view(reason) == "text-snapshot-invalid") invalid_snapshot = true; };
        ComPtr<IFileOpenDialog> dialog;
        Check(CreateIsolatedDialog(DialogFactory::Open, services, IID_PPV_ARGS(&dialog)) == S_OK, "isolated native extension setter mirror");
        if (!dialog) continue;
        if (variant) Check(dialog->SetDefaultExtension(L"txt") == S_OK, "prior default extension succeeds");
        const std::wstring long_extension(32769, L'x');
        const HRESULT setter = dialog->SetDefaultExtension(variant == 2 ? L"" : variant == 3 ? long_extension.c_str() : nullptr);
        Check(setter == S_OK, "native null empty and long extension setter result preserved");
        ComPtr<IShellItem> initial; SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&initial));
        dialog->SetFolder(initial.Get()); dialog->SetFileName(L"owned.txt");
        dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
        Check(dialog->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED), "extension case cancellation preserved");
        Check(variant == 3 ? invalid_snapshot && native_calls == 1 && !captured : !invalid_snapshot && !native_calls && captured,
            "null or empty clear is bridged; oversized value retains bounded native fallback");
    }
}
void SeparatorControls(const std::wstring& folder) {
    for (unsigned variant = 0; variant < 3; ++variant) {
        Script state; script = &state; state.folder = folder; state.file = folder + L"\\export.psd";
        state.separators = true; state.cancel_before_accept = variant == 1;
        auto provider = std::make_shared<FileDialogInprocessProvider>();
        provider->module_lifetime = std::make_shared<int>(1); provider->allow_unverified_proof = true;
        auto& api = provider->api; api.size = sizeof(api); api.version = 2;
        api.Initialize = Initialize; api.ValidateConfig = Validate; api.Show = Show; api.Query = Query;
        api.GetWindow = Window; api.Close = Close; api.Shutdown = Shutdown; api.UpdateControl = UpdateControl;
        api.implemented_features = PULSE_PICKER_UI_V2_FEATURE_SAVE_NATIVE_DEFAULTS |
            PULSE_PICKER_UI_V2_FEATURE_CUSTOM_LABELS | PULSE_PICKER_UI_V2_FEATURE_CUSTOM_PROMINENT_CONTROL |
            (variant == 2 ? 0u : PULSE_PICKER_UI_V2_FEATURE_CUSTOM_SEPARATOR);
        unsigned native_calls = 0;
        FileDialogBridgeServices services; services.save_access_policy = AccessPolicy::LocalSaveV1;
        services.inprocess_provider = provider;
        services.validation_verified = [](const ContractDecision& d) { return d.validation_category == ValidationCategory::LocalSaveAccessV1; };
        services.show_native = [&](IFileDialog*, HWND) { ++native_calls; return E_FAIL; };
        ComPtr<IFileSaveDialog> save;
        Check(CreateIsolatedDialog(DialogFactory::Save, services, IID_PPV_ARGS(&save)) == S_OK, "isolated separator Save");
        if (!save) continue;
        ComPtr<IShellItem> initial; SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&initial));
        const COMDLG_FILTERSPEC filter{L"PSD", L"*.PSD;*.PSB"};
        Check(save->SetOptions(34826) == S_OK && save->SetFileTypes(1, &filter) == S_OK &&
            save->SetFileTypeIndex(1) == S_OK && save->SetDefaultExtension(L"PSD") == S_OK &&
            save->SetFileName(L"export.psd") == S_OK && save->SetTitle(L"Save") == S_OK &&
            save->SetFolder(initial.Get()) == S_OK, "logged Save options and setters accepted");
        ComPtr<IFileDialogCustomize> custom; save.As(&custom);
        Check(custom && custom->AddSeparator(2028) == S_OK && custom->AddSeparator(2029) == S_OK &&
            custom->AddPushButton(2024, L"Options") == S_OK && custom->MakeProminent(2024) == S_OK,
            "logged separator calls and prominent control accepted");
        CDCONTROLSTATEF control_state{};
        Check(custom && custom->SetControlState(2029, CDCS_VISIBLE) == S_OK &&
            custom->GetControlState(2029, &control_state) == S_OK && control_state == CDCS_VISIBLE,
            "separator own state round-trips before Show");
        show_action = [&] {
            Check(custom->SetControlState(2029, static_cast<CDCONTROLSTATEF>(3)) == S_OK &&
                custom->GetControlState(2029, &control_state) == S_OK && control_state == 3,
                "separator live state update reaches UI and native mirror");
            state.fail_update = true;
            Check(custom->SetControlState(2029, CDCS_VISIBLE) == E_ACCESSDENIED &&
                custom->GetControlState(2029, &control_state) == S_OK && control_state == 3,
                "failed separator live update restores both backends");
            Check(custom->AddSeparator(2030) == E_UNEXPECTED, "live separator insertion leaves structure unchanged");
        };
        const HRESULT shown = save->Show(nullptr);
        show_action = {};
        Check(variant == 2 ? shown == E_FAIL && native_calls == 1 && !state.shown :
            shown == (variant == 1 ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK) && !native_calls && state.shown == 1,
            variant == 2 ? "older provider falls back before showing separator UI" : "separator Save stays in Pulse on accept and cancel");
        ComPtr<IShellItem> result; const HRESULT got = save->GetResult(&result);
        if (variant == 0) {
            PWSTR path = nullptr;
            Check(got == S_OK && result && result->GetDisplayName(SIGDN_FILESYSPATH, &path) == S_OK &&
                path && state.file == path, "separator Save returns exact Shell path");
            CoTaskMemFree(path);
        } else Check(FAILED(got) && !result, "cancelled or native-fallback Save exposes no Pulse result");
        Check(!std::filesystem::exists(state.file), "separator Save contract creates no user file");
    }
}
void SaveNoTestCreate(const std::wstring& folder) {
    for(bool checkbox : {false,true})for(bool cancel : {false,true}){
        Script state;script=&state;state.folder=folder;state.file=folder+L"\\absent\\export.pdf";
        state.no_test_save=true;state.prominent_group=!checkbox;state.prominent_kind=checkbox?7u:0u;state.missing_veto=cancel;
        auto provider=std::make_shared<FileDialogInprocessProvider>();provider->module_lifetime=std::make_shared<int>(1);provider->allow_unverified_proof=true;
        auto& api=provider->api;api.size=sizeof(api);api.version=2;api.Initialize=Initialize;api.ValidateConfig=Validate;api.Show=Show;api.Query=Query;api.GetWindow=Window;api.Close=Close;api.Shutdown=Shutdown;api.UpdateControl=UpdateControl;
        api.implemented_features=PULSE_PICKER_UI_V2_FEATURE_CUSTOM_PROMINENT_GROUP|PULSE_PICKER_UI_V2_FEATURE_CUSTOM_PROMINENT_CONTROL|PULSE_PICKER_UI_V2_FEATURE_CUSTOM_CHECK|PULSE_PICKER_UI_V2_FEATURE_SAVE_NO_TEST_CREATE|PULSE_PICKER_UI_V2_FEATURE_SAVE_NATIVE_DEFAULTS;
        unsigned native_calls=0;FileDialogBridgeServices services;services.save_access_policy=AccessPolicy::LocalSaveV1;services.inprocess_provider=provider;
        services.validation_verified=[](const ContractDecision& d){return d.validation_category==ValidationCategory::LocalSaveAccessV1;};
        services.show_native=[&](IFileDialog*,HWND){++native_calls;return E_FAIL;};
        ComPtr<IFileSaveDialog> save;Check(CreateIsolatedDialog(DialogFactory::Save,services,IID_PPV_ARGS(&save))==S_OK,"isolated no-test-create Save");if(!save)continue;
        ComPtr<IShellItem> initial;SHCreateItemFromParsingName(folder.c_str(),nullptr,IID_PPV_ARGS(&initial));
        COMDLG_FILTERSPEC filter{L"PDF",L"*.pdf"};
        Check(save->SetDefaultExtension(L"pdf")==S_OK&&save->SetFileTypes(1,&filter)==S_OK&&save->SetFileName(L"export.pdf")==S_OK&&
            save->SetOptions(0x1000au)==S_OK&&save->SetFolder(initial.Get())==S_OK,"exact no-test-create Save configuration");
        ComPtr<IFileDialogCustomize> custom;save.As(&custom);
        if(checkbox)Check(custom->AddCheckButton(1047,L"Option",FALSE)==S_OK&&custom->MakeProminent(1047)==S_OK,"Save prominent standalone checkbox captured");
        else Check(custom->StartVisualGroup(1047,L"Group")==S_OK&&custom->AddPushButton(1048,L"Action")==S_OK&&custom->EndVisualGroup()==S_OK&&custom->MakeProminent(1047)==S_OK,"Save prominent group captured");
        SinkState observed;ComPtr<IFileDialogEvents> sink;sink.Attach(new Sink(observed));save->Advise(sink.Get(),&observed.cookie);
        Check(save->Show(nullptr)==(cancel?HRESULT_FROM_WIN32(ERROR_CANCELLED):S_OK)&&!native_calls&&state.shown==1&&observed.calls==(cancel?2u:1u),"Save missing-parent accept or veto cancellation remains bridged");
        ComPtr<IShellItem> result;PWSTR path=nullptr;const HRESULT got=save->GetResult(&result);
        if(cancel)Check(FAILED(got)&&!result,"cancelled Save exposes no stale result");
        else Check(got==S_OK&&result&&result->GetDisplayName(SIGDN_FILESYSPATH,&path)==S_OK&&path&&state.file==path,"accepted Save missing-parent result exact");
        CoTaskMemFree(path);save->Unadvise(observed.cookie);
        Check(!std::filesystem::exists(folder+L"\\absent"),"Save result materialization creates no parent or file");
    }
}
void ProminentControls(const std::wstring& folder) {
    for(uint32_t kind : {2u,3u,4u,6u,7u,8u})for(bool nested : {false,true}){
        Script state;script=&state;state.folder=folder;state.file=folder+L"\\owned.txt";state.prominent_kind=kind;state.prominent_nested=nested;state.cancel_before_accept=true;
        auto provider=std::make_shared<FileDialogInprocessProvider>();provider->module_lifetime=std::make_shared<int>(1);provider->allow_unverified_proof=true;
        auto& api=provider->api;api.size=sizeof(api);api.version=2;api.Initialize=Initialize;api.ValidateConfig=Validate;api.Show=Show;api.Query=Query;api.GetWindow=Window;api.Close=Close;api.Shutdown=Shutdown;api.UpdateControl=UpdateControl;
        api.implemented_features=PULSE_PICKER_UI_V2_FEATURE_CUSTOM_PROMINENT_CONTROL|PULSE_PICKER_UI_V2_FEATURE_CUSTOM_MENU|PULSE_PICKER_UI_V2_FEATURE_CUSTOM_COMBOBOX|PULSE_PICKER_UI_V2_FEATURE_CUSTOM_RADIO|PULSE_PICKER_UI_V2_FEATURE_CUSTOM_CHECK|PULSE_PICKER_UI_V2_FEATURE_CUSTOM_EDIT;
        unsigned native_calls=0;FileDialogBridgeServices services;services.access_policy=AccessPolicy::LocalOpenV1;services.inprocess_provider=provider;
        services.validation_verified=[](const ContractDecision& d){return d.validation_category==ValidationCategory::LocalOpenAccessV1;};services.show_native=[&](IFileDialog*,HWND){++native_calls;return E_FAIL;};
        ComPtr<IFileOpenDialog> open;Check(CreateIsolatedDialog(DialogFactory::Open,services,IID_PPV_ARGS(&open))==S_OK,"generic prominence system factory");if(!open)continue;
        ComPtr<IShellItem> initial;SHCreateItemFromParsingName(folder.c_str(),nullptr,IID_PPV_ARGS(&initial));open->SetFolder(initial.Get());open->SetOptions(FOS_FORCEFILESYSTEM|FOS_FILEMUSTEXIST|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR);
        ComPtr<IFileDialogCustomize> custom;open.As(&custom);if(nested)custom->StartVisualGroup(1046,L"Parent");HRESULT added=E_FAIL;
        switch(kind){case 2:added=custom->AddPushButton(1047,L"Action");break;case 3:added=custom->AddComboBox(1047);break;case 4:added=custom->AddMenu(1047,L"Menu");break;case 6:added=custom->AddRadioButtonList(1047);break;case 7:added=custom->AddCheckButton(1047,L"Check",FALSE);break;case 8:added=custom->AddEditBox(1047,L"Edit");break;}
        Check(added==S_OK,"create native control kind");if(kind==3||kind==4||kind==6)custom->AddControlItem(1047,1,L"Option");
        if(nested){custom->AddPushButton(1049,L"Ordinary sibling");custom->EndVisualGroup();}
        Check(custom->MakeProminent(1047)==S_OK,"generic native prominence result preserved");
        const bool inactive_parent=nested&&kind!=6&&kind!=8;
        if(inactive_parent)Check(custom->SetControlState(1046,CDCS_INACTIVE)==S_OK,"hide and disable prominent control parent");
        show_action=[&]{
            PulsePickerUiV2Event event{};event.size=sizeof(event);event.show_id=1;event.generation=state.generation;event.window=state.window;event.control_id=1047;
            event.kind=kind==2?PULSE_PICKER_UI_V2_EVENT_BUTTON_CLICKED:kind==4?PULSE_PICKER_UI_V2_EVENT_CONTROL_ACTIVATING:kind==7?PULSE_PICKER_UI_V2_EVENT_CHECK_TOGGLED:kind==8?PULSE_PICKER_UI_V2_EVENT_EDIT_CHANGED:PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED;
            event.item_id=kind==3||kind==6||kind==7?1u:0u;
            Check(navigation_callbacks->callback(navigation_callbacks->context,&event)==S_OK,"prominent control accepts its live interaction event");
            if(kind==7){BOOL checked=FALSE;Check(custom->GetCheckButtonState(1047,&checked)==S_OK&&checked,"prominent checkbox commits live toggle");}
            if(kind==3||kind==6){DWORD selected=0;Check(custom->GetSelectedControlItem(1047,&selected)==S_OK&&selected==1,"prominent selection commits live item");}
            if(inactive_parent){event.kind=PULSE_PICKER_UI_V2_EVENT_BUTTON_CLICKED;event.control_id=1049;event.item_id=0;
                Check(navigation_callbacks->callback(navigation_callbacks->context,&event)==E_INVALIDARG,"ordinary sibling still rejects interaction under inactive parent");}
        };
        Check(open->Show(nullptr)==HRESULT_FROM_WIN32(ERROR_CANCELLED)&&!native_calls&&state.shown==1,"generic prominence remains bridged");
        show_action={};
    }
}
void ProminentGroup(const std::wstring& folder) {
    for(unsigned variant=0;variant<3;++variant){
        Script state;script=&state;state.folder=folder;state.file=folder+L"\\owned.txt";state.prominent_group=true;state.cancel_before_accept=true;
        auto provider=std::make_shared<FileDialogInprocessProvider>();provider->module_lifetime=std::make_shared<int>(1);provider->allow_unverified_proof=true;
        auto& api=provider->api;api.size=sizeof(api);api.version=2;api.Initialize=Initialize;api.ValidateConfig=Validate;api.Show=Show;api.Query=Query;api.GetWindow=Window;api.Close=Close;api.Shutdown=Shutdown;api.UpdateControl=UpdateControl;
        api.implemented_features=variant==1?0u:PULSE_PICKER_UI_V2_FEATURE_CUSTOM_PROMINENT_GROUP;
        unsigned native_calls=0;FileDialogBridgeServices services;services.access_policy=AccessPolicy::LocalOpenV1;services.inprocess_provider=provider;
        services.validation_verified=[](const ContractDecision& d){return d.validation_category==ValidationCategory::LocalOpenAccessV1;};
        services.show_native=[&](IFileDialog*,HWND){++native_calls;return HRESULT_FROM_WIN32(ERROR_CANCELLED);};
        ComPtr<IFileOpenDialog> open;Check(CreateIsolatedDialog(DialogFactory::Open,services,IID_PPV_ARGS(&open))==S_OK,"isolated prominence wrapper");if(!open)continue;
        ComPtr<IShellItem> initial;SHCreateItemFromParsingName(folder.c_str(),nullptr,IID_PPV_ARGS(&initial));
        open->SetFolder(initial.Get());open->SetOptions(FOS_FORCEFILESYSTEM|FOS_FILEMUSTEXIST|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR);
        ComPtr<IFileDialogCustomize> custom;open.As(&custom);
        if(variant==2)Check(custom->MakeProminent(1047)==E_INVALIDARG,"unknown prominence ID retains native failure");
        Check(custom->StartVisualGroup(1047,L"Group")==S_OK&&custom->AddPushButton(1048,L"Action")==S_OK&&custom->EndVisualGroup()==S_OK,"capture real native group and child");
        Check(custom->MakeProminent(1047)==S_OK,"existing group prominence succeeds");
        Check(custom->SetControlState(1047,CDCS_VISIBLE)==S_OK&&custom->SetControlState(1047,static_cast<CDCONTROLSTATEF>(3))==S_OK,"state changes preserve independent prominence");
        CDCONTROLSTATEF native_state{};Check(custom->GetControlState(1047,&native_state)==S_OK&&native_state==static_cast<CDCONTROLSTATEF>(3),"prominence bit never leaks into COM state");
        Check(open->Show(nullptr)==HRESULT_FROM_WIN32(ERROR_CANCELLED)&&native_calls==(variant?1u:0u)&&state.shown==(variant?0u:1u),"prominence needs provider capability and preserves prior unsupported gate");
    }
}
void OpenMissing(const std::wstring& folder) {
    for(unsigned variant=0;variant<4;++variant){
        Script state;script=&state;state.folder=folder;state.missing_open=true;state.caller_validation=true;
        state.file=folder+(variant==0?L"\\missing.txt":L"\\absent\\missing.txt");state.folder_multi=variant==2;
        if(state.folder_multi){state.second=state.file;state.file=folder+L"\\owned.txt";}
        state.missing_veto=variant==3;
        auto provider=std::make_shared<FileDialogInprocessProvider>();provider->module_lifetime=std::make_shared<int>(1);provider->allow_unverified_proof=true;
        auto& api=provider->api;api.size=sizeof(api);api.version=2;api.Initialize=Initialize;api.ValidateConfig=Validate;api.Show=Show;api.Query=Query;api.GetWindow=Window;api.Close=Close;api.Shutdown=Shutdown;
        api.implemented_features=PULSE_PICKER_UI_V2_FEATURE_OPEN_MISSING|PULSE_PICKER_UI_V2_FEATURE_OPEN_SKIP_ACCESS|PULSE_PICKER_UI_V2_FEATURE_MULTI_SELECT;
        unsigned native_calls=0;FileDialogBridgeServices services;services.access_policy=AccessPolicy::LocalOpenV1;services.inprocess_provider=provider;
        services.validation_verified=[](const ContractDecision& d){return d.validation_category==ValidationCategory::LocalOpenAccessV1;};
        services.show_native=[&](IFileDialog*,HWND){++native_calls;return E_FAIL;};
        ComPtr<IFileOpenDialog> open;Check(CreateIsolatedDialog(DialogFactory::Open,services,IID_PPV_ARGS(&open))==S_OK,"isolated missing Open wrapper");if(!open)continue;
        ComPtr<IShellItem> initial;SHCreateItemFromParsingName(folder.c_str(),nullptr,IID_PPV_ARGS(&initial));
        Check(open->SetOptions(0x348u)==S_OK&&open->SetFolder(initial.Get())==S_OK,"exact missing Open 0x348 options");
        SinkState observed;ComPtr<IFileDialogEvents> sink;sink.Attach(new Sink(observed));open->Advise(sink.Get(),&observed.cookie);
        const HRESULT shown=open->Show(nullptr);
        Check(!native_calls&&observed.calls==(state.missing_veto?2u:1u)&&shown==(state.missing_veto?HRESULT_FROM_WIN32(ERROR_CANCELLED):S_OK),"missing candidates accept or veto then cancel without native fallback");
        ComPtr<IShellItemArray> results;const HRESULT got=open->GetResults(&results);
        if(state.missing_veto)Check(FAILED(got)&&!results,"veto cancellation exposes no result array");
        else {DWORD count=0;Check(got==S_OK&&results&&results->GetCount(&count)==S_OK&&count==(state.folder_multi?2u:1u),"accepted missing result count");
            for(DWORD i=0;i<count;++i){ComPtr<IShellItem> item;PWSTR path=nullptr;const auto expected=i?state.second:state.file;
                Check(results->GetItemAt(i,&item)==S_OK&&item->GetDisplayName(SIGDN_FILESYSPATH,&path)==S_OK&&path&&expected==path,"accepted missing result order");CoTaskMemFree(path);}}
        ComPtr<IShellItem> single;const HRESULT one=open->GetResult(&single);
        Check(state.missing_veto||state.folder_multi?FAILED(one)&&!single:one==S_OK&&single,"final single result rejects cancelled or multiple choices");
        open->Unadvise(observed.cookie);
        Check(!std::filesystem::exists(folder+L"\\missing.txt")&&!std::filesystem::exists(folder+L"\\absent"),"missing Open creates no files or parents");
    }
}
void NativeOptions(const std::wstring& folder) {
    for (unsigned variant = 0; variant < 11; ++variant) {
        Script state; script = &state; state.folder = folder; state.file = folder + L"\\owned.txt";
        const bool unsupported_provider = variant == 10;
        state.native_options_case = true; state.share = variant >= 2 && !unsupported_provider;
        state.save = variant == 8 || variant == 9;
        state.share_mode = state.save ? variant - 8 : state.share ? variant - 2 : 0;
        const uint32_t flags = (state.save ? DefaultDialogOptions(DialogFactory::Save) | FOS_FORCEFILESYSTEM : 0x1042u) |
            FOS_DONTADDTORECENT | (state.share ? FOS_SHAREAWARE : 0u) | (unsupported_provider ? FOS_STRICTFILETYPES : 0u);
        state.expected_native_options = flags;
        state.overwrite_response = state.share_mode == 1 || state.share_mode == 3 ? PULSE_PICKER_UI_V2_OVERWRITE_REFUSE :
            state.share_mode == 2 || state.share_mode == 5 ? PULSE_PICKER_UI_V2_OVERWRITE_DEFAULT : PULSE_PICKER_UI_V2_OVERWRITE_ACCEPT;
        auto provider = std::make_shared<FileDialogInprocessProvider>();
        provider->module_lifetime = std::make_shared<int>(1); provider->allow_unverified_proof = true;
        auto& api = provider->api; api.size = sizeof(api); api.version = 2;
        api.implemented_features = variant == 1 || unsupported_provider ? 0u : PULSE_PICKER_UI_V2_FEATURE_NATIVE_OPTIONS;
        api.Initialize = Initialize; api.ValidateConfig = Validate; api.Show = Show; api.Query = Query;
        api.GetWindow = Window; api.Close = Close; api.Shutdown = Shutdown;
        if (variant == 1) state.expected_native_options = 0;
        FileDialogBridgeServices services; services.access_policy = AccessPolicy::LocalOpenV1;
        services.save_access_policy = AccessPolicy::LocalSaveV1;
        services.inprocess_provider = provider;
        services.validation_verified = [](const ContractDecision& decision) {
            return RuntimeContractCertified(decision) || RuntimeLocalSaveContractCertified(decision);
        };
        unsigned native_calls = 0;
        services.show_native = [&](IFileDialog*, HWND) { ++native_calls; return E_FAIL; };
        ComPtr<IFileDialog> open;
        Check(CreateIsolatedDialog(state.save ? DialogFactory::Save : DialogFactory::Open, services, IID_PPV_ARGS(&open)) == S_OK, "native options isolated wrapper");
        if (!open) continue;
        ComPtr<IShellItem> initial; SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&initial));
        Check(open->SetFolder(initial.Get()) == S_OK && open->SetOptions(flags) == S_OK, "Firefox options accepted with real native mirror");
        if (state.save) Check(open->SetDefaultExtension(L"txt") == S_OK, "Save sharing default extension retained");
        SinkState first; first.expected = state.file; first.role = 1;
        SinkState second; second.expected = state.file; second.role = 2;
        ComPtr<IFileDialogEvents> sink; sink.Attach(new Sink(first)); open->Advise(sink.Get(), &first.cookie);
        ComPtr<IFileDialogEvents> extra;
        if (state.share_mode == 4) { extra.Attach(new Sink(second)); open->Advise(extra.Get(), &second.cookie); }
        HANDLE held = state.save ? CreateFileW(state.file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
        if (state.save) Check(held != INVALID_HANDLE_VALUE, "Save sharing callback uses an actually locked existing file");
        const HRESULT shown = open->Show(nullptr);
        if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
        if (unsupported_provider) {
            Check(native_calls == 1 && !state.shown && shown == E_FAIL, "old provider uses only native UI when an extended option is required");
            open->Unadvise(first.cookie); continue;
        }
        const bool cancelled = state.share && state.share_mode == 1;
        const bool closed = state.share && state.share_mode == 3;
        const bool stale = state.share && state.share_mode == 5;
        Check(!native_calls && state.shown == 1 && (cancelled ? shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) : closed ? shown == kClose : stale ? FAILED(shown) : shown == S_OK),
            "Firefox and sharing paths finish without a second native dialog");
        Check(state.share_calls == (!state.share || stale ? 0u : state.share_mode == 4 ? 2u : 1u), "sharing subscription order and E_NOTIMPL continuation");
        ComPtr<IShellItem> result;
        Check(cancelled || closed || stale ? FAILED(open->GetResult(&result)) && !result : open->GetResult(&result) == S_OK && result,
            "only accepted share or Firefox results are available after Show");
        open->Unadvise(first.cookie); if (extra) open->Unadvise(second.cookie);
    }
}
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); if (FAILED(com)) return 2;
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") / (L"inprocess-wrapper-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root.parent_path()); if (!CreateDirectoryW(root.c_str(), nullptr)) { CoUninitialize(); return 2; }
    const auto file = root / L"owned.txt";
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { RemoveDirectoryW(root.c_str()); CoUninitialize(); return 2; } CloseHandle(handle);
    const bool real_metadata = argc == 3 && std::wstring_view(argv[1]) == L"--metadata-real-config";
    const bool real_default = argc == 3 && std::wstring_view(argv[1]) == L"--default-folder-real-config";
    const bool real_separators = argc == 3 && std::wstring_view(argv[1]) == L"--separators-real-config";
    const bool real_values = argc == 3 && std::wstring_view(argv[1]) == L"--value-controls-real-config";
    if (real_metadata || real_values || real_default || real_separators) {
        HMODULE module = std::filesystem::path(argv[2]).is_absolute() ? LoadLibraryExW(argv[2], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32) : nullptr;
        using GetApi = HRESULT (WINAPI*)(uint32_t, uint32_t, PulsePickerUiV2Api*);
        const auto get = module ? reinterpret_cast<GetApi>(GetProcAddress(module, "PulsePickerUiGetApiV2")) : nullptr;
        if (!get || get(PULSE_PICKER_UI_V2_VERSION, sizeof(real_config_api), &real_config_api) != S_OK) {
            Check(false, "load real UI validator"); DeleteFileW(file.c_str()); RemoveDirectoryW(root.c_str()); CoUninitialize(); return 1;
        }
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--deferred-history") {
        isolated_native = true;
        DeferredHistoryTrace(root.wstring(), false);
        DeferredHistoryTrace(root.wstring(), true);
    }
    else if (real_separators || (argc == 2 && std::wstring_view(argv[1]) == L"--separators")) { SeparatorControls(root.wstring()); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--customize-gates") { isolated_native = true; Case(root.wstring(), 8); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--native-options") { isolated_native = true; NativeOptions(root.wstring()); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--null-extension") { NullExtension(root.wstring()); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--prominent-controls") { ProminentControls(root.wstring()); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--save-no-test-create") { SaveNoTestCreate(root.wstring()); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--prominent-group") { ProminentGroup(root.wstring()); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--open-missing") { OpenMissing(root.wstring()); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--pre-ui-fallback") { PreUiFallback(root.wstring()); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--navigate-contract") { navigation_case = true; FolderCase(root.wstring(), false); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--folders") { FolderCase(root.wstring(), false); FolderCase(root.wstring(), true); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--repeat-show-contract") { isolated_native = true; Case(root.wstring(), 0); Case(root.wstring(), 19); Case(root.wstring(), 26); }
    else if (real_default) { for(unsigned variant=0;variant<3;++variant)DefaultFolderTrace(root.wstring(),variant); }
    else if (real_metadata) { for(unsigned variant=0;variant<7;++variant)MetadataTrace(root.wstring(),variant); }
    else if (real_values) { ValueControlsTrace(root.wstring(),false); ValueControlsTrace(root.wstring(),true); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--rich-metadata") { MetadataTrace(root.wstring(), 7); MetadataTrace(root.wstring(), 8); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--metadata-contract") { for(unsigned variant=0;variant<6;++variant)MetadataTrace(root.wstring(),variant); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--value-controls-contract") { ValueControlsTrace(root.wstring(),false);ValueControlsTrace(root.wstring(),true); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--computer-place-contract") { ComputerPlaceTrace(root.wstring(),false); ComputerPlaceTrace(root.wstring(),true); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--menu-open-contract") { MenusTrace(root.wstring()); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--readonly-properties") { for (unsigned variant = 0; variant < 6; ++variant) ReadonlyProperties(root.wstring(), variant); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--client-places-contract") { OpenTrace(root.wstring(), 10); OpenTrace(root.wstring(), 11); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--caller-validation-open-contract") { OpenTrace(root.wstring(), 9); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--all-files-open-contract") { OpenTrace(root.wstring(), 8); }
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--notepad-open-contract") { for (unsigned variant = 0; variant < 8; ++variant) OpenTrace(root.wstring(), variant); }
    else { for (int mode = 0; mode < 49; ++mode) Case(root.wstring(), mode); OpenTrace(root.wstring()); }
    Check(DeleteFileW(file.c_str()) != FALSE && RemoveDirectoryW(root.c_str()) != FALSE, "delete only owned fixture");
    CoUninitialize(); std::printf("Checks=%u failures=%u\n", checks, failures); return failures ? 1 : 0;
}

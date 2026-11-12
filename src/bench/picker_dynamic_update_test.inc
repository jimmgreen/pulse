namespace dynamic_update_test {
struct State {
    PulsePickerUiV2Api* api = nullptr;
    PulsePickerUiV2UpdateText update = nullptr;
    PulsePickerUiV2Session session = 0;
    bool ready = false;
    unsigned invalid_drafts = 0, valid_drafts = 0;
    bool readonly_has_no_edit = false;
    bool narrow = false;
    std::wstring screenshot, scrolled_screenshot;
    std::thread driver;
};
HRESULT WINAPI Event(void* context, const PulsePickerUiV2Event* event) {
    auto& state = *static_cast<State*>(context);
    if (event->kind == PULSE_PICKER_UI_V2_EVENT_PROPERTY_CHANGED) {
        Check(event->control_id == 7 && event->property_value_count == 1, "scalar draft keeps ABI scalar index");
        if (event->property_value_count != 1) return E_INVALIDARG;
        const auto& value = event->property_values[0]; const std::wstring text(value.data, value.units);
        if (text == L"t") { ++state.invalid_drafts; return S_FALSE; }
        if (text == L"true") { ++state.valid_drafts; return S_OK; }
        return E_INVALIDARG;
    }
    if (event->kind != PULSE_PICKER_UI_V2_EVENT_READY) return S_OK;
    state.ready = true;
    for (uint32_t kind = PULSE_PICKER_UI_V2_TEXT_TITLE; kind <= PULSE_PICKER_UI_V2_TEXT_DEFAULT_EXTENSION; ++kind) {
        const std::wstring text = kind == PULSE_PICKER_UI_V2_TEXT_FILENAME ? L"changed-name.txt" : kind == PULSE_PICKER_UI_V2_TEXT_DEFAULT_EXTENSION ? L"log" : L"Changed label";
        Check(state.update(state.session, event->show_id, kind, {text.data(), static_cast<uint32_t>(text.size())}) == S_OK,
            "live text update accepted by actual UI");
    }
    wchar_t title[80]{}; GetWindowTextW(event->window, title, 80);
    Check(wcscmp(title, L"Changed label") == 0, "live title changes actual HWND");
    PulsePickerUiV2Snapshot snapshot{}; snapshot.size = sizeof(snapshot);
    const HRESULT queried = state.api->Query(state.session, event->show_id, 0, &snapshot);
    Check((queried == S_OK || queried == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) && snapshot.filename.units == 16,
        "live filename updates authoritative snapshot");
    PulsePickerUiV2ControlItem items[2]{{sizeof(items[0]), 9, 3, 0, {L"Nine", 4}}, {sizeof(items[0]), 7, 3, 0, {L"Seven", 5}}};
    PulsePickerUiV2Control control{}; control.size = sizeof(control); control.kind = PULSE_PICKER_UI_V2_CONTROL_COMBO;
    control.id = 42; control.state = 3; control.items = items; control.item_count = 2; control.has_selected = 1; control.selected_item_id = 7;
    Check(state.api->UpdateControl(state.session, event->show_id, &control) == S_OK, "live item addition replaces UI structure");
    control.items = items + 1; control.item_count = 1;
    Check(state.api->UpdateControl(state.session, event->show_id, &control) == S_OK, "live selected item retained after removal");
    control.item_count = 0; control.has_selected = 0; control.selected_item_id = 0;
    Check(state.api->UpdateControl(state.session, event->show_id, &control) == S_OK, "ABI supports removing final item");
    Check(state.update(state.session, event->show_id, 0, {}) == E_INVALIDARG, "unknown text kind rejected");
    Check(state.update(state.session, event->show_id, PULSE_PICKER_UI_V2_TEXT_TITLE, {}) == S_OK, "explicit empty live label retained");
    state.driver = std::thread([&state, window = event->window] {
        Sleep(150);
        if (state.narrow) SetWindowPos(window, nullptr, 0, 0, 700, 600, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        const HWND edit = GetDlgItem(window, 2002);
        state.readonly_has_no_edit = edit != nullptr;
        for (int id = 2003; id <= 2008; ++id)
            state.readonly_has_no_edit = state.readonly_has_no_edit && !GetDlgItem(window, id);
        DWORD_PTR ignored = 0;
        if (edit) {
            SendMessageTimeoutW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"t"), SMTO_ABORTIFHUNG, 2000, &ignored);
            Sleep(150);
            SendMessageTimeoutW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"true"), SMTO_ABORTIFHUNG, 2000, &ignored);
            Sleep(150);
        }
        CapturePickerWindow(window, state.screenshot);
        if (state.narrow) {
            RECT client{}; GetClientRect(window, &client);
            POINT point{client.right / 2, client.bottom - 130}; ClientToScreen(window, &point);
            SendMessageTimeoutW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA * 8)),
                MAKELPARAM(point.x, point.y), SMTO_ABORTIFHUNG, 2000, &ignored);
            Sleep(150);
            CapturePickerWindow(window, state.scrolled_screenshot);
        }
        PostMessageW(window, WM_CLOSE, 0, 0);
    });
    return S_OK;
}
int Run(const wchar_t* path, bool narrow = false) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    PulsePickerUiV2Api api{}; PulsePickerUiV2UpdateText update = nullptr;
#ifdef PULSE_REMOTE_PROVIDER_TEST
    std::shared_ptr<const pulse::shell_bridge::FileDialogInprocessProvider> provider;
    const HRESULT loaded = pulse::shell_bridge::LoadFileDialogRemoteProvider(path, provider, true);
    if (loaded == S_OK) { api = provider->api; update = provider->update_text; }
#else
    HMODULE module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const auto get = module ? reinterpret_cast<decltype(&PulsePickerUiGetApiV2)>(GetProcAddress(module, "PulsePickerUiGetApiV2")) : nullptr;
    const HRESULT loaded = get ? get(2, sizeof(api), &api) : E_NOINTERFACE;
    if (module) update = reinterpret_cast<PulsePickerUiV2UpdateText>(GetProcAddress(module, "PulsePickerUiUpdateTextV2"));
#endif
    Check(loaded == S_OK && update && (api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_DYNAMIC_TEXT) &&
        (api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_DYNAMIC_ITEMS) &&
        (api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_PROPERTY_PRESENTATION), "dynamic and readonly presentation features negotiated");
    if (!update || loaded != S_OK) return 1;
    State state; state.api = &api; state.update = update;
    state.narrow = narrow;
#ifdef PULSE_REMOTE_PROVIDER_TEST
    state.screenshot = std::filesystem::absolute(L"bench_data/dynamic-readonly-remote.png").wstring();
#else
    state.screenshot = std::filesystem::absolute(L"bench_data/dynamic-readonly-local.png").wstring();
#endif
    if (narrow) {
        state.screenshot.insert(state.screenshot.size() - 4, L"-narrow");
        state.scrolled_screenshot = state.screenshot;
        state.scrolled_screenshot.insert(state.scrolled_screenshot.size() - 4, L"-scrolled");
        std::filesystem::remove(state.scrolled_screenshot);
    }
    std::filesystem::remove(state.screenshot);
    PulsePickerUiV2Initialize initialize{sizeof(initialize), 2};
    Check(api.Initialize(&initialize, &state.session) == S_OK, "initialize dynamic fixture");
    const std::wstring folder = std::filesystem::absolute(L"bench_data").wstring();
    PulsePickerUiV2Control control{}; control.size = sizeof(control); control.kind = PULSE_PICKER_UI_V2_CONTROL_COMBO; control.id = 42; control.state = 3;
    PulsePickerUiV2Config config{}; config.size = sizeof(config); config.version = 2; config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
    config.initial_folder = {folder.data(), static_cast<uint32_t>(folder.size())}; config.controls = &control; config.control_count = 1;
    config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;
    config.save_options = PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;
    PulsePickerUiV2Text initial_property{L"false", 5};
    PulsePickerUiV2Text readonly_value{L"Read only value", 15};
    PulsePickerUiV2Text readonly_list[]{{L"First entry", 11}, {L"Second entry", 12}};
    PulsePickerUiV2Property properties[]{
        {sizeof(PulsePickerUiV2Property), 7, PULSE_PICKER_UI_V2_PROPERTY_SCALAR, {L"Flag", 4}, &initial_property, 1},
        {sizeof(PulsePickerUiV2Property), 8, PULSE_PICKER_UI_V2_PROPERTY_READONLY_SCALAR, {L"Read only scalar", 16}, &readonly_value, 1},
        {sizeof(PulsePickerUiV2Property), 9, PULSE_PICKER_UI_V2_PROPERTY_READONLY_LIST, {L"Read only list", 14}, readonly_list, 2},
        {sizeof(PulsePickerUiV2Property), 10, PULSE_PICKER_UI_V2_PROPERTY_GROUP, {L"Metadata group", 14}, nullptr, 0}};
    config.properties = properties; config.property_count = 4;
    PulsePickerUiV2Callbacks callbacks{}; callbacks.size = sizeof(callbacks); callbacks.context = &state; callbacks.callback = Event;
    std::vector<wchar_t> path_buffer(PULSE_PICKER_UI_V2_MAX_UNITS + 1), paths(PULSE_PICKER_UI_V2_MAX_PACKED_UNITS + 1);
    PulsePickerUiV2Result result{}; result.size = sizeof(result); result.path = {path_buffer.data(), static_cast<uint32_t>(path_buffer.size()), 0}; result.paths = {paths.data(), static_cast<uint32_t>(paths.size()), 0};
    const HRESULT shown = api.Show(state.session, &config, &callbacks, &result);
    std::printf("DYNAMIC_SHOW hr=%08lX status=%u win32=%u ready=%u\n", static_cast<unsigned long>(shown), result.status, result.win32_error, state.ready ? 1u : 0u);
    Check(shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) && state.ready,
        "live dynamic scenario reaches ready and cancels");
    if (state.driver.joinable()) state.driver.join();
    Check(state.readonly_has_no_edit, "readonly scalar/list/group add no editable HWND and preserve scalar edit ID");
    Check(std::filesystem::exists(state.screenshot), "readonly presentation live screenshot captured");
    if (narrow) Check(std::filesystem::exists(state.scrolled_screenshot), "narrow property panel scroll screenshot captured");
    std::printf("READONLY_SCREENSHOT=%ls\n", state.screenshot.c_str());
    Check(state.invalid_drafts == 1 && state.valid_drafts == 1, "partial scalar text survives and correction reaches same live dialog");
    Check(api.Shutdown(state.session) == S_OK, "shutdown dynamic fixture");
    CoUninitialize();
    std::printf("Dynamic checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
}

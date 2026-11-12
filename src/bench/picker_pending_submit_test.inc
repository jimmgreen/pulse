namespace pending_submit_test {
struct State {
    bool cancel = false, requested = false, ready = false;
    unsigned accepted = 0;
};
HRESULT WINAPI Event(void* context, const PulsePickerUiV2Event* event) {
    auto& state = *static_cast<State*>(context);
    if (event->kind == PULSE_PICKER_UI_V2_EVENT_TYPE_CHANGED && !state.requested) {
        state.requested = true;
        Check(!state.ready, "caller command is issued before Ready");
        SendMessageW(event->window, WM_COMMAND, state.cancel ? IDCANCEL : IDOK, 0);
        if (!state.cancel) SendMessageW(event->window, WM_COMMAND, IDOK, 0);
    }
    if (event->kind == PULSE_PICKER_UI_V2_EVENT_READY) state.ready = true;
    if (event->kind == PULSE_PICKER_UI_V2_EVENT_FILE_OK) {
        Check(state.ready, "deferred submit reaches FileOk only after visible Ready");
        if (++state.accepted == 1) { PostMessageW(event->window, WM_COMMAND, IDOK, 0); return S_FALSE; }
    }
    return S_OK;
}
int Run(const wchar_t* dll) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    HMODULE module = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const auto get = module ? reinterpret_cast<decltype(&PulsePickerUiGetApiV2)>(GetProcAddress(module, "PulsePickerUiGetApiV2")) : nullptr;
    PulsePickerUiV2Api api{};
    if (!get || get(2, sizeof(api), &api) != S_OK) return 2;
    const auto folder = std::filesystem::absolute(std::filesystem::path(L"bench_data") / (L"pending-submit-" + std::to_wstring(GetCurrentProcessId())));
    if (!std::filesystem::create_directory(folder)) return 2;
    const auto text = folder.wstring();
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread watchdog([done] { if (WaitForSingleObject(done, 20000) != WAIT_OBJECT_0) TerminateProcess(GetCurrentProcess(), 99); });
    for (unsigned cancel = 0; cancel != 2; ++cancel) {
        PulsePickerUiV2Initialize initialize{sizeof(initialize), 2}; PulsePickerUiV2Session session = 0;
        Check(api.Initialize(&initialize, &session) == S_OK, "initialize pending command fixture");
        PulsePickerUiV2Config config{}; config.size = sizeof(config); config.version = 2;
        config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;
        config.save_options = PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;
        config.initial_folder = {text.data(), static_cast<uint32_t>(text.size())}; config.filename = {L"owned.txt", 9};
        State state; state.cancel = cancel != 0;
        PulsePickerUiV2Callbacks callbacks{}; callbacks.size = sizeof(callbacks); callbacks.context = &state; callbacks.callback = Event;
        std::vector<wchar_t> path(PULSE_PICKER_UI_V2_MAX_UNITS + 1), paths(PULSE_PICKER_UI_V2_MAX_PACKED_UNITS + 1);
        PulsePickerUiV2Result result{}; result.size = sizeof(result);
        result.path = {path.data(), static_cast<uint32_t>(path.size()), 0}; result.paths = {paths.data(), static_cast<uint32_t>(paths.size()), 0};
        const HRESULT shown = api.Show(session, &config, &callbacks, &result);
        std::printf("PENDING_SUBMIT cancel=%u hr=%08lX ready=%u fileok=%u win32=%u\n", cancel, static_cast<unsigned long>(shown), state.ready, state.accepted, result.win32_error);
        Check(state.requested && (cancel ? shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) && !state.ready && !state.accepted :
            shown == S_OK && state.ready && state.accepted == 2), "pre-ready OK coalesces and preserves veto; Cancel stays immediate");
        Check(api.Shutdown(session) == S_OK, "shutdown pending command fixture");
    }
    SetEvent(done); watchdog.join(); CloseHandle(done);
    Check(std::filesystem::is_empty(folder) && std::filesystem::remove(folder), "pending command fixture writes no target and cleans up");
    FreeLibrary(module); CoUninitialize();
    std::printf("Pending checks=%u failures=%u\n", checks, failures); return failures ? 1 : 0;
}
}

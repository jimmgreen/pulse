namespace navigation_root_test {
struct State {
    PulsePickerUiV2Api* api = nullptr;
    PulsePickerUiV2Session session = 0;
    std::wstring parent, root, screenshot, expanded_screenshot;
    bool ready = false, up_outside = false, child_selected = false;
    std::thread driver;
};
HRESULT WINAPI Event(void* context, const PulsePickerUiV2Event* event) {
    auto& state = *static_cast<State*>(context);
    if (event->kind == PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGED) {
        std::vector<wchar_t> folder(PULSE_PICKER_UI_V2_MAX_UNITS + 1), name(folder.size()), selection(folder.size()), candidate(folder.size());
        PulsePickerUiV2Snapshot snapshot{}; snapshot.size = sizeof(snapshot);
        snapshot.folder = {folder.data(), static_cast<uint32_t>(folder.size()), 0};
        snapshot.filename = {name.data(), static_cast<uint32_t>(name.size()), 0};
        snapshot.selection = {selection.data(), static_cast<uint32_t>(selection.size()), 0};
        snapshot.candidate = {candidate.data(), static_cast<uint32_t>(candidate.size()), 0};
        const HRESULT queried = state.api->Query(state.session, event->show_id, 0, &snapshot);
        if (queried == S_OK && snapshot.folder.units < folder.size()) {
            state.up_outside |= state.parent == std::wstring(folder.data(), snapshot.folder.units);
            state.child_selected |= state.root + L"\\alpha\\leaf" == std::wstring(folder.data(), snapshot.folder.units);
        }
    }
    if (event->kind == PULSE_PICKER_UI_V2_EVENT_READY) {
        state.ready = true;
        state.driver = std::thread([&state, window = event->window] {
            using SetDpi = HANDLE (WINAPI*)(HANDLE);
            const auto set_dpi = reinterpret_cast<SetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
            const HANDLE previous_dpi = set_dpi ? set_dpi(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4))) : nullptr;
            Sleep(900);
            CapturePickerWindow(window, state.screenshot);
            using GetDpi = UINT (WINAPI*)(HWND);
            const auto get_dpi = reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
            const float scale = (get_dpi ? get_dpi(window) : 96) / 96.0f;
            RECT bounds{}; GetClientRect(window, &bounds);
            std::printf("ROOT_INPUT dpi=%u client=%ldx%ld\n", get_dpi ? get_dpi(window) : 96, bounds.right, bounds.bottom);
            const auto click = [&](float x, float y) {
                const auto point = MAKELPARAM(static_cast<int>(x * scale), static_cast<int>(y * scale));
                SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
                SendMessageW(window, WM_LBUTTONUP, 0, point);
            };
            click(183, 180); Sleep(500); // Expand alpha using the shared tree chevron.
            click(183, 248); Sleep(500); // beta moves down after alpha's child appears.
            CapturePickerWindow(window, state.expanded_screenshot);
            click(92, 214); Sleep(500); // Navigate through alpha's actual child row.
            click(70, 146); Sleep(500); // Return to the root row, then use user Up.
            click(86, 63);
            Sleep(500);
            PostMessageW(window, WM_CLOSE, 0, 0);
            if (previous_dpi) set_dpi(previous_dpi);
        });
    }
    return S_OK;
}
int Run(const wchar_t* path, bool old_peer = false) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    PulsePickerUiV2Api api{}; PulsePickerUiV2SetNavigationRoot set_root = nullptr;
#ifdef PULSE_REMOTE_PROVIDER_TEST
    std::shared_ptr<const pulse::shell_bridge::FileDialogInprocessProvider> provider;
    const HRESULT loaded = pulse::shell_bridge::LoadFileDialogRemoteProvider(path, provider, true);
    if (loaded == S_OK) { api = provider->api; set_root = provider->set_navigation_root; }
#else
    HMODULE module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const auto get = module ? reinterpret_cast<decltype(&PulsePickerUiGetApiV2)>(GetProcAddress(module, "PulsePickerUiGetApiV2")) : nullptr;
    const HRESULT loaded = get ? get(2, sizeof(api), &api) : E_NOINTERFACE;
    if (module) set_root = reinterpret_cast<PulsePickerUiV2SetNavigationRoot>(GetProcAddress(module, "PulsePickerUiSetNavigationRootV2"));
#endif
    if (loaded != S_OK || !set_root) return 2;
    const auto base = std::filesystem::absolute(std::filesystem::path(L"bench_data") / (L"root-tree-" + std::to_wstring(GetCurrentProcessId())));
    if (!std::filesystem::create_directory(base)) return 2;
    std::filesystem::create_directories(base / L"root" / L"alpha" / L"leaf");
    std::filesystem::create_directories(base / L"root" / L"beta" / L"leaf");
    const auto root = (base / L"root").wstring();
    State state; state.api = &api; state.parent = base.wstring(); state.root = root;
#ifdef PULSE_REMOTE_PROVIDER_TEST
    state.screenshot = std::filesystem::absolute(L"bench_data/navigation-root-remote.png").wstring();
#else
    state.screenshot = std::filesystem::absolute(L"bench_data/navigation-root-local.png").wstring();
#endif
    if (!old_peer) std::filesystem::remove(state.screenshot);
    state.expanded_screenshot = state.screenshot;
    state.expanded_screenshot.insert(state.expanded_screenshot.size() - 4, L"-expanded");
    if (!old_peer) std::filesystem::remove(state.expanded_screenshot);
    PulsePickerUiV2Initialize initialize{sizeof(initialize), 2};
    Check(api.Initialize(&initialize, &state.session) == S_OK, "initialize navigation tree fixture");
    const HRESULT rooted = set_root(state.session, Text(root));
    Check(old_peer ? rooted == E_NOTIMPL : rooted == S_OK, "navigation root honors explicit peer feature gate");
    if (!old_peer && rooted == S_OK) {
        PulsePickerUiV2Config config{}; config.size = sizeof(config); config.version = 2;
        config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1; config.initial_folder = Text(root);
        PulsePickerUiV2Callbacks callbacks{}; callbacks.size = sizeof(callbacks); callbacks.context = &state; callbacks.callback = Event;
        std::vector<wchar_t> result_path(PULSE_PICKER_UI_V2_MAX_UNITS + 1), paths(PULSE_PICKER_UI_V2_MAX_PACKED_UNITS + 1);
        PulsePickerUiV2Result result{}; result.size = sizeof(result);
        result.path = {result_path.data(), static_cast<uint32_t>(result_path.size()), 0}; result.paths = {paths.data(), static_cast<uint32_t>(paths.size()), 0};
        const HRESULT shown = api.Show(state.session, &config, &callbacks, &result);
        if (state.driver.joinable()) state.driver.join();
        Check(shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) && state.ready, "navigation tree shows and cancels");
        Check(state.up_outside, "user Up may navigate above root as native permits");
        Check(state.child_selected, "expanded tree child navigates through normal folder callbacks");
        Check(std::filesystem::exists(state.screenshot), "actual navigation tree screenshot captured");
        Check(std::filesystem::exists(state.expanded_screenshot), "expanded navigation tree screenshot captured");
    }
    api.Shutdown(state.session);
    std::filesystem::remove_all(base); CoUninitialize();
    std::printf("Root checks=%u failures=%u\n", checks, failures); return failures ? 1 : 0;
}
}

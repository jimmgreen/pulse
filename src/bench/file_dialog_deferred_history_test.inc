void DeferredHistoryTrace(const std::wstring& input_folder, bool explicit_folder) {
    const auto folder = std::filesystem::path(input_folder).lexically_normal().make_preferred().wstring();
    const auto remembered = folder + L"\\deferred-remembered";
    Check(CreateDirectoryW(remembered.c_str(), nullptr) != FALSE, "create deferred-history fixture");
    Script state;
    script = &state;
    state.default_folder = true;
    state.cancel_before_accept = true;
    state.folder = explicit_folder ? folder : remembered;
    state.file = state.folder + L"\\owned.txt";
    auto provider = std::make_shared<FileDialogInprocessProvider>();
    provider->module_lifetime = std::make_shared<int>(1);
    provider->allow_unverified_proof = true;
    auto& api = provider->api;
    api.size = sizeof(api); api.version = 2;
    api.Initialize = Initialize; api.ValidateConfig = Validate; api.Show = Show;
    api.Query = Query; api.GetWindow = Window; api.Close = Close;
    api.Shutdown = Shutdown; api.UpdateControl = UpdateControl;
    api.implemented_features = PULSE_PICKER_UI_V2_FEATURE_MULTI_SELECT | PULSE_PICKER_UI_V2_FEATURE_CUSTOM_LABELS;
    unsigned native_calls = 0, loads = 0;
    FileDialogBridgeServices services;
    services.access_policy = AccessPolicy::LocalOpenV1;
    services.prefer_bridge = true;
    services.executable = L"C:\\not-used\\pulse.exe";
    services.load_inprocess_provider = [&] { ++loads; return provider; };
    services.validation_verified = [](const ContractDecision& decision) { return RuntimeContractCertified(decision); };
    services.show_native = [&](IFileDialog*, HWND) { ++native_calls; return E_UNEXPECTED; };
    const auto registry_root = L"Software\\PulseTestFixtures\\DeferredHistory-" + std::to_wstring(GetCurrentProcessId());
    services.client_state = std::make_shared<FileDialogClientState>(L"deferred-history-test", registry_root);
    GUID client{};
    CoCreateGuid(&client);
    const bool seeded = services.client_state->Write(client, remembered);
    Check(seeded && services.client_state->Read(client) == remembered, "seed deferred client history and read exact value");
    if (!seeded) {
        std::printf("DEFERRED_FIXTURE seed_failed path_units=%zu\n", remembered.size());
#if !defined(PULSE_DEFERRED_HISTORY_MEMORY)
        RegDeleteTreeW(HKEY_CURRENT_USER, registry_root.c_str());
#endif
        Check(RemoveDirectoryW(remembered.c_str()) != FALSE, "remove fixture after seed failure");
        return;
    }
    ComPtr<IFileOpenDialog> open;
    Check(CreateIsolatedDialog(DialogFactory::Open, services, IID_PPV_ARGS(&open)) == S_OK,
        "create wrapper with deferred provider");
    ComPtr<IShellItem> initial;
    SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&initial));
    const COMDLG_FILTERSPEC filter{L"All", L"*.*"};
    Check(open->SetClientGuid(client) == S_OK && open->SetOptions(static_cast<FILEOPENDIALOGOPTIONS>(0x1A48)) == S_OK &&
        open->SetFileTypes(1, &filter) == S_OK && open->SetFileTypeIndex(1) == S_OK &&
        open->SetDefaultFolder(initial.Get()) == S_OK, "capture deferred default-folder setup");
    if (explicit_folder) Check(open->SetFolder(initial.Get()) == S_OK, "explicit folder overrides remembered history");
    SinkState sink_state;
    ComPtr<IFileDialogEvents> sink;
    sink.Attach(new Sink(sink_state));
    Check(open->Advise(sink.Get(), &sink_state.cookie) == S_OK && loads == 0,
        "event subscription requires provider while preserving deferred loading");
    Check(open->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && native_calls == 0 && state.shown == 1 && loads == 1,
        "deferred history reaches provider once without native fallback");
    Check(services.client_state->Read(client) == remembered, "cancel preserves seeded history even with explicit folder");
    open->Unadvise(sink_state.cookie);
    open.Reset(); sink.Reset();
    Check(services.client_state->Clear(client) && !services.client_state->Read(client), "clear isolated remembered state");
#if !defined(PULSE_DEFERRED_HISTORY_MEMORY)
    Check(RegDeleteTreeW(HKEY_CURRENT_USER, registry_root.c_str()) == ERROR_SUCCESS, "remove deferred registry fixture");
#endif
    Check(RemoveDirectoryW(remembered.c_str()) != FALSE, "remove deferred folder fixture");
}

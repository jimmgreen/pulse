namespace navigation_root_contract {
unsigned root_calls = 0;
void Run(const std::wstring& folder) {
    for (unsigned mode = 0; mode != 4; ++mode) {
        root_calls = 0;
        Script state; state.folder = folder; state.file = folder + L"\\owned.txt"; script = &state;
        auto provider = std::make_shared<FileDialogInprocessProvider>();
        provider->module_lifetime = std::make_shared<int>(1); provider->allow_unverified_proof = true;
        auto& api = provider->api; api.size = sizeof(api); api.version = 2;
        api.Initialize = Initialize; api.ValidateConfig = Validate; api.Show = Show; api.Query = Query;
        api.GetWindow = Window; api.Close = Close; api.Shutdown = Shutdown;
        api.implemented_features = mode ? PULSE_PICKER_UI_V2_FEATURE_NAVIGATION_ROOT : 0;
        if (mode != 1) provider->set_navigation_root = [](PulsePickerUiV2Session, PulsePickerUiV2Text) -> HRESULT {
            ++root_calls; return S_OK;
        };
        unsigned native_calls = 0;
        FileDialogBridgeServices services; services.inprocess_provider = provider; services.access_policy = AccessPolicy::LocalOpenV1;
        services.validation_verified = [](const ContractDecision& d) { return RuntimeContractCertified(d); };
        services.show_native = [&](IFileDialog*, HWND) { ++native_calls; return HRESULT_FROM_WIN32(ERROR_CANCELLED); };
        ComPtr<IFileDialog> dialog;
        Check(CreateIsolatedDialog(DialogFactory::Open, services, IID_PPV_ARGS(&dialog)) == S_OK, "create native-backed root capability fixture");
        ComPtr<IShellItem> root;
        const HRESULT item = mode == 2 ? SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&root)) :
            SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&root));
        ComPtr<IFileDialog2> dialog2; if (dialog) dialog.As(&dialog2);
        Check(item == S_OK && dialog2 && dialog2->SetNavigationRoot(root.Get()) == S_OK, "capture native filesystem or virtual navigation root");
        if (!dialog) continue;
        if (mode != 2) dialog->SetFolder(root.Get());
        Check(dialog->SetOptions(FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT) == S_OK,
            "configure supported root fixture options");
        if (mode == 3) {
            Check(dialog->Show(nullptr) == S_OK && !native_calls && state.shown == 1 && root_calls == 1,
                "capable provider receives root and accepts through normal bridge");
            Check(dialog2->SetNavigationRoot(root.Get()) == S_OK, "root setter remains usable between completed Shows");
            Check(dialog->Show(nullptr) == S_OK && !native_calls && state.shown == 2 && root_calls == 2,
                "repeated Show serializes root again without native fallback");
            continue;
        }
        Check(dialog->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && native_calls == 1 && state.shown == 0 && root_calls == 0,
            "old feature, missing export, and virtual root preserve native route before UI");
        Check(mode == 2 ? state.initialized == 0 : state.initialized == 1 && state.stopped == 1,
            "root gate shuts down prepared session without interacting or replaying UI");
    }
}
}

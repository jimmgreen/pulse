namespace metadata_live {
struct Events final : IFileDialogEvents {
    ULONG references = 1;
    bool cancel = false, submitted = false;
    unsigned accepted = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IFileDialogEvents) return E_NOINTERFACE;
        *out = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    static void Inspect(IFileDialog* dialog, bool available) {
        ComPtr<IFileSaveDialog> save; dialog->QueryInterface(IID_PPV_ARGS(&save));
        ComPtr<IPropertyStore> properties; const HRESULT hr = save->GetProperties(&properties);
        Check(available ? hr == S_OK && properties : hr == E_UNEXPECTED && !properties, "real UI metadata getter follows FileOk/accepted lifetime");
        if (available && properties) {
            PROPVARIANT title{}, rating{}, flag{};
            properties->GetValue(PKEY_Title, &title); properties->GetValue(PKEY_Rating, &rating); properties->GetValue(PKEY_IsFlagged, &flag);
            Check(title.vt == VT_LPWSTR && title.pwszVal && wcscmp(title.pwszVal, L"Live source title") == 0 &&
                rating.vt == VT_UI4 && rating.ulVal == 50 && flag.vt == VT_BOOL && flag.boolVal == VARIANT_TRUE,
                "real UI preserves original string/integer/bool store through SaveAsItem");
            PropVariantClear(&title); PropVariantClear(&rating); PropVariantClear(&flag);
        }
    }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog* dialog) override {
        Inspect(dialog, false);
        if (submitted) return S_OK; submitted = true;
        ComPtr<IOleWindow> ole; HWND window = nullptr; dialog->QueryInterface(IID_PPV_ARGS(&ole));
        Check(ole && ole->GetWindow(&window) == S_OK && window, "real metadata UI exposes actual window");
        if (window) SetTimer(window, cancel ? 0xF12 : 0xF11, 250, [](HWND hwnd, UINT, UINT_PTR id, DWORD) {
            KillTimer(hwnd, id); PostMessageW(hwnd, WM_COMMAND, id == 0xF12 ? IDCANCEL : IDOK, 0);
        });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override { ++accepted; Inspect(dialog, true); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* out) override { *out = FDESVR_DEFAULT; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* out) override { *out = FDEOR_DEFAULT; return S_OK; }
};
void Run(const wchar_t* dll, const std::wstring& folder) {
    HMODULE module = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    auto get = module ? reinterpret_cast<decltype(&PulsePickerUiGetApiV2)>(GetProcAddress(module, "PulsePickerUiGetApiV2")) : nullptr;
    Check(get != nullptr, "load explicit real metadata UI provider"); if (!get) return;
    auto provider = std::make_shared<FileDialogInprocessProvider>();
    provider->module_lifetime = std::shared_ptr<void>(module, [](void* value) { FreeLibrary(static_cast<HMODULE>(value)); });
    provider->allow_unverified_proof = true;
    Check(get(2, sizeof(provider->api), &provider->api) == S_OK, "load metadata provider API");
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread watchdog([done] { if (WaitForSingleObject(done, 30000) != WAIT_OBJECT_0) TerminateProcess(GetCurrentProcess(), 99); });
    for (unsigned variant = 0; variant != 3; ++variant) {
        unsigned native_calls = 0; FileDialogBridgeServices services;
        services.inprocess_provider = provider; services.save_access_policy = AccessPolicy::LocalSaveV1;
        services.validation_verified = [](const ContractDecision& decision) { return RuntimeLocalSaveContractCertified(decision); };
        services.show_native = [&](IFileDialog*, HWND) { ++native_calls; return E_UNEXPECTED; };
        ComPtr<IFileSaveDialog> save;
        Check(CreateIsolatedDialog(DialogFactory::Save, services, IID_PPV_ARGS(&save)) == S_OK, "create real metadata wrapper");
        if (!save) continue;
        ComPtr<IShellItem> source; SHCreateItemFromParsingName((folder + L"\\owned.txt").c_str(), nullptr, IID_PPV_ARGS(&source));
        ComPtr<IPropertyStore> properties; PSCreateMemoryPropertyStore(IID_PPV_ARGS(&properties));
        PROPVARIANT value{}; InitPropVariantFromString(L"Live source title", &value); properties->SetValue(PKEY_Title, value); PropVariantClear(&value);
        value.vt = VT_UI4; value.ulVal = 50; properties->SetValue(PKEY_Rating, value);
        value = {}; value.vt = VT_BOOL; value.boolVal = VARIANT_TRUE; properties->SetValue(PKEY_IsFlagged, value);
        ComPtr<IPropertyDescriptionList> list;
        PSGetPropertyDescriptionListFromString(L"prop:System.Title;System.Rating;System.IsFlagged", IID_PPV_ARGS(&list));
        if (!variant) Check(save->SetSaveAsItem(source.Get()) == S_OK, "real source before metadata");
        Check(save->SetProperties(properties.Get()) == S_OK && save->SetCollectedProperties(list.Get(), FALSE) == S_OK, "real metadata store and collection");
        if (variant) Check(save->SetSaveAsItem(source.Get()) == S_OK, "real source after metadata");
        save->SetFileName(L"new-live-result.txt"); save->SetDefaultExtension(L"txt");
        save->SetOptions(FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT);
        Events::Inspect(save.Get(), false);
        Events events; events.cancel = variant == 2; DWORD cookie = 0; save->Advise(&events, &cookie);
        const HRESULT shown = save->Show(nullptr);
        Check(!native_calls && events.submitted && shown == (events.cancel ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK), "real metadata UI accepts or cancels without native replay");
        if (!events.cancel) { Check(events.accepted == 1, "real metadata FileOk fires once"); Events::Inspect(save.Get(), true); }
        else { ComPtr<IPropertyStore> result; Check(FAILED(save->GetProperties(&result)) && !result, "real cancellation does not publish metadata"); }
        save->Unadvise(cookie);
        Check(GetFileAttributesW((folder + L"\\new-live-result.txt").c_str()) == INVALID_FILE_ATTRIBUTES, "dialog selection does not write a target file");
    }
    SetEvent(done); watchdog.join(); CloseHandle(done);
}
}

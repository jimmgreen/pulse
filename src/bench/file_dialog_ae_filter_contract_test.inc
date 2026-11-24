// Contract shape, not an AE recording: filter strings and both media paths are fixtures.
namespace ae_filter_contract {
constexpr UINT kCount = 56, kSelected = 55;
constexpr FILEOPENDIALOGOPTIONS kOptions = 0x1A48;
std::wstring Label(UINT row) { return L"Fixture format " + std::to_wstring(row + 1); }
std::wstring Pattern(UINT row) {
    if (row + 1 == kSelected) return L"*.txt;*.png";
    if (row + 1 == kCount) return L"*.*";
    return L"*.fmt" + std::to_wstring(row + 1);
}
bool Equal(PulsePickerUiV2Text value, const std::wstring& text) {
    return value.data && std::wstring_view(value.data, value.units) == text;
}
HRESULT WINAPI ValidateList(PulsePickerUiV2Session session, const PulsePickerUiV2Config* config) {
    bool exact = config && config->filters && config->filter_count == kCount &&
        config->filter_index == kSelected - 1 && config->native_options == kOptions &&
        (config->open_options & PULSE_PICKER_UI_V2_OPEN_ALLOW_MULTI_SELECT);
    for (UINT i = 0; exact && i < kCount; ++i) {
        exact = Equal(config->filters[i].label, Label(i)) && Equal(config->filters[i].pattern, Pattern(i));
    }
    Check(exact, "all 56 synthetic names/patterns in order; ABI index 54 and multi-select preserved");
    return exact ? Validate(session, config) : E_UNEXPECTED;
}
uint32_t selection = 0;
HRESULT WINAPI QuerySelection(PulsePickerUiV2Session session, uint64_t show, uint64_t generation, PulsePickerUiV2Snapshot* out) {
    const auto hr = Query(session, show, generation, out);
    out->filter_index = selection;
    return hr;
}
HRESULT WINAPI ShowSelection(PulsePickerUiV2Session session, const PulsePickerUiV2Config* config,
    const PulsePickerUiV2Callbacks* callbacks, PulsePickerUiV2Result* out) {
    selection = config->filter_index;
    const auto hr = Show(session, config, callbacks, out);
    out->filter_index = selection;
    return hr;
}
void Run(const std::wstring& folder) {
    Script state;
    script = &state; state.folder_multi = true; // Ask the shared script to pack two real item paths.
    state.folder = folder; state.file = folder + L"\\owned.txt"; state.second = folder + L"\\second.png";
    auto provider = std::make_shared<FileDialogInprocessProvider>();
    provider->module_lifetime = std::make_shared<int>(1); provider->allow_unverified_proof = true;
    auto& api = provider->api;
    api.size = sizeof(api); api.version = 2; api.Initialize = Initialize; api.ValidateConfig = ValidateList;
    api.Show = ShowSelection; api.Query = QuerySelection; api.GetWindow = Window; api.Close = Close;
    api.Shutdown = Shutdown; api.UpdateControl = UpdateControl;
    api.implemented_features = PULSE_PICKER_UI_V2_FEATURE_MULTI_SELECT | PULSE_PICKER_UI_V2_FEATURE_NATIVE_OPTIONS |
        PULSE_PICKER_UI_V2_FEATURE_CUSTOM_LABELS;
    FileDialogBridgeServices services;
    services.inprocess_provider = provider; services.access_policy = AccessPolicy::LocalOpenV1;
    // Suppress client history writes; this case tests filters rather than persistence.
    services.client_state = std::make_shared<FileDialogClientState>(L"", L"");
    services.validation_verified = RuntimeContractCertified;
    unsigned native_routes = 0;
    services.show_native = [&](IFileDialog*, HWND) { ++native_routes; return E_UNEXPECTED; };
    ComPtr<IFileDialog> oracle;
    ComPtr<IFileOpenDialog> open;
    ComPtr<IShellItem> initial;
    GUID client{};
    Check(CoCreateGuid(&client) == S_OK && CreateSystemDialog(DialogFactory::Open, &oracle) == S_OK &&
        CreateIsolatedDialog(DialogFactory::Open, services, IID_PPV_ARGS(&open)) == S_OK &&
        SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&initial)) == S_OK,
        "create independent Windows oracle, native-backed wrapper and isolated starting folder");
    if (!oracle || !open || !initial) return;
    std::wstring labels[kCount], patterns[kCount];
    COMDLG_FILTERSPEC filters[kCount]{};
    for (UINT i = 0; i < kCount; ++i) {
        labels[i] = Label(i); patterns[i] = Pattern(i); filters[i] = {labels[i].c_str(), patterns[i].c_str()};
    }
    auto configure = [&](IFileDialog* dialog) {
        return dialog->SetFileTypes(kCount, filters) == S_OK && dialog->SetTitle(L"Fixture import") == S_OK &&
            dialog->SetClientGuid(client) == S_OK && dialog->SetOptions(kOptions) == S_OK &&
            dialog->SetFileTypeIndex(kSelected) == S_OK && dialog->SetOkButtonLabel(L"Import") == S_OK &&
            dialog->SetFolder(initial.Get()) == S_OK;
    };
    const bool configured = configure(oracle.Get()) && configure(open.Get());
    Check(configured, "native and wrapper accept all six ordered setters plus the isolated fixture folder");
    if (!configured) return;
    FILEOPENDIALOGOPTIONS native_options = 0, wrapper_options = 0;
    UINT reference = 0, before = 0, during = 0, after = 0;
    Check(oracle->GetOptions(&native_options) == S_OK && open->GetOptions(&wrapper_options) == S_OK &&
        native_options == kOptions && wrapper_options == native_options && oracle->GetFileTypeIndex(&reference) == S_OK &&
        open->GetFileTypeIndex(&before) == S_OK && reference == kSelected && before == reference,
        "native and wrapper independently report options 0x1A48 and one-based default 55");
    HANDLE second = CreateFileW(state.second.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(second != INVALID_HANDLE_VALUE, "create distinct second fixture");
    if (second == INVALID_HANDLE_VALUE) return;
    CloseHandle(second);
    show_action = [&] { Check(open->GetFileTypeIndex(&during) == S_OK && during == reference,
        "active COM getter returns 55 from the provider snapshot"); };
    const auto shown = open->Show(nullptr);
    show_action = {};
    Check(shown == S_OK && state.shown == 1 && native_routes == 0 &&
        open->GetFileTypeIndex(&after) == S_OK && after == reference,
        "accept preserves selected filter 55 without native fallback");
    ComPtr<IShellItemArray> results;
    DWORD count = 0;
    bool exact = open->GetResults(&results) == S_OK && results && results->GetCount(&count) == S_OK && count == 2;
    for (DWORD i = 0; exact && i < count; ++i) {
        ComPtr<IShellItem> item; PWSTR path = nullptr;
        exact = results->GetItemAt(i, &item) == S_OK && item && item->GetDisplayName(SIGDN_FILESYSPATH, &path) == S_OK &&
            path && (i == 0 ? state.file : state.second) == path;
        CoTaskMemFree(path);
    }
    Check(exact, "GetResults returns both real Shell paths in selection order");
    std::printf("AE_FILTER_CONTRACT synthetic=1 options=0x%lX filters=%u native=%u before=%u active=%u after=%u "
        "paths=%lu native_routes=%u hr=%08lX\n", wrapper_options, kCount, reference, before, during, after,
        count, native_routes, static_cast<unsigned long>(shown));
    Check(DeleteFileW(state.second.c_str()) != FALSE, "remove only the second fixture");
}
}

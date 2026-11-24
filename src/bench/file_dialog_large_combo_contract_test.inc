namespace large_combo_contract {
constexpr DWORD kCombo = 701;
unsigned count = 0;
DWORD Id(unsigned index) { return 4099u + 13u * index; }
std::wstring Label(unsigned index) { return L"Choice " + std::to_wstring(index + 1); }

HRESULT WINAPI ValidateList(PulsePickerUiV2Session session, const PulsePickerUiV2Config* config) {
    if (config->control_count != 1 || !config->controls) return E_UNEXPECTED;
    const auto& control = config->controls[0];
    bool exact = control.id == kCombo && control.kind == PULSE_PICKER_UI_V2_CONTROL_COMBO &&
        control.items && control.item_count == count && control.has_selected && control.selected_item_id == Id(0);
    for (unsigned i = 0; exact && i < count; ++i) {
        const auto& item = control.items[i];
        exact = item.id == Id(i) && item.state == 3 && item.label.data &&
            std::wstring_view(item.label.data, item.label.units) == Label(i);
    }
    Check(exact, "provider receives every combo ID, label and state in original order");
    return exact ? Validate(session, config) : E_UNEXPECTED;
}

bool Matches(IFileDialogCustomize* actual, IFileDialogCustomize* oracle, DWORD expected) {
    DWORD value = 0, reference = 0;
    const HRESULT hr = actual->GetSelectedControlItem(kCombo, &value);
    const HRESULT native_hr = oracle->GetSelectedControlItem(kCombo, &reference);
    std::printf("COMBO_SELECTION count=%u hr=%08lX native_hr=%08lX id=%lu native_id=%lu expected=%lu\n",
        count, static_cast<unsigned long>(hr), static_cast<unsigned long>(native_hr), value, reference, expected);
    return native_hr == S_OK && hr == native_hr && value == reference && value == expected;
}

class Events final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IFileDialogEvents, IFileDialogControlEvents> {
public:
    ComPtr<IFileDialogCustomize> oracle;
    unsigned item_calls = 0, ok_calls = 0;
    HRESULT STDMETHODCALLTYPE OnItemSelected(IFileDialogCustomize* custom, DWORD id, DWORD item) override {
        const DWORD expected = item_calls++ ? Id(count - 1) : Id(count / 2);
        Check(id == kCombo && item == expected && Matches(custom, oracle.Get(), expected),
            "OnItemSelected and its getter return the native item ID, not a row index");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        const DWORD expected = ok_calls++ ? Id(count - 1) : Id(count / 2);
        ComPtr<IFileDialogCustomize> custom; dialog->QueryInterface(IID_PPV_ARGS(&custom));
        Check(custom && Matches(custom.Get(), oracle.Get(), expected), "FileOk observes current native-matched combo ID");
        return ok_calls == 1 ? S_FALSE : S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnButtonClicked(IFileDialogCustomize*, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnControlActivating(IFileDialogCustomize*, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnCheckButtonToggled(IFileDialogCustomize*, DWORD, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* out) override { *out = FDESVR_DEFAULT; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* out) override { *out = FDEOR_DEFAULT; return S_OK; }
};

void Case(const std::wstring& folder, unsigned items, bool extended) {
    count = items;
    Script state; script = &state; state.folder = folder; state.file = folder + L"\\owned.txt";
    auto provider = std::make_shared<FileDialogInprocessProvider>();
    provider->module_lifetime = std::make_shared<int>(1); provider->allow_unverified_proof = true;
    if (extended) provider->max_control_items = 128;
    auto& api = provider->api; api.size = sizeof(api); api.version = 2;
    api.Initialize = Initialize; api.ValidateConfig = ValidateList; api.Show = Show; api.Query = Query;
    api.GetWindow = Window; api.Close = Close; api.Shutdown = Shutdown; api.UpdateControl = UpdateControl;
    api.implemented_features = PULSE_PICKER_UI_V2_FEATURE_CUSTOM_COMBOBOX;
    const bool takes_over = count <= (extended ? 128u : 32u);
    const DWORD initial = takes_over ? Id(0) : Id(count - 1);
    unsigned native_calls = 0;
    ComPtr<IFileDialog> reference;
    ComPtr<IFileDialogCustomize> oracle;
    Check(CreateSystemDialog(DialogFactory::Open, &reference) == S_OK && reference.As(&oracle) == S_OK,
        "create separate system FileOpenDialog oracle");
    if (!oracle) return;
    FileDialogBridgeServices services; services.inprocess_provider = provider; services.access_policy = AccessPolicy::LocalOpenV1;
    services.validation_verified = [](const ContractDecision& decision) { return RuntimeContractCertified(decision); };
    services.show_native = [&](IFileDialog* native, HWND) {
        ++native_calls;
        ComPtr<IFileDialogCustomize> mirror; native->QueryInterface(IID_PPV_ARGS(&mirror));
        bool complete = mirror && Matches(mirror.Get(), oracle.Get(), initial);
        for (unsigned i = 0; complete && i < count; ++i) {
            CDCONTROLSTATEF actual{}, expected{};
            const HRESULT hr = mirror->GetControlItemState(kCombo, Id(i), &actual);
            const HRESULT oracle_hr = oracle->GetControlItemState(kCombo, Id(i), &expected);
            complete = oracle_hr == S_OK && hr == oracle_hr && actual == expected;
        }
        Check(complete, "fallback receives native mirror with all items and exact last selection");
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    };
    ComPtr<IFileOpenDialog> open; ComPtr<IFileDialogCustomize> custom;
    Check(CreateIsolatedDialog(DialogFactory::Open, services, IID_PPV_ARGS(&open)) == S_OK && open.As(&custom) == S_OK,
        "create native-backed Pulse wrapper");
    if (!custom) return;
    ComPtr<IShellItem> root;
    Check(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&root)) == S_OK &&
        open->SetFolder(root.Get()) == S_OK && open->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
            FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT) == S_OK, "configure isolated existing-file contract");
    bool added = oracle->AddComboBox(kCombo) == S_OK && custom->AddComboBox(kCombo) == S_OK;
    for (unsigned i = 0; i < count; ++i) {
        const auto label = Label(i);
        const HRESULT expected = oracle->AddControlItem(kCombo, Id(i), label.c_str());
        const HRESULT actual = custom->AddControlItem(kCombo, Id(i), label.c_str());
        added = added && expected == S_OK && actual == expected;
    }
    Check(added, "every native AddControlItem HRESULT preserved, including overflow item");
    const HRESULT selection = oracle->SetSelectedControlItem(kCombo, initial);
    Check(selection == S_OK && custom->SetSelectedControlItem(kCombo, initial) == selection &&
        Matches(custom.Get(), oracle.Get(), initial), "initial selected ID/HRESULT match independent native oracle");
    if (extended && (count == 56 || count == 129)) {
        for (DWORD absent = 1; absent <= 4; ++absent) {
            const HRESULT native_hr = oracle->RemoveControlItem(kCombo, absent);
            const HRESULT hr = custom->RemoveControlItem(kCombo, absent);
            Check(native_hr == E_INVALIDARG && hr == native_hr &&
                Matches(custom.Get(), oracle.Get(), initial),
                "missing-item cleanup returns the native error without disturbing selection");
        }
        const DWORD extra = 90001;
        const HRESULT extra_native = oracle->AddControlItem(kCombo, extra, L"Transient");
        const HRESULT extra_hr = custom->AddControlItem(kCombo, extra, L"Transient");
        const HRESULT remove_native = oracle->RemoveControlItem(kCombo, extra);
        const HRESULT remove_hr = custom->RemoveControlItem(kCombo, extra);
        Check(extra_native == S_OK && extra_hr == extra_native && remove_native == S_OK &&
            remove_hr == remove_native, "a valid add/remove following rejected cleanup still matches native");
    }
    auto events = Microsoft::WRL::Make<Events>(); events->oracle = oracle;
    DWORD cookie = 0; Check(open->Advise(events.Get(), &cookie) == S_OK, "advise combo observer");
    const auto choose = [&](DWORD id) {
        Check(oracle->SetSelectedControlItem(kCombo, id) == S_OK, "native oracle selects the corresponding item");
        PulsePickerUiV2Event event{}; event.size = sizeof(event); event.kind = PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED;
        event.show_id = 1; event.generation = state.generation; event.window = state.window;
        event.control_id = kCombo; event.item_id = id;
        Check(navigation_callbacks->callback(navigation_callbacks->context, &event) == S_OK, "provider selection reaches wrapper");
    };
    show_action = [&] { choose(Id(count / 2)); };
    after_veto_action = [&] { choose(Id(count - 1)); };
    const HRESULT shown = open->Show(nullptr);
    show_action = {}; after_veto_action = {};
    std::printf("COMBO_ROUTE items=%u extended=%d initialized=%u provider_shows=%u native_routes=%u item_events=%u file_ok=%u hr=%08lX\n",
        count, extended, state.initialized, state.shown, native_calls, events->item_calls, events->ok_calls, static_cast<unsigned long>(shown));
    if (takes_over) {
        Check(shown == S_OK && state.shown == 1 && !native_calls && events->item_calls == 2 && events->ok_calls == 2 &&
            Matches(custom.Get(), oracle.Get(), Id(count - 1)), "one Show survives veto, selects last item and accepts exact ID");
        ComPtr<IShellItem> result; PWSTR path = nullptr;
        Check(open->GetResult(&result) == S_OK && result && result->GetDisplayName(SIGDN_FILESYSPATH, &path) == S_OK &&
            path && state.file == path, "accepted result is the exact owned file"); CoTaskMemFree(path);
    } else {
        Check(shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) && native_calls == 1 && !state.initialized &&
            !state.shown && !events->item_calls && !events->ok_calls, "unsupported list takes native route before provider initialization");
    }
    open->Unadvise(cookie);
}

void Run(const std::wstring& folder) {
    Case(folder, 32, false);
    Case(folder, 33, false);
    Case(folder, 56, false);
    Case(folder, 56, true);
    Case(folder, 128, true);
    Case(folder, 129, true);
}
}

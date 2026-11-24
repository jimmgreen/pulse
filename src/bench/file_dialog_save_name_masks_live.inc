// Compare the system dialog and the production UI provider using owned windows,
// explicit results and file events. Never click the active user desktop.
namespace save_name_mask_live {
struct Case {
    const wchar_t* pattern;
    const wchar_t* extension;
    const wchar_t* input;
    const wchar_t* expected;
    bool change = false, veto = false, refuse = false;
};
struct Events final : IFileDialogEvents {
    ULONG refs = 1;
    const Case* test = nullptr;
    std::atomic<UINT> selected{0};
    unsigned ok = 0, overwrite = 0;
    std::wstring expected;
    bool candidate_exact = true;
    static std::wstring Path(IShellItem* item) {
        PWSTR path = nullptr;
        if (!item || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return {};
        std::wstring value(path); CoTaskMemFree(path); return value;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_IFileDialogEvents) return E_NOINTERFACE;
        *out = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* d) override {
        ++ok; ComPtr<IShellItem> item;
        candidate_exact &= d->GetResult(&item) == S_OK && Path(item.Get()) == expected;
        return test->veto && ok == 1 ? S_FALSE : S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem* item, FDE_OVERWRITE_RESPONSE* response) override {
        ++overwrite; candidate_exact &= Path(item) == expected;
        *response = test->refuse ? FDEOR_REFUSE : FDEOR_ACCEPT; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog* d) override {
        UINT i = 0; if (d->GetFileTypeIndex(&i) == S_OK) selected.store(i); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* out) override {
        *out = FDESVR_DEFAULT; return S_OK;
    }
};
std::atomic<UINT> changed_filter{1};
#include "file_dialog_filter_keyboard_test.inc"
struct Driver {
    IFileDialog* d;
    Events* events;
    HWND owner;
    bool native;
    bool initiated = false;
    unsigned submissions = 0;
    std::thread menu;
};
thread_local Driver* active = nullptr;
void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    Driver& s = *active;
    ComPtr<IOleWindow> ole; HWND w = nullptr; DWORD pid = 0;
    if (FAILED(s.d->QueryInterface(IID_PPV_ARGS(&ole))) || FAILED(ole->GetWindow(&w)) || !w ||
        GetWindowThreadProcessId(w, &pid) != GetCurrentThreadId() || pid != GetCurrentProcessId() ||
        !IsWindowVisible(w) || !IsWindowEnabled(w)) return;
    auto& events = *s.events;
    if (events.test->change && !s.initiated) {
        s.initiated = true;
        if (s.native) {
            HWND combo = nullptr;
            EnumChildWindows(w, [](HWND c, LPARAM arg) -> BOOL {
                wchar_t cl[32]{}; GetClassNameW(c, cl, 32);
                if (_wcsicmp(cl, L"ComboBox") || SendMessageW(c, CB_GETCOUNT, 0, 0) != 2) return TRUE;
                *reinterpret_cast<HWND*>(arg) = c; return FALSE;
            }, reinterpret_cast<LPARAM>(&combo));
            if (combo) {
                SendMessageW(combo, CB_SHOWDROPDOWN, TRUE, 0); SendMessageW(combo, CB_SETCURSEL, 1, 0);
                SendMessageW(GetParent(combo), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(combo), CBN_SELENDOK), reinterpret_cast<LPARAM>(combo));
                SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
            }
        } else {
            const HWND edit = GetDlgItem(w, 102);
            const DWORD thread = GetCurrentThreadId();
            s.menu = std::thread([=] {
                if (SetThreadDesktop(GetThreadDesktop(thread)) && edit) StepFileType(w, edit, thread, pid, VK_DOWN, 2);
            });
        }
        return;
    }
    if (events.test->change) {
        UINT i = 0; if (s.d->GetFileTypeIndex(&i) != S_OK || i != 2) return;
        changed_filter.store(i);
    }
    if (events.test->refuse && events.overwrite) { PostMessageW(w, WM_COMMAND, IDCANCEL, 0); return; }
    if (!s.submissions || (events.test->veto && s.submissions == 1 && events.ok == 1)) {
        ++s.submissions; PostMessageW(w, WM_COMMAND, IDOK, 0);
    }
}
void Run(const wchar_t* dll, const std::wstring& folder) {
    const bool native = dll == nullptr;
    auto provider = std::make_shared<FileDialogInprocessProvider>();
    if (!native) {
        HMODULE mod = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        auto get = mod ? reinterpret_cast<decltype(&PulsePickerUiGetApiV2)>(GetProcAddress(mod, "PulsePickerUiGetApiV2")) : nullptr;
        Check(get && get(2, sizeof(provider->api), &provider->api) == S_OK, "load explicit production V2 provider");
        if (!get || !provider->api.Show) return;
        provider->module_lifetime = std::shared_ptr<void>(mod, [](void* value) { FreeLibrary(static_cast<HMODULE>(value)); });
        provider->allow_unverified_proof = true;
    }
    ComPtr<IShellItem> initial;
    if (FAILED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&initial)))) { Check(false, "fixture folder"); return; }
    HWND owner = CreateWindowW(L"Static", L"Owned Save mask test", WS_OVERLAPPED, 0, 0, 320, 180, nullptr, nullptr, nullptr, nullptr);
    if (!owner) { Check(false, "create owner"); return; }
    const Case cases[] = {
        {L"*", nullptr, L"new", L"new"}, {L"*", L"bak", L"new", L"new.bak"},
        {L"Makefile", nullptr, L"Makefile", L"Makefile"}, {L"Makefile", L"bak", L"new", L"new.bak"},
        {L"CMakeLists.txt", nullptr, L"new", L"new"}, {L"CMakeLists.txt", L"bak", L"new", L"new.txt"},
        {L"report-*.csv", nullptr, L"report-1.csv", L"report-1.csv"},
        {L"report-*.csv", L"bak", L"new.pulseunregistered", L"new.pulseunregistered.csv"},
        {L"*.txt;Makefile", nullptr, L"Makefile", L"Makefile"}, {L"*.txt;Makefile", L"bak", L"new", L"new.txt", false, true},
        {L"Makefile;*.txt", L"bak", L"new", L"new.txt"}, {L"report-*.csv", L"bak", L"new.txt", L"new.csv", true},
        {L"Makefile", L"bak", L"new.txt", L"new.bak", true}, {L"*", L"bak", L"new", L"new.prior", true},
        {L"report-*.csv", nullptr, L"new", L"new", true}, {L"*.txt;Makefile", L"bak", L"owned", L"owned.txt", false, false, true}
    };
    unsigned replay = 0;
    for (const auto& test : cases) {
        FileDialogBridgeServices services;
        services.on_native_reason = [](const char* reason, const IID*) {
            if (std::string_view(reason).starts_with("inprocess-api-show-return")) std::printf("PROVIDER %s\n", reason);
        };
        services.save_access_policy = AccessPolicy::LocalSaveV1; services.inprocess_provider = provider;
        services.validation_verified = RuntimeLocalSaveContractCertified;
        services.client_state = std::make_shared<FileDialogClientState>(L"", L"");
        services.show_native = [&](IFileDialog*, HWND) { ++replay; return E_UNEXPECTED; };
        ComPtr<IFileDialog> d;
        const HRESULT created = native ? CreateSystemDialog(DialogFactory::Save, &d) :
            CreateIsolatedDialog(DialogFactory::Save, services, IID_PPV_ARGS(&d));
        if (FAILED(created)) { Check(false, "create Save"); continue; }
        if (native) {
            constexpr GUID own = {0x948b3c0c,0x3961,0x4b18,{0xb9,0x1d,0x2a,0x12,0xef,0x0c,0x68,0x5e}};
            if (FAILED(d->SetClientGuid(own))) { Check(false, "bind test-only native GUID"); continue; }
        }
        COMDLG_FILTERSPEC filters[] = {{L"Prior", L"*.prior"}, {L"Test mask", test.pattern}};
        const bool configured = d->SetOptions(FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN | FOS_PATHMUSTEXIST |
                FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT | FOS_NOCHANGEDIR) == S_OK &&
            d->SetFolder(initial.Get()) == S_OK && d->SetFileTypes(2, filters) == S_OK &&
            d->SetFileTypeIndex(test.change ? 1 : 2) == S_OK && (!test.extension || d->SetDefaultExtension(test.extension) == S_OK) &&
            d->SetFileName(test.input) == S_OK;
        if (!configured) { Check(false, "native setter sequence accepted"); continue; }
        Events events; events.test = &test; events.expected = folder + L"\\" + test.expected;
        DWORD cookie = 0;
        if (FAILED(d->Advise(&events, &cookie))) { Check(false, "subscribe Save events"); continue; }
        Driver state{d.Get(), &events, owner, native, false, 0, {}}; active = &state; changed_filter.store(1);
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread watchdog([done] { if (WaitForSingleObject(done, 12000) != WAIT_OBJECT_0) {
            std::printf("[FAIL] Save mask driver timeout\n"); TerminateProcess(GetCurrentProcess(), 99);
        }});
        std::printf("BEGIN %ls default=%ls input=%ls change=%d\n", test.pattern, test.extension ? test.extension : L"(absent)", test.input, test.change);
        const auto timer = SetTimer(nullptr, 0, 150, Tick);
        const HRESULT show = timer ? d->Show(owner) : E_FAIL;
        KillTimer(nullptr, timer); active = nullptr;
        if (state.menu.joinable()) state.menu.join();
        SetEvent(done); watchdog.join(); CloseHandle(done);
        UINT index = 0; ComPtr<IShellItem> result;
        const auto got = d->GetResult(&result); const auto path = Events::Path(result.Get());
        const bool index_ok = d->GetFileTypeIndex(&index) == S_OK && index == 2;
        std::printf("RESULT native=%d mask=%ls hr=%08lX get=%08lX index=%u ok=%u overwrite=%u path=%ls replay=%u\n",
            native, test.pattern, show, got, index, events.ok, events.overwrite, path.c_str(), replay);
        Check(events.candidate_exact && !replay && index_ok && (test.refuse ?
            show == HRESULT_FROM_WIN32(ERROR_CANCELLED) && FAILED(got) && !result && events.overwrite == 1 :
            show == S_OK && got == S_OK && path == events.expected && events.ok == (test.veto ? 2u : 1u) &&
            GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES), "exact Save path/index/veto outcome with owned fixture");
        d->Unadvise(cookie);
        if (native) std::printf("test GUID ClearClientData=%08lX\n", d->ClearClientData());
    }
    DestroyWindow(owner);
}
}

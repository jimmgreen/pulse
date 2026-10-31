#include <windows.h>
#include <shobjidl.h>
#include <propsys.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <cstdio>
#include <thread>
#include <string_view>
#include <filesystem>
#include <string>
#include <atomic>

// Native COM observation only: no Show, files, property values, client GUID or
// saved-dialog preference cleanup. A successful setter is not sufficient proof
// that a usable empty IPropertyStore exists.
namespace {
using Microsoft::WRL::ComPtr;
unsigned checks = 0, failures = 0, cleanup_failures = 0;
void Check(bool value, const char* label, bool cleanup = false) {
    ++checks; if (!value) { ++failures; if (cleanup) ++cleanup_failures; }
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    std::fflush(stdout);
}
struct StoreObservation {
    HRESULT result = E_PENDING, counted = E_PENDING;
    DWORD count = MAXDWORD;
    bool present = false;
};
StoreObservation Observe(IFileSaveDialog* dialog, const char* stage) {
    StoreObservation result;
    ComPtr<IPropertyStore> store;
    result.result = dialog->GetProperties(&store);
    result.present = !!store;
    if (result.result == S_OK && store) result.counted = store->GetCount(&result.count);
    std::printf("NATIVE stage=%s get_hr=0x%08lX store=%u count_hr=0x%08lX count=%lu\n", stage,
        static_cast<unsigned long>(result.result), result.present ? 1u : 0u,
        static_cast<unsigned long>(result.counted), result.count);
    std::fflush(stdout);
    Check(SUCCEEDED(result.result) || !result.present, "failed GetProperties leaves no output store");
    return result;
}
void Case(IPropertyDescriptionList* list, const char* name) {
    ComPtr<IFileSaveDialog> dialog;
    const HRESULT created = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    std::printf("NATIVE case=%s create_hr=0x%08lX\n", name, static_cast<unsigned long>(created));
    Check(created == S_OK && dialog, "create actual native FileSaveDialog");
    if (!dialog) return;
    Observe(dialog.Get(), "before-collection"); // Report rather than invent an exact baseline HRESULT.
    const HRESULT configured = dialog->SetCollectedProperties(list, FALSE);
    std::printf("NATIVE case=%s set_hr=0x%08lX append=0 list=%u\n", name,
        static_cast<unsigned long>(configured), list ? 1u : 0u);
    Check(configured == S_OK, "native empty-only collection configuration succeeds");
    const auto first = Observe(dialog.Get(), "after-empty-collection");
    Check(first.result == S_OK && first.present && first.counted == S_OK && first.count == 0,
        "native configured empty collection supplies a real empty store before Show");
    const auto second = Observe(dialog.Get(), "repeat-empty-getter");
    Check(second.result == first.result && second.present == first.present && second.counted == first.counted && second.count == first.count,
        "repeated native getter preserves observed status and count");
    // No assertion about post-Show behavior is derived from this no-UI probe.
}
void Schema(IPropertyDescription* description, const char* source) {
    PROPERTYKEY key{}; VARTYPE type = VT_EMPTY;
    PROPDESC_TYPE_FLAGS flags{}; PROPDESC_VIEW_FLAGS view{}; PROPDESC_DISPLAYTYPE display{};
    const HRESULT keyed = description->GetPropertyKey(&key);
    const HRESULT typed = description->GetPropertyType(&type);
    const HRESULT flagged = description->GetTypeFlags(PDTF_MASK_ALL, &flags);
    const HRESULT viewed = description->GetViewFlags(&view);
    const HRESULT displayed = description->GetDisplayType(&display);
    wchar_t wide_guid[40]{}; char guid[40]{};
    if (keyed == S_OK && StringFromGUID2(key.fmtid, wide_guid, 40))
        for (size_t i = 0; i < 39 && wide_guid[i]; ++i) guid[i] = static_cast<char>(wide_guid[i]);
    PWSTR name = nullptr; const HRESULT named = description->GetCanonicalName(&name);
    char ascii_name[160]{};
    if (named == S_OK && name) for (size_t i = 0; i < 159 && name[i]; ++i) ascii_name[i] = name[i] >= 32 && name[i] <= 126 ? static_cast<char>(name[i]) : '?';
    std::printf("SCHEMA source=%s name_hr=0x%08lX name=%s key_hr=0x%08lX fmtid=%s pid=%lu\n", source,
        static_cast<unsigned long>(named), ascii_name, static_cast<unsigned long>(keyed), guid, key.pid);
    std::printf("SCHEMA source=%s type_hr=0x%08lX vt=0x%04X flags_hr=0x%08lX flags=0x%08lX innate_readonly=%u multiple=%u view_hr=0x%08lX view=0x%08lX display_hr=0x%08lX display=%u\n",
        source, static_cast<unsigned long>(typed), static_cast<unsigned>(type), static_cast<unsigned long>(flagged), static_cast<unsigned long>(flags),
        (flags & PDTF_ISINNATE) ? 1u : 0u, (flags & PDTF_MULTIPLEVALUES) ? 1u : 0u,
        static_cast<unsigned long>(viewed), static_cast<unsigned long>(view), static_cast<unsigned long>(displayed), static_cast<unsigned>(display));
    Check(named == S_OK && name && keyed == S_OK && typed == S_OK && flagged == S_OK && viewed == S_OK && displayed == S_OK,
        "system property schema queried without reading any value");
    CoTaskMemFree(name);
}
void EncryptionCandidate(IPropertySystem* system) {
    ComPtr<IPropertyDescriptionList> list;
    const HRESULT parsed = system->GetPropertyDescriptionListFromString(L"prop:System.Security.EncryptionOwners", IID_PPV_ARGS(&list));
    UINT count = 0xFFFFFFFFu; const HRESULT counted = parsed == S_OK && list ? list->GetCount(&count) : E_PENDING;
    std::printf("CANDIDATE parse_hr=0x%08lX count_hr=0x%08lX count=%u runtime_descriptor_match=unverified\n",
        static_cast<unsigned long>(parsed), static_cast<unsigned long>(counted), count);
    Check(parsed == S_OK && list && counted == S_OK && count == 1, "static candidate resolves to one actual Windows property description");
    if (parsed != S_OK || !list || counted != S_OK || count != 1) return;
    ComPtr<IPropertyDescription> in_list, standalone;
    const HRESULT at = list->GetAt(0, IID_PPV_ARGS(&in_list));
    Check(at == S_OK && in_list, "retrieve actual list descriptor");
    if (in_list) Schema(in_list.Get(), "list");
    const HRESULT direct = system->GetPropertyDescriptionByName(L"System.Security.EncryptionOwners", IID_PPV_ARGS(&standalone));
    Check(direct == S_OK && standalone, "retrieve standalone schema for list-influenced flag comparison");
    if (standalone) Schema(standalone.Get(), "standalone");
    ComPtr<IFileSaveDialog> dialog;
    const HRESULT created = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    Check(created == S_OK && dialog, "create native Save for candidate schema observation");
    if (!dialog) return;
    Observe(dialog.Get(), "candidate-before-collection");
    const HRESULT configured = dialog->SetCollectedProperties(list.Get(), FALSE);
    std::printf("CANDIDATE set_hr=0x%08lX append=0\n", static_cast<unsigned long>(configured));
    Check(configured == S_OK, "native Save accepts exact candidate collection descriptor");
    Observe(dialog.Get(), "candidate-after-collection");
    Observe(dialog.Get(), "candidate-repeat-getter");
    std::printf("CANDIDATE no_GetValue=1 no_SetProperties=1 no_ApplyProperties=1 no_file=1 no_Show=1 editable_UI=unobserved metadata_support=unverified\n");
}

void StoreSchema(IFileSaveDialog* dialog, IPropertySystem* system, const char* stage) {
    ComPtr<IPropertyStore> store;
    const HRESULT got = dialog->GetProperties(&store);
    DWORD count = 0; const HRESULT counted = got == S_OK && store ? store->GetCount(&count) : E_PENDING;
    std::printf("STORE stage=%s hr=0x%08lX present=%u count_hr=0x%08lX count=%lu no_GetValue=1\n", stage,
        static_cast<unsigned long>(got), store ? 1u : 0u, static_cast<unsigned long>(counted), count);
    Check(got == S_OK && store && counted == S_OK, "accepted native Save exposes actual property store");
    if (counted != S_OK) return;
    Check(count <= 16, "property store key enumeration remains bounded");
    for (DWORD i = 0; i < count && i < 16; ++i) {
        PROPERTYKEY key{}; const HRESULT at = store->GetAt(i, &key);
        wchar_t wide[40]{}; char guid[40]{};
        if (at == S_OK && StringFromGUID2(key.fmtid, wide, 40)) for (size_t n = 0; n < 39 && wide[n]; ++n) guid[n] = static_cast<char>(wide[n]);
        ComPtr<IPropertyDescription> description;
        const HRESULT described = at == S_OK ? system->GetPropertyDescription(key, IID_PPV_ARGS(&description)) : E_PENDING;
        VARTYPE type = VT_EMPTY; const HRESULT typed = described == S_OK && description ? description->GetPropertyType(&type) : E_PENDING;
        std::printf("STOREKEY stage=%s index=%lu at_hr=0x%08lX fmtid=%s pid=%lu schema_hr=0x%08lX schema_type_hr=0x%08lX schema_vt=0x%04X actual_value_type=not_read\n",
            stage, i, static_cast<unsigned long>(at), guid, key.pid, static_cast<unsigned long>(described), static_cast<unsigned long>(typed), static_cast<unsigned>(type));
        Check(at == S_OK && described == S_OK && typed == S_OK, "enumerate schema key and declared type without property values");
    }
}
struct SaveRun {
    IFileSaveDialog* dialog = nullptr;
    IPropertySystem* system = nullptr;
    HWND owner = nullptr, window = nullptr;
    DWORD thread = 0;
    ULONGLONG start = 0;
    bool submitted = false, timed_out = false;
    unsigned file_ok = 0;
    std::wstring path;
};
SaveRun* running = nullptr;
void CALLBACK SaveTimer(HWND, UINT, UINT_PTR, DWORD) {
    if (!running || running->timed_out) return;
    auto& run = *running;
    ComPtr<IOleWindow> ole; HWND window = nullptr;
    if (FAILED(run.dialog->QueryInterface(IID_PPV_ARGS(&ole))) || FAILED(ole->GetWindow(&window)) || !window) return;
    DWORD pid = 0;
    if (GetWindowThreadProcessId(window, &pid) != run.thread || pid != GetCurrentProcessId() ||
        GetWindow(window, GW_OWNER) != run.owner || !IsWindowVisible(window)) return;
    run.window = window;
    if (GetTickCount64() - run.start >= 15000) {
        run.timed_out = true;
        std::printf("DRIVER timeout=1 cancel_posted=%u\n", PostMessageW(window, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0) ? 1u : 0u);
        std::fflush(stdout); return;
    }
    HWND button = GetDlgItem(window, IDOK);
    if (!run.submitted && IsWindowEnabled(window) && button && IsChild(window, button) && IsWindowVisible(button) && IsWindowEnabled(button)) {
        run.submitted = PostMessageW(window, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), reinterpret_cast<LPARAM>(button)) != FALSE;
        std::printf("DRIVER own_native_window=1 submitted=%u physical_input=0\n", run.submitted ? 1u : 0u); std::fflush(stdout);
    }
}
class SaveEvents final : public IFileDialogEvents {
public:
    explicit SaveEvents(SaveRun& run) : run_(run) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IFileDialogEvents) return E_NOINTERFACE;
        *out = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto value = --refs_; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        ++run_.file_ok;
        Check(GetCurrentThreadId() == run_.thread, "actual native FileOk executes on Show STA");
        ComPtr<IShellItem> item; PWSTR path = nullptr; const HRESULT got = dialog->GetResult(&item);
        const HRESULT named = got == S_OK && item ? item->GetDisplayName(SIGDN_FILESYSPATH, &path) : E_PENDING;
        const bool exact = named == S_OK && path && _wcsicmp(path, run_.path.c_str()) == 0;
        Check(exact, "FileOk candidate is exact owned nonexistent target"); CoTaskMemFree(path);
        const DWORD attributes = GetFileAttributesW(run_.path.c_str()); const DWORD error = GetLastError();
        Check(attributes == INVALID_FILE_ATTRIBUTES && error == ERROR_FILE_NOT_FOUND, "native Save has not created target inside FileOk");
        StoreSchema(run_.dialog, run_.system, "FileOk");
        return exact ? S_OK : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* response) override { if (response) *response = FDESVR_DEFAULT; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* response) override { if (response) *response = FDEOR_REFUSE; Check(false, "new owned target never requires overwrite"); return S_OK; }
private:
    std::atomic<ULONG> refs_{1}; SaveRun& run_;
};
void EncryptionSave(IPropertySystem* system) {
    GUID guid{}; Check(CoCreateGuid(&guid) == S_OK, "create unique native Save fixture identifier");
    wchar_t token[40]{}; StringFromGUID2(guid, token, 40);
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") / (std::wstring(L"native-property-save-") + token));
    std::filesystem::create_directories(root.parent_path());
    const bool created_root = CreateDirectoryW(root.c_str(), nullptr) != FALSE;
    Check(created_root, "create exclusive owned fixture directory");
    if (!created_root) return;
    const DWORD attributes = GetFileAttributesW(root.c_str());
    const bool plain = attributes != INVALID_FILE_ATTRIBUTES && !(attributes & (FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_REPARSE_POINT));
    Check(plain, "owned test directory is neither encrypted nor a reparse point");
    std::printf("FIXTURE "); for (wchar_t c : root.wstring()) std::printf("\\u%04X", static_cast<unsigned>(c)); std::printf("\n"); std::fflush(stdout);
    if (!plain) { Check(RemoveDirectoryW(root.c_str()) != FALSE, "cleanup unsupported owned directory", true); return; }
    SaveRun run; run.system = system; run.thread = GetCurrentThreadId(); run.path = (root / L"new.txt").wstring();
    run.owner = CreateWindowExW(0, L"STATIC", L"Pulse native property Save oracle", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 480, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(run.owner != nullptr, "create owned native oracle owner");
    ComPtr<IFileSaveDialog> dialog; HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    ComPtr<IPropertyDescriptionList> list;
    if (hr == S_OK) hr = system->GetPropertyDescriptionListFromString(L"prop:System.Security.EncryptionOwners", IID_PPV_ARGS(&list));
    if (hr == S_OK) hr = dialog->SetCollectedProperties(list.Get(), FALSE);
    if (hr == S_OK) hr = dialog->SetOptions(static_cast<FILEOPENDIALOGOPTIONS>(0x0200884A));
    if (hr == S_OK) hr = dialog->SetClientGuid(guid);
    ComPtr<IShellItem> folder;
    if (hr == S_OK) hr = SHCreateItemFromParsingName(root.c_str(), nullptr, IID_PPV_ARGS(&folder));
    if (hr == S_OK) hr = dialog->SetFolder(folder.Get());
    if (hr == S_OK) hr = dialog->SetFileName(L"new.txt");
    if (hr == S_OK) hr = dialog->SetDefaultExtension(L"txt");
    const COMDLG_FILTERSPEC filters[]{{L"Text", L"*.txt"}};
    if (hr == S_OK) hr = dialog->SetFileTypes(1, filters);
    run.dialog = dialog.Get(); DWORD cookie = 0; ComPtr<IFileDialogEvents> sink; sink.Attach(new SaveEvents(run));
    if (hr == S_OK) hr = dialog->Advise(sink.Get(), &cookie);
    Check(hr == S_OK, "configure exact native Save collection and options contract");
    if (hr == S_OK && run.owner) {
        FILEOPENDIALOGOPTIONS actual{}; Check(dialog->GetOptions(&actual) == S_OK && actual == 0x0200884A, "native effective options are exactly 0x0200884A");
        running = &run; run.start = GetTickCount64(); const UINT_PTR timer = SetTimer(run.owner, 1, 100, SaveTimer);
        Check(timer != 0, "bounded own-window driver timer installed");
        const HRESULT shown = timer ? dialog->Show(run.owner) : E_FAIL;
        if (timer) KillTimer(run.owner, timer); running = nullptr;
        std::printf("SHOW hr=0x%08lX submitted=%u FileOk=%u timeout=%u\n", static_cast<unsigned long>(shown), run.submitted ? 1u : 0u, run.file_ok, run.timed_out ? 1u : 0u);
        Check(shown == S_OK && run.submitted && run.file_ok == 1 && !run.timed_out, "actual native Save accepts owned nonexistent txt target");
        if (shown == S_OK) {
            ComPtr<IShellItem> item; PWSTR path = nullptr; const HRESULT got = dialog->GetResult(&item);
            const HRESULT named = got == S_OK && item ? item->GetDisplayName(SIGDN_FILESYSPATH, &path) : E_PENDING;
            Check(named == S_OK && path && _wcsicmp(path, run.path.c_str()) == 0, "post-Show result remains exact target"); CoTaskMemFree(path);
            StoreSchema(dialog.Get(), system, "post-Show");
        }
        Check(IsWindowEnabled(run.owner) != FALSE, "native Show restores owned owner enable state");
    }
    if (dialog && cookie) Check(dialog->Unadvise(cookie) == S_OK, "unadvise owned native event sink");
    if (dialog) { const HRESULT cleaned = dialog->ClearClientData(); std::printf("CLEANUP ClearClientData=0x%08lX\n", static_cast<unsigned long>(cleaned)); Check(cleaned == S_OK, "cleanup unique client GUID data (failure retained)", true); }
    sink.Reset(); dialog.Reset(); folder.Reset(); list.Reset();
    if (run.owner) Check(DestroyWindow(run.owner) != FALSE, "destroy owned oracle owner");
    const DWORD target_attributes = GetFileAttributesW(run.path.c_str()); const DWORD target_error = GetLastError();
    Check(target_attributes == INVALID_FILE_ATTRIBUTES && target_error == ERROR_FILE_NOT_FOUND, "native Save acceptance did not create the target");
    Check(RemoveDirectoryW(root.c_str()) != FALSE, "remove only empty owned fixture directory", true);
}

}
int wmain(int argc, wchar_t** argv) {
    const bool encryption = argc == 2 && std::wstring_view(argv[1]) == L"--encryption-schema";
    const bool save = argc == 2 && std::wstring_view(argv[1]) == L"--encryption-save";
    if (argc != 1 && !encryption && !save) return 2;
    HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!completed) return 2;
    std::thread watchdog([completed] {
        if (WaitForSingleObject(completed, 20000) == WAIT_TIMEOUT) {
            std::fprintf(stderr, "WATCHDOG native-empty-properties pre-Show COM probe exceeded 20 seconds\n");
            std::fflush(stderr); TerminateProcess(GetCurrentProcess(), 99);
        }
    });
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Check(SUCCEEDED(initialized), "initialize caller STA");
    if (SUCCEEDED(initialized)) {
        ComPtr<IPropertySystem> system;
        const HRESULT created = CoCreateInstance(__uuidof(PropertySystem), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&system));
        Check(created == S_OK && system, "create Windows property system");
        if (save) { if (system) EncryptionSave(system.Get()); }
        else if (encryption) { if (system) EncryptionCandidate(system.Get()); }
        else {
        ComPtr<IPropertyDescriptionList> empty;
        const HRESULT parsed = system ? system->GetPropertyDescriptionListFromString(L"prop:", IID_PPV_ARGS(&empty)) : E_UNEXPECTED;
        UINT count = 0xFFFFFFFFu;
        const HRESULT counted = parsed == S_OK && empty ? empty->GetCount(&count) : E_PENDING;
        std::printf("NATIVE empty_list parse_hr=0x%08lX list=%u count_hr=0x%08lX count=%u\n",
            static_cast<unsigned long>(parsed), empty ? 1u : 0u, static_cast<unsigned long>(counted), count);
        Check(parsed == S_OK && empty && counted == S_OK && count == 0, "Windows prop: list is demonstrably empty");
        if (parsed == S_OK && empty && counted == S_OK && count == 0) Case(empty.Get(), "system-empty-list");
        Case(nullptr, "null-list");
        empty.Reset();
        }
        system.Reset(); CoUninitialize();
    }
    SetEvent(completed); watchdog.join(); CloseHandle(completed);
    std::printf("Checks=%u failures=%u functional_failures=%u cleanup_failures=%u Show_mode=%u metadata_support=unverified\n", checks, failures, failures - cleanup_failures, cleanup_failures, save ? 1u : 0u);
    return failures ? 1 : 0;
}

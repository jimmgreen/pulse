#include "../common/dialog_auto_registration.h"
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <cstdio>
#include <string>

namespace {
DWORD WINAPI Watchdog(void* context) {
    if (WaitForSingleObject(static_cast<HANDLE>(context), 30000) == WAIT_TIMEOUT) {
        std::fputs("[FAIL] activation watchdog expired; isolated test root may remain\n", stderr);
        std::fflush(stderr);
        ExitProcess(124);
    }
    return 0;
}
struct WatchdogScope {
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = stop ? CreateThread(nullptr, 0, Watchdog, stop, 0, nullptr) : nullptr;
    ~WatchdogScope() { if (thread) { SetEvent(stop); WaitForSingleObject(thread, INFINITE); CloseHandle(thread); } if (stop) CloseHandle(stop); }
};
struct Isolation {
    HKEY root = nullptr;
    HKEY classes = nullptr;
    bool user_overridden = false;
    bool classes_overridden = false;
    std::wstring path;
    DWORD Setup(bool override_classes = true) {
        GUID guid{};
        HRESULT hr = CoCreateGuid(&guid);
        if (FAILED(hr)) return static_cast<DWORD>(hr);
        wchar_t id[40]{};
        if (!StringFromGUID2(guid, id, 40)) return ERROR_INVALID_DATA;
        path = std::wstring(L"Software\\PulseDialogAutoActivationTest_") + id;
        DWORD disposition = 0;
        DWORD e = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS | pulse::dialog_auto::RegistryView, nullptr, &root, &disposition);
        if (e) return e;
        if (disposition != REG_CREATED_NEW_KEY) { path.clear(); return ERROR_ALREADY_EXISTS; }
        e = RegCreateKeyExW(root, L"Software\\Classes", 0, nullptr, 0, KEY_ALL_ACCESS | pulse::dialog_auto::RegistryView, nullptr, &classes, nullptr);
        if (e) return e;
        e = RegOverridePredefKey(HKEY_CURRENT_USER, root);
        if (e) return e;
        user_overridden = true;
        if (!override_classes) return ERROR_SUCCESS;
        e = RegOverridePredefKey(HKEY_CLASSES_ROOT, classes);
        if (e) return e;
        classes_overridden = true;
        return ERROR_SUCCESS;
    }
    DWORD Cleanup() {
        DWORD result = ERROR_SUCCESS;
        if (classes_overridden) {
            DWORD e = RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr);
            if (e) return e;
            classes_overridden = false;
        }
        if (user_overridden) {
            DWORD e = RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
            if (e) return e;
            user_overridden = false;
        }
        if (classes) { RegCloseKey(classes); classes = nullptr; }
        if (root) { RegCloseKey(root); root = nullptr; }
        if (!path.empty()) { result = RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); if (!result) path.clear(); }
        return result;
    }
    ~Isolation() { Cleanup(); }
};
struct UiProbe {
    IFileDialog* dialog = nullptr;
    DWORD thread = GetCurrentThreadId();
    bool observed = false;
    bool expired = false;
    ULONGLONG deadline = GetTickCount64() + 15000;
};
thread_local UiProbe* active_ui = nullptr;
BOOL CALLBACK ObserveUi(HWND window, LPARAM parameter) {
    auto& probe = *reinterpret_cast<UiProbe*>(parameter);
    DWORD pid = 0;
    if (GetWindowThreadProcessId(window, &pid) != probe.thread || pid != GetCurrentProcessId()) return TRUE;
    wchar_t name[128]{}; GetClassNameW(window, name, 128);
    if (IsWindowVisible(window) && wcscmp(name, L"PulseFolderPickerWindow") == 0) probe.observed = true;
    if (probe.expired) PostMessageW(window, WM_CLOSE, 0, 0);
    return TRUE;
}
void CALLBACK UiTick(HWND, UINT, UINT_PTR, DWORD) {
    if (!active_ui) return;
    active_ui->expired = GetTickCount64() >= active_ui->deadline;
    EnumThreadWindows(active_ui->thread, ObserveUi, reinterpret_cast<LPARAM>(active_ui));
    if (active_ui->observed || active_ui->expired) active_ui->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
}
template<class T> struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    void** Out() { return reinterpret_cast<void**>(&p); }
};
}

int wmain(int argc, wchar_t** argv) {
    const bool complex_extension = argc == 4 && wcscmp(argv[2], L"--complex-extension-ui") == 0;
    if (argc == 4 && (wcscmp(argv[2], L"--xmind-ui") == 0 || complex_extension)) {
        const FILEOPENDIALOGOPTIONS expected_options = complex_extension ? 0x1040u : 0x1240u;
        const char* scenario = complex_extension ? "complex extension" : "XMind";
        WatchdogScope watchdog;
        if (!watchdog.thread) return 2;
        Isolation isolation;
        DWORD error = isolation.Setup(false);
        if (!error) error = pulse::dialog_auto::Enable(argv[1]);
        if (error) { std::printf("[FAIL] isolated HKCU registration error=%lu\n", error); return 2; }
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(apartment)) return 2;
        HMODULE module = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        using Entry = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
        auto entry = module ? reinterpret_cast<Entry>(GetProcAddress(module, "DllGetClassObject")) : nullptr;
        bool pass = false;
        {
            ComPtr<IClassFactory> factory;
            ComPtr<IFileOpenDialog> dialog;
            ComPtr<IShellItem> folder;
            HRESULT hr = entry ? entry(CLSID_FileOpenDialog, IID_IClassFactory, factory.Out()) : E_FAIL;
            if (SUCCEEDED(hr)) hr = factory.p->CreateInstance(nullptr, IID_IFileOpenDialog, dialog.Out());
            if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(argv[3], nullptr, IID_IShellItem, folder.Out());
            if (SUCCEEDED(hr)) hr = dialog.p->SetOptions(expected_options);
            if (SUCCEEDED(hr)) hr = dialog.p->SetFileName(L"");
            COMDLG_FILTERSPEC filters[]={{L"XMind",L"*.xmind"},{L"All",L"*.*"}};
            if (SUCCEEDED(hr) && complex_extension) hr = dialog.p->SetDefaultExtension(L"*.td;*.bt;*.xl;*.downloading");
            if (SUCCEEDED(hr)) hr = dialog.p->SetFileTypes(complex_extension ? 1u : 2u, complex_extension ? filters + 1 : filters);
            if (SUCCEEDED(hr)) hr = dialog.p->SetFileTypeIndex(1);
            if (SUCCEEDED(hr)) hr = dialog.p->SetFolder(folder.p);
            FILEOPENDIALOGOPTIONS actual{};
            if (SUCCEEDED(hr)) hr = dialog.p->GetOptions(&actual);
            if (SUCCEEDED(hr) && actual == expected_options) {
                UiProbe probe; probe.dialog = dialog.p; active_ui = &probe;
                const auto timer = SetTimer(nullptr, 0, 50, UiTick);
                if (timer) {
                    hr = dialog.p->Show(nullptr);
                    KillTimer(nullptr, timer);
                    ComPtr<IShellItem> selected;
                    pass = probe.observed && !probe.expired && hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) && FAILED(dialog.p->GetResult(&selected.p));
                }
                active_ui = nullptr;
                std::printf("[%s] actual DLL direct %s UI (not registry activation) visible=%u expired=%u hr=%08lx\n", pass?"PASS":"FAIL", scenario, probe.observed, probe.expired, static_cast<unsigned long>(hr));
            } else std::printf("[FAIL] %s configuration hr=%08lx options=%08lx\n", scenario, static_cast<unsigned long>(hr), static_cast<unsigned long>(actual));
        }
        error = pulse::dialog_auto::Disable();
        if (error) pass = false;
        if (module) FreeLibrary(module);
        CoUninitialize();
        error = isolation.Cleanup();
        if (error) pass = false;
        return pass?0:1;
    }
    if (argc == 3 && wcscmp(argv[2], L"--direct-factory") == 0) {
        WatchdogScope watchdog;
        if (!watchdog.thread) return 2;
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(apartment)) return 2;
        HMODULE module = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        using Entry = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
        auto entry = module ? reinterpret_cast<Entry>(GetProcAddress(module, "DllGetClassObject")) : nullptr;
        int failures = entry ? 0 : 1;
        for (const auto* clsid : {&CLSID_FileOpenDialog, &CLSID_FileSaveDialog}) {
            ComPtr<IClassFactory> factory;
            ComPtr<IFileDialog> dialog;
            HRESULT hr = entry ? entry(*clsid, IID_IClassFactory, factory.Out()) : E_FAIL;
            if (SUCCEEDED(hr) && factory.p) hr = factory.p->CreateInstance(nullptr, IID_IFileDialog, dialog.Out());
            FILEOPENDIALOGOPTIONS options{};
            if (SUCCEEDED(hr) && dialog.p) hr = dialog.p->GetOptions(&options);
            const bool pass = SUCCEEDED(hr) && factory.p && dialog.p;
            if (!pass) ++failures;
            std::printf("[%s] direct DLL factory (not COM registry activation) hr=%08lx\n", pass ? "PASS" : "FAIL", static_cast<unsigned long>(hr));
        }
        if (module) FreeLibrary(module);
        CoUninitialize();
        return failures ? 1 : 0;
    }
    if (argc == 3 && wcscmp(argv[2], L"--registered") == 0) {
        WatchdogScope watchdog;
        const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(apartment)) return 2;
        int failures = 0;
        {
            ComPtr<IFileOpenDialog> open;
            ComPtr<IFileSaveDialog> save;
            const auto a = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, open.Out());
            const bool loaded_open = GetModuleHandleW(argv[1]) != nullptr;
            const auto b = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileSaveDialog, save.Out());
            const bool loaded_save = GetModuleHandleW(argv[1]) != nullptr;
            failures = FAILED(a) || !open.p || !loaded_open || FAILED(b) || !save.p || !loaded_save;
            std::printf("[%s] real-user COM entry open=%08lx save=%08lx loaded_open=%u loaded_save=%u\n",
                failures ? "FAIL" : "PASS", static_cast<unsigned long>(a), static_cast<unsigned long>(b), loaded_open, loaded_save);
        }
        CoUninitialize();
        return failures ? 1 : 0;
    }
    if (argc != 2) { std::puts("Usage: pulse_dialog_auto_com_activation_test <absolute native PulseDialogAuto DLL>"); return 2; }
    WatchdogScope watchdog;
    if (!watchdog.thread) { std::puts("[FAIL] watchdog initialization"); return 2; }
    int failures = 0;
    auto check = [&](bool pass, const char* name, unsigned long error = 0) {
        std::printf("[%s] %s (0x%08lX)\n", pass ? "PASS" : "FAIL", name, error);
        std::fflush(stdout);
        if (!pass) ++failures;
    };
    // This executable must not link or explicitly load the shim: module arrival is
    // evidence that COM activation of the real dialog CLSID selected the test registration.
    check(GetModuleHandleW(argv[1]) == nullptr, "shim initially absent");
    if (failures) return 1;
    Isolation isolation;
    DWORD error = isolation.Setup();
    check(error == ERROR_SUCCESS, "isolated HKCU and HKCR", error);
    if (error) return 1;
    error = pulse::dialog_auto::Enable(argv[1]);
    check(error == ERROR_SUCCESS, "register real open and save CLSIDs in isolated hive", error);
    if (error) return 1;
    for (const auto* key : {L"CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}\\InprocServer32", L"Wow6432Node\\CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}\\InprocServer32"}) {
        wchar_t value[32768]{}; DWORD bytes = sizeof(value);
        const DWORD queried = RegGetValueW(HKEY_CLASSES_ROOT, key, nullptr, RRF_RT_REG_SZ, nullptr, value, &bytes);
        std::printf("[INFO] isolated HKCR path=%ls error=%lu value=%ls\n", key, queried, value);
    }
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    check(SUCCEEDED(hr), "initialize STA", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return 1;
    {
        ComPtr<IFileOpenDialog> open;
        ComPtr<IFileSaveDialog> save;
        ComPtr<IClassFactory> cached;
        hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, open.Out());
        check(SUCCEEDED(hr) && open.p, "COM activates real FileOpenDialog CLSID", static_cast<unsigned long>(hr));
        check(GetModuleHandleW(argv[1]) != nullptr, "open activation loaded requested shim");
        hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileSaveDialog, save.Out());
        check(SUCCEEDED(hr) && save.p, "COM activates real FileSaveDialog CLSID", static_cast<unsigned long>(hr));
        check(GetModuleHandleW(argv[1]) != nullptr, "save activation retains requested shim");
        hr = CoGetClassObject(CLSID_FileOpenDialog, CLSCTX_INPROC_SERVER, nullptr, IID_IClassFactory, cached.Out());
        check(SUCCEEDED(hr) && cached.p, "cache real dialog factory", static_cast<unsigned long>(hr));
        error = pulse::dialog_auto::Disable();
        check(error == ERROR_SUCCESS && !pulse::dialog_auto::Inspect().enabled, "disable isolated registrations", error);
        FILEOPENDIALOGOPTIONS options = {};
        if (open.p) { hr = open.p->GetOptions(&options); check(SUCCEEDED(hr), "existing open dialog works after disable", static_cast<unsigned long>(hr)); }
        if (save.p) { hr = save.p->GetOptions(&options); check(SUCCEEDED(hr), "existing save dialog works after disable", static_cast<unsigned long>(hr)); }
        if (cached.p) {
            ComPtr<IFileOpenDialog> after;
            hr = cached.p->CreateInstance(nullptr, IID_IFileOpenDialog, after.Out());
            check(SUCCEEDED(hr) && after.p, "cached factory works after disable", static_cast<unsigned long>(hr));
            if (after.p) { hr = after.p->GetOptions(&options); check(SUCCEEDED(hr), "cached factory dialog supports native operation", static_cast<unsigned long>(hr)); }
        }
    }
    CoUninitialize();
    error = isolation.Cleanup();
    check(error == ERROR_SUCCESS, "restore predefined handles and remove exact isolated root", error);
    return failures ? 1 : 0;
}

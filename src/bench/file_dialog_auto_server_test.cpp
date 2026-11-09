// Isolated fixture: link this file instead of dialog_auto_registration.cpp.
// No HKCU mutation, UI, or user preferences are needed.
#include <atomic>
#include <cstdio>
#include <thread>
#include "../common/dialog_auto_registration.h"
namespace { std::atomic<bool> fixture_enabled{false}; }
namespace pulse::dialog_auto {
bool IsEnabledFor(const std::wstring&) noexcept { return fixture_enabled.load(); }
// The existing cases exercise the in-process provider route.
bool PreferOutOfProcess() noexcept { return false; }
bool PreferRemoteHost() noexcept { return false; }
Status Inspect() {
    wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, 32768);
    return {fixture_enabled.load(), false, ERROR_SUCCESS, path};
}
}
#include "../shell_bridge/file_dialog_auto_server.cpp"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
namespace {
class UiEvents final : public IFileDialogEvents {
    std::atomic<ULONG> refs_{1};
public:
    bool pulse = false, native = false, pulse_visible = false, native_visible = false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IFileDialogEvents) return E_NOINTERFACE;
        *out = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { auto n = --refs_; if (!n) delete this; return n; }
    void Inspect(IFileDialog* dialog) {
        ComPtr<IOleWindow> window; HWND hwnd = nullptr;
        if (FAILED(dialog->QueryInterface(IID_PPV_ARGS(&window))) || FAILED(window->GetWindow(&hwnd)) || !hwnd) return;
        DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId()) return;
        wchar_t name[128]{}; GetClassNameW(hwnd, name, 128);
        pulse = pulse || wcscmp(name, L"PulseFolderPickerWindow") == 0;
        native = native || wcscmp(name, L"#32770") == 0;
        pulse_visible |= wcscmp(name, L"PulseFolderPickerWindow") == 0 && IsWindowVisible(hwnd);
        native_visible |= wcscmp(name, L"#32770") == 0 && IsWindowVisible(hwnd);
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog* d) override { Inspect(d); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* r) override { *r = FDESVR_DEFAULT; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* r) override { *r = FDEOR_DEFAULT; return S_OK; }
};
thread_local IFileDialog* active_dialog;
thread_local UiEvents* active_events;
void CALLBACK CancelTimer(HWND, UINT, UINT_PTR, DWORD) {
    if (active_dialog) {
        active_events->Inspect(active_dialog);
        EnumWindows([](HWND hwnd, LPARAM value) -> BOOL {
            auto* events = reinterpret_cast<UiEvents*>(value);
            DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
            wchar_t name[128]{}; GetClassNameW(hwnd, name, 128);
            if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)) std::printf("[WINDOW] class=%ls\n", name);
            if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && wcscmp(name, L"#32770") == 0) {
                events->native = true; events->native_visible = true;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(active_events));
        if (active_events->pulse_visible || active_events->native_visible) {
            const auto closed = active_dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
            std::printf("[CLOSE] hr=%08lx\n", static_cast<unsigned long>(closed));
            if (active_events->native_visible) EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
                DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
                wchar_t name[128]{}, title[256]{}; GetClassNameW(hwnd, name, 128); GetWindowTextW(hwnd, title, 256);
                if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && wcscmp(name,L"#32770")==0) {
                    std::printf("[NATIVE] title=%ls\n", title);
                    PostMessageW(hwnd, WM_COMMAND, IDCANCEL, 0);
                }
                return TRUE;
            }, 0);
        }
    }
}
int UiFixture() {
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) return 1;
    const auto folder = std::filesystem::path(temp) / (L"pulse-auto-ui-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!CreateDirectoryW(folder.c_str(), nullptr)) return 1;
    const auto sample = folder / L"sample.txt";
    HANDLE file = CreateFileW(sample.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { RemoveDirectoryW(folder.c_str()); return 1; }
    CloseHandle(file);
    std::mutex mutex; std::condition_variable cv; bool done = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(30), [&] { return done; })) TerminateProcess(GetCurrentProcess(), 124);
    });
    int failures = 0;
    for (bool save : {false, true}) for (int pass = 0; pass < 3; ++pass) {
        fixture_enabled = true;
        ComPtr<IClassFactory> factory; ComPtr<IFileDialog> dialog;
        auto hr = DllGetClassObject(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&dialog));
        if (FAILED(hr)) { ++failures; continue; }
        const DWORD options = save ? FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR : FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR;
        hr = dialog->SetOptions(options);
        COMDLG_FILTERSPEC filter{L"Text", L"*.txt"};
        if (SUCCEEDED(hr)) hr = dialog->SetFileTypes(1, &filter);
        ComPtr<IShellItem> item;
        if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item));
        if (SUCCEEDED(hr)) hr = dialog->SetFolder(item.Get());
        if (SUCCEEDED(hr)) hr = dialog->SetFileName(save ? L"new.txt" : L"sample.txt");
        ComPtr<UiEvents> events; events.Attach(new UiEvents);
        DWORD cookie = 0;
        if (SUCCEEDED(hr)) hr = dialog->Advise(events.Get(), &cookie);
        fixture_enabled = pass != 2;
        active_dialog = dialog.Get(); active_events = events.Get();
        const auto timer = SetTimer(nullptr, 0, 1500, CancelTimer);
        if (SUCCEEDED(hr) && timer) hr = dialog->Show(nullptr);
        if (timer) KillTimer(nullptr, timer);
        active_dialog = nullptr; active_events = nullptr;
        if (cookie) dialog->Unadvise(cookie);
        const bool ok = timer && hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) && (pass == 2 ? events->native_visible && !events->pulse : events->pulse_visible && !events->native);
        std::printf("[%s] actual UI save=%u pass=%d pulse=%u native=%u hr=%08lx\n", ok ? "PASS" : "FAIL", save, pass, events->pulse, events->native, static_cast<unsigned long>(hr));
        failures += !ok;
    }
    { std::lock_guard<std::mutex> lock(mutex); done = true; } cv.notify_one(); watchdog.join();
    DeleteFileW(sample.c_str()); RemoveDirectoryW(folder.c_str());
    return failures;
}
}
int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    int failures = 0;
    auto check = [&](bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; };
    if (argc > 1 && wcscmp(argv[1], L"--diagnostics") == 0) {
        const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"dialog-diagnostics-" + std::to_wstring(GetCurrentProcessId())));
        check(log::Initialize(root.wstring(), "dialog_auto"), "initialize isolated diagnostic log");
        module = GetModuleHandleW(nullptr);
        LogModule("loaded_bridge", module);
        LogModule("missing_component", nullptr);
        LogRegistration(42);
        ObserveNativeReason("setter seq=5 method=SetFileTypeIndex hr=0x8000FFFF value=1", nullptr, 42);
        check(wcscmp(ContractReason(shell::NativeReason::SetterFailed), L"SetterFailed") == 0,
            "contract reason has readable name");
        check(log::Flush(), "diagnostics flush"); log::Shutdown();
        std::string all;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (entry.path().extension() != L".jsonl") continue;
            std::ifstream input(entry.path(), std::ios::binary);
            all.append(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        }
        check(all.find("loaded_bridge") != std::string::npos && all.find("pe_timestamp") != std::string::npos &&
            all.find("file_dialog_auto_server_test.exe") != std::string::npos, "mapped component path and PE identity logged");
        check(all.find("registered_dll") != std::string::npos && all.find("path_match") != std::string::npos,
            "registration and loaded path comparison logged");
        check(all.find("SetFileTypeIndex hr=0x8000FFFF") != std::string::npos && all.find("\"dialog\":42") != std::string::npos,
            "readable failed setter retains dialog correlation");
        check(all.find("missing_component") != std::string::npos && all.find("\"error\":126") != std::string::npos,
            "missing component reports explicit error");
        return failures ? 1 : 0;
    }
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 2;
    module = GetModuleHandleW(nullptr);
    void* value = reinterpret_cast<void*>(1);
    check(DllGetClassObject(CLSID_NULL, IID_IClassFactory, &value) == CLASS_E_CLASSNOTAVAILABLE && !value, "unknown class rejected");
    check(DllGetClassObject(CLSID_FileOpenDialog, IID_IClassFactory, nullptr) == E_POINTER, "null output rejected");
    check(DllCanUnloadNow() == S_FALSE, "server lifetime retained");
    for (const auto& clsid : {CLSID_FileOpenDialog, CLSID_FileSaveDialog}) {
        fixture_enabled = false;
        ComPtr<IClassFactory> factory;
        auto hr = DllGetClassObject(clsid, IID_PPV_ARGS(&factory));
        check(SUCCEEDED(hr) && factory, "export returns real factory");
        if (!factory) continue;
        ComPtr<IFileDialog> dialog;
        hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&dialog));
        check(SUCCEEDED(hr) && dialog, "disabled creates native dialog");
        dialog.Reset(); fixture_enabled = true;
        hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&dialog));
        check(SUCCEEDED(hr) && dialog, "enabled creates dialog through activation adapter");
        if (dialog) {
            FILEOPENDIALOGOPTIONS options{};
            check(SUCCEEDED(dialog->GetOptions(&options)), "enabled dialog forwards native options");
        }
        fixture_enabled = false;
        auto services = Services(); shell::FileDialogBridgeServices resolved;
        check(!services.resolve_on_show(resolved), "disabled at Show rejects Pulse routing");
        ComPtr<IFileDialog> native;
        ComPtr<IClassFactory> native_factory;
        hr = Native(clsid, CLSCTX_INPROC_SERVER, nullptr, IID_PPV_ARGS(&native_factory));
        if (SUCCEEDED(hr)) hr = native_factory->CreateInstance(nullptr, IID_PPV_ARGS(&native));
        bool native_shown = false;
        auto controlled = Services();
        controlled.show_native = [&](IFileDialog*, HWND) { native_shown = true; return HRESULT_FROM_WIN32(ERROR_CANCELLED); };
        ComPtr<IFileDialog> wrapped;
        if (SUCCEEDED(hr)) hr = shell::WrapNativeFileDialog(clsid == CLSID_FileOpenDialog ? shell::DialogFactory::Open : shell::DialogFactory::Save,
            native.Get(), controlled, IID_PPV_ARGS(&wrapped));
        check(SUCCEEDED(hr) && wrapped, "controlled object uses real wrapper");
        if (wrapped) {
            check(wrapped->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && native_shown,
                "existing wrapper disabled before Show routes native and preserves cancellation");
        }
        HRESULT other = E_FAIL;
        std::thread thread([&] {
            const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (SUCCEEDED(apartment)) {
                ComPtr<IFileDialog> created;
                other = factory->CreateInstance(nullptr, IID_PPV_ARGS(&created));
                created.Reset(); CoUninitialize();
            }
        });
        thread.join();
        check(SUCCEEDED(other), "cached factory creates on another apartment");
    }
    if (argc == 2 && wcscmp(argv[1], L"--ui") == 0) failures += UiFixture();
    CoUninitialize();
    log::Shutdown();
    return failures ? 1 : 0;
}




#include <windows.h>
#include <windowsx.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <propsys.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {
using Microsoft::WRL::ComPtr;
unsigned checks = 0, failures = 0;
void Check(bool value, const char* text) {
    ++checks; failures += !value;
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", text);
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
bool ModuleLoaded(DWORD pid, const std::filesystem::path& path) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W entry{sizeof(entry)}; bool found = false;
    if (Module32FirstW(snapshot, &entry)) do {
        if (_wcsicmp(entry.szExePath, path.c_str()) == 0) { found = true; break; }
    } while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot); return found;
}
struct State {
    IFileDialog* dialog = nullptr;
    IUnknown* identity = nullptr;
    DWORD thread = GetCurrentThreadId(), ui_pid = 0;
    std::filesystem::path bundle, target;
    std::wstring title;
    unsigned overwrite = 0, file_ok = 0, clicks = 0;
    bool cancel = false, veto = false, seen = false, native_seen = false, luma = false, expired = false;
    bool identities = true, candidates = true, bytes_unchanged = true;
    bool save_as_item = false, no_implicit_metadata = true;
    ULONGLONG started = 0, first_seen = 0, next_click = 0;
};
thread_local State* active = nullptr;
class Sink final : public IFileDialogEvents {
    std::atomic<ULONG> refs_{1}; State& state_;
    void Identity(IFileDialog* dialog) {
        ComPtr<IUnknown> identity;
        state_.identities &= dialog && dialog->QueryInterface(IID_PPV_ARGS(&identity)) == S_OK &&
            identity.Get() == state_.identity && GetCurrentThreadId() == state_.thread;
    }
public:
    explicit Sink(State& state) : state_(state) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER; *output = nullptr;
        if (iid != IID_IUnknown && iid != IID_IFileDialogEvents) return E_NOINTERFACE;
        *output = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto value = --refs_; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        Identity(dialog); ++state_.file_ok;
        ComPtr<IShellItem> result; PWSTR path = nullptr;
        const HRESULT hr = dialog->GetResult(&result);
        const HRESULT path_hr = result ? result->GetDisplayName(SIGDN_FILESYSPATH, &path) : E_NOINTERFACE;
        std::error_code error;
        const bool exact = hr == S_OK && path_hr == S_OK && path &&
            std::filesystem::equivalent(path, state_.target, error) && !error;
        CoTaskMemFree(path);
        state_.candidates &= exact;
        state_.bytes_unchanged &= Read(state_.target) == "owned-existing";
        if (state_.save_as_item) {
            ComPtr<IFileSaveDialog> save; ComPtr<IPropertyStore> store;
            const HRESULT queried = dialog->QueryInterface(IID_PPV_ARGS(&save));
            const HRESULT metadata = queried == S_OK ? save->GetProperties(&store) : queried;
            state_.no_implicit_metadata &= metadata == E_UNEXPECTED && !store;
            std::printf("SAVE_AS_METADATA phase=FileOk hr=%08lX store=%u\n", static_cast<unsigned long>(metadata), store ? 1u : 0u);
        }
        const bool veto = state_.veto && state_.file_ok == 1;
        std::printf("FILE_OK count=%u veto=%u exact=%u identity=%u original_bytes=%u\n",
            state_.file_ok, veto, exact, state_.identities, state_.bytes_unchanged);
        state_.next_click = GetTickCount64() + 500;
        return veto ? S_FALSE : S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog* dialog, IShellItem*, FDE_OVERWRITE_RESPONSE* answer) override {
        if (!answer) return E_POINTER;
        Identity(dialog); ++state_.overwrite; *answer = FDEOR_ACCEPT;
        std::printf("OVERWRITE count=%u\n", state_.overwrite); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* answer) override {
        if (!answer) return E_POINTER; *answer = FDESVR_DEFAULT; return S_OK;
    }
};
BOOL CALLBACK Inspect(HWND window, LPARAM) {
    if (!active || !IsWindowVisible(window)) return TRUE;
    wchar_t title[256]{}; GetWindowTextW(window, title, 256);
    if (active->title != title) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    wchar_t name[128]{}; GetClassNameW(window, name, 128);
    if (pid == GetCurrentProcessId() && wcscmp(name, L"#32770") == 0) {
        active->native_seen = true; PostMessageW(window, WM_COMMAND, IDCANCEL, 0); return TRUE;
    }
    if (pid == GetCurrentProcessId() || wcscmp(name, L"PulseFolderPickerWindow")) return TRUE;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    wchar_t image[32768]{}; DWORD units = 32768;
    const bool expected = process && QueryFullProcessImageNameW(process, 0, image, &units) &&
        _wcsicmp(image, (active->bundle / L"pulse.exe").c_str()) == 0;
    if (process) CloseHandle(process);
    if (!expected) return TRUE;
    active->seen = true; active->ui_pid = pid;
    active->luma = ModuleLoaded(pid, active->bundle / L"lumatext.dll");
    const auto now = GetTickCount64();
    if (!active->first_seen) { active->first_seen = now; active->next_click = now + 1000; }
    const unsigned limit = active->veto ? 2u : 1u;
    if (!active->luma || now < active->next_click || active->clicks >= limit) return TRUE;
    if (active->veto && active->clicks == 1 && active->file_ok != 1) return TRUE;
    using Dpi = UINT(WINAPI*)(HWND);
    const auto dpi = reinterpret_cast<Dpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    const float scale = dpi ? dpi(window) / 96.0f : 1.0f;
    RECT client{}; GetClientRect(window, &client);
    const int x = client.right - static_cast<int>((active->cancel ? 204.0f : 76.0f) * scale);
    const int y = client.bottom - static_cast<int>(30.0f * scale);
    ++active->clicks; active->next_click = now + 500;
    PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    PostMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    return TRUE;
}
void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    if (!active) return;
    EnumWindows(Inspect, 0);
    if (GetTickCount64() - active->started > 20000) {
        active->expired = true; active->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    }
}
DWORD WINAPI Watchdog(void* event) {
    if (WaitForSingleObject(event, 120000) == WAIT_TIMEOUT) ExitProcess(124);
    return 0;
}
}
int wmain(int argc, wchar_t** argv) {
    const bool save_as_item = argc == 4 && std::wstring_view(argv[3]) == L"--save-as-item";
    if (argc != 3 && !save_as_item) return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    using SetDpiAwareness = BOOL (WINAPI*)(HANDLE);
    const auto set_dpi = reinterpret_cast<SetDpiAwareness>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    const bool dpi_ready = set_dpi && set_dpi(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4))) != FALSE;
    Check(dpi_ready, "test driver explicitly enters PMv2 before coordinate interaction");
    if (!dpi_ready) return 2;
    const std::filesystem::path bundle(argv[1]), root(argv[2]);
    if (!bundle.is_absolute() || !root.is_absolute() || std::filesystem::exists(root)) return 2;
    std::filesystem::create_directory(root);
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", root.c_str());
    const auto target = root / L"owned.txt";
    { std::ofstream file(target, std::ios::binary); file << "owned-existing"; }
    if (save_as_item) {
        std::ofstream plan(root / L"expected-plan.json");
        plan << "{\"source\":\"dialog_no_overwrite_registered_test.cpp --save-as-item\",\"pid\":" << GetCurrentProcessId()
            << ",\"arch\":\"x" << sizeof(void*) * 8 << "\",\"entry_family\":\"COM\",\"deployment\":\"ordinary\",\"planned_count\":2,\"calls\":["
            << "{\"ordinal\":1,\"expected_status\":\"accepted\",\"consumption\":\"write_reopen\"},{\"ordinal\":2,\"expected_status\":\"cancelled\"}]}\n";
        plan.flush(); if (!plan.good()) return 2;
    }
    GUID nonce{}; if (FAILED(CoCreateGuid(&nonce))) return 2;
    wchar_t nonce_text[64]{}; StringFromGUID2(nonce, nonce_text, 64);
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(apartment)) return 2;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE watchdog = stop ? CreateThread(nullptr, 0, Watchdog, stop, 0, nullptr) : nullptr;
    if (!watchdog) return 2;
    std::printf("REGISTERED_SAVE caller_pid=%lu arch=%zu cases=%u bundle=%ls\n", GetCurrentProcessId(), sizeof(void*) * 8, save_as_item ? 2u : 4u, bundle.c_str());
    for (unsigned scenario = 0; scenario < (save_as_item ? 2u : 4u); ++scenario) {
        State state; state.bundle = bundle; state.target = target;
        state.save_as_item = save_as_item;
        state.cancel = scenario == 1; state.veto = scenario == 2;
        const bool prompt = scenario == 3;
        ComPtr<IFileDialog> dialog; ComPtr<IShellItem> folder; ComPtr<IUnknown> identity;
        HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        const auto shim = sizeof(void*) == 4 ? L"PulseDialogAuto32.dll" : L"PulseDialogAuto64.dll";
        Check(hr == S_OK && dialog && ModuleLoaded(GetCurrentProcessId(), bundle / shim), "ordinary CoCreateInstance activates exact registered architecture-matched bundle");
        if (FAILED(hr) || !dialog) break;
        hr = dialog->QueryInterface(IID_PPV_ARGS(&identity));
        if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(root.c_str(), nullptr, IID_PPV_ARGS(&folder));
        const DWORD options = FOS_NOREADONLYRETURN | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT | (prompt ? FOS_OVERWRITEPROMPT : 0u);
        if (SUCCEEDED(hr)) hr = dialog->SetOptions(options);
        if (SUCCEEDED(hr)) hr = dialog->SetFolder(folder.Get());
        if (SUCCEEDED(hr)) hr = dialog->SetFileName(L"owned.txt");
        if (save_as_item && SUCCEEDED(hr)) {
            ComPtr<IShellItem> source; ComPtr<IFileSaveDialog> save;
            hr = SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&source));
            if (SUCCEEDED(hr)) hr = dialog->QueryInterface(IID_PPV_ARGS(&save));
            if (SUCCEEDED(hr)) hr = save->SetSaveAsItem(source.Get());
            Check(hr == S_OK, "ordinary COM SetSaveAsItem retains owned real filesystem source");
            ComPtr<IPropertyStore> store; const HRESULT metadata = save ? save->GetProperties(&store) : E_NOINTERFACE;
            std::printf("SAVE_AS_METADATA phase=BeforeShow hr=%08lX store=%u\n", static_cast<unsigned long>(metadata), store ? 1u : 0u);
            Check(metadata == E_UNEXPECTED && !store, "ordinary initial-item getter matches native before Show without poisoning admission");
        }
        state.title = (save_as_item ? L"Pulse Registered SaveAsItem " : L"Pulse Registered NoOverwrite ") + std::to_wstring(GetCurrentProcessId()) + L" " + nonce_text + L" Case " + std::to_wstring(scenario);
        if (SUCCEEDED(hr)) hr = dialog->SetTitle(state.title.c_str());
        FILEOPENDIALOGOPTIONS actual{};
        if (SUCCEEDED(hr)) hr = dialog->GetOptions(&actual);
        Check(hr == S_OK && actual == options && static_cast<bool>(actual & FOS_OVERWRITEPROMPT) == prompt, "exact prompt/no-prompt flags survive ordinary wrapper setters");
        state.dialog = dialog.Get(); state.identity = identity.Get();
        ComPtr<IFileDialogEvents> sink; sink.Attach(new Sink(state)); DWORD cookie = 0;
        if (SUCCEEDED(hr)) hr = dialog->Advise(sink.Get(), &cookie);
        state.started = GetTickCount64(); active = &state;
        const UINT_PTR timer = SUCCEEDED(hr) ? SetTimer(nullptr, 0, 50, Tick) : 0;
        if (timer) hr = dialog->Show(nullptr);
        if (timer) KillTimer(nullptr, timer); active = nullptr;
        if (cookie) dialog->Unadvise(cookie);
        ComPtr<IShellItem> result;
        const HRESULT result_hr = dialog->GetResult(&result);
        bool exact = false;
        if (result) {
            PWSTR path = nullptr;
            if (result->GetDisplayName(SIGDN_FILESYSPATH, &path) == S_OK && path) {
                std::error_code error; exact = std::filesystem::equivalent(path, target, error) && !error;
            }
            CoTaskMemFree(path);
        }
        std::printf("CASE scenario=%u prompt=%u cancel=%u veto=%u hr=%08lX result_hr=%08lX overwrite=%u file_ok=%u clicks=%u remote=%u ui_pid=%lu caller_pid=%lu luma=%u native=%u expired=%u exact=%u\n",
            scenario, prompt, state.cancel, state.veto, static_cast<unsigned long>(hr), static_cast<unsigned long>(result_hr), state.overwrite, state.file_ok,
            state.clicks, state.seen && state.ui_pid != GetCurrentProcessId(), state.ui_pid, GetCurrentProcessId(), state.luma, state.native_seen, state.expired, exact);
        Check(timer && state.seen && state.ui_pid != GetCurrentProcessId() && state.luma && !state.native_seen && !state.expired,
            "unique title binds real remote pulse.exe and matching-bundle lumatext");
        Check(hr == (state.cancel ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK) && state.overwrite == (prompt ? 1u : 0u) &&
            state.file_ok == (state.cancel ? 0u : state.veto ? 2u : 1u), "ordinary remote chain preserves no-prompt overwrite count, veto and cancellation");
        Check(state.identities && state.candidates && state.bytes_unchanged && Read(target) == "owned-existing", "callbacks retain wrapper identity and selection never modifies existing bytes");
        Check(state.cancel ? FAILED(result_hr) && !result : result_hr == S_OK && exact, "ordinary remote result is exact owned fixture or cleared on cancellation");
        if (save_as_item) {
            ComPtr<IFileSaveDialog> save; dialog->QueryInterface(IID_PPV_ARGS(&save));
            ComPtr<IPropertyStore> store; const HRESULT metadata = save->GetProperties(&store);
            std::printf("SAVE_AS_METADATA phase=AfterShow hr=%08lX store=%u\n", static_cast<unsigned long>(metadata), store ? 1u : 0u);
            Check(state.no_implicit_metadata && metadata == E_UNEXPECTED && !store, "ordinary remote initial-item metadata getter matches actual native no-collection contract");
            if (!state.cancel && hr == S_OK && exact) {
                ComPtr<IPropertyStore> empty; PSCreateMemoryPropertyStore(IID_PPV_ARGS(&empty));
                const HRESULT applied = save->ApplyProperties(result.Get(), empty.Get(), nullptr, nullptr);
                std::printf("SAVE_AS_APPLY empty_hr=%08lX\n", static_cast<unsigned long>(applied));
                Check(applied == S_OK && Read(target) == "owned-existing", "ordinary accepted result retains native property helper without changing contents");
            }
        }
        if (!state.cancel && hr == S_OK && exact) {
            { std::ofstream file(target, std::ios::binary | std::ios::trunc); file << "registered-host-roundtrip"; }
            Check(Read(target) == "registered-host-roundtrip", "host writes and reopens the exact selected file");
            { std::ofstream file(target, std::ios::binary | std::ios::trunc); file << "owned-existing"; }
        }
    }
    SetEvent(stop); WaitForSingleObject(watchdog, INFINITE); CloseHandle(watchdog); CloseHandle(stop);
    CoUninitialize();
    Check(Read(target) == "owned-existing", "owned fixture bytes restored after host roundtrip");
    std::filesystem::remove(target);
    std::printf("%s Checks=%u failures=%u registration_mutations=0\n", save_as_item ? "REGISTERED_SAVE_AS_ITEM" : "REGISTERED_NO_OVERWRITE", checks, failures);
    return failures ? 1 : 0;
}

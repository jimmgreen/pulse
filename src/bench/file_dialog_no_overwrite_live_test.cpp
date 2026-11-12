#include "../picker_inprocess/picker_ui_api_v2.h"
#include <shlobj.h>
#include <propsys.h>
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
void Check(bool ok, const char* name) { ++checks; failures += !ok; std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); }
void Module(const char* name, const void* address) {
    HMODULE module = nullptr; wchar_t path[32768]{};
    const BOOL found = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address), &module);
    if (found) GetModuleFileNameW(module, path, 32768);
    std::printf("NATIVE_MODULE %s address=%p module=%p path=%ls error=%lu\n", name, address, static_cast<void*>(module), path, found ? 0ul : GetLastError());
}
void Item(const char* stage, HRESULT hr, IShellItem* item) {
    PWSTR path = nullptr;
    const HRESULT path_hr = item ? item->GetDisplayName(SIGDN_FILESYSPATH, &path) : E_NOINTERFACE;
    std::printf("NATIVE_ITEM stage=%s hr=%08lX path_hr=%08lX path=%ls\n", stage, static_cast<unsigned long>(hr), static_cast<unsigned long>(path_hr), path ? path : L"<null>");
    CoTaskMemFree(path);
}
void Snapshot(const char* stage, IFileDialog* dialog, bool result) {
    ComPtr<IShellItem> folder, selected;
    const HRESULT folder_hr = dialog->GetFolder(&folder);
    Item(stage, folder_hr, folder.Get());
    PWSTR filename = nullptr; const HRESULT filename_hr = dialog->GetFileName(&filename);
    std::printf("NATIVE_NAME stage=%s hr=%08lX filename=%ls\n", stage, static_cast<unsigned long>(filename_hr), filename ? filename : L"<null>");
    CoTaskMemFree(filename);
    if (result) { const HRESULT hr = dialog->GetResult(&selected); Item("OnFileOk-GetResult", hr, selected.Get()); }
}
bool SaveAsMetadata(const char* stage, IFileDialog* dialog) {
    ComPtr<IFileSaveDialog> save; const HRESULT queried = dialog->QueryInterface(IID_PPV_ARGS(&save));
    ComPtr<IPropertyStore> store; const HRESULT hr = SUCCEEDED(queried) ? save->GetProperties(&store) : queried;
    DWORD count = 0; const HRESULT counted = store ? store->GetCount(&count) : E_PENDING;
    std::printf("SAVE_AS_METADATA stage=%s hr=%08lX count_hr=%08lX count=%lu explicit_metadata_setters=0\n", stage,
        static_cast<unsigned long>(hr), static_cast<unsigned long>(counted), count);
    if (counted == S_OK && count <= 128) for (DWORD i = 0; i < count; ++i) {
        PROPERTYKEY key{}; PROPVARIANT value{}; const HRESULT key_hr = store->GetAt(i, &key);
        const HRESULT read = SUCCEEDED(key_hr) ? store->GetValue(key, &value) : key_hr;
        wchar_t guid[40]{}; StringFromGUID2(key.fmtid, guid, 40);
        std::printf("SAVE_AS_PROPERTY key=%ls:%lu type=%u hr=%08lX\n", guid, key.pid, static_cast<unsigned>(value.vt), static_cast<unsigned long>(read));
        PropVariantClear(&value);
    }
    return hr == E_UNEXPECTED && !store;
}
struct State {
    unsigned overwrite = 0, file_ok = 0;
    bool cancel = false, veto = false;
    unsigned submits = 0;
    ULONGLONG next_submit = 0;
    IFileDialog* native = nullptr;
    ComPtr<IShellItem> directory_item;
    std::filesystem::path directory;
    ULONGLONG navigation_tick = 0;
    unsigned renavigations = 0;
    std::filesystem::path expected_file;
    bool candidates_exact = true;
    bool save_as_item = false;
    ULONGLONG deadline = 0;
};
thread_local State* active = nullptr;
class Sink final : public IFileDialogEvents {
    std::atomic<ULONG> refs_{1}; State& state_;
public:
    explicit Sink(State& s) : state_(s) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IFileDialogEvents) return E_NOINTERFACE;
        *out = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        ++state_.file_ok; std::printf("NATIVE_EVENT FileOk=%u veto=%u\n", state_.file_ok, state_.veto && state_.file_ok == 1);
        Snapshot("OnFileOk", dialog, true);
        if (state_.save_as_item) Check(SaveAsMetadata("OnFileOk", dialog), "SaveAsItem without collection exposes no implicit metadata store in FileOk");
        ComPtr<IShellItem> selected; PWSTR path = nullptr;
        const HRESULT result_hr = dialog->GetResult(&selected);
        const HRESULT path_hr = selected ? selected->GetDisplayName(SIGDN_FILESYSPATH, &path) : E_NOINTERFACE;
        std::error_code error;
        const bool exact = result_hr == S_OK && path_hr == S_OK && path &&
            std::filesystem::equivalent(path, state_.expected_file, error) && !error;
        state_.candidates_exact &= exact;
        std::printf("NATIVE_EVENT candidate_exact=%u\n", exact);
        CoTaskMemFree(path);
        return state_.veto && state_.file_ok == 1 ? S_FALSE : S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* out) override {
        if (!out) return E_POINTER; ++state_.overwrite; *out = FDEOR_ACCEPT; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem* folder) override { Item("OnFolderChanging", S_OK, folder); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog* dialog) override {
        state_.navigation_tick = GetTickCount64(); Snapshot("OnFolderChange", dialog, false); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* out) override { if (!out) return E_POINTER; *out = FDESVR_DEFAULT; return S_OK; }
};
void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    if (!active) return;
    if (GetTickCount64() > active->deadline) { active->native->Close(HRESULT_FROM_WIN32(ERROR_TIMEOUT)); return; }
    if (GetTickCount64() < active->next_submit || active->submits >= (active->veto ? 2u : 1u)) return;
    if (!active->navigation_tick || GetTickCount64() - active->navigation_tick < 500) return;
    ComPtr<IShellItem> folder; PWSTR path = nullptr;
    const HRESULT folder_hr = active->native->GetFolder(&folder);
    const HRESULT path_hr = folder ? folder->GetDisplayName(SIGDN_FILESYSPATH, &path) : E_NOINTERFACE;
    std::error_code error;
    const bool same_folder = SUCCEEDED(folder_hr) && SUCCEEDED(path_hr) && path &&
        std::filesystem::equivalent(path, active->directory, error) && !error;
    CoTaskMemFree(path);
    if (!same_folder) {
        ++active->renavigations;
        const HRESULT hr = active->native->SetFolder(active->directory_item.Get());
        std::printf("NATIVE_DRIVER reanchor=%u SetFolder=%08lX\n", active->renavigations, static_cast<unsigned long>(hr));
        active->native->SetFileName(L"owned.txt");
        active->next_submit = GetTickCount64() + 500;
        return;
    }
    ComPtr<IOleWindow> ole;
    HWND window = nullptr;
    if (SUCCEEDED(active->native->QueryInterface(IID_PPV_ARGS(&ole))) && SUCCEEDED(ole->GetWindow(&window))) {
        if (IsWindowVisible(window) && IsWindowEnabled(GetDlgItem(window, IDOK))) {
            Snapshot("BeforeSubmit", active->native, false);
            ++active->submits;
            active->next_submit = GetTickCount64() + 500;
            SendMessageW(GetDlgItem(window, active->cancel ? IDCANCEL : IDOK), BM_CLICK, 0, 0);
        }
    }
}
HRESULT WINAPI Event(void* context, const PulsePickerUiV2Event* event) {
    auto& s = *static_cast<State*>(context);
    if (event->kind == PULSE_PICKER_UI_V2_EVENT_FILE_OK) {
        ++s.file_ok;
        if (s.veto && s.file_ok == 1) {
            PostMessageW(GetDlgItem(event->window, 102), WM_KEYDOWN, VK_RETURN, 0);
            return S_FALSE;
        }
    }
    if (event->kind == PULSE_PICKER_UI_V2_EVENT_READY) {
        if (s.cancel) PostMessageW(event->window, WM_CLOSE, 0, 0);
        else PostMessageW(GetDlgItem(event->window, 102), WM_KEYDOWN, VK_RETURN, 0);
    }
    return S_OK;
}
HRESULT WINAPI Overwrite(void* context, const PulsePickerUiV2OverwriteEvent*, uint32_t* answer) {
    ++static_cast<State*>(context)->overwrite; *answer = PULSE_PICKER_UI_V2_OVERWRITE_ACCEPT; return S_OK;
}
PulsePickerUiV2Text Text(const std::wstring& s) { return {s.data(), static_cast<uint32_t>(s.size())}; }
bool Unchanged(const std::filesystem::path& file) { std::ifstream f(file, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}) == "owned-existing"; }
}
int wmain(int argc, wchar_t** argv) {
    const bool native_only = argc == 4 && std::wstring_view(argv[3]) == L"--native-only";
    const bool metadata_only = argc == 4 && std::wstring_view(argv[3]) == L"--native-metadata";
    const bool save_as_item_only = argc == 4 && std::wstring_view(argv[3]) == L"--save-as-item-native";
    if (argc != 3 && !native_only && !metadata_only && !save_as_item_only) return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("SETUP begin");
    const auto root = std::filesystem::absolute(argv[2]); std::filesystem::create_directories(root);
    const auto target = root / L"owned.txt";
    if (std::filesystem::exists(target)) return 2;
    { std::ofstream f(target, std::ios::binary); f << "owned-existing"; }
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", root.c_str());
    const auto app_id = L"Pulse.Tests.SaveOracle." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    const HRESULT app_id_hr = SetCurrentProcessExplicitAppUserModelID(app_id.c_str());
    std::printf("SETUP isolated AppUserModelID hr=%08lX\n", static_cast<unsigned long>(app_id_hr));
    std::puts("SETUP COM begin");
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    std::puts("SETUP COM ready");
    wchar_t system[32768]{}; GetSystemDirectoryW(system, 32768);
    const auto com = LoadLibraryExW((std::wstring(system) + L"\\comdlg32.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using Factory = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
    const auto factory = com ? reinterpret_cast<Factory>(GetProcAddress(com, "DllGetClassObject")) : nullptr;
    Module("DllGetClassObject", reinterpret_cast<const void*>(factory));
    if (metadata_only) {
        ComPtr<IClassFactory> cf; ComPtr<IFileDialog> dialog;
        HRESULT hr = factory ? factory(CLSID_FileSaveDialog, IID_PPV_ARGS(&cf)) : E_NOINTERFACE;
        if (SUCCEEDED(hr)) hr = cf->CreateInstance(nullptr, IID_PPV_ARGS(&dialog));
        if (dialog) {
            const auto table = *reinterpret_cast<void***>(dialog.Get());
            Module("dialog-ClearClientData", table[25]);
            GUID guid{}; hr = CoCreateGuid(&guid);
            if (SUCCEEDED(hr)) hr = dialog->SetClientGuid(guid);
            if (SUCCEEDED(hr)) hr = dialog->ClearClientData();
            std::printf("NATIVE_METADATA no_Show=1 unique_GUID_ClearClientData=%08lX\n", static_cast<unsigned long>(hr));
        }
        dialog.Reset(); cf.Reset(); CoUninitialize(); std::filesystem::remove(target);
        return 0;
    }
    for (unsigned variant = 0; variant < (save_as_item_only ? 1u : 3u); ++variant) {
        const bool prompt = variant == 1;
        std::printf("NATIVE begin prompt=%u\n", prompt);
        State state; state.veto = variant == 2; state.expected_file = target; state.save_as_item = save_as_item_only;
        ComPtr<IClassFactory> cf; ComPtr<IFileDialog> dialog; ComPtr<IShellItem> folder;
        HRESULT hr = factory ? factory(CLSID_FileSaveDialog, IID_PPV_ARGS(&cf)) : E_NOINTERFACE;
        if (SUCCEEDED(hr)) hr = cf->CreateInstance(nullptr, IID_PPV_ARGS(&dialog));
        if (dialog) {
            const auto table = *reinterpret_cast<void***>(dialog.Get());
            Module("dialog-vtable", table); Module("dialog-Show", table[3]); Module("dialog-SetFolder", table[12]); Module("dialog-GetResult", table[20]);
        }
        GUID client{};
        if (SUCCEEDED(hr)) hr = CoCreateGuid(&client);
        if (save_as_item_only && SUCCEEDED(hr)) {
            wchar_t text[40]{}; StringFromGUID2(client, text, 40);
            std::ofstream journal(std::filesystem::absolute(L"bench_data/dialog-contracts-20261110/save-as-item-owned-guids.tsv"), std::ios::app);
            journal << "SaveAsItem\tSave\t"; for (const auto c : std::wstring_view(text)) journal << static_cast<char>(c);
            journal << "\tallocated-before-SetClientGuid\n"; journal.flush();
            if (!journal.good()) hr = E_FAIL;
            std::printf("SAVE_AS_GUID value=%ls journaled=%u\n", text, journal.good());
        }
        if (SUCCEEDED(hr)) hr = dialog->SetClientGuid(client);
        std::printf("NATIVE SetClientGuid=%08lX\n", static_cast<unsigned long>(hr));
        if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(root.c_str(), nullptr, IID_PPV_ARGS(&folder));
        if (SUCCEEDED(hr)) hr = dialog->SetOptions(FOS_NOCHANGEDIR | FOS_NOREADONLYRETURN | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT | (prompt ? FOS_OVERWRITEPROMPT : 0u));
        if (SUCCEEDED(hr)) hr = dialog->SetDefaultFolder(folder.Get());
        std::printf("NATIVE SetDefaultFolder=%08lX\n", static_cast<unsigned long>(hr));
        if (SUCCEEDED(hr)) hr = dialog->SetFolder(folder.Get());
        std::printf("NATIVE SetFolder=%08lX\n", static_cast<unsigned long>(hr));
        if (SUCCEEDED(hr)) hr = dialog->SetFileName(L"owned.txt");
        if (save_as_item_only && SUCCEEDED(hr)) {
            ComPtr<IShellItem> source; ComPtr<IFileSaveDialog> save;
            hr = SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&source));
            if (SUCCEEDED(hr)) hr = dialog->QueryInterface(IID_PPV_ARGS(&save));
            if (SUCCEEDED(hr)) hr = save->SetSaveAsItem(source.Get());
            std::printf("SAVE_AS_SET hr=%08lX\n", static_cast<unsigned long>(hr));
            Check(SaveAsMetadata("BeforeShow", dialog.Get()), "SaveAsItem without collection exposes no implicit metadata before Show");
        }
        if (dialog) Snapshot("Configured", dialog.Get(), false);
        Check(SUCCEEDED(hr), "native control configuration"); if (FAILED(hr)) continue;
        ComPtr<IFileDialogEvents> sink; sink.Attach(new Sink(state)); DWORD cookie = 0; dialog->Advise(sink.Get(), &cookie);
        state.native = dialog.Get(); state.directory_item = folder; state.directory = root; state.deadline = GetTickCount64() + 10000;
        state.next_submit = GetTickCount64() + 500;
        active = &state;
        const auto timer = SetTimer(nullptr, 0, 100, Tick); hr = dialog->Show(nullptr); KillTimer(nullptr, timer); active = nullptr; dialog->Unadvise(cookie);
        std::printf("NATIVE prompt=%u hr=%08lX overwrite=%u file_ok=%u\n", prompt, static_cast<unsigned long>(hr), state.overwrite, state.file_ok);
        Check(hr == S_OK && state.overwrite == (prompt ? 1u : 0u) && state.file_ok == (state.veto ? 2u : 1u) && Unchanged(target), "native prompt flag and FileOk veto preserve contract and file bytes");
        ComPtr<IShellItem> selected; PWSTR selected_path = nullptr;
        const auto result_hr = dialog->GetResult(&selected);
        const auto path_hr = selected ? selected->GetDisplayName(SIGDN_FILESYSPATH, &selected_path) : E_NOINTERFACE;
        std::error_code identity_error;
        const bool same_file = selected_path && std::filesystem::equivalent(selected_path, target, identity_error) && !identity_error;
        std::printf("NATIVE result_hr=%08lX path_hr=%08lX submits=%u identity=%u identity_error=%d\n",
            static_cast<unsigned long>(result_hr), static_cast<unsigned long>(path_hr), state.submits, same_file, identity_error.value());
        if (!same_file && selected_path) std::wprintf(L"NATIVE actual=%ls expected=%ls\n", selected_path, target.c_str());
        Check(hr == S_OK && result_hr == S_OK && path_hr == S_OK && same_file, "native result binds exact existing fixture identity");
        Check(state.candidates_exact, "every native FileOk candidate binds the exact owned fixture including veto");
        CoTaskMemFree(selected_path);
        if (save_as_item_only) {
            Check(SaveAsMetadata("AfterShow", dialog.Get()), "SaveAsItem without collection exposes no implicit metadata after actual acceptance");
            ComPtr<IFileSaveDialog> save; ComPtr<IShellItem> source; ComPtr<IPropertyStore> empty;
            dialog->QueryInterface(IID_PPV_ARGS(&save)); SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&source));
            PSCreateMemoryPropertyStore(IID_PPV_ARGS(&empty)); const HRESULT applied = save->ApplyProperties(source.Get(), empty.Get(), nullptr, nullptr);
            std::printf("SAVE_AS_APPLY empty_hr=%08lX unchanged=%u\n", static_cast<unsigned long>(applied), Unchanged(target));
            Check(applied == S_OK && Unchanged(target), "native property helper applies an empty owned store without changing document contents");
        }
        SetLastError(0); const HRESULT cleared = dialog->ClearClientData();
        std::printf("NATIVE ClearClientData-afterShow=%08lX last_error=%lu\n", static_cast<unsigned long>(cleared), GetLastError());
        Check(cleared == S_OK, "native control clears only its unique client state");
    }
    active = nullptr;
    if (native_only || save_as_item_only) {
        CoUninitialize(); std::filesystem::remove(target);
        std::printf("Checks=%u failures=%u\n", checks, failures); return failures ? 1 : 0;
    }
    const auto module = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    std::printf("PROVIDER module=%p error=%lu\n", static_cast<void*>(module), module ? 0ul : GetLastError());
    const auto get = module ? reinterpret_cast<decltype(&PulsePickerUiGetApiV2)>(GetProcAddress(module, "PulsePickerUiGetApiV2")) : nullptr;
    PulsePickerUiV2Api api{};
    Check(get && get(2, sizeof(api), &api) == S_OK && (api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_SAVE_NO_OVERWRITE), "provider explicitly advertises no-overwrite capability");
    if (!get || !api.Initialize) { CoUninitialize(); return 1; }
    PulsePickerUiV2Initialize init{sizeof(init), 2}; PulsePickerUiV2Session session = 0;
    Check(api.Initialize(&init, &session) == S_OK, "initialize UI provider");
    const auto directory = root.wstring(); const std::wstring name = L"owned.txt";
    PulsePickerUiV2Config config{}; config.size = sizeof(config); config.version = 2; config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;
    config.initial_folder = Text(directory); config.filename = Text(name);
    for (unsigned variant = 0; variant < 5; ++variant) {
        const bool prompt = variant == 2 || variant == 3, cancel = variant == 1 || variant == 3;
        std::printf("PULSE begin prompt=%u cancel=%u\n", prompt, cancel);
        State state; state.cancel = cancel; state.veto = variant == 4;
        config.save_options = PULSE_PICKER_UI_V2_SAVE_NATIVE_DEFAULTS | (prompt ? PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT : PULSE_PICKER_UI_V2_SAVE_NO_OVERWRITE_PROMPT);
        Check(api.ValidateConfig(session, &config) == S_OK, "explicit prompt/no-prompt config validates");
        PulsePickerUiV2Callbacks cb{sizeof(cb), 0, Event, &state, Overwrite};
        wchar_t path[32769]{}; PulsePickerUiV2Result result{}; result.size = sizeof(result); result.path = {path, 32769, 0};
        const auto hr = api.Show(session, &config, &cb, &result);
        std::printf("PULSE prompt=%u cancel=%u hr=%08lX overwrite=%u file_ok=%u\n", prompt, cancel, static_cast<unsigned long>(hr), state.overwrite, state.file_ok);
        Check(hr == (cancel ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK) && state.overwrite == (prompt && !cancel ? 1u : 0u) &&
            state.file_ok == (cancel ? 0u : state.veto ? 2u : 1u), "Pulse matches native overwrite callback, FileOk veto and cancellation");
        Check(cancel ? result.path.units == 0 : std::wstring(path) == target.wstring(), "result path is exact or cleared on cancellation");
        Check(Unchanged(target), "selection never truncates or writes existing target");
        if (!cancel && hr == S_OK && std::wstring(path) == target.wstring()) {
            { std::ofstream f(target, std::ios::binary | std::ios::trunc); f << "host-roundtrip"; }
            std::ifstream input(target, std::ios::binary);
            Check(std::string(std::istreambuf_iterator<char>(input), {}) == "host-roundtrip", "host writes selected target and reopens exact content");
            input.close(); { std::ofstream f(target, std::ios::binary | std::ios::trunc); f << "owned-existing"; }
        }
    }
    config.save_options = PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT | PULSE_PICKER_UI_V2_SAVE_NO_OVERWRITE_PROMPT;
    Check(FAILED(api.ValidateConfig(session, &config)), "contradictory prompt flags rejected before UI");
    config.save_options = 0; Check(FAILED(api.ValidateConfig(session, &config)), "absent negotiation is not interpreted as no prompt");
    api.Shutdown(session); CoUninitialize();
    std::filesystem::remove(target);
    std::printf("Checks=%u failures=%u\n", checks, failures); return failures ? 1 : 0;
}

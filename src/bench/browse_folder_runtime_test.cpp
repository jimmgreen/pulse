// Public SHBrowseForFolder exports are detoured in this isolated test process.
#define PULSE_RUNTIME_ENTRY_TEST
#include "../shell_bridge/file_dialog_runtime.cpp"
#include "../shell_bridge/file_dialog_client_state.h"
#include <wrl/client.h>
#include <shellapi.h>
#include <dlgs.h>
#include <cderr.h>
#include <array>
#include <type_traits>
#include <string_view>
#include <thread>
#include <fstream>

namespace {
unsigned failures = 0, checks = 0, native_calls = 0, bridge_calls = 0, native_show = 0, ui_ready = 0;
bool cancelled = false;
std::wstring fixture;
bool native_accept = false;
unsigned native_ticks = 0;
void Check(bool value, const char* label) {
    ++checks; if (!value) ++failures;
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
}
PIDLIST_ABSOLUTE WINAPI NativeW(LPBROWSEINFOW) { ++native_calls; return nullptr; }
PIDLIST_ABSOLUTE WINAPI NativeA(LPBROWSEINFOA) { ++native_calls; return nullptr; }
int CALLBACK Callback(HWND, UINT, LPARAM, LPARAM) { return 0; }
struct BrowseCallbackState {
    std::wstring outside, expected;
    std::string outside_ansi;
    unsigned initialized = 0, matched = 0, escaped = 0, enabled = 0;
};
int CALLBACK RootedCallback(HWND window, UINT event, LPARAM value, LPARAM cookie) {
    auto& state = *reinterpret_cast<BrowseCallbackState*>(cookie);
    if (event == BFFM_INITIALIZED) {
        ++state.initialized;
        SendMessageW(window, BFFM_SETOKTEXT, 0, reinterpret_cast<LPARAM>(L"Use this folder"));
        if (state.initialized == 1) {
            SendMessageW(window, BFFM_SETSELECTIONW, TRUE, reinterpret_cast<LPARAM>(state.outside.c_str()));
        } else {
            SendMessageW(window, BFFM_SETSELECTIONA, TRUE, reinterpret_cast<LPARAM>(state.outside_ansi.c_str()));
        }
        SendMessageW(window, BFFM_ENABLEOK, 0, FALSE);
        SendMessageW(window, WM_COMMAND, IDOK, 0);
        SendMessageW(window, BFFM_ENABLEOK, 0, TRUE);
        ++state.enabled;
    } else if (event == BFFM_SELCHANGED) {
        wchar_t path[32768]{};
        if (value && SHGetPathFromIDListEx(reinterpret_cast<PCIDLIST_ABSOLUTE>(value), path, ARRAYSIZE(path), GPFIDL_DEFAULT)) {
            if (state.expected == path) ++state.matched;
            if (state.outside == path) ++state.escaped;
        }
    }
    return 0;
}
decltype(PulsePickerUiV2Api::Show) real_show = nullptr;
struct Forward { const PulsePickerUiV2Callbacks* original; };
HRESULT WINAPI Event(void* context, const PulsePickerUiV2Event* event) {
    auto* original = static_cast<Forward*>(context)->original;
    const HRESULT hr = original && original->callback ? original->callback(original->context, event) : S_OK;
    if (event->kind == PULSE_PICKER_UI_V2_EVENT_READY) {
        ++ui_ready;
        if (cancelled) PostMessageW(event->window, WM_CLOSE, 0, 0);
        else PostMessageW(GetDlgItem(event->window, 102), WM_KEYDOWN, VK_RETURN, 0);
    }
    return hr;
}
bool Consume(PIDLIST_ABSOLUTE pidl) {
    if (!pidl) return false;
    wchar_t path[32768]{};
    const bool matches = SHGetPathFromIDListEx(pidl, path, 32768, GPFIDL_DEFAULT) && fixture == path;
    bool consumed = false;
    // The host consumes only the exact owned fixture, never an arbitrary result.
    if (matches) {
        HANDLE file = CreateFileW((fixture + L"\\probe.txt").c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            char text[8]{}; DWORD read = 0;
            consumed = ReadFile(file, text, sizeof(text), &read, nullptr) && read == 5 && std::memcmp(text, "probe", 5) == 0;
            CloseHandle(file);
        }
    }
    CoTaskMemFree(pidl);
    return consumed;
}
void CALLBACK DriveNative(HWND, UINT, UINT_PTR, DWORD) {
    ++native_ticks;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
        if (!IsWindowVisible(window)) return TRUE;
        wchar_t name[80]{}; GetClassNameW(window, name, 80);
        if (wcscmp(name, L"#32770")) return TRUE;
        if (!native_accept) PostMessageW(window, WM_CLOSE, 0, 0);
        else {
            SendMessageW(window, BFFM_SETSELECTIONW, TRUE, reinterpret_cast<LPARAM>(fixture.c_str()));
            if (native_ticks >= 3 && IsWindowEnabled(GetDlgItem(window, IDOK)))
                PostMessageW(window, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
        }
        return TRUE;
    }, 0);
}
size_t Count(const std::string& text, const char* token) {
    size_t count = 0, offset = 0;
    while ((offset = text.find(token, offset)) != text.npos) { ++count; offset += std::strlen(token); }
    return count;
}
}
#include "legacy_open_save_compat_cases.inc"
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && std::string_view(argv[1]) == "--legacy-native") return legacy_open_save_compat::Run(false);
    if (argc == 2 && std::string_view(argv[1]) == "--legacy-ui") return legacy_open_save_compat::Run(true);
    if (argc == 2 && std::string_view(argv[1]) == "--legacy-flags-native") return legacy_open_save_compat::Run(false, true);
    if (argc == 2 && std::string_view(argv[1]) == "--legacy-flags-ui") return legacy_open_save_compat::Run(true, true);
    if (argc == 2 && std::string_view(argv[1]) == "--legacy-save-readonly-fallback") return legacy_open_save_compat::Run(true, true, false, true);
    if (argc == 3 && std::string_view(argv[1]) == "--legacy-flags-ui" && std::string_view(argv[2]) == "--interactive")
        return legacy_open_save_compat::Run(true, true, true);
    const bool baseline = argc == 2 && std::string_view(argv[1]) == "--native-baseline";
    const bool ui = argc == 2 && std::string_view(argv[1]) == "--ui";
    if (argc != 2 || (!baseline && !ui && std::string_view(argv[1]) != "--contract")) return 2;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 2;
    const auto original_cwd = std::filesystem::current_path();
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") / (L"browse-" + std::to_wstring(GetCurrentProcessId())));
    if (!std::filesystem::create_directory(root)) return 2;
    fixture = root.wstring();
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", fixture.c_str());
    const auto registry = L"Software\\Pulse\\Tests\\Browse-" + std::to_wstring(GetCurrentProcessId());
    HANDLE seed = CreateFileW((root / L"probe.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    if (seed == INVALID_HANDLE_VALUE || !WriteFile(seed, "probe", 5, &written, nullptr) || written != 5) return 2;
    CloseHandle(seed);
    wchar_t wide[MAX_PATH] = L"unchanged"; char ansi[MAX_PATH] = "unchanged";
    BROWSEINFOW w{}; w.ulFlags = BIF_RETURNONLYFSDIRS; w.pszDisplayName = wide; w.iImage = -99; w.lpszTitle = L"Browse fixture";
    BROWSEINFOA a{}; a.ulFlags = w.ulFlags; a.pszDisplayName = ansi; a.iImage = -99; a.lpszTitle = "Browse fixture";
    const std::array<UINT, 3> modes{BIF_RETURNONLYFSDIRS, BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE,
        BIF_RETURNONLYFSDIRS | BIF_USENEWUI};
    SHFILEINFOW expected{};
    if (!SHGetFileInfoW(fixture.c_str(), 0, &expected, sizeof(expected), SHGFI_DISPLAYNAME | SHGFI_SYSICONINDEX | SHGFI_SMALLICON)) return 2;
    std::string expected_ansi;
    for (const wchar_t ch : std::wstring(expected.szDisplayName)) expected_ansi.push_back(static_cast<char>(ch));
    if (baseline) {
        for (const UINT mode : modes) {
            std::printf("NATIVE_ORACLE flags=%u\n", mode);
            w.ulFlags = a.ulFlags = mode;
            const UINT_PTR timer = SetTimer(nullptr, 0, 100, DriveNative);
            native_accept = true; native_ticks = 0;
            Check(Consume(SHBrowseForFolderW(&w)), "native W acceptance returns host-consumable absolute PIDL");
            Check(std::wstring(wide) == expected.szDisplayName && w.iImage == expected.iIcon, "native W display name and system image match Shell result");
            native_ticks = 0;
            Check(Consume(SHBrowseForFolderA(&a)), "native A acceptance returns host-consumable absolute PIDL");
            Check(std::string(ansi) == expected_ansi && a.iImage == expected.iIcon, "native A display name and system image match Shell result");
            native_accept = false;
            Check(SHBrowseForFolderW(&w) == nullptr, "native W cancellation returns null PIDL");
            Check(SHBrowseForFolderA(&a) == nullptr, "native A cancellation returns null PIDL");
            KillTimer(nullptr, timer);
        }
    } else {
        std::shared_ptr<shell::FileDialogInprocessProvider> provider;
        if (ui) {
            wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
            const auto dll = std::filesystem::path(executable).parent_path() / L"PulsePickerUiProof.dll";
            HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            using Get = HRESULT (WINAPI*)(uint32_t, uint32_t, PulsePickerUiV2Api*);
            auto get = module ? reinterpret_cast<Get>(GetProcAddress(module, "PulsePickerUiGetApiV2")) : nullptr;
            provider = std::make_shared<shell::FileDialogInprocessProvider>();
            if (!get || get(2, sizeof(provider->api), &provider->api) != S_OK) { std::printf("UI load failed error=%lu\n", GetLastError()); return 2; }
            provider->module_lifetime = std::shared_ptr<void>(module, [](void* value) { FreeLibrary(static_cast<HMODULE>(value)); });
            provider->allow_unverified_proof = true;
            provider->set_navigation_root = reinterpret_cast<PulsePickerUiV2SetNavigationRoot>(GetProcAddress(module, "PulsePickerUiSetNavigationRootV2"));
            real_show = provider->api.Show;
            provider->api.Show = [](PulsePickerUiV2Session session, const PulsePickerUiV2Config* config, const PulsePickerUiV2Callbacks* callbacks, PulsePickerUiV2Result* result) -> HRESULT {
                PulsePickerUiV2FolderPolicyConfig storage{};
                if (config->size < sizeof(*config) || config->size > sizeof(storage)) return E_INVALIDARG;
                std::memcpy(&storage, config, config->size);
                storage.folder_policy &= ~PULSE_PICKER_UI_V2_FOLDER_ALLOW_AUTO_JUMP;
                auto& automated_config = storage.base.base;
                automated_config.initial_folder = {fixture.c_str(), static_cast<uint32_t>(fixture.size())};
                Forward forward{callbacks}; auto automated_callbacks = *callbacks;
                automated_callbacks.context = &forward; automated_callbacks.callback = Event;
                return real_show(session, &automated_config, &automated_callbacks, result);
            };
        }
        Configuration state; state.log_records = 256; state.show_log_records = 0;
        state.host = L"browse-fixture.exe"; state.control = fixture; state.routing_enabled = true; current = &state;
        legacy_test_services = [&](shell::FileDialogBridgeServices& services) {
            services.client_state = std::make_shared<shell::FileDialogClientState>(L"isolated-browse-host", registry);
            services.inprocess_provider = provider;
            services.prefer_bridge = !ui;
            services.validation_verified = [](const auto& decision) { return shell::RuntimeContractCertified(decision); };
            services.show_native = [](IFileDialog*, HWND) { ++native_show; std::printf("INFO native Show fallback\n"); return HRESULT_FROM_WIN32(ERROR_CANCELLED); };
            services.on_native_reason = [](const char* reason, const IID*) {
                if (!std::strcmp(reason, "bridge-route-selected")) std::printf("INFO %s\n", reason);
            };
            services.observe_contract = [](const shell::ContractDecision& decision, bool, bool, size_t) {
                std::printf("INFO contract disposition=%u reason=%u category=%u\n", static_cast<unsigned>(decision.disposition), static_cast<unsigned>(decision.reason), static_cast<unsigned>(decision.validation_category));
            };
            if (!ui) services.run_bridge = [](const wire::Message& request, HWND, HANDLE) {
                ++bridge_calls; const auto& body = std::get<wire::Request>(request.body);
                wire::Message response; response.request_id = request.request_id;
                wire::Response value; value.operation = body.operation; value.status = cancelled ? wire::Status::Cancelled : wire::Status::Accepted;
                if (!cancelled) value.paths = {std::u16string(fixture.begin(), fixture.end())};
                response.body = value; return response;
            };
        };
        LONG attached = DetourTransactionBegin();
        if (!attached) attached = DetourUpdateThread(GetCurrentThread());
        if (!attached) attached = DetourAttach(reinterpret_cast<PVOID*>(&original_browse_w), reinterpret_cast<PVOID>(HookBrowseW));
        if (!attached) attached = DetourAttach(reinterpret_cast<PVOID*>(&original_browse_a), reinterpret_cast<PVOID>(HookBrowseA));
        if (!attached) attached = DetourTransactionCommit(); else DetourTransactionAbort();
        Check(attached == NO_ERROR, "install actual public W/A Detours entrypoints");
        Check(kCoverage == 63 && sizeof(PulseFileDialogRuntimeStatus) == 48,
            "coverage adds independent W/A bits while preserving v1 status ABI");
        if (!attached) {
            const auto trampoline_w = original_browse_w; const auto trampoline_a = original_browse_a;
            original_browse_w = NativeW; original_browse_a = NativeA;
            for (const UINT mode : modes) {
                std::printf("PULSE_BROWSE flags=%u\n", mode);
                w.ulFlags = a.ulFlags = mode; cancelled = false;
                Check(Consume(SHBrowseForFolderW(&w)), "public W accepted PIDL supports host filesystem consumption and CoTaskMemFree");
                Check(std::wstring(wide) == expected.szDisplayName && w.iImage == expected.iIcon, "public W commits native-equivalent folder name and system image index");
                Check(Consume(SHBrowseForFolderA(&a)), "public A accepted PIDL supports host filesystem consumption and CoTaskMemFree");
                Check(std::string(ansi) == expected_ansi && a.iImage == expected.iIcon, "public A commits native-equivalent lossless name and system image index");
                cancelled = true;
                wcscpy_s(wide, L"unchanged"); strcpy_s(ansi, "unchanged"); w.iImage = a.iImage = -99;
                Check(!SHBrowseForFolderW(&w) && std::wstring(wide) == L"unchanged" && w.iImage == -99, "public W cancellation preserves output fields");
                Check(!SHBrowseForFolderA(&a) && std::string(ansi) == "unchanged" && a.iImage == -99, "public A cancellation preserves output fields");
            }
            Check(native_calls == 0 && native_show == 0, "acceptance and cancellation never launch a second native dialog");
            if (ui) Check(ui_ready == 12, "twelve public calls display the real Pulse folder picker");
            else Check(bridge_calls == 12, "twelve public calls use the controlled folder bridge");
            w.ulFlags |= BIF_VALIDATE; a.ulFlags = w.ulFlags;
            w.lpfn = Callback; a.lpfn = Callback;
            SHBrowseForFolderW(&w); SHBrowseForFolderA(&a);
            w.lpfn = nullptr; a.lpfn = nullptr;
            PIDLIST_ABSOLUTE computer = nullptr;
            Check(SHGetSpecialFolderLocation(nullptr, CSIDL_DRIVES, &computer) == S_OK && computer,
                "build a real virtual This PC PIDL for the fallback fixture");
            w.ulFlags = a.ulFlags = BIF_RETURNONLYFSDIRS;
            w.pidlRoot = a.pidlRoot = computer;
            SHBrowseForFolderW(&w); SHBrowseForFolderA(&a);
            w.pidlRoot = nullptr; a.pidlRoot = nullptr;
            CoTaskMemFree(computer);
            w.ulFlags |= BIF_BROWSEINCLUDEFILES; a.ulFlags = w.ulFlags;
            SHBrowseForFolderW(&w); SHBrowseForFolderA(&a);
            Check(native_calls == 6 && native_show == 0, "virtual root, validation callback and non-folder requests retain native SHBrowse semantics");
            w.ulFlags = a.ulFlags = BIF_RETURNONLYFSDIRS;
            w.hwndOwner = a.hwndOwner = reinterpret_cast<HWND>(1);
            SHBrowseForFolderW(&w); SHBrowseForFolderA(&a);
            w.hwndOwner = a.hwndOwner = nullptr;
            std::thread mta([&] {
                const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                SHBrowseForFolderW(&w); SHBrowseForFolderA(&a);
                if (SUCCEEDED(co)) CoUninitialize();
            });
            mta.join();
            SHBrowseForFolderW(nullptr); SHBrowseForFolderA(nullptr);
            Check(native_calls == 12 && native_show == 0, "invalid owner MTA and null input decline before Show");
            const auto saved_services = legacy_test_services;
            legacy_test_services = {}; state.routing_enabled = false;
            SHBrowseForFolderW(&w); SHBrowseForFolderA(&a);
            state.routing_enabled = true; legacy_test_services = saved_services;
            Check(native_calls == 14 && native_show == 0, "disabled session keeps public W/A calls native");
            if (!ui) {
                cancelled = false;
                w.ulFlags = a.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_STATUSTEXT | BIF_DONTGOBELOWDOMAIN | BIF_RETURNFSANCESTORS;
                Check(Consume(SHBrowseForFolderW(&w)) && Consume(SHBrowseForFolderA(&a)),
                    "expanded safe BIF flags with NEWDIALOGSTYLE route through Pulse");
                w.ulFlags = a.ulFlags = BIF_RETURNONLYFSDIRS | BIF_STATUSTEXT;
                SHBrowseForFolderW(&w); SHBrowseForFolderA(&a);
                Check(native_calls == 16 && native_show == 0, "BIF_STATUSTEXT without NEWDIALOGSTYLE still declines to native");
                w.ulFlags = a.ulFlags = BIF_RETURNONLYFSDIRS;
                bool uninit_w = false, uninit_a = false;
                std::thread uninit([&] {
                    uninit_w = Consume(SHBrowseForFolderW(&w));
                    uninit_a = Consume(SHBrowseForFolderA(&a));
                });
                uninit.join();
                Check(uninit_w && uninit_a && native_calls == 16 && native_show == 0,
                    "uninitialized COM thread initializes balanced STA and routes through Pulse");
                PIDLIST_ABSOLUTE desktop = nullptr;
                Check(SHGetSpecialFolderLocation(nullptr, CSIDL_DESKTOP, &desktop) == S_OK && desktop,
                    "build empty Desktop PIDL matching WinForms FolderBrowserDialog");
                w.pidlRoot = a.pidlRoot = desktop;
                w.ulFlags = a.ulFlags = BIF_NEWDIALOGSTYLE;
                Check(Consume(SHBrowseForFolderW(&w)) && Consume(SHBrowseForFolderA(&a)) && native_calls == 16 && native_show == 0,
                    "WinForms FolderBrowserDialog (BIF_NEWDIALOGSTYLE without BIF_RETURNONLYFSDIRS + Desktop PIDL) routes through Pulse");
                w.pidlRoot = a.pidlRoot = nullptr;
                CoTaskMemFree(desktop);
                w.ulFlags = a.ulFlags = 0;
                Check(Consume(SHBrowseForFolderW(&w)) && Consume(SHBrowseForFolderA(&a)) && native_calls == 16 && native_show == 0,
                    "default zero-flag SHBrowseForFolder (ulFlags == 0) routes through Pulse");
            }

            if (ui) {
                cancelled = false;
                w.ulFlags = a.ulFlags = BIF_RETURNONLYFSDIRS | BIF_EDITBOX;
                const bool edit_w = Consume(SHBrowseForFolderW(&w));
                const bool edit_a = Consume(SHBrowseForFolderA(&a));
                Check(edit_w && edit_a,
                    "standalone BIF_EDITBOX uses the real Pulse picker for W and A");
                PIDLIST_ABSOLUTE root_id = nullptr;
                Check(SHParseDisplayName(fixture.c_str(), nullptr, &root_id, 0, nullptr) == S_OK && root_id,
                    "build filesystem root PIDL for the W and A callback fixture");
                w.pidlRoot = a.pidlRoot = root_id;
                w.ulFlags = a.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI | BIF_NONEWFOLDERBUTTON;
                const auto outside = root.parent_path().wstring();
                const int bytes = WideCharToMultiByte(CP_ACP, 0, outside.data(), static_cast<int>(outside.size()), nullptr, 0, nullptr, nullptr);
                std::string outside_ansi(static_cast<size_t>(bytes), '\0');
                if (bytes) WideCharToMultiByte(CP_ACP, 0, outside.data(), static_cast<int>(outside.size()), outside_ansi.data(), bytes, nullptr, nullptr);
                BrowseCallbackState callback{outside, fixture, std::move(outside_ansi)};
                w.lpfn = a.lpfn = RootedCallback;
                w.lParam = a.lParam = reinterpret_cast<LPARAM>(&callback);
                const bool root_w = Consume(SHBrowseForFolderW(&w));
                const bool root_a = Consume(SHBrowseForFolderA(&a));
                std::printf("BROWSE_ROOT_WA w=%d a=%d initialized=%u matched=%u escaped=%u enabled=%u\n",
                    root_w, root_a, callback.initialized, callback.matched, callback.escaped, callback.enabled);
                Check(root_w && root_a,
                    "rooted W and A with callbacks and hidden New Folder accept the allowed PIDL");
                Check(callback.initialized == 2 && callback.matched >= 2 && callback.enabled == 2 && callback.escaped == 0 && bytes > 0,
                    "W/A callbacks get live HWNDs, cookies and selection PIDLs; neither can leave the root");
                Check(native_calls == 14 && native_show == 0 && ui_ready == 16,
                    "all four new W/A cases open only the real Pulse picker");
                CoTaskMemFree(root_id);
            }
            original_browse_w = trampoline_w; original_browse_a = trampoline_a;
            LONG detached = DetourTransactionBegin();
            if (!detached) detached = DetourUpdateThread(GetCurrentThread());
            if (!detached) detached = DetourDetach(reinterpret_cast<PVOID*>(&original_browse_w), reinterpret_cast<PVOID>(HookBrowseW));
            if (!detached) detached = DetourDetach(reinterpret_cast<PVOID*>(&original_browse_a), reinterpret_cast<PVOID>(HookBrowseA));
            if (!detached) detached = DetourTransactionCommit(); else DetourTransactionAbort();
            Check(detached == NO_ERROR, "detach public entry hooks restores original SHBrowse exports");
            std::ifstream log(root / L"runtime-dialog-shows.jsonl");
            const std::string records((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
            const size_t shows = ui ? 16 : 20;
            Check(Count(records, "\"event\":\"dialog_show_begin\"") == shows && Count(records, "\"event\":\"dialog_show_terminal\"") == shows &&
                Count(records, "\"terminal_status\":\"accepted\"") == shows - 6 && Count(records, "\"terminal_status\":\"cancelled\"") == 6,
                "structured Show records pair all requests with exact accepted or cancelled terminals");
            Check(Count(records, "\"entry_family\":\"SHBROWSE_W\"") == shows && Count(records, "\"entry_family\":\"SHBROWSE_A\"") == shows &&
                Count(records, "\"ui_shown_known\":true") == (ui ? shows : 0u), "family identity and observed UI visibility remain exact");
            Check(Count(records, "\"identity_revision\":2") == 2 * shows &&
                Count(records, "\"creation_time_filetime\":0") == 0 &&
                Count(records, "\"app_instance_id\":0") == 0 &&
                Count(records, "\"event_sequence\":") == 2 * shows &&
                Count(records, "\"event_time_filetime\":") == 2 * shows,
                "local receipts carry cached image identity and ordered actual event timestamps");
            Check(records.find("probe.txt") == std::string::npos && records.find("selected_filename") == std::string::npos &&
                records.find("document_path") == std::string::npos,
                "application receipts never contain selected document paths");
            log.close();
            Check(DeleteFileW((root / L"runtime-dialog-shows.jsonl").c_str()) != FALSE, "remove isolated structured Show receipt");
        }
        legacy_test_services = {}; current = nullptr;
    }
    Check(std::filesystem::current_path() == original_cwd, "browse leaves host current directory unchanged");
    Check(DeleteFileW((root / L"probe.txt").c_str()) && RemoveDirectoryW(root.c_str()), "remove only isolated owned browse fixture");
    RegDeleteTreeW(HKEY_CURRENT_USER, registry.c_str());
    CoUninitialize();
    std::printf("BROWSE_%s checks=%u failures=%u\n", baseline ? "BASELINE" : ui ? "UI" : "CONTRACT", checks, failures);
    return failures ? 1 : 0;
}

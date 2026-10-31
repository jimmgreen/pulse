// Exercise the actual runtime hooks without installing Detours into this test.
#define PULSE_RUNTIME_ENTRY_TEST
#include "../shell_bridge/file_dialog_runtime.cpp"
#include <array>
#include <thread>
#include <type_traits>
#include <cderr.h>
#include <string_view>
namespace {
unsigned native_calls[3]{};
HRESULT WINAPI FakeCreate(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*) { ++native_calls[0]; return E_ACCESSDENIED; }
HRESULT WINAPI FakeEx(REFCLSID, IUnknown*, DWORD, COSERVERINFO*, DWORD, MULTI_QI*) { ++native_calls[1]; return E_ACCESSDENIED; }
HRESULT WINAPI FakeClass(REFCLSID, DWORD, LPVOID, REFIID, LPVOID*) { ++native_calls[2]; return E_ACCESSDENIED; }
unsigned child_native_calls = 0;
BOOL WINAPI PublicationNative(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) {
    ++child_native_calls; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
}
}
int PublicationTest() {
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool value, const char* label) {
        ++checks; if (!value) ++failures; std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    };
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"runtime-publication-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::error_code error;
    std::filesystem::create_directories(root.parent_path(), error);
    if (error || !std::filesystem::create_directory(root, error)) return 2;
    const auto rollback = root / L"rollback", success = root / L"success", removed = root / L"removed";
    if (!std::filesystem::create_directory(rollback, error) || !std::filesystem::create_directory(success, error) ||
        !std::filesystem::create_directory(removed, error)) return 2;
    config::original_create_process = PublicationNative;
    check(config::ConfigureChildRouting(rollback.wstring(), "unused-model.dll", ChildEnabled, ChildLog, ChildDiagnostic),
        "reproduce former ordering with routing configured before provider");
    STARTUPINFOW startup{sizeof(STARTUPINFOW)}; PROCESS_INFORMATION child{};
    config::HookCreateProcessW(nullptr, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child);
    auto observed = config::ReadChildRoutingDiagnostics();
    check(observed.count == 1 && observed.events[0].gate == config::ChildGateReason::NotPublished,
        "old ordering reproduces early creation g2 without launching a process");
    Configuration* captured = nullptr;
    {
        auto owner = std::make_unique<Configuration>(); owner->control = rollback.wstring(); owner->host = L"retained-model-host";
        PendingConfigurationPublication publication;
        check(publication.Publish(owner) && !owner, "publish complete provider before enabling child route");
        captured = publication.value;
        check(current.load() == captured && captured->routing_enabled.load(), "provider available before any ready receipt IO");
        config::HookCreateProcessW(nullptr, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child);
        const DWORD native_error = GetLastError(); observed = config::ReadChildRoutingDiagnostics();
        check(observed.count == 2 && observed.events[1].gate == config::ChildGateReason::ParentChain &&
            native_error == ERROR_ACCESS_DENIED && child_native_calls == 2, "early creation passes publication guard but still rejects invalid parent identity");
        check(GetFileAttributesW((rollback / config::kReadyFile).c_str()) == INVALID_FILE_ATTRIBUTES,
            "test transition occurs strictly before ready receipt");
    }
    check(!current.load() && captured && !captured->routing_enabled.load() && captured->host == L"retained-model-host",
        "failure revokes current and captured configuration without freeing captured storage");
    check(GetFileAttributesW((rollback / config::kDisableFile).c_str()) != INVALID_FILE_ATTRIBUTES,
        "failure leaves parent-chain disable marker for already-created children");
    {
        auto owner = std::make_unique<Configuration>(); owner->control = success.wstring();
        PendingConfigurationPublication publication;
        check(publication.Publish(owner), "publish normal success configuration");
        captured = publication.value; publication.Commit();
    }
    check(current.load() == captured && captured->routing_enabled.load() &&
        GetFileAttributesW((success / config::kDisableFile).c_str()) == INVALID_FILE_ATTRIBUTES,
        "committed normal path remains enabled with no false disable marker");
    captured->routing_enabled.store(false); current.store(nullptr);
    {
        auto owner = std::make_unique<Configuration>(); owner->control = removed.wstring();
        PendingConfigurationPublication publication; check(publication.Publish(owner), "publish marker-failure fixture");
        captured = publication.value;
        check(RemoveDirectoryW(removed.c_str()) != FALSE, "remove only owned empty control fixture");
        const DWORD revoke_error = publication.Revoke();
        check(revoke_error == ERROR_PATH_NOT_FOUND && !current.load() && !captured->routing_enabled.load(),
            "marker IO failure returns concrete error while revoking in-process authority");
    }
    bool clean = true;
    for (const auto& file : {rollback / config::kDisableFile, rollback / L"runtime-decisions.tsv"})
        if (!DeleteFileW(file.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) clean = false;
    check(clean && RemoveDirectoryW(rollback.c_str()) && RemoveDirectoryW(success.c_str()) && RemoveDirectoryW(root.c_str()),
        "clean only known unique publication fixture files");
    std::printf("Checks: %u, failures: %u\n", checks, failures); return failures ? 1 : 0;
}

unsigned legacy_native_calls = 0;
BOOL WINAPI LegacyNativeW(OPENFILENAMEW*) { ++legacy_native_calls; shell::legacy_dialog_error::SetBridge(9876); return FALSE; }
BOOL WINAPI LegacyNativeA(OPENFILENAMEA*) { ++legacy_native_calls; shell::legacy_dialog_error::SetBridge(9876); return FALSE; }
DWORD WINAPI LegacyNativeError() { return CDERR_NOTEMPLATE; }
int LegacyEntryTest() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    unsigned failures = 0, native = 0, bridge = 0;
    original_open_w = LegacyNativeW; original_save_w = LegacyNativeW;
    original_open_a = LegacyNativeA; original_save_a = LegacyNativeA;
    original_dialog_error = LegacyNativeError;
    auto check = [&](bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failures; };
    Configuration state; state.log_records = 256;
    state.control = std::filesystem::absolute(L"bench_data").wstring();
    state.host = L"isolated-test.exe"; state.routing_enabled = true; current = &state;
    const auto fixture = std::filesystem::absolute(std::filesystem::path(L"bench_data") / (L"legacy-entry-" + std::to_wstring(GetCurrentProcessId()) + L".txt")).wstring();
    HANDLE fixture_file = CreateFileW(fixture.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fixture_file == INVALID_HANDLE_VALUE) return 2;
    CloseHandle(fixture_file);
    const int ansi_count = WideCharToMultiByte(CP_ACP, 0, fixture.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string fixture_ansi(ansi_count, 0);
    WideCharToMultiByte(CP_ACP, 0, fixture.c_str(), -1, fixture_ansi.data(), ansi_count, nullptr, nullptr);
    fixture_ansi.pop_back();
    const auto expected_offset = fixture.find_last_of(L"\\") + 1;
    const auto expected_extension = fixture.find_last_of(L'.') + 1;
    const auto peer = fixture + L".peer.txt";
    fixture_file = CreateFileW(peer.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fixture_file == INVALID_HANDLE_VALUE) { DeleteFileW(fixture.c_str()); return 2; }
    CloseHandle(fixture_file);
    bool multiple = false;
    bool cancel = false;
    legacy_test_services = [&](shell::FileDialogBridgeServices& services) {
        services.access_policy = shell::AccessPolicy::None;
        services.save_access_policy = shell::AccessPolicy::None;
        services.validation_verified = [](const auto&) { return true; }; // Isolated fixed response only.
        services.show_native = [&](IFileDialog*, HWND) { ++native; return HRESULT_FROM_WIN32(ERROR_CANCELLED); };
        services.run_bridge = [&](const wire::Message& request, HWND, HANDLE) {
            ++bridge; const auto& body = std::get<wire::Request>(request.body);
            wire::Message response; response.request_id = request.request_id;
            wire::Response value; value.operation = body.operation;
            value.status = cancel ? wire::Status::Cancelled : wire::Status::Accepted;
            if (!cancel) {
                value.paths = {std::u16string(fixture.begin(), fixture.end())};
                if (multiple) value.paths.push_back(std::u16string(peer.begin(), peer.end()));
            }
            response.body = value; return response;
        };
    };
    wchar_t file[512]{}; char ansi[512]{};
    OPENFILENAMEW w{}; w.lStructSize = sizeof(w); w.lpstrFile = file; w.nMaxFile = 512;
    w.lpstrInitialDir = L"C:\\"; w.lpstrFilter = L"Text\0*.txt\0\0";
    w.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    OPENFILENAMEA a{}; a.lStructSize = sizeof(a); a.lpstrFile = ansi; a.nMaxFile = 512;
    a.lpstrInitialDir = "C:\\"; a.lpstrFilter = "Text\0*.txt\0\0"; a.Flags = w.Flags;
    check(ObserveOpenW(&w) && std::wstring(file) == fixture && w.nFileOffset == expected_offset && w.nFileExtension == expected_extension && HookDialogError() == 0, "runtime OpenW commits path offsets and error");
    check(ObserveOpenA(&a) && std::string(ansi) == fixture_ansi && a.nFileOffset == expected_offset && a.nFileExtension == expected_extension && HookDialogError() == 0, "runtime OpenA commits byte offsets");
    multiple = true; file[0] = 0; ansi[0] = 0; w.Flags |= OFN_ALLOWMULTISELECT; a.Flags = w.Flags;
    const auto parent = std::filesystem::path(fixture).parent_path().wstring();
    const auto leaf = std::filesystem::path(fixture).filename().wstring();
    check(ObserveOpenW(&w) && std::wstring(file) == parent && w.nFileOffset == parent.size() + 1 &&
        std::wstring(file + w.nFileOffset) == leaf && w.nFileExtension == 0, "runtime W multiselect produces directory plus leaves");
    check(ObserveOpenA(&a) && a.nFileOffset == parent.size() + 1 && a.nFileExtension == 0 &&
        std::string(ansi + a.nFileOffset) == std::filesystem::path(fixture).filename().string(), "runtime A multiselect produces byte offsets");
    multiple = false; w.Flags &= ~OFN_ALLOWMULTISELECT;
    w.Flags = (w.Flags & ~OFN_FILEMUSTEXIST) | OFN_OVERWRITEPROMPT | OFN_NOREADONLYRETURN;
    a.Flags = w.Flags; file[0] = 0; ansi[0] = 0;
    check(ObserveSaveW(&w) && HookDialogError() == 0, "runtime SaveW reaches bridge");
    check(ObserveSaveA(&a) && HookDialogError() == 0, "runtime SaveA reaches bridge");
    w.lpstrDefExt = L"csv"; file[0] = 0;
    check(ObserveSaveW(&w) && (w.Flags & OFN_EXTENSIONDIFFERENT), "different extension sets output flag");
    check(ObserveSaveW(&w) && (w.Flags & OFN_EXTENSIONDIFFERENT), "same OFN with output flag and absolute result can be reused");
    w.lpstrDefExt = nullptr;
    cancel = true; wcscpy_s(file, L"unchanged.txt"); strcpy_s(ansi, "unchanged.txt");
    check(!ObserveSaveW(&w) && std::wstring(file) == L"unchanged.txt" && HookDialogError() == 0, "runtime Unicode cancellation keeps buffer");
    check(!ObserveSaveA(&a) && std::string(ansi) == "unchanged.txt" && HookDialogError() == 0, "runtime ANSI cancellation keeps buffer");
    cancel = false; file[0] = 0; ansi[0] = 0; w.nMaxFile = 2; a.nMaxFile = 2;
    check(!ObserveSaveW(&w) && HookDialogError() == FNERR_BUFFERTOOSMALL && file[0] == fixture.size() + 1, "runtime W short buffer carries required size");
    WORD required = 0;
    const BOOL short_result = ObserveSaveA(&a); std::memcpy(&required, ansi, sizeof(required));
    check(!short_result && HookDialogError() == FNERR_BUFFERTOOSMALL && required == fixture_ansi.size() + 1, "runtime A short buffer uses first WORD");
    check(native == 0 && bridge == 12, "all twelve runtime calls dispatched once without native replay");
    original_open_w = LegacyNativeW; original_save_w = LegacyNativeW;
    original_open_a = LegacyNativeA; original_save_a = LegacyNativeA;
    original_dialog_error = LegacyNativeError;
    w.Flags |= OFN_ENABLEHOOK; a.Flags |= OFN_ENABLETEMPLATE;
    check(!ObserveOpenW(&w) && HookDialogError() == CDERR_NOTEMPLATE, "hook OpenW preserves native error after nested source mutation");
    check(!ObserveSaveW(&w) && HookDialogError() == CDERR_NOTEMPLATE, "hook SaveW falls back before Show");
    check(!ObserveOpenA(&a) && HookDialogError() == CDERR_NOTEMPLATE, "template OpenA preserves native error");
    check(!ObserveSaveA(&a) && HookDialogError() == CDERR_NOTEMPLATE, "template SaveA falls back before Show");
    check(legacy_native_calls == 4 && bridge == 12, "unsupported modes never dispatch a replacement");
    check(DeleteFileW(peer.c_str()) && DeleteFileW(fixture.c_str()), "remove isolated legacy fixture");
    legacy_test_services = {}; current = nullptr;
    std::printf("Legacy failures: %u\n", failures); return failures ? 1 : 0;
}

namespace {
bool legacy_auto_cancel = false, legacy_cwd_observed = false;
unsigned legacy_ui_ready = 0, legacy_file_ok = 0;
std::wstring legacy_expected_folder;
struct LegacyUiForward { const PulsePickerUiV2Callbacks* callbacks; };
HRESULT WINAPI LegacyAutomatedEvent(void* context, const PulsePickerUiV2Event* event) {
    const auto* original = static_cast<LegacyUiForward*>(context)->callbacks;
    const HRESULT result = original && original->callback ? original->callback(original->context, event) : S_OK;
    if (event->kind == PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGED) {
        wchar_t cwd[32768]{}; GetCurrentDirectoryW(32768, cwd);
        legacy_cwd_observed |= legacy_expected_folder == cwd;
    }
    if (event->kind == PULSE_PICKER_UI_V2_EVENT_FILE_OK) ++legacy_file_ok;
    if (event->kind == PULSE_PICKER_UI_V2_EVENT_READY) {
        ++legacy_ui_ready;
        if (legacy_auto_cancel) PostMessageW(event->window, WM_CLOSE, 0, 0);
        else PostMessageW(GetDlgItem(event->window, 102), WM_KEYDOWN, VK_RETURN, 0);
    }
    return result;
}
}
int LegacyUiTest(bool save, bool ansi, bool filter_tail = false, bool cancel_ui = false) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
    const auto dll = std::filesystem::path(executable).parent_path() / L"PulsePickerUiProof.dll";
    HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) { std::printf("UI load error=%lu\n", GetLastError()); return 2; }
    using GetApi = HRESULT (WINAPI*)(uint32_t, uint32_t, PulsePickerUiV2Api*);
    const auto get = reinterpret_cast<GetApi>(GetProcAddress(module, "PulsePickerUiGetApiV2"));
    auto provider = std::make_shared<shell::FileDialogInprocessProvider>();
    if (!get || get(2, sizeof(provider->api), &provider->api) != S_OK) return 2;
    provider->module_lifetime = std::shared_ptr<void>(module, [](void* handle) { FreeLibrary(static_cast<HMODULE>(handle)); });
    provider->allow_unverified_proof = true;
    if (filter_tail) {
        static decltype(provider->api.Show) real_show = nullptr;
        real_show = provider->api.Show;
        provider->api.Show = [](PulsePickerUiV2Session session, const PulsePickerUiV2Config* config, const PulsePickerUiV2Callbacks* callbacks, PulsePickerUiV2Result* result)->HRESULT {
            LegacyUiForward forward{callbacks}; PulsePickerUiV2Callbacks automated = *callbacks;
            automated.context = &forward; automated.callback = LegacyAutomatedEvent;
            return real_show(session, config, &automated, result);
        };
        legacy_auto_cancel = cancel_ui; legacy_ui_ready = 0; legacy_file_ok = 0; legacy_cwd_observed = false;
    }
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") / (L"legacy-ui-" + std::to_wstring(GetCurrentProcessId())));
    if (!std::filesystem::create_directory(root)) return 2;
    legacy_expected_folder = root.wstring();
    const auto original_cwd = std::filesystem::current_path();
    const auto fixture = root / L"probe.txt";
    HANDLE file = CreateFileW(fixture.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 2;
    constexpr char probe_bytes[] = "Pulse Legacy Open fixture\r\n";
    constexpr char save_bytes[] = "Pulse Legacy Save roundtrip\r\n";
    DWORD written = 0;
    const bool seeded = WriteFile(file, probe_bytes, sizeof(probe_bytes) - 1, &written, nullptr) && written == sizeof(probe_bytes) - 1;
    CloseHandle(file);
    if (!seeded) { DeleteFileW(fixture.c_str()); RemoveDirectoryW(root.c_str()); return 2; }
    Configuration state; state.log_records = 256; state.control = root.wstring(); state.host = L"isolated-test.exe"; state.routing_enabled = true; current = &state;
    unsigned native_fallback = 0;
    legacy_test_services = [&](shell::FileDialogBridgeServices& services) {
        services.inprocess_provider = provider;
        services.validation_verified = [](const shell::ContractDecision& decision) {
            return shell::RuntimeContractCertified(decision) || shell::RuntimeLocalSaveContractCertified(decision);
        };
        services.show_native = [&](IFileDialog*, HWND) { ++native_fallback; return E_FAIL; };
    };
    original_open_w = LegacyNativeW; original_save_w = LegacyNativeW;
    original_open_a = LegacyNativeA; original_save_a = LegacyNativeA;
    original_dialog_error = LegacyNativeError;
    const DWORD flags = filter_tail ? 0x00881A04u : OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT | OFN_PATHMUSTEXIST |
        (save ? OFN_OVERWRITEPROMPT | OFN_NOREADONLYRETURN : OFN_FILEMUSTEXIST);
    HANDLE ended = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ended) { DeleteFileW(fixture.c_str()); RemoveDirectoryW(root.c_str()); return 2; }
    const DWORD ui_thread = GetCurrentThreadId();
    std::thread watchdog([ended, ui_thread] {
        if (WaitForSingleObject(ended, 150000) == WAIT_TIMEOUT) {
            std::printf("LEGACY_UI timeout: closing only test-thread windows\n");
            EnumThreadWindows(ui_thread, [](HWND window, LPARAM) -> BOOL { PostMessageW(window, WM_CLOSE, 0, 0); return TRUE; }, 0);
        }
    });
    BOOL accepted = FALSE;
    std::wstring returned_path;
    std::printf("LEGACY_UI mode=%s encoding=%s fixture=%ls\n", save ? "save" : "open", ansi ? "A" : "W", root.c_str());
    if (ansi) {
        char buffer[1024]{}; strcpy_s(buffer, save ? "output.txt" : "probe.txt");
        const auto directory = root.string();
        OPENFILENAMEA input{}; input.lStructSize = sizeof(input); input.lpstrFile = buffer; input.nMaxFile = 1024;
        input.lpstrTitle = save ? "Legacy test: Save A" : "Legacy test: Open A";
        input.lpstrInitialDir = directory.c_str(); input.lpstrFilter = filter_tail ? "Text\0;*.txt;;*.zip;\0\0" : "Text\0*.txt\0\0"; input.Flags = flags;
        accepted = save ? ObserveSaveA(&input) : ObserveOpenA(&input);
        if (accepted) {
            const int units = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, buffer, -1, nullptr, 0);
            if (units > 0) {
                returned_path.resize(units);
                if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, buffer, -1, returned_path.data(), units)) returned_path.pop_back();
                else returned_path.clear();
            }
        }
        std::printf("LEGACY_UI result=%u error=%lu path=%s offset=%u extension=%u\n", accepted, HookDialogError(), buffer, input.nFileOffset, input.nFileExtension);
    } else {
        wchar_t buffer[1024]{}; wcscpy_s(buffer, save ? L"output.txt" : L"probe.txt");
        OPENFILENAMEW input{}; input.lStructSize = sizeof(input); input.lpstrFile = buffer; input.nMaxFile = 1024;
        input.lpstrTitle = save ? L"Legacy test: Save W" : L"Legacy test: Open W";
        input.lpstrInitialDir = root.c_str(); input.lpstrFilter = filter_tail ? L"Text\0;*.txt;;*.zip;\0\0" : L"Text\0*.txt\0\0"; input.Flags = flags;
        accepted = save ? ObserveSaveW(&input) : ObserveOpenW(&input);
        if (accepted) returned_path = buffer;
        std::printf("LEGACY_UI result=%u error=%lu path=%ls offset=%u extension=%u\n", accepted, HookDialogError(), buffer, input.nFileOffset, input.nFileExtension);
    }
    SetEvent(ended); watchdog.join(); CloseHandle(ended);
    const bool cwd_expected = std::filesystem::current_path() == (cancel_ui ? original_cwd : filter_tail ? root : original_cwd);
    std::filesystem::current_path(original_cwd);
    const auto expected = save ? root / L"output.txt" : fixture;
    const bool path_matches = accepted && returned_path == expected.wstring();
    bool output_created = false, consumed = false;
    // Never operate on an arbitrary picker result: only the exact owned target.
    if (path_matches) {
        bool ready = true;
        if (save) {
            HANDLE output = CreateFileW(returned_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            ready = output != INVALID_HANDLE_VALUE;
            if (ready) {
                output_created = true;
                written = 0;
                ready = WriteFile(output, save_bytes, sizeof(save_bytes) - 1, &written, nullptr) && written == sizeof(save_bytes) - 1;
                CloseHandle(output);
            }
        }
        if (ready) {
            HANDLE input = CreateFileW(returned_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (input != INVALID_HANDLE_VALUE) {
                char bytes[128]{}; DWORD read = 0; LARGE_INTEGER size{};
                const auto expected_bytes = save ? save_bytes : probe_bytes;
                const DWORD expected_count = static_cast<DWORD>(save ? sizeof(save_bytes) - 1 : sizeof(probe_bytes) - 1);
                consumed = GetFileSizeEx(input, &size) && size.QuadPart == expected_count &&
                    ReadFile(input, bytes, sizeof(bytes), &read, nullptr) && read == expected_count &&
                    std::memcmp(bytes, expected_bytes, expected_count) == 0;
                CloseHandle(input);
            }
        }
    }
    bool clean = !output_created || DeleteFileW(expected.c_str());
    if (!DeleteFileW(fixture.c_str())) clean = false;
    if (!RemoveDirectoryW(root.c_str())) clean = false;
    std::printf("LEGACY_UI path_matches=%u host_consumed=%u native=%u native_api=%u cleanup=%u\n",
        path_matches, consumed, native_fallback, legacy_native_calls, clean);
    legacy_test_services = {}; current = nullptr;
    if (filter_tail) {
        std::printf("LEGACY_PULSE ready=%u file_ok=%u cwd_during=%u cwd_after=%u cancel=%u\n", legacy_ui_ready, legacy_file_ok, legacy_cwd_observed, cwd_expected, cancel_ui);
        return (cancel_ui ? !accepted && HookDialogError() == 0 : accepted && path_matches && consumed) &&
            legacy_ui_ready == 1 && legacy_file_ok == (cancel_ui ? 0u : 1u) && legacy_cwd_observed && cwd_expected && !native_fallback && !legacy_native_calls && clean ? 0 : 1;
    }
    return accepted && path_matches && consumed && !native_fallback && !legacy_native_calls && clean ? 0 : 1;
}

namespace {
thread_local DWORD other_dialog_error = CDERR_INITIALIZATION;
thread_local const void* other_dialog_input = nullptr;
thread_local bool other_dialog_entry_native = false;
thread_local BOOL other_dialog_bool_result = FALSE;
thread_local HRESULT other_dialog_hr_result = E_FAIL;
thread_local HWND other_dialog_hwnd_result = nullptr;
constexpr DWORD kOtherLastError = 0x1234;
DWORD WINAPI OtherDialogError() { return other_dialog_error; }
template<class Input> BOOL WINAPI OtherBoolDialog(Input* input) {
    other_dialog_input = input;
    other_dialog_entry_native = HookDialogError() == other_dialog_error;
    // Simulate a nested Pulse file-dialog completion inside the native callback.
    shell::legacy_dialog_error::SetBridge(9876);
    SetLastError(kOtherLastError);
    return other_dialog_bool_result;
}
template<class Input> HRESULT WINAPI OtherHresultDialog(Input* input) {
    other_dialog_input = input;
    other_dialog_entry_native = HookDialogError() == other_dialog_error;
    shell::legacy_dialog_error::SetBridge(9876);
    SetLastError(kOtherLastError);
    return other_dialog_hr_result;
}
template<class Input> HWND WINAPI OtherWindowDialog(Input* input) {
    other_dialog_input = input;
    other_dialog_entry_native = HookDialogError() == other_dialog_error;
    shell::legacy_dialog_error::SetBridge(9876);
    SetLastError(kOtherLastError);
    return other_dialog_hwnd_result;
}
BOOL WINAPI OtherCppException(LPCHOOSECOLORW) {
    shell::legacy_dialog_error::SetBridge(9876);
    throw 73;
}
constexpr DWORD kOtherException = 0xE0421001;
BOOL WINAPI OtherSehException(LPCHOOSECOLORW) {
    shell::legacy_dialog_error::SetBridge(9876);
    RaiseException(kOtherException, 0, 0, nullptr);
    return FALSE;
}
bool CatchOtherSehException() {
    __try {
        HookChooseColorW(nullptr);
    } __except (GetExceptionCode() == kOtherException ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
    return false;
}
bool other_nested_source_ok = false;
BOOL WINAPI OtherNestedDialog(LPCHOOSECOLORW) {
    const bool before = HookDialogError() == other_dialog_error;
    shell::legacy_dialog_error::SetBridge(4444);
    CHOOSEFONTW inner{};
    const BOOL nested = HookChooseFontW(&inner);
    other_nested_source_ok = before && nested == other_dialog_bool_result && HookDialogError() == other_dialog_error;
    shell::legacy_dialog_error::SetBridge(5555);
    return TRUE;
}
}
int OtherCommonDialogErrorTest() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    unsigned checks = 0, failures = 0;
    auto check = [&](bool good, const char* label) {
        ++checks; if (!good) ++failures;
        std::printf("[%s] %s\n", good ? "PASS" : "FAIL", label);
    };
    original_dialog_error = OtherDialogError;
    auto bool_case = [&](auto& original, auto hook, auto* input, const char* label) {
        using Input = std::remove_pointer_t<decltype(input)>;
        original = &OtherBoolDialog<Input>;
        // Zero is cancellation; a nonzero code is native failure. Successful
        // common dialogs have unspecified extended error: only delegation is tested.
        for (BOOL expected : {FALSE, TRUE}) for (DWORD native_error : {DWORD(0), DWORD(CDERR_INITIALIZATION)}) {
            other_dialog_error = native_error;
            other_dialog_bool_result = expected;
            shell::legacy_dialog_error::SetBridge(1234);
            const BOOL result = hook(input);
            const DWORD last_error = GetLastError();
            check(result == expected && other_dialog_input == input && other_dialog_entry_native &&
                HookDialogError() == other_dialog_error && last_error == kOtherLastError, label);
        }
    };
    auto hr_case = [&](auto& original, auto hook, auto* input, const char* label) {
        using Input = std::remove_pointer_t<decltype(input)>;
        original = &OtherHresultDialog<Input>;
        for (HRESULT expected : {S_OK, S_FALSE, E_FAIL}) {
            other_dialog_hr_result = expected;
            shell::legacy_dialog_error::SetBridge(1234);
            const HRESULT result = hook(input);
            const DWORD last_error = GetLastError();
            check(result == expected && other_dialog_input == input && other_dialog_entry_native &&
                HookDialogError() == other_dialog_error && last_error == kOtherLastError, label);
        }
    };
    auto hwnd_case = [&](auto& original, auto hook, auto* input, const char* label) {
        using Input = std::remove_pointer_t<decltype(input)>;
        original = &OtherWindowDialog<Input>;
        for (HWND expected : {HWND(nullptr), reinterpret_cast<HWND>(static_cast<ULONG_PTR>(0x1234))})
            for (DWORD native_error : {DWORD(0), DWORD(FRERR_BUFFERLENGTHZERO)}) {
                other_dialog_error = native_error;
                other_dialog_hwnd_result = expected;
                shell::legacy_dialog_error::SetBridge(1234);
                const HWND result = hook(input);
                const DWORD last_error = GetLastError();
                check(result == expected && other_dialog_input == input && other_dialog_entry_native &&
                    HookDialogError() == other_dialog_error && last_error == kOtherLastError, label);
            }
    };
    CHOOSECOLORW color_w{}; CHOOSECOLORA color_a{};
    CHOOSEFONTW font_w{}; CHOOSEFONTA font_a{};
    PRINTDLGW print_w{}; PRINTDLGA print_a{};
    PRINTDLGEXW print_ex_w{}; PRINTDLGEXA print_ex_a{};
    PAGESETUPDLGW page_w{}; PAGESETUPDLGA page_a{};
    bool_case(original_ChooseColorW, HookChooseColorW, &color_w, "ChooseColorW keeps native BOOL, pointer, last-error and error source");
    bool_case(original_ChooseColorA, HookChooseColorA, &color_a, "ChooseColorA keeps native BOOL, pointer, last-error and error source");
    bool_case(original_ChooseFontW, HookChooseFontW, &font_w, "ChooseFontW keeps native BOOL, pointer, last-error and error source");
    bool_case(original_ChooseFontA, HookChooseFontA, &font_a, "ChooseFontA keeps native BOOL, pointer, last-error and error source");
    bool_case(original_PrintDlgW, HookPrintDlgW, &print_w, "PrintDlgW keeps native BOOL, pointer, last-error and error source");
    bool_case(original_PrintDlgA, HookPrintDlgA, &print_a, "PrintDlgA keeps native BOOL, pointer, last-error and error source");
    hr_case(original_PrintDlgExW, HookPrintDlgExW, &print_ex_w, "PrintDlgExW keeps exact HRESULT, pointer, last-error and error source");
    hr_case(original_PrintDlgExA, HookPrintDlgExA, &print_ex_a, "PrintDlgExA keeps exact HRESULT, pointer, last-error and error source");
    bool_case(original_PageSetupDlgW, HookPageSetupDlgW, &page_w, "PageSetupDlgW keeps native BOOL, pointer, last-error and error source");
    bool_case(original_PageSetupDlgA, HookPageSetupDlgA, &page_a, "PageSetupDlgA keeps native BOOL, pointer, last-error and error source");
    FINDREPLACEW find_w{}; FINDREPLACEA find_a{};
    hwnd_case(original_FindTextW, HookFindTextW, &find_w, "FindTextW keeps exact HWND and delegates FRERR source");
    hwnd_case(original_FindTextA, HookFindTextA, &find_a, "FindTextA keeps exact HWND and delegates FRERR source");
    hwnd_case(original_ReplaceTextW, HookReplaceTextW, &find_w, "ReplaceTextW keeps exact HWND and delegates FRERR source");
    hwnd_case(original_ReplaceTextA, HookReplaceTextA, &find_a, "ReplaceTextA keeps exact HWND and delegates FRERR source");
    original_ChooseColorW = OtherNestedDialog;
    shell::legacy_dialog_error::SetBridge(1234);
    check(HookChooseColorW(&color_w) == TRUE && other_nested_source_ok && HookDialogError() == other_dialog_error,
        "nested native dialog and nested bridge completions restore the outer native error source");
    original_ChooseColorW = OtherCppException;
    bool caught_cpp = false;
    try { HookChooseColorW(nullptr); } catch (int value) { caught_cpp = value == 73; }
    check(caught_cpp && HookDialogError() == other_dialog_error, "C++ exception propagates unchanged and finally resets source");
    original_ChooseColorW = OtherSehException;
    check(CatchOtherSehException() && HookDialogError() == other_dialog_error, "SEH exception propagates unchanged and finally resets source");
    shell::legacy_dialog_error::SetBridge(2468);
    bool worker_good = false;
    std::thread worker([&] {
        other_dialog_error = CDERR_NOTEMPLATE;
        shell::legacy_dialog_error::SetBridge(1357);
        CHOOSEFONTA input{};
        HookChooseFontA(&input);
        worker_good = other_dialog_entry_native && HookDialogError() == CDERR_NOTEMPLATE;
    });
    worker.join();
    check(worker_good && HookDialogError() == 2468, "other thread reset cannot overwrite this thread's bridge error");
    std::printf("Other common dialog checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--legacy-pulse-filter-tail") {
        unsigned failed = 0;
        for (bool ansi : {false, true}) for (bool cancel : {false, true}) failed += LegacyUiTest(false, ansi, true, cancel) != 0;
        std::printf("Legacy Pulse filter-tail failures=%u\n", failed); return failed ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--other-common-dialog-errors") return OtherCommonDialogErrorTest();
    if (argc == 2 && std::string_view(argv[1]) == "--legacy-ui-all") {
        int failures = 0;
        for (const bool ansi : {false, true}) for (const bool save : {false, true}) failures += LegacyUiTest(save, ansi) != 0;
        std::printf("LEGACY_UI_ALL cases=4 failures=%d\n", failures);
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]).starts_with("--legacy-ui-")) {
        const std::string_view mode(argv[1]);
        return LegacyUiTest(mode.find("save") != mode.npos, mode.ends_with("-a"));
    }
    if (argc == 2 && std::string_view(argv[1]) == "--legacy-only") return LegacyEntryTest();
    if (argc == 2 && std::string_view(argv[1]) == "--publication-only") {
        std::setvbuf(stdout, nullptr, _IONBF, 0); return PublicationTest();
    }
    if (argc != 1) return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    unsigned checks = 0, failures = 0;
    auto check = [&](bool value, const char* label) {
        ++checks; if (!value) ++failures;
        std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    };
    original_create = FakeCreate; original_ex = FakeEx; original_class = FakeClass;
    auto invoke = [&](REFCLSID clsid) {
        void* output = nullptr;
        MULTI_QI result{&IID_IFileOpenDialog, nullptr, S_OK};
        check(HookCreate(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, &output) == E_ACCESSDENIED,
            "single entry preserves native failure");
        check(HookEx(clsid, nullptr, CLSCTX_INPROC_SERVER, nullptr, 1, &result) == E_ACCESSDENIED,
            "Ex entry preserves native failure");
        check(HookClass(clsid, CLSCTX_INPROC_SERVER, nullptr, IID_IClassFactory, &output) == E_ACCESSDENIED,
            "class entry preserves native failure");
    };
    invoke(CLSID_FileOpenDialog);
    check(entry_counts[0].early == 1 && entry_counts[1].early == 1 && entry_counts[2].early == 1,
        "all three entries count target activation before readiness");
    const auto directory = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"runtime-entry-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::error_code error;
    std::filesystem::create_directories(directory.parent_path(), error);
    if (error || !std::filesystem::create_directory(directory, error)) return 2;
    Configuration state; state.log_records = 256; state.control = directory.wstring();
    current.store(&state);
    invoke(CLSID_FileOpenDialog);
    check(admitted == 0, "disabled configuration does not admit a dialog");
    state.routing_enabled.store(true);
    invoke(CLSID_FileOpenDialog);
    invoke(CLSID_ShellLink);
    check(native_calls[0] == 4 && native_calls[1] == 4 && native_calls[2] == 4,
        "every native call executes exactly once across early disabled enabled and unrelated paths");
    check(entry_counts[0].targets == 3 && entry_counts[1].targets == 3 && entry_counts[2].targets == 3,
        "unrelated COM calls do not inflate target activation counts");
    current.store(nullptr);
    std::filesystem::remove(directory / L"runtime-decisions.tsv", error);
    check(!error && std::filesystem::remove(directory, error) && !error, "owned diagnostic fixture cleaned");
    std::printf("Checks: %u, failures: %u\n", checks, failures);
    return failures ? 1 : 0;
}



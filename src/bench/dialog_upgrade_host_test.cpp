#include "../common/dialog_bundle.h"
#include "../shell_bridge/file_dialog_ui_provider.h"
#include <objbase.h>
#include <filesystem>
#include <cstdio>
#include <share.h>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr HRESULT kCancelled = HRESULT_FROM_WIN32(ERROR_CANCELLED);
struct Round {
    const PulsePickerUiV2Api* api = nullptr;
    PulsePickerUiV2Session session = 0;
    HWND owner = nullptr;
    unsigned ready = 0;
    bool actual_window = false;
    bool owner_disabled = false;
    HRESULT closed = E_FAIL;
};
HRESULT WINAPI Callback(void* context, const PulsePickerUiV2Event* event) {
    auto& round = *static_cast<Round*>(context);
    if (event->kind != PULSE_PICKER_UI_V2_EVENT_READY) return S_OK;
    ++round.ready;
    round.actual_window = event->window && IsWindow(event->window) && IsWindowVisible(event->window);
    round.owner_disabled = IsWindow(round.owner) && !IsWindowEnabled(round.owner);
    round.closed = round.api->Close(round.session, event->show_id, kCancelled);
    return S_OK;
}
bool WriteNew(const std::filesystem::path& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
        written == text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}
bool Ready(const std::filesystem::path& directory, int round, const std::string& digest) {
    const auto name = L"ready-" + std::to_wstring(round);
    const auto temporary = directory / (name + L".tmp");
    const std::string content = "{\"pid\":" + std::to_string(GetCurrentProcessId()) +
        ",\"round\":" + std::to_string(round) + ",\"manifest_sha256\":\"" + digest + "\"}\n";
    return WriteNew(temporary, content) && MoveFileW(temporary.c_str(), (directory / name).c_str());
}
bool LocalDirectory(const std::filesystem::path& path) {
    const auto text = path.wstring();
    if (!path.is_absolute() || text.size() < 3 || text[1] != L':' || text[2] != L'\\' ||
        path.lexically_normal() != path) return false;
    auto current = path;
    for (;;) {
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const auto parent = current.parent_path();
        if (parent == current) return true;
        current = parent;
    }
}
bool WaitSignal(const std::filesystem::path& file, ULONGLONG deadline) {
    while (GetTickCount64() < deadline) {
        const DWORD attributes = GetFileAttributesW(file.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
            return !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return false;
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 25, QS_ALLINPUT);
    }
    return false;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 4 || (wcscmp(argv[1], L"--hold") && wcscmp(argv[1], L"--once"))) return 2;
    const bool hold = wcscmp(argv[1], L"--hold") == 0;
    const std::filesystem::path installed(argv[2]), control(argv[3]);
    std::vector<wchar_t> data(32768);
    const DWORD length = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data.data(), static_cast<DWORD>(data.size()));
    if (!LocalDirectory(installed) || !LocalDirectory(control) || !length || length >= data.size() ||
        std::filesystem::path(data.data()) != control || !std::filesystem::is_empty(control)) return 2;
    if (!WriteNew(control / L"host-claim", std::to_string(GetCurrentProcessId()))) return 2;
    FILE* log = _wfsopen((control / L"host.log").c_str(), L"w", _SH_DENYNO);
    if (!log) return 2;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label); fflush(log); failures += !ok;
    };
    HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!finished) { fclose(log); return 2; }
    const ULONGLONG deadline = GetTickCount64() + 120000;
    std::thread watchdog([finished, control] {
        if (WaitForSingleObject(finished, 120000) == WAIT_TIMEOUT) {
            WriteNew(control / L"watchdog-failed", "120-second host watchdog expired\n");
            TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
        }
    });
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    check(SUCCEEDED(apartment), "STA initialized");
    if (SUCCEEDED(apartment)) {
        pulse::dialog_bundle::BundleLease lease;
        DWORD resolved = ERROR_GEN_FAILURE;
        std::thread resolver([&] { resolved = pulse::dialog_bundle::ResolveInstalledBundle(installed.wstring(), lease); });
        resolver.join();
        check(resolved == ERROR_SUCCESS && lease, "installed pointer resolves and retains complete bundle lease");
        fprintf(log, "resolve_error=%lu pid=%lu digest=%s\n", resolved, GetCurrentProcessId(), lease.ManifestDigest().c_str());
        std::shared_ptr<const pulse::shell_bridge::FileDialogInprocessProvider> provider;
        const HRESULT loaded = lease ? pulse::shell_bridge::LoadFileDialogUiProvider(lease.Host(), provider, true) : E_FAIL;
        check(loaded == S_OK && provider, "actual private UI provider loaded");
        fprintf(log, "load_hr=%08lx\n", static_cast<unsigned long>(loaded)); fflush(log);
        if (provider) {
            HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse isolated upgrade host", WS_OVERLAPPEDWINDOW,
                100, 100, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            check(owner != nullptr, "real owner created");
            if (owner) ShowWindow(owner, SW_SHOWNOACTIVATE);
            PulsePickerUiV2Initialize init{}; init.size = sizeof(init); init.version = PULSE_PICKER_UI_V2_VERSION;
            PulsePickerUiV2Session session = 0;
            const auto& api = provider->api;
            const HRESULT initialized = owner ? api.Initialize(&init, &session) : E_FAIL;
            check(initialized == S_OK && session, "real provider session initialized");
            if (session) {
                const auto folder = control.wstring();
                PulsePickerUiV2Config config{}; config.size = sizeof(config); config.version = PULSE_PICKER_UI_V2_VERSION;
                config.owner = owner; config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
                config.initial_folder = {folder.c_str(), static_cast<uint32_t>(folder.size())};
                check(api.ValidateConfig(session, &config) == S_OK, "isolated local open config accepted");
                for (int index = 0; index < (hold ? 3 : 1) && !failures; ++index) {
                    if (index && !WaitSignal(control / (L"next-" + std::to_wstring(index)), deadline)) {
                        check(false, "next round signal before deadline"); break;
                    }
                    check(pulse::dialog_bundle::RecheckDirectory(lease) == ERROR_SUCCESS, "same leased bundle remains intact");
                    Round round{&api, session, owner};
                    PulsePickerUiV2Callbacks callbacks{}; callbacks.size = sizeof(callbacks);
                    callbacks.callback = Callback; callbacks.context = &round;
                    std::vector<wchar_t> path(32768);
                    PulsePickerUiV2Result result{}; result.size = sizeof(result);
                    result.path = {path.data(), static_cast<uint32_t>(path.size()), 0};
                    const HRESULT shown = api.Show(session, &config, &callbacks, &result);
                    fprintf(log, "round=%d show_hr=%08lx terminal_hr=%08lx ready=%u close_hr=%08lx\n", index,
                        static_cast<unsigned long>(shown), static_cast<unsigned long>(result.terminal_hr), round.ready,
                        static_cast<unsigned long>(round.closed));
                    check(round.ready == 1 && round.actual_window, "READY reports a real visible dialog");
                    check(round.owner_disabled && IsWindowEnabled(owner), "modal owner disabled then restored");
                    check(round.closed == S_OK && shown == kCancelled && result.terminal_hr == kCancelled && !result.path_count,
                        "Close from READY cancels actual Show without selection");
                    if (!failures) check(Ready(control, index, lease.ManifestDigest()), "round ready marker published after UI assertions");
                }
                if (hold && !failures) check(WaitSignal(control / L"release", deadline), "parent releases surviving host");
                check(api.Shutdown(session) == S_OK, "provider session shuts down normally");
            }
            if (owner) DestroyWindow(owner);
            provider.reset();
        }
        CoUninitialize();
    }
    fprintf(log, "failures=%d\n", failures); fclose(log);
    SetEvent(finished); watchdog.join(); CloseHandle(finished);
    return failures ? 1 : 0;
}

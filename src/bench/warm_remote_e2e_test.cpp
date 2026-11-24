#include "../common/dialog_bridge_paths.h"
#include "../ipc/picker_transport.h"
#include "../shell_bridge/dialog_core_abi.h"
#include "../shell_bridge/file_dialog_remote_provider.h"

#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;
namespace paths = pulse::dialog_bridge_paths;
namespace shell = pulse::shell_bridge;
using Microsoft::WRL::ComPtr;

namespace {
struct TempDir {
    fs::path dir;
    ~TempDir() {
        std::error_code ec;
        if (!dir.empty()) fs::remove_all(dir, ec);
    }
};

HRESULT ExerciseV2HandshakeAndValidate(const std::shared_ptr<const shell::FileDialogInprocessProvider>& provider,
                                       const std::wstring& valid_folder,
                                       const char* tag) {
    PulsePickerUiV2Initialize init{};
    init.size = sizeof(init);
    init.version = PULSE_PICKER_UI_V2_VERSION;
    PulsePickerUiV2Session session = 0;
    const HRESULT init_hr = provider->api.Initialize(&init, &session);
    if (FAILED(init_hr)) {
        std::printf("[DIAG] %s Initialize failed: 0x%08lX\n", tag, static_cast<unsigned long>(init_hr));
        return init_hr;
    }

    PulsePickerUiV2Config cfg{};
    cfg.size = sizeof(cfg);
    cfg.version = PULSE_PICKER_UI_V2_VERSION;
    cfg.required_capabilities = 0;
    cfg.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
    cfg.initial_folder = {valid_folder.c_str(), static_cast<uint32_t>(valid_folder.size())};
    const HRESULT val_hr = provider->api.ValidateConfig(session, &cfg);
    const HRESULT shut_hr = provider->api.Shutdown(session);
    if (FAILED(val_hr) || FAILED(shut_hr)) {
        std::printf("[DIAG] %s ValidateConfig=0x%08lX Shutdown=0x%08lX\n",
                    tag, static_cast<unsigned long>(val_hr), static_cast<unsigned long>(shut_hr));
    }
    return SUCCEEDED(val_hr) ? shut_hr : val_hr;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        std::printf("[FAIL] usage: %ls <pulse.exe> <PulseDialogStub64.dll> <PulseDialogCore64.dll>\n", argv[0]);
        return 2;
    }

    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };

    check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "CoInitializeEx STA");

    const std::wstring pulse_exe = argv[1];
    const std::wstring stub_src = argv[2];
    const std::wstring core_src = argv[3];

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    TempDir fixture;
    fixture.dir = fs::path(temp) / (L"pulse-e2e-" + std::to_wstring(GetCurrentProcessId()) +
                                    L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(fixture.dir);

    // =========================================================================
    // Part A: REAL pulse.exe Cold Spawn vs REAL pulse.exe Persistent Warm Host
    // =========================================================================
    {
        // 1. Cold baseline: force cold CreateProcessW(pulse.exe --picker-remote-host).
        SetEnvironmentVariableW(L"PULSE_DISABLE_WARM_REMOTE", L"1");
        SetEnvironmentVariableW(L"PULSE_WARM_REMOTE_PIPE_OVERRIDE", nullptr);

        shell::RemoteProviderTimings cold_timings{};
        std::shared_ptr<const shell::FileDialogInprocessProvider> cold_provider;
        const HRESULT cold_load = shell::LoadFileDialogRemoteProvider(pulse_exe, cold_provider, true, &cold_timings);
        const HRESULT cold_val = (SUCCEEDED(cold_load) && cold_provider)
            ? ExerciseV2HandshakeAndValidate(cold_provider, fixture.dir.wstring(), "COLD") : E_FAIL;
        cold_provider.reset();

        std::printf("[INFO] REAL pulse.exe COLD: hr=0x%08lX spawn=%llums accept=%llums hello=%llums total=%llums pid=%u\n",
                    static_cast<unsigned long>(cold_load),
                    static_cast<unsigned long long>(cold_timings.spawn_ms),
                    static_cast<unsigned long long>(cold_timings.accept_ms),
                    static_cast<unsigned long long>(cold_timings.hello_ms),
                    static_cast<unsigned long long>(cold_timings.total_ms),
                    cold_timings.host_pid);
        check(SUCCEEDED(cold_load) && SUCCEEDED(cold_val) && !cold_timings.warm_connected,
              "REAL pulse.exe cold --picker-remote-host completed Hello + Initialize + ValidateConfig + Shutdown");

        // 2. Spawn REAL pulse.exe --picker-warm-remote-host on an isolated test pipe.
        const std::wstring suffix = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        const std::wstring test_pipe = L"\\\\.\\pipe\\PulseRealWarmE2E-" + suffix;
        const std::wstring stop_name = L"Local\\PulseRealWarmStop-" + suffix;
        HANDLE stop_evt = CreateEventW(nullptr, TRUE, FALSE, stop_name.c_str());
        check(stop_evt != nullptr, "created stop event for real warm pulse.exe");

        std::wstring cmd = L"\"" + pulse_exe + L"\" --picker-warm-remote-host " + test_pipe + L" " + stop_name;
        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const BOOL started = CreateProcessW(pulse_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        check(started != FALSE, "launched real pulse.exe --picker-warm-remote-host");

        if (started) {
            // Wait up to 8s for warm host to finish pre-warming D2D/LumaText and start listening.
            bool pipe_ready = false;
            for (int i = 0; i < 160; ++i) {
                if (WaitNamedPipeW(test_pipe.c_str(), 50) || GetLastError() == ERROR_SEM_TIMEOUT) {
                    pipe_ready = true;
                    break;
                }
                if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
                Sleep(50);
            }
            check(pipe_ready, "real warm pulse.exe pre-warmed and opened persistent V2 pipe");

            SetEnvironmentVariableW(L"PULSE_DISABLE_WARM_REMOTE", nullptr);
            SetEnvironmentVariableW(L"PULSE_WARM_REMOTE_PIPE_OVERRIDE", test_pipe.c_str());

            uint64_t warm_total_sum = 0;
            bool all_warm_ok = true;
            for (int iter = 0; iter < 3; ++iter) {
                shell::RemoteProviderTimings wt{};
                std::shared_ptr<const shell::FileDialogInprocessProvider> wp;
                const HRESULT hr_load = shell::LoadFileDialogRemoteProvider(pulse_exe, wp, true, &wt);
                const HRESULT hr_val = (SUCCEEDED(hr_load) && wp)
                    ? ExerciseV2HandshakeAndValidate(wp, fixture.dir.wstring(), "WARM") : E_FAIL;
                wp.reset();
                std::printf("[INFO] REAL pulse.exe WARM #%d: hr=0x%08lX warm=%d spawn=%llums accept=%llums hello=%llums total=%llums pid=%u\n",
                            iter + 1, static_cast<unsigned long>(hr_load), wt.warm_connected ? 1 : 0,
                            static_cast<unsigned long long>(wt.spawn_ms),
                            static_cast<unsigned long long>(wt.accept_ms),
                            static_cast<unsigned long long>(wt.hello_ms),
                            static_cast<unsigned long long>(wt.total_ms),
                            wt.host_pid);
                if (FAILED(hr_load) || FAILED(hr_val) || !wt.warm_connected || wt.spawn_ms != 0 || wt.host_pid != pi.dwProcessId) {
                    all_warm_ok = false;
                }
                warm_total_sum += wt.total_ms;
            }
            const double warm_avg_ms = static_cast<double>(warm_total_sum) / 3.0;
            std::printf("[INFO] REAL pulse.exe E2E summary: cold=%llums vs warm_avg=%.2fms (3 sequential sessions on 1 process PID=%lu)\n",
                        static_cast<unsigned long long>(cold_timings.total_ms), warm_avg_ms, pi.dwProcessId);
            check(all_warm_ok, "3 sequential V2 sessions reused single warm pulse.exe with spawn_ms=0");
            check(warm_avg_ms < static_cast<double>(cold_timings.total_ms),
                  "REAL warm pulse.exe total handshake latency is strictly faster than cold spawn");

            SetEvent(stop_evt);
            const bool clean_exit = WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0;
            DWORD exit_code = 999;
            GetExitCodeProcess(pi.hProcess, &exit_code);
            if (!clean_exit) TerminateProcess(pi.hProcess, 1);
            check(clean_exit && exit_code == 0, "real warm pulse.exe shut down cleanly on stop event");
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }

        SetEnvironmentVariableW(L"PULSE_WARM_REMOTE_PIPE_OVERRIDE", nullptr);
        if (stop_evt) CloseHandle(stop_evt);
    }

    // =========================================================================
    // Part B: REAL PulseDialogStub64.dll -> REAL PulseDialogCore64.dll E2E
    // =========================================================================
    {
        const fs::path bridge_root = fixture.dir / L"RealBridge";
        const fs::path core_dir = bridge_root / L"v" / L"realcore";
        fs::create_directories(core_dir);
        fs::copy_file(stub_src, bridge_root / paths::kStubDllName64, fs::copy_options::overwrite_existing);
        fs::copy_file(core_src, core_dir / paths::kCoreDllName64, fs::copy_options::overwrite_existing);

        paths::ActiveBridgePointer ptr{};
        ptr.schema = 1;
        ptr.generation = 1;
        ptr.core_dir_x64 = core_dir.wstring();
        ptr.host_exe = pulse_exe;
        ptr.install_root = fs::path(pulse_exe).parent_path().wstring();
        check(paths::WriteActiveBridgePointer(bridge_root.wstring(), ptr) == ERROR_SUCCESS,
              "staged real PulseDialogStub64.dll + real PulseDialogCore64.dll + active-bridge.json");

        HMODULE h_comdlg = LoadLibraryW(L"comdlg32.dll");
        HMODULE stub = LoadLibraryW((bridge_root / paths::kStubDllName64).c_str());
        check(stub != nullptr, "LoadLibraryW(real PulseDialogStub64.dll) succeeds");
        if (stub) {
            using ActivateFn = DWORD (WINAPI*)(uint32_t);
            using StatusFn = HRESULT (WINAPI*)(PulseStubStatusV1*);
            using GcoFn = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
            auto p_activate = reinterpret_cast<ActivateFn>(GetProcAddress(stub, "PulseStubActivate"));
            auto p_status = reinterpret_cast<StatusFn>(GetProcAddress(stub, "PulseStubGetStatus"));
            auto p_gco = reinterpret_cast<GcoFn>(GetProcAddress(stub, "DllGetClassObject"));
            check(p_activate && p_status && p_gco, "resolved real Stub exports");

            const DWORD act_err = p_activate(PULSE_STUB_FLAG_INSTALL_SINK_HOOKS | PULSE_STUB_FLAG_DIRECT_REVEAL);
            PulseStubStatusV1 st{};
            st.size = sizeof(st);
            const HRESULT st_hr = p_status(&st);
            std::printf("[INFO] Real Stub->Core status: act_err=%lu loaded=%u gen=%llu build_id=%llu hook_mask=0x%X\n",
                        act_err, st.core_loaded, static_cast<unsigned long long>(st.generation),
                        static_cast<unsigned long long>(st.core_build_id), st.active_hook_mask);
            check(act_err == ERROR_SUCCESS && SUCCEEDED(st_hr) &&
                      st.core_loaded == 1 && st.generation == 1 &&
                      st.core_build_id >= 52 && st.active_hook_mask != 0,
                  "real PulseDialogStub64.dll loaded real PulseDialogCore64.dll and installed live sink hooks");

            // Create real wrapped IFileOpenDialog + IFileSaveDialog via Stub -> Core -> native comdlg32 (zero recursion!).
            ComPtr<IClassFactory> open_factory, save_factory;
            ComPtr<IFileOpenDialog> open_dlg;
            ComPtr<IFileSaveDialog> save_dlg;
            const HRESULT hr_of = p_gco(CLSID_FileOpenDialog, IID_PPV_ARGS(&open_factory));
            const HRESULT hr_od = SUCCEEDED(hr_of) && open_factory ? open_factory->CreateInstance(nullptr, IID_PPV_ARGS(&open_dlg)) : E_FAIL;
            const HRESULT hr_sf = p_gco(CLSID_FileSaveDialog, IID_PPV_ARGS(&save_factory));
            const HRESULT hr_sd = SUCCEEDED(hr_sf) && save_factory ? save_factory->CreateInstance(nullptr, IID_PPV_ARGS(&save_dlg)) : E_FAIL;
            FILEOPENDIALOGOPTIONS opts = 0;
            check(SUCCEEDED(hr_of) && SUCCEEDED(hr_od) && open_dlg && SUCCEEDED(open_dlg->GetOptions(&opts)),
                  "real Stub -> Core DllGetClassObject creates working wrapped IFileOpenDialog with zero recursion");
            check(SUCCEEDED(hr_sf) && SUCCEEDED(hr_sd) && save_dlg && SUCCEEDED(save_dlg->GetOptions(&opts)),
                  "real Stub -> Core DllGetClassObject creates working wrapped IFileSaveDialog with zero recursion");
        }
        if (h_comdlg) FreeLibrary(h_comdlg);
    }

    CoUninitialize();
    return failures ? 1 : 0;
}

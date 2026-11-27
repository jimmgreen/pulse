#include "../common/dialog_admin_trust.h"
#include "../common/dialog_bridge_paths.h"
#include "../common/dialog_bridge_state.h"
#include "../common/dialog_com_hklm.h"
#include "../dialog_broker/etw_process_watcher.h"
#include "../dialog_broker/process_admission.h"
#include "../dialog_broker/stub_injector.h"
#include "../ipc/picker_transport.h"
#include "../shell_bridge/dialog_core_abi.h"
#include "../shell_bridge/dialog_sink_hooks.h"

#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace paths = pulse::dialog_bridge_paths;
namespace state_ns = pulse::dialog_bridge_state;
namespace hklm = pulse::dialog_com_hklm;
namespace broker = pulse::dialog_broker;
namespace ipc = pulse::ipc::picker;
namespace sink = pulse::shell_bridge::sink_hooks;
using Microsoft::WRL::ComPtr;

namespace {

void CreateFakeClsidKey(HKEY root, const std::wstring& classes_prefix, const wchar_t* clsid,
                        const std::wstring& orig_dll, DWORD orig_type) {
    const std::wstring sub = classes_prefix + L"\\" + clsid + L"\\InprocServer32";
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, nullptr, 0, orig_type, reinterpret_cast<const BYTE*>(orig_dll.c_str()),
                       static_cast<DWORD>((orig_dll.size() + 1) * sizeof(wchar_t)));
        const wchar_t model[] = L"Apartment";
        RegSetValueExW(k, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(model), sizeof(model));
        RegCloseKey(k);
    }
}

std::wstring SelfExePath() {
    wchar_t buf[32768]{};
    const DWORD len = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    return (len && len < ARRAYSIZE(buf)) ? std::wstring(buf, len) : std::wstring{};
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // Child helper mode for cold-vs-warm latency comparison.
    if (argc >= 3 && _wcsicmp(argv[1], L"--cold-responder") == 0) {
        const std::wstring pipe_name = argv[2];
        const DWORD parent_pid = argc >= 4 ? static_cast<DWORD>(wcstoul(argv[3], nullptr, 10)) : 0;
        HANDLE parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent_pid);
        ipc::Channel ch;
        if (parent && ipc::Connect(pipe_name, parent, ch, {nullptr, 5000})) {
            std::vector<uint8_t> frame;
            if (ch.ReceiveFrame(frame, {nullptr, 5000})) {
                ch.SendFrame(frame, {nullptr, 5000});
            }
        }
        if (parent) CloseHandle(parent);
        return 0;
    }

    // Child helper mode for live process enumeration / InspectLiveProcess test.
    if (argc >= 3 && _wcsicmp(argv[1], L"--sleep-child") == 0) {
        HANDLE evt = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
        if (evt) {
            WaitForSingleObject(evt, 10000);
            CloseHandle(evt);
        }
        return 0;
    }

    // Child helper mode for the broker new-process sweep: a GUI-capable (user32 mapped)
    // process that the console broker must detect within about a second of starting.
    if (argc >= 3 && _wcsicmp(argv[1], L"--sweep-target") == 0) {
        LoadLibraryW(L"user32.dll");
        HANDLE evt = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
        if (evt) {
            WaitForSingleObject(evt, 15000);
            CloseHandle(evt);
        }
        return 0;
    }

    // Child helper mode for real InjectSameArchStub -> PulseDialogStub64 -> PulseDialogCore64
    // zero-race comdlg32 preload + multi-generation hot-swap verification (this executable does NOT statically link comdlg32.dll).
    if (argc >= 6 && _wcsicmp(argv[1], L"--real-stub-target") == 0) {
        int child_fail = 0;
        auto ccheck = [&](bool ok, const char* name) {
            std::printf("[%s] [stub-target] %s\n", ok ? "PASS" : "FAIL", name);
            if (!ok) ++child_fail;
        };
        ccheck(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "CoInitializeEx STA");
        ccheck(GetModuleHandleW(L"comdlg32.dll") == nullptr, "comdlg32.dll NOT loaded before Stub injection");

        HANDLE ready_evt = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[2]);
        HANDLE proceed_evt = OpenEventW(SYNCHRONIZE, FALSE, argv[3]);
        HANDLE phase2_ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[4]);
        HANDLE phase3_proceed = OpenEventW(SYNCHRONIZE, FALSE, argv[5]);
        if (ready_evt) SetEvent(ready_evt);
        const DWORD waited = proceed_evt ? WaitForSingleObject(proceed_evt, 15000) : WAIT_FAILED;
        if (ready_evt) CloseHandle(ready_evt);
        if (proceed_evt) CloseHandle(proceed_evt);
        ccheck(waited == WAIT_OBJECT_0, "received parent proceed signal after InjectSameArchStub");

        // Phase 1: Zero sleep — Core gen 1 must have synchronously preloaded and hooked comdlg32.dll.
        HMODULE stub_mod = GetModuleHandleW(L"PulseDialogStub64.dll");
        HMODULE core_mod = GetModuleHandleW(L"PulseDialogCore64.dll");
        HMODULE comdlg_mod = GetModuleHandleW(L"comdlg32.dll");
        ccheck(stub_mod != nullptr && core_mod != nullptr && comdlg_mod != nullptr,
               "PulseDialogStub64, PulseDialogCore64, and preloaded comdlg32 are all mapped immediately");

        using GetStatusFn = HRESULT (WINAPI*)(PulseStubStatusV1*);
        using GcoFn = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
        auto p_status = stub_mod ? reinterpret_cast<GetStatusFn>(GetProcAddress(stub_mod, "PulseStubGetStatus")) : nullptr;
        auto p_stub_gco = stub_mod ? reinterpret_cast<GcoFn>(GetProcAddress(stub_mod, "DllGetClassObject")) : nullptr;
        PulseStubStatusV1 st{};
        st.size = sizeof(st);
        const HRESULT shr = p_status ? p_status(&st) : E_FAIL;
        const uint32_t expected_mask = sink::kSinkGroupComDlgAll | sink::kSinkShell32DirectReveal;
        ccheck(SUCCEEDED(shr) && st.core_loaded == 1 && st.generation == 1 &&
               (st.active_hook_mask & expected_mask) == expected_mask,
               "Gen 1: PulseStubGetStatus reports Core loaded with full ComDlg + DirectReveal hooks");

        auto gco = comdlg_mod ? reinterpret_cast<GcoFn>(GetProcAddress(comdlg_mod, "DllGetClassObject")) : nullptr;
        ComPtr<IClassFactory> factory;
        const HRESULT ghr = gco ? gco(CLSID_FileSaveDialog, IID_PPV_ARGS(&factory)) : E_FAIL;
        ComPtr<IFileSaveDialog> dlg;
        const HRESULT chr = SUCCEEDED(ghr) && factory ? factory->CreateInstance(nullptr, IID_PPV_ARGS(&dlg)) : E_FAIL;
        const HRESULT ohr = SUCCEEDED(chr) && dlg ? dlg->SetOptions(0x42u) : E_FAIL;
        const HRESULT ehr = SUCCEEDED(chr) && dlg ? dlg->SetDefaultExtension(L"dwg") : E_FAIL;
        ccheck(SUCCEEDED(ghr) && SUCCEEDED(chr) && SUCCEEDED(ohr) && SUCCEEDED(ehr),
               "Gen 1: immediate comdlg32!DllGetClassObject(CLSID_FileSaveDialog) + setters succeed");
        dlg.Reset();
        factory.Reset();

        // Phase 2: Signal parent to upgrade active-bridge.json to gen 2, then trigger PASSIVE COM hot-swap
        // via Stub!DllGetClassObject (reproducing exact acad.exe upgrade bug!).
        if (phase2_ready) { SetEvent(phase2_ready); CloseHandle(phase2_ready); }
        const DWORD waited3 = phase3_proceed ? WaitForSingleObject(phase3_proceed, 15000) : WAIT_FAILED;
        if (phase3_proceed) CloseHandle(phase3_proceed);
        ccheck(waited3 == WAIT_OBJECT_0, "received parent signal after gen 2 passive + gen 3 Broker hot-reload");

        // Verify passive COM hot-swap + renamed-Stub Broker reload + same-Core host_exe reload (gen 4) preserved full sink hooks!
        PulseStubStatusV1 st_final{};
        st_final.size = sizeof(st_final);
        const HRESULT shr_final = p_status ? p_status(&st_final) : E_FAIL;
        std::printf("[INFO] [stub-target] final status: loaded=%u gen=%llu mask=0x%X last_err=%lu\n",
                    st_final.core_loaded, static_cast<unsigned long long>(st_final.generation),
                    st_final.active_hook_mask, st_final.last_error);
        ccheck(SUCCEEDED(shr_final) && st_final.core_loaded == 1 && st_final.generation == 4 &&
               (st_final.active_hook_mask & expected_mask) == expected_mask,
               "Gen 4: After renamed-Stub gen 3 reload + same-Core host_exe gen 4 reload, full ComDlg + DirectReveal hooks remain active");

        // Confirm Stub!DllGetClassObject and comdlg32!DllGetClassObject both still work cleanly after gen 4!
        ComPtr<IClassFactory> f3;
        const HRESULT ghr3 = p_stub_gco ? p_stub_gco(CLSID_FileSaveDialog, IID_PPV_ARGS(&f3)) : E_FAIL;
        ComPtr<IFileSaveDialog> d3;
        const HRESULT chr3 = SUCCEEDED(ghr3) && f3 ? f3->CreateInstance(nullptr, IID_PPV_ARGS(&d3)) : E_FAIL;
        ccheck(SUCCEEDED(ghr3) && SUCCEEDED(chr3) && d3,
               "Gen 4: Stub!DllGetClassObject creates IFileSaveDialog cleanly after live multi-gen upgrade");
        d3.Reset();
        f3.Reset();
        CoUninitialize();
        return child_fail ? 1 : 0;
    }

    // Secondary passive COM hot-swap verification child (isolated check for gen 1 -> gen 2 passive DllGetClassObject).
    if (argc >= 4 && _wcsicmp(argv[1], L"--passive-hotswap-target") == 0) {
        int child_fail = 0;
        auto ccheck = [&](bool ok, const char* name) {
            std::printf("[%s] [passive-target] %s\n", ok ? "PASS" : "FAIL", name);
            if (!ok) ++child_fail;
        };
        ccheck(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "CoInitializeEx STA");
        HANDLE ready_evt = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[2]);
        HANDLE check_evt = OpenEventW(SYNCHRONIZE, FALSE, argv[3]);
        if (ready_evt) { SetEvent(ready_evt); CloseHandle(ready_evt); }
        const DWORD waited = check_evt ? WaitForSingleObject(check_evt, 15000) : WAIT_FAILED;
        if (check_evt) CloseHandle(check_evt);
        ccheck(waited == WAIT_OBJECT_0, "received parent gen-2 ready signal");

        HMODULE stub_mod = GetModuleHandleW(L"PulseDialogStub64.dll");
        using GetStatusFn = HRESULT (WINAPI*)(PulseStubStatusV1*);
        using GcoFn = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
        auto p_status = stub_mod ? reinterpret_cast<GetStatusFn>(GetProcAddress(stub_mod, "PulseStubGetStatus")) : nullptr;
        auto p_stub_gco = stub_mod ? reinterpret_cast<GcoFn>(GetProcAddress(stub_mod, "DllGetClassObject")) : nullptr;
        Sleep(300); // Exceed 250ms Stub check interval so DllGetClassObject reads gen 2.
        ComPtr<IClassFactory> f2;
        const HRESULT ghr2 = p_stub_gco ? p_stub_gco(CLSID_FileOpenDialog, IID_PPV_ARGS(&f2)) : E_FAIL;
        PulseStubStatusV1 st2{};
        st2.size = sizeof(st2);
        const HRESULT shr2 = p_status ? p_status(&st2) : E_FAIL;
        const uint32_t expected_mask = sink::kSinkGroupComDlgAll | sink::kSinkShell32DirectReveal;
        std::printf("[INFO] [passive-target] after DllGetClassObject hot-swap: gen=%llu mask=0x%X hr=0x%08lX\n",
                    static_cast<unsigned long long>(st2.generation), st2.active_hook_mask, static_cast<unsigned long>(ghr2));
        ccheck(SUCCEEDED(ghr2) && f2 && SUCCEEDED(shr2) && st2.core_loaded == 1 && st2.generation == 2 &&
               (st2.active_hook_mask & expected_mask) == expected_mask,
               "Real PulseDialogCore64 passive DllGetClassObject hot-swap preserved full ComDlg + DirectReveal sink hooks");
        f2.Reset();
        CoUninitialize();
        return child_fail ? 1 : 0;
    }

    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };

    const std::wstring suffix = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());

    // =========================================================================
    // 1. Shared Memory State Page (Seqlock concurrency + Exclusions + Active Ledger)
    // =========================================================================
    {
        const std::wstring sec_name = L"Local\\PulsePhase2StateTest-" + suffix;
        state_ns::StatePublisher pub;
        check(pub.Open(sec_name, false) == ERROR_SUCCESS, "StatePublisher opens isolated shared section");

        state_ns::StateSnapshot initial;
        initial.flags = state_ns::kFlagGlobalEnabled | state_ns::kFlagSinkHooksEnabled | state_ns::kFlagWinRtEnabled;
        initial.generation = 7;
        initial.bridge_root = L"C:\\Program Files\\Common Files\\Pulse\\DialogBridge";
        initial.core_dir_x64 = L"C:\\Program Files\\Common Files\\Pulse\\DialogBridge\\v\\abc";
        initial.core_dir_x86 = L"C:\\Program Files\\Common Files\\Pulse\\DialogBridge\\v\\def";
        initial.host_exe = L"D:\\Pulse\\pulse.exe";
        initial.excluded_hashes = {state_ns::HashProcessName(L"CustomExcluded.exe")};
        check(pub.Publish(initial) == ERROR_SUCCESS, "StatePublisher publishes initial generation 7");

        state_ns::StateReader reader;
        check(reader.Open(sec_name) == ERROR_SUCCESS, "StateReader opens read-only view");
        state_ns::StateSnapshot read_back;
        check(reader.Read(read_back) &&
                  read_back.generation == 7 &&
                  read_back.flags == initial.flags &&
                  read_back.bridge_root == initial.bridge_root &&
                  read_back.host_exe == initial.host_exe &&
                  read_back.IsProcessExcluded(L"customexcluded.exe") &&
                  !read_back.IsProcessExcluded(L"notepad.exe"),
              "StateReader decodes all fields & case-insensitive exclusion hashes accurately");

        // Concurrent seqlock stress: writer advances generation 10..210 while reader checks monotonic consistency.
        std::atomic<bool> done{false};
        std::atomic<bool> torn_detected{false};
        std::thread writer([&]() {
            for (uint64_t g = 10; g <= 210; ++g) {
                state_ns::StateSnapshot s = initial;
                s.generation = g;
                s.core_dir_x64 = L"C:\\Bridge\\v\\" + std::to_wstring(g);
                pub.Publish(s);
            }
            done.store(true, std::memory_order_release);
        });
        uint64_t max_seen = 0;
        int reads = 0;
        while (!done.load(std::memory_order_acquire) || reads < 50) {
            state_ns::StateSnapshot s;
            if (reader.Read(s)) {
                ++reads;
                if (s.generation >= 10) {
                    const std::wstring expected = L"C:\\Bridge\\v\\" + std::to_wstring(s.generation);
                    if (s.core_dir_x64 != expected || s.generation < max_seen) {
                        torn_detected.store(true);
                    }
                    max_seen = s.generation;
                }
            }
        }
        writer.join();
        check(!torn_detected.load() && reads > 0 && max_seen == 210,
              "Seqlock shared memory had zero torn reads across 200 concurrent updates");

        // Active Dialog Deduplication Ledger (Layer 1/2 -> Layer 3)
        state_ns::ActiveDialogLedger ledger;
        check(ledger.OpenOrCreate(L"Local\\PulsePhase2ActiveTest-" + suffix) == ERROR_SUCCESS,
              "ActiveDialogLedger opens isolated section");
        check(!ledger.IsTakenOver(12345), "unmarked PID not reported as taken over");
        ledger.MarkActive(12345, 0x10020, 1);
        check(ledger.IsTakenOver(12345), "marked PID reported as taken over");
        ledger.ClearActive(12345, 0x10020);
        check(!ledger.IsTakenOver(12345), "cleared PID no longer reported as taken over");
    }

    // =========================================================================
    // 2. Transactional HKLM COM Registration + Backup + Atomic Restore (Isolated Hive!)
    // =========================================================================
    {
        const std::wstring base = L"Software\\PulsePhase2ComTest-" + suffix;
        hklm::RegistryScope scope;
        scope.root = HKEY_CURRENT_USER;
        scope.classes_prefix = base + L"\\Classes\\CLSID";
        scope.backup_prefix = base + L"\\Backup";
        scope.use_wow64_views = false;

        const std::wstring orig_comdlg = L"%SystemRoot%\\System32\\comdlg32.dll";
        const std::wstring stub_v1 = L"C:\\Program Files\\Common Files\\Pulse\\DialogBridge\\PulseDialogStub64.dll";
        const std::wstring stub_v2 = L"C:\\Program Files\\Common Files\\Pulse\\DialogBridge\\PulseDialogStub64_alt.dll";
        CreateFakeClsidKey(scope.root, scope.classes_prefix, hklm::kClsidOpen, orig_comdlg, REG_EXPAND_SZ);
        CreateFakeClsidKey(scope.root, scope.classes_prefix, hklm::kClsidSave, orig_comdlg, REG_EXPAND_SZ);

        check(hklm::RegisterStub(stub_v1, false, scope) == ERROR_SUCCESS,
              "RegisterStub backs up original comdlg32.dll and registers PulseDialogStub64.dll");
        const auto insp1 = hklm::Inspect(stub_v1, false, scope);
        check(insp1.error == ERROR_SUCCESS && insp1.open_matches && insp1.save_matches && insp1.backup_present,
              "Inspect confirms Open + Save match Stub and backup is present");

        // Re-registering to stub_v2 MUST NOT clobber original comdlg32.dll backup!
        check(hklm::RegisterStub(stub_v2, false, scope) == ERROR_SUCCESS,
              "Idempotent re-registration preserves original system backup");

        // Restore must bring back exact original %SystemRoot%\System32\comdlg32.dll (REG_EXPAND_SZ)!
        check(hklm::RestoreBackup(false, scope) == ERROR_SUCCESS, "RestoreBackup succeeds");
        const auto insp2 = hklm::Inspect(orig_comdlg, false, scope);
        check(insp2.error == ERROR_SUCCESS && insp2.open_matches && insp2.save_matches && !insp2.backup_present,
              "RestoreBackup restored original comdlg32.dll for Open + Save and cleared backup");

        // Verify original value type REG_EXPAND_SZ was preserved!
        HKEY check_key = nullptr;
        DWORD restored_type = 0;
        const std::wstring open_sub = scope.classes_prefix + L"\\" + hklm::kClsidOpen + L"\\InprocServer32";
        if (RegOpenKeyExW(scope.root, open_sub.c_str(), 0, KEY_READ, &check_key) == ERROR_SUCCESS) {
            RegQueryValueExW(check_key, nullptr, nullptr, &restored_type, nullptr, nullptr);
            RegCloseKey(check_key);
        }
        check(restored_type == REG_EXPAND_SZ, "RestoreBackup preserved exact REG_EXPAND_SZ value type");

        // Clean up isolated test hive.
        RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
    }

    // =========================================================================
    // 3. Broker Process Admission Filter (PPL / CIG / ACG / Anti-Cheat / CUI / WOW64)
    // =========================================================================
    {
        auto make_base = []() {
            broker::ProcessCharacteristics p{};
            p.pid = 4000;
            p.session_id = 1;
            p.machine = IMAGE_FILE_MACHINE_AMD64;
            p.pe_subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
            p.basename = L"Photoshop.exe";
            return p;
        };

        auto ok_x64 = make_base();
        check(broker::EvaluateProcessPolicy(ok_x64).verdict == broker::AdmissionVerdict::Eligible,
              "Standard x64 GUI app admitted");

        auto ok_x86 = make_base();
        ok_x86.machine = IMAGE_FILE_MACHINE_I386;
        ok_x86.basename = L"LegacyApp32.exe";
        const auto d_x86 = broker::EvaluateProcessPolicy(ok_x86);
        check(d_x86.verdict == broker::AdmissionVerdict::Eligible && d_x86.target_machine == IMAGE_FILE_MACHINE_I386,
              "WOW64 x86 GUI app admitted with I386 target machine");

        auto s0 = make_base(); s0.session_id = 0;
        check(broker::EvaluateProcessPolicy(s0).verdict == broker::AdmissionVerdict::SkipSessionZero,
              "Session 0 process rejected");

        auto ppl = make_base(); ppl.is_protected = true;
        check(broker::EvaluateProcessPolicy(ppl).verdict == broker::AdmissionVerdict::SkipProtectedProcess,
              "PPL protected process rejected");

        auto cig = make_base(); cig.has_cig_ms_only = true;
        check(broker::EvaluateProcessPolicy(cig).verdict == broker::AdmissionVerdict::SkipCigSignedOnly,
              "CIG MicrosoftSignedOnly process rejected");

        auto acg = make_base(); acg.has_acg_no_dyn_code = true;
        check(broker::EvaluateProcessPolicy(acg).verdict == broker::AdmissionVerdict::SkipAcgNoDynamicCode,
              "ACG ProhibitDynamicCode process rejected");

        auto eac = make_base(); eac.basename = L"EasyAntiCheat_EOS.exe";
        check(broker::EvaluateProcessPolicy(eac).verdict == broker::AdmissionVerdict::SkipBuiltInExcluded,
              "Anti-cheat process rejected by built-in exclusion");

        auto av = make_base(); av.basename = L"HipsDaemon.exe";
        check(broker::EvaluateProcessPolicy(av).verdict == broker::AdmissionVerdict::SkipBuiltInExcluded,
              "AV/EDR process rejected by built-in exclusion");

        auto self_pulse = make_base(); self_pulse.basename = L"PulseDialogBroker.exe";
        check(broker::EvaluateProcessPolicy(self_pulse).verdict == broker::AdmissionVerdict::SkipBuiltInExcluded,
              "Pulse own broker process never self-injected");

        const wchar_t* pulse_helpers[] = {
            L"pulse_shell.exe", L"pulse_elevated.exe", L"pulse.index.exe",
            L"pulse.preview.exe", L"pulse_7zip_worker.exe", L"pulse_hotkey_broker.exe",
            L"PulseDialogLauncher.exe",
        };
        bool all_helpers_excluded = true;
        for (const wchar_t* name : pulse_helpers) {
            auto helper = make_base();
            helper.basename = name;
            if (broker::EvaluateProcessPolicy(helper).verdict != broker::AdmissionVerdict::SkipBuiltInExcluded) {
                all_helpers_excluded = false;
            }
        }
        check(all_helpers_excluded, "All Pulse helper binaries excluded from Broker injection");

        auto cui = make_base(); cui.pe_subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI; cui.has_gui_dll_loaded = false;
        check(broker::EvaluateProcessPolicy(cui).verdict == broker::AdmissionVerdict::SkipConsoleOnlySubsystem,
              "Pure CUI console utility without GUI DLL skipped");

        cui.has_gui_dll_loaded = true;
        check(broker::EvaluateProcessPolicy(cui).verdict == broker::AdmissionVerdict::Eligible,
              "CUI process with loaded GUI DLL admitted");

        // Pure CLI shells/build tools that transitively map user32.dll must still be skipped.
        const wchar_t* cli_tools[] = {
            L"git.exe", L"cl.exe", L"link.exe", L"cmake.exe", L"ninja.exe",
            L"pwsh.exe", L"powershell.exe", L"cmd.exe", L"bash.exe", L"sing-box.exe",
        };
        bool all_cli_skipped = true;
        for (const wchar_t* name : cli_tools) {
            auto cli = make_base();
            cli.pe_subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
            cli.has_gui_dll_loaded = true;
            cli.basename = name;
            const auto d = broker::EvaluateProcessPolicy(cli);
            if (d.verdict != broker::AdmissionVerdict::SkipConsoleOnlySubsystem) all_cli_skipped = false;
        }
        check(all_cli_skipped, "Pure CUI CLI/build tools (git/cl/pwsh/bash/cmake/sing-box) skipped even when user32.dll is mapped");

        auto bg_host = make_base(); bg_host.basename = L"backgroundTaskHost.exe";
        check(broker::EvaluateProcessPolicy(bg_host).verdict == broker::AdmissionVerdict::SkipBuiltInExcluded,
              "Headless backgroundTaskHost.exe excluded from Broker injection");

        auto py_gui = make_base(); py_gui.pe_subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
        py_gui.has_gui_dll_loaded = true; py_gui.basename = L"python.exe";
        check(broker::EvaluateProcessPolicy(py_gui).verdict == broker::AdmissionVerdict::Eligible,
              "CUI scripting runtime (python.exe) with user32.dll remains eligible for GUI dialogs");

        auto already = make_base(); already.already_has_stub = true;
        check(broker::EvaluateProcessPolicy(already).verdict == broker::AdmissionVerdict::SkipAlreadyLoaded,
              "Process already carrying Stub skipped cleanly");

        state_ns::StateSnapshot user_state;
        user_state.excluded_hashes = {state_ns::HashProcessName(L"Photoshop.exe")};
        check(broker::EvaluateProcessPolicy(ok_x64, &user_state).verdict == broker::AdmissionVerdict::SkipUserExcluded,
              "User-excluded process hash honored");

        // Live PE header inspection on our own binary.
        WORD pe_machine = 0, pe_sub = 0;
        const auto self = SelfExePath();
        check(broker::ReadPeImageHeader(self, pe_machine, pe_sub) &&
                  pe_machine == IMAGE_FILE_MACHINE_AMD64 &&
                  pe_sub == IMAGE_SUBSYSTEM_WINDOWS_CUI,
              "ReadPeImageHeader reads live AMD64 CUI image accurately");

        // Live QueryProcessCreationTime + EnumerateInteractiveProcessIds + InspectLiveProcess on child process.
        {
            const std::wstring child_stop_name = L"Local\\PulsePhase2ChildStop-" + suffix;
            HANDLE child_stop = CreateEventW(nullptr, TRUE, FALSE, child_stop_name.c_str());
            std::wstring cmd = L"\"" + self + L"\" --sleep-child " + child_stop_name;
            STARTUPINFOW si{}; si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            const BOOL spawned = CreateProcessW(self.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            uint64_t child_ct = 0;
            bool found_child = false;
            bool excluded_self = true;
            broker::ProcessCharacteristics inspected{};
            if (spawned) {
                const bool got_ct = broker::QueryProcessCreationTime(pi.dwProcessId, child_ct);
                const auto interactive = broker::EnumerateInteractiveProcessIds();
                std::vector<uint8_t> fast_scratch;
                const auto interactive_fast = broker::EnumerateInteractiveProcessIdsFast(fast_scratch);
                found_child = std::find(interactive.begin(), interactive.end(), pi.dwProcessId) != interactive.end() &&
                              std::find(interactive_fast.begin(), interactive_fast.end(), pi.dwProcessId) != interactive_fast.end();
                excluded_self = std::find(interactive.begin(), interactive.end(), GetCurrentProcessId()) == interactive.end() &&
                                std::find(interactive_fast.begin(), interactive_fast.end(), GetCurrentProcessId()) == interactive_fast.end();
                inspected = broker::InspectLiveProcess(pi.dwProcessId);
                if (child_stop) SetEvent(child_stop);
                WaitForSingleObject(pi.hProcess, 5000);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                check(got_ct && child_ct != 0 && found_child && excluded_self &&
                      inspected.creation_time == child_ct &&
                      inspected.session_id != 0 &&
                      inspected.machine == IMAGE_FILE_MACHINE_AMD64 &&
                      _wcsicmp(inspected.basename.c_str(), L"pulse_dialog_phase2_test.exe") == 0,
                      "QueryProcessCreationTime + EnumerateInteractiveProcessIds + InspectLiveProcess match live child");
            } else {
                check(false, "Spawn live child for QueryProcessCreationTime / EnumerateInteractiveProcessIds");
            }
            if (child_stop) CloseHandle(child_stop);
        }

        // Read-only live-machine admission audit across real running interactive processes (zero injection).
        {
            const auto pids = broker::EnumerateInteractiveProcessIds();
            int inspected_count = 0, eligible = 0, already_loaded = 0, cui_cli = 0, cui_no_gui = 0, builtin_excl = 0, other_skip = 0;
            bool leaked_cli_or_bg = false;
            for (DWORD pid : pids) {
                const auto ch = broker::InspectLiveProcess(pid);
                if (ch.pid == 0 || ch.basename.empty()) continue;
                ++inspected_count;
                const auto dec = broker::EvaluateProcessPolicy(ch);
                if (dec.verdict == broker::AdmissionVerdict::Eligible) {
                    ++eligible;
                    if (_wcsicmp(ch.basename.c_str(), L"pwsh.exe") == 0 ||
                        _wcsicmp(ch.basename.c_str(), L"powershell.exe") == 0 ||
                        _wcsicmp(ch.basename.c_str(), L"cmd.exe") == 0 ||
                        _wcsicmp(ch.basename.c_str(), L"git.exe") == 0 ||
                        _wcsicmp(ch.basename.c_str(), L"backgroundtaskhost.exe") == 0) {
                        leaked_cli_or_bg = true;
                    }
                } else if (dec.verdict == broker::AdmissionVerdict::SkipAlreadyLoaded) {
                    ++already_loaded;
                } else if (dec.verdict == broker::AdmissionVerdict::SkipConsoleOnlySubsystem) {
                    if (dec.reason_text && std::strcmp(dec.reason_text, "cui_cli_excluded") == 0) ++cui_cli;
                    else ++cui_no_gui;
                } else if (dec.verdict == broker::AdmissionVerdict::SkipBuiltInExcluded) {
                    ++builtin_excl;
                } else {
                    ++other_skip;
                }
            }
            std::printf("[INFO] Live-machine read-only admission audit: total_pids=%zu inspected=%d eligible=%d already_loaded=%d cui_cli_excluded=%d cui_no_gui=%d builtin_excluded=%d other_skip=%d\n",
                        pids.size(), inspected_count, eligible, already_loaded, cui_cli, cui_no_gui, builtin_excl, other_skip);
            check(inspected_count > 0 && !leaked_cli_or_bg,
                  "Live-machine read-only admission audit inspected real processes with zero CLI/background host leaks");
        }
    }

    // =========================================================================
    // 4. Warm Persistent Picker Host Pipe vs Cold Spawn (Functional + Latency Benchmark)
    // =========================================================================
    {
        const std::wstring self = SelfExePath();
        const std::wstring warm_pipe = L"\\\\.\\pipe\\PulsePhase2WarmTest-" + suffix;

        // Verify WarmRemotePipeName includes user SID and -v2.
        const auto default_warm = ipc::WarmRemotePipeName();
        check(default_warm.find(L"\\\\.\\pipe\\PulsePickerHost-") == 0 &&
                  default_warm.find(L"-v2") != std::wstring::npos,
              "WarmRemotePipeName formats per-user per-session v2 pipe name");

        // Cold spawn baseline (1 round-trip via CreateProcessW).
        auto measure_cold_us = [&]() -> long long {
            const auto t0 = std::chrono::steady_clock::now();
            ipc::Server srv;
            if (!ipc::Server::Create(srv)) return -1;
            std::wstring cmd = L"\"" + self + L"\" --cold-responder " + srv.Name() + L" " +
                               std::to_wstring(GetCurrentProcessId());
            STARTUPINFOW si{}; si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (!CreateProcessW(self.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                return -1;
            }
            ipc::Channel ch;
            bool ok = static_cast<bool>(srv.Accept(pi.hProcess, ch, {nullptr, 5000}));
            const uint8_t ping[] = {'H', 'E', 'L', 'O'};
            std::vector<uint8_t> pong;
            if (ok) ok = static_cast<bool>(ch.SendFrame(ping, {nullptr, 5000}));
            if (ok) ok = static_cast<bool>(ch.ReceiveFrame(pong, {nullptr, 5000})) && pong.size() == 4;
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            const auto t1 = std::chrono::steady_clock::now();
            return ok ? std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() : -1;
        };

        const long long cold_us = measure_cold_us();
        check(cold_us > 0, "Cold CreateProcessW + pipe handshake baseline succeeded");

        // Warm persistent server handling 3 sequential dialog sessions without respawning!
        ipc::WarmRemoteServer warm_server;
        check(static_cast<bool>(warm_server.Start(warm_pipe)), "WarmRemoteServer starts persistent rendezvous");

        HANDLE stop_evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::atomic<int> served_sessions{0};
        std::thread server_thread([&]() {
            for (int i = 0; i < 3; ++i) {
                ipc::Channel ch;
                DWORD client_pid = 0;
                if (!warm_server.AcceptNext(ch, client_pid, {stop_evt, 5000})) break;
                std::vector<uint8_t> req;
                if (ch.ReceiveFrame(req, {stop_evt, 5000})) {
                    ch.SendFrame(req, {stop_evt, 5000});
                    ++served_sessions;
                }
            }
        });

        long long warm_total_us = 0;
        bool warm_all_ok = true;
        for (int i = 0; i < 3; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            ipc::Channel ch;
            DWORD srv_pid = 0;
            const auto conn = ipc::ConnectWarmRemote(self, {}, ch, srv_pid, {nullptr, 1000}, warm_pipe);
            const uint8_t payload[] = {'W', 'A', 'R', static_cast<uint8_t>('0' + i)};
            std::vector<uint8_t> reply;
            const bool ok = static_cast<bool>(conn) && srv_pid == GetCurrentProcessId() &&
                            static_cast<bool>(ch.SendFrame(payload, {nullptr, 2000})) &&
                            static_cast<bool>(ch.ReceiveFrame(reply, {nullptr, 2000})) &&
                            reply.size() == 4 && reply[3] == payload[3];
            const auto t1 = std::chrono::steady_clock::now();
            warm_total_us += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            if (!ok) warm_all_ok = false;
        }
        SetEvent(stop_evt);
        server_thread.join();
        CloseHandle(stop_evt);
        warm_server.Stop();

        const long long warm_avg_us = warm_total_us / 3;
        std::printf("[INFO] Latency comparison: cold_spawn=%lld us (%.2f ms), warm_persistent_avg=%lld us (%.2f ms)\n",
                    cold_us, cold_us / 1000.0, warm_avg_us, warm_avg_us / 1000.0);
        check(warm_all_ok && served_sessions.load() == 3,
              "WarmRemoteServer served 3 sequential dialog sessions over persistent pipe");
        check(warm_avg_us < cold_us,
              "Warm persistent pipe round-trip is measurably faster than cold process spawn");

        // Verify wrong expected executable is rejected (mutual peer image authentication!).
        ipc::WarmRemoteServer auth_server;
        auth_server.Start(warm_pipe + L"-auth");
        std::thread auth_thr([&]() {
            ipc::Channel ch;
            DWORD pid = 0;
            auth_server.AcceptNext(ch, pid, {nullptr, 2000});
        });
        ipc::Channel bogus_ch;
        DWORD bogus_pid = 0;
        const auto rejected = ipc::ConnectWarmRemote(
            L"C:\\Windows\\System32\\notepad.exe", {}, bogus_ch, bogus_pid, {nullptr, 1000}, warm_pipe + L"-auth");
        auth_thr.join();
        auth_server.Stop();
        check(!rejected && rejected.error == ERROR_ACCESS_DENIED,
              "ConnectWarmRemote rejects server with mismatching executable image");

        // Verify fast-fail when no warm server is running.
        const auto t_miss0 = std::chrono::steady_clock::now();
        ipc::Channel miss_ch;
        DWORD miss_pid = 0;
        const auto missed = ipc::ConnectWarmRemote(self, {}, miss_ch, miss_pid, {nullptr, 80}, warm_pipe + L"-absent");
        const auto miss_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_miss0).count();
        check(!missed && missed.error == ERROR_FILE_NOT_FOUND && miss_ms < 50,
              "ConnectWarmRemote fails fast (<50ms) when absent for seamless cold fallback");
    }

    // =========================================================================
    // 5. Broker ETW Diagnostics, Isolated Console Smoke Test & Real Stub+Core Injection
    // =========================================================================
    {
        const std::wstring self = SelfExePath();
        const fs::path bin_dir = fs::path(self).parent_path();

        // 5a. EtwProcessWatcher payload parser (v2 + v3) & custom session isolation.
        {
            // Verify v2 layout: ProcessID(0), CreateTime(4), ParentProcessID(12), SessionID(16).
            uint8_t v2_buf[20]{};
            const uint32_t v2_pid = 4321, v2_parent = 1000, v2_sess = 1;
            const uint64_t v2_ct = 134400077440000000ull;
            std::memcpy(v2_buf + 0, &v2_pid, 4);
            std::memcpy(v2_buf + 4, &v2_ct, 8);
            std::memcpy(v2_buf + 12, &v2_parent, 4);
            std::memcpy(v2_buf + 16, &v2_sess, 4);
            broker::ProcessStartEvent parsed{};
            const bool v2_ok = broker::ParseProcessStartPayload(2, v2_buf, sizeof(v2_buf), parsed) &&
                parsed.pid == v2_pid && parsed.create_time == v2_ct &&
                parsed.parent_pid == v2_parent && parsed.session_id == v2_sess;

            // Verify v3 layout: ProcessID(0), Seq(4), CreateTime(12), ParentPID(20), ParentSeq(24), SessionID(32).
            uint8_t v3_buf[36]{};
            const uint32_t v3_pid = 9956, v3_parent = 2000, v3_sess = 0;
            const uint64_t v3_seq = 7777, v3_ct = 134400077441234567ull, v3_pseq = 1111;
            std::memcpy(v3_buf + 0, &v3_pid, 4);
            std::memcpy(v3_buf + 4, &v3_seq, 8);
            std::memcpy(v3_buf + 12, &v3_ct, 8);
            std::memcpy(v3_buf + 20, &v3_parent, 4);
            std::memcpy(v3_buf + 24, &v3_pseq, 8);
            std::memcpy(v3_buf + 32, &v3_sess, 4);
            broker::ProcessStartEvent parsed3{};
            const bool v3_ok = broker::ParseProcessStartPayload(3, v3_buf, sizeof(v3_buf), parsed3) &&
                parsed3.pid == v3_pid && parsed3.create_time == v3_ct &&
                parsed3.parent_pid == v3_parent && parsed3.session_id == v3_sess;
            const bool trunc_ok = !broker::ParseProcessStartPayload(3, v3_buf, 20, parsed3) &&
                !broker::ParseProcessStartPayload(2, v2_buf, 16, parsed);
            check(v2_ok && v3_ok && trunc_ok,
                  "ParseProcessStartPayload decodes v2 and v3 CreateTime/ParentPID/SessionID offsets accurately");

            const std::wstring probe_session = L"PulsePhase2EtwProbe-" + suffix;
            broker::EtwProcessWatcher watcher;
            const DWORD err = watcher.Start([](const broker::ProcessStartEvent&) {}, probe_session);
            check(watcher.SessionName() == probe_session,
                  "EtwProcessWatcher preserves custom session name without touching live service session");
            if (pulse::dialog_admin::IsElevated()) {
                check(err == ERROR_SUCCESS && watcher.Healthy() && std::strcmp(watcher.LastStartStage(), "ok") == 0,
                      "EtwProcessWatcher elevated Start succeeds and reports Healthy() + stage=ok");
            } else {
                check(err != ERROR_SUCCESS && !watcher.Healthy() && std::strcmp(watcher.LastStartStage(), "none") != 0,
                      "EtwProcessWatcher non-elevated Start reports failure stage and !Healthy()");
            }
            watcher.Stop();
            check(!watcher.Healthy(), "EtwProcessWatcher Stop leaves watcher cleanly unhealthy");
        }

        // 5b. Real PulseDialogBroker.exe --console smoke test (isolated log-root + bridge-root).
        {
            const fs::path broker_exe = bin_dir / L"PulseDialogBroker.exe";
            check(fs::exists(broker_exe), "PulseDialogBroker.exe exists beside test binary");
            if (fs::exists(broker_exe)) {
                wchar_t temp[MAX_PATH]{};
                GetTempPathW(MAX_PATH, temp);
                const fs::path work_dir = fs::path(temp) / (L"pulse-broker-smoke-" + suffix);
                const fs::path log_root = work_dir / L"logs";
                const fs::path bridge_dir = work_dir / L"bridge";
                std::error_code ec;
                fs::create_directories(log_root, ec);
                fs::create_directories(bridge_dir, ec);

                const std::wstring stop_name = L"Local\\PulseBrokerSmokeStop-" + suffix;
                HANDLE stop_evt = CreateEventW(nullptr, TRUE, FALSE, stop_name.c_str());

                std::wstring cmd = L"\"" + broker_exe.wstring() + L"\" --console --stop-event " + stop_name +
                                   L" --log-root \"" + log_root.wstring() + L"\" --bridge-root \"" +
                                   bridge_dir.wstring() + L"\"";
                STARTUPINFOW si{}; si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};
                const BOOL started = CreateProcessW(broker_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                                    CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
                check(started != FALSE, "Launched PulseDialogBroker.exe --console with isolated roots");
                if (started) {
                    const fs::path log_file = log_root / L"Diagnostics" / L"Runtime" /
                        (L"dialog-broker-" + std::to_wstring(pi.dwProcessId) + L".jsonl");

                    auto read_log = [&]() -> std::string {
                        std::ifstream in(log_file, std::ios::binary);
                        if (!in) return {};
                        std::ostringstream ss;
                        ss << in.rdbuf();
                        return ss.str();
                    };

                    std::string content;
                    for (int i = 0; i < 60; ++i) {
                        content = read_log();
                        if (content.find("\"process_scan\"") != std::string::npos) break;
                        Sleep(50);
                    }
                    if (stop_evt) SetEvent(stop_evt);
                    const DWORD wait_rc = WaitForSingleObject(pi.hProcess, 8000);
                    DWORD exit_code = 999;
                    if (wait_rc == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &exit_code);
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);

                    content = read_log();
                    const bool has_started = content.find("\"broker_started\"") != std::string::npos;
                    const bool has_etw = content.find("\"etw_start\"") != std::string::npos;
                    const bool has_scan = content.find("\"process_scan\"") != std::string::npos;
                    const bool has_stopped = content.find("\"broker_stopped\"") != std::string::npos;
                    const bool has_console_session =
                        content.find("PulseDialogBrokerConsoleSession-") != std::string::npos;
                    const bool fallback_ok = pulse::dialog_admin::IsElevated()
                        ? true
                        : (content.find("\"fallback_poll_state\"") != std::string::npos);

                    check(wait_rc == WAIT_OBJECT_0 && exit_code == 0,
                          "PulseDialogBroker.exe --console exited cleanly on stop-event");
                    check(has_started && has_etw && has_scan && has_stopped && has_console_session && fallback_ok,
                          "PulseDialogBroker runtime log recorded broker_started/etw_start/process_scan/fallback/broker_stopped with isolated session");
                }
                if (stop_evt) CloseHandle(stop_evt);
                fs::remove_all(work_dir, ec);
            }
        }

        // 5b2. Broker new-process sweep: with an enabled (but unprotected, so never
        //      injectable) isolated bridge, a freshly started GUI-capable child must be
        //      evaluated within ~1 s even when ETW is unavailable or late.
        {
            const fs::path broker_exe = bin_dir / L"PulseDialogBroker.exe";
            if (fs::exists(broker_exe)) {
                wchar_t temp[MAX_PATH]{};
                GetTempPathW(MAX_PATH, temp);
                const fs::path work_dir = fs::path(temp) / (L"pulse-broker-sweep-" + suffix);
                const fs::path log_root = work_dir / L"logs";
                const fs::path bridge_dir = work_dir / L"bridge";
                std::error_code ec;
                fs::create_directories(log_root, ec);
                fs::create_directories(bridge_dir / L"v" / L"gen1", ec);
                paths::ActiveBridgePointer ptr;
                ptr.schema = 1;
                ptr.generation = 1;
                ptr.core_dir_x64 = (bridge_dir / L"v" / L"gen1").wstring();
                ptr.host_exe = (bin_dir / L"pulse.exe").wstring();
                ptr.install_root = bin_dir.wstring();
                const DWORD write_err = paths::WriteActiveBridgePointer(bridge_dir.wstring(), ptr);
                check(write_err == ERROR_SUCCESS, "Sweep test staged enabled isolated bridge without any stub DLL");

                const std::wstring stop_name = L"Local\\PulseBrokerSweepStop-" + suffix;
                const std::wstring child_name = L"Local\\PulseBrokerSweepChild-" + suffix;
                HANDLE stop_evt = CreateEventW(nullptr, TRUE, FALSE, stop_name.c_str());
                std::wstring cmd = L"\"" + broker_exe.wstring() + L"\" --console --stop-event " + stop_name +
                                   L" --log-root \"" + log_root.wstring() + L"\" --bridge-root \"" +
                                   bridge_dir.wstring() + L"\"";
                STARTUPINFOW si{}; si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};
                const BOOL started = write_err == ERROR_SUCCESS &&
                    CreateProcessW(broker_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
                check(started != FALSE, "Launched console broker for new-process sweep test");
                if (started) {
                    const fs::path log_file = log_root / L"Diagnostics" / L"Runtime" /
                        (L"dialog-broker-" + std::to_wstring(pi.dwProcessId) + L".jsonl");
                    auto read_log = [&]() -> std::string {
                        std::ifstream in(log_file, std::ios::binary);
                        if (!in) return {};
                        std::ostringstream ss;
                        ss << in.rdbuf();
                        return ss.str();
                    };
                    for (int i = 0; i < 100 && read_log().find("startup_scan") == std::string::npos; ++i) Sleep(50);
                    // Let the startup backlog (every running process) drain before timing.
                    Sleep(2500);

                    std::string line;
                    bool spawned_any = false;
                    int live_broker_preempted = 0;
                    // On a host running the live PulseDialogBroker service, the live service
                    // may inject PulseDialogStub64.dll into our test child within ~300 ms,
                    // causing the isolated console broker to correctly skip it as already_loaded.
                    // Retry up to 4 times so at least one un-injected child exercises the sweep.
                    for (int attempt = 0; attempt < 4 && line.empty(); ++attempt) {
                        const std::wstring attempt_evt_name = child_name + L"-" + std::to_wstring(attempt);
                        HANDLE attempt_evt = CreateEventW(nullptr, TRUE, FALSE, attempt_evt_name.c_str());
                        std::wstring child_cmd = L"\"" + self + L"\" --sweep-target " + attempt_evt_name;
                        STARTUPINFOW csi{}; csi.cb = sizeof(csi);
                        PROCESS_INFORMATION cpi{};
                        const BOOL child_ok = CreateProcessW(self.c_str(), child_cmd.data(), nullptr, nullptr, FALSE,
                                                             CREATE_NO_WINDOW, nullptr, nullptr, &csi, &cpi);
                        if (!child_ok) { if (attempt_evt) CloseHandle(attempt_evt); break; }
                        spawned_any = true;
                        const std::string needle = "\"pid\":" + std::to_string(cpi.dwProcessId) + ",";
                        bool preempted_by_live_broker = false;
                        for (int i = 0; i < 60 && line.empty(); ++i) {
                            const std::string content = read_log();
                            size_t pos = 0;
                            while ((pos = content.find(needle, pos)) != std::string::npos) {
                                const size_t begin = content.rfind('\n', pos);
                                const size_t end = content.find('\n', pos);
                                const std::string candidate = content.substr(begin == std::string::npos ? 0 : begin + 1,
                                    (end == std::string::npos ? content.size() : end) - (begin == std::string::npos ? 0 : begin + 1));
                                if (candidate.find("\"process_verify_failed\"") != std::string::npos) { line = candidate; break; }
                                pos += needle.size();
                            }
                            if (!line.empty()) break;
                            if (i >= 4 && broker::InspectLiveProcess(cpi.dwProcessId).already_has_stub) {
                                preempted_by_live_broker = true;
                                ++live_broker_preempted;
                                break;
                            }
                            Sleep(50);
                        }
                        if (attempt_evt) { SetEvent(attempt_evt); CloseHandle(attempt_evt); }
                        WaitForSingleObject(cpi.hProcess, 5000);
                        CloseHandle(cpi.hThread);
                        CloseHandle(cpi.hProcess);
                        if (!preempted_by_live_broker && line.empty()) break;
                    }
                    check(spawned_any, "Spawned GUI-capable sweep target child");
                    auto field = [&](const char* key) -> long long {
                        const std::string k = std::string("\"") + key + "\":";
                        const size_t p = line.find(k);
                        return p == std::string::npos ? -1 : std::atoll(line.c_str() + p + k.size());
                    };
                    auto text_field = [&](const char* key) -> std::string {
                        const std::string k = std::string("\"") + key + "\":\"";
                        const size_t p = line.find(k);
                        if (p == std::string::npos) return {};
                        const size_t e = line.find('"', p + k.size());
                        return line.substr(p + k.size(), e == std::string::npos ? std::string::npos : e - p - k.size());
                    };
                    const long long detect_ms = field("detect_ms");
                    const std::string source = text_field("source");
                    std::printf("[INFO] Sweep target detection: source=%s detect_ms=%lld age_ms=%lld live_preempted=%d\n",
                                source.c_str(), detect_ms, field("age_ms"), live_broker_preempted);
                    if (line.empty() && live_broker_preempted == 4) {
                        std::printf("[INFO] All 4 sweep target children were injected first by the live host PulseDialogBroker service (<250 ms)\n");
                    } else {
                        check(!line.empty(), "Console broker evaluated the new child (verify fails on unprotected bridge, never injects)");
                        check(detect_ms >= 0 && detect_ms <= 2000,
                              "New GUI process detected within 2 s of creation (sweep bound, independent of ETW delivery)");
                    }

                    if (stop_evt) SetEvent(stop_evt);
                    WaitForSingleObject(pi.hProcess, 8000);
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    const std::string content = read_log();
                    const size_t stopped = content.find("\"broker_stopped\"");
                    const size_t runs_pos = stopped == std::string::npos ? std::string::npos : content.find("\"sweep_runs\":", stopped);
                    const long long runs = runs_pos == std::string::npos ? -1 : std::atoll(content.c_str() + runs_pos + 13);
                    const size_t avg_pos = stopped == std::string::npos ? std::string::npos : content.find("\"sweep_avg_us\":", stopped);
                    const size_t max_pos = stopped == std::string::npos ? std::string::npos : content.find("\"sweep_max_us\":", stopped);
                    std::printf("[INFO] Sweep stats: runs=%lld avg_us=%lld max_us=%lld\n", runs,
                                avg_pos == std::string::npos ? -1 : std::atoll(content.c_str() + avg_pos + 15),
                                max_pos == std::string::npos ? -1 : std::atoll(content.c_str() + max_pos + 15));
                    check(runs >= 2, "broker_stopped reports periodic sweep runs");
                }
                if (stop_evt) CloseHandle(stop_evt);
                fs::remove_all(work_dir, ec);
            }
        }

        // 5c. Real InjectSameArchStub -> PulseDialogStub64.dll -> PulseDialogCore64.dll
        //     into fresh child processes verifying:
        //     (i)  zero-race comdlg32 preload,
        //     (ii) passive COM DllGetClassObject hot-swap (gen 1 -> gen 2) preserving sink hooks,
        //     (iii) active Broker hot-reload on already-loaded process AFTER Stub DLL is renamed (.old.)!
        {
            const fs::path stub_src = bin_dir / paths::kStubDllName64;
            const fs::path core_src = bin_dir / paths::kCoreDllName64;
            check(fs::exists(stub_src) && fs::exists(core_src),
                  "Real PulseDialogStub64.dll and PulseDialogCore64.dll exist beside test binary");
            if (fs::exists(stub_src) && fs::exists(core_src)) {
                wchar_t temp[MAX_PATH]{};
                GetTempPathW(MAX_PATH, temp);
                const fs::path stage_root = fs::path(temp) / (L"pulse-real-inject-" + suffix);
                const fs::path bridge_dir = stage_root / L"DialogBridge";
                const fs::path core_dir1 = bridge_dir / L"v" / L"gen1";
                const fs::path core_dir2 = bridge_dir / L"v" / L"gen2";
                const fs::path core_dir3 = bridge_dir / L"v" / L"gen3";
                std::error_code ec;
                fs::create_directories(core_dir1, ec);
                fs::create_directories(core_dir2, ec);
                fs::create_directories(core_dir3, ec);

                const fs::path stub_dst = bridge_dir / paths::kStubDllName64;
                fs::copy_file(stub_src, stub_dst, fs::copy_options::overwrite_existing, ec);
                const bool copied_stub = !ec;
                ec.clear();
                fs::copy_file(core_src, core_dir1 / paths::kCoreDllName64, fs::copy_options::overwrite_existing, ec);
                const bool copied_c1 = !ec;
                ec.clear();
                fs::copy_file(core_src, core_dir2 / paths::kCoreDllName64, fs::copy_options::overwrite_existing, ec);
                const bool copied_c2 = !ec;
                ec.clear();
                fs::copy_file(core_src, core_dir3 / paths::kCoreDllName64, fs::copy_options::overwrite_existing, ec);
                const bool copied_c3 = !ec;

                paths::ActiveBridgePointer ptr;
                ptr.schema = 1;
                ptr.generation = 1;
                ptr.core_dir_x64 = core_dir1.wstring();
                ptr.host_exe = (bin_dir / L"pulse.exe").wstring();
                ptr.install_root = bin_dir.wstring();
                const DWORD write_err = paths::WriteActiveBridgePointer(bridge_dir.wstring(), ptr);
                check(copied_stub && copied_c1 && copied_c2 && copied_c3 && write_err == ERROR_SUCCESS,
                      "Staged isolated real Stub64 + 3 Core64 generations + active-bridge.json");

                // 5c-1: Real passive DllGetClassObject hot-swap (gen 1 -> gen 2) with real PulseDialogCore64.dll!
                {
                    const std::wstring rdy_name = L"Local\\PulsePassiveReady-" + suffix;
                    const std::wstring chk_name = L"Local\\PulsePassiveCheck-" + suffix;
                    HANDLE rdy_evt = CreateEventW(nullptr, TRUE, FALSE, rdy_name.c_str());
                    HANDLE chk_evt = CreateEventW(nullptr, TRUE, FALSE, chk_name.c_str());
                    std::wstring cmd = L"\"" + self + L"\" --passive-hotswap-target " + rdy_name + L" " + chk_name;
                    STARTUPINFOW si{}; si.cb = sizeof(si);
                    PROCESS_INFORMATION pi{};
                    bool passive_ok = false;
                    if (CreateProcessW(self.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                        if (WaitForSingleObject(rdy_evt, 10000) == WAIT_OBJECT_0) {
                            const uint32_t flags = PULSE_STUB_FLAG_INSTALL_SINK_HOOKS | PULSE_STUB_FLAG_DIRECT_REVEAL;
                            const auto res1 = broker::InjectSameArchStub(pi.dwProcessId, stub_dst.wstring(), flags);
                            check(res1.error == ERROR_SUCCESS && res1.activate_result == ERROR_SUCCESS && !res1.already_loaded,
                                  "Passive hot-swap test: initial InjectSameArchStub gen 1 succeeded");
                            // Upgrade active-bridge.json to gen 2 WITHOUT calling Broker hot-reload.
                            paths::ActiveBridgePointer ptr2 = ptr;
                            ptr2.generation = 2;
                            ptr2.core_dir_x64 = core_dir2.wstring();
                            paths::WriteActiveBridgePointer(bridge_dir.wstring(), ptr2);
                        }
                        if (chk_evt) SetEvent(chk_evt);
                        DWORD exit_code = 1;
                        if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0 &&
                            GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code == 0) {
                            passive_ok = true;
                        }
                        CloseHandle(pi.hThread);
                        CloseHandle(pi.hProcess);
                    }
                    if (rdy_evt) CloseHandle(rdy_evt);
                    if (chk_evt) CloseHandle(chk_evt);
                    check(passive_ok, "Real Core64 passive COM DllGetClassObject hot-swap (gen 1->2) kept all sink hooks");
                    // Reset pointer back to gen 1 for 5c-2.
                    paths::WriteActiveBridgePointer(bridge_dir.wstring(), ptr);
                }

                // 5c-2: Real active Broker hot-reload on already-loaded process AFTER Stub64 renamed to .old.!
                const std::wstring ready_name = L"Local\\PulseRealInjectReady-" + suffix;
                const std::wstring proceed_name = L"Local\\PulseRealInjectProceed-" + suffix;
                const std::wstring p2_ready_name = L"Local\\PulseRealInjectP2Ready-" + suffix;
                const std::wstring p3_proc_name = L"Local\\PulseRealInjectP3Proc-" + suffix;
                HANDLE ready_evt = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
                HANDLE proceed_evt = CreateEventW(nullptr, TRUE, FALSE, proceed_name.c_str());
                HANDLE p2_ready = CreateEventW(nullptr, TRUE, FALSE, p2_ready_name.c_str());
                HANDLE p3_proc = CreateEventW(nullptr, TRUE, FALSE, p3_proc_name.c_str());

                std::wstring cmd = L"\"" + self + L"\" --real-stub-target " + ready_name + L" " + proceed_name +
                                   L" " + p2_ready_name + L" " + p3_proc_name;
                STARTUPINFOW si{}; si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};
                const BOOL spawned = CreateProcessW(self.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                                    0, nullptr, nullptr, &si, &pi);
                bool inject_ok = false;
                bool reload_ok = false;
                bool idempotent_ok = false;
                bool child_passed = false;
                if (spawned) {
                    const uint32_t flags = PULSE_STUB_FLAG_INSTALL_SINK_HOOKS | PULSE_STUB_FLAG_DIRECT_REVEAL;
                    if (WaitForSingleObject(ready_evt, 10000) == WAIT_OBJECT_0) {
                        const auto t0 = std::chrono::steady_clock::now();
                        const auto res = broker::InjectSameArchStub(pi.dwProcessId, stub_dst.wstring(), flags);
                        const auto inj_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
                        inject_ok = (res.error == ERROR_SUCCESS && res.activate_result == ERROR_SUCCESS && !res.already_loaded);
                        std::printf("[INFO] InjectSameArchStub gen1: error=%lu activate_result=%lu already_loaded=%d elapsed_ms=%lld\n",
                                    res.error, res.activate_result, res.already_loaded ? 1 : 0, inj_ms);
                    }
                    if (proceed_evt) SetEvent(proceed_evt);
                    if (WaitForSingleObject(p2_ready, 10000) == WAIT_OBJECT_0) {
                        // Simulate real upgrade: rename locked Stub64 to .old.test, stage fresh Stub64,
                        // advance active-bridge.json to gen 3, and run Broker active hot-reload!
                        const fs::path retired_stub = bridge_dir / L"PulseDialogStub64.dll.old.test";
                        const BOOL renamed = MoveFileExW(stub_dst.c_str(), retired_stub.c_str(), MOVEFILE_REPLACE_EXISTING);
                        ec.clear();
                        fs::copy_file(stub_src, stub_dst, fs::copy_options::overwrite_existing, ec);
                        paths::ActiveBridgePointer ptr3 = ptr;
                        ptr3.generation = 3;
                        ptr3.core_dir_x64 = core_dir3.wstring();
                        paths::WriteActiveBridgePointer(bridge_dir.wstring(), ptr3);

                        const auto t_rel0 = std::chrono::steady_clock::now();
                        const auto rel = broker::InjectSameArchStub(pi.dwProcessId, stub_dst.wstring(), flags);
                        const auto rel_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t_rel0).count();
                        reload_ok = (renamed != FALSE && !ec && rel.error == ERROR_SUCCESS &&
                                     rel.activate_result == ERROR_SUCCESS && rel.already_loaded);
                        std::printf("[INFO] Broker hot-reload gen3 (after Stub rename .old.): error=%lu activate_result=%lu already_loaded=%d elapsed_ms=%lld\n",
                                    rel.error, rel.activate_result, rel.already_loaded ? 1 : 0, rel_ms);

                        // Advance to gen 4 with SAME core_dir3 + updated host_exe to verify same-DLL Detours
                        // Uninstall -> Re-Install keeps all 16 real sink hooks active (mask=0x13F)!
                        paths::ActiveBridgePointer ptr4 = ptr3;
                        ptr4.generation = 4;
                        ptr4.host_exe = (bin_dir / L"pulse_v4.exe").wstring();
                        paths::WriteActiveBridgePointer(bridge_dir.wstring(), ptr4);
                        const auto t_rel4 = std::chrono::steady_clock::now();
                        const auto rel4 = broker::InjectSameArchStub(pi.dwProcessId, stub_dst.wstring(), flags);
                        const auto rel4_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t_rel4).count();
                        reload_ok = reload_ok && (rel4.error == ERROR_SUCCESS &&
                                                  rel4.activate_result == ERROR_SUCCESS && rel4.already_loaded);
                        std::printf("[INFO] Broker same-Core host_exe hot-reload gen4: error=%lu activate_result=%lu already_loaded=%d elapsed_ms=%lld\n",
                                    rel4.error, rel4.activate_result, rel4.already_loaded ? 1 : 0, rel4_ms);

                        // Measure idempotent same-gen re-activation latency.
                        const auto t_idem0 = std::chrono::steady_clock::now();
                        const auto idem = broker::InjectSameArchStub(pi.dwProcessId, stub_dst.wstring(), flags);
                        const auto idem_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t_idem0).count();
                        idempotent_ok = (idem.error == ERROR_SUCCESS && idem.activate_result == ERROR_SUCCESS && idem.already_loaded);
                        std::printf("[INFO] Idempotent same-gen re-activation: error=%lu activate_result=%lu elapsed_us=%lld (%.2f ms)\n",
                                    idem.error, idem.activate_result, idem_us, idem_us / 1000.0);
                    }
                    if (p3_proc) SetEvent(p3_proc);
                    DWORD exit_code = 1;
                    if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0 &&
                        GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code == 0) {
                        child_passed = true;
                    }
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                }
                if (ready_evt) CloseHandle(ready_evt);
                if (proceed_evt) CloseHandle(proceed_evt);
                if (p2_ready) CloseHandle(p2_ready);
                if (p3_proc) CloseHandle(p3_proc);
                check(inject_ok, "Real InjectSameArchStub activated PulseDialogStub64 + PulseDialogCore64 gen 1 cleanly");
                check(reload_ok, "Real Broker hot-reload on already-loaded renamed (.old.) Stub switched live process to gen 3");
                check(idempotent_ok, "Idempotent same-generation PulseStubActivate fast-path succeeded cleanly");
                check(child_passed, "Injected child kept full ComDlg + DirectReveal hooks and COM Save interception through gen 3");
                fs::remove_all(stage_root, ec);
            }
        }
    }

    return failures ? 1 : 0;
}

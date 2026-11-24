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

    // Child helper mode for real InjectSameArchStub -> PulseDialogStub64 -> PulseDialogCore64
    // zero-race comdlg32 preload verification (this executable does NOT statically link comdlg32.dll).
    if (argc >= 4 && _wcsicmp(argv[1], L"--real-stub-target") == 0) {
        int child_fail = 0;
        auto ccheck = [&](bool ok, const char* name) {
            std::printf("[%s] [stub-target] %s\n", ok ? "PASS" : "FAIL", name);
            if (!ok) ++child_fail;
        };
        ccheck(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "CoInitializeEx STA");
        ccheck(GetModuleHandleW(L"comdlg32.dll") == nullptr, "comdlg32.dll NOT loaded before Stub injection");

        HANDLE ready_evt = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[2]);
        HANDLE proceed_evt = OpenEventW(SYNCHRONIZE, FALSE, argv[3]);
        if (ready_evt) SetEvent(ready_evt);
        const DWORD waited = proceed_evt ? WaitForSingleObject(proceed_evt, 15000) : WAIT_FAILED;
        if (ready_evt) CloseHandle(ready_evt);
        if (proceed_evt) CloseHandle(proceed_evt);
        ccheck(waited == WAIT_OBJECT_0, "received parent proceed signal after InjectSameArchStub");

        // Zero sleep: Core must have synchronously preloaded and hooked comdlg32.dll during PulseStubActivate.
        HMODULE stub_mod = GetModuleHandleW(L"PulseDialogStub64.dll");
        HMODULE core_mod = GetModuleHandleW(L"PulseDialogCore64.dll");
        HMODULE comdlg_mod = GetModuleHandleW(L"comdlg32.dll");
        ccheck(stub_mod != nullptr && core_mod != nullptr && comdlg_mod != nullptr,
               "PulseDialogStub64, PulseDialogCore64, and preloaded comdlg32 are all mapped immediately");

        using GetStatusFn = HRESULT (WINAPI*)(PulseStubStatusV1*);
        auto p_status = stub_mod ? reinterpret_cast<GetStatusFn>(GetProcAddress(stub_mod, "PulseStubGetStatus")) : nullptr;
        PulseStubStatusV1 st{};
        st.size = sizeof(st);
        const HRESULT shr = p_status ? p_status(&st) : E_FAIL;
        ccheck(SUCCEEDED(shr) && st.core_loaded == 1 &&
               (st.active_hook_mask & sink::kSinkGroupComDlgAll) == sink::kSinkGroupComDlgAll,
               "PulseStubGetStatus reports Core loaded with full kSinkGroupComDlgAll mask synchronously");

        using GcoFn = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
        auto gco = comdlg_mod ? reinterpret_cast<GcoFn>(GetProcAddress(comdlg_mod, "DllGetClassObject")) : nullptr;
        ComPtr<IClassFactory> factory;
        const HRESULT ghr = gco ? gco(CLSID_FileSaveDialog, IID_PPV_ARGS(&factory)) : E_FAIL;
        ComPtr<IFileSaveDialog> dlg;
        const HRESULT chr = SUCCEEDED(ghr) && factory ? factory->CreateInstance(nullptr, IID_PPV_ARGS(&dlg)) : E_FAIL;
        const HRESULT ohr = SUCCEEDED(chr) && dlg ? dlg->SetOptions(0x42u) : E_FAIL;
        const HRESULT ehr = SUCCEEDED(chr) && dlg ? dlg->SetDefaultExtension(L"dwg") : E_FAIL;
        ccheck(SUCCEEDED(ghr) && SUCCEEDED(chr) && SUCCEEDED(ohr) && SUCCEEDED(ehr),
               "immediate comdlg32!DllGetClassObject(CLSID_FileSaveDialog) + SetOptions(0x42) + SetDefaultExtension(dwg) succeeds");

        dlg.Reset();
        factory.Reset();
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
                found_child = std::find(interactive.begin(), interactive.end(), pi.dwProcessId) != interactive.end();
                excluded_self = std::find(interactive.begin(), interactive.end(), GetCurrentProcessId()) == interactive.end();
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

        // 5a. EtwProcessWatcher custom session isolation & diagnostic state reporting.
        {
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

        // 5c. Real InjectSameArchStub -> PulseDialogStub64.dll -> PulseDialogCore64.dll
        //     into a fresh child process that does NOT statically link comdlg32.dll!
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
                const fs::path core_dir = bridge_dir / L"v" / L"gen1";
                std::error_code ec;
                fs::create_directories(core_dir, ec);

                const fs::path stub_dst = bridge_dir / paths::kStubDllName64;
                const fs::path core_dst = core_dir / paths::kCoreDllName64;
                fs::copy_file(stub_src, stub_dst, fs::copy_options::overwrite_existing, ec);
                const bool copied_stub = !ec;
                ec.clear();
                fs::copy_file(core_src, core_dst, fs::copy_options::overwrite_existing, ec);
                const bool copied_core = !ec;

                paths::ActiveBridgePointer ptr;
                ptr.schema = 1;
                ptr.generation = 1;
                ptr.core_dir_x64 = core_dir.wstring();
                ptr.host_exe = (bin_dir / L"pulse.exe").wstring();
                ptr.install_root = bin_dir.wstring();
                const DWORD write_err = paths::WriteActiveBridgePointer(bridge_dir.wstring(), ptr);
                check(copied_stub && copied_core && write_err == ERROR_SUCCESS,
                      "Staged isolated real Stub64 + Core64 + active-bridge.json");

                const std::wstring ready_name = L"Local\\PulseRealInjectReady-" + suffix;
                const std::wstring proceed_name = L"Local\\PulseRealInjectProceed-" + suffix;
                HANDLE ready_evt = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
                HANDLE proceed_evt = CreateEventW(nullptr, TRUE, FALSE, proceed_name.c_str());

                std::wstring cmd = L"\"" + self + L"\" --real-stub-target " + ready_name + L" " + proceed_name;
                STARTUPINFOW si{}; si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};
                const BOOL spawned = CreateProcessW(self.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                                    0, nullptr, nullptr, &si, &pi);
                bool inject_ok = false;
                bool child_passed = false;
                if (spawned) {
                    if (WaitForSingleObject(ready_evt, 10000) == WAIT_OBJECT_0) {
                        const uint32_t flags = PULSE_STUB_FLAG_INSTALL_SINK_HOOKS | PULSE_STUB_FLAG_DIRECT_REVEAL;
                        const auto res = broker::InjectSameArchStub(pi.dwProcessId, stub_dst.wstring(), flags);
                        inject_ok = (res.error == ERROR_SUCCESS && res.activate_result == ERROR_SUCCESS && !res.already_loaded);
                        std::printf("[INFO] InjectSameArchStub result: error=%lu activate_result=%lu already_loaded=%d\n",
                                    res.error, res.activate_result, res.already_loaded ? 1 : 0);
                    }
                    if (proceed_evt) SetEvent(proceed_evt);
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
                check(inject_ok, "Real InjectSameArchStub activated PulseDialogStub64 + PulseDialogCore64 cleanly");
                check(child_passed, "Injected child without static comdlg32 immediately intercepted CLSID_FileSaveDialog");
                fs::remove_all(stage_root, ec);
            }
        }
    }

    return failures ? 1 : 0;
}

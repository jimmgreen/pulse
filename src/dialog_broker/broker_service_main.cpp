#include "deferred_cleanup.h"
#include "etw_process_watcher.h"
#include "process_admission.h"
#include "stub_injector.h"
#include "../common/dialog_admin_trust.h"
#include "../common/dialog_bridge_paths.h"
#include "../common/dialog_bridge_state.h"
#include "../common/dialog_com_hklm.h"
#include "../common/runtime_log.h"
#include "../shell_bridge/dialog_core_abi.h"

#include <windows.h>
#include <shlobj.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

namespace paths = pulse::dialog_bridge_paths;
namespace state_ns = pulse::dialog_bridge_state;
namespace broker = pulse::dialog_broker;
namespace rlog = pulse::diagnostics::runtime;

constexpr wchar_t kServiceName[] = L"PulseDialogBroker";
constexpr DWORD kFallbackPollIntervalMs = 1500;
constexpr DWORD kSteadyWaitIntervalMs = 5000;
constexpr DWORD kNewProcessSettleMs = 120;
constexpr DWORD kTransientRetryDelayMs = 500;
constexpr std::array<DWORD, 4> kEtwRetryBackoffMs = {5000, 15000, 60000, 300000};

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_service_status{};
HANDLE g_stop_event = nullptr;

std::wstring AsciiToWide(const char* text) {
    if (!text) return {};
    std::wstring out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
        out.push_back(static_cast<wchar_t>(*p));
    }
    return out;
}

std::wstring DefaultLogRoot() {
    wchar_t buf[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf)) && buf[0]) {
        return std::wstring(buf) + L"\\Pulse";
    }
    return L"C:\\ProgramData\\Pulse";
}

uint64_t ProcessAgeMs(uint64_t creation_time) noexcept {
    if (creation_time == 0) return 0;
    FILETIME now_ft{};
    GetSystemTimeAsFileTime(&now_ft);
    ULARGE_INTEGER now_uli{};
    now_uli.LowPart = now_ft.dwLowDateTime;
    now_uli.HighPart = now_ft.dwHighDateTime;
    if (now_uli.QuadPart <= creation_time) return 0;
    return (now_uli.QuadPart - creation_time) / 10000ull;
}

enum class CandidateSource : uint8_t {
    Etw = 0,
    StartupScan = 1,
    FallbackPoll = 2,
    Retry = 3,
};

const wchar_t* SourceName(CandidateSource src) noexcept {
    switch (src) {
    case CandidateSource::Etw: return L"etw";
    case CandidateSource::StartupScan: return L"startup_scan";
    case CandidateSource::FallbackPoll: return L"fallback_poll";
    case CandidateSource::Retry: return L"retry";
    default: return L"unknown";
    }
}

struct PendingCandidate {
    DWORD pid = 0;
    uint64_t creation_time = 0;
    CandidateSource source = CandidateSource::Etw;
    uint8_t attempt = 0;
    ULONGLONG ready_tick = 0;
};

struct TrackedProcess {
    uint64_t creation_time = 0;
    bool settled = false;
    bool queued = false;
    ULONGLONG last_touch_tick = 0;
};

struct BrokerOptions {
    bool console_mode = false;
    std::wstring log_root_override;
    std::wstring bridge_root_override;
};

void ReportServiceState(DWORD current_state, DWORD exit_code = NO_ERROR, DWORD wait_hint = 0) noexcept {
    if (!g_status_handle) return;
    static DWORD checkpoint = 1;
    g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_service_status.dwCurrentState = current_state;
    g_service_status.dwWin32ExitCode = exit_code;
    g_service_status.dwWaitHint = wait_hint;
    g_service_status.dwControlsAccepted = (current_state == SERVICE_START_PENDING) ? 0
        : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
    g_service_status.dwCheckPoint = (current_state == SERVICE_RUNNING || current_state == SERVICE_STOPPED)
        ? 0 : checkpoint++;
    SetServiceStatus(g_status_handle, &g_service_status);
}

DWORD WINAPI ServiceCtrlHandler(DWORD control, DWORD, void*, void*) noexcept {
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        ReportServiceState(SERVICE_STOP_PENDING, NO_ERROR, 5000);
        if (g_stop_event) SetEvent(g_stop_event);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

struct BrokerRuntime {
    std::wstring bridge_root;
    std::wstring etw_session_name;
    state_ns::StatePublisher publisher;
    broker::EtwProcessWatcher etw;
    std::mutex state_mutex;
    state_ns::StateSnapshot current_state;
    paths::ActiveBridgePointer current_pointer;
    uint64_t last_seen_generation = 0;
    bool last_seen_enabled = false;
    bool state_initialized = false;

    std::mutex queue_mutex;
    std::deque<PendingCandidate> pending_queue;
    std::vector<PendingCandidate> retry_queue;
    std::unordered_map<DWORD, TrackedProcess> tracked;
    HANDLE work_event = nullptr;
    HANDLE cleanup_wake_event = nullptr;

    std::atomic<uint64_t> injected_ok{0};
    std::atomic<uint64_t> injected_fail{0};
    uint32_t etw_start_attempts = 0;

    struct RefreshOutcome {
        bool enabled = false;
        bool became_enabled = false;
        bool generation_changed = false;
        uint64_t generation = 0;
    };

    RefreshOutcome RefreshFromDisk() noexcept {
        paths::ActiveBridgePointer ptr{};
        state_ns::StateSnapshot snap{};
        snap.bridge_root = bridge_root;
        if (paths::ReadActiveBridgePointer(bridge_root, ptr) == ERROR_SUCCESS) {
            snap.flags = state_ns::kFlagGlobalEnabled | state_ns::kFlagSinkHooksEnabled |
                         state_ns::kFlagWinRtEnabled | state_ns::kFlagDirectRevealEnabled |
                         state_ns::kFlagOverlayEnabled;
            snap.generation = ptr.generation;
            snap.core_dir_x64 = ptr.core_dir_x64;
            snap.core_dir_x86 = ptr.core_dir_x86;
            snap.host_exe = ptr.host_exe;
        }
        RefreshOutcome out{};
        out.enabled = (snap.flags & state_ns::kFlagGlobalEnabled) != 0;
        out.generation = snap.generation;

        bool state_changed = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            out.generation_changed = (ptr.generation != 0 && ptr.generation != last_seen_generation);
            out.became_enabled = (!state_initialized && out.enabled) || (state_initialized && !last_seen_enabled && out.enabled);
            state_changed = !state_initialized || (out.enabled != last_seen_enabled) || out.generation_changed;
            if (ptr.generation != 0) last_seen_generation = ptr.generation;
            last_seen_enabled = out.enabled;
            state_initialized = true;
            current_state = snap;
            current_pointer = ptr;
        }
        publisher.Publish(snap);
        if (state_changed) {
            rlog::Event("bridge_state_refreshed", {
                {"enabled", out.enabled ? 1u : 0u},
                {"generation", out.generation},
            });
        }
        if (out.generation_changed && cleanup_wake_event) {
            SetEvent(cleanup_wake_event);
        }
        return out;
    }

    bool EnqueueCandidate(DWORD pid, uint64_t creation_time, CandidateSource source) noexcept {
        if (pid <= 4 || pid == GetCurrentProcessId()) return false;
        const ULONGLONG now = GetTickCount64();
        bool enqueued = false;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            auto it = tracked.find(pid);
            if (it != tracked.end()) {
                TrackedProcess& entry = it->second;
                const bool pid_reused = (creation_time != 0 && entry.creation_time != 0 &&
                                         entry.creation_time != creation_time);
                if (!pid_reused) {
                    if (entry.queued) return false;
                    if (entry.settled) {
                        if (entry.creation_time != 0 && creation_time != 0 &&
                            entry.creation_time == creation_time) {
                            return false;
                        }
                        if ((now - entry.last_touch_tick) < 10000) return false;
                    }
                }
            }
            if (pending_queue.size() >= 512) return false;
            tracked[pid] = TrackedProcess{creation_time, false, true, now};
            pending_queue.push_back(PendingCandidate{pid, creation_time, source, 0, now});
            enqueued = true;
        }
        if (enqueued && work_event) SetEvent(work_event);
        return enqueued;
    }

    void ClearSettledTracking() noexcept {
        std::lock_guard<std::mutex> lock(queue_mutex);
        for (auto it = tracked.begin(); it != tracked.end();) {
            if (it->second.queued) {
                ++it;
            } else {
                it = tracked.erase(it);
            }
        }
    }

    void MarkTrackedResult(DWORD pid, uint64_t creation_time, bool settled) noexcept {
        const ULONGLONG now = GetTickCount64();
        std::lock_guard<std::mutex> lock(queue_mutex);
        auto it = tracked.find(pid);
        if (it == tracked.end()) {
            tracked[pid] = TrackedProcess{creation_time, settled, false, now};
            return;
        }
        if (creation_time != 0) it->second.creation_time = creation_time;
        it->second.settled = settled;
        it->second.queued = false;
        it->second.last_touch_tick = now;
    }

    bool ScheduleTransientRetry(const PendingCandidate& prev, uint64_t creation_time) noexcept {
        if (prev.attempt > 0) return false;
        uint64_t alive_ct = 0;
        if (!broker::QueryProcessCreationTime(prev.pid, alive_ct) || alive_ct == 0) return false;
        if (creation_time != 0 && alive_ct != creation_time) return false;

        const ULONGLONG now = GetTickCount64();
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (retry_queue.size() >= 128) return false;
            PendingCandidate next{};
            next.pid = prev.pid;
            next.creation_time = alive_ct;
            next.source = CandidateSource::Retry;
            next.attempt = static_cast<uint8_t>(prev.attempt + 1);
            next.ready_tick = now + kTransientRetryDelayMs;
            retry_queue.push_back(next);
            auto& entry = tracked[prev.pid];
            entry.creation_time = alive_ct;
            entry.settled = false;
            entry.queued = true;
            entry.last_touch_tick = now;
        }
        if (work_event) SetEvent(work_event);
        return true;
    }

    size_t ScanInteractiveProcesses(const wchar_t* reason, CandidateSource source, bool log_if_empty) noexcept {
        bool bridge_enabled = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            bridge_enabled = (current_state.flags & state_ns::kFlagGlobalEnabled) != 0;
        }
        if (!bridge_enabled) {
            if (log_if_empty) {
                rlog::TextEvent("process_scan", {{"reason", reason}}, {
                    {"bridge_enabled", 0},
                    {"total_interactive", 0},
                    {"newly_enqueued", 0},
                    {"pruned", 0},
                });
            }
            return 0;
        }

        const auto pids = broker::EnumerateInteractiveProcessIds();
        std::unordered_set<DWORD> alive(pids.begin(), pids.end());

        size_t pruned = 0;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            for (auto it = tracked.begin(); it != tracked.end();) {
                if (!it->second.queued && alive.find(it->first) == alive.end()) {
                    it = tracked.erase(it);
                    ++pruned;
                } else {
                    ++it;
                }
            }
        }

        size_t newly_enqueued = 0;
        for (DWORD pid : pids) {
            bool already_queued = false;
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                auto it = tracked.find(pid);
                if (it != tracked.end() && it->second.queued) already_queued = true;
            }
            if (already_queued) continue;

            uint64_t ct = 0;
            broker::QueryProcessCreationTime(pid, ct);
            if (EnqueueCandidate(pid, ct, source)) {
                ++newly_enqueued;
            }
        }

        if (newly_enqueued > 0 || log_if_empty) {
            rlog::TextEvent("process_scan", {{"reason", reason}}, {
                {"bridge_enabled", 1},
                {"total_interactive", static_cast<uint64_t>(pids.size())},
                {"newly_enqueued", static_cast<uint64_t>(newly_enqueued)},
                {"pruned", static_cast<uint64_t>(pruned)},
            });
        }
        return newly_enqueued;
    }

    void ProcessCandidate(const PendingCandidate& cand, HANDLE stop_evt) noexcept {
        if (stop_evt && WaitForSingleObject(stop_evt, 0) == WAIT_OBJECT_0) {
            MarkTrackedResult(cand.pid, cand.creation_time, false);
            return;
        }
        state_ns::StateSnapshot snap;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            snap = current_state;
        }
        if (!(snap.flags & state_ns::kFlagGlobalEnabled)) {
            MarkTrackedResult(cand.pid, cand.creation_time, false);
            return;
        }

        uint64_t creation_time = cand.creation_time;
        if (creation_time == 0) {
            broker::QueryProcessCreationTime(cand.pid, creation_time);
        }

        // Wait only the remaining settle budget for brand-new processes so already-running
        // processes caught by startup/catchup scan are evaluated with zero sleep overhead.
        if (cand.attempt == 0) {
            DWORD settle_wait = 0;
            if (creation_time == 0) {
                settle_wait = (cand.source == CandidateSource::Etw) ? kNewProcessSettleMs : 0;
            } else {
                const uint64_t age_ms = ProcessAgeMs(creation_time);
                if (age_ms < kNewProcessSettleMs) {
                    settle_wait = static_cast<DWORD>(kNewProcessSettleMs - age_ms);
                }
            }
            if (settle_wait > 0) {
                if (stop_evt) {
                    if (WaitForSingleObject(stop_evt, settle_wait) == WAIT_OBJECT_0) {
                        MarkTrackedResult(cand.pid, creation_time, false);
                        return;
                    }
                } else {
                    Sleep(settle_wait);
                }
            }
        }

        const auto info = broker::InspectLiveProcess(cand.pid);
        if (info.creation_time != 0) creation_time = info.creation_time;
        const auto decision = broker::EvaluateProcessPolicy(info, &snap);
        if (decision.verdict != broker::AdmissionVerdict::Eligible) {
            if (info.is_elevated &&
                decision.verdict != broker::AdmissionVerdict::SkipBuiltInExcluded &&
                decision.verdict != broker::AdmissionVerdict::SkipAlreadyLoaded &&
                decision.verdict != broker::AdmissionVerdict::SkipInvalidProcess) {
                rlog::TextEvent("process_skip_elevated", {
                    {"image", info.basename},
                    {"reason", AsciiToWide(decision.reason_text)},
                    {"source", SourceName(cand.source)},
                }, {
                    {"pid", info.pid},
                    {"session_id", info.session_id},
                    {"machine", info.machine},
                    {"subsystem", info.pe_subsystem},
                    {"verdict", static_cast<uint64_t>(decision.verdict)},
                });
            }
            MarkTrackedResult(cand.pid, creation_time, true);
            return;
        }
        if (stop_evt && WaitForSingleObject(stop_evt, 0) == WAIT_OBJECT_0) {
            MarkTrackedResult(cand.pid, creation_time, false);
            return;
        }

        const uint32_t activate_flags = PULSE_STUB_FLAG_INSTALL_SINK_HOOKS |
            ((snap.flags & state_ns::kFlagWinRtEnabled) ? PULSE_STUB_FLAG_WINRT_ENABLED : 0u) |
            ((snap.flags & state_ns::kFlagDirectRevealEnabled) ? PULSE_STUB_FLAG_DIRECT_REVEAL : 0u);

        broker::InjectionResult res{};
        bool attempted = false;
        if (decision.target_machine == IMAGE_FILE_MACHINE_AMD64) {
            const std::wstring stub64 = bridge_root + L"\\" + paths::kStubDllName64;
            const DWORD verify_err = pulse::dialog_admin::VerifyProtectedPath(stub64, false);
            if (verify_err == ERROR_SUCCESS) {
                attempted = true;
                res = broker::InjectSameArchStub(cand.pid, stub64, activate_flags);
            } else {
                rlog::TextEvent("process_verify_failed", {
                    {"image", info.basename},
                    {"path", stub64},
                }, {
                    {"pid", info.pid},
                    {"machine", decision.target_machine},
                    {"error", verify_err},
                }, rlog::Level::Warning);
            }
        } else if (decision.target_machine == IMAGE_FILE_MACHINE_I386) {
            const std::wstring helper32 = bridge_root + L"\\" + paths::kInject32ExeName;
            const std::wstring stub32 = bridge_root + L"\\" + paths::kStubDllName32;
            const DWORD verify_helper = pulse::dialog_admin::VerifyProtectedPath(helper32, false);
            const DWORD verify_stub = (verify_helper == ERROR_SUCCESS)
                ? pulse::dialog_admin::VerifyProtectedPath(stub32, false)
                : verify_helper;
            if (verify_stub == ERROR_SUCCESS) {
                attempted = true;
                res = broker::InjectViaHelper32(helper32, cand.pid, stub32, activate_flags);
            } else {
                rlog::TextEvent("process_verify_failed", {
                    {"image", info.basename},
                    {"path", verify_helper != ERROR_SUCCESS ? helper32 : stub32},
                }, {
                    {"pid", info.pid},
                    {"machine", decision.target_machine},
                    {"error", verify_stub},
                }, rlog::Level::Warning);
            }
        }

        if (!attempted) {
            MarkTrackedResult(cand.pid, creation_time, true);
            return;
        }

        const bool ok = (res.error == ERROR_SUCCESS && res.activate_result == ERROR_SUCCESS);
        if (!res.already_loaded) {
            if (ok) injected_ok.fetch_add(1, std::memory_order_relaxed);
            else injected_fail.fetch_add(1, std::memory_order_relaxed);
        }
        const wchar_t* stage = ok
            ? (res.already_loaded ? L"already_loaded" : L"ok")
            : (res.error != ERROR_SUCCESS ? L"inject_failed" : L"activate_failed");

        rlog::TextEvent("process_inject", {
            {"image", info.basename},
            {"source", SourceName(cand.source)},
            {"stage", stage},
        }, {
            {"pid", info.pid},
            {"session_id", info.session_id},
            {"machine", decision.target_machine},
            {"elevated", info.is_elevated ? 1u : 0u},
            {"error", res.error},
            {"remote_return", res.activate_result},
            {"already_loaded", res.already_loaded ? 1u : 0u},
            {"attempt", cand.attempt},
        }, ok ? rlog::Level::Info : rlog::Level::Warning);

        if (!ok && cand.attempt == 0 && res.error != ERROR_EXE_MACHINE_TYPE_MISMATCH) {
            if (ScheduleTransientRetry(cand, creation_time)) return;
        }
        MarkTrackedResult(cand.pid, creation_time, true);
    }

    DWORD StartEtwWatcher() noexcept {
        ++etw_start_attempts;
        const DWORD err = etw.Start([this](const broker::ProcessStartEvent& evt) {
            if (evt.session_id == 0) return;
            EnqueueCandidate(evt.pid, 0, CandidateSource::Etw);
        }, etw_session_name);
        const bool healthy = (err == ERROR_SUCCESS && etw.Healthy());
        rlog::TextEvent("etw_start", {
            {"stage", AsciiToWide(etw.LastStartStage())},
            {"session", etw_session_name},
        }, {
            {"error", err},
            {"attempt", etw_start_attempts},
            {"healthy", healthy ? 1u : 0u},
        }, healthy ? rlog::Level::Info : rlog::Level::Warning);
        return err;
    }

    int RunLoop(HANDLE stop_evt, const BrokerOptions& opts) {
        bridge_root = opts.bridge_root_override.empty()
            ? paths::CommonDialogBridgeRoot()
            : opts.bridge_root_override;
        etw_session_name = opts.console_mode
            ? (L"PulseDialogBrokerConsoleSession-" + std::to_wstring(GetCurrentProcessId()))
            : std::wstring(broker::kDefaultEtwSessionName);

        const std::wstring log_root = opts.log_root_override.empty()
            ? DefaultLogRoot()
            : opts.log_root_override;
        rlog::Initialize(log_root, "dialog-broker");

        work_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        cleanup_wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        const std::wstring state_name = (opts.console_mode && !opts.bridge_root_override.empty())
            ? (L"Local\\PulseDialogBridgeStateConsole-" + std::to_wstring(GetCurrentProcessId()))
            : std::wstring(state_ns::kDefaultGlobalStateName);
        publisher.Open(state_name, !opts.console_mode);
        const RefreshOutcome initial_state = RefreshFromDisk();

        rlog::TextEvent("broker_started", {
            {"bridge_root", bridge_root},
            {"etw_session", etw_session_name},
        }, {
            {"console_mode", opts.console_mode ? 1u : 0u},
            {"elevated", pulse::dialog_admin::IsElevated() ? 1u : 0u},
            {"bridge_enabled", initial_state.enabled ? 1u : 0u},
            {"generation", initial_state.generation},
        });

        const DWORD etw_err = StartEtwWatcher();
        bool etw_was_healthy = (etw_err == ERROR_SUCCESS && etw.Healthy());
        bool fallback_poll_active = !etw_was_healthy;
        size_t retry_backoff_index = 0;
        const ULONGLONG start_tick = GetTickCount64();
        ULONGLONG next_etw_retry_tick = fallback_poll_active
            ? (start_tick + kEtwRetryBackoffMs[retry_backoff_index])
            : 0;
        ULONGLONG last_fallback_scan_tick = start_tick;

        if (opts.console_mode) {
            std::printf("PulseDialogBroker console mode: bridge_root=%ls session=%ls etw_start_err=%lu stage=%s\n",
                        bridge_root.c_str(), etw_session_name.c_str(), etw_err, etw.LastStartStage());
        }

        if (fallback_poll_active) {
            rlog::TextEvent("fallback_poll_state", {{"reason", L"etw_start_failed"}}, {
                {"active", 1},
                {"interval_ms", kFallbackPollIntervalMs},
                {"next_etw_retry_ms", kEtwRetryBackoffMs[retry_backoff_index]},
            }, rlog::Level::Warning);
        }

        // Watch bridge_root for active-bridge.json updates to republish Global\PulseDialogBridgeState immediately.
        HANDLE dir_watch = FindFirstChangeNotificationW(
            bridge_root.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);

        // Background non-intrusive cleanup thread: automatically retires unlocked legacy
        // DialogBridge\<hash>\ dirs, empty DialogBridge\, obsolete AppRuntime versions,
        // and stale Common Files .old./v\ artifacts after processes exit or on reboot.
        // Sleeps indefinitely (zero polling) once no residuals remain.
        std::thread cleanup_thread;
        if (!opts.console_mode && cleanup_wake_event) {
            cleanup_thread = std::thread([this, stop_evt]() {
                constexpr DWORD kInitialDelayMs = 5000;
                constexpr DWORD kRetryPendingMs = 10u * 60u * 1000u;
                DWORD next_wait = kInitialDelayMs;
                HANDLE waits[2]{stop_evt, cleanup_wake_event};
                for (;;) {
                    const DWORD w = WaitForMultipleObjects(2, waits, FALSE, next_wait);
                    if (w == WAIT_OBJECT_0) break;
                    paths::ActiveBridgePointer ptr;
                    {
                        std::lock_guard<std::mutex> lock(state_mutex);
                        ptr = current_pointer;
                    }
                    broker::ResidualCleanupOptions clean_opts;
                    clean_opts.verify_protected = true;
                    clean_opts.pass_test_flag = false;
                    const auto res = broker::RunResidualCleanupPass(bridge_root, ptr, clean_opts, stop_evt);
                    rlog::Event("cleanup_pass", {
                        {"stale_files_removed", res.stale_files_removed},
                        {"core_dirs_removed", res.core_dirs_removed},
                        {"install_cleanup_launched", res.install_cleanup_launched ? 1u : 0u},
                        {"install_cleanup_exit_code", res.install_cleanup_exit_code},
                        {"pending_after", res.pending_after ? 1u : 0u},
                    });
                    if (WaitForSingleObject(stop_evt, 0) == WAIT_OBJECT_0) break;
                    next_wait = res.pending_after ? kRetryPendingMs : INFINITE;
                }
            });
        }

        std::thread worker([this, stop_evt]() {
            HANDLE waits[2]{stop_evt, work_event};
            for (;;) {
                PendingCandidate cand{};
                bool have_cand = false;
                DWORD wait_ms = INFINITE;
                {
                    const ULONGLONG now = GetTickCount64();
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    if (!pending_queue.empty()) {
                        cand = pending_queue.front();
                        pending_queue.pop_front();
                        have_cand = true;
                    } else if (!retry_queue.empty()) {
                        size_t ready_idx = retry_queue.size();
                        ULONGLONG soonest = UINT64_MAX;
                        for (size_t i = 0; i < retry_queue.size(); ++i) {
                            if (retry_queue[i].ready_tick <= now) {
                                ready_idx = i;
                                break;
                            }
                            if (retry_queue[i].ready_tick < soonest) {
                                soonest = retry_queue[i].ready_tick;
                            }
                        }
                        if (ready_idx < retry_queue.size()) {
                            cand = retry_queue[ready_idx];
                            retry_queue.erase(retry_queue.begin() + static_cast<ptrdiff_t>(ready_idx));
                            have_cand = true;
                        } else if (soonest != UINT64_MAX) {
                            const ULONGLONG delta = soonest - now;
                            wait_ms = delta > 5000 ? 5000u : static_cast<DWORD>(delta);
                        }
                    }
                }
                if (have_cand) {
                    if (WaitForSingleObject(stop_evt, 0) == WAIT_OBJECT_0) break;
                    ProcessCandidate(cand, stop_evt);
                    continue;
                }
                const DWORD w = WaitForMultipleObjects(2, waits, FALSE, wait_ms);
                if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) break;
            }
        });

        // Catch up already-running interactive GUI processes (including elevated apps that
        // started before the Broker service started or upgraded).
        ScanInteractiveProcesses(L"startup_scan", CandidateSource::StartupScan, true);

        for (;;) {
            HANDLE handles[2]{stop_evt, dir_watch};
            const DWORD count = (dir_watch && dir_watch != INVALID_HANDLE_VALUE) ? 2u : 1u;
            const DWORD wait_timeout = fallback_poll_active ? kFallbackPollIntervalMs : kSteadyWaitIntervalMs;
            const DWORD wait = WaitForMultipleObjects(count, handles, FALSE, wait_timeout);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;

            bool state_triggered_scan = false;
            const wchar_t* state_scan_reason = nullptr;
            if (count == 2 && wait == WAIT_OBJECT_0 + 1) {
                if (WaitForSingleObject(stop_evt, 50) == WAIT_OBJECT_0) break;
                const RefreshOutcome refreshed = RefreshFromDisk();
                FindNextChangeNotification(dir_watch);
                if (refreshed.became_enabled) {
                    ClearSettledTracking();
                    state_triggered_scan = true;
                    state_scan_reason = L"bridge_enabled_scan";
                } else if (refreshed.generation_changed && refreshed.enabled) {
                    state_triggered_scan = true;
                    state_scan_reason = L"generation_changed_scan";
                }
            } else if (wait == WAIT_TIMEOUT) {
                const RefreshOutcome refreshed = RefreshFromDisk();
                if (refreshed.became_enabled) {
                    ClearSettledTracking();
                    state_triggered_scan = true;
                    state_scan_reason = L"bridge_enabled_scan";
                } else if (refreshed.generation_changed && refreshed.enabled) {
                    state_triggered_scan = true;
                    state_scan_reason = L"generation_changed_scan";
                }
            }

            const ULONGLONG now = GetTickCount64();

            // Monitor ETW consumer thread health; detect silent ProcessTrace exits and recover.
            const bool etw_now_healthy = etw.Healthy();
            if (etw_was_healthy && !etw_now_healthy) {
                const DWORD pt_err = etw.LastProcessTraceError();
                const uint64_t evts = etw.EventsReceived();
                rlog::TextEvent("etw_exited", {{"session", etw_session_name}}, {
                    {"process_trace_error", pt_err},
                    {"events_received", evts},
                }, rlog::Level::Warning);
                etw.Stop();
                etw_was_healthy = false;
                fallback_poll_active = true;
                retry_backoff_index = 0;
                next_etw_retry_tick = now + kEtwRetryBackoffMs[retry_backoff_index];
                rlog::TextEvent("fallback_poll_state", {{"reason", L"etw_consumer_exited"}}, {
                    {"active", 1},
                    {"interval_ms", kFallbackPollIntervalMs},
                    {"next_etw_retry_ms", kEtwRetryBackoffMs[retry_backoff_index]},
                }, rlog::Level::Warning);
                ScanInteractiveProcesses(L"etw_degraded_scan", CandidateSource::FallbackPoll, true);
                last_fallback_scan_tick = now;
            } else if (!etw_now_healthy && now >= next_etw_retry_tick) {
                etw.Stop();
                const DWORD retry_err = StartEtwWatcher();
                if (retry_err == ERROR_SUCCESS && etw.Healthy()) {
                    etw_was_healthy = true;
                    fallback_poll_active = false;
                    retry_backoff_index = 0;
                    next_etw_retry_tick = 0;
                    rlog::TextEvent("fallback_poll_state", {{"reason", L"etw_recovered"}}, {
                        {"active", 0},
                        {"interval_ms", 0},
                        {"next_etw_retry_ms", 0},
                    });
                    ScanInteractiveProcesses(L"etw_recovered_scan", CandidateSource::StartupScan, true);
                    last_fallback_scan_tick = now;
                    state_triggered_scan = false;
                } else {
                    if (retry_backoff_index + 1 < kEtwRetryBackoffMs.size()) {
                        ++retry_backoff_index;
                    }
                    next_etw_retry_tick = now + kEtwRetryBackoffMs[retry_backoff_index];
                }
            }

            if (state_triggered_scan && state_scan_reason) {
                ScanInteractiveProcesses(state_scan_reason, CandidateSource::StartupScan, true);
                last_fallback_scan_tick = now;
            } else if (fallback_poll_active && (now - last_fallback_scan_tick) >= kFallbackPollIntervalMs) {
                ScanInteractiveProcesses(L"fallback_poll", CandidateSource::FallbackPoll, false);
                last_fallback_scan_tick = now;
            }
        }

        const uint64_t total_etw_events = etw.EventsReceived();
        etw.Stop();
        if (worker.joinable()) worker.join();
        if (cleanup_thread.joinable()) cleanup_thread.join();
        if (dir_watch && dir_watch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(dir_watch);
        if (work_event) CloseHandle(work_event);
        if (cleanup_wake_event) CloseHandle(cleanup_wake_event);
        publisher.Close();

        rlog::Event("broker_stopped", {
            {"etw_events_received", total_etw_events},
            {"injected_ok", injected_ok.load(std::memory_order_relaxed)},
            {"injected_fail", injected_fail.load(std::memory_order_relaxed)},
        });
        rlog::Flush(2000);
        rlog::Shutdown();
        return 0;
    }
};

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_status_handle = RegisterServiceCtrlHandlerExW(kServiceName, &ServiceCtrlHandler, nullptr);
    if (!g_status_handle) return;
    ReportServiceState(SERVICE_START_PENDING, NO_ERROR, 3000);

    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop_event) {
        ReportServiceState(SERVICE_STOPPED, GetLastError());
        return;
    }

    ReportServiceState(SERVICE_RUNNING);
    BrokerRuntime runtime;
    BrokerOptions opts{};
    runtime.RunLoop(g_stop_event, opts);

    CloseHandle(g_stop_event);
    g_stop_event = nullptr;
    ReportServiceState(SERVICE_STOPPED);
}

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl) noexcept {
    if (ctrl == CTRL_C_EVENT || ctrl == CTRL_BREAK_EVENT || ctrl == CTRL_CLOSE_EVENT) {
        if (g_stop_event) SetEvent(g_stop_event);
        return TRUE;
    }
    return FALSE;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    BrokerOptions opts{};
    std::wstring stop_event_name;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--console") == 0) opts.console_mode = true;
        else if (_wcsicmp(argv[i], L"--stop-event") == 0 && i + 1 < argc) stop_event_name = argv[++i];
        else if (_wcsicmp(argv[i], L"--log-root") == 0 && i + 1 < argc) opts.log_root_override = argv[++i];
        else if (_wcsicmp(argv[i], L"--bridge-root") == 0 && i + 1 < argc) opts.bridge_root_override = argv[++i];
    }

    if (opts.console_mode) {
        if (!stop_event_name.empty()) {
            g_stop_event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, stop_event_name.c_str());
        }
        if (!g_stop_event) {
            g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        }
        SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE);
        BrokerRuntime runtime;
        const int rc = runtime.RunLoop(g_stop_event, opts);
        if (g_stop_event) CloseHandle(g_stop_event);
        return rc;
    }

    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), &ServiceMain},
        {nullptr, nullptr}
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        return static_cast<int>(GetLastError());
    }
    return 0;
}

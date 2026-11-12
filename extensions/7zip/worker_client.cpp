// worker_client.cpp - implements the engine::* interface inside pulse_7zip.dll
// by delegating all 7z.dll decoding/encoding/hashing to a Job-Object-governed
// out-of-process worker executable (pulse_7zip_worker.exe).
// pulse_7zip.dll NEVER links or loads 7z.dll in-process.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "worker_client.h"
#include "worker_ipc.h"
#include "worker_protocol.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace worker_ipc;
namespace fs = std::filesystem;

constexpr DWORD kConnectTimeoutMs = 5000u;
constexpr DWORD kHandshakeTimeoutMs = 5000u;
constexpr DWORD kControlWriteTimeoutMs = 2000u;
constexpr DWORD kRequestWriteTimeoutMs = 10000u;

std::mutex g_config_lock;
worker_client::WorkerLimits g_limits{};

worker_client::WorkerLimits CurrentLimits() noexcept {
    std::lock_guard guard(g_config_lock);
    return g_limits;
}

std::atomic<uint64_t> g_next_request_id{0};
std::atomic<uint64_t> g_next_task_generation{0};
std::atomic<uint64_t> g_preview_generation{0};

void AnchorInClientModule() noexcept {}

fs::path ResolveWorkerExecutablePath() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&AnchorInClientModule), &self) || !self) {
        return {};
    }
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(self, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        if (buf.size() >= 32768) return {};
        buf.resize(buf.size() * 2);
    }
    return fs::path(buf).parent_path() / L"pulse_7zip_worker.exe";
}

bool SafeCallCancelled(const engine::Cancelled& cancelled) noexcept {
    try { return cancelled && cancelled(); } catch (...) { return true; }
}

std::string Hex32(DWORD code) {
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(code));
    return buf;
}

class WorkerSession {
    HANDLE job_ = nullptr;
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
    size_t memory_limit_bytes_ = 0;
    bool allow_faults_ = false;
    bool tainted_ = false;
    PipeChannel channel_;

public:
    WorkerSession() = default;
    ~WorkerSession() { ShutdownCleanly(); }
    WorkerSession(const WorkerSession&) = delete;
    WorkerSession& operator=(const WorkerSession&) = delete;

    DWORD Pid() const noexcept { return pid_; }
    size_t MemoryLimitBytes() const noexcept { return memory_limit_bytes_; }
    bool AllowFaults() const noexcept { return allow_faults_; }

    bool Healthy() const noexcept {
        return !tainted_ && process_ != nullptr && channel_.Valid() &&
               WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
    }

    void KillAndReap() noexcept {
        tainted_ = true;
        channel_.Close();
        if (job_) {
            TerminateJobObject(job_, ERROR_PROCESS_ABORTED);
        }
        if (process_) {
            if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process_, ERROR_PROCESS_ABORTED);
            }
            WaitForSingleObject(process_, 3000);
            CloseHandle(process_);
            process_ = nullptr;
        }
        if (job_) {
            CloseHandle(job_);
            job_ = nullptr;
        }
        pid_ = 0;
    }

    void ShutdownCleanly() noexcept {
        if (!process_) {
            channel_.Close();
            if (job_) { CloseHandle(job_); job_ = nullptr; }
            return;
        }
        if (!tainted_ && channel_.Valid() && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
            channel_.WriteFrame(FrameType::MsgShutdown, 0, 0, nullptr, 0, process_, 100);
            channel_.Close();
            if (WaitForSingleObject(process_, 150) == WAIT_OBJECT_0) {
                CloseHandle(process_);
                process_ = nullptr;
                if (job_) { CloseHandle(job_); job_ = nullptr; }
                pid_ = 0;
                return;
            }
        }
        KillAndReap();
    }

    bool Spawn(size_t memory_limit_bytes, bool allow_faults, std::string& error) {
        KillAndReap();
        tainted_ = false;
        memory_limit_bytes_ = memory_limit_bytes;
        allow_faults_ = allow_faults;

        const fs::path worker_exe = ResolveWorkerExecutablePath();
        std::error_code ec;
        if (worker_exe.empty() || !fs::exists(worker_exe, ec)) {
            error = "pulse_7zip_worker.exe not found in extension package directory";
            tainted_ = true;
            return false;
        }

        const std::wstring pipe_name = MakeUniquePipeName();
        HANDLE raw_pipe = CreateAuthenticatedServerPipe(pipe_name);
        if (raw_pipe == INVALID_HANDLE_VALUE) {
            error = "failed to create authenticated worker named pipe";
            tainted_ = true;
            return false;
        }
        channel_.Reset(raw_pipe);

        job_ = CreateJobObjectW(nullptr, nullptr);
        if (!job_) {
            error = "failed to create Windows Job Object for worker";
            KillAndReap();
            return false;
        }

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
            JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
            JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        if (memory_limit_bytes > 0) {
            limits.BasicLimitInformation.LimitFlags |=
                JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY;
            limits.ProcessMemoryLimit = memory_limit_bytes;
            limits.JobMemoryLimit = memory_limit_bytes;
        }
        if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            error = "failed to configure Windows Job Object limits";
            KillAndReap();
            return false;
        }

        std::wstring cmdline = L"\"" + worker_exe.wstring() + L"\" --pipe " + pipe_name;
        if (allow_faults) cmdline += L" --allow-fault-injection";
        const std::wstring work_dir = worker_exe.parent_path().wstring();

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(worker_exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                            nullptr, work_dir.c_str(), &si, &pi)) {
            error = "failed to launch pulse_7zip_worker.exe (" + Hex32(GetLastError()) + ")";
            KillAndReap();
            return false;
        }
        process_ = pi.hProcess;
        pid_ = pi.dwProcessId;

        if (!AssignProcessToJobObject(job_, process_)) {
            CloseHandle(pi.hThread);
            error = "failed to assign pulse_7zip_worker.exe to Job Object (" + Hex32(GetLastError()) + ")";
            KillAndReap();
            return false;
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);

        if (WaitServerPipeConnect(channel_.NativeHandle(), process_, kConnectTimeoutMs) != IoStatus::Ok) {
            error = "pulse_7zip_worker.exe failed to connect to pipe";
            KillAndReap();
            return false;
        }

        ULONG client_pid = 0;
        if (!GetNamedPipeClientProcessId(channel_.NativeHandle(), &client_pid) || client_pid != pid_) {
            error = "named pipe client PID verification failed";
            KillAndReap();
            return false;
        }

        std::array<uint8_t, kSessionNonceBytes> nonce{};
        if (!RandomBytes(nonce.data(), nonce.size())) {
            error = "CSPRNG failure generating session nonce";
            KillAndReap();
            return false;
        }
        if (channel_.WriteFrame(FrameType::Hello, 0, 0, nonce.data(),
                                static_cast<uint32_t>(nonce.size()), process_, kHandshakeTimeoutMs) != IoStatus::Ok) {
            error = "failed sending worker handshake";
            KillAndReap();
            return false;
        }

        FrameHeader ack_hdr{};
        std::vector<uint8_t> ack_payload;
        if (channel_.ReadFrame(ack_hdr, ack_payload, process_, {}, kHandshakeTimeoutMs) != IoStatus::Ok ||
            static_cast<FrameType>(ack_hdr.type) != FrameType::HelloAck ||
            ack_hdr.request_id != 0 || ack_hdr.generation != 0) {
            error = "invalid worker handshake reply";
            KillAndReap();
            return false;
        }
        ByteReader ack_reader(ack_payload);
        std::array<uint8_t, kSessionNonceBytes> echoed_nonce{};
        uint32_t echoed_pid = 0;
        if (!ack_reader.Raw(echoed_nonce.data(), echoed_nonce.size()) ||
            !ack_reader.U32(echoed_pid) || !ack_reader.Empty() ||
            echoed_nonce != nonce || echoed_pid != pid_) {
            error = "worker handshake nonce or PID mismatch";
            KillAndReap();
            return false;
        }
        return true;
    }

    void ClassifyWorkerDeath(DWORD saved_pid, engine::Status& out_status, std::string& out_error) noexcept {
        DWORD exit_code = 0;
        if (process_) {
            WaitForSingleObject(process_, 500);
            GetExitCodeProcess(process_, &exit_code);
        }
        size_t peak_mem = 0;
        if (job_) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            if (QueryInformationJobObject(job_, JobObjectExtendedLimitInformation, &info, sizeof(info), nullptr)) {
                peak_mem = info.PeakProcessMemoryUsed;
            }
        }
        const bool hit_mem_cap =
            exit_code == 0xC0000017u ||  // STATUS_NO_MEMORY
            exit_code == 0xC0000044u ||  // STATUS_QUOTA_EXCEEDED
            exit_code == ERROR_NOT_ENOUGH_MEMORY ||
            exit_code == ERROR_COMMITMENT_LIMIT ||
            (memory_limit_bytes_ > 0 && peak_mem + (4u * 1024u * 1024u) >= memory_limit_bytes_);
        if (hit_mem_cap) {
            out_status = engine::Status::WorkerMemoryLimit;
            out_error = "pulse_7zip_worker.exe exceeded memory cap (pid " + std::to_string(saved_pid) +
                        ", peak " + std::to_string(peak_mem / (1024u * 1024u)) + " MiB, exit " + Hex32(exit_code) + ")";
        } else {
            out_status = engine::Status::WorkerCrashed;
            out_error = "pulse_7zip_worker.exe exited unexpectedly (pid " + std::to_string(saved_pid) +
                        ", exit " + Hex32(exit_code) + ")";
        }
    }

    bool Execute(FrameType req_type,
                 FrameType expected_resp_type,
                 uint64_t request_id,
                 uint64_t generation,
                 const std::vector<uint8_t>& req_payload,
                 const engine::Cancelled& cancelled,
                 const engine::Report& report,
                 const engine::AskPassword& ask_password,
                 uint32_t hard_timeout_ms,
                 uint32_t cancel_grace_ms,
                 std::vector<uint8_t>& out_resp_payload,
                 engine::Status& out_failure_status,
                 std::string& out_failure_error) {
        out_resp_payload.clear();
        const DWORD saved_pid = pid_;

        const auto write_io = channel_.WriteFrame(req_type, request_id, generation, req_payload,
                                                  process_, kRequestWriteTimeoutMs);
        if (write_io != IoStatus::Ok) {
            if (write_io == IoStatus::OversizedFrame) {
                out_failure_status = engine::Status::InvalidInput;
                out_failure_error = "request exceeds IPC frame limit";
                return false;
            }
            ClassifyWorkerDeath(saved_pid, out_failure_status, out_failure_error);
            KillAndReap();
            return false;
        }

        const bool has_deadline = hard_timeout_ms > 0;
        ULONGLONG deadline_tick = has_deadline ? GetTickCount64() + hard_timeout_ms : 0;
        bool cancel_sent = false;
        ULONGLONG cancel_kill_tick = 0;
        bool timed_out = false;
        bool cancel_grace_expired = false;

        for (;;) {
            FrameHeader hdr{};
            std::vector<uint8_t> payload;
            const auto poll_abort = [&]() -> bool {
                const ULONGLONG now = GetTickCount64();
                if (!cancel_sent && SafeCallCancelled(cancelled)) {
                    // Send cooperative cancel on full-duplex pipe WITHOUT interrupting ReadFrame!
                    cancel_sent = true;
                    cancel_kill_tick = now + std::max<uint32_t>(cancel_grace_ms, 50u);
                    channel_.WriteFrame(FrameType::MsgCancel, request_id, generation,
                                        nullptr, 0, process_, kControlWriteTimeoutMs);
                }
                if (cancel_sent && now >= cancel_kill_tick) {
                    cancel_grace_expired = true;
                    return true;
                }
                if (has_deadline && now >= deadline_tick) {
                    timed_out = true;
                    return true;
                }
                return false;
            };

            const auto io = channel_.ReadFrame(hdr, payload, process_, poll_abort, INFINITE);
            if (io != IoStatus::Ok) {
                if (cancel_grace_expired || (cancel_sent && SafeCallCancelled(cancelled))) {
                    KillAndReap();
                    out_failure_status = engine::Status::Cancelled;
                    out_failure_error = "Cancelled";
                    return false;
                }
                if (timed_out || io == IoStatus::TimedOut) {
                    KillAndReap();
                    out_failure_status = engine::Status::WorkerTimedOut;
                    out_failure_error = "pulse_7zip_worker.exe timed out (pid " + std::to_string(saved_pid) +
                                        ", limit " + std::to_string(hard_timeout_ms) + " ms)";
                    return false;
                }
                if (io == IoStatus::OversizedFrame) {
                    KillAndReap();
                    out_failure_status = engine::Status::LimitReached;
                    out_failure_error = "worker response exceeded 16 MiB IPC frame limit";
                    return false;
                }
                if (io == IoStatus::ProtocolError) {
                    KillAndReap();
                    out_failure_status = engine::Status::WorkerCrashed;
                    out_failure_error = "worker sent malformed IPC frame";
                    return false;
                }
                ClassifyWorkerDeath(saved_pid, out_failure_status, out_failure_error);
                KillAndReap();
                return false;
            }

            // Strict request_id & generation isolation check (D2/D3).
            if (hdr.request_id != request_id || hdr.generation != generation) {
                WipeSensitive(payload);
                KillAndReap();
                out_failure_status = engine::Status::WorkerCrashed;
                out_failure_error = "worker returned mismatched request_id or generation";
                return false;
            }

            const auto type = static_cast<FrameType>(hdr.type);
            if (type == FrameType::EvtProgress) {
                ByteReader reader(payload);
                engine::Progress progress{};
                if (!reader.Progress(progress) || !reader.Empty()) {
                    KillAndReap();
                    out_failure_status = engine::Status::WorkerCrashed;
                    out_failure_error = "invalid progress frame from worker";
                    return false;
                }
                if (report && !cancel_sent) {
                    try { report(progress); } catch (...) {}
                }
                continue;
            }

            if (type == FrameType::EvtNeedPassword) {
                ByteReader reader(payload);
                bool retry = false;
                if (!reader.Bool(retry) || !reader.Empty()) {
                    KillAndReap();
                    out_failure_status = engine::Status::WorkerCrashed;
                    out_failure_error = "invalid password prompt frame from worker";
                    return false;
                }
                const ULONGLONG prompt_started = GetTickCount64();
                std::wstring entered_pw;
                bool accepted = false;
                if (!cancel_sent && !SafeCallCancelled(cancelled) && ask_password) {
                    try { accepted = ask_password(retry, entered_pw); } catch (...) { accepted = false; }
                }
                if (has_deadline) {
                    deadline_tick += (GetTickCount64() - prompt_started);
                }

                ByteWriter reply(/*sensitive=*/true);
                reply.Bool(accepted);
                reply.WStr(accepted ? entered_pw : std::wstring{});
                WipeSensitive(entered_pw);

                if (channel_.WriteFrame(FrameType::MsgPasswordReply, request_id, generation,
                                        reply.Bytes(), process_, kControlWriteTimeoutMs) != IoStatus::Ok) {
                    ClassifyWorkerDeath(saved_pid, out_failure_status, out_failure_error);
                    KillAndReap();
                    return false;
                }
                continue;
            }

            if (type == expected_resp_type) {
                out_resp_payload = std::move(payload);
                return true;
            }

            WipeSensitive(payload);
            KillAndReap();
            out_failure_status = engine::Status::WorkerCrashed;
            out_failure_error = "unexpected IPC response opcode from worker";
            return false;
        }
    }
};

std::mutex g_warm_lock;
std::unique_ptr<WorkerSession> g_warm_preview_worker;

std::unique_ptr<WorkerSession> AcquirePreviewSession(const worker_client::WorkerLimits& limits, std::string& error) {
    {
        std::lock_guard guard(g_warm_lock);
        if (g_warm_preview_worker) {
            auto candidate = std::move(g_warm_preview_worker);
            if (candidate->Healthy() &&
                candidate->MemoryLimitBytes() == limits.preview_memory_bytes &&
                candidate->AllowFaults() == limits.allow_fault_injection) {
                return candidate;
            }
            candidate->ShutdownCleanly();
        }
    }
    auto fresh = std::make_unique<WorkerSession>();
    if (!fresh->Spawn(limits.preview_memory_bytes, limits.allow_fault_injection, error)) {
        return nullptr;
    }
    return fresh;
}

void ReleasePreviewSession(std::unique_ptr<WorkerSession> session) noexcept {
    if (!session) return;
    if (!session->Healthy()) {
        session->KillAndReap();
        return;
    }
    std::unique_ptr<WorkerSession> displaced;
    {
        std::lock_guard guard(g_warm_lock);
        if (!g_warm_preview_worker) {
            g_warm_preview_worker = std::move(session);
        } else {
            displaced = std::move(session);
        }
    }
    if (displaced) displaced->ShutdownCleanly();
}

std::mutex g_formats_lock;
std::vector<std::wstring> g_cached_formats;

uint32_t EffectiveHardTimeoutMs(uint32_t budget_duration_ms, uint32_t slack_ms, uint32_t explicit_cap_ms) noexcept {
    if (explicit_cap_ms > 0) return explicit_cap_ms;
    if (budget_duration_ms > 0) {
        return budget_duration_ms > UINT32_MAX - slack_ms ? UINT32_MAX : budget_duration_ms + slack_ms;
    }
    return 0;
}

} // namespace

namespace worker_client {

void ConfigureForTests(const WorkerLimits& limits) noexcept {
    {
        std::lock_guard guard(g_config_lock);
        g_limits = limits;
    }
    engine::ShutdownWorkers();
    std::lock_guard fmt_guard(g_formats_lock);
    g_cached_formats.clear();
}

void ResetForTests() noexcept {
    ConfigureForTests(WorkerLimits{});
}

uint32_t WarmWorkerPidForTests() noexcept {
    std::lock_guard guard(g_warm_lock);
    return g_warm_preview_worker && g_warm_preview_worker->Healthy() ? g_warm_preview_worker->Pid() : 0u;
}

FaultOutcome RunFaultForTests(FaultKind kind,
                              uint64_t param_bytes,
                              const std::wstring& lock_file,
                              const engine::Cancelled& cancelled,
                              bool use_warm_preview_slot) {
    FaultOutcome outcome;
    const auto limits = CurrentLimits();
    std::string spawn_error;
    std::unique_ptr<WorkerSession> session;
    if (use_warm_preview_slot) {
        session = AcquirePreviewSession(limits, spawn_error);
    } else {
        session = std::make_unique<WorkerSession>();
        if (!session->Spawn(limits.task_memory_bytes, limits.allow_fault_injection, spawn_error)) {
            session.reset();
        }
    }
    if (!session) {
        outcome.status = engine::Status::WorkerCrashed;
        outcome.error = spawn_error;
        return outcome;
    }
    outcome.worker_pid = session->Pid();

    FrameType opcode = FrameType::ReqFaultCrashAv;
    ByteWriter req;
    switch (kind) {
    case FaultKind::CrashAccessViolation:
        opcode = FrameType::ReqFaultCrashAv;
        break;
    case FaultKind::HangInfinite:
        opcode = FrameType::ReqFaultHang;
        req.WStr(lock_file);
        break;
    case FaultKind::ExceedMemory:
        opcode = FrameType::ReqFaultExceedMemory;
        req.U64(param_bytes);
        break;
    case FaultKind::OversizeFrame:
        opcode = FrameType::ReqFaultOversizeFrame;
        break;
    case FaultKind::WrongGeneration:
        opcode = FrameType::ReqFaultWrongGeneration;
        break;
    }

    const uint64_t req_id = ++g_next_request_id;
    const uint64_t gen = use_warm_preview_slot ? ++g_preview_generation : ++g_next_task_generation;
    const uint32_t hard_timeout_ms = limits.task_hard_timeout_ms > 0
        ? limits.task_hard_timeout_ms
        : (use_warm_preview_slot ? limits.preview_hard_timeout_slack_ms : 0u);

    std::vector<uint8_t> resp;
    if (!session->Execute(opcode, FrameType::RespFault, req_id, gen, req.Bytes(),
                          cancelled, {}, {}, hard_timeout_ms, limits.cancel_grace_ms,
                          resp, outcome.status, outcome.error)) {
        session->KillAndReap();
        return outcome;
    }

    ByteReader reader(resp);
    if (!reader.Status(outcome.status) || !reader.Str(outcome.error) || !reader.Empty()) {
        session->KillAndReap();
        outcome.status = engine::Status::WorkerCrashed;
        outcome.error = "malformed fault reply";
        return outcome;
    }
    if (use_warm_preview_slot && outcome.status == engine::Status::Ok) {
        ReleasePreviewSession(std::move(session));
    } else if (use_warm_preview_slot) {
        ReleasePreviewSession(std::move(session));
    } else {
        session->ShutdownCleanly();
    }
    return outcome;
}

} // namespace worker_client

namespace engine {

void ShutdownWorkers() noexcept {
    std::unique_ptr<WorkerSession> warm;
    {
        std::lock_guard guard(g_warm_lock);
        warm = std::move(g_warm_preview_worker);
    }
    if (warm) warm->ShutdownCleanly();
}

bool SafeRelativePath(const std::wstring& path) {
    if (path.empty() || path.size() > 32000 || path.front() == L'\\' || path.front() == L'/' ||
        (path.size() > 1 && path[1] == L':')) return false;
    size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find_first_of(L"\\/", start);
        auto part = path.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') return false;
        for (wchar_t c : part) if (c < 32 || std::wstring_view(L":*?\"<>|").find(c) != std::wstring_view::npos) return false;
        auto stem = part.substr(0, part.find(L'.'));
        for (auto& c : stem) c = static_cast<wchar_t>(towupper(c));
        if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
            (stem.size() == 4 && (stem.rfind(L"COM", 0) == 0 || stem.rfind(L"LPT", 0) == 0) && iswdigit(stem[3]))) return false;
        if (end == std::wstring::npos) return true;
        start = end + 1;
    }
    return false;
}

std::wstring FormatExtension(Format format) {
    switch (format) {
    case Format::Zip: return L".zip";
    case Format::Tar: return L".tar";
    case Format::TarGz: return L".tar.gz";
    case Format::TarXz: return L".tar.xz";
    case Format::TarBz2: return L".tar.bz2";
    default: return L".7z";
    }
}

std::vector<std::string> ArchiveExtensions() {
    return {".7z", ".zip", ".rar", ".tar", ".gz", ".tgz", ".xz", ".txz", ".bz2", ".tbz2", ".tbz", ".lzma", ".z",
            ".iso", ".cab", ".wim", ".arj", ".lzh", ".lha", ".cpio", ".rpm", ".deb", ".001", ".zipx", ".jar",
            ".apk", ".msi", ".vhd", ".vhdx", ".dmg", ".udf", ".squashfs"};
}

std::vector<std::wstring> SupportedFormats() {
    {
        std::lock_guard guard(g_formats_lock);
        if (!g_cached_formats.empty()) return g_cached_formats;
    }

    const auto limits = CurrentLimits();
    std::string spawn_error;
    auto session = AcquirePreviewSession(limits, spawn_error);
    if (!session) return {};

    const uint64_t req_id = ++g_next_request_id;
    const uint64_t gen = ++g_next_task_generation;
    std::vector<uint8_t> resp;
    Status fail_status = Status::WorkerCrashed;
    std::string fail_error;
    if (!session->Execute(FrameType::ReqFormats, FrameType::RespFormats, req_id, gen, {},
                          {}, {}, {}, 5000u, limits.cancel_grace_ms, resp, fail_status, fail_error)) {
        session->KillAndReap();
        return {};
    }

    ByteReader reader(resp);
    Status status = Status::WorkerCrashed;
    std::vector<std::wstring> formats;
    if (!reader.Status(status) || !reader.WStrVec(formats) || !reader.Empty() || status != Status::Ok) {
        session->KillAndReap();
        return {};
    }
    ReleasePreviewSession(std::move(session));

    if (!formats.empty()) {
        std::lock_guard guard(g_formats_lock);
        g_cached_formats = formats;
    }
    return formats;
}

Listing List(const std::wstring& path, const Cancelled& cancelled, const AskPassword& password,
             size_t max_entries, const Budget& budget) {
    Listing result;
    if (SafeCallCancelled(cancelled)) {
        result.status = Status::Cancelled;
        result.error = "Cancelled";
        return result;
    }

    const auto limits = CurrentLimits();
    const bool is_preview = !password && budget.max_duration_ms > 0 && budget.max_duration_ms <= 15000u;

    ByteWriter req;
    if (!req.WStr(path) ||
        !req.U64(static_cast<uint64_t>(max_entries)) ||
        !req.Budget(budget) ||
        !req.Bool(static_cast<bool>(password))) {
        result.status = Status::InvalidInput;
        result.error = "Invalid list arguments";
        return result;
    }

    std::string spawn_error;
    std::unique_ptr<WorkerSession> session;
    if (is_preview) {
        session = AcquirePreviewSession(limits, spawn_error);
    } else {
        session = std::make_unique<WorkerSession>();
        if (!session->Spawn(limits.preview_memory_bytes, limits.allow_fault_injection, spawn_error)) {
            session.reset();
        }
    }
    if (!session) {
        result.status = Status::WorkerCrashed;
        result.error = spawn_error;
        return result;
    }

    const uint64_t req_id = ++g_next_request_id;
    const uint64_t gen = is_preview ? ++g_preview_generation : ++g_next_task_generation;
    const Cancelled effective_cancel = [&cancelled, is_preview, gen]() -> bool {
        if (SafeCallCancelled(cancelled)) return true;
        if (is_preview && g_preview_generation.load(std::memory_order_acquire) != gen) return true;
        return false;
    };

    const uint32_t hard_timeout_ms = EffectiveHardTimeoutMs(
        budget.max_duration_ms, limits.preview_hard_timeout_slack_ms, limits.task_hard_timeout_ms);

    std::vector<uint8_t> resp;
    if (!session->Execute(FrameType::ReqList, FrameType::RespList, req_id, gen, req.Bytes(),
                          effective_cancel, {}, password, hard_timeout_ms, limits.cancel_grace_ms,
                          resp, result.status, result.error)) {
        session->KillAndReap();
        return result;
    }

    ByteReader reader(resp);
    uint32_t entry_count = 0;
    if (!reader.Status(result.status) ||
        !reader.Str(result.error) ||
        !reader.WStr(result.format) ||
        !reader.Bool(result.truncated) ||
        !reader.Bool(result.nested) ||
        !reader.U32(entry_count) ||
        entry_count > budget.max_entries ||
        static_cast<size_t>(entry_count) * 28u > reader.Remaining()) {
        session->KillAndReap();
        result = Listing{};
        result.status = Status::WorkerCrashed;
        result.error = "malformed list response from worker";
        return result;
    }

    result.entries.reserve(std::min<size_t>(entry_count, 4096u));
    for (uint32_t i = 0; i < entry_count; ++i) {
        Entry entry;
        if (!reader.WStr(entry.path) ||
            !reader.U64(entry.size) ||
            !reader.U64(entry.packed) ||
            !reader.U64(entry.modified) ||
            !reader.U32(entry.attributes) ||
            !reader.Bool(entry.dir) ||
            !reader.Bool(entry.encrypted) ||
            !reader.Bool(entry.link)) {
            session->KillAndReap();
            result = Listing{};
            result.status = Status::WorkerCrashed;
            result.error = "corrupted entry in worker list response";
            return result;
        }
        result.entries.push_back(std::move(entry));
    }
    if (!reader.Empty()) {
        session->KillAndReap();
        result = Listing{};
        result.status = Status::WorkerCrashed;
        result.error = "trailing bytes in worker list response";
        return result;
    }

    if (is_preview) {
        if (g_preview_generation.load(std::memory_order_acquire) != gen) {
            ReleasePreviewSession(std::move(session));
            result = Listing{};
            result.status = Status::Cancelled;
            result.error = "Superseded by newer preview generation";
            return result;
        }
        ReleasePreviewSession(std::move(session));
    } else {
        session->ShutdownCleanly();
    }
    return result;
}

ExtractResult Extract(const std::wstring& path, const std::wstring& destination, bool test_only,
                      const Cancelled& cancelled, const Report& report, const AskPassword& password,
                      const Budget& budget) {
    ExtractResult result;
    if (SafeCallCancelled(cancelled)) {
        result.status = Status::Cancelled;
        result.error = "Cancelled";
        return result;
    }

    ByteWriter req;
    if (!req.WStr(path) ||
        !req.WStr(destination) ||
        !req.Bool(test_only) ||
        !req.Budget(budget) ||
        !req.Bool(static_cast<bool>(report)) ||
        !req.Bool(static_cast<bool>(password))) {
        result.status = Status::InvalidInput;
        result.error = "Invalid extract arguments";
        return result;
    }

    const auto limits = CurrentLimits();
    std::string spawn_error;
    WorkerSession session;
    if (!session.Spawn(limits.task_memory_bytes, limits.allow_fault_injection, spawn_error)) {
        result.status = Status::WorkerCrashed;
        result.error = spawn_error;
        return result;
    }

    const uint64_t req_id = ++g_next_request_id;
    const uint64_t gen = ++g_next_task_generation;
    const uint32_t hard_timeout_ms = EffectiveHardTimeoutMs(
        budget.max_duration_ms, limits.preview_hard_timeout_slack_ms, limits.task_hard_timeout_ms);

    std::vector<uint8_t> resp;
    if (!session.Execute(FrameType::ReqExtract, FrameType::RespExtract, req_id, gen, req.Bytes(),
                         cancelled, report, password, hard_timeout_ms, limits.cancel_grace_ms,
                         resp, result.status, result.error)) {
        session.KillAndReap();
        return result;
    }

    ByteReader reader(resp);
    if (!reader.Status(result.status) ||
        !reader.Str(result.error) ||
        !reader.U64(result.files) ||
        !reader.U64(result.bytes) ||
        !reader.U64(result.skipped_links) ||
        !reader.Empty()) {
        session.KillAndReap();
        result = ExtractResult{};
        result.status = Status::WorkerCrashed;
        result.error = "malformed extract response from worker";
        return result;
    }
    session.ShutdownCleanly();
    return result;
}

CreateResult Create(const std::vector<std::wstring>& sources, const std::wstring& destination,
                    const std::wstring& name, const CreateOptions& options,
                    const Cancelled& cancelled, const Report& report) {
    CreateResult result;
    if (SafeCallCancelled(cancelled)) {
        result.status = Status::Cancelled;
        result.error = "Cancelled";
        return result;
    }

    ByteWriter req(/*sensitive=*/!options.password.empty());
    if (!req.WStrVec(sources) ||
        !req.WStr(destination) ||
        !req.WStr(name) ||
        !req.U32(static_cast<uint32_t>(options.format)) ||
        !req.U32(options.level) ||
        !req.Bool(options.solid) ||
        !req.WStr(options.password) ||
        !req.Bool(options.encrypt_names) ||
        !req.Bool(options.zip_aes) ||
        !req.U64(options.volume_size) ||
        !req.Bool(static_cast<bool>(report))) {
        result.status = Status::InvalidInput;
        result.error = "Invalid create arguments";
        return result;
    }

    const auto limits = CurrentLimits();
    std::string spawn_error;
    WorkerSession session;
    if (!session.Spawn(limits.task_memory_bytes, limits.allow_fault_injection, spawn_error)) {
        result.status = Status::WorkerCrashed;
        result.error = spawn_error;
        return result;
    }

    const uint64_t req_id = ++g_next_request_id;
    const uint64_t gen = ++g_next_task_generation;
    std::vector<uint8_t> resp;
    if (!session.Execute(FrameType::ReqCreate, FrameType::RespCreate, req_id, gen, req.Bytes(),
                         cancelled, report, {}, limits.task_hard_timeout_ms, limits.cancel_grace_ms,
                         resp, result.status, result.error)) {
        session.KillAndReap();
        return result;
    }

    ByteReader reader(resp);
    if (!reader.Status(result.status) ||
        !reader.Str(result.error) ||
        !reader.WStrVec(result.outputs) ||
        !reader.U64(result.files) ||
        !reader.U64(result.bytes) ||
        !reader.U64(result.skipped) ||
        !reader.Empty()) {
        session.KillAndReap();
        result = CreateResult{};
        result.status = Status::WorkerCrashed;
        result.error = "malformed create response from worker";
        return result;
    }
    session.ShutdownCleanly();
    return result;
}

std::vector<HashResult> Hash(const std::vector<std::wstring>& files,
                             const Cancelled& cancelled,
                             const Report& report) {
    if (files.empty() || SafeCallCancelled(cancelled)) return {};

    ByteWriter req;
    if (!req.WStrVec(files) || !req.Bool(static_cast<bool>(report))) return {};

    const auto limits = CurrentLimits();
    std::string spawn_error;
    WorkerSession session;
    if (!session.Spawn(limits.task_memory_bytes, limits.allow_fault_injection, spawn_error)) {
        return {};
    }

    const uint64_t req_id = ++g_next_request_id;
    const uint64_t gen = ++g_next_task_generation;
    std::vector<uint8_t> resp;
    Status fail_status = Status::WorkerCrashed;
    std::string fail_error;
    if (!session.Execute(FrameType::ReqHash, FrameType::RespHash, req_id, gen, req.Bytes(),
                         cancelled, report, {}, limits.task_hard_timeout_ms, limits.cancel_grace_ms,
                         resp, fail_status, fail_error)) {
        session.KillAndReap();
        return {};
    }

    ByteReader reader(resp);
    Status status = Status::WorkerCrashed;
    uint32_t count = 0;
    if (!reader.Status(status) || !reader.U32(count) || count > files.size() ||
        static_cast<size_t>(count) * 24u > reader.Remaining()) {
        session.KillAndReap();
        return {};
    }
    std::vector<HashResult> results;
    results.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        HashResult item;
        if (!reader.WStr(item.path) ||
            !reader.U64(item.size) ||
            !reader.Str(item.crc32) ||
            !reader.Str(item.sha1) ||
            !reader.Str(item.sha256) ||
            !reader.Str(item.error)) {
            session.KillAndReap();
            return {};
        }
        results.push_back(std::move(item));
    }
    if (!reader.Empty()) {
        session.KillAndReap();
        return {};
    }
    session.ShutdownCleanly();
    return results;
}

} // namespace engine

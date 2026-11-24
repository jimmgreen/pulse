#ifndef PULSE_NATIVE_WORKER_HPP
#define PULSE_NATIVE_WORKER_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "tasks.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY
#define PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY 0x0002000Eu
#endif
#ifndef PROCESS_CREATION_CHILD_PROCESS_RESTRICTED
#define PROCESS_CREATION_CHILD_PROCESS_RESTRICTED 0x01u
#endif

namespace pulse::worker {

class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept
        : handle_(handle == INVALID_HANDLE_VALUE ? nullptr : handle) {}
    ~UniqueHandle() { Reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    HANDLE Get() const noexcept { return handle_; }
    bool Valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    explicit operator bool() const noexcept { return Valid(); }

    HANDLE Release() noexcept { return std::exchange(handle_, nullptr); }

    void Reset(HANDLE next = nullptr) noexcept {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = (next == INVALID_HANDLE_VALUE) ? nullptr : next;
    }

private:
    HANDLE handle_ = nullptr;
};

enum class FrameKind : uint8_t {
    Request = 1,
    Reply = 2,
    Progress = 3,
    Error = 4,
    Shutdown = 5,
};

struct WireFrame {
    uint8_t kind = 0;
    uint32_t tag = 0;
    std::string payload;
};

enum class WorkerStatus : uint32_t {
    Ok = 0,
    InvalidArgument = 1,
    SpawnFailed = 2,
    JobFailed = 3,
    Cancelled = 4,
    TimedOut = 5,
    ProtocolError = 6,
    FrameTooLarge = 7,
    WorkerExited = 8,
    IoError = 9,
};

struct WorkerError {
    WorkerStatus status = WorkerStatus::Ok;
    DWORD win32_error = 0;
    DWORD exit_code = 0;
    std::string message;
};

struct WorkerLimits {
    size_t memory_limit_bytes = 256u * 1024u * 1024u;
    uint32_t active_process_limit = 1u;
    uint32_t max_frame_bytes = 4u * 1024u * 1024u;
    uint32_t timeout_ms = 60000u;
    uint32_t cancel_poll_ms = 10u;
    bool kill_on_close = true;
    bool die_on_unhandled_exception = true;
    bool restrict_child_processes = true;
    bool detached_process = true;
};

inline std::wstring ResolveSiblingExecutable(std::wstring_view filename,
                                             const void* address_in_module = nullptr) {
    if (filename.empty()) return {};
    const void* probe = address_in_module
        ? address_in_module
        : reinterpret_cast<const void*>(&ResolveSiblingExecutable);
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(probe),
            &mod)) {
        mod = nullptr;
    }
    std::wstring path(32768, L'\0');
    const DWORD len = GetModuleFileNameW(mod, path.data(), static_cast<DWORD>(path.size()));
    if (len == 0 || len >= path.size()) return {};
    path.resize(len);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring(filename);
    path.resize(slash + 1);
    path.append(filename);
    return path;
}

namespace detail {

constexpr uint32_t kWireHeaderBytes = 5u; // uint8_t kind + uint32_t tag
constexpr uint32_t kMinMaxFrameBytes = 64u;
constexpr uint32_t kMaxMaxFrameBytes = 64u * 1024u * 1024u;

inline uint32_t ClampFrameLimit(uint32_t value) noexcept {
    if (value < kMinMaxFrameBytes) return kMinMaxFrameBytes;
    if (value > kMaxMaxFrameBytes) return kMaxMaxFrameBytes;
    return value;
}

inline uint32_t ClampPollMs(uint32_t value) noexcept {
    if (value < 1u) return 1u;
    if (value > 250u) return 250u;
    return value;
}

inline void SetError(WorkerError* out_err,
                     WorkerStatus status,
                     DWORD win32_error = 0,
                     DWORD exit_code = 0,
                     std::string_view message = {}) {
    if (!out_err) return;
    out_err->status = status;
    out_err->win32_error = win32_error;
    out_err->exit_code = exit_code;
    out_err->message.assign(message.data(), message.size());
}

inline void WriteU32Le(uint8_t* dst, uint32_t value) noexcept {
    dst[0] = static_cast<uint8_t>(value & 0xFFu);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

inline uint32_t ReadU32Le(const uint8_t* src) noexcept {
    return static_cast<uint32_t>(src[0]) |
           (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) |
           (static_cast<uint32_t>(src[3]) << 24);
}

inline bool WriteExact(HANDLE handle, const void* data, size_t bytes, DWORD* out_win32) noexcept {
    const auto* cursor = static_cast<const uint8_t*>(data);
    size_t remaining = bytes;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>((std::min)(remaining, size_t{65536}));
        DWORD written = 0;
        if (!WriteFile(handle, cursor, chunk, &written, nullptr) || written == 0) {
            if (out_win32) *out_win32 = GetLastError();
            return false;
        }
        cursor += written;
        remaining -= written;
    }
    return true;
}

inline bool ReadExact(HANDLE handle, void* data, size_t bytes, DWORD* out_win32) noexcept {
    auto* cursor = static_cast<uint8_t*>(data);
    size_t remaining = bytes;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>((std::min)(remaining, size_t{65536}));
        DWORD got = 0;
        if (!ReadFile(handle, cursor, chunk, &got, nullptr) || got == 0) {
            if (out_win32) *out_win32 = GetLastError();
            return false;
        }
        cursor += got;
        remaining -= got;
    }
    return true;
}

inline UniqueHandle CreateGovernedJob(const WorkerLimits& limits, DWORD* out_win32) noexcept {
    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        if (out_win32) *out_win32 = GetLastError();
        return {};
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    DWORD flags = 0;
    if (limits.kill_on_close) flags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (limits.die_on_unhandled_exception) flags |= JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (limits.active_process_limit > 0) {
        flags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        info.BasicLimitInformation.ActiveProcessLimit = limits.active_process_limit;
    }
    if (limits.memory_limit_bytes > 0) {
        flags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        info.ProcessMemoryLimit = limits.memory_limit_bytes;
    }
    info.BasicLimitInformation.LimitFlags = flags;
    if (flags != 0 &&
        !SetInformationJobObject(job.Get(),
                                 JobObjectExtendedLimitInformation,
                                 &info,
                                 static_cast<DWORD>(sizeof(info)))) {
        if (out_win32) *out_win32 = GetLastError();
        return {};
    }
    return job;
}

class ProcThreadAttrBuffer {
public:
    ~ProcThreadAttrBuffer() { Reset(); }
    ProcThreadAttrBuffer() = default;
    ProcThreadAttrBuffer(const ProcThreadAttrBuffer&) = delete;
    ProcThreadAttrBuffer& operator=(const ProcThreadAttrBuffer&) = delete;

    bool Initialize(DWORD count, DWORD* out_win32) {
        Reset();
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, count, 0, &bytes);
        if (bytes == 0) {
            if (out_win32) *out_win32 = GetLastError();
            return false;
        }
        storage_.assign(bytes, 0);
        list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!InitializeProcThreadAttributeList(list_, count, 0, &bytes)) {
            if (out_win32) *out_win32 = GetLastError();
            list_ = nullptr;
            storage_.clear();
            return false;
        }
        return true;
    }

    bool Update(DWORD_PTR attribute, void* value, SIZE_T size, DWORD* out_win32) noexcept {
        if (!list_) return false;
        if (!UpdateProcThreadAttribute(list_, 0, attribute, value, size, nullptr, nullptr)) {
            if (out_win32) *out_win32 = GetLastError();
            return false;
        }
        return true;
    }

    LPPROC_THREAD_ATTRIBUTE_LIST Get() const noexcept { return list_; }

    void Reset() noexcept {
        if (list_) {
            DeleteProcThreadAttributeList(list_);
            list_ = nullptr;
        }
        storage_.clear();
    }

private:
    std::vector<uint8_t> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

} // namespace detail

inline WorkerStatus WriteWireFrame(HANDLE pipe,
                                   uint8_t kind,
                                   uint32_t tag,
                                   std::string_view payload,
                                   uint32_t max_frame_bytes = 4u * 1024u * 1024u,
                                   WorkerError* out_err = nullptr) {
    if (!pipe || pipe == INVALID_HANDLE_VALUE) {
        detail::SetError(out_err, WorkerStatus::InvalidArgument, 0, 0, "invalid pipe handle");
        return WorkerStatus::InvalidArgument;
    }
    const uint32_t cap = detail::ClampFrameLimit(max_frame_bytes);
    if (payload.size() > static_cast<size_t>(cap - detail::kWireHeaderBytes)) {
        detail::SetError(out_err, WorkerStatus::FrameTooLarge, 0, 0, "wire frame exceeds max_frame_bytes");
        return WorkerStatus::FrameTooLarge;
    }
    const uint32_t total_len = detail::kWireHeaderBytes + static_cast<uint32_t>(payload.size());
    std::array<uint8_t, 9> hdr{};
    detail::WriteU32Le(hdr.data(), total_len);
    hdr[4] = kind;
    detail::WriteU32Le(hdr.data() + 5, tag);

    DWORD win32_err = 0;
    if (!detail::WriteExact(pipe, hdr.data(), hdr.size(), &win32_err)) {
        detail::SetError(out_err, WorkerStatus::IoError, win32_err, 0, "failed writing wire frame header");
        return WorkerStatus::IoError;
    }
    if (!payload.empty() && !detail::WriteExact(pipe, payload.data(), payload.size(), &win32_err)) {
        detail::SetError(out_err, WorkerStatus::IoError, win32_err, 0, "failed writing wire frame payload");
        return WorkerStatus::IoError;
    }
    return WorkerStatus::Ok;
}

inline WorkerStatus ReadWireFrame(HANDLE pipe,
                                  WireFrame& out_frame,
                                  uint32_t max_frame_bytes = 4u * 1024u * 1024u,
                                  WorkerError* out_err = nullptr) {
    out_frame = {};
    if (!pipe || pipe == INVALID_HANDLE_VALUE) {
        detail::SetError(out_err, WorkerStatus::InvalidArgument, 0, 0, "invalid pipe handle");
        return WorkerStatus::InvalidArgument;
    }
    const uint32_t cap = detail::ClampFrameLimit(max_frame_bytes);
    std::array<uint8_t, 4> len_buf{};
    DWORD win32_err = 0;
    if (!detail::ReadExact(pipe, len_buf.data(), len_buf.size(), &win32_err)) {
        detail::SetError(out_err, WorkerStatus::IoError, win32_err, 0, "pipe closed or read failed");
        return WorkerStatus::IoError;
    }
    const uint32_t total_len = detail::ReadU32Le(len_buf.data());
    if (total_len < detail::kWireHeaderBytes) {
        detail::SetError(out_err, WorkerStatus::ProtocolError, 0, 0, "wire frame shorter than header");
        return WorkerStatus::ProtocolError;
    }
    if (total_len > cap) {
        detail::SetError(out_err, WorkerStatus::FrameTooLarge, 0, 0, "wire frame exceeds max_frame_bytes");
        return WorkerStatus::FrameTooLarge;
    }
    std::array<uint8_t, 5> meta{};
    if (!detail::ReadExact(pipe, meta.data(), meta.size(), &win32_err)) {
        detail::SetError(out_err, WorkerStatus::IoError, win32_err, 0, "failed reading wire frame metadata");
        return WorkerStatus::IoError;
    }
    out_frame.kind = meta[0];
    out_frame.tag = detail::ReadU32Le(meta.data() + 1);
    const size_t payload_len = static_cast<size_t>(total_len - detail::kWireHeaderBytes);
    out_frame.payload.resize(payload_len);
    if (payload_len > 0 && !detail::ReadExact(pipe, out_frame.payload.data(), payload_len, &win32_err)) {
        out_frame = {};
        detail::SetError(out_err, WorkerStatus::IoError, win32_err, 0, "failed reading wire frame payload");
        return WorkerStatus::IoError;
    }
    return WorkerStatus::Ok;
}

class WorkerProcess {
public:
    WorkerProcess() = default;
    ~WorkerProcess() { Terminate(); }

    WorkerProcess(const WorkerProcess&) = delete;
    WorkerProcess& operator=(const WorkerProcess&) = delete;

    WorkerProcess(WorkerProcess&& other) noexcept = default;
    WorkerProcess& operator=(WorkerProcess&& other) noexcept {
        if (this != &other) {
            Terminate();
            job_ = std::move(other.job_);
            process_ = std::move(other.process_);
            stdin_write_ = std::move(other.stdin_write_);
            stdout_read_ = std::move(other.stdout_read_);
            limits_ = other.limits_;
            read_buf_ = std::move(other.read_buf_);
        }
        return *this;
    }

    WorkerStatus Start(const std::wstring& exe_path,
                       const std::wstring& extra_args = {},
                       const WorkerLimits& limits = {},
                       WorkerError* out_err = nullptr) {
        Terminate();
        if (exe_path.empty()) {
            detail::SetError(out_err, WorkerStatus::InvalidArgument, 0, 0, "exe_path must be non-empty");
            return WorkerStatus::InvalidArgument;
        }
        if (limits.max_frame_bytes < detail::kMinMaxFrameBytes) {
            detail::SetError(out_err, WorkerStatus::InvalidArgument, 0, 0, "max_frame_bytes must be >= 64");
            return WorkerStatus::InvalidArgument;
        }

        WorkerLimits effective = limits;
        effective.max_frame_bytes = detail::ClampFrameLimit(limits.max_frame_bytes);
        effective.cancel_poll_ms = detail::ClampPollMs(limits.cancel_poll_ms);

        DWORD win32_err = 0;
        UniqueHandle job = detail::CreateGovernedJob(effective, &win32_err);
        if (!job) {
            detail::SetError(out_err, WorkerStatus::JobFailed, win32_err, 0, "CreateJobObjectW / SetInformationJobObject failed");
            return WorkerStatus::JobFailed;
        }

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE raw_in_r = nullptr, raw_in_w = nullptr;
        HANDLE raw_out_r = nullptr, raw_out_w = nullptr;
        if (!CreatePipe(&raw_in_r, &raw_in_w, &sa, 65536)) {
            detail::SetError(out_err, WorkerStatus::SpawnFailed, GetLastError(), 0, "CreatePipe stdin failed");
            return WorkerStatus::SpawnFailed;
        }
        UniqueHandle child_stdin(raw_in_r);
        UniqueHandle parent_stdin_w(raw_in_w);

        if (!CreatePipe(&raw_out_r, &raw_out_w, &sa, 65536)) {
            detail::SetError(out_err, WorkerStatus::SpawnFailed, GetLastError(), 0, "CreatePipe stdout failed");
            return WorkerStatus::SpawnFailed;
        }
        UniqueHandle parent_stdout_r(raw_out_r);
        UniqueHandle child_stdout(raw_out_w);

        if (!SetHandleInformation(parent_stdin_w.Get(), HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(parent_stdout_r.Get(), HANDLE_FLAG_INHERIT, 0)) {
            detail::SetError(out_err, WorkerStatus::SpawnFailed, GetLastError(), 0, "SetHandleInformation failed");
            return WorkerStatus::SpawnFailed;
        }

        std::array<HANDLE, 2> inherit_handles{child_stdin.Get(), child_stdout.Get()};
        DWORD child_policy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;

        detail::ProcThreadAttrBuffer attrs;
        const DWORD desired_attrs = effective.restrict_child_processes ? 2u : 1u;
        if (!attrs.Initialize(desired_attrs, &win32_err) ||
            !attrs.Update(PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                          inherit_handles.data(),
                          inherit_handles.size() * sizeof(HANDLE),
                          &win32_err)) {
            detail::SetError(out_err, WorkerStatus::SpawnFailed, win32_err, 0, "ProcThreadAttribute HANDLE_LIST failed");
            return WorkerStatus::SpawnFailed;
        }
        if (effective.restrict_child_processes) {
            DWORD policy_err = 0;
            if (!attrs.Update(PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY,
                              &child_policy,
                              sizeof(child_policy),
                              &policy_err)) {
                if (policy_err == ERROR_NOT_SUPPORTED || policy_err == ERROR_INVALID_PARAMETER) {
                    if (!attrs.Initialize(1u, &win32_err) ||
                        !attrs.Update(PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      inherit_handles.data(),
                                      inherit_handles.size() * sizeof(HANDLE),
                                      &win32_err)) {
                        detail::SetError(out_err, WorkerStatus::SpawnFailed, win32_err, 0, "ProcThreadAttribute fallback failed");
                        return WorkerStatus::SpawnFailed;
                    }
                } else {
                    detail::SetError(out_err, WorkerStatus::SpawnFailed, policy_err, 0, "CHILD_PROCESS_POLICY failed");
                    return WorkerStatus::SpawnFailed;
                }
            }
        }

        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = child_stdin.Get();
        si.StartupInfo.hStdOutput = child_stdout.Get();
        si.StartupInfo.hStdError = INVALID_HANDLE_VALUE;
        si.lpAttributeList = attrs.Get();

        std::wstring cmd = L"\"" + exe_path + L"\"";
        if (!extra_args.empty()) {
            cmd.push_back(L' ');
            cmd.append(extra_args);
        }

        PROCESS_INFORMATION pi{};
        DWORD creation_flags = CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT;
        if (effective.detached_process) creation_flags |= DETACHED_PROCESS;

        if (!CreateProcessW(exe_path.c_str(),
                            cmd.data(),
                            nullptr,
                            nullptr,
                            TRUE,
                            creation_flags,
                            nullptr,
                            nullptr,
                            &si.StartupInfo,
                            &pi)) {
            detail::SetError(out_err, WorkerStatus::SpawnFailed, GetLastError(), 0, "CreateProcessW failed");
            return WorkerStatus::SpawnFailed;
        }
        UniqueHandle child_thread(pi.hThread);
        UniqueHandle child_proc(pi.hProcess);

        if (!AssignProcessToJobObject(job.Get(), child_proc.Get())) {
            win32_err = GetLastError();
            TerminateProcess(child_proc.Get(), 1);
            detail::SetError(out_err, WorkerStatus::JobFailed, win32_err, 0, "AssignProcessToJobObject failed");
            return WorkerStatus::JobFailed;
        }

        child_stdin.Reset();
        child_stdout.Reset();

        if (ResumeThread(child_thread.Get()) == static_cast<DWORD>(-1)) {
            win32_err = GetLastError();
            TerminateJobObject(job.Get(), 1);
            detail::SetError(out_err, WorkerStatus::SpawnFailed, win32_err, 0, "ResumeThread failed");
            return WorkerStatus::SpawnFailed;
        }
        child_thread.Reset();

        job_ = std::move(job);
        process_ = std::move(child_proc);
        stdin_write_ = std::move(parent_stdin_w);
        stdout_read_ = std::move(parent_stdout_r);
        limits_ = effective;
        read_buf_.clear();
        return WorkerStatus::Ok;
    }

    bool IsRunning() const noexcept {
        return process_.Valid() && WaitForSingleObject(process_.Get(), 0) == WAIT_TIMEOUT;
    }

    WorkerStatus Send(uint8_t kind,
                      uint32_t tag,
                      std::string_view payload = {},
                      WorkerError* out_err = nullptr) {
        if (!stdin_write_.Valid()) {
            detail::SetError(out_err, WorkerStatus::WorkerExited, 0, QueryExitCode(), "worker stdin unavailable");
            return WorkerStatus::WorkerExited;
        }
        return WriteWireFrame(stdin_write_.Get(), kind, tag, payload, limits_.max_frame_bytes, out_err);
    }

    void CloseInput() noexcept {
        stdin_write_.Reset();
    }

    WorkerStatus Receive(WireFrame& out_frame,
                         const std::function<bool()>& is_cancelled = {},
                         uint32_t timeout_ms_override = 0,
                         WorkerError* out_err = nullptr) {
        out_frame = {};
        if (!stdout_read_.Valid() || !process_.Valid()) {
            detail::SetError(out_err, WorkerStatus::InvalidArgument, 0, 0, "worker is not started");
            return WorkerStatus::InvalidArgument;
        }

        const uint32_t effective_timeout = timeout_ms_override ? timeout_ms_override : limits_.timeout_ms;
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = effective_timeout > 0
            ? start + std::chrono::milliseconds(effective_timeout)
            : std::chrono::steady_clock::time_point::max();

        for (;;) {
            WorkerStatus buffered = TryConsumeBufferedFrame(out_frame, out_err);
            if (buffered == WorkerStatus::Ok || buffered == WorkerStatus::ProtocolError ||
                buffered == WorkerStatus::FrameTooLarge) {
                return buffered;
            }

            if (is_cancelled && is_cancelled()) {
                Terminate();
                detail::SetError(out_err, WorkerStatus::Cancelled, 0, 0, "worker cancelled");
                return WorkerStatus::Cancelled;
            }

            DWORD avail = 0;
            if (PeekNamedPipe(stdout_read_.Get(), nullptr, 0, nullptr, &avail, nullptr)) {
                if (avail > 0) {
                    if (!ReadAvailableChunk(avail, out_err)) {
                        return out_err ? out_err->status : WorkerStatus::IoError;
                    }
                    continue;
                }
            } else {
                const DWORD pipe_err = GetLastError();
                WaitForSingleObject(process_.Get(), 50);
                const DWORD exit_code = QueryExitCode();
                detail::SetError(out_err, WorkerStatus::WorkerExited, pipe_err, exit_code, "worker pipe closed");
                return WorkerStatus::WorkerExited;
            }

            if (WaitForSingleObject(process_.Get(), 0) == WAIT_OBJECT_0) {
                DWORD final_avail = 0;
                while (PeekNamedPipe(stdout_read_.Get(), nullptr, 0, nullptr, &final_avail, nullptr) &&
                       final_avail > 0) {
                    if (!ReadAvailableChunk(final_avail, out_err)) {
                        return out_err ? out_err->status : WorkerStatus::IoError;
                    }
                    buffered = TryConsumeBufferedFrame(out_frame, out_err);
                    if (buffered == WorkerStatus::Ok || buffered == WorkerStatus::ProtocolError ||
                        buffered == WorkerStatus::FrameTooLarge) {
                        return buffered;
                    }
                }
                const DWORD exit_code = QueryExitCode();
                detail::SetError(out_err, WorkerStatus::WorkerExited, 0, exit_code, "worker process exited");
                return WorkerStatus::WorkerExited;
            }

            if (effective_timeout > 0 && std::chrono::steady_clock::now() >= deadline) {
                Terminate();
                detail::SetError(out_err, WorkerStatus::TimedOut, WAIT_TIMEOUT, 0, "worker timed out");
                return WorkerStatus::TimedOut;
            }

            DWORD sleep_ms = limits_.cancel_poll_ms;
            if (effective_timeout > 0) {
                const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (rem <= 0) {
                    Terminate();
                    detail::SetError(out_err, WorkerStatus::TimedOut, WAIT_TIMEOUT, 0, "worker timed out");
                    return WorkerStatus::TimedOut;
                }
                if (rem < static_cast<long long>(sleep_ms)) {
                    sleep_ms = static_cast<DWORD>(rem);
                }
            }
            Sleep(sleep_ms);
        }
    }

    WorkerStatus Call(uint32_t tag,
                      std::string_view request,
                      std::string& out_reply,
                      const std::function<void(uint32_t, std::string_view)>& on_progress = {},
                      const std::function<bool()>& is_cancelled = {},
                      uint32_t timeout_ms_override = 0,
                      WorkerError* out_err = nullptr) {
        out_reply.clear();
        const WorkerStatus send_st = Send(static_cast<uint8_t>(FrameKind::Request), tag, request, out_err);
        if (send_st != WorkerStatus::Ok) {
            return send_st;
        }

        const uint32_t effective_timeout = timeout_ms_override ? timeout_ms_override : limits_.timeout_ms;
        const auto deadline = effective_timeout > 0
            ? std::chrono::steady_clock::now() + std::chrono::milliseconds(effective_timeout)
            : std::chrono::steady_clock::time_point::max();

        for (;;) {
            uint32_t rem_ms = 0;
            if (effective_timeout > 0) {
                const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (rem <= 0) {
                    Terminate();
                    detail::SetError(out_err, WorkerStatus::TimedOut, WAIT_TIMEOUT, 0, "worker call timed out");
                    return WorkerStatus::TimedOut;
                }
                rem_ms = static_cast<uint32_t>((std::max)(long long{1}, rem));
            }
            WireFrame frame;
            const WorkerStatus recv_st = Receive(frame, is_cancelled, rem_ms, out_err);
            if (recv_st != WorkerStatus::Ok) {
                return recv_st;
            }
            if (frame.kind == static_cast<uint8_t>(FrameKind::Progress)) {
                if (on_progress) on_progress(frame.tag, frame.payload);
                continue;
            }
            if (frame.kind == static_cast<uint8_t>(FrameKind::Reply)) {
                if (frame.tag != tag) {
                    detail::SetError(out_err, WorkerStatus::ProtocolError, 0, 0, "reply tag mismatch");
                    return WorkerStatus::ProtocolError;
                }
                out_reply = std::move(frame.payload);
                return WorkerStatus::Ok;
            }
            if (frame.kind == static_cast<uint8_t>(FrameKind::Error)) {
                detail::SetError(out_err, WorkerStatus::IoError, 0, 0, frame.payload);
                return WorkerStatus::IoError;
            }
            detail::SetError(out_err, WorkerStatus::ProtocolError, 0, 0, "unexpected frame kind from worker");
            return WorkerStatus::ProtocolError;
        }
    }

    WorkerStatus Call(uint32_t tag,
                      std::string_view request,
                      std::string& out_reply,
                      const PulseTaskContext* task_ctx,
                      const std::function<void(uint32_t, std::string_view)>& on_progress = {},
                      uint32_t timeout_ms_override = 0,
                      WorkerError* out_err = nullptr) {
        return Call(
            tag,
            request,
            out_reply,
            on_progress,
            [task_ctx]() noexcept -> bool {
                return task_ctx && task_ctx->cancelled && task_ctx->cancelled(task_ctx->context) != 0;
            },
            timeout_ms_override,
            out_err);
    }

    WorkerStatus Shutdown(uint32_t grace_ms = 250) noexcept {
        if (stdin_write_.Valid()) {
            (void)WriteWireFrame(stdin_write_.Get(),
                                 static_cast<uint8_t>(FrameKind::Shutdown),
                                 0,
                                 {},
                                 limits_.max_frame_bytes,
                                 nullptr);
            stdin_write_.Reset();
        }
        if (process_.Valid() && grace_ms > 0) {
            WaitForSingleObject(process_.Get(), grace_ms);
        }
        const bool clean_exit = process_.Valid() && WaitForSingleObject(process_.Get(), 0) == WAIT_OBJECT_0;
        Terminate();
        return clean_exit ? WorkerStatus::Ok : WorkerStatus::WorkerExited;
    }

    void Terminate() noexcept {
        stdin_write_.Reset();
        stdout_read_.Reset();
        if (job_.Valid()) {
            TerminateJobObject(job_.Get(), 1);
            job_.Reset();
        }
        process_.Reset();
        read_buf_.clear();
    }

    static WorkerStatus RunOneShot(const std::wstring& exe_path,
                                   std::string_view request,
                                   std::string& out_reply,
                                   const WorkerLimits& limits = {},
                                   const std::function<void(uint32_t, std::string_view)>& on_progress = {},
                                   const std::function<bool()>& is_cancelled = {},
                                   WorkerError* out_err = nullptr) {
        WorkerProcess proc;
        const WorkerStatus st = proc.Start(exe_path, {}, limits, out_err);
        if (st != WorkerStatus::Ok) return st;
        const WorkerStatus call_st = proc.Call(1u, request, out_reply, on_progress, is_cancelled, 0, out_err);
        if (call_st == WorkerStatus::Ok) {
            (void)proc.Shutdown();
        } else {
            proc.Terminate();
        }
        return call_st;
    }

private:
    DWORD QueryExitCode() const noexcept {
        DWORD code = 0;
        if (process_.Valid() && GetExitCodeProcess(process_.Get(), &code)) {
            return code;
        }
        return 0;
    }

    WorkerStatus TryConsumeBufferedFrame(WireFrame& out_frame, WorkerError* out_err) {
        if (read_buf_.size() < 4) return WorkerStatus::IoError; // incomplete
        const uint32_t total_len = detail::ReadU32Le(read_buf_.data());
        if (total_len < detail::kWireHeaderBytes) {
            detail::SetError(out_err, WorkerStatus::ProtocolError, 0, 0, "wire frame shorter than header");
            return WorkerStatus::ProtocolError;
        }
        if (total_len > limits_.max_frame_bytes) {
            detail::SetError(out_err, WorkerStatus::FrameTooLarge, 0, 0, "wire frame exceeds max_frame_bytes");
            return WorkerStatus::FrameTooLarge;
        }
        const size_t full_bytes = 4u + static_cast<size_t>(total_len);
        if (read_buf_.size() < full_bytes) return WorkerStatus::IoError; // incomplete

        out_frame.kind = read_buf_[4];
        out_frame.tag = detail::ReadU32Le(read_buf_.data() + 5);
        const size_t payload_len = static_cast<size_t>(total_len - detail::kWireHeaderBytes);
        if (payload_len > 0) {
            out_frame.payload.assign(reinterpret_cast<const char*>(read_buf_.data() + 9), payload_len);
        } else {
            out_frame.payload.clear();
        }
        read_buf_.erase(read_buf_.begin(), read_buf_.begin() + static_cast<ptrdiff_t>(full_bytes));
        return WorkerStatus::Ok;
    }

    bool ReadAvailableChunk(DWORD avail, WorkerError* out_err) {
        const DWORD to_read = (std::min)(avail, DWORD{65536});
        const size_t old_size = read_buf_.size();
        const size_t max_buf = static_cast<size_t>(limits_.max_frame_bytes) + 65536u;
        if (old_size > max_buf) {
            detail::SetError(out_err, WorkerStatus::FrameTooLarge, 0, 0, "receive buffer exceeded max frame size");
            return false;
        }
        read_buf_.resize(old_size + to_read);
        DWORD got = 0;
        if (!ReadFile(stdout_read_.Get(), read_buf_.data() + old_size, to_read, &got, nullptr) || got == 0) {
            const DWORD err = GetLastError();
            read_buf_.resize(old_size);
            WaitForSingleObject(process_.Get(), 50);
            detail::SetError(out_err, WorkerStatus::WorkerExited, err, QueryExitCode(), "worker stdout read failed");
            return false;
        }
        read_buf_.resize(old_size + got);
        return true;
    }

    UniqueHandle job_;
    UniqueHandle process_;
    UniqueHandle stdin_write_;
    UniqueHandle stdout_read_;
    WorkerLimits limits_{};
    std::vector<uint8_t> read_buf_;
};

class WorkerServer {
public:
    class Responder {
    public:
        Responder(HANDLE out_handle, uint32_t max_frame_bytes) noexcept
            : out_(out_handle), max_frame_bytes_(max_frame_bytes) {}

        WorkerStatus SendProgress(uint32_t tag, std::string_view payload, WorkerError* out_err = nullptr) const {
            return WriteWireFrame(out_, static_cast<uint8_t>(FrameKind::Progress), tag, payload, max_frame_bytes_, out_err);
        }

        WorkerStatus SendReply(uint32_t tag, std::string_view payload, WorkerError* out_err = nullptr) const {
            return WriteWireFrame(out_, static_cast<uint8_t>(FrameKind::Reply), tag, payload, max_frame_bytes_, out_err);
        }

        WorkerStatus SendError(uint32_t tag, std::string_view message, WorkerError* out_err = nullptr) const {
            return WriteWireFrame(out_, static_cast<uint8_t>(FrameKind::Error), tag, message, max_frame_bytes_, out_err);
        }

    private:
        HANDLE out_ = nullptr;
        uint32_t max_frame_bytes_ = 4u * 1024u * 1024u;
    };

    static void ConfigureProcess() noexcept {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    }

    template <class Handler>
    static int RunLoop(Handler&& handler, uint32_t max_frame_bytes = 4u * 1024u * 1024u) {
        ConfigureProcess();
        HANDLE in_handle = GetStdHandle(STD_INPUT_HANDLE);
        HANDLE out_handle = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!in_handle || in_handle == INVALID_HANDLE_VALUE ||
            !out_handle || out_handle == INVALID_HANDLE_VALUE) {
            return 2;
        }
        const uint32_t cap = detail::ClampFrameLimit(max_frame_bytes);
        Responder responder(out_handle, cap);
        for (;;) {
            WireFrame frame;
            WorkerError err;
            const WorkerStatus st = ReadWireFrame(in_handle, frame, cap, &err);
            if (st == WorkerStatus::IoError) {
                return 0; // clean EOF when parent closes pipe or exits
            }
            if (st != WorkerStatus::Ok) {
                return 3;
            }
            if (frame.kind == static_cast<uint8_t>(FrameKind::Shutdown)) {
                return 0;
            }
            if (frame.kind == static_cast<uint8_t>(FrameKind::Request)) {
                if (!handler(frame, responder)) {
                    return 0;
                }
            }
        }
    }
};

} // namespace pulse::worker

#endif

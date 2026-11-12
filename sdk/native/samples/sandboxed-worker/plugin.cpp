#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <pulse/abi.h>
#include <pulse/client.hpp>
#include <pulse/commands.h>
#include <pulse/tasks.h>
#include <pulse/ui_mount.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

PULSE_PLUGIN_SDK_PREVIEW()

namespace {

#ifndef PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY
#define PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY \
    ProcThreadAttributeValue(14, FALSE, TRUE, FALSE)
#endif
#ifndef PROCESS_CREATION_CHILD_PROCESS_RESTRICTED
#define PROCESS_CREATION_CHILD_PROCESS_RESTRICTED 0x01
#endif

constexpr uint32_t kMaxIpcFrameBytes = 4u * 1024u * 1024u;
constexpr SIZE_T kWorkerProcessMemoryCapBytes = 128u * 1024u * 1024u;

struct UniqueHandle {
    HANDLE handle = nullptr;

    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) noexcept : handle(h) {}
    ~UniqueHandle() { Reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }

    void Reset(HANDLE next = nullptr) noexcept {
        if (handle && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
        handle = next;
    }

    explicit operator bool() const noexcept {
        return handle != nullptr && handle != INVALID_HANDLE_VALUE;
    }
};

std::wstring PluginDirectory() {
    HMODULE self_module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&PluginDirectory),
            &self_module) ||
        !self_module) {
        return {};
    }
    std::wstring buf(32768, L'\0');
    const DWORD len = GetModuleFileNameW(self_module, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0 || len >= buf.size()) {
        return {};
    }
    buf.resize(len);
    const size_t slash = buf.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? std::wstring{} : buf.substr(0, slash);
}

bool WritePipeExact(HANDLE pipe, const void* data, uint32_t total) noexcept {
    const auto* cursor = static_cast<const uint8_t*>(data);
    uint32_t remaining = total;
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, cursor, remaining, &written, nullptr) || written == 0) {
            return false;
        }
        cursor += written;
        remaining -= written;
    }
    return true;
}

bool ReadPipeAvailable(
    HANDLE pipe,
    HANDLE process,
    const PulseTaskContext* ctx,
    std::string& out_reply,
    bool& cancelled_out) {
    cancelled_out = false;
    std::vector<uint8_t> raw;
    raw.reserve(256);

    for (;;) {
        if (ctx && ctx->cancelled && ctx->cancelled(ctx->context) != 0) {
            cancelled_out = true;
            return false;
        }

        DWORD avail = 0;
        if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            std::array<uint8_t, 4096> chunk{};
            const DWORD to_read = avail < chunk.size() ? avail : static_cast<DWORD>(chunk.size());
            DWORD got = 0;
            if (!ReadFile(pipe, chunk.data(), to_read, &got, nullptr) || got == 0) {
                break;
            }
            if (raw.size() + got > kMaxIpcFrameBytes + sizeof(uint32_t)) {
                return false;
            }
            raw.insert(raw.end(), chunk.data(), chunk.data() + got);
            if (raw.size() >= sizeof(uint32_t)) {
                uint32_t expected_len = 0;
                std::memcpy(&expected_len, raw.data(), sizeof(uint32_t));
                if (expected_len > kMaxIpcFrameBytes) {
                    return false;
                }
                if (raw.size() >= sizeof(uint32_t) + expected_len) {
                    out_reply.assign(
                        reinterpret_cast<const char*>(raw.data() + sizeof(uint32_t)),
                        expected_len);
                    return true;
                }
            }
            continue;
        }

        const DWORD wait_rc = WaitForSingleObject(process, 10);
        if (wait_rc == WAIT_OBJECT_0) {
            // Drain any final bytes queued in the anonymous pipe before process exit.
            while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                std::array<uint8_t, 4096> chunk{};
                DWORD got = 0;
                if (!ReadFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr) || got == 0) {
                    break;
                }
                raw.insert(raw.end(), chunk.data(), chunk.data() + got);
            }
            if (raw.size() >= sizeof(uint32_t)) {
                uint32_t expected_len = 0;
                std::memcpy(&expected_len, raw.data(), sizeof(uint32_t));
                if (expected_len <= kMaxIpcFrameBytes && raw.size() >= sizeof(uint32_t) + expected_len) {
                    out_reply.assign(
                        reinterpret_cast<const char*>(raw.data() + sizeof(uint32_t)),
                        expected_len);
                    return true;
                }
            }
            return false;
        }
    }
    return false;
}

UniqueHandle CreateGovernedWorkerJob() noexcept {
    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        return {};
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
        JOB_OBJECT_LIMIT_PROCESS_MEMORY |
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = kWorkerProcessMemoryCapBytes;
    if (!SetInformationJobObject(
            job.handle,
            JobObjectExtendedLimitInformation,
            &limits,
            static_cast<DWORD>(sizeof(limits)))) {
        return {};
    }
    return job;
}

struct WorkerJobPayload {
    std::wstring worker_exe;
    std::vector<std::string> selected_paths;
};

PulseTaskState RunSandboxedWorker(const WorkerJobPayload& job_payload, const PulseTaskContext* ctx) {
    UniqueHandle job = CreateGovernedWorkerJob();
    if (!job) {
        PulseTaskProgress err{};
        err.size = sizeof(err);
        err.error = pulse::sdk::View("Failed to configure Win32 Job Object for sandboxed worker");
        ctx->report(ctx->context, &err);
        return PULSE_TASK_FAILED;
    }

    SECURITY_ATTRIBUTES inheritable_sa{};
    inheritable_sa.nLength = sizeof(inheritable_sa);
    inheritable_sa.bInheritHandle = TRUE;

    HANDLE raw_in_read = nullptr;
    HANDLE raw_in_write = nullptr;
    HANDLE raw_out_read = nullptr;
    HANDLE raw_out_write = nullptr;
    if (!CreatePipe(&raw_in_read, &raw_in_write, &inheritable_sa, 65536)) {
        return PULSE_TASK_FAILED;
    }
    UniqueHandle child_stdin_read(raw_in_read);
    UniqueHandle parent_stdin_write(raw_in_write);

    if (!CreatePipe(&raw_out_read, &raw_out_write, &inheritable_sa, 65536)) {
        return PULSE_TASK_FAILED;
    }
    UniqueHandle parent_stdout_read(raw_out_read);
    UniqueHandle child_stdout_write(raw_out_write);

    // Ensure parent-owned pipe ends are NOT inheritable.
    SetHandleInformation(parent_stdin_write.handle, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parent_stdout_read.handle, HANDLE_FLAG_INHERIT, 0);

    std::array<HANDLE, 2> inherited_handles = {child_stdin_read.handle, child_stdout_write.handle};
    DWORD child_policy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;

    auto spawn_with_attrs = [&](DWORD attr_count) -> std::optional<PROCESS_INFORMATION> {
        SIZE_T attr_size = 0;
        InitializeProcThreadAttributeList(nullptr, attr_count, 0, &attr_size);
        if (attr_size == 0) {
            return std::nullopt;
        }
        std::vector<uint8_t> attr_storage(attr_size, 0);
        auto* attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_storage.data());
        if (!InitializeProcThreadAttributeList(attr_list, attr_count, 0, &attr_size)) {
            return std::nullopt;
        }
        bool ok = UpdateProcThreadAttribute(
            attr_list,
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited_handles.data(),
            inherited_handles.size() * sizeof(HANDLE),
            nullptr,
            nullptr);
        if (ok && attr_count >= 2) {
            ok = UpdateProcThreadAttribute(
                attr_list,
                0,
                PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY,
                &child_policy,
                sizeof(child_policy),
                nullptr,
                nullptr);
        }

        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = child_stdin_read.handle;
        si.StartupInfo.hStdOutput = child_stdout_write.handle;
        si.StartupInfo.hStdError = child_stdout_write.handle;
        si.lpAttributeList = attr_list;

        std::wstring cmdline = L"\"" + job_payload.worker_exe + L"\"";
        PROCESS_INFORMATION pi{};
        const BOOL created = ok && CreateProcessW(
            job_payload.worker_exe.c_str(),
            cmdline.data(),
            nullptr,
            nullptr,
            TRUE,
            EXTENDED_STARTUPINFO_PRESENT | DETACHED_PROCESS | CREATE_SUSPENDED,
            nullptr,
            nullptr,
            &si.StartupInfo,
            &pi);

        DeleteProcThreadAttributeList(attr_list);
        if (!created) {
            return std::nullopt;
        }
        return pi;
    };

    std::optional<PROCESS_INFORMATION> proc_info = spawn_with_attrs(2);
    if (!proc_info) {
        proc_info = spawn_with_attrs(1);
    }
    if (!proc_info) {
        PulseTaskProgress err{};
        err.size = sizeof(err);
        err.error = pulse::sdk::View("Failed to launch sandboxed_worker.exe");
        ctx->report(ctx->context, &err);
        return PULSE_TASK_FAILED;
    }

    UniqueHandle child_process(proc_info->hProcess);
    UniqueHandle child_thread(proc_info->hThread);

    // Close child-side pipe handles in parent immediately so EOF is observed cleanly.
    child_stdin_read.Reset();
    child_stdout_write.Reset();

    if (!AssignProcessToJobObject(job.handle, child_process.handle)) {
        TerminateProcess(child_process.handle, 1);
        WaitForSingleObject(child_process.handle, 2000);
        PulseTaskProgress err{};
        err.size = sizeof(err);
        err.error = pulse::sdk::View("AssignProcessToJobObject failed");
        ctx->report(ctx->context, &err);
        return PULSE_TASK_FAILED;
    }
    ResumeThread(child_thread.handle);
    child_thread.Reset();

    // Serialize newline-delimited UTF-8 selection paths into framed request.
    std::string request_body;
    for (const auto& path : job_payload.selected_paths) {
        request_body.append(path);
        request_body.push_back('\n');
    }
    const uint32_t req_len = static_cast<uint32_t>(request_body.size());
    const bool sent = WritePipeExact(parent_stdin_write.handle, &req_len, sizeof(req_len)) &&
                      (req_len == 0 || WritePipeExact(parent_stdin_write.handle, request_body.data(), req_len));
    parent_stdin_write.Reset();
    if (!sent) {
        job.Reset();
        WaitForSingleObject(child_process.handle, 2000);
        return PULSE_TASK_FAILED;
    }

    std::string reply;
    bool was_cancelled = false;
    const bool got_reply = ReadPipeAvailable(
        parent_stdout_read.handle,
        child_process.handle,
        ctx,
        reply,
        was_cancelled);
    parent_stdout_read.Reset();

    if (was_cancelled) {
        // Closing the Job Object with KILL_ON_JOB_CLOSE terminates the worker immediately.
        job.Reset();
        WaitForSingleObject(child_process.handle, 2000);
        return PULSE_TASK_CANCELLED;
    }

    WaitForSingleObject(child_process.handle, 2000);
    DWORD exit_code = 1;
    GetExitCodeProcess(child_process.handle, &exit_code);
    job.Reset();

    if (!got_reply || exit_code != 0 || !reply.starts_with("OK\t")) {
        char code_buf[32]{};
        std::snprintf(code_buf, sizeof(code_buf), "0x%08lx", static_cast<unsigned long>(exit_code));
        PulseTaskProgress err{};
        err.size = sizeof(err);
        const std::string msg = reply.empty()
            ? (std::string("Worker exited without a valid reply frame (exit_code=") + code_buf + ")")
            : (reply + " (exit_code=" + code_buf + ")");
        err.error = pulse::sdk::View(msg);
        ctx->report(ctx->context, &err);
        return PULSE_TASK_FAILED;
    }

    // Parse "OK\t<count>\t<bytes>\t<sha256>"
    const size_t last_tab = reply.rfind('\t');
    const std::string digest = (last_tab != std::string::npos) ? reply.substr(last_tab + 1) : reply;
    const std::string notice = "SHA-256: " + digest;
    const std::string log_line =
        "Sandboxed worker completed inside Win32 Job Object (ActiveProcessLimit=1, 128 MiB cap): " + reply;

    PulseTaskProgress done{};
    done.size = sizeof(done);
    done.percent = 100;
    done.completed_items = job_payload.selected_paths.size();
    done.total_items = job_payload.selected_paths.size();
    done.result = pulse::sdk::View(notice);
    done.log = pulse::sdk::View(log_line);
    ctx->report(ctx->context, &done);
    return PULSE_TASK_COMPLETED;
}

class SandboxedWorkerPlugin final {
public:
    explicit SandboxedWorkerPlugin(const PulseHostV1* host)
        : host_(*host),
          plugin_dir_(PluginDirectory()) {
        PulseCommandsV1 cmds{};
        if (host_.Query(PULSE_COMMANDS_INTERFACE_NAME, PULSE_COMMANDS_VERSION_1, cmds) == PULSE_OK) {
            commands_ = cmds;
        }
        PulseTasksV1 tasks{};
        if (host_.Query(PULSE_TASKS_INTERFACE_NAME, PULSE_TASKS_VERSION_1, tasks) == PULSE_OK) {
            tasks_ = tasks;
            task_group_.emplace(tasks);
        }
        // Optional experimental UI probe: gracefully ignored when absent (--omit-experimental-ui).
        PulseUiMountV1 mount{};
        if (host_.Query(PULSE_UI_MOUNT_INTERFACE, PULSE_UI_MOUNT_VERSION_1, mount) == PULSE_OK) {
            optional_mount_ = mount;
        }
    }

    PulseStatus RegisterCommands() noexcept {
        if (!commands_ || !task_group_) {
            return PULSE_ERROR_UNAVAILABLE;
        }
        try {
            PulseCommandDescriptor desc{};
            desc.size = sizeof(desc);
            desc.id = pulse::sdk::View("hash-selection");
            desc.title = pulse::sdk::View("Compute SHA-256 in Sandboxed Worker");
            desc.detail = pulse::sdk::View("SHA-256 via Job-Object sidecar ({base})");
            desc.icon = PULSE_COMMAND_ICON_DEFAULT;
            desc.kinds = PULSE_COMMAND_FILES | PULSE_COMMAND_DIRECTORIES;
            desc.min_selection = 0;
            desc.max_selection = 4096;
            return command_reg_.Register(
                *commands_,
                desc,
                [this](PulseCommandId, const PulseContextSelectionSnapshot& selection) {
                    OnInvokeSelection(selection);
                });
        } catch (...) {
            return PULSE_ERROR_INITIALIZATION;
        }
    }

    PulseStatus Activate() noexcept {
        active_ = true;
        return PULSE_OK;
    }

    void Deactivate() noexcept {
        active_ = false;
        if (task_group_) {
            task_group_->CancelAll();
            task_group_->Collect();
        }
        command_reg_.Reset();
    }

    void BeforeDestroy() noexcept {
        Deactivate();
    }

private:
    void OnInvokeSelection(const PulseContextSelectionSnapshot& selection) {
        if (!active_ || !task_group_) {
            return;
        }
        task_group_->Collect();
        auto payload = std::make_shared<WorkerJobPayload>();
        payload->worker_exe = plugin_dir_ + L"\\sandboxed_worker.exe";
        if (selection.entries && selection.entry_count > 0) {
            payload->selected_paths.reserve(selection.entry_count);
            for (uint32_t i = 0; i < selection.entry_count; ++i) {
                const auto& row = selection.entries[i];
                if (row.path.data && row.path.length > 0) {
                    payload->selected_paths.emplace_back(row.path.data, row.path.length);
                }
            }
        }

        PulseTaskId task_id = 0;
        task_group_->Start(
            "Compute SHA-256 in Sandboxed Worker",
            "checksum.sandboxed",
            [payload](const PulseTaskContext& ctx) noexcept -> PulseTaskState {
                try {
                    return RunSandboxedWorker(*payload, &ctx);
                } catch (...) {
                    return PULSE_TASK_FAILED;
                }
            },
            {},
            &task_id);
    }

    pulse::sdk::Host host_;
    std::optional<PulseCommandsV1> commands_;
    std::optional<PulseTasksV1> tasks_;
    std::optional<PulseUiMountV1> optional_mount_;
    std::optional<pulse::sdk::TaskGroup> task_group_;
    pulse::sdk::CommandRegistration command_reg_;
    std::wstring plugin_dir_;
    bool active_ = false;
};

PulseStatus PULSE_CALL PluginActivate(void* instance) noexcept {
    return instance ? static_cast<SandboxedWorkerPlugin*>(instance)->Activate() : PULSE_ERROR_ARGUMENT;
}

void PULSE_CALL PluginDeactivate(void* instance) noexcept {
    if (instance) {
        static_cast<SandboxedWorkerPlugin*>(instance)->Deactivate();
    }
}

void PULSE_CALL PluginDestroy(void* instance) noexcept {
    auto* self = static_cast<SandboxedWorkerPlugin*>(instance);
    if (self) {
        self->BeforeDestroy();
        delete self;
    }
}

} // namespace

extern "C" __declspec(dllexport) PulseStatus PULSE_CALL PulsePluginEntryV1(
    const PulseHostV1* host,
    PulsePluginV1* out_plugin) noexcept {
    if (!host || !out_plugin || host->size < sizeof(PulseHostV1) || host->version != PULSE_ABI_VERSION_1 ||
        out_plugin->size < sizeof(PulsePluginV1)) {
        return PULSE_ERROR_ABI;
    }
    try {
        auto plugin = std::make_unique<SandboxedWorkerPlugin>(host);
        const PulseStatus reg_status = plugin->RegisterCommands();
        if (reg_status != PULSE_OK) {
            return reg_status;
        }
        out_plugin->version = PULSE_ABI_VERSION_1;
        out_plugin->instance = plugin.release();
        out_plugin->activate = &PluginActivate;
        out_plugin->deactivate = &PluginDeactivate;
        out_plugin->destroy = &PluginDestroy;
        return PULSE_OK;
    } catch (...) {
        return PULSE_ERROR_INITIALIZATION;
    }
}

// transcode_manager.cpp - Job-Object supervised video wallpaper transcoding controller.
#include "host/video/transcode_manager.h"

#include <algorithm>
#include <filesystem>

namespace pulse::desktop::video {
namespace {

namespace fs = std::filesystem;

std::wstring QuoteArg(const std::wstring& s) {
    if (!s.empty() && s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring out = L"\"";
    for (size_t i = 0; i < s.size(); ++i) {
        size_t slashes = 0;
        while (i < s.size() && s[i] == L'\\') { ++i; ++slashes; }
        if (i == s.size()) {
            out.append(slashes * 2, L'\\');
            break;
        }
        if (s[i] == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(slashes, L'\\');
            out.push_back(s[i]);
        }
    }
    out.push_back(L'"');
    return out;
}

} // namespace

TranscodeManager::~TranscodeManager() {
    Cancel();
    WaitUntilIdle();
}

std::wstring TranscodeManager::ResolveSiblingTranscoderExe() {
    wchar_t mod_path[MAX_PATH * 4]{};
    const DWORD len = GetModuleFileNameW(nullptr, mod_path, ARRAYSIZE(mod_path));
    if (len == 0 || len >= ARRAYSIZE(mod_path)) return {};
    const fs::path exe = fs::path(std::wstring(mod_path, len)).parent_path() / L"pulse_wallpaper_transcoder.exe";
    return exe.wstring();
}

void TranscodeManager::CleanupJobHandlesLocked() noexcept {
    if (proc_handle_) {
        CloseHandle(proc_handle_);
        proc_handle_ = nullptr;
    }
    if (job_handle_) {
        CloseHandle(job_handle_);
        job_handle_ = nullptr;
    }
}

void TranscodeManager::WaitUntilIdle() {
    std::thread to_join;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (watch_thread_.joinable() && watch_thread_.get_id() != std::this_thread::get_id()) {
            to_join = std::move(watch_thread_);
        }
    }
    if (to_join.joinable()) to_join.join();
}

void TranscodeManager::Cancel() {
    cancel_requested_.store(true, std::memory_order_release);
    if (pipe_.IsConnected()) {
        pipe_.Send(ipc::MessageType::CommandActionMsg, next_req_id_.fetch_add(1), "{\"cmd\":\"cancel\"}");
    }
    HANDLE job = nullptr;
    HANDLE proc = nullptr;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        job = job_handle_;
        proc = proc_handle_;
    }
    if (job) {
        TerminateJobObject(job, 3u);
    } else if (proc) {
        TerminateProcess(proc, 3u);
    }
}

void TranscodeManager::SetPaused(bool paused) {
    const bool prev = paused_.exchange(paused, std::memory_order_acq_rel);
    if (prev == paused) return;
    if (pipe_.IsConnected()) {
        pipe_.Send(ipc::MessageType::CommandActionMsg, next_req_id_.fetch_add(1),
                   paused ? "{\"cmd\":\"pause\"}" : "{\"cmd\":\"resume\"}");
    }
}

bool TranscodeManager::StartJob(const TranscodeJobSpec& spec, TranscodeCallbacks callbacks) {
    Cancel();
    WaitUntilIdle();

    const std::wstring exe = spec.transcoder_exe_override.empty()
                                 ? ResolveSiblingTranscoderExe()
                                 : spec.transcoder_exe_override;
    if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    if (spec.source_path.empty() || spec.temp_output_path.empty() || spec.final_output_path.empty()) {
        return false;
    }

    std::error_code ec;
    const fs::path out_p(spec.temp_output_path);
    if (out_p.has_parent_path()) fs::create_directories(out_p.parent_path(), ec);
    DeleteFileW(spec.temp_output_path.c_str());

    const std::wstring pipe_name = ipc::MakeUniquePipeName();
    if (pipe_name.empty()) return false;

    cancel_requested_.store(false, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    progress_.store(0.0f, std::memory_order_release);
    active_job_id_.store(spec.job_id, std::memory_order_release);

    const uint64_t jid = spec.job_id;
    auto on_prog = callbacks.on_progress;
    if (!pipe_.StartServer(
            pipe_name,
            [this, jid, on_prog](ipc::Frame&& f) {
                double prog = 0.0;
                if (ipc::JsonFindDouble(f.PayloadText(), "progress", prog)) {
                    const float clamped = static_cast<float>(std::clamp(prog, 0.0, 1.0));
                    progress_.store(clamped, std::memory_order_release);
                    if (on_prog && !cancel_requested_.load(std::memory_order_acquire)) {
                        try { on_prog(jid, clamped); } catch (...) {}
                    }
                }
            },
            {},
            8000)) {
        return false;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        pipe_.Stop();
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
        (spec.memory_limit_bytes > 0 ? JOB_OBJECT_LIMIT_JOB_MEMORY : 0u);
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.JobMemoryLimit = static_cast<SIZE_T>(spec.memory_limit_bytes);
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(job);
        pipe_.Stop();
        return false;
    }

    std::wstring cmdline = QuoteArg(exe) +
                           L" --input " + QuoteArg(spec.source_path) +
                           L" --output " + QuoteArg(spec.temp_output_path) +
                           L" --width " + std::to_wstring(spec.target_width) +
                           L" --height " + std::to_wstring(spec.target_height) +
                           L" --codec " + CodecChoiceArgName(spec.codec) +
                           L" --pipe " + QuoteArg(pipe_name);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS;
    if (!CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, flags,
                        nullptr, nullptr, &si, &pi)) {
        CloseHandle(job);
        pipe_.Stop();
        return false;
    }
    if (!AssignProcessToJobObject(job, pi.hProcess) || ResumeThread(pi.hThread) == DWORD(-1)) {
        TerminateProcess(pi.hProcess, 6u);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        pipe_.Stop();
        return false;
    }
    CloseHandle(pi.hThread);

    {
        std::lock_guard<std::mutex> lk(mtx_);
        CleanupJobHandlesLocked();
        job_handle_ = job;
        proc_handle_ = pi.hProcess;
        temp_path_ = spec.temp_output_path;
        active_.store(true, std::memory_order_release);
        watch_thread_ = std::thread([this, spec, cb = std::move(callbacks)]() {
            HANDLE proc = nullptr;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                proc = proc_handle_;
            }
            if (proc) {
                WaitForSingleObject(proc, INFINITE);
            }
            DWORD exit_code = 6u;
            if (proc) GetExitCodeProcess(proc, &exit_code);

            pipe_.Stop();
            {
                std::lock_guard<std::mutex> lk(mtx_);
                CleanupJobHandlesLocked();
            }

            TranscodeOutcome outcome = TranscodeOutcome::Failed;
            if (cancel_requested_.load(std::memory_order_acquire) || exit_code == 3u) {
                outcome = TranscodeOutcome::Cancelled;
                DeleteFileW(spec.temp_output_path.c_str());
            } else if (exit_code == 0u) {
                if (MoveFileExW(spec.temp_output_path.c_str(), spec.final_output_path.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    outcome = TranscodeOutcome::Succeeded;
                    progress_.store(1.0f, std::memory_order_release);
                } else {
                    outcome = TranscodeOutcome::Failed;
                    DeleteFileW(spec.temp_output_path.c_str());
                }
            } else if (exit_code == 4u || exit_code == 0xC0000017ul || exit_code == 0xC000012Dul) {
                outcome = TranscodeOutcome::OutOfMemory;
                DeleteFileW(spec.temp_output_path.c_str());
            } else if (exit_code == 5u) {
                outcome = TranscodeOutcome::CodecError;
                DeleteFileW(spec.temp_output_path.c_str());
            } else {
                outcome = TranscodeOutcome::Failed;
                DeleteFileW(spec.temp_output_path.c_str());
            }

            active_.store(false, std::memory_order_release);
            if (cb.on_finished) {
                try {
                    cb.on_finished(spec.job_id, outcome,
                                   outcome == TranscodeOutcome::Succeeded ? spec.final_output_path : std::wstring{});
                } catch (...) {}
            }
        });
    }
    return true;
}

} // namespace pulse::desktop::video

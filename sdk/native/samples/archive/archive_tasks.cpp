#include "archive_tasks.h"
#include "archive_reader.h"
#include <pulse/sdk.hpp>
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <stdexcept>

using pulse::sdk::View;
struct ArchiveTasks::Job {
    std::wstring path;
    std::vector<std::wstring> sources;
    std::wstring archive_name;
    archive_writer::Format format = archive_writer::Format::SevenZip;
    unsigned level = 1;
    PulseStagingV1 staging_api{};
    std::string destination;
    std::atomic<bool> creation_done{false};
    PulseStatus creation_status = PULSE_ERROR_STATE;
    PulseStageId stage_id = 0;
    std::string stage_path;
    static void PULSE_CALL Created(void* value, PulseStatus status, PulseStageId stage, PulseStringView path) {
        auto& job = *static_cast<Job*>(value);
        job.stage_id = stage;
        job.creation_status = status;
        try { if (path.data && path.length) job.stage_path.assign(path.data, path.length); }
        catch (...) { job.creation_status = PULSE_ERROR_INITIALIZATION; }
        job.creation_done.store(true, std::memory_order_release);
    }
    struct StageResult {
        PulseStageState state = PULSE_STAGE_PREPARING;
        std::string error;
        static void PULSE_CALL Read(void* value, const PulseStageSnapshot* snapshot) {
            auto& result = *static_cast<StageResult*>(value);
            result.state = snapshot->state;
            try { if (snapshot->error.data && snapshot->error.length)
                result.error.assign(snapshot->error.data, snapshot->error.length); }
            catch (...) { result.error.clear(); }
        }
    };
    std::atomic<bool> cancel{false};
    static void PULSE_CALL Cancel(void* value, PulseTaskId) { static_cast<Job*>(value)->cancel = true; }
    static PulseTaskState PULSE_CALL Run(void* value, const PulseTaskContext* task) {
        auto& job = *static_cast<Job*>(value);
        std::filesystem::path staging;
        PulseTaskProgress progress{}; progress.size = sizeof(progress); progress.percent = UINT32_MAX;
        const auto cancelled = [&] { return job.cancel || task->cancelled(task->context); };
        const auto fail = [&](const std::string& error) {
            progress.error = View(error);
            task->report(task->context, &progress);
            progress.error = {};
        };
        PulseTaskState outcome = PULSE_TASK_FAILED;
        const bool publishing = !job.destination.empty();
        bool stage_requested = false, commit_started = false, commit_terminal = false;
        try {
            if (cancelled()) return PULSE_TASK_CANCELLED;
            if (publishing) {
                const auto accepted = job.staging_api.create(job.staging_api.context, View(job.destination), Created, &job);
                if (accepted != PULSE_OK) { fail("Cannot request a private archive stage"); return PULSE_TASK_FAILED; }
                stage_requested = true;
                // Accepted creation owns a callback into this Job, even after cancel.
                while (!job.creation_done.load(std::memory_order_acquire)) Sleep(10);
                if (job.creation_status != PULSE_OK || !job.stage_id || job.stage_path.empty()) {
                    fail("Cannot create the private archive stage");
                } else {
                    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, job.stage_path.data(),
                        static_cast<int>(job.stage_path.size()), nullptr, 0);
                    if (!length) throw std::runtime_error("Invalid staging path");
                    std::wstring path(static_cast<size_t>(length), L'\0');
                    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, job.stage_path.data(),
                        static_cast<int>(job.stage_path.size()), path.data(), length);
                    staging = path;
                }
            } else {
                wchar_t root[32768]{};
                const auto length = GetTempPathW(32768, root);
                if (!length || length >= 32768) { fail("Temporary directory is unavailable"); return PULSE_TASK_FAILED; }
                staging = std::filesystem::path(root) / (L"PulseArchiveVerify-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(task->id));
                if (!CreateDirectoryW(staging.c_str(), nullptr)) {
                    staging.clear(); fail("Cannot create a private verification directory"); return PULSE_TASK_FAILED;
                }
            }
            if (!staging.empty()) {
                const auto report_progress = [&](const auto& value) {
                    progress.completed_bytes = value.completed_bytes; progress.total_bytes = value.total_bytes;
                    progress.completed_items = value.completed_items; progress.total_items = value.total_items;
                    progress.percent = value.total_bytes ? static_cast<uint32_t>(value.completed_bytes * (publishing ? 90u : 100u) / value.total_bytes) : UINT32_MAX;
                    progress.current_item = View(value.path);
                    task->report(task->context, &progress);
                    progress.current_item = {};
                };
                if (!job.sources.empty()) {
                    const auto result = archive_writer::Create(job.sources, staging.wstring(), job.archive_name,
                        job.format, job.level, cancelled, report_progress);
                    if (result.status == archive_writer::Status::Cancelled) outcome = PULSE_TASK_CANCELLED;
                    else if (result.status == archive_writer::Status::Ok) outcome = PULSE_TASK_COMPLETED;
                    else { fail(result.error); outcome = PULSE_TASK_FAILED; }
                } else {
                    const auto result = archive_reader::Extract(job.path, staging.wstring(), cancelled, report_progress);
                    if (result.status == archive_reader::Status::Cancelled) outcome = PULSE_TASK_CANCELLED;
                    else if (result.status == archive_reader::Status::Ok) outcome = PULSE_TASK_COMPLETED;
                    else { fail(result.error); outcome = PULSE_TASK_FAILED; }
                }
            }
            if (publishing && outcome == PULSE_TASK_COMPLETED) {
                if (cancelled()) outcome = PULSE_TASK_CANCELLED;
                else if (job.staging_api.commit(job.staging_api.context, job.stage_id) != PULSE_OK) {
                    fail("Cannot submit generated files for host confirmation"); outcome = PULSE_TASK_FAILED;
                } else {
                    commit_started = true;
                    progress.percent = 90;
                    progress.current_item = View("Waiting for host confirmation and file operations");
                    task->report(task->context, &progress); progress.current_item = {};
                    for (;;) {
                        if (cancelled()) job.staging_api.cancel(job.staging_api.context, job.stage_id);
                        StageResult state;
                        if (job.staging_api.get(job.staging_api.context, job.stage_id, StageResult::Read, &state) != PULSE_OK) {
                            fail("Cannot read archive commit status"); outcome = PULSE_TASK_FAILED; break;
                        }
                        if (state.state >= PULSE_STAGE_COMPLETED) {
                            commit_terminal = true;
                            outcome = state.state == PULSE_STAGE_COMPLETED ? PULSE_TASK_COMPLETED
                                : state.state == PULSE_STAGE_PARTIAL ? PULSE_TASK_PARTIAL
                                : state.state == PULSE_STAGE_CANCELLED ? PULSE_TASK_CANCELLED : PULSE_TASK_FAILED;
                            if (!state.error.empty()) fail(state.error);
                            if (outcome == PULSE_TASK_COMPLETED) {
                                progress.percent = 100; task->report(task->context, &progress);
                            }
                            break;
                        }
                        Sleep(10);
                    }
                }
            }
        } catch (...) { fail(publishing ? "Archive generation failed" : "Archive verification failed"); }
        if (stage_requested) {
            while (!job.creation_done.load(std::memory_order_acquire)) Sleep(10);
            if (job.stage_id) {
                if (commit_started && !commit_terminal) {
                    job.staging_api.cancel(job.staging_api.context, job.stage_id);
                    // Host retains a commit whose snapshot became unavailable;
                    // never release a directory that its file operation may use.
                    for (;;) {
                        StageResult state;
                        if (job.staging_api.get(job.staging_api.context, job.stage_id, StageResult::Read, &state) != PULSE_OK) break;
                        if (state.state >= PULSE_STAGE_COMPLETED) { commit_terminal = true; break; }
                        Sleep(10);
                    }
                }
                if ((!commit_started || commit_terminal) &&
                    job.staging_api.release(job.staging_api.context, job.stage_id) != PULSE_OK) {
                    fail("Cannot release the private archive stage"); outcome = PULSE_TASK_FAILED;
                }
            }
            return outcome;
        }
        // Verification owns its temporary directory; publishing stages above
        // are released only through the host, never removed by the plugin.
        if (!staging.empty()) {
            std::error_code error;
            std::filesystem::remove_all(staging, error);
            if (error) { fail("Cannot clean the private verification directory"); outcome = PULSE_TASK_FAILED; }
        }
        return outcome;
    }
};
ArchiveTasks::ArchiveTasks(PulseTasksV1 api, PulseStagingV1 staging) : tasks_(api), staging_(staging) {}
ArchiveTasks::~ArchiveTasks() = default; // Host drains task callbacks before destroy.
PulseStatus ArchiveTasks::Compress(const std::vector<std::wstring>& paths, const std::string& destination,
    const std::wstring& name, archive_writer::Format format, unsigned level) {
    if (paths.empty() || destination.empty() || name.empty()) return PULSE_ERROR_ARGUMENT;
    tasks_.Collect();
    auto job = std::make_shared<Job>();
    job->sources = paths; job->destination = destination; job->archive_name = name;
    job->format = format; job->level = level; job->staging_api = staging_;
    PulseTaskId id = 0;
    return tasks_.Start("Compress files", "Archive",
        [job](const PulseTaskContext& context) { return Job::Run(job.get(), &context); },
        [job](PulseTaskId id) { Job::Cancel(job.get(), id); }, &id);
}
PulseStatus ArchiveTasks::Verify(const std::wstring& path) { return Start(path, {}); }
PulseStatus ArchiveTasks::Extract(const std::wstring& path, const std::string& destination) {
    if (destination.empty()) return PULSE_ERROR_ARGUMENT;
    return Start(path, destination);
}
PulseStatus ArchiveTasks::Start(const std::wstring& path, const std::string& destination) {
    tasks_.Collect();
    auto job = std::make_shared<Job>();
    job->path = path; job->destination = destination; job->staging_api = staging_;
    PulseTaskId id = 0;
    return tasks_.Start(destination.empty() ? "Verify archive" : "Extract archive", "Archive",
        [job](const PulseTaskContext& context) { return Job::Run(job.get(), &context); },
        [job](PulseTaskId id) { Job::Cancel(job.get(), id); }, &id);
}

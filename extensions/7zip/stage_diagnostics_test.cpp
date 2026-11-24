#ifndef PULSE_7ZIP_JOBS_SOURCE
#ifdef PULSE_7ZIP_STAGE_ORIGINAL
#define PULSE_7ZIP_JOBS_SOURCE "../../bench_data/sevenzip-root-path-20261119/diagnostic-original/jobs.cpp"
#else
#define PULSE_7ZIP_JOBS_SOURCE "jobs.cpp"
#endif
#endif
#include PULSE_7ZIP_JOBS_SOURCE
#include <thread>

// This target exercises staging and task reporting, not the archive engine or UI.
// Unused Jobs closures retain linker references; unexpected execution fails hard.
namespace engine {
ExtractResult Extract(const std::wstring&, const std::wstring&, bool, const Cancelled&, const Report&, const AskPassword&, const Budget&) { std::abort(); }
CreateResult Create(const std::vector<std::wstring>&, const std::wstring&, const std::wstring&, const CreateOptions&, const Cancelled&, const Report&) { std::abort(); }
std::vector<HashResult> Hash(const std::vector<std::wstring>&, const Cancelled&, const Report&) { std::abort(); }
}
void Dialogs::AskPassword(const std::wstring&, bool, bool, std::function<void(bool, std::wstring, bool)>) { std::abort(); }
void Dialogs::ShowChecksums(const std::vector<engine::HashResult>&, std::function<void(bool)>) { std::abort(); }
void Dialogs::ShowTestResult(const std::wstring&, uint64_t, uint64_t, uint64_t, bool, std::function<void(bool)>) { std::abort(); }

namespace {
struct StageHost {
    PulseStatus accepted = PULSE_OK, callback = PULSE_OK, get_status = PULSE_OK, commit_status = PULSE_OK;
    PulseStageId id = 41;
    std::string path = "C:\\fixture\\stage", error = "destination access denied (win32=5)";
    unsigned gets = 0, releases = 0;
    std::thread worker;
    ~StageHost() { if (worker.joinable()) worker.join(); }
    PulseStagingV1 Api() {
        PulseStagingV1 api{};
        api.context = this;
        api.create = [](void* context, PulseStringView, PulseStageCreateFn notify, void* user) -> PulseStatus {
            auto& host = *static_cast<StageHost*>(context);
            if (host.accepted != PULSE_OK) return host.accepted;
            host.worker = std::thread([&host, notify, user] {
                Sleep(5);
                notify(user, host.callback, host.id, View(host.path));
            });
            return PULSE_OK;
        };
        api.get = [](void* context, PulseStageId, PulseStageSnapshotFn notify, void* user) -> PulseStatus {
            auto& host = *static_cast<StageHost*>(context);
            ++host.gets;
            if (host.get_status != PULSE_OK) return host.get_status;
            PulseStageSnapshot snapshot{};
            snapshot.id = host.id; snapshot.state = PULSE_STAGE_FAILED; snapshot.error = View(host.error);
            notify(user, &snapshot);
            return PULSE_OK;
        };
        api.release = [](void* context, PulseStageId) -> PulseStatus {
            ++static_cast<StageHost*>(context)->releases;
            return PULSE_OK; // Acceptance only: this fake does not claim filesystem cleanup.
        };
        api.commit = [](void* context, PulseStageId) -> PulseStatus { return static_cast<StageHost*>(context)->commit_status; };
        api.cancel = [](void*, PulseStageId) -> PulseStatus { return PULSE_OK; };
        return api;
    }
};
struct ReportCapture {
    std::string log, error, assurance;
    PulseTaskContext Context() {
        PulseTaskContext task{};
        task.context = this;
        task.report = [](void* context, const PulseTaskProgress* progress) -> PulseStatus {
            auto& capture = *static_cast<ReportCapture*>(context);
            const auto copy = [](PulseStringView value) { return value.length ? std::string(value.data, value.length) : std::string{}; };
            capture.log = copy(progress->log); capture.error = copy(progress->error); capture.assurance = copy(progress->assurance);
            return PULSE_OK;
        };
        return task;
    }
};
struct TaskHost {
    ReportCapture capture;
    PulseTaskState outcome = PULSE_TASK_RUNNING;
    PulseTasksV1 Api() {
        PulseTasksV1 api{}; api.context = this;
        api.create = [](void* context, const PulseTaskDescriptor* descriptor, PulseTaskId* id) -> PulseStatus {
            auto& host = *static_cast<TaskHost*>(context);
            *id = 101;
            auto task = host.capture.Context(); task.id = *id;
            task.cancelled = [](void*) -> uint32_t { return 0; };
            host.outcome = descriptor->work(descriptor->user, &task);
            return PULSE_OK;
        };
        api.get = [](void* context, PulseTaskId id, PulseTaskSnapshotFn callback, void* user) -> PulseStatus {
            PulseTaskSnapshot snapshot{}; snapshot.id = id; snapshot.state = static_cast<TaskHost*>(context)->outcome;
            callback(user, &snapshot); return PULSE_OK;
        };
        api.cancel = [](void*, PulseTaskId) -> PulseStatus { return PULSE_OK; };
        api.release = [](void*, PulseTaskId) -> PulseStatus { return PULSE_OK; };
        return api;
    }
};
template<class S>
void ReportFailure(S& stage, Reporter& report) {
    if constexpr (requires { stage.ReportCreateFailure(report); }) stage.ReportCreateFailure(report);
    else report.Fail(text::Get(T::ErrStage));
}
int failed = 0;
void Check(bool pass, const char* label) {
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", label);
    failed += !pass;
}
}

int main() {
    for (int scenario = 0; scenario < 5; ++scenario) {
        StageHost host;
        if (scenario == 0) host.accepted = PULSE_ERROR_PERMISSION;
        if (scenario == 1 || scenario == 2) { host.callback = PULSE_ERROR_STATE; host.path.clear(); }
        if (scenario == 2) host.get_status = PULSE_ERROR_UNAVAILABLE;
        if (scenario == 3) { host.id = 0; host.path.clear(); }
        ReportCapture capture;
        const auto context = capture.Context();
        Reporter report(context);
        {
            Stage stage(host.Api());
            const bool created = stage.Create("C:\\fixture");
            Check(created == (scenario == 4), "create outcome unchanged");
            if (!created) {
                ReportFailure(stage, report);
                Check(capture.error == text::Get(T::ErrStage), "localized stage heading preserved");
                if (scenario == 0) Check(capture.log.find("status=8") != std::string::npos, "direct refusal status retained in Diagnostics");
                if (scenario == 1) Check(host.gets == 1 && capture.log.find("id=41") != std::string::npos &&
                    capture.log.find(host.error) != std::string::npos, "callback failure retains stage id and host snapshot error");
                if (scenario == 2) Check(host.gets == 1 && capture.log.find("get_status=3") != std::string::npos &&
                    capture.log.find("status=5") != std::string::npos, "get refusal does not erase callback status");
                if (scenario == 3) Check(capture.log.find("id=0") != std::string::npos &&
                    capture.log.find("path_empty=1") != std::string::npos, "invalid success callback preserves missing id/path diagnosis");
            } else Check(capture.log.empty() && host.gets == 0 && !stage.Path().empty(), "successful create adds no error or extra get");
        }
        Check(host.releases == (scenario == 0 || scenario == 3 ? 0u : 1u), "release requested only for accepted owned stage id");
    }
    for (bool compress : {false, true}) {
        StageHost staging;
        staging.callback = PULSE_ERROR_STATE; staging.path.clear();
        TaskHost host;
        pulse::sdk::DispatchQueue dispatch(PulseDispatchV1{});
        Dialogs dialogs(PulseDialogsV1{});
        Jobs jobs(host.Api(), staging.Api(), dispatch, dialogs);
        CompressChoice choice; choice.name = L"archive.7z";
        const auto accepted = compress ? jobs.Compress({L"C:\\fixture\\input.txt"}, "C:\\fixture", choice)
            : jobs.Extract(L"C:\\fixture\\input.7z", "C:\\fixture", Jobs::ExtractMode::Into);
        Check(accepted == PULSE_OK && host.outcome == PULSE_TASK_FAILED, "real Jobs staging failure returns failed task");
        Check(host.capture.log.find(staging.error) != std::string::npos, "real Jobs caller reports host staging diagnostics");
        Check(host.capture.assurance.empty(), "real Jobs failure makes no unverified cleanup assurance");
        Check(host.capture.error == text::Get(T::ErrStage), "real Jobs failure keeps localized stage headline");
    }
    for (int scenario = 0; scenario < 3; ++scenario) {
        StageHost host;
        if (scenario == 0) host.commit_status = PULSE_ERROR_PERMISSION;
        if (scenario == 1) host.get_status = PULSE_ERROR_UNAVAILABLE;
        ReportCapture capture;
        const auto context = capture.Context();
        Reporter report(context);
        Stage stage(host.Api());
        Check(stage.Create("C:\\fixture"), "commit diagnostic fixture stage created");
        Check(stage.Commit([] { return false; }, report, 0, {}) == PULSE_TASK_FAILED, "commit failure outcome unchanged");
        if (scenario == 0) Check(capture.log.find("stage.commit") != std::string::npos && capture.log.find("status=8") != std::string::npos,
            "commit direct refusal records numeric status");
        if (scenario == 1) Check(capture.log.find("stage.get") != std::string::npos && capture.log.find("status=3") != std::string::npos,
            "commit get refusal records numeric status");
        if (scenario == 2) Check(capture.log.find(host.error) != std::string::npos && capture.log.find("id=41") != std::string::npos,
            "commit terminal snapshot error retained in Diagnostics");
    }
    return failed ? 1 : 0;
}

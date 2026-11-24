#include "admin_auto_attach.h"
#include "user_auto_attach.h"
#include "../common/runtime_log.h"
#include "../shell_bridge/dialog_application_identity.h"
#include <shlobj.h>
#include <shellapi.h>
#include <cstring>
#include <filesystem>
#include <cstdio>
#include <string_view>
namespace admin = pulse::dialog_launcher::admin_auto;
namespace user_auto = pulse::dialog_launcher::user_auto;
namespace runtime_log = pulse::diagnostics::runtime;
namespace {
HANDLE stop_event = nullptr;
BOOL WINAPI Stop(DWORD reason) {
    if (reason == CTRL_C_EVENT || reason == CTRL_BREAK_EVENT || reason == CTRL_CLOSE_EVENT ||
        reason == CTRL_LOGOFF_EVENT || reason == CTRL_SHUTDOWN_EVENT) {
        if (stop_event) SetEvent(stop_event);
        return TRUE;
    }
    return FALSE;
}
uint32_t Stage(std::string_view value) {
    constexpr std::string_view stages[]{
        "discovered", "identity_rejected", "architecture_rejected", "protection_query_failed",
        "protected_process_rejected", "excluded_image", "probe_eligible_no_attach", "target_access_failed",
        "target_reused", "existing_runtime_native", "binding_published", "load_queued", "target_exited",
        "ready", "ready_rejected", "ready_failed", "remote_thread_timeout_retained",
        "remote_thread_wait_failed", "attach_rejected", "attach_submitted", "loaded_module_missing",
        "attach_thread_failed", "candidate_skipped", "runtime_status", "runtime_status_unavailable"};
    for (uint32_t i = 0; i < std::size(stages); ++i) if (stages[i] == value) return i + 1;
    return 0;
}
}
int RunMain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    admin::Options options;
    bool user_adaptive = false;
    bool no_reg_check = false;
    DWORD parent_pid = 0;
    std::wstring custom_data_dir;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg == L"--installed-root" && i + 1 < argc && options.installed_root.empty()) options.installed_root = argv[++i];
        else if (arg == L"--companion") options.companion = true;
        else if (arg == L"--probe-only") options.probe_only = true;
        else if (arg == L"--user-adaptive") user_adaptive = true;
        else if (arg == L"--no-reg-check") no_reg_check = true;
        else if (arg == L"--data-dir" && i + 1 < argc && custom_data_dir.empty()) custom_data_dir = argv[++i];
        else if (arg == L"--parent-pid" && i + 1 < argc) {
            wchar_t* end = nullptr;
            const auto pid = wcstoul(argv[++i], &end, 10);
            if (!end || *end || !pid) return 2;
            parent_pid = pid;
        } else if (arg == L"--duration-ms" && i + 1 < argc) {
            wchar_t* end = nullptr;
            const auto duration = wcstoul(argv[++i], &end, 10);
            if (!end || *end || !duration || duration > 300000) return 2;
            options.duration_ms = duration;
        } else return 2;
    }
    if (options.installed_root.empty() || !std::filesystem::path(options.installed_root).is_absolute()) return 2;
    if (options.probe_only && !options.duration_ms) options.duration_ms = 3000;
    std::wstring data = custom_data_dir;
    if (data.empty()) {
        PWSTR local = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return 2;
        data = std::wstring(local) + L"\\Pulse";
        CoTaskMemFree(local);
    }
    runtime_log::Initialize(data, user_adaptive ? "dialog_user_auto" : "dialog_admin_auto");
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event) return 2;
    options.stop = stop_event;
    SetConsoleCtrlHandler(Stop, TRUE);
    DWORD result = 0;
    if (user_adaptive) {
        HANDLE parent = nullptr;
        if (parent_pid) {
            parent = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
            if (!parent) {
                SetConsoleCtrlHandler(Stop, FALSE);
                CloseHandle(stop_event);
                stop_event = nullptr;
                return static_cast<int>(ERROR_PROCESS_ABORTED);
            }
        }
        user_auto::Options uopts;
        uopts.installed_root = options.installed_root;
        uopts.data_dir = data;
        uopts.stop = stop_event;
        uopts.parent_process = parent;
        uopts.companion = options.companion;
        uopts.require_dialog_auto_enabled = !no_reg_check;
        uopts.duration_ms = options.duration_ms;
        uopts.log = [](const user_auto::Event& event) {
            const std::wstring stage(event.stage, event.stage + strlen(event.stage));
            runtime_log::TextEvent("user_dialog_auto", {{"stage", stage}, {"app_basename", event.app_basename}},
                {{"pid", event.pid}, {"error", event.error}, {"entries", event.entries}, {"match_mask", event.match_mask}},
                event.error ? runtime_log::Level::Warning : runtime_log::Level::Info);
            std::printf("user_auto stage=%s pid=%lu error=%lu entries=%lu mask=%u\n",
                event.stage, event.pid, event.error, event.entries, event.match_mask);
            std::fflush(stdout);
        };
        result = user_auto::Run(uopts);
        if (parent) CloseHandle(parent);
    } else {
        options.log = [](const admin::Event& event) {
            const auto creation = pulse::shell_bridge::application_identity::Filetime(event.created);
            const std::wstring stage(event.stage, event.stage + strlen(event.stage));
            if (event.pid && creation && event.app_instance_id && !event.app_basename.empty()) {
                runtime_log::TextEvent("admin_auto_target_result", {{"app_basename", event.app_basename}, {"attach_stage_name", stage}},
                    {{"target_pid", event.pid}, {"target_creation_time_filetime", creation}, {"app_instance_id", event.app_instance_id},
                     {"identity_revision", 2}, {"error", event.error}, {"ready", std::string_view(event.stage) == "ready" ? 1u : 0u},
                     {"attach_stage", Stage(event.stage)}, {"entry_coverage", event.entries},
                     {"patched_slots", event.patched_slots}, {"skipped_modules", event.skipped_modules}, {"foreign_slots", event.foreign_slots}},
                    event.error ? runtime_log::Level::Warning : runtime_log::Level::Info);
            } else {
                runtime_log::TextEvent("admin_auto_helper", {{"attach_stage_name", stage}},
                    {{"observed_pid", event.pid}, {"error", event.error}, {"attach_stage", Stage(event.stage)}},
                    event.error ? runtime_log::Level::Warning : runtime_log::Level::Info);
            }
            std::printf("admin_auto stage=%s pid=%lu error=%lu entries=%lu\n", event.stage, event.pid, event.error, event.entries);
            std::fflush(stdout);
        };
        result = admin::Run(options);
    }
    runtime_log::Event(user_adaptive ? "user_auto_exit" : "admin_auto_exit",
        {{"error", result}, {"probe_only", options.probe_only ? 1u : 0u}});
    runtime_log::Flush();
    runtime_log::Shutdown();
    SetConsoleCtrlHandler(Stop, FALSE);
    CloseHandle(stop_event);
    stop_event = nullptr;
    return static_cast<int>(result);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 2;
    int result = 2;
    try { result = RunMain(argc, argv); }
    catch (...) { result = static_cast<int>(ERROR_GEN_FAILURE); }
    LocalFree(argv);
    return result;
}

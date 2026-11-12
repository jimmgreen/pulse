#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <array>
#include "../common/dialog_auto_arch_status.h"
#include "../common/dialog_takeover_history.h"
#include "../common/dialog_restart_inventory.h"
#include "dialog_record_apps.h"
#include <functional>
#include <unordered_map>
namespace pulse::app {
bool DialogAutoPathMismatch(bool enabled, const std::wstring& registered, const std::wstring& bundle_root);
enum class DialogManagementOperation { PollHistory, Refresh, SelectBundle, SelectApplication, PrepareLaunch, Launch, PrepareAdminLaunch, LaunchAdmin, Disable, EnableAuto, DisableAuto, ExportDiagnostics };
struct DialogManagementConfiguration {
    std::wstring bundle_root, application;
    std::string manifest_digest;
};
struct DialogManagedSession {
    std::wstring directory, application;
    DWORD pid = 0, status_error = 0;
    bool process_alive = false, historical_handshake = false, local_disabled = false;
    HRESULT initialization = E_PENDING;
    uint32_t entry_coverage = 0;
    // No field claims current routing or successful dialog replacement.
};
struct DialogManagementResult {
    DialogManagementOperation operation = DialogManagementOperation::Refresh;
    uint64_t generation = 0;
    DWORD error = 0;
    bool auto_enabled = false, auto_conflict = false, auto_path_mismatch = false;
    DWORD auto_error = 0, auto_rollback_error = 0, bundle_error = 0;
    std::wstring auto_dll, diagnostics_path;
    std::array<dialog_auto::ArchStatus, 2> auto_arch;
    bool launcher_started = false;
    bool launch_preflight_checked = false, launch_scan_complete = false;
    uint32_t existing_target_count = 0, launch_query_errors = 0;
    bool bundle_verified = false, application_valid = false;
    uint32_t application_machine = 0;
    bool launch_bundle_verified = false;
    DWORD launch_bundle_error = 0;
    bool admin_launch_ready = false;
    DWORD admin_launch_error = 0;
    bool admin_auto_checked = false, admin_auto_running = false;
    DWORD admin_auto_error = 0;
    DialogManagementConfiguration configuration;
    std::vector<DialogManagedSession> sessions;
    dialog_history::History history;
    DWORD history_error = 0;
    dialog_restart::Inventory restart_inventory;
    // Program name and icon per record fingerprint (history and restart rows).
    std::unordered_map<uint64_t, DialogRecordApp> record_apps;
    bool restart_checked = false;
};
// UI owner thread API. All filesystem/process work runs on an owned worker.
// The wake carries no pointer. Drain only returns the current generation.
// An empty data root is resolved on the worker via GetPulseDataDir()/DialogManagement.
// Closing/cancelling never terminates an application or joins a blocked worker.
class DialogManagementController {
public:
    // Finds each record's program (name, icon) on the worker; the app passes
    // ResolveDialogRecordApps, tests leave it empty and records stay unresolved.
    using RecordAppResolver = std::function<std::unordered_map<uint64_t, DialogRecordApp>(
        const std::vector<DialogRecordAppQuery>&)>;
    DialogManagementController(HWND wake_host, UINT wake_message, std::wstring data_root,
                               RecordAppResolver resolve_record_apps = {});
    ~DialogManagementController();
    DialogManagementController(const DialogManagementController&) = delete;
    DialogManagementController& operator=(const DialogManagementController&) = delete;
    uint64_t SelectBundle(std::wstring absolute_directory, std::string expected_digest = {});
    uint64_t SelectApplication(std::wstring absolute_executable);
    uint64_t PrepareLaunch();
    uint64_t Launch();
    uint64_t PrepareAdminLaunch();
    uint64_t LaunchAdmin();
    uint64_t Refresh();
    uint64_t PollHistory(bool refresh_running_applications = false);
    uint64_t EnableAuto();
    uint64_t DisableAuto();
    uint64_t ExportDiagnostics();
    uint64_t Disable(std::wstring exact_session_directory);
    void Cancel() noexcept;
    std::vector<DialogManagementResult> Drain();
private:
    struct State;
    std::shared_ptr<State> state_;
    uint64_t Submit(DialogManagementOperation, std::wstring = {}, std::string = {});
};
}

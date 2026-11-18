#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace pulse::app {
bool DialogAutoPathMismatch(bool enabled, const std::wstring& registered, const std::wstring& bundle_root);
enum class DialogManagementOperation { Refresh, SelectBundle, SelectApplication, Launch, Disable, EnableAuto, DisableAuto, ExportDiagnostics };
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
    DWORD auto_error = 0, bundle_error = 0;
    std::wstring auto_dll, diagnostics_path;
    bool launcher_started = false;
    bool bundle_verified = false, application_valid = false;
    DialogManagementConfiguration configuration;
    std::vector<DialogManagedSession> sessions;
};
// UI owner thread API. All filesystem/process work runs on an owned worker.
// The wake carries no pointer. Drain only returns the current generation.
// An empty data root is resolved on the worker via GetPulseDataDir()/DialogManagement.
// Closing/cancelling never terminates an application or joins a blocked worker.
class DialogManagementController {
public:
    DialogManagementController(HWND wake_host, UINT wake_message, std::wstring data_root);
    ~DialogManagementController();
    DialogManagementController(const DialogManagementController&) = delete;
    DialogManagementController& operator=(const DialogManagementController&) = delete;
    uint64_t SelectBundle(std::wstring absolute_directory, std::string expected_digest = {});
    uint64_t SelectApplication(std::wstring absolute_executable);
    uint64_t Launch();
    uint64_t Refresh();
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

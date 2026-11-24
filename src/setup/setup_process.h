#pragma once
// Closing running Pulse before files are replaced. Port of the
// "closing running Pulse" section of installer/PulseSetup.iss.

#include <string>
#include <vector>
#include <windows.h>

namespace pulse::setup {

enum class CloseState { Done = 0, Failed = 1, Busy = 2, McpGateFailed = 3, ProcessesRemain = 4 };

// Retain the per-installation gate through file replacement/rollback. It is
// released automatically even when setup fails or is interrupted.
class McpUpdateGuard {
public:
    McpUpdateGuard() = default;
    ~McpUpdateGuard();
    McpUpdateGuard(const McpUpdateGuard&) = delete;
    McpUpdateGuard& operator=(const McpUpdateGuard&) = delete;
    bool Activate(const std::vector<std::wstring>& dirs);
private:
    std::vector<HANDLE> files_;
    bool active_ = false;
};

// Asks every Pulse (window class PulseMainWindow) whose exe lives in `dirs` to
// exit with the Pulse.PrepareUpdateShutdown.v1 handshake. Unattended updates
// never terminate a UI process that did not acknowledge; they keep waiting
// while it reports busy, exactly like the Inno script.
CloseState ClosePulseForUpdate(const std::vector<std::wstring>& dirs, bool unattended_update,
                               McpUpdateGuard* mcp_update = nullptr);

// Stops the PulseIndex service (when it runs from `dirs`) and ends
// Index, preview, Shell, AI and content/document hosts from `dirs`.
void StopPulseHosts(const std::vector<std::wstring>& dirs, bool touch_service);
bool WaitUntilImageGone(const wchar_t* image, const std::vector<std::wstring>& dirs, unsigned timeout_ms);

}  // namespace pulse::setup

#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace pulse::ops {
enum class ShellCollisionPolicy { System, Replace, KeepBoth };
struct ShellTransferResult {
    HRESULT hr = S_OK;
    std::wstring error;
    bool helper_unavailable = false; // Helper executable missing before submission.
    bool cancelled = false;
    bool mutated = false; // Includes completed descendants of a partial folder transfer.
    // True only for ordinary file transfers to previously absent destinations;
    // folder merges, replacement, reparse points and uncertain probes disable it.
    bool undo_safe = false;
    std::vector<std::wstring> sources;
    std::vector<std::wstring> destinations;
};
using ShellTransferProgress = std::function<void(const std::wstring&, float)>;

// Read-only, conservative probes. False does not guarantee descendant access.
bool TargetNeedsElevation(const std::wstring& destination,
                          DWORD required_access = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY);
bool NeedsShellTransfer(const std::vector<std::wstring>& sources,
                        const std::wstring& destination, bool move, const std::wstring& target_name = {});
// Must run on an initialized STA worker, never on the UI thread. Shell owns
// elevation/conflict UI; cancellation is observed at Shell progress callbacks.
ShellTransferResult TransferWithShell(const std::vector<std::wstring>& sources,
    const std::wstring& destination, bool move, HWND owner, std::atomic<bool>& cancel,
    ShellCollisionPolicy policy = ShellCollisionPolicy::System,
    ShellTransferProgress progress = {});
// Exposed for a noninteractive regression check of collision policy.
DWORD ShellTransferFlags(ShellCollisionPolicy policy);
} // namespace pulse::ops

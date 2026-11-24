#pragma once
#include <windows.h>
#include <memory>
#include <cstdint>
namespace pulse {
// Acquired/destroyed on the calling STA. Same-process owner transitions run on
// their owning thread with bounded waits; foreign transitions use an isolated lease helper;
// Alive may be polled by a monitor while this object remains alive.
enum class PickerOwnerFailureStage : uint32_t {
    None = 0, Window = 1, OpenProcess = 2, ProcessTime = 3, Session = 4,
    TokenIdentity = 5, IdentityMismatch = 6, ForeignEnabled = 7,
    SecurityAttributes = 8, MutexCreate = 9, MutexAcquire = 10,
    WindowIdentity = 11, WindowProperty = 12, ThreadTransition = 13, EventHook = 14,
    UserMismatch = 15, IntegrityMismatch = 16
};
struct PickerOwnerIdentityDiagnostics {
    bool identity_available = false;
    bool session_available = false;
    bool same_user = false;
    bool same_session = false;
    DWORD caller_integrity = 0;
    DWORD owner_integrity = 0;
    bool restore_attempted = false;
    bool restore_owned_disable = false;
    bool dialog_was_foreground = false;
    bool activation_attempted = false;
    bool activation_succeeded = false;
    bool restore_skipped_switched = false;
};
struct PickerRemoteOwnerValidation {
    DWORD error = ERROR_SUCCESS;
    PickerOwnerFailureStage stage = PickerOwnerFailureStage::None;
    DWORD caller_pid = 0, owner_pid = 0, owner_thread = 0;
    FILETIME caller_created{}, owner_created{};
    PickerOwnerIdentityDiagnostics identity;
};
// Read-only preflight: parent is the process object authenticated by the pipe,
// never a PID supplied in a command. Preflight permits a validated enabled owner;
// Show requires any owner to be disabled by the caller's modal guard.
PickerRemoteOwnerValidation ValidateRemotePickerOwner(HWND window, HANDLE authenticated_parent, bool showing) noexcept;
int RunPickerOwnerLease(int argc, wchar_t** argv) noexcept;
class PickerOwnerGuard;
namespace picker_owner_detail {
PickerOwnerFailureStage ForeignIdentityPolicy(bool same_user, bool same_session,
    DWORD caller_integrity, DWORD owner_integrity) noexcept;
void RestoreBeforeDialogHideForTest(pulse::PickerOwnerGuard& guard, HWND dialog) noexcept;
}
class PickerOwnerGuard {
public:
    explicit PickerOwnerGuard(HWND window);
    PickerOwnerGuard(HWND window, const wchar_t* lease_executable);
    // Internal remote host: the authenticated parent owns the modal guard, but
    // its nominated disabled window may belong to another validated process.
    struct CallerManagedRemoteOwner { DWORD authenticated_parent_pid; };
    PickerOwnerGuard(HWND window, CallerManagedRemoteOwner parent);
    ~PickerOwnerGuard();
    PickerOwnerGuard(const PickerOwnerGuard&) = delete;
    PickerOwnerGuard& operator=(const PickerOwnerGuard&) = delete;
    DWORD Error() const noexcept;
    PickerOwnerFailureStage FailureStage() const noexcept;
    PickerOwnerIdentityDiagnostics Diagnostics() const noexcept;
    bool Alive() const;
    HWND Window() const noexcept;
    // Restores only our disable transition; originally disabled owners stay disabled.
    // A foreground identity-bound picker returns activation to its identity-bound owner.
    // Activation delegates keyboard focus restoration to the owner's handlers.
    void RestoreBeforeDialogHide(HWND dialog) noexcept;
    // Out-of-process host handshake: waitable handle signaled by the foreground
    // picker host immediately before hiding its window so the caller's STA thread
    // re-enables and activates the owner BEFORE the popup disappears (preventing
    // Windows from raising the window underneath a disabled owner).
    HANDLE PreHideEvent() const noexcept;
    void CompletePreHideRestore() noexcept;
private:
    friend void picker_owner_detail::RestoreBeforeDialogHideForTest(PickerOwnerGuard& guard, HWND dialog) noexcept;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// For out-of-process picker hosts that only hold a placement_owner (no local guard).
void RestoreExternalPickerOwnerBeforeHide(HWND dialog, HWND owner) noexcept;
}

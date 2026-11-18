#pragma once
#include <windows.h>
#include <memory>
#include <cstdint>
namespace pulse {
// Acquired/destroyed on the calling STA. Same-process owner transitions run on
// their owning thread with bounded waits; foreign-process owners are observed only;
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
};
class PickerOwnerGuard {
public:
    explicit PickerOwnerGuard(HWND window);
    // Internal remote host: the authenticated parent owns the modal guard.
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
    // Restores only our disable transition; foreign/originally disabled owners stay untouched.
    // Activation delegates keyboard focus restoration to the owner's handlers.
    void RestoreBeforeDialogHide(HWND dialog) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

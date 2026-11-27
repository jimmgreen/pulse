#pragma once

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <roapi.h>
#include <cstdint>
#include <functional>

namespace pulse::shell_bridge::sink_hooks {

// Bitmask of active system-DLL sink hooks.
inline constexpr uint32_t kSinkComDlgClassObject      = 0x00000001u; // comdlg32!DllGetClassObject
inline constexpr uint32_t kSinkComDlgOpenSaveW        = 0x00000002u; // comdlg32!GetOpen/SaveFileNameW
inline constexpr uint32_t kSinkComDlgOpenSaveA        = 0x00000004u; // comdlg32!GetOpen/SaveFileNameA
inline constexpr uint32_t kSinkComDlgExtendedError    = 0x00000008u; // comdlg32!CommDlgExtendedError
inline constexpr uint32_t kSinkShell32Browse          = 0x00000010u; // shell32!SHBrowseForFolderW/A
inline constexpr uint32_t kSinkShell32DirectReveal    = 0x00000020u; // shell32!SHOpenFolderAndSelectItems + ShellExecute*
inline constexpr uint32_t kSinkCombaseWinRtFactory    = 0x00000040u; // combase!RoGetActivationFactory
inline constexpr uint32_t kSinkCombaseWinRtActivate   = 0x00000080u; // combase!RoActivateInstance
inline constexpr uint32_t kSinkKernel32CreateProcessW = 0x00000100u; // kernel32!CreateProcessW

inline constexpr uint32_t kSinkGroupComDlgAll =
    kSinkComDlgClassObject | kSinkComDlgOpenSaveW | kSinkComDlgOpenSaveA | kSinkComDlgExtendedError;
inline constexpr uint32_t kSinkGroupShell32All =
    kSinkShell32Browse | kSinkShell32DirectReveal;
inline constexpr uint32_t kSinkGroupCombaseAll =
    kSinkCombaseWinRtFactory | kSinkCombaseWinRtActivate;
inline constexpr uint32_t kSinkGroupAll =
    kSinkGroupComDlgAll | kSinkGroupShell32All | kSinkGroupCombaseAll | kSinkKernel32CreateProcessW;

using ComDlgGetClassObjectFn = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
using GetOpenFileNameWFn = BOOL (WINAPI*)(LPOPENFILENAMEW);
using GetSaveFileNameWFn = BOOL (WINAPI*)(LPOPENFILENAMEW);
using GetOpenFileNameAFn = BOOL (WINAPI*)(LPOPENFILENAMEA);
using GetSaveFileNameAFn = BOOL (WINAPI*)(LPOPENFILENAMEA);
using CommDlgExtendedErrorFn = DWORD (WINAPI*)();
using SHBrowseForFolderWFn = PIDLIST_ABSOLUTE (WINAPI*)(LPBROWSEINFOW);
using SHBrowseForFolderAFn = PIDLIST_ABSOLUTE (WINAPI*)(LPBROWSEINFOA);
using SHOpenFolderAndSelectItemsFn = HRESULT (WINAPI*)(PCIDLIST_ABSOLUTE, UINT, PCUITEMID_CHILD_ARRAY, DWORD);
using ShellExecuteWFn = HINSTANCE (WINAPI*)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
using ShellExecuteExWFn = BOOL (WINAPI*)(SHELLEXECUTEINFOW*);
using ShellExecuteAFn = HINSTANCE (WINAPI*)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT);
using ShellExecuteExAFn = BOOL (WINAPI*)(SHELLEXECUTEINFOA*);
using CreateProcessWFn = decltype(&::CreateProcessW);
using RoGetActivationFactoryFn = HRESULT (WINAPI*)(HSTRING, REFIID, void**);
using RoActivateInstanceFn = HRESULT (WINAPI*)(HSTRING, IInspectable**);

// Callbacks supplied by the active Core runtime. Any null callback falls straight
// through to the original system-DLL trampoline.
struct SinkCallbacks {
    // Called when comdlg32!DllGetClassObject is invoked for CLSID_FileOpenDialog or
    // CLSID_FileSaveDialog and the thread is not inside ScopedNativeComDlgBypass.
    // Call `original_get_class_object` (or construct inside ScopedNativeComDlgBypass)
    // to obtain the real native dialog without recursion.
    std::function<HRESULT(REFCLSID clsid, REFIID iid, void** out, ComDlgGetClassObjectFn original)> on_comdlg_class_object;

    std::function<BOOL(LPOPENFILENAMEW ofn, GetOpenFileNameWFn original)> on_get_open_w;
    std::function<BOOL(LPOPENFILENAMEW ofn, GetSaveFileNameWFn original)> on_get_save_w;
    std::function<BOOL(LPOPENFILENAMEA ofn, GetOpenFileNameAFn original)> on_get_open_a;
    std::function<BOOL(LPOPENFILENAMEA ofn, GetSaveFileNameAFn original)> on_get_save_a;
    std::function<DWORD(CommDlgExtendedErrorFn original)> on_commdlg_error;

    std::function<PIDLIST_ABSOLUTE(LPBROWSEINFOW bi, SHBrowseForFolderWFn original)> on_browse_w;
    std::function<PIDLIST_ABSOLUTE(LPBROWSEINFOA bi, SHBrowseForFolderAFn original)> on_browse_a;
    std::function<HRESULT(PCIDLIST_ABSOLUTE folder, UINT count, PCUITEMID_CHILD_ARRAY items, DWORD flags,
                          SHOpenFolderAndSelectItemsFn original)> on_reveal_items;
    std::function<HINSTANCE(HWND hwnd, LPCWSTR verb, LPCWSTR file, LPCWSTR params, LPCWSTR dir, INT show,
                            ShellExecuteWFn original)> on_shell_execute_w;
    std::function<BOOL(SHELLEXECUTEINFOW* info, ShellExecuteExWFn original)> on_shell_execute_ex_w;
    std::function<HINSTANCE(HWND hwnd, LPCSTR verb, LPCSTR file, LPCSTR params, LPCSTR dir, INT show,
                            ShellExecuteAFn original)> on_shell_execute_a;
    std::function<BOOL(SHELLEXECUTEINFOA* info, ShellExecuteExAFn original)> on_shell_execute_ex_a;

    std::function<BOOL(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
                       BOOL inherit, DWORD flags, LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW startup,
                       LPPROCESS_INFORMATION pi, CreateProcessWFn original)> on_create_process_w;

    std::function<HRESULT(HSTRING class_id, REFIID iid, void** factory,
                          RoGetActivationFactoryFn original)> on_ro_get_activation_factory;
    std::function<HRESULT(HSTRING class_id, IInspectable** instance,
                          RoActivateInstanceFn original)> on_ro_activate_instance;
};

struct InstallOptions {
    uint32_t requested_mask = kSinkGroupAll;
    // When true, watch late-loaded system DLLs via LdrRegisterDllNotification and
    // attach hooks automatically on a lock-free signal + watcher thread.
    bool watch_late_dll_loads = true;
    // When true and ComDlg hooks are requested, synchronously load System32\comdlg32.dll
    // if it is not yet mapped so the first CoCreateInstance(CLSID_FileOpen/SaveDialog)
    // or GetOpen/SaveFileName call cannot race ahead of the async loader watcher.
    bool preload_comdlg32 = false;
};

struct InstallStatus {
    DWORD error = ERROR_SUCCESS;
    uint32_t active_mask = 0;
    uint32_t hooked_count = 0;
    bool dll_watcher_active = false;
};

// Thread-local reentrancy guard: while alive on the calling thread, any call into
// comdlg32!DllGetClassObject passes directly to the original comdlg32 implementation.
class ScopedNativeComDlgBypass {
public:
    ScopedNativeComDlgBypass() noexcept;
    ~ScopedNativeComDlgBypass() noexcept;
    ScopedNativeComDlgBypass(const ScopedNativeComDlgBypass&) = delete;
    ScopedNativeComDlgBypass& operator=(const ScopedNativeComDlgBypass&) = delete;
    static bool Active() noexcept;
};

// Thread-local guard preventing RoActivateInstance -> internal RoGetActivationFactory
// double-wrapping on the same thread.
class ScopedWinRtActivationBypass {
public:
    ScopedWinRtActivationBypass() noexcept;
    ~ScopedWinRtActivationBypass() noexcept;
    ScopedWinRtActivationBypass(const ScopedWinRtActivationBypass&) = delete;
    ScopedWinRtActivationBypass& operator=(const ScopedWinRtActivationBypass&) = delete;
    static bool Active() noexcept;
};

// Installs sink inline hooks for all currently loaded target system DLLs and
// registers the LdrRegisterDllNotification watcher for late-loaded DLLs.
InstallStatus Install(SinkCallbacks callbacks, const InstallOptions& options = {}) noexcept;

// Synchronously attaches hooks for any target system DLLs loaded since Install().
// Safe to call outside loader lock (e.g. from tests or watcher wakeup).
InstallStatus FlushPendingHooks() noexcept;

// Returns current active sink bitmask and count.
InstallStatus QueryStatus() noexcept;

// Unregisters the loader notification watcher, stops the watcher thread, and
// detaches all currently active Detours sink hooks cleanly.
DWORD Uninstall() noexcept;

} // namespace pulse::shell_bridge::sink_hooks

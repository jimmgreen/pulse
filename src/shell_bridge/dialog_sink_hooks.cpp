#include "dialog_sink_hooks.h"

#include <detours.h>
#include <shobjidl.h>
#include <winternl.h>
#include <atomic>
#include <cwchar>
#include <memory>
#include <mutex>

namespace pulse::shell_bridge::sink_hooks {
namespace {

constexpr ULONG kLdrDllNotificationReasonLoaded = 1;
constexpr uint32_t kDllBitComDlg32 = 1u << 0;
constexpr uint32_t kDllBitShell32  = 1u << 1;
constexpr uint32_t kDllBitCombase  = 1u << 2;

struct LdrDllLoadedNotificationData {
    ULONG flags;
    PCUNICODE_STRING full_dll_name;
    PCUNICODE_STRING base_dll_name;
    PVOID dll_base;
    ULONG size_of_image;
};

using LdrDllNotificationFn = void (NTAPI*)(ULONG reason, const void* notification_data, void* context);
using LdrRegisterDllNotificationFn = LONG (NTAPI*)(ULONG flags, LdrDllNotificationFn callback, void* context, void** cookie);
using LdrUnregisterDllNotificationFn = LONG (NTAPI*)(void* cookie);

thread_local uint32_t tls_comdlg_bypass_depth = 0;
thread_local uint32_t tls_winrt_bypass_depth = 0;

SRWLOCK g_hook_lock = SRWLOCK_INIT;
SRWLOCK g_cb_lock = SRWLOCK_INIT;
std::shared_ptr<const SinkCallbacks> g_callbacks;

uint32_t g_requested_mask = 0;
std::atomic<uint32_t> g_active_mask{0};
std::atomic<uint32_t> g_hooked_count{0};
std::atomic<uint32_t> g_pending_dlls{0};
std::atomic<bool> g_installed{false};

HMODULE g_mod_comdlg32 = nullptr;
HMODULE g_mod_shell32 = nullptr;
HMODULE g_mod_combase = nullptr;

void* g_ldr_cookie = nullptr;
LdrUnregisterDllNotificationFn g_ldr_unregister = nullptr;
HANDLE g_wake_event = nullptr;
HANDLE g_stop_event = nullptr;
HANDLE g_watcher_thread = nullptr;

// Original trampoline pointers (populated before DetourAttach, cleared after DetourDetach).
ComDlgGetClassObjectFn g_orig_comdlg_class_object = nullptr;
GetOpenFileNameWFn g_orig_get_open_w = nullptr;
GetSaveFileNameWFn g_orig_get_save_w = nullptr;
GetOpenFileNameAFn g_orig_get_open_a = nullptr;
GetSaveFileNameAFn g_orig_get_save_a = nullptr;
CommDlgExtendedErrorFn g_orig_commdlg_error = nullptr;

SHBrowseForFolderWFn g_orig_browse_w = nullptr;
SHBrowseForFolderAFn g_orig_browse_a = nullptr;
SHOpenFolderAndSelectItemsFn g_orig_reveal_items = nullptr;
ShellExecuteWFn g_orig_shell_execute_w = nullptr;
ShellExecuteExWFn g_orig_shell_execute_ex_w = nullptr;
ShellExecuteAFn g_orig_shell_execute_a = nullptr;
ShellExecuteExAFn g_orig_shell_execute_ex_a = nullptr;

RoGetActivationFactoryFn g_orig_ro_factory = nullptr;
RoActivateInstanceFn g_orig_ro_activate = nullptr;

std::shared_ptr<const SinkCallbacks> LoadCallbacks() noexcept {
    AcquireSRWLockShared(&g_cb_lock);
    auto cb = g_callbacks;
    ReleaseSRWLockShared(&g_cb_lock);
    return cb;
}

bool EqualsIgnoreCaseAscii(const wchar_t* text, size_t len, const wchar_t* literal) noexcept {
    size_t i = 0;
    for (; i < len && literal[i] != L'\0'; ++i) {
        wchar_t a = text[i];
        wchar_t b = literal[i];
        if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z') b = static_cast<wchar_t>(b - L'A' + L'a');
        if (a != b) return false;
    }
    return i == len && literal[i] == L'\0';
}

// --- Detour hook entry points ---

HRESULT WINAPI HookComDlgGetClassObject(REFCLSID clsid, REFIID iid, void** out) {
    const auto orig = g_orig_comdlg_class_object;
    if (!orig) return CLASS_E_CLASSNOTAVAILABLE;
    if (tls_comdlg_bypass_depth != 0 ||
        (clsid != CLSID_FileOpenDialog && clsid != CLSID_FileSaveDialog)) {
        return orig(clsid, iid, out);
    }
    const auto cb = LoadCallbacks();
    if (!cb || !cb->on_comdlg_class_object) {
        return orig(clsid, iid, out);
    }
    try {
        return cb->on_comdlg_class_object(clsid, iid, out, orig);
    } catch (...) {
        ScopedNativeComDlgBypass bypass;
        return orig(clsid, iid, out);
    }
}

BOOL WINAPI HookGetOpenFileNameW(LPOPENFILENAMEW ofn) {
    const auto orig = g_orig_get_open_w;
    if (!orig) return FALSE;
    const auto cb = LoadCallbacks();
    if (!cb || !cb->on_get_open_w) return orig(ofn);
    try { return cb->on_get_open_w(ofn, orig); } catch (...) { return orig(ofn); }
}

BOOL WINAPI HookGetSaveFileNameW(LPOPENFILENAMEW ofn) {
    const auto orig = g_orig_get_save_w;
    if (!orig) return FALSE;
    const auto cb = LoadCallbacks();
    if (!cb || !cb->on_get_save_w) return orig(ofn);
    try { return cb->on_get_save_w(ofn, orig); } catch (...) { return orig(ofn); }
}

BOOL WINAPI HookGetOpenFileNameA(LPOPENFILENAMEA ofn) {
    const auto orig = g_orig_get_open_a;
    if (!orig) return FALSE;
    const auto cb = LoadCallbacks();
    if (!cb || !cb->on_get_open_a) return orig(ofn);
    try { return cb->on_get_open_a(ofn, orig); } catch (...) { return orig(ofn); }
}

BOOL WINAPI HookGetSaveFileNameA(LPOPENFILENAMEA ofn) {
    const auto orig = g_orig_get_save_a;
    if (!orig) return FALSE;
    const auto cb = LoadCallbacks();
    if (!cb || !cb->on_get_save_a) return orig(ofn);
    try { return cb->on_get_save_a(ofn, orig); } catch (...) { return orig(ofn); }
}

DWORD WINAPI HookCommDlgExtendedError() {
    const auto orig = g_orig_commdlg_error;
    if (!orig) return 0;
    const auto cb = LoadCallbacks();
    if (!cb || !cb->on_commdlg_error) return orig();
    try { return cb->on_commdlg_error(orig); } catch (...) { return orig(); }
}

PIDLIST_ABSOLUTE WINAPI HookSHBrowseForFolderW(LPBROWSEINFOW bi) {
    const auto orig = g_orig_browse_w;
    if (!orig) return nullptr;
    const auto cb = LoadCallbacks();
    if (!cb || !cb->on_browse_w) return orig(bi);
    try { return cb->on_browse_w(bi, orig); } catch (...) { return orig(bi); }
}

PIDLIST_ABSOLUTE WINAPI HookSHBrowseForFolderA(LPBROWSEINFOA bi) {
    const auto orig = g_orig_browse_a;
    if (!orig) return nullptr;
    const auto cb = LoadCallbacks();
    if (!cb || !cb->on_browse_a) return orig(bi);
    try { return cb->on_browse_a(bi, orig); } catch (...) { return orig(bi); }
}

HRESULT WINAPI HookSHOpenFolderAndSelectItems(PCIDLIST_ABSOLUTE folder, UINT count, PCUITEMID_CHILD_ARRAY items, DWORD flags) {
    const auto orig = g_orig_reveal_items;
    if (!orig) return E_FAIL;
    const auto cb = LoadCallbacks();
    if (!cb || !cb->on_reveal_items) return orig(folder, count, items, flags);
    try { return cb->on_reveal_items(folder, count, items, flags, orig); } catch (...) { return orig(folder, count, items, flags); }
}

HINSTANCE WINAPI HookShellExecuteW(HWND hwnd, LPCWSTR verb, LPCWSTR file, LPCWSTR params, LPCWSTR dir, INT show) {
    const auto orig = g_orig_shell_execute_w;
    if (!orig) return nullptr;
    const auto cb = LoadCallbacks();
    if (!cb || !cb->on_shell_execute_w) return orig(hwnd, verb, file, params, dir, show);
    try { return cb->on_shell_execute_w(hwnd, verb, file, params, dir, show, orig); } catch (...) { return orig(hwnd, verb, file, params, dir, show); }
}

BOOL WINAPI HookShellExecuteExW(SHELLEXECUTEINFOW* info) {
    const auto orig = g_orig_shell_execute_ex_w;
    if (!orig) return FALSE;
    const auto cb = LoadCallbacks();
    if (!cb || !cb->on_shell_execute_ex_w) return orig(info);
    try { return cb->on_shell_execute_ex_w(info, orig); } catch (...) { return orig(info); }
}

HINSTANCE WINAPI HookShellExecuteA(HWND hwnd, LPCSTR verb, LPCSTR file, LPCSTR params, LPCSTR dir, INT show) {
    const auto orig = g_orig_shell_execute_a;
    if (!orig) return nullptr;
    const auto cb = LoadCallbacks();
    if (!cb || !cb->on_shell_execute_a) return orig(hwnd, verb, file, params, dir, show);
    try { return cb->on_shell_execute_a(hwnd, verb, file, params, dir, show, orig); } catch (...) { return orig(hwnd, verb, file, params, dir, show); }
}

BOOL WINAPI HookShellExecuteExA(SHELLEXECUTEINFOA* info) {
    const auto orig = g_orig_shell_execute_ex_a;
    if (!orig) return FALSE;
    const auto cb = LoadCallbacks();
    if (!cb || !cb->on_shell_execute_ex_a) return orig(info);
    try { return cb->on_shell_execute_ex_a(info, orig); } catch (...) { return orig(info); }
}

HRESULT WINAPI HookRoGetActivationFactory(HSTRING class_id, REFIID iid, void** factory) {
    const auto orig = g_orig_ro_factory;
    if (!orig) return E_FAIL;
    if (tls_winrt_bypass_depth != 0) return orig(class_id, iid, factory);
    const auto cb = LoadCallbacks();
    if (!cb || !cb->on_ro_get_activation_factory) return orig(class_id, iid, factory);
    try { return cb->on_ro_get_activation_factory(class_id, iid, factory, orig); } catch (...) { return orig(class_id, iid, factory); }
}

HRESULT WINAPI HookRoActivateInstance(HSTRING class_id, IInspectable** instance) {
    const auto orig = g_orig_ro_activate;
    if (!orig) return E_FAIL;
    if (tls_winrt_bypass_depth != 0) return orig(class_id, instance);
    const auto cb = LoadCallbacks();
    if (!cb || !cb->on_ro_activate_instance) return orig(class_id, instance);
    try {
        // Suppress HookRoGetActivationFactory if combase!RoActivateInstance calls it internally.
        ScopedWinRtActivationBypass bypass;
        return cb->on_ro_activate_instance(class_id, instance, orig);
    } catch (...) {
        ScopedWinRtActivationBypass bypass;
        return orig(class_id, instance);
    }
}

// Helper to retain a module reference outside of g_hook_lock if currently loaded.
HMODULE RetainLoadedModule(const wchar_t* name) noexcept {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(0, name, &mod)) return nullptr;
    return mod;
}

// Loads a system DLL strictly from System32 outside g_hook_lock and pins it in
// the host process so a later Uninstall() (e.g. Core hot-reload or disable) can
// cleanly detach all Detours hooks without ever unmapping comdlg32.dll out from
// under live IFileDialog / IClassFactory instances or combase's class cache.
HMODULE LoadSystemModule(const wchar_t* dll_name) noexcept {
    wchar_t sysdir[MAX_PATH]{};
    const UINT len = GetSystemDirectoryW(sysdir, ARRAYSIZE(sysdir));
    if (!len || len >= ARRAYSIZE(sysdir)) return nullptr;
    wchar_t full_path[MAX_PATH + 64]{};
    if (swprintf_s(full_path, ARRAYSIZE(full_path), L"%s\\%s", sysdir, dll_name) <= 0) return nullptr;
    HMODULE mod = LoadLibraryExW(full_path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (mod) {
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(mod), &pinned);
    }
    return mod;
}

template <typename Fn>
bool TryAttachExport(HMODULE mod, const char* name, Fn& orig_slot, Fn hook_fn, uint32_t& count) noexcept {
    if (orig_slot != nullptr) return true;
    auto* proc = reinterpret_cast<Fn>(GetProcAddress(mod, name));
    if (!proc) return false;
    orig_slot = proc;
    if (DetourAttach(reinterpret_cast<PVOID*>(&orig_slot), reinterpret_cast<PVOID>(hook_fn)) != NO_ERROR) {
        orig_slot = nullptr;
        return false;
    }
    ++count;
    return true;
}

template <typename Fn>
void TryDetachSlot(Fn& orig_slot, Fn hook_fn) noexcept {
    if (!orig_slot) return;
    if (DetourDetach(reinterpret_cast<PVOID*>(&orig_slot), reinterpret_cast<PVOID>(hook_fn)) == NO_ERROR) {
        orig_slot = nullptr;
    }
}

// Attaches any missing sink hooks for currently loaded target system DLLs.
// Acquires module handles BEFORE locking g_hook_lock to preserve lock ordering.
InstallStatus AttachLoadedSinksLockedOuter(bool preload_comdlg32 = false) noexcept {
    HMODULE cand_comdlg = nullptr;
    HMODULE cand_shell = nullptr;
    HMODULE cand_combase = nullptr;

    // Acquire module references outside g_hook_lock so loader lock is never taken under g_hook_lock.
    if ((g_requested_mask & kSinkGroupComDlgAll) && !g_mod_comdlg32) {
        cand_comdlg = RetainLoadedModule(L"comdlg32.dll");
        if (!cand_comdlg && preload_comdlg32) {
            cand_comdlg = LoadSystemModule(L"comdlg32.dll");
        }
    }
    if ((g_requested_mask & kSinkGroupShell32All) && !g_mod_shell32) {
        cand_shell = RetainLoadedModule(L"shell32.dll");
    }
    if ((g_requested_mask & kSinkGroupCombaseAll) && !g_mod_combase) {
        cand_combase = RetainLoadedModule(L"combase.dll");
    }

    InstallStatus status{};
    AcquireSRWLockExclusive(&g_hook_lock);
    if (!g_installed.load(std::memory_order_acquire)) {
        ReleaseSRWLockExclusive(&g_hook_lock);
        if (cand_comdlg) FreeLibrary(cand_comdlg);
        if (cand_shell) FreeLibrary(cand_shell);
        if (cand_combase) FreeLibrary(cand_combase);
        return status;
    }

    // Transfer ownership of newly retained module handles if still unset.
    auto adopt = [](HMODULE& slot, HMODULE& cand) {
        if (!slot && cand) { slot = cand; cand = nullptr; }
    };
    adopt(g_mod_comdlg32, cand_comdlg);
    adopt(g_mod_shell32, cand_shell);
    adopt(g_mod_combase, cand_combase);

    const uint32_t want = g_requested_mask;
    uint32_t active = g_active_mask.load(std::memory_order_acquire);
    uint32_t added_bits = 0;
    uint32_t added_count = 0;

    const bool need_comdlg = g_mod_comdlg32 && ((want & kSinkGroupComDlgAll) & ~active);
    const bool need_shell  = g_mod_shell32 && ((want & kSinkGroupShell32All) & ~active);
    const bool need_combase = g_mod_combase && ((want & kSinkGroupCombaseAll) & ~active);

    if (need_comdlg || need_shell || need_combase) {
        LONG tx = DetourTransactionBegin();
        if (tx == NO_ERROR) {
            tx = DetourUpdateThread(GetCurrentThread());
        }
        if (tx == NO_ERROR) {
            if (need_comdlg) {
                if ((want & kSinkComDlgClassObject) && !(active & kSinkComDlgClassObject)) {
                    if (TryAttachExport(g_mod_comdlg32, "DllGetClassObject", g_orig_comdlg_class_object, &HookComDlgGetClassObject, added_count))
                        added_bits |= kSinkComDlgClassObject;
                }
                if ((want & kSinkComDlgOpenSaveW) && !(active & kSinkComDlgOpenSaveW)) {
                    const bool ok_open = TryAttachExport(g_mod_comdlg32, "GetOpenFileNameW", g_orig_get_open_w, &HookGetOpenFileNameW, added_count);
                    const bool ok_save = TryAttachExport(g_mod_comdlg32, "GetSaveFileNameW", g_orig_get_save_w, &HookGetSaveFileNameW, added_count);
                    if (ok_open || ok_save) added_bits |= kSinkComDlgOpenSaveW;
                }
                if ((want & kSinkComDlgOpenSaveA) && !(active & kSinkComDlgOpenSaveA)) {
                    const bool ok_open = TryAttachExport(g_mod_comdlg32, "GetOpenFileNameA", g_orig_get_open_a, &HookGetOpenFileNameA, added_count);
                    const bool ok_save = TryAttachExport(g_mod_comdlg32, "GetSaveFileNameA", g_orig_get_save_a, &HookGetSaveFileNameA, added_count);
                    if (ok_open || ok_save) added_bits |= kSinkComDlgOpenSaveA;
                }
                if ((want & kSinkComDlgExtendedError) && !(active & kSinkComDlgExtendedError)) {
                    if (TryAttachExport(g_mod_comdlg32, "CommDlgExtendedError", g_orig_commdlg_error, &HookCommDlgExtendedError, added_count))
                        added_bits |= kSinkComDlgExtendedError;
                }
            }
            if (need_shell) {
                if ((want & kSinkShell32Browse) && !(active & kSinkShell32Browse)) {
                    const bool ok_w = TryAttachExport(g_mod_shell32, "SHBrowseForFolderW", g_orig_browse_w, &HookSHBrowseForFolderW, added_count);
                    const bool ok_a = TryAttachExport(g_mod_shell32, "SHBrowseForFolderA", g_orig_browse_a, &HookSHBrowseForFolderA, added_count);
                    if (ok_w || ok_a) added_bits |= kSinkShell32Browse;
                }
                if ((want & kSinkShell32DirectReveal) && !(active & kSinkShell32DirectReveal)) {
                    const bool ok_sel = TryAttachExport(g_mod_shell32, "SHOpenFolderAndSelectItems", g_orig_reveal_items, &HookSHOpenFolderAndSelectItems, added_count);
                    const bool ok_sw  = TryAttachExport(g_mod_shell32, "ShellExecuteW", g_orig_shell_execute_w, &HookShellExecuteW, added_count);
                    const bool ok_exw = TryAttachExport(g_mod_shell32, "ShellExecuteExW", g_orig_shell_execute_ex_w, &HookShellExecuteExW, added_count);
                    const bool ok_sa  = TryAttachExport(g_mod_shell32, "ShellExecuteA", g_orig_shell_execute_a, &HookShellExecuteA, added_count);
                    const bool ok_exa = TryAttachExport(g_mod_shell32, "ShellExecuteExA", g_orig_shell_execute_ex_a, &HookShellExecuteExA, added_count);
                    if (ok_sel || ok_sw || ok_exw || ok_sa || ok_exa) added_bits |= kSinkShell32DirectReveal;
                }
            }
            if (need_combase) {
                if ((want & kSinkCombaseWinRtFactory) && !(active & kSinkCombaseWinRtFactory)) {
                    if (TryAttachExport(g_mod_combase, "RoGetActivationFactory", g_orig_ro_factory, &HookRoGetActivationFactory, added_count))
                        added_bits |= kSinkCombaseWinRtFactory;
                }
                if ((want & kSinkCombaseWinRtActivate) && !(active & kSinkCombaseWinRtActivate)) {
                    if (TryAttachExport(g_mod_combase, "RoActivateInstance", g_orig_ro_activate, &HookRoActivateInstance, added_count))
                        added_bits |= kSinkCombaseWinRtActivate;
                }
            }

            if (added_count > 0) {
                tx = DetourTransactionCommit();
                if (tx == NO_ERROR) {
                    active |= added_bits;
                    g_active_mask.store(active, std::memory_order_release);
                    g_hooked_count.fetch_add(added_count, std::memory_order_acq_rel);
                } else {
                    // Commit failed: roll back any newly populated orig pointers not active before.
                    if (added_bits & kSinkComDlgClassObject) g_orig_comdlg_class_object = nullptr;
                    if (added_bits & kSinkComDlgOpenSaveW) { g_orig_get_open_w = nullptr; g_orig_get_save_w = nullptr; }
                    if (added_bits & kSinkComDlgOpenSaveA) { g_orig_get_open_a = nullptr; g_orig_get_save_a = nullptr; }
                    if (added_bits & kSinkComDlgExtendedError) g_orig_commdlg_error = nullptr;
                    if (added_bits & kSinkShell32Browse) { g_orig_browse_w = nullptr; g_orig_browse_a = nullptr; }
                    if (added_bits & kSinkShell32DirectReveal) {
                        g_orig_reveal_items = nullptr;
                        g_orig_shell_execute_w = nullptr;
                        g_orig_shell_execute_ex_w = nullptr;
                        g_orig_shell_execute_a = nullptr;
                        g_orig_shell_execute_ex_a = nullptr;
                    }
                    if (added_bits & kSinkCombaseWinRtFactory) g_orig_ro_factory = nullptr;
                    if (added_bits & kSinkCombaseWinRtActivate) g_orig_ro_activate = nullptr;
                    status.error = static_cast<DWORD>(tx);
                }
            } else {
                DetourTransactionAbort();
            }
        } else {
            status.error = static_cast<DWORD>(tx);
        }
    }

    status.active_mask = g_active_mask.load(std::memory_order_acquire);
    status.hooked_count = g_hooked_count.load(std::memory_order_acquire);
    status.dll_watcher_active = (g_ldr_cookie != nullptr);
    ReleaseSRWLockExclusive(&g_hook_lock);

    // Release redundant module references outside the lock.
    if (cand_comdlg) FreeLibrary(cand_comdlg);
    if (cand_shell) FreeLibrary(cand_shell);
    if (cand_combase) FreeLibrary(cand_combase);
    return status;
}

// Loader-lock safe: strictly no heap allocations, no LoadLibrary, no user-mode mutexes.
void NTAPI OnLdrDllNotification(ULONG reason, const void* raw_data, void*) noexcept {
    if (reason != kLdrDllNotificationReasonLoaded || !raw_data) return;
    const auto* loaded = static_cast<const LdrDllLoadedNotificationData*>(raw_data);
    if (!loaded->base_dll_name || !loaded->base_dll_name->Buffer || loaded->base_dll_name->Length == 0) return;

    const wchar_t* name = loaded->base_dll_name->Buffer;
    const size_t chars = loaded->base_dll_name->Length / sizeof(wchar_t);
    uint32_t bit = 0;
    if (EqualsIgnoreCaseAscii(name, chars, L"comdlg32.dll")) bit = kDllBitComDlg32;
    else if (EqualsIgnoreCaseAscii(name, chars, L"shell32.dll")) bit = kDllBitShell32;
    else if (EqualsIgnoreCaseAscii(name, chars, L"combase.dll")) bit = kDllBitCombase;
    if (!bit) return;

    g_pending_dlls.fetch_or(bit, std::memory_order_release);
    if (g_wake_event) SetEvent(g_wake_event);
}

DWORD WINAPI WatcherThreadProc(void*) noexcept {
    HANDLE handles[2]{g_stop_event, g_wake_event};
    for (;;) {
        const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (wait != WAIT_OBJECT_0 + 1) break;
        if (!g_installed.load(std::memory_order_acquire)) break;
        if (g_pending_dlls.exchange(0, std::memory_order_acq_rel) != 0) {
            AttachLoadedSinksLockedOuter();
        }
    }
    return 0;
}

} // namespace

ScopedNativeComDlgBypass::ScopedNativeComDlgBypass() noexcept { ++tls_comdlg_bypass_depth; }
ScopedNativeComDlgBypass::~ScopedNativeComDlgBypass() noexcept { if (tls_comdlg_bypass_depth > 0) --tls_comdlg_bypass_depth; }
bool ScopedNativeComDlgBypass::Active() noexcept { return tls_comdlg_bypass_depth != 0; }

ScopedWinRtActivationBypass::ScopedWinRtActivationBypass() noexcept { ++tls_winrt_bypass_depth; }
ScopedWinRtActivationBypass::~ScopedWinRtActivationBypass() noexcept { if (tls_winrt_bypass_depth > 0) --tls_winrt_bypass_depth; }
bool ScopedWinRtActivationBypass::Active() noexcept { return tls_winrt_bypass_depth != 0; }

InstallStatus Install(SinkCallbacks callbacks, const InstallOptions& options) noexcept {
    InstallStatus status{};
    try {
        auto cb_ptr = std::make_shared<const SinkCallbacks>(std::move(callbacks));
        AcquireSRWLockExclusive(&g_cb_lock);
        g_callbacks = std::move(cb_ptr);
        ReleaseSRWLockExclusive(&g_cb_lock);
    } catch (...) {
        status.error = ERROR_NOT_ENOUGH_MEMORY;
        return status;
    }

    bool start_watcher = false;
    AcquireSRWLockExclusive(&g_hook_lock);
    g_requested_mask |= options.requested_mask;
    g_installed.store(true, std::memory_order_release);
    if (options.watch_late_dll_loads && !g_ldr_cookie) {
        start_watcher = true;
    }
    ReleaseSRWLockExclusive(&g_hook_lock);

    if (start_watcher) {
        HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto reg_fn = ntdll ? reinterpret_cast<LdrRegisterDllNotificationFn>(
                                  GetProcAddress(ntdll, "LdrRegisterDllNotification"))
                            : nullptr;
        auto unreg_fn = ntdll ? reinterpret_cast<LdrUnregisterDllNotificationFn>(
                                    GetProcAddress(ntdll, "LdrUnregisterDllNotification"))
                              : nullptr;

        if (wake && stop && reg_fn && unreg_fn) {
            AcquireSRWLockExclusive(&g_hook_lock);
            if (!g_ldr_cookie && g_installed.load(std::memory_order_acquire)) {
                g_wake_event = wake;
                g_stop_event = stop;
                g_ldr_unregister = unreg_fn;
                wake = nullptr;
                stop = nullptr;
                g_watcher_thread = CreateThread(nullptr, 0, WatcherThreadProc, nullptr, 0, nullptr);
                if (g_watcher_thread) {
                    void* cookie = nullptr;
                    if (reg_fn(0, &OnLdrDllNotification, nullptr, &cookie) == 0 && cookie) {
                        g_ldr_cookie = cookie;
                    } else {
                        SetEvent(g_stop_event);
                    }
                }
            }
            ReleaseSRWLockExclusive(&g_hook_lock);
        }
        if (wake) CloseHandle(wake);
        if (stop) CloseHandle(stop);
    }

    return AttachLoadedSinksLockedOuter(options.preload_comdlg32);
}

InstallStatus FlushPendingHooks() noexcept {
    g_pending_dlls.exchange(0, std::memory_order_acq_rel);
    return AttachLoadedSinksLockedOuter(false);
}

InstallStatus QueryStatus() noexcept {
    InstallStatus status{};
    status.active_mask = g_active_mask.load(std::memory_order_acquire);
    status.hooked_count = g_hooked_count.load(std::memory_order_acquire);
    AcquireSRWLockShared(&g_hook_lock);
    status.dll_watcher_active = (g_ldr_cookie != nullptr);
    ReleaseSRWLockShared(&g_hook_lock);
    return status;
}

DWORD Uninstall() noexcept {
    // 1. Immediately disable callbacks so any concurrent callers pass through cleanly.
    AcquireSRWLockExclusive(&g_cb_lock);
    g_callbacks.reset();
    ReleaseSRWLockExclusive(&g_cb_lock);

    void* cookie = nullptr;
    LdrUnregisterDllNotificationFn unreg = nullptr;
    HANDLE stop = nullptr;
    HANDLE wake = nullptr;
    HANDLE thread = nullptr;

    AcquireSRWLockExclusive(&g_hook_lock);
    g_installed.store(false, std::memory_order_release);
    cookie = g_ldr_cookie;
    g_ldr_cookie = nullptr;
    unreg = g_ldr_unregister;
    g_ldr_unregister = nullptr;
    stop = g_stop_event;
    wake = g_wake_event;
    thread = g_watcher_thread;
    g_watcher_thread = nullptr;
    ReleaseSRWLockExclusive(&g_hook_lock);

    // 2. Unregister loader callback & join watcher thread outside g_hook_lock.
    if (cookie && unreg) {
        unreg(cookie);
    }
    if (stop) SetEvent(stop);
    if (thread) {
        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
    }

    // 3. Detach all active Detours trampolines.
    HMODULE mod_comdlg = nullptr;
    HMODULE mod_shell = nullptr;
    HMODULE mod_combase = nullptr;
    DWORD result = ERROR_SUCCESS;

    AcquireSRWLockExclusive(&g_hook_lock);
    if (g_wake_event == wake) g_wake_event = nullptr;
    if (g_stop_event == stop) g_stop_event = nullptr;

    if (g_active_mask.load(std::memory_order_acquire) != 0) {
        LONG tx = DetourTransactionBegin();
        if (tx == NO_ERROR) tx = DetourUpdateThread(GetCurrentThread());
        if (tx == NO_ERROR) {
            TryDetachSlot(g_orig_comdlg_class_object, &HookComDlgGetClassObject);
            TryDetachSlot(g_orig_get_open_w, &HookGetOpenFileNameW);
            TryDetachSlot(g_orig_get_save_w, &HookGetSaveFileNameW);
            TryDetachSlot(g_orig_get_open_a, &HookGetOpenFileNameA);
            TryDetachSlot(g_orig_get_save_a, &HookGetSaveFileNameA);
            TryDetachSlot(g_orig_commdlg_error, &HookCommDlgExtendedError);

            TryDetachSlot(g_orig_browse_w, &HookSHBrowseForFolderW);
            TryDetachSlot(g_orig_browse_a, &HookSHBrowseForFolderA);
            TryDetachSlot(g_orig_reveal_items, &HookSHOpenFolderAndSelectItems);
            TryDetachSlot(g_orig_shell_execute_w, &HookShellExecuteW);
            TryDetachSlot(g_orig_shell_execute_ex_w, &HookShellExecuteExW);
            TryDetachSlot(g_orig_shell_execute_a, &HookShellExecuteA);
            TryDetachSlot(g_orig_shell_execute_ex_a, &HookShellExecuteExA);

            TryDetachSlot(g_orig_ro_factory, &HookRoGetActivationFactory);
            TryDetachSlot(g_orig_ro_activate, &HookRoActivateInstance);

            tx = DetourTransactionCommit();
            if (tx == NO_ERROR) {
                g_active_mask.store(0, std::memory_order_release);
                g_hooked_count.store(0, std::memory_order_release);
            } else {
                result = static_cast<DWORD>(tx);
            }
        } else {
            result = static_cast<DWORD>(tx);
        }
    }
    g_requested_mask = 0;
    g_pending_dlls.store(0, std::memory_order_release);

    if (result == ERROR_SUCCESS) {
        mod_comdlg = g_mod_comdlg32; g_mod_comdlg32 = nullptr;
        mod_shell = g_mod_shell32;   g_mod_shell32 = nullptr;
        mod_combase = g_mod_combase; g_mod_combase = nullptr;
    }
    ReleaseSRWLockExclusive(&g_hook_lock);

    if (wake) CloseHandle(wake);
    if (stop) CloseHandle(stop);
    if (mod_comdlg) FreeLibrary(mod_comdlg);
    if (mod_shell) FreeLibrary(mod_shell);
    if (mod_combase) FreeLibrary(mod_combase);
    return result;
}

} // namespace pulse::shell_bridge::sink_hooks

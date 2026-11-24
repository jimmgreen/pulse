#include "dialog_core_abi.h"
#include "../common/json_value.h"

#include <windows.h>
#include <shobjidl.h>
#include <atomic>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

using GetClassObjectFn = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
using CanUnloadNowFn = HRESULT (WINAPI*)();

constexpr wchar_t kActivePointerFile[] = L"active-bridge.json";
#if defined(_WIN64)
constexpr wchar_t kCoreDllName[] = L"PulseDialogCore64.dll";
constexpr const char* kCoreDirKey = "core_dir_x64";
#else
constexpr wchar_t kCoreDllName[] = L"PulseDialogCore32.dll";
constexpr const char* kCoreDirKey = "core_dir_x86";
#endif
constexpr ULONGLONG kReloadCheckIntervalMs = 250;

struct CoreSlot;
void DeferRetiredSlot(CoreSlot* slot) noexcept;

struct CoreSlot {
    std::atomic<LONG> refs{1};
    HMODULE module = nullptr;
    PulseDialogCoreV1 table{};
    uint64_t generation = 0;
    std::wstring core_dir;
    std::wstring host_exe;
    std::wstring bridge_root;
    bool initialized = false;

    void AddRef() noexcept {
        refs.fetch_add(1, std::memory_order_acq_rel);
    }
    bool CanFreeModule() const noexcept {
        if (!module) return true;
        auto fn = reinterpret_cast<CanUnloadNowFn>(GetProcAddress(module, "DllCanUnloadNow"));
        return !fn || fn() == S_OK;
    }
    void Release() noexcept {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (initialized && table.shutdown) {
                initialized = false;
                try { table.shutdown(); } catch (...) {}
            }
            if (module && !CanFreeModule()) {
                DeferRetiredSlot(this);
                return;
            }
            if (module) {
                FreeLibrary(module);
                module = nullptr;
            }
            delete this;
        }
    }
};

struct CoreGuard {
    CoreSlot* slot = nullptr;
    ~CoreGuard() { if (slot) slot->Release(); }
    CoreSlot* operator->() const noexcept { return slot; }
    explicit operator bool() const noexcept { return slot != nullptr; }
};

HMODULE g_self_module = nullptr;
std::atomic<bool> g_pinned{false};
SRWLOCK g_slot_lock = SRWLOCK_INIT;
CoreSlot* g_active_slot = nullptr;
std::vector<CoreSlot*> g_retired_slots;
std::wstring g_bridge_root_override;
ULONGLONG g_last_check_tick = 0;
DWORD g_last_error = ERROR_SUCCESS;
uint32_t g_last_activate_flags = 0;

void DeferRetiredSlot(CoreSlot* slot) noexcept {
    if (!slot) return;
    try {
        AcquireSRWLockExclusive(&g_slot_lock);
        g_retired_slots.push_back(slot);
        ReleaseSRWLockExclusive(&g_slot_lock);
    } catch (...) {
        // Keep the module loaded rather than freeing underneath a live COM vtable.
    }
}

void ReapRetiredSlots() noexcept {
    std::vector<CoreSlot*> ready;
    AcquireSRWLockExclusive(&g_slot_lock);
    for (auto it = g_retired_slots.begin(); it != g_retired_slots.end();) {
        if ((*it)->CanFreeModule()) {
            ready.push_back(*it);
            it = g_retired_slots.erase(it);
        } else {
            ++it;
        }
    }
    ReleaseSRWLockExclusive(&g_slot_lock);
    for (CoreSlot* slot : ready) {
        if (slot->module) {
            FreeLibrary(slot->module);
            slot->module = nullptr;
        }
        delete slot;
    }
}

std::wstring Utf8ToWide(std::string_view input) {
    if (input.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), out.data(), needed);
    return out;
}

std::wstring StripTrailingSlash(std::wstring value) {
    while (value.size() > 3 && (value.back() == L'\\' || value.back() == L'/')) {
        value.pop_back();
    }
    return value;
}

std::wstring SelfDirectory() {
    if (!g_bridge_root_override.empty()) return g_bridge_root_override;
    wchar_t buf[32768]{};
    const DWORD len = GetModuleFileNameW(g_self_module, buf, ARRAYSIZE(buf));
    if (!len || len >= ARRAYSIZE(buf)) return {};
    std::wstring path(buf, len);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return StripTrailingSlash(path.substr(0, slash));
}

bool IsPathInsideRoot(const std::wstring& root, const std::wstring& child) noexcept {
    if (root.empty() || child.size() <= root.size() + 1) return false;
    if (child.find(L"..") != std::wstring::npos) return false;
    const std::wstring prefix = root + L"\\";
    return CompareStringOrdinal(child.data(), static_cast<int>(prefix.size()),
                                prefix.data(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

void PinSelf() noexcept {
    if (g_pinned.exchange(true, std::memory_order_acq_rel)) return;
    HMODULE ignored = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&PinSelf), &ignored);
}

struct PointerInfo {
    uint64_t generation = 0;
    std::wstring core_dir;
    std::wstring host_exe;
    bool disabled = false;
};

DWORD ReadPointer(const std::wstring& root, PointerInfo& out) noexcept {
    out = {};
    try {
        if (root.size() < 4 || root[1] != L':' || root[2] != L'\\') return ERROR_INVALID_NAME;
        const std::wstring path = root + L"\\" + kActivePointerFile;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) return GetLastError();
        BY_HANDLE_FILE_INFORMATION info{};
        LARGE_INTEGER size{};
        const BOOL ok_info = GetFileInformationByHandle(file, &info);
        const BOOL ok_size = GetFileSizeEx(file, &size);
        if (!ok_info || !ok_size) {
            const DWORD err = GetLastError();
            CloseHandle(file);
            return err;
        }
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            CloseHandle(file);
            return ERROR_REPARSE_TAG_INVALID;
        }
        if (size.QuadPart <= 0 || size.QuadPart > 65536) {
            CloseHandle(file);
            return ERROR_INVALID_DATA;
        }
        std::string raw(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        const BOOL ok_read = ReadFile(file, raw.data(), static_cast<DWORD>(raw.size()), &read, nullptr);
        CloseHandle(file);
        if (!ok_read || read != raw.size()) return ERROR_READ_FAULT;

        pulse::json::Value doc;
        if (!pulse::json::Parse(raw, doc) || !doc.is_object()) return ERROR_INVALID_DATA;
        int64_t schema = 0;
        const auto* schema_v = doc.find("schema");
        if (!schema_v || !schema_v->get_int(schema) || schema != 1) return ERROR_INVALID_DATA;

        if (const auto* dis = doc.find("disabled"); dis && dis->is_bool() && dis->as_bool()) {
            out.disabled = true;
            return ERROR_SUCCESS;
        }
        if (const auto* gen = doc.find("generation")) {
            int64_t g = 0;
            if (gen->get_int(g) && g >= 0) out.generation = static_cast<uint64_t>(g);
        }
        if (const auto* dir = doc.find(kCoreDirKey); dir && dir->is_string()) {
            out.core_dir = StripTrailingSlash(Utf8ToWide(dir->as_string()));
        }
        if (const auto* host = doc.find("host_exe"); host && host->is_string()) {
            out.host_exe = Utf8ToWide(host->as_string());
        }
        if (out.core_dir.empty()) return ERROR_FILE_NOT_FOUND;
        return ERROR_SUCCESS;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}

HRESULT NativeComDlgGetClassObject(REFCLSID clsid, REFIID iid, void** out) noexcept {
    wchar_t sysdir[MAX_PATH]{};
    const UINT len = GetSystemDirectoryW(sysdir, ARRAYSIZE(sysdir));
    if (!len || len >= ARRAYSIZE(sysdir)) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    std::wstring dll = std::wstring(sysdir, len) + L"\\comdlg32.dll";
    HMODULE mod = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mod) return HRESULT_FROM_WIN32(GetLastError());
    auto fn = reinterpret_cast<GetClassObjectFn>(GetProcAddress(mod, "DllGetClassObject"));
    if (!fn) return HRESULT_FROM_WIN32(GetLastError());
    return fn(clsid, iid, out);
}

// Ensures g_active_slot matches active-bridge.json, hot-swapping to a newer Core DLL
// when generation or core_dir changes, or retiring the old Core when disabled.
CoreGuard AcquireCurrentCore(bool force_check, uint32_t init_flags) noexcept {
    ReapRetiredSlots();
    CoreGuard guard;
    std::wstring root;
    uint64_t cur_gen = 0;
    std::wstring cur_dir;
    bool need_check = force_check;

    AcquireSRWLockShared(&g_slot_lock);
    const ULONGLONG now = GetTickCount64();
    if (!force_check && g_active_slot && (now - g_last_check_tick) < kReloadCheckIntervalMs) {
        g_active_slot->AddRef();
        guard.slot = g_active_slot;
        ReleaseSRWLockShared(&g_slot_lock);
        return guard;
    }
    root = SelfDirectory();
    if (g_active_slot) {
        cur_gen = g_active_slot->generation;
        cur_dir = g_active_slot->core_dir;
    }
    need_check = true;
    ReleaseSRWLockShared(&g_slot_lock);

    if (!need_check || root.empty()) return guard;

    PointerInfo ptr;
    const DWORD read_err = ReadPointer(root, ptr);

    if (read_err != ERROR_SUCCESS || ptr.disabled) {
        CoreSlot* retired = nullptr;
        AcquireSRWLockExclusive(&g_slot_lock);
        g_last_check_tick = GetTickCount64();
        g_last_error = ptr.disabled ? ERROR_SUCCESS : read_err;
        if (ptr.disabled && g_active_slot) {
            retired = g_active_slot;
            g_active_slot = nullptr;
        } else if (g_active_slot) {
            g_active_slot->AddRef();
            guard.slot = g_active_slot;
        }
        ReleaseSRWLockExclusive(&g_slot_lock);
        if (retired) retired->Release();
        return guard;
    }

    // No change since currently active core.
    if (!cur_dir.empty() && ptr.generation == cur_gen &&
        CompareStringOrdinal(ptr.core_dir.c_str(), -1, cur_dir.c_str(), -1, TRUE) == CSTR_EQUAL) {
        AcquireSRWLockExclusive(&g_slot_lock);
        g_last_check_tick = GetTickCount64();
        g_last_error = ERROR_SUCCESS;
        if (g_active_slot) {
            g_active_slot->AddRef();
            guard.slot = g_active_slot;
        }
        ReleaseSRWLockExclusive(&g_slot_lock);
        return guard;
    }

    // Security check: Core directory must stay strictly inside bridge_root.
    if (!IsPathInsideRoot(root, ptr.core_dir)) {
        AcquireSRWLockExclusive(&g_slot_lock);
        g_last_check_tick = GetTickCount64();
        g_last_error = ERROR_ACCESS_DENIED;
        ReleaseSRWLockExclusive(&g_slot_lock);
        return guard;
    }

    const std::wstring core_dll = ptr.core_dir + L"\\" + kCoreDllName;
    HMODULE mod = LoadLibraryExW(core_dll.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mod) {
        const DWORD err = GetLastError();
        AcquireSRWLockExclusive(&g_slot_lock);
        g_last_check_tick = GetTickCount64();
        g_last_error = err;
        if (g_active_slot) {
            g_active_slot->AddRef();
            guard.slot = g_active_slot;
        }
        ReleaseSRWLockExclusive(&g_slot_lock);
        return guard;
    }

    auto query_fn = reinterpret_cast<PulseDialogCoreQueryV1Fn>(GetProcAddress(mod, "PulseDialogCoreQueryV1"));
    PulseDialogCoreV1 table{};
    table.size = sizeof(table);
    const HRESULT qhr = query_fn ? query_fn(&table) : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    if (FAILED(qhr) || table.magic != PULSE_DIALOG_CORE_MAGIC ||
        table.abi_version != PULSE_DIALOG_CORE_ABI_VERSION ||
        table.size < sizeof(PulseDialogCoreV1)) {
        FreeLibrary(mod);
        AcquireSRWLockExclusive(&g_slot_lock);
        g_last_check_tick = GetTickCount64();
        g_last_error = ERROR_BAD_EXE_FORMAT;
        if (g_active_slot) {
            g_active_slot->AddRef();
            guard.slot = g_active_slot;
        }
        ReleaseSRWLockExclusive(&g_slot_lock);
        return guard;
    }

    auto* fresh = new (std::nothrow) CoreSlot();
    if (!fresh) {
        FreeLibrary(mod);
        return guard;
    }
    fresh->module = mod;
    fresh->table = table;
    fresh->generation = ptr.generation;
    fresh->core_dir = ptr.core_dir;
    fresh->host_exe = ptr.host_exe;
    fresh->bridge_root = root;

    // Retire old core BEFORE initializing new core if hooks are active so sink detours never collide.
    CoreSlot* retired = nullptr;
    AcquireSRWLockExclusive(&g_slot_lock);
    retired = g_active_slot;
    g_active_slot = nullptr;
    ReleaseSRWLockExclusive(&g_slot_lock);

    if (retired) {
        // Release the global slot reference; calls shutdown() + FreeLibrary when in-flight calls finish.
        retired->Release();
        retired = nullptr;
    }

    PulseStubConfigV1 cfg{};
    cfg.size = sizeof(cfg);
    cfg.version = PULSE_DIALOG_CORE_ABI_VERSION;
    cfg.flags = init_flags ? init_flags : (g_last_activate_flags ? g_last_activate_flags : PULSE_STUB_FLAG_COM_ONLY);
    cfg.generation = fresh->generation;
    cfg.bridge_root = fresh->bridge_root.c_str();
    cfg.core_dir = fresh->core_dir.c_str();
    cfg.host_exe = fresh->host_exe.c_str();

    if (fresh->table.initialize) {
        const HRESULT ihr = fresh->table.initialize(&cfg);
        if (FAILED(ihr)) {
            fresh->Release();
            AcquireSRWLockExclusive(&g_slot_lock);
            g_last_check_tick = GetTickCount64();
            g_last_error = HRESULT_FACILITY(ihr) == FACILITY_WIN32 ? HRESULT_CODE(ihr) : ERROR_GEN_FAILURE;
            ReleaseSRWLockExclusive(&g_slot_lock);
            return guard;
        }
        fresh->initialized = true;
    }

    AcquireSRWLockExclusive(&g_slot_lock);
    g_last_check_tick = GetTickCount64();
    g_last_error = ERROR_SUCCESS;
    CoreSlot* raced = g_active_slot;
    g_active_slot = fresh;
    g_active_slot->AddRef();
    guard.slot = g_active_slot;
    ReleaseSRWLockExclusive(&g_slot_lock);

    if (raced) raced->Release();
    return guard;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (clsid != CLSID_FileOpenDialog && clsid != CLSID_FileSaveDialog) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    PinSelf();
    CoreGuard core = AcquireCurrentCore(false, PULSE_STUB_FLAG_COM_ONLY);
    if (core && core->table.dll_get_class_object) {
        return core->table.dll_get_class_object(clsid, iid, output);
    }
    return NativeComDlgGetClassObject(clsid, iid, output);
}

extern "C" HRESULT WINAPI DllCanUnloadNow() {
    // Keep the tiny Stub pinned once activated so CoFreeUnusedLibraries never races with active factories or hooks.
    return g_pinned.load(std::memory_order_acquire) ? S_FALSE : S_OK;
}

extern "C" DWORD WINAPI PulseStubActivate(uint32_t flags) {
    PinSelf();
    const uint32_t effective_flags = flags ? flags
        : (PULSE_STUB_FLAG_INSTALL_SINK_HOOKS | PULSE_STUB_FLAG_WINRT_ENABLED | PULSE_STUB_FLAG_DIRECT_REVEAL);
    AcquireSRWLockExclusive(&g_slot_lock);
    g_last_activate_flags = effective_flags;
    ReleaseSRWLockExclusive(&g_slot_lock);

    CoreGuard core = AcquireCurrentCore(true, effective_flags);
    if (!core) {
        AcquireSRWLockShared(&g_slot_lock);
        const DWORD err = g_last_error ? g_last_error : ERROR_MOD_NOT_FOUND;
        ReleaseSRWLockShared(&g_slot_lock);
        return err;
    }
    if (!core->table.activate_process) return ERROR_SUCCESS;

    PulseStubConfigV1 cfg{};
    cfg.size = sizeof(cfg);
    cfg.version = PULSE_DIALOG_CORE_ABI_VERSION;
    cfg.flags = effective_flags;
    cfg.generation = core->generation;
    cfg.bridge_root = core->bridge_root.c_str();
    cfg.core_dir = core->core_dir.c_str();
    cfg.host_exe = core->host_exe.c_str();
    return core->table.activate_process(&cfg);
}

extern "C" DWORD WINAPI PulseStubHotReload() {
    CoreGuard core = AcquireCurrentCore(true, 0);
    if (!core) {
        AcquireSRWLockShared(&g_slot_lock);
        const DWORD err = g_last_error;
        ReleaseSRWLockShared(&g_slot_lock);
        return err;
    }
    return ERROR_SUCCESS;
}

extern "C" HRESULT WINAPI PulseStubGetStatus(PulseStubStatusV1* out_status) {
    if (!out_status || out_status->size < sizeof(PulseStubStatusV1)) return E_INVALIDARG;
    PulseStubStatusV1 snap{};
    snap.size = sizeof(snap);
    snap.version = PULSE_DIALOG_CORE_ABI_VERSION;

    CoreGuard core;
    AcquireSRWLockShared(&g_slot_lock);
    snap.last_error = g_last_error;
    if (g_active_slot) {
        g_active_slot->AddRef();
        core.slot = g_active_slot;
    }
    ReleaseSRWLockShared(&g_slot_lock);

    if (core) {
        snap.core_loaded = 1;
        snap.generation = core->generation;
        snap.core_build_id = core->table.core_build_id;
        snap.active_hook_mask = core->table.get_active_hook_mask ? core->table.get_active_hook_mask() : 0;
    }
    *out_status = snap;
    return S_OK;
}

extern "C" DWORD WINAPI PulseInitializeDirectReveal(void* diagnostic_out) {
    PinSelf();
    CoreGuard core = AcquireCurrentCore(false, PULSE_STUB_FLAG_DIRECT_REVEAL);
    if (!core || !core->table.initialize_direct_reveal) return ERROR_NOT_READY;
    return core->table.initialize_direct_reveal(diagnostic_out);
}

extern "C" DWORD WINAPI PulseStubSetBridgeRootForTest(const wchar_t* override_root) {
    CoreSlot* retired = nullptr;
    AcquireSRWLockExclusive(&g_slot_lock);
    g_bridge_root_override = override_root ? StripTrailingSlash(override_root) : std::wstring{};
    g_last_check_tick = 0;
    retired = g_active_slot;
    g_active_slot = nullptr;
    ReleaseSRWLockExclusive(&g_slot_lock);
    if (retired) retired->Release();
    ReapRetiredSlots();
    return ERROR_SUCCESS;
}

#include "../common/dialog_admin_trust.h"
#include "../common/dialog_bundle.h"
#include "../common/process_token_restriction.h"
#include "file_dialog_runtime.h"
#include "runtime_config.h"
#include "legacy_com_provider.h"
#include "legacy_dialog_error.h"
#include "file_dialog_ui_provider.h"
#include <mutex>
#if defined(PULSE_DIALOG_UI_PROOF)
#include "runtime_child_observer.h"
#include "legacy_call_observer.h"
#include <mutex>
#endif
#include "runtime_child_process.h"
#include "runtime_child_session.h"
#include "file_dialog_activation.h"
#include "file_dialog_activation_ex.h"
#include "file_dialog_class_factory.h"
#include "../ipc/picker_client.h"
#include <detours.h>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <cstdio>
#include <cstring>

namespace {
namespace shell = pulse::shell_bridge;
namespace config = pulse::shell_bridge::runtime;
namespace wire = pulse::ipc::picker;
std::wstring AdminDrivePath(std::wstring path) {
    // Canonical emits extended drive paths. Do not normalize UNC or devices.
    if (path.size() >= 7 && path.compare(0, 4, LR"(\\?\)") == 0 &&
        ((path[4] >= L'A' && path[4] <= L'Z') || (path[4] >= L'a' && path[4] <= L'z')) &&
        path[5] == L':' && path[6] == L'\\') path.erase(0, 4);
    return path;
}

shell::CoCreateInstanceFunction original_create = &::CoCreateInstance;
shell::CoCreateInstanceExFunction original_ex = &::CoCreateInstanceEx;
shell::CoGetClassObjectFunction original_class = &::CoGetClassObject;
constexpr uint32_t kCoverage = config::kCoCreateInstance | config::kCoCreateInstanceEx | config::kCoGetClassObject | config::kCreateProcessW;
struct EntryCounters { std::atomic<uint64_t> calls{0}, targets{0}, early{0}; };
EntryCounters entry_counts[3];
constexpr const char* entry_names[]{"CoCreateInstance", "CoCreateInstanceEx", "CoGetClassObject"};
std::atomic<LONG> install_result{ERROR_NOT_READY};
std::atomic<HRESULT> initialization{E_PENDING};
std::atomic<uint64_t> activations{0}, admitted{0}, rejected{0};
std::atomic<uint64_t> early_wrappers{0}, early_resolve_ready{0}, early_resolve_native{0};
struct Configuration {
    bool admin = false;
    std::wstring host, control;
    std::once_flag ui_once;
    std::shared_ptr<const shell::FileDialogInprocessProvider> ui_provider;
    HANDLE directory = INVALID_HANDLE_VALUE;
    std::atomic<unsigned> log_records{0};
    std::atomic<bool> routing_enabled{false};
    SRWLOCK log_lock = SRWLOCK_INIT;
    ~Configuration() { if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory); }
};
// Deliberately process lifetime: wrappers can outlive deactivation. The DLL is
// pinned before publishing this immutable configuration; no hot unload exists.
std::atomic<Configuration*> current{nullptr};
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::wstring Environment(const wchar_t* name) {
    const DWORD count = GetEnvironmentVariableW(name, nullptr, 0);
    if (!count || count > 32768) return {};
    std::wstring result(count, 0);
    const DWORD read = GetEnvironmentVariableW(name, result.data(), count);
    if (!read || read >= count) return {};
    result.resize(read);
    return result;
}
std::wstring Canonical(const std::wstring& path, bool directory) {
    if (path.empty() || !std::filesystem::path(path).is_absolute()) return {};
    Handle file{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info) ||
        ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory) return {};
    std::wstring result(32768, 0);
    const DWORD count = GetFinalPathNameByHandleW(file.value, result.data(),
        static_cast<DWORD>(result.size()), FILE_NAME_NORMALIZED);
    if (!count || count >= result.size()) return {};
    result.resize(count);
    return result;
}
bool Same(const std::wstring& left, const std::wstring& right) {
    return !left.empty() && !right.empty() && _wcsicmp(left.c_str(), right.c_str()) == 0;
}
bool Disabled(const Configuration& state) noexcept {
    try {
        if (!state.routing_enabled.load(std::memory_order_acquire)) return true;
        const auto marker = state.control + L"\\" + config::kDisableFile;
        if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
        const DWORD error = GetLastError();
        // A missing marker enables routing; a removed/inaccessible control
        // directory or any other error cannot authorize it.
        if (error != ERROR_FILE_NOT_FOUND) return true;
        return GetFileAttributesW(state.control.c_str()) == INVALID_FILE_ATTRIBUTES ||
            (!state.admin && config::ValidateReadyParentChain(state.control) != ERROR_SUCCESS);
    } catch (...) { return true; }
}
void Log(Configuration& state, const char* reason, unsigned options = 0, unsigned category = 0, const char* entry = "runtime") noexcept {
    try {
        if (state.log_records.fetch_add(1) >= 256) return;
        char line[256]{};
        const int count = std::snprintf(line, sizeof(line),
            "interface=%s tick=%llu reason=%s options=0x%08X category=%u\r\n",
            entry, GetTickCount64(), reason, options, category);
        if (count <= 0 || count >= static_cast<int>(sizeof(line))) return;
        const auto path = state.control + L"\\runtime-decisions.tsv";
        AcquireSRWLockExclusive(&state.log_lock);
        HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, line, static_cast<DWORD>(count), &written, nullptr);
            CloseHandle(file);
        }
        ReleaseSRWLockExclusive(&state.log_lock);
    } catch (...) {}
}
// The fully validated provider must be visible before child-routing publication.
// This guard owns rollback, never the published allocation: captured wrappers or
// concurrent child hooks can retain it until process exit.
struct PendingConfigurationPublication {
    Configuration* value = nullptr;
    std::wstring revoke_path;
    bool committed = false;
    bool Publish(std::unique_ptr<Configuration>& owner) {
        revoke_path = owner->control + L"\\" + config::kDisableFile;
        owner->routing_enabled.store(true, std::memory_order_release);
        Configuration* expected = nullptr;
        if (!current.compare_exchange_strong(expected, owner.get(), std::memory_order_release, std::memory_order_relaxed)) {
            owner->routing_enabled.store(false, std::memory_order_release); return false;
        }
        value = owner.release(); return true;
    }
    DWORD Revoke() noexcept {
        if (!value || committed) return ERROR_SUCCESS;
        const DWORD saved = GetLastError();
        value->routing_enabled.store(false, std::memory_order_release);
        Configuration* expected = value;
        current.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
        // Children may already have been created before the ready receipt. Their
        // existing parent-chain check sees this same marker; do not kill them.
        HANDLE marker = CreateFileW(revoke_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD error = marker == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (marker != INVALID_HANDLE_VALUE) CloseHandle(marker);
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) error = ERROR_SUCCESS;
        Log(*value, error ? "initialization-revoked-marker-failed" : "initialization-revoked-marker-written", error);
        committed = true;
        SetLastError(saved);
        return error;
    }
    void Commit() noexcept { committed = true; }
    ~PendingConfigurationPublication() noexcept { Revoke(); }
};
bool TokenSafe(bool allow_elevated = false) noexcept {
    Handle thread;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &thread.value)) return false;
    if (GetLastError() != ERROR_NO_TOKEN) return false;
    Handle process;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process.value)) return false;
    TOKEN_ELEVATION elevation{}; DWORD size = 0, container = 0;
    if (!GetTokenInformation(process.value, TokenElevation, &elevation, sizeof(elevation), &size) ||
        (bool(elevation.TokenIsElevated) != allow_elevated) ||
        pulse::security::CheckUnrestrictedToken(process.value) != ERROR_SUCCESS ||
        !GetTokenInformation(process.value, TokenIsAppContainer, &container, sizeof(container), &size) || container) return false;
    alignas(TOKEN_MANDATORY_LABEL) BYTE buffer[1024]{};
    if (!GetTokenInformation(process.value, TokenIntegrityLevel, buffer, sizeof(buffer), &size)) return false;
    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer);
    if (!IsValidSid(label->Label.Sid)) return false;
    const auto count = *GetSidSubAuthorityCount(label->Label.Sid);
    return count && *GetSidSubAuthority(label->Label.Sid, count - 1) >= SECURITY_MANDATORY_MEDIUM_RID;
}
bool Verify(Configuration& state, const shell::ContractDecision& decision, const char* entry) {
    const unsigned options = decision.effective_options.value_or(0);
    const unsigned category = static_cast<unsigned>(decision.validation_category);
    const char* reason = "capability-combination-not-certified";
    bool accepted = false;
    if (Disabled(state)) reason = "session-disabled";
    else if (!TokenSafe(state.admin)) reason = "show-token-context-not-supported";
    else if (decision.validation_category == shell::ValidationCategory::DefaultOpen) reason = "default-open-validation-not-certified";
    else if (decision.validation_category == shell::ValidationCategory::DefaultSave) reason = "default-save-validation-not-certified";
    else if ((accepted = shell::RuntimeLocalSaveContractCertified(decision)))
        reason = "single-local-save-access-v1-admitted";
    else if ((accepted = shell::RuntimeContractCertified(decision)))
        reason = decision.validation_category == shell::ValidationCategory::Folder ?
            "single-filesystem-folder-admitted" : decision.validation_category == shell::ValidationCategory::LocalOpenAccessV1 ?
            "single-local-open-access-v1-admitted" : "explicit-single-open-novalidate-admitted";
    if (accepted) ++admitted;
    Log(state, reason, options, category, entry);
    return accepted;
}
#if defined(PULSE_RUNTIME_ENTRY_TEST)
std::function<void(shell::FileDialogBridgeServices&)> legacy_test_services;
#endif
shell::FileDialogBridgeServices Services(Configuration* state, const char* entry) {
    shell::FileDialogBridgeServices services;
    services.executable = state->host;
    std::call_once(state->ui_once, [state, entry] {
        const HRESULT loaded = shell::LoadFileDialogUiProvider(state->host, state->ui_provider, true);
        Log(*state, "inprocess-ui-proof-load", static_cast<unsigned>(loaded), 0, entry);
    });
    services.inprocess_provider = state->ui_provider;
    services.save_access_policy = shell::AccessPolicy::LocalSaveV1;
#if defined(PULSE_DIALOG_UI_PROOF)
    services.on_native_reason = [state, entry](const char* reason, const IID* iid) {
        char detail[384]{};
        if (iid) {
            std::snprintf(detail, sizeof(detail), "%s-iid=%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", reason,
                static_cast<unsigned long>(iid->Data1), iid->Data2, iid->Data3,
                iid->Data4[0], iid->Data4[1], iid->Data4[2], iid->Data4[3], iid->Data4[4], iid->Data4[5], iid->Data4[6], iid->Data4[7]);
        } else std::snprintf(detail, sizeof(detail), "%s", reason);
        Log(*state, detail, 0, 0, entry);
    };
#endif
    services.access_policy = shell::AccessPolicy::LocalOpenV1;
    services.observe_contract = [state, entry](const shell::ContractDecision& decision, bool native_only,
        bool shown_before, size_t subscriptions) {
        char reason[160]{};
        std::snprintf(reason, sizeof(reason), "contract-disposition=%u-reason=%u-call=%zu-native=%u-shown=%u-events=%zu",
            static_cast<unsigned>(decision.disposition), static_cast<unsigned>(decision.reason), decision.call_index,
            static_cast<unsigned>(native_only), static_cast<unsigned>(shown_before), subscriptions);
        Log(*state, reason, decision.effective_options.value_or(0), static_cast<unsigned>(decision.validation_category), entry);
    };
    services.validation_verified = [state, entry](const auto& decision) { return Verify(*state, decision, entry); };
    services.show_native = [state, entry](IFileDialog* native, HWND owner) {
        FILEOPENDIALOGOPTIONS options{};
        const HRESULT read = native->GetOptions(&options);
        ++rejected;
        Log(*state, Disabled(*state) ? "show-session-disabled-native" : "native-contract-or-validation-fallback",
            SUCCEEDED(read) ? static_cast<unsigned>(options) : 0, 0, entry);
        return native->Show(owner);
    };
    return services;
}
shell::FileDialogBridgeServices DeferredServices(const char* entry) {
    shell::FileDialogBridgeServices services;
    services.resolve_on_show = [entry](shell::FileDialogBridgeServices& resolved) {
        auto* state = current.load(std::memory_order_acquire);
        if (!state) { ++early_resolve_native; return false; }
        if (Disabled(*state)) { ++early_resolve_native; Log(*state, "resolve-disabled-native", 0, 0, entry); return false; }
        resolved = Services(state, entry);
        ++early_resolve_ready;
        Log(*state, "resolve-ready", 0, 0, entry);
        return true;
    };
    services.on_native_reason = [entry](const char* reason, const IID*) {
        if (std::strcmp(reason, "early-wrapper-created") == 0) ++early_wrappers;
        if (auto* state = current.load(std::memory_order_acquire)) Log(*state, reason, 0, 0, entry);
    };
    return services;
}
Configuration* Enter(unsigned index, REFCLSID clsid, bool& deferred) noexcept {
    auto& counters = entry_counts[index];
    const auto call = ++counters.calls;
    const bool target = clsid == CLSID_FileOpenDialog || clsid == CLSID_FileSaveDialog;
    if (target) { ++counters.targets; ++activations; }
    // Snapshot initialization first: publication between these reads must not
    // turn an observed early target into an unwrapped native object.
    const bool initializing = initialization.load(std::memory_order_acquire) == E_PENDING;
    auto* state = current.load(std::memory_order_acquire);
    if (!state) { if (target) ++counters.early; deferred = target && initializing; return nullptr; }
    // Limit unrelated COM diagnostics so normal startup cannot exhaust target logs.
    if (!target) {
        if (call <= 8) Log(*state, "non-dialog-class-native", 0, 0, entry_names[index]);
        return nullptr;
    }
    if (Disabled(*state)) {
        Log(*state, "activation-session-disabled", 0, 0, entry_names[index]); return nullptr;
    }
    Log(*state, "activation-adapter-entered", 0, 0, entry_names[index]);
    return state;
}
decltype(&GetOpenFileNameW) original_open_w = &GetOpenFileNameW;
decltype(&GetSaveFileNameW) original_save_w = &GetSaveFileNameW;
decltype(&GetOpenFileNameA) original_open_a = &GetOpenFileNameA;
decltype(&GetSaveFileNameA) original_save_a = &GetSaveFileNameA;
decltype(&CommDlgExtendedError) original_dialog_error = &CommDlgExtendedError;
DWORD WINAPI HookDialogError() { return shell::legacy_dialog_error::Query(original_dialog_error); }

// These entry points remain entirely native. Clear only our TLS error source.
// __finally also runs during C++/SEH unwinding; no exception is swallowed and
// neither the return value nor Win32 last-error value is translated.
template<class Function, class Input>
auto CallNativeCommonDialog(Function original, Input input) -> decltype(original(input)) {
    shell::legacy_dialog_error::UseNative();
    __try {
        return original(input);
    } __finally {
        // A nested Pulse file dialog must not replace the outer native source.
        shell::legacy_dialog_error::UseNative();
    }
}
decltype(&ChooseColorW) original_ChooseColorW = &ChooseColorW;
BOOL WINAPI HookChooseColorW(LPCHOOSECOLORW input) { return CallNativeCommonDialog(original_ChooseColorW, input); }
decltype(&ChooseColorA) original_ChooseColorA = &ChooseColorA;
BOOL WINAPI HookChooseColorA(LPCHOOSECOLORA input) { return CallNativeCommonDialog(original_ChooseColorA, input); }
decltype(&ChooseFontW) original_ChooseFontW = &ChooseFontW;
BOOL WINAPI HookChooseFontW(LPCHOOSEFONTW input) { return CallNativeCommonDialog(original_ChooseFontW, input); }
decltype(&ChooseFontA) original_ChooseFontA = &ChooseFontA;
BOOL WINAPI HookChooseFontA(LPCHOOSEFONTA input) { return CallNativeCommonDialog(original_ChooseFontA, input); }
decltype(&PrintDlgW) original_PrintDlgW = &PrintDlgW;
BOOL WINAPI HookPrintDlgW(LPPRINTDLGW input) { return CallNativeCommonDialog(original_PrintDlgW, input); }
decltype(&PrintDlgA) original_PrintDlgA = &PrintDlgA;
BOOL WINAPI HookPrintDlgA(LPPRINTDLGA input) { return CallNativeCommonDialog(original_PrintDlgA, input); }
decltype(&PrintDlgExW) original_PrintDlgExW = &PrintDlgExW;
HRESULT WINAPI HookPrintDlgExW(LPPRINTDLGEXW input) { return CallNativeCommonDialog(original_PrintDlgExW, input); }
decltype(&PrintDlgExA) original_PrintDlgExA = &PrintDlgExA;
HRESULT WINAPI HookPrintDlgExA(LPPRINTDLGEXA input) { return CallNativeCommonDialog(original_PrintDlgExA, input); }
decltype(&PageSetupDlgW) original_PageSetupDlgW = &PageSetupDlgW;
BOOL WINAPI HookPageSetupDlgW(LPPAGESETUPDLGW input) { return CallNativeCommonDialog(original_PageSetupDlgW, input); }
decltype(&PageSetupDlgA) original_PageSetupDlgA = &PageSetupDlgA;
BOOL WINAPI HookPageSetupDlgA(LPPAGESETUPDLGA input) { return CallNativeCommonDialog(original_PageSetupDlgA, input); }
decltype(&FindTextW) original_FindTextW = &FindTextW;
HWND WINAPI HookFindTextW(LPFINDREPLACEW input) { return CallNativeCommonDialog(original_FindTextW, input); }
decltype(&FindTextA) original_FindTextA = &FindTextA;
HWND WINAPI HookFindTextA(LPFINDREPLACEA input) { return CallNativeCommonDialog(original_FindTextA, input); }
decltype(&ReplaceTextW) original_ReplaceTextW = &ReplaceTextW;
HWND WINAPI HookReplaceTextW(LPFINDREPLACEW input) { return CallNativeCommonDialog(original_ReplaceTextW, input); }
decltype(&ReplaceTextA) original_ReplaceTextA = &ReplaceTextA;
HWND WINAPI HookReplaceTextA(LPFINDREPLACEA input) { return CallNativeCommonDialog(original_ReplaceTextA, input); }
template<class Ofn, class Original>
BOOL RouteLegacy(Ofn* input, Original original, bool save, const char* entry) {
    shell::legacy_dialog_error::UseNative();
    auto* state = current.load(std::memory_order_acquire);
    bool enabled = state && !Disabled(*state);
#if defined(PULSE_RUNTIME_ENTRY_TEST)
    if (state && legacy_test_services) enabled = true;
#endif
    if (enabled) {
        BOOL result = FALSE;
        try {
            const shell::LegacyDialogFactory create = [state, entry](bool saving, IFileDialog** output) {
                IUnknown* native = nullptr;
                HRESULT hr = shell::CreateSystemLegacyDialog(saving, &native);
                if (FAILED(hr)) return hr;
                try { auto services = Services(state, entry);
#if defined(PULSE_RUNTIME_ENTRY_TEST)
                    if (legacy_test_services) legacy_test_services(services);
#endif
                    hr = shell::WrapNativeFileDialog(saving ? shell::DialogFactory::Save : shell::DialogFactory::Open,
                    native, services, IID_IFileDialog, reinterpret_cast<void**>(output)); }
                catch (...) { hr = E_FAIL; }
                native->Release();
                return hr;
            };
            const shell::LegacyDiagnostic diagnostic = [state, entry](const shell::LegacyComDiagnostic& value) {
                char detail[240]{};
                std::snprintf(detail, sizeof(detail), "legacy-input size=%lu flags=%lu flags_ex=%lu presence=%lu max_file=%lu max_title=%lu max_custom=%lu filter_index=%lu",
                    value.structure_size, value.flags, value.flags_ex, value.presence, value.max_file, value.max_title, value.max_custom_filter, value.filter_index);
                Log(*state, detail, value.flags, value.presence, entry);
                Log(*state, value.reason, static_cast<unsigned>(value.error), 0, entry);
            };
            if (shell::RunLegacyComDialog(input, save, create, result, diagnostic)) return result;
            Log(*state, "legacy-adapter-declined", 0, save ? 1u : 0u, entry);
        } catch (...) { /* Service construction failed before any Show. */ }
    }
    shell::legacy_dialog_error::UseNative();
    const BOOL result = original(input);
    shell::legacy_dialog_error::UseNative();
    return result;
}
BOOL WINAPI ObserveOpenW(LPOPENFILENAMEW input) { return RouteLegacy(input, original_open_w, false, "GetOpenFileNameW"); }
BOOL WINAPI ObserveSaveW(LPOPENFILENAMEW input) { return RouteLegacy(input, original_save_w, true, "GetSaveFileNameW"); }
BOOL WINAPI ObserveOpenA(LPOPENFILENAMEA input) { return RouteLegacy(input, original_open_a, false, "GetOpenFileNameA"); }
BOOL WINAPI ObserveSaveA(LPOPENFILENAMEA input) { return RouteLegacy(input, original_save_a, true, "GetSaveFileNameA"); }
HRESULT WINAPI HookCreate(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* output) noexcept {
    bool deferred = false;
    auto* state = Enter(0, clsid, deferred);
    if (!state && !deferred) return original_create(clsid, outer, context, iid, output);
    try {
        shell::FileDialogActivation activation;
        activation.original = original_create; activation.enabled = true;
        activation.services = state ? Services(state, entry_names[0]) : DeferredServices(entry_names[0]);
        const auto hr = shell::ActivateFileDialog(activation, clsid, outer, context, iid, output);
        if (state) Log(*state, "adapter-return", static_cast<unsigned>(hr), context, entry_names[0]);
        return hr;
    } catch (...) {
        if (state) Log(*state, "service-construction-failed-native", 0, 0, entry_names[0]);
        return original_create(clsid, outer, context, iid, output);
    }
}
HRESULT WINAPI HookEx(REFCLSID clsid, IUnknown* outer, DWORD context, COSERVERINFO* server,
    DWORD count, MULTI_QI* results) noexcept {
    bool deferred = false;
    auto* state = Enter(1, clsid, deferred);
    if (!state && !deferred) return original_ex(clsid, outer, context, server, count, results);
    try {
        shell::FileDialogActivationEx activation;
        activation.original = original_ex; activation.enabled = true;
        activation.services = state ? Services(state, entry_names[1]) : DeferredServices(entry_names[1]);
        const auto hr = shell::ActivateFileDialogEx(activation, clsid, outer, context, server, count, results);
        if (state) Log(*state, "adapter-return", static_cast<unsigned>(hr), context, entry_names[1]);
        return hr;
    } catch (...) {
        if (state) Log(*state, "service-construction-failed-native", 0, 0, entry_names[1]);
        return original_ex(clsid, outer, context, server, count, results);
    }
}
HRESULT WINAPI HookClass(REFCLSID clsid, DWORD context, LPVOID server, REFIID iid, LPVOID* output) noexcept {
    bool deferred = false;
    auto* state = Enter(2, clsid, deferred);
    if (!state && !deferred) return original_class(clsid, context, server, iid, output);
    try {
        shell::FileDialogClassActivation activation;
        activation.original = original_class; activation.enabled = true;
        activation.services = state ? Services(state, entry_names[2]) : DeferredServices(entry_names[2]);
        const auto hr = shell::ActivateFileDialogClass(activation, clsid, context, server, iid, output);
        if (state) Log(*state, "adapter-return", static_cast<unsigned>(hr), context, entry_names[2]);
        return hr;
    } catch (...) {
        if (state) Log(*state, "service-construction-failed-native", 0, 0, entry_names[2]);
        return original_class(clsid, context, server, iid, output);
    }
}
void StartupDiagnostics(Configuration& state) noexcept {
    Log(state, "validated-session-provider-published", kCoverage);
    Log(state, "early-wrapper-created", static_cast<unsigned>(early_wrappers.load()));
    Log(state, "resolve-ready", static_cast<unsigned>(early_resolve_ready.load()));
    Log(state, "resolve-not-ready-native", static_cast<unsigned>(early_resolve_native.load()));
    for (unsigned i = 0; i != 3; ++i) {
        char reason[160]{};
        std::snprintf(reason, sizeof(reason), "startup-counters-calls=%llu-targets=%llu-before-ready=%llu",
            entry_counts[i].calls.load(), entry_counts[i].targets.load(), entry_counts[i].early.load());
        Log(state, reason, 0, 0, entry_names[i]);
        // Record forwarded export addresses, not assumptions about ole32/combase.
        for (const wchar_t* module : {L"ole32.dll", L"combase.dll"}) {
            const auto handle = GetModuleHandleW(module);
            const auto address = handle ? GetProcAddress(handle, entry_names[i]) : nullptr;
            MEMORY_BASIC_INFORMATION memory{};
            if (address) VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory));
            std::snprintf(reason, sizeof(reason), "%s-export=%p-allocation=%p",
                module[0] == L'o' ? "ole32" : "combase", reinterpret_cast<void*>(address), memory.AllocationBase);
            Log(state, reason, 0, 0, entry_names[i]);
        }
    }
}
bool ChildEnabled() noexcept {
    const auto reject = [](config::ChildGateReason reason, DWORD error) {
        config::SetChildGateDiagnostic(reason, error); return false;
    };
    try {
        const auto* state = current.load(std::memory_order_acquire);
        if (!state) return reject(config::ChildGateReason::NotPublished, ERROR_NOT_READY);
        if (state->admin) return reject(config::ChildGateReason::Revoked, ERROR_ACCESS_DENIED);
        if (!state->routing_enabled.load(std::memory_order_acquire))
            return reject(config::ChildGateReason::Revoked, ERROR_CANCELLED);
        const auto marker = state->control + L"\\" + config::kDisableFile;
        if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES)
            return reject(config::ChildGateReason::MarkerPresent, ERROR_CANCELLED);
        const DWORD marker_error = GetLastError();
        if (marker_error != ERROR_FILE_NOT_FOUND) return reject(config::ChildGateReason::MarkerQuery, marker_error);
        if (GetFileAttributesW(state->control.c_str()) == INVALID_FILE_ATTRIBUTES)
            return reject(config::ChildGateReason::ControlMissing, GetLastError());
        const DWORD chain_error = config::ValidateParentChain(state->control);
        if (chain_error) return reject(config::ChildGateReason::ParentChain, chain_error);
        return true;
    } catch (...) { return reject(config::ChildGateReason::Exception, ERROR_GEN_FAILURE); }
}
void ChildLog(const char* reason, DWORD error, DWORD pid, config::ChildCallEntry entry) noexcept {
    if (auto* state = current.load(std::memory_order_acquire)) Log(*state, reason, error, pid,
        entry == config::ChildCallEntry::CreateProcessAsUserW ? "CreateProcessAsUserW" : "CreateProcessW");
}
void ChildDiagnostic(const config::ChildCallDiagnostic& event) noexcept {
    if (auto* state = current.load(std::memory_order_acquire)) {
        char reason[192]{};
        const int count = std::snprintf(reason, sizeof(reason),
            "child-call:n=%llu;t0=%llu;t1=%llu;b=%u;g=%u;s=%lu;ok=%ld;ge=%lu;tr=%lu;early=%u",
            event.sequence, event.entered_tick, event.returned_tick,
            static_cast<unsigned>(event.branch), static_cast<unsigned>(event.gate),
            event.prepare_stage, event.result, event.gate_error, event.token_reason, event.early_resolved ? 1u : 0u);
        if (count > 0 && count < static_cast<int>(sizeof(reason)))
            Log(*state, reason, event.native_error, event.child_pid,
                event.entry == config::ChildCallEntry::CreateProcessW ? "CreateProcessW" :
                event.entry == config::ChildCallEntry::CreateProcessA ? "CreateProcessA" :
                event.entry == config::ChildCallEntry::CreateProcessAsUserW ? "CreateProcessAsUserW" :
                event.entry == config::ChildCallEntry::CreateProcessWithTokenW ? "CreateProcessWithTokenW" : "CreateProcessWithLogonW");
    }
}
std::string DllAnsiPath(HMODULE module) {
    std::wstring path(32768, 0);
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length);
    const UINT codepage = GetACP();
    BOOL replaced = FALSE;
    const DWORD flags = codepage == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    BOOL* replacement = codepage == CP_UTF8 ? nullptr : &replaced;
    const int size = WideCharToMultiByte(codepage, flags, path.c_str(), -1, nullptr, 0, nullptr, replacement);
    if (!size || replaced) return {};
    std::string result(static_cast<size_t>(size), 0);
    if (!WideCharToMultiByte(codepage, flags, path.c_str(), -1, result.data(), size, nullptr, replacement) || replaced) return {};
    std::wstring roundtrip(path.size() + 1, 0);
    if (MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, result.c_str(), -1, roundtrip.data(),
        static_cast<int>(roundtrip.size())) != static_cast<int>(path.size() + 1) ||
        std::wstring(roundtrip.c_str()) != path) return {};
    result.resize(result.size() - 1); return result;
}
std::atomic<const std::wstring*> early_authority_control{nullptr};
bool EarlyChildEnabled() noexcept {
    if (pulse::dialog_admin::IsElevated()) return false;
    if (current.load(std::memory_order_acquire)) return ChildEnabled();
    return initialization.load(std::memory_order_acquire) == E_PENDING;
}
bool ResolveEarlyChildRouting(config::EarlyChildRouting& output) noexcept {
    try {
        if (!EarlyChildEnabled() || install_result.load() != NO_ERROR) return false;
        const auto nonce = Environment(L"PULSE_DIALOG_NONCE");
        const auto host = Canonical(Environment(L"PULSE_DIALOG_HOST"), false);
        const auto control = Canonical(Environment(L"PULSE_DIALOG_CONTROL"), true);
        if (host.empty() || control.empty() ||
            config::ValidateBoundChildRouting(control, host, nonce) != ERROR_SUCCESS) return false;
        // Query our already mapped image; no LoadLibrary, worker join or COM
        // activation is permitted on this early process-creation path.
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(reinterpret_cast<const void*>(&ResolveEarlyChildRouting), &memory, sizeof(memory))) return false;
        auto dll = DllAnsiPath(static_cast<HMODULE>(memory.AllocationBase));
        if (dll.empty()) return false;
        // Publish only a fully checked authority so initialization failure can
        // revoke descendants that were bound before provider publication.
        auto authority = std::make_unique<std::wstring>(control);
        const std::wstring* expected = nullptr;
        if (early_authority_control.compare_exchange_strong(expected, authority.get(),
                std::memory_order_release, std::memory_order_relaxed)) authority.release();
        if (!EarlyChildEnabled()) return false;
        output.control = control; output.dll = std::move(dll);
        output.enabled = EarlyChildEnabled; output.log = ChildLog; output.diagnostics = ChildDiagnostic;
        return true;
    } catch (...) { return false; }
}
[[maybe_unused]] DWORD WINAPI Initialize(void*) noexcept {
    struct FinishInitialization {
        ~FinishInitialization() {
            HRESULT pending = E_PENDING;
            initialization.compare_exchange_strong(pending, E_FAIL);
            if (FAILED(initialization.load(std::memory_order_acquire))) {
                if (const auto* control = early_authority_control.load(std::memory_order_acquire)) {
                    try {
                        Handle marker{CreateFileW((*control + L"\\" + config::kDisableFile).c_str(),
                            GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
                        DWORD error = marker.value == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
                        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) error = ERROR_SUCCESS;
                        Configuration diagnostic; diagnostic.control = *control;
                        Log(diagnostic, error ? "early-initialization-revoke-marker-failed" :
                            "early-initialization-revoke-marker-written", error);
                    } catch (...) {}
                }
            }
        }
    } finish_initialization;
    try {
        // Pin outside DllMain. There is no detach API; all captured callbacks
        // and Detours trampolines remain valid until the application exits.
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&Initialize), &pinned)) {
            initialization = HRESULT_FROM_WIN32(GetLastError()); return 0;
        }
        const auto nonce = Environment(L"PULSE_DIALOG_NONCE");
        GUID parsed{};
        if (nonce.size() != 38 || FAILED(CLSIDFromString(nonce.c_str(), &parsed))) return 0;
        const auto host = Canonical(Environment(L"PULSE_DIALOG_HOST"), false);
        const auto control = Canonical(Environment(L"PULSE_DIALOG_CONTROL"), true);
        if (host.empty() || control.empty()) return 0;
        // Elevation, not untrusted session bytes, determines the read boundary.
        if (pulse::dialog_admin::IsElevated()) {
            DWORD error = pulse::dialog_admin::VerifyProtectedPath(AdminDrivePath(control), true);
            if (!error) error = pulse::dialog_admin::VerifyProtectedPath(AdminDrivePath(std::filesystem::path(control).parent_path().wstring()), true);
            if (!error) error = pulse::dialog_admin::VerifyProtectedPath(AdminDrivePath(control + L"\\" + config::kSessionFile), false);
            if (error) { initialization = HRESULT_FROM_WIN32(error); return 0; }
        }
        auto session = std::make_unique<config::Session>();
        Handle file{CreateFileW((control + L"\\" + config::kSessionFile).c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        LARGE_INTEGER size{}; DWORD read = 0;
        if (file.value == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.value, &size) ||
            size.QuadPart != static_cast<LONGLONG>(sizeof(config::Session)) ||
            !ReadFile(file.value, session.get(), sizeof(config::Session), &read, nullptr) || read != sizeof(config::Session)) return 0;
        if (session->magic != config::kConfigMagic || session->version != config::kVersion ||
            session->size != sizeof(config::Session) || (session->reserved & ~config::kAdminSession) || session->nonce[39] ||
            session->host_path[config::kPathUnits - 1] || session->target_path[config::kPathUnits - 1] ||
            session->control_path[config::kPathUnits - 1] || session->launcher_path[config::kPathUnits - 1] ||
            session->target_pid != GetCurrentProcessId() || nonce != session->nonce) return 0;
#ifdef _WIN64
        constexpr uint32_t machine = IMAGE_FILE_MACHINE_AMD64;
#else
        constexpr uint32_t machine = IMAGE_FILE_MACHINE_I386;
#endif
        FILETIME created{}, exit{}, kernel{}, user{};
        std::wstring image(32768, 0);
        const DWORD image_size = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
        if (!image_size || image_size >= image.size()) return 0;
        image.resize(image_size);
        if (session->machine != machine || !GetProcessTimes(GetCurrentProcess(), &created, &exit, &kernel, &user) ||
            CompareFileTime(&created, &session->target_created) != 0 ||
            !Same(Canonical(session->host_path, false), host) ||
            !Same(Canonical(session->control_path, true), control) ||
            !Same(Canonical(session->target_path, false), Canonical(image, false)) ||
            Canonical(session->launcher_path, false).empty()) return 0;
        const bool admin = session->reserved == config::kAdminSession;
        if (admin) {
            DWORD error = pulse::dialog_admin::IsElevated() ? ERROR_SUCCESS : ERROR_ELEVATION_REQUIRED;
            if (!error && !TokenSafe(true)) error = ERROR_ACCESS_DENIED;
            const auto bundle_root = std::filesystem::path(AdminDrivePath(host)).parent_path();
            if (!error) error = pulse::dialog_admin::VerifyProtectedTree(bundle_root.wstring());
            if (!error) error = pulse::dialog_admin::VerifyProtectedPath(AdminDrivePath(control), true);
            if (!error) error = pulse::dialog_admin::VerifyProtectedPath(AdminDrivePath(std::filesystem::path(control).parent_path().wstring()), true);
            if (!error) error = pulse::dialog_admin::VerifyProtectedPath(AdminDrivePath(control + L"\\" + config::kSessionFile), false);
            pulse::dialog_bundle::BundleLease bundle;
            if (!error) error = pulse::dialog_bundle::VerifyBundle(bundle_root.wstring(), {}, bundle);
            wchar_t module[32768]{};
            const DWORD count = GetModuleFileNameW(pinned, module, 32768);
            if (!error && (!count || count >= 32768 ||
                !Same(Canonical(bundle.Host(), false), host) ||
                !Same(Canonical(bundle.Launcher(), false), Canonical(session->launcher_path, false)) ||
                !Same(Canonical(bundle.Runtime(), false), Canonical(module, false)))) error = ERROR_INVALID_DATA;
            if (error) { initialization = HRESULT_FROM_WIN32(error); return 0; }
        }
        auto state = std::make_unique<Configuration>();
        state->host = host; state->control = control; state->admin = admin;
        state->directory = CreateFileW(control.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (state->directory == INVALID_HANDLE_VALUE) return 0;
        const auto runtime_dll = DllAnsiPath(pinned);
        config::Ready ready;
        ready.target_pid = session->target_pid; ready.machine = machine;
        ready.target_created = created;
        wcscpy_s(ready.nonce, session->nonce);
        const LONG installed = install_result.load();
        ready.status = installed == NO_ERROR ? S_OK : HRESULT_FROM_WIN32(installed);
        ready.installed_entries = installed == NO_ERROR ? kCoverage : 0;
        PendingConfigurationPublication publication;
        Configuration* published = nullptr;
        bool child_configured = false;
        if (installed == NO_ERROR) {
            // No waiting and no partial provider: identity checks above are all
            // complete. Child routing cannot observe current == nullptr here.
            if (!publication.Publish(state)) { initialization = HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED); return 0; }
            published = publication.value;
            child_configured = !published->admin && !runtime_dll.empty() && config::ConfigureChildRouting(control, runtime_dll, ChildEnabled, ChildLog, ChildDiagnostic);
        }
        const auto temporary = control + L"\\runtime-ready.tmp";
        Handle receipt{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        DWORD written = 0;
        if (receipt.value == INVALID_HANDLE_VALUE || !WriteFile(receipt.value, &ready, sizeof(ready), &written, nullptr)) {
            initialization = HRESULT_FROM_WIN32(GetLastError()); return 0;
        }
        if (written != sizeof(ready)) { initialization = HRESULT_FROM_WIN32(ERROR_WRITE_FAULT); return 0; }
        if (!FlushFileBuffers(receipt.value)) { initialization = HRESULT_FROM_WIN32(GetLastError()); return 0; }
        CloseHandle(receipt.value); receipt.value = INVALID_HANDLE_VALUE;
        const auto destination = control + L"\\" + config::kReadyFile;
        // Ready may be consumed immediately: publish the provider before the
        // atomic receipt rename. On failure revoke even already captured services.
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
            initialization = HRESULT_FROM_WIN32(GetLastError());
            return 0; // publication guard revokes memory and parent-chain authority.
        }
        publication.Commit();
        initialization = ready.status;
        if (published) {
            StartupDiagnostics(*published);
            Log(*published, child_configured ? "child-routing-configured" : "child-routing-unavailable-native", 0, 0, "CreateProcessW");
#if defined(PULSE_DIALOG_UI_PROOF)
            Log(*published, "public-create-hooks-installed", 4, 0, "runtime");
            Log(*published, "legacy-com-adapters-installed", 4, 0, "runtime");
#endif
            config::PublishChildRoutingDiagnostics();
            const auto child_snapshot = config::ReadChildRoutingDiagnostics();
            Log(*published, "child-observed-startup-count", static_cast<unsigned>(child_snapshot.calls),
                static_cast<unsigned>(child_snapshot.dropped), "CreateProcessW");
#if defined(PULSE_DIALOG_UI_PROOF)
            Log(*published, "public-create-observed-startup-count", static_cast<unsigned>(child_snapshot.total_calls),
                static_cast<unsigned>(child_snapshot.total_dropped), "runtime");
#endif
        }
    } catch (...) { initialization = E_FAIL; }
    return 0;
}
}
extern "C" HRESULT WINAPI PulseFileDialogRuntimeQuery(PulseFileDialogRuntimeStatus* output, uint32_t size) noexcept {
    if (!output || size != sizeof(*output)) return E_INVALIDARG;
    PulseFileDialogRuntimeStatus status;
    status.process_id = GetCurrentProcessId();
    status.installed_entries = install_result == NO_ERROR ? kCoverage : 0;
    status.initialization = initialization.load();
    const auto* state = current.load(std::memory_order_acquire);
    status.ready = state && !Disabled(*state);
    status.activations = activations.load(); status.admitted = admitted.load(); status.rejected = rejected.load();
    *output = status;
    return S_OK;
}
#ifndef PULSE_RUNTIME_ENTRY_TEST
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH || DetourIsHelperProcess()) return TRUE;
    config::SetEarlyChildRoutingResolver(ResolveEarlyChildRouting);
    DisableThreadLibraryCalls(instance);
    DetourRestoreAfterWith();
    LONG result = DetourTransactionBegin();
    if (result == NO_ERROR) {
        result = DetourUpdateThread(GetCurrentThread());
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_create), reinterpret_cast<PVOID>(HookCreate));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_ex), reinterpret_cast<PVOID>(HookEx));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_class), reinterpret_cast<PVOID>(HookClass));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&config::original_create_process), reinterpret_cast<PVOID>(config::HookCreateProcessW));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_open_w), reinterpret_cast<PVOID>(ObserveOpenW));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_save_w), reinterpret_cast<PVOID>(ObserveSaveW));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_open_a), reinterpret_cast<PVOID>(ObserveOpenA));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_save_a), reinterpret_cast<PVOID>(ObserveSaveA));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_dialog_error), reinterpret_cast<PVOID>(HookDialogError));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_ChooseColorW), reinterpret_cast<PVOID>(HookChooseColorW));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_ChooseColorA), reinterpret_cast<PVOID>(HookChooseColorA));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_ChooseFontW), reinterpret_cast<PVOID>(HookChooseFontW));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_ChooseFontA), reinterpret_cast<PVOID>(HookChooseFontA));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_PrintDlgW), reinterpret_cast<PVOID>(HookPrintDlgW));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_PrintDlgA), reinterpret_cast<PVOID>(HookPrintDlgA));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_PrintDlgExW), reinterpret_cast<PVOID>(HookPrintDlgExW));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_PrintDlgExA), reinterpret_cast<PVOID>(HookPrintDlgExA));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_PageSetupDlgW), reinterpret_cast<PVOID>(HookPageSetupDlgW));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_PageSetupDlgA), reinterpret_cast<PVOID>(HookPageSetupDlgA));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_FindTextW), reinterpret_cast<PVOID>(HookFindTextW));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_FindTextA), reinterpret_cast<PVOID>(HookFindTextA));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_ReplaceTextW), reinterpret_cast<PVOID>(HookReplaceTextW));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_ReplaceTextA), reinterpret_cast<PVOID>(HookReplaceTextA));
#if defined(PULSE_DIALOG_UI_PROOF)
        // Imported public exports are already resolved by the loader. No module
        // loading, environment parsing or logging occurs under loader lock.
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&config::original_observed_create_a), reinterpret_cast<PVOID>(config::ObserveCreateProcessA));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&config::original_observed_as_user), reinterpret_cast<PVOID>(config::ObserveCreateProcessAsUserW));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&config::original_observed_with_token), reinterpret_cast<PVOID>(config::ObserveCreateProcessWithTokenW));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&config::original_observed_with_logon), reinterpret_cast<PVOID>(config::ObserveCreateProcessWithLogonW));
#endif
        if (result == NO_ERROR) result = DetourTransactionCommit();
        else DetourTransactionAbort();
    }
    install_result = result;
    // No wait, COM, IPC, environment parsing, file access or application policy
    // under loader lock. Early calls and initialization failures stay native.
    HANDLE worker = CreateThread(nullptr, 0, Initialize, nullptr, 0, nullptr);
    if (worker) CloseHandle(worker);
    else initialization = HRESULT_FROM_WIN32(GetLastError());
    return TRUE;
}

#endif


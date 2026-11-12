#include "dialog_application_identity.h"
#if defined(PULSE_DIALOG_HISTORY_GLOBAL)
#include "../common/runtime_log.h"
#include <shlobj.h>
#endif
#include "../common/dialog_admin_trust.h"
#include "../common/dialog_bundle.h"
#include "../common/process_token_restriction.h"
#include "file_dialog_runtime.h"
#include "runtime_config.h"
#if defined(PULSE_EXPERIMENTAL_WINRT_PICKER)
#include "winrt_picker_activation.h"
#endif
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
#include "runtime_auto_entry.h"
#include "runtime_auto_imports.h"
#include "runtime_auto_lease.h"
#include "runtime_auto_monitor.h"
#include <array>
#endif
#include "legacy_com_provider.h"
#include "browse_folder_adapter.h"
#include "legacy_dialog_error.h"
#include "file_dialog_ui_provider.h"
#include "file_dialog_companion_provider.h"
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
#include <iterator>

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
#if defined(PULSE_EXPERIMENTAL_WINRT_PICKER)
shell::winrt_picker::GetActivationFactoryFunction original_ro_factory = &::RoGetActivationFactory;
#endif
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
std::atomic<bool> auto_started{false};
std::atomic<bool> auto_require_binding{false};
HMODULE auto_module = nullptr;
std::atomic<uint32_t> auto_mask{0}, auto_slots{0}, auto_skipped{0}, auto_foreign{0}, auto_protection_restored{1};
void RecordAutoImports(const config::AutoImportResult& result) noexcept {
    auto_mask = result.api_mask; auto_slots = result.patched_slots;
    auto_skipped = result.skipped_modules; auto_foreign = result.foreign_slots;
    auto_protection_restored = result.protection_restored ? 1u : 0u;
}
#else
constexpr uint32_t kCoverage = config::kCoCreateInstance | config::kCoCreateInstanceEx | config::kCoGetClassObject | config::kCreateProcessW |
    config::kSHBrowseForFolderW | config::kSHBrowseForFolderA
#if defined(PULSE_EXPERIMENTAL_WINRT_PICKER)
    | config::kRoGetActivationFactory
#endif
    ;
#endif
struct EntryCounters { std::atomic<uint64_t> calls{0}, targets{0}, early{0}; };
EntryCounters entry_counts[3];
constexpr const char* entry_names[]{"CoCreateInstance", "CoCreateInstanceEx", "CoGetClassObject"};
std::atomic<LONG> install_result{ERROR_NOT_READY};
std::atomic<HRESULT> initialization{E_PENDING};
std::atomic<uint64_t> activations{0}, admitted{0}, rejected{0};
std::atomic<uint64_t> early_wrappers{0}, early_resolve_ready{0}, early_resolve_native{0};
struct Configuration {
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
    config::AutoLease auto_lease;
#endif
    bool admin = false;
    std::wstring host, control;
    std::once_flag ui_once;
    std::shared_ptr<const shell::FileDialogInprocessProvider> ui_provider;
    std::shared_ptr<pulse::dialog_bundle::BundleLease> bundle;
    HANDLE directory = INVALID_HANDLE_VALUE;
    std::atomic<unsigned> log_records{0};
#if defined(PULSE_RUNTIME_ENTRY_TEST)
    // Existing in-source runtime fixtures opt out of filesystem diagnostics.
    std::atomic<unsigned> show_log_records{512};
#else
    std::atomic<unsigned> show_log_records{0};
#endif
    const ULONGLONG log_session = GetTickCount64();
    std::atomic<uint64_t> event_sequence{0};
#if defined(PULSE_DIALOG_HISTORY_GLOBAL)
    std::once_flag global_log_once;
#endif
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
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
        if (!state.auto_lease.Alive()) return true;
#endif
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
const char* EntryFamily(const char* entry) noexcept {
    if (!std::strcmp(entry, "SHBrowseForFolderW")) return "SHBROWSE_W";
    if (!std::strcmp(entry, "SHBrowseForFolderA")) return "SHBROWSE_A";
    if (!std::strcmp(entry, "GetOpenFileNameW") || !std::strcmp(entry, "GetSaveFileNameW")) return "OFN_W";
    if (!std::strcmp(entry, "GetOpenFileNameA") || !std::strcmp(entry, "GetSaveFileNameA")) return "OFN_A";
    return "COM";
}
const char* ContractReasonName(shell::NativeReason reason) noexcept {
    constexpr const char* names[]{"None", "SetterFailed", "UnrecognizedSuccess", "UnknownSetting", "UnsupportedOptions",
        "FilesystemNotForced", "RecentItemsPolicy", "MissingValidationOptions", "DefaultDirectoryPolicy",
        "NonFilesystemFolder", "UnsupportedFilename", "UnsupportedExtension", "UnsupportedFilter",
        "UnsupportedSettingOrder", "FolderFileSettings", "SaveContract", "Events", "Customization",
        "Labels", "Places", "Persistence", "SaveMetadata", "ShellFilter", "InvalidData", "Limit"};
    const auto index = static_cast<unsigned>(reason);
    return index < std::size(names) ? names[index] : "Unknown";
}
const char* FallbackReasonName(shell::FileDialogShowFallback reason) noexcept {
    constexpr const char* names[]{"none", "resolve-not-ready", "provider-unavailable", "initial-folder-unavailable",
        "contract", "provider-prepare-failed", "provider-failed-before-ui"};
    const auto index = static_cast<unsigned>(reason);
    return index < std::size(names) ? names[index] : "unknown";
}
#if defined(PULSE_DIALOG_HISTORY_GLOBAL)
void LogGlobalShow(Configuration& state, const char* entry, uint64_t dialog, bool begin,
    const shell::FileDialogShowObservation& value, uint64_t sequence) {
    namespace log = pulse::diagnostics::runtime;
    std::call_once(state.global_log_once, [&state] {
        if (!log::Enabled()) {
            PWSTR root = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &root))) {
                const std::wstring directory(root); CoTaskMemFree(root);
                log::Initialize(directory + L"\\Pulse", "dialog_runtime");
            }
        }
        const auto& identity = shell::application_identity::CurrentApplication();
        log::TextEvent("dialog_identity", {{"app_basename", identity.basename}},
            {{"source_session", state.log_session}, {"creation_time_filetime", identity.creation_time}, {"app_image_id", identity.image_id},
             {"app_instance_id", identity.instance_id}, {"identity_revision", 2}, {"process_arch", sizeof(void*) * 8}});
    });
    const auto request = std::to_wstring(dialog) + L":" + std::to_wstring(value.sequence);
    const wchar_t* kind = !value.factory ? L"unknown" : *value.factory == shell::DialogFactory::Save ? L"save" : L"open";
    const char* family = EntryFamily(entry);
    const std::wstring wide_family(family, family + std::strlen(family));
    if (begin) {
        log::TextEvent("dialog_show_begin", {{"request", request}, {"entry_family", wide_family}, {"launch_route", L"managed"}, {"dialog_kind", kind}},
            {{"dialog", dialog}, {"show_sequence", value.sequence}, {"source_session", state.log_session}, {"source_event_sequence", sequence}, {"process_arch", sizeof(void*) * 8}});
    } else {
        const wchar_t* terminal = value.native_fallback ? L"native" : value.accepted_result ? L"accepted" :
            value.result == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? L"cancelled" : FAILED(value.result) ? L"failed" : L"closed";
        const char* reason = ContractReasonName(value.contract_reason);
        const char* fallback = FallbackReasonName(value.fallback);
        log::TextEvent("dialog_show_terminal", {{"request", request}, {"terminal_status", terminal},
            {"reason_name", std::wstring(reason, reason + std::strlen(reason))}, {"fallback_reason", std::wstring(fallback, fallback + std::strlen(fallback))}},
            {{"dialog", dialog}, {"show_sequence", value.sequence}, {"source_session", state.log_session}, {"source_event_sequence", sequence},
             {"hr", static_cast<unsigned>(value.result)}, {"native_fallback", value.native_fallback},
             {"provider_started", value.provider_started}, {"ui_shown_known", value.ui_shown.has_value()}, {"ui_shown", value.ui_shown.value_or(false)}});
    }
}
#endif
void LogShow(Configuration& state, const char* entry, uint64_t dialog, bool begin,
    const shell::FileDialogShowObservation& value) noexcept {
    try {
        if (state.show_log_records.fetch_add(1) >= 512) return;
        const char* terminal = begin ? "pending" : value.native_fallback ? "native" :
            value.accepted_result ? "accepted" : value.result == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? "cancelled" :
            FAILED(value.result) ? "failed" : "closed";
        const auto sequence = ++state.event_sequence;
#if defined(PULSE_DIALOG_HISTORY_GLOBAL)
        LogGlobalShow(state, entry, dialog, begin, value, sequence);
#endif
        const auto& identity = shell::application_identity::CurrentApplication();
        FILETIME now{}; GetSystemTimeAsFileTime(&now);
        const char* kind = !value.factory ? "unknown" : *value.factory == shell::DialogFactory::Save ? "save" : "open";
        char line[4096]{};
        const int count = std::snprintf(line, sizeof(line),
            "{\"session\":\"%lu-%llu\",\"pid\":%lu,\"event\":\"dialog_show_%s\",\"data\":{"
            "\"event_sequence\":%llu,\"app_basename\":\"%s\",\"app_image_id\":%llu,\"app_instance_id\":%llu,\"identity_revision\":2,\"creation_time_filetime\":%llu,\"event_time_filetime\":%llu,\"dialog_kind\":\"%s\","
            "\"request\":\"%llu:%llu\",\"dialog\":%llu,\"show_sequence\":%llu,\"process_arch\":%u,"
            "\"entry_family\":\"%s\",\"launch_route\":\"managed\",\"terminal_status\":\"%s\",\"hr\":%lu,"
            "\"provider_started\":%s,\"ui_shown_known\":%s,\"ui_shown\":%s,\"reason_name\":\"%s\",\"fallback_reason\":\"%s\"}}\r\n",
            GetCurrentProcessId(), state.log_session, GetCurrentProcessId(), begin ? "begin" : "terminal",
            sequence, identity.json_basename.c_str(), identity.image_id, identity.instance_id, identity.creation_time, shell::application_identity::Filetime(now), kind,
            dialog, value.sequence, dialog, value.sequence, static_cast<unsigned>(sizeof(void*) * 8), EntryFamily(entry), terminal,
            static_cast<unsigned long>(value.result), value.provider_started ? "true" : "false", value.ui_shown.has_value() ? "true" : "false",
            value.ui_shown.value_or(false) ? "true" : "false", ContractReasonName(value.contract_reason), FallbackReasonName(value.fallback));
        if (count <= 0 || count >= static_cast<int>(sizeof(line))) return;
        const auto path = state.control + L"\\runtime-dialog-shows.jsonl";
        AcquireSRWLockExclusive(&state.log_lock);
        HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0; WriteFile(file, line, static_cast<DWORD>(count), &written, nullptr); CloseHandle(file);
        }
        ReleaseSRWLockExclusive(&state.log_lock);
    } catch (...) {}
}
std::function<void(bool, const shell::FileDialogShowObservation&)> ShowObserver(Configuration* state, const char* entry) {
    static std::atomic<uint64_t> next_dialog{0};
    // Allocate on the first Show, not Services construction: a class factory
    // copies its unused services into multiple independently sequenced wrappers.
    return [state, entry, dialog = uint64_t{0}, active = static_cast<Configuration*>(nullptr)](bool begin, const shell::FileDialogShowObservation& value) mutable {
        if (begin && !dialog) dialog = ++next_dialog;
        if (begin) active = state ? state : current.load(std::memory_order_acquire);
        if (active) LogShow(*active, entry, dialog, begin, value);
        if (!begin) active = nullptr;
    };
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
    services.observe_show = ShowObserver(state, entry);
    services.executable = state->host;
#if !defined(_WIN64)
    if (state->bundle && state->bundle->IsThinCompanion()) {
        services.prefer_bridge = false;
        services.load_inprocess_provider = [state, entry] {
            std::shared_ptr<const shell::FileDialogInprocessProvider> provider;
            const HRESULT loaded = shell::LoadFileDialogRemoteProvider(state->bundle->Host(), provider, true);
            Log(*state, "thin-companion-remote-provider", static_cast<unsigned>(loaded), 0, entry);
            return SUCCEEDED(loaded) ? shell::RetainCompanionProvider(std::move(provider), state->bundle) :
                std::shared_ptr<const shell::FileDialogInprocessProvider>{};
        };
    } else
#endif
    {
        std::call_once(state->ui_once, [state, entry] {
            const HRESULT loaded = shell::LoadFileDialogUiProvider(state->host, state->ui_provider, true);
            Log(*state, "inprocess-ui-proof-load", static_cast<unsigned>(loaded), 0, entry);
        });
        services.inprocess_provider = state->ui_provider;
    }
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
    services.observe_show = ShowObserver(nullptr, entry);
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
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
    constexpr bool initializing = false; // Partially patched imports remain native until publication.
#else
    const bool initializing = initialization.load(std::memory_order_acquire) == E_PENDING;
#endif
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
decltype(&SHBrowseForFolderW) original_browse_w = &SHBrowseForFolderW;
decltype(&SHBrowseForFolderA) original_browse_a = &SHBrowseForFolderA;
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
            const bool wants_hook = input && input->lStructSize == sizeof(Ofn) &&
                (input->Flags & (OFN_ENABLEHOOK | OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE));
            shell::FileDialogBridgeServices hook_services;
            bool verified_local_hook_host = false;
            if (wants_hook) {
                hook_services = Services(state, entry);
                // Services only assigns this member after the guarded same-bitness
                // local DLL loader succeeds; thin companions use the remote loader.
                verified_local_hook_host = static_cast<bool>(hook_services.inprocess_provider) && !hook_services.load_inprocess_provider;
            }
            const shell::LegacyDialogFactory create = [state, entry, wants_hook, &hook_services](bool saving, IFileDialog** output) {
                IUnknown* native = nullptr;
                HRESULT hr = shell::CreateSystemLegacyDialog(saving, &native);
                if (FAILED(hr)) return hr;
                try { auto services = wants_hook ? hook_services : Services(state, entry);
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
            if (shell::RunLegacyComDialog(input, save, create, result, diagnostic, verified_local_hook_host)) return result;
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
template<class Input, class Original>
PIDLIST_ABSOLUTE RouteBrowse(Input* input, Original original, const char* entry) {
    auto* state = current.load(std::memory_order_acquire);
    bool enabled = state && !Disabled(*state) && TokenSafe(state->admin);
#if defined(PULSE_RUNTIME_ENTRY_TEST)
    if (state && legacy_test_services) enabled = true;
#endif
    if (state) Log(*state, enabled ? "browse-family-entered" : "browse-session-or-token-native", input ? input->ulFlags : 0, 0, entry);
    if (enabled) {
        PIDLIST_ABSOLUTE result = nullptr;
        try {
            const shell::LegacyDialogFactory create = [state, entry](bool, IFileDialog** output) {
                IUnknown* native = nullptr;
                HRESULT hr = shell::CreateSystemLegacyDialog(false, &native);
                if (FAILED(hr)) return hr;
                try {
                    auto services = Services(state, entry);
#if defined(PULSE_RUNTIME_ENTRY_TEST)
                    if (legacy_test_services) legacy_test_services(services);
#endif
                    hr = shell::WrapNativeFileDialog(shell::DialogFactory::Open, native, services,
                        IID_IFileDialog, reinterpret_cast<void**>(output));
                } catch (...) { hr = E_FAIL; }
                native->Release();
                return hr;
            };
            const shell::BrowseFolderObserver observer = [state, entry](const shell::BrowseFolderDiagnostic& value) {
                Log(*state, value.reason, value.flags, value.presence, entry);
                if (FAILED(value.error)) Log(*state, "browse-result-hresult", static_cast<unsigned>(value.error), 0, entry);
            };
            if (shell::RunBrowseFolderDialog(input, create, result, observer)) return result;
        } catch (...) { Log(*state, "browse-service-construction-native", 0, 0, entry); }
    }
    return original(input);
}
PIDLIST_ABSOLUTE WINAPI HookBrowseW(LPBROWSEINFOW input) { return RouteBrowse(input, original_browse_w, "SHBrowseForFolderW"); }
PIDLIST_ABSOLUTE WINAPI HookBrowseA(LPBROWSEINFOA input) { return RouteBrowse(input, original_browse_a, "SHBrowseForFolderA"); }
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
#if defined(PULSE_EXPERIMENTAL_WINRT_PICKER)
HRESULT WINAPI HookRoFactory(HSTRING name, REFIID iid, void** output) noexcept {
    auto* state = current.load(std::memory_order_acquire);
    if (!state || Disabled(*state)) return original_ro_factory(name, iid, output);
    try {
        shell::winrt_picker::Services bridge;
        bridge.run = [state](const wire::Message& request, HWND owner, HANDLE cancel) {
            if (Disabled(*state)) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
            return wire::RunPickerClient(state->host, request, owner, cancel);
        };
        return shell::winrt_picker::GetActivationFactory(name, iid, output, bridge, original_ro_factory);
    } catch (...) { return original_ro_factory(name, iid, output); }
}
#endif
void StartupDiagnostics(Configuration& state) noexcept {
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
    Log(state, "validated-session-provider-published", auto_mask.load());
    Log(state, "static-iat-slots-late-delay-dynamic-imports-native", auto_slots.load(), auto_skipped.load());
#else
    Log(state, "validated-session-provider-published", kCoverage);
#endif
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
[[maybe_unused]] DWORD WINAPI Initialize(void* parameter) noexcept {
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
    std::unique_ptr<config::PulseRuntimeAutoStartV1> explicit_request(static_cast<config::PulseRuntimeAutoStartV1*>(parameter));
    config::AutoImportPatch auto_import_patch;
#else
    (void)parameter;
#endif
    struct FinishInitialization {
        bool active = true;
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
        config::AutoImportPatch* imports = nullptr;
#endif
        ~FinishInitialization() {
            if (!active) return;
            HRESULT pending = E_PENDING;
            initialization.compare_exchange_strong(pending, E_FAIL);
            if (FAILED(initialization.load(std::memory_order_acquire))) {
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
                RecordAutoImports(imports->Rollback());
                install_result = ERROR_NOT_READY;
#endif
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
    } finish_initialization
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
        {true, &auto_import_patch}
#endif
        ;
    try {
        // Pin outside DllMain. There is no detach API; all captured callbacks
        // and Detours trampolines remain valid until the application exits.
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&Initialize), &pinned)) {
            initialization = HRESULT_FROM_WIN32(GetLastError()); return 0;
        }
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
        if (!explicit_request || !pulse::dialog_admin::IsElevated() || !TokenSafe(true)) {
            initialization = E_ACCESSDENIED; return 0;
        }
        const std::wstring nonce = explicit_request->nonce;
#else
        const auto nonce = Environment(L"PULSE_DIALOG_NONCE");
#endif
        GUID parsed{};
        if (nonce.size() != 38 || FAILED(CLSIDFromString(nonce.c_str(), &parsed))) return 0;
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
        const auto host = Canonical(explicit_request->host, false);
        const auto control = Canonical(explicit_request->control, true);
#else
        const auto host = Canonical(Environment(L"PULSE_DIALOG_HOST"), false);
        const auto control = Canonical(Environment(L"PULSE_DIALOG_CONTROL"), true);
#endif
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
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
        if (!admin) { initialization = E_ACCESSDENIED; return 0; }
#endif
        wchar_t module[32768]{};
        const DWORD module_count = GetModuleFileNameW(pinned, module, 32768);
        if (!module_count || module_count >= 32768) return 0;
        const auto bundle_root = std::filesystem::path(AdminDrivePath(module)).parent_path();
        const auto host_root = std::filesystem::path(AdminDrivePath(host)).parent_path();
        const bool external_host = !Same(Canonical(bundle_root.wstring(), true), Canonical(host_root.wstring(), true));
        if (admin) {
            DWORD error = pulse::dialog_admin::IsElevated() && TokenSafe(true) ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
            if (!error) error = pulse::dialog_admin::VerifyProtectedTree(bundle_root.wstring());
            if (!error && external_host) error = pulse::dialog_admin::VerifyProtectedTree(host_root.wstring());
            if (error) { initialization = HRESULT_FROM_WIN32(error); return 0; }
        }
        std::shared_ptr<pulse::dialog_bundle::BundleLease> bound_bundle;
        if (external_host || admin) {
            bound_bundle = std::make_shared<pulse::dialog_bundle::BundleLease>();
            DWORD error = pulse::dialog_bundle::VerifyBundle(bundle_root.wstring(), {}, *bound_bundle);
            if (!error && ((external_host && !bound_bundle->IsThinCompanion()) || bound_bundle->Machine() != machine ||
                !Same(Canonical(bound_bundle->Host(), false), host) ||
                !Same(Canonical(bound_bundle->Launcher(), false), Canonical(session->launcher_path, false)) ||
                !Same(Canonical(
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
                    bound_bundle->AutoRuntime(),
#else
                    bound_bundle->Runtime(),
#endif
                    false), Canonical(module, false)))) error = ERROR_INVALID_DATA;
            if (error) { initialization = HRESULT_FROM_WIN32(error); return 0; }
        }
        if (admin) {
            DWORD error = pulse::dialog_admin::IsElevated() ? ERROR_SUCCESS : ERROR_ELEVATION_REQUIRED;
            if (!error && !TokenSafe(true)) error = ERROR_ACCESS_DENIED;
            if (!error) error = pulse::dialog_admin::VerifyProtectedTree(bundle_root.wstring());
            if (!error && external_host) error = pulse::dialog_admin::VerifyProtectedTree(host_root.wstring());
            if (!error) error = pulse::dialog_admin::VerifyProtectedPath(AdminDrivePath(control), true);
            if (!error) error = pulse::dialog_admin::VerifyProtectedPath(AdminDrivePath(std::filesystem::path(control).parent_path().wstring()), true);
            if (!error) error = pulse::dialog_admin::VerifyProtectedPath(AdminDrivePath(control + L"\\" + config::kSessionFile), false);
            if (error) { initialization = HRESULT_FROM_WIN32(error); return 0; }
        }
        auto state = std::make_unique<Configuration>();
        state->host = host; state->control = control; state->admin = admin;
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
        const DWORD lease_error = state->auto_lease.Open(AdminDrivePath(control), nonce, auto_require_binding.load());
        if (lease_error) { initialization = HRESULT_FROM_WIN32(lease_error); return 0; }
        config::AutoModuleEpoch module_epoch;
        if (!module_epoch.Start()) { initialization = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); return 0; }
#endif
        state->bundle = std::move(bound_bundle);
        state->directory = CreateFileW(control.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (state->directory == INVALID_HANDLE_VALUE) return 0;
        const auto runtime_dll = DllAnsiPath(pinned);
        config::Ready ready;
        ready.target_pid = session->target_pid; ready.machine = machine;
        ready.target_created = created;
        wcscpy_s(ready.nonce, session->nonce);
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
        // All session, token, bundle and path checks precede atomic data-slot writes.
        const std::array<config::AutoImportHook, 11> hooks{{
            {reinterpret_cast<void*>(original_create), reinterpret_cast<void*>(&HookCreate)},
            {reinterpret_cast<void*>(original_ex), reinterpret_cast<void*>(&HookEx)},
            {reinterpret_cast<void*>(original_class), reinterpret_cast<void*>(&HookClass)},
#if defined(PULSE_EXPERIMENTAL_WINRT_PICKER)
            {reinterpret_cast<void*>(original_ro_factory), reinterpret_cast<void*>(&HookRoFactory)},
#else
            {},
#endif
            {reinterpret_cast<void*>(original_open_w), reinterpret_cast<void*>(&ObserveOpenW)},
            {reinterpret_cast<void*>(original_save_w), reinterpret_cast<void*>(&ObserveSaveW)},
            {reinterpret_cast<void*>(original_open_a), reinterpret_cast<void*>(&ObserveOpenA)},
            {reinterpret_cast<void*>(original_save_a), reinterpret_cast<void*>(&ObserveSaveA)},
            {reinterpret_cast<void*>(original_dialog_error), reinterpret_cast<void*>(&HookDialogError)},
            {reinterpret_cast<void*>(original_browse_w), reinterpret_cast<void*>(&HookBrowseW)},
            {reinterpret_cast<void*>(original_browse_a), reinterpret_cast<void*>(&HookBrowseA)}
        }};
        const auto permit = [](void* context) noexcept { return static_cast<Configuration*>(context)->auto_lease.Alive(); };
        const auto imports = auto_import_patch.Install(pinned, hooks, permit, state.get());
        RecordAutoImports(imports);
        if (FAILED(imports.status) || !imports.patched_slots || !imports.api_mask) {
            initialization = FAILED(imports.status) ? imports.status : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
            return 0;
        }
        install_result = NO_ERROR;
#endif
        const LONG installed = install_result.load();
        ready.status = installed == NO_ERROR ? S_OK : HRESULT_FROM_WIN32(installed);
        ready.installed_entries = installed == NO_ERROR ?
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
            auto_mask.load() : 0;
#else
            kCoverage : 0;
#endif
        PendingConfigurationPublication publication;
        Configuration* published = nullptr;
        bool child_configured = false;
        if (installed == NO_ERROR) {
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
            if (!state->auto_lease.Alive()) { initialization = HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED); return 0; }
#endif
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
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
        if (published && !published->auto_lease.Alive()) { initialization = HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED); return 0; }
#endif
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
            initialization = HRESULT_FROM_WIN32(GetLastError());
            return 0; // publication guard revokes memory and parent-chain authority.
        }
        publication.Commit();
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
        auto_import_patch.Commit();
        finish_initialization.active = false;
#endif
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
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
        if (published) {
            const auto enabled = [](void* context) noexcept { return !Disabled(*static_cast<Configuration*>(context)); };
            const auto revoke = [](void* context) noexcept {
                auto* value = static_cast<Configuration*>(context);
                value->routing_enabled.store(false, std::memory_order_release);
                Configuration* expected = value; current.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
            };
            const auto restored = config::MonitorAutoImports(pinned, hooks, auto_import_patch, module_epoch,
                permit, published, enabled, revoke, RecordAutoImports);
            install_result = ERROR_NOT_READY; initialization = restored.status;
            const bool rebound = !restored.api_mask && restored.protection_restored;
            Log(*published, rebound ? "auto-lease-ended-slots-restored-ready-for-rebind" :
                "auto-restore-incomplete-native-restart-target", restored.patched_slots, 0, "auto-runtime");
            if (rebound) auto_started.store(false, std::memory_order_release);
        }
#endif
    } catch (...) { initialization = E_FAIL; }
    return 0;
}
}
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
namespace {
bool ValidAutoRequest(const config::PulseRuntimeAutoStartV1* request, uint32_t size) noexcept {
    if (!request || size != sizeof(*request) || request->size != size || request->version != 1 ||
        request->nonce[38] || request->nonce[39] || wcsnlen_s(request->nonce, 40) != 38 ||
        !request->host[0] || !request->control[0] || request->host[config::kPathUnits - 1] ||
        request->control[config::kPathUnits - 1]) return false;
    GUID parsed{};
    return SUCCEEDED(CLSIDFromString(request->nonce, &parsed));
}
bool CopyAutoRequest(const void* source, config::PulseRuntimeAutoStartV1& output) noexcept {
    __try {
        if (!source || static_cast<const config::PulseRuntimeAutoStartV1*>(source)->size != sizeof(output)) return false;
        std::memcpy(&output, source, sizeof(output));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
static HRESULT QueueAutoStart(const void* request, uint32_t size, bool require_binding) noexcept {
    if (!request || size != sizeof(config::PulseRuntimeAutoStartV1)) return E_INVALIDARG;
    try {
        auto copied = std::make_unique<config::PulseRuntimeAutoStartV1>();
        if (!CopyAutoRequest(request, *copied) || !ValidAutoRequest(copied.get(), size)) return E_INVALIDARG;
    if (auto_started.load(std::memory_order_acquire)) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    if (!pulse::dialog_admin::IsElevated() || !TokenSafe(true)) return E_ACCESSDENIED;
        HMODULE pinned = nullptr;
        if (!auto_module) return E_UNEXPECTED;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&PulseFileDialogRuntimeAutoStart), &pinned)) return HRESULT_FROM_WIN32(GetLastError());
        bool expected = false;
        if (!auto_started.compare_exchange_strong(expected, true)) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        initialization.store(E_PENDING, std::memory_order_release);
        auto_require_binding.store(require_binding, std::memory_order_release);
        auto* payload = copied.release();
        HANDLE worker = CreateThread(nullptr, 0, Initialize, payload, 0, nullptr);
        if (!worker) { const auto error = HRESULT_FROM_WIN32(GetLastError()); delete payload; initialization = error; return error; }
        CloseHandle(worker);
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}
extern "C" __declspec(dllexport) HRESULT WINAPI PulseFileDialogRuntimeAutoStart(
    const config::PulseRuntimeAutoStartV1* request, uint32_t size) noexcept { return QueueAutoStart(request, size, false); }
extern "C" __declspec(dllexport) HRESULT WINAPI PulseFileDialogRuntimeAutoAttach(void* request) noexcept {
    return QueueAutoStart(request, sizeof(config::PulseRuntimeAutoStartV1), true);
}
extern "C" __declspec(dllexport) HRESULT WINAPI PulseFileDialogRuntimeAutoQuery(
    config::PulseRuntimeAutoCoverageV1* output, uint32_t size) noexcept {
    if (!output || size != sizeof(*output)) return E_INVALIDARG;
    config::PulseRuntimeAutoCoverageV1 value;
    value.api_mask = auto_mask.load(); value.patched_slots = auto_slots.load();
    value.skipped_modules = auto_skipped.load(); value.foreign_slots = auto_foreign.load();
    value.protection_restored = auto_protection_restored.load();
    *output = value;
    return S_OK;
}
#endif
extern "C" HRESULT WINAPI PulseFileDialogRuntimeQuery(PulseFileDialogRuntimeStatus* output, uint32_t size) noexcept {
    if (!output || size != sizeof(*output)) return E_INVALIDARG;
    PulseFileDialogRuntimeStatus status;
    status.process_id = GetCurrentProcessId();
    status.installed_entries = install_result == NO_ERROR ?
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
        auto_mask.load() : 0;
#else
        kCoverage : 0;
#endif
    status.initialization = initialization.load();
    const auto* state = current.load(std::memory_order_acquire);
    status.ready = state && !Disabled(*state);
    status.activations = activations.load(); status.admitted = admitted.load(); status.rejected = rejected.load();
    *output = status;
    return S_OK;
}
#ifndef PULSE_RUNTIME_ENTRY_TEST
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
    if (reason == DLL_PROCESS_ATTACH) { auto_module = instance; DisableThreadLibraryCalls(instance); }
    return TRUE;
#else
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
#if defined(PULSE_EXPERIMENTAL_WINRT_PICKER)
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_ro_factory), reinterpret_cast<PVOID>(HookRoFactory));
#endif
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&config::original_create_process), reinterpret_cast<PVOID>(config::HookCreateProcessW));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_open_w), reinterpret_cast<PVOID>(ObserveOpenW));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_save_w), reinterpret_cast<PVOID>(ObserveSaveW));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_open_a), reinterpret_cast<PVOID>(ObserveOpenA));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_save_a), reinterpret_cast<PVOID>(ObserveSaveA));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_dialog_error), reinterpret_cast<PVOID>(HookDialogError));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_browse_w), reinterpret_cast<PVOID>(HookBrowseW));
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&original_browse_a), reinterpret_cast<PVOID>(HookBrowseA));
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
#endif
}

#endif


#include "dialog_application_identity.h"
#include "dialog_core_abi.h"
#include "dialog_sink_hooks.h"
#include "direct_reveal_hook.h"
#include "explorer_process_route.h"
#include "../common/dialog_bridge_state.h"
#if defined(PULSE_EXPERIMENTAL_WINRT_PICKER)
#include "winrt_picker_activation.h"
#include "../ipc/picker_client.h"
#endif
#include "../common/process_token_restriction.h"
#include "file_dialog_class_factory.h"
#include "file_dialog_generation.h"
#include "file_dialog_runtime.h"
#include "file_dialog_diagnostic.h"
#include "file_dialog_token_diagnostics.h"
#include "file_dialog_ui_provider.h"
#include "file_dialog_remote_provider.h"
#include "file_dialog_companion_provider.h"
#include "browse_folder_adapter.h"
#include "legacy_com_provider.h"
#include "legacy_dialog_error.h"
#include "../common/dialog_auto_registration.h"
#include "../common/json_value.h"
#include "../common/runtime_log.h"
#include "../common/dialog_admin_trust.h"
#include <atomic>
#include <shellapi.h>
#include <shlobj.h>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <wrl/client.h>
#include <rpc.h>
#include <rpcasync.h>

namespace {
namespace shell = pulse::shell_bridge;
namespace log = pulse::diagnostics::runtime;
namespace td = pulse::shell_bridge::token_diagnostics;
using Microsoft::WRL::ComPtr;
HMODULE module;
shell::GenerationAdmission generation_admission;
std::atomic<bool> stub_managed{false};
SRWLOCK stub_config_lock = SRWLOCK_INIT;
std::wstring stub_bridge_root;
std::wstring stub_host_exe;
uint64_t stub_generation = 0;
std::mutex stub_pointer_mutex;
ULONGLONG stub_pointer_checked_tick = 0;
bool stub_pointer_enabled = true;
std::atomic<LONG> live_com_objects{0};

struct ComObjectRef {
    ComObjectRef() noexcept { live_com_objects.fetch_add(1, std::memory_order_acq_rel); }
    ~ComObjectRef() noexcept { live_com_objects.fetch_sub(1, std::memory_order_acq_rel); }
    ComObjectRef(const ComObjectRef&) = delete;
    ComObjectRef& operator=(const ComObjectRef&) = delete;
};

bool UserExplicitlyDisabledDialogAuto() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, pulse::dialog_auto::ConfigKey, 0,
                      KEY_QUERY_VALUE | pulse::dialog_auto::RegistryView, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0, value = 1, size = sizeof(value);
    const LSTATUS read = RegQueryValueExW(key, L"Enabled", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(key);
    return read == ERROR_SUCCESS && type == REG_DWORD && size == sizeof(value) && value == 0;
}

std::wstring Utf8ToWideLocal(std::string_view input) {
    if (input.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), out.data(), needed);
    return out;
}

bool StubBridgeEnabled(bool force_refresh = false) noexcept;

std::wstring StubHostExe() {
    AcquireSRWLockShared(&stub_config_lock);
    auto copy = stub_host_exe;
    ReleaseSRWLockShared(&stub_config_lock);
    if (stub_managed.load(std::memory_order_acquire) &&
        (copy.empty() || GetFileAttributesW(copy.c_str()) == INVALID_FILE_ATTRIBUTES)) {
        (void)StubBridgeEnabled(true);
        AcquireSRWLockShared(&stub_config_lock);
        copy = stub_host_exe;
        ReleaseSRWLockShared(&stub_config_lock);
    }
    return copy;
}

bool StubBridgeEnabled(bool force_refresh) noexcept {
    try {
        std::wstring root;
        AcquireSRWLockShared(&stub_config_lock);
        root = stub_bridge_root;
        ReleaseSRWLockShared(&stub_config_lock);
        if (root.empty()) return true;

        std::lock_guard<std::mutex> lock(stub_pointer_mutex);
        const ULONGLONG now = GetTickCount64();
        if (!force_refresh && stub_pointer_checked_tick != 0 && (now - stub_pointer_checked_tick) < 250) {
            return stub_pointer_enabled;
        }
        stub_pointer_checked_tick = now;

        const std::wstring path = root + L"\\active-bridge.json";
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) stub_pointer_enabled = false;
            return stub_pointer_enabled;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        LARGE_INTEGER size{};
        const bool ok_meta = GetFileInformationByHandle(file, &info) && GetFileSizeEx(file, &size) &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            size.QuadPart > 0 && size.QuadPart <= 65536;
        std::string raw;
        bool ok_read = false;
        if (ok_meta) {
            raw.resize(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            ok_read = ReadFile(file, raw.data(), static_cast<DWORD>(raw.size()), &read, nullptr) && read == raw.size();
        }
        CloseHandle(file);
        if (!ok_read) {
            stub_pointer_enabled = false;
            return false;
        }
        pulse::json::Value doc;
        int64_t schema = 0;
        if (!pulse::json::Parse(raw, doc) || !doc.is_object()) {
            stub_pointer_enabled = false;
            return false;
        }
        const auto* schema_v = doc.find("schema");
        if (!schema_v || !schema_v->get_int(schema) || schema != 1) {
            stub_pointer_enabled = false;
            return false;
        }
        const auto* disabled = doc.find("disabled");
        stub_pointer_enabled = !(disabled && disabled->is_bool() && disabled->as_bool());
        if (stub_pointer_enabled) {
            if (const auto* host = doc.find("host_exe"); host && host->is_string() && !host->as_string().empty()) {
                const std::wstring candidate = Utf8ToWideLocal(host->as_string());
                if (candidate.size() >= 4 && candidate[1] == L':' &&
                    (candidate[2] == L'\\' || candidate[2] == L'/') &&
                    candidate.find(L"..") == std::wstring::npos) {
                    bool trust_ok = true;
                    if (pulse::dialog_admin::IsElevated()) {
                        const auto slash = candidate.find_last_of(L"\\/");
                        trust_ok = slash != std::wstring::npos &&
                            pulse::dialog_admin::VerifyProtectedTree(candidate.substr(0, slash)) == ERROR_SUCCESS;
                    }
                    if (trust_ok) {
                        AcquireSRWLockExclusive(&stub_config_lock);
                        stub_host_exe = candidate;
                        ReleaseSRWLockExclusive(&stub_config_lock);
                    }
                }
            }
        }
        return stub_pointer_enabled;
    } catch (...) {
        return false;
    }
}

bool GenerationEnabled(const std::wstring& own, const pulse::dialog_auto::Status& status) {
    if (stub_managed.load(std::memory_order_acquire)) {
        // Stub owns activation via active-bridge.json; honor live disabled:true updates
        // and explicit per-user HKCU opt-out (Enabled == 0), without letting unrelated
        // legacy HKCU conflicts block Stub+Core.
        return StubBridgeEnabled() && !UserExplicitlyDisabledDialogAuto();
    }
    if (!generation_admission.Enabled(own, status)) return false;
    // Worker waits pump messages; disabling or switching registration during
    // that interval must revoke this request before any provider is selected.
    const auto current = pulse::dialog_auto::Inspect();
    return current.enabled && !current.conflict && !current.error && current.dll_path == status.dll_path;
}
constexpr uint64_t kDiagnosticsRevision = 52;
std::wstring OwnPath() {
    wchar_t path[32768]{};
    const DWORD n = GetModuleFileNameW(module, path, 32768);
    return n && n < 32768 ? std::wstring(path, n) : std::wstring();
}
// Read mapped-image identity, not the on-disk file which an update may replace.
void LogModule(const char* event, HMODULE handle) noexcept {
    wchar_t path[32768]{};
    SetLastError(ERROR_SUCCESS);
    const DWORD length = handle ? GetModuleFileNameW(handle, path, 32768) : 0;
    const DWORD error = length && length < 32768 ? ERROR_SUCCESS :
        (handle ? GetLastError() : ERROR_MOD_NOT_FOUND);
    DWORD timestamp = 0, image_size = 0;
    if (handle) {
        const auto* base = reinterpret_cast<const BYTE*>(handle);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        timestamp = nt->FileHeader.TimeDateStamp;
        image_size = nt->OptionalHeader.SizeOfImage;
    }
    log::Event(event, {{"error", error}, {"pe_timestamp", timestamp}, {"image_size", image_size}});
}
auto LogRegistration(uint64_t dialog) {
    const auto status = pulse::dialog_auto::Inspect();
    const auto own = OwnPath();
    log::Event("registration_snapshot", {{"dialog", dialog}, {"enabled", status.enabled}, {"conflict", status.conflict},
         {"error", status.error}, {"path_match", status.dll_path == own}});
    return status;
}
void LogCallContext() noexcept {
    const DWORD saved = GetLastError();
    ComPtr<IServerSecurity> security;
    const HRESULT context = CoGetCallContext(IID_PPV_ARGS(&security));
    DWORD authn = 0, level = 0, capabilities = 0;
    const HRESULT blanket = CoQueryClientBlanket(&authn, nullptr, nullptr, &level, nullptr, nullptr, &capabilities);
    log::Event("com_call_context", {{"context_hr", static_cast<unsigned>(context)}, {"blanket_hr", static_cast<unsigned>(blanket)},
        {"context_present", context == S_OK && security}, {"impersonating", security && security->IsImpersonating()},
        {"authn_svc", authn}, {"authn_level", level}, {"capabilities", capabilities}});
    using Query = RPC_STATUS (RPC_ENTRY*)(RPC_BINDING_HANDLE, void*);
    const auto rpc_module = GetModuleHandleW(L"Rpcrt4.dll");
    const auto query = rpc_module ? reinterpret_cast<Query>(GetProcAddress(rpc_module, "RpcServerInqCallAttributesW")) : nullptr;
    RPC_CALL_ATTRIBUTES_V2_W attributes{};
    attributes.Version = 2; attributes.Flags = RPC_QUERY_CLIENT_PID;
    const RPC_STATUS rpc_status = query ? query(nullptr, &attributes) : ERROR_PROC_NOT_FOUND;
    const bool pid_known = rpc_status == RPC_S_OK && attributes.ClientPID;
    log::Event("rpc_call_context", {{"rpc_status", static_cast<unsigned>(rpc_status)}, {"client_pid_known", pid_known},
        {"client_pid", pid_known ? reinterpret_cast<uintptr_t>(attributes.ClientPID) : 0},
        {"protocol", rpc_status == RPC_S_OK ? attributes.ProtocolSequence : 0}});
    SetLastError(saved);
}
std::wstring DiagnosticText(const char* text) {
    std::wstring result;
    if (text) for (size_t i = 0; text[i] && i < 2048; ++i) result.push_back(static_cast<unsigned char>(text[i]));
    return result;
}
const wchar_t* ContractReason(shell::NativeReason reason) noexcept {
    static constexpr const wchar_t* names[] = {L"None", L"SetterFailed", L"UnrecognizedSuccess", L"UnknownSetting",
        L"UnsupportedOptions", L"FilesystemNotForced", L"RecentItemsPolicy", L"MissingValidationOptions",
        L"DefaultDirectoryPolicy", L"NonFilesystemFolder", L"UnsupportedFilename", L"UnsupportedExtension",
        L"UnsupportedFilter", L"UnsupportedSettingOrder", L"FolderFileSettings", L"SaveContract", L"Events",
        L"Customization", L"Labels", L"Places", L"Persistence", L"SaveMetadata", L"ShellFilter", L"InvalidData", L"Limit"};
    const auto index = static_cast<size_t>(reason);
    return index < std::size(names) ? names[index] : L"Unknown";
}
// These additional token queries are observations, never admission checks.
void ObserveTokenTypes(HANDLE token, td::Evidence& evidence) noexcept {
    const DWORD saved = GetLastError();
    DWORD size = 0;
    if (!GetTokenInformation(token, TokenElevationType, &evidence.elevation_type, sizeof(evidence.elevation_type), &size)) {
        evidence.elevation_type_error = GetLastError(); evidence.elevation_type = 0;
    }
    if (!GetTokenInformation(token, TokenType, &evidence.token_type, sizeof(evidence.token_type), &size)) {
        evidence.token_type_error = GetLastError(); evidence.token_type = 0;
    }
    SetLastError(saved);
}
void LogCallerSecurityContext() noexcept {
    const DWORD saved = GetLastError();
    td::Evidence evidence;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        evidence.reason = 3; evidence.error = GetLastError();
    } else {
        TOKEN_ELEVATION elevation{}; DWORD size = 0;
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
            evidence.elevated_known = true; evidence.elevated = elevation.TokenIsElevated != 0;
        } else { evidence.reason = 4; evidence.error = GetLastError(); }
        ObserveTokenTypes(token, evidence);
        CloseHandle(token);
    }
    td::LogToken("caller_security_context", td::Stage::Initialize, evidence, log::Level::Info,
        evidence.reason ? td::ReasonName(evidence.reason) : L"PROCESS_TOKEN_OBSERVED");
    SetLastError(saved);
}
bool TokenSafe(td::Stage stage = td::Stage::Activation, td::Evidence* observed = nullptr) noexcept {
    td::Evidence evidence;
    auto reject = [&](unsigned reason, DWORD error = ERROR_SUCCESS) {
        evidence.reason = reason; evidence.error = error;
        if (observed) *observed = evidence;
        td::LogToken("token_rejected", stage, evidence, log::Level::Warning);
        return false;
    };
    HANDLE token = nullptr;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) { CloseHandle(token); return reject(1); }
    const DWORD thread_error = GetLastError();
    if (thread_error != ERROR_NO_TOKEN) return reject(2, thread_error);
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return reject(3, GetLastError());
    struct Close { HANDLE h; ~Close() { CloseHandle(h); } } close{token};
    TOKEN_ELEVATION elevation{}; DWORD size = 0, container = 0;
    if (!GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) return reject(4, GetLastError());
    evidence.elevated_known = true; evidence.elevated = elevation.TokenIsElevated != 0;
    ObserveTokenTypes(token, evidence);
    if (elevation.TokenIsElevated) {
        DWORD trust_error = ERROR_NOT_ENOUGH_MEMORY;
        try {
            const auto own = OwnPath();
            const auto separator = own.find_last_of(L"\\/");
            trust_error = separator == std::wstring::npos ? ERROR_BAD_PATHNAME :
                pulse::dialog_admin::VerifyProtectedTree(own.substr(0, separator));
        } catch (...) {}
        evidence.trust_checked = true; evidence.trust_error = trust_error;
        td::LogGate("elevated_bundle_trust", stage, trust_error ? L"ELEVATED_BUNDLE_UNTRUSTED" : L"TRUSTED",
            {{"error", trust_error}, {"checked", 1}, {"accepted", trust_error == ERROR_SUCCESS}});
        if (trust_error) return reject(5, trust_error);
    }
    const DWORD restricted = pulse::security::CheckUnrestrictedToken(token);
    if (restricted != ERROR_SUCCESS) return reject(6, restricted);
    if (!GetTokenInformation(token, TokenIsAppContainer, &container, sizeof(container), &size)) return reject(7, GetLastError());
    if (container) return reject(8);
    alignas(TOKEN_MANDATORY_LABEL) BYTE buffer[1024]{};
    if (!GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size)) return reject(9, GetLastError());
    const auto sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer)->Label.Sid;
    if (!IsValidSid(sid)) return reject(10);
    const auto count = *GetSidSubAuthorityCount(sid);
    if (!count || *GetSidSubAuthority(sid, count - 1) < SECURITY_MANDATORY_MEDIUM_RID) return reject(11);
    if (observed) *observed = evidence;
    td::LogToken("token_accepted", stage, evidence);
    return true;
}
bool DirectRevealEnabled() noexcept {
    try {
        if (stub_managed.load(std::memory_order_acquire) && !StubBridgeEnabled()) return false;
        return TokenSafe(td::Stage::Activation);
    }
    catch (...) { return false; }
}
std::atomic<HRESULT> direct_reveal_folder_status{E_PENDING};
DWORD WINAPI StartDirectReveal(void* diagnostic) noexcept {
    LONG status = ERROR_GEN_FAILURE;
    for (unsigned attempt = 0; attempt != 3; ++attempt) {
        if (attempt) Sleep(attempt * 50);
        status = shell::runtime::InstallDirectRevealHook(module, DirectRevealEnabled,
            [](const char* entry, unsigned result) noexcept {
                const unsigned id = std::strcmp(entry, "ShellExecuteW") == 0 ? 1u :
                    std::strcmp(entry, "ShellExecuteExW") == 0 ? 2u :
                    std::strcmp(entry, "ShellExecuteA") == 0 ? 4u :
                    std::strcmp(entry, "ShellExecuteExA") == 0 ? 5u : 3u;
                log::Event("direct_reveal_call", {{"entry", id}, {"result", result}});
            }, &shell::runtime::DefaultDirectRevealSend, static_cast<pulse::DirectRevealInstallDiagnostic*>(diagnostic),
            &shell::runtime::DefaultDirectOpenFolderSend);
        log::Event("direct_reveal_hook", {{"status", static_cast<unsigned>(status)}, {"attempt", attempt + 1}});
        // Thread exit between snapshot and transaction, or another short-lived
        // Detours transaction, can invalidate an otherwise eligible install.
        if (status != ERROR_INVALID_PARAMETER && status != ERROR_INVALID_HANDLE &&
            status != ERROR_BUSY && status != ERROR_INVALID_OPERATION) break;
    }
    return static_cast<DWORD>(status);
}
void PinModule() {
    if (stub_managed.load(std::memory_order_acquire)) return;
    static std::once_flag pin_once;
    std::call_once(pin_once, [] {
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&module), &pinned)) throw HRESULT_FROM_WIN32(GetLastError());
    });
}
void Initialize(bool queue_reveal = true) {
    static std::once_flag once;
    std::call_once(once, [queue_reveal] {
        PinModule();
        PWSTR root = nullptr;
        const HRESULT folder_status = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &root);
        direct_reveal_folder_status.store(folder_status);
        if (SUCCEEDED(folder_status)) {
            log::Initialize(std::wstring(root) + L"\\Pulse", "dialog_auto");
            const auto& identity = shell::application_identity::CurrentApplication();
            log::TextEvent("dialog_identity", {{"app_basename", identity.basename}},
                {{"app_image_id", identity.image_id}, {"app_instance_id", identity.instance_id},
                 {"creation_time_filetime", identity.creation_time}, {"identity_revision", 2},
                 {"process_arch", sizeof(void*) * 8}, {"diagnostics_revision", kDiagnosticsRevision}});
            LogModule("loaded_bridge", module);
            LogModule("caller_process", GetModuleHandleW(nullptr));
            LogCallerSecurityContext();
            CoTaskMemFree(root);
        }
        // Never patch executable pages inside DllMain or the caller's loader lock.
        // The pinned worker owns installation; failure leaves the shell native.
        // In stub-managed Core mode, DirectReveal is owned by InstallCoreSinkHooks
        // and cleanly uninstalled by CoreShutdown before FreeLibrary.
        if (queue_reveal && !stub_managed.load(std::memory_order_acquire)) {
            HANDLE worker = CreateThread(nullptr, 0, StartDirectReveal, nullptr, 0, nullptr);
            if (worker) CloseHandle(worker);
        }
    });
}
HRESULT WINAPI Native(REFCLSID clsid, DWORD, LPVOID, REFIID iid, void** output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    using Entry = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
    static Entry entry = []() -> Entry {
        wchar_t path[32768]{};
        const UINT n = GetSystemDirectoryW(path, 32768);
        if (!n || n >= 32768) return nullptr;
        const auto dll = LoadLibraryExW((std::wstring(path, n) + L"\\comdlg32.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return dll ? reinterpret_cast<Entry>(GetProcAddress(dll, "DllGetClassObject")) : nullptr;
    }();
    shell::sink_hooks::ScopedNativeComDlgBypass bypass;
    return entry ? entry(clsid, iid, output) : CLASS_E_CLASSNOTAVAILABLE;
}
std::shared_ptr<const shell::FileDialogInprocessProvider> original_provider;
HRESULT WINAPI LoggedShow(PulsePickerUiV2Session session, const PulsePickerUiV2Config* config,
    const PulsePickerUiV2Callbacks* callbacks, PulsePickerUiV2Result* result) {
    const auto request = log::NextId();
    log::Event("pulse_show", {{"request", request}});
    const auto hr = original_provider->api.Show(session, config, callbacks, result);
    log::Event("pulse_result", {{"request", request}, {"status", result ? result->status : 0}, {"hr", static_cast<unsigned>(hr)}, {"flags", result ? result->flags : 0},
        {"error", result ? result->win32_error : 0}});
    return hr;
}
HRESULT Provider(const std::wstring& host, std::shared_ptr<const shell::FileDialogInprocessProvider>& output) {
    static std::mutex mutex;
    static std::shared_ptr<const shell::FileDialogInprocessProvider> cached;
    std::lock_guard<std::mutex> guard(mutex);
    if (cached) { log::Event("provider_cache", {{"reused", 1}}); LogModule("loaded_picker", GetModuleHandleW(L"PulsePickerUiProof.dll")); output = cached; return S_OK; }
    std::shared_ptr<const shell::FileDialogInprocessProvider> loaded;
    log::Event("provider_load");
    const auto hr = shell::LoadFileDialogUiProvider(host, loaded, true);
    LogModule("loaded_picker", GetModuleHandleW(L"PulsePickerUiProof.dll"));
    if (FAILED(hr) || !loaded) return FAILED(hr) ? hr : E_FAIL;
    auto decorated = std::make_shared<shell::FileDialogInprocessProvider>(*loaded);
    original_provider = loaded;
    decorated->api.Show = LoggedShow;
    cached = decorated;
    output = cached;
    return S_OK;
}
// The complete V2 provider backed by pulse.exe --picker-remote-host: events,
// veto, custom controls and properties without loading the UI into this
// process. Per STA thread (the remote session is thread-affine), reused while
// any dialog of this thread still holds it, so its authenticated host is too.
HRESULT RemoteProvider(const std::wstring& host, std::shared_ptr<const shell::FileDialogInprocessProvider>& output) {
    static thread_local std::weak_ptr<const shell::FileDialogInprocessProvider> cached;
    if (auto alive = cached.lock()) { log::Event("remote_provider_cache", {{"reused", 1}}); output = std::move(alive); return S_OK; }
    std::shared_ptr<const shell::FileDialogInprocessProvider> loaded;
    log::Event("remote_provider_load");
    shell::RemoteProviderTimings timings{};
    const HRESULT hr = shell::LoadFileDialogRemoteProvider(host, loaded, true, &timings);
    log::Event("remote_provider", {
        {"hr", static_cast<unsigned>(hr)},
        {"features", loaded ? loaded->api.implemented_features : 0},
        {"host_pid", timings.host_pid},
        {"spawn_ms", timings.spawn_ms},
        {"accept_ms", timings.accept_ms},
        {"hello_ms", timings.hello_ms},
        {"total_ms", timings.total_ms}});
    if (FAILED(hr) || !loaded) return FAILED(hr) ? hr : E_FAIL;
    cached = loaded;
    output = std::move(loaded);
    return S_OK;
}
#if !defined(_WIN64)
DWORD AutomaticBundle(const std::wstring& own, std::shared_ptr<pulse::dialog_bundle::BundleLease>& output) {
    static std::mutex mutex;
    static std::shared_ptr<pulse::dialog_bundle::BundleLease> cached;
    std::lock_guard<std::mutex> guard(mutex);
    if (cached) { output = cached; return ERROR_SUCCESS; }
    auto lease = std::make_shared<pulse::dialog_bundle::BundleLease>();
    const auto slash = own.find_last_of(L"\\/");
    if (slash == own.npos) return ERROR_BAD_PATHNAME;
    const DWORD error = pulse::dialog_bundle::VerifyBundle(own.substr(0, slash), {}, *lease);
    if (error) return error;
    if (lease->Machine() != IMAGE_FILE_MACHINE_I386) return ERROR_EXE_MACHINE_TYPE_MISMATCH;
    if (lease->IsThinCompanion() && pulse::dialog_admin::IsElevated()) {
        const auto& host = lease->Host();
        const DWORD trust = pulse::dialog_admin::VerifyProtectedTree(host.substr(0, host.find_last_of(L"\\/")));
        if (trust) return trust;
    }
    cached = lease; output = std::move(lease); return ERROR_SUCCESS;
}
#endif
void ObserveNativeReason(const char* reason, const IID* iid, uint64_t dialog = 0) {
    log::TextEvent("bridge_trace", {{"detail", DiagnosticText(reason)}}, {{"dialog", dialog}});
    const auto diagnostic = shell::ParseSetterDiagnostic(reason ? reason : "");
    const auto& v = diagnostic.values;
    using Kind = shell::SetterDiagnostic::Kind;
    switch (diagnostic.kind) {
    case Kind::Extension:
        log::Event("extension", {{"seq", v[0]}, {"available", v[1]}, {"units", v[2]}, {"redacted", v[3]}, {"truncated", v[4]}}); break;
    case Kind::Chunk:
        log::Event("extension_utf16", {{"seq", v[0]}, {"offset", v[1]}, {"units", diagnostic.units},
            {"u0", v[2]}, {"u1", v[3]}, {"u2", v[4]}, {"u3", v[5]}, {"u4", v[6]}, {"u5", v[7]}, {"u6", v[8]}, {"u7", v[9]}}); break;
    case Kind::Filter:
        log::Event("filter", {{"seq", v[0]}, {"index", v[1]}, {"units", v[2]}, {"star", v[3]}, {"all", v[4]}, {"semicolon", v[5]}}); break;
    case Kind::Shape:
        log::Event("filter_shape", {{"seq", v[0]}, {"index", v[1]}, {"extension_stars", v[2]}, {"questions", v[3]}, {"forbidden", v[4]}}); break;
    case Kind::Text:
        log::Event("text_snapshot", {{"setting", v[0]}, {"null", v[1]}, {"units", v[2]}, {"captured", v[3]}, {"hr", v[4]}}); break;
    case Kind::Prominent:
        log::Event("custom_prominent", {{"id", v[0]}, {"kind", v[1]}, {"parent", v[2]}, {"nested", v[3]}, {"capture", v[4]}, {"feature", v[5]}, {"hr", v[6]}}); break;
    default: break;
    }
    uint64_t hash = 14695981039346656037ull;
    if (reason) for (const char* p = reason; *p; ++p) { hash ^= static_cast<unsigned char>(*p); hash *= 1099511628211ull; }
    log::Event("native_reason", {{"reason_hash", hash}});
    if (iid) {
        uint64_t tail = 0;
        for (unsigned i = 0; i < 8; ++i) tail = (tail << 8) | iid->Data4[i];
        log::Event("queried_interface", {{"data1", iid->Data1}, {"data2", iid->Data2}, {"data3", iid->Data3}, {"data4", tail}});
    }
    const char* marker = reason ? std::strstr(reason, " hr=0x") : nullptr;
    if (marker) {
        unsigned long hr = 0;
        if (sscanf_s(marker, " hr=0x%lx", &hr) == 1) {
            uint64_t stage = 14695981039346656037ull;
            for (const char* p = reason; p != marker; ++p) { stage ^= static_cast<unsigned char>(*p); stage *= 1099511628211ull; }
            log::Event("call_result", {{"stage_hash", stage}, {"hr", hr}});
        }
    }
}
HRESULT ShowNative(IFileDialog* native, HWND owner, uint64_t dialog = 0) {
    const auto request = log::NextId();
    log::Event("native", {{"reason", 2}, {"request", request}, {"dialog", dialog}});
    const auto hr = native->Show(owner);
    log::Event("native_result", {{"hr", static_cast<unsigned>(hr)}, {"request", request}});
    return hr;
}
shell::FileDialogBridgeServices Services(const wchar_t* entry_family = L"COM") {
    shell::FileDialogBridgeServices services;
    services.private_interface_passthrough = CompareStringOrdinal(
        shell::application_identity::CurrentApplication().basename.c_str(), -1, L"PickerHost.exe", -1, TRUE) == CSTR_EQUAL;
    const auto dialog = log::NextId();
    const auto active_request = std::make_shared<std::wstring>();
    const auto com_ref = std::make_shared<ComObjectRef>();
    services.observe_show = [dialog, active_request, entry_family, com_ref](bool begin, const shell::FileDialogShowObservation& value) {
        static pulse::dialog_bridge_state::ActiveDialogLedger active_ledger;
        static std::once_flag ledger_once;
        std::call_once(ledger_once, [] { active_ledger.OpenOrCreate(); });
        const wchar_t* kind = !value.factory ? L"unknown" : *value.factory == shell::DialogFactory::Save ? L"save" : L"open";
        if (begin) {
            active_ledger.MarkActive(GetCurrentProcessId(), 0, 1);
            *active_request = std::to_wstring(dialog) + L":" + std::to_wstring(value.sequence);
            log::TextEvent("dialog_show_begin", {{"request", *active_request}, {"entry_family", entry_family}, {"launch_route", L"ordinary"}, {"dialog_kind", kind}},
                {{"dialog", dialog}, {"show_sequence", value.sequence}, {"process_arch", sizeof(void*) * 8}});
            return;
        }
        active_ledger.ClearActive(GetCurrentProcessId());
        const wchar_t* status = value.native_fallback ? L"native" :
            value.result == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? L"cancelled" :
            FAILED(value.result) ? L"failed" : value.accepted_result ? L"accepted" : L"closed";
        const wchar_t* fallback = L"";
        switch (value.fallback) {
        case shell::FileDialogShowFallback::ResolveNotReady: fallback = L"ENTRY_DISABLED_OR_UNAVAILABLE"; break;
        case shell::FileDialogShowFallback::InitialFolderUnavailable: fallback = L"INITIAL_FOLDER_UNREPRESENTABLE"; break;
        case shell::FileDialogShowFallback::ProviderUnavailable:
        case shell::FileDialogShowFallback::ProviderFailedBeforeUi: fallback = L"PROVIDER_START_FAILED_BEFORE_UI"; break;
        case shell::FileDialogShowFallback::ProviderPrepareFailed: fallback = L"PROVIDER_PREPARE_FAILED_BEFORE_UI"; break;
        case shell::FileDialogShowFallback::Contract: fallback = L"NATIVE_CONTRACT_FALLBACK"; break;
        default: break;
        }
        const wchar_t* failure_phase = L"";
        switch (value.failure_phase) {
        case shell::FileDialogShowFailurePhase::ProviderShowFailed: failure_phase = L"PROVIDER_SHOW_FAILED"; break;
        case shell::FileDialogShowFailurePhase::BridgeResponseInvalid: failure_phase = L"BRIDGE_RESPONSE_INVALID"; break;
        case shell::FileDialogShowFailurePhase::BridgeResultFailed: failure_phase = L"BRIDGE_RESULT_FAILED"; break;
        case shell::FileDialogShowFailurePhase::ShellItemMaterializeFailed: failure_phase = L"SHELL_ITEM_MATERIALIZE_FAILED"; break;
        case shell::FileDialogShowFailurePhase::CloseRequested: failure_phase = L"CLOSE_REQUESTED"; break;
        case shell::FileDialogShowFailurePhase::UnhandledException: failure_phase = L"UNHANDLED_EXCEPTION"; break;
        default: break;
        }
        log::TextEvent("dialog_show_terminal", {{"request", *active_request}, {"terminal_status", status},
            {"reason_name", ContractReason(value.contract_reason)}, {"fallback_reason", fallback},
            {"failure_phase", failure_phase}},
            {{"dialog", dialog}, {"show_sequence", value.sequence}, {"hr", static_cast<unsigned>(value.result)},
             {"native_fallback", value.native_fallback}, {"provider_started", value.provider_started},
             {"ui_shown_known", value.ui_shown.has_value()}, {"ui_shown", value.ui_shown.value_or(false)}});
        active_request->clear();
    };
    services.on_native_reason = [dialog](const char* reason, const IID* iid) { ObserveNativeReason(reason, iid, dialog); };
    services.show_native = [dialog](IFileDialog* native, HWND owner) { return ShowNative(native, owner, dialog); };
    services.resolve_on_show = [dialog, active_request](shell::FileDialogBridgeServices& resolved) {
        const auto status = LogRegistration(dialog);
        const auto own = OwnPath();
        td::Evidence token;
        const bool token_safe = TokenSafe(td::Stage::Resolve, &token);
        const bool enabled = token_safe && GenerationEnabled(own, status);
        const auto reason = td::GateReason(token_safe, token, status.enabled, status.conflict, status.error, enabled);
        td::LogGate("resolve_gate", td::Stage::Resolve,
            enabled ? L"ELIGIBLE" : wcscmp(reason, L"ELIGIBLE") == 0 ? L"REGISTRATION_CHANGED_OR_NOT_CURRENT" : reason,
            {{"dialog", dialog}, {"token_safe", token_safe}, {"enabled", enabled}, {"error", status.error}});
        if (!enabled) {
            log::Event("native", {{"reason", 1}}); return false;
        }
        const auto stub_host = StubHostExe();
        resolved.executable = !stub_host.empty()
            ? stub_host
            : (own.substr(0, own.find_last_of(L"\\/")) + L"\\pulse.exe");
        std::shared_ptr<pulse::dialog_bundle::BundleLease> bundle;
        bool thin = false;
        const bool stub_remote_only = stub_managed.load(std::memory_order_acquire);
        if (stub_remote_only && resolved.executable.empty()) return false;
#if !defined(_WIN64)
        if (!stub_remote_only) {
            const DWORD bundle_error = AutomaticBundle(own, bundle);
            if (bundle_error) { log::Event("companion_bundle", {{"error", bundle_error}}); return false; }
            thin = bundle->IsThinCompanion();
            if (thin) resolved.executable = bundle->Host();
        }
#endif
        resolved.prefer_bridge = pulse::dialog_auto::PreferOutOfProcess();
        log::Event("route", {{"prefer_bridge", resolved.prefer_bridge}});
        if (thin) {
            const bool remote = pulse::dialog_auto::PreferRemoteHost();
            log::Event("companion_bundle", {{"error", 0}, {"thin", 1}, {"remote_host", remote}, {"host_arch", 64}});
            if (!resolved.prefer_bridge || !remote) return false;
            // All thin-companion requests use V2, including basic Open/Save.
            // No attempt is made to load a 64-bit UI DLL into the x86 caller.
            resolved.prefer_bridge = false;
            resolved.load_inprocess_provider = [host = resolved.executable, bundle]() {
                std::shared_ptr<const shell::FileDialogInprocessProvider> provider;
                if (FAILED(RemoteProvider(host, provider))) return std::shared_ptr<const shell::FileDialogInprocessProvider>{};
                return shell::RetainCompanionProvider(std::move(provider), bundle);
            };
        } else if (stub_remote_only) {
            const bool remote = pulse::dialog_auto::PreferRemoteHost();
            log::Event("route", {{"remote_host", remote}, {"stub_remote_only", 1}});
            if (resolved.prefer_bridge) {
                resolved.load_inprocess_provider = [executable = resolved.executable, remote]() -> std::shared_ptr<const shell::FileDialogInprocessProvider> {
                    if (!remote) return nullptr;
                    std::shared_ptr<const shell::FileDialogInprocessProvider> provider;
                    if (SUCCEEDED(RemoteProvider(executable, provider)) && provider) return provider;
                    return nullptr;
                };
            } else {
                if (!remote) return false;
                const HRESULT hr = RemoteProvider(resolved.executable, resolved.inprocess_provider);
                if (FAILED(hr) || !resolved.inprocess_provider) {
                    td::LogGate("resolve_gate", td::Stage::Resolve, L"PROVIDER_UNAVAILABLE",
                        {{"dialog", dialog}, {"hr", static_cast<unsigned>(hr)}});
                    return false;
                }
            }
        } else if (resolved.prefer_bridge) {
            // pulse.exe shows basic dialogs; the in-process UI is loaded into
            // this process only by a Show that needs it.
            // With RemoteHost on, a Show that leaves the basic pulse.exe route
            // still stays out of this process: the remote V2 provider first.
            // Only when its host cannot start (nothing is shown yet) does the
            // in-process UI load.
            const bool remote = pulse::dialog_auto::PreferRemoteHost();
            log::Event("route", {{"remote_host", remote}});
            resolved.load_inprocess_provider = [executable = resolved.executable, remote]() -> std::shared_ptr<const shell::FileDialogInprocessProvider> {
                std::shared_ptr<const shell::FileDialogInprocessProvider> provider;
                if (remote && SUCCEEDED(RemoteProvider(executable, provider)) && provider) return provider;
                const HRESULT hr = Provider(executable, provider);
                log::Event("provider", {{"hr", static_cast<unsigned>(hr)}, {"deferred", 1}, {"remote_fallback", 1}});
                return SUCCEEDED(hr) ? provider : nullptr;
            };
        } else {
            const HRESULT hr = Provider(resolved.executable, resolved.inprocess_provider);
            log::Event("provider", {{"hr", static_cast<unsigned>(hr)}});
            if (FAILED(hr) || !resolved.inprocess_provider) {
                td::LogGate("resolve_gate", td::Stage::Resolve, L"PROVIDER_UNAVAILABLE",
                    {{"dialog", dialog}, {"hr", static_cast<unsigned>(hr)}});
                return false;
            }
        }
        resolved.observe_contract = [dialog, active_request](const shell::ContractDecision& decision, bool native, bool shown, size_t subscriptions) {
            log::TextEvent("contract_decision", {{"reason_name", ContractReason(decision.reason)}, {"request", *active_request}},
                {{"dialog", dialog}, {"hr", static_cast<unsigned>(decision.observed_result)},
                 {"options_known", decision.effective_options.has_value()}, {"call", decision.call_index}});
            log::Event("observe_contract", {{"reason", static_cast<unsigned>(decision.reason)},
                {"disposition", static_cast<unsigned>(decision.disposition)}, {"call", decision.call_index},
                {"options", decision.effective_options.value_or(0)}, {"native_options", decision.native_options}, {"native", native}, {"shown", shown},
                {"subscriptions", subscriptions}});
        };
        resolved.on_native_reason = [dialog](const char* reason, const IID* iid) { ObserveNativeReason(reason, iid, dialog); };
        resolved.access_policy = shell::AccessPolicy::LocalOpenV1;
        resolved.save_access_policy = shell::AccessPolicy::LocalSaveV1;
        resolved.validation_verified = [own](const shell::ContractDecision& decision) {
            td::Evidence token;
            const bool token_safe = TokenSafe(td::Stage::Validation, &token);
            const auto status = pulse::dialog_auto::Inspect();
            const bool enabled = token_safe && GenerationEnabled(own, status);
            const bool accepted = enabled &&
                (shell::RuntimeContractCertified(decision) || shell::RuntimeLocalSaveContractCertified(decision));
            td::LogGate("validation_gate", td::Stage::Validation, !token_safe ? td::ReasonName(token.reason) :
                !enabled ? L"REGISTRATION_CHANGED_OR_NOT_CURRENT" : accepted ? L"ELIGIBLE" : L"CONTRACT_NOT_CERTIFIED",
                {{"token_safe", token_safe}, {"enabled", enabled}, {"accepted", accepted}});
            log::Event("contract", {{"accepted", accepted}, {"category", static_cast<unsigned>(decision.validation_category)}});
            return accepted;
        };
        resolved.show_native = [dialog](IFileDialog* native, HWND owner) { return ShowNative(native, owner, dialog); };
        return true;
    };
    return services;
}
// ---- GetOpen/SaveFileName A/W and SHBrowseForFolder A/W (stub-managed sink hooks) ----
// These adapt the old entry points onto the same IFileDialog wrapper as COM
// activation. The native dialog is built inside the comdlg32 bypass so the
// ClassObject sink never hands back an already wrapped dialog. Any decline
// before UI returns to the caller's original API with its untouched structure:
// the modern native dialog would lose the OPENFILENAME / BROWSEINFO contract.
bool LegacyEntryEligible(const wchar_t* entry) {
    Initialize();
    td::Evidence token;
    const bool token_safe = TokenSafe(td::Stage::Activation, &token);
    const auto status = pulse::dialog_auto::Inspect();
    const bool enabled = token_safe && GenerationEnabled(OwnPath(), status);
    log::TextEvent("legacy_entry", {{"entry", entry}},
        {{"enabled", enabled}, {"token_safe", token_safe}, {"conflict", status.conflict}, {"error", status.error}});
    return enabled;
}
HRESULT NativeDialog(bool save, IUnknown** output) {
    *output = nullptr;
    ComPtr<IClassFactory> factory;
    const HRESULT hr = Native(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, CLSCTX_INPROC_SERVER, nullptr,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    shell::sink_hooks::ScopedNativeComDlgBypass bypass;
    return factory->CreateInstance(nullptr, IID_IUnknown, reinterpret_cast<void**>(output));
}
// Services() resolves show_native back to the modern native dialog on Show;
// keep the original-API fallback across that resolution.
void UseOriginalOnNativeFallback(shell::FileDialogBridgeServices& services, bool* use_original) {
    const auto fallback = [use_original](IFileDialog*, HWND) {
        *use_original = true;
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    };
    services.show_native = fallback;
    services.resolve_on_show = [resolve = std::move(services.resolve_on_show), fallback](shell::FileDialogBridgeServices& resolved) {
        const bool ready = resolve && resolve(resolved);
        resolved.show_native = fallback;
        return ready;
    };
}
// A failure before any UI is shown (and not a cancel) retries the original API.
void UseOriginalOnFailureBeforeUi(shell::FileDialogBridgeServices& services, bool* use_original) {
    services.observe_show = [previous = std::move(services.observe_show), use_original](bool begin, const shell::FileDialogShowObservation& value) {
        if (previous) previous(begin, value);
        if (!begin && FAILED(value.result) && value.result != HRESULT_FROM_WIN32(ERROR_CANCELLED) &&
            !value.ui_shown.value_or(false)) *use_original = true;
    };
}
template<class Ofn, class Original>
BOOL RouteLegacy(Ofn* input, Original original, bool save, const wchar_t* entry) {
    shell::legacy_dialog_error::UseNative();
    try {
        if (LegacyEntryEligible(entry)) {
            BOOL result = FALSE;
            bool use_original = false;
            const shell::LegacyDialogFactory create = [&use_original, entry](bool saving, IFileDialog** output) {
                IUnknown* native = nullptr;
                HRESULT hr = NativeDialog(saving, &native);
                if (FAILED(hr)) return hr;
                try {
                    auto services = Services(entry);
                    UseOriginalOnNativeFallback(services, &use_original);
                    UseOriginalOnFailureBeforeUi(services, &use_original);
                    hr = shell::WrapNativeFileDialog(saving ? shell::DialogFactory::Save : shell::DialogFactory::Open,
                        native, services, IID_IFileDialog, reinterpret_cast<void**>(output));
                } catch (...) { hr = E_FAIL; }
                native->Release();
                return hr;
            };
            const shell::LegacyDiagnostic diagnostic = [entry](const shell::LegacyComDiagnostic& value) {
                log::TextEvent("legacy_route", {{"entry", entry}, {"reason", DiagnosticText(value.reason)}},
                    {{"flags", value.flags}, {"flags_ex", value.flags_ex}, {"presence", value.presence},
                     {"hr", static_cast<unsigned>(value.error)}});
            };
            // Hook-only OFN_ENABLEHOOK dialogs run out-of-process via pulse.exe --picker-remote-host;
            // template customizations require an in-process parent HWND and remain on the original API.
            const bool remote_hook_ready = pulse::dialog_auto::PreferOutOfProcess() && pulse::dialog_auto::PreferRemoteHost();
            if (shell::RunLegacyComDialog(input, save, create, result, diagnostic, false, &use_original, remote_hook_ready)) return result;
        }
    } catch (...) { /* Declined before any Show. */ }
    shell::legacy_dialog_error::UseNative();
    const BOOL result = original(input);
    shell::legacy_dialog_error::UseNative();
    return result;
}
template<class Input, class Original>
PIDLIST_ABSOLUTE RouteBrowse(Input* input, Original original, const wchar_t* entry) {
    try {
        if (LegacyEntryEligible(entry)) {
            PIDLIST_ABSOLUTE result = nullptr;
            const shell::BrowseFolderFactory create = [entry](const shell::BrowseFolderPolicy& policy, IFileDialog** output) {
                IUnknown* native = nullptr;
                HRESULT hr = NativeDialog(false, &native);
                if (FAILED(hr)) return hr;
                try {
                    auto services = Services(entry);
                    if (policy.restricted || policy.hide_new_folder || policy.callback) {
                        const uint32_t browse_policy = PULSE_PICKER_UI_V2_FOLDER_LEGACY_BROWSE |
                            (policy.hide_new_folder ? PULSE_PICKER_UI_V2_FOLDER_HIDE_NEW_FOLDER : 0u);
                        std::function<void(HWND, UINT, LPARAM)> browse_callback;
                        if (policy.callback) browse_callback = [policy](HWND hwnd, UINT event, LPARAM value) {
                            policy.callback(hwnd, event, value, policy.callback_data);
                        };
                        auto apply_browse = [browse_policy, browse_callback](shell::FileDialogBridgeServices& target) {
                            target.prefer_bridge = false;
                            target.legacy_browse_policy = browse_policy;
                            if (browse_callback) target.legacy_browse_callback = browse_callback;
                        };
                        apply_browse(services);
                        services.resolve_on_show = [resolve = std::move(services.resolve_on_show), apply_browse](shell::FileDialogBridgeServices& resolved) {
                            const bool ready = resolve && resolve(resolved);
                            apply_browse(resolved);
                            return ready;
                        };
                    }
                    if (policy.use_original) {
                        UseOriginalOnNativeFallback(services, policy.use_original);
                        // A callback may already have run; only a callback-free failure retries.
                        if (!policy.callback) UseOriginalOnFailureBeforeUi(services, policy.use_original);
                    }
                    hr = shell::WrapNativeFileDialog(shell::DialogFactory::Open, native, services,
                        IID_IFileDialog, reinterpret_cast<void**>(output));
                } catch (...) { hr = E_FAIL; }
                native->Release();
                return hr;
            };
            const shell::BrowseFolderObserver observer = [entry](const shell::BrowseFolderDiagnostic& value) {
                log::TextEvent("browse_route", {{"entry", entry}, {"reason", DiagnosticText(value.reason)}},
                    {{"flags", value.flags}, {"presence", value.presence}, {"hr", static_cast<unsigned>(value.error)}});
            };
            if (shell::RunBrowseFolderDialog(input, create, result, observer)) return result;
        }
    } catch (...) { /* Declined before any Show. */ }
    return original(input);
}
// COM may cache this factory across apartments. Never retain an apartment-bound
// native/proxy factory: build and consume it on each caller's current apartment.
class Factory final : public IClassFactory {
    std::atomic<ULONG> references_{1};
    CLSID clsid_;
    ComObjectRef module_ref_;
public:
    explicit Factory(REFCLSID clsid) : clsid_(clsid) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory) return E_NOINTERFACE;
        *output = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { const auto n = --references_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        try {
            shell::FileDialogClassActivation activation;
            activation.original = Native;
            LogCallContext();
            td::Evidence token;
            const bool token_safe = TokenSafe(td::Stage::Activation, &token);
            const auto status = pulse::dialog_auto::Inspect();
            const auto own = OwnPath();
            LogModule("loaded_bridge", module);
            LogModule("caller_process", GetModuleHandleW(nullptr));
            LogRegistration(0);
            const bool path_match = status.dll_path == own;
            const bool generation_enabled = token_safe && GenerationEnabled(own, status);
            activation.enabled = token_safe && generation_enabled;
            td::LogGate("activation_gate", td::Stage::Activation,
                td::GateReason(token_safe, token, status.enabled, status.conflict, status.error, generation_enabled),
                {{"token_safe", token_safe}, {"registered", status.enabled},
                {"conflict", status.conflict}, {"error", status.error}, {"path_match", path_match},
                {"path_case_match", !own.empty() && CompareStringOrdinal(status.dll_path.c_str(), -1, own.c_str(), -1, TRUE) == CSTR_EQUAL}});
            activation.services = Services();
            log::Event("activation", {{"enabled", activation.enabled}, {"save", clsid_ == CLSID_FileSaveDialog}, {"diagnostics_revision", kDiagnosticsRevision}});
            ComPtr<IClassFactory> factory;
            HRESULT hr = shell::ActivateFileDialogClass(activation, clsid_, CLSCTX_INPROC_SERVER, nullptr, IID_PPV_ARGS(&factory));
            return SUCCEEDED(hr) ? factory->CreateInstance(outer, iid, output) : hr;
        } catch (...) { return E_OUTOFMEMORY; }
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) noexcept override {
        ComPtr<IClassFactory> factory;
        const auto hr = Native(clsid_, CLSCTX_INPROC_SERVER, nullptr, IID_PPV_ARGS(&factory));
        return SUCCEEDED(hr) ? factory->LockServer(lock) : hr;
    }
};
}
extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (clsid != CLSID_FileOpenDialog && clsid != CLSID_FileSaveDialog) return CLASS_E_CLASSNOTAVAILABLE;
    try {
        Initialize();
        if (iid != IID_IUnknown && iid != IID_IClassFactory) return Native(clsid, CLSCTX_INPROC_SERVER, nullptr, iid, output);
        auto* factory = new Factory(clsid);
        const auto hr = factory->QueryInterface(iid, output);
        factory->Release(); return hr;
    } catch (HRESULT hr) { return hr; } catch (...) { return E_OUTOFMEMORY; }
}
// Explicit same-user bootstrap entry: called by a remote worker only after the
// installed immutable bundle and target identity have been verified by Pulse.
extern "C" DWORD WINAPI PulseInitializeDirectReveal(void* output) noexcept {
    pulse::DirectRevealBootstrapDiagnostic diagnostic;
    const auto finish = [&](DWORD result) {
        diagnostic.folder_status = direct_reveal_folder_status.load();
        diagnostic.log_error = log::GetHealth().initialization_error;
        if (output) {
            pulse::DirectRevealBootstrapDiagnostic request;
            SIZE_T bytes = 0;
            if (ReadProcessMemory(GetCurrentProcess(), output, &request, sizeof(request), &bytes) &&
                bytes == sizeof(request) && request.size == sizeof(request) && request.version == 1)
                WriteProcessMemory(GetCurrentProcess(), output, &diagnostic, sizeof(diagnostic), &bytes);
        }
        return result;
    };
    try {
        diagnostic.stage = 1;
        // Pin the module without starting a per-process runtime_log heartbeat
        // thread: background reveal targets must not churn Diagnostics\Runtime
        // and evict real file-dialog logs. DllGetClassObject initializes logging
        // on demand when a process actually invokes COM file dialogs.
        PinModule();
        direct_reveal_folder_status.store(S_OK);
        diagnostic.stage = 2;
        td::Evidence token;
        if (!TokenSafe(td::Stage::Activation, &token)) {
            diagnostic.token_reason = token.reason; diagnostic.token_error = token.error;
            return finish(ERROR_ACCESS_DENIED);
        }
        diagnostic.stage = 3;
        const DWORD status = StartDirectReveal(&diagnostic.install);
        if (!status) diagnostic.stage = 4;
        return finish(status);
    } catch (...) { return finish(ERROR_GEN_FAILURE); }
}
extern "C" BOOL WINAPI PulseIsDirectRevealInstalled() noexcept {
    return (shell::runtime::IsDirectRevealHookInstalled() ||
            (shell::sink_hooks::QueryStatus().active_mask & shell::sink_hooks::kSinkShell32DirectReveal) != 0)
        ? TRUE : FALSE;
}

extern "C" BOOL WINAPI PulseIsExplorerProcessInstalled() noexcept {
    return (shell::runtime::IsExplorerProcessHookInstalled() ||
            ((shell::sink_hooks::QueryStatus().active_mask & shell::sink_hooks::kSinkKernel32CreateProcessW) != 0 &&
             shell::explorer_process::IsReady()))
        ? TRUE : FALSE;
}

namespace {
DWORD InstallCoreSinkHooks(uint32_t flags) noexcept {
    using namespace shell::sink_hooks;
    uint32_t mask = kSinkComDlgClassObject | kSinkComDlgOpenSaveW | kSinkComDlgOpenSaveA |
        kSinkComDlgExtendedError | kSinkShell32Browse;
    if (flags & PULSE_STUB_FLAG_DIRECT_REVEAL) {
        mask |= kSinkShell32DirectReveal;
        {
            (void)shell::explorer_process::Configure(module, DirectRevealEnabled);
            // A CreateProcess callback may still be on another thread during shutdown.
            // Pin only when enabling this new sink: Disable/Uninstall revoke routing,
            // but this generation's code lives until target exit (updates require restart).
            HMODULE pinned = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(&InstallCoreSinkHooks), &pinned) && pinned == module)
                mask |= kSinkKernel32CreateProcessW;
            else
                shell::explorer_process::Disable();
        }
    } else {
        shell::explorer_process::Disable();
    }
#if defined(PULSE_EXPERIMENTAL_WINRT_PICKER)
    if (flags & PULSE_STUB_FLAG_WINRT_ENABLED) mask |= kSinkGroupCombaseAll;
#endif

    SinkCallbacks cb;
    cb.on_comdlg_class_object = [](REFCLSID clsid, REFIID iid, void** out, ComDlgGetClassObjectFn) -> HRESULT {
        return ::DllGetClassObject(clsid, iid, out);
    };
    cb.on_get_open_w = [](LPOPENFILENAMEW input, GetOpenFileNameWFn original) {
        return RouteLegacy(input, original, false, L"GetOpenFileNameW");
    };
    cb.on_get_save_w = [](LPOPENFILENAMEW input, GetSaveFileNameWFn original) {
        return RouteLegacy(input, original, true, L"GetSaveFileNameW");
    };
    cb.on_get_open_a = [](LPOPENFILENAMEA input, GetOpenFileNameAFn original) {
        return RouteLegacy(input, original, false, L"GetOpenFileNameA");
    };
    cb.on_get_save_a = [](LPOPENFILENAMEA input, GetSaveFileNameAFn original) {
        return RouteLegacy(input, original, true, L"GetSaveFileNameA");
    };
    cb.on_commdlg_error = [](CommDlgExtendedErrorFn original) {
        return shell::legacy_dialog_error::Query(original);
    };
    cb.on_browse_w = [](LPBROWSEINFOW input, SHBrowseForFolderWFn original) {
        return RouteBrowse(input, original, L"SHBrowseForFolderW");
    };
    cb.on_browse_a = [](LPBROWSEINFOA input, SHBrowseForFolderAFn original) {
        return RouteBrowse(input, original, L"SHBrowseForFolderA");
    };
#if defined(PULSE_EXPERIMENTAL_WINRT_PICKER)
    if (flags & PULSE_STUB_FLAG_WINRT_ENABLED) {
        auto make_winrt_bridge = []() -> shell::winrt_picker::Services {
            shell::winrt_picker::Services bridge;
            bridge.run = [](const pulse::ipc::picker::Message& request, HWND owner, HANDLE cancel) {
                const auto host = StubHostExe();
                const auto exe = !host.empty() ? host : (OwnPath().substr(0, OwnPath().find_last_of(L"\\/")) + L"\\pulse.exe");
                return pulse::ipc::picker::RunPickerClient(exe, request, owner, cancel);
            };
            return bridge;
        };
        cb.on_ro_get_activation_factory = [make_winrt_bridge](HSTRING cls, REFIID iid, void** out,
                                                              RoGetActivationFactoryFn orig) -> HRESULT {
            if ((stub_managed.load(std::memory_order_acquire) &&
                 (!StubBridgeEnabled() || UserExplicitlyDisabledDialogAuto())) ||
                !TokenSafe(td::Stage::Activation)) return orig(cls, iid, out);
            return shell::winrt_picker::GetActivationFactory(cls, iid, out, make_winrt_bridge(), orig);
        };
        cb.on_ro_activate_instance = [make_winrt_bridge](HSTRING cls, IInspectable** out,
                                                         RoActivateInstanceFn orig) -> HRESULT {
            if ((stub_managed.load(std::memory_order_acquire) &&
                 (!StubBridgeEnabled() || UserExplicitlyDisabledDialogAuto())) ||
                !TokenSafe(td::Stage::Activation)) return orig(cls, out);
            return shell::winrt_picker::ActivateInstance(cls, out, make_winrt_bridge(), orig);
        };
    }
#endif
    if (flags & PULSE_STUB_FLAG_DIRECT_REVEAL) {
        cb.on_reveal_items = [](PCIDLIST_ABSOLUTE folder, UINT count, PCUITEMID_CHILD_ARRAY items, DWORD fl,
                                SHOpenFolderAndSelectItemsFn orig) -> HRESULT {
            if (DirectRevealEnabled()) {
                const auto result = shell::runtime::DispatchDirectRevealPidl(folder, count, items, fl);
                if (!pulse::ShouldUseNativeBrowse(result)) return shell::runtime::CompleteDirectReveal(result);
            }
            return shell::runtime::FallbackDirectRevealPidl(orig, folder, count, items, fl);
        };
        cb.on_shell_execute_w = [](HWND hwnd, LPCWSTR verb, LPCWSTR file, LPCWSTR params, LPCWSTR dir, INT show,
                                   ShellExecuteWFn orig) -> HINSTANCE {
            auto result = pulse::DirectRevealResult::NotHandled;
            if (DirectRevealEnabled()) {
                result = shell::runtime::DispatchDirectShellExecuteW(verb, file, params, dir, show);
                if (!pulse::ShouldUseNativeBrowse(result)) return shell::runtime::CompleteDirectShellExecute(result);
            }
            return shell::runtime::FallbackShellExecuteW(result, orig, hwnd, verb, file, params, dir, show);
        };
        cb.on_shell_execute_ex_w = [](SHELLEXECUTEINFOW* info, ShellExecuteExWFn orig) -> BOOL {
            auto result = pulse::DirectRevealResult::NotHandled;
            if (DirectRevealEnabled()) {
                result = shell::runtime::DispatchDirectShellExecuteExW(info);
                if (!pulse::ShouldUseNativeBrowse(result)) return shell::runtime::CompleteDirectShellExecuteEx(info, result);
            }
            return shell::runtime::FallbackShellExecuteExW(result, orig, info);
        };
        cb.on_shell_execute_a = [](HWND hwnd, LPCSTR verb, LPCSTR file, LPCSTR params, LPCSTR dir, INT show,
                                   ShellExecuteAFn orig) -> HINSTANCE {
            auto result = pulse::DirectRevealResult::NotHandled;
            if (DirectRevealEnabled()) {
                result = shell::runtime::DispatchDirectShellExecuteA(verb, file, params, dir, show);
                if (!pulse::ShouldUseNativeBrowse(result)) return shell::runtime::CompleteDirectShellExecute(result);
            }
            return shell::runtime::FallbackShellExecuteA(result, orig, hwnd, verb, file, params, dir, show);
        };
        cb.on_shell_execute_ex_a = [](SHELLEXECUTEINFOA* info, ShellExecuteExAFn orig) -> BOOL {
            auto result = pulse::DirectRevealResult::NotHandled;
            if (DirectRevealEnabled()) {
                result = shell::runtime::DispatchDirectShellExecuteExA(info);
                if (!pulse::ShouldUseNativeBrowse(result)) return shell::runtime::CompleteDirectShellExecuteEx(info, result);
            }
            return shell::runtime::FallbackShellExecuteExA(result, orig, info);
        };
    }

    if (mask & kSinkKernel32CreateProcessW) {
        cb.on_create_process_w = [](LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
            BOOL inherit, DWORD creation_flags, LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW startup,
            LPPROCESS_INFORMATION pi, CreateProcessWFn original) -> BOOL {
            const DWORD incoming = GetLastError();
            BOOL result = FALSE;
            if (shell::explorer_process::TryRoute(original, app, cmd, pa, ta, inherit, creation_flags,
                    env, cwd, startup, pi, result)) return result;
            SetLastError(incoming);
            return shell::explorer_process::FallbackCreateProcess(original, app, cmd, pa, ta, inherit, creation_flags, env, cwd, startup, pi);
        };
    }
    InstallOptions opts;
    opts.requested_mask = mask;
    opts.watch_late_dll_loads = true;
    opts.preload_comdlg32 = true;
    const auto st = Install(std::move(cb), opts);
    return st.error;
}

HRESULT WINAPI CoreInitialize(const PulseStubConfigV1* cfg) noexcept {
    if (!cfg || cfg->size < sizeof(PulseStubConfigV1) || cfg->version != PULSE_DIALOG_CORE_ABI_VERSION) {
        return E_INVALIDARG;
    }
    try {
        stub_managed.store(true, std::memory_order_release);
        AcquireSRWLockExclusive(&stub_config_lock);
        stub_bridge_root = cfg->bridge_root ? cfg->bridge_root : L"";
        stub_host_exe = cfg->host_exe ? cfg->host_exe : L"";
        stub_generation = cfg->generation;
        ReleaseSRWLockExclusive(&stub_config_lock);
        {
            std::lock_guard<std::mutex> lock(stub_pointer_mutex);
            stub_pointer_checked_tick = 0;
            stub_pointer_enabled = true;
        }
        if (cfg->flags & PULSE_STUB_FLAG_INSTALL_SINK_HOOKS) {
            const DWORD err = InstallCoreSinkHooks(cfg->flags);
            if (err != ERROR_SUCCESS) return HRESULT_FROM_WIN32(err);
        }
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

void WINAPI CoreShutdown() noexcept {
    shell::explorer_process::Disable();
    shell::sink_hooks::Uninstall();
    log::Shutdown();
}

DWORD WINAPI CoreActivateProcess(const PulseStubConfigV1* cfg) noexcept {
    if (!cfg || cfg->size < sizeof(PulseStubConfigV1)) return ERROR_INVALID_PARAMETER;
    const HRESULT hr = CoreInitialize(cfg);
    if (FAILED(hr)) return HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : ERROR_GEN_FAILURE;
    if (!(cfg->flags & PULSE_STUB_FLAG_INSTALL_SINK_HOOKS)) {
        return InstallCoreSinkHooks(cfg->flags | PULSE_STUB_FLAG_INSTALL_SINK_HOOKS);
    }
    return ERROR_SUCCESS;
}

uint32_t WINAPI CoreGetActiveHookMask() noexcept {
    return shell::sink_hooks::QueryStatus().active_mask;
}
} // namespace

extern "C" HRESULT WINAPI PulseDialogCoreQueryV1(PulseDialogCoreV1* out_table) noexcept {
    if (!out_table || out_table->size < sizeof(PulseDialogCoreV1)) return E_INVALIDARG;
    PulseDialogCoreV1 table{};
    table.magic = PULSE_DIALOG_CORE_MAGIC;
    table.abi_version = PULSE_DIALOG_CORE_ABI_VERSION;
    table.size = sizeof(table);
    table.core_build_id = kDiagnosticsRevision;
    table.initialize = &CoreInitialize;
    table.shutdown = &CoreShutdown;
    table.dll_get_class_object = &DllGetClassObject;
    table.activate_process = &CoreActivateProcess;
    table.initialize_direct_reveal = &PulseInitializeDirectReveal;
    table.get_active_hook_mask = &CoreGetActiveHookMask;
    *out_table = table;
    return S_OK;
}

extern "C" HRESULT WINAPI DllCanUnloadNow() {
    return (stub_managed.load(std::memory_order_acquire) &&
            live_com_objects.load(std::memory_order_acquire) == 0) ? S_OK : S_FALSE;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) module = instance;
    return TRUE;
}



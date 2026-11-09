#include "../common/process_token_restriction.h"
#include "file_dialog_class_factory.h"
#include "file_dialog_runtime.h"
#include "file_dialog_diagnostic.h"
#include "file_dialog_ui_provider.h"
#include "file_dialog_remote_provider.h"
#include "../common/dialog_auto_registration.h"
#include "../common/runtime_log.h"
#include "../common/dialog_admin_trust.h"
#include <atomic>
#include <shlobj.h>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <wrl/client.h>

namespace {
namespace shell = pulse::shell_bridge;
namespace log = pulse::diagnostics::runtime;
using Microsoft::WRL::ComPtr;
HMODULE module;
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
    log::TextEvent(event, {{"path", error ? L"" : path}},
        {{"error", error}, {"pe_timestamp", timestamp}, {"image_size", image_size}});
}
void LogRegistration(uint64_t dialog) {
    const auto status = pulse::dialog_auto::Inspect();
    const auto own = OwnPath();
    log::TextEvent("registration_snapshot", {{"registered_dll", status.dll_path}, {"loaded_dll", own}},
        {{"dialog", dialog}, {"enabled", status.enabled}, {"conflict", status.conflict},
         {"error", status.error}, {"path_match", status.dll_path == own}});
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
bool TokenSafe() noexcept {
    auto reject = [](unsigned reason, DWORD error = ERROR_SUCCESS) {
        log::Event("token_rejected", {{"reason", reason}, {"error", error}}, log::Level::Warning);
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
    if (elevation.TokenIsElevated) {
        DWORD trust_error = ERROR_NOT_ENOUGH_MEMORY;
        try {
            const auto own = OwnPath();
            const auto separator = own.find_last_of(L"\\/");
            trust_error = separator == std::wstring::npos ? ERROR_BAD_PATHNAME :
                pulse::dialog_admin::VerifyProtectedTree(own.substr(0, separator));
        } catch (...) {}
        log::Event("elevated_bundle_trust", {{"error", trust_error}});
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
    return true;
}
void Initialize() {
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&module), &pinned)) throw HRESULT_FROM_WIN32(GetLastError());
        PWSTR root = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &root))) {
            log::Initialize(std::wstring(root) + L"\\Pulse", "dialog_auto");
            wchar_t executable[32768]{};
            GetModuleFileNameW(nullptr, executable, 32768);
            const wchar_t* base = wcsrchr(executable, L'\\');
            base = base ? base + 1 : executable;
            // Fixed numeric identities keep paths and document names out of runtime logs.
            const uint64_t app = !_wcsicmp(base, L"Cursor.exe") ? 1 :
                (!_wcsicmp(base, L"Photos.exe") || !_wcsicmp(base, L"Microsoft.Photos.exe")) ? 2 :
                !_wcsicmp(base, L"msedge.exe") ? 3 : !_wcsicmp(base, L"QQ.exe") ? 4 : 0;
            log::Event("dialog_identity", {{"app", app}, {"diagnostics_revision", 47}});
            LogModule("loaded_bridge", module);
            LogModule("caller_process", GetModuleHandleW(nullptr));
            CoTaskMemFree(root);
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
    log::TextEvent("provider_load", {{"host", host}});
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
    log::TextEvent("remote_provider_load", {{"host", host}});
    const HRESULT hr = shell::LoadFileDialogRemoteProvider(host, loaded, true);
    log::Event("remote_provider", {{"hr", static_cast<unsigned>(hr)}, {"features", loaded ? loaded->api.implemented_features : 0}});
    if (FAILED(hr) || !loaded) return FAILED(hr) ? hr : E_FAIL;
    cached = loaded;
    output = std::move(loaded);
    return S_OK;
}
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
shell::FileDialogBridgeServices Services() {
    shell::FileDialogBridgeServices services;
    const auto dialog = log::NextId();
    services.on_native_reason = [dialog](const char* reason, const IID* iid) { ObserveNativeReason(reason, iid, dialog); };
    services.show_native = [dialog](IFileDialog* native, HWND owner) { return ShowNative(native, owner, dialog); };
    services.resolve_on_show = [dialog](shell::FileDialogBridgeServices& resolved) {
        LogRegistration(dialog);
        const auto own = OwnPath();
        if (!TokenSafe() || !pulse::dialog_auto::IsEnabledFor(own)) {
            log::Event("native", {{"reason", 1}}); return false;
        }
        resolved.executable = own.substr(0, own.find_last_of(L"\\/")) + L"\\pulse.exe";
        resolved.prefer_bridge = pulse::dialog_auto::PreferOutOfProcess();
        log::Event("route", {{"prefer_bridge", resolved.prefer_bridge}});
        if (resolved.prefer_bridge) {
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
            if (FAILED(hr) || !resolved.inprocess_provider) return false;
        }
        resolved.observe_contract = [dialog](const shell::ContractDecision& decision, bool native, bool shown, size_t subscriptions) {
            log::TextEvent("contract_decision", {{"reason_name", ContractReason(decision.reason)}},
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
            const bool accepted = TokenSafe() && pulse::dialog_auto::IsEnabledFor(own) &&
                (shell::RuntimeContractCertified(decision) || shell::RuntimeLocalSaveContractCertified(decision));
            log::Event("contract", {{"accepted", accepted}, {"category", static_cast<unsigned>(decision.validation_category)}});
            return accepted;
        };
        resolved.show_native = [dialog](IFileDialog* native, HWND owner) { return ShowNative(native, owner, dialog); };
        return true;
    };
    return services;
}
// COM may cache this factory across apartments. Never retain an apartment-bound
// native/proxy factory: build and consume it on each caller's current apartment.
class Factory final : public IClassFactory {
    std::atomic<ULONG> references_{1};
    CLSID clsid_;
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
            const bool token_safe = TokenSafe();
            const auto status = pulse::dialog_auto::Inspect();
            const auto own = OwnPath();
            LogModule("loaded_bridge", module);
            LogModule("caller_process", GetModuleHandleW(nullptr));
            LogRegistration(0);
            const bool path_match = status.dll_path == own;
            activation.enabled = token_safe && status.enabled && !status.conflict && !status.error && path_match;
            log::Event("activation_gate", {{"token_safe", token_safe}, {"registered", status.enabled},
                {"conflict", status.conflict}, {"error", status.error}, {"path_match", path_match},
                {"path_case_match", !own.empty() && CompareStringOrdinal(status.dll_path.c_str(), -1, own.c_str(), -1, TRUE) == CSTR_EQUAL}});
            activation.services = Services();
            log::Event("activation", {{"enabled", activation.enabled}, {"save", clsid_ == CLSID_FileSaveDialog}, {"diagnostics_revision", 47}});
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
extern "C" HRESULT WINAPI DllCanUnloadNow() { return S_FALSE; }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) module = instance;
    return TRUE;
}



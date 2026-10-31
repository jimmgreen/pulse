#include "../common/process_token_restriction.h"
#include "file_dialog_class_factory.h"
#include "file_dialog_runtime.h"
#include "file_dialog_diagnostic.h"
#include "file_dialog_ui_provider.h"
#include "../common/dialog_auto_registration.h"
#include "../common/runtime_log.h"
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
bool TokenSafe() noexcept {
    HANDLE token = nullptr;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) { CloseHandle(token); return false; }
    if (GetLastError() != ERROR_NO_TOKEN || !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    struct Close { HANDLE h; ~Close() { CloseHandle(h); } } close{token};
    TOKEN_ELEVATION elevation{}; DWORD size = 0, container = 0;
    if (!GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) || elevation.TokenIsElevated ||
        pulse::security::CheckUnrestrictedToken(token) != ERROR_SUCCESS ||
        !GetTokenInformation(token, TokenIsAppContainer, &container, sizeof(container), &size) || container) return false;
    alignas(TOKEN_MANDATORY_LABEL) BYTE buffer[1024]{};
    if (!GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size)) return false;
    const auto sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer)->Label.Sid;
    if (!IsValidSid(sid)) return false;
    const auto count = *GetSidSubAuthorityCount(sid);
    return count && *GetSidSubAuthority(sid, count - 1) >= SECURITY_MANDATORY_MEDIUM_RID;
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
                !_wcsicmp(base, L"msedge.exe") ? 3 : 0;
            log::Event("dialog_identity", {{"app", app}, {"diagnostics_revision", 43}});
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
    if (cached) { output = cached; return S_OK; }
    std::shared_ptr<const shell::FileDialogInprocessProvider> loaded;
    const auto hr = shell::LoadFileDialogUiProvider(host, loaded, true);
    if (FAILED(hr) || !loaded) return FAILED(hr) ? hr : E_FAIL;
    auto decorated = std::make_shared<shell::FileDialogInprocessProvider>(*loaded);
    original_provider = loaded;
    decorated->api.Show = LoggedShow;
    cached = decorated;
    output = cached;
    return S_OK;
}
void ObserveNativeReason(const char* reason, const IID* iid) {
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
HRESULT ShowNative(IFileDialog* native, HWND owner) {
    const auto request = log::NextId();
    log::Event("native", {{"reason", 2}, {"request", request}});
    const auto hr = native->Show(owner);
    log::Event("native_result", {{"hr", static_cast<unsigned>(hr)}, {"request", request}});
    return hr;
}
shell::FileDialogBridgeServices Services() {
    shell::FileDialogBridgeServices services;
    services.on_native_reason = ObserveNativeReason;
    services.show_native = ShowNative;
    services.resolve_on_show = [](shell::FileDialogBridgeServices& resolved) {
        const auto own = OwnPath();
        if (!TokenSafe() || !pulse::dialog_auto::IsEnabledFor(own)) {
            log::Event("native", {{"reason", 1}}); return false;
        }
        resolved.executable = own.substr(0, own.find_last_of(L"\\/")) + L"\\pulse.exe";
        const HRESULT hr = Provider(resolved.executable, resolved.inprocess_provider);
        log::Event("provider", {{"hr", static_cast<unsigned>(hr)}});
        if (FAILED(hr) || !resolved.inprocess_provider) return false;
        resolved.observe_contract = [](const shell::ContractDecision& decision, bool native, bool shown, size_t subscriptions) {
            log::Event("observe_contract", {{"reason", static_cast<unsigned>(decision.reason)},
                {"disposition", static_cast<unsigned>(decision.disposition)}, {"call", decision.call_index},
                {"options", decision.effective_options.value_or(0)}, {"native", native}, {"shown", shown},
                {"subscriptions", subscriptions}});
        };
        resolved.on_native_reason = ObserveNativeReason;
        resolved.access_policy = shell::AccessPolicy::LocalOpenV1;
        resolved.save_access_policy = shell::AccessPolicy::LocalSaveV1;
        resolved.validation_verified = [own](const shell::ContractDecision& decision) {
            const bool accepted = TokenSafe() && pulse::dialog_auto::IsEnabledFor(own) &&
                (shell::RuntimeContractCertified(decision) || shell::RuntimeLocalSaveContractCertified(decision));
            log::Event("contract", {{"accepted", accepted}, {"category", static_cast<unsigned>(decision.validation_category)}});
            return accepted;
        };
        resolved.show_native = ShowNative;
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
            activation.enabled = TokenSafe() && pulse::dialog_auto::IsEnabledFor(OwnPath());
            activation.services = Services();
            log::Event("activation", {{"enabled", activation.enabled}, {"save", clsid_ == CLSID_FileSaveDialog}});
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



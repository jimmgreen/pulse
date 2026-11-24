#include <cstdio>
#include <string>
#include <string_view>
#include <stdexcept>
#include <wrl/client.h>
namespace activation_routes {
using namespace pulse::shell_bridge;
using Microsoft::WRL::ComPtr;
namespace {
template <class Activation>
Activation Config(Activation config, const char** stage) {
    if constexpr (requires { config.outcome; }) config.outcome = stage;
    return config;
}
int calls;
bool fail_original;
ComPtr<IUnknown> original_identity;
HRESULT Capture(HRESULT hr, void* ptr) {
    ++calls;
    if (ptr) static_cast<IUnknown*>(ptr)->QueryInterface(IID_PPV_ARGS(original_identity.ReleaseAndGetAddressOf()));
    return hr;
}
HRESULT WINAPI Single(REFCLSID id, IUnknown* outer, DWORD ctx, REFIID iid, void** out) {
    HRESULT hr = fail_original ? E_ACCESSDENIED : CoCreateInstance(id, outer, ctx, iid, out);
    return Capture(hr, *out);
}
HRESULT WINAPI Ex(REFCLSID id, IUnknown* outer, DWORD ctx, COSERVERINFO* s, DWORD n, MULTI_QI* r) {
    HRESULT hr = fail_original ? E_ACCESSDENIED : CoCreateInstanceEx(id, outer, ctx, s, n, r);
    return Capture(hr, r->pItf);
}
HRESULT WINAPI Class(REFCLSID id, DWORD ctx, void* s, REFIID iid, void** out) {
    HRESULT hr = fail_original ? E_ACCESSDENIED : CoGetClassObject(id, ctx, s, iid, out);
    return Capture(hr, *out);
}
}
int Run() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    int failures = 0;
    for (int api = 0; api < 3; ++api) for (const bool save : {false, true})
    for (DWORD ctx : {DWORD(CLSCTX_INPROC_SERVER), DWORD(CLSCTX_ALL)}) for (int mode : {0, 1, 2}) {
        if (mode && (save || ctx != DWORD(CLSCTX_ALL))) continue;
        calls = 0; fail_original = mode == 2;
        const CLSID& id = save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
        const IID& iid = api == 2 ? IID_IClassFactory : save ? IID_IFileSaveDialog : IID_IFileOpenDialog;
        std::string reason;
        FileDialogBridgeServices services;
        services.on_native_reason = [&](const char* v, const IID*) {
            reason = v; if (mode == 1) throw std::runtime_error("observer");
        };
        const char* stage = "unrecorded";
        void* value = nullptr;
        HRESULT hr;
        if (api == 0) hr = ActivateFileDialog(Config(FileDialogActivation{true, Single, services}, &stage), id, nullptr, ctx, iid, &value);
        else if (api == 1) {
            MULTI_QI r{&iid, nullptr, S_OK};
            hr = ActivateFileDialogEx(Config(FileDialogActivationEx{true, Ex, services}, &stage), id, nullptr, ctx, nullptr, 1, &r);
            value = r.pItf;
        } else hr = ActivateFileDialogClass(Config(FileDialogClassActivation{true, Class, services}, &stage), id, ctx, nullptr, iid, &value);
        ComPtr<IUnknown> item; item.Attach(static_cast<IUnknown*>(value));
        ComPtr<IUnknown> identity; if (item) item.As(&identity);
        bool native = identity && identity.Get() == original_identity.Get();
        bool ok = calls == 1 && (fail_original ? hr == E_ACCESSDENIED && !identity : hr == S_OK && identity && native == (ctx == DWORD(CLSCTX_ALL)));
        const auto expected = fail_original ? "original-failed" : ctx == DWORD(CLSCTX_ALL) ? "native-context" : "wrapper-published";
        ok &= reason.find(expected) != std::string::npos && std::string_view(stage).find(expected) != std::string_view::npos;
        std::printf("%s %s %s context=0x%02lX mode=%d calls=%d hr=%08lX identity=%s route=%s event=%s\n", ok?"PASS":"FAIL", api==0?"single":api==1?"ex":"class", save?"save":"open", ctx, mode, calls, hr, !identity?"none":native?"native":"wrapped", stage, reason.empty()?"<none>":reason.c_str());
        failures += !ok;
        original_identity.Reset();
    }
    CoUninitialize();
    return failures ? 1 : 0;
}
} // namespace activation_routes

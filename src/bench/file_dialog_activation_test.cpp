#include "../shell_bridge/file_dialog_activation.h"

#include <iostream>
#include <memory>
#include <new>
#include <thread>
#include <wrl/client.h>

using namespace pulse::shell_bridge;
using Microsoft::WRL::ComPtr;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* name, HRESULT hr = S_OK) {
    ++checks;
    if (!ok) ++failures;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name;
    if (!ok) std::cout << " HRESULT=" << std::hex << static_cast<unsigned long>(hr) << std::dec;
    std::cout << '\n';
}
struct Probe {
    int calls = 0, creations = 0, depth = 0;
    CLSID clsid{};
    IID iid{};
    DWORD context = 0;
    IUnknown* outer = nullptr;
    LPVOID* output_argument = nullptr;
    void* requested_interface = nullptr;
    HRESULT result = E_UNEXPECTED;
    HRESULT override_success = S_OK;
    bool fail = false, recurse = false;
    const FileDialogActivation* activation = nullptr;
    ComPtr<IUnknown> native_identity;
};
thread_local Probe* active_probe = nullptr;
HRESULT WINAPI Original(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* output) {
    auto& probe = *active_probe;
    ++probe.calls;
    probe.clsid = clsid; probe.iid = iid; probe.context = context;
    probe.outer = outer; probe.output_argument = output;
    if (probe.fail) return probe.result = E_ACCESSDENIED;
    ++probe.depth;
    HRESULT hr;
    if (probe.recurse && probe.depth == 1) {
        hr = ActivateFileDialog(*probe.activation, clsid, outer, context, iid, output);
    } else {
        ++probe.creations;
        hr = ::CoCreateInstance(clsid, outer, context, iid, output);
        if (SUCCEEDED(hr) && output && *output) {
            probe.requested_interface = *output;
            static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&probe.native_identity));
            hr = probe.override_success;
        }
    }
    --probe.depth;
    return probe.result = hr;
}
ComPtr<IUnknown> Identity(IUnknown* object) {
    ComPtr<IUnknown> identity;
    if (object) object->QueryInterface(IID_PPV_ARGS(&identity));
    return identity;
}
FileDialogActivation Configuration() {
    FileDialogActivation activation;
    activation.enabled = true;
    activation.original = Original;
    return activation;
}
struct ThrowOnCopy {
    std::shared_ptr<bool> fail;
    explicit ThrowOnCopy(std::shared_ptr<bool> value) : fail(std::move(value)) {}
    ThrowOnCopy(const ThrowOnCopy& other) : fail(other.fail) { if (*fail) throw std::bad_alloc(); }
    HRESULT operator()(IFileDialog*, HWND) const { return E_ABORT; }
};
void WrappedCreation(DialogFactory factory, REFIID requested) {
    Probe probe; active_probe = &probe;
    auto activation = Configuration();
    int native_shows = 0;
    bool same_native = false;
    activation.services.show_native = [&](IFileDialog* native, HWND) {
        ++native_shows;
        same_native = Identity(native).Get() == probe.native_identity.Get();
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    };
    void* output = nullptr;
    const auto clsid = factory == DialogFactory::Open ? CLSID_FileOpenDialog : CLSID_FileSaveDialog;
    const auto hr = ActivateFileDialog(activation, clsid, nullptr, CLSCTX_INPROC_SERVER, requested, &output);
    ComPtr<IUnknown> returned; returned.Attach(static_cast<IUnknown*>(output));
    Check(hr == S_OK && returned && probe.calls == 1 && probe.creations == 1, "supported activation calls original once", hr);
    Check(probe.clsid == clsid && probe.iid == requested && probe.context == CLSCTX_INPROC_SERVER &&
        !probe.outer && probe.output_argument == &output, "original receives exact activation arguments");
    ComPtr<IFileDialog> dialog; if (returned) returned.As(&dialog);
    Check(dialog && Identity(returned.Get()).Get() != probe.native_identity.Get(), "supported activation returns wrapper identity");
    ComPtr<IModalWindow> modal; ComPtr<IFileDialogCustomize> customize;
    if (dialog) { dialog.As(&modal); dialog.As(&customize); }
    Check(modal && customize && Identity(modal.Get()).Get() == Identity(returned.Get()).Get() &&
        Identity(customize.Get()).Get() == Identity(returned.Get()).Get(), "activation interface paths share controlling unknown");
    Check(dialog && Identity(returned.Get()).Get() != probe.native_identity.Get() && dialog->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && native_shows == 1 && same_native,
        "default validation gate uses original object for native Show without recreation");
}
void Bypass(REFCLSID clsid, REFIID iid, DWORD context, bool enabled) {
    Probe probe; active_probe = &probe;
    auto activation = Configuration(); activation.enabled = enabled;
    void* output = nullptr;
    const auto hr = ActivateFileDialog(activation, clsid, nullptr, context, iid, &output);
    ComPtr<IUnknown> returned; returned.Attach(static_cast<IUnknown*>(output));
    Check(probe.calls == 1 && probe.creations == 1 && probe.clsid == clsid && probe.iid == iid &&
        probe.context == context && hr == probe.result, "bypass preserves arguments and original HRESULT", hr);
    Check(SUCCEEDED(hr) ? returned && returned.Get() == probe.requested_interface && Identity(returned.Get()).Get() == probe.native_identity.Get() : !returned,
        "bypass preserves native identity or zero failure output", hr);
}
}
int main() {
    std::cout << std::unitbuf;
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 2;
    WrappedCreation(DialogFactory::Open, IID_IFileOpenDialog);
    WrappedCreation(DialogFactory::Save, IID_IFileSaveDialog);
    WrappedCreation(DialogFactory::Open, IID_IUnknown);
    WrappedCreation(DialogFactory::Save, IID_IFileDialog);
    WrappedCreation(DialogFactory::Open, IID_IModalWindow);
    WrappedCreation(DialogFactory::Save, IID_IFileDialogCustomize);
    WrappedCreation(DialogFactory::Open, IID_IFileDialog2);
    WrappedCreation(DialogFactory::Save, IID_IFileDialog2);
    Bypass(CLSID_FileOpenDialog, IID_IFileOpenDialog, CLSCTX_INPROC_SERVER, false);
    Bypass(CLSID_ShellLink, IID_IUnknown, CLSCTX_INPROC_SERVER, true);
    Bypass(CLSID_FileOpenDialog, IID_IOleWindow, CLSCTX_INPROC_SERVER, true);
    Bypass(CLSID_FileOpenDialog, IID_IPersist, CLSCTX_INPROC_SERVER, true);
    Bypass(CLSID_FileOpenDialog, IID_IFileSaveDialog, CLSCTX_INPROC_SERVER, true);
    Bypass(CLSID_FileOpenDialog, IID_IFileOpenDialog, CLSCTX_INPROC_SERVER | CLSCTX_DISABLE_AAA, true);
    {
        Probe probe; active_probe = &probe;
        auto activation = Configuration();
        ComPtr<IUnknown> outer;
        Check(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&outer))), "create aggregation fixture");
        void* output = nullptr;
        const auto hr = ActivateFileDialog(activation, CLSID_FileOpenDialog, outer.Get(), CLSCTX_INPROC_SERVER, IID_IUnknown, &output);
        ComPtr<IUnknown> returned; returned.Attach(static_cast<IUnknown*>(output));
        Check(probe.calls == 1 && probe.outer == outer.Get() && hr == probe.result &&
            (FAILED(hr) ? !returned : Identity(returned.Get()).Get() == probe.native_identity.Get()), "aggregation passes original outer and native outcome", hr);
    }
    {
        Probe probe; active_probe = &probe;
        auto activation = Configuration();
        probe.fail = true;
        void* output = reinterpret_cast<void*>(1);
        Check(ActivateFileDialog(activation, CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown, &output) == E_ACCESSDENIED &&
            !output && probe.calls == 1 && probe.creations == 0, "original creation failure is preserved with zero output");
        probe.fail = false;
        const auto hr = ActivateFileDialog(activation, CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown, nullptr);
        Check(hr == probe.result && FAILED(hr) && !probe.output_argument && probe.calls == 2, "null output reaches original unchanged", hr);
        activation.original = nullptr; output = reinterpret_cast<void*>(1);
        Check(ActivateFileDialog(activation, CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown, &output) == E_INVALIDARG && !output,
            "missing original callable fails safely without output");
    }
    {
        Probe probe; active_probe = &probe;
        auto activation = Configuration();
        auto fail = std::make_shared<bool>(false);
        activation.services.show_native = ThrowOnCopy(fail); *fail = true;
        void* output = nullptr;
        const auto hr = ActivateFileDialog(activation, CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, &output);
        ComPtr<IUnknown> returned; returned.Attach(static_cast<IUnknown*>(output));
        Check(hr == S_OK && returned && returned.Get() == probe.requested_interface && Identity(returned.Get()).Get() == probe.native_identity.Get() && probe.creations == 1,
            "wrapper allocation failure returns original requested native interface without recreation", hr);
        ComPtr<IFileOpenDialog> open; if (returned) returned.As(&open);
        FILEOPENDIALOGOPTIONS options = 0;
        Check(open && SUCCEEDED(open->GetOptions(&options)), "fallback native interface remains usable after wrapper failure");
    }
    {
        Probe probe; active_probe = &probe; probe.override_success = S_FALSE;
        auto activation = Configuration();
        void* output = nullptr;
        const auto hr = ActivateFileDialog(activation, CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileSaveDialog, &output);
        ComPtr<IUnknown> returned; returned.Attach(static_cast<IUnknown*>(output));
        Check(hr == S_FALSE && returned && Identity(returned.Get()).Get() != probe.native_identity.Get(), "noncanonical original success HRESULT is not replaced by wrapper QI result", hr);
    }
    {
        Probe probe; active_probe = &probe;
        auto activation = Configuration(); probe.recurse = true; probe.activation = &activation;
        bool raw_native = false;
        activation.services.show_native = [&](IFileDialog* native, HWND) {
            raw_native = Identity(native).Get() == probe.native_identity.Get(); return E_ABORT;
        };
        ComPtr<IFileDialog> dialog;
        const auto hr = ActivateFileDialog(activation, CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        Check(SUCCEEDED(hr) && dialog && probe.calls == 2 && probe.creations == 1, "recursive entry bypasses wrapping and creates only one native object", hr);
        Check(dialog && dialog->Show(nullptr) == E_ABORT && raw_native, "TLS guard prevents nested wrapper around the original object");
    }
    {
        std::thread mta([] {
            const auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            Check(SUCCEEDED(hr), "initialize MTA bypass fixture", hr);
            if (SUCCEEDED(hr)) {
                Bypass(CLSID_FileOpenDialog, IID_IFileOpenDialog, CLSCTX_INPROC_SERVER, true);
                CoUninitialize();
            }
        });
        mta.join();
    }
    {
        std::thread uninitialized([] { Bypass(CLSID_FileOpenDialog, IID_IUnknown, CLSCTX_INPROC_SERVER, true); });
        uninitialized.join();
    }
    {
        ComPtr<IFileOpenDialog> native;
        CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&native));
        void* output = reinterpret_cast<void*>(1);
        Check(WrapNativeFileDialog(DialogFactory::Save, native.Get(), {}, IID_IFileSaveDialog, &output) == E_NOINTERFACE && !output,
            "wrap factory mismatch fails without output or replacement creation");
        Check(WrapNativeFileDialog(DialogFactory::Open, nullptr, {}, IID_IFileOpenDialog, &output) == E_INVALIDARG && !output,
            "null native wrapper input is rejected");
    }
    {
        ComPtr<IFileOpenDialog> native;
        CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&native));
        auto token = std::make_shared<int>(1); std::weak_ptr<int> lifetime = token;
        FileDialogBridgeServices services;
        services.show_native = [token](IFileDialog* original, HWND) {
            FILEOPENDIALOGOPTIONS options = 0;
            return SUCCEEDED(original->GetOptions(&options)) ? E_ABORT : E_FAIL;
        };
        ComPtr<IFileDialog> wrapped;
        const auto hr = WrapNativeFileDialog(DialogFactory::Open, native.Get(), services, IID_PPV_ARGS(&wrapped));
        native.Reset(); token.reset(); services = {};
        Check(SUCCEEDED(hr) && wrapped && !lifetime.expired() && wrapped->Show(nullptr) == E_ABORT,
            "wrapper retains supplied native object after caller releases original reference", hr);
        wrapped.Reset();
        Check(lifetime.expired(), "wrapped native factory state releases at final wrapper Release");
    }
    CoUninitialize();
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures ? 1 : 0;
}

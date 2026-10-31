#include "../shell_bridge/file_dialog_class_factory.h"
#include "../shell_bridge/file_dialog_activation.h"
#include "../shell_bridge/file_dialog_activation_ex.h"

#include <atomic>
#include <iostream>
#include <functional>
#include <memory>
#include <new>
#include <ocidl.h>
#include <thread>
#include <wrl/client.h>

using namespace pulse::shell_bridge;
using Microsoft::WRL::ComPtr;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* name, HRESULT hr = S_OK) {
    ++checks; if (!ok) ++failures;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name;
    if (!ok) std::cout << " HRESULT=" << std::hex << static_cast<unsigned long>(hr) << std::dec;
    std::cout << '\n';
}
ComPtr<IUnknown> Identity(IUnknown* object) {
    ComPtr<IUnknown> identity; if (object) object->QueryInterface(IID_PPV_ARGS(&identity)); return identity;
}
struct Probe {
    int gets = 0, creates = 0, locks = 0, destroyed = 0, nested = 0;
    bool controlled = true, licensed = false, fail_get = false, fail_create = false, throw_create = false, throw_lock = false;
    int route = 0;
    CLSID clsid = CLSID_FileOpenDialog;
    DWORD context = 0; LPVOID server = nullptr; IID factory_iid{}, object_iid{};
    IUnknown* outer = nullptr; void** output = nullptr;
    IUnknown* factory_identity = nullptr; void* returned_factory = nullptr; void* returned_object = nullptr;
    HRESULT get_result = E_UNEXPECTED, create_result = E_UNEXPECTED, lock_result = S_OK;
    BOOL locked = FALSE;
    ComPtr<IUnknown> object_identity;
    FileDialogActivation* single = nullptr; FileDialogActivationEx* ex = nullptr;
    IClassFactory* entry_factory = nullptr;
    std::function<void()> during_create;
};
thread_local Probe* active = nullptr;
HRESULT WINAPI RealSingle(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* output) {
    ++active->nested; return ::CoCreateInstance(clsid, outer, context, iid, output);
}
HRESULT WINAPI RealEx(REFCLSID clsid, IUnknown* outer, DWORD context, COSERVERINFO* server, DWORD count, MULTI_QI* results) {
    ++active->nested; return ::CoCreateInstanceEx(clsid, outer, context, server, count, results);
}
class NativeFixture final : public IClassFactory2 {
public:
    explicit NativeFixture(ComPtr<IClassFactory> native) : native_(std::move(native)), probe_(active) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER; *output = nullptr;
        if (iid == IID_IUnknown || iid == IID_IClassFactory) *output = static_cast<IClassFactory*>(this);
        else if (iid == IID_IClassFactory2 && probe_->licensed) *output = static_cast<IClassFactory2*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** output) override {
        ++probe_->creates; probe_->outer = outer; probe_->object_iid = iid; probe_->output = output;
        if (probe_->during_create) probe_->during_create();
        if (probe_->throw_create) throw std::bad_alloc();
        HRESULT hr;
        if (probe_->fail_create) { if (output) *output = nullptr; hr = E_ACCESSDENIED; }
        else if (probe_->route == 1) hr = ActivateFileDialog(*probe_->single, probe_->clsid, outer, CLSCTX_INPROC_SERVER, iid, output);
        else if (probe_->route == 2) {
            MULTI_QI result{&iid, nullptr, S_OK};
            hr = ActivateFileDialogEx(*probe_->ex, probe_->clsid, outer, CLSCTX_INPROC_SERVER, nullptr, 1, &result);
            if (output) *output = result.pItf;
        } else hr = native_->CreateInstance(outer, iid, output);
        probe_->returned_object = output ? *output : nullptr;
        if (SUCCEEDED(hr) && output && *output) probe_->object_identity = Identity(static_cast<IUnknown*>(*output));
        return probe_->create_result = hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        ++probe_->locks; probe_->locked = lock;
        if (probe_->throw_lock) throw std::bad_alloc();
        return probe_->lock_result;
    }
    HRESULT STDMETHODCALLTYPE GetLicInfo(LICINFO*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RequestLicKey(DWORD, BSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateInstanceLic(IUnknown*, IUnknown*, REFIID, BSTR, void**) override { return E_NOTIMPL; }
private:
    ~NativeFixture() { ++probe_->destroyed; }
    std::atomic<ULONG> references_{1}; ComPtr<IClassFactory> native_; Probe* probe_;
};
HRESULT WINAPI OriginalGet(REFCLSID clsid, DWORD context, LPVOID server, REFIID iid, LPVOID* output) {
    auto& probe = *active; ++probe.gets;
    probe.clsid = clsid; probe.context = context; probe.server = server; probe.factory_iid = iid;
    HRESULT hr;
    if (probe.fail_get) { if (output) *output = nullptr; hr = E_ACCESSDENIED; }
    else if (!probe.controlled) hr = ::CoGetClassObject(clsid, context, server, iid, output);
    else {
        ComPtr<IClassFactory> native;
        hr = ::CoGetClassObject(clsid, context, server, IID_PPV_ARGS(&native));
        if (SUCCEEDED(hr)) {
            auto* fixture = new NativeFixture(native);
            hr = fixture->QueryInterface(iid, output); fixture->Release();
        }
    }
    probe.returned_factory = output ? *output : nullptr;
    if (SUCCEEDED(hr) && output && *output) probe.factory_identity = Identity(static_cast<IUnknown*>(*output)).Get();
    return probe.get_result = hr;
}
FileDialogClassActivation Config() {
    FileDialogClassActivation config; config.enabled = true; config.original = OriginalGet; return config;
}
ComPtr<IClassFactory> Get(const FileDialogClassActivation& config, REFCLSID clsid = CLSID_FileOpenDialog) {
    ComPtr<IClassFactory> factory;
    const auto hr = ActivateFileDialogClass(config, clsid, CLSCTX_INPROC_SERVER, nullptr, IID_PPV_ARGS(&factory));
    Check(SUCCEEDED(hr) && factory, "get standard class factory", hr); return factory;
}
struct ThrowCopy {
    std::shared_ptr<bool> fail;
    explicit ThrowCopy(std::shared_ptr<bool> value) : fail(std::move(value)) {}
    ThrowCopy(const ThrowCopy& other) : fail(other.fail) { if (*fail) throw std::bad_alloc(); }
    HRESULT operator()(IFileDialog*, HWND) const { return E_ABORT; }
};
HRESULT WINAPI ViaFactorySingle(REFCLSID, LPUNKNOWN outer, DWORD, REFIID iid, LPVOID* output) {
    ++active->nested; return active->entry_factory->CreateInstance(outer, iid, output);
}
HRESULT WINAPI ViaFactoryEx(REFCLSID, IUnknown* outer, DWORD, COSERVERINFO*, DWORD count, MULTI_QI* results) {
    ++active->nested;
    ComPtr<IUnknown> object;
    const auto hr = active->entry_factory->CreateInstance(outer, IID_PPV_ARGS(&object));
    if (FAILED(hr)) return hr;
    for (DWORD i = 0; i < count; ++i) results[i].hr = object->QueryInterface(*results[i].pIID, reinterpret_cast<void**>(&results[i].pItf));
    return S_OK;
}
}
int main() {
    std::cout << std::unitbuf;
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 2;
    for (const auto& clsid : {CLSID_FileOpenDialog, CLSID_FileSaveDialog}) {
        Probe probe; active = &probe; probe.controlled = false;
        auto config = Config(); int shows = 0;
        config.services.show_native = [&](IFileDialog*, HWND) { ++shows; return E_ABORT; };
        auto factory = Get(config, clsid);
        ComPtr<IUnknown> identity = Identity(factory.Get()); ComPtr<IClassFactory> again;
        factory.As(&again);
        Check(probe.gets == 1 && probe.context == CLSCTX_INPROC_SERVER && probe.factory_iid == IID_IClassFactory &&
            identity.Get() == Identity(again.Get()).Get() && identity.Get() != probe.factory_identity,
            "real Open and Save factories expose proxy identity after one original lookup");
        ComPtr<IClassFactory2> licensed;
        Check(factory.As(&licensed) == E_NOINTERFACE && !licensed, "proxy factory interface-set limit never leaks native licensed identity");
        ComPtr<IFileDialog> dialog;
        const auto hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&dialog));
        Check(SUCCEEDED(hr) && dialog && identity.Get() != probe.factory_identity && dialog->Show(nullptr) == E_ABORT && shows == 1,
            "real standard factory creates default-gated wrapped dialog", hr);
        ComPtr<IClassFactory> native; ::CoGetClassObject(clsid, CLSCTX_INPROC_SERVER, nullptr, IID_PPV_ARGS(&native));
        const auto a = factory->LockServer(TRUE), b = native->LockServer(TRUE);
        const auto c = factory->LockServer(FALSE), d = native->LockServer(FALSE);
        Check(a == b && c == d, "real factory LockServer mirrors balanced native results");
    }
    {
        Probe probe; active = &probe; auto config = Config();
        int shows = 0; bool same_native = false;
        config.services.show_native = [&](IFileDialog* object, HWND) { ++shows; same_native = Identity(object).Get() == probe.object_identity.Get(); return E_ABORT; };
        auto factory = Get(config);
        const auto added = factory->AddRef(); Check(factory->Release() + 1 == added, "factory AddRef and Release balance");
        probe.lock_result = E_ACCESSDENIED;
        Check(factory->LockServer(7) == E_ACCESSDENIED && probe.locks == 1 && probe.locked == 7, "LockServer forwards exact BOOL and native HRESULT once");
        probe.lock_result = S_FALSE;
        Check(factory->LockServer(FALSE) == S_FALSE && probe.locks == 2 && !probe.locked, "LockServer does not invent lock balance or normalize success");
        ComPtr<IFileOpenDialog> dialog;
        const auto hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&dialog));
        Check(SUCCEEDED(hr) && dialog && probe.creates == 1 && probe.object_iid == IID_IFileOpenDialog && !probe.outer &&
            dialog->Show(nullptr) == E_ABORT && shows == 1 && same_native, "CreateInstance reuses exactly one native object");
        probe.fail_create = true; void* output = reinterpret_cast<void*>(1);
        Check(factory->CreateInstance(nullptr, IID_IUnknown, &output) == E_ACCESSDENIED && !output && probe.creates == 2,
            "native creation failure retains HRESULT zero output and no retry");
        probe.fail_create = false;
        ComPtr<IUnknown> outer; ::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&outer));
        const auto aggregate = factory->CreateInstance(outer.Get(), IID_IUnknown, &output);
        Check(aggregate == probe.create_result && probe.outer == outer.Get() && output == probe.returned_object,
            "aggregated creation is unchanged native result", aggregate);
        if (output) static_cast<IUnknown*>(output)->Release(); output = nullptr;
        const auto unknown = factory->CreateInstance(nullptr, IID_IOleWindow, &output);
        Check(unknown == probe.create_result && output == probe.returned_object,
            "unknown object IID is forwarded rather than confused with factory QI", unknown);
        if (output) static_cast<IUnknown*>(output)->Release(); output = nullptr;
        probe.throw_create = true;
        Check(factory->CreateInstance(nullptr, IID_IUnknown, &output) == E_OUTOFMEMORY && !output, "native fixture exception cannot cross CreateInstance ABI");
        probe.throw_lock = true;
        Check(factory->LockServer(TRUE) == E_OUTOFMEMORY, "native fixture exception cannot cross LockServer ABI");
        factory.Reset(); Check(probe.destroyed == 1, "final factory Release destroys native factory without intrinsic cycle");
    }
    {
        Probe probe; active = &probe; auto config = Config(); int shown = 0;
        config.services.show_native = [&](IFileDialog*, HWND) { ++shown; return E_ABORT; };
        auto factory = Get(config);
        ComPtr<IFileDialog2> dialog2;
        const auto hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&dialog2));
        Check(hr == S_OK && dialog2 && dialog2.Get() != probe.returned_object && probe.creates == 1 &&
            dialog2->Show(nullptr) == E_ABORT && shown == 1,
            "class factory recognizes IFileDialog2 object IID and wraps the same native creation", hr);
    }
    for (int mode = 0; mode < 4; ++mode) {
        Probe probe; active = &probe; auto config = Config();
        auto fail = std::make_shared<bool>(false); config.services.show_native = ThrowCopy(fail);
        if (mode == 0) config.enabled = false;
        if (mode == 1) probe.licensed = true;
        if (mode == 2) *fail = true;
        ComPtr<IClassFactory> factory = Get(config);
        if (mode < 3) {
            const char* names[] = {"disabled lookup returns exact native factory", "licensed factory capability preserves whole native factory", "service copy failure preserves exact native factory"};
            Check(factory.Get() == probe.returned_factory && Identity(factory.Get()).Get() == probe.factory_identity, names[mode]);
        } else {
            *fail = true; ComPtr<IFileOpenDialog> dialog;
            const auto hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&dialog));
            Check(hr == S_OK && dialog.Get() == probe.returned_object && probe.creates == 1,
                "object wrapper service copy failure returns original native interface without retry", hr);
        }
    }
    {
        Probe probe; active = &probe; probe.licensed = true; auto config = Config();
        ComPtr<IClassFactory2> licensed;
        const auto hr = ActivateFileDialogClass(config, CLSID_FileOpenDialog, CLSCTX_INPROC_SERVER, nullptr, IID_PPV_ARGS(&licensed));
        Check(hr == S_OK && licensed.Get() == probe.returned_factory, "initial unknown factory IID bypasses directly to licensed native interface", hr);
        probe.fail_get = true; COSERVERINFO server{}; void* output = reinterpret_cast<void*>(1);
        Check(ActivateFileDialogClass(config, CLSID_FileOpenDialog, CLSCTX_REMOTE_SERVER, &server, IID_IClassFactory, &output) == E_ACCESSDENIED &&
            !output && probe.server == &server && probe.context == CLSCTX_REMOTE_SERVER, "remote server/context preserved in controlled failure without remote request");
    }
    for (int mode = 0; mode < 4; ++mode) {
        Probe probe; active = &probe;
        FileDialogActivation single; single.enabled = true; single.original = mode < 2 ? RealSingle : ViaFactorySingle;
        FileDialogActivationEx ex; ex.enabled = true; ex.original = mode < 2 ? RealEx : ViaFactoryEx;
        probe.single = &single; probe.ex = &ex;
        auto config = Config(); int shows = 0; bool raw = false;
        auto show = [&](IFileDialog* object, HWND) { ++shows; raw = Identity(object).Get() == probe.object_identity.Get(); return E_ABORT; };
        config.services.show_native = show; single.services.show_native = show; ex.services.show_native = show;
        auto factory = Get(config); probe.entry_factory = factory.Get();
        ComPtr<IFileDialog> dialog; HRESULT hr;
        if (mode < 2) { probe.route = mode + 1; hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&dialog)); }
        else if (mode == 2) hr = ActivateFileDialog(single, CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        else {
            MULTI_QI result{&IID_IFileDialog, nullptr, S_OK};
            hr = ActivateFileDialogEx(ex, CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, nullptr, 1, &result);
            dialog.Attach(static_cast<IFileDialog*>(result.pItf));
        }
        Check(SUCCEEDED(hr) && dialog && probe.creates == 1 && probe.nested == 1 && dialog->Show(nullptr) == E_ABORT && shows == 1 && raw,
            "shared recursion scope prevents factory and single/Ex double wrapping in both directions", hr);
    }
    {
        Probe probe; active = &probe; auto config = Config(); auto factory = Get(config);
        HRESULT created = S_OK, locked = S_OK; const int before = probe.creates;
        std::thread worker([&] { void* output = nullptr; created = factory->CreateInstance(nullptr, IID_IUnknown, &output); locked = factory->LockServer(FALSE); });
        worker.join();
        Check(created == RPC_E_WRONG_THREAD && locked == RPC_E_WRONG_THREAD && probe.creates == before && probe.locks == 0,
            "proxy CreateInstance and LockServer enforce creating STA thread");
    }
    {
        Probe probe; active = &probe; auto config = Config();
        config.services.show_native = [](IFileDialog*, HWND) { return E_ABORT; };
        auto factory = Get(config);
        probe.during_create = [&] { factory.Reset(); };
        IClassFactory* invoked = factory.Get();
        ComPtr<IFileDialog> dialog;
        const auto hr = invoked->CreateInstance(nullptr, IID_PPV_ARGS(&dialog));
        Check(SUCCEEDED(hr) && dialog && !factory && probe.destroyed == 1 && dialog->Show(nullptr) == E_ABORT,
            "in-flight CreateInstance retains factory through reentrant final caller Release", hr);
    }
    {
        Probe probe; active = &probe; auto config = Config();
        ComPtr<IClassFactory> factory;
        const DWORD flags = CLSCTX_INPROC_SERVER | CLSCTX_DISABLE_AAA;
        const auto hr = ActivateFileDialogClass(config, CLSID_FileOpenDialog, flags, nullptr, IID_PPV_ARGS(&factory));
        Check(hr == probe.get_result && probe.context == flags && factory.Get() == probe.returned_factory,
            "additional class-context flags preserve original factory identity", hr);
        ComPtr<IClassFactory> other;
        const auto other_hr = ActivateFileDialogClass(config, CLSID_ShellLink, CLSCTX_INPROC_SERVER, nullptr, IID_PPV_ARGS(&other));
        Check(other_hr == probe.get_result && other.Get() == probe.returned_factory,
            "unknown class retains original class object", other_hr);
    }
    {
        std::thread worker([] {
            const auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            Check(SUCCEEDED(hr), "initialize MTA class-object fixture", hr);
            if (SUCCEEDED(hr)) {
                { Probe probe; active = &probe; auto config = Config();
                  ComPtr<IClassFactory> factory;
                  const auto created = ActivateFileDialogClass(config, CLSID_FileOpenDialog, CLSCTX_INPROC_SERVER, nullptr, IID_PPV_ARGS(&factory));
                  Check(created == probe.get_result && factory.Get() == probe.returned_factory,
                      "MTA class lookup bypasses without altering native result", created); }
                CoUninitialize();
            }
        }); worker.join();
    }
    CoUninitialize();
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures ? 1 : 0;
}

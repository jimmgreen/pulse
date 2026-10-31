#include "file_dialog_class_factory.h"
#include "file_dialog_activation_scope.h"

#include <atomic>
#include <new>
#include <ocidl.h>
#include <wrl/client.h>

namespace pulse::shell_bridge {
namespace {
using Microsoft::WRL::ComPtr;
bool Sta() {
    APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
    return SUCCEEDED(CoGetApartmentType(&apartment, &qualifier)) && (apartment == APTTYPE_STA || apartment == APTTYPE_MAINSTA);
}
bool KnownObject(REFIID iid, DialogFactory factory) {
    return iid == IID_IUnknown || iid == IID_IModalWindow || iid == IID_IFileDialog || iid == IID_IFileDialog2 || iid == IID_IFileDialogCustomize ||
        (iid == IID_IFileOpenDialog && factory == DialogFactory::Open) || (iid == IID_IFileSaveDialog && factory == DialogFactory::Save);
}
class Factory final : public IClassFactory {
public:
    Factory(DialogFactory factory, ComPtr<IClassFactory> native, const FileDialogBridgeServices& services)
        : factory_(factory), native_(std::move(native)), services_(services), thread_(GetCurrentThreadId()) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory) return E_NOINTERFACE;
        *output = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const auto count = --references_;
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** output) noexcept override {
        if (output) *output = nullptr;
        if (GetCurrentThreadId() != thread_) return RPC_E_WRONG_THREAD;
        AddRef(); struct Hold { Factory* self; ~Hold() { self->Release(); } } hold{this};
        detail::ActivationScope recursion;
        HRESULT created = E_FAIL;
        try {
            created = native_->CreateInstance(outer, iid, output);
            if (FAILED(created)) { if (output) *output = nullptr; return created; }
        } catch (const std::bad_alloc&) { if (output) *output = nullptr; return E_OUTOFMEMORY; }
        catch (...) { if (output) *output = nullptr; return E_FAIL; }
        if (created != S_OK || recursion.nested || outer || !output || !*output || !KnownObject(iid, factory_)) return created;
        ComPtr<IUnknown> native;
        native.Attach(static_cast<IUnknown*>(*output));
        void* wrapped = nullptr;
        const auto hr = WrapNativeFileDialog(factory_, native.Get(), services_, iid, &wrapped);
        *output = SUCCEEDED(hr) && wrapped ? wrapped : native.Detach();
        return created;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) noexcept override {
        if (GetCurrentThreadId() != thread_) return RPC_E_WRONG_THREAD;
        AddRef(); struct Hold { Factory* self; ~Hold() { self->Release(); } } hold{this};
        try { return native_->LockServer(lock); }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        catch (...) { return E_FAIL; }
    }
private:
    std::atomic<ULONG> references_{1};
    DialogFactory factory_;
    ComPtr<IClassFactory> native_;
    FileDialogBridgeServices services_;
    DWORD thread_;
};
}

HRESULT ActivateFileDialogClass(const FileDialogClassActivation& activation, REFCLSID clsid,
    DWORD context, LPVOID server, REFIID iid, LPVOID* output) noexcept {
    if (output) *output = nullptr;
    if (!activation.original) return E_INVALIDARG;
    detail::ActivationScope recursion;
    HRESULT created = E_FAIL;
    try { created = activation.original(clsid, context, server, iid, output); }
    catch (const std::bad_alloc&) { if (output) *output = nullptr; return E_OUTOFMEMORY; }
    catch (...) { if (output) *output = nullptr; return E_FAIL; }
    if (FAILED(created)) { if (output) *output = nullptr; return created; }
    if (created != S_OK || !activation.enabled || recursion.nested || !output || !*output || server ||
        context != CLSCTX_INPROC_SERVER || (iid != IID_IUnknown && iid != IID_IClassFactory) || !Sta() ||
        (clsid != CLSID_FileOpenDialog && clsid != CLSID_FileSaveDialog)) return created;
    // Retain exactly the requested native interface until all preparation works.
    ComPtr<IUnknown> original;
    original.Attach(static_cast<IUnknown*>(*output));
    try {
        ComPtr<IClassFactory> native;
        ComPtr<IClassFactory2> licensed;
        if (SUCCEEDED(original.As(&native)) && native && original.As(&licensed) == E_NOINTERFACE) {
            auto* proxy = new Factory(clsid == CLSID_FileSaveDialog ? DialogFactory::Save : DialogFactory::Open, native, activation.services);
            void* wrapped = nullptr;
            const auto hr = proxy->QueryInterface(iid, &wrapped);
            proxy->Release();
            if (SUCCEEDED(hr) && wrapped) { *output = wrapped; return created; }
        }
    } catch (...) {
        // A service copy/allocation failure must preserve the native factory.
    }
    *output = original.Detach(); return created;
}
}

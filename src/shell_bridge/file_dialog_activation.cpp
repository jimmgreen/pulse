#include "file_dialog_activation.h"
#include "file_dialog_activation_scope.h"

#include <wrl/client.h>

namespace pulse::shell_bridge {
namespace {
const char* NativeGate(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, DialogFactory& factory) {
    if (outer) return "single-native-aggregation";
    if (!detail::InprocActivationContext(context)) return "single-native-context";
    if (clsid == CLSID_FileOpenDialog) factory = DialogFactory::Open;
    else if (clsid == CLSID_FileSaveDialog) factory = DialogFactory::Save;
    else return "single-native-class";
    if (iid != IID_IUnknown && iid != IID_IModalWindow && iid != IID_IFileDialog && iid != IID_IFileDialog2 &&
        iid != IID_IFileDialogCustomize &&
        !(iid == IID_IFileOpenDialog && factory == DialogFactory::Open) &&
        !(iid == IID_IFileSaveDialog && factory == DialogFactory::Save)) return "single-native-requested-interface";
    APTTYPE apartment{};
    APTTYPEQUALIFIER qualifier{};
    return SUCCEEDED(CoGetApartmentType(&apartment, &qualifier)) &&
        (apartment == APTTYPE_STA || apartment == APTTYPE_MAINSTA) ? nullptr : "single-native-apartment";
}
}

HRESULT ActivateFileDialog(const FileDialogActivation& activation, REFCLSID clsid,
                           LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* output) noexcept {
    if (!activation.original) {
        if (output) *output = nullptr;
        return E_INVALIDARG;
    }
    if (output) *output = nullptr;
    // Keep the caller's exact IID/context/output argument on the original call,
    // including invalid combinations and null output. Never retry activation.
    detail::ActivationScope recursion;
    const bool bypass = !activation.enabled || recursion.nested || !output;
    const HRESULT created = activation.original(clsid, outer, context, iid, output);
    const auto trace = [&](const char* stage, HRESULT hr) {
        if (activation.outcome) *activation.outcome = stage;
        detail::TraceActivation(activation.services.on_native_reason, recursion.nested, clsid, stage, context, hr);
    };
    if (FAILED(created)) {
        if (output) *output = nullptr;
        trace("single-original-failed", created);
        return created;
    }
    if (bypass || !*output) return created;
    DialogFactory factory{};
    if (const auto gate = NativeGate(clsid, outer, context, iid, factory)) {
        trace(gate, created);
        return created;
    }

    // Every COM interface begins with IUnknown. Adopt the requested interface,
    // retaining its exact pointer for rollback if wrapping cannot be completed.
    Microsoft::WRL::ComPtr<IUnknown> native;
    native.Attach(static_cast<IUnknown*>(*output));
    void* wrapped = nullptr;
    const HRESULT wrapped_hr = WrapNativeFileDialog(factory, native.Get(), activation.services, iid, &wrapped);
    const bool published = SUCCEEDED(wrapped_hr) && wrapped;
    *output = published ? wrapped : native.Detach();
    trace(published ? "single-wrapper-published" : "single-native-wrapper-creation", wrapped_hr);
    return created;
}
}

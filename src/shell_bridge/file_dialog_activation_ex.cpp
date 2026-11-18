#include "file_dialog_activation_ex.h"
#include "file_dialog_activation_scope.h"

#include <array>
#include <cstdio>
#include <wrl/client.h>

namespace pulse::shell_bridge {
namespace {
using Microsoft::WRL::ComPtr;
constexpr DWORD kMaxInterfaces = 64;
bool Known(REFIID iid, DialogFactory factory) {
    return iid == IID_IUnknown || iid == IID_IModalWindow || iid == IID_IFileDialog || iid == IID_IFileDialog2 || iid == IID_IFileDialogCustomize ||
        (factory == DialogFactory::Open && iid == IID_IFileOpenDialog) ||
        (factory == DialogFactory::Save && iid == IID_IFileSaveDialog);
}
void Observe(const FileDialogActivationEx& activation, const char* reason, const IID* iid = nullptr) noexcept {
    try {
        if (activation.services.on_native_reason) activation.services.on_native_reason(reason, iid);
    } catch (...) {
        // A diagnostic must never change activation or trigger a second call.
    }
}
const char* Eligibility(REFCLSID clsid, IUnknown* outer, DWORD context, COSERVERINFO* server,
              DWORD count, MULTI_QI* results, DialogFactory& factory, const IID*& rejected) {
    if (outer) return "ex-native-aggregation";
    if (server) return "ex-native-server-info";
    if (context != CLSCTX_INPROC_SERVER) return "ex-native-context";
    if (!count || count > kMaxInterfaces || !results) return "ex-native-result-array";
    if (clsid == CLSID_FileOpenDialog) factory = DialogFactory::Open;
    else if (clsid == CLSID_FileSaveDialog) factory = DialogFactory::Save;
    else return "ex-native-class";
    APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
    if (FAILED(CoGetApartmentType(&apartment, &qualifier))) return "ex-native-apartment-unavailable";
    if (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) return "ex-native-apartment";
    for (DWORD i = 0; i < count; ++i) {
        if (!results[i].pIID) return "ex-native-input-iid";
        if (results[i].pItf) return "ex-native-input-pointer";
        // The documented input HRESULT is zero, but local COM also accepts
        // seeded status fields. Preserve them for the original call and decide
        // from its canonical successful outputs, never from the stale status.
        if (!Known(*results[i].pIID, factory)) {
            rejected = results[i].pIID;
            return "ex-native-requested-interface";
        }
    }
    return nullptr;
}
}

HRESULT ActivateFileDialogEx(const FileDialogActivationEx& activation, REFCLSID clsid,
    IUnknown* outer, DWORD context, COSERVERINFO* server, DWORD count, MULTI_QI* results) noexcept {
    if (!activation.original) { Observe(activation, "ex-missing-original"); return E_INVALIDARG; }
    detail::ActivationScope recursion;
    DialogFactory factory{};
    const IID* rejected = nullptr;
    const char* reason = !activation.enabled ? "ex-native-disabled" : recursion.nested ? "ex-native-nested" :
        Eligibility(clsid, outer, context, server, count, results, factory, rejected);
    const bool eligible = !reason;
    char inputs[128]{};
    std::snprintf(inputs, sizeof(inputs), "ex-input-context=%08lX-count=%lu-factory=%u",
        static_cast<unsigned long>(context), static_cast<unsigned long>(count),
        clsid == CLSID_FileOpenDialog ? 1u : clsid == CLSID_FileSaveDialog ? 2u : 0u);
    Observe(activation, inputs);
    if (reason) Observe(activation, reason, rejected);
    std::array<IID, kMaxInterfaces> requested{};
    if (eligible) for (DWORD i = 0; i < count; ++i) {
        requested[i] = *results[i].pIID;
        if (results[i].hr != S_OK) {
            char status[96]{};
            std::snprintf(status, sizeof(status), "ex-seeded-result index=%lu hr=%08lX",
                static_cast<unsigned long>(i), static_cast<unsigned long>(results[i].hr));
            Observe(activation, status);
        }
    }
    const HRESULT created = activation.original(clsid, outer, context, server, count, results);
    if (!eligible || created != S_OK) {
        if (eligible) Observe(activation, "ex-native-aggregate-result");
        return created;
    }
    try {
        ComPtr<IUnknown> identity;
        for (DWORD i = 0; i < count; ++i) {
            if (results[i].hr != S_OK || !results[i].pItf || !results[i].pIID || *results[i].pIID != requested[i]) { Observe(activation, "ex-native-output-state"); return created; }
            ComPtr<IUnknown> candidate;
            if (results[i].pItf->QueryInterface(IID_PPV_ARGS(&candidate)) != S_OK || !candidate) { Observe(activation, "ex-native-identity-query"); return created; }
            if (!identity) identity = candidate;
            else if (identity.Get() != candidate.Get()) { Observe(activation, "ex-native-mixed-identities"); return created; }
        }
        ComPtr<IUnknown> wrapper;
        if (WrapNativeFileDialog(factory, identity.Get(), activation.services, IID_PPV_ARGS(&wrapper)) != S_OK || !wrapper) { Observe(activation, "ex-native-wrapper-creation"); return created; }
        std::array<ComPtr<IUnknown>, kMaxInterfaces> staged;
        for (DWORD i = 0; i < count; ++i) {
            void* queried = nullptr;
            const HRESULT hr = wrapper->QueryInterface(requested[i], &queried);
            staged[i].Attach(static_cast<IUnknown*>(queried));
            if (hr != S_OK || !staged[i]) { Observe(activation, "ex-native-wrapper-interface", &requested[i]); return created; }
        }
        // Publish every result before releasing any original caller reference:
        // a native final Release can run arbitrary code and reenter the caller.
        std::array<ComPtr<IUnknown>, kMaxInterfaces> originals;
        for (DWORD i = 0; i < count; ++i) {
            originals[i].Attach(results[i].pItf);
            results[i].pItf = staged[i].Detach();
        }
        Observe(activation, "ex-wrapper-published");
        return created;
    } catch (...) {
        Observe(activation, "ex-native-wrapper-exception");
        // No original field is changed until the allocation-free commit above.
        return created;
    }
}
}

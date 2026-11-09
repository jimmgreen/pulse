#pragma once

#include "file_dialog_contract.h"
#include <functional>
#include <memory>
#include <shobjidl.h>

namespace pulse::shell_bridge {
struct FileDialogInprocessProvider;
class FileDialogClientState;
struct FileDialogBridgeServices {
    std::wstring executable;
    std::shared_ptr<FileDialogClientState> client_state; // Optional isolated state store for embedding/tests.
    // Optional passive early-activation capture. Resolve once on the Show STA;
    // false/throw means native Show, without waiting or retrying activation.
    std::function<bool(FileDialogBridgeServices&)> resolve_on_show;
    // Default is absent: unverified native validation always uses native Show.
    // A provider must inspect original options, validation category and runtime
    // obligations as well as the exact request; wire data alone loses native
    // semantics. Tests use a controlled provider to exercise wrapper routing.
    std::function<bool(const ContractDecision&)> validation_verified;
    std::function<ipc::picker::Message(const ipc::picker::Message&, HWND, HANDLE)> run_bridge;
    // Optional deterministic test seam. Absent means real native Show(owner).
    std::function<HRESULT(IFileDialog*, HWND)> show_native;
    // Diagnostics cannot authorize a contract or affect its outcome.
    std::function<void(const ContractDecision&, bool, bool, size_t)> observe_contract;
    AccessPolicy save_access_policy = AccessPolicy::None; // Independent opt-in; LocalSaveV1 is inprocess-only.
    AccessPolicy access_policy = AccessPolicy::None; // Opt-in; Save/native parity unchanged.
    // Absent by default. Explicit v2 provider; no runtime admission is enabled here.
    std::shared_ptr<const FileDialogInprocessProvider> inprocess_provider;
    // Show basic dialogs (no events, custom controls, properties or places)
    // in pulse.exe over the picker pipe even when an in-process provider is
    // available; anything the host cannot carry keeps the in-process route.
    bool prefer_bridge = false;
    // Optional: load inprocess_provider only when a Show actually takes the
    // in-process route (a pulse.exe Show never loads it). Called on the Show
    // STA; null means the provider is unavailable and that Show stays native.
    std::function<std::shared_ptr<const FileDialogInprocessProvider>()> load_inprocess_provider;
    // Diagnostic only, no paths. IID is borrowed for this call, or null.
    // Includes harmless unsupported QI probes as well as actual native gates.
    std::function<void(const char* reason, const IID* iid)> on_native_reason;
};
// Explicit opt-in factory only. No registration, injection, or system hooks.
// STA-affine; QI exposes the chosen factory interfaces, native-supported
// IFileDialog2 and Customize with one
// controlling identity. Without an explicit v2 provider events force native Show;
// unsupported customization does. A pending resolver only captures native state;
// it cannot authorize Show. Unknown interfaces still return E_NOINTERFACE, so this
// wrapper does not promise universal native QI transparency.
// V2 FileOk uses ordered subscription snapshots; control
// callbacks also receive wrapper identity. IFileDialog2-only settings also
// force native Show. Unknown interfaces are not exposed.
// Wrap a freshly activated, unconfigured native instance on its creating STA.
// Does not create another object or consume the caller's native reference.
// Failure leaves output null and the native object available to the caller.
HRESULT WrapNativeFileDialog(DialogFactory factory, IUnknown* native,
                            const FileDialogBridgeServices& services, REFIID iid, void** output) noexcept;
HRESULT CreatePulseFileDialog(DialogFactory factory, const FileDialogBridgeServices& services,
                              REFIID iid, void** output) noexcept;
}

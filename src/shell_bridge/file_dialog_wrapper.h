#pragma once

#include "file_dialog_contract.h"
#include <functional>
#include <memory>
#include <shobjidl.h>

namespace pulse::shell_bridge {
// Undocumented Windows file-dialog interface (comdlg32, shell32, FileDialogBroker).
// The brokered picker host (PickerHost.exe, behind Settings and packaged apps)
// requires it and abandons the picker when it is missing.
inline constexpr IID kFileDialogPrivateIid{0x9EA5491C, 0x89C8, 0x4BEF, {0x93, 0xD3, 0x7F, 0x66, 0x5F, 0xB8, 0x2A, 0x33}};
struct FileDialogInprocessProvider;
class FileDialogClientState;
enum class FileDialogShowFallback {
    None, ResolveNotReady, ProviderUnavailable, InitialFolderUnavailable, Contract,
    ProviderPrepareFailed, ProviderFailedBeforeUi
};
enum class FileDialogShowFailurePhase {
    None,
    ProviderShowFailed,
    BridgeResponseInvalid,
    BridgeResultFailed,
    ShellItemMaterializeFailed,
    CloseRequested,
    UnhandledException
};
// Passive per-Show evidence. ui_shown describes Pulse only, and remains unknown
// when the route has no explicit provider UI_SHOWN result (including the pipe).
struct FileDialogShowObservation {
    uint64_t sequence = 0;
    std::optional<DialogFactory> factory;
    HRESULT result = E_PENDING;
    bool native_fallback = false, provider_started = false, accepted_result = false;
    std::optional<bool> ui_shown;
    NativeReason contract_reason = NativeReason::None;
    FileDialogShowFallback fallback = FileDialogShowFallback::None;
    FileDialogShowFailurePhase failure_phase = FileDialogShowFailurePhase::None;
};
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
    // Local legacy-folder contract; never sent to a remote picker.
    uint32_t legacy_browse_policy = 0;
    std::function<void(HWND, UINT, LPARAM)> legacy_browse_callback;
    // Diagnostics cannot authorize a contract or affect its outcome.
    std::function<void(const ContractDecision&, bool, bool, size_t)> observe_contract;
    // true marks begin, false marks terminal; exceptions never affect Show.
    std::function<void(bool, const FileDialogShowObservation&)> observe_show;
    AccessPolicy save_access_policy = AccessPolicy::None; // Independent opt-in; LocalSaveV1 is inprocess-only.
    AccessPolicy access_policy = AccessPolicy::None; // Opt-in; Save/native parity unchanged.
    // Absent by default. Explicit v2 provider; no runtime admission is enabled here.
    std::shared_ptr<const FileDialogInprocessProvider> inprocess_provider;
    // Show basic dialogs (no events, custom controls, properties or places)
    // in pulse.exe over the picker pipe even when an in-process provider is
    // available; anything the host cannot carry keeps the in-process route.
    bool prefer_bridge = false;
    // The brokered picker host queries kFileDialogPrivateIid, then still calls Show
    // and reads results through this wrapper. true answers that query with the
    // native interface and keeps Pulse eligible; false keeps the native dialog.
    bool private_interface_passthrough = false;
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

#include "file_dialog_wrapper.h"
#include "../common/picker_shell_namespace.h"
#include "picker_shell_results.h"
#include "file_dialog_events.h"
#include "file_dialog_inprocess_session.h"
#include "file_dialog_client_state.h"
#include "file_dialog_properties.h"
#include "../common/picker_navigation.h"
#include "../ipc/picker_client.h"
#include "../ui/file_save_model.h"

#include <shlobj.h>
#include <propsys.h>
#include <propkey.h>
#include <atomic>
#include <cstdio>
#include <algorithm>
#include <new>
#include <map>
#include <wrl/client.h>

namespace pulse::shell_bridge {
namespace {
namespace wire = ipc::picker;
using Microsoft::WRL::ComPtr;
// The pulse.exe host's pre-UI local-save check (picker_host_mode.cpp), run by
// the caller so a target the host would decline never crosses the pipe.
bool BridgeSaveTargetResolves(const wire::Request& body) {
    const auto wide = [](const std::u16string& text) { return std::wstring(text.begin(), text.end()); };
    ui::SaveTargetRequest target;
    target.directory = wide(body.initial_directory);
    target.filename = body.filename.empty() ? L"pulse-validation" : wide(body.filename);
    for (const auto& filter : body.filters) target.filters.push_back({wide(filter.label), wide(filter.pattern)});
    target.filter_index = body.filter_index;
    target.default_extension = wide(body.default_extension);
    target.native_save_defaults = (body.options & wire::SaveNativeDefaults) != 0;
    target.no_test_file_create = (body.options & wire::SaveNoTestFileCreate) != 0;
    target.require_existing_save_parent = (body.options & wire::SaveRequireExistingParent) != 0;
    return ui::ResolveSaveTarget(target).state == ui::SaveTargetState::Resolved;
}
class FileDialog final : public IFileOpenDialog, public IFileSaveDialog, public IFileDialogCustomize, public IFileDialog2, public IOleWindow {
public:
    FileDialog(DialogFactory factory, ComPtr<IFileDialog> native, ComPtr<IFileDialog2> native2, FileDialogBridgeServices services)
        : factory_(factory), native_(std::move(native)), native2_(std::move(native2)), services_(std::move(services)), thread_(GetCurrentThreadId()),
          event_owner_(std::make_shared<detail::FileDialogEventOwner>()) { event_owner_->dialog = Identity(); if (services_.resolve_on_show) ObserveNativeReason("early-wrapper-created"); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid == IID_IUnknown || iid == IID_IModalWindow || iid == IID_IFileDialog)
            *output = Identity();
        else if (iid == IID_IFileOpenDialog && factory_ == DialogFactory::Open) *output = static_cast<IFileOpenDialog*>(this);
        else if (iid == IID_IFileSaveDialog && factory_ == DialogFactory::Save) *output = static_cast<IFileSaveDialog*>(this);
        else if (iid == IID_IFileDialog2 && native2_) *output = static_cast<IFileDialog2*>(this);
        else if (iid == IID_IOleWindow) {
            *output = static_cast<IOleWindow*>(this);
        }
        else if (iid == IID_IFileDialogCustomize) {
            ObserveNativeReason("query-interface-customize-only", &iid);
            *output = static_cast<IFileDialogCustomize*>(this);
        }
        else { ObserveNativeReason("query-interface-not-supported", &iid); return E_NOINTERFACE; }
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count;
        {
            std::lock_guard lock(event_owner_->mutex);
            count = --references_;
            if (!count) event_owner_->dialog = nullptr;
        }
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE Show(HWND owner) override {
        if (FAILED(Thread())) return RPC_E_WRONG_THREAD;
        if (showing_ || inspecting_metadata_) return HRESULT_FROM_WIN32(ERROR_BUSY);
        if (custom_desynchronized_) return E_UNEXPECTED;
        AddRef();
        struct Hold { FileDialog* self; ~Hold() { self->inprocess_.reset(); self->pending_folders_.clear(); self->showing_ = false; self->Release(); } } hold{this};
        showing_ = true;
        // Clear the previous result before any fallible preparation of this Show.
        item_.Reset(); items_.Reset(); returned_name_.clear(); result_count_ = 0; result_index_ = 0; bridge_result_ = false;
        cancel_.Reset();
        readonly_properties_result_ = false;
        properties_accepted_ = false;
        if (properties_owned_) { const HRESULT reset = properties_.ResetEdits(); if (FAILED(reset)) return reset; }
        close_result_ = S_OK; close_requested_ = false; inprocess_used_ = false; allow_missing_result_ = false; result_native_options_ = 0;
        try {
            if (services_.resolve_on_show) {
                auto resolve = std::move(services_.resolve_on_show);
                services_.resolve_on_show = {};
                FileDialogBridgeServices resolved;
                bool ready = false;
                try { ready = resolve(resolved); } catch (...) {}
                if (!ready) {
                    ObserveNativeReason("resolve-not-ready-native");
                    shown_ = true;
                    return services_.show_native ? services_.show_native(native_.Get(), owner) : native_->Show(owner);
                }
                resolved.resolve_on_show = {};
                services_ = std::move(resolved);
                // Deferred capture is not authorization. IPC cannot represent
                // the retained event/custom-control contract.
                if (!services_.inprocess_provider && !services_.load_inprocess_provider && (!subscriptions_.empty() || !custom_controls_.empty()))
                    MarkNativeOnly("deferred-contract-needs-inprocess");
                ObserveNativeReason("deferred-services-bound-at-show");
            }
            GUID guid{};
            HRESULT hr = CoCreateGuid(&guid);
            if (FAILED(hr)) return hr;
            wire::RequestId id{};
            for (size_t i = 0; i < 4; ++i) id[i] = static_cast<uint8_t>(guid.Data1 >> (i * 8));
            for (size_t i = 0; i < 2; ++i) { id[4 + i] = static_cast<uint8_t>(guid.Data2 >> (i * 8)); id[6 + i] = static_cast<uint8_t>(guid.Data3 >> (i * 8)); }
            for (size_t i = 0; i < 8; ++i) id[8 + i] = guid.Data4[i];
            const auto policy = factory_ == DialogFactory::Save ? services_.save_access_policy : services_.access_policy;
            const auto provided = [&](uint32_t feature) {
                return services_.inprocess_provider && (services_.inprocess_provider->api.implemented_features & feature);
            };
            // pulse.exe route: basic dialogs only; everything retained for
            // events, custom controls, properties or places stays in-process.
            const bool bridge_route = services_.prefer_bridge && (services_.run_bridge || !services_.executable.empty()) &&
                subscriptions_.empty() && custom_controls_.empty() && !custom_group_ && !properties_owned_ && places_.empty();
            // The pulse.exe host carries local-save access and caller-validated
            // Open/Save over the pipe; native extended options, places,
            // properties and missing-file Open remain provider features.
            const auto make_context = [&](bool bridge) {
                PreparationContext c;
                c.native_options_supported = !bridge && provided(PULSE_PICKER_UI_V2_FEATURE_NATIVE_OPTIONS);
                c.save_properties_owned = !bridge && properties_owned_ && provided(PULSE_PICKER_UI_V2_FEATURE_PROPERTIES);
                c.client_state_owned = (bridge || services_.inprocess_provider) && static_cast<bool>(client_state_);
                c.filesystem_places_owned = !bridge && provided(PULSE_PICKER_UI_V2_FEATURE_FILESYSTEM_PLACES);
                c.native_save_defaults_supported = bridge || provided(PULSE_PICKER_UI_V2_FEATURE_SAVE_NATIVE_DEFAULTS);
                c.save_no_test_create_supported = bridge || provided(PULSE_PICKER_UI_V2_FEATURE_SAVE_NO_TEST_CREATE);
                c.save_caller_validation_supported = bridge || provided(PULSE_PICKER_UI_V2_FEATURE_SAVE_SKIP_ACCESS);
                c.open_missing_supported = !bridge && provided(PULSE_PICKER_UI_V2_FEATURE_OPEN_MISSING);
                c.caller_validation_supported = bridge || provided(PULSE_PICKER_UI_V2_FEATURE_OPEN_SKIP_ACCESS);
                return c;
            };
            if ((bridge_route || services_.inprocess_provider) && client_state_ && client_guid_ && !explicit_folder_) {
                if (auto remembered = client_state_->Read(*client_guid_)) {
                    ComPtr<IShellItem> folder;
                    if (SHCreateItemFromParsingName(remembered->c_str(), nullptr, IID_PPV_ARGS(&folder)) == S_OK)
                        native_->SetFolder(folder.Get());
                }
            }
            // Only the new explicit policy resolves the mirrored native starting folder.
            // GetFolder is documented before Show; it does not display a dialog.
            // False: the folder cannot be represented for this route.
            const auto resolve_initial = [&](PreparationContext& c) {
                if (!(((policy == AccessPolicy::LocalOpenV1 && factory_ == DialogFactory::Open) ||
                    (policy == AccessPolicy::LocalSaveV1 && factory_ == DialogFactory::Save)) && !native_only_)) return true;
                ComPtr<IShellItem> folder;
                PWSTR path = nullptr;
                SFGAOF attributes = SFGAO_FILESYSTEM | SFGAO_FOLDER;
                const HRESULT folder_hr = native_->GetFolder(&folder);
                if (SUCCEEDED(folder_hr) && folder && SUCCEEDED(folder->GetAttributes(attributes, &attributes)) &&
                    (attributes & SFGAO_FOLDER) &&
                    ((attributes & SFGAO_FILESYSTEM) || c.native_options_supported) &&
                    SUCCEEDED(folder->GetDisplayName((attributes & SFGAO_FILESYSTEM) ? SIGDN_FILESYSPATH : SIGDN_DESKTOPABSOLUTEPARSING, &path))) {
                    std::u16string resolved;
                    bool copied = false;
                    try { copied = CopyText(path, resolved); }
                    catch (...) { CoTaskMemFree(path); throw; }
                    CoTaskMemFree(path);
                    if (!copied) return false;
                    c.resolved_initial_directory = std::move(resolved);
                    return true;
                }
                CoTaskMemFree(path);
                return false;
            };
            std::optional<wire::Message> bridge_request;
            ContractDecision decision;
            if (bridge_route) {
                auto c = make_context(true);
                if (resolve_initial(c)) {
                    auto candidate = PrepareFileDialogContract(factory_, calls_, id, policy, c);
                    if (!native_only_ && candidate.disposition == Disposition::Prepared && candidate.request &&
                        services_.validation_verified && services_.validation_verified(candidate)) {
                        bridge_request = BridgeRequestFor(candidate);
                        // A host answer after the pipe is crossed is terminal, so
                        // run the host's local-save target check here first: a
                        // target it would decline keeps the in-process route.
                        if (bridge_request && candidate.supported_access_policy == AccessPolicy::LocalSaveV1 &&
                            !BridgeSaveTargetResolves(std::get<wire::Request>(bridge_request->body))) bridge_request.reset();
                        if (bridge_request) decision = std::move(candidate);
                    }
                }
                ObserveNativeReason(bridge_request ? "bridge-route-selected" : "bridge-route-declined");
            }
            if (!bridge_request) {
                if (!services_.inprocess_provider && services_.load_inprocess_provider && !native_only_) {
                    // Deferred provider: only a Show that leaves the pulse.exe
                    // route loads the in-process UI; failure keeps it native.
                    std::shared_ptr<const FileDialogInprocessProvider> loaded;
                    try { loaded = services_.load_inprocess_provider(); } catch (...) {}
                    ObserveNativeReason(loaded ? "provider-loaded-on-demand" : "provider-load-on-demand-failed");
                    if (!loaded) {
                        shown_ = true;
                        return services_.show_native ? services_.show_native(native_.Get(), owner) : native_->Show(owner);
                    }
                    services_.inprocess_provider = std::move(loaded);
                }
                auto context = make_context(false);
                if (!resolve_initial(context)) {
                    shown_ = true;
                    return services_.show_native ? services_.show_native(native_.Get(), owner) : native_->Show(owner);
                }
                decision = PrepareFileDialogContract(factory_, calls_, id, policy, context);
            }
            if (services_.observe_contract) {
                try { services_.observe_contract(decision, native_only_, shown_, subscriptions_.size()); }
                catch (...) {} // A diagnostic failure must not change dialog behavior.
            }
            const bool eligible = !native_only_ && !custom_group_ && decision.disposition == Disposition::Prepared && decision.request &&
                services_.validation_verified && services_.validation_verified(decision);
            shown_ = true;
            const bool local_save = decision.supported_access_policy == AccessPolicy::LocalSaveV1;
            // The v1 pipe without bridge_request cannot carry local save or labels.
            if (local_save && !services_.inprocess_provider && !bridge_request)
                return services_.show_native ? services_.show_native(native_.Get(), owner) : native_->Show(owner);
            if ((decision.title || decision.ok_button_label || decision.file_name_label || decision.open_default_extension || decision.require_unencrypted_save) &&
                !services_.inprocess_provider && !bridge_request)
                return services_.show_native ? services_.show_native(native_.Get(), owner) : native_->Show(owner);
            if (!eligible) return services_.show_native ? services_.show_native(native_.Get(), owner) : native_->Show(owner);
            if (services_.inprocess_provider && !custom_group_ && !bridge_request) {
                if (factory_ != DialogFactory::Open && !local_save)
                    return services_.show_native ? services_.show_native(native_.Get(), owner) : native_->Show(owner);
                DWORD owner_pid = 0;
                const DWORD owner_thread = owner ? GetWindowThreadProcessId(owner, &owner_pid) : 0;
                char diagnostic[224]{};
                std::snprintf(diagnostic, sizeof(diagnostic), "inprocess-owner provided=%u valid=%u samepid=%u samethread=%u enabled=%u",
                    owner ? 1u : 0u, owner && IsWindow(owner) ? 1u : 0u,
                    owner && owner_pid == GetCurrentProcessId() ? 1u : 0u,
                    owner && owner_thread == GetCurrentThreadId() ? 1u : 0u, owner && IsWindowEnabled(owner) ? 1u : 0u);
                ObserveNativeReason(diagnostic);
                auto session = std::make_unique<FileDialogInprocessSession>(services_.inprocess_provider,
                    [this](const char* reason) { ObserveNativeReason(reason); });
                ObserveNativeReason("inprocess-prepare-enter");
                HRESULT prepared = E_FAIL;
                try {
                    std::vector<FileDialogPropertyView> property_views;
                    if (properties_owned_) {
                        for (const auto& field : properties_.Fields()) {
                            FileDialogPropertyView view;
                            view.index = static_cast<uint32_t>(property_views.size());
                            view.kind = field.type == VT_LPWSTR ? PULSE_PICKER_UI_V2_PROPERTY_SCALAR : PULSE_PICKER_UI_V2_PROPERTY_LIST;
                            view.label = field.label; view.values = field.values;
                            property_views.push_back(std::move(view));
                        }
                    }
                    prepared = session->Prepare(decision, owner, custom_controls_, places_, property_views);
                }
                catch (...) { ObserveNativeReason("inprocess-prepare-exception"); throw; }
                std::snprintf(diagnostic, sizeof(diagnostic), "inprocess-prepare-return hr=0x%08lX", static_cast<unsigned long>(prepared));
                ObserveNativeReason(diagnostic);
                if (prepared != S_OK) {
                    session.reset();
                    return services_.show_native ? services_.show_native(native_.Get(), owner) : native_->Show(owner);
                }
                result_native_options_ = decision.effective_options.value_or(0);
                inprocess_ = std::move(session); inprocess_used_ = true; bridge_result_ = true;
                allow_missing_result_ = decision.allow_missing_open || decision.no_test_file_create;
                PulsePickerUiV2Callbacks callbacks{}; callbacks.size = sizeof(callbacks);
                callbacks.context = this; callbacks.callback = InprocessEvent;
                if (local_save) callbacks.overwrite = InprocessOverwrite;
                if (result_native_options_ & FOS_SHAREAWARE) callbacks.share_violation = InprocessShareViolation;
                PulsePickerUiV2Result result{}; std::wstring path;
                inprocess_callback_seen_ = false;
                const HRESULT shown = inprocess_->Show(callbacks, result, path);
                if (inprocess_->Closed()) return inprocess_->CloseResult();
                // Show clears output buffer pointers only after validating the provider's
                // terminal result. Any callback, even malformed, excludes startup retry.
                if (FAILED(shown) && shown != HRESULT_FROM_WIN32(ERROR_CANCELLED) &&
                    result.status == PULSE_PICKER_UI_V2_FAILED && result.terminal_hr == shown &&
                    !result.show_id && !result.flags && !inprocess_callback_seen_ &&
                    !result.path.data && !result.paths.data && !result.path.units && !result.paths.units && !result.path_count) {
                    inprocess_.reset(); inprocess_used_ = false; bridge_result_ = false;
                    ObserveNativeReason("inprocess-startup-native-fallback");
                    return services_.show_native ? services_.show_native(native_.Get(), owner) : native_->Show(owner);
                }
                if (FAILED(shown) || result.status != PULSE_PICKER_UI_V2_ACCEPTED) return shown;
                const HRESULT materialized = CreatePickerShellItemArray(ResultOperation(), inprocess_->AcceptedPaths(), &items_, allow_missing_result_, result_native_options_);
                if (FAILED(materialized)) return materialized;
                const HRESULT first = items_->GetItemAt(0, &item_);
                if (FAILED(first)) { items_.Reset(); return first; }
                readonly_properties_result_ = decision.require_unencrypted_save;
                properties_accepted_ = properties_owned_;
                result_count_ = inprocess_->AcceptedPaths().size(); result_index_ = result.filter_index + 1;
                const auto slash = path.find_last_of(L"\\/");
                if (result_count_ == 1) returned_name_ = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
                if (decision.effective_options && !(*decision.effective_options & FOS_DONTADDTORECENT)) {
                    for (DWORD i = 0; i < result_count_; ++i) { ComPtr<IShellItem> recent; if (SUCCEEDED(items_->GetItemAt(i, &recent))) SHAddToRecentDocs(SHARD_SHELLITEM, recent.Get()); }
                }
                if (client_state_ && client_guid_) {
                    ComPtr<IShellItem> parent;
                    PWSTR folder = nullptr;
                    if (item_->GetParent(&parent) == S_OK && parent->GetDisplayName(SIGDN_FILESYSPATH, &folder) == S_OK) {
                        const bool saved = client_state_->Write(*client_guid_, folder);
                        if (!saved) ObserveNativeReason("client-state-write-failed");
                    }
                    CoTaskMemFree(folder);
                }
                return S_OK;
            }
            // Once this boundary is crossed, every failure is terminal. Never
            // reopen a native dialog after acceptance, cancellation or IPC error.
            bridge_result_ = true;
            cancel_.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!cancel_) return HRESULT_FROM_WIN32(GetLastError());
            const auto& request = bridge_request ? *bridge_request : *decision.request;
            const auto response = services_.run_bridge ? services_.run_bridge(request, owner, cancel_.Get()) :
                wire::RunPickerClient(services_.executable, request, owner, cancel_.Get());
            cancel_.Reset();
            if (close_requested_) return close_result_;
            if (!wire::ValidateResponse(request, response)) return E_UNEXPECTED;
            const auto& result = std::get<wire::Response>(response.body);
            if (result.status == wire::Status::Cancelled) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            if (result.status == wire::Status::Unsupported) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            if (result.status == wire::Status::Failed) {
                if (result.error_source == wire::ErrorSource::HResult)
                    return FAILED(static_cast<HRESULT>(result.error_code)) ? static_cast<HRESULT>(result.error_code) : E_FAIL;
                return HRESULT_FROM_WIN32(result.error_source == wire::ErrorSource::Protocol ? ERROR_INVALID_DATA : result.error_code);
            }
            std::vector<std::wstring> paths;
            for (const auto& path : result.paths) paths.emplace_back(path.begin(), path.end());
            ComPtr<IShellItemArray> items;
            ComPtr<IShellItem> item;
            hr = CreatePickerShellItemArray(result.operation, paths, &items);
            if (FAILED(hr)) return hr;
            hr = items->GetItemAt(0, &item);
            if (FAILED(hr)) return hr;
            if (paths.size() == 1) {
                const auto slash = paths.front().find_last_of(L"\\/");
                returned_name_ = paths.front().substr(slash == std::wstring::npos ? 0 : slash + 1);
            }
            result_index_ = result.filter_index + 1;
            result_count_ = paths.size();
            items_ = std::move(items); item_ = std::move(item);
            if (bridge_request) {
                // As the in-process route after acceptance: every result goes
                // to Recent unless excluded, and the client folder is remembered.
                result_native_options_ = decision.effective_options.value_or(0);
                readonly_properties_result_ = decision.require_unencrypted_save;
                if (decision.effective_options && !(*decision.effective_options & FOS_DONTADDTORECENT)) {
                    for (DWORD i = 0; i < result_count_; ++i) { ComPtr<IShellItem> recent; if (SUCCEEDED(items_->GetItemAt(i, &recent))) SHAddToRecentDocs(SHARD_SHELLITEM, recent.Get()); }
                }
                if (client_state_ && client_guid_) {
                    ComPtr<IShellItem> parent;
                    PWSTR folder = nullptr;
                    if (item_->GetParent(&parent) == S_OK && parent->GetDisplayName(SIGDN_FILESYSPATH, &folder) == S_OK &&
                        !client_state_->Write(*client_guid_, folder)) ObserveNativeReason("client-state-write-failed");
                    CoTaskMemFree(folder);
                }
                return S_OK;
            }
            if (decision.supported_access_policy == AccessPolicy::LocalOpenV1 && decision.effective_options &&
                !(*decision.effective_options & FOS_DONTADDTORECENT)) {
                // Caller-side attribution. This void public API has no HRESULT;
                // do not claim to reproduce private dialog folder persistence.
                SHAddToRecentDocs(SHARD_SHELLITEM, item_.Get());
            }
            return S_OK;
        } catch (const std::bad_alloc&) { cancel_.Reset(); return E_OUTOFMEMORY; }
        catch (...) { cancel_.Reset(); return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE SetFileTypes(UINT count, const COMDLG_FILTERSPEC* filters) override {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        const HRESULT hr = native_->SetFileTypes(count, filters);
        SetterCall call; call.setting = Setting::FileTypes; call.native_result = hr;
        try {
            if (SUCCEEDED(hr) && filters && count <= wire::kMaxFilters) {
                for (UINT i = 0; i < count; ++i) {
                    wire::Filter filter;
                    if (!CopyText(filters[i].pszName, filter.label) || !CopyText(filters[i].pszSpec, filter.pattern)) { MarkNativeOnly("filter-snapshot-invalid"); break; }
                    call.filters.push_back(std::move(filter));
                }
            } else if (SUCCEEDED(hr)) MarkNativeOnly("filter-snapshot-unavailable");
            return Track(std::move(call));
        } catch (...) { MarkNativeOnly("filter-snapshot-allocation"); return E_OUTOFMEMORY; }
    }
    HRESULT STDMETHODCALLTYPE SetFileTypeIndex(UINT index) override {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        SetterCall call; call.setting = Setting::FileTypeIndex; call.value = index; call.native_result = native_->SetFileTypeIndex(index);
        const HRESULT setter_result = call.native_result;
        if (setter_result == S_OK) {
            UINT observed = 0;
            if (native_->GetFileTypeIndex(&observed) == S_OK) call.observed_filter_index = observed;
        }
        // Getter/trace failure cannot change the caller's native setter result.
        // Track failure already marks this wrapper native-only.
        Track(std::move(call));
        return setter_result;
    }
    HRESULT STDMETHODCALLTYPE GetFileTypeIndex(UINT* index) override {
        if (!index) return E_POINTER;
        if (FAILED(Thread())) return RPC_E_WRONG_THREAD;
        if (inprocess_) {
            FileDialogInprocessSnapshot snapshot; const HRESULT hr = inprocess_->Read(snapshot);
            if (FAILED(hr)) { *index = 0; return DiagnosticResult("getter-GetFileTypeIndex-snapshot", hr); }
            *index = snapshot.filter_index + 1; return DiagnosticResult("getter-GetFileTypeIndex", S_OK);
        }
        if (!bridge_result_) return native_->GetFileTypeIndex(index);
        if (!item_) { *index = 0; return E_UNEXPECTED; }
        *index = result_index_; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Advise(IFileDialogEvents* sink, DWORD* cookie) override {
        if (const auto hr = inprocess_ ? Thread() : Mutable(); FAILED(hr)) return hr;
        if (!CanCaptureInprocess()) MarkNativeOnly("advise-without-inprocess");
        AddRef();
        struct Hold { FileDialog* self; ~Hold() { self->Release(); } } hold{this};
        if (!sink) return native_->Advise(nullptr, cookie);
        ComPtr<IFileDialogEvents> proxy;
        HRESULT hr = detail::CreateFileDialogEventProxy(event_owner_, sink, &proxy);
        if (FAILED(hr)) return hr;
        hr = native_->Advise(proxy.Get(), cookie);
        if (FAILED(hr)) return hr;
        if (!cookie) return E_UNEXPECTED;
        try {
            if (subscriptions_.emplace(*cookie, Subscription{proxy, ++subscription_order_}).second) return hr;
        } catch (...) {
            native_->Unadvise(*cookie); *cookie = 0; return E_OUTOFMEMORY;
        }
        native_->Unadvise(*cookie); *cookie = 0; return E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE Unadvise(DWORD cookie) override {
        if (const auto hr = inprocess_ ? Thread() : Mutable(); FAILED(hr)) return hr;
        // The documented input is a cookie returned by Advise. Do not pass
        // arbitrary/stale values to native code: this wrapper defines those as
        // E_INVALIDARG, rather than claiming an undocumented native HRESULT.
        if (!subscriptions_.contains(cookie)) return E_INVALIDARG;
        if (!CanCaptureInprocess()) MarkNativeOnly("unadvise-without-inprocess");
        AddRef();
        struct Hold { FileDialog* self; ~Hold() { self->Release(); } } hold{this};
        // Remove before entering native code, so a callback cannot recursively
        // unregister the same cookie. The node retains the proxy/sink throughout.
        auto subscription = subscriptions_.extract(cookie);
        const HRESULT hr = native_->Unadvise(cookie);
        if (FAILED(hr)) subscriptions_.insert(std::move(subscription));
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetOptions(FILEOPENDIALOGOPTIONS options) override {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        SetterCall call; call.setting = Setting::Options; call.value = options; call.native_result = native_->SetOptions(options);
        return Track(std::move(call));
    }
    HRESULT STDMETHODCALLTYPE GetOptions(FILEOPENDIALOGOPTIONS* options) override {
        if (FAILED(Thread())) return RPC_E_WRONG_THREAD;
        return native_->GetOptions(options);
    }
    HRESULT STDMETHODCALLTYPE SetDefaultFolder(IShellItem* folder) override {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        SetterCall call; call.setting = Setting::DefaultFolder; call.native_result = native_->SetDefaultFolder(folder);
        // Do not turn a default into an explicit folder: native GetFolder and
        // the owned client history resolve precedence when Show prepares.
        return Track(std::move(call));
    }
    HRESULT STDMETHODCALLTYPE SetFolder(IShellItem* folder) override {
        if (const auto hr = Thread(); FAILED(hr)) return hr;
        if (inprocess_) {
            if (!folder) return E_INVALIDARG;
            PWSTR text = nullptr;
            const HRESULT extracted = folder->GetDisplayName(picker_shell::Enabled(result_native_options_) ? SIGDN_DESKTOPABSOLUTEPARSING : SIGDN_FILESYSPATH, &text);
            if (FAILED(extracted)) return extracted;
            std::wstring path;
            try { path = text ? text : L""; } catch (...) { CoTaskMemFree(text); return E_OUTOFMEMORY; }
            CoTaskMemFree(text);
            if (pending_folders_.size() >= 64) return HRESULT_FROM_WIN32(ERROR_BUSY);
            const auto request = ++navigation_request_;
            try { pending_folders_.push_back({request, path, folder}); } catch (...) { return E_OUTOFMEMORY; }
            const HRESULT navigated = inprocess_->Navigate(path);
            if (navigated == E_PENDING) return S_OK;
            std::erase_if(pending_folders_, [request](const auto& value) { return value.id == request; });
            if (navigated != S_OK) return navigated;
        } else if (const auto hr = Mutable(); FAILED(hr)) return hr;
        SetterCall call; call.setting = Setting::Folder; call.native_result = native_->SetFolder(folder);
        if (call.native_result == S_OK) explicit_folder_ = true;
        PWSTR path = nullptr;
        SFGAOF attributes = SFGAO_FILESYSTEM | SFGAO_FOLDER;
        if (SUCCEEDED(call.native_result) && folder && SUCCEEDED(folder->GetAttributes(attributes, &attributes)) &&
            (attributes & SFGAO_FOLDER) &&
            SUCCEEDED(folder->GetDisplayName((attributes & SFGAO_FILESYSTEM) ? SIGDN_FILESYSPATH : SIGDN_DESKTOPABSOLUTEPARSING, &path))) {
            try { const bool copied = CopyText(path, call.text); call.filesystem_folder = copied && (attributes & SFGAO_FILESYSTEM); }
            catch (...) { CoTaskMemFree(path); MarkNativeOnly("folder-snapshot-allocation"); return E_OUTOFMEMORY; }
        }
        CoTaskMemFree(path);
        return Track(std::move(call));
    }
    HRESULT STDMETHODCALLTYPE GetFolder(IShellItem** folder) override {
        if (!folder) return E_POINTER;
        *folder = nullptr;
        if (FAILED(Thread())) return RPC_E_WRONG_THREAD;
        if (inprocess_) return DiagnosticResult("getter-GetFolder", SnapshotItem(SnapshotPart::Folder, folder));
        if (!bridge_result_) return native_->GetFolder(folder);
        return item_ ? item_->GetParent(folder) : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentSelection(IShellItem** item) override {
        if (!item) return E_POINTER;
        *item = nullptr;
        if (FAILED(Thread())) return RPC_E_WRONG_THREAD;
        if (inprocess_) return DiagnosticResult("getter-GetCurrentSelection", SnapshotItem(SnapshotPart::Selection, item));
        if (!bridge_result_) { if (!CanCaptureInprocess()) MarkNativeOnly("current-selection-without-inprocess"); return native_->GetCurrentSelection(item); }
        return item_ ? item_.CopyTo(item) : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE SetFileName(LPCWSTR text) override { return Text(Setting::FileName, text, &IFileDialog::SetFileName); }
    HRESULT STDMETHODCALLTYPE GetFileName(LPWSTR* text) override {
        if (!text) return E_POINTER;
        *text = nullptr;
        if (FAILED(Thread())) return RPC_E_WRONG_THREAD;
        if (inprocess_) {
            FileDialogInprocessSnapshot snapshot; const HRESULT hr = inprocess_->Read(snapshot);
            return DiagnosticResult("getter-GetFileName", FAILED(hr) ? hr : CopyResultText(snapshot.filename, text));
        }
        if (!bridge_result_) return native_->GetFileName(text);
        if (!item_ || returned_name_.empty()) return E_UNEXPECTED;
        const auto bytes = (returned_name_.size() + 1) * sizeof(wchar_t);
        auto copy = static_cast<wchar_t*>(CoTaskMemAlloc(bytes));
        if (!copy) return E_OUTOFMEMORY;
        std::copy(returned_name_.begin(), returned_name_.end(), copy); copy[returned_name_.size()] = 0;
        *text = copy; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTitle(LPCWSTR text) override { return Text(Setting::Title, text, &IFileDialog::SetTitle); }
    HRESULT STDMETHODCALLTYPE SetOkButtonLabel(LPCWSTR text) override { return Text(Setting::OkButtonLabel, text, &IFileDialog::SetOkButtonLabel); }
    HRESULT STDMETHODCALLTYPE SetFileNameLabel(LPCWSTR text) override { return Text(Setting::FileNameLabel, text, &IFileDialog::SetFileNameLabel); }
    HRESULT STDMETHODCALLTYPE GetResult(IShellItem** item) override {
        if (!item) return E_POINTER;
        *item = nullptr;
        if (FAILED(Thread())) return RPC_E_WRONG_THREAD;
        if (inprocess_) return DiagnosticResult("getter-GetResult", inprocess_->InFileOk() && !inprocess_->Closed() ? SnapshotItem(SnapshotPart::Candidate, item) : E_UNEXPECTED);
        return bridge_result_ ? (item_ && result_count_ == 1 ? item_.CopyTo(item) : E_UNEXPECTED) : native_->GetResult(item);
    }
    HRESULT STDMETHODCALLTYPE AddPlace(IShellItem* item, FDAP location) override {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        const HRESULT hr = native_->AddPlace(item, location);
        if (!CanCaptureInprocess() || hr != S_OK) return Unsupported(Setting::Place, hr);
        try {
            SFGAOF attributes = SFGAO_FILESYSTEM | SFGAO_FOLDER;
            struct Text { PWSTR value = nullptr; ~Text() { CoTaskMemFree(value); } } path, label;
            FileDialogPlace place;
            ComPtr<IShellItem2> item2;
            if (item && item->QueryInterface(IID_PPV_ARGS(&item2)) == S_OK)
                item2->GetString(PKEY_ItemNameDisplay, &label.value);
            if ((!label.value || !*label.value) && item) {
                CoTaskMemFree(label.value); label.value = nullptr;
                item->GetDisplayName(SIGDN_NORMALDISPLAY, &label.value);
            }
            const HRESULT attributes_hr = item ? item->GetAttributes(attributes, &attributes) : E_POINTER;
            const HRESULT path_hr = item ? item->GetDisplayName(SIGDN_FILESYSPATH, &path.value) : E_POINTER;
            const bool label_ok = CustomLabel(label.value, place.label);
            char detail[224]{};
            std::snprintf(detail, sizeof(detail), "place seq=%zu attrhr=%08lX attrs=%08lX pathhr=%08lX path=%u label=%u position=%u",
                calls_.size(), static_cast<unsigned long>(attributes_hr), static_cast<unsigned long>(attributes),
                static_cast<unsigned long>(path_hr), path.value ? 1u : 0u, label_ok ? 1u : 0u, static_cast<unsigned>(location));
            ObserveNativeReason(detail);
            bool computer = false;
            if (item && (attributes & SFGAO_FOLDER) && !(attributes & SFGAO_FILESYSTEM)) {
                ComPtr<IShellItem> known; int order = 1;
                computer = SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&known)) == S_OK &&
                    item->Compare(known.Get(), SICHINT_CANONICAL, &order) == S_OK && order == 0;
            }
            if (computer && places_.size() < 32 && (location == FDAP_TOP || location == FDAP_BOTTOM) && label_ok) {
                place.path = picker_navigation::kComputerPath; place.location = static_cast<uint32_t>(location);
                places_.push_back(std::move(place));
                ObserveNativeReason("place-computer-captured");
                SetterCall call; call.setting = Setting::Place; call.native_result = hr;
                return Track(std::move(call));
            }
            if (!item || places_.size() >= 32 || (location != FDAP_TOP && location != FDAP_BOTTOM) ||
                attributes_hr != S_OK ||
                (attributes & (SFGAO_FILESYSTEM | SFGAO_FOLDER)) != (SFGAO_FILESYSTEM | SFGAO_FOLDER) ||
                path_hr != S_OK || !path.value || !label_ok) {
                // Only log a namespace GUID, never a caller path or display name.
                Text parsing;
                if (item && item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &parsing.value) == S_OK && parsing.value &&
                    wcsncmp(parsing.value, L"::{", 3) == 0 && wcslen(parsing.value) >= 40 && parsing.value[39] == L'}') {
                    bool guid_only = true;
                    for (size_t i = 3; i < 39; ++i) { const auto c = parsing.value[i]; if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F') || c == L'-')) guid_only = false; }
                    if (guid_only) { char namespace_id[39]{}; for (size_t i = 0; i < 38; ++i) namespace_id[i] = static_cast<char>(parsing.value[i + 2]);
                        std::snprintf(detail, sizeof(detail), "place-namespace=%s", namespace_id); ObserveNativeReason(detail); }
                }
                return Unsupported(Setting::Place, hr);
            }
            std::u16string copied;
            if (!CopyText(path.value, copied)) return Unsupported(Setting::Place, hr);
            place.path.assign(copied.begin(), copied.end()); place.location = static_cast<uint32_t>(location);
            places_.push_back(std::move(place));
            SetterCall call; call.setting = Setting::Place; call.native_result = hr;
            return Track(std::move(call));
        } catch (...) { MarkNativeOnly("place-snapshot-failed"); return hr; }
    }
    HRESULT STDMETHODCALLTYPE SetDefaultExtension(LPCWSTR text) override { return Text(Setting::DefaultExtension, text, &IFileDialog::SetDefaultExtension); }
    HRESULT STDMETHODCALLTYPE Close(HRESULT result) override {
        if (FAILED(Thread())) return RPC_E_WRONG_THREAD;
        if (inprocess_) return inprocess_->Close(result);
        if (showing_ && bridge_result_ && cancel_) {
            close_requested_ = true; close_result_ = result;
            return SetEvent(cancel_.Get()) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
        }
        return native_->Close(result);
    }
    HRESULT STDMETHODCALLTYPE SetClientGuid(REFGUID guid) override {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        const HRESULT hr = native_->SetClientGuid(guid);
        if (!CanCaptureInprocess() || hr != S_OK) return Unsupported(Setting::ClientGuid, hr);
        try {
            if (!client_state_) client_state_ = services_.client_state ? services_.client_state : std::make_shared<FileDialogClientState>();
            client_guid_ = guid;
            SetterCall call; call.setting = Setting::ClientGuid; call.native_result = hr;
            return Track(std::move(call));
        } catch (...) { MarkNativeOnly("client-state-initialize-failed"); return hr; }
    }
    HRESULT STDMETHODCALLTYPE ClearClientData() override {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        const HRESULT hr = native_->ClearClientData();
        if (!CanCaptureInprocess() || hr != S_OK || !client_state_ || !client_guid_)
            return Unsupported(Setting::ClearClientData, hr);
        if (!client_state_->Clear(*client_guid_)) return Unsupported(Setting::ClearClientData, hr);
        SetterCall call; call.setting = Setting::ClearClientData; call.native_result = hr;
        return Track(std::move(call));
    }
    HRESULT STDMETHODCALLTYPE SetFilter(IShellItemFilter* filter) override {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        return Unsupported(Setting::ShellFilter, native_->SetFilter(filter));
    }
    HRESULT STDMETHODCALLTYPE GetResults(IShellItemArray** items) override {
        if (!items) return E_POINTER;
        *items = nullptr;
        if (FAILED(Thread())) return RPC_E_WRONG_THREAD;
        if (factory_ != DialogFactory::Open) return E_NOINTERFACE;
        if (inprocess_) return SnapshotArray(true, items);
        if (bridge_result_) return items_ ? items_.CopyTo(items) : E_UNEXPECTED;
        ComPtr<IFileOpenDialog> open; const auto hr = native_.As(&open);
        return FAILED(hr) ? hr : open->GetResults(items);
    }
    HRESULT STDMETHODCALLTYPE GetSelectedItems(IShellItemArray** items) override {
        if (!items) return E_POINTER;
        *items = nullptr;
        if (FAILED(Thread())) return RPC_E_WRONG_THREAD;
        if (factory_ != DialogFactory::Open) return E_NOINTERFACE;
        if (inprocess_) return SnapshotArray(false, items);
        if (bridge_result_) return items_ ? items_.CopyTo(items) : E_UNEXPECTED;
        if (!CanCaptureInprocess()) MarkNativeOnly("selected-items-without-inprocess");
        ComPtr<IFileOpenDialog> open; const auto hr = native_.As(&open);
        return FAILED(hr) ? hr : open->GetSelectedItems(items);
    }
    HRESULT STDMETHODCALLTYPE SetSaveAsItem(IShellItem* item) override {
        return SaveCall("SetSaveAsItem", Setting::SaveAsItem, item != nullptr, 0, [&](IFileSaveDialog* save) { return save->SetSaveAsItem(item); });
    }
    HRESULT STDMETHODCALLTYPE SetProperties(IPropertyStore* properties) override {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        if (factory_ != DialogFactory::Save) return E_NOINTERFACE;
        if (inspecting_metadata_) return HRESULT_FROM_WIN32(ERROR_BUSY);
        AddRef();
        struct Hold { FileDialog* self; ~Hold() { self->inspecting_metadata_ = false; self->Release(); } } hold{this};
        inspecting_metadata_ = true;
        ComPtr<IFileSaveDialog> save;
        const HRESULT queried = native_.As(&save); if (FAILED(queried)) return queried;
        const HRESULT result = save->SetProperties(properties);
        if (result != S_OK) return Unsupported(Setting::SaveProperties, result);
        metadata_seen_ = true; readonly_collection_ = false;
        const HRESULT captured = properties_.SetStore(properties);
        DiagnosticResult("properties-capture", captured);
        if (FAILED(captured) && properties) {
            DWORD count = 0;
            if (properties->GetCount(&count) == S_OK) {
                for (DWORD i = 0; i < std::min<DWORD>(count, 16); ++i) {
                    PROPERTYKEY key{}; PROPVARIANT value{};
                    const HRESULT key_hr = properties->GetAt(i, &key);
                    const HRESULT value_hr = key_hr == S_OK ? properties->GetValue(key, &value) : key_hr;
                    char diagnostic[128]{};
                    std::snprintf(diagnostic, sizeof(diagnostic), "property-store-schema index=%lu type=%u hr=0x%08lX",
                        static_cast<unsigned long>(i), static_cast<unsigned>(value.vt), static_cast<unsigned long>(value_hr));
                    ObserveNativeReason(diagnostic);
                    PropVariantClear(&value);
                }
            }
        }
        if (captured != S_OK) { properties_owned_ = false; return Unsupported(Setting::SaveProperties, result); }
        properties_owned_ = true;
        SetterCall call; call.setting = Setting::SaveProperties; call.native_result = result;
        return Track(std::move(call));
    }
    HRESULT STDMETHODCALLTYPE SetCollectedProperties(IPropertyDescriptionList* properties, BOOL append) override {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        if (factory_ != DialogFactory::Save) return E_NOINTERFACE;
        if (inspecting_metadata_) return HRESULT_FROM_WIN32(ERROR_BUSY);
        AddRef();
        struct Hold { FileDialog* self; ~Hold() { self->inspecting_metadata_ = false; self->Release(); } } hold{this};
        inspecting_metadata_ = true;
        const uint64_t epoch = ++metadata_epoch_;
        readonly_collection_ = false;
        HRESULT result = E_FAIL; bool invoked = false;
        try {
            ComPtr<IFileSaveDialog> save; const HRESULT queried = native_.As(&save); if (FAILED(queried)) return queried;
            ComPtr<IPropertyDescriptionList> list = properties;
            result = save->SetCollectedProperties(properties, append); invoked = true;
            UINT count = 0; const HRESULT counted = result == S_OK ? (list ? list->GetCount(&count) : S_OK) : E_PENDING;
            char detail[128]{};
            std::snprintf(detail, sizeof(detail), "collected hr=0x%08lX append=%u list=%u count_hr=0x%08lX count=%u",
                static_cast<unsigned long>(result), append ? 1u : 0u, properties ? 1u : 0u, static_cast<unsigned long>(counted), count);
            ObserveNativeReason(detail);
            if (result == S_OK && counted == S_OK && list) ReportCollectedSchema(list.Get(), count);
            bool supported = result == S_OK && counted == S_OK && count == 1 && append == FALSE && !metadata_seen_;
            ComPtr<IPropertyDescription> description;
            if (supported) supported = list && list->GetAt(0, IID_PPV_ARGS(&description)) == S_OK && description;
            if (supported) {
                constexpr GUID ownership = {0x5F5AFF6A, 0x37E5, 0x4780, {0x97, 0xEA, 0x80, 0xC7, 0x56, 0x5C, 0xF5, 0x35}};
                PROPERTYKEY key{}; VARTYPE type{}; PROPDESC_TYPE_FLAGS flags{};
                supported = description->GetPropertyKey(&key) == S_OK && key.fmtid == ownership && key.pid == 34 &&
                    description->GetPropertyType(&type) == S_OK && type == (VT_VECTOR | VT_LPWSTR) &&
                    description->GetTypeFlags(PDTF_MASK_ALL, &flags) == S_OK && static_cast<uint32_t>(flags) == 0x8000001Bu;
                struct Name { PWSTR value = nullptr; ~Name() { CoTaskMemFree(value); } } name;
                supported = supported && description->GetCanonicalName(&name.value) == S_OK && name.value &&
                    wcscmp(name.value, L"System.Security.EncryptionOwners") == 0;
            }
            supported = supported && epoch == metadata_epoch_ && !metadata_seen_;
            if (supported) {
                readonly_collection_ = true;
                SetterCall call; call.setting = Setting::ReadonlyOwnershipCollection; call.native_result = result;
                return Track(std::move(call));
            }
            metadata_seen_ = true;
            const HRESULT captured = result == S_OK && append == FALSE && epoch == metadata_epoch_ ? properties_.SetCollected(list.Get()) : E_NOTIMPL;
            DiagnosticResult("properties-collection-capture", captured);
            if (FAILED(captured) && list) {
                ComPtr<IPropertyStore> snapshot;
                if (properties_.Snapshot(&snapshot) == S_OK) {
                    for (UINT i = 0; i < std::min<UINT>(count, 16); ++i) {
                        ComPtr<IPropertyDescription> field; PROPERTYKEY key{}; VARTYPE expected = VT_EMPTY; PROPVARIANT value{};
                        HRESULT value_hr = list->GetAt(i, IID_PPV_ARGS(&field));
                        if (value_hr == S_OK && field) value_hr = field->GetPropertyKey(&key);
                        if (value_hr == S_OK) value_hr = field->GetPropertyType(&expected);
                        if (value_hr == S_OK) value_hr = snapshot->GetValue(key, &value);
                        char schema[144]{};
                        std::snprintf(schema, sizeof(schema), "property-value-schema index=%u expected=%u actual=%u hr=0x%08lX",
                            i, static_cast<unsigned>(expected), static_cast<unsigned>(value.vt), static_cast<unsigned long>(value_hr));
                        ObserveNativeReason(schema); PropVariantClear(&value);
                    }
                }
            }
            if (captured == S_OK) {
                properties_owned_ = true; properties_collected_ = true;
                SetterCall call; call.setting = Setting::SaveProperties; call.native_result = result;
                return Track(std::move(call));
            }
            properties_owned_ = false;
            return Unsupported(Setting::SaveProperties, result);
        } catch (...) {
            metadata_seen_ = true; MarkNativeOnly("collected-inspection-exception");
            return invoked ? Unsupported(Setting::SaveProperties, result) : E_FAIL;
        }
    }
    HRESULT STDMETHODCALLTYPE GetProperties(IPropertyStore** properties) override {
        if (!properties) return E_POINTER;
        *properties = nullptr;
        if (const auto hr = Thread(); FAILED(hr)) return hr;
        if (properties_owned_ && bridge_result_) {
            if (properties_collected_ && ((inprocess_ && !inprocess_->Closed()) || properties_accepted_))
                return properties_.Snapshot(properties);
            return E_UNEXPECTED;
        }
        if (properties_owned_) {
            ComPtr<IFileSaveDialog> save; const HRESULT queried = native_.As(&save);
            return FAILED(queried) ? queried : save->GetProperties(properties);
        }
        if (readonly_collection_ && !metadata_seen_) {
            if (inprocess_) {
                if (!inprocess_->InFileOk() || inprocess_->Closed()) return E_UNEXPECTED;
                return PSCreateMemoryPropertyStore(IID_PPV_ARGS(properties));
            }
            if (readonly_properties_result_) return PSCreateMemoryPropertyStore(IID_PPV_ARGS(properties));
            if (bridge_result_) return E_UNEXPECTED; // Cancel/Close/failure is not a saved result.
            ComPtr<IFileSaveDialog> save; const HRESULT queried = native_.As(&save);
            return FAILED(queried) ? queried : save->GetProperties(properties);
        }
        if (bridge_result_) return E_NOTIMPL;
        return SaveCall("GetProperties", Setting::SaveProperties, properties != nullptr, 0, [&](IFileSaveDialog* save) { return save->GetProperties(properties); });
    }
    HRESULT STDMETHODCALLTYPE ApplyProperties(IShellItem* item, IPropertyStore* properties, HWND owner, IFileOperationProgressSink* sink) override {
        if (properties_owned_ && bridge_result_) {
            if (const auto hr = Thread(); FAILED(hr)) return hr;
            if (!properties_accepted_ && !(inprocess_ && inprocess_->InFileOk() && !inprocess_->Closed())) return E_UNEXPECTED;
            ComPtr<IFileSaveDialog> save; const HRESULT queried = native_.As(&save);
            return FAILED(queried) ? queried : save->ApplyProperties(item, properties, owner, sink);
        }
        if (bridge_result_) return E_NOTIMPL;
        return SaveCall("ApplyProperties", Setting::SaveProperties, properties != nullptr, 0, [&](IFileSaveDialog* save) { return save->ApplyProperties(item, properties, owner, sink); });
    }
    HRESULT STDMETHODCALLTYPE SetCancelButtonLabel(LPCWSTR text) override {
        return Dialog2Call([&](IFileDialog2* dialog) { return dialog->SetCancelButtonLabel(text); });
    }
    HRESULT STDMETHODCALLTYPE SetNavigationRoot(IShellItem* root) override {
        return Dialog2Call([&](IFileDialog2* dialog) { return dialog->SetNavigationRoot(root); });
    }
    HRESULT STDMETHODCALLTYPE EnableOpenDropDown(DWORD id) override {
        return AddContainer(PULSE_PICKER_UI_V2_CONTROL_OPEN_DROPDOWN, id, nullptr);
    }
    HRESULT STDMETHODCALLTYPE AddMenu(DWORD id, LPCWSTR text) override {
        return AddContainer(PULSE_PICKER_UI_V2_CONTROL_MENU, id, text);
    }
    HRESULT STDMETHODCALLTYPE AddPushButton(DWORD id, LPCWSTR text) override {
        return AddCustom(false, id, text);
    }
    HRESULT STDMETHODCALLTYPE AddComboBox(DWORD id) override {
        return AddCombo(id);
    }
    HRESULT STDMETHODCALLTYPE AddRadioButtonList(DWORD id) override {
        return AddValueControl(6,id,nullptr,FALSE);
    }
    HRESULT STDMETHODCALLTYPE AddCheckButton(DWORD id, LPCWSTR text, BOOL checked) override {
        return AddValueControl(7,id,text,checked);
    }
    HRESULT STDMETHODCALLTYPE AddEditBox(DWORD id, LPCWSTR text) override {
        return AddValueControl(8,id,text,FALSE);
    }
    HRESULT STDMETHODCALLTYPE AddSeparator(DWORD id) override {
        return CustomizeCall("AddSeparator", false, id, 0, 0, [&](IFileDialogCustomize* customize) { return customize->AddSeparator(id); });
    }
    HRESULT STDMETHODCALLTYPE AddText(DWORD id, LPCWSTR text) override {
        return CustomizeCall("AddText", false, id, 0, 0, [&](IFileDialogCustomize* customize) { return customize->AddText(id, text); });
    }
    HRESULT STDMETHODCALLTYPE SetControlLabel(DWORD id, LPCWSTR text) override {
        return ChangeCustom(id, text, 0, true);
    }
    HRESULT STDMETHODCALLTYPE GetControlState(DWORD id, CDCONTROLSTATEF* state) override {
        if (FAILED(Thread())) return RPC_E_WRONG_THREAD;
        if (inprocess_) { if (!state) return E_POINTER; const auto* control = FindCustom(id); if (!control) return E_INVALIDARG; *state = static_cast<CDCONTROLSTATEF>(control->state & 3u); return S_OK; }
        if (FindCustom(id) && CanCaptureInprocess()) { ComPtr<IFileDialogCustomize> native; const auto hr = native_.As(&native); return FAILED(hr) ? hr : native->GetControlState(id, state); }
        return CustomizeCall("GetControlState", true, id, 0, 0, [&](IFileDialogCustomize* customize) { return customize->GetControlState(id, state); });
    }
    HRESULT STDMETHODCALLTYPE SetControlState(DWORD id, CDCONTROLSTATEF state) override {
        return ChangeCustom(id, nullptr, static_cast<DWORD>(state), false);
    }
    HRESULT STDMETHODCALLTYPE GetEditBoxText(DWORD id, WCHAR** text) override {
        if(FAILED(Thread()))return RPC_E_WRONG_THREAD;
        if(CanCaptureInprocess() && FindCustom(id) && FindCustom(id)->kind==8) { ComPtr<IFileDialogCustomize> native;const auto hr=native_.As(&native);return FAILED(hr)?hr:native->GetEditBoxText(id,text); }
        return CustomizeCall("GetEditBoxText", true, id, 0, 0, [&](IFileDialogCustomize* customize) { return customize->GetEditBoxText(id, text); });
    }
    HRESULT STDMETHODCALLTYPE SetEditBoxText(DWORD id, LPCWSTR text) override {
        return ChangeValueControl(id,text,FALSE,true);
    }
    HRESULT STDMETHODCALLTYPE GetCheckButtonState(DWORD id, BOOL* checked) override {
        if(FAILED(Thread()))return RPC_E_WRONG_THREAD;
        if(CanCaptureInprocess() && FindCustom(id) && FindCustom(id)->kind==7) { ComPtr<IFileDialogCustomize> native;const auto hr=native_.As(&native);return FAILED(hr)?hr:native->GetCheckButtonState(id,checked); }
        return CustomizeCall("GetCheckButtonState", true, id, 0, 0, [&](IFileDialogCustomize* customize) { return customize->GetCheckButtonState(id, checked); });
    }
    HRESULT STDMETHODCALLTYPE SetCheckButtonState(DWORD id, BOOL checked) override {
        return ChangeValueControl(id,nullptr,checked,false);
    }
    HRESULT STDMETHODCALLTYPE AddControlItem(DWORD id, DWORD item, LPCWSTR text) override {
        return ChangeItem(ItemMutation::Add, id, item, text, 0);
    }
    HRESULT STDMETHODCALLTYPE RemoveControlItem(DWORD id, DWORD item) override {
        return ChangeItem(ItemMutation::Remove, id, item, nullptr, 0);
    }
    HRESULT STDMETHODCALLTYPE RemoveAllControlItems(DWORD id) override {
        return ChangeItem(ItemMutation::Clear, id, 0, nullptr, 0);
    }
    HRESULT STDMETHODCALLTYPE GetControlItemState(DWORD id, DWORD item, CDCONTROLSTATEF* state) override {
        if (FAILED(Thread())) return RPC_E_WRONG_THREAD;
        if (auto* control = FindCustom(id); CanCaptureInprocess() && control && control->kind >= PULSE_PICKER_UI_V2_CONTROL_COMBO && control->kind<=6) {
            ComPtr<IFileDialogCustomize> native; const HRESULT hr = native_.As(&native);
            return FAILED(hr) ? hr : native->GetControlItemState(id, item, state);
        }
        return CustomizeCall("GetControlItemState", true, id, item, 0, [&](IFileDialogCustomize* customize) { return customize->GetControlItemState(id, item, state); });
    }
    HRESULT STDMETHODCALLTYPE SetControlItemState(DWORD id, DWORD item, CDCONTROLSTATEF state) override {
        return ChangeItem(ItemMutation::State, id, item, nullptr, static_cast<DWORD>(state));
    }
    HRESULT STDMETHODCALLTYPE GetSelectedControlItem(DWORD id, DWORD* item) override {
        if (FAILED(Thread())) return RPC_E_WRONG_THREAD;
        if (inprocess_ || inprocess_used_) { if(!item) return E_POINTER; const auto* control=FindCustom(id); if(!control || !control->has_selected || control->kind==4) return E_INVALIDARG; *item=control->selected_item_id; return S_OK; }
        if (auto* control = FindCustom(id); CanCaptureInprocess() && control && control->kind >= PULSE_PICKER_UI_V2_CONTROL_COMBO && control->kind<=6) {
            ComPtr<IFileDialogCustomize> native; const HRESULT hr = native_.As(&native);
            return FAILED(hr) ? hr : native->GetSelectedControlItem(id, item);
        }
        return CustomizeCall("GetSelectedControlItem", true, id, 0, 0, [&](IFileDialogCustomize* customize) { return customize->GetSelectedControlItem(id, item); });
    }
    HRESULT STDMETHODCALLTYPE SetSelectedControlItem(DWORD id, DWORD item) override {
        return ChangeItem(ItemMutation::Select, id, item, nullptr, 0);
    }
    HRESULT STDMETHODCALLTYPE StartVisualGroup(DWORD id, LPCWSTR text) override {
        return AddCustom(true, id, text);
    }
    HRESULT STDMETHODCALLTYPE EndVisualGroup() override {
        return EndCustom();
    }
    HRESULT STDMETHODCALLTYPE MakeProminent(DWORD id) override {
        const auto report = [&](HRESULT hr) {
            const auto* control=FindCustom(id);
            const uint32_t kind=control?control->kind:0;
            const auto feature=kind==PULSE_PICKER_UI_V2_CONTROL_GROUP?PULSE_PICKER_UI_V2_FEATURE_CUSTOM_PROMINENT_GROUP:PULSE_PICKER_UI_V2_FEATURE_CUSTOM_PROMINENT_CONTROL;
            char detail[192]{};
            std::snprintf(detail,sizeof(detail),"prominent id=%lu kind=%u parent=%lu nested=%u capture=%u feature=%u hr=%lu",
                id,kind,static_cast<unsigned long>(control?control->parent:0),control&&control->has_parent?1u:0u,CanCaptureInprocess()?1u:0u,
                services_.inprocess_provider&&(services_.inprocess_provider->api.implemented_features&feature)?1u:0u,static_cast<unsigned long>(hr));
            ObserveNativeReason(detail);
            return hr;
        };
        const auto* control=FindCustom(id);
        const bool supported=control && (control->kind==PULSE_PICKER_UI_V2_CONTROL_GROUP || control->kind==PULSE_PICKER_UI_V2_CONTROL_BUTTON ||
            control->kind==PULSE_PICKER_UI_V2_CONTROL_COMBO || control->kind==PULSE_PICKER_UI_V2_CONTROL_MENU || control->kind==PULSE_PICKER_UI_V2_CONTROL_CHECK);
        const bool unchanged=control && (control->kind==PULSE_PICKER_UI_V2_CONTROL_RADIO || control->kind==PULSE_PICKER_UI_V2_CONTROL_EDIT);
        if (CanCaptureInprocess() && (supported || unchanged)) {
            if (const auto hr=Mutable(); FAILED(hr)) return hr;
            ComPtr<IFileDialogCustomize> native; HRESULT hr=native_.As(&native); if(FAILED(hr)) return hr;
            hr=native->MakeProminent(id);
            if(hr==S_OK && supported) FindCustom(id)->state |= PULSE_PICKER_UI_V2_CONTROL_PROMINENT;
            return report(hr);
        }
        return report(CustomizeCall("MakeProminent", false, id, 0, 0, [&](IFileDialogCustomize* customize) { return customize->MakeProminent(id); }));
    }
    HRESULT STDMETHODCALLTYPE SetControlItemText(DWORD id, DWORD item, LPCWSTR text) override {
        return ChangeItem(ItemMutation::Label, id, item, text, 0);
    }
    HRESULT STDMETHODCALLTYPE GetWindow(HWND* window) override {
        if (!window) return E_POINTER; *window = nullptr;
        if (FAILED(Thread())) return RPC_E_WRONG_THREAD;
        if (inprocess_) return DiagnosticResult("getter-GetWindow", inprocess_->GetWindow(window));
        if (inprocess_used_ || bridge_result_) return E_UNEXPECTED;
        if (!CanCaptureInprocess()) MarkNativeOnly("get-window-without-inprocess", &IID_IOleWindow);
        ComPtr<IOleWindow> ole; const HRESULT hr = native_.As(&ole);
        return FAILED(hr) ? hr : ole->GetWindow(window);
    }
    HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL enter) override {
        if (FAILED(Thread())) return RPC_E_WRONG_THREAD;
        if (inprocess_ || inprocess_used_) return E_NOTIMPL;
        MarkNativeOnly("context-sensitive-help", &IID_IOleWindow);
        ComPtr<IOleWindow> ole; const HRESULT hr = native_.As(&ole);
        return FAILED(hr) ? hr : ole->ContextSensitiveHelp(enter);
    }
private:
    void ObserveNativeReason(const char* reason, const IID* iid = nullptr) noexcept {
        if (!services_.on_native_reason || notifying_reason_.test_and_set(std::memory_order_acquire)) return;
        try { services_.on_native_reason(reason, iid); } catch (...) {} // Diagnostics cannot affect COM semantics.
        notifying_reason_.clear(std::memory_order_release);
    }
    HRESULT DiagnosticResult(const char* stage, HRESULT hr) noexcept {
        char detail[160]{};
        std::snprintf(detail, sizeof(detail), "%s hr=0x%08lX", stage, static_cast<unsigned long>(hr));
        ObserveNativeReason(detail); return hr;
    }
    void MarkNativeOnly(const char* reason, const IID* iid = nullptr) noexcept {
        native_only_ = true;
        ObserveNativeReason(reason, iid);
    }
    wire::Operation ResultOperation() const {
        if (factory_ == DialogFactory::Save) return wire::Operation::SaveFile;
        FILEOPENDIALOGOPTIONS options{};
        if (native_->GetOptions(&options) == S_OK && (options & FOS_PICKFOLDERS)) return wire::Operation::PickFolder;
        return wire::Operation::OpenFiles;
    }
    enum class SnapshotPart { Folder, Selection, Candidate };
    static HRESULT CopyResultText(const std::wstring& value, LPWSTR* text) {
        const size_t bytes = (value.size() + 1) * sizeof(wchar_t);
        auto* copy = static_cast<wchar_t*>(CoTaskMemAlloc(bytes));
        if (!copy) return E_OUTOFMEMORY;
        std::copy(value.begin(), value.end(), copy); copy[value.size()] = 0; *text = copy; return S_OK;
    }
    HRESULT SnapshotItem(SnapshotPart part, IShellItem** item) {
        try {
            FileDialogInprocessSnapshot snapshot; const HRESULT hr = inprocess_->Read(snapshot);
            if (FAILED(hr)) return DiagnosticResult("getter-ShellItem-snapshot", hr);
            const std::wstring& path = part == SnapshotPart::Folder ? snapshot.folder :
                (part == SnapshotPart::Candidate ? snapshot.candidate : snapshot.selection);
            if (part == SnapshotPart::Folder && picker_navigation::IsComputerPath(path))
                return SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(item));
            if (path.empty()) return DiagnosticResult("getter-ShellItem-empty", E_FAIL);
            if (part == SnapshotPart::Candidate && (!(snapshot.flags & PULSE_PICKER_UI_V2_STATE_CANDIDATE_READY) || snapshot.candidates.size() != 1)) return E_UNEXPECTED;
            return CreatePickerShellItem(part == SnapshotPart::Folder ? wire::Operation::PickFolder : ResultOperation(), path, item, part != SnapshotPart::Folder && allow_missing_result_, result_native_options_);
        } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; } catch (...) { return E_FAIL; }
    }
    HRESULT SnapshotArray(bool result, IShellItemArray** items) {
        if (result && (!inprocess_->InFileOk() || inprocess_->Closed())) return E_UNEXPECTED;
        try {
            FileDialogInprocessSnapshot snapshot; const HRESULT hr = inprocess_->Read(snapshot);
            if (FAILED(hr)) return hr;
            if (result && !(snapshot.flags & PULSE_PICKER_UI_V2_STATE_CANDIDATE_READY)) return E_UNEXPECTED;
            const auto& paths = result ? snapshot.candidates : snapshot.selections;
            if (paths.empty()) return E_FAIL;
            return CreatePickerShellItemArray(ResultOperation(), paths, items, allow_missing_result_, result_native_options_);
        } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; } catch (...) { return E_FAIL; }
    }
    static HRESULT WINAPI InprocessOverwrite(void* context, const PulsePickerUiV2OverwriteEvent* event, uint32_t* response) noexcept {
        auto* self = static_cast<FileDialog*>(context);
        self->inprocess_callback_seen_ = true;
        if (!response) return E_POINTER;
        *response = PULSE_PICKER_UI_V2_OVERWRITE_DEFAULT;
        try { return event ? self->DispatchOverwrite(*event, response) : E_INVALIDARG; }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; } catch (...) { return E_FAIL; }
    }
    HRESULT DispatchOverwrite(const PulsePickerUiV2OverwriteEvent& event, uint32_t* response) {
        if (FAILED(Thread()) || !inprocess_) return RPC_E_WRONG_THREAD;
        if (factory_ != DialogFactory::Save) return E_NOTIMPL;
        const HRESULT observed = inprocess_->ObserveOverwrite(event);
        DiagnosticResult("inprocess-overwrite-observe", observed);
        if (FAILED(observed)) return observed;
        struct Exit { FileDialogInprocessSession* session; ~Exit() { session->LeaveEvent(); } } exit{inprocess_.get()};
        if (subscriptions_.empty()) return S_OK;
        struct Entry { DWORD cookie; ComPtr<IFileDialogEvents> proxy; uint64_t order; };
        std::vector<Entry> subscriptions;
        for (const auto& [cookie, value] : subscriptions_) subscriptions.push_back({cookie, value.proxy, value.order});
        std::sort(subscriptions.begin(), subscriptions.end(), [](const Entry& a, const Entry& b) { return a.order < b.order; });
        const std::wstring target(event.target.data, event.target.units);
        FileDialogInprocessSnapshot snapshot;
        const HRESULT queried = inprocess_->Read(snapshot);
        if (FAILED(queried)) return queried;
        if (snapshot.candidate != target) return E_UNEXPECTED;
        ComPtr<IShellItem> item;
        const HRESULT materialized = CreatePickerShellItem(wire::Operation::SaveFile, target, &item);
        if (FAILED(materialized)) return materialized;
        for (const auto& subscription : subscriptions) {
            const auto current = subscriptions_.find(subscription.cookie);
            if (current == subscriptions_.end() || current->second.order != subscription.order) continue;
            FDE_OVERWRITE_RESPONSE native_response = FDEOR_DEFAULT;
            const HRESULT hr = subscription.proxy->OnOverwrite(Identity(), item.Get(), &native_response);
            char diagnostic[160]{};
            std::snprintf(diagnostic, sizeof(diagnostic), "inprocess-overwrite-sink order=%llu rawhr=0x%08lX response=%u",
                subscription.order, static_cast<unsigned long>(hr), static_cast<unsigned>(native_response));
            ObserveNativeReason(diagnostic);
            if (inprocess_->Closed()) { *response = PULSE_PICKER_UI_V2_OVERWRITE_REFUSE; return S_OK; }
            if (hr == E_NOTIMPL) continue;
            if (hr != S_OK) return hr;
            if (native_response == FDEOR_DEFAULT) *response = PULSE_PICKER_UI_V2_OVERWRITE_DEFAULT;
            else if (native_response == FDEOR_ACCEPT) *response = PULSE_PICKER_UI_V2_OVERWRITE_ACCEPT;
            else if (native_response == FDEOR_REFUSE) *response = PULSE_PICKER_UI_V2_OVERWRITE_REFUSE;
            else return E_UNEXPECTED;
            return S_OK; // First handled response wins, including DEFAULT.
        }
        return S_OK; // All E_NOTIMPL leaves the initialized DEFAULT response.

    }
    static HRESULT WINAPI InprocessShareViolation(void* context, const PulsePickerUiV2OverwriteEvent* event, uint32_t* response) noexcept {
        auto* self = static_cast<FileDialog*>(context);
        self->inprocess_callback_seen_ = true;
        if (!response) return E_POINTER;
        *response = PULSE_PICKER_UI_V2_OVERWRITE_DEFAULT;
        try { return event ? self->DispatchShareViolation(*event, response) : E_INVALIDARG; }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; } catch (...) { return E_FAIL; }
    }
    HRESULT DispatchShareViolation(const PulsePickerUiV2OverwriteEvent& event, uint32_t* response) {
        if (FAILED(Thread()) || !inprocess_) return RPC_E_WRONG_THREAD;
        if (!(result_native_options_ & FOS_SHAREAWARE)) return E_NOTIMPL;
        const HRESULT observed = inprocess_->ObserveShareViolation(event);
        DiagnosticResult("inprocess-share-violation-observe", observed);
        if (FAILED(observed)) return observed;
        struct Exit { FileDialogInprocessSession* session; ~Exit() { session->LeaveEvent(); } } exit{inprocess_.get()};
        if (subscriptions_.empty()) return S_OK;
        struct Entry { DWORD cookie; ComPtr<IFileDialogEvents> proxy; uint64_t order; };
        std::vector<Entry> subscriptions;
        for (const auto& [cookie, value] : subscriptions_) subscriptions.push_back({cookie, value.proxy, value.order});
        std::sort(subscriptions.begin(), subscriptions.end(), [](const Entry& a, const Entry& b) { return a.order < b.order; });
        const std::wstring target(event.target.data, event.target.units);
        FileDialogInprocessSnapshot snapshot;
        const HRESULT queried = inprocess_->Read(snapshot);
        if (FAILED(queried)) return queried;
        if (std::find(snapshot.candidates.begin(), snapshot.candidates.end(), target) == snapshot.candidates.end()) return E_UNEXPECTED;
        ComPtr<IShellItem> item;
        // The target already exists but is locked. Re-running Save validation
        // here would reject it before the caller receives OnShareViolation.
        const HRESULT materialized = CreatePickerShellItem(wire::Operation::OpenFiles, target, &item, false, result_native_options_);
        if (FAILED(materialized)) return materialized;
        for (const auto& subscription : subscriptions) {
            const auto current = subscriptions_.find(subscription.cookie);
            if (current == subscriptions_.end() || current->second.order != subscription.order) continue;
            FDE_SHAREVIOLATION_RESPONSE native_response = FDESVR_DEFAULT;
            const HRESULT hr = subscription.proxy->OnShareViolation(Identity(), item.Get(), &native_response);
            char diagnostic[160]{};
            std::snprintf(diagnostic, sizeof(diagnostic), "inprocess-share-violation-sink order=%llu rawhr=0x%08lX response=%u",
                subscription.order, static_cast<unsigned long>(hr), static_cast<unsigned>(native_response));
            ObserveNativeReason(diagnostic);
            if (inprocess_->Closed()) { *response = PULSE_PICKER_UI_V2_OVERWRITE_REFUSE; return S_OK; }
            if (hr == E_NOTIMPL) continue;
            if (hr != S_OK) return hr;
            if (native_response == FDESVR_DEFAULT) *response = PULSE_PICKER_UI_V2_OVERWRITE_DEFAULT;
            else if (native_response == FDESVR_ACCEPT) *response = PULSE_PICKER_UI_V2_OVERWRITE_ACCEPT;
            else if (native_response == FDESVR_REFUSE) *response = PULSE_PICKER_UI_V2_OVERWRITE_REFUSE;
            else return E_UNEXPECTED;
            return S_OK; // First handled response wins, including DEFAULT.
        }
        return S_OK; // All E_NOTIMPL leaves the initialized DEFAULT response.

    }
    static HRESULT WINAPI InprocessEvent(void* context, const PulsePickerUiV2Event* event) noexcept {
        auto* self = static_cast<FileDialog*>(context);
        self->inprocess_callback_seen_ = true;
        char diagnostic[128]{};
        const unsigned kind = event ? event->kind : 0;
        std::snprintf(diagnostic, sizeof(diagnostic), "inprocess-event-enter kind=%u", kind);
        self->ObserveNativeReason(diagnostic);
        HRESULT hr = E_FAIL;
        try { hr = event ? self->DispatchInprocess(*event) : E_INVALIDARG; }
        catch (const std::bad_alloc&) { hr = E_OUTOFMEMORY; } catch (...) { hr = E_FAIL; }
        std::snprintf(diagnostic, sizeof(diagnostic), "inprocess-event-return kind=%u hr=0x%08lX", kind, static_cast<unsigned long>(hr));
        self->ObserveNativeReason(diagnostic);
        return hr;
    }
    HRESULT DispatchInprocess(const PulsePickerUiV2Event& event) {
        if (FAILED(Thread()) || !inprocess_) return RPC_E_WRONG_THREAD;
        HRESULT hr = inprocess_->Observe(event);
        DiagnosticResult("inprocess-observe-return", hr);
        if (FAILED(hr)) return hr;
        struct Exit { FileDialogInprocessSession* session; ~Exit() { session->LeaveEvent(); } } exit{inprocess_.get()};
        if (event.kind == PULSE_PICKER_UI_V2_EVENT_NAVIGATION_COMPLETE) {
            if (!event.proposed_folder.data || !event.proposed_folder.units || event.proposed_folder.units > PULSE_PICKER_UI_V2_MAX_UNITS) return E_INVALIDARG;
            const std::wstring path(event.proposed_folder.data, event.proposed_folder.units);
            const auto pending = std::find_if(pending_folders_.begin(), pending_folders_.end(), [&](const auto& item) { return item.path == path; });
            if (pending == pending_folders_.end()) return E_UNEXPECTED;
            auto folder = pending->folder;
            pending_folders_.erase(pending);
            if (event.navigation_result != S_OK) return S_OK;
            FileDialogInprocessSnapshot state;
            if (inprocess_->Read(state) != S_OK || _wcsicmp(state.folder.c_str(), path.c_str())) return E_UNEXPECTED;
            SetterCall call; call.setting = Setting::Folder; call.native_result = native_->SetFolder(folder.Get());
            if (call.native_result != S_OK) return call.native_result;
            explicit_folder_ = true; call.filesystem_folder = true;
            call.text.assign(path.begin(), path.end());
            return Track(std::move(call));
        }
        // Strong snapshot in Advise order. A callback can remove itself/another
        // sink or register a replacement using a recycled native cookie.
        struct Entry { DWORD cookie; ComPtr<IFileDialogEvents> proxy; uint64_t order; };
        std::vector<Entry> snapshot;
        for (const auto& [cookie, subscription] : subscriptions_) snapshot.push_back({cookie, subscription.proxy, subscription.order});
        std::sort(snapshot.begin(), snapshot.end(), [](const Entry& a, const Entry& b) { return a.order < b.order; });
        if (event.kind == PULSE_PICKER_UI_V2_EVENT_READY) return S_OK;
        if (event.kind == PULSE_PICKER_UI_V2_EVENT_PROPERTY_CHANGED) {
            if (!properties_owned_ || event.control_id >= properties_.Fields().size() ||
                event.property_value_count > 16 || (event.property_value_count && !event.property_values)) return E_INVALIDARG;
            std::vector<std::wstring> values;
            for (uint32_t i = 0; i < event.property_value_count; ++i) {
                const auto& value = event.property_values[i];
                if (value.units > PULSE_PICKER_UI_V2_MAX_UNITS || (value.units && !value.data)) return E_INVALIDARG;
                values.emplace_back(value.units ? std::wstring(value.data, value.units) : std::wstring());
            }
            const PROPERTYKEY key = properties_.Fields()[event.control_id].key;
            return properties_.Edit(key, values);
        }
        const auto interactive = [&](const FileDialogCustomControl* control) {
            if (!control || (control->state & 3u) != 3u) return false;
            if (!control->has_parent || (control->state & PULSE_PICKER_UI_V2_CONTROL_PROMINENT)) return true;
            const auto* parent=FindCustom(control->parent);
            return parent && (parent->state & 3u)==3u;
        };
        if (event.kind == PULSE_PICKER_UI_V2_EVENT_BUTTON_CLICKED) {
            const auto* button = FindCustom(event.control_id);
            if (!interactive(button) || button->kind != PULSE_PICKER_UI_V2_CONTROL_BUTTON) return E_INVALIDARG;
        }
        if(event.kind==PULSE_PICKER_UI_V2_EVENT_CONTROL_ACTIVATING) {
            const auto* control=FindCustom(event.control_id);
            if(!interactive(control) || (control->kind!=4 && control->kind!=5)) return E_INVALIDARG;
        }
        if (event.kind == PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED || event.kind == PULSE_PICKER_UI_V2_EVENT_OPEN_DROPDOWN_SELECTED) {
            auto* control = FindCustom(event.control_id);
            auto* item = control ? FindItem(*control, event.item_id) : nullptr;
            if (!interactive(control) || (event.kind==PULSE_PICKER_UI_V2_EVENT_OPEN_DROPDOWN_SELECTED ? control->kind!=5 : (control->kind!=3 && control->kind!=4 && control->kind!=6)) || !item || item->state != 3) return E_INVALIDARG;
            ComPtr<IFileDialogCustomize> native; hr = native_.As(&native);
            if (SUCCEEDED(hr) && (control->kind == PULSE_PICKER_UI_V2_CONTROL_COMBO || control->kind==6)) hr = native->SetSelectedControlItem(event.control_id, event.item_id);
            if (hr != S_OK) { inprocess_->Close(FAILED(hr) ? hr : E_UNEXPECTED); return hr; }
            if(control->kind!=PULSE_PICKER_UI_V2_CONTROL_MENU) { control->has_selected = true; control->selected_item_id = event.item_id; }
            if(event.kind==PULSE_PICKER_UI_V2_EVENT_OPEN_DROPDOWN_SELECTED) return S_OK;
        }
        if(event.kind==PULSE_PICKER_UI_V2_EVENT_CHECK_TOGGLED || event.kind==PULSE_PICKER_UI_V2_EVENT_EDIT_CHANGED) {
            auto* control=FindCustom(event.control_id);
            const bool text_event=event.kind==PULSE_PICKER_UI_V2_EVENT_EDIT_CHANGED;
            if(!interactive(control) || control->kind!=(text_event?8u:7u) || (!text_event && event.item_id>1))return E_INVALIDARG;
            std::wstring value;
            if(text_event) {
                if(event.control_text.units>PULSE_PICKER_UI_V2_MAX_UNITS || (event.control_text.units && !event.control_text.data))return E_INVALIDARG;
                if(event.control_text.units)value.assign(event.control_text.data,event.control_text.units);
                if(value.find(L'\0')!=value.npos)return E_INVALIDARG;
            }
            ComPtr<IFileDialogCustomize> native;hr=native_.As(&native);
            if(SUCCEEDED(hr))hr=text_event?native->SetEditBoxText(control->id,value.c_str()):native->SetCheckButtonState(control->id,event.item_id?TRUE:FALSE);
            if(hr!=S_OK){inprocess_->Close(FAILED(hr)?hr:E_UNEXPECTED);return hr;}
            if(text_event){control->text=std::move(value);return S_OK;}
            control->checked=event.item_id!=0;
        }
        ComPtr<IShellItem> proposed;
        if (event.kind == PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGING) {
            if (!event.proposed_folder.data || !event.proposed_folder.units || event.proposed_folder.units > PULSE_PICKER_UI_V2_MAX_UNITS) return E_INVALIDARG;
            const std::wstring path(event.proposed_folder.data, event.proposed_folder.units);
            hr = picker_navigation::IsComputerPath(path) ?
                SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&proposed)) :
                CreatePickerShellItem(wire::Operation::PickFolder, path, &proposed, false, result_native_options_);
            if (FAILED(hr)) return hr;
        }
        for (const auto& entry : snapshot) {
            const auto current = subscriptions_.find(entry.cookie);
            if (current == subscriptions_.end() || current->second.order != entry.order) continue;
            char sink_diagnostic[160]{};
            std::snprintf(sink_diagnostic, sizeof(sink_diagnostic), "inprocess-sink-enter kind=%lu order=%llu", static_cast<unsigned long>(event.kind), entry.order);
            ObserveNativeReason(sink_diagnostic);
            RECT before_rect{};
            const BOOL before_valid = GetWindowRect(event.window, &before_rect);
            const BOOL before_visible = IsWindowVisible(event.window);
            const LONG_PTR before_style = GetWindowLongPtrW(event.window, GWL_EXSTYLE);
            switch (event.kind) {
            case PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGING:
                hr = entry.proxy->OnFolderChanging(Identity(), proposed.Get());
                break;
            case PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGED: hr = entry.proxy->OnFolderChange(Identity()); break;
            case PULSE_PICKER_UI_V2_EVENT_SELECTION_CHANGED: hr = entry.proxy->OnSelectionChange(Identity()); break;
            case PULSE_PICKER_UI_V2_EVENT_TYPE_CHANGED: hr = entry.proxy->OnTypeChange(Identity()); break;
            case PULSE_PICKER_UI_V2_EVENT_FILE_OK: hr = entry.proxy->OnFileOk(Identity()); break;
            case PULSE_PICKER_UI_V2_EVENT_CHECK_TOGGLED:
            case PULSE_PICKER_UI_V2_EVENT_CONTROL_ACTIVATING:
            case PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED:
            case PULSE_PICKER_UI_V2_EVENT_BUTTON_CLICKED: {
                ComPtr<IFileDialogControlEvents> controls;
                hr = entry.proxy.As(&controls);
                if (SUCCEEDED(hr)) hr = event.kind == PULSE_PICKER_UI_V2_EVENT_CHECK_TOGGLED ? controls->OnCheckButtonToggled(static_cast<IFileDialogCustomize*>(this),event.control_id,event.item_id?TRUE:FALSE) : event.kind == PULSE_PICKER_UI_V2_EVENT_CONTROL_ACTIVATING ? controls->OnControlActivating(static_cast<IFileDialogCustomize*>(this),event.control_id) : event.kind == PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED ?
                    controls->OnItemSelected(static_cast<IFileDialogCustomize*>(this), event.control_id, event.item_id) :
                    controls->OnButtonClicked(static_cast<IFileDialogCustomize*>(this), event.control_id);
                break;
            }
            default: return E_INVALIDARG;
            }
            RECT after_rect{};
            const BOOL after_valid = GetWindowRect(event.window, &after_rect);
            const BOOL after_visible = IsWindowVisible(event.window);
            const LONG_PTR after_style = GetWindowLongPtrW(event.window, GWL_EXSTYLE);
            char window_diagnostic[320]{};
            std::snprintf(window_diagnostic, sizeof(window_diagnostic),
                "inprocess-sink-window kind=%lu order=%llu valid=%u/%u visible=%u/%u rect=%ld,%ld,%ld,%ld/%ld,%ld,%ld,%ld exstyle=%llX/%llX",
                static_cast<unsigned long>(event.kind), entry.order, before_valid ? 1u : 0u, after_valid ? 1u : 0u,
                before_visible ? 1u : 0u, after_visible ? 1u : 0u,
                before_rect.left, before_rect.top, before_rect.right, before_rect.bottom,
                after_rect.left, after_rect.top, after_rect.right, after_rect.bottom,
                static_cast<unsigned long long>(before_style), static_cast<unsigned long long>(after_style));
            ObserveNativeReason(window_diagnostic);
            std::snprintf(sink_diagnostic, sizeof(sink_diagnostic), "inprocess-sink-return kind=%lu order=%llu rawhr=0x%08lX",
                static_cast<unsigned long>(event.kind), entry.order, static_cast<unsigned long>(hr));
            ObserveNativeReason(sink_diagnostic);
            if (event.kind == PULSE_PICKER_UI_V2_EVENT_BUTTON_CLICKED || event.kind == PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED || event.kind == PULSE_PICKER_UI_V2_EVENT_CONTROL_ACTIVATING || event.kind == PULSE_PICKER_UI_V2_EVENT_CHECK_TOGGLED) hr = S_OK; // Native ignores all control-event HRESULTs.
            // Native oracle (both Advise orders): E_NOTIMPL is unhandled,
            // not a veto, for these standard events. Preserve every other HR.
            if (hr == E_NOTIMPL && (event.kind == PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGING || event.kind == PULSE_PICKER_UI_V2_EVENT_TYPE_CHANGED ||
                event.kind == PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGED || event.kind == PULSE_PICKER_UI_V2_EVENT_SELECTION_CHANGED ||
                event.kind == PULSE_PICKER_UI_V2_EVENT_FILE_OK)) hr = S_OK;
            if (inprocess_->Closed()) return S_OK;
            // Native FileOk oracle: Advise order, first non-S_OK stops this round.
            // Other notification aggregation remains experimental, not certified.
            if (hr != S_OK) return event.kind == PULSE_PICKER_UI_V2_EVENT_FILE_OK ? inprocess_->CompleteFileOk(hr) : hr;
        }
        return event.kind == PULSE_PICKER_UI_V2_EVENT_FILE_OK ? inprocess_->CompleteFileOk(S_OK) : S_OK;
    }
    ~FileDialog() {
        // Final Release already disconnected weak owner access. Releasing a
        // native subscription can destroy an arbitrary caller-provided sink.
        for (const auto& entry : subscriptions_) native_->Unadvise(entry.first);
        subscriptions_.clear();
    }
    IFileDialog* Identity() { return factory_ == DialogFactory::Open ? static_cast<IFileDialog*>(static_cast<IFileOpenDialog*>(this)) : static_cast<IFileDialog*>(static_cast<IFileSaveDialog*>(this)); }
    HRESULT Thread() const { return GetCurrentThreadId() == thread_ ? S_OK : RPC_E_WRONG_THREAD; }
    HRESULT Mutable() const {
        if (FAILED(Thread())) return RPC_E_WRONG_THREAD;
        return showing_ && bridge_result_ ? E_UNEXPECTED : S_OK;
    }
    static bool CopyText(LPCWSTR text, std::u16string& copy, size_t* observed_units = nullptr) {
        if (!text) return false;
        size_t size = 0;
        while (size <= wire::kMaxStringUnits && text[size]) ++size;
        if (observed_units) *observed_units = size;
        if (size > wire::kMaxStringUnits) return false;
        copy.assign(text, text + size); return true;
    }
    static const char* SettingName(Setting setting) noexcept {
        switch (setting) {
        case Setting::Options: return "SetOptions";
        case Setting::FileTypes: return "SetFileTypes";
        case Setting::FileTypeIndex: return "SetFileTypeIndex";
        case Setting::Folder: return "SetFolder";
        case Setting::DefaultFolder: return "SetDefaultFolder";
        case Setting::FileName: return "SetFileName";
        case Setting::DefaultExtension: return "SetDefaultExtension";
        case Setting::Title: return "SetTitle";
        case Setting::OkButtonLabel: return "SetOkButtonLabel";
        case Setting::FileNameLabel: return "SetFileNameLabel";
        case Setting::Place: return "AddPlace";
        case Setting::ClientGuid: return "SetClientGuid";
        case Setting::ClearClientData: return "ClearClientData";
        case Setting::Events: return "Events";
        case Setting::Customize: return "Customize";
        case Setting::SaveProperties: return "SaveProperties";
        case Setting::ReadonlyOwnershipCollection: return "ReadonlyOwnershipCollection";
        case Setting::SaveAsItem: return "SetSaveAsItem";
        case Setting::ShellFilter: return "SetFilter";
        default: return "Unknown";
        }
    }
    void ReportSetter(const SetterCall& call) noexcept {
        // Runtime's entire record is bounded to 256 bytes. Emit the sequence
        // independently; extension chunks stay below 120 bytes including metadata.
        if (calls_.size() >= 256) return;
        char detail[128]{};
        std::snprintf(detail, sizeof(detail), "setter seq=%zu method=%s hr=0x%08lX value=%lu",
            calls_.size(), SettingName(call.setting), static_cast<unsigned long>(call.native_result), static_cast<unsigned long>(call.value));
        ObserveNativeReason(detail);
        if (call.setting == Setting::FileTypes) {
            for (size_t i = 0; i < call.filters.size(); ++i) {
                const auto& pattern = call.filters[i].pattern;
                // Classify syntax only: never log caller filenames or labels.
                std::snprintf(detail, sizeof(detail), "filter seq=%zu index=%zu units=%zu star=%u all=%u semicolon=%u",
                    calls_.size(), i, pattern.size(), pattern == u"*" ? 1u : 0u,
                    pattern == u"*.*" ? 1u : 0u, pattern.find(u';') != pattern.npos ? 1u : 0u);
                ObserveNativeReason(detail);
                unsigned extension_stars = 0, questions = 0, invalid = 0;
                bool extension = false;
                for (size_t offset = 0; offset < pattern.size(); ++offset) {
                    const auto ch = pattern[offset];
                    if (ch == u';') { extension = false; continue; }
                    if (ch == u'.') extension = true;
                    if (ch == u'*' && extension) ++extension_stars;
                    if (ch == u'?') ++questions;
                    if (ch < 32 || std::u16string_view(u"<>:\"/\\| ").find(ch) != std::u16string_view::npos) ++invalid;
                }
                std::snprintf(detail, sizeof(detail), "filter-shape seq=%zu index=%zu extension_stars=%u questions=%u forbidden=%u",
                    calls_.size(), i, extension_stars, questions, invalid);
                ObserveNativeReason(detail);
            }
        }
        if (call.setting != Setting::DefaultExtension) return;
        const bool available = SUCCEEDED(call.native_result);
        const bool path_like = call.text.find_first_of(u"\\/:") != call.text.npos;
        std::snprintf(detail, sizeof(detail), "extension seq=%zu available=%u units=%zu redacted=%u truncated=%u",
            calls_.size(), available ? 1u : 0u, call.text.size(), path_like ? 1u : 0u, call.text.size() > 128 ? 1u : 0u);
        ObserveNativeReason(detail);
        if (!available || path_like) return;
        for (size_t offset = 0; offset < call.text.size() && offset < 128; offset += 8) {
            const int header = std::snprintf(detail, sizeof(detail), "extension seq=%zu offset=%zu utf16=", calls_.size(), offset);
            if (header <= 0 || static_cast<size_t>(header) >= sizeof(detail)) return;
            size_t used = static_cast<size_t>(header);
            for (size_t i = offset; i < call.text.size() && i < offset + 8; ++i) {
                const int written = std::snprintf(detail + used, sizeof(detail) - used, "\\u%04X", static_cast<unsigned>(call.text[i]));
                if (written != 6) return;
                used += 6;
            }
            ObserveNativeReason(detail);
        }
    }
    HRESULT Track(SetterCall call) noexcept {
        const HRESULT hr = call.native_result;
        ReportSetter(call);
        try { calls_.push_back(std::move(call)); return hr; }
        catch (...) { MarkNativeOnly("setter-trace-allocation"); return E_OUTOFMEMORY; }
    }
    HRESULT Unsupported(Setting setting, HRESULT hr) noexcept {
        MarkNativeOnly("unsupported-setting");
        SetterCall call; call.setting = setting; call.native_result = hr;
        return Track(std::move(call));
    }
    HRESULT Text(Setting setting, LPCWSTR text, HRESULT (STDMETHODCALLTYPE IFileDialog::*method)(LPCWSTR)) {
        if (inprocess_ && (setting == Setting::Title || setting == Setting::OkButtonLabel || setting == Setting::FileNameLabel)) return E_NOTIMPL;
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        const HRESULT hr = (native_.Get()->*method)(text);
        SetterCall call; call.setting = setting; call.native_result = hr;
        try {
            // The system dialog accepts null here and clears a previous default.
            // Other text setters retain their own native validation and bounds.
            const bool cleared_extension = setting == Setting::DefaultExtension && text == nullptr;
            size_t units = 0;
            const bool captured = SUCCEEDED(hr) && (cleared_extension || CopyText(text, call.text, &units));
            if (SUCCEEDED(hr) && !captured) MarkNativeOnly("text-snapshot-invalid");
            char diagnostic[160]{};
            std::snprintf(diagnostic, sizeof(diagnostic), "text-snapshot setting=%u null=%u units=%zu captured=%u hr=0x%08lX",
                static_cast<unsigned>(setting), text ? 0u : 1u, units, captured ? 1u : 0u, static_cast<unsigned long>(hr));
            ObserveNativeReason(diagnostic);
            return Track(std::move(call));
        } catch (...) { MarkNativeOnly("text-snapshot-allocation"); return E_OUTOFMEMORY; }
    }
    template<class F> HRESULT Dialog2Call(F&& call) {
        if (const auto hr = Thread(); FAILED(hr)) return hr;
        MarkNativeOnly("dialog2-method", &IID_IFileDialog2);
        if (bridge_result_) return E_UNEXPECTED;
        if (!native2_) return E_NOINTERFACE;
        AddRef();
        struct Hold { FileDialog* self; ~Hold() { self->Release(); } } hold{this};
        try { return call(native2_.Get()); }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        catch (...) { return E_FAIL; }
    }
    bool CanCaptureInprocess() const noexcept {
        return services_.inprocess_provider || static_cast<bool>(services_.resolve_on_show) ||
            static_cast<bool>(services_.load_inprocess_provider);
    }
    FileDialogCustomControl* FindCustom(DWORD id) {
        for (auto& control : custom_controls_) if (control.id == id) return &control;
        return nullptr;
    }
    static bool CustomLabel(LPCWSTR text, std::wstring& label) {
        if (!text) return false;
        size_t n = 0; while (n <= 256 && text[n]) { if (text[n] == L'\r' || text[n] == L'\n' || text[n] == L'\t') return false; ++n; }
        if (n > 256) return false; label.assign(text, n); return true;
    }
    enum class ItemMutation { Add, Remove, Clear, State, Select, Label };
    static FileDialogCustomItem* FindItem(FileDialogCustomControl& control, DWORD id) {
        for (auto& item : control.items) if (item.id == id) return &item;
        return nullptr;
    }
    static bool EditText(LPCWSTR text,std::wstring& value) {
        if(!text)return false;
        size_t n=0;while(n<=PULSE_PICKER_UI_V2_MAX_UNITS && text[n])++n;
        if(n>PULSE_PICKER_UI_V2_MAX_UNITS)return false;
        value.assign(text,n);return value.find_first_of(L"\r\n") == value.npos;
    }
    HRESULT AddValueControl(uint32_t kind,DWORD id,LPCWSTR text,BOOL checked) {
        const auto call=[&](IFileDialogCustomize* c){return kind==6?c->AddRadioButtonList(id):kind==7?c->AddCheckButton(id,text,checked):c->AddEditBox(id,text);};
        const char* name=kind==6?"AddRadioButtonList":kind==7?"AddCheckButton":"AddEditBox";
        if(!CanCaptureInprocess())return CustomizeCall(name,false,id,0,checked,call);
        if(const auto hr=Mutable();FAILED(hr))return hr;
        ComPtr<IFileDialogCustomize> native;HRESULT hr=native_.As(&native);if(FAILED(hr))return hr;
        hr=call(native.Get());if(hr!=S_OK)return hr;
        try {
            FileDialogCustomControl control;control.kind=kind;control.id=id;control.checked=checked!=FALSE;
            if(FindCustom(id) || custom_controls_.size()>=16 || (kind==7 && !CustomLabel(text,control.label)) || (kind==8 && !EditText(text,control.text))) {MarkNativeOnly("value-control-unrepresentable");return hr;}
            control.has_parent=custom_group_;control.parent=custom_group_?custom_group_id_:0;
            custom_controls_.push_back(std::move(control));
        }catch(...){MarkNativeOnly("value-control-allocation");}
        return hr;
    }
    HRESULT ChangeValueControl(DWORD id,LPCWSTR text,BOOL checked,bool text_value) {
        if(FAILED(Thread()))return RPC_E_WRONG_THREAD;
        const auto call=[&](IFileDialogCustomize* c){return text_value?c->SetEditBoxText(id,text):c->SetCheckButtonState(id,checked);};
        auto* control=FindCustom(id);
        if(!CanCaptureInprocess() || !control || control->kind!=(text_value?8u:7u))return CustomizeCall(text_value?"SetEditBoxText":"SetCheckButtonState",false,id,0,checked,call);
        if(!inprocess_){if(const auto hr=Mutable();FAILED(hr))return hr;}
        try {
            auto next=*control;const bool valid=!text_value || EditText(text,next.text);if(!text_value)next.checked=checked!=FALSE;
            if(inprocess_ && !valid)return E_INVALIDARG;
            ComPtr<IFileDialogCustomize> native;HRESULT hr=native_.As(&native);if(FAILED(hr))return hr;
            hr=call(native.Get());if(hr!=S_OK || !valid){if(!inprocess_)MarkNativeOnly("value-control-unrepresentable");return hr;}
            if(!inprocess_){*control=std::move(next);return hr;}
            hr=inprocess_->UpdateControl(next);
            if(hr==S_OK){*control=std::move(next);return S_OK;}
            const HRESULT restored=text_value?native->SetEditBoxText(id,control->text.c_str()):native->SetCheckButtonState(id,control->checked?TRUE:FALSE);
            const HRESULT ui_restored=inprocess_->UpdateControl(*control);
            if(restored!=S_OK || ui_restored!=S_OK){custom_desynchronized_=true;inprocess_->Close(E_UNEXPECTED);}
            return FAILED(hr)?hr:E_UNEXPECTED;
        }catch(...){if(inprocess_)inprocess_->Close(E_OUTOFMEMORY);else MarkNativeOnly("value-control-allocation");return E_OUTOFMEMORY;}
    }
    HRESULT AddContainer(uint32_t kind, DWORD id, LPCWSTR text) {
        const auto call = [&](IFileDialogCustomize* c) { return kind == PULSE_PICKER_UI_V2_CONTROL_MENU ? c->AddMenu(id,text) : c->EnableOpenDropDown(id); };
        if (!CanCaptureInprocess()) return CustomizeCall(kind==4 ? "AddMenu" : "EnableOpenDropDown",false,id,0,0,call);
        if (const auto hr=Mutable(); FAILED(hr)) return hr;
        ComPtr<IFileDialogCustomize> native; HRESULT hr=native_.As(&native); if(FAILED(hr)) return hr;
        hr=call(native.Get()); if(hr!=S_OK) return hr;
        try {
            FileDialogCustomControl control; control.kind=kind;control.id=id;
            if(FindCustom(id) || custom_controls_.size()>=16 || (kind==4 && !CustomLabel(text,control.label))) { MarkNativeOnly("container-unrepresentable"); return hr; }
            control.has_parent=kind==4 && custom_group_;control.parent=control.has_parent ? custom_group_id_ : 0;
            custom_controls_.push_back(std::move(control));
        } catch(...) { MarkNativeOnly("container-allocation"); }
        return hr;
    }
    HRESULT AddCombo(DWORD id) {
        if (!CanCaptureInprocess()) return CustomizeCall("AddComboBox", false, id, 0, 0, [&](IFileDialogCustomize* c) { return c->AddComboBox(id); });
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        ComPtr<IFileDialogCustomize> c; HRESULT hr = native_.As(&c); if (FAILED(hr)) return hr;
        hr = c->AddComboBox(id);
        if (hr != S_OK) { MarkNativeOnly("combo-native-failure"); return hr; }
        try {
            if (FindCustom(id) || custom_controls_.size() >= 16) { MarkNativeOnly("combo-unrepresentable"); return hr; }
            FileDialogCustomControl control; control.kind = PULSE_PICKER_UI_V2_CONTROL_COMBO; control.id = id;
            control.has_parent = custom_group_; control.parent = control.has_parent ? custom_group_id_ : 0;
            custom_controls_.push_back(std::move(control));
        } catch (...) { MarkNativeOnly("combo-allocation"); }
        return hr;
    }
    HRESULT ChangeItem(ItemMutation operation, DWORD id, DWORD item, LPCWSTR text, DWORD state) {
        if (FAILED(Thread())) return RPC_E_WRONG_THREAD;
        const auto call = [&](IFileDialogCustomize* c) -> HRESULT {
            switch (operation) {
            case ItemMutation::Add: return c->AddControlItem(id, item, text);
            case ItemMutation::Remove: return c->RemoveControlItem(id, item);
            case ItemMutation::Clear: return c->RemoveAllControlItems(id);
            case ItemMutation::State: return c->SetControlItemState(id, item, static_cast<CDCONTROLSTATEF>(state));
            case ItemMutation::Select: return c->SetSelectedControlItem(id, item);
            case ItemMutation::Label: return c->SetControlItemText(id, item, text);
            }
            return E_UNEXPECTED;
        };
        const char* method = operation == ItemMutation::Add ? "AddControlItem" : operation == ItemMutation::Remove ? "RemoveControlItem" :
            operation == ItemMutation::Clear ? "RemoveAllControlItems" : operation == ItemMutation::State ? "SetControlItemState" :
            operation == ItemMutation::Select ? "SetSelectedControlItem" : "SetControlItemText";
        auto* control = FindCustom(id);
        if (!CanCaptureInprocess() || !control || control->kind < PULSE_PICKER_UI_V2_CONTROL_COMBO || control->kind>6)
            return CustomizeCall(method, false, id, item, state, call);
        const bool structural = operation == ItemMutation::Add || operation == ItemMutation::Remove || operation == ItemMutation::Clear;
        if (inprocess_ && structural) return E_NOTIMPL;
        if (!inprocess_) { if (const auto hr = Mutable(); FAILED(hr)) return hr; }
        try {
            auto next = *control;
            auto* changed = FindItem(next, item);
            bool valid = true;
            if (operation == ItemMutation::Add) {
                FileDialogCustomItem addition; addition.id = item;
                valid = !changed && next.items.size() < PULSE_PICKER_UI_V2_MAX_CONTROL_ITEMS && CustomLabel(text, addition.label);
                if (valid) next.items.push_back(std::move(addition));
            } else if (operation == ItemMutation::Clear) { next.items.clear(); next.has_selected = false; next.selected_item_id = 0; }
            else if (!changed) valid = false;
            else if (operation == ItemMutation::Remove) {
                std::erase_if(next.items, [&](const auto& value) { return value.id == item; });
                if (next.has_selected && next.selected_item_id == item) { next.has_selected = false; next.selected_item_id = 0; }
            } else if (operation == ItemMutation::Select) { next.has_selected = true; next.selected_item_id = item; }
            else if (operation == ItemMutation::State) { valid = !(state & ~3u); changed->state = state; }
            else valid = CustomLabel(text, changed->label);
            if (inprocess_ && !valid) return E_INVALIDARG;
            ComPtr<IFileDialogCustomize> native; HRESULT hr = native_.As(&native); if (FAILED(hr)) return hr;
            hr = call(native.Get());
            if (hr != S_OK || !valid) {
                char diagnostic[160]{};
                std::snprintf(diagnostic, sizeof(diagnostic), "control-item-rejected method=%s id=%lu item=%lu valid=%u hr=0x%08lX live=%u",
                    method, static_cast<unsigned long>(id), static_cast<unsigned long>(item), valid ? 1u : 0u,
                    static_cast<unsigned long>(hr), inprocess_ ? 1u : 0u);
                ObserveNativeReason(diagnostic);
                if (!inprocess_) MarkNativeOnly("combo-item-unrepresentable");
                return hr;
            }
            // Native may choose a selection when adding/removing an item; mirror
            // the observed getter, never assume item zero means no selection.
            DWORD selected = 0; const HRESULT selected_hr = native->GetSelectedControlItem(id, &selected);
            next.has_selected = selected_hr == S_OK; next.selected_item_id = next.has_selected ? selected : 0;
            if (next.has_selected && !FindItem(next, selected)) { MarkNativeOnly("combo-selection-unrepresentable"); if (inprocess_) inprocess_->Close(E_UNEXPECTED); return E_UNEXPECTED; }
            if (!inprocess_) { *control = std::move(next); return hr; }
            try { hr = inprocess_->UpdateControl(next); } catch (...) { hr = E_FAIL; }
            if (hr == S_OK) { *control = std::move(next); return S_OK; }
            const auto* old_item = FindItem(*control, item);
            HRESULT restored = E_UNEXPECTED;
            if (operation == ItemMutation::State && old_item) restored = native->SetControlItemState(id, item, static_cast<CDCONTROLSTATEF>(old_item->state));
            else if (operation == ItemMutation::Label && old_item) restored = native->SetControlItemText(id, item, old_item->label.c_str());
            else if (operation == ItemMutation::Select && control->has_selected) restored = native->SetSelectedControlItem(id, control->selected_item_id);
            HRESULT ui_restored = E_FAIL; try { ui_restored = inprocess_->UpdateControl(*control); } catch (...) {}
            if (restored != S_OK || ui_restored != S_OK) { custom_desynchronized_ = true; MarkNativeOnly("combo-rollback-failed"); inprocess_->Close(E_UNEXPECTED); return E_UNEXPECTED; }
            return hr;
        } catch (...) { MarkNativeOnly("combo-allocation"); if (inprocess_) inprocess_->Close(E_OUTOFMEMORY); return E_OUTOFMEMORY; }
    }
    HRESULT AddCustom(bool group, DWORD id, LPCWSTR text) {
        if (!CanCaptureInprocess()) return CustomizeCall(group ? "StartVisualGroup" : "AddPushButton", false, id, 0, 0,
            [&](IFileDialogCustomize* c) { return group ? c->StartVisualGroup(id, text) : c->AddPushButton(id, text); });
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        ComPtr<IFileDialogCustomize> c; HRESULT hr = native_.As(&c); if (FAILED(hr)) return hr;
        hr = group ? c->StartVisualGroup(id, text) : c->AddPushButton(id, text);
        if (hr != S_OK) { MarkNativeOnly("custom-model-native-failure"); return hr; }
        try {
            FileDialogCustomControl control; control.kind = group ? PULSE_PICKER_UI_V2_CONTROL_GROUP : PULSE_PICKER_UI_V2_CONTROL_BUTTON;
            control.id = id; control.has_parent = !group && custom_group_; control.parent = control.has_parent ? custom_group_id_ : 0;
            if (FindCustom(id) || (group && custom_group_) || custom_controls_.size() >= 16 || !CustomLabel(text, control.label)) { MarkNativeOnly("custom-model-unrepresentable"); return hr; }
            custom_controls_.push_back(std::move(control));
            if (group) { custom_group_ = true; custom_group_id_ = id; }
        } catch (...) { MarkNativeOnly("custom-model-allocation"); }
        return hr;
    }
    HRESULT EndCustom() {
        if (!CanCaptureInprocess()) return CustomizeCall("EndVisualGroup", false, 0, 0, 0, [](IFileDialogCustomize* c) { return c->EndVisualGroup(); });
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        ComPtr<IFileDialogCustomize> c; HRESULT hr = native_.As(&c); if (FAILED(hr)) return hr;
        hr = c->EndVisualGroup();
        if (hr != S_OK || !custom_group_) MarkNativeOnly("custom-model-group-end");
        else custom_group_ = false;
        return hr;
    }
    HRESULT ChangeCustom(DWORD id, LPCWSTR text, DWORD state, bool label) {
        if (FAILED(Thread())) return RPC_E_WRONG_THREAD;
        auto* control = FindCustom(id);
        if (!CanCaptureInprocess() || !control) return CustomizeCall(label ? "SetControlLabel" : "SetControlState", false, id, 0, state,
            [&](IFileDialogCustomize* c) { return label ? c->SetControlLabel(id, text) : c->SetControlState(id, static_cast<CDCONTROLSTATEF>(state)); });
        try {
            auto next = *control;
            const bool valid = label ? CustomLabel(text, next.label) : !(state & ~3u);
            if (!label) next.state = state | (control->state & PULSE_PICKER_UI_V2_CONTROL_PROMINENT);
            if (inprocess_) {
                if (!valid) return E_INVALIDARG;
                ComPtr<IFileDialogCustomize> native;
                HRESULT hr = native_.As(&native); if (FAILED(hr)) return hr;
                const auto apply = [&](const FileDialogCustomControl& value) {
                    return label ? native->SetControlLabel(id, value.label.c_str()) :
                        native->SetControlState(id, static_cast<CDCONTROLSTATEF>(value.state & 3u));
                };
                // A later Show can use the native dialog. Commit both backends,
                // not just the active UI; failed UI updates restore the mirror.
                hr = apply(next);
                if (hr != S_OK) return hr;
                try { hr = inprocess_->UpdateControl(next); } catch (...) { hr = E_FAIL; }
                if (hr == S_OK) { *control = std::move(next); return S_OK; }
                // A provider can fail after applying an update (for example a
                // snapshot callback failure), so restore both sides explicitly.
                HRESULT ui_restored = E_FAIL;
                try { ui_restored = inprocess_->UpdateControl(*control); } catch (...) {}
                const HRESULT restored = apply(*control);
                if (restored != S_OK || ui_restored != S_OK) {
                    custom_desynchronized_ = true;
                    MarkNativeOnly("custom-model-rollback-failed");
                    inprocess_->Close(E_UNEXPECTED);
                    return E_UNEXPECTED;
                }
                return hr;
            }
            if (const auto hr = Mutable(); FAILED(hr)) return hr;
            ComPtr<IFileDialogCustomize> c; HRESULT hr = native_.As(&c); if (FAILED(hr)) return hr;
            hr = label ? c->SetControlLabel(id, text) : c->SetControlState(id, static_cast<CDCONTROLSTATEF>(state));
            if (hr == S_OK && valid) *control = std::move(next); else MarkNativeOnly("custom-model-update-failure");
            return hr;
        } catch (...) { MarkNativeOnly("custom-model-allocation"); return E_OUTOFMEMORY; }
    }
    template<class F> HRESULT CustomizeCall(const char* method, bool read_only, DWORD control, DWORD item, DWORD value, F&& call) {
        const auto report = [&](const char* stage, HRESULT result, bool invoked) {
            char reason[256]{};
            std::snprintf(reason, sizeof(reason),
                "customize-%s method=%s hr=0x%08lX kind=%s id=%lu item=%lu state_arg=%lu called=%u outcome=%s",
                stage, method, static_cast<unsigned long>(result), read_only ? "read" : "mutation",
                control, item, value, invoked ? 1u : 0u, !invoked ? "not-invoked" :
                    FAILED(result) ? "failed" : result == S_OK ? (read_only ? "read-ok" : "mutation-ok") : "other-success");
            if (stage[0] == 'c') MarkNativeOnly(reason, &IID_IFileDialogCustomize);
            else ObserveNativeReason(reason, &IID_IFileDialogCustomize);
        };
        if (const auto hr = Thread(); FAILED(hr)) { report("result", hr, false); return hr; }
        report("call", E_PENDING, false);
        // Keep every existing native gate, including reads and failed calls.
        // Diagnostics carry only static method names, numeric IDs and HRESULT.
        if (bridge_result_) { report("result", E_UNEXPECTED, false); return E_UNEXPECTED; }
        AddRef();
        struct Hold { FileDialog* self; ~Hold() { self->Release(); } } hold{this};
        ComPtr<IFileDialogCustomize> customize;
        const HRESULT queried = native_.As(&customize);
        if (FAILED(queried)) { report("result", queried, false); return queried; }
        const HRESULT result = call(customize.Get());
        report("result", result, true);
        return result;
    }
    void ReportCollectedSchema(IPropertyDescriptionList* properties, UINT count) noexcept {
        // Property schema only: never call GetValue, enumerate a property store,
        // or read display text/metadata from a file. Keep every reason <128 bytes.
        try {
            ComPtr<IPropertyDescriptionList> list = properties;
            for (UINT i = 0; i < count && i < 8; ++i) {
                ComPtr<IPropertyDescription> description;
                const HRESULT got = list->GetAt(i, IID_PPV_ARGS(&description));
                char detail[128]{};
                std::snprintf(detail, sizeof(detail), "property i=%u get_hr=0x%08lX present=%u", i, static_cast<unsigned long>(got), description ? 1u : 0u);
                ObserveNativeReason(detail);
                if (got != S_OK || !description) continue;
                PROPERTYKEY key{};
                const HRESULT keyed = description->GetPropertyKey(&key);
                wchar_t guid[40]{}; char ascii_guid[40]{};
                if (keyed == S_OK && StringFromGUID2(key.fmtid, guid, 40)) for (size_t n = 0; n < 39 && guid[n]; ++n) ascii_guid[n] = static_cast<char>(guid[n]);
                std::snprintf(detail, sizeof(detail), "property-key i=%u hr=0x%08lX fmtid=%s pid=%lu", i, static_cast<unsigned long>(keyed), ascii_guid, key.pid);
                ObserveNativeReason(detail);
                VARTYPE type = VT_EMPTY; PROPDESC_TYPE_FLAGS flags{};
                const HRESULT typed = description->GetPropertyType(&type);
                const HRESULT flagged = description->GetTypeFlags(PDTF_MASK_ALL, &flags);
                std::snprintf(detail, sizeof(detail), "property-type i=%u hr=0x%08lX vt=%u flags_hr=0x%08lX flags=0x%08lX", i,
                    static_cast<unsigned long>(typed), static_cast<unsigned>(type), static_cast<unsigned long>(flagged), static_cast<unsigned long>(flags));
                ObserveNativeReason(detail);
                struct Name { PWSTR value = nullptr; ~Name() { CoTaskMemFree(value); } } name;
                const HRESULT named = description->GetCanonicalName(&name.value);
                size_t units = 0; if (named == S_OK && name.value) while (units < 128 && name.value[units]) ++units;
                std::snprintf(detail, sizeof(detail), "property-name i=%u hr=0x%08lX present=%u units_bounded=%zu", i,
                    static_cast<unsigned long>(named), name.value ? 1u : 0u, units);
                ObserveNativeReason(detail);
                for (size_t offset = 0; offset < units; offset += 8) {
                    const int header = std::snprintf(detail, sizeof(detail), "property-name i=%u offset=%zu utf16=", i, offset);
                    if (header <= 0 || static_cast<size_t>(header) >= sizeof(detail)) break;
                    size_t used = static_cast<size_t>(header);
                    for (size_t n = offset; n < units && n < offset + 8; ++n) {
                        const int written = std::snprintf(detail + used, sizeof(detail) - used, "\\u%04X", static_cast<unsigned>(name.value[n]));
                        if (written != 6) break;
                        used += 6;
                    }
                    ObserveNativeReason(detail);
                }
            }
            if (count > 8) ObserveNativeReason("property-schema truncated_after=8");
        } catch (...) { ObserveNativeReason("property-schema inspection_exception"); }
    }
    template<class F> HRESULT SaveCall(const char* method, Setting setting, bool input_present, DWORD argument, F&& call) {
        if (const auto hr = Mutable(); FAILED(hr)) return hr;
        if (factory_ != DialogFactory::Save) return E_NOINTERFACE;
        ++metadata_epoch_; metadata_seen_ = true; readonly_collection_ = false;
        AddRef(); struct Hold { FileDialog* self; ~Hold() { self->Release(); } } hold{this};
        ComPtr<IFileSaveDialog> save; const auto hr = native_.As(&save);
        if (FAILED(hr)) return hr;
        const HRESULT result = call(save.Get());
        char detail[192]{};
        std::snprintf(detail, sizeof(detail), "save-method method=%s hr=0x%08lX input_present=%u argument=%lu", method,
            static_cast<unsigned long>(result), input_present ? 1u : 0u, argument);
        ObserveNativeReason(detail);
        return Unsupported(setting, result);
    }
    std::atomic<ULONG> references_{1};
    DialogFactory factory_;
    ComPtr<IFileDialog> native_;
    ComPtr<IFileDialog2> native2_;
    FileDialogBridgeServices services_;
    std::unique_ptr<FileDialogInprocessSession> inprocess_;
    bool inprocess_used_ = false;
    bool inprocess_callback_seen_ = false;
    uint32_t result_native_options_ = 0;
    bool allow_missing_result_ = false;
    std::shared_ptr<FileDialogClientState> client_state_;
    std::optional<GUID> client_guid_;
    bool explicit_folder_ = false;
    std::vector<FileDialogPlace> places_;
    FileDialogProperties properties_;
    bool properties_owned_ = false, properties_accepted_ = false, properties_collected_ = false;
    bool readonly_collection_ = false, readonly_properties_result_ = false, metadata_seen_ = false, inspecting_metadata_ = false;
    uint64_t metadata_epoch_ = 0;
    std::vector<FileDialogCustomControl> custom_controls_;
    bool custom_group_ = false;
    bool custom_desynchronized_ = false;
    DWORD custom_group_id_ = 0;
    DWORD thread_;
    std::shared_ptr<detail::FileDialogEventOwner> event_owner_;
    struct Subscription { ComPtr<IFileDialogEvents> proxy; uint64_t order; };
    std::map<DWORD, Subscription> subscriptions_;
    uint64_t subscription_order_ = 0;
    std::atomic<bool> native_only_{false};
    std::atomic_flag notifying_reason_ = ATOMIC_FLAG_INIT;
    struct PendingFolder { uint64_t id; std::wstring path; ComPtr<IShellItem> folder; };
    std::vector<PendingFolder> pending_folders_;
    uint64_t navigation_request_ = 0;
    bool showing_ = false, shown_ = false, bridge_result_ = false, close_requested_ = false;
    HRESULT close_result_ = S_OK;
    wire::PipeHandle cancel_;
    std::vector<SetterCall> calls_;
    ComPtr<IShellItem> item_;
    ComPtr<IShellItemArray> items_;
    std::wstring returned_name_;
    UINT result_index_ = 0;
    size_t result_count_ = 0;
};
}

HRESULT WrapNativeFileDialog(DialogFactory factory, IUnknown* native_object,
                            const FileDialogBridgeServices& services, REFIID iid, void** output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!native_object || (factory != DialogFactory::Open && factory != DialogFactory::Save)) return E_INVALIDARG;
    try {
        APTTYPE apartment{};
        APTTYPEQUALIFIER qualifier{};
        const HRESULT apartment_hr = CoGetApartmentType(&apartment, &qualifier);
        if (FAILED(apartment_hr)) return apartment_hr;
        if (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) return RPC_E_WRONG_THREAD;
        ComPtr<IFileDialog> native;
        // Validate the factory-specific interface before exposing its wrapper.
        if (factory == DialogFactory::Open) {
            ComPtr<IFileOpenDialog> open;
            const HRESULT hr = native_object->QueryInterface(IID_PPV_ARGS(&open));
            if (FAILED(hr)) return hr;
            native = open;
        } else {
            ComPtr<IFileSaveDialog> save;
            const HRESULT hr = native_object->QueryInterface(IID_PPV_ARGS(&save));
            if (FAILED(hr)) return hr;
            native = save;
        }
        ComPtr<IFileDialog2> native2;
        const auto dialog2_hr = native.As(&native2);
        if (dialog2_hr != S_OK && dialog2_hr != E_NOINTERFACE) return FAILED(dialog2_hr) ? dialog2_hr : E_UNEXPECTED;
        if (dialog2_hr == S_OK && !native2) return E_UNEXPECTED;
        auto* wrapper = new FileDialog(factory, std::move(native), std::move(native2), services);
        const HRESULT queried = wrapper->QueryInterface(iid, output);
        wrapper->Release();
        return queried;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}

HRESULT CreatePulseFileDialog(DialogFactory factory, const FileDialogBridgeServices& services, REFIID iid, void** output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (factory != DialogFactory::Open && factory != DialogFactory::Save) return E_INVALIDARG;
    APTTYPE apartment{};
    APTTYPEQUALIFIER qualifier{};
    const HRESULT apartment_hr = CoGetApartmentType(&apartment, &qualifier);
    if (FAILED(apartment_hr)) return apartment_hr;
    if (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) return RPC_E_WRONG_THREAD;
    ComPtr<IFileDialog> native;
    const HRESULT hr = CoCreateInstance(factory == DialogFactory::Save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog,
        nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&native));
    return FAILED(hr) ? hr : WrapNativeFileDialog(factory, native.Get(), services, iid, output);
}
}

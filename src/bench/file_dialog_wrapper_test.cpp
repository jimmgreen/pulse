#include "../shell_bridge/file_dialog_wrapper.h"
#include "../shell_bridge/picker_shell_results.h"
#include <filesystem>
#include <atomic>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <wrl/client.h>

using namespace pulse::shell_bridge;
namespace wire = pulse::ipc::picker;
using Microsoft::WRL::ComPtr;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* name, HRESULT hr = S_OK) {
    ++checks; if (!ok) ++failures;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name;
    if (!ok) std::cout << " HRESULT=" << std::hex << static_cast<unsigned long>(hr) << std::dec;
    std::cout << '\n';
}
struct Counters {
    int native = 0, bridge = 0;
    wire::Status result = wire::Status::Cancelled;
    std::vector<std::u16string> paths;
    std::optional<wire::Request> last; // the request the host received
};
FileDialogBridgeServices Services(Counters& counters, bool verified) {
    FileDialogBridgeServices services;
    if (verified) services.validation_verified = [](const ContractDecision&) { return true; };
    services.show_native = [&](IFileDialog*, HWND) { ++counters.native; return HRESULT_FROM_WIN32(ERROR_CANCELLED); };
    services.run_bridge = [&](const wire::Message& request, HWND, HANDLE) {
        ++counters.bridge;
        counters.last = std::get<wire::Request>(request.body);
        wire::Response response;
        response.operation = std::get<wire::Request>(request.body).operation;
        response.status = counters.result;
        if (response.status == wire::Status::Accepted) response.paths = counters.paths;
        if (response.status == wire::Status::Unsupported) { response.error_source = wire::ErrorSource::Win32; response.error_code = ERROR_NOT_SUPPORTED; }
        return wire::Message{request.request_id, std::move(response)};
    };
    return services;
}
ComPtr<IFileDialog> Make(const FileDialogBridgeServices& services, DialogFactory factory = DialogFactory::Open) {
    ComPtr<IFileDialog> dialog;
    const auto hr = CreatePulseFileDialog(factory, services, IID_PPV_ARGS(&dialog));
    Check(SUCCEEDED(hr) && dialog, "explicit factory creates real-native-backed wrapper", hr);
    return dialog;
}
bool Configure(IFileDialog* dialog, const std::filesystem::path& folder, DialogFactory factory = DialogFactory::Open, bool multi = false) {
    if (!dialog) return false;
    ComPtr<IShellItem> item;
    if (FAILED(CreatePickerShellItem(wire::Operation::PickFolder, folder.wstring(), &item))) return false;
    const auto options = DefaultDialogOptions(factory) | FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT | (multi ? FOS_ALLOWMULTISELECT : 0);
    return SUCCEEDED(dialog->SetOptions(options)) && SUCCEEDED(dialog->SetFolder(item.Get()));
}
// A real native-backed Open object whose IFileDialog2 capability is absent or
// fails. Keeping its own IUnknown identity tests capability negotiation itself.
class RestrictedOpen final : public IFileOpenDialog {
public:
    RestrictedOpen(ComPtr<IFileOpenDialog> native, HRESULT dialog2_result, HRESULT index_result = S_OK)
        : native_(std::move(native)), dialog2_result_(dialog2_result), index_result_(index_result) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid == IID_IFileDialog2) return dialog2_result_;
        if (iid != IID_IUnknown && iid != IID_IModalWindow && iid != IID_IFileDialog && iid != IID_IFileOpenDialog) return E_NOINTERFACE;
        *output = static_cast<IFileOpenDialog*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto value = --references_; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE Show(HWND owner) override { return native_->Show(owner); }
    HRESULT STDMETHODCALLTYPE SetFileTypes(UINT count, const COMDLG_FILTERSPEC* filters) override { return native_->SetFileTypes(count, filters); }
    HRESULT STDMETHODCALLTYPE SetFileTypeIndex(UINT index) override { return native_->SetFileTypeIndex(index); }
    HRESULT STDMETHODCALLTYPE GetFileTypeIndex(UINT* index) override {
        if (index_result_ != S_OK) { if (index) *index = 1; return index_result_; }
        return native_->GetFileTypeIndex(index);
    }
    HRESULT STDMETHODCALLTYPE Advise(IFileDialogEvents* sink, DWORD* cookie) override { return native_->Advise(sink, cookie); }
    HRESULT STDMETHODCALLTYPE Unadvise(DWORD cookie) override { return native_->Unadvise(cookie); }
    HRESULT STDMETHODCALLTYPE SetOptions(FILEOPENDIALOGOPTIONS options) override { return native_->SetOptions(options); }
    HRESULT STDMETHODCALLTYPE GetOptions(FILEOPENDIALOGOPTIONS* options) override { return native_->GetOptions(options); }
    HRESULT STDMETHODCALLTYPE SetDefaultFolder(IShellItem* folder) override { return native_->SetDefaultFolder(folder); }
    HRESULT STDMETHODCALLTYPE SetFolder(IShellItem* folder) override { return native_->SetFolder(folder); }
    HRESULT STDMETHODCALLTYPE GetFolder(IShellItem** folder) override { return native_->GetFolder(folder); }
    HRESULT STDMETHODCALLTYPE GetCurrentSelection(IShellItem** item) override { return native_->GetCurrentSelection(item); }
    HRESULT STDMETHODCALLTYPE SetFileName(LPCWSTR text) override { return native_->SetFileName(text); }
    HRESULT STDMETHODCALLTYPE GetFileName(LPWSTR* text) override { return native_->GetFileName(text); }
    HRESULT STDMETHODCALLTYPE SetTitle(LPCWSTR text) override { return native_->SetTitle(text); }
    HRESULT STDMETHODCALLTYPE SetOkButtonLabel(LPCWSTR text) override { return native_->SetOkButtonLabel(text); }
    HRESULT STDMETHODCALLTYPE SetFileNameLabel(LPCWSTR text) override { return native_->SetFileNameLabel(text); }
    HRESULT STDMETHODCALLTYPE GetResult(IShellItem** item) override { return native_->GetResult(item); }
    HRESULT STDMETHODCALLTYPE AddPlace(IShellItem* item, FDAP location) override { return native_->AddPlace(item, location); }
    HRESULT STDMETHODCALLTYPE SetDefaultExtension(LPCWSTR text) override { return native_->SetDefaultExtension(text); }
    HRESULT STDMETHODCALLTYPE Close(HRESULT result) override { return native_->Close(result); }
    HRESULT STDMETHODCALLTYPE SetClientGuid(REFGUID guid) override { return native_->SetClientGuid(guid); }
    HRESULT STDMETHODCALLTYPE ClearClientData() override { return native_->ClearClientData(); }
    HRESULT STDMETHODCALLTYPE SetFilter(IShellItemFilter* filter) override { return native_->SetFilter(filter); }
    HRESULT STDMETHODCALLTYPE GetResults(IShellItemArray** items) override { return native_->GetResults(items); }
    HRESULT STDMETHODCALLTYPE GetSelectedItems(IShellItemArray** items) override { return native_->GetSelectedItems(items); }
private:
    std::atomic<ULONG> references_{1};
    ComPtr<IFileOpenDialog> native_;
    HRESULT dialog2_result_;
    HRESULT index_result_;
};
std::u16string U16(const std::filesystem::path& path) { const auto w = path.wstring(); return {w.begin(), w.end()}; }
}
int main() {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 2;
    const auto folder = std::filesystem::absolute(L"bench_data") / (L"dialog-wrapper-" + std::to_wstring(GetCurrentProcessId()));
    if (!CreateDirectoryW(folder.c_str(), nullptr)) { CoUninitialize(); return 2; }
    const auto fixture = folder / L"selected.txt";
    std::ofstream(fixture) << "wrapper fixture";
    {
        Counters counts; bool mapped_zero = false;
        auto services = Services(counts, true);
        services.validation_verified = [&](const ContractDecision& decision) {
            mapped_zero = decision.request && std::get<wire::Request>(decision.request->body).filter_index == 0;
            return mapped_zero;
        };
        auto dialog = Make(services);
        const COMDLG_FILTERSPEC all_files{L"All files", L"*.*"};
        Check(Configure(dialog.Get(), folder) && dialog->SetFileTypes(1, &all_files) == S_OK,
            "configure real-native-backed single All files filter");
        const HRESULT set_zero = dialog->SetFileTypeIndex(0);
        UINT observed_index = 0;
        const HRESULT get_index = dialog->GetFileTypeIndex(&observed_index);
        Check(set_zero == S_OK && get_index == S_OK && observed_index == 1,
            "wrapper preserves real native Set0 success and separately observed Get1", set_zero);
        Check(dialog->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && mapped_zero && counts.bridge == 1 && counts.native == 0,
            "real native normalized index maps to zero-based bridge filter");
    }
    for (const HRESULT getter_result : {E_ACCESSDENIED, S_FALSE}) {
        ComPtr<IFileOpenDialog> native;
        const auto created = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&native));
        Check(created == S_OK && native, "create real native getter-failure fixture", created);
        if (!native) continue;
        ComPtr<IFileOpenDialog> restricted; restricted.Attach(new RestrictedOpen(native, E_NOINTERFACE, getter_result));
        Counters counts; ComPtr<IFileDialog> dialog;
        const auto wrapped = WrapNativeFileDialog(DialogFactory::Open, restricted.Get(), Services(counts, true), IID_PPV_ARGS(&dialog));
        const COMDLG_FILTERSPEC all_files{L"All files", L"*.*"};
        Check(wrapped == S_OK && dialog && Configure(dialog.Get(), folder) && dialog->SetFileTypes(1, &all_files) == S_OK,
            "configure failed-observation wrapper fixture", wrapped);
        if (!dialog) continue;
        Check(dialog->SetFileTypeIndex(0) == S_OK, "failed or non-S_OK getter cannot alter original native setter HRESULT");
        Check(dialog->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && counts.native == 1 && counts.bridge == 0,
            "getter output written alongside failure is not evidence for normalizing zero");
    }
    for (const bool verified : {false, true}) {
        Counters counts;
        auto services = Services(counts, verified);
        unsigned observed = 0;
        services.observe_contract = [&](const ContractDecision& decision, bool native_only, bool shown, size_t subscriptions) {
            ++observed;
            Check(decision.disposition == Disposition::Prepared && !native_only && !shown && subscriptions == 0,
                "diagnostic observes original eligibility without changing it");
            throw std::bad_alloc();
        };
        auto dialog = Make(services);
        Check(Configure(dialog.Get(), folder), "configure diagnostic exception regression");
        const auto hr = dialog->Show(nullptr);
        Check(observed == 1 && hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) &&
              counts.bridge == (verified ? 1 : 0) && counts.native == (verified ? 0 : 1),
              "diagnostic exception preserves exactly one chosen Show path", hr);
    }
    {
        Counters counts;
        auto dialog = Make(Services(counts, false));
        ComPtr<IFileOpenDialog> open;
        ComPtr<IModalWindow> modal;
        ComPtr<IUnknown> first, second, third;
        const HRESULT open_hr = dialog.As(&open), modal_hr = dialog.As(&modal);
        dialog.As(&first);
        if (open) open.As(&second);
        if (modal) modal.As(&third);
        Check(SUCCEEDED(open_hr) && SUCCEEDED(modal_hr) && first.Get() == second.Get() && first.Get() == third.Get(), "IUnknown identity is stable across Open/Dialog/Modal");
        ComPtr<IFileSaveDialog> save;
        Check(dialog.As(&save) == E_NOINTERFACE && !save, "Open factory does not expose Save identity");
        ComPtr<IFileDialogCustomize> customize;
        ComPtr<IUnknown> custom_identity;
        const auto custom_hr = dialog.As(&customize);
        if (customize) customize.As(&custom_identity);
        Check(SUCCEEDED(custom_hr) && custom_identity.Get() == first.Get(), "Customize preserves wrapper controlling identity");
        Check(dialog->QueryInterface(IID_IUnknown, nullptr) == E_POINTER, "QI null output rejected");
        const ULONG incremented = dialog->AddRef();
        Check(dialog->Release() + 1 == incremented, "explicit AddRef Release balances wrapper references");
    }
    {
        Counters counts;
        auto dialog = Make(Services(counts, false), DialogFactory::Save);
        ComPtr<IFileSaveDialog> save;
        ComPtr<IUnknown> a, b;
        Check(SUCCEEDED(dialog.As(&save)) && save, "Save factory exposes Save interface");
        dialog.As(&a); save.As(&b);
        Check(a.Get() == b.Get(), "Save interface has same controlling unknown");
        ComPtr<IFileDialogCustomize> customize; ComPtr<IUnknown> custom_identity;
        const auto custom_hr = dialog.As(&customize);
        if (customize) customize.As(&custom_identity);
        Check(SUCCEEDED(custom_hr) && custom_identity.Get() == a.Get(), "Save Customize shares controlling unknown");
        ComPtr<IFileOpenDialog> open;
        Check(dialog.As(&open) == E_NOINTERFACE, "Save factory does not expose Open identity");
    }
    {
        auto token = std::make_shared<int>(1);
        std::weak_ptr<int> weak = token;
        Counters counts;
        auto services = Services(counts, false);
        services.validation_verified = [token](const ContractDecision&) { return false; };
        auto dialog = Make(services);
        services.validation_verified = {}; token.reset();
        Check(!weak.expired(), "wrapper retains service lifetime");
        dialog.Reset();
        Check(weak.expired(), "last Release destroys owned services");
    }
    {
        Counters counts;
        auto dialog = Make(Services(counts, false));
        Check(Configure(dialog.Get(), folder), "configure supported request without validation certification");
        Check(dialog->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && counts.native == 1 && counts.bridge == 0,
            "default validation gate keeps native Show");
    }
    for (const auto factory : {DialogFactory::Open, DialogFactory::Save}) {
        Counters counts;
        auto services = Services(counts, false);
        FILEOPENDIALOGOPTIONS expected_options = 0;
        bool received_contract = false;
        services.validation_verified = [&](const ContractDecision& decision) {
            received_contract = decision.effective_options && *decision.effective_options == expected_options &&
                decision.validation_category == (factory == DialogFactory::Open ? ValidationCategory::DefaultOpen : ValidationCategory::DefaultSave) &&
                decision.required_runtime_checks == (NativeValidationParity | ShellResultContract) &&
                decision.disposition == Disposition::Prepared && decision.request.has_value();
            return false; // Receiving the full context does not certify parity.
        };
        auto dialog = Make(services, factory);
        Check(Configure(dialog.Get(), folder, factory) && SUCCEEDED(dialog->GetOptions(&expected_options)),
            "configure original options for runtime policy context");
        Check(dialog->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && received_contract && counts.native == 1 && counts.bridge == 0,
            "runtime gate receives native options category and obligations and can retain native Show");
    }
    {
        Counters counts;
        auto dialog = Make(Services(counts, true));
        Configure(dialog.Get(), folder);
        Check(SUCCEEDED(dialog->SetTitle(L"Native custom title")), "unsupported title mirrors native setter");
        Check(dialog->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && counts.native == 1 && counts.bridge == 0,
            "unsupported settings fall back before bridge starts");
    }
    {
        Counters counts;
        auto dialog = Make(Services(counts, true));
        Configure(dialog.Get(), folder);
        DWORD cookie = 99;
        ComPtr<IFileDialog> native;
        const auto native_created = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&native));
        DWORD native_cookie = 99;
        const auto expected = SUCCEEDED(native_created) ? native->Advise(nullptr, &native_cookie) : native_created;
        const auto advised = dialog->Advise(nullptr, &cookie);
        Check(SUCCEEDED(native_created) && advised == expected && cookie == native_cookie, "invalid event subscription preserves native HRESULT and cookie semantics");
        Check(dialog->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && counts.native == 1 && counts.bridge == 0,
            "event use prevents custom bridge Show");
    }
    {
        Counters counts;
        auto dialog = Make(Services(counts, true));
        Configure(dialog.Get(), folder);
        const COMDLG_FILTERSPEC filter{L"All", L"*.*"};
        Check(SUCCEEDED(dialog->SetFileTypes(1, &filter)), "first native SetFileTypes succeeds");
        Check(FAILED(dialog->SetFileTypes(1, &filter)), "native duplicate SetFileTypes failure is not hidden");
        Check(dialog->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && counts.native == 1 && counts.bridge == 0,
            "failed setter trace keeps native path");
    }
    {
        Counters counts;
        auto dialog = Make(Services(counts, true));
        Configure(dialog.Get(), folder);
        const auto hr = dialog->Show(nullptr);
        Check(hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) && counts.bridge == 1 && counts.native == 0, "bridge cancellation is terminal with no native replay", hr);
        ComPtr<IShellItem> item;
        Check(FAILED(dialog->GetResult(&item)) && !item, "cancel leaves no successful result");
    }
    {
        Counters counts; counts.result = wire::Status::Unsupported;
        auto dialog = Make(Services(counts, true)); Configure(dialog.Get(), folder);
        Check(dialog->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) && counts.bridge == 1 && counts.native == 0,
            "late host Unsupported cannot reopen native dialog");
    }
    {
        // pulse.exe route: a basic local Save with a caller title crosses the
        // pipe with local-save access and labels, with no in-process provider.
        Counters counts; counts.result = wire::Status::Accepted; counts.paths = {U16(folder / L"bridge-save.txt")};
        auto services = Services(counts, true); services.prefer_bridge = true; services.save_access_policy = AccessPolicy::LocalSaveV1;
        auto dialog = Make(services, DialogFactory::Save); Configure(dialog.Get(), folder, DialogFactory::Save);
        dialog->SetFileName(L"bridge-save"); dialog->SetTitle(L"Export");
        const auto hr = dialog->Show(nullptr);
        const bool carried = counts.last && (counts.last->required_capabilities & wire::LocalSaveAccessV1) &&
            (counts.last->options & wire::ValidateLocalSaveAccessV1) && (counts.last->required_capabilities & wire::DialogLabels) &&
            (counts.last->label_flags & wire::LabelTitle) && counts.last->title == u"Export";
        Check(hr == S_OK && counts.bridge == 1 && counts.native == 0 && carried, "basic local save goes to pulse.exe with access and labels", hr);
        PWSTR name = nullptr;
        Check(SUCCEEDED(dialog->GetFileName(&name)) && name && std::wstring(name) == L"bridge-save.txt", "pulse.exe save result is the dialog result");
        CoTaskMemFree(name);
    }
    {
        // A target the host would decline is checked before the pipe, so the
        // terminal host answer is never reached: without a provider, native.
        Counters counts; counts.result = wire::Status::Accepted; counts.paths = {U16(folder / L"x.txt")};
        auto services = Services(counts, true); services.prefer_bridge = true; services.save_access_policy = AccessPolicy::LocalSaveV1;
        auto dialog = Make(services, DialogFactory::Save); Configure(dialog.Get(), folder, DialogFactory::Save);
        dialog->SetFileName(L"CON");
        dialog->Show(nullptr);
        Check(counts.bridge == 0 && counts.native == 1, "unresolvable save target stays off the pipe");
    }
    {
        // Without the preference a local Save keeps its previous route.
        Counters counts; counts.result = wire::Status::Accepted; counts.paths = {U16(folder / L"bridge-save.txt")};
        auto services = Services(counts, true); services.save_access_policy = AccessPolicy::LocalSaveV1;
        auto dialog = Make(services, DialogFactory::Save); Configure(dialog.Get(), folder, DialogFactory::Save);
        dialog->SetFileName(L"bridge-save");
        dialog->Show(nullptr);
        Check(counts.bridge == 0 && counts.native == 1, "local save without pulse.exe preference is unchanged");
    }
    {
        // Deferred provider: a basic pulse.exe Show never loads the in-process UI.
        Counters counts; counts.result = wire::Status::Accepted; counts.paths = {U16(folder / L"bridge-save.txt")};
        int loads = 0;
        auto services = Services(counts, true); services.prefer_bridge = true; services.save_access_policy = AccessPolicy::LocalSaveV1;
        services.load_inprocess_provider = [&loads] { ++loads; return std::shared_ptr<const FileDialogInprocessProvider>{}; };
        auto dialog = Make(services, DialogFactory::Save); Configure(dialog.Get(), folder, DialogFactory::Save);
        dialog->SetFileName(L"bridge-save");
        const auto hr = dialog->Show(nullptr);
        Check(hr == S_OK && counts.bridge == 1 && loads == 0, "basic pulse.exe dialog never loads the in-process provider", hr);
    }
    {
        // A dialog the host cannot carry (a captured place) loads the provider
        // once at Show; an unavailable provider keeps that Show native.
        Counters counts; int loads = 0;
        auto services = Services(counts, true); services.prefer_bridge = true;
        services.load_inprocess_provider = [&loads] { ++loads; return std::shared_ptr<const FileDialogInprocessProvider>{}; };
        auto dialog = Make(services); Configure(dialog.Get(), folder);
        ComPtr<IShellItem> place;
        const bool placed = SUCCEEDED(CreatePickerShellItem(wire::Operation::PickFolder, folder.wstring(), &place)) &&
            SUCCEEDED(dialog->AddPlace(place.Get(), FDAP_TOP));
        dialog->Show(nullptr);
        Check(placed && loads == 1 && counts.bridge == 0 && counts.native == 1, "non-basic dialog loads the provider on demand, native when unavailable");
    }
    {
        // A dialog already bound to native never loads the in-process UI.
        Counters counts; int loads = 0;
        auto services = Services(counts, true); services.prefer_bridge = true;
        services.load_inprocess_provider = [&loads] { ++loads; return std::shared_ptr<const FileDialogInprocessProvider>{}; };
        auto dialog = Make(services);
        dialog->SetOptions(DefaultDialogOptions(DialogFactory::Open) | FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT | FOS_ALLNONSTORAGEITEMS);
        dialog->Show(nullptr);
        Check(loads <= 1 && counts.native == 1 && counts.bridge == 0, "unsupported native options stay native");
    }
    {
        Counters counts; counts.result = wire::Status::Accepted; counts.paths = {U16(fixture)};
        auto dialog = Make(Services(counts, true)); Configure(dialog.Get(), folder);
        const auto hr = dialog->Show(nullptr);
        Check(SUCCEEDED(hr) && counts.bridge == 1 && counts.native == 0, "accepted bridge returns real Shell result", hr);
        ComPtr<IShellItem> item;
        Check(SUCCEEDED(dialog->GetResult(&item)) && item, "GetResult owns valid Shell item");
        PWSTR name = nullptr;
        Check(SUCCEEDED(dialog->GetFileName(&name)) && name && std::wstring(name) == L"selected.txt", "post-bridge filename comes from accepted result");
        CoTaskMemFree(name);
        ComPtr<IFileOpenDialog> open; dialog.As(&open);
        ComPtr<IShellItemArray> items; DWORD count = 0;
        const auto results_hr = open->GetResults(&items);
        Check(SUCCEEDED(results_hr) && items && SUCCEEDED(items->GetCount(&count)) && count == 1, "Open GetResults owns Shell array");
    }
    {
        Counters counts; counts.result = wire::Status::Accepted; counts.paths = {U16(fixture), U16(fixture)};
        auto dialog = Make(Services(counts, true)); Configure(dialog.Get(), folder, DialogFactory::Open, true);
        Check(SUCCEEDED(dialog->Show(nullptr)), "multi-selection result built");
        ComPtr<IShellItem> item;
        Check(FAILED(dialog->GetResult(&item)) && !item, "multi-selection does not misreport first item as single result");
    }
    {
        Counters counts; counts.result = wire::Status::Accepted; counts.paths = {U16(folder / L"missing.txt")};
        auto dialog = Make(Services(counts, true)); Configure(dialog.Get(), folder);
        const auto hr = dialog->Show(nullptr);
        Check(FAILED(hr) && counts.bridge == 1 && counts.native == 0, "result materialization failure never repeats Show", hr);
        ComPtr<IShellItem> item;
        Check(FAILED(dialog->GetResult(&item)) && !item, "failed result build cannot expose partial success");
    }
    {
        Counters counts; counts.result = wire::Status::Accepted; counts.paths = {U16(folder / L"new-save.txt")};
        auto dialog = Make(Services(counts, true), DialogFactory::Save); Configure(dialog.Get(), folder, DialogFactory::Save);
        dialog->SetFileName(L"new-save.txt");
        const auto hr = dialog->Show(nullptr);
        Check(SUCCEEDED(hr) && !std::filesystem::exists(folder / L"new-save.txt") && counts.native == 0,
            "Save result builds without creating file or native replay", hr);
    }
    {
        Counters counts;
        auto services = Services(counts, true);
        IFileDialog* active = nullptr;
        services.run_bridge = [&](const wire::Message& request, HWND, HANDLE cancel) {
            ++counts.bridge;
            Check(active->SetFileName(L"late.txt") == E_UNEXPECTED, "setter during bridge cannot silently mutate only native state");
            ComPtr<IFileDialog2> active2; active->QueryInterface(IID_PPV_ARGS(&active2));
            Check(active2 && active2->SetCancelButtonLabel(L"Late") == E_UNEXPECTED && active2->SetNavigationRoot(nullptr) == E_UNEXPECTED,
                "IFileDialog2 setters during bridge cannot silently change native-only state");
            Check(active->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_BUSY), "reentrant Show rejected");
            Check(active->Close(E_ABORT) == S_OK && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0, "Close signals active bridge cancellation");
            wire::Response response; response.status = wire::Status::Cancelled;
            return wire::Message{request.request_id, response};
        };
        auto dialog = Make(services); active = dialog.Get(); Configure(active, folder);
        Check(dialog->Show(nullptr) == E_ABORT && counts.bridge == 1 && counts.native == 0, "Close HRESULT preserved without replay");
        HRESULT wrong_thread = S_OK;
        std::thread worker([&] { wrong_thread = dialog->SetFileName(L"cross-thread.txt"); }); worker.join();
        Check(wrong_thread == RPC_E_WRONG_THREAD, "STA-affine setter rejects cross-thread use");
    }
    for (auto factory : {DialogFactory::Open, DialogFactory::Save}) {
        Counters counts;
        auto dialog = Make(Services(counts, true), factory);
        Check(Configure(dialog.Get(), folder, factory), "configure IFileDialog2 identity fixture");
        ComPtr<IFileDialog2> dialog2, native2; ComPtr<IFileDialog> native;
        CoCreateInstance(factory == DialogFactory::Open ? CLSID_FileOpenDialog : CLSID_FileSaveDialog,
            nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&native));
        const auto native_hr = native.As(&native2), wrapper_hr = dialog.As(&dialog2);
        ComPtr<IUnknown> first, second; dialog.As(&first); if (dialog2) dialog2.As(&second);
        Check(wrapper_hr == native_hr && SUCCEEDED(wrapper_hr) && dialog2 && first.Get() == second.Get(),
            "Open and Save IFileDialog2 capability matches native and preserves controlling identity", wrapper_hr);
        if (!dialog2 || !native2) continue;
        FILEOPENDIALOGOPTIONS base_options = 0;
        Check(SUCCEEDED(dialog2->GetOptions(&base_options)), "IFileDialog2 inherited methods dispatch through wrapper");
        const auto label_hr = dialog2->SetCancelButtonLabel(L"Leave");
        Check(label_hr == native2->SetCancelButtonLabel(L"Leave"), "Cancel label preserves native HRESULT");
        Check(dialog2->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && counts.native == 1 && counts.bridge == 0,
            "Cancel label customization forces native before Show");
        HRESULT thread_hr = S_OK;
        std::thread worker([&] { thread_hr = dialog2->SetCancelButtonLabel(L"Wrong thread"); }); worker.join();
        Check(thread_hr == RPC_E_WRONG_THREAD, "IFileDialog2 setter enforces creating STA");
    }
    {
        Counters counts;
        auto dialog = Make(Services(counts, true)); Configure(dialog.Get(), folder);
        ComPtr<IFileDialog2> dialog2, native2; dialog.As(&dialog2);
        CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&native2));
        ComPtr<IShellItem> root; CreatePickerShellItem(wire::Operation::PickFolder, folder.wstring(), &root);
        const auto root_hr = dialog2 ? dialog2->SetNavigationRoot(root.Get()) : E_NOINTERFACE;
        Check(dialog2 && native2 && root && root_hr == native2->SetNavigationRoot(root.Get()), "navigation root preserves native HRESULT and Shell item");
        Check(dialog->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && counts.native == 1 && counts.bridge == 0,
            "restricted navigation root is never silently lost to Pulse bridge");
    }
    {
        Counters counts;
        auto dialog = Make(Services(counts, true)); Configure(dialog.Get(), folder);
        ComPtr<IFileDialog2> dialog2; dialog.As(&dialog2);
        Check(dialog2 && dialog2->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && counts.bridge == 1 && counts.native == 0,
            "querying IFileDialog2 alone preserves verified base dialog bridge behavior");
        Check(dialog2 && dialog2->SetCancelButtonLabel(L"Late") == E_UNEXPECTED && dialog2->SetNavigationRoot(nullptr) == E_UNEXPECTED,
            "IFileDialog2 settings after bridge cannot mutate unrelated native dialog");
    }
    for (const auto capability_hr : {E_NOINTERFACE, E_OUTOFMEMORY}) {
        ComPtr<IFileOpenDialog> native; CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&native));
        ComPtr<IFileOpenDialog> restricted; restricted.Attach(new RestrictedOpen(native, capability_hr));
        Counters counts; ComPtr<IFileDialog> dialog;
        const auto hr = WrapNativeFileDialog(DialogFactory::Open, restricted.Get(), Services(counts, false), IID_PPV_ARGS(&dialog));
        if (capability_hr == E_NOINTERFACE) {
            ComPtr<IFileDialog2> dialog2;
            Check(hr == S_OK && dialog && dialog.As(&dialog2) == E_NOINTERFACE && !dialog2,
                "wrapper does not invent IFileDialog2 when native object lacks capability");
        } else {
            FILEOPENDIALOGOPTIONS options = 0;
            Check(hr == capability_hr && !dialog && SUCCEEDED(restricted->GetOptions(&options)),
                "native capability query error aborts wrapping and preserves caller native object");
        }
    }
    {
        Counters counts;
        auto services = Services(counts, false);
        services.access_policy = AccessPolicy::LocalOpenV1;
        bool checked = false;
        services.validation_verified = [&](const ContractDecision& decision) {
            checked = decision.request && decision.effective_options == DefaultDialogOptions(DialogFactory::Open) &&
                decision.supported_access_policy == AccessPolicy::LocalOpenV1 &&
                decision.validation_category == ValidationCategory::LocalOpenAccessV1 &&
                decision.required_runtime_checks == (SupportedAccessPolicy | ShellResultContract) &&
                !std::get<wire::Request>(decision.request->body).initial_directory.empty();
            return checked;
        };
        auto dialog = Make(services);
        Check(dialog && dialog->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && checked && counts.bridge == 1 && counts.native == 0,
            "opted-in default Open resolves native starting folder before bridge without SetFolder");
        ComPtr<IShellItem> cancelled_item;
        Check(dialog && FAILED(dialog->GetResult(&cancelled_item)) && !cancelled_item, "default Open cancelled bridge has no result");
    }
    {
        Counters counts;
        auto services = Services(counts, false); services.access_policy = AccessPolicy::LocalOpenV1;
        auto dialog = Make(services);
        Check(dialog && dialog->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && counts.native == 1 && counts.bridge == 0,
            "local policy opt-in without verifying provider remains native");
    }
    {
        const auto initial = folder / L"initial";
        const bool created = CreateDirectoryW(initial.c_str(), nullptr) != FALSE;
        Check(created, "create owned initial directory distinct from accepted file parent");
        Counters counts; counts.result = wire::Status::Accepted; counts.paths = {U16(fixture)};
        auto services = Services(counts, false); services.access_policy = AccessPolicy::LocalOpenV1;
        services.validation_verified = [&](const ContractDecision& decision) {
            return decision.request && decision.supported_access_policy == AccessPolicy::LocalOpenV1 &&
                decision.required_runtime_checks == (SupportedAccessPolicy | ShellResultContract) &&
                decision.effective_options == (DefaultDialogOptions(DialogFactory::Open) | FOS_DONTADDTORECENT) &&
                std::get<wire::Request>(decision.request->body).initial_directory == U16(initial);
        };
        auto dialog = Make(services);
        ComPtr<IShellItem> initial_item, accepted_folder;
        Check(created && SUCCEEDED(CreatePickerShellItem(wire::Operation::PickFolder, initial.wstring(), &initial_item)) &&
            SUCCEEDED(dialog->SetFolder(initial_item.Get())) &&
            SUCCEEDED(dialog->SetOptions(DefaultDialogOptions(DialogFactory::Open) | FOS_DONTADDTORECENT)),
            "configure owned initial folder with Recent explicitly suppressed");
        Check(dialog && dialog->Show(nullptr) == S_OK && counts.bridge == 1 && counts.native == 0,
            "local bridge accepts result from a different directory");
        PWSTR returned_folder = nullptr;
        const bool matches = dialog && SUCCEEDED(dialog->GetFolder(&accepted_folder)) && accepted_folder &&
            SUCCEEDED(accepted_folder->GetDisplayName(SIGDN_FILESYSPATH, &returned_folder)) &&
            CompareStringOrdinal(returned_folder, -1, folder.c_str(), -1, TRUE) == CSTR_EQUAL;
        Check(matches, "bridge GetFolder returns accepted file parent rather than native initial folder");
        CoTaskMemFree(returned_folder);
        Check(created && RemoveDirectoryW(initial.c_str()) != FALSE, "owned initial directory cleaned");
    }
    Check(DeleteFileW(fixture.c_str()) != FALSE && RemoveDirectoryW(folder.c_str()) != FALSE, "wrapper fixtures cleaned up");
    CoUninitialize();
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures ? 1 : 0;
}

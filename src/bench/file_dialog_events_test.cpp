#include "../shell_bridge/file_dialog_events.h"
#include "../shell_bridge/file_dialog_wrapper.h"

#include <atomic>
#include <functional>
#include <filesystem>
#include <iostream>
#include <thread>
#include <wrl/client.h>

using namespace pulse::shell_bridge;
using Microsoft::WRL::ComPtr;
namespace {
int checks = 0, failures = 0;
std::atomic<const char*> current_stage{"startup"};
void Stage(const char* name) {
    current_stage = name;
    std::cout << "[STAGE] " << name << std::endl;
}
struct Watchdog {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread worker;
    bool Start() {
        if (!done) return false;
        try {
            worker = std::thread([this] {
                if (WaitForSingleObject(done, 40000) == WAIT_OBJECT_0) return;
                std::cerr << "[FAIL] events-test watchdog expired at stage: " << current_stage.load() << std::endl;
                TerminateProcess(GetCurrentProcess(), 99); // Only this isolated test process.
            });
            return true;
        } catch (...) { return false; }
    }
    ~Watchdog() {
        if (done) SetEvent(done);
        if (worker.joinable()) worker.join();
        if (done) CloseHandle(done);
    }
};
void Check(bool ok, const char* name) {
    ++checks; if (!ok) ++failures;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
}
struct State {
    IUnknown* identity = nullptr; // Comparison only; callbacks own their argument.
    int calls = 0, destroyed = 0;
    bool identities = true;
    bool controls = false;
    DWORD control_id = 0, item_id = 0;
    BOOL checked = FALSE;
    int control_calls = 0;
    std::function<HRESULT(IFileDialogCustomize*)> on_control;
    std::function<HRESULT(IFileDialog*)> on_ok;
    std::function<void()> on_destroy;
};
class Sink final : public IFileDialogEvents, public IFileDialogControlEvents {
public:
    explicit Sink(std::shared_ptr<State> state) : state_(std::move(state)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid == IID_IUnknown || iid == IID_IFileDialogEvents) *output = static_cast<IFileDialogEvents*>(this);
        else if (iid == IID_IFileDialogControlEvents && state_->controls) *output = static_cast<IFileDialogControlEvents*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        Observe(dialog); return state_->on_ok ? state_->on_ok(dialog) : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog* dialog, IShellItem*) override { Observe(dialog); return E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog* dialog) override { Observe(dialog); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog* dialog) override { Observe(dialog); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog* dialog, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* response) override {
        Observe(dialog); *response = FDESVR_ACCEPT; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog* dialog) override { Observe(dialog); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog* dialog, IShellItem*, FDE_OVERWRITE_RESPONSE* response) override {
        Observe(dialog); *response = FDEOR_REFUSE; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnItemSelected(IFileDialogCustomize* dialog, DWORD id, DWORD item) override {
        state_->item_id = item; return ObserveControl(dialog, id);
    }
    HRESULT STDMETHODCALLTYPE OnButtonClicked(IFileDialogCustomize* dialog, DWORD id) override { return ObserveControl(dialog, id); }
    HRESULT STDMETHODCALLTYPE OnCheckButtonToggled(IFileDialogCustomize* dialog, DWORD id, BOOL checked) override {
        state_->checked = checked; return ObserveControl(dialog, id);
    }
    HRESULT STDMETHODCALLTYPE OnControlActivating(IFileDialogCustomize* dialog, DWORD id) override { return ObserveControl(dialog, id); }
private:
    HRESULT ObserveControl(IFileDialogCustomize* dialog, DWORD id) {
        ++state_->control_calls; state_->control_id = id;
        ComPtr<IUnknown> identity;
        state_->identities = state_->identities && dialog && SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&identity))) &&
            identity.Get() == state_->identity;
        return state_->on_control ? state_->on_control(dialog) : E_ACCESSDENIED;
    }
    ~Sink() { ++state_->destroyed; if (state_->on_destroy) state_->on_destroy(); }
    void Observe(IFileDialog* dialog) {
        ++state_->calls;
        ComPtr<IUnknown> identity;
        state_->identities = state_->identities && dialog && SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&identity))) &&
            identity.Get() == state_->identity;
    }
    std::atomic<ULONG> references_{1};
    std::shared_ptr<State> state_;
};
ComPtr<IFileDialogEvents> MakeSink(const std::shared_ptr<State>& state) {
    ComPtr<IFileDialogEvents> sink; sink.Attach(new Sink(state)); return sink;
}
ComPtr<IFileDialog> Wrapper(int& native_shows, int* bridge_shows = nullptr) {
    FileDialogBridgeServices services;
    services.validation_verified = [](const auto&) { return true; };
    services.show_native = [&](IFileDialog*, HWND) { ++native_shows; return HRESULT_FROM_WIN32(ERROR_CANCELLED); };
    services.run_bridge = [bridge_shows](const auto& request, HWND, HANDLE) -> pulse::ipc::picker::Message {
        if (bridge_shows) ++*bridge_shows;
        pulse::ipc::picker::Response response;
        response.operation = std::get<pulse::ipc::picker::Request>(request.body).operation;
        response.status = pulse::ipc::picker::Status::Cancelled;
        return {request.request_id, response};
    };
    ComPtr<IFileDialog> wrapper;
    Stage("factory CreatePulseFileDialog");
    Check(SUCCEEDED(CreatePulseFileDialog(DialogFactory::Open, services, IID_PPV_ARGS(&wrapper))), "create event-capable wrapper");
    ComPtr<IShellItem> folder;
    const auto path = std::filesystem::absolute(L"bench_data").wstring();
    Stage("configure native folder and options");
    const HRESULT folder_hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&folder));
    Check(wrapper && SUCCEEDED(folder_hr) &&
        SUCCEEDED(wrapper->SetOptions(DefaultDialogOptions(DialogFactory::Open) | FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT)) &&
        SUCCEEDED(wrapper->SetFolder(folder.Get())), "configure otherwise bridge-eligible fixture before events");
    return wrapper;
}
void SetIdentity(const std::shared_ptr<State>& state, IFileDialog* dialog) {
    ComPtr<IUnknown> identity; dialog->QueryInterface(IID_PPV_ARGS(&identity)); state->identity = identity.Get();
}
}
int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    Watchdog watchdog;
    if (!watchdog.Start()) { std::cerr << "[FAIL] cannot start events-test watchdog\n"; return 2; }
    Stage("CoInitializeEx");
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 2;
    {
        int native_shows = 0, bridge_shows = 0;
        auto wrapper = Wrapper(native_shows, &bridge_shows);
        Stage("reject unknown cookie before any subscription");
        Check(wrapper->Unadvise(MAXDWORD) == E_INVALIDARG, "unregistered cookie rejected locally");
        Check(wrapper->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && bridge_shows == 1 && native_shows == 0,
            "invalid Unadvise does not mutate routing eligibility");
    }
    {
        int native_shows = 0;
        auto wrapper = Wrapper(native_shows);
        auto state = std::make_shared<State>(); SetIdentity(state, wrapper.Get());
        auto sink = MakeSink(state);
        auto owner = std::make_shared<detail::FileDialogEventOwner>(); owner->dialog = wrapper.Get();
        ComPtr<IFileDialogEvents> proxy;
        Stage("create standalone event proxy");
        Check(SUCCEEDED(detail::CreateFileDialogEventProxy(owner, sink.Get(), &proxy)), "create identity-rewriting event proxy");
        ComPtr<IFileDialog> inner;
        Stage("CoCreateInstance standalone inner dialog");
        CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&inner));
        Stage("proxy OnFileOk");
        Check(proxy->OnFileOk(inner.Get()) == S_FALSE, "OnFileOk veto preserved");
        Stage("proxy OnFolderChanging");
        Check(proxy->OnFolderChanging(inner.Get(), nullptr) == E_ACCESSDENIED, "OnFolderChanging HRESULT preserved");
        Stage("proxy OnFolderChange");
        Check(proxy->OnFolderChange(inner.Get()) == S_OK, "OnFolderChange forwarded");
        Stage("proxy OnSelectionChange");
        Check(proxy->OnSelectionChange(inner.Get()) == S_OK, "OnSelectionChange forwarded");
        FDE_SHAREVIOLATION_RESPONSE share = FDESVR_DEFAULT;
        Stage("proxy OnShareViolation");
        Check(proxy->OnShareViolation(inner.Get(), nullptr, &share) == S_OK && share == FDESVR_ACCEPT, "share decision forwarded");
        Stage("proxy OnTypeChange");
        Check(proxy->OnTypeChange(inner.Get()) == S_OK, "OnTypeChange forwarded");
        FDE_OVERWRITE_RESPONSE overwrite = FDEOR_DEFAULT;
        Stage("proxy OnOverwrite");
        Check(proxy->OnOverwrite(inner.Get(), nullptr, &overwrite) == S_OK && overwrite == FDEOR_REFUSE, "overwrite decision forwarded");
        Check(state->calls == 7 && state->identities, "all seven callbacks expose wrapper controlling unknown never inner identity");
        Check(proxy->OnOverwrite(inner.Get(), nullptr, nullptr) == E_POINTER, "null callback output rejected");
        HRESULT cross_thread = S_OK;
        Stage("wrong-thread callback and join");
        std::thread other([&] { cross_thread = proxy->OnTypeChange(inner.Get()); }); other.join();
        Check(cross_thread == RPC_E_WRONG_THREAD && state->calls == 7, "callback not dispatched across apartment thread");
        {
            Stage("disconnect standalone owner control");
            std::lock_guard lock(owner->mutex); owner->dialog = nullptr;
        }
        share = FDESVR_ACCEPT;
        Check(proxy->OnShareViolation(inner.Get(), nullptr, &share) == CO_E_OBJNOTCONNECTED && share == FDESVR_DEFAULT && state->calls == 7,
            "disconnected owner rejects late callbacks with initialized output");
        Stage("release standalone proxy and sink");
        proxy.Reset(); sink.Reset();
        Check(state->destroyed == 1, "disconnected proxy releases caller sink");
        Stage("first scope destructor cleanup");
    }
    {
        int native_shows = 0;
        auto wrapper = Wrapper(native_shows);
        auto state = std::make_shared<State>(); SetIdentity(state, wrapper.Get());
        auto sink = MakeSink(state);
        DWORD cookie = MAXDWORD;
        Stage("wrapper Advise real native subscription");
        Check(SUCCEEDED(wrapper->Advise(sink.Get(), &cookie)), "wrapper registers proxy with real native Advise");
        Stage("release external sink after Advise");
        sink.Reset();
        Check(state->destroyed == 0, "native subscription retains sink");
        const DWORD unknown_cookie = cookie == MAXDWORD ? MAXDWORD - 1 : MAXDWORD;
        Stage("reject unknown cookie while valid subscription remains active");
        Check(wrapper->Unadvise(unknown_cookie) == E_INVALIDARG && state->destroyed == 0,
            "unknown cookie neither unregisters nor releases an active sink");
        Stage("wrapper Show native seam");
        Check(wrapper->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && native_shows == 1, "events still force pre-Show native route");
        Stage("wrapper Unadvise valid cookie");
        Check(SUCCEEDED(wrapper->Unadvise(cookie)), "real native cookie unregisters");
        Check(state->destroyed == 1, "Unadvise releases both native and wrapper proxy ownership");
        Stage("reject already-unregistered cookie");
        Check(wrapper->Unadvise(cookie) == E_INVALIDARG && state->destroyed == 1,
            "repeated Unadvise fails without a second sink release");
        Stage("reject unknown cookie after subscription cleanup");
        Check(wrapper->Unadvise(unknown_cookie) == E_INVALIDARG && state->destroyed == 1,
            "unknown cookie after cleanup is local failure without side effects");
        Stage("subscription scope destructor cleanup");
    }
    {
        int native_shows = 0;
        auto wrapper = Wrapper(native_shows);
        auto state = std::make_shared<State>(); SetIdentity(state, wrapper.Get());
        auto sink = MakeSink(state); DWORD cookie = 0;
        Stage("Advise before final owner release");
        Check(SUCCEEDED(wrapper->Advise(sink.Get(), &cookie)), "register active subscription before owner release");
        Stage("release external sink before final owner release");
        sink.Reset();
        Stage("final owner Release with active native subscription");
        wrapper.Reset();
        Check(state->destroyed == 1, "final owner Release breaks proxy ownership without Unadvise by caller");
    }
    {
        int native_shows = 0;
        auto wrapper = Wrapper(native_shows);
        auto state = std::make_shared<State>(); SetIdentity(state, wrapper.Get());
        auto sink = MakeSink(state); DWORD cookie = 0;
        Stage("Advise before reentrant callback");
        Check(SUCCEEDED(wrapper->Advise(sink.Get(), &cookie)), "register callback reentry subscription");
        auto owner = std::make_shared<detail::FileDialogEventOwner>(); owner->dialog = wrapper.Get();
        ComPtr<IFileDialogEvents> proxy;
        detail::CreateFileDialogEventProxy(owner, sink.Get(), &proxy);
        bool unadvised = false, queried_after_release = false;
        state->on_ok = [&](IFileDialog* borrowed) {
            Stage("callback calls Unadvise");
            unadvised = SUCCEEDED(borrowed->Unadvise(cookie));
            Stage("callback releases external wrapper");
            wrapper.Reset();
            Stage("callback releases external proxy");
            proxy.Reset();
            FILEOPENDIALOGOPTIONS flags = 0;
            queried_after_release = SUCCEEDED(borrowed->GetOptions(&flags));
            return S_FALSE;
        };
        sink.Reset();
        IFileDialogEvents* invoked = proxy.Get();
        Stage("invoke callback with Unadvise and Release reentry");
        Check(invoked->OnFileOk(nullptr) == S_FALSE && unadvised && queried_after_release && state->identities,
            "callback may Unadvise and release external owner and proxy while borrowed identity remains alive");
        { std::lock_guard lock(owner->mutex); owner->dialog = nullptr; }
        Check(state->destroyed == 1 && !wrapper && !proxy, "callback temporary references unwind without cycles");
    }
    {
        Stage("customization native mirror and identity");
        int native_shows = 0, bridge_shows = 0;
        auto wrapper = Wrapper(native_shows, &bridge_shows);
        ComPtr<IFileDialogCustomize> customize, native_customize;
        ComPtr<IFileOpenDialog> native;
        Check(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&native))) &&
            SUCCEEDED(native.As(&native_customize)) && SUCCEEDED(wrapper.As(&customize)), "obtain wrapper and native customization interfaces");
        ComPtr<IUnknown> root, control_root; wrapper.As(&root); customize.As(&control_root);
        Check(root.Get() == control_root.Get(), "customize controlling unknown is wrapper identity");
        Check(customize->AddComboBox(100) == native_customize->AddComboBox(100), "combo creation mirrors native HRESULT");
        Check(customize->AddControlItem(100, 7, L"UTF-8") == native_customize->AddControlItem(100, 7, L"UTF-8"), "combo item addition mirrors native HRESULT");
        Check(customize->SetSelectedControlItem(100, 7) == native_customize->SetSelectedControlItem(100, 7), "combo selection mirrors native HRESULT");
        DWORD actual = 0, expected = 0;
        const HRESULT actual_hr = customize->GetSelectedControlItem(100, &actual);
        const HRESULT expected_hr = native_customize->GetSelectedControlItem(100, &expected);
        Check(SUCCEEDED(actual_hr) && actual_hr == expected_hr && actual == expected && actual == 7, "combo selected item returns native value");
        Check(customize->SetControlState(100, CDCS_VISIBLE) == native_customize->SetControlState(100, CDCS_VISIBLE), "control state mirrors native HRESULT");
        CDCONTROLSTATEF actual_state = CDCS_INACTIVE, expected_state = CDCS_INACTIVE;
        const auto state_hr = customize->GetControlState(100, &actual_state);
        const auto native_state_hr = native_customize->GetControlState(100, &expected_state);
        Check(SUCCEEDED(state_hr) && state_hr == native_state_hr && actual_state == expected_state, "control state getter mirrors native state");
        Check(customize->SetControlLabel(999, L"missing") == native_customize->SetControlLabel(999, L"missing"), "invalid control ID preserves native HRESULT");
        HRESULT thread_hr = S_OK;
        std::thread wrong_thread([&] { thread_hr = customize->AddComboBox(200); }); wrong_thread.join();
        Check(thread_hr == RPC_E_WRONG_THREAD, "customization enforces owner apartment thread");
        Check(wrapper->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && native_shows == 1 && bridge_shows == 0,
            "customization forces native Show without opening bridge gate");
    }
    {
        Stage("conditional control event QI and all callbacks");
        int native_shows = 0;
        auto wrapper = Wrapper(native_shows);
        auto owner = std::make_shared<detail::FileDialogEventOwner>(); owner->dialog = wrapper.Get();
        auto state = std::make_shared<State>(); SetIdentity(state, wrapper.Get());
        auto sink = MakeSink(state);
        ComPtr<IFileDialogEvents> proxy;
        Check(SUCCEEDED(detail::CreateFileDialogEventProxy(owner, sink.Get(), &proxy)), "create plain event proxy");
        ComPtr<IFileDialogControlEvents> controls;
        Check(proxy.As(&controls) == E_NOINTERFACE && !controls, "proxy does not invent unsupported control event interface");
        proxy.Reset(); sink.Reset();
        state = std::make_shared<State>(); state->controls = true; SetIdentity(state, wrapper.Get()); sink = MakeSink(state);
        Check(SUCCEEDED(detail::CreateFileDialogEventProxy(owner, sink.Get(), &proxy)) && SUCCEEDED(proxy.As(&controls)), "proxy exposes caller-supported control events");
        ComPtr<IUnknown> a, b; proxy.As(&a); controls.As(&b);
        Check(a.Get() == b.Get(), "event and control event proxy share controlling unknown");
        a.Reset(); b.Reset();
        Check(controls->OnItemSelected(nullptr, 101, 77) == E_ACCESSDENIED && state->control_id == 101 && state->item_id == 77, "selection callback preserves IDs and HRESULT");
        Check(controls->OnButtonClicked(nullptr, 102) == E_ACCESSDENIED && state->control_id == 102, "button callback preserves ID and HRESULT");
        Check(controls->OnCheckButtonToggled(nullptr, 103, TRUE) == E_ACCESSDENIED && state->control_id == 103 && state->checked == TRUE, "check callback preserves ID bool and HRESULT");
        Check(controls->OnControlActivating(nullptr, 104) == E_ACCESSDENIED && state->control_id == 104 && state->control_calls == 4 && state->identities,
            "all four control callbacks expose wrapper Customize identity");
        HRESULT thread_hr = S_OK;
        std::thread wrong_thread([&] { thread_hr = controls->OnButtonClicked(nullptr, 105); }); wrong_thread.join();
        Check(thread_hr == RPC_E_WRONG_THREAD && state->control_calls == 4, "control events reject wrong thread without caller callback");
        { std::lock_guard lock(owner->mutex); owner->dialog = nullptr; }
        Check(controls->OnButtonClicked(nullptr, 105) == CO_E_OBJNOTCONNECTED && state->control_calls == 4, "late control callback rejects disconnected owner");
        sink.Reset(); proxy.Reset(); controls.Reset();
        Check(state->destroyed == 1, "both event interfaces release caller sink without cycle");
    }
    {
        Stage("control callback Unadvise and Release reentry");
        int native_shows = 0;
        auto wrapper = Wrapper(native_shows);
        auto state = std::make_shared<State>(); state->controls = true; SetIdentity(state, wrapper.Get());
        auto sink = MakeSink(state); DWORD cookie = 0;
        Check(SUCCEEDED(wrapper->Advise(sink.Get(), &cookie)), "register control-capable sink with native dialog");
        auto owner = std::make_shared<detail::FileDialogEventOwner>(); owner->dialog = wrapper.Get();
        ComPtr<IFileDialogEvents> proxy; ComPtr<IFileDialogControlEvents> controls;
        detail::CreateFileDialogEventProxy(owner, sink.Get(), &proxy); proxy.As(&controls);
        bool unadvised = false, borrowed_alive = false;
        state->on_control = [&](IFileDialogCustomize* borrowed) {
            ComPtr<IFileDialog> dialog; borrowed->QueryInterface(IID_PPV_ARGS(&dialog));
            unadvised = dialog && SUCCEEDED(dialog->Unadvise(cookie));
            wrapper.Reset(); proxy.Reset(); controls.Reset();
            borrowed_alive = SUCCEEDED(borrowed->AddComboBox(300));
            return S_FALSE;
        };
        sink.Reset();
        auto* invoked = controls.Get();
        Check(invoked->OnButtonClicked(nullptr, 300) == S_FALSE && unadvised && borrowed_alive && state->identities,
            "control callback may Unadvise and release owner and proxy while borrowed Customize survives");
        { std::lock_guard lock(owner->mutex); owner->dialog = nullptr; }
        Check(state->destroyed == 1 && !wrapper && !proxy && !controls, "control callback temporary references unwind without cycles");
    }
    {
        Stage("control sink destruction reenters Unadvise");
        int native_shows = 0;
        auto wrapper = Wrapper(native_shows);
        auto state = std::make_shared<State>(); state->controls = true;
        auto sink = MakeSink(state); DWORD cookie = 0;
        Check(SUCCEEDED(wrapper->Advise(sink.Get(), &cookie)), "subscribe control sink before destructor reentry");
        HRESULT repeated = S_OK;
        state->on_destroy = [&] { repeated = wrapper->Unadvise(cookie); };
        sink.Reset();
        Check(SUCCEEDED(wrapper->Unadvise(cookie)) && repeated == E_INVALIDARG && state->destroyed == 1,
            "control sink destructor may reenter Unadvise without duplicate native unsubscribe");
    }
    {
        Stage("control subscription final owner destruction");
        int native_shows = 0;
        auto wrapper = Wrapper(native_shows);
        auto state = std::make_shared<State>(); state->controls = true;
        auto sink = MakeSink(state); DWORD cookie = 0;
        Check(SUCCEEDED(wrapper->Advise(sink.Get(), &cookie)), "subscribe control sink before owner destruction");
        sink.Reset(); wrapper.Reset();
        Check(state->destroyed == 1, "active control subscription has no intrinsic owner cycle");
    }
    Stage("CoUninitialize");
    CoUninitialize();
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures ? 1 : 0;
}

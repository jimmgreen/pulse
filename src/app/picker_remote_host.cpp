#include "picker_remote_host.h"
#include "../ipc/picker_remote_protocol.h"
#include "../ipc/picker_transport.h"
#include <objbase.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <optional>
namespace pulse::picker_inprocess { HRESULT ConfigureRemoteParent(DWORD) noexcept; }
namespace pulse::app {
namespace {
namespace wire = pulse::ipc::picker::remote;
using pulse::ipc::picker::Channel;
using pulse::ipc::picker::PipeHandle;
constexpr UINT kDispatch = WM_APP + 417;
struct Host {
    Channel channel;
    PulsePickerUiV2Api api{};
    PulsePickerUiV2Session session = 0;
    PipeHandle cancel{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HWND window = nullptr;
    DWORD parent = 0;
    std::mutex mutex;
    std::deque<wire::Message> incoming;
    std::atomic<bool> failed{false};
    std::jthread reader;
    pulse::ipc::picker::RequestId request{};
    bool bound = false, showing = false, done = false, hello = false;
    uint64_t last_command = 0, next_event = 0, waiting_event = 0, active_show = 0;
    std::optional<wire::Message> event_reply;
    ~Host() {
        if (cancel) SetEvent(cancel.Get());
        if (reader.joinable()) reader.join();
        if (session) api.Shutdown(session);
        if (window) DestroyWindow(window);
    }
    void Fail() {
        failed = true;
        if (showing && active_show) api.Close(session, active_show, HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE));
        if (!showing) done = true;
    }
    void PumpOnce() {
        MSG msg{};
        if (waiting_event) {
            // An in-process callback blocks the picker thread. Keep that
            // contract while a remote callback is outstanding: run sent
            // messages and this host window's dispatch/wake-ups, but leave the
            // picker's queued input (e.g. Enter -> FileOk) until it returns,
            // instead of raising a nested event that must be rejected.
            if (PeekMessageW(&msg, window, 0, 0, PM_REMOVE)) { DispatchMessageW(&msg); return; }
            if (PeekMessageW(&msg, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE)) { Fail(); return; }
            MsgWaitForMultipleObjectsEx(0, nullptr, INFINITE, QS_ALLINPUT, 0);
            return;
        }
        if (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        else Fail();
    }
    bool Send(const wire::Message& message) {
        std::vector<uint8_t> bytes;
        if (!wire::Encode(message, bytes)) { Fail(); return false; }
        // Neither pipe direction blocks the STA: callbacks and caller RPCs may pump.
        std::atomic<bool> complete{false};
        bool sent = false;
        std::jthread writer([&] {
            sent = static_cast<bool>(channel.SendFrame(bytes, {cancel.Get(), 10000}));
            complete = true;
            PostMessageW(window, WM_NULL, 0, 0);
        });
        while (!complete) PumpOnce();
        writer.join();
        if (!sent) Fail();
        return sent;
    }
    void StartReader() {
        reader = std::jthread([this] {
            try {
                while (!failed) {
                    std::vector<uint8_t> bytes; wire::Message message;
                    if (!channel.ReceiveFrame(bytes, {cancel.Get(), INFINITE}) || !wire::Decode(bytes, message)) break;
                    { std::lock_guard lock(mutex); if (incoming.size() >= 64) break; incoming.push_back(std::move(message)); }
                    if (!PostMessageW(window, kDispatch, 0, 0)) break;
                }
            } catch (...) {}
            failed = true; PostMessageW(window, kDispatch, 0, 0);
        });
    }
    void Dispatch(wire::Message message);
    void Drain() {
        if (failed) { Fail(); return; }
        for (;;) {
            wire::Message message;
            { std::lock_guard lock(mutex); if (incoming.empty()) break; message = std::move(incoming.front()); incoming.pop_front(); }
            Dispatch(std::move(message));
            if (failed || done) break;
        }
    }
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        Host* self = reinterpret_cast<Host*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) { self = static_cast<Host*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
        if (msg == kDispatch && self) { try { self->Drain(); } catch (...) { self->Fail(); } return 0; }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    HRESULT Event(wire::Message message, uint32_t* response);
    static HRESULT WINAPI Callback(void* context, const PulsePickerUiV2Event* event) {
        try { wire::Message m; m.kind = wire::Kind::Event; if (!wire::FromAbi(*event, m.event)) return E_INVALIDARG; return static_cast<Host*>(context)->Event(std::move(m), nullptr); } catch (...) { return E_OUTOFMEMORY; }
    }
    static HRESULT Overwrite(void* context, const PulsePickerUiV2OverwriteEvent* event, uint32_t* response, wire::Kind kind) {
        try { wire::Message m; m.kind = kind; if (!wire::FromAbi(*event, m.event)) return E_INVALIDARG; return static_cast<Host*>(context)->Event(std::move(m), response); } catch (...) { return E_OUTOFMEMORY; }
    }
    static HRESULT WINAPI OverwriteCallback(void* c, const PulsePickerUiV2OverwriteEvent* e, uint32_t* r) { return Overwrite(c,e,r,wire::Kind::Overwrite); }
    static HRESULT WINAPI ShareCallback(void* c, const PulsePickerUiV2OverwriteEvent* e, uint32_t* r) { return Overwrite(c,e,r,wire::Kind::ShareViolation); }
};
HRESULT Host::Event(wire::Message message, uint32_t* response) {
    active_show = message.event.show_id;
    if (failed) { Fail(); return HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE); }
    if (waiting_event || next_event == UINT64_MAX) return E_UNEXPECTED;
    message.request_id = request; message.sequence = ++next_event;
    waiting_event = message.sequence; event_reply.reset();
    if (!Send(message)) { waiting_event = 0; return HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE); }
    while (!event_reply && !failed) PumpOnce();
    waiting_event = 0;
    if (failed) { Fail(); return HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE); }
    const auto reply = std::move(*event_reply); event_reply.reset();
    if (response) *response = reply.response;
    return static_cast<HRESULT>(reply.hresult);
}
void Host::Dispatch(wire::Message message) {
    if (!bound) {
        if (message.kind != wire::Kind::Hello) { Fail(); return; }
        request = message.request_id; bound = true;
    }
    if (message.request_id != request || !message.sequence) { Fail(); return; }
    if (message.kind == wire::Kind::EventReply) {
        if (!waiting_event || message.sequence != waiting_event || event_reply) { Fail(); return; }
        event_reply = std::move(message); return;
    }
    if (message.sequence <= last_command) { Fail(); return; }
    last_command = message.sequence;
    wire::Message reply; reply.request_id = request; reply.sequence = message.sequence; reply.kind = wire::Kind::Reply;
    HRESULT hr = E_INVALIDARG;
    if (message.kind == wire::Kind::Hello) {
        hr = !hello && message.version == PULSE_PICKER_UI_V2_VERSION ? S_OK : E_INVALIDARG;
        if (SUCCEEDED(hr)) { hello = true; reply.version = api.version; reply.capabilities = api.capabilities; reply.features = api.implemented_features; }
    } else if (!hello) { Fail(); return; }
    else if (message.kind == wire::Kind::ValidateConfig || message.kind == wire::Kind::Show) {
        if (showing) hr = HRESULT_FROM_WIN32(ERROR_BUSY);
        else {
            DWORD pid = 0;
            const HWND owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(message.config.owner));
            if (owner && (!IsWindow(owner) || !GetWindowThreadProcessId(owner, &pid) || pid != parent)) hr = E_ACCESSDENIED;
            else {
                if (!session) { PulsePickerUiV2Initialize init{sizeof(init), PULSE_PICKER_UI_V2_VERSION, {0,0}}; hr = api.Initialize(&init, &session); }
                else hr = S_OK;
                if (SUCCEEDED(hr)) {
                    wire::OwnedConfig config(message.config);
                    hr = api.ValidateConfig(session, &config.Get());
                    if (SUCCEEDED(hr) && message.kind == wire::Kind::Show) {
                        std::vector<wchar_t> path(PULSE_PICKER_UI_V2_MAX_UNITS + 1), paths(PULSE_PICKER_UI_V2_MAX_PACKED_UNITS + 1);
                        PulsePickerUiV2Result result{}; result.size = sizeof(result);
                        result.path = {path.data(), static_cast<uint32_t>(path.size()), 0};
                        result.paths = {paths.data(), static_cast<uint32_t>(paths.size()), 0};
                        PulsePickerUiV2Callbacks callbacks{sizeof(callbacks), 0, Callback, this, OverwriteCallback, ShareCallback};
                        showing = true; active_show = 0;
                        hr = api.Show(session, &config.Get(), &callbacks, &result);
                        showing = false; active_show = 0;
                        if (!wire::FromAbi(result, reply.result)) { Fail(); return; }
                    }
                }
            }
        }
        if (message.kind == wire::Kind::Show) {
            reply.kind = wire::Kind::Terminal;
            if (!reply.result.show_id) { reply.result.status = PULSE_PICKER_UI_V2_FAILED; reply.result.terminal_hr = static_cast<uint32_t>(hr); }
        }
    } else if (message.kind == wire::Kind::GetWindow) {
        HWND hwnd = nullptr; hr = api.GetWindow(session, message.show_id, &hwnd); reply.window = reinterpret_cast<uintptr_t>(hwnd);
    } else if (message.kind == wire::Kind::Close) hr = api.Close(session, message.show_id, static_cast<HRESULT>(message.hresult));
    else if (message.kind == wire::Kind::UpdateControl) { wire::OwnedControl control(message.control); hr = api.UpdateControl(session, message.show_id, &control.Get()); }
    else if (message.kind == wire::Kind::Navigate) hr = api.Navigate(session, message.show_id, {message.folder.data(), static_cast<uint32_t>(message.folder.size())});
    else if (message.kind == wire::Kind::Query) {
        PulsePickerUiV2Snapshot snapshot{}; snapshot.size = sizeof(snapshot);
        hr = api.Query(session, message.show_id, message.expected_generation, &snapshot);
        if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) {
            PulsePickerUiV2Buffer* buffers[]{&snapshot.folder,&snapshot.filename,&snapshot.selection,&snapshot.candidate,&snapshot.selection_paths,&snapshot.candidate_paths};
            std::vector<wchar_t> storage[6];
            for (size_t i=0;i<6;++i) { storage[i].resize(static_cast<size_t>(buffers[i]->units)+1); buffers[i]->data=storage[i].data(); buffers[i]->capacity=static_cast<uint32_t>(storage[i].size()); }
            hr = api.Query(session, message.show_id, snapshot.generation, &snapshot);
            if (SUCCEEDED(hr) && !wire::FromAbi(snapshot, reply.snapshot)) hr = E_UNEXPECTED;
        } else if (SUCCEEDED(hr) && !wire::FromAbi(snapshot, reply.snapshot)) {
            hr = E_UNEXPECTED;
        }
    } else { Fail(); return; }
    reply.hresult = static_cast<uint32_t>(hr);
    if (!failed) Send(reply);
    if (failed) Fail();
}
}
int RunPickerRemoteHost(const std::wstring& pipe, DWORD parent_pid) {
    try {
        Host host; host.parent = parent_pid;
        if (!host.cancel) return ERROR_NOT_ENOUGH_MEMORY;
        PipeHandle parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, parent_pid));
        if (!parent) return static_cast<int>(GetLastError());
        const auto connected = pulse::ipc::picker::Connect(pipe, parent.Get(), host.channel);
        if (!connected) return static_cast<int>(connected.error);
        if (FAILED(pulse::picker_inprocess::ConfigureRemoteParent(parent_pid))) return ERROR_INVALID_PARAMETER;
        if (FAILED(PulsePickerUiGetApiV2(PULSE_PICKER_UI_V2_VERSION, sizeof(host.api), &host.api))) return ERROR_NOT_SUPPORTED;
        WNDCLASSW wc{}; wc.lpfnWndProc = Host::WindowProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"Pulse.PickerRemoteHost";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return static_cast<int>(GetLastError());
        host.window = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, &host);
        if (!host.window) return static_cast<int>(GetLastError());
        host.StartReader();
        while (!host.done) host.PumpOnce();
        return host.failed ? ERROR_BROKEN_PIPE : ERROR_SUCCESS;
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
}

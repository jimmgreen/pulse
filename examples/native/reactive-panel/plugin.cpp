#include <pulse/client.hpp>
#include <atomic>
#include <condition_variable>
#include <thread>
namespace {
using pulse::sdk::View;
struct Plugin {
    pulse::sdk::Host host;
    PulseUiModelV1 ui{};
    PulseUiEventsV1 events{};
    pulse::sdk::Panel panel;
    pulse::sdk::Subscription subscription;
    std::unique_ptr<pulse::sdk::DispatchQueue> queue;
    std::mutex mutex;
    std::condition_variable_any changed;
    std::string latest;
    bool pending = false, active = false;
    std::atomic_uint64_t generation{0};
    std::jthread worker;
    explicit Plugin(const PulseHostV1& h) : host(h) {}
    void Work(std::stop_token stop) noexcept {
        try {
            while (!stop.stop_requested()) {
                std::string input; uint64_t revision;
                {
                    std::unique_lock lock(mutex);
                    if (!changed.wait(lock, stop, [&] { return pending; })) return;
                    input = latest; revision = generation.load(); pending = false;
                }
                size_t tokens = 0, codepoints = 0; bool in_token = false;
                std::string transformed = input;
                for (char& c : transformed) {
                    if (stop.stop_requested() || revision != generation.load()) break;
                    const auto byte = static_cast<unsigned char>(c);
                    if ((byte & 0xc0) != 0x80) ++codepoints;
                    const bool space = c == ' ' || c == '\t' || c == '\r' || c == '\n';
                    if (!space && !in_token) ++tokens;
                    in_token = !space;
                    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
                }
                if (stop.stop_requested() || revision != generation.load()) continue;
                const std::string summary = "Complete: " + std::to_string(tokens) + " tokens, " + std::to_string(codepoints) + " code points.";
                // No UI calls from this worker; dispatcher owns the handoff.
                queue->Post([this, revision, original = std::move(input), text = std::move(transformed), summary] {
                    if (!active || generation.load() != revision) return;
                    // A new committed input can precede its deferred state event.
                    std::string current;
                    if (panel.Get("query", current) != PULSE_OK || current != original) return;
                    if (panel.Set("result", text) == PULSE_OK) panel.Set("message", summary);
                    else panel.Set("message", "Result update failed.");
                });
            }
        } catch (...) {
            try { queue->Post([this] { if (active) panel.Set("message", "Worker failed."); }); } catch (...) {}
        }
    }
    PulseStatus Start() {
        if (worker.joinable()) return PULSE_ERROR_STATE;
        PulseStatus code = host.Query(PULSE_UI_MODEL_INTERFACE, 1, ui); if (code != PULSE_OK) return code;
        code = host.Query(PULSE_UI_EVENTS_INTERFACE_NAME, 1, events); if (code != PULSE_OK) return code;
        PulseDispatchV1 dispatch{}; code = host.Query(PULSE_DISPATCH_INTERFACE, 1, dispatch); if (code != PULSE_OK) return code;
        queue = std::make_unique<pulse::sdk::DispatchQueue>(dispatch);
        using pulse::sdk::ModelNode;
        using N = PulseUiModelNode;
        pulse::sdk::ModelTree tree(ModelNode(PULSE_UI_MODEL_STACK, "reactive-root").Int(&N::gap, 12).Children({
            ModelNode(PULSE_UI_MODEL_TEXT_INPUT, "query-input").String(&N::text, "Text to analyze").String(&N::state, "query").String(&N::placeholder, "Type words, including Unicode"),
            ModelNode(PULSE_UI_MODEL_TEXT, "result-text").String(&N::binding, "state.result"),
            ModelNode(PULSE_UI_MODEL_TEXT, "message-text").String(&N::binding, "state.message")}));
        tree.State("query", "").State("result", "").State("message", "Ready. Type to start background analysis.");
        uint64_t id = 0;
        code = ui.create(ui.context, View("Reactive C++ worker"), &tree.Build(), &id); if (code != PULSE_OK) return code;
        panel.Adopt(ui, id); active = true;
        code = subscription.State(ui, events, id, "query", PULSE_UI_STATE_USER,
            [this](uint64_t, const PulseUiStateBatch& batch) {
                if (!active) return;
                for (uint32_t i = 0; i < batch.count; ++i) {
                    const auto& item = batch.changes[i];
                    std::lock_guard lock(mutex); latest.assign(item.value.data, item.value.length); ++generation; pending = true;
                }
                panel.Set("message", "Computing..."); changed.notify_one();
            });
        if (code != PULSE_OK) return code;
        worker = std::jthread([this](std::stop_token stop) { Work(stop); });
        return PULSE_OK;
    }
    void Stop() noexcept {
        active = false; ++generation; subscription.Reset();
        worker.request_stop(); changed.notify_all();
        if (queue) queue->Close();
        panel.Close();
        // No join on a control callback/deactivation. Destruction owns the join.
    }
    ~Plugin() { Stop(); if (worker.joinable()) worker.join(); queue.reset(); }
};
PulseStatus PULSE_CALL Activate(void* p) noexcept { try { return static_cast<Plugin*>(p)->Start(); } catch (...) { return PULSE_ERROR_INITIALIZATION; } }
void PULSE_CALL Deactivate(void* p) noexcept { static_cast<Plugin*>(p)->Stop(); }
void PULSE_CALL Destroy(void* p) noexcept { delete static_cast<Plugin*>(p); }
}
PULSE_PLUGIN_SDK_PREVIEW()
extern "C" __declspec(dllexport) PulseStatus PULSE_CALL PulsePluginEntryV1(const PulseHostV1* host, PulsePluginV1* out) noexcept {
    if (!host || !out || host->size < sizeof(*host) || out->size < sizeof(*out) || host->version != 1 || out->version != 1) return PULSE_ERROR_ABI;
    try { *out = {sizeof(*out), 1, new Plugin(*host), Activate, Deactivate, Destroy}; return PULSE_OK; } catch (...) { return PULSE_ERROR_INITIALIZATION; }
}

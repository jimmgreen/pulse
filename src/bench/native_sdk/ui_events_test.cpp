#include "../../app/extensions/native/ui_adapter.h"
#include "../../common/windows_compat.h"
#include "ui_model_fixture.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <objbase.h>
using namespace native_ui_fixture;
namespace {
int failures = 0;
void Check(bool pass, const char *label) {
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", label);
    failures += !pass;
}
void Pump() {
    MSG message{};
    unsigned count = 0;
    while (count++ < 1000 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message != WM_QUIT) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}
std::string Copy(PulseStringView value) { return value.length ? std::string(value.data, value.length) : std::string(); }
struct Events {
    unsigned calls = 0, depth = 0, maximum_depth = 0;
    uint64_t revision = 0;
    std::map<std::string, std::pair<std::string, uint32_t>> changes;
    std::function<void(uint64_t)> action;
    static void PULSE_CALL Receive(void *context, uint64_t panel, const PulseUiStateBatch *batch) {
        auto &self = *static_cast<Events *>(context);
        ++self.calls;
        ++self.depth;
        self.maximum_depth = (std::max)(self.maximum_depth, self.depth);
        self.revision = batch->revision;
        self.changes.clear();
        for (uint32_t i = 0; i < batch->count; ++i)
            self.changes[Copy(batch->changes[i].key)] = {Copy(batch->changes[i].value), batch->changes[i].source_mask};
        if (self.action)
            self.action(panel);
        --self.depth;
    }
};
struct Fixture {
    PulseUiModelNode root = Node(PULSE_UI_MODEL_STACK, "root");
    std::array<PulseUiModelNode, 5> children{Node(PULSE_UI_MODEL_TEXT_INPUT, "input"),
                                             Node(PULSE_UI_MODEL_CHECKBOX, "check"),
                                             Node(PULSE_UI_MODEL_SELECT, "select"), Node(PULSE_UI_MODEL_TEXT, "output"),
                                             Node(PULSE_UI_MODEL_BUTTON, "toggle")};
    std::array<PulseStringView, 2> choices{S("One"), S("Two")};
    std::array<PulseUiState, 5> states{{{S("name"), S("initial")},
                                        {S("flag"), S("false")},
                                        {S("selection"), S("0")},
                                        {S("output"), S("idle")},
                                        {S("toggle"), S("false")}}};
    PulseStringView command = S("changed");
    PulseUiModelTree tree{};
    Fixture() {
        root.children = {1, children.data(), static_cast<uint32_t>(children.size())};
        children[0].text = T("Name");
        children[0].state = T("name");
        children[1].text = T("Enabled");
        children[1].state = T("flag");
        children[2].text = T("Choice");
        children[2].state = T("selection");
        children[2].options = {1, choices.data(), 2};
        children[3].text = T("Result");
        children[3].binding = T("state.output");
        children[4].text = T("Toggle");
        children[4].kind = T("toggle");
        children[4].state = T("toggle");
        children[4].command = T("changed");
        tree.size = sizeof(tree);
        tree.root = &root;
        tree.states = states.data();
        tree.state_count = static_cast<uint32_t>(states.size());
        tree.commands = &command;
        tree.command_count = 1;
    }
};
} // namespace
int wmain() {
    pulse::compat::EnableDpiAwareness();
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        return 2;
    HWND owner = CreateWindowExW(0, L"STATIC", L"Native events fixture", WS_OVERLAPPEDWINDOW, 0, 0, 800, 600, nullptr,
                                 nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner) {
        CoUninitialize();
        return 2;
    }
    ShowWindow(owner, SW_SHOWNOACTIVATE);
    {
        bool alive = true;
        pulse::extensions::native::UiAdapter adapter(owner, false, D2D1::ColorF(0x0078d4), 1, [&] { return alive; });
        PulseUiModelV1 api{};
        PulseUiModelV1 model{};
        PulseUiEventsV1 events{};
        Check(adapter.QueryModel(PULSE_UI_MODEL_VERSION, &api, sizeof(api)) == PULSE_OK &&
                  adapter.QueryModel(PULSE_UI_MODEL_VERSION, &model, sizeof(model)) == PULSE_OK &&
                  adapter.QueryEvents(PULSE_UI_EVENTS_VERSION_1, &events, sizeof(events)) == PULSE_OK,
              "query independent typed UI events interface");
        Check(adapter.QueryEvents(2, &events, sizeof(events)) == PULSE_ERROR_ABI, "events version mismatch rejected");
        Fixture fixture;
        uint64_t panel = 0;
        Check(model.create(api.context, S("State events"), &fixture.tree, &panel) == PULSE_OK,
              "create real editable host controls");
        if (!panel) {
            adapter.Stop();
            DestroyWindow(owner);
            CoUninitialize();
            return 1;
        }
        Pump();
        Events all, user, program;
        uint64_t all_token = 0, user_token = 0, program_token = 0;
        Check(events.subscribe_state(events.context, panel, {}, PULSE_UI_STATE_ALL, Events::Receive, &all,
                                     &all_token) == PULSE_OK &&
                  events.subscribe_state(events.context, panel, S("name"), PULSE_UI_STATE_USER, Events::Receive, &user,
                                         &user_token) == PULSE_OK &&
                  events.subscribe_state(events.context, panel, {}, PULSE_UI_STATE_PROGRAMMATIC, Events::Receive,
                                         &program, &program_token) == PULSE_OK,
              "subscribe all keys and specific source filters");
        Pump();
        Check(!all.calls, "subscription does not synthesize initial changes");
        HWND input = adapter.NodeWindow("input"), checkbox = adapter.NodeWindow("check"),
             select = adapter.NodeWindow("select");
        Check(input && checkbox && select, "input checkbox select use real controls");
        SetWindowTextW(input, L"typed");
        Check(!all.calls, "user change callback waits until native notification returns");
        Pump();
        Check(all.calls == 1 && user.calls == 1 && !program.calls && all.changes["name"].first == "typed" &&
                  all.changes["name"].second == PULSE_UI_STATE_USER,
              "edit commit emits typed user-source event");
        struct NestedNotification {
            Events *events;
            bool deferred = false;
            static void PULSE_CALL Run(void *context, uint64_t, PulseStringView) {
                auto &self = *static_cast<NestedNotification *>(context);
                const auto before = self.events->calls;
                Pump();
                self.deferred = self.events->calls == before;
            }
        } nested{&all};
        uint64_t click_token = 0;
        model.subscribe_command(model.context, panel, S("changed"), NestedNotification::Run, &nested, &click_token);
        const auto initial_revision = all.revision;
        SetWindowTextW(input, L"first");
        SetWindowTextW(input, L"final");
        SendMessageW(checkbox, BM_CLICK, 0, 0);
        SendMessageW(adapter.NodeWindow("toggle"), BM_CLICK, 0, 0);
        Check(nested.deferred, "nested command message pump cannot deliver before control notification unwinds");
        SendMessageW(select, CB_SETCURSEL, 1, 0);
        SendMessageW(GetParent(select), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(select), CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(select));
        Pump();
        Check(all.calls == 2 && all.changes.size() == 4 && all.changes["name"].first == "final" &&
                  all.changes["flag"].first == "true" && all.changes["selection"].first == "1" &&
                  all.revision > initial_revision,
              "same-turn input checkbox selection changes coalesce with final values and increasing revision");
        PulseUiState changes[]{{S("name"), S("program")}, {S("output"), S("done")}};
        api.set_states(api.context, panel, changes, 2);
        Check(program.calls == 0, "programmatic transaction is also deferred");
        Pump();
        Check(program.calls == 1 && user.calls == 2 && all.changes.size() == 2 &&
                  all.changes["name"].second == PULSE_UI_STATE_PROGRAMMATIC,
              "programmatic batch is distinct from user edits");
        const auto stable_calls = all.calls;
        api.set_states(api.context, panel, changes, 2);
        Pump();
        Check(all.calls == stable_calls, "unchanged values do not emit events");
        SendMessageW(input, WM_IME_STARTCOMPOSITION, 0, 0);
        SetWindowTextW(input, L"preedit");
        Pump();
        Check(all.calls == stable_calls, "IME preedit has no committed user event");
        SendMessageW(input, WM_IME_ENDCOMPOSITION, 0, 0);
        Pump();
        Check(all.calls == stable_calls + 1 && all.changes["name"].first == "preedit",
              "IME end commits once through ordinary user event");
        SetWindowTextW(input, L"removed");
        const auto before_unsubscribe = user.calls;
        Check(api.unsubscribe(api.context, user_token) == PULSE_OK, "existing UI unsubscribe removes event token");
        Pump();
        Check(user.calls == before_unsubscribe, "unsubscribe suppresses already queued change");
        Events late;
        uint64_t late_token = 0;
        SetWindowTextW(input, L"before-subscribe");
        events.subscribe_state(events.context, panel, {}, PULSE_UI_STATE_ALL, Events::Receive, &late, &late_token);
        Pump();
        Check(!late.calls, "new subscriber cannot receive changes queued before registration");
        events.unsubscribe(events.context, late_token);
        bool inside = false;
        all.action = [&](uint64_t current) {
            inside = adapter.InCallback();
            if (all.changes.contains("name")) {
                PulseUiState value{S("output"), S("callback-result")};
                api.set_states(api.context, current, &value, 1);
                Pump(); // A DLL can pump messages; this must not recursively enter its own event.
            }
        };
        const auto before_reentrant = all.calls;
        SetWindowTextW(input, L"callback");
        Pump();
        Check(all.calls == before_reentrant + 2 && all.maximum_depth == 1 && inside && !adapter.InCallback(),
              "callback state update is next batch and shares unload gate without recursive entry");
        all.action = {};
        SetWindowTextW(input, L"pending-update");
        const auto before_update = all.calls;
        Check(model.update(model.context, panel, &fixture.tree) == PULSE_OK,
              "successful tree update keeps real model valid");
        Pump();
        Check(all.calls == before_update, "tree update discards pre-update pending delivery");
        Events retained;
        uint64_t retained_token = 0;
        events.subscribe_state(events.context, panel, S("name"), PULSE_UI_STATE_USER, Events::Receive, &retained,
                               &retained_token);
        Check(model.update(model.context, panel, &fixture.tree) == PULSE_OK,
              "identical model update preserves valid subscriptions");
        SetWindowTextW(adapter.NodeWindow("input"), L"after-update");
        Pump();
        Check(all.calls == before_update + 1 && retained.calls == 1 && retained.changes["name"].first == "after-update",
              "all-key and surviving-key subscriptions receive new input after update");
        Events removed;
        uint64_t removed_token = 0;
        events.subscribe_state(events.context, panel, S("toggle"), PULSE_UI_STATE_ALL, Events::Receive, &removed,
                               &removed_token);
        fixture.root.children.count = 4;
        fixture.tree.state_count = 4;
        Check(model.update(model.context, panel, &fixture.tree) == PULSE_OK,
              "model update can remove a control and its state key");
        Check(events.unsubscribe(events.context, removed_token) == PULSE_ERROR_STATE,
              "deleted-key subscription token is retired");
        const auto retained_before = retained.calls;
        bool updated_inside = false;
        all.action = [&](uint64_t current) {
            updated_inside = model.update(model.context, current, &fixture.tree) == PULSE_OK;
        };
        SetWindowTextW(adapter.NodeWindow("input"), L"update-in-callback");
        Pump();
        Check(updated_inside && retained.calls == retained_before,
              "callback model update stops remaining listeners of the old batch");
        all.action = {};
        SetWindowTextW(adapter.NodeWindow("input"), L"new-batch");
        Pump();
        Check(retained.calls == retained_before + 1,
              "retained listener resumes on a fresh batch after reentrant update");
        Check(events.unsubscribe(events.context, retained_token) == PULSE_OK,
              "surviving-key token remains valid for explicit unsubscribe");
        Events closer, after_close;
        uint64_t close_token = 0, after_token = 0;
        closer.action = [&](uint64_t current) {
            inside = adapter.InCallback();
            api.close(api.context, current);
        };
        events.subscribe_state(events.context, panel, {}, PULSE_UI_STATE_ALL, Events::Receive, &closer, &close_token);
        events.subscribe_state(events.context, panel, {}, PULSE_UI_STATE_ALL, Events::Receive, &after_close,
                               &after_token);
        SetWindowTextW(adapter.NodeWindow("input"), L"close");
        Pump();
        Check(closer.calls == 1 && inside && !after_close.calls && !adapter.InCallback(),
              "callback closing panel prevents later listeners and safely exits unload gate");
        Check(events.subscribe_state(events.context, panel, {}, PULSE_UI_STATE_ALL, Events::Receive, &all,
                                     &all_token) == PULSE_ERROR_STATE,
              "closed panel rejects subscription");
        adapter.Stop();
    }
    DestroyWindow(owner);
    CoUninitialize();
    return failures ? 1 : 0;
}

#include <pulse/client.hpp>
#include <cstdio>
#include <atomic>
#include <thread>
#include <string_view>
namespace {
struct CommandHost {
    PulseCommandDescriptor descriptor{};
    PulseStatus result = PULSE_OK;
    int removed = 0;
    static PulseStatus PULSE_CALL Register(void* p, const PulseCommandDescriptor* d, PulseCommandId* id) {
        auto& h = *static_cast<CommandHost*>(p);
        if (h.result != PULSE_OK) return h.result;
        h.descriptor = *d; *id = 42; return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Remove(void* p, PulseCommandId) {
        ++static_cast<CommandHost*>(p)->removed; return PULSE_OK;
    }
    PulseCommandsV1 Api() { return {sizeof(PulseCommandsV1), 1, this, Register, Remove}; }
    void Invoke() { PulseContextSelectionSnapshot snapshot{}; descriptor.invoke(descriptor.user, 42, &snapshot); }
};
struct TaskHost {
    PulseTaskDescriptor descriptor{};
    PulseTaskState state = PULSE_TASK_RUNNING;
    PulseStatus create_result = PULSE_OK, get_result = PULSE_OK, release_result = PULSE_OK;
    bool synchronous = false;
    int releases = 0, cancels = 0;
    static PulseStatus PULSE_CALL Create(void* p, const PulseTaskDescriptor* d, PulseTaskId* id) {
        auto& h = *static_cast<TaskHost*>(p);
        if (h.create_result != PULSE_OK) return h.create_result;
        h.descriptor = *d; *id = 9;
        if (h.synchronous) { PulseTaskContext context{}; context.id = *id; h.state = d->work(d->user, &context); }
        return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Cancel(void* p, PulseTaskId id) {
        auto& h = *static_cast<TaskHost*>(p); ++h.cancels;
        h.descriptor.on_cancel(h.descriptor.user, id); return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Get(void* p, PulseTaskId id, PulseTaskSnapshotFn fn, void* user) {
        auto& h = *static_cast<TaskHost*>(p);
        if (h.get_result != PULSE_OK) return h.get_result;
        PulseTaskSnapshot snapshot{}; snapshot.size = sizeof(snapshot); snapshot.id = id; snapshot.state = h.state;
        fn(user, &snapshot); return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Release(void* p, PulseTaskId) {
        auto& h = *static_cast<TaskHost*>(p); ++h.releases; return h.release_result;
    }
    PulseTasksV1 Api() { return {sizeof(PulseTasksV1), 1, this, Create, Cancel, Get, Release}; }
};
template<class Check> void TestOwnership(Check check) {
    CommandHost host;
    PulseCommandDescriptor descriptor{}; descriptor.size = sizeof(descriptor);
    pulse::sdk::CommandRegistration command;
    int calls = 0;
    check(command.Register(host.Api(), descriptor, [&](PulseCommandId id, const PulseContextSelectionSnapshot&) { if (id == 42) ++calls; }) == PULSE_OK, "command registers");
    pulse::sdk::CommandRegistration moved(std::move(command));
    host.Invoke();
    check(command.Id() == 0 && moved.Id() == 42 && calls == 1 && host.removed == 0, "command move transfers live callback without unregistering");
    command = std::move(moved); command.Reset(); command.Reset();
    check(moved.Id() == 0 && host.removed == 1, "move assignment and repeated reset unregister exactly once");
    auto lifetime = std::make_shared<int>(1); std::weak_ptr<int> weak = lifetime;
    host.result = PULSE_ERROR_STATE;
    check(command.Register(host.Api(), descriptor, [lifetime](PulseCommandId, const PulseContextSelectionSnapshot&) {}) == PULSE_ERROR_STATE && command.Id() == 0, "failed registration leaves no owned command");
    lifetime.reset(); check(weak.expired() && host.removed == 1, "failed registration releases functor without unregistering");
    host.result = PULSE_OK; lifetime = std::make_shared<int>(2); weak = lifetime;
    bool alive_inside = false;
    command.Register(host.Api(), descriptor, [&, lifetime](PulseCommandId, const PulseContextSelectionSnapshot&) { command.Reset(); alive_inside = !weak.expired(); });
    lifetime.reset(); host.Invoke();
    check(alive_inside && weak.expired() && command.Id() == 0, "self unregister keeps callback alive until invocation returns");
    command.Register(host.Api(), descriptor, [](PulseCommandId, const PulseContextSelectionSnapshot&) { throw 1; });
    host.Invoke(); check(command.Id() == 42, "command exception stays inside C trampoline");

    TaskHost tasks; tasks.synchronous = true;
    pulse::sdk::TaskGroup group(tasks.Api()); PulseTaskId id = 0;
    bool starting_safe = false;
    check(group.Start("sync", "test", [&](const PulseTaskContext& context) {
        starting_safe = context.id == 9 && group.Pending() == 1 && group.Collect() == 0;
        group.CancelAll(); return PULSE_TASK_COMPLETED;
    }, {}, &id) == PULSE_OK && id == 9 && starting_safe && tasks.cancels == 0, "synchronous work can reenter collection before create returns");
    check(group.Collect() == 1 && group.Pending() == 0 && tasks.releases == 1, "completed task releases exactly once");
    tasks.synchronous = false; tasks.state = PULSE_TASK_RUNNING;
    lifetime = std::make_shared<int>(3); weak = lifetime; int cancelled = 0;
    group.Start("cancel", "test", [lifetime](const PulseTaskContext&) { return PULSE_TASK_CANCELLED; }, [&](PulseTaskId task) { if (task == 9) ++cancelled; throw 1; }, &id);
    lifetime.reset(); group.CancelAll();
    check(cancelled == 1 && tasks.cancels == 1 && group.Collect() == 0 && !weak.expired(), "cancel callback exception is contained and running userdata retained");
    tasks.state = PULSE_TASK_CANCELLED; tasks.release_result = PULSE_ERROR_STATE;
    check(group.Collect() == 0 && group.Pending() == 1 && !weak.expired(), "failed terminal release retains userdata");
    tasks.get_result = PULSE_ERROR_STATE; tasks.release_result = PULSE_OK;
    const int releases = tasks.releases;
    check(group.Collect() == 0 && group.Pending() == 1 && !weak.expired() && tasks.releases == releases, "unknown task ID is retained without release");
    tasks.get_result = PULSE_OK;
    check(group.Collect() == 1 && weak.expired(), "successful terminal release frees callback captures");
    tasks.create_result = PULSE_ERROR_STATE; lifetime = std::make_shared<int>(4); weak = lifetime; id = 123;
    check(group.Start("fail", "test", [lifetime](const PulseTaskContext&) { return PULSE_TASK_COMPLETED; }, {}, &id) == PULSE_ERROR_STATE && id == 0 && group.Pending() == 0, "failed task creation clears output and pending slot");
    lifetime.reset(); check(weak.expired(), "failed task creation releases captures");
    tasks.create_result = PULSE_OK; tasks.synchronous = true;
    group.Start("throw", "test", [](const PulseTaskContext&) -> PulseTaskState { throw 1; }, {}, &id);
    check(tasks.state == PULSE_TASK_FAILED && group.Collect() == 1, "work exception becomes failed terminal task");
}
bool Equals(PulseStringView value, std::string_view expected) {
    return std::string_view(value.data ? value.data : "", value.length) == expected;
}
template<class Check> void TestBuilder(Check check) {
    using namespace pulse::sdk;
    ModelNode child(PULSE_UI_MODEL_DATA_TABLE, std::string("table"));
    child.Options({std::string("option")}).DataColumns({ModelColumn(std::string("col"), std::string("Column"), 100)})
        .Rows({ModelRow(std::string("row"), std::string("Parent")).Cells({std::string("cell")})
            .Children({ModelRow(std::string("nested"), std::string("Child")).Cells({}).Children({})})});
    ModelNode root(PULSE_UI_MODEL_STACK, std::string("root"));
    root.String(&PulseUiModelNode::text, std::string("old")).String(&PulseUiModelNode::text, std::string(""))
        .Bool(&PulseUiModelNode::enabled, false).Int(&PulseUiModelNode::gap, 0).Children({child});
    ModelTree tree(std::move(root)); tree.State(std::string("key"), std::string("old")).State("key", std::string("value")).Command(std::string("run"));
    tree.Build();
    ModelTree copied = tree;
    tree = ModelTree(ModelNode(PULSE_UI_MODEL_TEXT, "replacement"));
    ModelTree moved = std::move(copied);
    const auto& built = moved.Build();
    check(built.size == sizeof(built) && Equals(built.root->id, "root") && built.state_count == 1 && Equals(built.states[0].value, "value") && built.command_count == 1 && Equals(built.commands[0], "run"), "builder owns temporary strings across copy move and rebuild");
    const auto& node = *built.root;
    check(node.text.present == 1 && node.text.value.length == 0 && node.enabled.present == 1 && node.enabled.value == 0 && node.gap.present == 1 && node.gap.value == 0 && node.placeholder.present == 0, "explicit empty string false and zero differ from absent fields");
    const auto& table = node.children.data[0];
    const auto& row = table.rows.data[0];
    check(node.children.present == 1 && node.children.count == 1 && table.options.count == 1 && Equals(table.options.data[0], "option") && table.data_columns.count == 1 && Equals(table.data_columns.data[0].text, "Column") && table.data_columns.data[0].width.value == 100 && Equals(row.cells.data[0], "cell") && row.children.count == 1 && Equals(row.children.data[0].text.value, "Child"), "nested node column row and string arrays survive rebuild");
    const auto& leaf = row.children.data[0];
    check(leaf.cells.present == 1 && leaf.cells.count == 0 && leaf.children.present == 1 && leaf.children.count == 0 && table.children.present == 0, "explicit empty row arrays differ from absent arrays");
    ModelNode empty(PULSE_UI_MODEL_STACK, "empty"); empty.Children({}).Options({}).DataColumns({}).Rows({});
    const auto& arrays = empty.Build();
    check(arrays.children.present == 1 && arrays.children.count == 0 && arrays.options.present == 1 && arrays.options.count == 0 && arrays.data_columns.present == 1 && arrays.data_columns.count == 0 && arrays.rows.present == 1 && arrays.rows.count == 0, "all node arrays preserve explicit empty values");
}
struct Fake {
    PulseDispatchCallback pending = nullptr;
    void* user = nullptr;
    bool immediate = false;
    bool block_post = false;
    std::atomic_bool entered{false}, release{false};
    PulseUiStateChangedFn state = nullptr;
    void* state_user = nullptr;
    int unsubscribed = 0, closed = 0;
    static PulseStatus PULSE_CALL Post(void* p, PulseDispatchCallback fn, void* user, uint64_t* token) {
        auto& self = *static_cast<Fake*>(p); *token = 7;
        if (self.block_post) { self.entered = true; while (!self.release.load()) std::this_thread::yield(); }
        if (self.immediate) fn(user); else { self.pending = fn; self.user = user; }
        return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Cancel(void* p, uint64_t) {
        auto& self = *static_cast<Fake*>(p);
        if (!self.pending) return PULSE_ERROR_STATE;
        self.pending = nullptr; self.user = nullptr; return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Subscribe(void* p, uint64_t, PulseStringView, uint32_t, PulseUiStateChangedFn fn, void* user, uint64_t* token) {
        auto& self = *static_cast<Fake*>(p); self.state = fn; self.state_user = user; *token = 1; return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Unsubscribe(void* p, uint64_t) {
        auto& self = *static_cast<Fake*>(p); ++self.unsubscribed; self.state = nullptr; self.state_user = nullptr; return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Close(void* p, uint64_t) { ++static_cast<Fake*>(p)->closed; return PULSE_OK; }
};
}
int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failures; };
    TestOwnership(check);
    TestBuilder(check);
    Fake fake;
    PulseDispatchV1 dispatch{sizeof(dispatch), 1, &fake, Fake::Post, Fake::Cancel};
    {
        pulse::sdk::DispatchQueue queue(dispatch); fake.immediate = true; int calls = 0;
        check(queue.Post([&] { ++calls; queue.Close(); }) == PULSE_OK && calls == 1, "callback before post return can close queue safely");
        check(queue.Post([] {}) == PULSE_ERROR_STATE, "closed queue rejects new work");
    }
    std::weak_ptr<int> released;
    {
        pulse::sdk::DispatchQueue queue(dispatch); fake.immediate = false;
        auto lifetime = std::make_shared<int>(1); std::weak_ptr<int> weak = lifetime; released = lifetime;
        queue.Post([lifetime] {}); lifetime.reset(); queue.Close();
        check(!fake.pending && !weak.expired(), "cancel suppresses callback while slot retains safe ownership");
    }
    check(released.expired(), "cancelled userdata is released when queue is destroyed");
    {
        pulse::sdk::DispatchQueue queue(dispatch); fake.block_post = true;
        std::thread producer([&] { queue.Post([] {}); });
        while (!fake.entered.load()) std::this_thread::yield();
        queue.Close(); fake.release = true; producer.join();
        check(fake.pending == nullptr, "close racing post before token assignment cancels after return");
        fake.block_post = false;
    }
    PulseUiModelV1 ui{}; ui.context = &fake; ui.unsubscribe = Fake::Unsubscribe; ui.close = Fake::Close;
    PulseUiEventsV1 events{sizeof(events), 1, &fake, Fake::Subscribe, Fake::Unsubscribe};
    pulse::sdk::Panel panel; panel.Adopt(ui, 4);
    pulse::sdk::Subscription subscription;
    int calls = 0;
    subscription.State(ui, events, 4, "query", PULSE_UI_STATE_USER, [&](uint64_t, const PulseUiStateBatch&) {
        subscription.Reset(); panel.Close(); ++calls;
    });
    PulseUiStateBatch batch{}; fake.state(fake.state_user, 4, &batch);
    check(calls == 1 && fake.unsubscribed == 1 && fake.closed == 1, "callback can unregister itself and close its panel");
    subscription.State(ui, events, 4, "query", PULSE_UI_STATE_USER, [](uint64_t, const PulseUiStateBatch&) { throw 1; });
    fake.state(fake.state_user, 4, &batch);
    check(true, "functor exceptions do not cross C callback boundary");
    return failures ? 1 : 0;
}

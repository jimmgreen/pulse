#include "../../app/extensions/native/context_adapter.h"
#include <cstdio>
#include <thread>
#include <vector>
#include <stdexcept>
using namespace pulse::extensions;
using namespace pulse::extensions::native;
namespace {
int passes = 0, failures = 0;
void Check(bool ok, const char *label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); ok ? ++passes : ++failures; }
std::string Copy(PulseStringView s) { return {s.data ? s.data : "", s.length}; }
struct Receiver {
    int calls = 0;
    uint32_t changes = 0;
    uint64_t revision = 0, navigation = 0, selection = 0, count = 0;
    bool loading = false;
    std::string directory, name, mode;
    std::function<void()> during;
    static void PULSE_CALL Call(void *user, const PulseContextSnapshot *value) {
        auto &r = *static_cast<Receiver *>(user);
        ++r.calls; r.changes = value->changes; r.revision = value->revision;
        r.navigation = value->navigation_revision; r.selection = value->selection_revision;
        r.count = value->selection_count; r.loading = value->loading != 0;
        r.directory = Copy(value->directory); r.name = Copy(value->selection_name); r.mode = Copy(value->view_mode);
        if (r.during) r.during();
    }
};
struct Fixture {
    authorization::Registry registry;
    CallerBinding caller;
    ContextObservation state;
    bool live = true;
    ContextSelection selection;
    PulseStatus selection_status = PULSE_OK;
    Fixture() : caller(Binding()) { state.directory = "C:\\fixture"; state.tab_key = 1; state.snapshot_key = 2; state.view_mode = "details"; }
    CallerBinding Binding() {
        registry.SyncPackages({{{0, "native-context-test", "r1", 1}, true}});
        auto owner = registry.RegisterOwner(1);
        return *registry.CaptureBinding({0, "native-context-test", "r1", 1}, *owner);
    }
    ContextBackend Backend() {
        return {[this](const CallerBinding &) { return live; },
            [this] { return state; }, [this](ContextSelection &out) { out = selection; return selection_status; }};
    }
};
}
int main() {
    Fixture f; ContextAdapter adapter(f.caller, f.Backend()); auto api = adapter.Table();
    PulseContextV1 queried{};
    Check(adapter.Query(1, &queried, sizeof(queried)) == PULSE_OK && queried.version == 1, "versioned native context table query");
    Check(adapter.Query(2, &queried, sizeof(queried)) == PULSE_ERROR_ABI && adapter.Query(1, nullptr, sizeof(queried)) == PULSE_ERROR_ARGUMENT,
        "unknown version and missing destination rejected");
    Receiver r; PulseContextSubscriptionId id = 99;
    Check(api.subscribe(api.context, Receiver::Call, &r, &id) == PULSE_OK && id && !r.calls, "initial subscription is deferred");
    adapter.Poll();
    Check(r.calls == 1 && r.directory == f.state.directory && r.mode == "details" && (r.changes & PULSE_CONTEXT_INITIAL), "initial loaded context uses borrowed typed snapshot");
    const auto initial = r.revision; adapter.Poll(); adapter.Poll();
    Check(r.calls == 1 && r.revision == initial, "unchanged ticks do not emit callbacks");
    f.state.selection_count = 2; ++f.state.selection_epoch; adapter.Poll();
    Check(r.calls == 2 && r.count == 2 && r.changes == PULSE_CONTEXT_SELECTION, "selection-only change has specific event mask");
    const auto selected = r.selection; ++f.state.selection_epoch; adapter.Poll();
    Check(r.calls == 3 && r.selection > selected && r.count == 2 && r.name.empty(), "equal-count multi-selection changes remain observable without enumeration");
    f.state.loading = true; f.state.selection_name = "stale.txt"; adapter.Poll();
    Check(r.loading && !r.count && r.name.empty() && (r.changes & PULSE_CONTEXT_NAVIGATION), "loading suppresses stale selection summary");
    f.state.directory = "C:\\other"; f.state.loading = false; f.state.selection_count = 1; f.state.selection_name = "new.txt"; adapter.Poll();
    Check(r.directory == "C:\\other" && r.name == "new.txt" && !r.loading && r.count == 1, "navigation restores fresh selection after loading");
    f.state.view_mode = "tiles"; adapter.Poll();
    Check(r.changes == PULSE_CONTEXT_VIEW && r.mode == "tiles", "view changes use machine mode and specific mask");
    ++f.state.tab_key; adapter.Poll();
    Check((r.changes & PULSE_CONTEXT_NAVIGATION) && (r.changes & PULSE_CONTEXT_SELECTION), "same-path owner tab switch invalidates context identity");
    ++f.state.snapshot_key; adapter.Poll();
    Check(r.changes == PULSE_CONTEXT_SELECTION, "replacement snapshot invalidates selection even with same summary");
    Receiver direct; ++f.state.selection_epoch;
    Check(api.get(api.context, Receiver::Call, &direct) == PULSE_OK && direct.calls == 1 && direct.name == "new.txt", "get synchronously reads latest already-loaded state");
    adapter.Poll();
    Check(r.changes == PULSE_CONTEXT_SELECTION && r.selection == direct.selection, "get cannot consume pending subscriber change flags");
    const auto oldcalls = r.calls;
    Check(api.unsubscribe(api.context, id) == PULSE_OK && api.unsubscribe(api.context, id) == PULSE_ERROR_STATE,
        "unsubscribe invalidates subscription exactly once");
    ++f.state.selection_epoch; adapter.Poll(); Check(r.calls == oldcalls, "unsubscribed callback never fires again");
    Check(api.get(api.context, Receiver::Call, &direct) == PULSE_OK, "native context requires no resource declaration");
    std::vector<uint64_t> subscriptions;
    for (int n = 0; n != 16; ++n) { uint64_t token = 0; if (api.subscribe(api.context, Receiver::Call, &r, &token) == PULSE_OK) subscriptions.push_back(token); }
    id = 99;
    Check(subscriptions.size() == 16 && api.subscribe(api.context, Receiver::Call, &r, &id) == PULSE_ERROR_UNAVAILABLE && id == 0,
        "bounded subscription quota rejects overflow");
    PulseStatus thread_status = PULSE_OK;
    std::thread thread([&] { thread_status = api.get(api.context, Receiver::Call, &direct); }); thread.join();
    Check(thread_status == PULSE_ERROR_STATE, "wrong-thread read rejected before touching owner state");
    for (auto token : subscriptions) api.unsubscribe(api.context, token);
    Receiver first, second; uint64_t a = 0, b = 0;
    api.subscribe(api.context, Receiver::Call, &first, &a); api.subscribe(api.context, Receiver::Call, &second, &b);
    PulseStatus recursive = PULSE_OK;
    first.during = [&] { recursive = api.get(api.context, Receiver::Call, &direct); api.unsubscribe(api.context, b); Check(adapter.CallbackActive(), "callback unload gate is active while DLL callback runs"); };
    adapter.Poll();
    Check(first.calls == 1 && second.calls == 0 && recursive == PULSE_ERROR_STATE, "callback may remove later callback; nested callback reads rejected");
    first.during = [&] { adapter.Poll(); adapter.Close(); };
    ++f.state.selection_epoch; adapter.Poll();
    const auto stopped_calls = first.calls; ++f.state.selection_epoch; adapter.Poll();
    Check(first.calls == stopped_calls && !adapter.CallbackActive() && api.get(api.context, Receiver::Call, &direct) == PULSE_ERROR_STATE,
        "stop from callback suppresses future delivery and clears unload gate");
    Fixture other; ContextAdapter different(other.caller, other.Backend()); auto otherapi = different.Table();
    Check(otherapi.unsubscribe(otherapi.context, a) == PULSE_ERROR_STATE, "foreign attachment subscription rejected");
    Receiver revoked; uint64_t token = 0; otherapi.subscribe(otherapi.context, Receiver::Call, &revoked, &token);
    other.live = false; different.Poll();
    Check(revoked.calls == 0, "owner revocation suppresses queued initial callback");
    Fixture invalid; ContextAdapter malformed(invalid.caller, invalid.Backend()); auto bad = malformed.Table();
    invalid.state.directory = std::string(1, '\xff');
    Check(bad.get(bad.context, Receiver::Call, &direct) == PULSE_ERROR_STATE, "malformed UTF-8 owner state cannot cross ABI");
    Fixture throwing; ContextAdapter resilient(throwing.caller, throwing.Backend()); auto throwingapi = resilient.Table(); Receiver throws;
    throws.during = [] { throw std::runtime_error("plugin"); }; token = 0;
    throwingapi.subscribe(throwingapi.context, Receiver::Call, &throws, &token); resilient.Poll(); resilient.Poll();
    Check(throws.calls == 1 && !resilient.CallbackActive() && throwingapi.get(throwingapi.context, Receiver::Call, &direct) == PULSE_ERROR_STATE,
        "plugin exception stops context callbacks without crossing ABI");
    Fixture selection_fixture;
    selection_fixture.selection.directory = "C:\\fixture";
    selection_fixture.selection.entries = {{"C:\\fixture\\folder", "folder", true}, {"C:\\fixture\\file.txt", "file.txt", false}};
    ContextAdapter selection_adapter(selection_fixture.caller, selection_fixture.Backend());
    auto selection_api = selection_adapter.Table();
    struct SelectionReceiver {
        int calls = 0;
        uint64_t revision = 0;
        std::string directory, first_path;
        uint32_t count = 0, flags = 0;
        std::function<void()> during;
        static void PULSE_CALL Call(void *user, const PulseContextSelectionSnapshot *snapshot) {
            auto &r = *static_cast<SelectionReceiver *>(user);
            ++r.calls; r.revision = snapshot->selection_revision;
            r.directory = Copy(snapshot->directory); r.count = snapshot->entry_count;
            if (r.count) { r.first_path = Copy(snapshot->entries[0].path); r.flags = snapshot->entries[0].flags; }
            if (r.during) r.during();
        }
    } selection_receiver;
    auto read_selection = [&] { return selection_api.get_selection(selection_api.context, SelectionReceiver::Call, &selection_receiver); };
    Check(read_selection() == PULSE_OK && selection_receiver.count == 2 && selection_receiver.flags == PULSE_CONTEXT_ENTRY_DIRECTORY &&
        selection_receiver.first_path == "C:\\fixture\\folder" && selection_receiver.revision, "loaded selection exposes absolute paths, directory flags and revision");
    selection_fixture.selection.entries.clear();
    Check(read_selection() == PULSE_OK && selection_receiver.count == 0 && selection_receiver.directory == "C:\\fixture", "empty ordinary selection retains directory");
    const auto selection_calls = selection_receiver.calls;
    selection_fixture.state.loading = true;
    Check(read_selection() == PULSE_ERROR_STATE && selection_receiver.calls == selection_calls, "loading selection fails without callback");
    selection_fixture.state.loading = false;
    selection_fixture.selection_status = PULSE_ERROR_UNAVAILABLE;
    Check(read_selection() == PULSE_ERROR_UNAVAILABLE && selection_receiver.calls == selection_calls, "unsupported selection fails without callback");
    selection_fixture.selection_status = PULSE_OK;
    selection_fixture.selection.entries.resize(4097);
    Check(read_selection() == PULSE_ERROR_UNAVAILABLE && selection_receiver.calls == selection_calls, "entry limit rejects whole selection without truncation");
    selection_fixture.selection.entries.assign(40, {std::string(120000, 'p'), std::string(1000, 'n'), false});
    Check(read_selection() == PULSE_ERROR_UNAVAILABLE && selection_receiver.calls == selection_calls, "aggregate UTF-8 budget rejects whole selection");
    selection_fixture.selection.entries.clear();
    selection_receiver.during = [&] { Check(read_selection() == PULSE_ERROR_STATE, "recursive selection callback rejected"); selection_adapter.Close(); };
    Check(read_selection() == PULSE_OK && !selection_adapter.CallbackActive(), "stop during selection callback preserves borrowed snapshot lifetime");
    Check(read_selection() == PULSE_ERROR_STATE && selection_api.get_selection(selection_api.context, nullptr, nullptr) == PULSE_ERROR_ARGUMENT,
        "stopped selection and missing callback rejected");
    Fixture cancelled_selection;
    auto cancelled_backend = cancelled_selection.Backend();
    cancelled_backend.capture_selection = [&](ContextSelection &) { cancelled_selection.live = false; return PULSE_OK; };
    ContextAdapter cancelled_adapter(cancelled_selection.caller, std::move(cancelled_backend));
    auto cancelled_api = cancelled_adapter.Table();
    const auto before_cancelled = selection_receiver.calls;
    Check(cancelled_api.get_selection(cancelled_api.context, SelectionReceiver::Call, &selection_receiver) == PULSE_ERROR_STATE &&
        selection_receiver.calls == before_cancelled, "revocation during capture suppresses selection delivery");
    Fixture destroyed_selection;
    destroyed_selection.selection.directory = "C:\\fixture";
    auto destroyed_adapter = std::make_unique<ContextAdapter>(destroyed_selection.caller, destroyed_selection.Backend());
    auto destroyed_api = destroyed_adapter->Table();
    selection_receiver.during = [&] { destroyed_adapter.reset(); };
    Check(destroyed_api.get_selection(destroyed_api.context, SelectionReceiver::Call, &selection_receiver) == PULSE_OK && !destroyed_adapter,
        "selection shared state survives adapter destruction inside callback");
    std::printf("%d PASS, %d FAIL\n", passes, failures);
    return failures ? 1 : 0;
}

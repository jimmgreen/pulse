#include "../ipc/picker_event_state.h"
#include <iostream>
#include <limits>

using namespace pulse::ipc::picker;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) ++failures;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
}
RequestId Id() { RequestId id{}; id[0] = 7; return id; }
EventSnapshot Snapshot(uint64_t generation = 1) {
    EventSnapshot value;
    value.generation = generation;
    value.window = 123;
    value.folder = u"C:\\fixture";
    value.filename = u"one.txt";
    value.selections = {u"C:\\fixture\\one.txt"};
    return value;
}
Message Event(uint64_t id, EventKind kind, EventSnapshot snapshot = Snapshot()) {
    SessionEvent event{id, kind, std::move(snapshot), {}};
    if (kind == EventKind::FolderChanging) event.proposed_folder = u"C:\\other";
    return Message{Id(), event};
}
Message Reply(uint64_t id, uint64_t generation, uint32_t hr = 0) {
    return Message{Id(), EventReply{id, generation, hr}};
}
Message Command(uint64_t id, CommandKind kind, uint64_t generation = 0, uint32_t hr = 0) {
    return Message{Id(), SessionCommand{id, kind, generation, hr}};
}
void Ready(PickerEventState& state) {
    Check(state.Begin(Event(1, EventKind::Ready)), "begin Ready");
    Check(state.Apply(Reply(1, 1)) == ReplyOutcome::Continue, "complete Ready");
}
void Basics() {
    PickerEventState state(Id());
    Check(!state.Begin(Event(1, EventKind::SelectionChanged)), "Ready must be first");
    auto wrong = Event(1, EventKind::Ready);
    wrong.request_id[0] ^= 1;
    Check(!state.Begin(wrong), "wrong request id rejected");
    Check(state.Begin(Event(1, EventKind::Ready)), "valid Ready begins");
    Check(!state.Begin(Event(2, EventKind::SelectionChanged)), "nested event rejected");
    Message output;
    Check(state.Command(Command(1, CommandKind::GetWindow), output) &&
        std::get<CommandReply>(output.body).snapshot.window == 123, "GetWindow during pending callback");
    Check(state.Command(Command(2, CommandKind::Query, 1), output) &&
        std::get<CommandReply>(output.body).snapshot == Snapshot(), "Query pending snapshot");
    const auto sentinel = output;
    Check(!state.Command(Command(2, CommandKind::Query), output) && output == sentinel, "command replay leaves output unchanged");
    auto bad_command = Command(3, CommandKind::Query);
    bad_command.request_id[0] ^= 1;
    Check(!state.Command(bad_command, output) && output == sentinel, "wrong request command leaves state unchanged");
    Check(!state.Command(Command(3, CommandKind::Close, 1), output) && output == sentinel,
        "Close with generation rejected before mutation");
    Check(state.Command(Command(3, CommandKind::Query, 99), output) &&
        std::get<CommandReply>(output.body).hresult == 0x800704d5u &&
        std::get<CommandReply>(output.body).snapshot == EventSnapshot{}, "stale Query returns retry with empty snapshot");
    Check(state.Apply(Reply(2, 1)) == ReplyOutcome::Invalid && state.Pending(), "wrong event id preserves pending");
    Check(state.Apply(Reply(1, 2)) == ReplyOutcome::Invalid && state.Pending(), "wrong generation preserves pending");
    auto bad_reply = Reply(1, 1);
    bad_reply.request_id[0] ^= 1;
    Check(state.Apply(bad_reply) == ReplyOutcome::Invalid && state.Pending(), "wrong request reply preserves pending");
    Check(state.Apply(Reply(1, 1)) == ReplyOutcome::Continue, "matching reply continues");
    Check(state.Apply(Reply(1, 1)) == ReplyOutcome::Invalid, "reply replay rejected");
    Check(!state.Begin(Event(1, EventKind::SelectionChanged)), "event replay rejected");
    Check(!state.Begin(Event(2, EventKind::Ready)), "second Ready rejected");
    auto changed = Snapshot(2);
    changed.window++;
    Check(!state.Begin(Event(2, EventKind::SelectionChanged, changed)) && !state.Refresh(changed), "window identity cannot change");
    Check(!state.Begin(Event(2, EventKind::FileOk)), "empty FileOk candidates rejected");
    Check(!state.Refresh(Snapshot()), "refresh requires strictly newer generation");
    auto drift = Snapshot();
    drift.filename = u"changed.txt";
    Check(!state.Begin(Event(2, EventKind::SelectionChanged, drift)) && state.Snapshot() == Snapshot() && !state.Pending(),
        "same generation with changed snapshot is rejected atomically");
    Check(state.Begin(Event(2, EventKind::SelectionChanged)) && state.Apply(Reply(2, 1)) == ReplyOutcome::Continue,
        "same generation with identical full snapshot is allowed");
    Check(state.Refresh(Snapshot(3)) && !state.Begin(Event(3, EventKind::SelectionChanged, Snapshot(2))),
        "event generation cannot regress");
}
void VetoAndAccept() {
    PickerEventState state(Id());
    Ready(state);
    auto snapshot = Snapshot(2);
    snapshot.filename = u"new.txt";
    Check(state.Begin(Event(2, EventKind::FolderChanging, snapshot)), "folder change begins");
    Check(state.Apply(Reply(2, 2, 1)) == ReplyOutcome::Veto && state.Snapshot() == snapshot,
        "folder veto retains current committed snapshot");
    snapshot.candidates = {u"C:\\fixture\\new.txt"};
    snapshot.generation = 3;
    Check(state.Begin(Event(3, EventKind::FileOk, snapshot)), "FileOk begins");
    Check(state.Apply(Reply(3, 3, 1)) == ReplyOutcome::Veto && state.Snapshot().generation == 4 &&
        state.Snapshot().candidates.empty(), "FileOk veto invalidates candidates");
    snapshot.generation = 5;
    Check(state.Begin(Event(4, EventKind::FileOk, snapshot)), "fresh FileOk begins");
    Check(state.Apply(Reply(4, 5)) == ReplyOutcome::Accept && state.AcceptedCandidates() == snapshot.candidates,
        "only matching S_OK accepts candidates");
    Check(!state.Begin(Event(5, EventKind::SelectionChanged, Snapshot(4))) && !state.Refresh(Snapshot(4)), "accepted state is terminal");
}
void CloseWins() {
    PickerEventState state(Id());
    Ready(state);
    auto snapshot = Snapshot(2);
    snapshot.candidates = {u"C:\\fixture\\one.txt"};
    Check(state.Begin(Event(2, EventKind::FileOk, snapshot)), "close test FileOk begins");
    Message output;
    Check(state.Command(Command(1, CommandKind::Close), output) && state.Closed() && state.Result() == 0,
        "Close S_OK closes without accepting");
    Check(state.Command(Command(2, CommandKind::Close, 0, 0x80004005u), output) && state.Result() == 0,
        "first Close HRESULT remains latched");
    auto wrong = Reply(2, 2);
    wrong.request_id[0] ^= 1;
    Check(state.Apply(wrong) == ReplyOutcome::Invalid && state.Pending() && state.AcceptedCandidates().empty(),
        "closed wrong request reply remains invalid without candidates");
    Check(state.Apply(Reply(3, 2)) == ReplyOutcome::Invalid && state.Apply(Reply(2, 1)) == ReplyOutcome::Invalid &&
        state.Apply(Reply(0, 2)) == ReplyOutcome::Invalid && state.Pending() && state.Result() == 0 &&
        state.AcceptedCandidates().empty(), "closed invalid reply preserves pending and first result");
    Check(state.Apply(Reply(2, 2, 0x80070005u)) == ReplyOutcome::Closed && state.Result() == 0 &&
        state.AcceptedCandidates().empty(), "Close wins over later callback failure");
    Check(state.Command(Command(3, CommandKind::Query), output) &&
        std::get<CommandReply>(output.body).hresult == 0x8007139fu, "closed Query returns invalid state");
    Check(!state.Begin(Event(3, EventKind::SelectionChanged, Snapshot(2))) && !state.Refresh(Snapshot(2)), "closed state cannot reopen");
}
void StaleAndOverflow() {
    PickerEventState state(Id());
    Ready(state);
    auto snapshot = Snapshot(2);
    snapshot.candidates = {u"C:\\fixture\\one.txt"};
    Check(state.Begin(Event(2, EventKind::FileOk, snapshot)) && state.Refresh(Snapshot(3)), "pending callback snapshot refreshes");
    Check(state.Apply(Reply(2, 2)) == ReplyOutcome::Retry && state.Snapshot().generation == 4 &&
        state.AcceptedCandidates().empty() && state.Snapshot().candidates.empty(), "stale approval never accepts");
    snapshot.generation = (std::numeric_limits<uint64_t>::max)();
    Check(state.Begin(Event(3, EventKind::FileOk, snapshot)), "maximum generation begins");
    Check(state.Apply(Reply(3, snapshot.generation, 1)) == ReplyOutcome::Invalid && state.Pending() && state.Snapshot() == snapshot,
        "veto generation overflow leaves state untouched");
    PickerEventState stale(Id());
    Ready(stale);
    auto initial = Snapshot(2);
    initial.candidates = {u"C:\\fixture\\one.txt"};
    Check(stale.Begin(Event(2, EventKind::FileOk, initial)) && stale.Refresh(snapshot), "overflow stale event begins");
    Check(stale.Apply(Reply(2, 2)) == ReplyOutcome::Invalid && stale.Pending() && stale.Snapshot() == snapshot,
        "stale generation overflow leaves state untouched");
}
}
int main() {
    Basics(); VetoAndAccept(); CloseWins(); StaleAndOverflow();
    for (const uint32_t hr : {0x80070005u, 2u, 1u}) {
        PickerEventState state(Id());
        Check(state.Begin(Event(1, EventKind::Ready)), "failure fixture begins");
        Check(state.Apply(Reply(1, 1, hr)) == ReplyOutcome::Failed &&
            state.Result() == ((hr & 0x80000000u) ? hr : 0x8000ffffu), "failure exact HRESULT or unexpected success");
    }
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures ? 1 : 0;
}

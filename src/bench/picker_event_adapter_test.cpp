#include "../app/picker_event_adapter.h"
#include <iostream>
using namespace pulse::ipc::picker;
using namespace pulse::app;
using pulse::ui::PickerDialogEvent;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* name) { ++checks; if (!ok) ++failures; std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n'; }
}
int main() {
    RequestId id{}; id[0] = 1;
    PickerEventAdapter adapter(id);
    pulse::ui::PickerDialogSnapshot snapshot;
    snapshot.generation = 1;
    HWND window = reinterpret_cast<HWND>(uintptr_t{123});
    Message output{id, Cancel{}};
    const auto sentinel = output;
    for (auto kind : {PickerDialogEvent::Snapshot, PickerDialogEvent::TypeChanged, PickerDialogEvent::FolderChanging, PickerDialogEvent::FolderChanged, PickerDialogEvent::Shown})
        Check(adapter.Notify(window, kind, snapshot, L"", output) == PickerAdaptResult::UnsupportedBeforeReady && output == sentinel, "initial callback explicitly unsupported");
    Check(adapter.Notify(window, PickerDialogEvent::Ready, snapshot, L"", output) == PickerAdaptResult::Invalid, "empty folder cannot establish Ready");
    snapshot.folder = L"C:\\fixture";
    snapshot.filename = L"\u6587\U0001f600.txt";
    snapshot.selections = {L"C:\\fixture\\one.txt", L"C:\\fixture\\two.txt"};
    snapshot.selection = snapshot.selections.front();
    Check(adapter.Notify(window, PickerDialogEvent::Ready, snapshot, L"", output) == PickerAdaptResult::Event &&
        std::get<SessionEvent>(output.body).kind == EventKind::Ready && std::get<SessionEvent>(output.body).snapshot.window == 123 &&
        std::get<SessionEvent>(output.body).snapshot.filename == u"\u6587\U0001f600.txt" &&
        std::get<SessionEvent>(output.body).snapshot.selections.size() == 2, "Ready preserves UTF16 HWND and multiselect");
    Check(adapter.Apply(Message{id, EventReply{1, 1, 0}}) == ReplyOutcome::Continue, "Ready acknowledged");
    const auto ready = output;
    Check(adapter.Notify(window, PickerDialogEvent::Snapshot, snapshot, L"", output) == PickerAdaptResult::Unchanged && output == ready, "duplicate Snapshot is explicit unchanged");
    snapshot.filename = L"changed.txt";
    Check(adapter.Notify(window, PickerDialogEvent::Snapshot, snapshot, L"", output) == PickerAdaptResult::Invalid, "same generation mutation rejected");
    snapshot.generation = 2;
    Check(adapter.Notify(window, PickerDialogEvent::Snapshot, snapshot, L"", output) == PickerAdaptResult::SnapshotUpdated && output == ready, "Snapshot refresh emits no callback frame");
    uint64_t event_id = 1;
    for (auto kind : {PickerDialogEvent::FolderChanging, PickerDialogEvent::FolderChanged, PickerDialogEvent::SelectionChanged, PickerDialogEvent::TypeChanged}) {
        const auto proposed = kind == PickerDialogEvent::FolderChanging ? L"C:\\next" : L"";
        Check(adapter.Notify(window, kind, snapshot, proposed, output) == PickerAdaptResult::Event, "ordinary callback maps");
        ++event_id;
        const auto& event = std::get<SessionEvent>(output.body);
        Check(event.event_id == event_id && event.snapshot.generation == 2 &&
            static_cast<uint32_t>(event.kind) == event_id &&
            (kind != PickerDialogEvent::FolderChanging || event.proposed_folder == u"C:\\next"), "mapping preserves event ordering and proposed folder");
        Check(adapter.Apply(Message{id, EventReply{event_id, 2, 0}}) == ReplyOutcome::Continue, "mapped callback acknowledged");
    }
    for (auto kind : {PickerDialogEvent::ButtonClicked, PickerDialogEvent::ItemSelected, PickerDialogEvent::ControlActivating, PickerDialogEvent::OpenDropDownSelected, PickerDialogEvent::CheckToggled, PickerDialogEvent::EditChanged, PickerDialogEvent::Shown})
        Check(adapter.Notify(window, kind, snapshot, L"", output) == PickerAdaptResult::UnsupportedEvent, "control and Shown events are not silently mapped");
    snapshot.control_id = 1;
    Check(adapter.Notify(window, PickerDialogEvent::TypeChanged, snapshot, L"", output) == PickerAdaptResult::Invalid, "control payload on ordinary event rejected");
    snapshot.control_id = 0;
    snapshot.selection = L"wrong";
    Check(adapter.Notify(window, PickerDialogEvent::Snapshot, snapshot, L"", output) == PickerAdaptResult::Invalid, "inconsistent singular selection rejected");
    snapshot.selection = snapshot.selections.front();
    Check(adapter.Notify(reinterpret_cast<HWND>(uintptr_t{124}), PickerDialogEvent::Snapshot, snapshot, L"", output) == PickerAdaptResult::Invalid, "window identity change rejected");
    snapshot.generation = 3; snapshot.candidates = snapshot.selections; snapshot.candidate = snapshot.candidates.front();
    Check(adapter.Notify(window, PickerDialogEvent::FileOk, snapshot, L"", output) == PickerAdaptResult::Event &&
        std::get<SessionEvent>(output.body).snapshot.candidates.size() == 2, "FileOk preserves all candidates");
    ++event_id;
    snapshot.generation = 4; snapshot.candidates.clear(); snapshot.candidate.clear();
    Check(adapter.Notify(window, PickerDialogEvent::Snapshot, snapshot, L"", output) == PickerAdaptResult::SnapshotUpdated, "pending FileOk snapshot refreshes");
    Check(adapter.Apply(Message{id, EventReply{event_id, 3, 0}}) == ReplyOutcome::Retry && adapter.State().AcceptedCandidates().empty(), "stale FileOk approval never accepts");
    Check(adapter.Notify(window, PickerDialogEvent::Snapshot, snapshot, L"", output) == PickerAdaptResult::Invalid, "UI generation behind protocol retry is rejected");
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures ? 1 : 0;
}

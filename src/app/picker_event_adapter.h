#pragma once

#include "../ipc/picker_event_state.h"
#include "../ui/folder_picker_dialog.h"

namespace pulse::app {
enum class PickerAdaptResult { Event, SnapshotUpdated, Unchanged, UnsupportedBeforeReady, UnsupportedEvent, Invalid };

// Experimental mapping only; no production UI callback is installed here.
class PickerEventAdapter {
public:
    explicit PickerEventAdapter(ipc::picker::RequestId id) : id_(id), state_(id) {}
    PickerAdaptResult Notify(HWND window, ui::PickerDialogEvent kind, const ui::PickerDialogSnapshot& snapshot,
                             const std::wstring& proposed, ipc::picker::Message& output);
    ipc::picker::ReplyOutcome Apply(const ipc::picker::Message& reply);
    bool Command(const ipc::picker::Message& command, ipc::picker::Message& output);
    const ipc::picker::PickerEventState& State() const { return state_; }
private:
    ipc::picker::RequestId id_;
    ipc::picker::PickerEventState state_;
    uint64_t event_id_ = 0;
    bool ready_ = false, terminal_ = false;
};
}

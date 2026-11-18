#include "picker_event_adapter.h"

#include <limits>

namespace pulse::app {
namespace {
std::u16string Copy(const std::wstring& text) {
    static_assert(sizeof(wchar_t) == sizeof(char16_t));
    return std::u16string(text.begin(), text.end());
}
bool CopyPaths(const std::wstring& first, const std::vector<std::wstring>& paths, std::vector<std::u16string>& output) {
    if (first != (paths.empty() ? std::wstring{} : paths.front())) return false;
    for (const auto& path : paths) output.push_back(Copy(path));
    return true;
}
}
PickerAdaptResult PickerEventAdapter::Notify(HWND window, ui::PickerDialogEvent kind,
    const ui::PickerDialogSnapshot& source, const std::wstring& proposed, ipc::picker::Message& output) {
    using namespace ipc::picker;
    using ui::PickerDialogEvent;
    if (!ready_ && kind != PickerDialogEvent::Ready) return PickerAdaptResult::UnsupportedBeforeReady;
    if (terminal_ || state_.Closed()) return PickerAdaptResult::Invalid;
    EventKind mapped;
    switch (kind) {
    case PickerDialogEvent::Snapshot: mapped = EventKind::SelectionChanged; break;
    case PickerDialogEvent::Ready: mapped = EventKind::Ready; break;
    case PickerDialogEvent::FolderChanging: mapped = EventKind::FolderChanging; break;
    case PickerDialogEvent::FolderChanged: mapped = EventKind::FolderChanged; break;
    case PickerDialogEvent::SelectionChanged: mapped = EventKind::SelectionChanged; break;
    case PickerDialogEvent::TypeChanged: mapped = EventKind::TypeChanged; break;
    case PickerDialogEvent::FileOk: mapped = EventKind::FileOk; break;
    default: return PickerAdaptResult::UnsupportedEvent;
    }
    if (source.control_id || source.item_id || !source.control_text.empty() ||
        source.filter_index > (std::numeric_limits<uint32_t>::max)() ||
        event_id_ == (std::numeric_limits<uint64_t>::max)()) return PickerAdaptResult::Invalid;
    EventSnapshot snapshot;
    snapshot.generation = source.generation;
    snapshot.window = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(window));
    snapshot.filter_index = static_cast<uint32_t>(source.filter_index);
    snapshot.folder = Copy(source.folder);
    snapshot.filename = Copy(source.filename);
    if (!CopyPaths(source.selection, source.selections, snapshot.selections) ||
        !CopyPaths(source.candidate, source.candidates, snapshot.candidates)) return PickerAdaptResult::Invalid;
    Message message{id_, SessionEvent{event_id_ + 1, mapped, snapshot, Copy(proposed)}};
    std::vector<uint8_t> encoded;
    if (Encode(message, encoded) != CodecError::None) return PickerAdaptResult::Invalid;
    if (kind == PickerDialogEvent::Snapshot) {
        if (snapshot == state_.Snapshot()) return PickerAdaptResult::Unchanged;
        return state_.Refresh(snapshot) ? PickerAdaptResult::SnapshotUpdated : PickerAdaptResult::Invalid;
    }
    if (!state_.Begin(message)) return PickerAdaptResult::Invalid;
    ++event_id_;
    ready_ = true;
    output = std::move(message);
    return PickerAdaptResult::Event;
}
ipc::picker::ReplyOutcome PickerEventAdapter::Apply(const ipc::picker::Message& reply) {
    using ipc::picker::ReplyOutcome;
    const auto result = state_.Apply(reply);
    if (result == ReplyOutcome::Accept || result == ReplyOutcome::Failed || result == ReplyOutcome::Closed) terminal_ = true;
    return result;
}
bool PickerEventAdapter::Command(const ipc::picker::Message& command, ipc::picker::Message& output) {
    return state_.Command(command, output);
}
}

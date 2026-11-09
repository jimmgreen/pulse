#pragma once

#include "picker_protocol.h"
#include <limits>
#include <optional>

namespace pulse::ipc::picker {

enum class ReplyOutcome { Invalid, Continue, Veto, Retry, Failed, Closed, Accept };

// Deterministic protocol model only; this does not enable a production UI path.
class PickerEventState {
public:
    explicit PickerEventState(RequestId request_id) : request_id_(request_id) {}

    bool Begin(const Message& message) {
        const auto* event = std::get_if<SessionEvent>(&message.body);
        if (!event || !Valid(message) || terminal_ || pending_ || event->event_id <= last_event_id_ ||
            (!ready_ && event->kind != EventKind::Ready) || (ready_ && event->kind == EventKind::Ready) ||
            (ready_ && (event->snapshot.window != snapshot_.window || event->snapshot.generation < snapshot_.generation ||
                (event->snapshot.generation == snapshot_.generation && event->snapshot != snapshot_))))
            return false;
        if (event->kind == EventKind::FileOk && event->snapshot.candidates.empty()) return false;
        snapshot_ = event->snapshot;
        pending_ = *event;
        last_event_id_ = event->event_id;
        ready_ = true;
        return true;
    }

    bool Refresh(const EventSnapshot& snapshot) {
        if (!ready_ || terminal_ || snapshot.window != snapshot_.window || snapshot.generation <= snapshot_.generation)
            return false;
        SessionEvent validation{last_event_id_, EventKind::SelectionChanged, snapshot, {}};
        if (!Valid(Message{request_id_, validation})) return false;
        snapshot_ = snapshot;
        return true;
    }

    ReplyOutcome Apply(const Message& message) {
        const auto* reply = std::get_if<EventReply>(&message.body);
        if (!reply || !Valid(message) || !pending_ || reply->event_id != pending_->event_id ||
            reply->generation != pending_->snapshot.generation) return ReplyOutcome::Invalid;
        if (closed_) {
            pending_.reset();
            return ReplyOutcome::Closed;
        }
        if (snapshot_.generation != pending_->snapshot.generation) {
            if (!CanAdvance()) return ReplyOutcome::Invalid;
            snapshot_.candidates.clear();
            ++snapshot_.generation;
            pending_.reset();
            return ReplyOutcome::Retry;
        }
        const auto kind = pending_->kind;
        if (reply->hresult == 1 && (kind == EventKind::FileOk || kind == EventKind::FolderChanging)) {
            if (kind == EventKind::FileOk && !CanAdvance()) return ReplyOutcome::Invalid;
            if (kind == EventKind::FileOk) {
                snapshot_.candidates.clear();
                ++snapshot_.generation;
            }
            pending_.reset();
            return ReplyOutcome::Veto;
        }
        pending_.reset();
        if (reply->hresult != 0) {
            result_ = (reply->hresult & 0x80000000u) ? reply->hresult : 0x8000ffffu;
            terminal_ = true;
            return ReplyOutcome::Failed;
        }
        if (kind == EventKind::FileOk) {
            accepted_candidates_ = snapshot_.candidates;
            terminal_ = true;
            return ReplyOutcome::Accept;
        }
        return ReplyOutcome::Continue;
    }

    bool Command(const Message& message, Message& output) {
        const auto* command = std::get_if<SessionCommand>(&message.body);
        if (!command || !Valid(message) || command->command_id <= last_command_id_) return false;
        CommandReply reply;
        reply.command_id = command->command_id;
        reply.kind = command->kind;
        bool close = false;
        if (command->kind == CommandKind::Close && ready_ && (!terminal_ || closed_)) {
            close = !closed_;
        } else if (!ready_ || terminal_) reply.hresult = 0x8007139fu;
        else if (command->expected_generation && command->expected_generation != snapshot_.generation)
            reply.hresult = 0x800704d5u;
        else reply.snapshot = snapshot_;
        Message answer{request_id_, reply};
        if (!Valid(answer)) return false;
        last_command_id_ = command->command_id;
        if (close) {
            closed_ = terminal_ = true;
            result_ = command->hresult;
            accepted_candidates_.clear();
        }
        output = std::move(answer);
        return true;
    }

    const EventSnapshot& Snapshot() const { return snapshot_; }
    bool Pending() const { return pending_.has_value(); }
    bool Closed() const { return closed_; }
    uint32_t Result() const { return result_; }
    const std::vector<std::u16string>& AcceptedCandidates() const { return accepted_candidates_; }

private:
    bool Valid(const Message& message) const {
        if (message.request_id != request_id_) return false;
        std::vector<uint8_t> bytes;
        return Encode(message, bytes) == CodecError::None;
    }
    bool CanAdvance() const { return snapshot_.generation != (std::numeric_limits<uint64_t>::max)(); }
    RequestId request_id_;
    EventSnapshot snapshot_;
    std::optional<SessionEvent> pending_;
    std::vector<std::u16string> accepted_candidates_;
    uint64_t last_event_id_ = 0, last_command_id_ = 0;
    uint32_t result_ = 0;
    bool ready_ = false, terminal_ = false, closed_ = false;
};

} // namespace pulse::ipc::picker

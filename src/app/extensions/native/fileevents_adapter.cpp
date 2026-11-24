#include "fileevents_adapter.h"
#include "diagnostics_adapter.h"

#include <atomic>
#include <cstring>
#include <set>
#include <utility>

namespace pulse::extensions::native {

struct FileEventsAdapter::State : std::enable_shared_from_this<State> {
    DWORD thread = GetCurrentThreadId();
    std::string package_id;
    std::function<bool()> alive;
    std::shared_ptr<DiagnosticsSink> diagnostics;
    fileevents::Hub* hub = nullptr;
    uint64_t owner_key = 0;
    int callbacks = 0;
    bool closed = false;
    std::set<PulseFileEventSubscription> owned;

    State(std::string pkg, std::function<bool()> live, std::shared_ptr<DiagnosticsSink> sink, fileevents::Hub* h)
        : package_id(std::move(pkg)),
          alive(std::move(live)),
          diagnostics(std::move(sink)),
          hub(h ? h : &fileevents::Hub::Instance()) {
        static std::atomic<uint64_t> next_owner{1};
        owner_key = next_owner.fetch_add(1);
    }

    bool Valid() const noexcept { return GetCurrentThreadId() == thread && !closed && (!alive || alive()); }

    PulseStatus Fail(PulseStatus status, std::string_view op, std::string_view field, std::string_view msg) const {
        if (diagnostics) diagnostics->Record(status, PULSE_FILEEVENTS_INTERFACE_NAME, op, "", field, msg);
        return status;
    }

    template <class F>
    static PulseStatus Guard(void* context, F action) noexcept {
        if (!context) return PULSE_ERROR_ARGUMENT;
        try {
            auto self = static_cast<State*>(context)->shared_from_this();
            if (GetCurrentThreadId() != self->thread)
                return self->Fail(PULSE_ERROR_STATE, "", "thread", "pulse.fileevents must be called on the owner UI thread");
            if (!self->Valid()) return PULSE_ERROR_STATE;
            return action(*self);
        } catch (...) {
            return PULSE_ERROR_STATE;
        }
    }

    static PulseStatus PULSE_CALL Subscribe(void* context, uint32_t kinds, PulseFileEventFn callback, void* user,
                                            PulseFileEventSubscription* out) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (out) *out = 0;
            if (!callback || !out)
                return self.Fail(PULSE_ERROR_ARGUMENT, "subscribe", "callback", "callback and out_subscription are required");
            if ((kinds & ~static_cast<uint32_t>(PULSE_FILE_EVENT_ALL_KINDS)) != 0)
                return self.Fail(PULSE_ERROR_ARGUMENT, "subscribe", "kinds", "kinds must be PULSE_FILE_EVENT_MASK bits or 0");
            if (self.owned.size() >= kMaxSubscriptions)
                return self.Fail(PULSE_ERROR_STATE, "subscribe", "", "a session may hold at most 8 file event subscriptions");
            std::weak_ptr<State> weak = self.weak_from_this();
            auto id = std::make_shared<uint64_t>(0);
            const uint64_t handle = self.hub->Add(self.owner_key, kinds, [weak, callback, user, id](const PulseFileEvent& event) {
                auto state = weak.lock();
                if (!state || !state->Valid() || !state->owned.contains(*id)) return;
                ++state->callbacks;
                try { callback(user, &event); } catch (...) {}
                --state->callbacks;
            });
            *id = handle;
            self.owned.insert(handle);
            *out = handle;
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL Unsubscribe(void* context, PulseFileEventSubscription subscription) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!self.owned.erase(subscription) || !self.hub->Remove(subscription))
                return self.Fail(PULSE_ERROR_ARGUMENT, "unsubscribe", "subscription", "unknown subscription");
            return PULSE_OK;
        });
    }
};

FileEventsAdapter::FileEventsAdapter(std::string package_id, std::function<bool()> alive,
                                     std::shared_ptr<DiagnosticsSink> diagnostics, fileevents::Hub* hub)
    : state_(std::make_shared<State>(std::move(package_id), std::move(alive), std::move(diagnostics), hub)) {}

FileEventsAdapter::~FileEventsAdapter() { Close(); }

PulseStatus FileEventsAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    try {
        if (!state_->Valid()) return PULSE_ERROR_STATE;
        if (version != PULSE_FILEEVENTS_VERSION_1) return PULSE_ERROR_ABI;
        if (!out || size < sizeof(PulseFileEventsV1)) return PULSE_ERROR_ARGUMENT;
        const PulseFileEventsV1 table{
            sizeof(PulseFileEventsV1),
            PULSE_FILEEVENTS_VERSION_1,
            state_.get(),
            State::Subscribe,
            State::Unsubscribe,
        };
        std::memcpy(out, &table, sizeof(table));
        return PULSE_OK;
    } catch (...) {
        return PULSE_ERROR_STATE;
    }
}

void FileEventsAdapter::Close() noexcept {
    if (!state_ || GetCurrentThreadId() != state_->thread || state_->closed) return;
    state_->closed = true;
    state_->owned.clear();
    try { state_->hub->RemoveOwner(state_->owner_key); } catch (...) {}
}

bool FileEventsAdapter::CallbackActive() const noexcept { return state_ && state_->callbacks > 0; }

} // namespace pulse::extensions::native

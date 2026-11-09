#include "context_adapter.h"
#include <atomic>
#include <cstring>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>
#include <windows.h>
namespace pulse::extensions::native {
namespace {
std::atomic<uint64_t> next_subscription{0};
uint64_t Allocate() {
    auto value = next_subscription.load();
    while (value != UINT64_MAX) {
        if (next_subscription.compare_exchange_weak(value, value + 1)) return value + 1;
    }
    return 0;
}
PulseStringView View(const std::string &text) {
    return {text.data(), static_cast<uint32_t>(text.size())};
}
bool ValidText(const std::string &text) {
    return text.size() <= 131072 && text.find('\0') == std::string::npos &&
        (text.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            text.data(), static_cast<int>(text.size()), nullptr, 0) > 0);
}
}
struct ContextAdapter::State : std::enable_shared_from_this<State> {
    struct Subscription {
        PulseContextChangeFn callback;
        void *user;
        uint64_t delivered = 0, navigation = 0, selection = 0, view = 0;
    };
    CallerBinding caller;
    ContextBackend backend;
    DWORD thread = GetCurrentThreadId();
    bool closed = false, active = false, initialized = false;
    uint64_t revision = 0, navigation = 0, selection = 0, view_revision = 0;
    ContextObservation current;
    std::map<uint64_t, Subscription> subscriptions;
    State(CallerBinding binding, ContextBackend functions) : caller(std::move(binding)), backend(std::move(functions)) {}
    bool Ready() const {
        return !closed && GetCurrentThreadId() == thread && backend.live && backend.live(caller);
    }
    uint32_t Capture() {
        auto value = backend.capture();
        if (!ValidText(value.directory) || !ValidText(value.selection_name) || !ValidText(value.view_mode))
            throw std::runtime_error("invalid_context_text");
        if (value.loading) { value.selection_count = 0; value.selection_name.clear(); }
        uint32_t changes = initialized ? 0u : PULSE_CONTEXT_INITIAL;
        const bool navigated = !initialized || value.tab_key != current.tab_key ||
            value.directory != current.directory || value.loading != current.loading;
        const bool selected = navigated || value.snapshot_key != current.snapshot_key ||
            value.selection_epoch != current.selection_epoch || value.selection_count != current.selection_count ||
            value.selection_name != current.selection_name;
        if (navigated) changes |= PULSE_CONTEXT_NAVIGATION;
        if (selected) changes |= PULSE_CONTEXT_SELECTION;
        if (!initialized || value.view_mode != current.view_mode) changes |= PULSE_CONTEXT_VIEW;
        if (!changes) return 0;
        if (revision == UINT64_MAX || (navigated && navigation == UINT64_MAX) ||
            (selected && selection == UINT64_MAX)) throw std::runtime_error("context_revision_exhausted");
        ++revision;
        if (changes & PULSE_CONTEXT_VIEW) view_revision = revision;
        if (navigated) ++navigation;
        if (selected) ++selection;
        current = std::move(value);
        initialized = true;
        return changes;
    }
    void Invoke(PulseContextChangeFn callback, void *user, uint32_t changes) {
        // Copy before calling the DLL: a callback may read/update host state or stop.
        const auto value = current;
        PulseContextSnapshot snapshot{sizeof(PulseContextSnapshot), changes, revision, navigation, selection,
            View(value.directory), View(value.selection_name), View(value.view_mode), value.selection_count,
            value.loading ? 1u : 0u, 0};
        active = true;
        try { callback(user, &snapshot); } catch (...) { Close(); }
        active = false;
    }
    void Close() { closed = true; subscriptions.clear(); }
    static PulseStatus PULSE_CALL Get(void *context, PulseContextChangeFn callback, void *user) {
        try {
            auto *raw = static_cast<State *>(context);
            if (!raw || !callback) return PULSE_ERROR_ARGUMENT;
            const auto self = raw->shared_from_this();
            if (self->active || !self->Ready() || !self->backend.capture) return PULSE_ERROR_STATE;
            const auto changes = self->Capture();
            self->Invoke(callback, user, changes);
            return PULSE_OK;
        } catch (...) { return PULSE_ERROR_STATE; }
    }
    static PulseStatus PULSE_CALL GetSelection(void *context, PulseContextSelectionFn callback, void *user) {
        try {
            auto *raw = static_cast<State *>(context);
            if (!raw || !callback) return PULSE_ERROR_ARGUMENT;
            const auto self = raw->shared_from_this();
            if (self->active || !self->Ready() || !self->backend.capture) return PULSE_ERROR_STATE;
            if (!self->backend.capture_selection) return PULSE_ERROR_UNAVAILABLE;
            self->Capture();
            if (self->current.loading) return PULSE_ERROR_STATE;
            ContextSelection value;
            const auto status = self->backend.capture_selection(value);
            if (status != PULSE_OK) return status;
            if (!self->Ready()) return PULSE_ERROR_STATE;
            if (value.entries.size() > 4096) return PULSE_ERROR_UNAVAILABLE;
            size_t bytes = value.directory.size();
            if (!ValidText(value.directory)) return PULSE_ERROR_STATE;
            std::vector<PulseContextSelectionEntry> entries;
            entries.reserve(value.entries.size());
            for (const auto &entry : value.entries) {
                bytes += entry.path.size() + entry.name.size();
                if (bytes > 4 * 1024 * 1024) return PULSE_ERROR_UNAVAILABLE;
                if (!ValidText(entry.path) || !ValidText(entry.name)) return PULSE_ERROR_STATE;
                entries.push_back({sizeof(PulseContextSelectionEntry), entry.directory ? PULSE_CONTEXT_ENTRY_DIRECTORY : 0u,
                    View(entry.path), View(entry.name)});
            }
            PulseContextSelectionSnapshot snapshot{sizeof(PulseContextSelectionSnapshot), self->selection,
                View(value.directory), entries.data(), static_cast<uint32_t>(entries.size())};
            self->active = true;
            try { callback(user, &snapshot); }
            catch (...) { self->Close(); self->active = false; return PULSE_ERROR_STATE; }
            self->active = false;
            return PULSE_OK;
        } catch (...) { return PULSE_ERROR_STATE; }
    }
    static PulseStatus PULSE_CALL Subscribe(void *context, PulseContextChangeFn callback, void *user,
        PulseContextSubscriptionId *out) {
        if (out) *out = 0;
        try {
            auto *self = static_cast<State *>(context);
            if (!self || !callback || !out) return PULSE_ERROR_ARGUMENT;
            if (!self->Ready()) return PULSE_ERROR_STATE;
            if (self->subscriptions.size() >= 16) return PULSE_ERROR_UNAVAILABLE;
            const auto id = Allocate();
            if (!id) return PULSE_ERROR_UNAVAILABLE;
            self->subscriptions.emplace(id, Subscription{callback, user});
            *out = id;
            return PULSE_OK;
        } catch (...) { return PULSE_ERROR_STATE; }
    }
    static PulseStatus PULSE_CALL Unsubscribe(void *context, PulseContextSubscriptionId id) {
        try {
            auto *self = static_cast<State *>(context);
            if (!self || !id) return PULSE_ERROR_ARGUMENT;
            if (GetCurrentThreadId() != self->thread || self->closed) return PULSE_ERROR_STATE;
            return self->subscriptions.erase(id) ? PULSE_OK : PULSE_ERROR_STATE;
        } catch (...) { return PULSE_ERROR_STATE; }
    }
    void Poll() {
        if (active || GetCurrentThreadId() != thread || closed || subscriptions.empty()) return;
        if (!Ready()) { Close(); return; }
        Capture();
        std::vector<uint64_t> ids;
        for (const auto &[id, subscription] : subscriptions)
            if (subscription.delivered != revision) ids.push_back(id);
        for (const auto id : ids) {
            if (closed || !Ready()) break;
            const auto found = subscriptions.find(id);
            if (found == subscriptions.end()) continue;
            const auto callback = found->second.callback;
            const auto user = found->second.user;
            uint32_t flags = found->second.delivered ? 0u : PULSE_CONTEXT_INITIAL;
            if (found->second.navigation != navigation) flags |= PULSE_CONTEXT_NAVIGATION;
            if (found->second.selection != selection) flags |= PULSE_CONTEXT_SELECTION;
            if (found->second.view != view_revision) flags |= PULSE_CONTEXT_VIEW;
            found->second.delivered = revision;
            found->second.navigation = navigation;
            found->second.selection = selection;
            found->second.view = view_revision;
            Invoke(callback, user, flags);
        }
    }
};
ContextAdapter::ContextAdapter(CallerBinding caller, ContextBackend backend)
    : state_(std::make_shared<State>(std::move(caller), std::move(backend))) {}
ContextAdapter::~ContextAdapter() { Close(); }
PulseContextV1 ContextAdapter::Table() const noexcept {
    return {sizeof(PulseContextV1), PULSE_CONTEXT_VERSION_1, state_.get(), State::Get, State::Subscribe, State::Unsubscribe, State::GetSelection};
}
PulseStatus ContextAdapter::Query(uint32_t version, void *table, uint32_t size) const noexcept {
    if (version != PULSE_CONTEXT_VERSION_1) return PULSE_ERROR_ABI;
    if (!table || size < sizeof(PulseContextV1)) return PULSE_ERROR_ARGUMENT;
    const auto value = Table(); std::memcpy(table, &value, sizeof(value)); return PULSE_OK;
}
void ContextAdapter::Poll() noexcept {
    const auto self = state_;
    try { self->Poll(); } catch (...) { self->Close(); }
}
void ContextAdapter::Close() noexcept { state_->Close(); }
bool ContextAdapter::CallbackActive() const noexcept { return state_->active; }
}

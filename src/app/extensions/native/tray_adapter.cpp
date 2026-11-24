#include "tray_adapter.h"
#include "diagnostics_adapter.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pulse::extensions::native {
namespace {

bool ValidUtf8Wide(PulseStringView view, size_t max_bytes, bool required, std::wstring& out) {
    out.clear();
    if (!view.data) return !required && view.length == 0;
    if (view.length > max_bytes || (required && view.length == 0)) return false;
    if (std::memchr(view.data, 0, view.length) != nullptr) return false;
    if (view.length == 0) return true;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, view.data,
                                          static_cast<int>(view.length), nullptr, 0);
    if (count <= 0) return false;
    out.resize(static_cast<size_t>(count));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, view.data,
                        static_cast<int>(view.length), out.data(), count);
    return true;
}

bool ValidAbsolutePath(const std::wstring& path) {
    if (path.size() < 3) return false;
    for (const wchar_t c : path) {
        if (c < 0x20) return false;
    }
    const auto is_sep = [](wchar_t c) { return c == L'\\' || c == L'/'; };
    const auto is_alpha = [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
    };
    if (is_alpha(path[0]) && path[1] == L':' && is_sep(path[2])) return true;
    if (path.size() >= 5 && is_sep(path[0]) && is_sep(path[1]) && !is_sep(path[2])) return true;
    return false;
}

PulseStringView View(const std::string& text) noexcept {
    return {text.empty() ? "" : text.data(), static_cast<uint32_t>(text.size())};
}

bool SameTrayEntries(const TraySnapshotData& a, const TraySnapshotData& b) noexcept {
    if (a.total_bytes != b.total_bytes || a.entries.size() != b.entries.size()) return false;
    for (size_t i = 0; i < a.entries.size(); ++i) {
        const auto& x = a.entries[i];
        const auto& y = b.entries[i];
        if (x.item_id != y.item_id ||
            x.batch_id != y.batch_id ||
            x.size_bytes != y.size_bytes ||
            x.flags != y.flags ||
            x.path != y.path ||
            x.name != y.name) {
            return false;
        }
    }
    return true;
}

} // namespace

struct TrayAdapter::State : std::enable_shared_from_this<State> {
    struct SubscriptionRecord {
        PulseTraySubscription id = 0;
        PulseTraySnapshotFn callback = nullptr;
        void* user = nullptr;
        bool initial_delivered = false;
    };

    DWORD thread = GetCurrentThreadId();
    TrayBackend backend;
    std::function<bool()> alive;
    std::shared_ptr<DiagnosticsSink> diagnostics;
    std::function<void()> wake;
    bool closed = false;
    int callbacks = 0;
    PulseTraySubscription next_sub_id = 1;
    std::map<PulseTraySubscription, SubscriptionRecord> subscriptions;

    TraySnapshotData last_observed;
    bool has_observed = false;
    uint64_t synthetic_revision = 1;
    TraySnapshotData last_notified;
    bool has_notified = false;

    State(TrayBackend impl, std::function<bool()> live, std::shared_ptr<DiagnosticsSink> sink,
          std::function<void()> wake_fn)
        : backend(std::move(impl)),
          alive(std::move(live)),
          diagnostics(std::move(sink)),
          wake(std::move(wake_fn)) {}

    bool Valid() const noexcept {
        return GetCurrentThreadId() == thread && !closed &&
               (!backend.valid_owner || backend.valid_owner()) &&
               (!alive || alive());
    }

    PulseStatus Fail(PulseStatus status, std::string_view op, std::string_view subject,
                     std::string_view field, std::string_view msg) const {
        if (diagnostics) diagnostics->Record(status, PULSE_TRAY_INTERFACE_NAME, op, subject, field, msg);
        return status;
    }

    template <class F>
    static PulseStatus Guard(void* context, F action) noexcept {
        if (!context) return PULSE_ERROR_ARGUMENT;
        try {
            auto self = static_cast<State*>(context)->shared_from_this();
            return self->Valid() ? action(*self) : PULSE_ERROR_STATE;
        } catch (...) {
            return PULSE_ERROR_STATE;
        }
    }

    void NotifyChanged() noexcept {
        if (!subscriptions.empty() && wake) wake();
    }

    PulseStatus ReadResolvedSnapshot(TraySnapshotData& out) {
        if (!backend.snapshot) return PULSE_ERROR_UNAVAILABLE;
        out = TraySnapshotData{};
        const PulseStatus st = backend.snapshot(out);
        if (st != PULSE_OK) return st;
        if (!has_observed) {
            has_observed = true;
            if (out.revision != 0) synthetic_revision = out.revision;
            out.revision = synthetic_revision;
            last_observed = out;
        } else {
            const bool content_changed = !SameTrayEntries(last_observed, out);
            if (out.revision > synthetic_revision) {
                synthetic_revision = out.revision;
            } else if (content_changed) {
                ++synthetic_revision;
            }
            out.revision = synthetic_revision;
            if (content_changed || last_observed.revision != out.revision) {
                last_observed = out;
            }
        }
        return PULSE_OK;
    }

    static void BuildAbiSnapshot(const TraySnapshotData& data,
                                 std::vector<PulseTrayEntry>& abi_entries,
                                 PulseTraySnapshot& abi_snap) {
        abi_entries.clear();
        abi_entries.reserve(data.entries.size());
        for (const auto& item : data.entries) {
            PulseTrayEntry e{};
            e.item_id = item.item_id;
            e.batch_id = item.batch_id;
            e.size_bytes = item.size_bytes;
            e.flags = item.flags;
            e.reserved = 0;
            e.path = View(item.path);
            e.name = View(item.name);
            abi_entries.push_back(e);
        }
        abi_snap.size = sizeof(PulseTraySnapshot);
        abi_snap.entry_count = static_cast<uint32_t>(abi_entries.size());
        abi_snap.revision = data.revision;
        abi_snap.total_bytes = data.total_bytes;
        abi_snap.entries = abi_entries.empty() ? nullptr : abi_entries.data();
    }

    static PulseStatus PULSE_CALL Snapshot(void* context, PulseTraySnapshotFn callback, void* user) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!callback)
                return self.Fail(PULSE_ERROR_ARGUMENT, "snapshot", "", "callback", "callback must be non-null");
            if (!self.backend.snapshot)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "snapshot", "", "", "tray snapshot backend is unavailable");
            TraySnapshotData data;
            const PulseStatus st = self.ReadResolvedSnapshot(data);
            if (st != PULSE_OK)
                return self.Fail(st, "snapshot", "", "", "failed to read staging tray snapshot");
            std::vector<PulseTrayEntry> abi_entries;
            PulseTraySnapshot abi_snap{};
            BuildAbiSnapshot(data, abi_entries, abi_snap);
            ++self.callbacks;
            try {
                callback(user, &abi_snap);
            } catch (...) {}
            --self.callbacks;
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL Collect(void* context, const PulseStringView* paths,
                                          uint32_t path_count, uint32_t move_intent) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!paths || path_count == 0 || path_count > 256)
                return self.Fail(PULSE_ERROR_ARGUMENT, "collect", "", "paths",
                                 "path_count must be 1..256 with a non-null paths array");
            std::vector<std::wstring> wide_paths;
            wide_paths.reserve(path_count);
            for (uint32_t i = 0; i < path_count; ++i) {
                std::wstring wide;
                if (!ValidUtf8Wide(paths[i], 32768, true, wide) || !ValidAbsolutePath(wide)) {
                    const std::string sub = "paths[" + std::to_string(i) + "]";
                    return self.Fail(PULSE_ERROR_ARGUMENT, "collect", sub, "path",
                                     "each path must be a valid UTF-8 absolute drive or UNC path");
                }
                wide_paths.push_back(std::move(wide));
            }
            if (!self.backend.collect)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "collect", "", "", "tray collect backend is unavailable");
            const PulseStatus st = self.backend.collect(wide_paths, move_intent != 0);
            if (st != PULSE_OK)
                return self.Fail(st, "collect", "", "", "host rejected staging tray collect");
            self.NotifyChanged();
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL RemoveItem(void* context, uint64_t item_id) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (item_id == 0)
                return self.Fail(PULSE_ERROR_ARGUMENT, "remove_item", "", "item_id", "item_id must be non-zero");
            if (!self.backend.remove_item)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "remove_item", "", "",
                                 "tray remove_item backend is unavailable");
            const PulseStatus st = self.backend.remove_item(item_id);
            if (st != PULSE_OK)
                return self.Fail(st, "remove_item", "", "item_id", "item_id not found in staging tray");
            self.NotifyChanged();
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL Clear(void* context) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!self.backend.clear)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "clear", "", "", "tray clear backend is unavailable");
            const PulseStatus st = self.backend.clear();
            if (st != PULSE_OK)
                return self.Fail(st, "clear", "", "", "failed to clear staging tray");
            self.NotifyChanged();
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL ReleaseToDirectory(void* context, PulseStringView destination_dir,
                                                     uint32_t release_mode) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (release_mode > PULSE_TRAY_RELEASE_MOVE)
                return self.Fail(PULSE_ERROR_ARGUMENT, "release_to_directory", "", "release_mode",
                                 "unsupported tray release mode");
            std::wstring wide_dest;
            if (!ValidUtf8Wide(destination_dir, 32768, true, wide_dest) || !ValidAbsolutePath(wide_dest))
                return self.Fail(PULSE_ERROR_ARGUMENT, "release_to_directory", "", "destination_dir",
                                 "destination_dir must be a valid UTF-8 absolute drive or UNC path");
            if (!self.backend.release_to_directory)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "release_to_directory", "", "",
                                 "tray release_to_directory backend is unavailable");
            const PulseStatus st = self.backend.release_to_directory(wide_dest, release_mode);
            if (st != PULSE_OK)
                return self.Fail(st, "release_to_directory", "", "destination_dir",
                                 "failed to release staging tray items to destination directory");
            self.NotifyChanged();
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL Subscribe(void* context, PulseTraySnapshotFn callback, void* user,
                                            PulseTraySubscription* out_subscription) noexcept {
        if (out_subscription) *out_subscription = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!callback || !out_subscription)
                return self.Fail(PULSE_ERROR_ARGUMENT, "subscribe", "", "callback",
                                 "callback and out_subscription must be non-null");
            if (!self.backend.snapshot)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "subscribe", "", "", "tray snapshot backend is unavailable");
            if (self.subscriptions.size() >= 16)
                return self.Fail(PULSE_ERROR_STATE, "subscribe", "", "subscriptions",
                                 "maximum 16 tray subscriptions per session");
            const PulseTraySubscription id = self.next_sub_id++;
            self.subscriptions.emplace(id, SubscriptionRecord{id, callback, user, false});
            *out_subscription = id;
            if (self.wake) self.wake();
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL Unsubscribe(void* context, PulseTraySubscription subscription) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (subscription == 0)
                return self.Fail(PULSE_ERROR_ARGUMENT, "unsubscribe", "", "subscription",
                                 "subscription must be non-zero");
            if (!self.subscriptions.erase(subscription))
                return self.Fail(PULSE_ERROR_STATE, "unsubscribe", "", "subscription",
                                 "unknown or already unsubscribed tray token");
            return PULSE_OK;
        });
    }

    void SyncSubscriptions() noexcept {
        if (!Valid() || subscriptions.empty() || !backend.snapshot) return;
        TraySnapshotData data;
        try {
            if (ReadResolvedSnapshot(data) != PULSE_OK) return;
        } catch (...) {
            return;
        }
        const bool changed = !has_notified ||
                             last_notified.revision != data.revision ||
                             !SameTrayEntries(last_notified, data);
        std::vector<SubscriptionRecord> due;
        std::vector<PulseTrayEntry> abi_entries;
        PulseTraySnapshot abi_snap{};
        try {
            for (auto& [id, sub] : subscriptions) {
                (void)id;
                if (changed || !sub.initial_delivered) {
                    sub.initial_delivered = true;
                    due.push_back(sub);
                }
            }
            if (due.empty()) return;
            last_notified = data;
            has_notified = true;
            BuildAbiSnapshot(data, abi_entries, abi_snap);
        } catch (...) {
            return;
        }
        for (const auto& sub : due) {
            if (!Valid() || !subscriptions.contains(sub.id) || !sub.callback) continue;
            ++callbacks;
            try {
                sub.callback(sub.user, &abi_snap);
            } catch (...) {}
            --callbacks;
        }
    }
};

TrayAdapter::TrayAdapter(TrayBackend backend, std::function<bool()> alive,
                         std::shared_ptr<DiagnosticsSink> diagnostics,
                         std::function<void()> wake)
    : state_(std::make_shared<State>(std::move(backend), std::move(alive), std::move(diagnostics), std::move(wake))) {}

TrayAdapter::~TrayAdapter() { Close(); }

void TrayAdapter::SetDiagnostics(std::shared_ptr<DiagnosticsSink> sink) {
    if (state_) state_->diagnostics = std::move(sink);
}

void TrayAdapter::SetWake(std::function<void()> wake) {
    if (state_) state_->wake = std::move(wake);
}

PulseStatus TrayAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    try {
        if (!state_->Valid()) return PULSE_ERROR_STATE;
        if (version != PULSE_TRAY_VERSION_1) return PULSE_ERROR_ABI;
        if (!out || size < sizeof(PulseTrayV1)) return PULSE_ERROR_ARGUMENT;
        const PulseTrayV1 table{
            sizeof(PulseTrayV1),
            PULSE_TRAY_VERSION_1,
            state_.get(),
            State::Snapshot,
            State::Collect,
            State::RemoveItem,
            State::Clear,
            State::ReleaseToDirectory,
            State::Subscribe,
            State::Unsubscribe,
        };
        std::memcpy(out, &table, sizeof(table));
        return PULSE_OK;
    } catch (...) {
        return PULSE_ERROR_STATE;
    }
}

void TrayAdapter::Sync() noexcept {
    if (state_) state_->SyncSubscriptions();
}

bool TrayAdapter::HasSubscriptions() const noexcept {
    return state_ && !state_->closed && !state_->subscriptions.empty();
}

void TrayAdapter::Close() noexcept {
    if (!state_ || GetCurrentThreadId() != state_->thread || state_->closed) return;
    state_->closed = true;
    state_->subscriptions.clear();
}

bool TrayAdapter::CallbackActive() const noexcept {
    return state_ && state_->callbacks > 0;
}

} // namespace pulse::extensions::native

#include "associations_adapter.h"
#include "diagnostics_adapter.h"
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulse::extensions::native {
namespace {

constexpr size_t kMaxExtensionsPerRequest = 64;
constexpr size_t kMaxLabelBytes = 128;

bool ValidUtf8(PulseStringView view, size_t max_bytes, bool required, std::string& out) {
    out.clear();
    if (!view.data) return !required && view.length == 0;
    if (view.length > max_bytes || (required && view.length == 0)) return false;
    if (std::memchr(view.data, 0, view.length) != nullptr) return false;
    if (view.length == 0) return true;
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, view.data,
                                      static_cast<int>(view.length), nullptr, 0);
    if (n <= 0) return false;
    out.assign(view.data, view.length);
    return true;
}

bool ValidIconResource(std::string_view spec) noexcept {
    if (spec.empty()) return true;
    if (spec.size() > 128 || spec.find("..") != std::string_view::npos) return false;
    for (const char c : spec) {
        if (c == '\\' || c == '/' || c == ':' || c == '\0') return false;
    }
    const size_t comma = spec.find(',');
    const std::string_view file = comma == std::string_view::npos ? spec : spec.substr(0, comma);
    if (file.empty() || file == "." || file == "..") return false;
    for (const char c : file) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    if (comma != std::string_view::npos) {
        std::string_view idx = spec.substr(comma + 1);
        if (idx.empty()) return false;
        if (idx.front() == '-') idx.remove_prefix(1);
        if (idx.empty() || idx.size() > 8) return false;
        for (const char c : idx) {
            if (c < '0' || c > '9') return false;
        }
    }
    return true;
}

PulseStringView View(const std::string& s) noexcept {
    return {s.empty() ? "" : s.data(), static_cast<uint32_t>(s.size())};
}

} // namespace

struct AssociationsAdapter::State : std::enable_shared_from_this<State> {
    enum class RequestKind { Query, Mutate };

    struct ActiveRequest {
        PulseAssociationRequestId id = 0;
        RequestKind kind = RequestKind::Query;
        PulseAssociationsStatusFn status_callback = nullptr;
        PulseAssociationsCompleteFn complete_callback = nullptr;
        void* user = nullptr;
    };

    struct ReadyQuery {
        PulseAssociationRequestId id = 0;
        AssociationsQueryReply reply;
    };

    struct ReadyComplete {
        PulseAssociationRequestId id = 0;
        PulseStatus status = PULSE_OK;
    };

    DWORD thread = GetCurrentThreadId();
    std::string package_id;
    std::vector<std::pair<std::string, std::string>> declared_order;
    std::unordered_map<std::string, std::string> declared_ext_to_handler;
    std::unordered_map<std::string, ManifestFileHandlerContribution> declared_handlers;
    AssociationsBackend backend;
    std::function<bool()> alive;
    std::shared_ptr<DiagnosticsSink> diagnostics;
    mutable std::mutex wake_mutex;
    std::function<void()> wake;
    bool closed = false;
    int callbacks = 0;
    PulseAssociationRequestId next_request_id = 1;
    std::map<PulseAssociationRequestId, ActiveRequest> active;

    mutable std::mutex ready_mutex;
    std::deque<ReadyQuery> ready_queries;
    std::deque<ReadyComplete> ready_completes;

    State(std::string pkg_id,
          std::vector<ManifestFileHandlerContribution> handlers,
          AssociationsBackend impl,
          std::function<bool()> live,
          std::shared_ptr<DiagnosticsSink> sink,
          std::function<void()> wake_fn)
        : package_id(std::move(pkg_id)),
          backend(std::move(impl)),
          alive(std::move(live)),
          diagnostics(std::move(sink)),
          wake(std::move(wake_fn)) {
        for (const auto& h : handlers) {
            for (const auto& ext : h.extensions) {
                declared_order.emplace_back(h.id, ext);
                declared_ext_to_handler.emplace(ext, h.id);
            }
            declared_handlers.emplace(h.id, h);
        }
    }

    bool Valid() const noexcept {
        return GetCurrentThreadId() == thread && !closed &&
               (!backend.valid_owner || backend.valid_owner()) &&
               (!alive || alive());
    }

    void SignalWake() const noexcept {
        std::function<void()> fn;
        {
            std::lock_guard lock(wake_mutex);
            fn = wake;
        }
        if (fn) {
            try {
                fn();
            } catch (...) {}
        }
    }

    PulseStatus Fail(PulseStatus status, std::string_view op, std::string_view subject,
                     std::string_view field, std::string_view msg) const {
        if (diagnostics) diagnostics->Record(status, PULSE_ASSOCIATIONS_INTERFACE_NAME, op, subject, field, msg);
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

    void EnqueueQueryReply(PulseAssociationRequestId id, AssociationsQueryReply reply) noexcept {
        try {
            {
                std::lock_guard lock(ready_mutex);
                ready_queries.push_back(ReadyQuery{id, std::move(reply)});
            }
            SignalWake();
        } catch (...) {}
    }

    void EnqueueCompleteReply(PulseAssociationRequestId id, PulseStatus status) noexcept {
        try {
            {
                std::lock_guard lock(ready_mutex);
                ready_completes.push_back(ReadyComplete{id, status});
            }
            SignalWake();
        } catch (...) {}
    }

    bool ResolveDeclaredExtensions(const PulseStringView* extensions, uint32_t count,
                                   std::string_view op,
                                   std::vector<std::pair<std::string, std::string>>& out_pairs,
                                   PulseStatus& err) const {
        out_pairs.clear();
        if (count > kMaxExtensionsPerRequest) {
            err = Fail(PULSE_ERROR_ARGUMENT, op, "", "count", "too many extensions in request");
            return false;
        }
        if (count == 0) {
            if (extensions != nullptr) {
                err = Fail(PULSE_ERROR_ARGUMENT, op, "", "extensions", "extensions must be null when count is 0");
                return false;
            }
            out_pairs = declared_order;
            return true;
        }
        if (!extensions) {
            err = Fail(PULSE_ERROR_ARGUMENT, op, "", "extensions", "extensions must be non-null when count > 0");
            return false;
        }
        std::set<std::string> seen;
        out_pairs.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            std::string ext;
            if (!ValidUtf8(extensions[i], 32, true, ext) || IsForbiddenAssociationExtension(ext)) {
                err = Fail(PULSE_ERROR_ARGUMENT, op, "", "extension", "invalid or forbidden extension");
                return false;
            }
            const auto found = declared_ext_to_handler.find(ext);
            if (found == declared_ext_to_handler.end()) {
                err = Fail(PULSE_ERROR_PERMISSION, op, ext, "extension",
                           "extension is not declared in manifest contributes.fileHandlers");
                return false;
            }
            if (!seen.insert(ext).second) {
                err = Fail(PULSE_ERROR_ARGUMENT, op, ext, "extension", "duplicate extension in request");
                return false;
            }
            out_pairs.emplace_back(found->second, std::move(ext));
        }
        return true;
    }

    static PulseStatus PULSE_CALL QueryOp(void* context,
                                          const PulseStringView* extensions,
                                          uint32_t count,
                                          PulseAssociationsStatusFn callback,
                                          void* user,
                                          PulseAssociationRequestId* out_request) noexcept {
        if (out_request) *out_request = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!callback || !out_request)
                return self.Fail(PULSE_ERROR_ARGUMENT, "query", "", "callback",
                                 "callback and out_request must be non-null");
            std::vector<std::pair<std::string, std::string>> target;
            PulseStatus err = PULSE_OK;
            if (!self.ResolveDeclaredExtensions(extensions, count, "query", target, err)) return err;
            if (!self.backend.query)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "query", "", "", "associations backend unavailable");
            if (self.active.size() >= 16)
                return self.Fail(PULSE_ERROR_STATE, "query", "", "requests",
                                 "maximum 16 active association requests per session");

            const PulseAssociationRequestId id = self.next_request_id++;
            self.active.emplace(id, ActiveRequest{id, RequestKind::Query, callback, nullptr, user});
            const std::weak_ptr<State> weak_self = self.weak_from_this();
            const PulseStatus st = self.backend.query(
                id,
                std::move(target),
                [weak_self, id](AssociationsQueryReply reply) {
                    if (auto locked = weak_self.lock()) locked->EnqueueQueryReply(id, std::move(reply));
                });
            if (st != PULSE_OK) {
                self.active.erase(id);
                std::lock_guard lock(self.ready_mutex);
                std::erase_if(self.ready_queries, [id](const ReadyQuery& q) { return q.id == id; });
                return self.Fail(st, "query", "", "", "failed to start associations query");
            }
            *out_request = id;
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL ApplyOp(void* context,
                                          const PulseAssociationEntry* entries,
                                          uint32_t count,
                                          PulseAssociationsCompleteFn callback,
                                          void* user,
                                          PulseAssociationRequestId* out_request) noexcept {
        if (out_request) *out_request = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!callback || !out_request)
                return self.Fail(PULSE_ERROR_ARGUMENT, "apply", "", "callback",
                                 "callback and out_request must be non-null");
            if (count > kMaxExtensionsPerRequest)
                return self.Fail(PULSE_ERROR_ARGUMENT, "apply", "", "count", "too many entries in apply");
            if (count > 0 && !entries)
                return self.Fail(PULSE_ERROR_ARGUMENT, "apply", "", "entries", "entries must be non-null when count > 0");

            std::vector<AssociationEntryRequest> validated;
            validated.reserve(count);
            std::set<std::string> seen_exts;
            for (uint32_t i = 0; i < count; ++i) {
                const auto& item = entries[i];
                if (item.size < sizeof(PulseAssociationEntry))
                    return self.Fail(PULSE_ERROR_ARGUMENT, "apply", "", "size", "invalid PulseAssociationEntry size");
                if (item.action != PULSE_ASSOC_ACTION_COMMAND && item.action != PULSE_ASSOC_ACTION_PREVIEW)
                    return self.Fail(PULSE_ERROR_ARGUMENT, "apply", "", "action", "unknown association action");
                AssociationEntryRequest req;
                req.action = item.action;
                if (!ValidUtf8(item.handler_id, 64, true, req.handler_id))
                    return self.Fail(PULSE_ERROR_ARGUMENT, "apply", "", "handler_id", "invalid handler_id");
                if (!ValidUtf8(item.extension, 32, true, req.extension) || IsForbiddenAssociationExtension(req.extension))
                    return self.Fail(PULSE_ERROR_ARGUMENT, "apply", "", "extension", "invalid or forbidden extension");
                const auto handler_it = self.declared_handlers.find(req.handler_id);
                if (handler_it == self.declared_handlers.end())
                    return self.Fail(PULSE_ERROR_PERMISSION, "apply", req.handler_id, "handler_id",
                                     "handler_id is not declared in manifest contributes.fileHandlers");
                const auto& allowed_exts = handler_it->second.extensions;
                if (std::find(allowed_exts.begin(), allowed_exts.end(), req.extension) == allowed_exts.end())
                    return self.Fail(PULSE_ERROR_PERMISSION, "apply", req.extension, "extension",
                                     "extension is not declared under this handler_id");
                if (!seen_exts.insert(req.extension).second)
                    return self.Fail(PULSE_ERROR_ARGUMENT, "apply", req.extension, "extension",
                                     "duplicate extension in apply");
                if (!ValidUtf8(item.display_name, kMaxLabelBytes, true, req.display_name))
                    return self.Fail(PULSE_ERROR_ARGUMENT, "apply", req.extension, "display_name",
                                     "display_name must be 1..128 valid UTF-8 bytes");
                if (!ValidUtf8(item.verb_label, kMaxLabelBytes, false, req.verb_label))
                    return self.Fail(PULSE_ERROR_ARGUMENT, "apply", req.extension, "verb_label",
                                     "verb_label must be 0..128 valid UTF-8 bytes");
                if (!ValidUtf8(item.icon_resource, 128, false, req.icon_resource) ||
                    !ValidIconResource(req.icon_resource))
                    return self.Fail(PULSE_ERROR_ARGUMENT, "apply", req.extension, "icon_resource",
                                     "invalid flat icon_resource specification");
                validated.push_back(std::move(req));
            }

            if (!self.backend.apply)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "apply", "", "", "associations backend unavailable");
            if (self.active.size() >= 16)
                return self.Fail(PULSE_ERROR_STATE, "apply", "", "requests",
                                 "maximum 16 active association requests per session");

            const PulseAssociationRequestId id = self.next_request_id++;
            self.active.emplace(id, ActiveRequest{id, RequestKind::Mutate, nullptr, callback, user});
            const std::weak_ptr<State> weak_self = self.weak_from_this();
            const PulseStatus st = self.backend.apply(
                id,
                std::move(validated),
                [weak_self, id](PulseStatus result) {
                    if (auto locked = weak_self.lock()) locked->EnqueueCompleteReply(id, result);
                });
            if (st != PULSE_OK) {
                self.active.erase(id);
                std::lock_guard lock(self.ready_mutex);
                std::erase_if(self.ready_completes, [id](const ReadyComplete& r) { return r.id == id; });
                return self.Fail(st, "apply", "", "", "failed to start associations apply");
            }
            *out_request = id;
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL RemoveOp(void* context,
                                           const PulseStringView* extensions,
                                           uint32_t count,
                                           PulseAssociationsCompleteFn callback,
                                           void* user,
                                           PulseAssociationRequestId* out_request) noexcept {
        if (out_request) *out_request = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!callback || !out_request)
                return self.Fail(PULSE_ERROR_ARGUMENT, "remove", "", "callback",
                                 "callback and out_request must be non-null");
            std::vector<std::string> exts;
            if (count > 0) {
                std::vector<std::pair<std::string, std::string>> pairs;
                PulseStatus err = PULSE_OK;
                if (!self.ResolveDeclaredExtensions(extensions, count, "remove", pairs, err)) return err;
                exts.reserve(pairs.size());
                for (auto& [h, e] : pairs) exts.push_back(std::move(e));
            } else if (extensions != nullptr) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "remove", "", "extensions",
                                 "extensions must be null when count is 0");
            }
            if (!self.backend.remove)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "remove", "", "", "associations backend unavailable");
            if (self.active.size() >= 16)
                return self.Fail(PULSE_ERROR_STATE, "remove", "", "requests",
                                 "maximum 16 active association requests per session");

            const PulseAssociationRequestId id = self.next_request_id++;
            self.active.emplace(id, ActiveRequest{id, RequestKind::Mutate, nullptr, callback, user});
            const std::weak_ptr<State> weak_self = self.weak_from_this();
            const PulseStatus st = self.backend.remove(
                id,
                std::move(exts),
                [weak_self, id](PulseStatus result) {
                    if (auto locked = weak_self.lock()) locked->EnqueueCompleteReply(id, result);
                });
            if (st != PULSE_OK) {
                self.active.erase(id);
                std::lock_guard lock(self.ready_mutex);
                std::erase_if(self.ready_completes, [id](const ReadyComplete& r) { return r.id == id; });
                return self.Fail(st, "remove", "", "", "failed to start associations remove");
            }
            *out_request = id;
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL CancelOp(void* context, PulseAssociationRequestId request_id) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (request_id == 0)
                return self.Fail(PULSE_ERROR_ARGUMENT, "cancel", "", "request_id", "request_id must be non-zero");
            if (!self.active.erase(request_id))
                return self.Fail(PULSE_ERROR_STATE, "cancel", "", "request_id",
                                 "unknown, completed, or already cancelled association request_id");
            {
                std::lock_guard lock(self.ready_mutex);
                std::erase_if(self.ready_queries, [request_id](const ReadyQuery& item) { return item.id == request_id; });
                std::erase_if(self.ready_completes, [request_id](const ReadyComplete& item) { return item.id == request_id; });
            }
            if (self.backend.cancel) {
                try {
                    self.backend.cancel(request_id);
                } catch (...) {}
            }
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL OpenChooserOp(void* context, PulseStringView extension) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            std::string ext;
            if (!ValidUtf8(extension, 32, true, ext) || IsForbiddenAssociationExtension(ext))
                return self.Fail(PULSE_ERROR_ARGUMENT, "open_chooser", "", "extension",
                                 "invalid or forbidden extension");
            if (self.declared_ext_to_handler.find(ext) == self.declared_ext_to_handler.end())
                return self.Fail(PULSE_ERROR_PERMISSION, "open_chooser", ext, "extension",
                                 "extension is not declared in manifest contributes.fileHandlers");
            if (!self.backend.open_chooser)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "open_chooser", ext, "", "chooser backend unavailable");
            return self.backend.open_chooser(std::move(ext));
        });
    }

    static PulseStatus PULSE_CALL OpenDefaultAppsOp(void* context) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!self.backend.open_default_apps)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "open_default_apps", "", "",
                                 "default apps backend unavailable");
            return self.backend.open_default_apps();
        });
    }

    void PollReplies() noexcept {
        if (!Valid()) return;
        std::deque<ReadyQuery> query_batch;
        std::deque<ReadyComplete> complete_batch;
        {
            std::lock_guard lock(ready_mutex);
            if (ready_queries.empty() && ready_completes.empty()) return;
            query_batch.swap(ready_queries);
            complete_batch.swap(ready_completes);
        }

        for (auto& q : query_batch) {
            if (!Valid()) break;
            const auto it = active.find(q.id);
            if (it == active.end() || it->second.kind != RequestKind::Query) continue;
            const ActiveRequest req = it->second;
            active.erase(it);
            if (!req.status_callback) continue;

            std::vector<PulseAssociationStatus> abi_items;
            try {
                abi_items.reserve(q.reply.items.size());
                for (const auto& item : q.reply.items) {
                    PulseAssociationStatus row{};
                    row.size = sizeof(row);
                    row.owner = item.owner;
                    row.flags = item.flags;
                    row.reserved = 0;
                    row.extension = View(item.extension);
                    row.handler_id = View(item.handler_id);
                    row.current_app = View(item.current_app);
                    abi_items.push_back(row);
                }
            } catch (...) {
                continue;
            }
            ++callbacks;
            try {
                req.status_callback(req.user, req.id, q.reply.status,
                                    abi_items.empty() ? nullptr : abi_items.data(),
                                    static_cast<uint32_t>(abi_items.size()));
            } catch (...) {}
            --callbacks;
        }

        for (auto& c : complete_batch) {
            if (!Valid()) break;
            const auto it = active.find(c.id);
            if (it == active.end() || it->second.kind != RequestKind::Mutate) continue;
            const ActiveRequest req = it->second;
            active.erase(it);
            if (!req.complete_callback) continue;

            ++callbacks;
            try {
                req.complete_callback(req.user, req.id, c.status);
            } catch (...) {}
            --callbacks;
        }
    }
};

AssociationsAdapter::AssociationsAdapter(std::string package_id,
                                         std::vector<ManifestFileHandlerContribution> declared_handlers,
                                         AssociationsBackend backend,
                                         std::function<bool()> alive,
                                         std::shared_ptr<DiagnosticsSink> diagnostics,
                                         std::function<void()> wake)
    : state_(std::make_shared<State>(std::move(package_id),
                                     std::move(declared_handlers),
                                     std::move(backend),
                                     std::move(alive),
                                     std::move(diagnostics),
                                     std::move(wake))) {}

AssociationsAdapter::~AssociationsAdapter() { Close(); }

void AssociationsAdapter::SetDiagnostics(std::shared_ptr<DiagnosticsSink> sink) {
    if (state_) state_->diagnostics = std::move(sink);
}

void AssociationsAdapter::SetWake(std::function<void()> wake) {
    if (!state_) return;
    std::lock_guard lock(state_->wake_mutex);
    state_->wake = std::move(wake);
}

PulseStatus AssociationsAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    if (!state_ || !state_->Valid()) return PULSE_ERROR_STATE;
    if (version != PULSE_ASSOCIATIONS_VERSION_1) return PULSE_ERROR_ABI;
    if (!out || size < sizeof(PulseAssociationsV1)) return PULSE_ERROR_ARGUMENT;
    *static_cast<PulseAssociationsV1*>(out) = PulseAssociationsV1{
        sizeof(PulseAssociationsV1),
        PULSE_ASSOCIATIONS_VERSION_1,
        state_.get(),
        &State::QueryOp,
        &State::ApplyOp,
        &State::RemoveOp,
        &State::CancelOp,
        &State::OpenChooserOp,
        &State::OpenDefaultAppsOp,
    };
    return PULSE_OK;
}

void AssociationsAdapter::Poll() noexcept {
    if (state_) state_->PollReplies();
}

bool AssociationsAdapter::HasPendingWork() const noexcept {
    if (!state_ || state_->closed) return false;
    if (!state_->active.empty()) return true;
    std::lock_guard lock(state_->ready_mutex);
    return !state_->ready_queries.empty() || !state_->ready_completes.empty();
}

void AssociationsAdapter::Close() noexcept {
    if (!state_) return;
    state_->closed = true;
    state_->active.clear();
    {
        std::lock_guard lock(state_->ready_mutex);
        state_->ready_queries.clear();
        state_->ready_completes.clear();
    }
    SetWake({});
}

bool AssociationsAdapter::CallbackActive() const noexcept {
    return state_ && state_->callbacks != 0;
}

} // namespace pulse::extensions::native

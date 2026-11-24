#include "search_adapter.h"
#include "diagnostics_adapter.h"
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
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

} // namespace

struct SearchAdapter::State : std::enable_shared_from_this<State> {
    struct ActiveRequest {
        PulseSearchRequestId id = 0;
        PulseSearchReplyFn callback = nullptr;
        void* user = nullptr;
    };

    struct ReadyReply {
        PulseSearchRequestId id = 0;
        SearchBackendReply reply;
    };

    DWORD thread = GetCurrentThreadId();
    SearchBackend backend;
    std::function<bool()> alive;
    std::shared_ptr<DiagnosticsSink> diagnostics;
    mutable std::mutex wake_mutex;
    std::function<void()> wake;
    bool closed = false;
    int callbacks = 0;
    PulseSearchRequestId next_request_id = 1;
    std::map<PulseSearchRequestId, ActiveRequest> active;

    mutable std::mutex ready_mutex;
    std::deque<ReadyReply> ready;

    State(SearchBackend impl, std::function<bool()> live, std::shared_ptr<DiagnosticsSink> sink,
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
        if (diagnostics) diagnostics->Record(status, PULSE_SEARCH_INTERFACE_NAME, op, subject, field, msg);
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

    void EnqueueCompletion(PulseSearchRequestId id, SearchBackendReply reply) noexcept {
        try {
            {
                std::lock_guard lock(ready_mutex);
                ready.push_back(ReadyReply{id, std::move(reply)});
            }
            SignalWake();
        } catch (...) {}
    }

    static PulseStatus PULSE_CALL StartQuery(void* context, const PulseSearchRequest* request,
                                             PulseSearchReplyFn callback, void* user,
                                             PulseSearchRequestId* out_request_id) noexcept {
        if (out_request_id) *out_request_id = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!request || request->size < sizeof(PulseSearchRequest))
                return self.Fail(PULSE_ERROR_ARGUMENT, "query", "", "size",
                                 "null request or invalid PulseSearchRequest size");
            if (!callback || !out_request_id)
                return self.Fail(PULSE_ERROR_ARGUMENT, "query", "", "callback",
                                 "callback and out_request_id must be non-null");
            if ((request->flags & ~static_cast<uint32_t>(PULSE_SEARCH_ALL_FLAGS)) != 0 || request->reserved != 0)
                return self.Fail(PULSE_ERROR_ARGUMENT, "query", "", "flags",
                                 "invalid search flags or non-zero reserved field");
            if ((request->flags & PULSE_SEARCH_FLAG_FOLDERS_ONLY) != 0 &&
                (request->flags & PULSE_SEARCH_FLAG_FILES_ONLY) != 0) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "query", "", "flags",
                                 "PULSE_SEARCH_FLAG_FOLDERS_ONLY and PULSE_SEARCH_FLAG_FILES_ONLY are mutually exclusive");
            }

            SearchQuerySpec spec;
            spec.flags = request->flags;
            spec.max_results = request->max_results == 0
                ? 20u : std::clamp<uint32_t>(request->max_results, 1u, 50u);
            if (!ValidUtf8Wide(request->query, 512, true, spec.query))
                return self.Fail(PULSE_ERROR_ARGUMENT, "query", "", "query",
                                 "query must be 1..512 valid UTF-8 bytes without NUL");
            for (const wchar_t c : spec.query) {
                if (c < 0x20) {
                    return self.Fail(PULSE_ERROR_ARGUMENT, "query", "", "query",
                                     "query must not contain control characters");
                }
            }
            if (!ValidUtf8Wide(request->kind_token, 64, false, spec.kind_token))
                return self.Fail(PULSE_ERROR_ARGUMENT, "query", "", "kind_token",
                                 "kind_token must be 0..64 valid UTF-8 bytes without NUL");
            if (!ValidUtf8Wide(request->scope_directory, 32768, false, spec.scope_directory) ||
                (!spec.scope_directory.empty() && !ValidAbsolutePath(spec.scope_directory))) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "query", "", "scope_directory",
                                 "scope_directory must be empty or a valid UTF-8 absolute drive/UNC path");
            }

            if (!self.backend.start)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "query", "", "", "search backend is unavailable");
            if (self.active.size() >= 16)
                return self.Fail(PULSE_ERROR_STATE, "query", "", "requests",
                                 "maximum 16 active search requests per session");

            const PulseSearchRequestId id = self.next_request_id++;
            spec.request_id = id;
            self.active.emplace(id, ActiveRequest{id, callback, user});
            const std::weak_ptr<State> weak_self = self.weak_from_this();
            const PulseStatus st = self.backend.start(
                std::move(spec),
                [weak_self, id](SearchBackendReply reply) {
                    if (auto locked = weak_self.lock()) {
                        locked->EnqueueCompletion(id, std::move(reply));
                    }
                });
            if (st != PULSE_OK) {
                self.active.erase(id);
                {
                    std::lock_guard lock(self.ready_mutex);
                    std::erase_if(self.ready, [id](const ReadyReply& item) { return item.id == id; });
                }
                return self.Fail(st, "query", "", "", "failed to start asynchronous search query");
            }
            *out_request_id = id;
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL CancelQuery(void* context, PulseSearchRequestId request_id) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (request_id == 0)
                return self.Fail(PULSE_ERROR_ARGUMENT, "cancel", "", "request_id", "request_id must be non-zero");
            if (!self.active.erase(request_id))
                return self.Fail(PULSE_ERROR_STATE, "cancel", "", "request_id",
                                 "unknown, completed, or already cancelled search request_id");
            {
                std::lock_guard lock(self.ready_mutex);
                std::erase_if(self.ready, [request_id](const ReadyReply& item) { return item.id == request_id; });
            }
            if (self.backend.cancel) {
                try {
                    self.backend.cancel(request_id);
                } catch (...) {}
            }
            return PULSE_OK;
        });
    }

    void PollCompletions() noexcept {
        if (!Valid()) return;
        std::deque<ReadyReply> batch;
        {
            std::lock_guard lock(ready_mutex);
            if (ready.empty()) return;
            batch.swap(ready);
        }
        for (auto& item : batch) {
            if (!Valid()) break;
            const auto it = active.find(item.id);
            if (it == active.end()) continue;
            const ActiveRequest req = it->second;
            active.erase(it);
            if (!req.callback) continue;

            std::vector<PulseSearchHit> abi_hits;
            try {
                abi_hits.reserve(item.reply.hits.size());
                for (const auto& h : item.reply.hits) {
                    PulseSearchHit abi_hit{};
                    abi_hit.path = View(h.path);
                    abi_hit.name = View(h.name);
                    abi_hit.size_bytes = h.size_bytes;
                    abi_hit.modified_unix_ms = h.modified_unix_ms;
                    abi_hit.flags = h.flags;
                    abi_hit.reserved = 0;
                    abi_hits.push_back(abi_hit);
                }
            } catch (...) {
                continue;
            }

            PulseSearchReply abi_reply{};
            abi_reply.size = sizeof(PulseSearchReply);
            abi_reply.status = item.reply.status;
            abi_reply.request_id = req.id;
            abi_reply.total_matches = item.reply.total_matches;
            abi_reply.hits = abi_hits.empty() ? nullptr : abi_hits.data();
            abi_reply.hit_count = static_cast<uint32_t>(abi_hits.size());
            abi_reply.index_connected = item.reply.index_connected ? 1u : 0u;

            ++callbacks;
            try {
                req.callback(req.user, &abi_reply);
            } catch (...) {}
            --callbacks;
        }
    }
};

SearchAdapter::SearchAdapter(SearchBackend backend, std::function<bool()> alive,
                             std::shared_ptr<DiagnosticsSink> diagnostics,
                             std::function<void()> wake)
    : state_(std::make_shared<State>(std::move(backend), std::move(alive), std::move(diagnostics), std::move(wake))) {}

SearchAdapter::~SearchAdapter() { Close(); }

void SearchAdapter::SetDiagnostics(std::shared_ptr<DiagnosticsSink> sink) {
    if (state_) state_->diagnostics = std::move(sink);
}

void SearchAdapter::SetWake(std::function<void()> wake) {
    if (!state_) return;
    std::lock_guard lock(state_->wake_mutex);
    state_->wake = std::move(wake);
}

PulseStatus SearchAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    try {
        if (!state_->Valid()) return PULSE_ERROR_STATE;
        if (version != PULSE_SEARCH_VERSION_1) return PULSE_ERROR_ABI;
        if (!out || size < sizeof(PulseSearchV1)) return PULSE_ERROR_ARGUMENT;
        const PulseSearchV1 table{
            sizeof(PulseSearchV1),
            PULSE_SEARCH_VERSION_1,
            state_.get(),
            State::StartQuery,
            State::CancelQuery,
        };
        std::memcpy(out, &table, sizeof(table));
        return PULSE_OK;
    } catch (...) {
        return PULSE_ERROR_STATE;
    }
}

void SearchAdapter::Poll() noexcept {
    if (state_) state_->PollCompletions();
}

bool SearchAdapter::HasPendingWork() const noexcept {
    if (!state_ || state_->closed) return false;
    if (!state_->active.empty()) return true;
    std::lock_guard lock(state_->ready_mutex);
    return !state_->ready.empty();
}

void SearchAdapter::Close() noexcept {
    if (!state_ || GetCurrentThreadId() != state_->thread || state_->closed) return;
    state_->closed = true;
    std::vector<PulseSearchRequestId> to_cancel;
    try {
        to_cancel.reserve(state_->active.size());
        for (const auto& [id, _] : state_->active) {
            to_cancel.push_back(id);
        }
    } catch (...) {}
    state_->active.clear();
    {
        std::lock_guard lock(state_->ready_mutex);
        state_->ready.clear();
    }
    if (state_->backend.cancel) {
        for (const auto id : to_cancel) {
            try {
                state_->backend.cancel(id);
            } catch (...) {}
        }
    }
}

bool SearchAdapter::CallbackActive() const noexcept {
    return state_ && state_->callbacks > 0;
}

} // namespace pulse::extensions::native

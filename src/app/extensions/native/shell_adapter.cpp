#include "shell_adapter.h"
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

struct ShellAdapter::State : std::enable_shared_from_this<State> {
    struct ActiveMenuSession {
        PulseShellMenuSessionId id = 0;
        PulseShellMenuReplyFn callback = nullptr;
        void* user = nullptr;
    };

    struct ReadyMenuReply {
        PulseShellMenuSessionId id = 0;
        ShellMenuBackendReply reply;
    };

    struct ActivePreviewRequest {
        PulseShellPreviewRequestId id = 0;
        PulseShellPreviewFn callback = nullptr;
        void* user = nullptr;
    };

    struct ReadyPreviewReply {
        PulseShellPreviewRequestId id = 0;
        ShellPreviewData data;
    };

    DWORD thread = GetCurrentThreadId();
    ShellBackend backend;
    std::function<bool()> alive;
    std::shared_ptr<DiagnosticsSink> diagnostics;
    mutable std::mutex wake_mutex;
    std::function<void()> wake;
    bool closed = false;
    int callbacks = 0;

    PulseShellMenuSessionId next_menu_id = 1;
    std::map<PulseShellMenuSessionId, ActiveMenuSession> active_menus;

    PulseShellPreviewRequestId next_preview_id = 1;
    std::map<PulseShellPreviewRequestId, ActivePreviewRequest> active_previews;

    mutable std::mutex ready_mutex;
    std::deque<ReadyMenuReply> ready_menus;
    std::deque<ReadyPreviewReply> ready_previews;

    State(ShellBackend impl, std::function<bool()> live, std::shared_ptr<DiagnosticsSink> sink,
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
        if (diagnostics) diagnostics->Record(status, PULSE_SHELL_INTERFACE_NAME, op, subject, field, msg);
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

    void InvokeTagListCallback(const std::vector<ColorTagInfoData>& src,
                               PulseColorTagListFn callback, void* user) noexcept {
        std::vector<PulseColorTagInfo> abi_tags;
        try {
            abi_tags.reserve(src.size());
            for (const auto& t : src) {
                PulseColorTagInfo item{};
                item.id = View(t.id);
                item.name = View(t.name);
                item.argb = t.argb;
                item.favorite_slot = t.favorite_slot;
                abi_tags.push_back(item);
            }
        } catch (...) {
            return;
        }
        ++callbacks;
        try {
            callback(user, abi_tags.empty() ? nullptr : abi_tags.data(),
                     static_cast<uint32_t>(abi_tags.size()));
        } catch (...) {}
        --callbacks;
    }

    void EnqueueMenuReply(PulseShellMenuSessionId id, ShellMenuBackendReply reply) noexcept {
        try {
            {
                std::lock_guard lock(ready_mutex);
                ready_menus.push_back(ReadyMenuReply{id, std::move(reply)});
            }
            SignalWake();
        } catch (...) {}
    }

    void EnqueuePreviewReply(PulseShellPreviewRequestId id, ShellPreviewData data) noexcept {
        try {
            {
                std::lock_guard lock(ready_mutex);
                ready_previews.push_back(ReadyPreviewReply{id, std::move(data)});
            }
            SignalWake();
        } catch (...) {}
    }

    static PulseStatus PULSE_CALL GetTagCatalog(void* context, PulseColorTagListFn callback, void* user) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!callback)
                return self.Fail(PULSE_ERROR_ARGUMENT, "get_tag_catalog", "", "callback", "callback must be non-null");
            if (!self.backend.get_tag_catalog)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "get_tag_catalog", "", "", "tag catalog backend is unavailable");
            std::vector<ColorTagInfoData> tags;
            const PulseStatus st = self.backend.get_tag_catalog(tags);
            if (st != PULSE_OK)
                return self.Fail(st, "get_tag_catalog", "", "", "failed to query color tag catalog");
            self.InvokeTagListCallback(tags, callback, user);
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL GetPathTags(void* context, PulseStringView path,
                                              PulseColorTagListFn callback, void* user) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!callback)
                return self.Fail(PULSE_ERROR_ARGUMENT, "get_path_tags", "", "callback", "callback must be non-null");
            std::wstring wide_path;
            if (!ValidUtf8Wide(path, 32768, true, wide_path) || !ValidAbsolutePath(wide_path))
                return self.Fail(PULSE_ERROR_ARGUMENT, "get_path_tags", "", "path",
                                 "path must be a valid UTF-8 absolute drive or UNC path");
            if (!self.backend.get_path_tags)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "get_path_tags", "", "", "path tags backend is unavailable");
            std::vector<ColorTagInfoData> tags;
            const PulseStatus st = self.backend.get_path_tags(wide_path, tags);
            if (st != PULSE_OK)
                return self.Fail(st, "get_path_tags", "", "path", "failed to read tags for path");
            self.InvokeTagListCallback(tags, callback, user);
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL SetPathTag(void* context, PulseStringView path,
                                             PulseStringView tag_id, uint32_t tagged) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            std::wstring wide_path;
            if (!ValidUtf8Wide(path, 32768, true, wide_path) || !ValidAbsolutePath(wide_path))
                return self.Fail(PULSE_ERROR_ARGUMENT, "set_path_tag", "", "path",
                                 "path must be a valid UTF-8 absolute drive or UNC path");
            std::wstring wide_tag_id;
            if (!ValidUtf8Wide(tag_id, 128, true, wide_tag_id))
                return self.Fail(PULSE_ERROR_ARGUMENT, "set_path_tag", "", "tag_id",
                                 "tag_id must be 1..128 valid UTF-8 bytes without NUL");
            if (!self.backend.set_path_tag)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "set_path_tag", "", "", "set_path_tag backend is unavailable");
            const PulseStatus st = self.backend.set_path_tag(wide_path, wide_tag_id, tagged != 0);
            if (st != PULSE_OK)
                return self.Fail(st, "set_path_tag", "", "tag_id", "failed to update color tag assignment");
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL QueryContextMenu(void* context, const PulseShellMenuRequest* request,
                                                   PulseShellMenuReplyFn callback, void* user,
                                                   PulseShellMenuSessionId* out_session_id) noexcept {
        if (out_session_id) *out_session_id = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!request || request->size < sizeof(PulseShellMenuRequest))
                return self.Fail(PULSE_ERROR_ARGUMENT, "query_context_menu", "", "size",
                                 "null request or invalid PulseShellMenuRequest size");
            if (!callback || !out_session_id)
                return self.Fail(PULSE_ERROR_ARGUMENT, "query_context_menu", "", "callback",
                                 "callback and out_session_id must be non-null");
            if ((request->flags & ~static_cast<uint32_t>(PULSE_SHELL_MENU_QUERY_ALL_FLAGS)) != 0 ||
                request->reserved != 0) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "query_context_menu", "", "flags",
                                 "invalid context menu query flags or non-zero reserved field");
            }
            if (!request->paths || request->path_count == 0 || request->path_count > 64) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "query_context_menu", "", "paths",
                                 "path_count must be 1..64 with a non-null paths array");
            }
            std::vector<std::wstring> wide_paths;
            wide_paths.reserve(request->path_count);
            for (uint32_t i = 0; i < request->path_count; ++i) {
                std::wstring wide;
                if (!ValidUtf8Wide(request->paths[i], 32768, true, wide) || !ValidAbsolutePath(wide)) {
                    const std::string sub = "paths[" + std::to_string(i) + "]";
                    return self.Fail(PULSE_ERROR_ARGUMENT, "query_context_menu", sub, "path",
                                     "each path must be a valid UTF-8 absolute drive or UNC path");
                }
                wide_paths.push_back(std::move(wide));
            }
            if (!self.backend.query_context_menu)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "query_context_menu", "", "",
                                 "context menu backend is unavailable");
            if (self.active_menus.size() >= 16)
                return self.Fail(PULSE_ERROR_STATE, "query_context_menu", "", "sessions",
                                 "maximum 16 active context menu sessions per session");

            const PulseShellMenuSessionId sid = self.next_menu_id++;
            self.active_menus.emplace(sid, ActiveMenuSession{sid, callback, user});
            const std::weak_ptr<State> weak_self = self.weak_from_this();
            const PulseStatus st = self.backend.query_context_menu(
                sid,
                wide_paths,
                request->flags,
                [weak_self, sid](ShellMenuBackendReply reply) {
                    if (auto locked = weak_self.lock()) {
                        locked->EnqueueMenuReply(sid, std::move(reply));
                    }
                });
            if (st != PULSE_OK) {
                self.active_menus.erase(sid);
                {
                    std::lock_guard lock(self.ready_mutex);
                    std::erase_if(self.ready_menus, [sid](const ReadyMenuReply& item) { return item.id == sid; });
                }
                return self.Fail(st, "query_context_menu", "", "", "failed to start shell context menu query");
            }
            *out_session_id = sid;
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL InvokeContextMenu(void* context, PulseShellMenuSessionId session_id,
                                                    uint32_t item_id, PulseStringView verb) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (session_id == 0)
                return self.Fail(PULSE_ERROR_ARGUMENT, "invoke_context_menu", "", "session_id",
                                 "session_id must be non-zero");
            std::wstring wide_verb;
            if (!ValidUtf8Wide(verb, 256, false, wide_verb))
                return self.Fail(PULSE_ERROR_ARGUMENT, "invoke_context_menu", "", "verb",
                                 "verb must be 0..256 valid UTF-8 bytes without NUL");
            if (item_id == 0 && wide_verb.empty())
                return self.Fail(PULSE_ERROR_ARGUMENT, "invoke_context_menu", "", "item_id",
                                 "either item_id or verb must be specified");
            if (!self.active_menus.erase(session_id))
                return self.Fail(PULSE_ERROR_STATE, "invoke_context_menu", "", "session_id",
                                 "unknown or already closed context menu session_id");
            {
                std::lock_guard lock(self.ready_mutex);
                std::erase_if(self.ready_menus,
                              [session_id](const ReadyMenuReply& item) { return item.id == session_id; });
            }
            if (!self.backend.invoke_context_menu)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "invoke_context_menu", "", "",
                                 "invoke_context_menu backend is unavailable");
            const PulseStatus st = self.backend.invoke_context_menu(session_id, item_id, wide_verb);
            if (st != PULSE_OK)
                return self.Fail(st, "invoke_context_menu", "", "", "failed to invoke context menu command");
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL CloseContextMenu(void* context, PulseShellMenuSessionId session_id) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (session_id == 0)
                return self.Fail(PULSE_ERROR_ARGUMENT, "close_context_menu", "", "session_id",
                                 "session_id must be non-zero");
            if (!self.active_menus.erase(session_id))
                return self.Fail(PULSE_ERROR_STATE, "close_context_menu", "", "session_id",
                                 "unknown or already closed context menu session_id");
            {
                std::lock_guard lock(self.ready_mutex);
                std::erase_if(self.ready_menus,
                              [session_id](const ReadyMenuReply& item) { return item.id == session_id; });
            }
            if (self.backend.close_context_menu) {
                const PulseStatus st = self.backend.close_context_menu(session_id);
                if (st != PULSE_OK)
                    return self.Fail(st, "close_context_menu", "", "", "failed to close context menu session");
            }
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL ReadPreview(void* context, PulseStringView path, uint32_t max_thumb_px,
                                              PulseShellPreviewFn callback, void* user,
                                              PulseShellPreviewRequestId* out_request_id) noexcept {
        if (out_request_id) *out_request_id = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!callback || !out_request_id)
                return self.Fail(PULSE_ERROR_ARGUMENT, "read_preview", "", "callback",
                                 "callback and out_request_id must be non-null");
            std::wstring wide_path;
            if (!ValidUtf8Wide(path, 32768, true, wide_path) || !ValidAbsolutePath(wide_path))
                return self.Fail(PULSE_ERROR_ARGUMENT, "read_preview", "", "path",
                                 "path must be a valid UTF-8 absolute drive or UNC path");
            if (!self.backend.read_preview)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "read_preview", "", "", "preview backend is unavailable");
            if (self.active_previews.size() >= 16)
                return self.Fail(PULSE_ERROR_STATE, "read_preview", "", "requests",
                                 "maximum 16 active preview requests per session");

            const uint32_t clamped_thumb_px = max_thumb_px == 0
                ? 256u : std::clamp<uint32_t>(max_thumb_px, 32u, 512u);
            const PulseShellPreviewRequestId rid = self.next_preview_id++;
            self.active_previews.emplace(rid, ActivePreviewRequest{rid, callback, user});
            const std::weak_ptr<State> weak_self = self.weak_from_this();
            const PulseStatus st = self.backend.read_preview(
                rid,
                wide_path,
                clamped_thumb_px,
                [weak_self, rid](ShellPreviewData data) {
                    if (auto locked = weak_self.lock()) {
                        locked->EnqueuePreviewReply(rid, std::move(data));
                    }
                });
            if (st != PULSE_OK) {
                self.active_previews.erase(rid);
                {
                    std::lock_guard lock(self.ready_mutex);
                    std::erase_if(self.ready_previews,
                                  [rid](const ReadyPreviewReply& item) { return item.id == rid; });
                }
                return self.Fail(st, "read_preview", "", "", "failed to start asynchronous preview read");
            }
            *out_request_id = rid;
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL CancelPreview(void* context, PulseShellPreviewRequestId request_id) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (request_id == 0)
                return self.Fail(PULSE_ERROR_ARGUMENT, "cancel_preview", "", "request_id",
                                 "request_id must be non-zero");
            if (!self.active_previews.erase(request_id))
                return self.Fail(PULSE_ERROR_STATE, "cancel_preview", "", "request_id",
                                 "unknown, completed, or already cancelled preview request_id");
            {
                std::lock_guard lock(self.ready_mutex);
                std::erase_if(self.ready_previews,
                              [request_id](const ReadyPreviewReply& item) { return item.id == request_id; });
            }
            if (self.backend.cancel_preview) {
                try {
                    self.backend.cancel_preview(request_id);
                } catch (...) {}
            }
            return PULSE_OK;
        });
    }

    void PollCompletions() noexcept {
        if (!Valid()) return;
        std::deque<ReadyMenuReply> menu_batch;
        std::deque<ReadyPreviewReply> preview_batch;
        {
            std::lock_guard lock(ready_mutex);
            menu_batch.swap(ready_menus);
            preview_batch.swap(ready_previews);
        }

        for (auto& item : menu_batch) {
            if (!Valid()) break;
            const auto it = active_menus.find(item.id);
            if (it == active_menus.end()) continue;
            const ActiveMenuSession sess = it->second;
            if (item.reply.status != PULSE_OK) {
                active_menus.erase(it);
            }
            if (!sess.callback) continue;

            std::vector<PulseShellMenuItem> abi_items;
            try {
                abi_items.reserve(item.reply.items.size());
                for (const auto& m : item.reply.items) {
                    PulseShellMenuItem abi{};
                    abi.item_id = m.item_id;
                    abi.flags = m.flags;
                    abi.depth = m.depth;
                    abi.mnemonic_codepoint = m.mnemonic_codepoint;
                    abi.verb = View(m.verb);
                    abi.label = View(m.label);
                    abi.handler_name = View(m.handler_name);
                    abi_items.push_back(abi);
                }
            } catch (...) {
                continue;
            }

            PulseShellMenuReply abi_reply{};
            abi_reply.size = sizeof(PulseShellMenuReply);
            abi_reply.status = item.reply.status;
            abi_reply.session_id = sess.id;
            abi_reply.items = abi_items.empty() ? nullptr : abi_items.data();
            abi_reply.item_count = static_cast<uint32_t>(abi_items.size());
            abi_reply.partial = item.reply.partial ? 1u : 0u;

            ++callbacks;
            try {
                sess.callback(sess.user, &abi_reply);
            } catch (...) {}
            --callbacks;
        }

        for (auto& item : preview_batch) {
            if (!Valid()) break;
            const auto it = active_previews.find(item.id);
            if (it == active_previews.end()) continue;
            const ActivePreviewRequest req = it->second;
            active_previews.erase(it);
            if (!req.callback) continue;

            PulseShellPreviewSummary summary{};
            summary.size = sizeof(PulseShellPreviewSummary);
            summary.status = item.data.status;
            summary.request_id = req.id;
            summary.path = View(item.data.path);
            summary.display_type = View(item.data.display_type);
            summary.text_excerpt = View(item.data.text_excerpt);
            summary.size_bytes = item.data.size_bytes;
            summary.modified_unix_ms = item.data.modified_unix_ms;
            summary.image_width = item.data.image_width;
            summary.image_height = item.data.image_height;
            summary.thumb_width = item.data.thumb_width;
            summary.thumb_height = item.data.thumb_height;
            summary.thumb_stride = item.data.thumb_stride;
            summary.thumb_byte_count = static_cast<uint32_t>(item.data.thumb_bgra.size());
            summary.thumb_bgra = item.data.thumb_bgra.empty() ? nullptr : item.data.thumb_bgra.data();

            ++callbacks;
            try {
                req.callback(req.user, &summary);
            } catch (...) {}
            --callbacks;
        }
    }
};

ShellAdapter::ShellAdapter(ShellBackend backend, std::function<bool()> alive,
                           std::shared_ptr<DiagnosticsSink> diagnostics,
                           std::function<void()> wake)
    : state_(std::make_shared<State>(std::move(backend), std::move(alive), std::move(diagnostics), std::move(wake))) {}

ShellAdapter::~ShellAdapter() { Close(); }

void ShellAdapter::SetDiagnostics(std::shared_ptr<DiagnosticsSink> sink) {
    if (state_) state_->diagnostics = std::move(sink);
}

void ShellAdapter::SetWake(std::function<void()> wake) {
    if (!state_) return;
    std::lock_guard lock(state_->wake_mutex);
    state_->wake = std::move(wake);
}

PulseStatus ShellAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    try {
        if (!state_->Valid()) return PULSE_ERROR_STATE;
        if (version != PULSE_SHELL_VERSION_1) return PULSE_ERROR_ABI;
        if (!out || size < sizeof(PulseShellV1)) return PULSE_ERROR_ARGUMENT;
        const PulseShellV1 table{
            sizeof(PulseShellV1),
            PULSE_SHELL_VERSION_1,
            state_.get(),
            State::GetTagCatalog,
            State::GetPathTags,
            State::SetPathTag,
            State::QueryContextMenu,
            State::InvokeContextMenu,
            State::CloseContextMenu,
            State::ReadPreview,
            State::CancelPreview,
        };
        std::memcpy(out, &table, sizeof(table));
        return PULSE_OK;
    } catch (...) {
        return PULSE_ERROR_STATE;
    }
}

void ShellAdapter::Poll() noexcept {
    if (state_) state_->PollCompletions();
}

bool ShellAdapter::HasPendingWork() const noexcept {
    if (!state_ || state_->closed) return false;
    if (!state_->active_previews.empty()) return true;
    std::lock_guard lock(state_->ready_mutex);
    return !state_->ready_menus.empty() || !state_->ready_previews.empty();
}

void ShellAdapter::Close() noexcept {
    if (!state_ || GetCurrentThreadId() != state_->thread || state_->closed) return;
    state_->closed = true;
    std::vector<PulseShellMenuSessionId> menus_to_close;
    std::vector<PulseShellPreviewRequestId> previews_to_cancel;
    try {
        menus_to_close.reserve(state_->active_menus.size());
        for (const auto& [sid, _] : state_->active_menus) menus_to_close.push_back(sid);
        previews_to_cancel.reserve(state_->active_previews.size());
        for (const auto& [rid, _] : state_->active_previews) previews_to_cancel.push_back(rid);
    } catch (...) {}
    state_->active_menus.clear();
    state_->active_previews.clear();
    {
        std::lock_guard lock(state_->ready_mutex);
        state_->ready_menus.clear();
        state_->ready_previews.clear();
    }
    if (state_->backend.close_context_menu) {
        for (const auto sid : menus_to_close) {
            try {
                state_->backend.close_context_menu(sid);
            } catch (...) {}
        }
    }
    if (state_->backend.cancel_preview) {
        for (const auto rid : previews_to_cancel) {
            try {
                state_->backend.cancel_preview(rid);
            } catch (...) {}
        }
    }
}

bool ShellAdapter::CallbackActive() const noexcept {
    return state_ && state_->callbacks > 0;
}

} // namespace pulse::extensions::native

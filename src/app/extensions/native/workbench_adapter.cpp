#include "workbench_adapter.h"
#include "diagnostics_adapter.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>

namespace pulse::extensions::native {
namespace {

std::atomic<uint64_t> g_next_action_token{1};
std::mutex g_token_mutex;

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

bool ValidActionId(PulseStringView view, std::string& out) {
    out.clear();
    if (!view.data || view.length == 0 || view.length > 64) return false;
    out.assign(view.data, view.length);
    return std::all_of(out.begin(), out.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '-';
    });
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

struct WorkbenchAdapter::State : std::enable_shared_from_this<State> {
    struct PendingNotification {
        uint64_t notification_id = 0;
        PulseWorkbenchActionFn callback = nullptr;
        void* user = nullptr;
        std::vector<std::pair<uint64_t, std::string>> tokens;
    };

    struct AppearanceSubscriptionRecord {
        PulseAppearanceSubscription id = 0;
        PulseWorkbenchAppearanceFn callback = nullptr;
        void* user = nullptr;
        bool initial_delivered = false;
    };

    struct LocaleSubscriptionRecord {
        PulseLocaleSubscription id = 0;
        PulseWorkbenchLocaleFn callback = nullptr;
        void* user = nullptr;
        bool initial_delivered = false;
    };

    static bool AppearanceEquals(const PulseHostAppearance& a, const PulseHostAppearance& b) noexcept {
        return a.flags == b.flags &&
               a.window_effect == b.window_effect &&
               a.opacity_percent == b.opacity_percent &&
               a.accent_argb == b.accent_argb &&
               a.font_scale_milli == b.font_scale_milli;
    }

    static PulseHostAppearance NormalizeAppearance(PulseHostAppearance value) noexcept {
        value.size = sizeof(PulseHostAppearance);
        value.flags = (value.flags & PULSE_APPEARANCE_DARK) ? PULSE_APPEARANCE_DARK : PULSE_APPEARANCE_LIGHT;
        value.window_effect = std::min<uint32_t>(value.window_effect, PULSE_BACKDROP_MICA_ALT);
        value.opacity_percent = std::clamp<uint32_t>(value.opacity_percent, 30u, 100u);
        if (value.font_scale_milli == 0) value.font_scale_milli = 1000u;
        return value;
    }

    static void CopyNormalizedTag(char (&dst)[32], const char* src, const char* fallback) noexcept {
        std::array<char, 32> clean{};
        size_t len = 0;
        if (src) {
            while (len + 1 < clean.size() && src[len] != '\0') {
                clean[len] = src[len];
                ++len;
            }
        }
        if (len == 0 && fallback) {
            while (len + 1 < clean.size() && fallback[len] != '\0') {
                clean[len] = fallback[len];
                ++len;
            }
        }
        std::memcpy(dst, clean.data(), sizeof(dst));
    }

    static bool LocaleEquals(const PulseHostLocale& a, const PulseHostLocale& b) noexcept {
        return a.flags == b.flags &&
               std::strcmp(a.language_tag, b.language_tag) == 0 &&
               std::strcmp(a.preference_tag, b.preference_tag) == 0;
    }

    static PulseHostLocale NormalizeLocale(PulseHostLocale value) noexcept {
        value.size = sizeof(PulseHostLocale);
        value.flags = (value.flags & PULSE_LOCALE_FOLLOW_SYSTEM) ? PULSE_LOCALE_FOLLOW_SYSTEM : 0u;
        char lang_tmp[32]{};
        char pref_tmp[32]{};
        std::memcpy(lang_tmp, value.language_tag, sizeof(lang_tmp));
        std::memcpy(pref_tmp, value.preference_tag, sizeof(pref_tmp));
        lang_tmp[sizeof(lang_tmp) - 1] = '\0';
        pref_tmp[sizeof(pref_tmp) - 1] = '\0';
        CopyNormalizedTag(value.language_tag, lang_tmp, "en-US");
        CopyNormalizedTag(value.preference_tag, pref_tmp,
                          (value.flags & PULSE_LOCALE_FOLLOW_SYSTEM) ? "system" : value.language_tag);
        return value;
    }

    static auto& GlobalTokenMap() {
        static auto* table = new std::map<uint64_t, std::weak_ptr<State>>();
        return *table;
    }

    DWORD thread = GetCurrentThreadId();
    WorkbenchBackend backend;
    std::function<bool()> alive;
    std::shared_ptr<DiagnosticsSink> diagnostics;
    std::function<void()> wake;
    bool closed = false;
    int callbacks = 0;
    uint64_t next_notification_id = 1;
    std::deque<PendingNotification> pending;
    PulseAppearanceSubscription next_appearance_sub = 1;
    std::map<PulseAppearanceSubscription, AppearanceSubscriptionRecord> appearance_subs;
    PulseHostAppearance last_appearance{};
    bool has_last_appearance = false;
    PulseLocaleSubscription next_locale_sub = 1;
    std::map<PulseLocaleSubscription, LocaleSubscriptionRecord> locale_subs;
    PulseHostLocale last_locale{};
    bool has_last_locale = false;

    State(WorkbenchBackend impl, std::function<bool()> live, std::shared_ptr<DiagnosticsSink> sink,
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
        if (diagnostics) diagnostics->Record(status, PULSE_WORKBENCH_INTERFACE_NAME, op, subject, field, msg);
        return status;
    }

    template<class F> static PulseStatus Guard(void* context, F action) noexcept {
        if (!context) return PULSE_ERROR_ARGUMENT;
        try {
            auto self = static_cast<State*>(context)->shared_from_this();
            return self->Valid() ? action(*self) : PULSE_ERROR_STATE;
        } catch (...) {
            return PULSE_ERROR_STATE;
        }
    }

    void UnregisterTokens(const PendingNotification& entry) noexcept {
        std::lock_guard lock(g_token_mutex);
        auto& map = GlobalTokenMap();
        for (const auto& [token, _] : entry.tokens) {
            map.erase(token);
        }
    }

    void ClearAllTokens() noexcept {
        std::lock_guard lock(g_token_mutex);
        auto& map = GlobalTokenMap();
        for (const auto& entry : pending) {
            for (const auto& [token, _] : entry.tokens) {
                map.erase(token);
            }
        }
        pending.clear();
    }

    bool ParseActions(std::string_view op, const PulseWorkbenchAction* actions, uint32_t count,
                      bool assign_tokens, std::vector<WorkbenchActionSpec>& out) {
        out.clear();
        if (count > 2 || (count && !actions)) {
            Fail(PULSE_ERROR_ARGUMENT, op, "", "actions", "action_count must be 0..2 with a valid actions pointer");
            return false;
        }
        std::set<std::string> unique_ids;
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto& src = actions[i];
            const std::string sub = "actions[" + std::to_string(i) + "]";
            if (src.size < sizeof(PulseWorkbenchAction) || src.reserved != 0) {
                Fail(PULSE_ERROR_ARGUMENT, op, sub, "size", "invalid PulseWorkbenchAction size or reserved field");
                return false;
            }
            WorkbenchActionSpec item;
            if (!ValidActionId(src.id, item.id)) {
                Fail(PULSE_ERROR_ARGUMENT, op, sub, "id",
                     "action id must be 1..64 ASCII [A-Za-z0-9._-] bytes");
                return false;
            }
            if (!unique_ids.insert(item.id).second) {
                Fail(PULSE_ERROR_ARGUMENT, op, sub, "id", "duplicate action id");
                return false;
            }
            if (!ValidUtf8Wide(src.label, 64, true, item.label)) {
                Fail(PULSE_ERROR_ARGUMENT, op, sub, "label", "action label must be 1..64 valid UTF-8 bytes");
                return false;
            }
            if (assign_tokens) {
                item.token = g_next_action_token.fetch_add(1, std::memory_order_relaxed);
            }
            out.push_back(std::move(item));
        }
        return true;
    }

    void RegisterPending(uint64_t notification_id, const std::vector<WorkbenchActionSpec>& actions,
                         PulseWorkbenchActionFn callback, void* user) {
        if (!callback || actions.empty()) return;
        while (pending.size() >= 32) {
            UnregisterTokens(pending.front());
            pending.pop_front();
        }
        PendingNotification record;
        record.notification_id = notification_id;
        record.callback = callback;
        record.user = user;
        record.tokens.reserve(actions.size());
        {
            std::lock_guard lock(g_token_mutex);
            auto& map = GlobalTokenMap();
            for (const auto& a : actions) {
                if (a.token != 0) {
                    record.tokens.emplace_back(a.token, a.id);
                    map[a.token] = weak_from_this();
                }
            }
        }
        pending.push_back(std::move(record));
    }

    bool TriggerToken(uint64_t token) noexcept {
        if (token == 0 || !Valid()) return false;
        PendingNotification matched{};
        std::string action_id;
        bool found = false;
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            for (const auto& [tok, id] : it->tokens) {
                if (tok == token) {
                    action_id = id;
                    matched = std::move(*it);
                    pending.erase(it);
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
        if (!found) return false;
        UnregisterTokens(matched);
        if (!matched.callback) return false;
        ++callbacks;
        try {
            matched.callback(matched.user, matched.notification_id, View(action_id));
        } catch (...) {}
        --callbacks;
        return true;
    }

    static PulseStatus PULSE_CALL ShowSnack(void* context, const PulseSnackRequest* request,
                                            PulseWorkbenchActionFn on_action, void* user,
                                            PulseNotificationId* out_id) noexcept {
        if (out_id) *out_id = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!request || request->size < sizeof(PulseSnackRequest))
                return self.Fail(PULSE_ERROR_ARGUMENT, "show_snack", "", "size",
                                 "null request or invalid PulseSnackRequest size");
            if ((request->flags & ~static_cast<uint32_t>(PULSE_SNACK_ALL_FLAGS)) != 0 || request->reserved != 0)
                return self.Fail(PULSE_ERROR_ARGUMENT, "show_snack", "", "flags",
                                 "invalid snack flags or non-zero reserved field");
            if (request->kind > PULSE_NOTICE_ERROR)
                return self.Fail(PULSE_ERROR_ARGUMENT, "show_snack", "", "kind", "unknown notice kind");
            SnackPresentationSpec spec;
            spec.flags = request->flags;
            spec.kind = request->kind;
            spec.duration_ms = request->duration_ms == 0
                ? 5000u : std::clamp(request->duration_ms, 1500u, 15000u);
            if (!ValidUtf8Wide(request->text, 256, true, spec.text) ||
                !ValidUtf8Wide(request->detail, 256, false, spec.detail))
                return self.Fail(PULSE_ERROR_ARGUMENT, "show_snack", "", "text",
                                 "invalid UTF-8 or length in snack text (1..256) or detail (0..256)");
            if (!self.ParseActions("show_snack", request->actions, request->action_count,
                                   on_action != nullptr, spec.actions))
                return PULSE_ERROR_ARGUMENT;
            if (!self.backend.show_snack)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "show_snack", "", "", "workbench snack backend is unavailable");
            const uint64_t id = self.next_notification_id++;
            spec.notification_id = id;
            const auto actions_copy = spec.actions;
            if (!self.backend.show_snack(std::move(spec)))
                return self.Fail(PULSE_ERROR_INITIALIZATION, "show_snack", "", "", "failed to display snack notification");
            self.RegisterPending(id, actions_copy, on_action, user);
            if (out_id) *out_id = id;
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL ShowNotice(void* context, const PulseNoticeRequest* request,
                                             PulseWorkbenchActionFn on_action, void* user,
                                             PulseNotificationId* out_id) noexcept {
        if (out_id) *out_id = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!request || request->size < sizeof(PulseNoticeRequest))
                return self.Fail(PULSE_ERROR_ARGUMENT, "show_notice", "", "size",
                                 "null request or invalid PulseNoticeRequest size");
            if (request->reserved != 0 || request->kind > PULSE_NOTICE_ERROR)
                return self.Fail(PULSE_ERROR_ARGUMENT, "show_notice", "", "kind",
                                 "invalid notice kind or non-zero reserved field");
            NoticePresentationSpec spec;
            spec.kind = request->kind;
            if (!ValidUtf8Wide(request->title, 128, true, spec.title) ||
                !ValidUtf8Wide(request->message, 1024, true, spec.message))
                return self.Fail(PULSE_ERROR_ARGUMENT, "show_notice", "", "title",
                                 "invalid UTF-8 or length in notice title (1..128) or message (1..1024)");
            if (!self.ParseActions("show_notice", request->actions, request->action_count,
                                   on_action != nullptr, spec.actions))
                return PULSE_ERROR_ARGUMENT;
            if (!self.backend.show_notice)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "show_notice", "", "", "workbench notice backend is unavailable");
            const uint64_t id = self.next_notification_id++;
            spec.notification_id = id;
            const auto actions_copy = spec.actions;
            if (!self.backend.show_notice(std::move(spec)))
                return self.Fail(PULSE_ERROR_INITIALIZATION, "show_notice", "", "", "failed to display notice card");
            self.RegisterPending(id, actions_copy, on_action, user);
            if (out_id) *out_id = id;
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL RevealPath(void* context, PulseStringView path, uint32_t mode) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (mode > PULSE_NAVIGATE_REVEAL_ITEM)
                return self.Fail(PULSE_ERROR_ARGUMENT, "reveal_path", "", "mode", "unsupported navigation mode");
            std::wstring wide_path;
            if (!ValidUtf8Wide(path, 32768, true, wide_path) || !ValidAbsolutePath(wide_path))
                return self.Fail(PULSE_ERROR_ARGUMENT, "reveal_path", "", "path",
                                 "path must be a valid UTF-8 absolute drive or UNC path");
            if (!self.backend.reveal_path)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "reveal_path", "", "", "workbench navigation backend is unavailable");
            const PulseStatus status = self.backend.reveal_path(wide_path, mode);
            if (status != PULSE_OK)
                return self.Fail(status, "reveal_path", "", "path", "host rejected path navigation/reveal");
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL ShowTextDiff(void* context, PulseStringView left_path,
                                               PulseStringView right_path) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            std::wstring left, right;
            if (!ValidUtf8Wide(left_path, 32768, true, left) || !ValidAbsolutePath(left))
                return self.Fail(PULSE_ERROR_ARGUMENT, "show_text_diff", "", "left_path",
                                 "left_path must be a valid UTF-8 absolute drive or UNC path");
            if (!ValidUtf8Wide(right_path, 32768, true, right) || !ValidAbsolutePath(right))
                return self.Fail(PULSE_ERROR_ARGUMENT, "show_text_diff", "", "right_path",
                                 "right_path must be a valid UTF-8 absolute drive or UNC path");
            if (!self.backend.show_text_diff)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "show_text_diff", "", "", "text diff viewer is unavailable");
            const PulseStatus status = self.backend.show_text_diff(left, right);
            if (status != PULSE_OK)
                return self.Fail(status, "show_text_diff", "", "", "failed to open text diff window");
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL SetClipboardText(void* context, PulseStringView text) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            std::wstring wide;
            if (!ValidUtf8Wide(text, 1048576, false, wide))
                return self.Fail(PULSE_ERROR_ARGUMENT, "set_clipboard_text", "", "text",
                                 "clipboard text must be <=1 MiB valid UTF-8 without NUL");
            if (!self.backend.set_clipboard_text)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "set_clipboard_text", "", "", "clipboard backend is unavailable");
            const PulseStatus status = self.backend.set_clipboard_text(wide);
            if (status != PULSE_OK)
                return self.Fail(status, "set_clipboard_text", "", "", "failed to write text to system clipboard");
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL OpenSearch(void* context, PulseStringView query) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            std::wstring wide_query;
            if (!ValidUtf8Wide(query, 512, true, wide_query))
                return self.Fail(PULSE_ERROR_ARGUMENT, "open_search", "", "query",
                                 "query must be 1..512 valid UTF-8 bytes without NUL");
            for (const wchar_t c : wide_query) {
                if (c < 0x20) {
                    return self.Fail(PULSE_ERROR_ARGUMENT, "open_search", "", "query",
                                     "query must not contain control characters");
                }
            }
            if (!self.backend.open_search)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "open_search", "", "", "workbench search backend is unavailable");
            const PulseStatus status = self.backend.open_search(wide_query);
            if (status != PULSE_OK)
                return self.Fail(status, "open_search", "", "query", "host rejected search tab open");
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL RevealPathFromRect(void* context, PulseStringView path, uint32_t mode,
                                                     const PulseScreenRect* origin_rect) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (mode > PULSE_NAVIGATE_DISMISS_HOVER_PREVIEW)
                return self.Fail(PULSE_ERROR_ARGUMENT, "reveal_path_from_rect", "", "mode",
                                 "unsupported navigation mode");
            std::wstring wide_path;
            if (!ValidUtf8Wide(path, 32768, true, wide_path) || !ValidAbsolutePath(wide_path))
                return self.Fail(PULSE_ERROR_ARGUMENT, "reveal_path_from_rect", "", "path",
                                 "path must be a valid UTF-8 absolute drive or UNC path");
            if (origin_rect && (origin_rect->width < 0 || origin_rect->height < 0))
                return self.Fail(PULSE_ERROR_ARGUMENT, "reveal_path_from_rect", "", "origin_rect",
                                 "origin_rect width and height must be non-negative");
            PulseStatus status = PULSE_ERROR_UNAVAILABLE;
            if (self.backend.reveal_path_from_rect) {
                status = self.backend.reveal_path_from_rect(wide_path, mode, origin_rect);
            } else if (self.backend.reveal_path) {
                status = self.backend.reveal_path(wide_path, mode);
            } else {
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "reveal_path_from_rect", "", "",
                                 "workbench navigation backend is unavailable");
            }
            if (status != PULSE_OK)
                return self.Fail(status, "reveal_path_from_rect", "", "path", "host rejected path navigation/reveal");
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL QueryAppearance(void* context, PulseHostAppearance* out_appearance) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!out_appearance || out_appearance->size < sizeof(PulseHostAppearance))
                return self.Fail(PULSE_ERROR_ARGUMENT, "query_appearance", "", "out_appearance",
                                 "out_appearance must be non-null with size >= sizeof(PulseHostAppearance)");
            if (!self.backend.query_appearance)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "query_appearance", "", "",
                                 "workbench appearance backend is unavailable");
            PulseHostAppearance resolved{};
            resolved.size = sizeof(PulseHostAppearance);
            const PulseStatus status = self.backend.query_appearance(resolved);
            if (status != PULSE_OK)
                return self.Fail(status, "query_appearance", "", "", "failed to query host appearance");
            resolved = NormalizeAppearance(resolved);
            std::memcpy(out_appearance, &resolved, sizeof(PulseHostAppearance));
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL SubscribeAppearance(void* context, PulseWorkbenchAppearanceFn callback,
                                                      void* user,
                                                      PulseAppearanceSubscription* out_subscription) noexcept {
        if (out_subscription) *out_subscription = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!callback || !out_subscription)
                return self.Fail(PULSE_ERROR_ARGUMENT, "subscribe_appearance", "", "callback",
                                 "callback and out_subscription must be non-null");
            if (!self.backend.query_appearance)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "subscribe_appearance", "", "",
                                 "workbench appearance backend is unavailable");
            if (self.appearance_subs.size() >= 16)
                return self.Fail(PULSE_ERROR_STATE, "subscribe_appearance", "", "subscriptions",
                                 "maximum 16 appearance subscriptions per session");
            const PulseAppearanceSubscription id = self.next_appearance_sub++;
            self.appearance_subs.emplace(id, AppearanceSubscriptionRecord{id, callback, user, false});
            *out_subscription = id;
            if (self.wake) self.wake();
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL UnsubscribeAppearance(void* context,
                                                        PulseAppearanceSubscription subscription) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (subscription == 0)
                return self.Fail(PULSE_ERROR_ARGUMENT, "unsubscribe_appearance", "", "subscription",
                                 "subscription must be non-zero");
            if (!self.appearance_subs.erase(subscription))
                return self.Fail(PULSE_ERROR_STATE, "unsubscribe_appearance", "", "subscription",
                                 "unknown or already unsubscribed appearance token");
            return PULSE_OK;
        });
    }

    void SyncAppearance() noexcept {
        if (!Valid() || appearance_subs.empty() || !backend.query_appearance) return;
        PulseHostAppearance current{};
        current.size = sizeof(PulseHostAppearance);
        try {
            if (backend.query_appearance(current) != PULSE_OK) return;
        } catch (...) {
            return;
        }
        current = NormalizeAppearance(current);
        const bool changed = !has_last_appearance || !AppearanceEquals(last_appearance, current);
        std::vector<AppearanceSubscriptionRecord> due;
        try {
            for (auto& [id, sub] : appearance_subs) {
                (void)id;
                if (changed || !sub.initial_delivered) {
                    sub.initial_delivered = true;
                    due.push_back(sub);
                }
            }
        } catch (...) {
            return;
        }
        last_appearance = current;
        has_last_appearance = true;
        for (const auto& sub : due) {
            if (!Valid() || !appearance_subs.contains(sub.id) || !sub.callback) continue;
            ++callbacks;
            try {
                sub.callback(sub.user, &current);
            } catch (...) {}
            --callbacks;
        }
    }

    static PulseStatus PULSE_CALL QueryLocale(void* context, PulseHostLocale* out_locale) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!out_locale || out_locale->size < sizeof(PulseHostLocale))
                return self.Fail(PULSE_ERROR_ARGUMENT, "query_locale", "", "out_locale",
                                 "out_locale must be non-null with size >= sizeof(PulseHostLocale)");
            if (!self.backend.query_locale)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "query_locale", "", "",
                                 "workbench locale backend is unavailable");
            PulseHostLocale resolved{};
            resolved.size = sizeof(PulseHostLocale);
            const PulseStatus status = self.backend.query_locale(resolved);
            if (status != PULSE_OK)
                return self.Fail(status, "query_locale", "", "", "failed to query host locale");
            resolved = NormalizeLocale(resolved);
            std::memcpy(out_locale, &resolved, sizeof(PulseHostLocale));
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL SubscribeLocale(void* context, PulseWorkbenchLocaleFn callback,
                                                  void* user,
                                                  PulseLocaleSubscription* out_subscription) noexcept {
        if (out_subscription) *out_subscription = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!callback || !out_subscription)
                return self.Fail(PULSE_ERROR_ARGUMENT, "subscribe_locale", "", "callback",
                                 "callback and out_subscription must be non-null");
            if (!self.backend.query_locale)
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "subscribe_locale", "", "",
                                 "workbench locale backend is unavailable");
            if (self.locale_subs.size() >= 16)
                return self.Fail(PULSE_ERROR_STATE, "subscribe_locale", "", "subscriptions",
                                 "maximum 16 locale subscriptions per session");
            const PulseLocaleSubscription id = self.next_locale_sub++;
            self.locale_subs.emplace(id, LocaleSubscriptionRecord{id, callback, user, false});
            *out_subscription = id;
            if (self.wake) self.wake();
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL UnsubscribeLocale(void* context,
                                                    PulseLocaleSubscription subscription) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (subscription == 0)
                return self.Fail(PULSE_ERROR_ARGUMENT, "unsubscribe_locale", "", "subscription",
                                 "subscription must be non-zero");
            if (!self.locale_subs.erase(subscription))
                return self.Fail(PULSE_ERROR_STATE, "unsubscribe_locale", "", "subscription",
                                 "unknown or already unsubscribed locale token");
            return PULSE_OK;
        });
    }

    void SyncLocale() noexcept {
        if (!Valid() || locale_subs.empty() || !backend.query_locale) return;
        PulseHostLocale current{};
        current.size = sizeof(PulseHostLocale);
        try {
            if (backend.query_locale(current) != PULSE_OK) return;
        } catch (...) {
            return;
        }
        current = NormalizeLocale(current);
        const bool changed = !has_last_locale || !LocaleEquals(last_locale, current);
        std::vector<LocaleSubscriptionRecord> due;
        try {
            for (auto& [id, sub] : locale_subs) {
                (void)id;
                if (changed || !sub.initial_delivered) {
                    sub.initial_delivered = true;
                    due.push_back(sub);
                }
            }
        } catch (...) {
            return;
        }
        last_locale = current;
        has_last_locale = true;
        for (const auto& sub : due) {
            if (!Valid() || !locale_subs.contains(sub.id) || !sub.callback) continue;
            ++callbacks;
            try {
                sub.callback(sub.user, &current);
            } catch (...) {}
            --callbacks;
        }
    }
};

WorkbenchAdapter::WorkbenchAdapter(WorkbenchBackend backend, std::function<bool()> alive,
                                   std::shared_ptr<DiagnosticsSink> diagnostics,
                                   std::function<void()> wake)
    : state_(std::make_shared<State>(std::move(backend), std::move(alive), std::move(diagnostics), std::move(wake))) {}

WorkbenchAdapter::~WorkbenchAdapter() { Close(); }

void WorkbenchAdapter::SetDiagnostics(std::shared_ptr<DiagnosticsSink> sink) {
    if (state_) state_->diagnostics = std::move(sink);
}

void WorkbenchAdapter::SetWake(std::function<void()> wake) {
    if (state_) state_->wake = std::move(wake);
}

PulseStatus WorkbenchAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    try {
        if (!state_->Valid()) return PULSE_ERROR_STATE;
        if (version != PULSE_WORKBENCH_VERSION_1 && version != PULSE_WORKBENCH_VERSION_2 &&
            version != PULSE_WORKBENCH_VERSION_3)
            return PULSE_ERROR_ABI;
        if (!out) return PULSE_ERROR_ARGUMENT;
        PulseWorkbenchV3 table{
            sizeof(PulseWorkbenchV3),
            version,
            state_.get(),
            State::ShowSnack,
            State::ShowNotice,
            State::RevealPath,
            State::ShowTextDiff,
            State::SetClipboardText,
            State::OpenSearch,
            State::RevealPathFromRect,
            State::QueryAppearance,
            State::SubscribeAppearance,
            State::UnsubscribeAppearance,
            State::QueryLocale,
            State::SubscribeLocale,
            State::UnsubscribeLocale,
        };
        if (version == PULSE_WORKBENCH_VERSION_1) {
            if (size < sizeof(PulseWorkbenchV1)) return PULSE_ERROR_ARGUMENT;
            table.size = sizeof(PulseWorkbenchV1);
            std::memcpy(out, &table, sizeof(PulseWorkbenchV1));
            return PULSE_OK;
        }
        if (version == PULSE_WORKBENCH_VERSION_2) {
            if (size < sizeof(PulseWorkbenchV2)) return PULSE_ERROR_ARGUMENT;
            table.size = sizeof(PulseWorkbenchV2);
            std::memcpy(out, &table, sizeof(PulseWorkbenchV2));
            return PULSE_OK;
        }
        if (size < sizeof(PulseWorkbenchV3)) return PULSE_ERROR_ARGUMENT;
        std::memcpy(out, &table, sizeof(PulseWorkbenchV3));
        return PULSE_OK;
    } catch (...) {
        return PULSE_ERROR_STATE;
    }
}

bool WorkbenchAdapter::InvokeActionToken(uint64_t token) noexcept {
    return state_ && state_->TriggerToken(token);
}

void WorkbenchAdapter::Sync() noexcept {
    if (!state_) return;
    state_->SyncAppearance();
    state_->SyncLocale();
}

bool WorkbenchAdapter::HasSubscriptions() const noexcept {
    return state_ && !state_->closed && (!state_->appearance_subs.empty() || !state_->locale_subs.empty());
}

bool WorkbenchAdapter::DispatchHostActionToken(uint64_t token) noexcept {
    std::shared_ptr<State> target;
    {
        std::lock_guard lock(g_token_mutex);
        auto& map = State::GlobalTokenMap();
        const auto it = map.find(token);
        if (it != map.end()) target = it->second.lock();
    }
    return target && target->TriggerToken(token);
}

void WorkbenchAdapter::Close() noexcept {
    if (!state_ || GetCurrentThreadId() != state_->thread || state_->closed) return;
    state_->closed = true;
    state_->appearance_subs.clear();
    state_->locale_subs.clear();
    state_->ClearAllTokens();
}

bool WorkbenchAdapter::CallbackActive() const noexcept {
    return state_ && state_->callbacks > 0;
}

} // namespace pulse::extensions::native

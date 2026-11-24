#include "columns_adapter.h"
#include "diagnostics_adapter.h"
#include "../icon_registry.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <utility>

namespace pulse::extensions::native {
namespace {

std::string_view View(PulseStringView view) {
    return view.data && view.length ? std::string_view(view.data, view.length) : std::string_view{};
}

bool ValidId(std::string_view id) {
    if (id.empty() || id.size() > 64 || id == "." || id == "..") return false;
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// UTF-8 text without control characters, 1..max_bytes (0.. when optional).
bool ValidText(PulseStringView view, size_t max_bytes, bool required, std::wstring& out) {
    out.clear();
    if (view.length == 0) return !required;
    if (!view.data || view.length > max_bytes) return false;
    const std::string_view text(view.data, view.length);
    for (const unsigned char ch : text)
        if (ch < 0x20u || ch == 0x7Fu) return false;
    return columns::Utf8ToWide(text, out) && (!required || !out.empty());
}

} // namespace

struct ColumnsAdapter::State : std::enable_shared_from_this<State> {
    DWORD thread = GetCurrentThreadId();
    std::string package_id;
    std::function<bool()> alive;
    std::shared_ptr<DiagnosticsSink> diagnostics;
    std::mutex wake_mutex;
    std::function<void()> wake;
    columns::Registry* registry = nullptr;
    std::shared_ptr<std::atomic<uint32_t>> active_calls = std::make_shared<std::atomic<uint32_t>>(0);
    bool closed = false;
    std::set<PulseColumnHandle> owned;

    State(std::string pkg, std::function<bool()> live, std::shared_ptr<DiagnosticsSink> sink,
          std::function<void()> wake_fn, columns::Registry* reg)
        : package_id(std::move(pkg)),
          alive(std::move(live)),
          diagnostics(std::move(sink)),
          wake(std::move(wake_fn)),
          registry(reg ? reg : &columns::Registry::Instance()) {}

    bool Valid() const noexcept {
        return GetCurrentThreadId() == thread && !closed && (!alive || alive());
    }

    PulseStatus Fail(PulseStatus status, std::string_view op, std::string_view subject,
                     std::string_view field, std::string_view msg) const {
        if (diagnostics) diagnostics->Record(status, PULSE_COLUMNS_INTERFACE_NAME, op, subject, field, msg);
        return status;
    }

    template <class F>
    static PulseStatus Guard(void* context, F action) noexcept {
        if (!context) return PULSE_ERROR_ARGUMENT;
        try {
            auto self = static_cast<State*>(context)->shared_from_this();
            if (GetCurrentThreadId() != self->thread) {
                return self->Fail(PULSE_ERROR_STATE, "", "", "thread",
                                  "pulse.columns must be called on the owner UI thread");
            }
            if (!self->Valid()) return PULSE_ERROR_STATE;
            return action(*self);
        } catch (...) {
            return PULSE_ERROR_STATE;
        }
    }

    void NotifyWake() noexcept {
        std::function<void()> fn;
        {
            std::lock_guard lock(wake_mutex);
            fn = wake;
        }
        if (fn) {
            try { fn(); } catch (...) {}
        }
    }

    // Shared by register_column and register_badge.
    PulseStatus Register(columns::Kind kind, std::string_view op, uint32_t applies_to, PulseStringView id_view,
                         PulseStringView title_view, PulseStringView extensions_view, uint32_t width_dip,
                         uint32_t align, PulseColumnQueryFn query, void* user, PulseColumnHandle* out_handle) {
        const std::string id(View(id_view));
        if (!ValidId(id))
            return Fail(PULSE_ERROR_ARGUMENT, op, id, "id", "id must be 1..64 ASCII [A-Za-z0-9._-] bytes");
        if ((applies_to & ~static_cast<uint32_t>(PULSE_COLUMN_APPLIES_ALL)) != 0u)
            return Fail(PULSE_ERROR_ARGUMENT, op, id, "applies_to", "applies_to has undocumented bits");
        std::wstring title;
        if (!ValidText(title_view, 64, true, title))
            return Fail(PULSE_ERROR_ARGUMENT, op, id, "title",
                        "title must be 1..64 valid UTF-8 bytes without control characters");
        std::wstring extensions_text;
        std::vector<std::wstring> extensions;
        if (!ValidText(extensions_view, 512, false, extensions_text) ||
            !columns::ParseExtensions(extensions_text, extensions))
            return Fail(PULSE_ERROR_ARGUMENT, op, id, "extensions",
                        "extensions must be a ';'-separated list like \".jpg;.png\" (<=512 bytes, <=64 entries)");
        if (width_dip != 0 && (width_dip < 48 || width_dip > 480))
            return Fail(PULSE_ERROR_ARGUMENT, op, id, "width_dip", "width_dip must be 0 or 48..480");
        if (align != PULSE_COLUMN_ALIGN_START && align != PULSE_COLUMN_ALIGN_END)
            return Fail(PULSE_ERROR_ARGUMENT, op, id, "align", "align must be PULSE_COLUMN_ALIGN_START or _END");
        if (!query) return Fail(PULSE_ERROR_ARGUMENT, op, id, "query", "query callback is required");

        columns::Registration reg;
        reg.kind = kind;
        reg.package_id = package_id;
        reg.id = id;
        reg.title = std::move(title);
        reg.width_dip = width_dip ? static_cast<float>(width_dip) : 120.0f;
        reg.align_end = align == PULSE_COLUMN_ALIGN_END;
        reg.applies_to = applies_to ? applies_to : static_cast<uint32_t>(PULSE_COLUMN_APPLIES_ALL);
        reg.extensions = std::move(extensions);
        reg.query = query;
        reg.user = user;
        reg.session_active_calls = active_calls;
        if (kind == columns::Kind::Badge) {
            reg.resolve_icon = [pkg = package_id](std::string_view icon) -> uint32_t {
                if (icon.starts_with("icon:")) icon.remove_prefix(5);
                const auto resolved = icons::GlobalRegistry().ResolveName(pkg, icon);
                return resolved ? *resolved : 0u;
            };
        }
        std::weak_ptr<State> weak_self = shared_from_this();
        reg.on_call_finished = [weak_self] {
            if (const auto locked = weak_self.lock()) locked->NotifyWake();
        };
        uint64_t handle = 0;
        const PulseStatus st = registry->Register(std::move(reg), handle);
        if (st != PULSE_OK)
            return Fail(st, op, id, "id",
                        st == PULSE_ERROR_STATE
                            ? "id already registered by this package, or the package has 4 providers of this kind"
                            : "invalid provider registration");
        owned.insert(handle);
        *out_handle = handle;
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL RegisterColumn(void* context, const PulseColumnDesc* desc,
                                                 PulseColumnHandle* out_handle) noexcept {
        if (out_handle) *out_handle = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!desc || !out_handle)
                return self.Fail(PULSE_ERROR_ARGUMENT, "register_column", "", "desc",
                                 "desc and out_handle must be non-null");
            if (desc->size < sizeof(PulseColumnDesc))
                return self.Fail(PULSE_ERROR_ARGUMENT, "register_column", "", "size",
                                 "desc->size must be >= sizeof(PulseColumnDesc)");
            return self.Register(columns::Kind::Column, "register_column", desc->applies_to, desc->id,
                                 desc->title, desc->extensions, desc->width_dip, desc->align, desc->query,
                                 desc->user, out_handle);
        });
    }

    static PulseStatus PULSE_CALL RegisterBadge(void* context, const PulseBadgeDesc* desc,
                                                PulseColumnHandle* out_handle) noexcept {
        if (out_handle) *out_handle = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!desc || !out_handle)
                return self.Fail(PULSE_ERROR_ARGUMENT, "register_badge", "", "desc",
                                 "desc and out_handle must be non-null");
            if (desc->size < sizeof(PulseBadgeDesc))
                return self.Fail(PULSE_ERROR_ARGUMENT, "register_badge", "", "size",
                                 "desc->size must be >= sizeof(PulseBadgeDesc)");
            return self.Register(columns::Kind::Badge, "register_badge", desc->applies_to, desc->id,
                                 desc->title, desc->extensions, 0, PULSE_COLUMN_ALIGN_START, desc->query,
                                 desc->user, out_handle);
        });
    }

    static PulseStatus PULSE_CALL SetTitle(void* context, PulseColumnHandle handle, PulseStringView title) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (handle == 0 || !self.owned.contains(handle))
                return self.Fail(PULSE_ERROR_ARGUMENT, "set_title", "", "handle", "unknown or unowned handle");
            std::wstring text;
            if (!ValidText(title, 64, true, text))
                return self.Fail(PULSE_ERROR_ARGUMENT, "set_title", "", "title",
                                 "title must be 1..64 valid UTF-8 bytes without control characters");
            return self.registry->SetTitle(handle, std::move(text)) ? PULSE_OK : PULSE_ERROR_STATE;
        });
    }

    static PulseStatus PULSE_CALL Invalidate(void* context, PulseColumnHandle handle, PulseStringView path) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (handle == 0 || !self.owned.contains(handle))
                return self.Fail(PULSE_ERROR_ARGUMENT, "invalidate", "", "handle", "unknown or unowned handle");
            std::wstring wide;
            if (path.length > 32768 || (path.length && !path.data) ||
                !columns::Utf8ToWide(View(path), wide))
                return self.Fail(PULSE_ERROR_ARGUMENT, "invalidate", "", "path",
                                 "path must be empty or valid UTF-8 (<=32768 bytes)");
            return self.registry->Invalidate(handle, wide) ? PULSE_OK : PULSE_ERROR_STATE;
        });
    }

    static PulseStatus PULSE_CALL Unregister(void* context, PulseColumnHandle handle) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (handle == 0 || !self.owned.contains(handle))
                return self.Fail(PULSE_ERROR_ARGUMENT, "unregister", "", "handle", "unknown or unowned handle");
            self.owned.erase(handle);
            // Waits for a running query of this provider (it sees is_cancelled),
            // so the plugin may free the provider's user data on return.
            self.registry->Unregister(handle, true);
            return PULSE_OK;
        });
    }
};

ColumnsAdapter::ColumnsAdapter(std::string package_id, std::function<bool()> alive,
                               std::shared_ptr<DiagnosticsSink> diagnostics, std::function<void()> wake,
                               columns::Registry* registry)
    : state_(std::make_shared<State>(std::move(package_id), std::move(alive), std::move(diagnostics),
                                     std::move(wake), registry)) {}

ColumnsAdapter::~ColumnsAdapter() { Close(); }

void ColumnsAdapter::SetDiagnostics(std::shared_ptr<DiagnosticsSink> sink) {
    if (state_) state_->diagnostics = std::move(sink);
}

PulseStatus ColumnsAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    try {
        if (!state_->Valid()) return PULSE_ERROR_STATE;
        if (version != PULSE_COLUMNS_VERSION_1) return PULSE_ERROR_ABI;
        if (!out || size < sizeof(PulseColumnsV1)) return PULSE_ERROR_ARGUMENT;
        const PulseColumnsV1 table{
            sizeof(PulseColumnsV1),
            PULSE_COLUMNS_VERSION_1,
            state_.get(),
            State::RegisterColumn,
            State::RegisterBadge,
            State::SetTitle,
            State::Invalidate,
            State::Unregister,
        };
        std::memcpy(out, &table, sizeof(table));
        return PULSE_OK;
    } catch (...) {
        return PULSE_ERROR_STATE;
    }
}

void ColumnsAdapter::Close() noexcept {
    if (!state_ || GetCurrentThreadId() != state_->thread || state_->closed) return;
    state_->closed = true;
    const auto handles = std::move(state_->owned);
    state_->owned.clear();
    for (const auto handle : handles) state_->registry->Unregister(handle);
}

bool ColumnsAdapter::CallbackActive() const noexcept {
    return state_ && state_->active_calls && state_->active_calls->load(std::memory_order_acquire) > 0;
}

} // namespace pulse::extensions::native

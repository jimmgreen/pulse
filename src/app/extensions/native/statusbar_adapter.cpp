#include "statusbar_adapter.h"
#include "diagnostics_adapter.h"
#include "../icon_registry.h"

#include <atomic>
#include <cstring>
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

bool Utf8ToWide(std::string_view text, std::wstring& out) {
    out.clear();
    if (text.empty()) return true;
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return false;
    out.resize(static_cast<size_t>(n));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), n) == n;
}

// UTF-8 text without control characters, 0..max_bytes.
bool ValidText(PulseStringView view, size_t max_bytes, std::wstring& out) {
    out.clear();
    if (view.length == 0) return true;
    if (!view.data || view.length > max_bytes) return false;
    const std::string_view text(view.data, view.length);
    for (const unsigned char ch : text)
        if (ch < 0x20u || ch == 0x7Fu) return false;
    return Utf8ToWide(text, out);
}

} // namespace

struct StatusBarAdapter::State : std::enable_shared_from_this<State> {
    DWORD thread = GetCurrentThreadId();
    std::string package_id;
    std::function<bool()> alive;
    std::shared_ptr<DiagnosticsSink> diagnostics;
    statusbar::Registry* registry = nullptr;
    uint64_t owner_key = 0;
    int callbacks = 0;
    bool closed = false;
    std::set<PulseStatusItemHandle> owned;

    State(std::string pkg, std::function<bool()> live, std::shared_ptr<DiagnosticsSink> sink, statusbar::Registry* reg)
        : package_id(std::move(pkg)),
          alive(std::move(live)),
          diagnostics(std::move(sink)),
          registry(reg ? reg : &statusbar::Registry::Instance()) {
        static std::atomic<uint64_t> next_owner{1};
        owner_key = next_owner.fetch_add(1);
    }

    bool Valid() const noexcept { return GetCurrentThreadId() == thread && !closed && (!alive || alive()); }

    PulseStatus Fail(PulseStatus status, std::string_view op, std::string_view subject,
                     std::string_view field, std::string_view msg) const {
        if (diagnostics) diagnostics->Record(status, PULSE_STATUSBAR_INTERFACE_NAME, op, subject, field, msg);
        return status;
    }

    template <class F>
    static PulseStatus Guard(void* context, F action) noexcept {
        if (!context) return PULSE_ERROR_ARGUMENT;
        try {
            auto self = static_cast<State*>(context)->shared_from_this();
            if (GetCurrentThreadId() != self->thread)
                return self->Fail(PULSE_ERROR_STATE, "", "", "thread", "pulse.statusbar must be called on the owner UI thread");
            if (!self->Valid()) return PULSE_ERROR_STATE;
            return action(*self);
        } catch (...) {
            return PULSE_ERROR_STATE;
        }
    }

    // Validates desc into item (text, tooltip, icon, tone, click). id is checked by the caller.
    PulseStatus Fill(std::string_view op, const std::string& id, const PulseStatusItemDesc& desc, statusbar::Item& item) {
        if (desc.tone > PULSE_STATUS_TONE_DANGER)
            return Fail(PULSE_ERROR_ARGUMENT, op, id, "tone", "tone must be one of PULSE_STATUS_TONE_*");
        if (!ValidText(desc.text, 64, item.text))
            return Fail(PULSE_ERROR_ARGUMENT, op, id, "text", "text must be 0..64 valid UTF-8 bytes without control characters");
        if (!ValidText(desc.tooltip, 256, item.tooltip))
            return Fail(PULSE_ERROR_ARGUMENT, op, id, "tooltip",
                        "tooltip must be 0..256 valid UTF-8 bytes without control characters");
        std::string_view icon = View(desc.icon);
        if (icon.size() > 96) return Fail(PULSE_ERROR_ARGUMENT, op, id, "icon", "icon must be 0..96 bytes");
        if (icon.starts_with("icon:")) icon.remove_prefix(5);
        item.icon = 0;
        if (!icon.empty()) {
            const auto resolved = icons::GlobalRegistry().ResolveName(package_id, icon);
            if (!resolved)
                return Fail(PULSE_ERROR_ARGUMENT, op, id, "icon",
                            "icon is not a built-in icon, a package icon, \"<package>/<name>\" or \"#<id>\"");
            item.icon = *resolved;
        }
        if (item.text.empty() && item.icon == 0)
            return Fail(PULSE_ERROR_ARGUMENT, op, id, "text", "an item needs text or an icon");
        item.tone = desc.tone;
        item.click = nullptr;
        if (desc.on_click) {
            std::weak_ptr<State> weak = weak_from_this();
            const auto fn = desc.on_click;
            void* const user = desc.user;
            item.click = [weak, fn, user, handle = item.handle]() {
                auto self = weak.lock();
                if (!self || !self->Valid() || !self->owned.contains(handle)) return;
                ++self->callbacks;
                try { fn(user, handle); } catch (...) {}
                --self->callbacks;
            };
        }
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL AddItem(void* context, const PulseStatusItemDesc* desc, PulseStatusItemHandle* out) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (out) *out = 0;
            if (!desc || !out) return self.Fail(PULSE_ERROR_ARGUMENT, "add_item", "", "desc", "desc and out_handle are required");
            if (desc->size < sizeof(PulseStatusItemDesc))
                return self.Fail(PULSE_ERROR_ABI, "add_item", "", "size", "desc.size is smaller than PulseStatusItemDesc");
            const std::string id(View(desc->id));
            if (!ValidId(id))
                return self.Fail(PULSE_ERROR_ARGUMENT, "add_item", id, "id", "id must be 1..64 ASCII [A-Za-z0-9._-] bytes");
            if (self.registry->HasId(self.package_id, id))
                return self.Fail(PULSE_ERROR_STATE, "add_item", id, "id", "an item with this id is already registered");
            if (self.registry->CountForPackage(self.package_id) >= kMaxItemsPerPackage)
                return self.Fail(PULSE_ERROR_STATE, "add_item", id, "", "a package may add at most 2 status bar items");
            statusbar::Item item;
            item.handle = self.registry->NewHandle();
            item.owner = self.owner_key;
            item.package_id = self.package_id;
            item.id = id;
            const PulseStatus checked = self.Fill("add_item", id, *desc, item);
            if (checked != PULSE_OK) return checked;
            const uint64_t handle = self.registry->Add(std::move(item));
            self.owned.insert(handle);
            *out = handle;
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL UpdateItem(void* context, PulseStatusItemHandle handle, const PulseStatusItemDesc* desc) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!desc) return self.Fail(PULSE_ERROR_ARGUMENT, "update_item", "", "desc", "desc is required");
            if (desc->size < sizeof(PulseStatusItemDesc))
                return self.Fail(PULSE_ERROR_ABI, "update_item", "", "size", "desc.size is smaller than PulseStatusItemDesc");
            const auto* current = self.owned.contains(handle) ? self.registry->Find(handle) : nullptr;
            if (!current) return self.Fail(PULSE_ERROR_ARGUMENT, "update_item", "", "handle", "unknown status bar item");
            const std::string id(View(desc->id));
            if (!id.empty() && id != current->id)
                return self.Fail(PULSE_ERROR_ARGUMENT, "update_item", id, "id", "id must be empty or the registered id");
            statusbar::Item fields = *current;
            const PulseStatus checked = self.Fill("update_item", current->id, *desc, fields);
            if (checked != PULSE_OK) return checked;
            self.registry->Update(handle, fields);
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL SetVisible(void* context, PulseStatusItemHandle handle, uint32_t visible) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!self.owned.contains(handle) || !self.registry->SetVisible(handle, visible != 0))
                return self.Fail(PULSE_ERROR_ARGUMENT, "set_visible", "", "handle", "unknown status bar item");
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL RemoveItem(void* context, PulseStatusItemHandle handle) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!self.owned.erase(handle) || !self.registry->Remove(handle))
                return self.Fail(PULSE_ERROR_ARGUMENT, "remove_item", "", "handle", "unknown status bar item");
            return PULSE_OK;
        });
    }
};

StatusBarAdapter::StatusBarAdapter(std::string package_id, std::function<bool()> alive,
                                   std::shared_ptr<DiagnosticsSink> diagnostics, statusbar::Registry* registry)
    : state_(std::make_shared<State>(std::move(package_id), std::move(alive), std::move(diagnostics), registry)) {}

StatusBarAdapter::~StatusBarAdapter() { Close(); }

PulseStatus StatusBarAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    try {
        if (!state_->Valid()) return PULSE_ERROR_STATE;
        if (version != PULSE_STATUSBAR_VERSION_1) return PULSE_ERROR_ABI;
        if (!out || size < sizeof(PulseStatusBarV1)) return PULSE_ERROR_ARGUMENT;
        const PulseStatusBarV1 table{
            sizeof(PulseStatusBarV1),
            PULSE_STATUSBAR_VERSION_1,
            state_.get(),
            State::AddItem,
            State::UpdateItem,
            State::SetVisible,
            State::RemoveItem,
        };
        std::memcpy(out, &table, sizeof(table));
        return PULSE_OK;
    } catch (...) {
        return PULSE_ERROR_STATE;
    }
}

void StatusBarAdapter::Close() noexcept {
    if (!state_ || GetCurrentThreadId() != state_->thread || state_->closed) return;
    state_->closed = true;
    state_->owned.clear();
    try { state_->registry->RemoveOwner(state_->owner_key); } catch (...) {}
}

bool StatusBarAdapter::CallbackActive() const noexcept { return state_ && state_->callbacks > 0; }

} // namespace pulse::extensions::native

#include "settings_pages_adapter.h"
#include <cstring>
namespace pulse::extensions::native {
struct SettingsPagesAdapter::State : std::enable_shared_from_this<State> {
    DWORD thread = GetCurrentThreadId();
    HWND owner;
    bool closed = false;
    std::function<bool()> alive;
    UiAdapter ui;
    SettingsPageDescriptor page;
    State(HWND window, bool dark, D2D1_COLOR_F accent, float scale, std::function<bool()> live)
        : owner(window), alive(std::move(live)), ui(window, dark, accent, scale, [this] { return Valid(); }) { ui.Embed(); }
    bool Valid() const { return GetCurrentThreadId() == thread && !closed && IsWindow(owner) && alive && alive(); }
    bool Has(uint64_t id) const { return Valid() && id && id == page.id && ui.HasPanel(id); }
    template<class F> static PulseStatus Guard(void* context, F action) noexcept {
        if (!context) return PULSE_ERROR_ARGUMENT;
        try { auto self = static_cast<State*>(context)->shared_from_this();
            return self->Valid() ? action(*self) : PULSE_ERROR_STATE;
        } catch (...) { return PULSE_ERROR_STATE; }
    }
    static PulseStatus PULSE_CALL Create(void* context, PulseStringView title, const PulseUiModelTree* tree, uint64_t* out) {
        if (out) *out = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!out || !tree || !title.data || !title.length || title.length > 128 || std::memchr(title.data, 0, title.length)) return PULSE_ERROR_ARGUMENT;
            if (self.Has(self.page.id)) return PULSE_ERROR_STATE;
            const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, title.data, static_cast<int>(title.length), nullptr, 0);
            if (!count) return PULSE_ERROR_ARGUMENT;
            std::wstring caption(static_cast<size_t>(count), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, title.data, static_cast<int>(title.length), caption.data(), count);
            PulseUiModelV1 model{}; const auto queried = self.ui.QueryModel(1, &model, sizeof(model));
            if (queried != PULSE_OK) return queried;
            uint64_t id = 0; const auto status = model.create(model.context, title, tree, &id);
            if (status == PULSE_OK) { self.page = {id, std::move(caption)}; *out = id; if (IsWindow(self.owner)) InvalidateRect(self.owner, nullptr, FALSE); }
            return status;
        });
    }
    static PulseStatus PULSE_CALL PageQuery(void* context, uint64_t id, PulseStringView name, uint32_t version, void* out, uint32_t size) {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!self.Has(id)) return PULSE_ERROR_STATE;
            if (!name.data || !name.length || name.length > 64 || std::memchr(name.data, 0, name.length)) return PULSE_ERROR_ARGUMENT;
            const std::string value(name.data, name.length);
            if (value == "pulse.ui.model") {
                const auto status = self.ui.QueryModel(version, out, size);
                if (status == PULSE_OK) static_cast<PulseUiModelV1*>(out)->create = [](void*, PulseStringView, const PulseUiModelTree*, uint64_t* page) -> PulseStatus { if (page) *page = 0; return PULSE_ERROR_STATE; };
                return status;
            }
            if (value == "pulse.ui.events") return self.ui.QueryEvents(version, out, size);
            return PULSE_ERROR_UNAVAILABLE;
        });
    }
    static PulseStatus PULSE_CALL Remove(void* context, uint64_t id) {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!self.Has(id)) return PULSE_ERROR_STATE;
            PulseUiModelV1 api{}; const auto status = self.ui.QueryModel(1, &api, sizeof(api));
            if (status != PULSE_OK) return status;
            const auto result = api.close(api.context, id);
            if (result == PULSE_OK) { self.page = {}; if (IsWindow(self.owner)) InvalidateRect(self.owner, nullptr, FALSE); }
            return result;
        });
    }
};
SettingsPagesAdapter::SettingsPagesAdapter(HWND owner, bool dark, D2D1_COLOR_F accent, float scale, std::function<bool()> alive)
    : state_(std::make_shared<State>(owner, dark, accent, scale, std::move(alive))) {}
SettingsPagesAdapter::~SettingsPagesAdapter() { Close(); }
PulseStatus SettingsPagesAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    try {
        if (!state_->Valid()) return PULSE_ERROR_STATE;
        if (version != PULSE_SETTINGS_PAGES_VERSION_1) return PULSE_ERROR_ABI;
        if (!out || size < sizeof(PulseSettingsPagesV1)) return PULSE_ERROR_ARGUMENT;
        const PulseSettingsPagesV1 api{sizeof(api), PULSE_SETTINGS_PAGES_VERSION_1, state_.get(), State::Create, State::PageQuery, State::Remove};
        std::memcpy(out, &api, sizeof(api)); return PULSE_OK;
    } catch (...) { return PULSE_ERROR_STATE; }
}
std::optional<SettingsPageDescriptor> SettingsPagesAdapter::Page() const { return state_->Has(state_->page.id) ? std::optional(state_->page) : std::nullopt; }
bool SettingsPagesAdapter::Show(uint64_t page, RECT bounds, bool dark, D2D1_COLOR_F accent, float scale) {
    return state_->Has(page) && state_->ui.ShowEmbedded(bounds, dark, accent, scale);
}
void SettingsPagesAdapter::Hide() { state_->ui.HideEmbedded(); }
void SettingsPagesAdapter::Close() { if (GetCurrentThreadId() == state_->thread) { state_->closed = true; state_->ui.Stop(); state_->page = {}; if (IsWindow(state_->owner)) InvalidateRect(state_->owner, nullptr, FALSE); } }
bool SettingsPagesAdapter::InCallback() const { return state_->ui.InCallback(); }
UiAdapter& SettingsPagesAdapter::Ui() { return state_->ui; }
}

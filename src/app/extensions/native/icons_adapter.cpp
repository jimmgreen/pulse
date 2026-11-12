#include "icons_adapter.h"
#include "../icon_registry.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>

namespace pulse::extensions::native {

struct IconsAdapter::State {
    std::string package_id;
    std::function<bool()> live;
    DWORD thread = GetCurrentThreadId();
    bool closed = false;
    std::vector<PulseIconId> owned_icons;

    bool CheckThread() const noexcept {
        return !closed && GetCurrentThreadId() == thread && (!live || live());
    }

    static PulseStatus PULSE_CALL RegisterIcon(void* context,
                                               const PulseIconDescriptor* descriptor,
                                               PulseIconId* out_id) noexcept {
        if (out_id) *out_id = 0;
        auto* self = static_cast<State*>(context);
        if (!self || !self->CheckThread()) return PULSE_ERROR_STATE;
        if (!descriptor || !out_id) return PULSE_ERROR_ARGUMENT;
        try {
            if (self->owned_icons.size() >= icons::kMaxDynamicIconsPerSession) {
                return PULSE_ERROR_UNAVAILABLE;
            }
            PulseIconId created = 0;
            const PulseStatus status = icons::GlobalRegistry().RegisterDynamic(
                self->package_id, descriptor, &created);
            if (status == PULSE_OK) {
                self->owned_icons.push_back(created);
                *out_id = created;
            }
            return status;
        } catch (...) {
            return PULSE_ERROR_UNAVAILABLE;
        }
    }

    static PulseStatus PULSE_CALL UpdateIcon(void* context,
                                             PulseIconId id,
                                             const PulseIconDescriptor* descriptor) noexcept {
        auto* self = static_cast<State*>(context);
        if (!self || !self->CheckThread()) return PULSE_ERROR_STATE;
        if (!descriptor || !icons::IsCustomIconId(id)) return PULSE_ERROR_ARGUMENT;
        try {
            if (std::find(self->owned_icons.begin(), self->owned_icons.end(), id) == self->owned_icons.end()) {
                return PULSE_ERROR_STATE;
            }
            return icons::GlobalRegistry().UpdateDynamic(self->package_id, id, descriptor);
        } catch (...) {
            return PULSE_ERROR_UNAVAILABLE;
        }
    }

    static PulseStatus PULSE_CALL UnregisterIcon(void* context, PulseIconId id) noexcept {
        auto* self = static_cast<State*>(context);
        if (!self || !self->CheckThread()) return PULSE_ERROR_STATE;
        if (!icons::IsCustomIconId(id)) return PULSE_ERROR_ARGUMENT;
        try {
            const auto it = std::find(self->owned_icons.begin(), self->owned_icons.end(), id);
            if (it == self->owned_icons.end()) return PULSE_ERROR_STATE;
            const PulseStatus status = icons::GlobalRegistry().UnregisterDynamic(self->package_id, id);
            if (status == PULSE_OK) self->owned_icons.erase(it);
            return status;
        } catch (...) {
            return PULSE_ERROR_UNAVAILABLE;
        }
    }

    static PulseStatus PULSE_CALL ResolveBuiltin(void* context,
                                                 PulseStringView name,
                                                 PulseIconId* out_id) noexcept {
        if (out_id) *out_id = 0;
        auto* self = static_cast<State*>(context);
        if (!self || !self->CheckThread()) return PULSE_ERROR_STATE;
        if (!out_id || !name.data || name.length == 0 || name.length > icons::kMaxIconNameBytes) {
            return PULSE_ERROR_ARGUMENT;
        }
        const auto resolved = icons::ResolveBuiltinIconName(std::string_view(name.data, name.length));
        if (!resolved) return PULSE_ERROR_UNAVAILABLE;
        *out_id = *resolved;
        return PULSE_OK;
    }
};

IconsAdapter::IconsAdapter(std::string package_id, std::function<bool()> live)
    : state_(std::make_shared<State>()) {
    state_->package_id = std::move(package_id);
    state_->live = std::move(live);
}

IconsAdapter::~IconsAdapter() {
    Close();
}

PulseStatus IconsAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    if (version != PULSE_ICONS_VERSION_1) return PULSE_ERROR_ABI;
    if (!state_ || !state_->CheckThread()) return PULSE_ERROR_STATE;
    if (!out || size < sizeof(PulseIconsV1)) return PULSE_ERROR_ARGUMENT;
    const PulseIconsV1 table{
        sizeof(PulseIconsV1),
        PULSE_ICONS_VERSION_1,
        state_.get(),
        State::RegisterIcon,
        State::UpdateIcon,
        State::UnregisterIcon,
        State::ResolveBuiltin,
    };
    std::memcpy(out, &table, sizeof(table));
    return PULSE_OK;
}

void IconsAdapter::Close() noexcept {
    if (!state_ || state_->closed) return;
    state_->closed = true;
    try {
        icons::GlobalRegistry().UnregisterDynamicBatch(state_->package_id, state_->owned_icons);
        state_->owned_icons.clear();
    } catch (...) {}
}

} // namespace pulse::extensions::native

#include "diagnostics_adapter.h"
#include <windows.h>
#include <cstring>

namespace pulse::extensions::native {
namespace {

PulseStringView View(const std::string& text) noexcept {
    return {text.empty() ? "" : text.data(), static_cast<uint32_t>(text.size())};
}

} // namespace

struct DiagnosticsAdapter::State : std::enable_shared_from_this<State> {
    DWORD thread = GetCurrentThreadId();
    std::shared_ptr<DiagnosticsSink> sink;
    std::function<bool()> alive;
    bool closed = false;
    int callbacks = 0;

    State(std::shared_ptr<DiagnosticsSink> s, std::function<bool()> live)
        : sink(s ? std::move(s) : std::make_shared<DiagnosticsSink>()), alive(std::move(live)) {}

    bool Valid() const noexcept {
        return GetCurrentThreadId() == thread && !closed && (!alive || alive());
    }

    static PulseStatus PULSE_CALL GetLastError(void* context, PulseDiagnosticFn callback, void* user) noexcept {
        if (!context || !callback) return PULSE_ERROR_ARGUMENT;
        try {
            auto self = static_cast<State*>(context)->shared_from_this();
            if (!self->Valid()) return PULSE_ERROR_STATE;
            const auto snap = self->sink->Read();
            PulseDiagnosticInfo info{};
            info.size = sizeof(info);
            info.status = snap.status;
            info.sequence = snap.sequence;
            info.interface_name = View(snap.interface_name);
            info.operation = View(snap.operation);
            info.subject = View(snap.subject);
            info.field = View(snap.field);
            info.message = View(snap.message);
            ++self->callbacks;
            try { callback(user, &info); } catch (...) {}
            --self->callbacks;
            return PULSE_OK;
        } catch (...) {
            return PULSE_ERROR_STATE;
        }
    }

    static PulseStatus PULSE_CALL ClearLastError(void* context) noexcept {
        if (!context) return PULSE_ERROR_ARGUMENT;
        try {
            auto self = static_cast<State*>(context)->shared_from_this();
            if (!self->Valid()) return PULSE_ERROR_STATE;
            self->sink->Clear();
            return PULSE_OK;
        } catch (...) {
            return PULSE_ERROR_STATE;
        }
    }
};

DiagnosticsAdapter::DiagnosticsAdapter(std::shared_ptr<DiagnosticsSink> sink, std::function<bool()> alive)
    : sink_(sink ? std::move(sink) : std::make_shared<DiagnosticsSink>()),
      state_(std::make_shared<State>(sink_, std::move(alive))) {}

DiagnosticsAdapter::~DiagnosticsAdapter() { Close(); }

PulseStatus DiagnosticsAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    try {
        if (!state_->Valid()) return PULSE_ERROR_STATE;
        if (version != PULSE_DIAGNOSTICS_VERSION_1) return PULSE_ERROR_ABI;
        if (!out || size < sizeof(PulseDiagnosticsV1)) return PULSE_ERROR_ARGUMENT;
        const PulseDiagnosticsV1 table{
            sizeof(PulseDiagnosticsV1),
            PULSE_DIAGNOSTICS_VERSION_1,
            state_.get(),
            State::GetLastError,
            State::ClearLastError,
        };
        std::memcpy(out, &table, sizeof(table));
        return PULSE_OK;
    } catch (...) {
        return PULSE_ERROR_STATE;
    }
}

void DiagnosticsAdapter::Close() noexcept {
    if (GetCurrentThreadId() != state_->thread || state_->closed) return;
    state_->closed = true;
}

bool DiagnosticsAdapter::CallbackActive() const noexcept {
    return state_->callbacks > 0;
}

} // namespace pulse::extensions::native

#include "extcapsules_adapter.h"
#include "diagnostics_adapter.h"

#include <atomic>
#include <cstring>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pulse::extensions::native {
namespace {

std::string_view View(PulseStringView view) {
    return view.data && view.length ? std::string_view(view.data, view.length) : std::string_view{};
}

bool NormalizeExtension(PulseStringView raw, std::wstring& out) {
    out.clear();
    std::string_view sv = View(raw);
    if (!sv.empty() && sv.front() == '.') sv.remove_prefix(1);
    if (sv.empty() || sv.size() > 24) return false;
    out.reserve(sv.size());
    for (const char ch : sv) {
        if (ch >= 'A' && ch <= 'Z') out.push_back(static_cast<wchar_t>(ch - 'A' + 'a'));
        else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') out.push_back(static_cast<wchar_t>(ch));
        else return false;
    }
    return true;
}

bool ValidateStyle(const PulseExtCapsuleStyle* raw, ui::ext_capsule::Style& out, std::string_view& bad_field) {
    out = {};
    if (!raw) return true;
    if (raw->size < sizeof(PulseExtCapsuleStyle)) {
        bad_field = "style.size";
        return false;
    }
    if (raw->corner_radius_milli > 12000u) { bad_field = "corner_radius_milli"; return false; }
    if (raw->padding_x_milli != 0 && (raw->padding_x_milli < 1000u || raw->padding_x_milli > 16000u)) {
        bad_field = "padding_x_milli"; return false;
    }
    if (raw->padding_y_milli > 8000u) { bad_field = "padding_y_milli"; return false; }
    if (raw->border_width_milli > 3000u) { bad_field = "border_width_milli"; return false; }
    if (raw->gap_milli > 16000u) { bad_field = "gap_milli"; return false; }
    if (raw->font_size_delta_milli < -4000 || raw->font_size_delta_milli > 2000) {
        bad_field = "font_size_delta_milli"; return false;
    }
    if (raw->font_weight != 0 && (raw->font_weight < 100u || raw->font_weight > 900u)) {
        bad_field = "font_weight"; return false;
    }
    if (raw->corner_radius_milli) out.corner_radius_dip = static_cast<float>(raw->corner_radius_milli) / 1000.0f;
    if (raw->padding_x_milli) out.padding_x_dip = static_cast<float>(raw->padding_x_milli) / 1000.0f;
    if (raw->padding_y_milli) out.padding_y_dip = static_cast<float>(raw->padding_y_milli) / 1000.0f;
    if (raw->border_width_milli) out.border_width_dip = static_cast<float>(raw->border_width_milli) / 1000.0f;
    if (raw->gap_milli) out.gap_dip = static_cast<float>(raw->gap_milli) / 1000.0f;
    if (raw->font_size_delta_milli != 0) out.font_size_delta_dip = static_cast<float>(raw->font_size_delta_milli) / 1000.0f;
    if (raw->font_weight) out.font_weight = raw->font_weight;
    return true;
}

}  // namespace

struct ExtCapsulesAdapter::State : std::enable_shared_from_this<State> {
    DWORD thread = GetCurrentThreadId();
    std::string package_id;
    std::function<bool()> alive;
    std::shared_ptr<DiagnosticsSink> diagnostics;
    extcapsules::Registry* registry = nullptr;
    uint64_t owner_key = 0;
    bool closed = false;

    State(std::string pkg, std::function<bool()> live, std::shared_ptr<DiagnosticsSink> sink,
          extcapsules::Registry* reg)
        : package_id(std::move(pkg)),
          alive(std::move(live)),
          diagnostics(std::move(sink)),
          registry(reg ? reg : &extcapsules::Registry::Instance()) {
        static std::atomic<uint64_t> next_owner{1};
        owner_key = next_owner.fetch_add(1);
    }

    bool Valid() const noexcept { return GetCurrentThreadId() == thread && !closed && (!alive || alive()); }

    PulseStatus Fail(PulseStatus status, std::string_view op, std::string_view subject,
                     std::string_view field, std::string_view msg) const {
        if (diagnostics) diagnostics->Record(status, PULSE_EXTCAPSULES_INTERFACE_NAME, op, subject, field, msg);
        return status;
    }

    template <class F>
    static PulseStatus Guard(void* context, F action) noexcept {
        if (!context) return PULSE_ERROR_ARGUMENT;
        try {
            auto self = static_cast<State*>(context)->shared_from_this();
            if (GetCurrentThreadId() != self->thread)
                return self->Fail(PULSE_ERROR_STATE, "", "", "thread",
                                  "pulse.extcapsules must be called on the owner UI thread");
            if (!self->Valid()) return PULSE_ERROR_STATE;
            return action(*self);
        } catch (...) {
            return PULSE_ERROR_STATE;
        }
    }

    static PulseStatus PULSE_CALL SetRules(void* context,
                                           const PulseExtCapsuleStyle* raw_style,
                                           const PulseExtCapsuleRule* rules,
                                           uint32_t rule_count) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (rule_count > kMaxRulesPerPackage) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "set_rules", "", "rule_count",
                                 "at most 256 extension capsule rules per package");
            }
            if (rule_count > 0 && !rules) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "set_rules", "", "rules",
                                 "rules pointer is required when rule_count > 0");
            }
            ui::ext_capsule::Style style{};
            std::string_view bad_field;
            if (!ValidateStyle(raw_style, style, bad_field)) {
                const PulseStatus st = bad_field == "style.size" ? PULSE_ERROR_ABI : PULSE_ERROR_ARGUMENT;
                return self.Fail(st, "set_rules", "", bad_field, "invalid extension capsule style");
            }
            std::vector<extcapsules::Entry> entries;
            entries.reserve(rule_count);
            std::unordered_set<std::wstring> seen;
            seen.reserve(rule_count);
            constexpr uint32_t kKnownFlags = PULSE_EXT_CAPSULE_FLAG_UPPERCASE | PULSE_EXT_CAPSULE_FLAG_STRIP_DOT;
            for (uint32_t i = 0; i < rule_count; ++i) {
                const PulseExtCapsuleRule& r = rules[i];
                if (r.size < sizeof(PulseExtCapsuleRule)) {
                    return self.Fail(PULSE_ERROR_ABI, "set_rules", "", "rule.size",
                                     "rule.size is smaller than PulseExtCapsuleRule");
                }
                if ((r.flags & ~kKnownFlags) != 0) {
                    return self.Fail(PULSE_ERROR_ARGUMENT, "set_rules", "", "rule.flags",
                                     "unknown PULSE_EXT_CAPSULE_FLAG_* bits");
                }
                std::wstring ext;
                if (!NormalizeExtension(r.extension, ext)) {
                    return self.Fail(PULSE_ERROR_ARGUMENT, "set_rules", "", "rule.extension",
                                     "extension must be 1..24 ASCII [A-Za-z0-9_-] characters");
                }
                if ((r.light_bg == 0 && r.dark_bg == 0) || (r.light_text == 0 && r.dark_text == 0)) {
                    return self.Fail(PULSE_ERROR_ARGUMENT, "set_rules", "", "rule.color",
                                     "capsule background and text colors are required");
                }
                if (!seen.insert(ext).second) continue;  // first rule in array wins
                ui::ext_capsule::Rule compiled;
                compiled.light_bg = r.light_bg ? r.light_bg : r.dark_bg;
                compiled.light_text = r.light_text ? r.light_text : r.dark_text;
                compiled.light_border = r.light_border;
                compiled.dark_bg = r.dark_bg ? r.dark_bg : r.light_bg;
                compiled.dark_text = r.dark_text ? r.dark_text : r.light_text;
                compiled.dark_border = r.dark_border;
                compiled.uppercase = (r.flags & PULSE_EXT_CAPSULE_FLAG_UPPERCASE) != 0;
                compiled.strip_dot = (r.flags & PULSE_EXT_CAPSULE_FLAG_STRIP_DOT) != 0;
                compiled.style = style;
                entries.push_back(extcapsules::Entry{std::move(ext), compiled});
            }
            self.registry->SetRules(self.owner_key, self.package_id, std::move(entries));
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL ClearRules(void* context) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            self.registry->RemoveOwner(self.owner_key);
            return PULSE_OK;
        });
    }
};

ExtCapsulesAdapter::ExtCapsulesAdapter(std::string package_id, std::function<bool()> alive,
                                       std::shared_ptr<DiagnosticsSink> diagnostics, extcapsules::Registry* registry)
    : state_(std::make_shared<State>(std::move(package_id), std::move(alive), std::move(diagnostics), registry)) {}

ExtCapsulesAdapter::~ExtCapsulesAdapter() { Close(); }

PulseStatus ExtCapsulesAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    try {
        if (!state_->Valid()) return PULSE_ERROR_STATE;
        if (!out || size < sizeof(PulseExtCapsulesV1)) return PULSE_ERROR_ARGUMENT;
        if (version != PULSE_EXTCAPSULES_VERSION_1) return PULSE_ERROR_UNAVAILABLE;
        PulseExtCapsulesV1 table{};
        table.size = sizeof(table);
        table.version = PULSE_EXTCAPSULES_VERSION_1;
        table.context = state_.get();
        table.set_rules = &State::SetRules;
        table.clear_rules = &State::ClearRules;
        std::memcpy(out, &table, sizeof(table));
        return PULSE_OK;
    } catch (...) {
        return PULSE_ERROR_STATE;
    }
}

void ExtCapsulesAdapter::Close() noexcept {
    try {
        if (!state_ || state_->closed) return;
        state_->closed = true;
        if (state_->registry) state_->registry->RemoveOwner(state_->owner_key);
    } catch (...) {}
}

}  // namespace pulse::extensions::native

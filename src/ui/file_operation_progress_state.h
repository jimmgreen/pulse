#pragma once
#include "../ops/ops_manager.h"
#include "../common/localization.h"

namespace pulse::ui {
inline bool TransferHasAdvisoryEta(const ops::OpStatus& status) {
    return !status.external && status.active && status.task_id != 0 &&
        status.eta_detail.task_id == status.task_id && status.eta_detail.stage != ops::EtaStage::Legacy &&
        (status.type == ops::OpType::RealDelete || status.type == ops::OpType::RecycleDelete);
}
inline ops::EtaStage TransferEtaStage(const ops::OpStatus& status, uint64_t now) {
    if (!TransferHasAdvisoryEta(status)) return ops::EtaStage::Legacy;
    const auto stage = status.eta_detail.stage;
    if ((stage == ops::EtaStage::Learning || stage == ops::EtaStage::Range) &&
        (now < status.eta_detail.sample_tick || now - status.eta_detail.sample_tick >= 3000))
        return ops::EtaStage::Unknown;
    return stage;
}
template<class Duration>
std::optional<std::wstring> TransferAdvisoryEtaText(const ops::OpStatus& status, uint64_t now, Duration&& duration) {
    switch (TransferEtaStage(status, now)) {
    case ops::EtaStage::Legacy: return std::nullopt;
    case ops::EtaStage::Learning: return l10n::Get(l10n::StringId::OpEstimating);
    case ops::EtaStage::Finishing: return l10n::Get(l10n::StringId::OpEtaFinishing);
    case ops::EtaStage::Range:
        if (status.eta_detail.lower_seconds > 0 && status.eta_detail.upper_seconds >= status.eta_detail.lower_seconds) {
            const uint64_t low = status.eta_detail.lower_seconds, high = status.eta_detail.upper_seconds;
            if (low == high) return l10n::Get(l10n::StringId::OpApprox) + duration(low);
            const uint64_t unit = low >= 3600 ? 3600 : low >= 60 ? 60 : 1;
            const auto suffix = unit == 3600 ? l10n::StringId::OpHours : unit == 60 ? l10n::StringId::OpMinutes : l10n::StringId::OpSeconds;
            return l10n::Get(l10n::StringId::OpApprox) + std::to_wstring(low / unit) + L"\x2013" +
                std::to_wstring(high / unit + (high % unit != 0)) + l10n::Get(suffix);
        }
        [[fallthrough]];
    case ops::EtaStage::Unknown: return l10n::Get(l10n::StringId::OpEtaUnknownTotal);
    }
    return std::nullopt;
}
inline bool TransferHasChildWork(const ops::OpStatus& status) {
    return TransferHasAdvisoryEta(status) && status.eta_detail.has_processed_items;
}
inline bool TransferTerminal(const ops::OpStatus& status) {
    return !status.active && (status.phase == ops::OpPhase::Completed || status.phase == ops::OpPhase::Failed);
}
inline bool TransferIndeterminate(const ops::OpStatus& status) {
    return !TransferTerminal(status) && (status.phase == ops::OpPhase::Scanning ||
        !status.enumeration_complete || status.percent < 0.0f);
}
inline bool TransferShowsEmptyingSubtitle(const ops::OpStatus& status) {
    return status.type == ops::OpType::EmptyRecycle && !TransferTerminal(status);
}
inline bool TransferNearDoneSuppressesAutoShow(const ops::OpStatus& status) {
    // A selected folder can reach its top-level count while its children are still being deleted.
    return status.type != ops::OpType::EmptyRecycle &&
        status.type != ops::OpType::RealDelete && status.type != ops::OpType::RecycleDelete &&
        (status.percent >= 90.0f || (status.external && status.percent >= 84.0f));
}
inline bool TransferCanAutoClose(const ops::OpStatus& status) {
    return (!status.external || status.external_outcome == ops::ExternalOutcome::Completed) &&
        !status.active && status.phase == ops::OpPhase::Completed &&
        status.failed_items == 0 && status.last_error.empty();
}
}

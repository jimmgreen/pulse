#pragma once
#include "extensions/store/install_service.h"
#include "extensions/store/installed_store.h"
#include "extensions/ui_tree.h"
namespace pulse::app {
struct StoreReviewDecision {
    enum class Kind { Ignore, Decline, Trust, MissingConsent, Install };
    Kind kind = Kind::Ignore;
    extensions::store::InstallConsent consent;
};
// UI response only; the worker still rechecks its private review and disk state.
inline StoreReviewDecision EvaluateStoreReview(const extensions::store::InstallServiceSnapshot& current,
    uint64_t generation, bool trust, int button, const std::vector<extensions::UiStateEntry>& states) {
    using Phase = extensions::store::InstallServicePhase;
    using Kind = StoreReviewDecision::Kind;
    if (current.generation != generation || current.phase != (trust ? Phase::AwaitingPublisherTrust : Phase::AwaitingInstall)) return {};
    if (button != 0) return {Kind::Decline, {}};
    if (trust) return {Kind::Trust, {}};
    if (!current.install_review) return {};
    const auto checked = [&](const char* key) {
        for (const auto& state : states) if (state.key == key) return state.value == "true";
        return false;
    };
    const auto& review = *current.install_review;
    if ((review.native_code && !checked("native")) || (!review.added_resources.empty() && !checked("resources"))) return {Kind::MissingConsent, {}};
    return {Kind::Install, {true, !review.native_code || checked("native"), review.added_resources.empty() || checked("resources"), checked("enable")}};
}
struct StoreChangeDecision {
    enum class Kind { Ignore, Decline, MissingConsent, Approve };
    Kind kind = Kind::Ignore;
    extensions::store::StoreChangeConsent consent;
};
inline StoreChangeDecision EvaluateStoreChangeReview(const extensions::store::InstalledStoreServiceSnapshot& current,
    uint64_t generation, int button, const std::vector<extensions::UiStateEntry>& states) {
    using Kind = StoreChangeDecision::Kind;
    if (current.generation != generation || current.phase != extensions::store::InstalledStorePhase::AwaitingConfirmation) return {};
    if (button != 0) return {Kind::Decline, {}};
    if (!current.review) return {};
    const auto checked = [&](const char* key) {
        for (const auto& state : states) if (state.key == key) return state.value == "true";
        return false;
    };
    const auto& review = *current.review;
    if ((review.native_code && !checked("native")) || (!review.added_resources.empty() && !checked("resources"))) return {Kind::MissingConsent, {}};
    return {Kind::Approve, {true, review.native_code && checked("native"), !review.added_resources.empty() && checked("resources")}};
}
}

#pragma once
#include "places.h"
#include <functional>
namespace pulse::app {
enum class TagAdsFailureStage { JournalRead, JournalWrite, StreamWrite, Pending };
struct TagAdsFailure {
    std::wstring path;
    TagAdsFailureStage stage = TagAdsFailureStage::JournalRead;
    DWORD error = ERROR_GEN_FAILURE;
    bool unsupported = false;
};
std::wstring TagAdsFailureMessage(const TagAdsFailure& failure);
struct TagAdsResult {
    uint64_t revision = 0;
    std::vector<TagAdsFailure> failures;
};
struct TagAdsNoticeState {
    std::wstring title, message;
};
inline bool AcceptTagAdsResult(uint64_t& current, uint64_t incoming) {
    if (incoming <= current) return false;
    current = incoming;
    return true;
}
void ApplyTagAdsNotice(TagAdsNoticeState& owned, const std::vector<TagAdsFailure>& failures,
                       std::wstring& title, std::wstring& message, bool show_new,
                       const std::wstring& current_path = {});
// Called on the serial ADS lane; persists intent before changing any stream.
std::vector<std::wstring> SyncTagAdsUpdates(const std::vector<TagAdsUpdate>& updates,
                                         std::vector<TagAdsFailure>* diagnostics = nullptr);
bool HasPendingTagAds(const std::wstring& path);
// Each continuation is a separate serial job. A rejected enqueue (shutdown)
// leaves the persisted intent for the next startup.
void QueueTagAdsDrain(std::vector<TagAdsUpdate> updates,
    std::function<void(std::function<void()>)> enqueue,
    std::function<void(std::vector<TagAdsFailure>)> publish);
}

#pragma once
#include "../../../../sdk/native/include/pulse/staging.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <utility>
namespace pulse::extensions::native {
struct StageSubmission {
    uint64_t id = 0;
    std::wstring destination;
    std::vector<std::wstring> sources;
    HANDLE cancel_event = nullptr;
};
struct StageSubmitResult { PulseStatus status = PULSE_ERROR_STATE; uint64_t task = 0; };
struct StageOperation {
    PulseStageState state = PULSE_STAGE_RUNNING;
    uint32_t percent = UINT32_MAX;
    uint64_t completed_items = 0, total_items = 0, completed_bytes = 0, total_bytes = 0;
    std::string error;
};
struct StagingBackend {
    std::function<std::pair<PulseStatus, std::wstring>(const std::wstring&)> choose_destination;
    std::function<StageSubmitResult(const StageSubmission&)> submit;
    std::function<StageOperation(uint64_t)> query;
    std::function<void(uint64_t)> cancel;
};
class StagingAdapter {
public:
    StagingAdapter(HWND owner, std::function<bool()> live, StagingBackend backend);
    ~StagingAdapter();
    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    void Poll() noexcept;
    void Close() noexcept;
    void CleanupRemaining() noexcept;
    bool CallbackActive() const noexcept;
private:
    struct State;
    std::shared_ptr<State> state_;
};
}

#pragma once
#include <atomic>
#include <memory>
#include <functional>
#include <string>
#include <vector>
#include "../ui/file_operation_dialog.h"
namespace pulse {
struct DeletePreflight {
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    // Optional read-only probe gate for deterministic cancellation tests.
    std::function<void()> before_probe;
    bool failed = false;
    bool permanent = false;
    bool offer_recycle = false;
    std::vector<std::wstring> paths;
    std::vector<std::wstring> direct;
    ui::PermanentDeleteInfo::NoBinReason reason = ui::PermanentDeleteInfo::NoBinReason::TooLarge;
};
inline bool CanSubmitDeletePreflight(const std::shared_ptr<DeletePreflight>& current,
                                    const std::shared_ptr<DeletePreflight>& request) {
    return request && current == request && request->done.load()
        && !request->cancel.load() && !request->failed;
}
void StartDeletePreflight(const std::shared_ptr<DeletePreflight>& request);
}

#pragma once
#include "runtime.h"
#include <functional>
#include <windows.h>
namespace pulse {
struct AppState;
}
namespace pulse::extensions {
using RenameReviewSnapshot = std::function<bool(const std::wstring &)>;
using RenameReviewProbe = std::function<void(HWND, RenameReviewSnapshot)>;
// Host-only hook, ignored unless both isolatedTest and the capture environment are set.
void SetRenameReviewProbe(RenameReviewProbe probe);
class RenameService {
  public:
    using Completion = std::function<void(json::Value)>;
    using Cancel = std::function<void()>;
    static Cancel Prepare(AppState &owner, const CallerLease &caller, const json::Value &items, Completion complete);
    static Cancel Commit(AppState &owner, const CallerLease &caller, std::string plan_id, Completion complete);
    static std::optional<json::Value> Task(AppState &owner, const CallerLease &caller, uint64_t task, bool cancel);
    static void ReleaseOwner(uintptr_t owner);
};
} // namespace pulse::extensions

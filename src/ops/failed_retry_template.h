#pragma once
#include "ops_manager.h"

namespace pulse::ops {
inline OpRequest MakeFailedRetryTemplate(const OpRequest& request) {
    OpRequest result = request;
    // RetryFailedItems reconstructs sources from the retained failures and
    // never reuses lock owners from the original request. Keep every other
    // option, but do not retain successful paths or their vector storage.
    std::vector<std::wstring>().swap(result.sources);
    std::vector<LockOwner>().swap(result.close_first);
    return result;
}
} // namespace pulse::ops

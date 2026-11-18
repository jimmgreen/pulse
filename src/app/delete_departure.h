#pragma once
#include "../ops/ops_manager.h"

namespace pulse::app {
inline bool RecordConfirmedDelete(const ops::CompletedOperation& completed,
    std::vector<std::wstring>& paths, uint64_t& since, uint64_t& done_at, uint64_t& task, uint64_t now) {
    if (completed.refresh_only || (completed.type != ops::OpType::RecycleDelete &&
        completed.type != ops::OpType::RealDelete) || !completed.task_id ||
        completed.sources.empty() || completed.sources.size() > 64) return false;
    if (paths.empty() || done_at || now - since > 8000) { paths.clear(); since = now; }
    paths.insert(paths.end(), completed.sources.begin(), completed.sources.end());
    done_at = now;
    task = completed.task_id;
    return true;
}
} // namespace pulse::app

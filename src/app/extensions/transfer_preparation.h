#pragma once
#include "transfer_snapshot.h"

namespace pulse::extensions::transfer_preparation {

// Captured on the UI thread from a specific owner and selection. This value
// contains no AppState pointers and must not be updated while preparation runs.
struct Request {
    transfer_plan::Operation operation = transfer_plan::Operation::Copy;
    std::vector<std::wstring> sources;
    std::vector<uint64_t> source_sizes;
    std::vector<uint64_t> source_modified;
    std::wstring target;
    bool directory_tree = false;
    std::wstring tree_source;
    uint64_t tree_source_modified = 0;
};
struct Result {
    transfer_plan::Error error = transfer_plan::Error::None;
    DWORD filesystem_error = ERROR_SUCCESS;
    std::optional<std::string> plan_id;
    explicit operator bool() const noexcept { return plan_id.has_value(); }
};

// Worker-only. Captures real objects, then publishes an unconfirmed, immutable
// plan. No filesystem mutation, confirmation, receipt or execution occurs here.
Result Prepare(const Request& request, const authorization::PrincipalBinding& principal,
    authorization::Registry& registry, transfer_plan::Store& store,
    const std::atomic<bool>& cancelled);

}

#pragma once
#include "../../common/json_value.h"
#include <cstdint>
#include <optional>
#include <string>

namespace pulse::extensions {
enum class ResourceOperation { Context, List, Stat, TaskGet, TaskCancel, SettingsGet, SettingsSet, Audit, RenamePrepare, RenameCommit };
struct ResourceRequest {
    ResourceOperation operation = ResourceOperation::Context;
    std::string name, cursor, entry, key;
    std::optional<std::string> value;
    uint64_t task = 0, version = 0;
    size_t limit = 64;
    json::Value rename_items;
    std::string plan_id;
};
std::optional<ResourceRequest> ParseResourceRequest(const json::Value& input);
json::Value ResourceSuccess(json::Value data);
json::Value ResourceFailure(std::string_view code);
}

#pragma once
#include "../../common/json_value.h"
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
namespace pulse::extensions {
// Worker-only. The host supplies its trusted data directory and package identity.
json::Value ReadOwnSettings(const std::wstring &root_directory, std::string_view package_id);
json::Value WriteOwnSetting(const std::wstring &root_directory, std::string_view package_id,
                            std::string_view key, const std::optional<std::string> &value,
                            uint64_t expected_version, const std::function<bool()> &authorized);
} // namespace pulse::extensions

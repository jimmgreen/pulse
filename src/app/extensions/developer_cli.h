#pragma once
#include "../../common/json_value.h"
#include <optional>

namespace pulse::extensions {
struct DeveloperCliResult {
    json::Value output;
    int exit_code = 0;
};
// nullopt means no developer flag was present. Evaluation never changes runtime state.
std::optional<DeveloperCliResult> EvaluateDeveloperCli(int argc, wchar_t** argv);
// Writes one UTF-8 JSON object to the inherited stdout handle; output failure is exit 3.
std::optional<int> HandleDeveloperCli(int argc, wchar_t** argv);
}

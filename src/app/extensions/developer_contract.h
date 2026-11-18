#pragma once
#include "../../common/json_value.h"

namespace pulse::extensions {

// Public declarative extension surface, independent of the internal action registry.
json::Value DescribeExtensionApi();
// Pure validation: args must contain exactly one string member named manifest.
json::Value ValidateExtensionManifest(const json::Value& args);

}

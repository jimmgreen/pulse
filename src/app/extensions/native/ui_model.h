#pragma once
#include "../../../../sdk/native/include/pulse/ui_model.h"
#include "../ui_tree.h"
#include <set>
#include <string>
namespace pulse::extensions::native {
// Transactional: output and commands are unchanged on rejection.
PulseStatus ConvertUiModel(const PulseUiModelTree *input, UiTree &output, std::set<std::string> &commands,
                           std::string &error);
} // namespace pulse::extensions::native

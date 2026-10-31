#pragma once
#include <string>
#include <vector>
#include <cstdint>
namespace pulse::shell_bridge {
struct FileDialogPropertyView {
    uint32_t index = 0, kind = 0;
    std::wstring label;
    std::vector<std::wstring> values;
};
}

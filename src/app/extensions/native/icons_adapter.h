#pragma once

#include "../../../../sdk/native/include/pulse/icons.h"

#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pulse::extensions::native {

class IconsAdapter {
public:
    IconsAdapter(std::string package_id, std::function<bool()> live);
    ~IconsAdapter();

    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    void Close() noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace pulse::extensions::native

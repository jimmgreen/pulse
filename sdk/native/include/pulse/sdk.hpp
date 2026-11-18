#pragma once
#include "abi.h"
#include <limits>
#include <string_view>
namespace pulse::sdk {
inline PulseStringView View(std::string_view text) noexcept {
    if (text.size() > (std::numeric_limits<uint32_t>::max)()) return {nullptr, 0};
    return {text.data(), static_cast<uint32_t>(text.size())};
}
class Host {
public:
    explicit Host(const PulseHostV1& value) noexcept : value_(value) {}
    template<class Table> PulseStatus Query(std::string_view name, uint32_t version, Table& out) const noexcept {
        if (!value_.query_interface || name.size() > (std::numeric_limits<uint32_t>::max)()) return PULSE_ERROR_ARGUMENT;
        out = {};
        return value_.query_interface(value_.context, View(name), version, &out, sizeof(Table));
    }
private:
    PulseHostV1 value_;
};
}

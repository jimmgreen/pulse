#pragma once

#include "../../../../sdk/native/include/pulse/extcapsules.h"
#include "../extcapsule_registry.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <string>

namespace pulse::extensions::native {

class DiagnosticsSink;

// pulse.extcapsules for one plugin session: validates extension capsule rules,
// owns them in the shared extcapsules::Registry, and removes them on Close.
class ExtCapsulesAdapter {
public:
    ExtCapsulesAdapter(std::string package_id,
                       std::function<bool()> alive,
                       std::shared_ptr<DiagnosticsSink> diagnostics = nullptr,
                       extcapsules::Registry* registry = &extcapsules::Registry::Instance());
    ~ExtCapsulesAdapter();
    ExtCapsulesAdapter(const ExtCapsulesAdapter&) = delete;
    ExtCapsulesAdapter& operator=(const ExtCapsulesAdapter&) = delete;

    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    void Close() noexcept;
    bool CallbackActive() const noexcept { return false; }

    static constexpr size_t kMaxRulesPerPackage = 256;

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace pulse::extensions::native

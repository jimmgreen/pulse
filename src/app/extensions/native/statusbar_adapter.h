#pragma once

#include "../../../../sdk/native/include/pulse/statusbar.h"
#include "../statusbar_registry.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <string>

namespace pulse::extensions::native {

class DiagnosticsSink;

// pulse.statusbar for one plugin session: validates items, owns them in the shared
// statusbar::Registry, and removes them on Close.
class StatusBarAdapter {
public:
    StatusBarAdapter(std::string package_id,
                     std::function<bool()> alive,
                     std::shared_ptr<DiagnosticsSink> diagnostics = nullptr,
                     statusbar::Registry* registry = &statusbar::Registry::Instance());
    ~StatusBarAdapter();
    StatusBarAdapter(const StatusBarAdapter&) = delete;
    StatusBarAdapter& operator=(const StatusBarAdapter&) = delete;

    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    void Close() noexcept;
    bool CallbackActive() const noexcept;

    static constexpr size_t kMaxItemsPerPackage = 2;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace pulse::extensions::native

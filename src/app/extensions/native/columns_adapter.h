#pragma once

#include "../../../../sdk/native/include/pulse/columns.h"
#include "../column_registry.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <string>

namespace pulse::extensions::native {

class DiagnosticsSink;

// pulse.columns for one plugin session: validates descriptors, owns the
// session's providers in the shared columns::Registry, and drops them on Close.
class ColumnsAdapter {
public:
    ColumnsAdapter(std::string package_id,
                   std::function<bool()> alive,
                   std::shared_ptr<DiagnosticsSink> diagnostics = nullptr,
                   std::function<void()> wake = nullptr,
                   columns::Registry* registry = &columns::Registry::Instance());
    ~ColumnsAdapter();
    ColumnsAdapter(const ColumnsAdapter&) = delete;
    ColumnsAdapter& operator=(const ColumnsAdapter&) = delete;

    void SetDiagnostics(std::shared_ptr<DiagnosticsSink> sink);
    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    void Close() noexcept;
    bool CallbackActive() const noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace pulse::extensions::native

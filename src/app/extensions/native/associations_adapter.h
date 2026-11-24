#pragma once

#include "../../../../sdk/native/include/pulse/associations.h"
#include "../associations_registry.h"
#include "../manifest.h"
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pulse::extensions::native {

class DiagnosticsSink;

struct AssociationsQueryReply {
    PulseStatus status = PULSE_OK;
    std::vector<AssociationQueryItem> items;
};

struct AssociationsBackend {
    std::function<bool()> valid_owner;
    std::function<PulseStatus(uint64_t request_id,
                              std::vector<std::pair<std::string, std::string>> handler_extensions,
                              std::function<void(AssociationsQueryReply)> complete)> query;
    std::function<PulseStatus(uint64_t request_id,
                              std::vector<AssociationEntryRequest> entries,
                              std::function<void(PulseStatus)> complete)> apply;
    std::function<PulseStatus(uint64_t request_id,
                              std::vector<std::string> extensions,
                              std::function<void(PulseStatus)> complete)> remove;
    std::function<void(uint64_t request_id)> cancel;
    std::function<PulseStatus(std::string extension)> open_chooser;
    std::function<PulseStatus()> open_default_apps;
};

class AssociationsAdapter {
public:
    AssociationsAdapter(std::string package_id,
                        std::vector<ManifestFileHandlerContribution> declared_handlers,
                        AssociationsBackend backend,
                        std::function<bool()> alive,
                        std::shared_ptr<DiagnosticsSink> diagnostics = nullptr,
                        std::function<void()> wake = nullptr);
    ~AssociationsAdapter();
    AssociationsAdapter(const AssociationsAdapter&) = delete;
    AssociationsAdapter& operator=(const AssociationsAdapter&) = delete;

    void SetDiagnostics(std::shared_ptr<DiagnosticsSink> sink);
    void SetWake(std::function<void()> wake);
    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    void Poll() noexcept;
    bool HasPendingWork() const noexcept;
    void Close() noexcept;
    bool CallbackActive() const noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace pulse::extensions::native

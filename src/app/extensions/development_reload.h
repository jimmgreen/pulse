#pragma once
#include "manifest.h"
#include "../../common/json_value.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <functional>

namespace pulse::extensions {
struct DevelopmentUpdate {
    uint64_t generation = 0;
    uint64_t revision = 0;
    std::optional<Manifest> manifest;
    std::string content_revision;
    bool identity_failed = false;
};
// All file reads, parsing and diagnostic writes run on this owned worker.
class DevelopmentReload {
public:
    DevelopmentReload();
    ~DevelopmentReload();
    bool Start(uint64_t generation, std::wstring path, std::wstring diagnostic,
        std::string initial_bytes, bool enabled);
    void Stop();
    std::optional<DevelopmentUpdate> TakeUpdate();
    bool Commit(const DevelopmentUpdate& update, bool enabled, const std::function<void()>& swap);
    void SetEnabled(bool enabled);
    json::Value Status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

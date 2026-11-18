#pragma once
#include <pulse/sdk.hpp>
#include <pulse/resources.h>
#include <pulse/settings_pages.h>
#include <pulse/staging.h>
#include <memory>
#include <string>

class ArchiveSettings {
public:
    ArchiveSettings();
    ~ArchiveSettings();
    PulseStatus Initialize(const pulse::sdk::Host&, PulseStagingV1);
    const std::string& Destination() const;
    bool Ready() const;
    bool Zip() const;
    unsigned Level() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};

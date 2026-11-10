// settings.h - the extension's page under Settings › 增强包 › 扩展设置. Every
// change saves at once; the menu commands read these values so common work
// runs in one step without dialogs.
#pragma once
#include <pulse/sdk.hpp>
#include <pulse/resources.h>
#include <pulse/settings_pages.h>
#include <pulse/staging.h>
#include <functional>
#include <memory>
#include <string>

class Settings {
public:
    enum class ExtractMode { Smart = 0, Folder = 1, Here = 2 };
    Settings();
    ~Settings();
    // `changed` runs on the UI thread after values load or change.
    PulseStatus Initialize(const pulse::sdk::Host&, PulseStagingV1, std::function<void()> changed);
    bool Ready() const;
    ExtractMode Mode() const;
    // UTF-8 fixed extraction folder; empty = next to the archive.
    std::string FixedFolder() const;
    int Format() const;   // 7z, zip, tar, tar.gz, tar.xz, tar.bz2
    int Level() const;    // store, fastest, fast, normal, maximum, ultra
    bool RememberPasswords() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};

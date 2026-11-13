// settings.h - the extension's page under Settings › 增强包 › 扩展设置. Every
// change saves at once; the menu commands read these values so common work
// runs in one step without dialogs.
#pragma once
#include <pulse/sdk.hpp>
#include <pulse/client.hpp>
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
    // `changed` runs on the UI thread after values load or change. `dispatch` brings the
    // association work (接管压缩包, a worker thread) back to the UI thread.
    PulseStatus Initialize(const pulse::sdk::Host&, PulseStagingV1, pulse::sdk::DispatchQueue& dispatch, std::function<void()> changed);
    bool Ready() const;
    ExtractMode Mode() const;
    // UTF-8 fixed extraction folder; empty = next to the archive.
    std::string FixedFolder() const;
    int Format() const;   // 7z, zip, tar, tar.gz, tar.xz, tar.bz2
    int Level() const;    // store, fastest, fast, normal, maximum, ultra
    bool RememberPasswords() const;
    // Context menu: the commands as one "7-Zip" submenu, and which ones are shown
    // (bit i = Command i in plugin.cpp order).
    bool MenuSubmenu() const;
    bool CommandShown(int index) const;
    // 接管压缩包: archives open with Pulse from File Explorer (association.h).
    bool TakeOver() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};

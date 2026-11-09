#pragma once

#include <string>

namespace pulse::setup {

// Creates or updates a .lnk. An existing shortcut that already points at
// `target` retains its properties so taskbar/Start pins survive upgrades (#72).
// Ordinary Pulse links acquire --activate and the shared taskbar identity.
// Existing empty-argument links migrate without resetting custom properties.
bool EnsureShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& args,
                    const std::wstring& workdir, const std::wstring& description, std::wstring& error);

}  // namespace pulse::setup

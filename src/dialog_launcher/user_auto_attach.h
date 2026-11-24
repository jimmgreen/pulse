#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>

namespace pulse::dialog_launcher::user_auto {

struct Event {
    const char* stage = "";
    DWORD pid = 0;
    DWORD error = 0;
    DWORD entries = 0;
    FILETIME created{};
    uint32_t match_mask = 0;
    std::wstring app_basename;
};

struct Options {
    std::wstring installed_root;
    std::wstring data_dir;
    HANDLE stop = nullptr;
    HANDLE parent_process = nullptr;
    bool companion = false;
    bool require_dialog_auto_enabled = true;
    DWORD duration_ms = 0;
    std::function<void(const Event&)> log;
};

// Runs the non-elevated same-user adaptive auto-attach loop until stop is
// signaled, dialog_auto is disabled (when required), parent exits, or duration_ms elapses.
DWORD Run(const Options& options) noexcept;

} // namespace pulse::dialog_launcher::user_auto

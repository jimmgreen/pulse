#pragma once
#include <windows.h>
#include <cstddef>
#include <string>
#include <vector>

namespace pulse::extensions::native::crash_guard {

// A native package runs in Pulse's process, so its crash is Pulse's crash.
// This keeps one such crash from repeating: the package that was loading or
// activating, or whose image the fault address fell in, is disabled at the
// next start until a different build of it is installed.
//
// Files live beside the package store (LOCALAPPDATA\Pulse\extensions, or the
// test profile's extensions folder):
//   native-activating.txt  "id\tsha256" while load + activate run
//   native-crashed.txt     written by the crash reporter for a fault inside a plugin image
//   crash-disabled.txt     one "id\tsha256" per line, kept across runs

// UI thread, once, after extensions::PrepareStartup and before any package
// starts. Disables recorded packages and returns the display names of those
// disabled because of the previous run, for a one-time notice.
std::vector<std::wstring> ApplyAtStartup(bool isolated);

// Brackets the plugin code the host calls before a session is live.
class ActivationScope {
public:
    explicit ActivationScope(size_t package);
    ~ActivationScope();
    ActivationScope(const ActivationScope&) = delete;
    ActivationScope& operator=(const ActivationScope&) = delete;
private:
    bool armed_ = false;
};

// After a successful load: remember the image range so a later fault inside
// it is attributed to this package. Forget before FreeLibrary.
// Returns whether it was registered; pair only a true result with ForgetModule.
bool TrackModule(size_t package, HMODULE image);
void ForgetModule(size_t package);

// The user turned the package back on: drop its crash record.
void ForgetPackageCrash(size_t package);

} // namespace pulse::extensions::native::crash_guard

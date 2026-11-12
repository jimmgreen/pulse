#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::extensions {
struct NativeRuntime;
}

namespace pulse::extensions::native::crash_guard {

// A native package runs in Pulse's process, so its crash is Pulse's crash.
// This keeps one such crash from repeating: the package that was loading or
// activating, or whose image (entry DLL or declared private sidecar DLL) the
// fault address fell in, is disabled at the next start until a different build
// of it is installed.
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

namespace detail {
inline thread_local const char* g_active_call_package = nullptr;
} // namespace detail

// Lock-free thread-local query for the extension id currently executing on the
// calling thread, used only as an unconfirmed diagnostic clue when a fault
// falls outside all tracked plugin module images.
inline const char* ActiveCallPackage() noexcept {
    return detail::g_active_call_package;
}

// Brackets an in-process call into plugin code (entry/activate/deactivate/
// destroy or registered callbacks/workers) on the current thread.
class CallScope {
public:
    explicit CallScope(const char* package_id) noexcept
        : previous_(detail::g_active_call_package) {
        if (package_id && *package_id) {
            detail::g_active_call_package = package_id;
            engaged_ = true;
        }
    }
    ~CallScope() noexcept {
        if (engaged_) detail::g_active_call_package = previous_;
    }
    CallScope(const CallScope&) = delete;
    CallScope& operator=(const CallScope&) = delete;
private:
    const char* previous_ = nullptr;
    bool engaged_ = false;
};

// After a successful load: remember the entry image range (and any loaded
// private sidecar DLLs declared in runtime->files under package_dir) so a
// later fault inside any of them is attributed to this package. Forget before
// or right after controlled unload once all sessions for the package close.
// Returns whether it was registered; pair only a true result with ForgetModule.
bool TrackModule(size_t package, HMODULE image);
bool TrackPackageModules(size_t package, HMODULE entry_image,
                         const std::wstring& package_dir,
                         const NativeRuntime* runtime);
void RefreshPackageModules(size_t package, const std::wstring& package_dir,
                           const NativeRuntime* runtime);
bool TrackModuleRange(size_t package, uintptr_t begin, uintptr_t end, bool add_ref = true);
void ForgetModule(size_t package);

// Lock-free address range lookup against currently tracked module slots
// without writing native-crashed.txt.
const char* FindAttributedPackage(const void* address) noexcept;

// The user turned the package back on: drop its crash record.
void ForgetPackageCrash(size_t package);

} // namespace pulse::extensions::native::crash_guard

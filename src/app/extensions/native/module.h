#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "../manifest.h"
#include "../../../../sdk/native/include/pulse/abi.h"
namespace pulse::extensions::native {
// Construct, operate and destroy on the owning UI thread. Trusted in-process
// code: this is neither a sandbox nor protection against malicious callbacks.
class Module final {
public:
    Module() noexcept;
    ~Module();
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    PulseStatus Load(const std::wstring& absolute_path, const PulseHostV1& host);
    PulseStatus LoadVerified(const std::wstring& absolute_path, const std::string& expected_sha256,
        const PulseHostV1& host);
    PulseStatus LoadVerified(const std::wstring& absolute_path, const NativeRuntime& native, const PulseHostV1& host);
    PulseStatus Activate();
    PulseStatus Deactivate();
    PulseStatus Close();
    bool Loaded() const noexcept { return library_ != nullptr; }
    bool Active() const noexcept { return active_; }
    // The loaded image, for crash attribution only; never FreeLibrary it.
    HMODULE Library() const noexcept { return library_; }
private:
    DWORD thread_;
    HMODULE library_ = nullptr;
    HANDLE verified_file_ = INVALID_HANDLE_VALUE;
    std::vector<HANDLE> ancestor_locks_;
    PulseHostV1 host_{};
    PulsePluginV1 plugin_{};
    std::vector<HANDLE> package_locks_;
    bool package_dependencies_ = false;
    bool active_ = false;
    bool invoking_ = false;
};
}

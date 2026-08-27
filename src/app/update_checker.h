#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace pulse::app {

enum class UpdateError : uint8_t {
    None,
    Disabled,
    AlreadyChecking,
    Network,
    HttpStatus,
    ResponseTooLarge,
    InvalidManifest,
    InvalidSignature,
    InsecureUrl,
    UnsupportedWindows,
};

struct UpdateManifest {
    uint32_t schema = 0;
    std::string version;
    uint32_t minimum_windows_build = 0;
    std::string download_page;
    std::string installer_sha256;
    std::string signature;
};

struct UpdateResult {
    UpdateError error = UpdateError::None;
    bool update_available = false;
    std::wstring version;
    std::wstring download_page;
    std::wstring installer_sha256;
    DWORD diagnostic_code = ERROR_SUCCESS;
};

std::string CanonicalUpdatePayload(const UpdateManifest& manifest);
bool ParseUpdateManifest(std::string_view document, UpdateManifest& manifest);
UpdateResult ValidateUpdateManifest(std::string_view document,
                                    std::string_view public_key_hex,
                                    std::string_view current_version,
                                    uint32_t current_windows_build);

class UpdateChecker {
public:
    UpdateChecker() = default;
    ~UpdateChecker();
    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

    static bool Enabled() noexcept;
    bool CheckAsync(HWND notify, UINT message);
    bool TakeResult(UpdateResult& result);
    bool checking() const noexcept { return checking_.load(); }
    void Stop();

private:
    std::atomic<bool> stopping_{false};
    std::atomic<bool> checking_{false};
    std::mutex mutex_;
    std::thread worker_;
    bool has_result_ = false;
    UpdateResult result_;
};

} // namespace pulse::app

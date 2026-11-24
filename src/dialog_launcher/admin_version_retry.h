#pragma once
#include <windows.h>
#include <functional>
#include <string>

namespace pulse::dialog_launcher::admin_auto {
// An install keeps a retired DialogBridge/AppRuntime version while a program
// still has its DLLs loaded. The primary helper retries through the installed
// uninstall.exe /CLEANUPVERSIONS, which owns verification and deletion.
inline constexpr ULONGLONG kVersionRetryFirstMs = 5ull * 60 * 1000;
inline constexpr ULONGLONG kVersionRetryMinMs = 30ull * 60 * 1000;
inline constexpr ULONGLONG kVersionRetryMaxMs = 8ull * 60 * 60 * 1000;
inline constexpr ULONGLONG kVersionRetryTimeoutMs = 10ull * 60 * 1000;
// Exit codes of uninstall.exe /CLEANUPVERSIONS (setup kExit*).
inline constexpr DWORD kCleanupOk = 0, kCleanupRetained = 8, kCleanupBusy = 9;

// True when a 64-hex version directory exists that no current pointer or
// previous marker names. Cheap: reads at most five small files.
bool HasRetiredVersions(const std::wstring& root) noexcept;

enum class RetryNext { Again, Stop };
// Interval policy after a cleanup run. A busy lock retries soon; retained
// versions back off up to kVersionRetryMaxMs; success or a non-retryable
// failure stops until the helper restarts (next logon or install).
RetryNext NextRetry(DWORD exit_code, ULONGLONG& interval) noexcept;

class VersionRetry {
public:
    using LogFn = std::function<void(const char* stage, DWORD error, DWORD value)>;
    VersionRetry(std::wstring root, ULONGLONG now, LogFn log);
    VersionRetry(const VersionRetry&) = delete;
    VersionRetry& operator=(const VersionRetry&) = delete;
    ~VersionRetry();
    // Never blocks: starts the cleanup process or collects its exit code.
    void Tick(ULONGLONG now);
private:
    std::wstring root_;
    LogFn log_;
    HANDLE process_ = nullptr;
    ULONGLONG next_ = 0, interval_ = kVersionRetryMinMs, started_ = 0;
    bool stopped_ = false;
};
}

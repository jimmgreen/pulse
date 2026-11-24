#include "admin_version_retry.h"
#include "../common/dialog_admin_trust.h"
#include <algorithm>
#include <set>
#include <string_view>

namespace pulse::dialog_launcher::admin_auto {
namespace {
bool Hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
bool Digest(std::string_view text) { return text.size() == 64 && std::all_of(text.begin(), text.end(), Hex); }
// First lowercase 64-hex run of a small plain file: a previous marker holds
// only the digest, a pointer names it in "directory" and "manifest_sha256".
bool ReadNamedDigest(const std::wstring& path, std::string& digest) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char bytes[4096]{};
    DWORD read = 0;
    FILE_ATTRIBUTE_TAG_INFO info{};
    const bool ok = GetFileInformationByHandleEx(file, FileAttributeTagInfo, &info, sizeof(info)) &&
        !(info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        ReadFile(file, bytes, sizeof(bytes), &read, nullptr);
    CloseHandle(file);
    if (!ok) return false;
    const std::string_view text(bytes, read);
    size_t run = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size() && Hex(text[i])) { ++run; continue; }
        if (run == 64) { digest.assign(text.substr(i - 64, 64)); return true; }
        run = 0;
    }
    return false;
}
}

bool HasRetiredVersions(const std::wstring& root) noexcept {
    try {
        std::set<std::string> named;
        std::string digest;
        // Without a verified current dialog version setup protects everything.
        if (!ReadNamedDigest(root + L"\\dialog-current.json", digest)) return false;
        named.insert(digest);
        for (const wchar_t* file : {L"\\dialog-x86-current.json", L"\\.pulse-dialog-previous",
                                    L"\\.pulse-dialog-x86-previous", L"\\app-current.json", L"\\.pulse-app-previous"})
            if (ReadNamedDigest(root + file, digest)) named.insert(digest);
        for (const wchar_t* versions : {L"\\DialogBridge", L"\\AppRuntime"}) {
            WIN32_FIND_DATAW found{};
            HANDLE search = FindFirstFileExW((root + versions + L"\\*").c_str(), FindExInfoBasic, &found,
                                             FindExSearchNameMatch, nullptr, 0);
            if (search == INVALID_HANDLE_VALUE) continue;
            bool retired = false;
            do {
                if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                    (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
                const std::wstring_view name = found.cFileName;
                if (name.size() != 64) continue;
                std::string narrow(64, '\0');
                bool hex = true;
                for (size_t i = 0; i < 64 && hex; ++i) {
                    hex = name[i] < 0x80;
                    narrow[i] = static_cast<char>(name[i]);
                }
                if (hex && Digest(narrow) && !named.contains(narrow)) retired = true;
            } while (!retired && FindNextFileW(search, &found));
            FindClose(search);
            if (retired) return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

RetryNext NextRetry(DWORD exit_code, ULONGLONG& interval) noexcept {
    switch (exit_code) {
    case kCleanupRetained:
        interval = interval ? (std::min)(interval * 2, kVersionRetryMaxMs) : kVersionRetryMinMs;
        return RetryNext::Again;
    case kCleanupBusy:
        interval = kVersionRetryMinMs;
        return RetryNext::Again;
    default:
        return RetryNext::Stop;
    }
}

VersionRetry::VersionRetry(std::wstring root, ULONGLONG now, LogFn log)
    : root_(std::move(root)), log_(std::move(log)), next_(now + kVersionRetryFirstMs), interval_(0) {}

VersionRetry::~VersionRetry() {
    if (process_) CloseHandle(process_);
}

void VersionRetry::Tick(ULONGLONG now) {
    if (stopped_) return;
    const auto log = [&](const char* stage, DWORD error, DWORD value) {
        if (log_) try { log_(stage, error, value); } catch (...) {}
    };
    if (process_) {
        DWORD code = 0;
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
            if (now - started_ < kVersionRetryTimeoutMs) return;
            code = kCleanupRetained;  // stop tracking a stuck run; it never holds locks on owners
            log("version_cleanup_timeout", WAIT_TIMEOUT, 0);
        } else if (!GetExitCodeProcess(process_, &code)) {
            code = GetLastError();
        }
        CloseHandle(process_);
        process_ = nullptr;
        const bool again = NextRetry(code, interval_) == RetryNext::Again;
        log("version_cleanup_exit", code, again ? static_cast<DWORD>(interval_ / 60000) : 0);
        if (again) next_ = now + interval_;
        else stopped_ = true;
        return;
    }
    if (now < next_) return;
    // Retired versions only appear through an install, which restarts this helper.
    if (!HasRetiredVersions(root_)) { stopped_ = true; return; }
    const std::wstring uninstaller = root_ + L"\\uninstall.exe";
    if (const DWORD error = dialog_admin::VerifyProtectedPath(uninstaller, false)) {
        log("version_cleanup_untrusted", error, 0);
        stopped_ = true;
        return;
    }
    std::wstring command = L"\"" + uninstaller + L"\" /CLEANUPVERSIONS /VERYSILENT";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(uninstaller.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, root_.c_str(), &startup, &child)) {
        const DWORD error = GetLastError();
        log("version_cleanup_launch_failed", error, 0);
        NextRetry(kCleanupRetained, interval_);
        next_ = now + interval_;
        return;
    }
    CloseHandle(child.hThread);
    process_ = child.hProcess;
    started_ = now;
    log("version_cleanup_started", 0, child.dwProcessId);
}
}

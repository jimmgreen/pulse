#include "deferred_cleanup.h"
#include "../common/dialog_admin_trust.h"

#include <algorithm>
#include <filesystem>
#include <vector>

namespace pulse::dialog_broker {
namespace {

namespace fs = std::filesystem;
namespace paths = pulse::dialog_bridge_paths;

bool SamePathInsensitive(const std::wstring& a, const std::wstring& b) noexcept {
    if (a.empty() || b.empty()) return false;
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring StripTrailingSlash(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}

bool IsPlainDirectory(const std::wstring& path) noexcept {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) &&
           !(attrs & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool IsPlainFile(const std::wstring& path) noexcept {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           !(attrs & FILE_ATTRIBUTE_DIRECTORY) &&
           !(attrs & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool FileAbsent(const std::wstring& path) noexcept {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const DWORD err = GetLastError();
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
}

bool IsStaleBridgeArtifactName(const std::wstring& name) noexcept {
    return name.find(L".old.") != std::wstring::npos ||
           name.find(L".new.") != std::wstring::npos;
}

bool IsCoreVersionDirName(const std::wstring& name) noexcept {
    if (name.size() != 16) return false;
    return std::all_of(name.begin(), name.end(), [](wchar_t ch) {
        return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f');
    });
}

bool IsOlderThanMs(const std::wstring& path, DWORD min_age_ms) noexcept {
    if (min_age_ms == 0) return true;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    FILETIME now_ft{};
    GetSystemTimeAsFileTime(&now_ft);
    const uint64_t written = (uint64_t{data.ftLastWriteTime.dwHighDateTime} << 32) |
                             data.ftLastWriteTime.dwLowDateTime;
    const uint64_t now = (uint64_t{now_ft.dwHighDateTime} << 32) | now_ft.dwLowDateTime;
    if (now <= written) return false;
    const uint64_t elapsed_ms = (now - written) / 10000ULL;
    return elapsed_ms >= min_age_ms;
}

bool StopRequested(HANDLE stop_event) noexcept {
    return stop_event && WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0;
}

bool HasPendingBridgeResiduals(const std::wstring& bridge_root,
                               const paths::ActiveBridgePointer& pointer) noexcept {
    try {
        const auto root = StripTrailingSlash(bridge_root);
        if (!IsPlainDirectory(root)) return false;

        std::error_code ec;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            const DWORD attrs = GetFileAttributesW(it->path().c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES ||
                (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
                continue;
            }
            if (IsStaleBridgeArtifactName(it->path().filename().wstring())) {
                return true;
            }
        }

        const std::wstring versions_root = root + L"\\v";
        const auto active_x64 = StripTrailingSlash(pointer.core_dir_x64);
        const auto active_x86 = StripTrailingSlash(pointer.core_dir_x86);
        if ((active_x64.empty() && active_x86.empty()) || !IsPlainDirectory(versions_root)) return false;
        ec.clear();
        for (fs::directory_iterator it(versions_root, ec), end; !ec && it != end; it.increment(ec)) {
            const auto entry_path = StripTrailingSlash(it->path().wstring());
            if (!IsPlainDirectory(entry_path)) continue;
            if (!IsCoreVersionDirName(it->path().filename().wstring())) continue;
            if (SamePathInsensitive(entry_path, active_x64) ||
                SamePathInsensitive(entry_path, active_x86)) {
                continue;
            }
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool HasPendingInstallResiduals(const paths::ActiveBridgePointer& pointer) noexcept {
    try {
        const auto install_root = StripTrailingSlash(pointer.install_root);
        if (install_root.size() < 4 || !IsPlainDirectory(install_root)) return false;
        if (!FileAbsent(install_root + L"\\.pulse-setup")) return true;

        // 1. Retired legacy DialogBridge folder on a migrated AppRuntime-only install.
        if (FileAbsent(install_root + L"\\dialog-current.json") &&
            IsPlainFile(install_root + L"\\app-current.json") &&
            IsPlainDirectory(install_root + L"\\DialogBridge")) {
            return true;
        }

        // 2. Obsolete AppRuntime versions beyond current + previous (2 protected dirs).
        const std::wstring app_runtime = install_root + L"\\AppRuntime";
        if (IsPlainDirectory(app_runtime)) {
            size_t version_dirs = 0;
            std::error_code ec;
            for (fs::directory_iterator it(app_runtime, ec), end; !ec && it != end; it.increment(ec)) {
                if (IsPlainDirectory(it->path().wstring()) &&
                    it->path().filename().wstring().size() == 64) {
                    if (++version_dirs > 2) return true;
                }
            }
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool RemoveDirectoryIfUnlocked(const fs::path& dir_path) noexcept {
    try {
        if (!IsPlainDirectory(dir_path.wstring())) return false;
        std::vector<fs::path> files;
        std::vector<fs::path> dirs;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(dir_path, ec), end; !ec && it != end; it.increment(ec)) {
            const DWORD attrs = GetFileAttributesW(it->path().c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
                return false;
            }
            if (attrs & FILE_ATTRIBUTE_DIRECTORY) dirs.push_back(it->path());
            else files.push_back(it->path());
            if (files.size() + dirs.size() > 256) return false;
        }
        if (ec) return false;

        // Only delete if every file inside is unlocked for delete sharing.
        std::vector<HANDLE> handles;
        handles.reserve(files.size());
        bool all_unlocked = true;
        for (const auto& f : files) {
            HANDLE h = CreateFileW(f.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                all_unlocked = false;
                break;
            }
            handles.push_back(h);
        }
        if (all_unlocked) {
            for (size_t i = 0; i < handles.size(); ++i) {
                FILE_DISPOSITION_INFO disp{TRUE};
                if (!SetFileInformationByHandle(handles[i], FileDispositionInfo, &disp, sizeof(disp))) {
                    all_unlocked = false;
                    for (size_t j = 0; j < i; ++j) {
                        FILE_DISPOSITION_INFO keep{FALSE};
                        SetFileInformationByHandle(handles[j], FileDispositionInfo, &keep, sizeof(keep));
                    }
                    break;
                }
            }
        }
        for (HANDLE h : handles) CloseHandle(h);
        if (!all_unlocked) return false;

        std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
            return a.native().size() > b.native().size();
        });
        for (const auto& d : dirs) RemoveDirectoryW(d.c_str());
        return RemoveDirectoryW(dir_path.c_str()) != FALSE;
    } catch (...) {
        return false;
    }
}

void PruneBridgeRoot(const std::wstring& bridge_root,
                     const paths::ActiveBridgePointer& pointer,
                     const ResidualCleanupOptions& options,
                     HANDLE stop_event,
                     ResidualCleanupResult& out) noexcept {
    try {
        const auto root = StripTrailingSlash(bridge_root);
        if (!IsPlainDirectory(root)) return;
        if (options.verify_protected &&
            pulse::dialog_admin::VerifyProtectedPath(root, true) != ERROR_SUCCESS) {
            return;
        }
        out.ran_bridge_prune = true;

        // 1. Delete unlocked *.old.* / *.new.* files directly under bridge_root.
        std::vector<fs::path> stale_files;
        std::error_code ec;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            const DWORD attrs = GetFileAttributesW(it->path().c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES ||
                (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
                continue;
            }
            if (IsStaleBridgeArtifactName(it->path().filename().wstring())) {
                stale_files.push_back(it->path());
            }
        }
        for (const auto& file : stale_files) {
            if (StopRequested(stop_event)) return;
            if (DeleteFileW(file.c_str())) {
                ++out.stale_files_removed;
            }
        }

        // 2. Delete unlocked obsolete Core version dirs under v\<16-hex>\.
        const std::wstring versions_root = root + L"\\v";
        const auto active_x64 = StripTrailingSlash(pointer.core_dir_x64);
        const auto active_x86 = StripTrailingSlash(pointer.core_dir_x86);
        if ((active_x64.empty() && active_x86.empty()) || !IsPlainDirectory(versions_root)) return;
        std::vector<fs::path> candidate_dirs;
        ec.clear();
        for (fs::directory_iterator it(versions_root, ec), end; !ec && it != end; it.increment(ec)) {
            const auto entry_path = StripTrailingSlash(it->path().wstring());
            if (!IsPlainDirectory(entry_path)) continue;
            if (!IsCoreVersionDirName(it->path().filename().wstring())) continue;
            if (SamePathInsensitive(entry_path, active_x64) ||
                SamePathInsensitive(entry_path, active_x86)) {
                continue;
            }
            if (!IsOlderThanMs(entry_path, options.min_core_age_ms)) continue;
            candidate_dirs.push_back(it->path());
        }
        for (const auto& dir : candidate_dirs) {
            if (StopRequested(stop_event)) return;
            if (RemoveDirectoryIfUnlocked(dir)) {
                ++out.core_dirs_removed;
            }
        }
    } catch (...) {}
}

void RunUninstallerCleanup(const paths::ActiveBridgePointer& pointer,
                           const ResidualCleanupOptions& options,
                           HANDLE stop_event,
                           ResidualCleanupResult& out) noexcept {
    try {
        if (StopRequested(stop_event)) return;
        const auto install_root = StripTrailingSlash(pointer.install_root);
        if (install_root.size() < 4 || !IsPlainDirectory(install_root)) return;
        // Never race with an active installer transaction.
        if (!FileAbsent(install_root + L"\\.pulse-setup")) return;

        const std::wstring uninstaller = install_root + L"\\uninstall.exe";
        if (!IsPlainFile(uninstaller)) return;

        // Security gate: PulseDialogBroker runs as LocalSystem; only execute
        // uninstall.exe from an admin/SYSTEM-protected directory with no reparse points.
        if (options.verify_protected) {
            if (pulse::dialog_admin::VerifyProtectedPath(install_root, true) != ERROR_SUCCESS ||
                pulse::dialog_admin::VerifyProtectedPath(uninstaller, false) != ERROR_SUCCESS) {
                return;
            }
        }

        out.ran_install_cleanup = true;
        std::wstring cmd = L"\"" + uninstaller + L"\" /CLEANUPVERSIONS";
        if (options.pass_test_flag) cmd += L" /PULSETEST";

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(uninstaller.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, install_root.c_str(), &si, &pi)) {
            out.install_cleanup_exit_code = GetLastError();
            return;
        }
        CloseHandle(pi.hThread);
        out.install_cleanup_launched = true;

        const ULONGLONG deadline = GetTickCount64() + options.uninstaller_timeout_ms;
        for (;;) {
            const ULONGLONG now = GetTickCount64();
            const DWORD remaining = (now >= deadline) ? 0u : static_cast<DWORD>(deadline - now);
            HANDLE waits[2]{pi.hProcess, stop_event};
            const DWORD count = stop_event ? 2u : 1u;
            const DWORD w = WaitForMultipleObjects(count, waits, FALSE, remaining);
            if (w == WAIT_OBJECT_0) {
                GetExitCodeProcess(pi.hProcess, &out.install_cleanup_exit_code);
                break;
            }
            if (w == WAIT_OBJECT_0 + 1) {
                // Service stopping: allow a brief graceful finish, never hard-kill mid-cleanup.
                if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0) {
                    GetExitCodeProcess(pi.hProcess, &out.install_cleanup_exit_code);
                } else {
                    out.install_cleanup_exit_code = ERROR_OPERATION_ABORTED;
                }
                break;
            }
            out.install_cleanup_exit_code = ERROR_TIMEOUT;
            break;
        }
        CloseHandle(pi.hProcess);
    } catch (...) {}
}

} // namespace

bool HasPendingResidualCleanup(const std::wstring& bridge_root,
                               const paths::ActiveBridgePointer& pointer) noexcept {
    return HasPendingBridgeResiduals(bridge_root, pointer) ||
           HasPendingInstallResiduals(pointer);
}

ResidualCleanupResult RunResidualCleanupPass(const std::wstring& bridge_root,
                                             const paths::ActiveBridgePointer& pointer,
                                             const ResidualCleanupOptions& options,
                                             HANDLE stop_event) noexcept {
    ResidualCleanupResult result{};
    if (StopRequested(stop_event)) {
        result.pending_after = HasPendingResidualCleanup(bridge_root, pointer);
        return result;
    }

    if (HasPendingBridgeResiduals(bridge_root, pointer)) {
        PruneBridgeRoot(bridge_root, pointer, options, stop_event, result);
    }
    if (!StopRequested(stop_event) && HasPendingInstallResiduals(pointer)) {
        RunUninstallerCleanup(pointer, options, stop_event, result);
    }

    result.pending_after = HasPendingResidualCleanup(bridge_root, pointer);
    return result;
}

} // namespace pulse::dialog_broker

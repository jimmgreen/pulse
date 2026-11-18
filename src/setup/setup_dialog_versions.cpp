#include "setup_dialog_versions.h"
#include "setup_common.h"
#include "setup_log.h"
#include "../common/dialog_bundle.h"
#include <algorithm>
#include <filesystem>

namespace pulse::setup {
namespace {
namespace fs = std::filesystem;
constexpr wchar_t kPrevious[] = L".pulse-dialog-previous";
bool Same(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
bool PlainDirectory(const fs::path& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool Exists(const fs::path& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool ReadDigest(const fs::path& path, std::string& digest) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    FILE_ATTRIBUTE_TAG_INFO info{}; char bytes[65]{}; DWORD read = 0;
    const bool ok = GetFileInformationByHandleEx(file, FileAttributeTagInfo, &info, sizeof(info)) &&
        !(info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        ReadFile(file, bytes, sizeof(bytes), &read, nullptr) && read == 64;
    CloseHandle(file); if (!ok) return false;
    digest.assign(bytes, 64); return IsDialogVersionDigest(digest);
}
bool Identity(const fs::path& path, BY_HANDLE_FILE_INFORMATION& info) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool ok = GetFileInformationByHandle(file, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    CloseHandle(file); return ok;
}
bool SameIdentity(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
        a.nFileIndexLow == b.nFileIndexLow && a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
        CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0;
}
}
bool IsDialogVersionDigest(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
std::wstring InstalledRuntimeFile(const std::wstring& app, const wchar_t* name) {
    dialog_bundle::BundleLease lease;
    if (Exists(fs::path(app) / L"dialog-current.json")) {
        if (dialog_bundle::ResolveInstalledBundle(app, lease)) return {};
        const auto candidate = fs::path(lease.Directory()) / name;
        if (Exists(candidate)) return candidate.wstring();
    }
    return (fs::path(app) / name).wstring();
}
bool IsDialogVersionFile(const std::wstring& relative) {
    auto rel = relative; std::replace(rel.begin(), rel.end(), L'/', L'\\');
    return rel.size() > 78 && Same(rel.substr(0, 13), L"DialogBridge\\") && rel[77] == L'\\' &&
        IsDialogVersionDigest(WideToUtf8(rel.substr(13, 64)));
}
bool PrepareDialogVersion(const std::wstring& app, const std::wstring& staging,
    std::vector<std::wstring>& files, DialogVersionPlan& plan, std::wstring& error) {
    plan = {};
    const bool pointer = std::any_of(files.begin(), files.end(), [](const auto& f) { return Same(f, L"dialog-current.json"); });
    if (!pointer) { plan.replacements = files; return true; }
    dialog_bundle::BundleLease staged;
    DWORD result = dialog_bundle::ResolveInstalledBundle(staging, staged);
    if (result) { error = L"Invalid staged dialog bundle: " + Win32Error(result); return false; }
    plan.versioned = true; plan.digest = staged.ManifestDigest();
    plan.relative_directory = L"DialogBridge\\" + Utf8ToWide(plan.digest);
    staged = {};
    const fs::path bridge = fs::path(app) / L"DialogBridge";
    if (Exists(bridge) && !PlainDirectory(bridge)) { error = L"DialogBridge is not a plain directory."; return false; }
    const fs::path target = fs::path(app) / plan.relative_directory;
    if (Exists(target)) {
        dialog_bundle::BundleLease installed;
        result = dialog_bundle::VerifyBundle(target.wstring(), plan.digest, installed);
        if (result) { error = L"Existing dialog version failed verification: " + Win32Error(result); return false; }
        plan.reused = true;
    }
    std::string previous;
    dialog_bundle::BundleLease current;
    if (Exists(fs::path(app) / L"dialog-current.json")) {
        result = dialog_bundle::ResolveInstalledBundle(app, current);
        if (result) { error = L"Current dialog bundle failed verification: " + Win32Error(result); return false; }
        if (current.ManifestDigest() != plan.digest) previous = current.ManifestDigest();
        else if (Exists(fs::path(app) / kPrevious)) {
            if (!ReadDigest(fs::path(app) / kPrevious, previous)) {
                error = L"Invalid previous dialog version marker."; return false;
            }
        }
    }
    if (!previous.empty()) {
        if (!WriteWholeFile((fs::path(staging) / kPrevious).wstring(), previous.data(), previous.size())) {
            error = L"Cannot stage previous dialog version marker."; return false;
        }
        files.emplace_back(kPrevious);
    }
    for (const auto& f : files) {
        if (Same(f, L"dialog-current.json")) continue;
        auto normalized = f; std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
        if (normalized.size() >= 13 && Same(normalized.substr(0, 13), L"DialogBridge\\") && !IsDialogVersionFile(f)) {
            error = L"Unmanaged DialogBridge entry in versioned payload."; return false;
        }
        if (IsDialogVersionFile(f)) {
            if (normalized.substr(0, 77) != plan.relative_directory) { error = L"Unexpected dialog version in payload."; return false; }
            continue;
        }
        plan.replacements.push_back(f);
    }
    // The pointer is journaled after every new bundle member and its rollback marker.
    plan.replacements.emplace_back(L"dialog-current.json");
    return true;
}
bool PublishDialogVersion(const std::wstring& app, const std::wstring& staging,
    const DialogVersionPlan& plan, std::wstring& error) {
    if (!plan.versioned) return true;
    const fs::path source = fs::path(staging) / plan.relative_directory;
    const fs::path target = fs::path(app) / plan.relative_directory;
    const auto bridge = target.parent_path();
    if (!Exists(bridge) && !CreateDirectoryW(bridge.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = L"Cannot create dialog version directory."; return false;
    }
    if (!PlainDirectory(bridge)) { error = L"DialogBridge is not a plain directory."; return false; }
    if (Exists(target)) {
        dialog_bundle::BundleLease existing;
        const DWORD result = dialog_bundle::VerifyBundle(target.wstring(), plan.digest, existing);
        if (result) { error = L"Cannot reuse dialog version: " + Win32Error(result); return false; }
        return true;
    }
    // This immutable publication is deliberately outside the root-file rollback.
    // A host can load B as soon as registration publishes it; rolling back to A
    // must not depend on deleting B while that host is alive.
    const ULONGLONG started = GetTickCount64();
    constexpr ULONGLONG retry_budget_ms = 10000;
    constexpr DWORD delays[] = {10, 25, 50, 100, 200, 400};
    for (size_t attempt = 0;; ++attempt) {
        if (MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
            if (attempt) Log(L"Published dialog version after waiting " + std::to_wstring(GetTickCount64() - started) + L" ms");
            return true;
        }
        const DWORD failure = GetLastError();
        const ULONGLONG elapsed = GetTickCount64() - started;
        if (!attempt) Log(L"Dialog version publication failed from " + source.wstring() + L" to " + target.wstring() + L": " + Win32Error(failure));
        if ((failure != ERROR_ACCESS_DENIED && failure != ERROR_SHARING_VIOLATION && failure != ERROR_LOCK_VIOLATION) ||
            elapsed >= retry_budget_ms) {
            error = L"Cannot publish dialog version: " + Win32Error(failure) +
                L"\nSource: " + source.wstring() + L"\nDestination: " + target.wstring() +
                L"\nWaited: " + std::to_wstring(elapsed) + L" ms";
            return false;
        }
        // Scanners can hold a newly extracted child without delete sharing for
        // several seconds; Windows reports access denied when renaming its directory.
        // Setup worker only. Never replace a destination that appeared while
        // waiting; another publisher's directory remains untouched.
        const DWORD delay = attempt < std::size(delays) ? delays[attempt] : 500;
        Sleep(static_cast<DWORD>(std::min<ULONGLONG>(delay, retry_budget_ms - elapsed)));
    }
}
DialogCleanupReport CleanupDialogVersionsReport(const std::wstring& app, bool uninstall) {
    DialogCleanupReport report;
    const fs::path bridge = fs::path(app) / L"DialogBridge";
    auto retain = [&](const fs::path& directory, const std::wstring& reason, DWORD error = ERROR_SUCCESS, bool blocked = false) {
        (blocked ? report.blocked_directories : report.retained_directories).push_back({directory.wstring(), reason, error});
        Log(L"Retained dialog version: " + directory.wstring() + L"; " + reason +
            (error ? L"; Win32: " + std::to_wstring(error) : L""));
    };
    auto busy = [](DWORD error) {
        return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_ACCESS_DENIED;
    };
    std::vector<std::wstring> blocked_files;
    auto identify = [&](const std::vector<fs::path>& files, DWORD failure) {
        for (const auto& file : files) {
            const auto relative = file.lexically_relative(app).wstring();
            if (std::find(blocked_files.begin(), blocked_files.end(), relative) == blocked_files.end()) blocked_files.push_back(relative);
        }
        report.locks.error = failure;
    };
    const DWORD bridge_attributes = GetFileAttributesW(bridge.c_str());
    if (bridge_attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD failure = GetLastError();
        if (failure != ERROR_FILE_NOT_FOUND && failure != ERROR_PATH_NOT_FOUND) {
            report.scan_complete = false;
            retain(bridge, L"Version root cannot be inspected; no versions changed", failure, busy(failure));
        }
        return report;
    }
    if (!(bridge_attributes & FILE_ATTRIBUTE_DIRECTORY) || (bridge_attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        retain(bridge, L"Not a plain directory; ownership is not established", ERROR_NOT_SUPPORTED);
        return report;
    }
    std::string current_digest, previous;
    if (!uninstall) {
        dialog_bundle::BundleLease current;
        const DWORD failure = dialog_bundle::ResolveInstalledBundle(app, current);
        if (failure) {
            report.scan_complete = false;
            retain(bridge, L"Current version cannot be verified; all versions protected", failure);
            return report;
        }
        current_digest = current.ManifestDigest();
        if (Exists(fs::path(app) / kPrevious) && !ReadDigest(fs::path(app) / kPrevious, previous)) {
            report.scan_complete = false;
            retain(bridge, L"Previous version marker is invalid; all versions protected", ERROR_INVALID_DATA);
            return report;
        }
    }
    // Snapshot all candidates before mutation. The old 16-entry pass repeatedly
    // visited protected/busy entries and could permanently starve later versions.
    constexpr size_t max_candidates = 4096;
    std::vector<fs::path> candidates;
    std::error_code ec;
    fs::directory_iterator it(bridge, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (candidates.size() == max_candidates) {
            report.scan_complete = false;
            retain(bridge, L"Candidate limit (4096) exceeded; no versions changed", ERROR_FILE_TOO_LARGE);
            return report;
        }
        candidates.push_back(it->path());
    }
    if (ec) {
        report.scan_complete = false;
        retain(bridge, L"Candidate enumeration failed; no versions changed", static_cast<DWORD>(ec.value()));
        return report;
    }
    std::sort(candidates.begin(), candidates.end());
    for (const auto& directory : candidates) {
        const auto digest = WideToUtf8(directory.filename().wstring());
        if (!IsDialogVersionDigest(digest)) { retain(directory, L"Unknown directory name; not installer-owned"); continue; }
        if (digest == current_digest || digest == previous) {
            report.protected_directories.push_back(directory.wstring());
            Log(L"Protected dialog version (current/previous): " + directory.wstring());
            continue;
        }
        if (!PlainDirectory(directory)) { retain(directory, L"Not a plain directory; reparse points are retained", ERROR_NOT_SUPPORTED); continue; }
        std::vector<fs::path> files, dirs;
        std::error_code scan;
        for (fs::recursive_directory_iterator entry(directory, scan), last; !scan && entry != last; entry.increment(scan)) {
            const DWORD attrs = GetFileAttributesW(entry->path().c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) || files.size() + dirs.size() >= 2048) {
                scan = std::make_error_code(std::errc::invalid_argument); break;
            }
            ((attrs & FILE_ATTRIBUTE_DIRECTORY) ? dirs : files).push_back(entry->path());
        }
        if (scan) { retain(directory, L"Unsafe, oversized or unreadable tree; no members deleted", static_cast<DWORD>(scan.value())); continue; }
        dialog_bundle::BundleLease lease;
        DWORD failure = dialog_bundle::VerifyBundle(directory.wstring(), digest, lease);
        if (failure) {
            if (busy(failure)) identify(files, failure);
            retain(directory, busy(failure) ? L"Version verification blocked; close owners and retry" :
                L"Unknown or damaged bundle; ownership verification failed", failure, busy(failure));
            continue;
        }
        std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) { return a.native().size() > b.native().size(); });
        dirs.push_back(directory);
        std::vector<fs::path> owned = files; owned.insert(owned.end(), dirs.begin(), dirs.end());
        std::vector<BY_HANDLE_FILE_INFORMATION> identities(owned.size());
        bool ready = true;
        for (size_t i = 0; i < owned.size(); ++i) if (!Identity(owned[i], identities[i])) { failure = GetLastError(); ready = false; break; }
        if (ready) { failure = dialog_bundle::RecheckDirectory(lease); ready = failure == ERROR_SUCCESS; }
        if (!ready) {
            lease = {};
            if (busy(failure)) identify(files, failure);
            retain(directory, L"Version changed or is blocked before cleanup; no members deleted", failure, busy(failure));
            continue;
        }
        lease = {};
        std::vector<HANDLE> locks;
        for (size_t i = 0; i < owned.size(); ++i) {
            HANDLE h = CreateFileW(owned[i].c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (h == INVALID_HANDLE_VALUE) { failure = GetLastError(); ready = false; break; }
            locks.push_back(h);
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(h, &info)) { failure = GetLastError(); ready = false; break; }
            if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !SameIdentity(identities[i], info)) {
                failure = ERROR_FILE_INVALID; ready = false; break;
            }
        }
        size_t marked = 0;
        bool payload_removed = false;
        if (ready) {
            // Hold every member before marking any file. A busy DLL must leave
            // the complete bundle, including manifests and licenses, unchanged.
            for (; marked < files.size(); ++marked) {
                FILE_DISPOSITION_INFO disposition{TRUE};
                if (!SetFileInformationByHandle(locks[marked], FileDispositionInfo, &disposition, sizeof(disposition))) {
                    failure = GetLastError(); ready = false; break;
                }
            }
            if (!ready) for (size_t i = 0; i < marked; ++i) {
                FILE_DISPOSITION_INFO keep{FALSE};
                if (!SetFileInformationByHandle(locks[i], FileDispositionInfo, &keep, sizeof(keep)))
                    Log(L"Dialog cleanup could not cancel pending deletion; retained tree requires manual ownership review: " + directory.wstring());
            }
            if (ready) {
                for (size_t i = 0; i < files.size(); ++i) { CloseHandle(locks[i]); locks[i] = nullptr; }
                payload_removed = true;
                for (size_t i = files.size(); i < locks.size(); ++i) {
                    FILE_DISPOSITION_INFO disposition{TRUE};
                    if (!SetFileInformationByHandle(locks[i], FileDispositionInfo, &disposition, sizeof(disposition))) {
                        failure = GetLastError(); ready = false;
                        Log(L"Dialog payload removed but empty directory retained: " + owned[i].wstring()); break;
                    }
                    CloseHandle(locks[i]); locks[i] = nullptr;
                }
            }
        }
        for (HANDLE h : locks) if (h) CloseHandle(h);
        if (!ready) {
            if (payload_removed) {
                retain(directory, L"All verified payload files deleted; empty directory removal failed", failure);
                continue;
            }
            if (busy(failure)) identify(files, failure);
            retain(directory, busy(failure) ? L"Deletion blocked; close owners and retry" : L"Deletion failed; tree retained for review", failure, busy(failure));
            continue;
        }
        report.deleted_directories.push_back(directory.wstring());
        Log(L"Deleted retired dialog version: " + directory.wstring());
    }
    if (!report.blocked_directories.empty()) {
        const DWORD failure = report.locks.error;
        if (!blocked_files.empty()) report.locks = InspectReplacementLocks(app, blocked_files, true);
        if (report.locks.files.empty() && !report.locks.error) report.locks.error = failure;
        Log(L"Dialog cleanup blockers:\n" + DescribeReplacementLocks(report.locks));
    }
    return report;
}
bool CleanupDialogVersions(const std::wstring& app, bool uninstall) {
    return CleanupDialogVersionsReport(app, uninstall).Complete();
}
}

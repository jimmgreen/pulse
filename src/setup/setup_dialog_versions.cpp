#include "setup_dialog_versions.h"
#include "setup_common.h"
#include "setup_log.h"
#include "../common/dialog_bundle.h"
#include "../common/json_value.h"
#include <map>
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

namespace {
constexpr wchar_t kX86Pointer[] = L"dialog-x86-current.json";
constexpr wchar_t kX86Previous[] = L".pulse-dialog-x86-previous";
DWORD Presence(const fs::path& path, bool& present) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    present = attributes != INVALID_FILE_ATTRIBUTES;
    if (present) return ERROR_SUCCESS;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ERROR_SUCCESS : error;
}
DWORD ResolveVersion(const std::wstring& app, bool companion, unsigned machine, dialog_bundle::BundleLease& lease) {
    const DWORD result = companion ? dialog_bundle::ResolveInstalledBundleForMachine(app, IMAGE_FILE_MACHINE_I386, lease) :
        dialog_bundle::ResolveInstalledBundle(app, lease);
    return result ? result : machine && lease.Machine() != machine ? ERROR_BAD_EXE_FORMAT : ERROR_SUCCESS;
}
bool OrphanPointer(const std::wstring& app, const wchar_t* name, std::wstring& missing) {
    // A missing manifest or component is not an orphan. Recheck the pointer and
    // pin every plain ancestor before testing the selected directory itself.
    const fs::path root(app);
    if (app.size() < 3 || app[1] != L':' || app[2] != L'\\' || app.find(L'/') != app.npos ||
        app.find(L':', 2) != app.npos || app.find(L'\0') != app.npos ||
        !root.is_absolute() || root.lexically_normal() != root) return false;
    struct Handles {
        std::vector<HANDLE> values;
        ~Handles() { for (const auto handle : values) CloseHandle(handle); }
        bool Add(const fs::path& path, bool directory) {
            const HANDLE handle = CreateFileW(path.c_str(), directory ? FILE_READ_ATTRIBUTES : GENERIC_READ,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr);
            if (handle == INVALID_HANDLE_VALUE) return false;
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory) {
                CloseHandle(handle); return false;
            }
            values.push_back(handle); return true;
        }
    } handles;
    auto ancestor = root.root_path();
    if (!handles.Add(ancestor, true)) return false;
    for (const auto& part : root.relative_path()) {
        ancestor /= part;
        if (!handles.Add(ancestor, true)) return false;
    }
    if (!handles.Add(root / L"DialogBridge", true) || !handles.Add(root / name, false)) return false;
    const HANDLE pointer = handles.values.back();
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(pointer, &size) || size.QuadPart <= 0 || size.QuadPart > 1024) return false;
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD count = 0;
    if (!ReadFile(pointer, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count != bytes.size()) return false;
    json::Value value;
    if (!json::Parse(bytes, value, nullptr, {1024, 8}) || !value.is_object() || value.size() != 3) return false;
    const auto schema = value.find("schema");
    int64_t number = 0;
    if (!schema || !schema->get_int(number, 1, 1)) return false;
    const auto digest = value.get_string("manifest_sha256"), directory = value.get_string("directory");
    if (!IsDialogVersionDigest(digest) || directory != "DialogBridge/" + digest) return false;
    const auto selected = root / L"DialogBridge" / Utf8ToWide(digest);
    if (GetFileAttributesW(selected.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
    missing = selected.wstring();
    return true;
}
bool PrepareOne(const std::wstring& app, const std::wstring& staging, bool companion,
    std::vector<std::wstring>& files, DialogVersionPlan& plan, std::wstring& error) {
    const wchar_t* pointer = companion ? kX86Pointer : L"dialog-current.json";
    const wchar_t* marker = companion ? kX86Previous : kPrevious;
    dialog_bundle::BundleLease staged;
    DWORD result = ResolveVersion(staging, companion, companion ? IMAGE_FILE_MACHINE_I386 : 0, staged);
    if (result) { error = L"Invalid staged dialog bundle: " + Win32Error(result); return false; }
    plan.versioned = true; plan.digest = staged.ManifestDigest(); plan.machine = staged.Machine();
    plan.relative_directory = L"DialogBridge\\" + Utf8ToWide(plan.digest);
    staged = {};
    const fs::path target = fs::path(app) / plan.relative_directory;
    if (Exists(target)) {
        dialog_bundle::BundleLease installed;
        result = dialog_bundle::VerifyBundle(target.wstring(), plan.digest, installed);
        if (!result && installed.Machine() != plan.machine) result = ERROR_BAD_EXE_FORMAT;
        if (result) { error = L"Existing dialog version failed verification: " + Win32Error(result); return false; }
        plan.reused = true;
    }
    bool has_current = false;
    result = Presence(fs::path(app) / pointer, has_current);
    if (result) { error = L"Current dialog pointer cannot be inspected: " + Win32Error(result); return false; }
    std::string previous;
    if (has_current) {
        dialog_bundle::BundleLease current;
        result = ResolveVersion(app, companion, plan.machine, current);
        if (result) {
            std::wstring missing;
            if (!companion || (result != ERROR_FILE_NOT_FOUND && result != ERROR_PATH_NOT_FOUND) || !OrphanPointer(app, pointer, missing)) {
                error = L"Current dialog bundle failed verification (" + std::wstring(pointer) + L"): " + Win32Error(result);
                return false;
            }
            // Keep old pointer/markers in the normal root transaction. An
            // unverified orphan must never become the trusted previous version.
            Log(L"Repairing orphan dialog pointer " + std::wstring(pointer) + L"; missing version directory: " + missing +
                L"; replacement staged bundle fully verified.");
        } else if (current.ManifestDigest() != plan.digest) previous = current.ManifestDigest();
        else {
            bool has_previous = false;
            result = Presence(fs::path(app) / marker, has_previous);
            if (result || (has_previous && !ReadDigest(fs::path(app) / marker, previous))) {
                error = L"Invalid previous dialog version marker."; return false;
            }
        }
    }
    if (!previous.empty()) {
        if (!WriteWholeFile((fs::path(staging) / marker).wstring(), previous.data(), previous.size())) {
            error = L"Cannot stage previous dialog version marker."; return false;
        }
        if (std::none_of(files.begin(), files.end(), [&](const auto& file) { return Same(file, marker); })) files.emplace_back(marker);
        plan.replacements.emplace_back(marker);
    }
    plan.replacements.emplace_back(pointer);
    return true;
}
}
bool PrepareDialogVersion(const std::wstring& app, const std::wstring& staging,
    std::vector<std::wstring>& files, DialogVersionPlan& plan, std::wstring& error) {
    plan = {};
    const bool pointer = std::any_of(files.begin(), files.end(), [](const auto& file) { return Same(file, L"dialog-current.json"); });
    const bool companion = std::any_of(files.begin(), files.end(), [](const auto& file) { return Same(file, kX86Pointer); });
    bool physical_companion = false;
    const DWORD presence = Presence(fs::path(staging) / kX86Pointer, physical_companion);
    if (presence || companion != physical_companion) { error = L"Unlisted or inaccessible x86 dialog pointer."; return false; }
    bool installed_companion = false;
    const DWORD installed_presence = Presence(fs::path(app) / kX86Pointer, installed_companion);
    if (installed_presence || (installed_companion && !companion)) {
        error = L"An installation with an x86 dialog companion requires a dual-architecture update.";
        return false;
    }
    if (!pointer) {
        if (companion) { error = L"An x86 companion requires the primary dialog pointer."; return false; }
        plan.replacements = files; return true;
    }
    const fs::path bridge = fs::path(app) / L"DialogBridge";
    if (Exists(bridge) && !PlainDirectory(bridge)) { error = L"DialogBridge is not a plain directory."; return false; }
    if (!PrepareOne(app, staging, false, files, plan, error)) return false;
    if (companion) {
        if (plan.machine != IMAGE_FILE_MACHINE_AMD64) { error = L"An x86 companion requires an x64 primary bundle."; return false; }
        DialogVersionPlan peer;
        if (!PrepareOne(app, staging, true, files, peer, error)) return false;
        plan.companions.push_back(std::move(peer));
    }
    plan.replacements.clear();
    for (const auto& file : files) {
        if (Same(file, L"dialog-current.json") || Same(file, kX86Pointer)) continue;
        auto normalized = file; std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
        if (normalized.size() >= 13 && Same(normalized.substr(0, 13), L"DialogBridge\\") && !IsDialogVersionFile(file)) {
            error = L"Unmanaged DialogBridge entry in versioned payload."; return false;
        }
        if (IsDialogVersionFile(file)) {
            const auto directory = normalized.substr(0, 77);
            if (directory != plan.relative_directory && (plan.companions.empty() || directory != plan.companions.front().relative_directory)) {
                error = L"Unexpected dialog version in payload."; return false;
            }
            continue;
        }
        plan.replacements.push_back(file);
    }
    if (companion) plan.replacements.emplace_back(kX86Pointer);
    // Existing engine inserts .pulse-files immediately before this final commit point.
    plan.replacements.emplace_back(L"dialog-current.json");
    return true;
}
static bool PublishSingleVersion(const std::wstring& app, const std::wstring& staging,
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
bool PublishDialogVersion(const std::wstring& app, const std::wstring& staging,
    const DialogVersionPlan& plan, std::wstring& error) {
    if (!plan.versioned) {
        if (!plan.companions.empty()) { error = L"Unversioned payload has dialog companions."; return false; }
        return true;
    }
    if (plan.companions.size() > 1 || (!plan.companions.empty() &&
        (plan.machine != IMAGE_FILE_MACHINE_AMD64 || !plan.companions.front().versioned ||
         plan.companions.front().machine != IMAGE_FILE_MACHINE_I386 || !plan.companions.front().companions.empty()))) {
        error = L"Invalid dialog companion plan."; return false;
    }
    std::vector<const DialogVersionPlan*> versions{&plan};
    for (const auto& peer : plan.companions) versions.push_back(&peer);
    // Verify both pointer bindings and full closures before publishing either
    // directory. Root pointers/markers remain in the caller's one file transaction.
    for (const auto* version : versions) {
        if (!IsDialogVersionDigest(version->digest) ||
            version->relative_directory != L"DialogBridge\\" + Utf8ToWide(version->digest)) {
            error = L"Invalid dialog version plan."; return false;
        }
        if (version->machine) {
            dialog_bundle::BundleLease staged;
            DWORD result = ResolveVersion(staging, version != &plan, version->machine, staged);
            if (!result && staged.ManifestDigest() != version->digest) result = ERROR_FILE_INVALID;
            if (result) { error = L"Staged dialog version changed before publication: " + Win32Error(result); return false; }
            const auto target = fs::path(app) / version->relative_directory;
            if (Exists(target)) {
                dialog_bundle::BundleLease installed;
                result = dialog_bundle::VerifyBundle(target.wstring(), version->digest, installed);
                if (!result && installed.Machine() != version->machine) result = ERROR_BAD_EXE_FORMAT;
                if (result) { error = L"Existing dialog version changed before publication: " + Win32Error(result); return false; }
            }
        }
    }
    for (const auto* version : versions) if (!PublishSingleVersion(app, staging, *version, error)) return false;
    return true;
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
    std::vector<std::string> protected_digests;
    if (!uninstall) {
        dialog_bundle::BundleLease current;
        const DWORD failure = dialog_bundle::ResolveInstalledBundle(app, current);
        if (failure) {
            report.scan_complete = false;
            retain(bridge, L"Current version cannot be verified; all versions protected", failure);
            return report;
        }
        protected_digests.push_back(current.ManifestDigest());
        bool companion = false;
        DWORD error = Presence(fs::path(app) / kX86Pointer, companion);
        if (!error && companion) {
            if (current.Machine() != IMAGE_FILE_MACHINE_AMD64) error = ERROR_BAD_EXE_FORMAT;
            else {
                dialog_bundle::BundleLease peer;
                error = ResolveVersion(app, true, IMAGE_FILE_MACHINE_I386, peer);
                if (!error) protected_digests.push_back(peer.ManifestDigest());
            }
        }
        if (error) {
            report.scan_complete = false;
            retain(bridge, L"Companion current version cannot be verified; all versions protected", error, busy(error));
            return report;
        }
        for (const auto* marker : {kPrevious, kX86Previous}) {
            bool present = false;
            std::string previous;
            error = Presence(fs::path(app) / marker, present);
            if (error || (present && !ReadDigest(fs::path(app) / marker, previous))) {
                report.scan_complete = false;
                retain(bridge, L"Previous version marker is invalid; all versions protected", error ? error : ERROR_INVALID_DATA);
                return report;
            }
            if (present) protected_digests.push_back(previous);
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
    // These bounded hints only order deletion and retain dependencies. They never
    // establish ownership: every deletion still requires VerifyBundle below.
    std::map<fs::path, fs::path> dependencies;
    for (const auto& directory : candidates) {
        if (!IsDialogVersionDigest(WideToUtf8(directory.filename().wstring())) || !PlainDirectory(directory)) continue;
        const auto manifest = directory / L"manifest.json";
        HANDLE file = CreateFileW(manifest.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        LARGE_INTEGER size{}; BY_HANDLE_FILE_INFORMATION info{}; DWORD read = 0;
        std::string bytes;
        bool readable = file != INVALID_HANDLE_VALUE && GetFileInformationByHandle(file, &info) &&
            !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
            GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart <= 1024 * 1024;
        if (readable) {
            bytes.resize(static_cast<size_t>(size.QuadPart));
            readable = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
        }
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        json::Value value;
        if (!readable || !json::Parse(bytes, value)) {
            report.scan_complete = false;
            retain(directory, L"Dependency manifest cannot be inspected; no versions changed", ERROR_INVALID_DATA);
            return report;
        }
        int64_t schema = 0;
        const auto* schema_value = value.find("schema");
        if (!schema_value || !schema_value->get_int(schema, 1, 3)) {
            report.scan_complete = false;
            retain(directory, L"Dependency manifest schema is invalid; no versions changed", ERROR_INVALID_DATA);
            return report;
        }
        const auto* binding = value.find("host_bundle");
        if (schema == 3 || binding) {
            const auto digest = binding ? binding->get_string("manifest_sha256") : std::string();
            if (!IsDialogVersionDigest(digest)) {
                report.scan_complete = false;
                retain(directory, L"Companion dependency is invalid; no versions changed", ERROR_INVALID_DATA);
                return report;
            }
            dependencies.emplace(directory, bridge / Utf8ToWide(digest));
        }
    }
    std::sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
        const bool a_depends = dependencies.contains(a), b_depends = dependencies.contains(b);
        return a_depends != b_depends ? a_depends : a < b;
    });
    for (const auto& directory : candidates) {
        const auto digest = WideToUtf8(directory.filename().wstring());
        if (!IsDialogVersionDigest(digest)) { retain(directory, L"Unknown directory name; not installer-owned"); continue; }
        if (std::find(protected_digests.begin(), protected_digests.end(), digest) != protected_digests.end()) {
            report.protected_directories.push_back(directory.wstring());
            Log(L"Protected dialog version (current/previous): " + directory.wstring());
            continue;
        }
        // A protected, busy, damaged or unknown-member companion must keep its
        // host intact so a later retry can still verify the complete closure.
        if (std::any_of(dependencies.begin(), dependencies.end(), [&](const auto& dependency) {
            return dependency.second == directory && Exists(dependency.first);
        })) {
            retain(directory, L"Retained companion still depends on this host");
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

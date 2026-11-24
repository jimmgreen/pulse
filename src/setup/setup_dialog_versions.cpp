#include "setup_dialog_versions.h"
#include "setup_common.h"
#include "setup_log.h"
#include "../common/dialog_bundle.h"
#include "../common/json_value.h"
#include "../common/dialog_auto_repair.h"
#include <bcrypt.h>
#include <array>
#include <map>
#include <set>
#include <algorithm>
#include <filesystem>

namespace pulse::setup {
namespace {
namespace fs = std::filesystem;
constexpr wchar_t kPrevious[] = L".pulse-dialog-previous";
constexpr wchar_t kAppPointer[] = L"app-current.json";
constexpr wchar_t kAppLayout[] = L".pulse-app-layout";
constexpr wchar_t kAppPrevious[] = L".pulse-app-previous";
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
    const fs::path root(app);
    if (Exists(root / kAppPointer) || Exists(root / kAppLayout)) {
        const DWORD error = dialog_bundle::ResolveInstalledAppRuntime(app, lease);
        if (error) return {};
        const auto candidate = fs::path(lease.Directory()) / name;
        return Exists(candidate) ? candidate.wstring() : std::wstring{};
    }
    if (Exists(fs::path(app) / L"dialog-current.json")) {
        if (dialog_bundle::ResolveInstalledBundle(app, lease)) return {};
        const auto candidate = fs::path(lease.Directory()) / name;
        if (Exists(candidate)) return candidate.wstring();
    }
    return (fs::path(app) / name).wstring();
}
std::wstring UninstallRuntimeFile(const std::wstring& app, const wchar_t* name) {
    if (!name || !*name) return {};
    if (const auto verified = InstalledRuntimeFile(app, name); !verified.empty() && Exists(verified)) return verified;
    dialog_bundle::BundleLease dialog;
    if (!dialog_bundle::ResolveInstalledBundle(app, dialog)) {
        const auto candidate = fs::path(dialog.Directory()) / name;
        if (Exists(candidate)) return candidate.wstring();
    }
    const fs::path root(app);
    if (app.size() < 3 || app[1] != L':' || app[2] != L'\\' || app.find(L'/') != app.npos ||
        app.find(L':', 2) != app.npos || app.find(L'\0') != app.npos ||
        !root.is_absolute() || root.lexically_normal() != root) return {};
    auto plain_file = [](const fs::path& path) {
        const DWORD attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    };
    auto ancestor = root.root_path();
    if (!PlainDirectory(ancestor)) return {};
    for (const auto& part : root.relative_path()) {
        ancestor /= part;
        if (!PlainDirectory(ancestor)) return {};
    }
    const fs::path pointer_path = root / L"dialog-current.json";
    if (plain_file(pointer_path) && PlainDirectory(root / L"DialogBridge")) {
        std::string raw;
        json::Value value;
        if (ReadWholeFile(pointer_path.wstring(), raw) && raw.size() <= 1024 &&
            json::Parse(raw, value, nullptr, {1024, 8}) && value.is_object()) {
            const auto digest = value.get_string("manifest_sha256");
            const auto directory = value.get_string("directory");
            if (IsDialogVersionDigest(digest) && directory == "DialogBridge/" + digest) {
                const fs::path selected = root / L"DialogBridge" / Utf8ToWide(digest);
                const fs::path candidate = selected / name;
                if (PlainDirectory(selected) && plain_file(candidate)) return candidate.wstring();
            }
        }
    }
    const fs::path fallback = root / name;
    return plain_file(fallback) ? fallback.wstring() : std::wstring{};
}
bool IsDialogVersionFile(const std::wstring& relative) {
    auto rel = relative; std::replace(rel.begin(), rel.end(), L'/', L'\\');
    return rel.size() > 78 && Same(rel.substr(0, 13), L"DialogBridge\\") && rel[77] == L'\\' &&
        IsDialogVersionDigest(WideToUtf8(rel.substr(13, 64)));
}
bool IsAppRuntimeVersionFile(const std::wstring& relative) {
    auto rel = relative;
    std::replace(rel.begin(), rel.end(), L'/', L'\\');
    constexpr size_t prefix = std::size(L"AppRuntime\\") - 1;
    return rel.size() > prefix + 65 && Same(rel.substr(0, prefix), L"AppRuntime\\") &&
        rel[prefix + 64] == L'\\' && IsDialogVersionDigest(WideToUtf8(rel.substr(prefix, 64)));
}
bool IsDialogBridgeStageFile(const std::wstring& relative) {
    auto rel = relative;
    std::replace(rel.begin(), rel.end(), L'/', L'\\');
    constexpr size_t prefix = std::size(L"DialogBridgeStage\\") - 1;
    return rel.size() > prefix && Same(rel.substr(0, prefix), L"DialogBridgeStage\\");
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
std::string LowerAscii(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return value;
}
bool StreamSha256Hex(HANDLE file, uint64_t expected_size, std::string& hex_out) {
    struct Hash {
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE handle = nullptr;
        ~Hash() {
            if (handle) BCryptDestroyHash(handle);
            if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        }
    } hash;
    if (BCryptOpenAlgorithmProvider(&hash.alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(hash.alg, &hash.handle, nullptr, 0, nullptr, 0, 0) < 0) return false;
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return false;
    std::vector<unsigned char> buffer(64 * 1024);
    uint64_t total = 0;
    while (total < expected_size) {
        const DWORD want = static_cast<DWORD>(std::min<uint64_t>(buffer.size(), expected_size - total));
        DWORD got = 0;
        if (!ReadFile(file, buffer.data(), want, &got, nullptr) || got == 0) return false;
        if (BCryptHashData(hash.handle, buffer.data(), got, 0) < 0) return false;
        total += got;
    }
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(hash.handle, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return false;
    hex_out.clear();
    hex_out.reserve(64);
    for (unsigned char b : digest) {
        hex_out.push_back("0123456789abcdef"[b >> 4]);
        hex_out.push_back("0123456789abcdef"[b & 15]);
    }
    return true;
}
std::wstring DiagnoseBundleDirectory(const fs::path& directory, std::string_view expected_digest, bool allow_thin = true) {
    struct ScopedHandle {
        HANDLE value = INVALID_HANDLE_VALUE;
        ~ScopedHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    };
    const DWORD dir_attrs = GetFileAttributesW(directory.c_str());
    if (dir_attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            return L"missing version directory: " + directory.wstring();
        return L"inaccessible version directory: " + directory.wstring() + L" (" + Win32Error(err) + L")";
    }
    if ((dir_attrs & FILE_ATTRIBUTE_REPARSE_POINT) || !(dir_attrs & FILE_ATTRIBUTE_DIRECTORY))
        return L"version path is not a plain directory: " + directory.wstring();

    const fs::path manifest_path = directory / L"manifest.json";
    ScopedHandle mf{CreateFileW(manifest_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (mf.value == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            return L"missing file in " + directory.filename().wstring() + L": manifest.json";
        return L"inaccessible manifest.json in " + directory.filename().wstring() + L" (" + Win32Error(err) + L")";
    }
    BY_HANDLE_FILE_INFORMATION mf_info{};
    LARGE_INTEGER mf_size{};
    if (!GetFileInformationByHandle(mf.value, &mf_info) ||
        (mf_info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(mf.value, &mf_size) || mf_size.QuadPart < 0 || mf_size.QuadPart > 1024 * 1024) {
        return L"invalid manifest.json attributes or size in " + directory.filename().wstring();
    }
    std::string mf_bytes(static_cast<size_t>(mf_size.QuadPart), '\0');
    DWORD mf_read = 0;
    if (!mf_bytes.empty() && (!ReadFile(mf.value, mf_bytes.data(), static_cast<DWORD>(mf_bytes.size()), &mf_read, nullptr) || mf_read != mf_bytes.size()))
        return L"unreadable manifest.json in " + directory.filename().wstring();
    std::string mf_digest;
    if (!StreamSha256Hex(mf.value, static_cast<uint64_t>(mf_size.QuadPart), mf_digest))
        return L"cannot hash manifest.json in " + directory.filename().wstring();
    if (!expected_digest.empty() && mf_digest != expected_digest)
        return L"manifest.json hash mismatch in " + directory.filename().wstring();
    json::Value manifest;
    if (!json::Parse(mf_bytes, manifest, nullptr, {1024 * 1024, 16}) || !manifest.is_object())
        return L"malformed manifest.json in " + directory.filename().wstring();

    const fs::path verified_path = directory / L"VERIFIED.json";
    ScopedHandle vf{CreateFileW(verified_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (vf.value == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            return L"missing file in " + directory.filename().wstring() + L": VERIFIED.json";
        return L"inaccessible VERIFIED.json in " + directory.filename().wstring() + L" (" + Win32Error(err) + L")";
    }
    BY_HANDLE_FILE_INFORMATION vf_info{};
    LARGE_INTEGER vf_size{};
    if (!GetFileInformationByHandle(vf.value, &vf_info) ||
        (vf_info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(vf.value, &vf_size) || vf_size.QuadPart <= 0 || vf_size.QuadPart > 1024) {
        return L"invalid VERIFIED.json in " + directory.filename().wstring();
    }
    std::string vf_bytes(static_cast<size_t>(vf_size.QuadPart), '\0');
    DWORD vf_read = 0;
    json::Value verified;
    if (!ReadFile(vf.value, vf_bytes.data(), static_cast<DWORD>(vf_bytes.size()), &vf_read, nullptr) ||
        vf_read != vf_bytes.size() || !json::Parse(vf_bytes, verified, nullptr, {1024, 8}) ||
        !verified.is_object() || verified.get_string("manifest_sha256") != mf_digest) {
        return L"invalid VERIFIED.json in " + directory.filename().wstring();
    }

    const auto* entries = manifest.find("files");
    if (!entries || !entries->is_array())
        return L"invalid files list in manifest.json";
    std::set<std::string> expected{"manifest.json", "verified.json"};
    for (const auto& item : entries->items()) {
        if (!item.is_object()) return L"invalid file entry in manifest.json";
        const auto rel = item.get_string("path");
        const auto sha = item.get_string("sha256");
        const auto* size_val = item.find("size");
        int64_t expected_size = 0;
        if (rel.empty() || !size_val || !size_val->get_int(expected_size, 0, 512LL * 1024 * 1024))
            return L"invalid file metadata in manifest.json";
        expected.insert(LowerAscii(rel));
        const auto wide_rel = Utf8ToWide(rel);
        fs::path full = directory / fs::path(wide_rel);
        full.make_preferred();
        ScopedHandle fh{CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (fh.value == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                return L"missing bundle file in " + directory.filename().wstring() + L": " + wide_rel;
            return L"inaccessible bundle file in " + directory.filename().wstring() + L": " + wide_rel + L" (" + Win32Error(err) + L")";
        }
        BY_HANDLE_FILE_INFORMATION fh_info{};
        LARGE_INTEGER actual_size{};
        if (!GetFileInformationByHandle(fh.value, &fh_info) ||
            (fh_info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !GetFileSizeEx(fh.value, &actual_size) || actual_size.QuadPart < 0) {
            return L"invalid attributes on bundle file in " + directory.filename().wstring() + L": " + wide_rel;
        }
        if (actual_size.QuadPart != expected_size) {
            return L"modified bundle file in " + directory.filename().wstring() + L": " + wide_rel +
                L" (size " + std::to_wstring(actual_size.QuadPart) + L", expected " + std::to_wstring(expected_size) + L")";
        }
        std::string actual_sha;
        if (!StreamSha256Hex(fh.value, static_cast<uint64_t>(actual_size.QuadPart), actual_sha) || actual_sha != sha) {
            return L"modified bundle file (hash mismatch) in " + directory.filename().wstring() + L": " + wide_rel;
        }
    }

    std::error_code scan_ec;
    size_t count = 0;
    std::vector<std::wstring> unlisted;
    for (fs::recursive_directory_iterator it(directory, scan_ec), end; !scan_ec && it != end; it.increment(scan_ec)) {
        if (++count > 2048) return L"too many entries in " + directory.filename().wstring();
        const DWORD attr = GetFileAttributesW(it->path().c_str());
        if (attr == INVALID_FILE_ATTRIBUTES)
            return L"inaccessible entry in " + directory.filename().wstring() + L": " + it->path().filename().wstring();
        const auto rel_wide = it->path().lexically_relative(directory).generic_wstring();
        const auto rel = LowerAscii(WideToUtf8(rel_wide));
        if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
            it.disable_recursion_pending();
            if (unlisted.size() < 3) unlisted.push_back(L"reparse:" + rel_wide);
            continue;
        }
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            if (rel != "licenses" && !rel.starts_with("licenses/")) {
                it.disable_recursion_pending();
                if (unlisted.size() < 3) unlisted.push_back(rel_wide + L"/");
            }
            continue;
        }
        if (!expected.contains(rel) && unlisted.size() < 3) unlisted.push_back(rel_wide);
    }
    if (scan_ec) return L"directory scan failed in " + directory.filename().wstring();
    if (!unlisted.empty()) {
        std::wstring summary = L"unlisted entry in " + directory.filename().wstring() + L": ";
        for (size_t i = 0; i < unlisted.size(); ++i) {
            if (i) summary += L", ";
            summary += unlisted[i];
        }
        return summary;
    }

    int64_t schema = 0;
    const auto* schema_val = manifest.find("schema");
    if (allow_thin && schema_val && schema_val->get_int(schema, 3, 3)) {
        const auto* binding = manifest.find("host_bundle");
        const auto host_digest = binding ? binding->get_string("manifest_sha256") : std::string();
        if (IsDialogVersionDigest(host_digest)) {
            const auto host_detail = DiagnoseBundleDirectory(directory.parent_path() / Utf8ToWide(host_digest), host_digest, false);
            if (!host_detail.empty()) return L"thin host bundle: " + host_detail;
        }
    }
    return {};
}
bool InspectRepairablePointer(const std::wstring& app, const wchar_t* name, bool companion,
    std::wstring& detail, std::wstring& reject_reason) {
    detail.clear();
    reject_reason.clear();
    // Pin every plain ancestor and inspect the pointer handle directly before
    // treating an unverified existing pointer/bundle as repairable.
    const fs::path root(app);
    if (app.size() < 3 || app[1] != L':' || app[2] != L'\\' || app.find(L'/') != app.npos ||
        app.find(L':', 2) != app.npos || app.find(L'\0') != app.npos ||
        !root.is_absolute() || root.lexically_normal() != root) {
        reject_reason = L"non-canonical installation root";
        return false;
    }
    struct Handles {
        std::vector<HANDLE> values;
        ~Handles() { for (const auto handle : values) CloseHandle(handle); }
        bool Add(const fs::path& path, bool directory, DWORD* open_error = nullptr) {
            const HANDLE handle = CreateFileW(path.c_str(), directory ? FILE_READ_ATTRIBUTES : GENERIC_READ,
                directory ? (FILE_SHARE_READ | FILE_SHARE_WRITE) : (FILE_SHARE_READ | FILE_SHARE_DELETE), nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                if (open_error) *open_error = GetLastError();
                return false;
            }
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(handle, &info)) {
                if (open_error) *open_error = GetLastError();
                CloseHandle(handle);
                return false;
            }
            if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory) {
                if (open_error) *open_error = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? ERROR_NOT_SUPPORTED : ERROR_DIRECTORY;
                CloseHandle(handle);
                return false;
            }
            values.push_back(handle);
            return true;
        }
    } handles;
    auto ancestor = root.root_path();
    if (!handles.Add(ancestor, true)) { reject_reason = L"inaccessible or reparse ancestor"; return false; }
    for (const auto& part : root.relative_path()) {
        ancestor /= part;
        if (!handles.Add(ancestor, true)) { reject_reason = L"inaccessible or reparse ancestor"; return false; }
    }
    bool has_bridge = false;
    if (Presence(root / L"DialogBridge", has_bridge) != ERROR_SUCCESS ||
        (has_bridge && !handles.Add(root / L"DialogBridge", true))) {
        reject_reason = L"DialogBridge is inaccessible or not a plain directory";
        return false;
    }
    DWORD ptr_error = ERROR_SUCCESS;
    if (!handles.Add(root / name, false, &ptr_error)) {
        reject_reason = L"pointer inaccessible or not a plain file (" + Win32Error(ptr_error) + L")";
        return false;
    }
    const HANDLE pointer = handles.values.back();
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(pointer, &size) || size.QuadPart < 0 || size.QuadPart > 1024) {
        reject_reason = L"pointer size out of bounds";
        return false;
    }
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD count = 0;
    if (!bytes.empty() && (!ReadFile(pointer, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count != bytes.size())) {
        reject_reason = L"pointer read failed";
        return false;
    }
    if (bytes.empty()) {
        detail = L"empty pointer file";
        return true;
    }
    json::Value value;
    if (!json::Parse(bytes, value, nullptr, {1024, 8})) {
        if (bytes.find("..") != std::string::npos || bytes.find('\0') != std::string::npos) {
            reject_reason = L"unsafe malformed pointer content";
            return false;
        }
        detail = L"corrupted pointer JSON";
        return true;
    }
    if (!value.is_object()) {
        reject_reason = L"pointer JSON is not an object";
        return false;
    }
    if (value.size() == 0) {
        detail = L"empty pointer JSON object";
        return true;
    }
    const auto schema = value.find("schema");
    int64_t number = 0;
    const auto digest = value.get_string("manifest_sha256"), directory = value.get_string("directory");
    if (value.size() != 3 || !schema || !schema->get_int(number, 1, 1) ||
        !IsDialogVersionDigest(digest) || directory != "DialogBridge/" + digest) {
        reject_reason = L"invalid pointer target or schema";
        return false;
    }
    const auto selected = root / L"DialogBridge" / Utf8ToWide(digest);
    const DWORD selected_attrs = GetFileAttributesW(selected.c_str());
    if (selected_attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
            reject_reason = L"selected version directory cannot be inspected (" + Win32Error(err) + L")";
            return false;
        }
        detail = L"missing version directory: " + selected.wstring();
        return true;
    }
    if ((selected_attrs & FILE_ATTRIBUTE_REPARSE_POINT) || !(selected_attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        reject_reason = L"selected version path is not a plain directory";
        return false;
    }
    detail = DiagnoseBundleDirectory(selected, digest);
    if (detail.empty()) {
        detail = companion ? L"primary or companion version binding failed verification"
                           : L"bundle verification failed in " + selected.filename().wstring();
    }
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
    plan.version = staged.Version();
    plan.relative_directory = L"DialogBridge\\" + Utf8ToWide(plan.digest);
    staged = {};
    const fs::path target = fs::path(app) / plan.relative_directory;
    if (Exists(target)) {
        dialog_bundle::BundleLease installed;
        result = dialog_bundle::VerifyBundle(target.wstring(), plan.digest, installed);
        if (!result && installed.Machine() != plan.machine) result = ERROR_BAD_EXE_FORMAT;
        if (result) {
            const auto detail = DiagnoseBundleDirectory(target, plan.digest);
            error = L"Existing dialog version failed verification (" + plan.relative_directory +
                (detail.empty() ? L"" : L"; " + detail) + L"): " + Win32Error(result);
            return false;
        }
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
            std::wstring detail, reject_reason;
            if (!InspectRepairablePointer(app, pointer, companion, detail, reject_reason)) {
                error = L"Current dialog bundle failed verification (" + std::wstring(pointer) +
                    (reject_reason.empty() ? L"" : L"; " + reject_reason) + L"): " + Win32Error(result);
                return false;
            }
            // Keep old pointer/markers in the normal root transaction. An
            // unverified bundle must never become the trusted previous version,
            // and cleanup retains unverified directories without deleting files.
            Log(L"Repairing unverified dialog pointer " + std::wstring(pointer) + L" (" + Win32Error(result) +
                (detail.empty() ? L"" : L"; " + detail) + L"); replacement staged bundle fully verified.");
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

bool PreviousDialogMirrorsApp(const fs::path& app, const dialog_bundle::BundleLease& core) {
    dialog_bundle::BundleLease dialog;
    if (dialog_bundle::ResolveInstalledBundle(app.wstring(), dialog) ||
        dialog.Version() != core.Version() || dialog.Machine() != core.Machine()) return false;
    // The prior bridge must still be a self-contained fallback for its own
    // clients. Do not retire a previous app which is already sharing its data.
    if (!Same(fs::path(dialog.Host()).parent_path().wstring(), dialog.Directory())) return false;
    std::error_code ec;
    if (fs::equivalent(core.Host(), dialog.Host(), ec) || ec) return false;

    auto read_manifest = [](const dialog_bundle::BundleLease& lease, json::Value& manifest) {
        std::string raw;
        return ReadWholeFile((fs::path(lease.Directory()) / L"manifest.json").wstring(), raw) &&
            raw.size() <= (1u << 20) && json::Parse(raw, manifest, nullptr, {1u << 20, 8});
    };
    json::Value app_manifest, dialog_manifest;
    if (!read_manifest(core, app_manifest) || !read_manifest(dialog, dialog_manifest)) return false;
    const auto* app_files = app_manifest.find("files");
    const auto* dialog_files = dialog_manifest.find("files");
    if (!app_files || !dialog_files || !app_files->is_array() || !dialog_files->is_array()) return false;
    std::map<std::string, std::string> available;
    for (const auto& item : dialog_files->items())
        available.emplace(LowerAscii(item.get_string("path")), item.get_string("sha256"));
    return !app_files->items().empty() &&
        std::all_of(app_files->items().begin(), app_files->items().end(), [&](const auto& item) {
            const auto slot = available.find(LowerAscii(item.get_string("path")));
            return slot != available.end() && slot->second == item.get_string("sha256");
        });
}

bool PrepareAppRuntime(const fs::path& app, const fs::path& staging, std::vector<std::wstring>& files,
                       DialogVersionPlan& plan, std::wstring& error) {
    const auto listed = [&](const auto* name) {
        return std::any_of(files.begin(), files.end(), [&](const auto& file) { return Same(file, name); });
    };
    bool pointer = false, marker = false, installed_marker = false, installed_pointer = false;
    if (Presence(staging / kAppPointer, pointer) || Presence(staging / kAppLayout, marker) ||
        Presence(app / kAppLayout, installed_marker) || Presence(app / kAppPointer, installed_pointer) ||
        listed(kAppPointer) != pointer || listed(kAppLayout) != marker || pointer != marker ||
        ((installed_marker || installed_pointer) && !pointer)) {
        error = L"Application runtime pointer/layout mismatch.";
        return false;
    }
    const auto family = app / dialog_bundle::kAppRuntimeDirectory;
    if (!pointer) return true;
    const auto attrs = GetFileAttributesW((staging / kAppLayout).c_str());
    std::string layout;
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !ReadWholeFile((staging / kAppLayout).wstring(), layout) || layout != "1\n" ||
        (Exists(family) && !PlainDirectory(family))) {
        error = L"Invalid application runtime layout.";
        return false;
    }
    dialog_bundle::BundleLease staged;
    DWORD result = dialog_bundle::ResolveInstalledAppRuntime(staging.wstring(), staged);
    if (!result && plan.versioned && (staged.Machine() != plan.machine || staged.Version() != plan.version))
        result = ERROR_REVISION_MISMATCH;
    if (result) {
        error = L"Invalid staged application runtime: " + Win32Error(result);
        return false;
    }
    auto& core = plan.app_runtime;
    core.versioned = true;
    core.digest = staged.ManifestDigest();
    core.machine = staged.Machine();
    core.relative_directory = L"AppRuntime\\" + Utf8ToWide(core.digest);
    std::error_code link_error;
    const bool staged_shared = plan.versioned && fs::equivalent(staged.Host(), staging / plan.relative_directory / L"pulse.exe", link_error);
    staged = {};
    const auto target = app / core.relative_directory;
    if (Exists(target)) {
        dialog_bundle::BundleLease existing;
        result = dialog_bundle::VerifyAppRuntime(target.wstring(), core.digest, existing);
        if (result) {
            error = L"Existing application runtime failed verification: " + Win32Error(result);
            return false;
        }
    }
    std::string previous;
    if (installed_pointer) {
        dialog_bundle::BundleLease current;
        if (!dialog_bundle::ResolveInstalledAppRuntime(app.wstring(), current)) {
            if (current.ManifestDigest() != core.digest) {
                if (staged_shared && !link_error && PreviousDialogMirrorsApp(app, current)) {
                    // The old duplicate is still available until transaction
                    // commit. Afterwards normal cleanup may remove it if idle.
                    Log(L"Retiring copied previous application runtime; the verified previous dialog still has identical files.");
                } else previous = current.ManifestDigest();
            }
            else {
                bool present = false;
                if (Presence(app / kAppPrevious, present) || (present && !ReadDigest(app / kAppPrevious, previous))) {
                    error = L"Invalid previous application runtime marker.";
                    return false;
                }
            }
        } else {
            Log(L"Replacing an unverified app pointer with the verified staged runtime.");
        }
    }
    // Keep the local marker tracked by the new file list even when a damaged
    // former current package could not be recorded as a trusted previous one.
    if (previous.empty() && Exists(app / kAppPrevious)) previous = core.digest;
    if (!previous.empty()) {
        if (!WriteWholeFile((staging / kAppPrevious).wstring(), previous.data(), previous.size())) {
            error = L"Cannot stage previous application runtime marker.";
            return false;
        }
        files.emplace_back(kAppPrevious);
    }
    return true;
}
}
bool PrepareDialogVersion(const std::wstring& app, const std::wstring& staging,
    std::vector<std::wstring>& files, DialogVersionPlan& plan, std::wstring& error) {
    plan = {};
    const bool pointer = std::any_of(files.begin(), files.end(), [](const auto& file) { return Same(file, L"dialog-current.json"); });
    const bool companion = std::any_of(files.begin(), files.end(), [](const auto& file) { return Same(file, kX86Pointer); });
    const bool app_pointer = std::any_of(files.begin(), files.end(), [](const auto& file) { return Same(file, kAppPointer); });
    bool physical_companion = false;
    const DWORD presence = Presence(fs::path(staging) / kX86Pointer, physical_companion);
    if (presence || companion != physical_companion) { error = L"Unlisted or inaccessible x86 dialog pointer."; return false; }
    bool installed_companion = false;
    const DWORD installed_presence = Presence(fs::path(app) / kX86Pointer, installed_companion);
    if (installed_presence || (installed_companion && !companion && !(!pointer && app_pointer))) {
        error = L"An installation with an x86 dialog companion requires a dual-architecture update.";
        return false;
    }
    if (!pointer) {
        if (companion) { error = L"An x86 companion requires the primary dialog pointer."; return false; }
        if (std::any_of(files.begin(), files.end(), [](const auto& file) { return Same(file, kAppPrevious); })) {
            error = L"Payload tries to supply a local previous application marker.";
            return false;
        }
        if (!PrepareAppRuntime(app, staging, files, plan, error)) return false;
        if (!plan.app_runtime.versioned) {
            if (std::any_of(files.begin(), files.end(), [](const auto& file) {
                return IsAppRuntimeVersionFile(file) || Same(file, kAppPrevious);
            })) { error = L"Unmanaged application runtime entry."; return false; }
            plan.replacements.clear();
            for (const auto& file : files) {
                if (IsDialogBridgeStageFile(file)) continue;
                plan.replacements.push_back(file);
            }
            return true;
        }
        plan.replacements.clear();
        for (const auto& file : files) {
            if (Same(file, kAppPointer) || Same(file, kAppLayout) || IsDialogBridgeStageFile(file)) continue;
            auto normalized = file; std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
            constexpr size_t app_prefix = std::size(L"AppRuntime\\") - 1;
            if (normalized.size() >= app_prefix && Same(normalized.substr(0, app_prefix), L"AppRuntime\\")) {
                if (!IsAppRuntimeVersionFile(file) ||
                    normalized.substr(0, app_prefix + 64) != plan.app_runtime.relative_directory) {
                    error = L"Unexpected application runtime entry in payload.";
                    return false;
                }
                continue;
            }
            if (normalized.size() >= 13 && Same(normalized.substr(0, 13), L"DialogBridge\\")) {
                error = L"Unmanaged DialogBridge entry in application runtime payload.";
                return false;
            }
            plan.replacements.push_back(file);
        }
        plan.replacements.insert(plan.replacements.begin(), kAppLayout);
        plan.replacements.emplace_back(kAppPointer);
        return true;
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
    if (std::any_of(files.begin(), files.end(), [](const auto& file) { return Same(file, kAppPrevious); })) {
        error = L"Payload tries to supply a local previous application marker.";
        return false;
    }
    if (!PrepareAppRuntime(app, staging, files, plan, error)) return false;
    plan.replacements.clear();
    for (const auto& file : files) {
        if (Same(file, L"dialog-current.json") || Same(file, kX86Pointer) ||
            Same(file, kAppPointer) || Same(file, kAppLayout) || IsDialogBridgeStageFile(file)) continue;
        auto normalized = file; std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
        constexpr size_t app_prefix = std::size(L"AppRuntime\\") - 1;
        if (normalized.size() >= app_prefix && Same(normalized.substr(0, app_prefix), L"AppRuntime\\")) {
            if (!plan.app_runtime.versioned || !IsAppRuntimeVersionFile(file) ||
                normalized.substr(0, app_prefix + 64) != plan.app_runtime.relative_directory) {
                error = L"Unexpected application runtime entry in payload.";
                return false;
            }
            continue;
        }
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
    plan.replacements.emplace_back(L"dialog-current.json");
    if (plan.app_runtime.versioned) {
        plan.replacements.insert(plan.replacements.begin(), kAppLayout);
        plan.replacements.emplace_back(kAppPointer);
    }
    return true;
}
static bool PublishSingleVersion(const std::wstring& app, const std::wstring& staging,
    const std::wstring& relative, const std::string& digest, bool app_runtime, std::wstring& error) {
    const fs::path source = fs::path(staging) / relative;
    const fs::path target = fs::path(app) / relative;
    const auto bridge = target.parent_path();
    if (!Exists(bridge) && !CreateDirectoryW(bridge.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = L"Cannot create version directory."; return false;
    }
    if (!PlainDirectory(bridge)) { error = L"Version root is not a plain directory."; return false; }
    if (Exists(target)) {
        dialog_bundle::BundleLease existing;
        const DWORD result = app_runtime ? dialog_bundle::VerifyAppRuntime(target.wstring(), digest, existing) :
            dialog_bundle::VerifyBundle(target.wstring(), digest, existing);
        if (result) { error = L"Cannot reuse version: " + Win32Error(result); return false; }
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
            if (attempt) Log(L"Published version after waiting " + std::to_wstring(GetTickCount64() - started) + L" ms");
            return true;
        }
        const DWORD failure = GetLastError();
        const ULONGLONG elapsed = GetTickCount64() - started;
        if (!attempt) Log(L"Version publication failed from " + source.wstring() + L" to " + target.wstring() + L": " + Win32Error(failure));
        if ((failure != ERROR_ACCESS_DENIED && failure != ERROR_SHARING_VIOLATION && failure != ERROR_LOCK_VIOLATION) ||
            elapsed >= retry_budget_ms) {
            error = L"Cannot publish version: " + Win32Error(failure) +
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
        if (!plan.app_runtime.versioned) return true;
    }
    std::vector<const DialogVersionPlan*> versions;
    if (plan.versioned) {
        if (plan.companions.size() > 1 || (!plan.companions.empty() &&
            (plan.machine != IMAGE_FILE_MACHINE_AMD64 || !plan.companions.front().versioned ||
             plan.companions.front().machine != IMAGE_FILE_MACHINE_I386 || !plan.companions.front().companions.empty()))) {
            error = L"Invalid dialog companion plan."; return false;
        }
        versions.push_back(&plan);
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
    }
    const auto& core = plan.app_runtime;
    if (core.versioned) {
        dialog_bundle::BundleLease staged;
        DWORD result = dialog_bundle::ResolveInstalledAppRuntime(staging, staged);
        if (!result && (staged.ManifestDigest() != core.digest || staged.Machine() != core.machine ||
            core.relative_directory != L"AppRuntime\\" + Utf8ToWide(core.digest))) result = ERROR_FILE_INVALID;
        if (result) { error = L"Staged application runtime changed: " + Win32Error(result); return false; }
    }
    for (const auto* version : versions)
        if (!PublishSingleVersion(app, staging, version->relative_directory, version->digest, false, error)) return false;
    if (core.versioned) {
        if (!PublishSingleVersion(app, staging, core.relative_directory, core.digest, true, error)) return false;
        dialog_bundle::BundleLease published;
        const DWORD result = dialog_bundle::VerifyAppRuntime((fs::path(app) / core.relative_directory).wstring(),
                                                            core.digest, published);
        if (result) { error = L"Published application runtime failed verification: " + Win32Error(result); return false; }
    }
    return true;
}
static DialogCleanupReport CleanupVersionsReport(const std::wstring& app, bool uninstall, bool app_runtime,
                                                 bool identify_owners) {
    DialogCleanupReport report;
    const fs::path bridge = fs::path(app) / (app_runtime ? dialog_bundle::kAppRuntimeDirectory : L"DialogBridge");
    auto retain = [&](const fs::path& directory, const std::wstring& reason, DWORD error = ERROR_SUCCESS, bool blocked = false) {
        (blocked ? report.blocked_directories : report.retained_directories).push_back({directory.wstring(), reason, error});
        Log(L"Retained version: " + directory.wstring() + L"; " + reason +
            (error ? L"; Win32: " + std::to_wstring(error) : L""));
    };
    auto busy = [](DWORD error) {
        return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_ACCESS_DENIED;
    };
    std::vector<std::wstring> blocked_files;
    auto push_blocked = [&](const fs::path& file) {
        const auto relative = file.lexically_relative(app).wstring();
        if (!relative.empty() &&
            std::find(blocked_files.begin(), blocked_files.end(), relative) == blocked_files.end()) {
            blocked_files.push_back(relative);
        }
    };
    auto is_probe_binary = [](const fs::path& file) {
        const auto name = file.filename().wstring();
        return EqualsNoCase(name, L"PulseDialogAuto64.dll") ||
               EqualsNoCase(name, L"PulseDialogAuto32.dll") ||
               EqualsNoCase(name, L"PulseDialogCore64.dll") ||
               EqualsNoCase(name, L"PulseDialogCore32.dll");
    };
    auto identify_files = [&](const std::vector<fs::path>& candidates_to_probe, DWORD failure) {
        for (const auto& file : candidates_to_probe) push_blocked(file);
        report.locks.error = failure;
    };
    // Keep Restart Manager input minimal (1-2 files per blocked bundle instead of every
    // license/text file): RM costs ~0.15-0.2s per registered path.
    auto identify_representative = [&](const std::vector<fs::path>& all_files, const fs::path* failed_file, DWORD failure) {
        std::vector<fs::path> probe;
        if (failed_file) {
            probe.push_back(*failed_file);
            if (!app_runtime) {
                for (const auto& file : all_files) {
                    if (is_probe_binary(file)) { probe.push_back(file); break; }
                }
            }
        } else {
            for (const auto& file : all_files) {
                const auto name = file.filename().wstring();
                if (is_probe_binary(file) || EqualsNoCase(name, L"pulse.exe") || EqualsNoCase(name, L"manifest.json")) {
                    probe.push_back(file);
                }
            }
            if (probe.empty() && !all_files.empty()) probe.push_back(all_files.front());
        }
        identify_files(probe, failure);
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
    bool retired_legacy_bridge = false;
    if (!uninstall) {
        bool has_dialog_pointer = true;
        if (!app_runtime && Presence(fs::path(app) / L"dialog-current.json", has_dialog_pointer) == ERROR_SUCCESS &&
            !has_dialog_pointer) {
            dialog_bundle::BundleLease active_app;
            if (dialog_bundle::ResolveInstalledAppRuntime(app, active_app) == ERROR_SUCCESS) {
                // Migrated AppRuntime-only install: legacy DialogBridge is retired in
                // favor of Common Files Stub+Core; only retain versions still named by
                // an active per-user COM registration until that user rebinds.
                retired_legacy_bridge = true;
            }
        }
        if (!retired_legacy_bridge) {
            dialog_bundle::BundleLease current;
            const DWORD failure = app_runtime ? dialog_bundle::ResolveInstalledAppRuntime(app, current) :
                dialog_bundle::ResolveInstalledBundle(app, current);
            if (failure) {
                report.scan_complete = false;
                retain(bridge, L"Current version cannot be verified; all versions protected", failure);
                return report;
            }
            protected_digests.push_back(current.ManifestDigest());
            bool companion = false;
            DWORD error = app_runtime ? ERROR_SUCCESS : Presence(fs::path(app) / kX86Pointer, companion);
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
            const std::vector<const wchar_t*> markers = app_runtime ? std::vector<const wchar_t*>{kAppPrevious} :
                std::vector<const wchar_t*>{kPrevious, kX86Previous};
            for (const auto* marker : markers) {
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
        // An enabled per-user COM registration loads its DLL from a fixed
        // version; deleting that version breaks every non-injected dialog.
        if (!app_runtime)
            for (const auto& registered : dialog_auto::RegisteredBundleDigests(app)) {
                Log(L"Protected version (registered dialog entry): " + registered);
                protected_digests.push_back(WideToUtf8(registered));
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
        if (app_runtime) break;
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
            Log(L"Protected version (current/previous): " + directory.wstring());
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
        DWORD failure = app_runtime ? dialog_bundle::VerifyAppRuntime(directory.wstring(), digest, lease) :
            dialog_bundle::VerifyBundle(directory.wstring(), digest, lease);
        if (failure) {
            if (busy(failure)) identify_representative(files, nullptr, failure);
            retain(directory, busy(failure) ? L"Version verification blocked; close owners and retry" :
                L"Unknown or damaged bundle; ownership verification failed", failure, busy(failure));
            continue;
        }
        std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) { return a.native().size() > b.native().size(); });
        dirs.push_back(directory);
        std::vector<fs::path> owned = files; owned.insert(owned.end(), dirs.begin(), dirs.end());
        std::vector<BY_HANDLE_FILE_INFORMATION> identities(owned.size());
        bool ready = true;
        size_t failed_index = owned.size();
        for (size_t i = 0; i < owned.size(); ++i) if (!Identity(owned[i], identities[i])) { failure = GetLastError(); failed_index = i; ready = false; break; }
        if (ready) { failure = dialog_bundle::RecheckDirectory(lease); ready = failure == ERROR_SUCCESS; }
        if (!ready) {
            lease = {};
            if (busy(failure)) {
                if (failed_index < files.size()) identify_representative(files, &files[failed_index], failure);
                else if (failed_index < owned.size()) report.locks.error = failure;
                else identify_representative(files, nullptr, failure);
            }
            retain(directory, L"Version changed or is blocked before cleanup; no members deleted", failure, busy(failure));
            continue;
        }
        lease = {};
        std::vector<HANDLE> locks;
        failed_index = owned.size();
        for (size_t i = 0; i < owned.size(); ++i) {
            HANDLE h = CreateFileW(owned[i].c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (h == INVALID_HANDLE_VALUE) { failure = GetLastError(); failed_index = i; ready = false; break; }
            locks.push_back(h);
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(h, &info)) { failure = GetLastError(); failed_index = i; ready = false; break; }
            if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !SameIdentity(identities[i], info)) {
                failure = ERROR_FILE_INVALID; failed_index = i; ready = false; break;
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
                    failure = GetLastError(); failed_index = marked; ready = false; break;
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
            if (busy(failure)) {
                if (failed_index < files.size()) identify_representative(files, &files[failed_index], failure);
                else if (failed_index < owned.size()) report.locks.error = failure;
                else identify_representative(files, nullptr, failure);
            }
            retain(directory, busy(failure) ? L"Deletion blocked; close owners and retry" : L"Deletion failed; tree retained for review", failure, busy(failure));
            continue;
        }
        report.deleted_directories.push_back(directory.wstring());
        Log(L"Deleted retired version: " + directory.wstring());
    }
    if (!app_runtime && (uninstall || retired_legacy_bridge) && report.Complete()) {
        RemoveDirectoryW(bridge.c_str());
    }
    if (!report.blocked_directories.empty() && identify_owners) {
        const DWORD failure = report.locks.error;
        if (!blocked_files.empty()) report.locks = InspectReplacementLocks(app, blocked_files, true);
        if (report.locks.files.empty() && !report.locks.error) report.locks.error = failure;
        Log(L"Dialog cleanup blockers:\n" + DescribeReplacementLocks(report.locks));
    }
    return report;
}
DialogCleanupReport CleanupDialogVersionsReport(const std::wstring& app, bool uninstall, bool identify_owners) {
    return CleanupVersionsReport(app, uninstall, false, identify_owners);
}
DialogCleanupReport CleanupAppRuntimeVersionsReport(const std::wstring& app, bool uninstall, bool identify_owners) {
    return CleanupVersionsReport(app, uninstall, true, identify_owners);
}
bool CleanupDialogVersions(const std::wstring& app, bool uninstall) {
    return CleanupDialogVersionsReport(app, uninstall, false).Complete();
}
VersionLock::~VersionLock() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
}
DWORD VersionLock::Acquire(const std::wstring& app, DWORD wait_ms) {
    if (handle_ != INVALID_HANDLE_VALUE) return ERROR_SUCCESS;
    const auto path = (fs::path(app) / L".pulse-versions.lock").wstring();
    const ULONGLONG deadline = GetTickCount64() + wait_ms;
    for (;;) {
        handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (handle_ != INVALID_HANDLE_VALUE) return ERROR_SUCCESS;
        const DWORD failure = GetLastError();
        // A holder that just closed leaves the file delete-pending briefly.
        if (failure != ERROR_SHARING_VIOLATION && failure != ERROR_ACCESS_DENIED) return failure;
        if (GetTickCount64() >= deadline) return ERROR_SHARING_VIOLATION;
        Sleep(100);
    }
}
int RunDeferredVersionCleanup(const std::wstring& app) {
    VersionLock lock;
    if (const DWORD locked = lock.Acquire(app, 0)) {
        Log(L"Deferred version cleanup skipped: version lock unavailable; Win32: " + std::to_wstring(locked));
        return kExitCleanupBusy;
    }
    // An interrupted install must be recovered by setup before anything is removed.
    if (Exists(fs::path(app) / L".pulse-setup")) {
        Log(L"Deferred version cleanup skipped: setup work directory present");
        return kExitCleanupBusy;
    }
    bool complete = true;
    for (const bool app_runtime : {false, true}) {
        const auto report = CleanupVersionsReport(app, false, app_runtime, false);
        Log(std::wstring(app_runtime ? L"Application runtime" : L"Dialog") + L" deferred cleanup: removed=" +
            std::to_wstring(report.deleted_directories.size()) + L" protected=" +
            std::to_wstring(report.protected_directories.size()) + L" blocked=" +
            std::to_wstring(report.blocked_directories.size()) + L" retained=" +
            std::to_wstring(report.retained_directories.size()));
        complete = complete && report.Complete();
    }
    return complete ? kExitOk : kExitCleanupRetained;
}
}

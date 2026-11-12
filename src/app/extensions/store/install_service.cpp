#include "install_service.h"
#include "../package_revision.h"
#include "../package_signature.h"
#include "../package_store.h"
#include "../../../common/json_value.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace pulse::extensions::store {
namespace {
namespace fs = std::filesystem;
using json::Value;
using Version = std::array<uint64_t, 3>;
bool ParseVersion(std::string_view text, Version& out, bool allow_leading_zero = false) {
    size_t pos = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        const size_t start = pos;
        uint64_t value = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            const auto digit = static_cast<uint64_t>(text[pos++] - '0');
            if (value > ((std::numeric_limits<uint64_t>::max)() - digit) / 10) return false;
            value = value * 10 + digit;
        }
        if (start == pos || (!allow_leading_zero && pos - start > 1 && text[start] == '0')) return false;
        out[i] = value;
        if (i + 1 < out.size() && (pos == text.size() || text[pos++] != '.')) return false;
    }
    return pos == text.size();
}
const char* ReleaseCompatibilityError(const Release& release, std::string_view host_version, int sdk_preview) {
    Version host{}, minimum{};
    if (!ParseVersion(host_version, host)) return "install_host_version_invalid";
    // Catalog v1 minPulse permits zero-padded numeric components.
    if (!ParseVersion(release.min_pulse, minimum, true)) return "install_min_version_invalid";
    if (host < minimum) return "install_host_too_old";
    if (sdk_preview <= 0 || release.sdk_preview != sdk_preview) return "install_sdk_incompatible";
    return nullptr;
}
struct LocalHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    LocalHandle() = default;
    LocalHandle(const LocalHandle&) = delete;
    LocalHandle& operator=(const LocalHandle&) = delete;
    LocalHandle(LocalHandle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    LocalHandle& operator=(LocalHandle&& other) noexcept {
        if (this != &other) {
            if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
            value = std::exchange(other.value, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    ~LocalHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
[[noreturn]] void LocalFail(const std::string& text) { throw std::runtime_error(text); }
[[noreturn]] void LocalIo(const char* op) { LocalFail(std::string(op) + ":" + std::to_string(GetLastError())); }
void LocalCheckCancel(const std::atomic<bool>& cancelled) { if (cancelled.load()) LocalFail("install_cancelled"); }
bool LocalExists(const fs::path& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) return true;
    const DWORD code = GetLastError();
    if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
        SetLastError(code);
        LocalIo("store_stat");
    }
    return false;
}
std::unique_ptr<LocalHandle> LocalOpen(const fs::path& path, bool directory, DWORD access = FILE_READ_ATTRIBUTES,
    DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE) {
    auto handle = std::make_unique<LocalHandle>();
    handle->value = CreateFileW(path.c_str(), access, sharing, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr);
    if (handle->value == INVALID_HANDLE_VALUE) LocalIo("store_open");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle->value, &info)) LocalIo("store_file_info");
    if (GetFileType(handle->value) != FILE_TYPE_DISK || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        (!!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory)) {
        LocalFail("store_requires_ordinary_path");
    }
    return handle;
}
std::string LocalRead(const fs::path& path, size_t limit, std::vector<std::unique_ptr<LocalHandle>>* pins = nullptr) {
    auto file = LocalOpen(path, false, GENERIC_READ, FILE_SHARE_READ);
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file->value, &length)) LocalIo("store_size");
    if (length.QuadPart < 0 || static_cast<uint64_t>(length.QuadPart) > limit) LocalFail("store_size_limit");
    std::string bytes(static_cast<size_t>(length.QuadPart), '\0');
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD count = 0;
        const auto wanted = static_cast<DWORD>((std::min)(bytes.size() - offset, size_t{65536}));
        if (!ReadFile(file->value, bytes.data() + offset, wanted, &count, nullptr) || count != wanted)
            LocalIo("store_read");
        offset += count;
    }
    if (pins) pins->push_back(std::move(file));
    return bytes;
}
bool LocalFields(const Value& v, std::initializer_list<std::string_view> allowed) {
    if (!v.is_object()) return false;
    for (const auto& key : v.keys()) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) return false;
    }
    return true;
}
bool LocalHash(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
struct UnsignedStoreSession {
    fs::path root;
    std::vector<std::unique_ptr<LocalHandle>> pins;
    std::unique_ptr<LocalHandle> lock;
    bool root_exists = false;
    explicit UnsignedStoreSession(const fs::path& path, bool create_root, bool allow_missing) : root(path) {
        if (!root.is_absolute() || root.root_name().native().size() != 2 ||
            root.native().find(L'\0') != fs::path::string_type::npos) {
            LocalFail("store_requires_absolute_local_path");
        }
        for (const auto& component : root) {
            const auto& name = component.native();
            if (name == L"." || name == L"..") LocalFail("store_path_alias");
            if (!name.empty() && (name.back() == L'.' || name.back() == L' ')) LocalFail("store_path_alias");
        }
        root = root.lexically_normal();
        if (root == root.root_path()) LocalFail("store_volume_root_forbidden");
        const auto type = GetDriveTypeW(root.root_path().c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_RAMDISK)
            LocalFail("store_requires_local_volume");
        std::vector<fs::path> ancestors;
        for (auto parent = root;; parent = parent.parent_path()) {
            ancestors.push_back(parent);
            if (parent == parent.root_path()) break;
        }
        for (auto parent = ancestors.rbegin(); parent != ancestors.rend(); ++parent) {
            if (!LocalExists(*parent)) {
                if (!create_root) {
                    if (allow_missing) return;
                    LocalFail("store_root_missing");
                }
                if (!CreateDirectoryW(parent->c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                    LocalIo("store_create_root");
            }
            pins.push_back(LocalOpen(*parent, true));
        }
        root_exists = true;
        const auto lock_path = root / L".registry.lock";
        LocalHandle created;
        created.value = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (created.value == INVALID_HANDLE_VALUE) LocalIo("store_lock_open");
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(created.value, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
            info.nNumberOfLinks != 1 || GetFileType(created.value) != FILE_TYPE_DISK) {
            LocalFail("store_lock_not_regular");
        }
        OVERLAPPED overlap{};
        if (!LockFileEx(created.value, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap))
            LocalFail("store_busy");
        lock = std::make_unique<LocalHandle>();
        lock->value = std::exchange(created.value, INVALID_HANDLE_VALUE);
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(lock->value, &size)) LocalIo("store_lock_size");
        if (size.QuadPart == 0) {
            DWORD written = 0;
            const char zero = 0;
            if (!WriteFile(lock->value, &zero, 1, &written, nullptr) || written != 1)
                LocalIo("store_lock_initialize");
        }
    }
    void EnsureDirectory(const fs::path& path) {
        if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            LocalIo("store_create_directory");
        pins.push_back(LocalOpen(path, true));
    }
};
Value ReadStoreRegistry(UnsignedStoreSession& session) {
    Value registry;
    if (!session.root_exists || !LocalExists(session.root / L"registry.json")) {
        registry = Value::Object();
        registry.set("formatVersion", 1);
        registry.set("packages", Value::Array());
        return registry;
    }
    std::string error;
    if (!json::Parse(LocalRead(session.root / L"registry.json", kMaxStoreRegistryBytes), registry, &error,
            {kMaxStoreRegistryBytes, 4}) ||
        !LocalFields(registry, {"formatVersion", "packages"}) ||
        !registry.find("formatVersion") || !registry.find("packages") || !registry.find("packages")->is_array()) {
        LocalFail("invalid_store_registry");
    }
    int64_t version = 0;
    if (!registry.find("formatVersion")->get_int(version, 1, 1) ||
        registry.find("packages")->size() > kMaxStorePackages) {
        LocalFail("invalid_store_registry");
    }
    std::set<std::string> ids;
    for (const auto& item : registry.find("packages")->items()) {
        if (!LocalFields(item, {"id", "name", "enabled", "activeRevision", "previousRevision"}) ||
            !item.find("enabled") || !item.find("enabled")->is_bool() ||
            !LocalHash(item.get_string("activeRevision")) ||
            (item.find("previousRevision") && !LocalHash(item.get_string("previousRevision")))) {
            LocalFail("invalid_store_entry");
        }
        auto identity = Value::Object();
        identity.set("apiVersion", 1);
        identity.set("id", item.get_string("id"));
        identity.set("name", item.get_string("name"));
        if (!ParseManifest(json::Serialize(identity)) || !ids.insert(item.get_string("id")).second)
            LocalFail("invalid_store_identity");
    }
    return registry;
}
bool InspectUnsignedPolicyRequiresSigned(UnsignedStoreSession& session, const VerifiedPackage& package) {
    if (!session.root_exists) return false;
    const auto policy_path = session.root / L"signature-policy.json";
    if (!LocalExists(policy_path)) return false;
    const auto policy_bytes = LocalRead(policy_path, kMaxStoreRegistryBytes, &session.pins);
    if (policy_bytes.empty()) LocalFail("invalid_signature_policy");
    std::string sig_error;
    if (!VerifyPackageSignature(package.ManifestBytes(), {}, policy_bytes, sig_error)) {
        if (sig_error == "signature_required") return true;
        LocalFail("store_publisher_policy:" + sig_error);
    }
    return false;
}
bool FileDefinitelyAbsent(const fs::path& path) {
    LocalHandle probe;
    probe.value = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (probe.value != INVALID_HANDLE_VALUE) return false;
    const DWORD code = GetLastError();
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}
InstallReview BuildUnsignedReview(UnsignedStoreSession& session, const VerifiedPackage& package, const Value& registry) {
    const bool policy_requires_signed = InspectUnsignedPolicyRequiresSigned(session, package);
    InstallReview review;
    review.root = session.root.native();
    review.id = package.Declaration().id;
    review.revision = package.Revision();
    review.publisher_id.clear();
    review.native_code = package.Declaration().native.has_value();
    review.added_resources = package.Declaration().resources;
    const Value* existing = nullptr;
    for (const auto& item : registry.find("packages")->items()) {
        if (item.get_string("id") == review.id) { existing = &item; break; }
    }
    if (existing) {
        review.active_revision = existing->get_string("activeRevision");
        review.currently_enabled = existing->get_bool("enabled");
        session.pins.push_back(LocalOpen(session.root / L"versions", true));
        const auto old_bytes = LocalRead(session.root / L"versions" / (review.active_revision + ".json"),
            kMaxManifestBytes, &session.pins);
        const auto old = ParseManifest(old_bytes);
        if (!old || PackageRevision(old_bytes) != review.active_revision ||
            old.manifest->id != review.id || old.manifest->name != existing->get_string("name")) {
            LocalFail("invalid_active_revision");
        }
        // Never allow an unsigned local package to overwrite or downgrade an installed signed package.
        if (!FileDefinitelyAbsent(session.root / L"versions" / (review.active_revision + ".sig.json")) ||
            !FileDefinitelyAbsent(session.root / L"versions" / (review.active_revision + ".rotation.json"))) {
            LocalFail("installed_publisher_mismatch");
        }
        if (policy_requires_signed) LocalFail("signature_required");
        std::erase_if(review.added_resources, [&](const auto& res) {
            return std::find(old.manifest->resources.begin(), old.manifest->resources.end(), res) !=
                old.manifest->resources.end();
        });
    } else {
        if (policy_requires_signed) LocalFail("signature_required");
        if (registry.find("packages")->size() >= kMaxStorePackages) LocalFail("store_full");
    }
    std::sort(review.added_resources.begin(), review.added_resources.end());
    return review;
}
struct LocalTempFile {
    fs::path path;
    std::unique_ptr<LocalHandle> handle;
    bool committed = false;
    ~LocalTempFile() {
        if (!committed && handle && handle->value != INVALID_HANDLE_VALUE) {
            FILE_DISPOSITION_INFO info{TRUE};
            SetFileInformationByHandle(handle->value, FileDispositionInfo, &info, sizeof(info));
        }
    }
};
std::unique_ptr<LocalTempFile> WriteLocalTemp(const fs::path& parent, std::string_view bytes,
    const std::atomic<bool>& cancelled) {
    static std::atomic<uint64_t> serial{0};
    auto file = std::make_unique<LocalTempFile>();
    file->handle = std::make_unique<LocalHandle>();
    for (int attempt = 0; attempt < 32; ++attempt) {
        LocalCheckCancel(cancelled);
        file->path = parent / (L".local-install-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(serial.fetch_add(1)));
        file->handle->value = CreateFileW(file->path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file->handle->value != INVALID_HANDLE_VALUE) break;
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) {
            SetLastError(code);
            LocalIo("install_create_temporary");
        }
    }
    if (file->handle->value == INVALID_HANDLE_VALUE) LocalFail("install_temporary_collision");
    size_t offset = 0;
    while (offset < bytes.size()) {
        LocalCheckCancel(cancelled);
        DWORD count = 0;
        const auto wanted = static_cast<DWORD>((std::min)(bytes.size() - offset, size_t{65536}));
        if (!WriteFile(file->handle->value, bytes.data() + offset, wanted, &count, nullptr) || count != wanted)
            LocalIo("install_write");
        offset += count;
    }
    if (!FlushFileBuffers(file->handle->value)) LocalIo("install_flush");
    return file;
}
void RenameLocalHandle(HANDLE handle, const fs::path& target, bool replace, const std::atomic<bool>* cancelled = nullptr) {
    const auto& name = target.native();
    const auto bytes = name.size() * sizeof(wchar_t);
    if (bytes > 65500) LocalFail("install_path_limit");
    std::vector<unsigned char> storage(sizeof(FILE_RENAME_INFO) + bytes);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    info->ReplaceIfExists = replace;
    info->FileNameLength = static_cast<DWORD>(bytes);
    std::memcpy(info->FileName, name.data(), bytes);
    for (unsigned attempt = 0;; ++attempt) {
        if (cancelled) LocalCheckCancel(*cancelled);
        if (SetFileInformationByHandle(handle, FileRenameInfo, info, static_cast<DWORD>(storage.size()))) return;
        const DWORD code = GetLastError();
        if (!cancelled || attempt == 8 ||
            (code != ERROR_ACCESS_DENIED && code != ERROR_SHARING_VIOLATION && code != ERROR_LOCK_VIOLATION)) {
            SetLastError(code);
            LocalIo("install_atomic_rename");
        }
        Sleep(25 * (attempt + 1));
    }
}
void PublishImmutableLocalFile(UnsignedStoreSession& session, const fs::path& target, std::string_view bytes,
    const std::atomic<bool>& cancelled, bool pin = true) {
    if (LocalExists(target)) {
        if (LocalRead(target, bytes.size(), pin ? &session.pins : nullptr) != bytes)
            LocalFail("immutable_revision_conflict");
        return;
    }
    auto temp = WriteLocalTemp(target.parent_path(), bytes, cancelled);
    LocalCheckCancel(cancelled);
    RenameLocalHandle(temp->handle->value, target, false);
    temp->committed = true;
    temp->handle.reset();
    if (pin && LocalRead(target, bytes.size(), &session.pins) != bytes)
        LocalFail("immutable_revision_conflict");
}
struct LocalPayloadTemp {
    fs::path path;
    std::unique_ptr<LocalHandle> handle;
    std::vector<std::string> files;
    bool committed = false;
    ~LocalPayloadTemp() {
        if (committed || !handle || handle->value == INVALID_HANDLE_VALUE) return;
        for (const auto& name : files) {
            try {
                auto file = LocalOpen(path / name, false, DELETE, FILE_SHARE_READ);
                FILE_DISPOSITION_INFO info{TRUE};
                SetFileInformationByHandle(file->value, FileDispositionInfo, &info, sizeof(info));
            } catch (...) {}
        }
        FILE_DISPOSITION_INFO info{TRUE};
        SetFileInformationByHandle(handle->value, FileDispositionInfo, &info, sizeof(info));
    }
};
void PublishMultiFileLocalPayload(UnsignedStoreSession& session, const VerifiedPackage& package,
    const std::atomic<bool>& cancelled) {
    const auto parent = session.root / L"packages";
    session.EnsureDirectory(parent);
    const auto target = parent / package.Revision();
    if (LocalExists(target)) {
        session.pins.push_back(LocalOpen(target, true));
        std::set<std::string> expected;
        for (const auto& file : package.Files()) {
            LocalCheckCancel(cancelled);
            expected.insert(file.name);
            if (LocalRead(target / file.name, file.bytes.size(), &session.pins) != file.bytes)
                LocalFail("immutable_revision_conflict");
        }
        for (const auto& item : fs::directory_iterator(target)) {
            if (!expected.contains(json::Utf8(item.path().filename().wstring())))
                LocalFail("immutable_revision_extra_member");
        }
        return;
    }
    static std::atomic<uint64_t> serial{0};
    LocalPayloadTemp temp;
    for (int attempt = 0; attempt < 32; ++attempt) {
        LocalCheckCancel(cancelled);
        temp.path = parent / (L".local-pkg-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(serial.fetch_add(1)));
        if (CreateDirectoryW(temp.path.c_str(), nullptr)) {
            temp.handle = LocalOpen(temp.path, true, FILE_READ_ATTRIBUTES | DELETE);
            break;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) LocalIo("install_payload_directory");
    }
    if (!temp.handle) LocalFail("install_payload_collision");
    for (const auto& file : package.Files()) {
        temp.files.push_back(file.name);
        PublishImmutableLocalFile(session, temp.path / file.name, file.bytes, cancelled, false);
    }
    LocalCheckCancel(cancelled);
    RenameLocalHandle(temp.handle->value, target, false, &cancelled);
    temp.committed = true;
    for (const auto& file : package.Files()) {
        LocalCheckCancel(cancelled);
        if (LocalRead(target / file.name, file.bytes.size(), &session.pins) != file.bytes)
            LocalFail("immutable_revision_conflict");
    }
}
bool ReviewUnsignedLocalInstall(const fs::path& root, const VerifiedPackage& package,
    InstallReview& out, std::string& error) {
    error.clear();
    try {
        UnsignedStoreSession session(root, false, true);
        const auto registry = ReadStoreRegistry(session);
        out = BuildUnsignedReview(session, package, registry);
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}
bool CommitUnsignedLocalInstall(const fs::path& root, const VerifiedPackage& package,
    const InstallReview& authorized, const InstallConsent& consent,
    const std::atomic<bool>& cancelled, InstallResult& out, std::string& error) {
    error.clear();
    try {
        LocalCheckCancel(cancelled);
        if (!consent.approved) LocalFail("install_confirmation_required");
        if (authorized.native_code && !consent.native_execution) LocalFail("native_confirmation_required");
        if (!authorized.added_resources.empty() && !consent.added_resources) LocalFail("resource_confirmation_required");
        UnsignedStoreSession session(root, true, false);
        auto registry = ReadStoreRegistry(session);
        const auto review = BuildUnsignedReview(session, package, registry);
        if (authorized.root != review.root || authorized.id != review.id ||
            authorized.revision != review.revision || authorized.active_revision != review.active_revision ||
            authorized.publisher_id != review.publisher_id ||
            authorized.previous_publisher_id != review.previous_publisher_id ||
            authorized.publisher_rotated != review.publisher_rotated ||
            authorized.currently_enabled != review.currently_enabled ||
            authorized.native_code != review.native_code ||
            authorized.added_resources != review.added_resources) {
            LocalFail("install_review_stale");
        }
        session.EnsureDirectory(session.root / L"versions");
        if (!FileDefinitelyAbsent(session.root / L"versions" / (review.revision + ".sig.json")))
            LocalFail("immutable_revision_conflict");
        if (package.Declaration().native) {
            const auto& native = *package.Declaration().native;
            if (native.files.empty()) {
                session.EnsureDirectory(session.root / L"binaries");
                const auto file = std::find_if(package.Files().begin(), package.Files().end(),
                    [&](const auto& item) { return item.name == native.library; });
                if (file == package.Files().end()) LocalFail("invalid_verified_payload");
                PublishImmutableLocalFile(session, session.root / L"binaries" / (native.sha256 + ".dll"),
                    file->bytes, cancelled);
            } else {
                PublishMultiFileLocalPayload(session, package, cancelled);
            }
        }
        PublishImmutableLocalFile(session, session.root / L"versions" / (review.revision + ".json"),
            package.ManifestBytes(), cancelled);
        InstallResult result;
        result.id = review.id;
        result.revision = review.revision;
        result.enabled = review.active_revision.empty() ? consent.enable_new : review.currently_enabled;
        result.changed = review.active_revision != review.revision;
        if (!result.changed) {
            result.restart_required = false;
            out = std::move(result);
            return true;
        }
        result.previous_revision = review.active_revision;
        result.restart_required = true;
        auto replacement = Value::Object();
        replacement.set("id", review.id);
        replacement.set("name", package.Declaration().name);
        replacement.set("enabled", result.enabled);
        replacement.set("activeRevision", review.revision);
        if (!review.active_revision.empty()) replacement.set("previousRevision", review.active_revision);
        auto packages = Value::Array();
        for (const auto& item : registry.find("packages")->items()) {
            if (item.get_string("id") != review.id) packages.push(item);
        }
        packages.push(std::move(replacement));
        registry.set("packages", std::move(packages));
        const auto bytes = json::Serialize(registry);
        if (bytes.size() > kMaxStoreRegistryBytes) LocalFail("registry_size_limit");
        auto temp = WriteLocalTemp(session.root, bytes, cancelled);
        LocalCheckCancel(cancelled);
        if (LocalExists(session.root / L"registry.json")) {
            auto pin = LocalOpen(session.root / L"registry.json", false);
        }
        RenameLocalHandle(temp->handle->value, session.root / L"registry.json", true);
        temp->committed = true;
        out = std::move(result);
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}
void CollectKnownPolicyPublishers(const fs::path& store_root, std::vector<Publisher>& out) {
    try {
        UnsignedStoreSession session(store_root, false, true);
        if (!session.root_exists || !LocalExists(session.root / L"signature-policy.json")) return;
        const auto bytes = LocalRead(session.root / L"signature-policy.json", kMaxStoreRegistryBytes);
        Value policy;
        if (!json::Parse(bytes, policy, nullptr, {kMaxStoreRegistryBytes, 6}) || !policy.is_object()) return;
        const auto* pins = policy.find("publishers");
        if (!pins || !pins->is_array()) return;
        for (const auto& pin : pins->items()) {
            const auto id = pin.get_string("id");
            const auto key = pin.get_string("publicKey");
            if (id.empty() || key.empty()) continue;
            const bool exists = std::any_of(out.begin(), out.end(), [&](const Publisher& p) { return p.id == id; });
            if (!exists) out.push_back(Publisher{id, {}, key, {}, id == kOfficialCatalogPublisher});
        }
    } catch (...) {}
}
std::string DeriveLocalArchiveVersion(const fs::path& archive_path, std::string_view extension_id) {
    const auto filename = json::Utf8(archive_path.filename().wstring());
    constexpr std::string_view suffix = ".pulse-ext";
    if (!filename.ends_with(suffix)) return {};
    const std::string prefix = std::string(extension_id) + "-";
    if (!filename.starts_with(prefix)) return {};
    const std::string_view candidate(filename.data() + prefix.size(),
        filename.size() - prefix.size() - suffix.size());
    Version parsed{};
    return ParseVersion(candidate, parsed) ? std::string(candidate) : std::string{};
}
struct CoreOperations {
    std::function<bool(const Catalog&, std::string_view, std::string_view,
        const std::atomic<bool>&, PackageDownload&, std::string&)> download =
        [](const Catalog& catalog, std::string_view id, std::string_view version,
            const std::atomic<bool>& cancel, PackageDownload& out, std::string& error) {
            return FetchCatalogPackage(catalog, id, version, cancel, out, error);
        };
    decltype(&ReviewPublisherTrust) review_trust = ReviewPublisherTrust;
    decltype(&CommitPublisherTrust) commit_trust = CommitPublisherTrust;
    decltype(&ReviewInstall) review_install = ReviewInstall;
    decltype(&CommitInstall) commit_install = CommitInstall;
};
}
bool IsReleaseCompatible(const Release& release, std::string_view host_version, int sdk_preview) {
    return ReleaseCompatibilityError(release, host_version, sdk_preview) == nullptr;
}
struct InstallService::Impl {
    struct Operations {
        decltype(CoreOperations::download) download;
        std::function<std::remove_pointer_t<decltype(&ReviewPublisherTrust)>> review_trust;
        std::function<std::remove_pointer_t<decltype(&CommitPublisherTrust)>> commit_trust;
        std::function<std::remove_pointer_t<decltype(&ReviewInstall)>> review_install;
        std::function<std::remove_pointer_t<decltype(&CommitInstall)>> commit_install;
        Operations() : download(CoreOperations{}.download), review_trust(ReviewPublisherTrust),
            commit_trust(CommitPublisherTrust), review_install(ReviewInstall), commit_install(CommitInstall) {}
    };
    struct Context {
        uint64_t generation = 0;
        std::shared_ptr<const CatalogSnapshot> catalog;
        std::string id, version;
        fs::path local_archive_path, local_signature_path;
        bool is_local = false;
        std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<const VerifiedPackage> package;
        PublisherTrustReview trust;
        InstallReview install;
    };
    enum class Command { Prepare, PrepareLocal, Trust, Install };
    struct Request {
        Command command;
        std::shared_ptr<Context> context;
        bool approved = false;
        InstallConsent consent;
    };
    Config config;
    Operations operations;
    mutable std::mutex mutex;
    std::condition_variable wake, changed;
    InstallServiceSnapshot snapshot;
    std::optional<Request> pending;
    std::shared_ptr<Context> ready;
    std::shared_ptr<std::atomic<bool>> active;
    bool stopping = false;
    std::thread worker;

    explicit Impl(Config value, Operations ops = {}) : config(std::move(value)), operations(std::move(ops)) {
        if (!config.store_root.is_absolute()) throw std::invalid_argument("install_store_root_not_absolute");
        Version host{};
        if (!ParseVersion(config.host_version, host)) throw std::invalid_argument("install_host_version_invalid");
        if (config.sdk_preview <= 0) throw std::invalid_argument("install_sdk_preview_invalid");
        if (!operations.download || !operations.review_trust || !operations.commit_trust ||
            !operations.review_install || !operations.commit_install) throw std::invalid_argument("install_operations_missing");
        worker = std::thread(&Impl::Run, this);
    }
    ~Impl() { Stop(); if (worker.joinable()) worker.join(); }
    void CancelWork() {
        if (active) active->store(true);
        if (pending) pending->context->cancelled->store(true);
        if (ready) ready->cancelled->store(true);
        pending.reset();
        ready.reset();
    }
    void Reset(InstallServicePhase phase, std::string error = {}) {
        const auto generation = snapshot.generation + 1;
        const bool restart = snapshot.pending_restart;
        snapshot = {};
        snapshot.generation = generation;
        snapshot.phase = phase;
        snapshot.error = std::move(error);
        snapshot.pending_restart = restart;
    }
    void Stop() {
        {
            std::lock_guard lock(mutex);
            if (stopping) return;
            stopping = true;
            CancelWork();
            Reset(InstallServicePhase::Cancelled, "install_service_stopped");
        }
        wake.notify_one();
        changed.notify_all();
    }
    bool Compatible(const Context& c, std::string& error) const {
        if (!c.catalog) { error = "install_catalog_missing"; return false; }
        for (const auto& extension : c.catalog->catalog.extensions) {
            if (extension.id != c.id) continue;
            for (const auto& release : extension.versions) {
                if (release.version != c.version) continue;
                if (IsReleaseCompatible(release, config.host_version, config.sdk_preview)) return true;
                error = ReleaseCompatibilityError(release, config.host_version, config.sdk_preview);
                return false;
            }
            error = "install_release_missing";
            return false;
        }
        error = "install_extension_missing";
        return false;
    }
    void Execute(const Request& request, InstallServiceSnapshot& out, bool& restart) {
        auto& c = *request.context;
        auto& cancelled = *c.cancelled;
        auto fail = [&] { out.phase = InstallServicePhase::Failed; if (out.error.empty()) out.error = "install_operation_failed"; };
        if (cancelled.load()) { out.phase = InstallServicePhase::Cancelled; out.error = "cancelled"; return; }
        if (request.command == Command::Prepare) {
            if (!Compatible(c, out.error)) { out.phase = InstallServicePhase::Incompatible; return; }
            PackageDownload download;
            if (!operations.download(c.catalog->catalog, c.id, c.version, cancelled, download, out.error)) { fail(); return; }
            if (cancelled.load()) return;
            if (!download.package) { out.error = "install_package_invalid"; fail(); return; }
            c.package = std::make_shared<const VerifiedPackage>(std::move(download.package));
            out.package = c.package;
            if (!operations.review_trust(config.store_root, *c.package, c.trust, out.error)) { fail(); return; }
            if (cancelled.load()) return;
            out.trust_review = std::make_shared<const PublisherTrustReview>(c.trust);
            if (!c.trust.already_trusted) { out.phase = InstallServicePhase::AwaitingPublisherTrust; return; }
        } else if (request.command == Command::PrepareLocal) {
            LocalPackageVerifyOptions opts;
            opts.signature_path = c.local_signature_path;
            opts.host_version = config.host_version;
            opts.sdk_preview = config.sdk_preview;
            if (c.catalog) opts.known_publishers = c.catalog->catalog.publishers;
            CollectKnownPolicyPublishers(config.store_root, opts.known_publishers);
            VerifiedPackage verified;
            if (!VerifyLocalPackage(c.local_archive_path, opts, cancelled, verified, out.error)) {
                if (cancelled.load()) return;
                if (out.error == "install_host_version_invalid" || out.error == "install_sdk_incompatible") {
                    out.phase = InstallServicePhase::Incompatible;
                    return;
                }
                fail();
                return;
            }
            if (cancelled.load()) return;
            c.id = verified.Declaration().id;
            c.version = DeriveLocalArchiveVersion(c.local_archive_path, c.id);
            out.id = c.id;
            out.version = c.version;
            c.package = std::make_shared<const VerifiedPackage>(std::move(verified));
            out.package = c.package;
            if (c.package->PublisherId().empty()) {
                if (!ReviewUnsignedLocalInstall(config.store_root, *c.package, c.install, out.error)) { fail(); return; }
                if (cancelled.load()) return;
                out.install_review = std::make_shared<const InstallReview>(c.install);
                out.phase = InstallServicePhase::AwaitingInstall;
                return;
            }
            if (!operations.review_trust(config.store_root, *c.package, c.trust, out.error)) { fail(); return; }
            if (cancelled.load()) return;
            out.trust_review = std::make_shared<const PublisherTrustReview>(c.trust);
            if (!c.trust.already_trusted) { out.phase = InstallServicePhase::AwaitingPublisherTrust; return; }
        } else if (request.command == Command::Trust) {
            if (!request.approved) { out.phase = InstallServicePhase::Cancelled; out.error = "publisher_confirmation_declined"; return; }
            PublisherTrustResult result;
            if (!operations.commit_trust(config.store_root, *c.package, c.trust, true, cancelled, result, out.error)) { fail(); return; }
            // A committed policy is intentionally not rolled back after cancellation.
            if (cancelled.load()) return;
        } else {
            InstallResult result;
            const bool committed = (c.is_local && c.package && c.package->PublisherId().empty())
                ? CommitUnsignedLocalInstall(config.store_root, *c.package, c.install, request.consent, cancelled, result, out.error)
                : operations.commit_install(config.store_root, *c.package, c.install, request.consent, cancelled, result, out.error);
            if (!committed) { fail(); return; }
            // Commit may have crossed its atomic rename before cancellation. Preserve
            // this fact even if this generation is now stale or the service stopped.
            restart = result.restart_required;
            out.enabled = result.enabled;
            out.phase = InstallServicePhase::Installed;
            return;
        }
        if (cancelled.load()) return;
        if (!operations.review_install(config.store_root, *c.package, c.install, out.error)) { fail(); return; }
        out.install_review = std::make_shared<const InstallReview>(c.install);
        out.phase = InstallServicePhase::AwaitingInstall;
    }
    void Run() {
        for (;;) {
            std::optional<Request> request;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || pending.has_value(); });
                if (stopping) return;
                request = std::move(pending);
                pending.reset();
                active = request->context->cancelled;
            }
            const auto& c = *request->context;
            InstallServiceSnapshot out;
            out.generation = c.generation;
            out.id = c.id;
            out.version = c.version;
            out.local_archive_path = c.local_archive_path;
            out.is_local_package = c.is_local;
            out.package = c.package;
            bool restart = false;
            try {
                Execute(*request, out, restart);
            } catch (const std::exception& ex) {
                out.phase = InstallServicePhase::Failed;
                out.error = std::string("install_service_exception: ") + ex.what();
            } catch (...) {
                out.phase = InstallServicePhase::Failed;
                out.error = "install_service_exception";
            }
            {
                std::lock_guard lock(mutex);
                active.reset();
                snapshot.pending_restart = snapshot.pending_restart || restart;
                if (!stopping && snapshot.generation == c.generation) {
                    out.pending_restart = snapshot.pending_restart;
                    if (c.cancelled->load()) { out.phase = InstallServicePhase::Cancelled; out.error = "cancelled"; }
                    if (out.phase == InstallServicePhase::AwaitingInstall || out.phase == InstallServicePhase::AwaitingPublisherTrust)
                        ready = request->context;
                    snapshot = std::move(out);
                }
            }
            changed.notify_all();
        }
    }
};
InstallService::InstallService(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}
InstallService::~InstallService() = default;
uint64_t InstallService::Prepare(std::shared_ptr<const CatalogSnapshot> catalog, std::string id, std::string version) {
    auto context = std::make_shared<Impl::Context>();
    context->catalog = std::move(catalog);
    context->id = std::move(id);
    context->version = std::move(version);
    uint64_t generation;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping) return impl_->snapshot.generation;
        impl_->CancelWork();
        impl_->Reset(InstallServicePhase::Preparing);
        generation = context->generation = impl_->snapshot.generation;
        impl_->snapshot.id = context->id;
        impl_->snapshot.version = context->version;
        impl_->pending = Impl::Request{Impl::Command::Prepare, std::move(context), false, {}};
    }
    impl_->wake.notify_one();
    impl_->changed.notify_all();
    return generation;
}
uint64_t InstallService::PrepareLocal(std::filesystem::path archive_path,
    std::filesystem::path signature_path,
    std::shared_ptr<const CatalogSnapshot> catalog) {
    auto context = std::make_shared<Impl::Context>();
    context->catalog = std::move(catalog);
    context->local_archive_path = std::move(archive_path);
    context->local_signature_path = std::move(signature_path);
    context->is_local = true;
    uint64_t generation;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping) return impl_->snapshot.generation;
        impl_->CancelWork();
        impl_->Reset(InstallServicePhase::Preparing);
        generation = context->generation = impl_->snapshot.generation;
        impl_->snapshot.is_local_package = true;
        impl_->snapshot.local_archive_path = context->local_archive_path;
        impl_->pending = Impl::Request{Impl::Command::PrepareLocal, std::move(context), false, {}};
    }
    impl_->wake.notify_one();
    impl_->changed.notify_all();
    return generation;
}
bool InstallService::ConfirmPublisherTrust(uint64_t generation, bool approved) {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping || impl_->snapshot.generation != generation ||
            impl_->snapshot.phase != InstallServicePhase::AwaitingPublisherTrust || !impl_->ready) return false;
        impl_->pending = Impl::Request{Impl::Command::Trust, std::move(impl_->ready), approved, {}};
        impl_->snapshot.phase = InstallServicePhase::Preparing;
    }
    impl_->wake.notify_one();
    impl_->changed.notify_all();
    return true;
}
bool InstallService::ConfirmInstall(uint64_t generation, InstallConsent consent) {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping || impl_->snapshot.generation != generation ||
            impl_->snapshot.phase != InstallServicePhase::AwaitingInstall || !impl_->ready) return false;
        impl_->pending = Impl::Request{Impl::Command::Install, std::move(impl_->ready), false, consent};
        impl_->snapshot.phase = InstallServicePhase::Committing;
    }
    impl_->wake.notify_one();
    impl_->changed.notify_all();
    return true;
}
uint64_t InstallService::Cancel() {
    uint64_t generation;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping) return impl_->snapshot.generation;
        impl_->CancelWork();
        impl_->Reset(InstallServicePhase::Cancelled, "cancelled");
        generation = impl_->snapshot.generation;
    }
    impl_->changed.notify_all();
    return generation;
}
InstallServiceSnapshot InstallService::GetSnapshot() const { std::lock_guard lock(impl_->mutex); return impl_->snapshot; }
void InstallService::Stop() { impl_->Stop(); }
#ifdef PULSE_EXTENSION_INSTALL_SERVICE_TESTING
InstallService::InstallService(Config config, Operations ops) {
    Impl::Operations operations;
    operations.download = std::move(ops.download);
    operations.review_trust = std::move(ops.review_trust);
    operations.commit_trust = std::move(ops.commit_trust);
    operations.review_install = std::move(ops.review_install);
    operations.commit_install = std::move(ops.commit_install);
    impl_ = std::make_unique<Impl>(std::move(config), std::move(operations));
}
bool InstallService::WaitForPhase(uint64_t generation, InstallServicePhase phase, std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->changed.wait_for(lock, timeout, [&] {
        return impl_->snapshot.generation > generation ||
            (impl_->snapshot.generation == generation && impl_->snapshot.phase == phase);
    });
}
bool InstallService::WaitForPendingRestart(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->changed.wait_for(lock, timeout, [&] { return impl_->snapshot.pending_restart; });
}
#endif
} // namespace pulse::extensions::store

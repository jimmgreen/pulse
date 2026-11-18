#include "install_transaction.h"
#include "publisher_trust.h"
#include "installed_store.h"
#include "../native_image_verify.h"
#include "../package_signature.h"
#include "../package_store.h"
#include "../../../common/json_value.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <set>
#include <stdexcept>

namespace pulse::extensions::store {
namespace {
namespace fs = std::filesystem;
using json::Value;
#ifdef PULSE_EXTENSION_INSTALL_TRANSACTION_TESTING
thread_local const testing::InstallHooks* install_hooks = nullptr;
#endif
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
[[noreturn]] void Fail(const std::string& text) { throw std::runtime_error(text); }
[[noreturn]] void Io(const char* operation) { Fail(std::string(operation) + ":" + std::to_string(GetLastError())); }
void CheckCancel(const std::atomic<bool>& cancelled) { if (cancelled) Fail("install_cancelled"); }
bool Exists(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) return true;
    if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) Io("store_stat");
    return false;
}
std::unique_ptr<Handle> Open(const fs::path& path, bool directory, DWORD access = FILE_READ_ATTRIBUTES,
    DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE) {
    auto handle = std::make_unique<Handle>();
    handle->value = CreateFileW(path.c_str(), access, sharing, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    if (handle->value == INVALID_HANDLE_VALUE) Io("store_open");
    if (!GetFileInformationByHandle(handle->value, &info)) Io("store_file_info");
    if (GetFileType(handle->value) != FILE_TYPE_DISK || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory) Fail("store_requires_ordinary_path");
    return handle;
}
std::string Read(const fs::path& path, size_t limit, std::vector<std::unique_ptr<Handle>>* pins = nullptr) {
    auto file = Open(path, false, GENERIC_READ, FILE_SHARE_READ);
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file->value, &length)) Io("store_size");
    if (length.QuadPart < 0 || static_cast<uint64_t>(length.QuadPart) > limit) Fail("store_size_limit");
    std::string bytes(static_cast<size_t>(length.QuadPart), '\0');
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD count = 0;
        const auto wanted = static_cast<DWORD>((std::min)(bytes.size() - offset, size_t{65536}));
        if (!ReadFile(file->value, bytes.data() + offset, wanted, &count, nullptr) || count != wanted) Io("store_read");
        offset += count;
    }
    if (pins) pins->push_back(std::move(file));
    return bytes;
}
bool Fields(const Value& v, std::initializer_list<std::string_view> allowed) {
    if (!v.is_object()) return false;
    for (const auto& key : v.keys()) if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) return false;
    return true;
}
bool Hash(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
struct Session {
    fs::path root;
    std::vector<std::unique_ptr<Handle>> pins;
    std::unique_ptr<Handle> lock;
    bool root_exists = false;
    bool root_created = false;
    explicit Session(const fs::path& path, bool create_root = false, bool allow_missing = false) : root(path) {
        if (!root.is_absolute() || root.root_name().native().size() != 2 || root.native().find(L'\0') != fs::path::string_type::npos)
            Fail("store_requires_absolute_local_path");
        for (const auto& component : root) {
            const auto name = component.native();
            if (!name.empty() && (name.back() == L'.' || name.back() == L' ')) Fail("store_path_alias");
        }
        root = root.lexically_normal();
        if (root == root.root_path()) Fail("store_volume_root_forbidden");
        const auto type = GetDriveTypeW(root.root_path().c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_RAMDISK) Fail("store_requires_local_volume");
        // Keep ancestors pinned against rename/reparse replacement while paths are used.
        std::vector<fs::path> ancestors;
        for (auto parent = root;; parent = parent.parent_path()) {
            ancestors.push_back(parent);
            if (parent == parent.root_path()) break;
        }
        for (auto parent = ancestors.rbegin(); parent != ancestors.rend(); ++parent) {
            if (!Exists(*parent)) {
                if (!create_root) {
                    if (allow_missing) return;
                    Fail("store_root_missing");
                }
                if (!CreateDirectoryW(parent->c_str(), nullptr)) {
                    if (const DWORD error = GetLastError(); error != ERROR_ALREADY_EXISTS) {
                        // Name the folder: an access-denied here is usually security software, not the code.
                        const auto name = parent->u8string();
                        Fail("store_create_root:" + std::to_string(error) + ":" + std::string(name.begin(), name.end()));
                    }
                } else if (*parent == root) root_created = true;
            }
            pins.push_back(Open(*parent, true));
        }
        root_exists = true;
        const auto lock_path = root / L".registry.lock";
        Handle created;
        created.value = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (created.value == INVALID_HANDLE_VALUE) Io("store_lock_open");
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(created.value, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || info.nNumberOfLinks != 1 ||
            GetFileType(created.value) != FILE_TYPE_DISK) Fail("store_lock_not_regular");
        OVERLAPPED overlap{};
        if (!LockFileEx(created.value, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap)) Fail("store_busy");
        lock = std::make_unique<Handle>(); lock->value = created.value; created.value = INVALID_HANDLE_VALUE;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(lock->value, &size)) Io("store_lock_size");
        if (size.QuadPart == 0) {
            DWORD written = 0; const char zero = 0;
            if (!WriteFile(lock->value, &zero, 1, &written, nullptr) || written != 1) Io("store_lock_initialize");
        }
    }
    void Directory(const fs::path& path) {
        if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) Io("store_create_directory");
        pins.push_back(Open(path, true));
    }
};
Value Registry(Session& session) {
    const auto path = session.root / L"registry.json";
    Value registry;
    std::string error;
    if (!Exists(path)) { registry = Value::Object(); registry.set("formatVersion", 1); registry.set("packages", Value::Array()); return registry; }
    if (!json::Parse(Read(path, kMaxStoreRegistryBytes), registry, &error, {kMaxStoreRegistryBytes, 4}) ||
        !Fields(registry, {"formatVersion", "packages"}) || !registry.find("formatVersion") ||
        !registry.find("packages") || !registry.find("packages")->is_array()) Fail("invalid_store_registry");
    int64_t version = 0;
    if (!registry.find("formatVersion")->get_int(version, 1, 1) || registry.find("packages")->size() > kMaxStorePackages) Fail("invalid_store_registry");
    std::set<std::string> ids;
    for (const auto& item : registry.find("packages")->items()) {
        if (!Fields(item, {"id", "name", "enabled", "activeRevision", "previousRevision"}) ||
            !item.find("enabled") || !item.find("enabled")->is_bool() || !Hash(item.get_string("activeRevision")) ||
            (item.find("previousRevision") && !Hash(item.get_string("previousRevision")))) Fail("invalid_store_entry");
        auto identity = Value::Object();
        identity.set("apiVersion", 1); identity.set("id", item.get_string("id")); identity.set("name", item.get_string("name"));
        if (!ParseManifest(json::Serialize(identity)) || !ids.insert(item.get_string("id")).second) Fail("invalid_store_identity");
    }
    return registry;
}
const Value* Entry(const Value& registry, const std::string& id) {
    for (const auto& item : registry.find("packages")->items()) if (item.get_string("id") == id) return &item;
    return nullptr;
}
std::string Policy(Session& session, const VerifiedPackage& package) {
    const auto path = session.root / L"signature-policy.json";
    if (!Exists(path)) Fail("publisher_trust_required");
    auto bytes = Read(path, kMaxStoreRegistryBytes, &session.pins);
    // Verify against the store policy again, not just the catalog's publisher.
    std::string error;
    if (!VerifyPackageSignature(package.ManifestBytes(), package.SignatureBytes(), bytes, error))
        Fail("store_publisher_policy:" + error);
    return bytes;
}
InstallReview Review(Session& session, const VerifiedPackage& package, const Value& registry) {
    InstallReview review; review.root = session.root.native();
    review.id = package.Declaration().id; review.revision = package.Revision(); review.publisher_id = package.PublisherId();
    review.native_code = package.Declaration().native.has_value();
    review.added_resources = package.Declaration().resources;
    const auto policy = Policy(session, package);
    if (const auto* entry = Entry(registry, review.id)) {
        review.active_revision = entry->get_string("activeRevision"); review.currently_enabled = entry->get_bool("enabled");
        session.pins.push_back(Open(session.root / L"versions", true));
        const auto old_bytes = Read(session.root / L"versions" / (review.active_revision + ".json"), kMaxManifestBytes, &session.pins);
        const auto old_signature = Read(session.root / L"versions" / (review.active_revision + ".sig.json"), 16384, &session.pins);
        const auto old = ParseManifest(old_bytes);
        std::string error;
        if (!old || PackageRevision(old_bytes) != review.active_revision || old.manifest->id != review.id ||
            old.manifest->name != entry->get_string("name") || !VerifyPackageSignature(old_bytes, old_signature, policy, error))
            Fail("invalid_active_revision");
        // Do not allow an unrelated trusted publisher to take over an installed id.
        Value signature;
        if (!json::Parse(old_signature, signature) || signature.get_string("publisher") != package.PublisherId()) Fail("installed_publisher_mismatch");
        std::erase_if(review.added_resources, [&](const auto& value) {
            return std::find(old.manifest->resources.begin(), old.manifest->resources.end(), value) != old.manifest->resources.end();
        });
    } else if (registry.find("packages")->size() >= kMaxStorePackages) Fail("store_full");
    std::sort(review.added_resources.begin(), review.added_resources.end());
    return review;
}
struct Temporary {
    fs::path path;
    std::unique_ptr<Handle> handle;
    bool committed = false;
    ~Temporary() {
        if (!committed && handle) {
            FILE_DISPOSITION_INFO info{TRUE};
            SetFileInformationByHandle(handle->value, FileDispositionInfo, &info, sizeof(info));
        }
    }
};
std::unique_ptr<Temporary> WriteTemporary(const fs::path& parent, std::string_view bytes, const std::atomic<bool>& cancelled) {
    static std::atomic<uint64_t> serial{0};
    auto file = std::make_unique<Temporary>(); file->handle = std::make_unique<Handle>();
    for (int attempt = 0; attempt < 32; ++attempt) {
        CheckCancel(cancelled);
        file->path = parent / (L".install-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(serial.fetch_add(1)));
        file->handle->value = CreateFileW(file->path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file->handle->value != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) Io("install_create_temporary");
    }
    if (file->handle->value == INVALID_HANDLE_VALUE) Fail("install_temporary_collision");
    size_t offset = 0;
    while (offset < bytes.size()) {
        CheckCancel(cancelled);
        DWORD count = 0; const auto wanted = static_cast<DWORD>((std::min)(bytes.size() - offset, size_t{65536}));
        if (!WriteFile(file->handle->value, bytes.data() + offset, wanted, &count, nullptr) || count != wanted) Io("install_write");
        offset += count;
    }
    if (!FlushFileBuffers(file->handle->value)) Io("install_flush");
    return file;
}
void RenameHandle(HANDLE handle, const fs::path& target, bool replace, const std::atomic<bool>* cancelled = nullptr) {
    const auto name = target.native(); const auto bytes = name.size() * sizeof(wchar_t);
    if (bytes > 65500) Fail("install_path_limit");
    std::vector<unsigned char> storage(sizeof(FILE_RENAME_INFO) + bytes);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    info->ReplaceIfExists = replace; info->FileNameLength = static_cast<DWORD>(bytes);
    std::memcpy(info->FileName, name.data(), bytes);
    // Open descendants can temporarily prevent directory publication on Windows.
    // Tolerate a short-lived reader, never bypass a denial.
    for (unsigned attempt = 0;; ++attempt) {
        if (cancelled) CheckCancel(*cancelled);
        if (SetFileInformationByHandle(handle, FileRenameInfo, info, static_cast<DWORD>(storage.size()))) return;
        const DWORD code = GetLastError();
        if (!cancelled || attempt == 8 || (code != ERROR_ACCESS_DENIED && code != ERROR_SHARING_VIOLATION && code != ERROR_LOCK_VIOLATION)) {
            SetLastError(code); Io("install_atomic_rename");
        }
#ifdef PULSE_EXTENSION_INSTALL_TRANSACTION_TESTING
        if (install_hooks && install_hooks->rename_retry) install_hooks->rename_retry(target, code);
#endif
        Sleep(25 * (attempt + 1));
    }
}
void Rename(Temporary& file, const fs::path& target, bool replace) {
    RenameHandle(file.handle->value, target, replace);
    file.committed = true;
}
void ImmutableFile(Session& session, const fs::path& target, std::string_view bytes, const std::atomic<bool>& cancelled, bool pin = true) {
    if (Exists(target)) {
        if (Read(target, bytes.size(), pin ? &session.pins : nullptr) != bytes) Fail("immutable_revision_conflict");
        return;
    }
    auto temporary = WriteTemporary(target.parent_path(), bytes, cancelled);
    CheckCancel(cancelled);
    Rename(*temporary, target, false);
    temporary->handle.reset();
    if (pin && Read(target, bytes.size(), &session.pins) != bytes) Fail("immutable_revision_conflict");
}
struct PayloadTemporary {
    fs::path path;
    std::unique_ptr<Handle> handle;
    std::vector<std::string> files;
    bool committed = false;
    ~PayloadTemporary() {
        if (committed || !handle) return;
        for (const auto& name : files) {
            try {
                auto file = Open(path / name, false, DELETE, FILE_SHARE_READ);
                FILE_DISPOSITION_INFO info{TRUE};
                SetFileInformationByHandle(file->value, FileDispositionInfo, &info, sizeof(info));
            } catch (...) {} // A failure leaves an inactive, uniquely named directory, never a live revision.
        }
        FILE_DISPOSITION_INFO info{TRUE};
        SetFileInformationByHandle(handle->value, FileDispositionInfo, &info, sizeof(info));
    }
};
void Payload(Session& session, const VerifiedPackage& package, const std::atomic<bool>& cancelled) {
    const auto parent = session.root / L"packages";
    session.Directory(parent);
    const auto target = parent / package.Revision();
    if (Exists(target)) {
        session.pins.push_back(Open(target, true));
        std::set<std::string> expected;
        for (const auto& file : package.Files()) {
            CheckCancel(cancelled);
            expected.insert(file.name);
            if (Read(target / file.name, file.bytes.size(), &session.pins) != file.bytes) Fail("immutable_revision_conflict");
        }
        for (const auto& item : fs::directory_iterator(target))
            if (!expected.contains(json::Utf8(item.path().filename().wstring()))) Fail("immutable_revision_extra_member");
        return;
    }
    static std::atomic<uint64_t> serial{0};
    PayloadTemporary temporary;
    for (int attempt = 0; attempt < 32; ++attempt) {
        CheckCancel(cancelled);
        temporary.path = parent / (L".package-install-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(serial.fetch_add(1)));
        if (CreateDirectoryW(temporary.path.c_str(), nullptr)) {
            temporary.handle = Open(temporary.path, true, FILE_READ_ATTRIBUTES | DELETE);
            break;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) Io("install_payload_directory");
    }
    if (!temporary.handle) Fail("install_payload_collision");
    for (const auto& file : package.Files()) {
        temporary.files.push_back(file.name);
        ImmutableFile(session, temporary.path / file.name, file.bytes, cancelled, false);
    }
    CheckCancel(cancelled);
#ifdef PULSE_EXTENSION_INSTALL_TRANSACTION_TESTING
    if (install_hooks && install_hooks->before_payload_publish) install_hooks->before_payload_publish(temporary.path);
#endif
    RenameHandle(temporary.handle->value, target, false, &cancelled);
    temporary.committed = true;
    for (const auto& file : package.Files()) {
        CheckCancel(cancelled);
        if (Read(target / file.name, file.bytes.size(), &session.pins) != file.bytes) Fail("immutable_revision_conflict");
    }
}
InstallResult Commit(Session& session, const VerifiedPackage& package, const InstallReview& authorized,
    const InstallConsent& consent, const std::atomic<bool>& cancelled, const std::function<void()>& hook) {
    CheckCancel(cancelled);
    auto registry = Registry(session);
    const auto review = Review(session, package, registry);
    if (authorized.root != review.root || authorized.id != review.id || authorized.revision != review.revision || authorized.active_revision != review.active_revision ||
        authorized.publisher_id != review.publisher_id ||
        authorized.currently_enabled != review.currently_enabled || authorized.native_code != review.native_code ||
        authorized.added_resources != review.added_resources) Fail("install_review_stale");
    if (!consent.approved) Fail("install_confirmation_required");
    if (review.native_code && !consent.native_execution) Fail("native_confirmation_required");
    if (!review.added_resources.empty() && !consent.added_resources) Fail("resource_confirmation_required");
    session.Directory(session.root / L"versions");
    if (package.Declaration().native) {
        const auto& native = *package.Declaration().native;
        if (native.files.empty()) {
            session.Directory(session.root / L"binaries");
            const auto file = std::find_if(package.Files().begin(), package.Files().end(), [&](const auto& item) { return item.name == native.library; });
            if (file == package.Files().end()) Fail("invalid_verified_payload");
            ImmutableFile(session, session.root / L"binaries" / (native.sha256 + ".dll"), file->bytes, cancelled);
        } else {
            Payload(session, package, cancelled);
        }
    }
    ImmutableFile(session, session.root / L"versions" / (review.revision + ".json"), package.ManifestBytes(), cancelled);
    ImmutableFile(session, session.root / L"versions" / (review.revision + ".sig.json"), package.SignatureBytes(), cancelled);
    InstallResult result;
    result.id = review.id; result.revision = review.revision;
    result.enabled = review.active_revision.empty() ? consent.enable_new : review.currently_enabled;
    result.changed = review.active_revision != review.revision;
    if (!result.changed) { result.restart_required = false; return result; }
    result.previous_revision = review.active_revision; result.restart_required = true;
    auto replacement = Value::Object();
    replacement.set("id", review.id); replacement.set("name", package.Declaration().name);
    replacement.set("enabled", result.enabled); replacement.set("activeRevision", review.revision);
    if (!review.active_revision.empty()) replacement.set("previousRevision", review.active_revision);
    auto packages = Value::Array();
    for (const auto& item : registry.find("packages")->items()) if (item.get_string("id") != review.id) packages.push(item);
    packages.push(std::move(replacement)); registry.set("packages", std::move(packages));
    const auto bytes = json::Serialize(registry);
    if (bytes.size() > kMaxStoreRegistryBytes) Fail("registry_size_limit");
    auto temporary = WriteTemporary(session.root, bytes, cancelled);
    if (hook) hook();
    CheckCancel(cancelled);
    if (Exists(session.root / L"registry.json")) { auto pin = Open(session.root / L"registry.json", false); }
    Rename(*temporary, session.root / L"registry.json", true);
    return result;
}
// Management shares the exact install I/O, lock, pin and atomic publication path.
std::string RegistryRevision(Session& session) {
    const auto path = session.root / L"registry.json";
    return Exists(path) ? PackageRevision(Read(path, kMaxStoreRegistryBytes)) : std::string{};
}
struct StoredPrivate {
    Manifest manifest;
    std::string publisher;
};
StoredPrivate VerifyStoredPrivate(Session& session, const Value& entry, const std::string& revision,
    bool active, bool payload) {
    session.pins.push_back(Open(session.root / L"versions", true));
    const auto bytes = Read(session.root / L"versions" / (revision + ".json"), kMaxManifestBytes, &session.pins);
    const auto parsed = ParseManifest(bytes);
    if (!parsed || PackageRevision(bytes) != revision || parsed.manifest->id != entry.get_string("id") ||
        (active && parsed.manifest->name != entry.get_string("name"))) Fail("invalid_stored_revision");
    const auto signature = Read(session.root / L"versions" / (revision + ".sig.json"), 16384, &session.pins);
    const auto policy = Read(session.root / L"signature-policy.json", kMaxStoreRegistryBytes, &session.pins);
    std::string error;
    if (!VerifyPackageSignature(bytes, signature, policy, error)) Fail("store_signature:" + error);
    Value signed_by;
    if (!json::Parse(signature, signed_by) || signed_by.get_string("publisher").empty()) Fail("stored_publisher_required");
    if (payload && parsed.manifest->native) {
        const auto& native = *parsed.manifest->native;
        // Pin each path before using the shared production hash/PE verifier.
        const auto parent = native.files.empty() ? session.root / L"binaries" : session.root / L"packages" / revision;
        if (!native.files.empty()) session.pins.push_back(Open(session.root / L"packages", true));
        session.pins.push_back(Open(parent, true));
        std::vector<HANDLE> locks;
        const auto entry_path = parent / (native.files.empty() ? native.sha256 + ".dll" : native.library);
        if (!OpenVerifiedNativePackage(entry_path.wstring(), native, locks, error)) Fail("stored_payload:" + error);
        // Transfer all read locks into Session before any further operation.
        struct Acquired { std::vector<HANDLE>& values; ~Acquired() { for (auto h : values) if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } } acquired{locks};
        session.pins.reserve(session.pins.size() + locks.size());
        for (auto& raw : locks) { auto pin = std::make_unique<Handle>(); pin->value = raw; raw = INVALID_HANDLE_VALUE; session.pins.push_back(std::move(pin)); }
        if (!native.files.empty()) {
            std::set<std::string> expected{native.library};
            for (const auto& file : native.files) expected.insert(file.path);
            for (const auto& file : fs::directory_iterator(parent))
                if (!expected.contains(json::Utf8(file.path().filename().wstring()))) Fail("immutable_revision_extra_member");
        }
    }
    return {*parsed.manifest, signed_by.get_string("publisher")};
}
StoreChangeReview ChangeReview(Session& session, const Value& registry, const std::string& id, StoreChangeAction action) {
    if (action != StoreChangeAction::Enable && action != StoreChangeAction::Disable &&
        action != StoreChangeAction::Uninstall && action != StoreChangeAction::Rollback) Fail("invalid_store_action");
    const auto* entry = Entry(registry, id);
    if (!entry) Fail("store_entry_missing");
    StoreChangeReview review;
    review.root = session.root; review.id = id; review.action = action;
    const auto root_pin = Open(session.root, true);
    BY_HANDLE_FILE_INFORMATION identity{};
    if (!GetFileInformationByHandle(root_pin->value, &identity)) Io("store_root_identity");
    review.store_identity = std::to_string(identity.dwVolumeSerialNumber) + ":" +
        std::to_string(identity.nFileIndexHigh) + ":" + std::to_string(identity.nFileIndexLow);
    review.registry_revision = RegistryRevision(session);
    review.active_revision = entry->get_string("activeRevision");
    review.previous_revision = entry->get_string("previousRevision");
    review.enabled = entry->get_bool("enabled");
    if (action == StoreChangeAction::Disable || action == StoreChangeAction::Uninstall) return review;
    review.policy_revision = PackageRevision(Read(session.root / L"signature-policy.json", kMaxStoreRegistryBytes, &session.pins));
    auto current = VerifyStoredPrivate(session, *entry, review.active_revision, true, action == StoreChangeAction::Enable);
    review.publisher_id = current.publisher;
    auto target = current;
    if (action == StoreChangeAction::Rollback) {
        if (review.previous_revision.empty() || review.previous_revision == review.active_revision) Fail("store_rollback_unavailable");
        target = VerifyStoredPrivate(session, *entry, review.previous_revision, false, true);
        if (target.publisher != current.publisher) Fail("installed_publisher_mismatch");
    }
    review.native_code = target.manifest.native.has_value();
    review.added_resources = target.manifest.resources;
    // Enabling is a new execution grant: disclose all resources, even if disabled
    // metadata was previously installed. Rollback discloses only newly added ones.
    if (action == StoreChangeAction::Rollback) std::erase_if(review.added_resources, [&](const auto& resource) {
        return std::find(current.manifest.resources.begin(), current.manifest.resources.end(), resource) != current.manifest.resources.end();
    });
    std::sort(review.added_resources.begin(), review.added_resources.end());
    return review;
}
StoreChangeResult CommitChange(Session& session, const StoreChangeReview& authorized, const StoreChangeConsent& consent,
    const std::atomic<bool>& cancelled, const std::function<void()>& hook) {
    CheckCancel(cancelled);
    auto registry = Registry(session);
    const auto review = ChangeReview(session, registry, authorized.id, authorized.action);
    if (review != authorized) Fail("store_review_stale");
    if (!consent.approved) Fail("store_confirmation_required");
    if (review.native_code && !consent.native_execution) Fail("native_confirmation_required");
    if (!review.added_resources.empty() && !consent.added_resources) Fail("resource_confirmation_required");
    StoreChangeResult result;
    result.changed = review.action == StoreChangeAction::Uninstall || review.action == StoreChangeAction::Rollback ||
        (review.action == StoreChangeAction::Enable ? !review.enabled : review.enabled);
    CheckCancel(cancelled);
    if (!result.changed) return result;
    auto packages = Value::Array();
    for (auto item : registry.find("packages")->items()) {
        if (item.get_string("id") == review.id) {
            if (review.action == StoreChangeAction::Uninstall) continue;
            if (review.action == StoreChangeAction::Rollback) {
                const auto target = VerifyStoredPrivate(session, item, review.previous_revision, false, false);
                item.set("name", target.manifest.name);
                item.set("activeRevision", review.previous_revision); item.set("previousRevision", review.active_revision);
            } else item.set("enabled", review.action == StoreChangeAction::Enable);
        }
        packages.push(std::move(item));
    }
    registry.set("packages", std::move(packages));
    auto temporary = WriteTemporary(session.root, json::Serialize(registry), cancelled);
    if (hook) hook();
    CheckCancel(cancelled);
    if (RegistryRevision(session) != review.registry_revision) Fail("store_review_stale");
    if (Exists(session.root / L"registry.json")) { auto pin = Open(session.root / L"registry.json", false); }
    Rename(*temporary, session.root / L"registry.json", true);
    result.restart_required = true; // No checks/cancellation after the commit point.
    return result;
}
struct TrustProposal {
    PublisherTrustReview review;
    std::string original, proposed;
};
TrustProposal Trust(Session& session, const VerifiedPackage& package) {
    TrustProposal proposal;
    auto& review = proposal.review;
    review.root = session.root.native(); review.package_revision = package.Revision();
    review.publisher_id = package.PublisherId(); review.store_exists = session.root_exists;
    Value policy;
    const auto path = session.root / L"signature-policy.json";
    if (session.root_exists && Exists(path)) {
        proposal.original = Read(path, kMaxStoreRegistryBytes);
        std::string error;
        // Reuse the complete policy validator without granting a signing identity.
        const bool valid = VerifyPackageSignature({}, {}, proposal.original, error);
        if ((!valid && error != "signature_required") || !json::Parse(proposal.original, policy)) Fail("invalid_signature_policy:" + error);
        review.policy_revision = PackageRevision(proposal.original);
    } else {
        policy = Value::Object(); policy.set("formatVersion", 1); policy.set("requireSigned", !session.root_exists || session.root_created); policy.set("publishers", Value::Array());
    }
    const auto* entries = policy.find("publishers");
    if (!entries || !entries->is_array()) Fail("invalid_signature_policy");
    auto pins = *entries;
    for (const auto& pin : pins.items()) if (pin.get_string("id") == package.PublisherId()) {
        if (pin.get_string("publicKey") != package.PublisherKey()) Fail("publisher_key_changed");
        review.already_trusted = true;
    }
    if (!review.already_trusted) {
        if (pins.size() >= 32) Fail("publisher_policy_full");
        auto pin = Value::Object(); pin.set("id", package.PublisherId()); pin.set("publicKey", package.PublisherKey()); pins.push(std::move(pin));
        policy.set("publishers", std::move(pins));
    }
    proposal.proposed = review.already_trusted ? proposal.original : json::Serialize(policy);
    if (proposal.proposed.size() > kMaxStoreRegistryBytes) Fail("publisher_policy_size_limit");
    std::string error;
    if (!VerifyPackageSignature(package.ManifestBytes(), package.SignatureBytes(), proposal.proposed, error)) Fail("publisher_proposal_invalid:" + error);
    return proposal;
}
} // namespace
bool ReadInstalledStore(const fs::path& root, InstalledSnapshot& out, std::string& error) {
    error.clear();
    try {
        Session session(root, false, true);
        InstalledSnapshot snapshot; snapshot.root = session.root; snapshot.missing = !session.root_exists;
        if (session.root_exists) {
            const auto registry = Registry(session); snapshot.registry_revision = RegistryRevision(session);
            for (const auto& item : registry.find("packages")->items()) {
                InstalledEntry row;
                row.id = item.get_string("id"); row.name = item.get_string("name"); row.enabled = item.get_bool("enabled");
                row.revision = item.get_string("activeRevision"); row.previous_revision = item.get_string("previousRevision");
                try {
                    const auto metadata = VerifyStoredPrivate(session, item, row.revision, true, false);
                    row.publisher_id = metadata.publisher; row.resources = metadata.manifest.resources;
                    row.native_code = metadata.manifest.native.has_value();
                } catch (const std::exception& e) { row.error = e.what(); }
                snapshot.entries.push_back(std::move(row));
            }
        }
        out = std::move(snapshot); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool ReviewStoreChange(const fs::path& root, const std::string& id, StoreChangeAction action,
    StoreChangeReview& out, std::string& error) {
    error.clear();
    try {
        Session session(root); const auto registry = Registry(session);
        auto review = ChangeReview(session, registry, id, action); out = std::move(review); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
namespace {
bool CommitChangePublic(const fs::path& root, const StoreChangeReview& review, const StoreChangeConsent& consent,
    const std::atomic<bool>& cancelled, StoreChangeResult& out, std::string& error, const std::function<void()>& hook) {
    error.clear();
    try {
        CheckCancel(cancelled);
        if (review.root != root.lexically_normal()) Fail("store_review_stale");
        Session session(root);
        auto result = CommitChange(session, review, consent, cancelled, hook); out = result; return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
}
bool CommitStoreChange(const fs::path& root, const StoreChangeReview& review, const StoreChangeConsent& consent,
    const std::atomic<bool>& cancelled, StoreChangeResult& out, std::string& error) {
    return CommitChangePublic(root, review, consent, cancelled, out, error, {});
}
#ifdef PULSE_EXTENSION_INSTALLED_STORE_TESTING
namespace testing {
bool CommitStoreChangeWithHook(const fs::path& root, const StoreChangeReview& review, const StoreChangeConsent& consent,
    const std::atomic<bool>& cancelled, StoreChangeResult& out, std::string& error, const std::function<void()>& hook) {
    return CommitChangePublic(root, review, consent, cancelled, out, error, hook);
}
}
#endif
bool ReviewPublisherTrust(const fs::path& root, const VerifiedPackage& package, PublisherTrustReview& out, std::string& error) {
    error.clear();
    try {
        if (!package) Fail("invalid_verified_package");
        Session session(root, false, true);
        auto proposal = Trust(session, package); out = std::move(proposal.review); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool CommitPublisherTrust(const fs::path& root, const VerifiedPackage& package, const PublisherTrustReview& review,
    bool approved, const std::atomic<bool>& cancelled, PublisherTrustResult& out, std::string& error) {
    error.clear();
    try {
        CheckCancel(cancelled);
        if (!package) Fail("invalid_verified_package");
        if (!approved) Fail("publisher_confirmation_required");
        if (review.root != root.lexically_normal().native() || review.package_revision != package.Revision() || review.publisher_id != package.PublisherId())
            Fail("publisher_review_stale");
        Session session(root, true);
        CheckCancel(cancelled);
        auto proposal = Trust(session, package);
        if (review.store_exists != !session.root_created || review.policy_revision != proposal.review.policy_revision ||
            review.already_trusted != proposal.review.already_trusted) Fail("publisher_review_stale");
        PublisherTrustResult result; result.root_created = session.root_created;
        if (proposal.proposed != proposal.original) {
            auto temporary = WriteTemporary(session.root, proposal.proposed, cancelled);
            const auto path = session.root / L"signature-policy.json";
            const bool exists = Exists(path);
            if (exists != !proposal.review.policy_revision.empty()) Fail("publisher_review_stale");
            const auto current = exists ? Read(path, kMaxStoreRegistryBytes) : std::string{};
            if (current != proposal.original) Fail("publisher_review_stale");
            CheckCancel(cancelled);
            Rename(*temporary, path, true);
            result.changed = true;
        } else CheckCancel(cancelled);
        out = result; return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool ReviewInstall(const fs::path& root, const VerifiedPackage& package, InstallReview& out, std::string& error) {
    error.clear();
    try {
        if (!package) Fail("invalid_verified_package");
        Session session(root); const auto registry = Registry(session);
        auto review = Review(session, package, registry); out = std::move(review); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
namespace {
bool CommitPublic(const fs::path& root, const VerifiedPackage& package, const InstallReview& review,
    const InstallConsent& consent, const std::atomic<bool>& cancelled, InstallResult& out, std::string& error,
    const std::function<void()>& hook) {
    error.clear();
    try {
        CheckCancel(cancelled); if (!package) Fail("invalid_verified_package");
        Session session(root); auto result = Commit(session, package, review, consent, cancelled, hook);
        out = std::move(result); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
}
bool CommitInstall(const fs::path& root, const VerifiedPackage& package, const InstallReview& review,
    const InstallConsent& consent, const std::atomic<bool>& cancelled, InstallResult& out, std::string& error) {
    return CommitPublic(root, package, review, consent, cancelled, out, error, {});
}
#ifdef PULSE_EXTENSION_INSTALL_TRANSACTION_TESTING
namespace testing {
bool CommitInstallWithHooks(const fs::path& root, const VerifiedPackage& package, const InstallReview& review,
    const InstallConsent& consent, const std::atomic<bool>& cancelled, InstallResult& out, std::string& error,
    const InstallHooks& hooks) {
    struct Scope {
        const InstallHooks* previous = install_hooks;
        explicit Scope(const InstallHooks& value) { install_hooks = &value; }
        ~Scope() { install_hooks = previous; }
    } scope(hooks);
    return CommitPublic(root, package, review, consent, cancelled, out, error, hooks.before_commit);
}
bool CommitInstallWithHook(const fs::path& root, const VerifiedPackage& package, const InstallReview& review,
    const InstallConsent& consent, const std::atomic<bool>& cancelled, InstallResult& out, std::string& error,
    const BeforeCommit& before_commit) {
    return CommitPublic(root, package, review, consent, cancelled, out, error, before_commit);
}
}
#endif
}

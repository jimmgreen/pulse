#include "runtime.h"
#include "developer_cli.h"
#include "audit_diagnostics_cli.h"
#include "development_reload.h"
#include "package_store.h"
#include "store_trust_watch.h"
#include "audit_persistence.h"

#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <unordered_map>

namespace pulse::extensions {
namespace {
struct RuntimePackage {
    Manifest manifest;
    bool enabled = true;
    uint64_t generation = 0;
    std::string revision;
    bool trust_revoked = false;
    std::wstring native_path;
};
std::vector<RuntimePackage> packages;
authorization::Registry callers;
bool identities_ready = false;
std::unordered_map<uintptr_t, authorization::OwnerHandle> owners;
bool SyncCallers() {
    std::vector<authorization::PackageState> states;
    for (size_t i = 0; i < packages.size(); ++i)
        states.push_back({{i, packages[i].manifest.id, packages[i].revision, packages[i].generation}, packages[i].enabled});
    identities_ready = callers.SyncPackages(states);
    if (identities_ready) return true;
    callers.SyncPackages({});
    return false;
}
bool package_store_mode = false;
bool shutting_down = false;
struct DevelopmentConfig {
    std::wstring path;
    std::wstring diagnostic;
    std::string initial_bytes;
};
std::optional<DevelopmentConfig> development;
std::unique_ptr<DevelopmentReload> reload;
std::unique_ptr<StoreTrustWatch> trust_watch;
std::wstring trust_root;
std::vector<StoreTrustSnapshot> trust_snapshots;
uint64_t trust_check_sequence = 0;
uint64_t generation = 0;
uint64_t manifest_generation = 0;

bool LocalDrivePath(const std::filesystem::path& path) {
    const auto root = path.root_name().wstring();
    if (root.size() != 2 || root[1] != L':') return false;
    const UINT type = GetDriveTypeW(path.root_path().c_str());
    return type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_RAMDISK;
}

StartupResult Fail(const std::string& error) {
    const std::string diagnostic = "Pulse extension: " + error + "\n";
    std::fputs(diagnostic.c_str(), stderr);
    OutputDebugStringA(diagnostic.c_str());
    return {true, 2};
}
void RecordDefaultStoreStatus(const std::filesystem::path& store, const std::string& error) {
    const std::string diagnostic = "Pulse extension default store: " +
        (error.empty() ? std::string("loaded") : std::string("disabled: ") + error) + "\n";
    OutputDebugStringA(diagnostic.c_str());
    try {
        auto status = json::Value::Object();
        status.set("formatVersion", 1);
        status.set("status", error.empty() ? "loaded" : "disabled");
        status.set("error", error);
        status.set("packageCount", static_cast<int>(packages.size()));
        FILETIME timestamp{};
        GetSystemTimeAsFileTime(&timestamp);
        const uint64_t ticks = (static_cast<uint64_t>(timestamp.dwHighDateTime) << 32) | timestamp.dwLowDateTime;
        status.set("timestampFileTime", std::to_string(ticks));
        const auto bytes = json::Serialize(status) + "\n";
        const auto path = store.parent_path() / L"extension-startup-status.json";
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            OutputDebugStringA("Pulse extension default store: cannot_write_startup_status\n");
            return;
        }
        BY_HANDLE_FILE_INFORMATION information{};
        DWORD written = 0;
        const bool saved = GetFileType(file) == FILE_TYPE_DISK && GetFileInformationByHandle(file, &information) &&
            !(information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
            SetEndOfFile(file) && WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
            written == bytes.size();
        CloseHandle(file);
        if (!saved) OutputDebugStringA("Pulse extension default store: cannot_write_startup_status\n");
    } catch (...) {
        OutputDebugStringA("Pulse extension default store: cannot_write_startup_status\n");
    }
}
void StartAudit(bool isolated) {
    wchar_t base[32768]{};
    const DWORD length = GetEnvironmentVariableW(isolated ? L"PULSE_TEST_DATA_DIR" : L"LOCALAPPDATA", base, 32768);
    if (!length || length >= 32768) {
        SetPersistentExtensionAuditStartupFailure(true);
        return;
    }
    const auto root = std::filesystem::path(base) /
        (isolated ? std::filesystem::path(L"extension-audit") : std::filesystem::path(L"Pulse") / L"extension-audit");
    try { StartPersistentExtensionAudit(root.wstring()); }
    catch (...) { SetPersistentExtensionAuditStartupFailure(false); }
}
}

std::optional<CallerBinding> CaptureCallerBinding(size_t package, uintptr_t owner) {
    if (!PackageEnabled(package) || !owner) return std::nullopt;
    auto found = owners.find(owner);
    if (found == owners.end()) {
        const auto registered = callers.RegisterOwner(owner);
        if (!registered) return std::nullopt;
        found = owners.emplace(owner, *registered).first;
    }
    const auto& entry = packages[package];
    authorization::PackageIdentity identity{package, entry.manifest.id, entry.revision, entry.generation};
    return callers.CaptureBinding(identity, found->second);
}
bool ValidateCallerBinding(const CallerBinding& binding) { return callers.ValidateBinding(binding); }
authorization::Registry& HostAuthorizationRegistry() { return callers; }
std::optional<CallerLease> IssueCaller(const CallerBinding& binding) {
    const auto ticket = callers.Issue(binding);
    if (!ticket) return std::nullopt;
    return CallerLease{*ticket, binding.Package(), binding.Owner()};
}
std::optional<CallerLease> IssueCaller(size_t package, uintptr_t owner) {
    const auto binding = CaptureCallerBinding(package, owner);
    return binding ? IssueCaller(*binding) : std::nullopt;
}
void ReleaseCallerOwner(uintptr_t owner) {
    const auto found = owners.find(owner);
    if (found == owners.end()) return;
    callers.UnregisterOwner(found->second);
    owners.erase(found);
}
bool ValidateCaller(const CallerLease& caller) {
    return callers.Validate(caller.ticket, caller.package, caller.owner);
}
const Manifest* CurrentManifest() {
    return PackageManifest(0);
}

bool Enabled() {
    return PackageEnabled(0);
}

void SetEnabled(bool value) {
    SetPackageEnabled(0, value);
}

size_t PackageCount() { return packages.size(); }
const Manifest* PackageManifest(size_t package) {
    return package < packages.size() ? &packages[package].manifest : nullptr;
}
std::wstring PackageNativePath(size_t package) {
    return package < packages.size() ? packages[package].native_path : std::wstring{};
}
bool PackageEnabled(size_t package) { return package < packages.size() && packages[package].enabled; }
void SetPackageEnabled(size_t package, bool enabled) {
    if (package >= packages.size()) return;
    if (enabled && (shutting_down || packages[package].trust_revoked)) return;
    packages[package].enabled = enabled;
    SyncCallers();
    if (package == 0 && reload) reload->SetEnabled(enabled);
}
uint64_t PackageGeneration(size_t package) { return package < packages.size() ? packages[package].generation : 0; }
bool PackageStoreMode() { return package_store_mode; }
std::wstring PackageStoreRoot() { return package_store_mode ? trust_root : std::wstring{}; }

void PrepareShutdown() {
    shutting_down = true;
    StopDevelopmentReload();
    for (auto& package : packages) package.enabled = false;
    SyncCallers();
    // No producer retains authority. Complete final JSON/file work while dependencies live.
    StopPersistentExtensionAudit();
}

StartupResult PrepareStartup(int argc, wchar_t** argv) {
    PrepareShutdown();
    shutting_down = false;
    development.reset();
    trust_root.clear();
    trust_snapshots.clear();
    packages.clear();
    SyncCallers();
    package_store_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--extension-script-dev") == 0)
            return Fail("--extension-script-dev has been removed; use a native C++ DLL package.");
    }
    if (const auto exit_code = HandleAuditDiagnosticsCli(argc, argv)) return {true, *exit_code};
    if (const auto exit_code = HandleDeveloperCli(argc, argv)) return {true, *exit_code};
    const wchar_t* manifest_path = nullptr;
    bool check_only = false;
    bool developer_mode = false;
    bool store_mode = false;
    bool isolated = false;
    for (int i = 1; i < argc; ++i) isolated |= std::wcscmp(argv[i], L"--test-instance") == 0;
    for (int i = 1; i < argc; ++i) {
        const bool load = std::wcscmp(argv[i], L"--extension-manifest") == 0;
        const bool check = std::wcscmp(argv[i], L"--extension-check") == 0;
        const bool dev = std::wcscmp(argv[i], L"--extension-dev") == 0;
        const bool store = std::wcscmp(argv[i], L"--extension-store") == 0;
        if (!load && !check && !dev && !store) continue;
        if (manifest_path) return Fail("Specify exactly one extension manifest option.");
        if (i + 1 >= argc || argv[i + 1][0] == L'-')
            return Fail("The extension option requires an absolute manifest path.");
        manifest_path = argv[++i];
        check_only = check;
        developer_mode = dev;
        store_mode = store;
    }
    std::wstring discovered_store;
    if (!manifest_path) {
        wchar_t base[32768]{};
        const DWORD length = GetEnvironmentVariableW(isolated ? L"PULSE_TEST_DATA_DIR" : L"LOCALAPPDATA", base, 32768);
        if (!length || length >= 32768) return {};
        const auto root = std::filesystem::path(base) /
            (isolated ? std::filesystem::path(L"extensions") : std::filesystem::path(L"Pulse") / L"extensions");
        if (!root.is_absolute()) return {};
        std::error_code error;
        if (!std::filesystem::exists(root / L"registry.json", error)) return {};
        discovered_store = root.wstring();
        manifest_path = discovered_store.c_str();
        store_mode = true;
    }
    const auto startup_failure = [&](const std::string& error) -> StartupResult {
        if (discovered_store.empty()) return Fail(error);
        // Automatic discovery is optional. A failed transaction exposes no packages.
        packages.clear();
        SyncCallers();
        package_store_mode = false;
        trust_root.clear();
        trust_snapshots.clear();
        StopPersistentExtensionAudit();
        RecordDefaultStoreStatus(std::filesystem::path(discovered_store), error);
        return {};
    };
    try {
        const std::filesystem::path path(manifest_path);
        if (!path.is_absolute()) return Fail("The manifest path must be absolute.");
        if (store_mode) {
            auto loaded = LoadPackageStore(path.wstring());
            if (!loaded) return startup_failure(loaded.error);
            std::vector<RuntimePackage> next;
            next.reserve(loaded.packages.size());
            for (size_t i = 0; i < loaded.packages.size(); ++i) {
                RuntimePackage entry{std::move(loaded.packages[i]), true, ++manifest_generation, loaded.revisions[i]};
                if (i < loaded.native_paths.size()) entry.native_path = std::move(loaded.native_paths[i]);
                next.push_back(std::move(entry));
            }
            packages = std::move(next);
            if (!SyncCallers()) { packages.clear(); return startup_failure("Cannot establish package identities."); }
            package_store_mode = true;
            trust_root = path.lexically_normal().wstring();
            for (size_t i = 0; i < loaded.manifest_bytes.size(); ++i)
                trust_snapshots.push_back({std::move(loaded.manifest_bytes[i]), std::move(loaded.signatures[i])});
            if (!packages.empty()) StartAudit(isolated);
            if (!discovered_store.empty()) RecordDefaultStoreStatus(path, {});
            return {};
        }
        std::wstring diagnostic;
        if (developer_mode) {
            wchar_t profile[32768]{};
            const DWORD length = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", profile, 32768);
            if (!isolated || !length || length >= 32768)
                return Fail("--extension-dev requires --test-instance and an existing absolute PULSE_TEST_DATA_DIR.");
            const std::filesystem::path directory(profile);
            if (!directory.is_absolute())
                return Fail("PULSE_TEST_DATA_DIR must be an existing absolute directory for extension development.");
            if (!LocalDrivePath(path) || !LocalDrivePath(directory))
                return Fail("Extension development requires local drive paths; UNC and network drives are not supported.");
            if (!std::filesystem::is_directory(directory))
                return Fail("PULSE_TEST_DATA_DIR must be an existing absolute directory for extension development.");
            diagnostic = (directory / L"extension-dev-status.json").wstring();
        }
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) return Fail("Cannot open the manifest file.");
        const auto length = input.tellg();
        if (length < 0 || length > static_cast<std::streamoff>(kMaxManifestBytes))
            return Fail("The manifest exceeds the 64 KiB limit or its size cannot be read.");
        std::string bytes(static_cast<size_t>(length), '\0');
        input.seekg(0);
        if (!bytes.empty() && !input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
            return Fail("Cannot read the complete manifest file.");
        auto result = ParseManifest(bytes);
        if (!result) return Fail(result.error);
        if (check_only) return {true, 0};
        const auto revision = PackageRevision(bytes);
        if (revision.empty()) return Fail("Cannot hash validated manifest.");
        packages.push_back({std::move(*result.manifest), true, ++manifest_generation, revision});
        if (packages[0].manifest.native)
            packages[0].native_path = (path.parent_path() /
                std::filesystem::path(packages[0].manifest.native->library)).lexically_normal().wstring();
        if (!SyncCallers()) { packages.clear(); return Fail("Cannot establish package identity."); }
        if (developer_mode) development = DevelopmentConfig{path.wstring(), std::move(diagnostic), std::move(bytes)};
        StartAudit(isolated);
        return {};
    } catch (const std::exception& error) {
        return startup_failure(error.what());
    }
}

bool DevelopmentMode() { return development.has_value(); }
uint64_t CurrentManifestGeneration() { return PackageGeneration(0); }
bool StartDevelopmentReload() {
    if (shutting_down) return false;
    if (package_store_mode && !trust_watch) {
        try { trust_watch = std::make_unique<StoreTrustWatch>(trust_root, ++generation, trust_snapshots); }
        catch (...) {
            for (size_t i = 0; i < packages.size(); ++i) {
                packages[i].trust_revoked = true;
                SetPackageEnabled(i, false);
            }
            OutputDebugStringA("Pulse extension trust: worker_start_failed\n");
            return false;
        }
    }
    if (!development) return true;
    if (reload) return true;
    auto worker = std::make_unique<DevelopmentReload>();
    if (!worker->Start(++generation, development->path, development->diagnostic, development->initial_bytes, Enabled())) return false;
    reload = std::move(worker);
    return true;
}
bool PollDevelopmentReload(const std::function<void()>& revoke_old_ui) {
    if (trust_watch) {
        auto update = trust_watch->TakeUpdate();
        if (update && update->generation == generation) {
            trust_check_sequence = update->check_sequence;
            bool changed = false;
            for (const auto index : update->revoked) {
                if (index >= packages.size() || packages[index].trust_revoked) continue;
                packages[index].trust_revoked = true;
                SetPackageEnabled(index, false);

                changed = true;
            }
            if (changed && revoke_old_ui) revoke_old_ui();
            if (changed) OutputDebugStringA("Pulse extension trust: active_package_revoked\n");
            if (changed) return true;
        }
    }
    if (!reload) return false;
    auto update = reload->TakeUpdate();
    if (!update || update->generation != generation) return false;
    if (update->identity_failed) {
        SetPackageEnabled(0, false);
        if (revoke_old_ui) revoke_old_ui();
        return false;
    }
    if (!update->manifest) return false;
    // Native modules retain their startup revision until the next process launch.
    if (packages[0].manifest.native || update->manifest->native) {
        OutputDebugStringA("Pulse extension native: manifest_update_requires_restart\n");
        return false;
    }
    // No UI ever sees the replacement while a popup still owns old command IDs.
    if (revoke_old_ui) revoke_old_ui();
    if (!reload || update->generation != generation) return false;
    return reload->Commit(*update, Enabled(), [&] {
        packages[0].manifest = std::move(*update->manifest);
        packages[0].generation = ++manifest_generation;
        packages[0].revision = update->content_revision;
        SyncCallers();
    });
}
void StopDevelopmentReload() {
    ++generation;
    trust_watch.reset();
    trust_check_sequence = 0;
    reload.reset();
}
uint64_t StoreTrustCheckSequence() { return trust_check_sequence; }
json::Value DevelopmentStatus() {
    return reload ? reload->Status() : json::Value::Object();
}

}

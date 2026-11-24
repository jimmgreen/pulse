#include "../app/extensions/store/install_service.h"
#include "../app/extensions/store/installed_store.h"
#include "../app/extensions/resource_settings.h"
#include "../app/extensions/package_store.h"
#include "../app/extension_store_permissions.h"
#include "extension_package_fixture.h"
#include "pulse_version.h"
#include "../../sdk/native/include/pulse/abi.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
using namespace pulse::extensions::store;
using namespace package_fixture;
using namespace std::chrono_literals;
namespace {
int passed = 0, failed = 0;
void Check(bool ok, const char* name, const std::string& error = {}) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name;
    if (!ok) std::cout << " : " << error;
    std::cout << '\n';
    ok ? ++passed : ++failed;
}
void Write(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("fixture_write_failed");
}
std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
struct Held {
    HANDLE value = INVALID_HANDLE_VALUE;
    void Close() { if (value != INVALID_HANDLE_VALUE) { CloseHandle(value); value = INVALID_HANDLE_VALUE; } }
    void Open(const fs::path& path) {
        Close();
        value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (value == INVALID_HANDLE_VALUE) throw std::runtime_error("fixture_reader_open_failed");
    }
    ~Held() { Close(); }
};
VerifiedPackage VerifyPackage(Fixture& fixture) {
    VerifiedPackage package;
    std::string error;
    if (!Verify(fixture, package, error)) throw std::runtime_error(error);
    return package;
}
bool HasTemporary(const fs::path& root) {
    for (const auto& item : fs::recursive_directory_iterator(root)) {
        const auto name = item.path().filename().wstring();
        if (name.starts_with(L".install-") || name.starts_with(L".package-install-") ||
            name.starts_with(L".local-install-") || name.starts_with(L".local-pkg-")) return true;
    }
    return false;
}
void Signed(const fs::path& base) {
    Key key;
    Fixture legacy_fixture(key, 1, Pe(), "pulse.7zip", "7-Zip development build");
    Fixture next_fixture(key, 3, Pe(), "pulse.7zip", "7-Zip release build");
    const auto legacy = VerifyPackage(legacy_fixture), next = VerifyPackage(next_fixture);
    const auto root = base / L"signed-profile-中文" / L"extensions";
    Write(root / L"signature-policy.json", Policy(key.publisher));
    std::atomic<bool> cancel = false;
    InstallReview review;
    InstallResult output;
    std::string error;
    const InstallConsent consent{true, true, true, true};
    if (!ReviewInstall(root, legacy, review, error) || !CommitInstall(root, legacy, review, consent, cancel, output, error))
        throw std::runtime_error(error);
    const auto old_registry = Read(root / L"registry.json");
    const auto old_dll = root / L"binaries" / (legacy.Declaration().native->sha256 + ".dll");
    Held legacy_reader;
    legacy_reader.Open(old_dll);
    auto settings = pulse::extensions::WriteOwnSetting(root.parent_path().wstring(), "pulse.7zip", "keep", "yes", 0, [] { return true; });
    const auto before_settings = pulse::json::Serialize(pulse::extensions::ReadOwnSettings(root.parent_path().wstring(), "pulse.7zip"));
    Check(pulse::json::Serialize(settings).find("yes") != std::string::npos, "prepare real existing plugin settings");
    if (!ReviewInstall(root, next, review, error)) throw std::runtime_error(error);
    Held reader;
    unsigned retries = 0;
    testing::InstallHooks hooks;
    hooks.before_commit = [&] { reader.Open(root / L"registry.json"); };
    hooks.rename_retry = [&](const fs::path& target, unsigned long code) {
        Check(target == root / L"registry.json" && (code == 5 || code == 32 || code == 33),
            "real registry publication conflict reaches retry policy");
        ++retries;
        reader.Close();
    };
    const bool installed = testing::CommitInstallWithHooks(root, next, review, consent, cancel, output, error, hooks);
    reader.Close();
    Check(installed && retries > 0, "old single-DLL to multi-file upgrade retries transient registry reader", error);
    if (!installed) {
        Check(Read(root / L"registry.json") == old_registry, "baseline failure preserves old registry");
        return;
    }
    Check(output.previous_revision == legacy.Revision() && output.restart_required && output.enabled,
        "upgrade retains previous revision and explicitly requests restart");
    Check(Read(old_dll) == legacy.Files()[0].bytes && fs::exists(root / L"packages" / next.Revision() / L"test.dll"),
        "old DLL may remain open without overwriting it or blocking new payload");
    Check(before_settings == pulse::json::Serialize(pulse::extensions::ReadOwnSettings(root.parent_path().wstring(), "pulse.7zip")),
        "upgrade preserves existing plugin settings");
    const auto loaded = pulse::extensions::LoadPackageStore(root.wstring());
    Check(loaded && loaded.packages.size() == 1, "committed store remains loadable", loaded.error);
    // A permanent reader must stay an atomic failure, with actionable diagnostics.
    const auto committed_registry = Read(root / L"registry.json");
    if (!ReviewInstall(root, legacy, review, error)) throw std::runtime_error(error);
    output.id = "sentinel";
    hooks.before_commit = [&] { reader.Open(root / L"registry.json"); };
    hooks.rename_retry = {};
    const auto start = std::chrono::steady_clock::now();
    const bool persistent = testing::CommitInstallWithHooks(root, legacy, review, consent, cancel, output, error, hooks);
    const auto duration = std::chrono::steady_clock::now() - start;
    reader.Close();
    Check(!persistent && duration < 4s && error.starts_with("install_atomic_rename:") && error.find(":target=") != std::string::npos,
        "persistent conflict fails within bounded time and identifies target", error);
    Check(output.id == "sentinel" && Read(root / L"registry.json") == committed_registry && !HasTemporary(root),
        "persistent conflict preserves live revision, output and cleans staging");
    // Cancellation during a registry retry must not cross the commit point.
    hooks.rename_retry = [&](const fs::path&, unsigned long) { cancel = true; reader.Close(); };
    const bool cancelled = testing::CommitInstallWithHooks(root, legacy, review, consent, cancel, output, error, hooks);
    reader.Close();
    Check(!cancelled && error == "install_cancelled" && Read(root / L"registry.json") == committed_registry && !HasTemporary(root),
        "cancellation during registry retry leaves live revision untouched", error);
    cancel = false;
    StoreChangeReview change;
    StoreChangeResult changed;
    Check(ReviewStoreChange(root, "pulse.7zip", StoreChangeAction::Rollback, change, error) &&
        CommitStoreChange(root, change, {true, true, true, false}, cancel, changed, error) &&
        pulse::extensions::LoadPackageStore(root.wstring()), "previous legacy revision remains available for real rollback", error);

    // Closed write-handle regression: simulated AV scanner opens staged single-DLL .install-* immediately after write
    // with GENERIC_READ | FILE_SHARE_READ (which previously failed under dwShareMode = 0), then releases on retry.
    const auto single_dll_root = base / L"signed-single-dll-after-write" / L"extensions";
    Write(single_dll_root / L"signature-policy.json", Policy(key.publisher));
    InstallReview single_review;
    InstallResult single_output;
    if (!ReviewInstall(single_dll_root, legacy, single_review, error)) throw std::runtime_error(error);
    Held temp_scanner;
    bool temp_opened = false;
    unsigned temp_retries = 0;
    testing::InstallHooks single_hooks;
    single_hooks.after_temp_write = [&](const fs::path& staged_path) {
        if (!temp_opened && staged_path.parent_path().filename() == L"binaries") {
            temp_scanner.Open(staged_path);
            temp_opened = true;
        }
    };
    single_hooks.rename_retry = [&](const fs::path& target, unsigned long code) {
        if (target.extension() == L".dll" && (code == 5 || code == 32 || code == 33)) {
            if (++temp_retries == 2) temp_scanner.Close();
        }
    };
    const bool single_installed = testing::CommitInstallWithHooks(
        single_dll_root, legacy, single_review, consent, cancel, single_output, error, single_hooks);
    temp_scanner.Close();
    Check(single_installed && temp_opened && temp_retries == 2 && !HasTemporary(single_dll_root) &&
        pulse::extensions::LoadPackageStore(single_dll_root.wstring()),
        "single-DLL install closes write handle before rename and tolerates AV shared-read lock", error);
}
void Local(const fs::path& base) {
    Key key;
    Fixture old_fixture(key, 1, Pe(), "pulse.7zip", "Local 7-Zip old build");
    Fixture new_fixture(key, 3, Pe(), "pulse.7zip", "Local 7-Zip new build");
    Fixture busy_new_fixture(key, 32, Pe(), "pulse.7zip", "Local 7-Zip scanner stress build");
    const auto old_archive = base / L"archives" / L"pulse.7zip-1.0.1.pulse-ext";
    const auto new_archive = base / L"archives" / L"pulse.7zip-1.0.2.pulse-ext";
    const auto busy_archive = base / L"archives" / L"pulse.7zip-1.0.3.pulse-ext";
    Write(old_archive, MakeZip(old_fixture.files).bytes);
    Write(new_archive, MakeZip(new_fixture.files).bytes);
    Write(busy_archive, MakeZip(busy_new_fixture.files).bytes);
    const auto root = base / L"unsigned-profile" / L"extensions";
    InstallService service({root, PULSE_VERSION_STRING_A, static_cast<int>(PULSE_SDK_PREVIEW)});
    const auto wait = [&](uint64_t generation, InstallServicePhase phase) { return service.WaitForPhase(generation, phase, 6s); };
    const InstallConsent consent{true, true, true, true};
    const auto first = service.PrepareLocal(old_archive);
    const bool initial = wait(first, InstallServicePhase::AwaitingInstall) && service.ConfirmInstall(first, consent) && wait(first, InstallServicePhase::Installed);
    Check(initial, "old unsigned local development package installs with explicit consent", service.GetSnapshot().error);
    if (!initial) return;
    const auto old_registry = Read(root / L"registry.json");
    const auto next = service.PrepareLocal(new_archive);
    if (!wait(next, InstallServicePhase::AwaitingInstall)) throw std::runtime_error(service.GetSnapshot().error);
    Held reader;
    reader.Open(root / L"registry.json");
    HANDLE held = reader.value;
    reader.value = INVALID_HANDLE_VALUE;
    std::thread release([held] { Sleep(300); CloseHandle(held); });
    const bool installed = service.ConfirmInstall(next, consent) && wait(next, InstallServicePhase::Installed);
    release.join();
    Check(installed, "unsigned local upgrade also survives a real transient registry reader", service.GetSnapshot().error);
    if (installed) {
        Check(service.GetSnapshot().pending_restart && service.GetSnapshot().enabled && Read(root / L"registry.json") != old_registry,
            "local upgrade atomically commits and reports pending restart");
        Check(!HasTemporary(root), "local upgrade leaves no staged temporary files");
    }

    // Exercise concurrent reader against unsigned multi-file .local-pkg-*/test.dll during upgrade.
    const auto scanner_root = base / L"unsigned-profile-scanner" / L"extensions";
    InstallService scanner_service({scanner_root, PULSE_VERSION_STRING_A, static_cast<int>(PULSE_SDK_PREVIEW)});
    const auto scan_first = scanner_service.PrepareLocal(old_archive);
    if (!scanner_service.WaitForPhase(scan_first, InstallServicePhase::AwaitingInstall, 6s) ||
        !scanner_service.ConfirmInstall(scan_first, consent) ||
        !scanner_service.WaitForPhase(scan_first, InstallServicePhase::Installed, 6s)) {
        Check(false, "prepare unsigned scanner baseline", scanner_service.GetSnapshot().error);
        return;
    }
    const auto scan_next = scanner_service.PrepareLocal(busy_archive);
    if (!scanner_service.WaitForPhase(scan_next, InstallServicePhase::AwaitingInstall, 6s)) {
        Check(false, "prepare unsigned scanner upgrade", scanner_service.GetSnapshot().error);
        return;
    }
    std::atomic<bool> stop_watch = false;
    std::atomic<bool> watcher_ready = false;
    std::thread scanner_watch([&] {
        watcher_ready.store(true);
        const auto packages_dir = scanner_root / L"packages";
        while (!stop_watch.load()) {
            std::error_code ec;
            for (fs::directory_iterator it(packages_dir, ec), end; it != end && !stop_watch.load(); it.increment(ec)) {
                if (ec) break;
                const auto name = it->path().filename().wstring();
                if (!name.starts_with(L".local-pkg-")) continue;
                const HANDLE h = CreateFileW((it->path() / L"test.dll").c_str(), GENERIC_READ, FILE_SHARE_READ,
                    nullptr, OPEN_EXISTING, 0, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    Sleep(250);
                    CloseHandle(h);
                    return;
                }
            }
            Sleep(1);
        }
    });
    while (!watcher_ready.load()) Sleep(1);
    const bool scanner_installed = scanner_service.ConfirmInstall(scan_next, consent) &&
        scanner_service.WaitForPhase(scan_next, InstallServicePhase::Installed, 10s);
    stop_watch.store(true);
    scanner_watch.join();
    Check(scanner_installed && !HasTemporary(scanner_root) &&
        pulse::extensions::LoadPackageStore(scanner_root.wstring()),
        "unsigned multi-file upgrade survives concurrent staging DLL reader", scanner_service.GetSnapshot().error);
}
void Payload(const fs::path& base) {
    Key key;
    Fixture fixture(key, 3, Pe(), "pulse.7zip", "7-Zip multi-file package");
    const auto package = VerifyPackage(fixture);
    std::atomic<bool> cancel = false;
    std::string error;
    InstallReview review;
    InstallResult output;
    const InstallConsent consent{true, true, true, true};
    const auto root = base / L"payload-reader";
    Write(root / L"signature-policy.json", Policy(key.publisher));
    if (!ReviewInstall(root, package, review, error)) throw std::runtime_error(error);
    Held reader;
    unsigned retries = 0;
    testing::InstallHooks hooks;
    hooks.before_payload_publish = [&](const fs::path& staging) { reader.Open(staging / L"test.dll"); };
    hooks.rename_retry = [&](const fs::path&, unsigned long) { if (++retries == 10) reader.Close(); };
    const bool installed = testing::CommitInstallWithHooks(root, package, review, consent, cancel, output, error, hooks);
    reader.Close();
    Check(installed && retries == 10, "multi-file publication tolerates scanner longer than old 900ms window", error);
    Check(!HasTemporary(root), "multi-file publication removes no live files and leaves no staging");

    // Verify cold AV DLL scan (> 2,000ms across 14 retries, beyond the old 1,875ms limit) still succeeds.
    const auto long_scan_root = base / L"payload-long-scan";
    Write(long_scan_root / L"signature-policy.json", Policy(key.publisher));
    InstallReview long_review;
    if (!ReviewInstall(long_scan_root, package, long_review, error)) throw std::runtime_error(error);
    unsigned long_retries = 0;
    testing::InstallHooks long_hooks;
    long_hooks.before_payload_publish = [&](const fs::path& staging) { reader.Open(staging / L"test.dll"); };
    long_hooks.rename_retry = [&](const fs::path&, unsigned long) { if (++long_retries == 14) reader.Close(); };
    const bool long_installed = testing::CommitInstallWithHooks(
        long_scan_root, package, long_review, consent, cancel, output, error, long_hooks);
    reader.Close();
    Check(long_installed && long_retries == 14 && !HasTemporary(long_scan_root),
        "multi-file publication tolerates cold AV scan exceeding 2s budget", error);

    const auto cancelled_root = base / L"payload-cancel";
    Write(cancelled_root / L"signature-policy.json", Policy(key.publisher));
    if (!ReviewInstall(cancelled_root, package, review, error)) throw std::runtime_error(error);
    hooks.rename_retry = [&](const fs::path&, unsigned long) { cancel = true; reader.Close(); };
    output.id = "sentinel";
    const bool committed = testing::CommitInstallWithHooks(cancelled_root, package, review, consent, cancel, output, error, hooks);
    reader.Close();
    Check(!committed && error == "install_cancelled" && output.id == "sentinel" &&
        !fs::exists(cancelled_root / L"registry.json") && !HasTemporary(cancelled_root),
        "payload retry remains cancellable and never activates a partial package", error);
}
void Messages() {
    using Id = pulse::l10n::StringId;
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    for (const auto* language : {L"zh-CN", L"en-US", L"zh-TW"}) {
        pulse::l10n::SetLanguage(language);
        const auto message = pulse::app::LocalizedStoreInstallError("install_atomic_rename:32:target=C:\\测试\\signature-policy.json");
        Check(!pulse::l10n::Get(Id::StoreErrorInstallBusy).empty() && message.find(pulse::l10n::Get(Id::StoreErrorInstallBusy)) == 0 && message.find(L"C:\\测试\\signature-policy.json") != std::wstring::npos,
            "sharing error is localized with Unicode target, not mistaken for signature failure");
        const auto denied = pulse::app::LocalizedStoreInstallError("install_atomic_rename:5");
        Check(denied.find(pulse::l10n::Get(Id::StoreErrorInstallDenied)) == 0,
            "legacy error5 gains an actionable localized explanation");
        const auto locked = pulse::app::LocalizedStoreInstallError("install_atomic_rename:33");
        Check(locked.find(pulse::l10n::Get(Id::StoreErrorInstallBusy)) == 0,
            "legacy error33 is recognized without target suffix");
    }
    Check(pulse::app::LocalizedStoreInstallError("install_atomic_rename:320") == L"install_atomic_rename:320",
        "unrecognized error numbers do not get misclassified as sharing errors");
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    std::cout << std::unitbuf;
    if (argc != 1 && argc != 3) return 2;
    try {
        const auto root = argc == 3 ? fs::absolute(argv[1]) : fs::temp_directory_path() /
            (L"pulse-install-retry-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        if (fs::exists(root)) throw std::runtime_error("fixture_must_be_new");
        fs::create_directories(root);
        const std::wstring mode = argc == 3 ? argv[2] : L"all";
        if (mode != L"signed" && mode != L"local" && mode != L"payload" && mode != L"messages" && mode != L"all") return 2;
        if (mode == L"signed" || mode == L"all") Signed(root);
        if (mode == L"local" || mode == L"all") Local(root);
        if (mode == L"payload" || mode == L"all") Payload(root);
        if (mode == L"messages" || mode == L"all") Messages();
    } catch (const std::exception& e) { Check(false, "unhandled fixture exception", e.what()); }
    std::cout << "Summary: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}

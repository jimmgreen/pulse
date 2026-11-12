// Headless Local .pulse-ext Verification, Install Service & Permission Localization Suite.
// Zero USER32/GDI32/SHELL32 imports; safe for Low Integrity execution.
#include "../common/windows_compat.h"
#include "../app/extension_store_permissions.h"
#include "../app/extensions/manifest_schema.generated.h"
#include "../app/extensions/package_store.h"
#include "../app/extensions/store/install_service.h"
#include "../app/extensions/store/installed_store.h"
#include "extension_package_fixture.h"
#include "pulse_version.h"
#include "../../sdk/native/include/pulse/abi.h"
#include <winioctl.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pulse::extensions::store;
using namespace package_fixture;
using Phase = InstallServicePhase;
using namespace std::chrono_literals;

namespace {

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const char* label, const std::string& detail = {}) {
    std::printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", label,
        ok || detail.empty() ? "" : ": ", ok ? "" : detail.c_str());
    if (ok) ++g_passed;
    else ++g_failed;
}

bool WriteBytes(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    return static_cast<bool>(out);
}

std::string PeExe() {
    std::string s = Pe();
    W16(s, 86, IMAGE_FILE_EXECUTABLE_IMAGE);
    return s;
}

struct LocalArchiveSpec {
    std::string id = "org.test.local";
    std::string name = "Local Extension 本地扩展";
    std::vector<std::string> resources = {"files.readCurrentDirectory", "operations.own", "settings.own"};
    std::string dll_bytes = Pe();
    std::vector<std::pair<std::string, std::string>> sidecars;
    std::string manifest_bytes;
    std::string archive_bytes;
};

LocalArchiveSpec BuildNativeArchive(std::string id = "org.test.local",
                                    std::vector<std::pair<std::string, std::string>> sidecars = {},
                                    std::string dll_bytes = Pe(),
                                    bool force_bad_dll_hash = false) {
    LocalArchiveSpec spec;
    spec.id = std::move(id);
    spec.dll_bytes = std::move(dll_bytes);
    spec.sidecars = std::move(sidecars);

    Value m = Value::Object();
    m.set("apiVersion", 1);
    m.set("id", spec.id);
    m.set("name", spec.name);
    Value res = Value::Array();
    for (const auto& r : spec.resources) res.push(r);
    m.set("resources", res);

    std::string declared_dll_hash = PackageRevision(spec.dll_bytes);
    if (force_bad_dll_hash) declared_dll_hash[0] = (declared_dll_hash[0] == 'a') ? 'b' : 'a';

    Value native = Value::Object();
    native.set("version", 1);
    native.set("architecture", "windows-x64");
    native.set("library", "test.dll");
    native.set("sha256", declared_dll_hash);

    Value meta_native = Value::Object();
    meta_native.set("library", "test.dll");
    meta_native.set("sha256", declared_dll_hash);

    std::vector<ZipFile> zfiles;
    zfiles.push_back({"test.dll", spec.dll_bytes});

    if (!spec.sidecars.empty()) {
        Value files_arr = Value::Array();
        for (const auto& [rel_path, content] : spec.sidecars) {
            Value entry = Value::Object();
            entry.set("path", rel_path);
            entry.set("sha256", PackageRevision(content));
            files_arr.push(std::move(entry));
            zfiles.push_back({rel_path, content});
        }
        native.set("files", files_arr);
        meta_native.set("files", std::move(files_arr));
    }

    m.set("native", std::move(native));
    spec.manifest_bytes = pulse::json::Serialize(m);
    const std::string rev = PackageRevision(spec.manifest_bytes);

    Value metadata = Value::Object();
    metadata.set("format", "pulse-extension-local");
    metadata.set("formatVersion", 2);
    metadata.set("apiVersion", 1);
    metadata.set("id", spec.id);
    metadata.set("manifestSha256", rev);
    metadata.set("native", std::move(meta_native));

    zfiles.insert(zfiles.begin(), {"package.json", pulse::json::Serialize(metadata)});
    zfiles.insert(zfiles.begin(), {"manifest.json", spec.manifest_bytes});
    spec.archive_bytes = MakeZip(zfiles).bytes;
    return spec;
}

InstallServiceConfig MakeConfig(const fs::path& store_root) {
    return {store_root, PULSE_VERSION_STRING_A, static_cast<int>(PULSE_SDK_PREVIEW)};
}

bool WaitPhase(InstallService& service, uint64_t generation, Phase expected) {
    if (!service.WaitForPhase(generation, expected, 5s)) return false;
    const auto snap = service.GetSnapshot();
    return snap.generation == generation && snap.phase == expected;
}

#pragma pack(push, 1)
struct MountPointReparseBuffer {
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    USHORT SubstituteNameOffset;
    USHORT SubstituteNameLength;
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
    WCHAR PathBuffer[1024];
};
#pragma pack(pop)

bool CreateDirectoryJunction(const fs::path& junction, const fs::path& target) {
    if (!CreateDirectoryW(junction.c_str(), nullptr)) return false;
    HANDLE dir = CreateFileW(junction.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (dir == INVALID_HANDLE_VALUE) return false;
    const std::wstring subst = L"\\??\\" + fs::absolute(target).wstring();
    const std::wstring print = fs::absolute(target).wstring();
    MountPointReparseBuffer buf{};
    buf.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    const auto subst_bytes = static_cast<USHORT>(subst.size() * sizeof(wchar_t));
    const auto print_bytes = static_cast<USHORT>(print.size() * sizeof(wchar_t));
    if (static_cast<size_t>(subst_bytes) + print_bytes + 4 > sizeof(buf.PathBuffer)) {
        CloseHandle(dir);
        return false;
    }
    buf.SubstituteNameOffset = 0;
    buf.SubstituteNameLength = subst_bytes;
    buf.PrintNameOffset = static_cast<USHORT>(subst_bytes + sizeof(wchar_t));
    buf.PrintNameLength = print_bytes;
    std::memcpy(buf.PathBuffer, subst.c_str(), subst_bytes);
    std::memcpy(reinterpret_cast<char*>(buf.PathBuffer) + buf.PrintNameOffset, print.c_str(), print_bytes);
    buf.ReparseDataLength = static_cast<USHORT>(8 + subst_bytes + sizeof(wchar_t) + print_bytes + sizeof(wchar_t));
    DWORD returned = 0;
    const DWORD total_size = static_cast<DWORD>(8 + buf.ReparseDataLength);
    const BOOL ok = DeviceIoControl(dir, FSCTL_SET_REPARSE_POINT, &buf, total_size, nullptr, 0, &returned, nullptr);
    CloseHandle(dir);
    return ok != FALSE;
}

struct ScopedTempDir {
    fs::path path;
    ~ScopedTempDir() {
        if (!path.empty()) {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }
};

} // namespace

int wmain() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t mod_buf[32768]{};
    GetModuleFileNameW(nullptr, mod_buf, 32768);
    ScopedTempDir temp{
        fs::path(mod_buf).parent_path() /
        (L"local-inst-work-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()))};
    fs::create_directories(temp.path);

    try {
        // 1. Valid unsigned local .pulse-ext (single-DLL AND multi-file .dll + .exe)
        {
            const fs::path store_root = temp.path / L"store-unsigned";
            const fs::path single_pkg_path = temp.path / L"pkgs" / L"org.test.single-1.2.3.pulse-ext";
            const auto single_spec = BuildNativeArchive("org.test.single");
            Check(WriteBytes(single_pkg_path, single_spec.archive_bytes), "write single-DLL unsigned .pulse-ext");

            InstallService service(MakeConfig(store_root));
            const auto g1 = service.PrepareLocal(single_pkg_path);
            Check(WaitPhase(service, g1, Phase::AwaitingInstall),
                "unsigned single-DLL PrepareLocal reaches AwaitingInstall without PublisherTrust step",
                service.GetSnapshot().error);
            auto snap = service.GetSnapshot();
            Check(snap.is_local_package && snap.package && snap.package->PublisherId().empty() &&
                  snap.version == "1.2.3" && snap.install_review && snap.install_review->native_code &&
                  snap.install_review->added_resources.size() == 3,
                "unsigned snapshot marks is_local_package, empty PublisherId, extracted version, and added resources");

            // Missing native consent must fail closed.
            Check(service.ConfirmInstall(g1, InstallConsent{true, false, true, true}) &&
                  WaitPhase(service, g1, Phase::Failed) &&
                  service.GetSnapshot().error == "native_confirmation_required",
                "unsigned local install rejects missing native execution consent");
            Check(!fs::exists(store_root / L"registry.json"), "rejected consent writes no registry.json");

            // Missing resource consent must fail closed.
            const auto g2 = service.PrepareLocal(single_pkg_path);
            Check(WaitPhase(service, g2, Phase::AwaitingInstall) &&
                  service.ConfirmInstall(g2, InstallConsent{true, true, false, true}) &&
                  WaitPhase(service, g2, Phase::Failed) &&
                  service.GetSnapshot().error == "resource_confirmation_required",
                "unsigned local install rejects missing added_resources consent");

            // Full consent commits atomically to isolated store.
            const auto g3 = service.PrepareLocal(single_pkg_path);
            Check(WaitPhase(service, g3, Phase::AwaitingInstall) &&
                  service.ConfirmInstall(g3, InstallConsent{true, true, true, true}) &&
                  WaitPhase(service, g3, Phase::Installed),
                "unsigned single-DLL local install commits with full explicit consent",
                service.GetSnapshot().error);
            snap = service.GetSnapshot();
            Check(snap.enabled && snap.pending_restart, "unsigned native install sets enabled and pending_restart");
            Check(!fs::exists(store_root / L"versions" / (PackageRevision(single_spec.manifest_bytes) + ".sig.json")),
                "unsigned local install writes no fabricated .sig.json");

            // Multi-file (.dll + .exe + data.txt) unsigned local .pulse-ext.
            const fs::path multi_pkg_path = temp.path / L"pkgs" / L"org.test.multi-2.0.0.pulse-ext";
            const auto multi_spec = BuildNativeArchive("org.test.multi",
                {{"worker.exe", PeExe()}, {"config.json", "{\"ok\":true}"}});
            Check(WriteBytes(multi_pkg_path, multi_spec.archive_bytes), "write multi-file (.dll + .exe) unsigned .pulse-ext");

            const auto g4 = service.PrepareLocal(multi_pkg_path);
            Check(WaitPhase(service, g4, Phase::AwaitingInstall) &&
                  service.ConfirmInstall(g4, InstallConsent{true, true, true, true}) &&
                  WaitPhase(service, g4, Phase::Installed),
                "unsigned multi-file (.dll + .exe) local install commits atomically",
                service.GetSnapshot().error);

            const auto loaded = pulse::extensions::LoadPackageStore(store_root.wstring());
            Check(loaded.error.empty() && loaded.packages.size() == 2 &&
                  loaded.publisher_ids.size() == 2 &&
                  loaded.publisher_ids[0].empty() && loaded.publisher_ids[1].empty() &&
                  loaded.native_paths.size() == 2 &&
                  !loaded.native_paths[0].empty() && !loaded.native_paths[1].empty(),
                "LoadPackageStore loads both committed unsigned single-DLL and multi-file packages cleanly",
                loaded.error);

            // Upgrading an unsigned local package with a newer unsigned local package succeeds.
            auto upgraded_dll = Pe();
            upgraded_dll[420] = 'U';
            const fs::path upgrade_pkg_path = temp.path / L"pkgs" / L"org.test.single-1.3.0.pulse-ext";
            const auto upgrade_spec = BuildNativeArchive("org.test.single", {}, upgraded_dll);
            Check(WriteBytes(upgrade_pkg_path, upgrade_spec.archive_bytes), "write upgraded unsigned .pulse-ext");
            const auto g5 = service.PrepareLocal(upgrade_pkg_path);
            Check(WaitPhase(service, g5, Phase::AwaitingInstall) &&
                  !service.GetSnapshot().install_review->active_revision.empty() &&
                  service.GetSnapshot().install_review->added_resources.empty() &&
                  service.ConfirmInstall(g5, InstallConsent{true, true, false, false}) &&
                  WaitPhase(service, g5, Phase::Installed),
                "upgrading existing unsigned package with newer unsigned package preserves continuity",
                service.GetSnapshot().error);
        }

        // 2. Valid signed local .pulse-ext + adjacent .sig.json -> PublisherTrust -> Install
        Key author_key;
        const fs::path shared_signed_store = temp.path / L"store-signed";
        {
            const auto signed_spec = BuildNativeArchive("org.test.signed", {{"helper.exe", PeExe()}});
            const fs::path pkg_path = temp.path / L"signed" / L"org.test.signed-1.0.0.pulse-ext";
            const fs::path sig_path = fs::path(pkg_path.native() + L".sig.json");
            const fs::path pub_path = fs::path(pkg_path.native() + L".pub");
            Check(WriteBytes(pkg_path, signed_spec.archive_bytes) &&
                  WriteBytes(sig_path, author_key.Sign(signed_spec.manifest_bytes)) &&
                  WriteBytes(pub_path, author_key.publisher.public_key),
                "write signed local .pulse-ext with adjacent .sig.json and .pub");

            InstallService service(MakeConfig(shared_signed_store));
            const auto g1 = service.PrepareLocal(pkg_path);
            Check(WaitPhase(service, g1, Phase::AwaitingPublisherTrust),
                "signed local .pulse-ext stops at AwaitingPublisherTrust for unpinned key",
                service.GetSnapshot().error);
            auto snap = service.GetSnapshot();
            Check(snap.is_local_package && snap.package &&
                  snap.package->PublisherId() == author_key.publisher.id,
                "signed local .pulse-ext exposes verified PublisherId");
            Check(service.ConfirmPublisherTrust(g1, true) &&
                  WaitPhase(service, g1, Phase::AwaitingInstall),
                "ConfirmPublisherTrust(true) pins key and transitions to AwaitingInstall",
                service.GetSnapshot().error);
            Check(service.ConfirmInstall(g1, InstallConsent{true, true, true, true}) &&
                  WaitPhase(service, g1, Phase::Installed),
                "ConfirmInstall commits signed local .pulse-ext and .sig.json",
                service.GetSnapshot().error);
            const std::string rev = PackageRevision(signed_spec.manifest_bytes);
            Check(fs::exists(shared_signed_store / L"versions" / (rev + ".sig.json")) &&
                  fs::exists(shared_signed_store / L"signature-policy.json"),
                "signed local commit persists detached .sig.json and signature-policy.json");

            // Also verify embedded publicKey in .sig.json works without a separate .pub file.
            Value sig_with_pk;
            Require(pulse::json::Parse(author_key.Sign(signed_spec.manifest_bytes), sig_with_pk));
            sig_with_pk.set("publicKey", author_key.publisher.public_key);
            const fs::path pkg2 = temp.path / L"signed-inline" / L"org.test.signed-1.0.0.pulse-ext";
            Check(WriteBytes(pkg2, signed_spec.archive_bytes) &&
                  WriteBytes(fs::path(pkg2.native() + L".sig.json"), pulse::json::Serialize(sig_with_pk)),
                "write signed local .pulse-ext with inline publicKey in .sig.json");
            std::atomic<bool> cancel = false;
            VerifiedPackage verified_inline;
            std::string verify_err;
            Check(VerifyLocalPackage(pkg2, cancel, verified_inline, verify_err) &&
                  verified_inline && verified_inline.PublisherId() == author_key.publisher.id,
                "VerifyLocalPackage accepts inline publicKey matching publisher fingerprint and canonicalizes signature",
                verify_err);
        }

        // 3. Official-key signed local .pulse-ext exposes PublisherId() == kOfficialCatalogPublisher (pin seam + anti-spoof)
        {
            Key official_sim_key;
            const auto off_spec = BuildNativeArchive("pulse.official.sample");
            const fs::path off_pkg = temp.path / L"official" / L"pulse.official.sample-1.0.0.pulse-ext";
            // Write ONLY 4-field .sig.json — NO .pub file, NO publicKey field, NO known_publishers!
            Check(WriteBytes(off_pkg, off_spec.archive_bytes) &&
                  WriteBytes(fs::path(off_pkg.native() + L".sig.json"), official_sim_key.Sign(off_spec.manifest_bytes)),
                "write official-pin-style .pulse-ext + 4-field .sig.json without external key hints");
            std::atomic<bool> cancel = false;
            VerifiedPackage verified_official;
            std::string err;
            Check(testing::VerifyLocalPackageWithOfficialPin(
                      off_pkg, LocalPackageVerifyOptions{},
                      official_sim_key.publisher.id, official_sim_key.publisher.public_key,
                      cancel, verified_official, err) &&
                  verified_official && verified_official.PublisherId() == official_sim_key.publisher.id,
                "built-in official publisher pin resolves key without external hints and exposes official PublisherId",
                err);

            // Attempt to spoof kOfficialCatalogPublisher in production VerifyLocalPackage must fail closed!
            const fs::path spoof_pkg = temp.path / L"official-spoof" / L"pulse.spoof-1.0.0.pulse-ext";
            Value spoof_sig;
            Require(pulse::json::Parse(official_sim_key.Sign(off_spec.manifest_bytes), spoof_sig));
            spoof_sig.set("publisher", std::string(kOfficialCatalogPublisher));
            Check(WriteBytes(spoof_pkg, off_spec.archive_bytes) &&
                  WriteBytes(fs::path(spoof_pkg.native() + L".sig.json"), pulse::json::Serialize(spoof_sig)),
                "write forged kOfficialCatalogPublisher .sig.json");
            VerifiedPackage untouched;
            err.clear();
            Check(!VerifyLocalPackage(spoof_pkg, cancel, untouched, err) &&
                  err == "package_signature_invalid" && !untouched,
                "forged kOfficialCatalogPublisher .sig.json fails closed against built-in official RSA-3072 key",
                err);
        }

        // 4. Broken/tampered .sig.json beside .pulse-ext fails closed (package_signature_invalid), NEVER falling back to unsigned
        {
            const auto spec = BuildNativeArchive("org.test.tampered.sig");
            const fs::path pkg_path = temp.path / L"tampered-sig" / L"org.test.tampered.sig.pulse-ext";
            std::string bad_sig = author_key.Sign(spec.manifest_bytes);
            const auto pos = bad_sig.find("\"signature\":\"");
            Require(pos != std::string::npos);
            const size_t flip_at = pos + 15;
            bad_sig[flip_at] = (bad_sig[flip_at] == '0') ? '1' : '0';
            Check(WriteBytes(pkg_path, spec.archive_bytes) &&
                  WriteBytes(fs::path(pkg_path.native() + L".sig.json"), bad_sig) &&
                  WriteBytes(fs::path(pkg_path.native() + L".pub"), author_key.publisher.public_key),
                "write .pulse-ext with 1-nibble-corrupted .sig.json");

            InstallService service(MakeConfig(temp.path / L"store-tampered-sig"));
            const auto gen = service.PrepareLocal(pkg_path);
            Check(WaitPhase(service, gen, Phase::Failed) &&
                  service.GetSnapshot().error == "package_signature_invalid" &&
                  !service.GetSnapshot().package,
                "tampered .sig.json fails closed with package_signature_invalid and never falls back to unsigned",
                service.GetSnapshot().error);

            // Corrupt JSON in .sig.json also fails closed.
            const fs::path broken_json_pkg = temp.path / L"tampered-json" / L"broken.pulse-ext";
            Check(WriteBytes(broken_json_pkg, spec.archive_bytes) &&
                  WriteBytes(fs::path(broken_json_pkg.native() + L".sig.json"), "{broken json"),
                "write .pulse-ext with malformed .sig.json");
            const auto gen2 = service.PrepareLocal(broken_json_pkg);
            Check(WaitPhase(service, gen2, Phase::Failed) &&
                  service.GetSnapshot().error == "package_signature_invalid",
                "malformed .sig.json fails closed with package_signature_invalid",
                service.GetSnapshot().error);
        }

        // 5. Tampered DLL SHA-256 or non-PE masquerading as .dll / .exe fails closed in VerifyLocalPackage
        {
            std::atomic<bool> cancel = false;
            VerifiedPackage out;
            std::string err;

            const auto bad_hash = BuildNativeArchive("org.test.badhash", {}, Pe(), true);
            const fs::path bad_hash_pkg = temp.path / L"bad-pe" / L"badhash.pulse-ext";
            WriteBytes(bad_hash_pkg, bad_hash.archive_bytes);
            Check(!VerifyLocalPackage(bad_hash_pkg, cancel, out, err) && err == "native_file_hash_mismatch",
                "tampered DLL SHA-256 rejected with native_file_hash_mismatch", err);

            const auto non_pe_dll = BuildNativeArchive("org.test.nonpe", {}, "not a real PE64 DLL image");
            const fs::path non_pe_pkg = temp.path / L"bad-pe" / L"nonpe.pulse-ext";
            WriteBytes(non_pe_pkg, non_pe_dll.archive_bytes);
            Check(!VerifyLocalPackage(non_pe_pkg, cancel, out, err) && err == "invalid_native_x64_pe_dll",
                "non-PE masquerading as .dll with valid SHA-256 rejected with invalid_native_x64_pe_dll", err);

            // DLL header masquerading as sidecar .exe (IMAGE_FILE_DLL bit set).
            const auto dll_as_exe = BuildNativeArchive("org.test.badexe", {{"fake.exe", Pe()}}, Pe());
            const fs::path bad_exe_pkg = temp.path / L"bad-pe" / L"badexe.pulse-ext";
            WriteBytes(bad_exe_pkg, dll_as_exe.archive_bytes);
            Check(!VerifyLocalPackage(bad_exe_pkg, cancel, out, err) && err == "invalid_native_x64_pe_exe",
                "DLL masquerading as sidecar .exe rejected with invalid_native_x64_pe_exe", err);
        }

        // 6. Store with requireSigned: true in signature-policy.json rejects unsigned local .pulse-ext (signature_required)
        {
            const fs::path strict_store = temp.path / L"store-require-signed";
            Value policy = Value::Object();
            policy.set("formatVersion", 1);
            policy.set("requireSigned", true);
            policy.set("publishers", Value::Array());
            Check(WriteBytes(strict_store / L"signature-policy.json", pulse::json::Serialize(policy)),
                "write signature-policy.json with requireSigned: true");

            const auto unsigned_spec = BuildNativeArchive("org.test.strict");
            const fs::path pkg_path = temp.path / L"strict" / L"org.test.strict.pulse-ext";
            WriteBytes(pkg_path, unsigned_spec.archive_bytes);

            InstallService service(MakeConfig(strict_store));
            const auto gen = service.PrepareLocal(pkg_path);
            Check(WaitPhase(service, gen, Phase::Failed) &&
                  service.GetSnapshot().error == "signature_required",
                "requireSigned: true store rejects unsigned local .pulse-ext with signature_required",
                service.GetSnapshot().error);
        }

        // 7. Store with already-installed signed package rejects upgrading same ID with unsigned local .pulse-ext
        {
            auto sneaky_dll = Pe();
            sneaky_dll[410] = 'X';
            const auto unsigned_upgrade = BuildNativeArchive("org.test.signed", {}, sneaky_dll);
            const fs::path downgrade_pkg = temp.path / L"downgrade" / L"org.test.signed-9.9.9.pulse-ext";
            Check(WriteBytes(downgrade_pkg, unsigned_upgrade.archive_bytes),
                "write unsigned upgrade targeting already-installed signed extension ID");

            InstallService service(MakeConfig(shared_signed_store));
            const auto gen = service.PrepareLocal(downgrade_pkg);
            Check(WaitPhase(service, gen, Phase::Failed) &&
                  service.GetSnapshot().error == "installed_publisher_mismatch",
                "upgrading signed extension with unsigned local .pulse-ext fails closed with installed_publisher_mismatch",
                service.GetSnapshot().error);

            // Also verify in a mixed store where requireSigned is explicitly false!
            const fs::path mixed_store = temp.path / L"store-mixed";
            fs::create_directories(mixed_store);
            const fs::path signed_pkg = temp.path / L"signed" / L"org.test.signed-1.0.0.pulse-ext";
            InstallService mixed_service(MakeConfig(mixed_store));
            const auto gm1 = mixed_service.PrepareLocal(signed_pkg);
            Check(WaitPhase(mixed_service, gm1, Phase::AwaitingPublisherTrust) &&
                  mixed_service.ConfirmPublisherTrust(gm1, true) &&
                  WaitPhase(mixed_service, gm1, Phase::AwaitingInstall) &&
                  mixed_service.ConfirmInstall(gm1, InstallConsent{true, true, true, true}) &&
                  WaitPhase(mixed_service, gm1, Phase::Installed),
                "install signed package into pre-existing mixed store (requireSigned: false)",
                mixed_service.GetSnapshot().error);
            const auto gm2 = mixed_service.PrepareLocal(downgrade_pkg);
            Check(WaitPhase(mixed_service, gm2, Phase::Failed) &&
                  mixed_service.GetSnapshot().error == "installed_publisher_mismatch",
                "mixed store (requireSigned: false) also blocks unsigned downgrade of signed package with installed_publisher_mismatch",
                mixed_service.GetSnapshot().error);
        }

        // 8. Reparse-point (NTFS junction), directory, relative, UNC, and device paths fail closed
        {
            const auto spec = BuildNativeArchive("org.test.pathcheck");
            const fs::path real_dir = temp.path / L"real-pkg-dir";
            const fs::path real_pkg = real_dir / L"pkg.pulse-ext";
            WriteBytes(real_pkg, spec.archive_bytes);

            std::atomic<bool> cancel = false;
            VerifiedPackage out;
            std::string err;

            // Relative path
            Check(!VerifyLocalPackage(fs::path(L"relative\\pkg.pulse-ext"), cancel, out, err) &&
                  err == "local_package_path_invalid",
                "relative path rejected with local_package_path_invalid", err);

            // Device / extended / UNC prefixes
            Check(!VerifyLocalPackage(fs::path(L"\\\\.\\" + real_pkg.wstring()), cancel, out, err) &&
                  err == "local_package_path_invalid",
                "\\\\.\\ device prefix rejected", err);
            Check(!VerifyLocalPackage(fs::path(L"\\\\?\\" + real_pkg.wstring()), cancel, out, err) &&
                  err == "local_package_path_invalid",
                "\\\\?\\ extended prefix rejected", err);
            Check(!VerifyLocalPackage(fs::path(L"\\\\127.0.0.1\\c$\\pkg.pulse-ext"), cancel, out, err) &&
                  err == "local_package_path_invalid",
                "UNC path rejected without network I/O", err);

            // Directory masquerading as .pulse-ext
            const fs::path fake_dir_pkg = temp.path / L"folder.pulse-ext";
            fs::create_directories(fake_dir_pkg);
            Check(!VerifyLocalPackage(fake_dir_pkg, cancel, out, err) &&
                  err == "local_package_is_directory",
                "directory named .pulse-ext rejected with local_package_is_directory", err);

            // NTFS directory junction in ancestor path
            const fs::path junction_dir = temp.path / L"junction-dir";
            const bool junction_created = CreateDirectoryJunction(junction_dir, real_dir);
            Check(junction_created, "create unprivileged NTFS directory junction fixture");
            if (junction_created) {
                const fs::path via_junction = junction_dir / L"pkg.pulse-ext";
                Check(!VerifyLocalPackage(via_junction, cancel, out, err) &&
                      err == "local_package_reparse_point",
                    "VerifyLocalPackage rejects ancestor NTFS junction with local_package_reparse_point", err);

                InstallService service(MakeConfig(temp.path / L"store-reparse"));
                const auto gen = service.PrepareLocal(via_junction);
                Check(WaitPhase(service, gen, Phase::Failed) &&
                      service.GetSnapshot().error == "local_package_reparse_point",
                    "PrepareLocal rejects ancestor NTFS junction on worker thread",
                    service.GetSnapshot().error);
                RemoveDirectoryW(junction_dir.c_str());
            }
        }

        // 9. Permission & Error Localization across zh-CN, en-US, and zh-TW
        {
            pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
            Check(pulse::extensions::generated::kResourceDeclarations.size() == 15,
                "canonical resource table has exactly 15 resource declarations");
            for (const wchar_t* lang : {L"zh-CN", L"en-US", L"zh-TW"}) {
                pulse::l10n::SetLanguage(lang);
                bool all_mapped = true;
                std::set<std::wstring> distinct;
                for (const std::string_view res : pulse::extensions::generated::kResourceDeclarations) {
                    const auto id = pulse::app::ResourcePermissionStringId(res);
                    const std::wstring text = pulse::app::LocalizedResourcePermission(res);
                    const std::wstring raw_wide(res.begin(), res.end());
                    if (!id.has_value() || text.empty() || text == raw_wide ||
                        text.find(L'.') == res.find('.')) {
                        all_mapped = false;
                    }
                    distinct.insert(text);
                }
                const std::string lang_utf8 = pulse::json::Utf8(lang);
                Check(all_mapped && distinct.size() == 15,
                    ("all 15 permissions localize to distinct human-readable descriptions in " + lang_utf8).c_str());

                const std::wstring sig_err = pulse::app::LocalizedStoreInstallError("package_signature_invalid");
                const std::wstring req_err = pulse::app::LocalizedStoreInstallError("signature_required");
                const std::wstring pub_err = pulse::app::LocalizedStoreInstallError("installed_publisher_mismatch");
                Check(!sig_err.empty() && sig_err != L"package_signature_invalid" &&
                      !req_err.empty() && req_err != L"signature_required" &&
                      !pub_err.empty() && pub_err != L"installed_publisher_mismatch",
                    ("security gate error codes localize to human-readable text in " + lang_utf8).c_str());
            }
        }
    } catch (const std::exception& ex) {
        std::printf("[FAIL] unhandled exception: %s\n", ex.what());
        ++g_failed;
    }

    std::printf("Summary: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

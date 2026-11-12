#include "../app/extensions/store/install_transaction.h"
#include "../app/extensions/package_signature.h"
#include "../app/extensions/package_store.h"
#include "../common/json_value.h"
#include "extension_package_fixture.h"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace pulse::extensions::store;
using namespace package_fixture;
namespace {
int passed = 0, failed = 0;
void Check(bool ok, const char* text, const std::string& error = {}) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << text;
    if (!ok && !error.empty()) std::cout << " : " << error;
    std::cout << '\n'; ok ? ++passed : ++failed;
}
void Write(const fs::path& path, std::string_view bytes) {
    std::ofstream file(path, std::ios::binary); file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("fixture write failed");
}
std::string Read(const fs::path& path) { std::ifstream in(path, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}}; }
VerifiedPackage Package(const Key& key, int native, const std::string& name = "Test package") {
    Fixture fixture(key, native, Pe(), "test.archive", name);
    VerifiedPackage package; std::string error;
    if (!Verify(fixture, package, error)) throw std::runtime_error("fixture verify: " + error);
    return package;
}
InstallConsent Consent(bool enable = false) { return {true, true, true, enable}; }
std::string SignRotation(const Key& signer, std::string_view id,
    std::string_view old_pub, std::string_view new_pub,
    std::string_view old_key_hex = {}, std::string_view new_key_hex = {}, bool wrong_domain = false) {
    constexpr char rot_prefix[] = "Pulse.Extension.Publisher.Rotation\0v1\0";
    constexpr char man_prefix[] = "Pulse.Extension.Manifest.Signature\0v1\0";
    std::string payload = wrong_domain ? std::string(man_prefix, sizeof(man_prefix) - 1)
                                       : std::string(rot_prefix, sizeof(rot_prefix) - 1);
    payload.append(id); payload.push_back('\n');
    payload.append(old_pub); payload.push_back('\n');
    payload.append(new_pub);
    const auto hex = PackageRevision(payload);
    std::vector<UCHAR> digest;
    for (size_t i = 0; i < hex.size(); i += 2) {
        digest.push_back(static_cast<UCHAR>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
    ULONG n = 0;
    Require(BCryptSignHash(signer.key, &padding, digest.data(), static_cast<ULONG>(digest.size()), nullptr, 0, &n, BCRYPT_PAD_PKCS1) >= 0);
    std::vector<UCHAR> sig(n);
    Require(BCryptSignHash(signer.key, &padding, digest.data(), static_cast<ULONG>(digest.size()), sig.data(), n, &n, BCRYPT_PAD_PKCS1) >= 0);
    Value v = Value::Object();
    v.set("formatVersion", 1);
    v.set("algorithm", "RSA-PKCS1-SHA256");
    v.set("id", std::string(id));
    v.set("oldPublisher", std::string(old_pub));
    v.set("newPublisher", std::string(new_pub));
    v.set("signature", Hex(sig));
    if (!old_key_hex.empty()) v.set("oldPublicKey", std::string(old_key_hex));
    if (!new_key_hex.empty()) v.set("newPublicKey", std::string(new_key_hex));
    return pulse::json::Serialize(v);
}
bool TemporaryFiles(const fs::path& root) {
    for (const auto& item : fs::recursive_directory_iterator(root))
        if (item.path().filename().wstring().starts_with(L".install-") || item.path().filename().wstring().starts_with(L".package-install-")) return true;
    return false;
}
}
int wmain() {
    std::cout << std::unitbuf;
    try {
        wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, 32768);
        const auto base = fs::path(module).parent_path() / (L"install-transaction-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directory(base);
        Key key; const auto first = Package(key, 0), second = Package(key, 0, "Upgrade"), native = Package(key, 1), multi = Package(key, 3);
        const auto Root = [&](const wchar_t* name, bool trust = true) {
            const auto root = base / name; fs::create_directory(root);
            if (trust) Write(root / L"signature-policy.json", Policy(key.publisher));
            return root;
        };
        std::string error; std::atomic<bool> cancel = false;
        InstallReview review; InstallResult output; output.id = "sentinel";
        const auto basic = Root(L"basic");
        Check(ReviewInstall(basic, first, review, error) && review.active_revision.empty() && review.added_resources == std::vector<std::string>{"settings.own"}, "initial review reports resources and no installed revision", error);
        Check(!CommitInstall(basic, first, review, {}, cancel, output, error) && error == "install_confirmation_required" && !fs::exists(basic / L"registry.json") && output.id == "sentinel", "no implicit installation consent", error);
        auto consent = Consent(); consent.added_resources = false;
        Check(!CommitInstall(basic, first, review, consent, cancel, output, error) && error == "resource_confirmation_required" && !fs::exists(basic / L"versions"), "resource grant required before payload publication", error);
        Check(CommitInstall(basic, first, review, Consent(), cancel, output, error) && output.changed && output.restart_required && !output.enabled, "install defaults disabled and requires restart", error);
        Check(Read(basic / L"versions" / (first.Revision() + ".json")) == first.ManifestBytes() && Read(basic / L"versions" / (first.Revision() + ".sig.json")) == first.SignatureBytes(), "manifest and detached signature preserve exact bytes");
        const auto another_store = Root(L"another-store");
        Check(!CommitInstall(another_store, first, review, Consent(), cancel, output, error) && error == "install_review_stale" && !fs::exists(another_store / L"versions"),
            "installation review cannot be replayed into another store", error);
        const auto original = Read(basic / L"registry.json");
        Check(ReviewInstall(basic, first, review, error) && review.added_resources.empty() && review.active_revision == first.Revision(), "installed review recognizes original revision", error);
        Check(CommitInstall(basic, first, review, Consent(), cancel, output, error) && !output.changed && Read(basic / L"registry.json") == original, "same-revision install is idempotent", error);
        InstallReview upgrade;
        Check(ReviewInstall(basic, second, upgrade, error) && CommitInstall(basic, second, upgrade, Consent(), cancel, output, error) && output.previous_revision == first.Revision() && !output.enabled, "upgrade retains previous immutable revision and enabled state", error);
        const auto upgraded = Read(basic / L"registry.json");
        Check(!CommitInstall(basic, first, review, Consent(), cancel, output, error) && error == "install_review_stale" && Read(basic / L"registry.json") == upgraded, "stale authorization cannot overwrite intervening upgrade", error);
        Check(fs::exists(basic / L"versions" / (first.Revision() + ".json")), "upgrade retains rollback manifest");
        const auto missing = Root(L"missing-trust", false);
        Check(!ReviewInstall(missing, first, review, error) && error == "publisher_trust_required" && !fs::exists(missing / L"signature-policy.json"), "publisher trust is never silently granted", error);
        const auto native_root = Root(L"native");
        Check(ReviewInstall(native_root, native, review, error) && review.native_code, "native execution disclosed in review", error);
        consent = Consent(true); consent.native_execution = false;
        Check(!CommitInstall(native_root, native, review, consent, cancel, output, error) && error == "native_confirmation_required" && !fs::exists(native_root / L"binaries"), "native consent required before writing DLL", error);
        Check(CommitInstall(native_root, native, review, Consent(true), cancel, output, error) && output.enabled && Read(native_root / L"binaries" / (native.Declaration().native->sha256 + ".dll")) == Pe(), "single native DLL published in compatible binaries layout", error);
        const auto loaded = pulse::extensions::LoadPackageStore(native_root.wstring());
        Check(loaded && loaded.packages.size() == 1 && loaded.packages[0].id == native.Declaration().id, "production startup loader verifies newly installed enabled native revision", loaded.error);
        const auto multi_root = Root(L"multi");
        Check(ReviewInstall(multi_root, multi, review, error) && CommitInstall(multi_root, multi, review, Consent(true), cancel, output, error), "multi-file revision publishes as one directory", error);
        const auto multi_loaded = pulse::extensions::LoadPackageStore(multi_root.wstring());
        Check(multi_loaded && multi_loaded.packages.size() == 1 && !TemporaryFiles(multi_root), "multi-file native revision passes real startup verifier without temporary residue", multi_loaded.error);
        const auto multi_registry = Read(multi_root / L"registry.json");
        Check(ReviewInstall(multi_root, multi, review, error), "existing multi-file review", error);
        Write(multi_root / L"packages" / multi.Revision() / L"extra.txt", "unexpected");
        Check(!CommitInstall(multi_root, multi, review, Consent(), cancel, output, error) && error == "immutable_revision_extra_member" && Read(multi_root / L"registry.json") == multi_registry, "existing extra payload member is refused, never deleted", error);
        const auto cancelled_root = Root(L"cancelled");
        Check(ReviewInstall(cancelled_root, multi, review, error), "prepare cancellation review", error);
        cancel = true; output.id = "sentinel";
        Check(!CommitInstall(cancelled_root, multi, review, Consent(), cancel, output, error) && error == "install_cancelled" && !fs::exists(cancelled_root / L"registry.json") && output.id == "sentinel", "pre-cancel preserves registry and output", error);
        cancel = false;
        Check(!testing::CommitInstallWithHook(cancelled_root, multi, review, Consent(), cancel, output, error, [&] { cancel = true; }) && error == "install_cancelled" && !fs::exists(cancelled_root / L"registry.json") && !TemporaryFiles(cancelled_root), "cancel immediately before registry commit leaves no live package or temporary files", error);
        cancel = false;
        Check(CommitInstall(cancelled_root, multi, review, Consent(), cancel, output, error), "verified inactive revision can be retried after cancellation", error);
        const auto rollback_root = Root(L"failed-commit");
        Check(ReviewInstall(rollback_root, first, review, error) && CommitInstall(rollback_root, first, review, Consent(), cancel, output, error), "prepare commit failure baseline", error);
        const auto before_failure = Read(rollback_root / L"registry.json");
        Check(ReviewInstall(rollback_root, second, review, error), "prepare next revision for commit failure", error);
        HANDLE held = INVALID_HANDLE_VALUE;
        const bool committed = testing::CommitInstallWithHook(rollback_root, second, review, Consent(), cancel, output, error, [&] {
            held = CreateFileW((rollback_root / L"registry.json").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        });
        if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
        Check(!committed && Read(rollback_root / L"registry.json") == before_failure && !TemporaryFiles(rollback_root), "failed atomic registry replacement preserves old registry", error);
        const auto locked = Root(L"locked");
        const auto lock_path = locked / L".registry.lock"; Write(lock_path, std::string(1, '\0'));
        HANDLE lock = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        OVERLAPPED overlap{};
        const bool acquired = lock != INVALID_HANDLE_VALUE && LockFileEx(lock, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap);
        Check(acquired && !ReviewInstall(locked, first, review, error) && error == "store_busy", "byte-range lock excludes another store writer", error);
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        Check(ReviewInstall(locked, first, review, error), "byte-range lock is released for subsequent writer", error);
        const auto corrupt = Root(L"corrupt-registry");
        Write(corrupt / L"registry.json", "{\"formatVersion\":1,\"formatVersion\":1,\"packages\":[]}");
        Check(!ReviewInstall(corrupt, first, review, error), "duplicate registry keys rejected", error);
        Check(!ReviewInstall(fs::path(L"relative"), first, review, error), "relative store path rejected", error);
        Check(!ReviewInstall(base / L"missing", first, review, error) && !fs::exists(base / L"missing"), "missing store root is not implicitly created", error);
        Check(!ReviewInstall(basic, VerifiedPackage{}, review, error) && error == "invalid_verified_package", "unverified package cannot enter transaction", error);
        const auto tampered = Root(L"tampered");
        Check(ReviewInstall(tampered, native, review, error) && CommitInstall(tampered, native, review, Consent(), cancel, output, error), "prepare native tamper test", error);
        const auto tampered_registry = Read(tampered / L"registry.json");
        Check(ReviewInstall(tampered, native, review, error), "review manifest before disk payload tamper", error);
        Write(tampered / L"binaries" / (native.Declaration().native->sha256 + ".dll"), "bad");
        Check(!CommitInstall(tampered, native, review, Consent(), cancel, output, error) && error == "immutable_revision_conflict" && Read(tampered / L"registry.json") == tampered_registry, "same-revision native disk tampering refused without repair", error);
        const auto hardlinked = Root(L"hardlinked-lock");
        const auto outside = base / L"outside-empty.txt"; Write(outside, "");
        const bool linked = CreateHardLinkW((hardlinked / L".registry.lock").c_str(), outside.c_str(), nullptr) != FALSE;
        Check(linked && !ReviewInstall(hardlinked, first, review, error) && error == "store_lock_not_regular" && fs::file_size(outside) == 0,
            "hardlinked lock cannot cause writes outside store", error);
        const auto pinned = Root(L"pinned-payload");
        Check(ReviewInstall(pinned, native, review, error), "prepare pinned publication", error);
        bool write_denied = false;
        Check(testing::CommitInstallWithHook(pinned, native, review, Consent(true), cancel, output, error, [&] {
            const auto path = pinned / L"binaries" / (native.Declaration().native->sha256 + ".dll");
            HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, 0, nullptr);
            write_denied = writer == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION;
            if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
        }) && write_denied, "verified payload stays write-locked through registry commit", error);
        Key other_key; const auto other_package = Package(other_key, 0);
        const auto publisher_bound = Root(L"publisher-bound-review");
        pulse::json::Value policy, other_policy;
        if (!pulse::json::Parse(Policy(key.publisher), policy) || !pulse::json::Parse(Policy(other_key.publisher), other_policy))
            throw std::runtime_error("policy fixture parse failed");
        auto pins = *policy.find("publishers"); pins.push(other_policy.find("publishers")->at(0)); policy.set("publishers", pins);
        Write(publisher_bound / L"signature-policy.json", pulse::json::Serialize(policy));
        Check(first.Revision() == other_package.Revision() && ReviewInstall(publisher_bound, first, review, error) && review.publisher_id == key.publisher.id,
            "review binds publisher even for byte-identical manifests signed by different trusted keys", error);
        Check(!CommitInstall(publisher_bound, other_package, review, Consent(), cancel, output, error) && error == "install_review_stale" &&
            !fs::exists(publisher_bound / L"registry.json"), "different publisher cannot reuse a same-manifest authorization", error);
        Check(CommitInstall(publisher_bound, first, review, Consent(), cancel, output, error), "reviewed publisher can commit original candidate", error);
        Check(!ReviewInstall(publisher_bound, other_package, review, error) && error == "installed_publisher_mismatch",
            "another trusted publisher cannot take over an installed identifier", error);
        const auto transient = Root(L"transient-child-reader");
        Check(ReviewInstall(transient, multi, review, error), "prepare transient descendant reader", error);
        HANDLE child = INVALID_HANDLE_VALUE; unsigned retries = 0;
        testing::InstallHooks hooks;
        hooks.before_payload_publish = [&](const fs::path& directory) {
            child = CreateFileW((directory / L"test.dll").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        };
        hooks.rename_retry = [&](const fs::path&, unsigned long) {
            ++retries; if (child != INVALID_HANDLE_VALUE) { CloseHandle(child); child = INVALID_HANDLE_VALUE; }
        };
        const bool retried = testing::CommitInstallWithHooks(transient, multi, review, Consent(true), cancel, output, error, hooks);
        if (child != INVALID_HANDLE_VALUE) { CloseHandle(child); child = INVALID_HANDLE_VALUE; }
        Check(retried && retries >= 1 && pulse::extensions::LoadPackageStore(transient.wstring()),
            "temporarily open descendant causes real rename failure then successful bounded retry", error);
        const auto retry_cancelled = Root(L"cancel-during-rename-retry");
        Check(ReviewInstall(retry_cancelled, multi, review, error), "prepare cancellation during directory publication retry", error);
        hooks.rename_retry = [&](const fs::path&, unsigned long) {
            if (child != INVALID_HANDLE_VALUE) { CloseHandle(child); child = INVALID_HANDLE_VALUE; }
            cancel = true;
        };
        const bool retry_committed = testing::CommitInstallWithHooks(retry_cancelled, multi, review, Consent(), cancel, output, error, hooks);
        if (child != INVALID_HANDLE_VALUE) CloseHandle(child);
        Check(!retry_committed && error == "install_cancelled" && !fs::exists(retry_cancelled / L"registry.json") && !TemporaryFiles(retry_cancelled),
            "cancellation during directory retry leaves registry unchanged and cleans staged directory", error);
        cancel = false;
        {
            // PLG-05 & PLG-12: Verified publisher_ids from LoadPackageStore & origin-bound publish trust keys.
            Check(loaded.publisher_ids == std::vector<std::string>{key.publisher.id},
                "LoadPackageStore records verified SHA-256 publisher_id for signed package");
            const auto unsigned_root = base / L"unsigned-dev-store";
            fs::create_directories(unsigned_root / L"versions");
            Value dev_manifest = Value::Object();
            dev_manifest.set("apiVersion", 1); dev_manifest.set("id", "dev.local"); dev_manifest.set("name", "Dev");
            const auto dev_bytes = pulse::json::Serialize(dev_manifest);
            const auto dev_rev = PackageRevision(dev_bytes);
            Write(unsigned_root / L"versions" / (dev_rev + ".json"), dev_bytes);
            Value dev_entry = Value::Object();
            dev_entry.set("id", "dev.local"); dev_entry.set("name", "Dev");
            dev_entry.set("enabled", true); dev_entry.set("activeRevision", dev_rev);
            Value dev_pkgs = Value::Array(); dev_pkgs.push(dev_entry);
            Value dev_reg = Value::Object(); dev_reg.set("formatVersion", 1); dev_reg.set("packages", dev_pkgs);
            Write(unsigned_root / L"registry.json", pulse::json::Serialize(dev_reg));
            const auto unsigned_loaded = pulse::extensions::LoadPackageStore(unsigned_root.wstring());
            Check(unsigned_loaded && unsigned_loaded.packages.size() == 1 &&
                unsigned_loaded.publisher_ids == std::vector<std::string>{""},
                "LoadPackageStore records empty publisher_id for unsigned local dev package", unsigned_loaded.error);

            std::vector<std::wstring> trusted{L"test.archive", L"other.pkg"};
            Check(!pulse::extensions::IsPublishTrusted(trusted, key.publisher.id, "test.archive") &&
                !pulse::extensions::IsPublishTrusted(trusted, "", "test.archive"),
                "legacy bare package_id never matches signed or unsigned publish trust");
            Check(pulse::extensions::FormatPublishTrustKey("", "test.archive") == L"unsigned:test.archive" &&
                pulse::extensions::FormatPublishTrustKey(key.publisher.id, "test.archive").size() == 65 + std::string_view("test.archive").size(),
                "FormatPublishTrustKey formats origin-bound signed and unsigned keys");
            Check(pulse::extensions::GrantPublishTrust(trusted, key.publisher.id, "test.archive") &&
                pulse::extensions::IsPublishTrusted(trusted, key.publisher.id, "test.archive") &&
                !pulse::extensions::IsPublishTrusted(trusted, other_key.publisher.id, "test.archive") &&
                !pulse::extensions::IsPublishTrusted(trusted, "", "test.archive") &&
                std::find(trusted.begin(), trusted.end(), L"test.archive") == trusted.end(),
                "GrantPublishTrust binds to (publisher, id), isolates other signers/unsigned, and cleans legacy bare id");
            pulse::extensions::GrantPublishTrust(trusted, other_key.publisher.id, "test.archive");
            pulse::extensions::GrantPublishTrust(trusted, "", "test.archive");
            pulse::extensions::GrantPublishTrust(trusted, key.publisher.id, "keep.pkg");
            Check(pulse::extensions::RevokePublishTrustForOrigin(trusted, key.publisher.id, "test.archive") == 1 &&
                !pulse::extensions::IsPublishTrusted(trusted, key.publisher.id, "test.archive") &&
                pulse::extensions::IsPublishTrusted(trusted, other_key.publisher.id, "test.archive") &&
                pulse::extensions::IsPublishTrusted(trusted, "", "test.archive"),
                "RevokePublishTrustForOrigin removes only the targeted publisher entry");
            Check(pulse::extensions::RevokePublishTrustForPackage(trusted, "test.archive") == 2 &&
                !pulse::extensions::IsPublishTrusted(trusted, other_key.publisher.id, "test.archive") &&
                !pulse::extensions::IsPublishTrusted(trusted, "", "test.archive") &&
                pulse::extensions::IsPublishTrusted(trusted, key.publisher.id, "keep.pkg"),
                "RevokePublishTrustForPackage removes all origins for target package without touching other packages");

            // PLG-12: Explicit publisher key rotation verification & install transaction flow.
            const auto valid_rot = SignRotation(key, "test.archive", key.publisher.id, other_key.publisher.id,
                key.publisher.public_key, other_key.publisher.public_key);
            Check(pulse::extensions::VerifyPublisherRotation("test.archive",
                key.publisher.id, key.publisher.public_key,
                other_key.publisher.id, other_key.publisher.public_key, valid_rot, error),
                "VerifyPublisherRotation accepts valid old-key signature over rotation domain", error);
            const auto wrong_signer_rot = SignRotation(other_key, "test.archive", key.publisher.id, other_key.publisher.id);
            Check(!pulse::extensions::VerifyPublisherRotation("test.archive",
                key.publisher.id, key.publisher.public_key,
                other_key.publisher.id, other_key.publisher.public_key, wrong_signer_rot, error) &&
                error == "rotation_signature_invalid",
                "VerifyPublisherRotation rejects rotation signed by new key instead of old key", error);
            const auto wrong_domain_rot = SignRotation(key, "test.archive", key.publisher.id, other_key.publisher.id, {}, {}, true);
            Check(!pulse::extensions::VerifyPublisherRotation("test.archive",
                key.publisher.id, key.publisher.public_key,
                other_key.publisher.id, other_key.publisher.public_key, wrong_domain_rot, error) &&
                error == "rotation_signature_invalid",
                "VerifyPublisherRotation enforces domain separation prefix", error);
            const auto wrong_id_rot = SignRotation(key, "other.id", key.publisher.id, other_key.publisher.id);
            Check(!pulse::extensions::VerifyPublisherRotation("test.archive",
                key.publisher.id, key.publisher.public_key,
                other_key.publisher.id, other_key.publisher.public_key, wrong_id_rot, error) &&
                error == "rotation_identity_mismatch",
                "VerifyPublisherRotation binds package id", error);
            const auto self_rot = SignRotation(key, "test.archive", key.publisher.id, key.publisher.id);
            Check(!pulse::extensions::VerifyPublisherRotation("test.archive",
                key.publisher.id, key.publisher.public_key,
                key.publisher.id, key.publisher.public_key, self_rot, error) &&
                error == "invalid_rotation_publisher",
                "VerifyPublisherRotation rejects self-rotation", error);

            const auto rotated_pkg = Package(other_key, 0, "Rotated Upgrade");
            const auto untrusted_rot_root = Root(L"untrusted-rotation");
            Check(ReviewInstall(untrusted_rot_root, first, review, error) &&
                CommitInstall(untrusted_rot_root, first, review, Consent(), cancel, output, error),
                "prepare untrusted rotation baseline", error);
            Check(!ReviewInstallWithRotation(untrusted_rot_root, rotated_pkg, valid_rot, review, error) &&
                error.find("unknown_publisher") != std::string::npos,
                "rotation proof never bypasses signature-policy trust for new publisher", error);

            Check(!ReviewInstallWithRotation(publisher_bound, other_package, valid_rot, review, error) &&
                error == "rotation_requires_new_revision",
                "publisher rotation requires a distinct new revision", error);
            Check(!ReviewInstallWithRotation(publisher_bound, rotated_pkg, wrong_signer_rot, review, error) &&
                error.find("invalid_publisher_rotation:") == 0,
                "invalid rotation proof rejected during install review", error);
            InstallReview rot_review;
            Check(ReviewInstallWithRotation(publisher_bound, rotated_pkg, valid_rot, rot_review, error) &&
                rot_review.publisher_rotated && rot_review.previous_publisher_id == key.publisher.id &&
                rot_review.publisher_id == other_key.publisher.id,
                "valid publisher rotation populates InstallReview::publisher_rotated and previous_publisher_id", error);
            Check(!CommitInstallWithRotation(publisher_bound, rotated_pkg, valid_rot, {}, {}, cancel, output, error),
                "rotated upgrade rejects empty review", error);
            Check(!CommitInstallWithRotation(publisher_bound, rotated_pkg, valid_rot, rot_review, {false, true, true, false}, cancel, output, error) &&
                error == "install_confirmation_required",
                "rotated upgrade requires explicit user approval", error);
            auto forged_review = rot_review;
            forged_review.publisher_rotated = false;
            Check(!CommitInstallWithRotation(publisher_bound, rotated_pkg, valid_rot, forged_review, Consent(), cancel, output, error) &&
                error == "install_review_stale",
                "commit rejects review that hides publisher_rotated", error);
            Check(CommitInstallWithRotation(publisher_bound, rotated_pkg, valid_rot, rot_review, Consent(true), cancel, output, error) &&
                output.changed && output.restart_required &&
                Read(publisher_bound / L"versions" / (rotated_pkg.Revision() + ".rotation.json")) == valid_rot,
                "rotated upgrade commits smoothly and persists immutable .rotation.json", error);
            Check(ReviewInstall(publisher_bound, rotated_pkg, rot_review, error) && !rot_review.publisher_rotated &&
                CommitInstall(publisher_bound, rotated_pkg, rot_review, Consent(), cancel, output, error) && !output.changed,
                "subsequent same-revision install after rotation remains idempotent", error);

            // Retired old publisher key in policy + on-disk versions/<new_sha>.rotation.json discovery.
            const auto retired_root = Root(L"retired-key-rotation");
            Check(ReviewInstall(retired_root, first, review, error) &&
                CommitInstall(retired_root, first, review, Consent(), cancel, output, error),
                "prepare retired-key rotation store", error);
            Write(retired_root / L"signature-policy.json", Policy(other_key.publisher));
            Write(retired_root / L"versions" / (rotated_pkg.Revision() + ".rotation.json"), valid_rot);
            Check(ReviewInstall(retired_root, rotated_pkg, rot_review, error) &&
                rot_review.publisher_rotated && rot_review.previous_publisher_id == key.publisher.id &&
                CommitInstall(retired_root, rotated_pkg, rot_review, Consent(), cancel, output, error) && output.changed,
                "ReviewInstall/CommitInstall discover on-disk .rotation.json even after old pin retired from policy", error);
        }
        Check(!TemporaryFiles(base), "no transaction temporary files leaked across cases");
        std::cout << "Install transaction tests: " << passed << " passed, " << failed << " failed\n";
        return failed ? 1 : 0;
    } catch (const std::exception& e) { std::cout << "FATAL: " << e.what() << '\n'; return 2; }
}

#include "../app/extensions/store/publisher_trust.h"
#include "../app/extensions/store/install_transaction.h"
#include "../app/extensions/package_signature.h"
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
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << text << (ok || error.empty() ? "" : " : " + error) << '\n';
    ok ? ++passed : ++failed;
}
void Write(const fs::path& path, std::string_view bytes) {
    std::ofstream file(path, std::ios::binary); file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("fixture write failed");
}
std::string Read(const fs::path& path) { std::ifstream in(path, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}}; }
VerifiedPackage Package(const Key& key, bool native = false) {
    Fixture fixture(key, native ? 1 : 0); VerifiedPackage package; std::string error;
    if (!Verify(fixture, package, error)) throw std::runtime_error(error);
    return package;
}
}
int wmain() {
    std::cout << std::unitbuf;
    try {
        wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, 32768);
        const auto base = fs::path(module).parent_path() / (L"publisher-trust-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directory(base);
        Key key, other; const auto package = Package(key), native = Package(key, true), another = Package(other);
        const auto new_root = base / L"new-parent" / L"extensions";
        std::string error; std::atomic<bool> cancelled = false;
        PublisherTrustReview review; PublisherTrustResult result; result.changed = true;
        Check(ReviewPublisherTrust(new_root, native, review, error) && !review.store_exists && !review.already_trusted &&
            review.publisher_id == key.publisher.id && !fs::exists(new_root.parent_path()), "trust preview never creates missing directories or silently trusts publisher", error);
        Check(!CommitPublisherTrust(new_root, native, review, false, cancelled, result, error) && error == "publisher_confirmation_required" &&
            !fs::exists(new_root.parent_path()) && result.changed, "no explicit approval leaves missing root and output untouched", error);
        cancelled = true;
        Check(!CommitPublisherTrust(new_root, native, review, true, cancelled, result, error) && error == "install_cancelled" && !fs::exists(new_root.parent_path()),
            "pre-cancel creates no root and writes no trust", error);
        cancelled = false;
        const auto other_root = base / L"another-store";
        Check(!CommitPublisherTrust(other_root, native, review, true, cancelled, result, error) && error == "publisher_review_stale" && !fs::exists(other_root),
            "trust review is bound to store root", error);
        Check(!CommitPublisherTrust(new_root, package, review, true, cancelled, result, error) && error == "publisher_review_stale" && !fs::exists(new_root),
            "trust review is bound to candidate revision", error);
        Check(CommitPublisherTrust(new_root, native, review, true, cancelled, result, error) && result.changed && result.root_created &&
            fs::exists(new_root / L"signature-policy.json") && !fs::exists(new_root / L"registry.json") && !fs::exists(new_root / L"versions"),
            "approved trust creates only root/lock/policy, never installs or enables native package", error);
        const auto first_policy = Read(new_root / L"signature-policy.json");
        Check(pulse::extensions::VerifyPackageSignature(native.ManifestBytes(), native.SignatureBytes(), first_policy, error),
            "provisioned policy verifies selected publisher's detached signature", error);
        Check(ReviewPublisherTrust(new_root, native, review, error) && review.already_trusted && review.store_exists && !review.policy_revision.empty(),
            "existing trusted identity is reported accurately", error);
        Check(CommitPublisherTrust(new_root, native, review, true, cancelled, result, error) && !result.changed && !result.root_created &&
            Read(new_root / L"signature-policy.json") == first_policy, "repeated explicit trust is byte-preserving idempotent", error);
        InstallReview install_review; InstallResult install_result;
        Check(ReviewInstall(new_root, native, install_review, error) && install_review.native_code, "ordinary install review works after explicit publisher trust", error);
        Check(!CommitInstall(new_root, native, install_review, {true, false, true, true}, cancelled, install_result, error) && error == "native_confirmation_required" &&
            !fs::exists(new_root / L"registry.json"), "publisher trust cannot substitute native execution consent", error);
        const auto legacy = base / L"legacy-no-policy"; fs::create_directory(legacy);
        Check(ReviewPublisherTrust(legacy, package, review, error) && CommitPublisherTrust(legacy, package, review, true, cancelled, result, error),
            "publisher can be explicitly trusted in an existing development store", error);
        Value legacy_policy; if (!pulse::json::Parse(Read(legacy / L"signature-policy.json"), legacy_policy)) throw std::runtime_error("legacy policy parse");
        Value fresh_policy; if (!pulse::json::Parse(first_policy, fresh_policy)) throw std::runtime_error("fresh policy parse");
        Check(!legacy_policy.get_bool("requireSigned", true) && fresh_policy.get_bool("requireSigned"),
            "legacy unsigned policy is preserved while freshly provisioned stores require signatures");
        const auto existing = base / L"existing-policy"; fs::create_directory(existing);
        Value policy; if (!pulse::json::Parse(Policy(other.publisher), policy)) throw std::runtime_error("policy parse");
        policy.set("requireSigned", false); Write(existing / L"signature-policy.json", pulse::json::Serialize(policy));
        Check(ReviewPublisherTrust(existing, package, review, error) && !review.already_trusted, "review recognizes an untrusted candidate in existing valid policy", error);
        Check(CommitPublisherTrust(existing, package, review, true, cancelled, result, error) && result.changed && !result.root_created,
            "approved publisher is added to existing policy", error);
        Value updated; if (!pulse::json::Parse(Read(existing / L"signature-policy.json"), updated)) throw std::runtime_error("updated policy parse");
        Check(!updated.get_bool("requireSigned", true) && updated.find("publishers")->size() == 2 &&
            updated.find("publishers")->at(0).get_string("id") == other.publisher.id, "existing policy settings and unrelated publisher remain intact");
        const auto stale = base / L"stale-policy"; fs::create_directory(stale);
        Write(stale / L"signature-policy.json", Policy(other.publisher));
        Check(ReviewPublisherTrust(stale, package, review, error), "prepare stale policy review", error);
        policy.set("requireSigned", true); policy.set("publishers", Value::Array());
        const auto changed_policy = pulse::json::Serialize(policy); Write(stale / L"signature-policy.json", changed_policy);
        Check(!CommitPublisherTrust(stale, package, review, true, cancelled, result, error) && error == "publisher_review_stale" &&
            Read(stale / L"signature-policy.json") == changed_policy, "changed policy invalidates prior consent instead of losing another writer's changes", error);
        const auto created = base / L"created-after-review";
        Check(ReviewPublisherTrust(created, package, review, error), "prepare absent store review", error);
        fs::create_directory(created);
        Check(!CommitPublisherTrust(created, package, review, true, cancelled, result, error) && error == "publisher_review_stale" && !fs::exists(created / L"signature-policy.json"),
            "a concurrently created store requires a fresh review", error);
        const auto swapped = base / L"swapped-publisher";
        Check(ReviewPublisherTrust(swapped, package, review, error) && package.Revision() == another.Revision(), "prepare identical manifest with another publisher", error);
        Check(!CommitPublisherTrust(swapped, another, review, true, cancelled, result, error) && error == "publisher_review_stale" && !fs::exists(swapped),
            "publisher identity cannot be swapped under identical manifest consent", error);
        const auto malformed = base / L"malformed"; fs::create_directory(malformed);
        for (const auto& bytes : {std::string{}, std::string{"{}"}, std::string{"{\"formatVersion\":1,\"formatVersion\":1}"}}) {
            Write(malformed / L"signature-policy.json", bytes);
            Check(!ReviewPublisherTrust(malformed, package, review, error) && Read(malformed / L"signature-policy.json") == bytes,
                "malformed or empty existing policy is never reset", error);
        }
        const auto blocked = base / L"blocked-rename"; fs::create_directory(blocked);
        Write(blocked / L"signature-policy.json", Policy(other.publisher));
        Check(ReviewPublisherTrust(blocked, package, review, error), "prepare atomic policy failure", error);
        const auto before = Read(blocked / L"signature-policy.json");
        HANDLE reader = CreateFileW((blocked / L"signature-policy.json").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        const bool committed = CommitPublisherTrust(blocked, package, review, true, cancelled, result, error);
        if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
        Check(!committed && Read(blocked / L"signature-policy.json") == before, "failed atomic trust rename preserves original policy", error);
        Check(!ReviewPublisherTrust(fs::path(L"relative"), package, review, error) && !ReviewPublisherTrust(base / L".." / L"x", package, review, error),
            "relative and dot traversal roots rejected", error);
        Check(!ReviewPublisherTrust(base / L".. " / L"outside", package, review, error) && error == "store_path_alias" &&
            !ReviewPublisherTrust(base / L"trailing.", package, review, error) && error == "store_path_alias", "Win32 trailing-dot/space path aliases rejected before provisioning", error);
        Check(!ReviewPublisherTrust(base.root_path(), package, review, error), "volume root is not an extension store", error);
        Check(!ReviewPublisherTrust(base / L"unverified", VerifiedPackage{}, review, error) && !fs::exists(base / L"unverified"), "unverified package cannot provision publisher trust", error);
        bool temporary = false;
        for (const auto& entry : fs::recursive_directory_iterator(base)) if (entry.path().filename().wstring().starts_with(L".install-")) temporary = true;
        Check(!temporary, "trust failures clean temporary policy files");
        std::cout << "Publisher trust tests: " << passed << " passed, " << failed << " failed\n";
        return failed ? 1 : 0;
    } catch (const std::exception& e) { std::cout << "FATAL: " << e.what() << '\n'; return 2; }
}

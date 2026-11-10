#include "../app/extensions/store/installed_store.h"
#include "../app/extensions/store/install_transaction.h"
#include "extension_package_fixture.h"
#include <condition_variable>
#include <iostream>
#include <mutex>

namespace fs = std::filesystem;
using namespace pulse::extensions::store;
using namespace package_fixture;
namespace {
int passed = 0, failed = 0;
void Check(bool ok, const char* text, const std::string& error = {}) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << text;
    if (!ok) std::cout << " : " << error;
    std::cout << '\n'; ok ? ++passed : ++failed;
}
void Write(const fs::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary); out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("fixture write failed");
}
std::string Read(const fs::path& path) { std::ifstream in(path, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}}; }
VerifiedPackage Package(const Key& key, int count, const std::string& name) {
    Fixture fixture(key, count, Pe(), "test.archive", name); VerifiedPackage out; std::string error;
    if (!Verify(fixture, out, error)) throw std::runtime_error(error); return out;
}
void Install(const fs::path& root, const VerifiedPackage& package) {
    InstallReview review; InstallResult result; std::string error; std::atomic<bool> cancel = false;
    if (!ReviewInstall(root, package, review, error) || !CommitInstall(root, package, review, {true,true,true,false}, cancel, result, error)) throw std::runtime_error(error);
}
struct Gate {
    std::mutex mutex; std::condition_variable cv; bool entered = false, released = false;
    void Enter() { std::unique_lock lock(mutex); entered = true; cv.notify_all(); cv.wait(lock, [&] { return released; }); }
    bool Wait() { std::unique_lock lock(mutex); return cv.wait_for(lock, std::chrono::seconds(10), [&] { return entered; }); }
    void Release() { std::lock_guard lock(mutex); released = true; cv.notify_all(); }
};
}
int wmain() {
    std::cout << std::unitbuf;
    try {
        wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, 32768);
        const auto base = fs::path(module).parent_path() / (L"installed-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directory(base);
        Key key; auto native = Package(key, 1, "First"), next = Package(key, 1, "Next"), multi = Package(key, 3, "Multi");
        const auto Root = [&](const wchar_t* name) { const auto root = base / name; fs::create_directory(root); Write(root / L"signature-policy.json", Policy(key.publisher)); return root; };
        std::string error; InstalledSnapshot snapshot; StoreChangeReview review; StoreChangeResult result; std::atomic<bool> cancel = false;
        const StoreChangeConsent yes{true,true,true};
        Check(ReadInstalledStore(base / L"missing" / L"child", snapshot, error) && snapshot.missing && snapshot.entries.empty() && !fs::exists(base / L"missing"), "missing snapshot creates no root/lock/policy", error);
        Check(!ReadInstalledStore(L"relative", snapshot, error), "relative root rejected", error);
        const auto root = Root(L"native"); Install(root, native);
        Check(ReadInstalledStore(root, snapshot, error) && !snapshot.missing && snapshot.entries.size() == 1 && !snapshot.entries[0].enabled && snapshot.entries[0].native_code && snapshot.entries[0].publisher_id == key.publisher.id && snapshot.entries[0].error.empty(), "disabled registry record displayed with verified metadata", error);
        const auto dll = root / L"binaries" / (native.Declaration().native->sha256 + ".dll");
        fs::rename(dll, dll.wstring() + L".held");
        Check(ReadInstalledStore(root, snapshot, error) && snapshot.entries[0].error.empty(), "snapshot does not read large payload", error);
        Check(!ReviewStoreChange(root, native.Declaration().id, StoreChangeAction::Enable, review, error), "enable rejects missing payload", error);
        fs::rename(dll.wstring() + L".held", dll);
        Check(ReviewStoreChange(root, native.Declaration().id, StoreChangeAction::Enable, review, error) && review.native_code && !review.added_resources.empty(), "enable review validates native payload and discloses resources", error);
        auto consent = yes; consent.native_execution = false;
        Check(!CommitStoreChange(root, review, consent, cancel, result, error) && error == "native_confirmation_required", "enable independently requires native consent", error);
        consent = yes; consent.added_resources = false;
        Check(!CommitStoreChange(root, review, consent, cancel, result, error) && error == "resource_confirmation_required", "enable independently requires resource consent", error);
        Check(!CommitStoreChange(root, review, {}, cancel, result, error), "no implicit management approval", error);
        const auto another = Root(L"another"); Install(another, native);
        Check(!CommitStoreChange(another, review, yes, cancel, result, error) && error == "store_review_stale", "root replay rejected", error);
        const auto identity_root = Root(L"identity"); Install(identity_root, native);
        StoreChangeReview identity_review;
        Check(ReviewStoreChange(identity_root, native.Declaration().id, StoreChangeAction::Enable, identity_review, error), "prepare directory identity review", error);
        fs::rename(identity_root, base / L"identity-original");
        fs::copy(base / L"identity-original", identity_root, fs::copy_options::recursive);
        Check(!CommitStoreChange(identity_root, identity_review, yes, cancel, result, error) && error == "store_review_stale", "same-path store replacement with identical bytes is stale", error);
        const auto policy_bytes = Read(root / L"signature-policy.json");
        Write(root / L"signature-policy.json", policy_bytes + " ");
        Check(!CommitStoreChange(root, review, yes, cancel, result, error) && error == "store_review_stale", "exact policy digest binds execution review", error);
        Write(root / L"signature-policy.json", policy_bytes);
        Check(!ReviewStoreChange(root, native.Declaration().id, static_cast<StoreChangeAction>(99), identity_review, error), "unknown management action rejected", error);
        const auto original = Read(root / L"registry.json");
        Write(root / L"registry.json", original + " ");
        Check(!CommitStoreChange(root, review, yes, cancel, result, error) && error == "store_review_stale", "exact registry digest binds review", error);
        Write(root / L"registry.json", original);
        Write(dll, "tampered");
        Check(!CommitStoreChange(root, review, yes, cancel, result, error) && Read(root / L"registry.json") == original, "commit revalidates payload and preserves registry on tamper", error);
        Write(dll, Pe()); cancel = true; result = {false,false};
        Check(!CommitStoreChange(root, review, yes, cancel, result, error) && !result.changed && Read(root / L"registry.json") == original, "pre-cancel writes nothing", error);
        cancel = false;
        Check(!testing::CommitStoreChangeWithHook(root, review, yes, cancel, result, error, [&] { cancel = true; }) && Read(root / L"registry.json") == original, "cancel at publication gate preserves registry", error);
        cancel = false; HANDLE held = INVALID_HANDLE_VALUE;
        const auto atomic = testing::CommitStoreChangeWithHook(root, review, yes, cancel, result, error, [&] { held = CreateFileW((root / L"registry.json").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr); });
        if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
        Check(!atomic && Read(root / L"registry.json") == original, "atomic rename failure preserves registry", error);
        Check(CommitStoreChange(root, review, yes, cancel, result, error) && result.changed && result.restart_required, "enable commits and reports restart", error);
        Check(!CommitStoreChange(root, review, yes, cancel, result, error) && error == "store_review_stale", "entry enabled mutation makes old review stale", error);
        Check(ReviewStoreChange(root, native.Declaration().id, StoreChangeAction::Enable, review, error) && CommitStoreChange(root, review, yes, cancel, result, error) && !result.changed && !result.restart_required, "already enabled is idempotent", error);
        Write(dll, "bad");
        Check(ReviewStoreChange(root, native.Declaration().id, StoreChangeAction::Disable, review, error) && CommitStoreChange(root, review, {true,false,false}, cancel, result, error) && result.changed, "disable never executes or reads bad payload", error);
        Write(dll, Pe()); Install(root, next);
        Check(ReviewStoreChange(root, native.Declaration().id, StoreChangeAction::Rollback, review, error) && review.previous_revision == native.Revision(), "rollback reviews previous revision", error);
        consent = yes; consent.native_execution = false;
        Check(!CommitStoreChange(root, review, consent, cancel, result, error), "rollback independently requires native grant", error);
        Write(dll, "bad");
        Check(!CommitStoreChange(root, review, yes, cancel, result, error), "rollback verifies old payload again", error);
        Write(dll, Pe());
        Check(CommitStoreChange(root, review, yes, cancel, result, error) && result.restart_required && ReadInstalledStore(root, snapshot, error) && snapshot.entries[0].revision == native.Revision() && snapshot.entries[0].previous_revision == next.Revision() && !snapshot.entries[0].enabled, "rollback swaps revisions and retains disabled state", error);
        const auto manifest = root / L"versions" / (native.Revision() + ".json");
        Write(manifest, "bad");
        Check(ReadInstalledStore(root, snapshot, error) && !snapshot.entries[0].error.empty(), "bad manifest remains visible with per-entry error", error);
        Check(!ReviewStoreChange(root, native.Declaration().id, StoreChangeAction::Enable, review, error) && !ReviewStoreChange(root, native.Declaration().id, StoreChangeAction::Rollback, review, error), "bad active manifest cannot enable or rollback", error);
        Check(ReviewStoreChange(root, native.Declaration().id, StoreChangeAction::Uninstall, review, error) && CommitStoreChange(root, review, {true,false,false}, cancel, result, error) && result.restart_required && fs::exists(manifest) && fs::exists(dll) && fs::exists(root / L"signature-policy.json") && ReadInstalledStore(root, snapshot, error) && snapshot.entries.empty(), "explicit uninstall removes only registry row and retains bad immutable files and policy", error);
        const auto multi_root = Root(L"multi"); Install(multi_root, multi);
        Write(multi_root / L"packages" / multi.Revision() / L"extra.txt", "unexpected");
        Check(!ReviewStoreChange(multi_root, multi.Declaration().id, StoreChangeAction::Enable, review, error), "multi-file extra member rejected", error);
        // Construct signed historic revisions to test permission and publisher boundaries.
        const auto history = Root(L"history"); Install(history, native); Install(history, next);
        auto registry = pulse::json::Value{}; pulse::json::Parse(Read(history / L"registry.json"), registry);
        auto modified = pulse::json::Value{}; pulse::json::Parse(native.ManifestBytes(), modified);
        auto resources = modified.find("resources") ? *modified.find("resources") : Value::Array(); resources.push("context.read"); modified.set("resources", resources);
        const auto old_bytes = pulse::json::Serialize(modified); const auto old_rev = PackageRevision(old_bytes);
        Write(history / L"versions" / (old_rev + ".json"), old_bytes); Write(history / L"versions" / (old_rev + ".sig.json"), key.Sign(old_bytes));
        auto entries = Value::Array(); auto item = registry.find("packages")->items()[0]; item.set("previousRevision", old_rev); entries.push(item); registry.set("packages", entries); Write(history / L"registry.json", pulse::json::Serialize(registry));
        Check(ReviewStoreChange(history, native.Declaration().id, StoreChangeAction::Rollback, review, error) && review.added_resources == std::vector<std::string>{"context.read"}, "rollback discloses only newly added resources", error);
        consent = yes; consent.added_resources = false;
        Check(!CommitStoreChange(history, review, consent, cancel, result, error) && error == "resource_confirmation_required", "rollback requires added-resource authorization", error);
        Key other; auto policy = Value{}; pulse::json::Parse(Policy(key.publisher), policy); auto pins = *policy.find("publishers"); auto pin = Value::Object(); pin.set("id", other.publisher.id); pin.set("publicKey", other.publisher.public_key); pins.push(pin); policy.set("publishers", pins); Write(history / L"signature-policy.json", pulse::json::Serialize(policy));
        Write(history / L"versions" / (old_rev + ".sig.json"), other.Sign(old_bytes));
        Check(!ReviewStoreChange(history, native.Declaration().id, StoreChangeAction::Rollback, review, error) && error == "installed_publisher_mismatch", "rollback refuses another trusted publisher takeover", error);
        const auto signature_path = identity_root / L"versions" / (native.Revision() + ".sig.json");
        Write(signature_path, "bad");
        Check(ReadInstalledStore(identity_root, snapshot, error) && !snapshot.entries[0].error.empty() && !ReviewStoreChange(identity_root, native.Declaration().id, StoreChangeAction::Enable, review, error), "bad detached signature shown and cannot enable", error);
        Write(signature_path, native.SignatureBytes());
        // A valid signature and matching payload hash must still pass production PE validation.
        auto pe_manifest = Value{}; pulse::json::Parse(native.ManifestBytes(), pe_manifest);
        auto pe_native = *pe_manifest.find("native"); pe_native.set("sha256", PackageRevision("not PE")); pe_manifest.set("native", pe_native);
        const auto pe_bytes = pulse::json::Serialize(pe_manifest); const auto pe_revision = PackageRevision(pe_bytes);
        Write(identity_root / L"versions" / (pe_revision + ".json"), pe_bytes); Write(identity_root / L"versions" / (pe_revision + ".sig.json"), key.Sign(pe_bytes));
        Write(identity_root / L"binaries" / (PackageRevision("not PE") + ".dll"), "not PE");
        auto pe_registry = Value{}; pulse::json::Parse(Read(identity_root / L"registry.json"), pe_registry);
        auto pe_entry = pe_registry.find("packages")->items()[0]; pe_entry.set("activeRevision", pe_revision); auto pe_entries = Value::Array(); pe_entries.push(pe_entry); pe_registry.set("packages", pe_entries);
        const auto identity_registry = Read(identity_root / L"registry.json"); Write(identity_root / L"registry.json", pulse::json::Serialize(pe_registry));
        Check(!ReviewStoreChange(identity_root, native.Declaration().id, StoreChangeAction::Enable, review, error) && error.find("PE32+") != std::string::npos, "signed matching-hash malformed PE cannot enable", error);
        Write(identity_root / L"registry.json", identity_registry);
        {
            InstalledStoreService real;
            const auto refresh = real.Refresh(identity_root);
            Check(real.WaitForPhase(refresh, InstalledStorePhase::Ready, std::chrono::seconds(10)) && real.GetSnapshot().installed->entries.size() == 1, "real worker reads disabled registry snapshot");
            const auto prepare = real.PrepareChange(identity_root, native.Declaration().id, StoreChangeAction::Enable);
            Check(real.WaitForPhase(prepare, InstalledStorePhase::AwaitingConfirmation, std::chrono::seconds(10)) && real.Confirm(prepare, yes) && real.WaitForPhase(prepare, InstalledStorePhase::Changed, std::chrono::seconds(10)) && real.GetSnapshot().installed->entries[0].enabled && real.GetSnapshot().pending_restart, "real worker commits enable then refreshes persisted rows");
            const auto again = real.PrepareChange(identity_root, native.Declaration().id, StoreChangeAction::Enable);
            Check(real.WaitForPhase(again, InstalledStorePhase::AwaitingConfirmation, std::chrono::seconds(10)) && real.Confirm(again, yes) && real.WaitForPhase(again, InstalledStorePhase::Changed, std::chrono::seconds(10)) && real.GetSnapshot().pending_restart, "idempotent worker commit cannot clear pending restart");
        }
        // Deterministic worker gates: stale completion and sticky post-commit restart.
        Gate gate; InstalledStoreService::Operations ops;
        ops.read = [&](const fs::path&, InstalledSnapshot& out, std::string&) { gate.Enter(); out.root = root; return true; };
        InstalledStoreService worker(ops);
        const auto first_gen = worker.Refresh(root); Check(gate.Wait(), "worker refresh reached CV gate");
        const auto latest = worker.PrepareChange(history, native.Declaration().id, StoreChangeAction::Disable);
        gate.Release();
        Check(worker.WaitForPhase(latest, InstalledStorePhase::AwaitingConfirmation, std::chrono::seconds(10)) && worker.GetSnapshot().generation != first_gen, "new generation discards stale refresh completion");
        Check(!worker.Confirm(first_gen, yes), "old generation confirmation rejected"); worker.Stop();
        Gate commit_gate; InstalledStoreService::Operations late_ops;
        late_ops.review = [](const fs::path& path, const std::string& id, StoreChangeAction action, StoreChangeReview& out, std::string&) { out.root = path; out.id = id; out.action = action; return true; };
        late_ops.commit = [&](const fs::path&, const StoreChangeReview&, const StoreChangeConsent&, const std::atomic<bool>&, StoreChangeResult& out, std::string&) { commit_gate.Enter(); out = {true,true}; return true; };
        InstalledStoreService late(late_ops); const auto gen = late.PrepareChange(root, "test.archive", StoreChangeAction::Disable);
        Check(late.WaitForPhase(gen, InstalledStorePhase::AwaitingConfirmation, std::chrono::seconds(10)) && late.Confirm(gen, yes) && !late.Confirm(gen, yes), "confirmation consumed exactly once");
        Check(commit_gate.Wait(), "commit reached CV gate"); late.Cancel(); commit_gate.Release();
        Check(late.WaitForPendingRestart(std::chrono::seconds(10)), "late successful commit preserves restart across cancel");
        late.Stop(); Check(late.GetSnapshot().pending_restart, "Stop cannot clear sticky restart");
        Gate stop_gate; InstalledStoreService::Operations stop_ops;
        stop_ops.review = late_ops.review;
        stop_ops.commit = [&](const fs::path&, const StoreChangeReview&, const StoreChangeConsent&, const std::atomic<bool>&, StoreChangeResult& out, std::string&) { stop_gate.Enter(); out = {true,true}; return true; };
        InstalledStoreService stopping(stop_ops); const auto stop_gen = stopping.PrepareChange(root, "test.archive", StoreChangeAction::Disable);
        Check(stopping.WaitForPhase(stop_gen, InstalledStorePhase::AwaitingConfirmation, std::chrono::seconds(10)) && stopping.Confirm(stop_gen, yes) && stop_gate.Wait(), "Stop test reaches commit gate");
        stopping.Stop(); stop_gate.Release();
        Check(stopping.WaitForPendingRestart(std::chrono::seconds(10)), "successful commit after Stop remains pending restart");
        std::atomic<bool> throw_once = true; InstalledStoreService::Operations recovery_ops;
        recovery_ops.read = [&](const fs::path&, InstalledSnapshot&, std::string&) { if (throw_once.exchange(false)) throw std::runtime_error("injected"); return true; };
        InstalledStoreService recovery(recovery_ops); const auto failed_gen = recovery.Refresh(root);
        Check(recovery.WaitForPhase(failed_gen, InstalledStorePhase::Failed, std::chrono::seconds(10)), "worker converts exception to failure");
        const auto recovered = recovery.Refresh(root);
        Check(recovery.WaitForPhase(recovered, InstalledStorePhase::Ready, std::chrono::seconds(10)), "worker accepts work after exception");
        std::cout << passed << " passed, " << failed << " failed\n";
        return failed ? 1 : 0;
    } catch (const std::exception& e) { std::cout << "[FAIL] exception: " << e.what() << '\n'; return 1; }
}

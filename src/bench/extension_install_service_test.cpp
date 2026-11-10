#include "../app/extensions/store/install_service.h"
#include "extension_package_fixture.h"
#include "pulse_version.h"
#include "../../sdk/native/include/pulse/abi.h"
#include <condition_variable>
#include <future>
#include <iostream>
#include <map>
#include <mutex>

namespace fs = std::filesystem;
using namespace pulse::extensions::store;
using namespace package_fixture;
using Phase = InstallServicePhase;
using namespace std::chrono_literals;
namespace {
int passed = 0, failed = 0;
void Check(bool ok, const char* text) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << text << '\n';
    ok ? ++passed : ++failed;
}
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, released = false;
    void Enter() {
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        if (!cv.wait_for(lock, 10s, [&] { return released; })) throw std::runtime_error("gate release timeout");
    }
    bool Wait() { std::unique_lock lock(mutex); return cv.wait_for(lock, 5s, [&] { return entered; }); }
    void Release() { std::lock_guard lock(mutex); released = true; cv.notify_all(); }
    ~Gate() { Release(); }
};
bool Wait(InstallService& service, uint64_t generation, Phase phase) {
    if (!service.WaitForPhase(generation, phase, 5s)) return false;
    const auto snapshot = service.GetSnapshot();
    if (snapshot.generation != generation || snapshot.phase != phase) {
        std::cout << "diagnostic: " << snapshot.error << '\n';
        return false;
    }
    return true;
}
InstallServiceConfig Config(const fs::path& root) {
    return {root, PULSE_VERSION_STRING_A, static_cast<int>(PULSE_SDK_PREVIEW)};
}
std::shared_ptr<const CatalogSnapshot> CatalogFor(const Fixture& a, const Fixture& b) {
    auto snapshot = std::make_shared<CatalogSnapshot>();
    snapshot->catalog.publishers = {a.publisher, b.publisher};
    for (const auto* f : {&a, &b}) {
        auto extension = f->extension;
        auto release = f->release;
        release.version = "1.0.0";
        release.min_pulse = PULSE_VERSION_STRING_A;
        release.sdk_preview = static_cast<int>(PULSE_SDK_PREVIEW);
        extension.versions = {release};
        snapshot->catalog.extensions.push_back(std::move(extension));
    }
    return snapshot;
}
struct Fake {
    fs::path root;
    std::map<std::string, VerifiedPackage> packages;
    std::atomic<int> downloads = 0, trusts = 0, installs = 0;
    std::atomic<bool> trusted = false, throw_download = false, unknown_download = false, fail_download = false;
    bool restart = true;
    std::shared_ptr<Gate> download_gate, trust_gate, install_gate;
    const PublisherTrustReview* private_trust = nullptr;
    const InstallReview* private_install = nullptr;
    InstallService::Operations Ops() {
        InstallService::Operations ops;
        ops.download = [this](const Catalog&, std::string_view id, std::string_view,
            const std::atomic<bool>&, PackageDownload& out, std::string& error) {
            ++downloads;
            if (download_gate) download_gate->Enter();
            if (throw_download.exchange(false)) throw std::runtime_error("download exploded");
            if (unknown_download.exchange(false)) throw 1;
            if (fail_download.exchange(false)) { error = "synthetic_download_failure"; return false; }
            out.package = packages.at(std::string(id));
            return true;
        };
        ops.review_trust = [this](const fs::path& path, const VerifiedPackage& p,
            PublisherTrustReview& out, std::string&) {
            out.root = path.wstring(); out.package_revision = p.Revision(); out.publisher_id = p.PublisherId();
            out.already_trusted = trusted.load(); private_trust = &out; return true;
        };
        ops.commit_trust = [this](const fs::path& path, const VerifiedPackage& p,
            const PublisherTrustReview& review, bool approved, const std::atomic<bool>&,
            PublisherTrustResult& out, std::string&) {
            ++trusts;
            if (trust_gate) trust_gate->Enter();
            if (!approved || path != root || review.root != root.wstring() || review.package_revision != p.Revision() ||
                review.publisher_id != p.PublisherId() || &review != private_trust) return false;
            trusted = true; out.changed = true; return true;
        };
        ops.review_install = [this](const fs::path& path, const VerifiedPackage& p, InstallReview& out, std::string&) {
            out.root = path.wstring(); out.id = p.Declaration().id; out.revision = p.Revision();
            out.publisher_id = p.PublisherId(); out.native_code = true; private_install = &out; return true;
        };
        ops.commit_install = [this](const fs::path& path, const VerifiedPackage& p, const InstallReview& review,
            const InstallConsent& consent, const std::atomic<bool>&, InstallResult& out, std::string& error) {
            ++installs;
            if (install_gate) install_gate->Enter();
            if (path != root || review.root != root.wstring() || review.id != p.Declaration().id ||
                review.revision != p.Revision() || review.publisher_id != p.PublisherId() || &review != private_install) {
                error = "fake_review_binding_failure"; return false;
            }
            if (!consent.approved || !consent.native_execution) { error = "native_confirmation_required"; return false; }
            out.restart_required = restart; out.enabled = consent.enable_new; return true;
        };
        return ops;
    }
};
}
int wmain() {
    std::cout << std::unitbuf;
    try {
        wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, 32768);
        const auto base = fs::path(module).parent_path() / (L"install-service-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directory(base);
        Key key, other;
        Fixture a(key, 1, Pe(), "test.first"), b(other, 1, Pe(), "test.second");
        VerifiedPackage pa, pb; std::string error;
        Check(Verify(a, pa, error) && Verify(b, pb, error), "real CNG signatures and native memory packages verify");
        if (!pa || !pb) throw std::runtime_error(error);
        const auto catalog = CatalogFor(a, b);
        auto make_fake = [&](const fs::path& root) {
            auto fake = std::make_unique<Fake>(); fake->root = root;
            fake->packages = {{a.extension.id, pa}, {b.extension.id, pb}}; return fake;
        };
        {
            auto fake = make_fake(base / L"fake-normal");
            InstallService service(Config(fake->root), fake->Ops());
            Check(service.GetSnapshot().phase == Phase::Idle, "initial state is idle");
            const auto gen = service.Prepare(catalog, a.extension.id, "1.0.0");
            Check(Wait(service, gen, Phase::AwaitingPublisherTrust), "prepare downloads and waits for explicit publisher trust");
            auto snapshot = service.GetSnapshot();
            Check(snapshot.package && snapshot.package->PublisherId() == a.publisher.id && !fake->trusted && fake->trusts == 0,
                "catalog authentication is not automatic publisher trust");
            Check(snapshot.trust_review && snapshot.trust_review.get() != fake->private_trust,
                "display trust review is not worker authority object");
            Check(!fs::exists(fake->root), "prepare and fake reviews create no store");
            Check(!service.ConfirmInstall(gen, {true, true, true, true}) && !service.ConfirmPublisherTrust(gen + 1, true),
                "wrong stage and stale trust generation rejected");
            Check(service.ConfirmPublisherTrust(gen, true) && !service.ConfirmPublisherTrust(gen, true), "trust command immediately consumes awaiting phase");
            Check(Wait(service, gen, Phase::AwaitingInstall) && fake->trusted && fake->trusts == 1, "trust approval commits then independently reviews install");
            snapshot = service.GetSnapshot();
            Check(snapshot.install_review && snapshot.install_review.get() != fake->private_install && snapshot.install_review->native_code,
                "display install review is separate and exposes native execution");
            Check(service.ConfirmInstall(gen, {true, false, true, true}), "native consent is not synthesized by coordinator");
            Check(Wait(service, gen, Phase::Failed) && service.GetSnapshot().error == "native_confirmation_required", "core rejects missing independent native grant");
            const auto next = service.Prepare(catalog, a.extension.id, "1.0.0");
            Check(Wait(service, next, Phase::AwaitingInstall) && fake->trusts == 1, "already trusted publisher skips trust only");
            Check(!service.ConfirmInstall(gen, {true, true, true, true}), "old install button rejected after reprepare");
            Check(service.ConfirmInstall(next, {true, true, true, true}) && !service.ConfirmInstall(next, {true, true, true, true}), "install command rejects double click");
            Check(Wait(service, next, Phase::Installed) && service.GetSnapshot().enabled && service.GetSnapshot().pending_restart,
                "native grant commits enabled result and pending restart");
            fake->restart = false;
            const auto repeat = service.Prepare(catalog, a.extension.id, "1.0.0");
            Check(Wait(service, repeat, Phase::AwaitingInstall) && service.ConfirmInstall(repeat, {true, true, true, false}) && Wait(service, repeat, Phase::Installed),
                "idempotent install can complete with no restart change");
            Check(service.GetSnapshot().pending_restart && !service.GetSnapshot().enabled, "idempotent false result does not clear sticky restart");
            service.Cancel(); Check(service.GetSnapshot().pending_restart, "cancel retains sticky restart");
        }
        {
            auto fake = make_fake(base / L"fake-switch");
            InstallService service(Config(fake->root), fake->Ops());
            const auto old = service.Prepare(catalog, a.extension.id, "1.0.0");
            Check(Wait(service, old, Phase::AwaitingPublisherTrust), "first publisher awaits trust before switch");
            const auto latest = service.Prepare(catalog, b.extension.id, "1.0.0");
            Check(!service.ConfirmPublisherTrust(old, true) && Wait(service, latest, Phase::AwaitingPublisherTrust), "switch rejects prior package and publisher button");
            Check(service.GetSnapshot().trust_review->publisher_id == b.publisher.id && service.ConfirmPublisherTrust(latest, true) &&
                Wait(service, latest, Phase::AwaitingInstall), "new package review remains bound to new publisher");
            Check(service.ConfirmInstall(latest, {true, true, true, true}) && Wait(service, latest, Phase::Installed), "worker originals retain root package publisher association");
            const auto declined = service.Prepare(catalog, a.extension.id, "1.0.0");
            Check(Wait(service, declined, Phase::AwaitingInstall), "trusted fake state still requires fresh install review");
        }
        {
            auto fake = make_fake(base / L"fake-decline");
            InstallService service(Config(fake->root), fake->Ops());
            const auto gen = service.Prepare(catalog, a.extension.id, "1.0.0");
            Check(Wait(service, gen, Phase::AwaitingPublisherTrust) && service.ConfirmPublisherTrust(gen, false) && Wait(service, gen, Phase::Cancelled), "explicit declined publisher trust cancels command");
            Check(fake->trusts == 0 && !fs::exists(fake->root), "decline never calls trust commit or creates root");
        }
        {
            auto fake = make_fake(base / L"fake-latest"); fake->download_gate = std::make_shared<Gate>();
            InstallService service(Config(fake->root), fake->Ops());
            const auto old = service.Prepare(catalog, a.extension.id, "1.0.0");
            Check(fake->download_gate->Wait(), "download gate entered without sleep polling");
            service.Prepare(catalog, a.extension.id, "1.0.0");
            const auto latest = service.Prepare(catalog, b.extension.id, "1.0.0");
            auto loading = service.GetSnapshot();
            Check(loading.generation == latest && latest > old && loading.phase == Phase::Preparing && !loading.package,
                "new prepare immediately publishes fresh loading generation");
            fake->download_gate->Release();
            Check(Wait(service, latest, Phase::AwaitingPublisherTrust) && fake->downloads == 2 && service.GetSnapshot().id == b.extension.id,
                "only latest pending request runs and late download cannot overwrite it");
        }
        {
            auto fake = make_fake(base / L"fake-stale-exception");
            fake->download_gate = std::make_shared<Gate>(); fake->throw_download = true;
            InstallService service(Config(fake->root), fake->Ops());
            service.Prepare(catalog, a.extension.id, "1.0.0");
            Check(fake->download_gate->Wait(), "old failing download is held at gate");
            const auto latest = service.Prepare(catalog, b.extension.id, "1.0.0");
            fake->download_gate->Release();
            Check(Wait(service, latest, Phase::AwaitingPublisherTrust) && service.GetSnapshot().error.empty() &&
                service.GetSnapshot().id == b.extension.id, "late old exception never replaces latest generation");
        }
        for (bool stop : {false, true}) {
            auto fake = make_fake(base / (stop ? L"fake-stop-commit" : L"fake-cancel-commit")); fake->trusted = true;
            fake->install_gate = std::make_shared<Gate>();
            InstallService service(Config(fake->root), fake->Ops());
            const auto old = service.Prepare(catalog, a.extension.id, "1.0.0");
            Check(Wait(service, old, Phase::AwaitingInstall) && service.ConfirmInstall(old, {true, true, true, true}) && fake->install_gate->Wait(),
                "install reaches controlled late-commit boundary");
            if (stop) service.Stop(); else service.Cancel();
            const auto cancelled = service.GetSnapshot().generation;
            // Queue a barrier generation for Cancel; for Stop destructor is the join barrier.
            fake->install_gate->Release();
            if (!stop) {
                const auto latest = service.Prepare(catalog, b.extension.id, "1.0.0");
                Check(Wait(service, latest, Phase::AwaitingInstall) && service.GetSnapshot().pending_restart && !service.GetSnapshot().enabled,
                    "late successful old commit leaves sticky restart without replacing new generation or enabled");
            } else {
                Check(service.Prepare(catalog, b.extension.id, "1.0.0") == cancelled && !service.ConfirmInstall(old, {true, true, true, true}),
                    "stopped service cannot restart or replay commands");
                Check(service.WaitForPendingRestart(5s) && service.GetSnapshot().generation == cancelled &&
                    service.GetSnapshot().phase == Phase::Cancelled && !service.GetSnapshot().enabled,
                    "Stop still records late successful commit restart without replacing cancelled state");
            }
        }
        {
            auto fake = make_fake(base / L"fake-trust-cancel"); fake->trust_gate = std::make_shared<Gate>();
            InstallService service(Config(fake->root), fake->Ops());
            const auto old = service.Prepare(catalog, a.extension.id, "1.0.0");
            Check(Wait(service, old, Phase::AwaitingPublisherTrust) && service.ConfirmPublisherTrust(old, true) && fake->trust_gate->Wait(), "trust reaches controlled commit boundary");
            service.Cancel(); const auto next = service.Prepare(catalog, a.extension.id, "1.0.0");
            fake->trust_gate->Release();
            Check(Wait(service, next, Phase::AwaitingInstall) && fake->trusted && fake->trusts == 1, "cancel after trust commit does not automatically revoke publisher");
        }
        {
            auto fake = make_fake(base / L"fake-compatible"); InstallService service(Config(fake->root), fake->Ops());
            for (int mode = 0; mode != 4; ++mode) {
                auto incompatible = std::make_shared<CatalogSnapshot>(*catalog);
                auto& release = incompatible->catalog.extensions.front().versions.front();
                if (mode == 0) release.min_pulse = "999999.0.0";
                if (mode == 1) ++release.sdk_preview;
                if (mode == 2) release.min_pulse = "1.2.3-extra";
                if (mode == 3) incompatible->catalog.extensions.clear();
                const auto gen = service.Prepare(incompatible, a.extension.id, "1.0.0");
                Check(Wait(service, gen, Phase::Incompatible) && fake->downloads == 0, "compatibility and catalog selection reject before any download");
            }
            const auto gen = service.Prepare({}, a.extension.id, "1.0.0");
            Check(Wait(service, gen, Phase::Incompatible) && fake->downloads == 0, "missing catalog rejected without download");
            auto helper_release = catalog->catalog.extensions.front().versions.front();
            Check(IsReleaseCompatible(helper_release, PULSE_VERSION_STRING_A, static_cast<int>(PULSE_SDK_PREVIEW)),
                "public helper accepts exact host version and SDK");
            Check(!IsReleaseCompatible(helper_release, PULSE_VERSION_STRING_A, static_cast<int>(PULSE_SDK_PREVIEW) + 1) &&
                !IsReleaseCompatible(helper_release, PULSE_VERSION_STRING_A, 0), "public helper rejects mismatched and invalid SDK");
            helper_release.min_pulse = "2.10.0";
            Check(IsReleaseCompatible(helper_release, "2.10.0", static_cast<int>(PULSE_SDK_PREVIEW)) &&
                IsReleaseCompatible(helper_release, "2.11.0", static_cast<int>(PULSE_SDK_PREVIEW)) &&
                !IsReleaseCompatible(helper_release, "2.9.99", static_cast<int>(PULSE_SDK_PREVIEW)),
                "public helper compares three numeric components, not strings");
            helper_release.min_pulse = "002.010.0000";
            Check(IsReleaseCompatible(helper_release, "2.10.0", static_cast<int>(PULSE_SDK_PREVIEW)),
                "public helper accepts catalog-valid leading zeros in minimum release version");
            helper_release.min_pulse = "2.10";
            Check(!IsReleaseCompatible(helper_release, "2.10.0", static_cast<int>(PULSE_SDK_PREVIEW)),
                "public helper returns false for invalid minimum version without throwing");
            helper_release.min_pulse = "0.0.0";
            for (const auto* invalid : {"", "1.2", "1.2.3.4", "01.2.3", "-1.2.3", "18446744073709551616.2.3"}) {
                Check(!IsReleaseCompatible(helper_release, invalid, static_cast<int>(PULSE_SDK_PREVIEW)),
                    "public helper returns false for invalid host without throwing");
                bool rejected = false; auto config = Config(fake->root); config.host_version = invalid;
                try { InstallService bad(config, fake->Ops()); } catch (const std::invalid_argument&) { rejected = true; }
                Check(rejected, "invalid host version rejected before worker starts");
            }
            auto padded = std::make_shared<CatalogSnapshot>(*catalog);
            padded->catalog.extensions.front().versions.front().min_pulse = "000.000.0000";
            const auto padded_gen = service.Prepare(padded, a.extension.id, "1.0.0");
            Check(Wait(service, padded_gen, Phase::AwaitingPublisherTrust) && fake->downloads == 1,
                "catalog-valid zero-padded minPulse uses numeric comparison");
            bool rejected = false;
            try { auto config = Config(L"relative-store"); InstallService bad(config, fake->Ops()); } catch (const std::invalid_argument&) { rejected = true; }
            Check(rejected, "relative store root rejected");
        }
        {
            auto fake = make_fake(base / L"fake-exceptions"); InstallService service(Config(fake->root), fake->Ops());
            for (int mode = 0; mode != 3; ++mode) {
                if (mode == 0) fake->throw_download = true;
                if (mode == 1) fake->unknown_download = true;
                if (mode == 2) fake->fail_download = true;
                const auto gen = service.Prepare(catalog, a.extension.id, "1.0.0");
                Check(Wait(service, gen, Phase::Failed) && !service.GetSnapshot().error.empty(), "operation failure or exception becomes diagnostic failed state");
            }
            const auto gen = service.Prepare(catalog, a.extension.id, "1.0.0");
            Check(Wait(service, gen, Phase::AwaitingPublisherTrust), "worker recovers after ordinary and unknown exceptions");
        }
        {
            auto fake = make_fake(base / L"fake-destroy"); fake->download_gate = std::make_shared<Gate>();
            auto service = std::make_unique<InstallService>(Config(fake->root), fake->Ops());
            service->Prepare(catalog, a.extension.id, "1.0.0"); Check(fake->download_gate->Wait(), "destruction test worker is active");
            service->Stop(); const auto generation = service->GetSnapshot().generation; service->Stop();
            Check(service->GetSnapshot().generation == generation && service->GetSnapshot().phase == Phase::Cancelled, "Stop is nonjoining and idempotent");
            auto destroyed = std::async(std::launch::async, [owner = std::move(service)]() mutable { owner.reset(); });
            fake->download_gate->Release();
            Check(destroyed.wait_for(5s) == std::future_status::ready, "destructor joins safely away from UI thread after cancellation");
            destroyed.get();
        }
        {
            // Only download is replaced: production trust and install operations
            // execute on an isolated nonexistent root with a real verified native package.
            const auto root = base / L"real-store";
            auto fake = make_fake(root); auto ops = fake->Ops();
            ops.review_trust = ReviewPublisherTrust; ops.commit_trust = CommitPublisherTrust;
            ops.review_install = ReviewInstall; ops.commit_install = CommitInstall;
            InstallService service(Config(root), std::move(ops));
            const auto gen = service.Prepare(catalog, a.extension.id, "1.0.0");
            Check(Wait(service, gen, Phase::AwaitingPublisherTrust) && !fs::exists(root), "real trust review leaves missing store uncreated");
            Check(service.ConfirmPublisherTrust(gen, true) && Wait(service, gen, Phase::AwaitingInstall) &&
                fs::exists(root / L"signature-policy.json") && !fs::exists(root / L"registry.json"), "real explicit trust creates policy but does not install");
            Check(service.ConfirmInstall(gen, {true, true, true, true}) && Wait(service, gen, Phase::Installed) &&
                fs::exists(root / L"registry.json") && service.GetSnapshot().pending_restart && service.GetSnapshot().enabled,
                "real trust to native consent to registry commit chain succeeds");
            const auto next = service.Prepare(catalog, a.extension.id, "1.0.0");
            Check(Wait(service, next, Phase::AwaitingInstall) && service.ConfirmInstall(next, {true, true, true, true}) && Wait(service, next, Phase::Installed) &&
                service.GetSnapshot().pending_restart, "real trusted idempotent repeat retains pending restart");
        }
        // Keep isolated evidence under the independent build directory; never touch prefs.
        std::cout << "fixture: " << base.string() << '\n';
    } catch (const std::exception& ex) { Check(false, ex.what()); }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}

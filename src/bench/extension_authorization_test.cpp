#include "../app/extensions/authorization.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>
#include <type_traits>

using namespace pulse::extensions::authorization;
namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
struct Fixture {
    uint64_t now = 100;
    uint8_t nonce = 1;
    bool random_ok = true;
    Registry registry;
    explicit Fixture(Limits limits = {}) : registry(limits, [this] { return now; }, [this](std::span<uint8_t> bytes) {
        std::fill(bytes.begin(), bytes.end(), nonce++);
        return random_ok;
    }) {}
};
std::optional<std::string> Issue(Registry& registry, const PackageIdentity& package, OwnerHandle owner) {
    const auto binding = registry.CaptureBinding(package, owner);
    return binding ? registry.Issue(*binding) : std::nullopt;
}
PackageIdentity Package(size_t slot = 0) {
    return {slot, "package." + std::to_string(slot), "revision.one", 1};
}
}

int main() {
    static_assert(!std::is_default_constructible_v<PrincipalBinding>);
    static_assert(!std::is_aggregate_v<PrincipalBinding>);
    {
        Limits limits; limits.lifetime_ms = 10;
        Fixture f(limits); const auto package = Package();
        f.registry.SyncPackages({{package, true}});
        const auto owner = *f.registry.RegisterOwner(7);
        const auto binding = *f.registry.CaptureBinding(package, owner);
        const auto first = *f.registry.Issue(binding);
        f.now += 10;
        Check(!f.registry.Validate(first, package, owner) && f.registry.ValidateBinding(binding),
            "expired async ticket does not expire the live host binding");
        const auto next = f.registry.Issue(binding);
        Check(next && *next != first && !f.registry.Validate(first, package, owner),
            "explicit new request may issue fresh short ticket without renewing old one");
        f.registry.SyncPackages({{package, false}});
        f.registry.SyncPackages({{package, true}});
        Check(!f.registry.ValidateBinding(binding) && !f.registry.Issue(binding),
            "old binding cannot issue after disable then reenable");
        const auto rebound = *f.registry.CaptureBinding(package, owner);
        Check(f.registry.ValidateBinding(rebound) && f.registry.Issue(rebound).has_value(),
            "explicitly captured new binding can issue after reenable");
        auto reloaded = package; ++reloaded.generation;
        f.registry.SyncPackages({{reloaded, true}});
        Check(!f.registry.ValidateBinding(rebound) && !f.registry.Issue(rebound), "reload revokes persistent binding");
        const auto reload_binding = *f.registry.CaptureBinding(reloaded, owner);
        const auto replacement_owner = *f.registry.RegisterOwner(owner.key);
        Check(!f.registry.ValidateBinding(reload_binding), "owner key reuse revokes persistent binding");
        const auto replacement = *f.registry.CaptureBinding(reloaded, replacement_owner);
        f.registry.UnregisterOwner(replacement_owner);
        Check(!f.registry.ValidateBinding(replacement) && !f.registry.Issue(replacement), "owner destruction revokes persistent binding");
        Fixture other(limits); other.registry.SyncPackages({{package, true}});
        other.registry.RegisterOwner(7);
        Check(!other.registry.ValidateBinding(binding) && !other.registry.Issue(binding),
            "binding cannot cross registry session even when counters match");
    }
    {
        Fixture f;
        const auto package = Package();
        const auto other_package = Package(1);
        Check(f.registry.SyncPackages({{package, true}, {other_package, true}}), "trusted snapshot accepted");
        const auto owner = *f.registry.RegisterOwner(10);
        const auto other_owner = *f.registry.RegisterOwner(20);
        const auto ticket = Issue(f.registry, package, owner);
        Check(ticket && ticket->size() == 80 && f.registry.Validate(*ticket, package, owner), "ticket binds enabled package and live owner");
        Check(Issue(f.registry, package, owner) == ticket, "live principal reuses its ticket");
        Check(!f.registry.Validate(*ticket, other_package, owner), "cross-package expected identity rejected");
        Check(!f.registry.Validate(*ticket, package, other_owner), "cross-owner expected identity rejected");
        auto claimed = package; claimed.id = "claimed";
        Check(!Issue(f.registry, claimed, owner) && !f.registry.Validate(*ticket, claimed, owner), "claimed identity cannot replace host identity");
        Check(!f.registry.Validate("", package, owner) && !f.registry.Validate(std::string(80, 'x'), package, owner), "unknown tickets rejected");
        Check(f.registry.SyncPackages({{package, true}, {other_package, true}}) && f.registry.Validate(*ticket, package, owner),
            "unchanged trusted snapshot preserves grant");
        f.registry.SyncPackages({{package, false}, {other_package, true}});
        Check(!f.registry.Validate(*ticket, package, owner) && !Issue(f.registry, package, owner), "disabled package cannot validate or issue");
        f.registry.SyncPackages({{package, true}, {other_package, true}});
        Check(!f.registry.Validate(*ticket, package, owner), "reenabling never revives old ticket");
        const auto renewed = Issue(f.registry, package, owner);
        Check(renewed && *renewed != *ticket, "reenabling issues a fresh grant");
        auto reloaded = package; reloaded.generation++;
        f.registry.SyncPackages({{reloaded, true}});
        Check(!f.registry.Validate(*renewed, reloaded, owner), "generation reload revokes prior grant");
        const auto generation_ticket = Issue(f.registry, reloaded, owner);
        reloaded.revision = "revision.two";
        f.registry.SyncPackages({{reloaded, true}});
        Check(!f.registry.Validate(*generation_ticket, reloaded, owner), "revision change revokes grant");
        const auto revision_ticket = Issue(f.registry, reloaded, owner);
        f.registry.SyncPackages({}); f.registry.SyncPackages({{reloaded, true}});
        Check(!f.registry.Validate(*revision_ticket, reloaded, owner), "remove and readd revokes prior grant");
    }
    {
        Fixture f;
        const auto package = Package();
        f.registry.SyncPackages({{package, true}});
        const auto first = *f.registry.RegisterOwner(100);
        const auto ticket = *Issue(f.registry, package, first);
        const auto second = *f.registry.RegisterOwner(100);
        Check(first.incarnation != second.incarnation && !f.registry.Validate(ticket, package, second) &&
            !f.registry.Validate(ticket, package, first), "owner key reuse creates new incarnation");
        const auto second_ticket = *Issue(f.registry, package, second);
        f.registry.UnregisterOwner(first);
        Check(f.registry.Validate(second_ticket, package, second), "stale owner teardown does not revoke newer incarnation");
        f.registry.UnregisterOwner(second);
        Check(!f.registry.Validate(second_ticket, package, second), "owner teardown revokes grant");
        Check(!f.registry.RegisterOwner(0), "null owner key rejected");
    }
    {
        Limits limits; limits.lifetime_ms = 10; limits.live_tickets = 1;
        Fixture f(limits);
        const auto package = Package(); f.registry.SyncPackages({{package, true}});
        const auto owner = *f.registry.RegisterOwner(1);
        const auto ticket = *Issue(f.registry, package, owner);
        f.now = 109; Check(f.registry.Validate(ticket, package, owner), "ticket valid before exact expiry");
        f.now = 110; Check(!f.registry.Validate(ticket, package, owner), "ticket invalid at expiry");
        const auto renewed = Issue(f.registry, package, owner);
        Check(renewed && *renewed != ticket, "expired ticket cleanup releases live quota");
        f.now = 90; Check(!f.registry.Validate(*renewed, package, owner), "backward injected clock fails closed");
    }
    {
        Fixture f;
        const auto package = Package(); f.registry.SyncPackages({{package, true}});
        const auto owner = *f.registry.RegisterOwner(1);
        f.random_ok = false;
        Check(!Issue(f.registry, package, owner), "random generator failure rejects issuance");
        f.random_ok = true; f.nonce = 1;
        const auto first = *Issue(f.registry, package, owner);
        const auto second_owner = *f.registry.RegisterOwner(2);
        f.nonce = 1;
        Check(!Issue(f.registry, package, second_owner), "duplicate previous random nonce rejected");
        f.nonce = 2; const auto second = Issue(f.registry, package, second_owner);
        const auto third_owner = *f.registry.RegisterOwner(3);
        f.nonce = 1;
        Check(second && !Issue(f.registry, package, third_owner), "duplicate live random nonce rejected");
        f.now += 5 * 60 * 1000;
        f.nonce = 1;
        const auto repeated_nonce = Issue(f.registry, package, owner);
        Check(repeated_nonce && *repeated_nonce != first && !f.registry.Validate(first, package, owner),
            "expired random nonce repetition cannot recreate a complete ticket");
    }
    {
        Limits limits; limits.packages = 1; limits.owners = 2; limits.live_tickets = 1;
        Fixture f(limits); const auto package = Package();
        f.registry.SyncPackages({{package, true}});
        Check(!f.registry.SyncPackages({{package, true}, {Package(1), true}}), "package quota enforced transactionally");
        const auto first = *f.registry.RegisterOwner(1);
        const auto second = *f.registry.RegisterOwner(2);
        Check(!f.registry.RegisterOwner(3), "owner quota enforced");
        const auto ticket = Issue(f.registry, package, first);
        Check(ticket && !Issue(f.registry, package, second), "live ticket quota enforced");
        f.registry.UnregisterOwner(first);
        Check(Issue(f.registry, package, second).has_value(), "revocation releases live ticket quota");
    }
    {
        Fixture f; const auto package = Package();
        f.registry.SyncPackages({{package, true}});
        const auto owner = *f.registry.RegisterOwner(1); const auto ticket = *Issue(f.registry, package, owner);
        auto duplicate = package; duplicate.slot = 1;
        Check(!f.registry.SyncPackages({{package, true}, {duplicate, true}}) && f.registry.Validate(ticket, package, owner),
            "duplicate package IDs reject snapshot without losing old grant");
        duplicate.id = "different"; duplicate.slot = 0;
        Check(!f.registry.SyncPackages({{package, true}, {duplicate, true}}), "duplicate slots rejected");
        std::atomic<int> valid{0};
        std::vector<std::thread> workers;
        for (int i = 0; i < 4; ++i) workers.emplace_back([&] {
            for (int j = 0; j < 100; ++j) if (f.registry.Validate(ticket, package, owner)) ++valid;
        });
        for (auto& worker : workers) worker.join();
        Check(valid == 400, "workers can concurrently validate host-owned identities");
    }
    {
        Registry production;
        const auto package = Package(); production.SyncPackages({{package, true}});
        const auto owner = *production.RegisterOwner(1);
        const auto ticket = Issue(production, package, owner);
        Check(ticket && production.Validate(*ticket, package, owner), "default production clock and BCrypt random issue valid ticket");
    }
    return failures ? 1 : 0;
}

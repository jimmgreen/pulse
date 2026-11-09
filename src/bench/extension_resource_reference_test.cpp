#include "../app/extensions/resource_reference_store.h"
#include <cstdio>

namespace {
int failures = 0;
void Check(bool value, const char *label) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    if (!value)
        ++failures;
}
} // namespace
int main() {
    using namespace pulse::extensions;
    uint64_t now = 1000;
    authorization::Registry registry({}, [&] { return now; });
    authorization::PackageIdentity first{0, "first", "revision-a", 1}, second{1, "second", "revision-b", 1};
    Check(registry.SyncPackages({{first, true}, {second, true}}),
          "real registry initializes two independent package identities");
    const auto owner = registry.RegisterOwner(10), other_owner = registry.RegisterOwner(20);
    if (!owner || !other_owner)
        return 1;
    const auto caller = registry.CaptureBinding(first, *owner),
               other_package = registry.CaptureBinding(second, *owner),
               other_window = registry.CaptureBinding(first, *other_owner);
    if (!caller || !other_package || !other_window)
        return 1;
    ResourceReferenceStore store(registry, [&] { return now; });
    const auto snapshot = std::make_shared<const int>(42), refreshed = std::make_shared<const int>(42);
    const std::wstring path = L"C:\\isolated";
    const auto entry = store.Issue(*caller, snapshot, path, 0, false);
    const auto cursor = store.Issue(*caller, snapshot, path, 2, true);
    Check(!entry.empty() && !cursor.empty() && entry != cursor,
          "actual secure opaque tokens distinguish entry and cursor");
    Check(store.Resolve(entry, *caller, snapshot, path, false) == 0 &&
              store.Resolve(cursor, *caller, snapshot, path, true) == 2,
          "own references resolve snapshot-bound positions including zero");
    Check(!store.Resolve(entry, *other_package, snapshot, path, false) &&
              !store.Resolve(cursor, *other_package, snapshot, path, true),
          "same owner different live package cannot resolve entry or cursor");
    Check(!store.Resolve(entry, *other_window, snapshot, path, false),
          "same package different owner cannot resolve reference");
    Check(!store.Resolve(entry, *caller, refreshed, path, false),
          "identical data in a different snapshot rejects old token");
    Check(!store.Resolve(entry, *caller, snapshot, L"C:\\different", false),
          "same snapshot at another path rejects token");
    Check(!store.Resolve(entry, *caller, snapshot, path, true) &&
              !store.Resolve(cursor, *caller, snapshot, path, false),
          "entry and pagination token kinds cannot be substituted");
    Check(ResourceReferenceStore::kLifetimeMs == 300000, "production reference TTL remains exactly five minutes");
    now = 1000 + 299999;
    Check(store.Resolve(entry, *caller, snapshot, path, false).has_value(),
          "token survives 299999 milliseconds without shortening production TTL");
    now += 1;
    Check(!store.Resolve(entry, *caller, snapshot, path, false) &&
              !store.Resolve(cursor, *caller, snapshot, path, true),
          "entry and cursor expire exactly at five minutes; read does not renew lifetime");
    const auto before_reload = store.Issue(*caller, snapshot, path, 3, false);
    auto next = first;
    next.revision = "revision-new";
    ++next.generation;
    Check(registry.SyncPackages({{next, true}, {second, true}}),
          "production registry applies same-package reload revision and generation");
    const auto reloaded = registry.CaptureBinding(next, *owner);
    if (!reloaded)
        return 1;
    Check(!registry.ValidateBinding(*caller) &&
              !store.Resolve(before_reload, *caller, snapshot, path, false) &&
              !store.Resolve(before_reload, *reloaded, snapshot, path, false),
          "reload rejects both stale principal and new principal using old token");
    const auto fresh = store.Issue(*reloaded, snapshot, path, 4, false);
    Check(store.Resolve(fresh, *reloaded, snapshot, path, false) == 4,
          "new reload principal receives usable new references");
    registry.UnregisterOwner(*owner);
    const auto reincarnated = registry.RegisterOwner(10);
    if (!reincarnated)
        return 1;
    const auto new_window = registry.CaptureBinding(next, *reincarnated);
    if (!new_window)
        return 1;
    Check(!store.Resolve(fresh, *new_window, snapshot, path, false),
          "same owner key with new incarnation cannot adopt old reference");
    const auto other_current = registry.CaptureBinding(next, *other_owner);
    if (!other_current)
        return 1;
    const auto kept = store.Issue(*other_current, snapshot, path, 5, false);
    const auto removed = store.Issue(*new_window, snapshot, path, 6, false);
    store.ReleaseOwner(10);
    Check(!store.Resolve(removed, *new_window, snapshot, path, false) &&
              store.Resolve(kept, *other_current, snapshot, path, false) == 5,
          "closing one resource owner clears its references without affecting another");
    auto retained = std::make_shared<const int>(7);
    std::weak_ptr<const int> weak = retained;
    store.Issue(*new_window, retained, path, 7, false);
    retained.reset();
    Check(!weak.expired(), "live reference retains immutable snapshot safely");
    now += ResourceReferenceStore::kLifetimeMs;
    store.Prune();
    Check(weak.expired(), "expiration releases retained snapshot instead of leaking stale data");
    const auto clock_entry = store.Issue(*new_window, snapshot, path, 8, false);
    --now;
    Check(!store.Resolve(clock_entry, *new_window, snapshot, path, false),
          "backward internal test clock fails closed");
    ++now;
    for (size_t i = 0; i < ResourceReferenceStore::kCapacity; ++i)
        if (store.Issue(*new_window, snapshot, path, i, false).empty()) {
            Check(false, "capacity setup failed");
            return 1;
        }
    Check(!store.HasCapacity(1) && store.Issue(*new_window, snapshot, path, 0, false).empty(),
          "4096 live reference budget fails closed");
    now += ResourceReferenceStore::kLifetimeMs;
    Check(store.HasCapacity(ResourceReferenceStore::kCapacity),
          "expiry recovers capacity without changing TTL or global clock");
    std::printf("Result: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

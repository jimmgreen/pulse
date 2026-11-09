#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <utility>

namespace pulse::extensions::authorization {

// Host-only shared production entropy source; false means callers must fail closed.
bool SecureRandomBytes(std::span<uint8_t> output);

struct PackageIdentity {
    size_t slot = 0;
    std::string id;
    std::string revision;
    uint64_t generation = 0;
    bool operator==(const PackageIdentity&) const = default;
};
struct PackageState {
    PackageIdentity identity;
    bool enabled = false;
};
struct OwnerHandle {
    uintptr_t key = 0;
    uint64_t incarnation = 0;
    bool operator==(const OwnerHandle&) const = default;
};
struct Limits {
    size_t packages = 32;
    size_t owners = 64;
    size_t live_tickets = 128;
    uint64_t lifetime_ms = 5 * 60 * 1000;
};

// A host-held, non-serializable binding. Only Registry can capture an epoch;
// claimed package/owner fields cannot be used to reconstruct a revoked binding.
class PrincipalBinding {
public:
    const PackageIdentity& Package() const noexcept { return package_; }
    OwnerHandle Owner() const noexcept { return owner_; }
private:
    friend class Registry;
    PrincipalBinding(PackageIdentity package, OwnerHandle owner, uint64_t epoch, std::weak_ptr<const uint8_t> issuer)
        : package_(std::move(package)), owner_(owner), grant_epoch_(epoch), issuer_(std::move(issuer)) {}
    PackageIdentity package_;
    OwnerHandle owner_;
    uint64_t grant_epoch_;
    std::weak_ptr<const uint8_t> issuer_;
};

// Host lifecycle registry for package generations, enabled state and owner lifetimes.
// Bindings and broker tickets prevent stale asynchronous work from surviving teardown;
// they do not restrict the authority of in-process native DLLs.
class Registry {
public:
    using Clock = std::function<uint64_t()>;
    using Random = std::function<bool(std::span<uint8_t>)>;
    explicit Registry(Limits limits = {}, Clock clock = {}, Random random = {});
    bool SyncPackages(const std::vector<PackageState>& packages);
    std::optional<OwnerHandle> RegisterOwner(uintptr_t key);
    void UnregisterOwner(OwnerHandle owner);
    std::optional<PrincipalBinding> CaptureBinding(const PackageIdentity& expected_package, OwnerHandle owner);
    bool ValidateBinding(const PrincipalBinding& binding);
    std::optional<std::string> Issue(const PrincipalBinding& binding);
    bool Validate(std::string_view ticket, const PackageIdentity& expected_package, OwnerHandle owner);

private:
    struct PackageRecord { PackageState state; uint64_t epoch = 0; };
    struct Grant { size_t slot; uint64_t epoch; OwnerHandle owner; uint64_t issued_ms; };
    bool PrincipalMatches(const PackageIdentity& package, OwnerHandle owner) const;
    bool BindingCurrent(const PrincipalBinding& binding) const;
    bool GrantCurrent(const Grant& grant, uint64_t now) const;
    void Cleanup(uint64_t now);
    Limits limits_;
    Clock clock_;
    Random random_;
    std::mutex mutex_;
    std::shared_ptr<const uint8_t> issuer_ = std::make_shared<const uint8_t>(uint8_t{0});
    uint64_t next_epoch_ = 0;
    uint64_t next_owner_ = 0;
    uint64_t next_ticket_ = 0;
    std::optional<std::array<uint8_t, 32>> last_nonce_;
    std::unordered_map<size_t, PackageRecord> packages_;
    std::unordered_map<uintptr_t, uint64_t> owners_;
    std::unordered_map<std::string, Grant> grants_;
};

}

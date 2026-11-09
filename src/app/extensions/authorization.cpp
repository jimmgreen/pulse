#include "authorization.h"
#include <windows.h>
#include <bcrypt.h>
#include <chrono>
#include <limits>
#include <utility>
#include <unordered_set>

namespace pulse::extensions::authorization {
namespace {
uint64_t MonotonicMilliseconds() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}
bool SecureRandomBytes(std::span<uint8_t> output) {
    if (output.size() > (std::numeric_limits<ULONG>::max)()) return false;
    return BCryptGenRandom(nullptr, output.data(), static_cast<ULONG>(output.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

Registry::Registry(Limits limits, Clock clock, Random random)
    : limits_(limits), clock_(clock ? std::move(clock) : Clock{MonotonicMilliseconds}),
      random_(random ? std::move(random) : Random{SecureRandomBytes}) {}

bool Registry::SyncPackages(const std::vector<PackageState>& packages) {
    std::lock_guard lock(mutex_);
    if (packages.size() > limits_.packages) return false;
    std::unordered_map<size_t, PackageRecord> next;
    std::unordered_set<std::string> ids;
    for (const auto& state : packages) {
        if (state.identity.id.empty() || state.identity.id.size() > 64 || state.identity.revision.empty() ||
            state.identity.revision.size() > 128 || next.contains(state.identity.slot) || !ids.insert(state.identity.id).second)
            return false;
        const auto existing = packages_.find(state.identity.slot);
        if (existing != packages_.end() && existing->second.state.identity == state.identity &&
            existing->second.state.enabled == state.enabled) {
            next.emplace(state.identity.slot, existing->second);
        } else {
            if (next_epoch_ == (std::numeric_limits<uint64_t>::max)()) return false;
            next.emplace(state.identity.slot, PackageRecord{state, ++next_epoch_});
        }
    }
    packages_.swap(next);
    Cleanup(clock_());
    return true;
}

std::optional<OwnerHandle> Registry::RegisterOwner(uintptr_t key) {
    std::lock_guard lock(mutex_);
    if (!key || (!owners_.contains(key) && owners_.size() >= limits_.owners) ||
        next_owner_ == (std::numeric_limits<uint64_t>::max)()) return std::nullopt;
    const OwnerHandle handle{key, ++next_owner_};
    owners_[key] = handle.incarnation;
    Cleanup(clock_());
    return handle;
}

void Registry::UnregisterOwner(OwnerHandle owner) {
    std::lock_guard lock(mutex_);
    const auto found = owners_.find(owner.key);
    if (found != owners_.end() && found->second == owner.incarnation) owners_.erase(found);
    Cleanup(clock_());
}

bool Registry::PrincipalMatches(const PackageIdentity& package, OwnerHandle owner) const {
    const auto found = packages_.find(package.slot);
    const auto window = owners_.find(owner.key);
    return found != packages_.end() && found->second.state.enabled && found->second.state.identity == package &&
        window != owners_.end() && window->second == owner.incarnation;
}

bool Registry::GrantCurrent(const Grant& grant, uint64_t now) const {
    const auto package = packages_.find(grant.slot);
    const auto owner = owners_.find(grant.owner.key);
    return package != packages_.end() && package->second.state.enabled && package->second.epoch == grant.epoch &&
        owner != owners_.end() && owner->second == grant.owner.incarnation && now >= grant.issued_ms &&
        now - grant.issued_ms < limits_.lifetime_ms;
}

std::optional<PrincipalBinding> Registry::CaptureBinding(const PackageIdentity& expected_package, OwnerHandle owner) {
    std::lock_guard lock(mutex_);
    if (!PrincipalMatches(expected_package, owner)) return std::nullopt;
    return PrincipalBinding(expected_package, owner, packages_.at(expected_package.slot).epoch, issuer_);
}

bool Registry::BindingCurrent(const PrincipalBinding& binding) const {
    return binding.issuer_.lock() == issuer_ && PrincipalMatches(binding.package_, binding.owner_) &&
        packages_.at(binding.package_.slot).epoch == binding.grant_epoch_;
}

bool Registry::ValidateBinding(const PrincipalBinding& binding) {
    std::lock_guard lock(mutex_);
    return BindingCurrent(binding);
}

void Registry::Cleanup(uint64_t now) {
    for (auto it = grants_.begin(); it != grants_.end();) {
        if (!GrantCurrent(it->second, now)) it = grants_.erase(it);
        else ++it;
    }
}

std::optional<std::string> Registry::Issue(const PrincipalBinding& binding) {
    std::lock_guard lock(mutex_);
    const uint64_t now = clock_();
    Cleanup(now);
    if (!BindingCurrent(binding)) return std::nullopt;
    const auto& expected_package = binding.package_;
    const auto owner = binding.owner_;
    const auto epoch = binding.grant_epoch_;
    for (const auto& [token, grant] : grants_) {
        if (grant.slot == expected_package.slot && grant.epoch == epoch && grant.owner == owner) return token;
    }
    if (!limits_.lifetime_ms || grants_.size() >= limits_.live_tickets || next_ticket_ == (std::numeric_limits<uint64_t>::max)())
        return std::nullopt;
    std::array<uint8_t, 32> bytes{};
    try {
        if (!random_(bytes)) return std::nullopt;
    } catch (...) { return std::nullopt; }
    if (last_nonce_ && *last_nonce_ == bytes) return std::nullopt;
    constexpr char hex[] = "0123456789abcdef";
    std::string token;
    token.reserve(bytes.size() * 2 + 16);
    for (uint8_t byte : bytes) {
        token.push_back(hex[byte >> 4]);
        token.push_back(hex[byte & 15]);
    }
    for (const auto& [existing, grant] : grants_) {
        (void)grant;
        if (existing.compare(0, 64, token) == 0) return std::nullopt;
    }
    // Randomness prevents guessing; the non-wrapping serial prevents replay even
    // if a random source repeats a long-expired nonce after other issuances.
    const auto serial = ++next_ticket_;
    for (int shift = 60; shift >= 0; shift -= 4) token.push_back(hex[(serial >> shift) & 15]);
    last_nonce_ = bytes;
    grants_.emplace(token, Grant{expected_package.slot, epoch, owner, now});
    return token;
}

bool Registry::Validate(std::string_view ticket, const PackageIdentity& expected_package, OwnerHandle owner) {
    std::lock_guard lock(mutex_);
    const uint64_t now = clock_();
    Cleanup(now);
    if (ticket.size() != 80 || !PrincipalMatches(expected_package, owner)) return false;
    const auto found = grants_.find(std::string(ticket));
    return found != grants_.end() && found->second.slot == expected_package.slot && found->second.owner == owner &&
        GrantCurrent(found->second, now);
}

}

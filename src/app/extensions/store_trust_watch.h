#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>
namespace pulse::extensions {
struct StoreTrustSnapshot { std::string manifest, signature; };
struct StoreTrustUpdate {
    uint64_t generation = 0;
    uint64_t check_sequence = 0;
    std::vector<size_t> revoked;
    std::string error;
};
// Host-only worker: immutable startup bytes, current policy, no registry reload.
class StoreTrustWatch {
  public:
    using PolicyReader = std::function<bool(const std::wstring&, std::string&)>;
    // reader is a test seam; injected captures must own their lifetime, never UI/runtime pointers.
    StoreTrustWatch(std::wstring root, uint64_t generation, std::vector<StoreTrustSnapshot> snapshots,
                    PolicyReader reader = {});
    ~StoreTrustWatch();
    StoreTrustWatch(const StoreTrustWatch&) = delete;
    StoreTrustWatch& operator=(const StoreTrustWatch&) = delete;
    std::optional<StoreTrustUpdate> TakeUpdate();
  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    std::thread worker_;
};
}

#pragma once
#include "authorization.h"
#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <windows.h>

namespace pulse::extensions {
// UI-thread-owned reference store. Clock injection is internal C++ testability, never an extension API.
class ResourceReferenceStore {
  public:
    using Snapshot = std::shared_ptr<const void>;
    using Clock = authorization::Registry::Clock;
    static constexpr uint64_t kLifetimeMs = 300000;
    static constexpr size_t kCapacity = 4096;
    explicit ResourceReferenceStore(
        authorization::Registry &registry, Clock clock = [] { return GetTickCount64(); })
        : registry_(registry), clock_(std::move(clock)) {}
    void Prune() {
        const auto now = clock_();
        std::erase_if(entries_, [&](const auto &item) {
            return now < item.second.created || now - item.second.created >= kLifetimeMs ||
                   !registry_.ValidateBinding(item.second.caller);
        });
    }
    bool HasCapacity(size_t required) {
        Prune();
        return required <= kCapacity && entries_.size() <= kCapacity - required;
    }
    std::string Issue(const authorization::PrincipalBinding &caller, Snapshot snapshot, std::wstring path,
                      size_t index, bool cursor) {
        if (!snapshot || path.empty() || !registry_.ValidateBinding(caller) || !HasCapacity(1))
            return {};
        std::array<uint8_t, 16> bytes{};
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            if (!authorization::SecureRandomBytes(bytes))
                return {};
            std::string id;
            constexpr char hex[] = "0123456789abcdef";
            for (auto byte : bytes) {
                id += hex[byte >> 4];
                id += hex[byte & 15];
            }
            if (entries_.emplace(id, Entry{caller, snapshot, path, index, cursor, clock_()}).second)
                return id;
        }
        return {};
    }
    std::optional<size_t> Resolve(const std::string &id, const authorization::PrincipalBinding &caller,
                                  const Snapshot &snapshot, const std::wstring &path, bool cursor) {
        Prune();
        if (!registry_.ValidateBinding(caller))
            return std::nullopt;
        const auto at = entries_.find(id);
        if (at == entries_.end())
            return std::nullopt;
        const auto &entry = at->second;
        if (entry.caller.Package() != caller.Package() || entry.caller.Owner() != caller.Owner() ||
            entry.snapshot != snapshot || entry.path != path || entry.cursor != cursor)
            return std::nullopt;
        return entry.index;
    }
    void ReleaseOwner(uintptr_t owner) {
        std::erase_if(entries_, [&](const auto &item) { return item.second.caller.Owner().key == owner; });
    }

  private:
    struct Entry {
        authorization::PrincipalBinding caller;
        Snapshot snapshot;
        std::wstring path;
        size_t index;
        bool cursor;
        uint64_t created;
    };
    authorization::Registry &registry_;
    Clock clock_;
    std::unordered_map<std::string, Entry> entries_;
};
} // namespace pulse::extensions

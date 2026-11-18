#pragma once
#include "authorization.h"
#include <memory>

namespace pulse::extensions::transfer_plan {

enum class Operation { Copy, Move };
struct FileIdentity {
    uint64_t volume = 0;
    std::array<uint8_t, 16> file_id{};
    uint64_t size = 0;
    uint64_t last_write = 0;
    bool operator==(const FileIdentity&) const = default;
};
// These are host-supplied snapshots, not verified paths or filesystem permissions.
struct ObjectIdentity {
    uint64_t volume = 0;
    std::array<uint8_t, 16> file_id{};
    bool operator==(const ObjectIdentity&) const = default;
};
struct SourceFile { std::wstring path; FileIdentity identity; ObjectIdentity parent; std::wstring relative_destination; };
struct TargetDirectory { std::wstring path; FileIdentity identity; };
struct TreeDirectory { std::wstring path; std::wstring relative_destination; FileIdentity identity; };
struct Draft {
    Operation operation = Operation::Copy;
    std::vector<SourceFile> sources;
    TargetDirectory target;
    bool directory_tree = false;
    std::wstring tree_root_name;
    std::vector<TreeDirectory> tree_directories;
};
struct Plan {
    Draft transfer;
    uint64_t created_ms = 0;
    uint64_t expires_ms = 0;
    std::string digest;
};
enum class Error { None, NotFound, Expired, Revoked, AlreadyConsumed, Cancelled, Quota,
                   RandomFailed, InvalidDraft, PlanMismatch, HashFailed, AlreadyStarted };
template<class T> struct Result {
    Error error = Error::None;
    std::optional<T> value;
    explicit operator bool() const noexcept { return error == Error::None && value.has_value(); }
};
struct Limits {
    size_t plans = 32;
    size_t sources = 64;
    size_t tree_files = 512;
    size_t tree_directories = 128;
    size_t path_chars = 32768;
    size_t plan_bytes = 1024 * 1024;
    uint64_t lifetime_ms = 5 * 60 * 1000;
};

// Only Store can issue this after consuming a confirmed receipt. Copies share
// one start gate, so queuing the same permit twice cannot execute twice.
class ExecutionPermit {
public:
    ExecutionPermit(const ExecutionPermit&) = default;
    ExecutionPermit& operator=(const ExecutionPermit&) = default;
    const transfer_plan::Plan& Plan() const noexcept;
    const authorization::PrincipalBinding& Principal() const noexcept;
    // Expiry gates start only. After success the executor must independently
    // check caller revocation and cancellation between chunks; no rollback is implied.
    Error TryStart(authorization::Registry& registry,
                   const authorization::PrincipalBinding& expected) const;
private:
    friend class Store;
    struct SharedState;
    ExecutionPermit(std::shared_ptr<const transfer_plan::Plan> plan,
                    const authorization::PrincipalBinding& principal,
                    authorization::Registry::Clock clock);
    std::shared_ptr<SharedState> state_;
};

// Host-only session records. Confirm must be reached only after real host UI
// confirmation; there is no caller-provided "userConfirmed" flag. Consuming a
// receipt does not authorize a filesystem operation: the future executor must
// recheck source/destination handles and identities immediately before mutation.
class Store {
public:
    using FrozenPlan = std::shared_ptr<const Plan>;
    using Binding = authorization::PrincipalBinding;
    using Clock = authorization::Registry::Clock;
    using Random = authorization::Registry::Random;
    explicit Store(authorization::Registry& registry, Limits limits = {}, Clock clock = {}, Random random = {});
    Result<std::string> Create(const Binding& principal, const Draft& draft);
    Result<FrozenPlan> Read(std::string_view plan_id, const Binding& expected);
    Result<std::string> Confirm(std::string_view plan_id, const Binding& expected);
    Result<ExecutionPermit> Consume(std::string_view receipt, const Binding& expected);
    Error Cancel(std::string_view plan_id, const Binding& expected);
private:
    enum class State { Pending, Confirmed, Consumed, Cancelled };
    struct Record {
        Binding principal;
        FrozenPlan plan;
        State state = State::Pending;
        std::string receipt;
        std::string confirmed_digest;
    };
    Error Check(const Record& record, const Binding& expected, uint64_t now);
    std::optional<std::string> Token();
    void Cleanup(uint64_t now);
    authorization::Registry& registry_;
    Limits limits_;
    Clock clock_;
    Random random_;
    std::mutex mutex_;
    uint64_t serial_ = 0;
    std::optional<std::array<uint8_t, 32>> last_nonce_;
    std::unordered_map<std::string, Record> plans_;
};

}

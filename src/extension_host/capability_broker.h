#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace pulse::extension_host {

enum class SandboxMode : uint8_t {
    RequireAppContainer = 0,
    RequireRestrictedToken = 1,
    AppContainerOrRestrictedToken = 2,
};

enum class IsolationTier : uint8_t {
    None = 0,
    AppContainer = 1,
    RestrictedToken = 2,
};

enum class BrokerCapability : uint32_t {
    None = 0,
    SettingsOwn = 1u << 0,
    ContextRead = 1u << 1,
    ReadAllowedRoots = 1u << 2,
};

constexpr BrokerCapability operator|(BrokerCapability lhs, BrokerCapability rhs) noexcept {
    return static_cast<BrokerCapability>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

constexpr BrokerCapability operator&(BrokerCapability lhs, BrokerCapability rhs) noexcept {
    return static_cast<BrokerCapability>(static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
}

constexpr BrokerCapability operator~(BrokerCapability mask) noexcept {
    return static_cast<BrokerCapability>(~static_cast<uint32_t>(mask));
}

inline BrokerCapability &operator|=(BrokerCapability &lhs, BrokerCapability rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

inline BrokerCapability &operator&=(BrokerCapability &lhs, BrokerCapability rhs) noexcept {
    lhs = lhs & rhs;
    return lhs;
}

constexpr bool HasBrokerCapability(BrokerCapability granted, BrokerCapability required) noexcept {
    return (static_cast<uint32_t>(granted) & static_cast<uint32_t>(required)) ==
           static_cast<uint32_t>(required);
}

enum class BrokerStatus : uint32_t {
    Ok = 0,
    PermissionDenied = 1,
    Revoked = 2,
    Conflict = 3,
    NotFound = 4,
    InvalidArgument = 5,
    PathOutsideRoot = 6,
    ReparsePointRejected = 7,
    BudgetExceeded = 8,
    AuthFailed = 9,
    Timeout = 10,
    TransportError = 11,
    InternalError = 12,
};

inline constexpr uint32_t kBrokerFrameMagic = 0x5042524Bu; // 'PBRK'
inline constexpr uint16_t kBrokerProtocolVersion = 1;
inline constexpr uint32_t kMaxBrokerPayloadBytes = 4u * 1024u * 1024u;
inline constexpr size_t kMaxSettingKeyBytes = 1024u;
inline constexpr size_t kMaxSettingValueBytes = 64u * 1024u;
inline constexpr size_t kBrokerSessionTokenBytes = 16u;
inline constexpr uint64_t kSettingsVersionUnconditional = UINT64_MAX;

enum class BrokerOp : uint16_t {
    Handshake = 1,
    HandshakeAck = 2,
    HostInvoke = 3,
    HostInvokeReply = 4,
    SettingsGet = 10,
    SettingsGetReply = 11,
    SettingsSet = 12,
    SettingsSetReply = 13,
    ContextGet = 14,
    ContextGetReply = 15,
    OpenReadFile = 16,
    OpenReadFileReply = 17,
};

#pragma pack(push, 1)
struct BrokerFrameHeader {
    uint32_t magic = kBrokerFrameMagic;
    uint16_t version = kBrokerProtocolVersion;
    uint16_t op = 0;
    uint64_t request_id = 0;
    uint32_t payload_size = 0;
    uint32_t status = 0;
};
#pragma pack(pop)
static_assert(sizeof(BrokerFrameHeader) == 24, "BrokerFrameHeader wire layout must be 24 bytes");

struct SanitizedContextSnapshot {
    std::wstring directory_label;
    uint32_t total_items = 0;
    uint32_t selected_items = 0;
};

struct BrokerReadFileResult {
    uint64_t worker_handle = 0;
    uint64_t file_size = 0;
};

struct BrokerConfig {
    std::wstring package_id = L"default";
    std::wstring executable_path;
    std::wstring extra_arguments;
    SandboxMode sandbox_mode = SandboxMode::AppContainerOrRestrictedToken;
    BrokerCapability initial_capabilities = BrokerCapability::None;
    std::vector<std::wstring> allowed_read_roots;
    uint32_t max_brokered_handles = 16;
    uint32_t memory_limit_mib = 64;
    uint32_t cpu_rate_hard_cap = 2500; // 25.00% in 1/100th percent units
    uint32_t active_process_limit = 1;
    uint32_t handshake_timeout_ms = 5000;
    uint32_t default_call_timeout_ms = 5000;
    std::wstring appcontainer_profile_name;
    bool delete_appcontainer_profile_on_close = true;
};

class CapabilityBroker {
  public:
    CapabilityBroker() = default;
    ~CapabilityBroker();
    CapabilityBroker(const CapabilityBroker &) = delete;
    CapabilityBroker &operator=(const CapabilityBroker &) = delete;

    bool Start(const BrokerConfig &config);
    void RevokeSession();

    void GrantCapabilities(BrokerCapability mask) noexcept;
    void RevokeCapabilities(BrokerCapability mask) noexcept;
    BrokerCapability CurrentCapabilities() const noexcept;

    void SetContextSnapshot(SanitizedContextSnapshot snapshot);

    BrokerStatus InvokeWorker(uint32_t command_op, const std::vector<uint8_t> &request_payload,
                              std::vector<uint8_t> &response_payload, uint32_t timeout_ms = 0);

    // Host-side entrypoints sharing the exact capability/revocation/path gates.
    BrokerStatus HandleSettingsGet(const std::wstring &key, std::string &value_out,
                                   uint64_t &version_out);
    BrokerStatus HandleSettingsSet(const std::wstring &key, const std::string &value,
                                   uint64_t expected_version, uint64_t &new_version_out);
    BrokerStatus HandleContextGet(SanitizedContextSnapshot &snapshot_out);
    BrokerStatus HandleOpenReadFile(const std::wstring &path, BrokerReadFileResult &result_out);

    IsolationTier ActiveIsolationTier() const noexcept { return isolation_tier_; }
    DWORD WorkerProcessId() const noexcept { return worker_pid_; }
    bool IsRevoked() const noexcept { return session_revoked_.load(std::memory_order_acquire); }
    bool IsWorkerAlive() const noexcept;
    uint64_t DeniedCallCount() const noexcept {
        return denied_call_count_.load(std::memory_order_acquire);
    }
    uint32_t BrokeredHandleCount() const noexcept {
        return brokered_handle_count_.load(std::memory_order_acquire);
    }
    const std::wstring &LastErrorStage() const noexcept { return stage_; }
    DWORD LastError() const noexcept { return error_; }

  private:
    struct SettingRecord {
        std::string value;
        uint64_t version = 0;
    };

    struct CanonicalRoot {
        std::wstring full_path;
        std::wstring final_path;
    };

    bool Fail(const wchar_t *stage, DWORD error);
    BrokerStatus CheckCapabilityGate(BrokerCapability required) noexcept;
    bool CanonicalizeAllowedRoots(const std::vector<std::wstring> &roots);
    bool CreateBrokerPipePair(HANDLE &client_pipe_out);
    bool ConfigureJobObject(HANDLE &job_out);
    bool LaunchAppContainer(HANDLE client_pipe, const std::wstring &command_line,
                            const std::wstring &system_root);
    bool LaunchRestrictedToken(HANDLE client_pipe, const std::wstring &command_line,
                               const std::wstring &system_root);
    bool PerformHandshake(uint32_t timeout_ms);
    BrokerStatus DispatchBrokerFrame(const BrokerFrameHeader &req_header,
                                     const std::vector<uint8_t> &req_payload,
                                     BrokerOp &reply_op_out,
                                     std::vector<uint8_t> &reply_payload_out);
    void TerminateAndCleanupWorker(DWORD exit_reason, bool mark_revoked);
    bool RemoveAppContainerProfile();

    BrokerConfig config_{};
    std::vector<CanonicalRoot> allowed_roots_;
    uint8_t session_token_[kBrokerSessionTokenBytes]{};

    std::atomic<uint32_t> granted_capabilities_{0};
    std::atomic<bool> session_revoked_{false};
    std::atomic<uint64_t> denied_call_count_{0};
    std::atomic<uint32_t> brokered_handle_count_{0};
    std::atomic<uint64_t> next_request_id_{1};

    mutable std::mutex state_mutex_;
    std::unordered_map<std::wstring, std::unordered_map<std::wstring, SettingRecord>> settings_;
    SanitizedContextSnapshot context_snapshot_{};

    mutable std::mutex io_mutex_;
    mutable std::mutex lifecycle_mutex_;
    HANDLE host_pipe_ = INVALID_HANDLE_VALUE;
    HANDLE io_event_ = nullptr;
    HANDLE cancel_event_ = nullptr;
    HANDLE job_ = nullptr;
    HANDLE worker_process_ = nullptr;
    DWORD worker_pid_ = 0;

    PSID appcontainer_sid_ = nullptr;
    std::wstring appcontainer_profile_;
    bool owns_appcontainer_profile_ = false;
    IsolationTier isolation_tier_ = IsolationTier::None;

    std::wstring stage_;
    DWORD error_ = 0;
};

class WorkerBrokerClient {
  public:
    WorkerBrokerClient() = default;
    ~WorkerBrokerClient();
    WorkerBrokerClient(const WorkerBrokerClient &) = delete;
    WorkerBrokerClient &operator=(const WorkerBrokerClient &) = delete;

    static bool ParseCommandLineArgs(int argc, wchar_t **argv, HANDLE &pipe_out,
                                     uint8_t (&token_out)[kBrokerSessionTokenBytes]);

    BrokerStatus Connect(HANDLE inherited_pipe,
                         const uint8_t (&session_token)[kBrokerSessionTokenBytes],
                         uint32_t timeout_ms = 5000);
    void Close() noexcept;

    BrokerStatus SettingsGet(const std::wstring &key, std::string &value_out, uint64_t &version_out,
                             uint32_t timeout_ms = 5000);
    BrokerStatus SettingsSet(const std::wstring &key, const std::string &value,
                             uint64_t expected_version, uint64_t &new_version_out,
                             uint32_t timeout_ms = 5000);
    BrokerStatus ContextGet(SanitizedContextSnapshot &snapshot_out, uint32_t timeout_ms = 5000);
    BrokerStatus OpenReadFile(const std::wstring &path, HANDLE &file_handle_out,
                              uint64_t &file_size_out, uint32_t timeout_ms = 5000);

    template <typename Handler>
    BrokerStatus RunDispatchLoop(Handler &&handler, uint32_t idle_timeout_ms = 30000) {
        for (;;) {
            uint32_t command_op = 0;
            uint64_t request_id = 0;
            std::vector<uint8_t> req_payload;
            const BrokerStatus recv_status =
                ReceiveHostInvoke(command_op, request_id, req_payload, idle_timeout_ms);
            if (recv_status != BrokerStatus::Ok) {
                return recv_status;
            }
            std::vector<uint8_t> resp_payload;
            const BrokerStatus cmd_status = handler(command_op, req_payload, resp_payload);
            const BrokerStatus send_status =
                SendHostInvokeReply(request_id, cmd_status, resp_payload, 5000);
            if (send_status != BrokerStatus::Ok) {
                return send_status;
            }
        }
    }

    HANDLE PipeHandle() const noexcept { return pipe_; }

  private:
    BrokerStatus ReceiveHostInvoke(uint32_t &command_op_out, uint64_t &request_id_out,
                                   std::vector<uint8_t> &payload_out, uint32_t timeout_ms);
    BrokerStatus SendHostInvokeReply(uint64_t request_id, BrokerStatus status,
                                     const std::vector<uint8_t> &payload, uint32_t timeout_ms);
    BrokerStatus CallBroker(BrokerOp request_op, BrokerOp expected_reply_op,
                            const std::vector<uint8_t> &request_payload,
                            std::vector<uint8_t> &reply_payload_out, uint32_t timeout_ms);

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE io_event_ = nullptr;
    uint64_t next_request_id_ = 1;
};

} // namespace pulse::extension_host

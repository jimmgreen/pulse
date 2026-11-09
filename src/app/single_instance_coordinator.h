#pragma once

#include <windows.h>
#include <string>
#include <string_view>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>
#include <deque>
#include <mutex>
#include <memory>
#include <atomic>

namespace pulse::app {
class OpenRequestPipe;
class ExplorerSubmissionPort;
struct ExplorerOpenContext;

class SingleInstanceCoordinator {
public:
    enum class AcquireResult { Primary, Existing, Failed };

    SingleInstanceCoordinator();
    ~SingleInstanceCoordinator();
    SingleInstanceCoordinator(const SingleInstanceCoordinator&) = delete;
    SingleInstanceCoordinator& operator=(const SingleInstanceCoordinator&) = delete;

    AcquireResult Acquire(std::wstring_view mutex_name = {});
    void Release();
    std::shared_ptr<ExplorerSubmissionPort> CreateExplorerSubmissionPort(HWND host, UINT wake);
    bool PublishEndpoint(HWND window, UINT wake_message = 0); // Zero explicitly publishes legacy v1 only.
    bool PublishWindow(HWND window) { return PublishEndpoint(window); }
    static std::optional<std::wstring> ResolveOpenPath(const std::wstring& path) {
        std::wstring normalized;
        if (!NormalizeLaunchPath(path, normalized)) return std::nullopt;
        return normalized;
    }
    bool OwnsEndpoint(HWND window) const;
    struct Endpoint {
        HWND window = nullptr;
        DWORD pid = 0;
        FILETIME created{};
        std::wstring nonce, pipe_name;
    };
    static std::optional<Endpoint> FindPrimaryEndpoint(std::wstring_view endpoint_name = {});
    static HWND FindPrimaryWindow(std::wstring_view endpoint_name = {});
    static bool NormalizeLaunchPath(const std::wstring& input, std::wstring& output);
    // Optional observation is the attempted ID, not proof of successful delivery.
    bool ForwardOpenPath(const std::wstring& path, DWORD timeout_ms = 2000,
                         std::array<unsigned char, 16>* out_attempted_id = nullptr) const;
    enum class OpenAcceptance { Invalid, New, Duplicate };
    enum class OpenDeliveryState { Pending, Queued, Dispatched, Cancelled };
    // A dispatch observation is not a rendering/presentation acknowledgement.
    std::optional<OpenDeliveryState> ObserveOpenDelivery(const std::array<unsigned char, 16>& id) const;
    struct OpenRequest {
        std::array<unsigned char, 16> id{};
        uint64_t deadline = 0;
        std::wstring path;
        std::shared_ptr<const ExplorerOpenContext> explorer;
        bool explorer_received = false;
        // Non-wire state. Only the UI claim may begin a separate busy budget.
        bool execution_started = false;
        uint64_t ui_wait_deadline = 0;
        // Internal delivery only; never encoded as part of the v2 request.
        std::shared_ptr<std::atomic<OpenDeliveryState>> delivery;
        // Internal quick-search intent; v2 wire requests cannot encode it.
        bool reveal = false;
    };
    static std::vector<unsigned char> EncodeOpenRequest(const OpenRequest& request);
    static bool DecodeOpenRequest(const COPYDATASTRUCT* data, OpenRequest& request);
    // Compatibility for receivers which synchronously consume on acceptance.
    OpenAcceptance AcceptOpenRequest(const OpenRequest& request, uint64_t now);
    enum class OpenSubmission { Invalid, Accepted, Duplicate, Retry };
    static constexpr DWORD_PTR RetryOpenReply = 2;
    static constexpr size_t OpenQueueCapacity = 64;
    using WakePoster = BOOL (WINAPI*)(HWND, UINT, WPARAM, LPARAM);
    OpenSubmission SubmitOpenRequest(OpenRequest request, uint64_t now, HWND host, UINT wake,
                                     WakePoster post = &PostMessageW);
    // Hook-safe local admission: one try_lock, a preallocated request and a
    // fixed slot. No tracing, allocation, expired-cache scan or waiting.
    bool TrySubmitKeyboardOpen(OpenRequest& request, uint64_t now, HWND host, UINT wake,
                               WakePoster post = &PostMessageW) noexcept;
    std::optional<OpenRequest> TakeQueuedOpenRequest(uint64_t now = GetTickCount64(), bool reserve_slot = false);
    // A claimed request reserves its original capacity slot until finished.
    // UI thread only; defer returns it to the queue without a second admission.
    void FinishOpenClaim(OpenRequest request, bool defer);
    struct OpenFailures { uint32_t timed_out = 0, failed = 0; };
    OpenFailures TakeOpenFailures();
    static void CompleteDelivery(const OpenRequest&, bool dispatched) noexcept;
    static ULONG_PTR OpenRequestMessageId() noexcept;

    static bool DecodeOpenPath(const COPYDATASTRUCT* data, std::wstring& path);
    static ULONG_PTR OpenPathMessageId() noexcept;
    static const wchar_t* WindowClassName() noexcept;

private:
    friend class ExplorerSubmissionPort;
    OpenSubmission SubmitOpenRequestImpl(OpenRequest request, uint64_t now, HWND host, UINT wake, WakePoster post, bool internal);
    std::shared_ptr<ExplorerSubmissionPort> explorer_port_;
    mutable std::mutex requests_mutex_;
    bool accepting_requests_ = true;
    std::deque<OpenRequest> open_queue_;
    std::optional<OpenRequest> keyboard_open_;
    std::shared_ptr<std::atomic<OpenDeliveryState>> open_claim_;
    std::shared_ptr<const ExplorerOpenContext> open_claim_explorer_;
    OpenFailures open_failures_;
    std::unique_ptr<OpenRequestPipe> pipe_service_;
    HANDLE capability_mapping_ = nullptr;
    void* capability_view_ = nullptr;
    HANDLE mutex_ = nullptr;
    HANDLE endpoint_mapping_ = nullptr;
    void* endpoint_view_ = nullptr;
    HWND endpoint_window_ = nullptr;
    std::wstring endpoint_name_;
    std::wstring endpoint_nonce_;
    std::map<std::pair<bool, std::array<unsigned char, 16>>, OpenRequest> accepted_;
};

} // namespace pulse::app

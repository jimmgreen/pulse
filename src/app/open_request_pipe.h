#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace pulse::app {
// Local RawV2 byte transport. Payload is EncodeOpenRequest's unchanged v2 bytes;
// this layer neither routes paths nor claims dispatch/presentation completion.
class OpenRequestPipe {
public:
    enum class Reply : uint32_t { Invalid = 0, Accepted = 1, Duplicate = 2, Retry = 3 };
    struct ServerIdentity { DWORD pid = 0; FILETIME created{}; };
    enum class Stage { None, Header, Payload, Reply };
    struct Result {
        std::optional<Reply> reply;
        DWORD error = ERROR_SUCCESS;
        Stage stage = Stage::None;
        bool request_may_have_been_sent = false;
    };
    static constexpr uint32_t Magic = 0x3252504f; // OPR2; distinct from old OpenOperation transport.
    static constexpr uint32_t Version = 2;
    static constexpr uint32_t MaxPayload = 32 + 32768 * 2;
    using Handler = std::function<Reply(std::span<const unsigned char>)>;
    OpenRequestPipe();
    ~OpenRequestPipe();
    OpenRequestPipe(const OpenRequestPipe&) = delete;
    OpenRequestPipe& operator=(const OpenRequestPipe&) = delete;
    // Construction/Name may throw allocation exceptions; Start/Exchange contain
    // all operational exceptions. Stop is noexcept. Never hold the queue lock
    // while joining: an in-flight handler may need that lock to finish.
    // Start/Stop are owner-thread serialized. Handler runs only on the worker,
    // must be bounded and must not call Stop or synchronously wait for the UI.
    bool Start(const std::wstring& name, Handler handler) noexcept;
    void Stop() noexcept;
    static std::wstring Name(DWORD pid, const std::wstring& nonce);
    static bool Identity(HANDLE process, ServerIdentity& result) noexcept;
    // One attempt only. Missing reply is not permission to start another primary
    // or replay through WM_COPYDATA. Caller owns same-ID/deadline retries.
    static Result Exchange(const std::wstring& name, const ServerIdentity& server,
                           std::span<const unsigned char> payload, uint64_t deadline) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

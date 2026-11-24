// desktop_ipc.h - Current-user SID DACL Named Pipe IPC with overlapped I/O
// and non-blocking background reader/writer threads for pulse.desktop.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace pulse::desktop::ipc {

inline constexpr uint32_t kFrameMagic = 0x5044534Bu; // 'PDSK'
inline constexpr uint16_t kProtocolVersion = 1u;
inline constexpr uint32_t kMaxPayloadBytes = 8u * 1024u * 1024u; // 8 MB cap (supports high-DPI Quick Look BGRA)

enum class MessageType : uint16_t {
    Hello = 1,
    AppearanceSync = 2,
    SettingsSync = 3,
    TraySnapshotSync = 4,
    TrayMutateReq = 5,
    SearchQueryReq = 6,
    SearchReplyMsg = 7,
    ShellMenuReq = 8,
    ShellMenuReplyMsg = 9,
    ShellMenuInvokeReq = 10,
    TagSyncMsg = 11,
    SetTagReq = 12,
    OpenInPulseReq = 13,
    Ping = 14,
    Pong = 15,
    ShutdownClean = 16,
    PreviewReq = 17,
    PreviewReplyMsg = 18,
    CommandActionMsg = 19,
};

#pragma pack(push, 1)
struct FrameHeader {
    uint32_t magic = kFrameMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = 0;
    uint64_t request_id = 0;
    uint64_t timestamp_ms = 0;
    uint32_t payload_size = 0;
    uint32_t reserved = 0;
};
#pragma pack(pop)
static_assert(sizeof(FrameHeader) == 32, "FrameHeader must be 32 bytes");

struct Frame {
    MessageType type = MessageType::Ping;
    uint64_t request_id = 0;
    uint64_t timestamp_ms = 0;
    std::vector<uint8_t> payload;

    std::string_view PayloadText() const noexcept {
        if (payload.empty()) return {};
        return std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size());
    }
};

// UTF-8 <-> UTF-16 & lightweight JSON helpers shared across IPC, mount snapshot, and settings.
std::string WideToUtf8(std::wstring_view wide);
std::wstring Utf8ToWide(std::string_view utf8);
std::string JsonEscape(std::string_view raw);
std::string JsonQuote(std::string_view raw);
std::string JsonQuoteWide(std::wstring_view wide);

bool JsonFindString(std::string_view json, std::string_view key, std::string& out_utf8);
bool JsonFindWide(std::string_view json, std::string_view key, std::wstring& out_wide);
bool JsonFindInt64(std::string_view json, std::string_view key, int64_t& out_val);
bool JsonFindDouble(std::string_view json, std::string_view key, double& out_val);
bool JsonFindUint64(std::string_view json, std::string_view key, uint64_t& out_val);
bool JsonFindBool(std::string_view json, std::string_view key, bool& out_val);

// Binary PreviewReplyMsg payload codec (header fields + UTF-8 strings + raw BGRA bytes).
struct PreviewPayloadData {
    int32_t status = 0;
    uint64_t size_bytes = 0;
    int64_t modified_unix_ms = 0;
    uint32_t image_width = 0;
    uint32_t image_height = 0;
    uint32_t thumb_width = 0;
    uint32_t thumb_height = 0;
    uint32_t thumb_stride = 0;
    std::wstring path;
    std::wstring display_type;
    std::wstring text_excerpt;
    std::vector<uint8_t> thumb_bgra;
};

std::vector<uint8_t> EncodePreviewPayload(const PreviewPayloadData& data);
bool DecodePreviewPayload(const std::vector<uint8_t>& bytes, PreviewPayloadData& out_data);

// Generates "\\.\pipe\PulseDesktop_<pid>_<nonce>" using BCrypt CSPRNG.
bool RandomBytes(void* out, size_t size) noexcept;
std::wstring MakeUniquePipeName();

// Creates a first-instance overlapped Named Pipe restricted exclusively to the current user's SID.
HANDLE CreateAuthenticatedServerPipe(const std::wstring& pipe_name);
HANDLE ConnectClientPipe(const std::wstring& pipe_name, DWORD wait_timeout_ms = 3000);

// Non-blocking full-duplex overlapped Named Pipe endpoint with dedicated reader/writer threads.
class AsyncPipeEndpoint {
public:
    using FrameCallback = std::function<void(Frame&&)>;
    using DisconnectCallback = std::function<void()>;

    AsyncPipeEndpoint() = default;
    ~AsyncPipeEndpoint();
    AsyncPipeEndpoint(const AsyncPipeEndpoint&) = delete;
    AsyncPipeEndpoint& operator=(const AsyncPipeEndpoint&) = delete;

    // Starts server endpoint: accepts one client in background, then pumps overlapped reads/writes.
    bool StartServer(const std::wstring& pipe_name,
                     FrameCallback on_frame,
                     DisconnectCallback on_disconnect = {},
                     DWORD accept_timeout_ms = 10000);

    // Connects client endpoint to server pipe and starts background overlapped reader/writer.
    bool StartClient(const std::wstring& pipe_name,
                     FrameCallback on_frame,
                     DisconnectCallback on_disconnect = {},
                     DWORD connect_timeout_ms = 5000);

    // O(1) non-blocking enqueue from any thread (including UI thread).
    bool Send(MessageType type, uint64_t request_id, std::string_view utf8_json);
    bool SendBinary(MessageType type, uint64_t request_id, std::vector<uint8_t> payload);

    void Stop() noexcept;
    bool IsConnected() const noexcept { return connected_.load(std::memory_order_acquire); }
    bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }

private:
    void IoLoop(bool is_server, DWORD connect_timeout_ms);
    void WriterLoop();
    bool OverlappedReadExact(uint8_t* dst, size_t count);
    bool OverlappedWriteExact(const uint8_t* src, size_t count);

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE stop_event_ = nullptr;
    HANDLE write_event_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    FrameCallback on_frame_;
    DisconnectCallback on_disconnect_;

    std::mutex out_mutex_;
    std::deque<std::vector<uint8_t>> out_queue_;
    size_t out_queued_bytes_ = 0;

    std::thread io_thread_;
    std::thread writer_thread_;
};

} // namespace pulse::desktop::ipc

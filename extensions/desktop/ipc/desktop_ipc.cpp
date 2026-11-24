#include <cmath>
// desktop_ipc.cpp - Current-user SID DACL Named Pipe IPC implementation.
#include "ipc/desktop_ipc.h"

#include <bcrypt.h>
#include <sddl.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>

namespace pulse::desktop::ipc {
namespace {

constexpr size_t kMaxOutboundQueueBytes = 32u * 1024u * 1024u;

struct ScopedEvent {
    HANDLE handle = nullptr;
    ScopedEvent() noexcept : handle(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ScopedEvent() {
        if (handle) CloseHandle(handle);
    }
    ScopedEvent(const ScopedEvent&) = delete;
    ScopedEvent& operator=(const ScopedEvent&) = delete;
};

std::wstring CurrentUserSidString() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        CloseHandle(token);
        return {};
    }
    std::vector<BYTE> buffer(needed);
    const BOOL ok = GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed);
    CloseHandle(token);
    if (!ok) return {};
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR sid_str = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sid_str) || !sid_str) return {};
    std::wstring out(sid_str);
    LocalFree(sid_str);
    return out;
}

size_t SkipJsonWs(std::string_view s, size_t pos) noexcept {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n')) {
        ++pos;
    }
    return pos;
}

bool ParseJsonStringAt(std::string_view s, size_t quote_pos, std::string& out, size_t* out_after = nullptr) {
    if (quote_pos >= s.size() || s[quote_pos] != '"') return false;
    out.clear();
    size_t i = quote_pos + 1;
    while (i < s.size()) {
        const char ch = s[i++];
        if (ch == '"') {
            if (out_after) *out_after = i;
            return true;
        }
        if (ch == '\\') {
            if (i >= s.size()) return false;
            const char esc = s[i++];
            switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (i + 4 > s.size()) return false;
                uint32_t cp = 0;
                for (int k = 0; k < 4; ++k) {
                    const char hc = s[i + k];
                    uint32_t v = 0;
                    if (hc >= '0' && hc <= '9') v = static_cast<uint32_t>(hc - '0');
                    else if (hc >= 'a' && hc <= 'f') v = static_cast<uint32_t>(hc - 'a' + 10);
                    else if (hc >= 'A' && hc <= 'F') v = static_cast<uint32_t>(hc - 'A' + 10);
                    else return false;
                    cp = (cp << 4) | v;
                }
                i += 4;
                if (cp >= 0xD800u && cp <= 0xDBFFu && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                    uint32_t low = 0;
                    bool low_ok = true;
                    for (int k = 0; k < 4; ++k) {
                        const char hc = s[i + 2 + k];
                        uint32_t v = 0;
                        if (hc >= '0' && hc <= '9') v = static_cast<uint32_t>(hc - '0');
                        else if (hc >= 'a' && hc <= 'f') v = static_cast<uint32_t>(hc - 'a' + 10);
                        else if (hc >= 'A' && hc <= 'F') v = static_cast<uint32_t>(hc - 'A' + 10);
                        else { low_ok = false; break; }
                        low = (low << 4) | v;
                    }
                    if (low_ok && low >= 0xDC00u && low <= 0xDFFFu) {
                        cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                        i += 6;
                    }
                }
                if (cp <= 0x7Fu) {
                    out.push_back(static_cast<char>(cp));
                } else if (cp <= 0x7FFu) {
                    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                } else if (cp <= 0xFFFFu) {
                    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
                    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                } else {
                    out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
                    out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
                    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
                    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                }
                break;
            }
            default:
                out.push_back(esc);
                break;
            }
        } else {
            out.push_back(ch);
        }
    }
    return false;
}

// Finds top-level or nested `"key":` value start while properly skipping string literals.
bool FindJsonKeyValuePos(std::string_view json, std::string_view key, size_t& out_val_pos) {
    std::string decoded_key;
    size_t i = 0;
    while (i < json.size()) {
        if (json[i] == '"') {
            size_t after_str = i + 1;
            if (!ParseJsonStringAt(json, i, decoded_key, &after_str)) return false;
            const size_t colon = SkipJsonWs(json, after_str);
            if (colon < json.size() && json[colon] == ':' && decoded_key == key) {
                out_val_pos = SkipJsonWs(json, colon + 1);
                return out_val_pos < json.size();
            }
            i = after_str;
        } else {
            ++i;
        }
    }
    return false;
}

template <typename T>
void AppendPod(std::vector<uint8_t>& buf, const T& value) {
    const auto* ptr = reinterpret_cast<const uint8_t*>(&value);
    buf.insert(buf.end(), ptr, ptr + sizeof(T));
}

template <typename T>
bool ReadPod(const std::vector<uint8_t>& buf, size_t& offset, T& out) noexcept {
    if (offset + sizeof(T) > buf.size()) return false;
    std::memcpy(&out, buf.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

void AppendLengthPrefixedWide(std::vector<uint8_t>& buf, std::wstring_view wide) {
    const std::string utf8 = WideToUtf8(wide);
    const uint32_t len = static_cast<uint32_t>(utf8.size());
    AppendPod(buf, len);
    if (len > 0) {
        const auto* ptr = reinterpret_cast<const uint8_t*>(utf8.data());
        buf.insert(buf.end(), ptr, ptr + len);
    }
}

bool ReadLengthPrefixedWide(const std::vector<uint8_t>& buf, size_t& offset, std::wstring& out) {
    uint32_t len = 0;
    if (!ReadPod(buf, offset, len)) return false;
    if (len > kMaxPayloadBytes || offset + len > buf.size()) return false;
    if (len == 0) {
        out.clear();
        return true;
    }
    const std::string_view slice(reinterpret_cast<const char*>(buf.data() + offset), len);
    offset += len;
    out = Utf8ToWide(slice);
    return true;
}

} // namespace

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

std::string JsonEscape(std::string_view raw) {
    std::string out;
    out.reserve(raw.size() + 8);
    for (const unsigned char ch : raw) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20u) {
                char buf[7]{};
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(ch));
                out += buf;
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return out;
}

std::string JsonQuote(std::string_view raw) {
    std::string out;
    out.reserve(raw.size() + 10);
    out.push_back('"');
    out += JsonEscape(raw);
    out.push_back('"');
    return out;
}

std::string JsonQuoteWide(std::wstring_view wide) {
    return JsonQuote(WideToUtf8(wide));
}

bool JsonFindString(std::string_view json, std::string_view key, std::string& out_utf8) {
    size_t pos = 0;
    if (!FindJsonKeyValuePos(json, key, pos)) return false;
    return ParseJsonStringAt(json, pos, out_utf8);
}

bool JsonFindWide(std::string_view json, std::string_view key, std::wstring& out_wide) {
    std::string utf8;
    if (!JsonFindString(json, key, utf8)) return false;
    out_wide = Utf8ToWide(utf8);
    return true;
}

bool JsonFindInt64(std::string_view json, std::string_view key, int64_t& out_val) {
    size_t pos = 0;
    if (!FindJsonKeyValuePos(json, key, pos)) return false;
    size_t end = pos;
    if (end < json.size() && json[end] == '-') ++end;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9') ++end;
    if (end == pos) return false;
    const auto res = std::from_chars(json.data() + pos, json.data() + end, out_val);
    return res.ec == std::errc{};
}

bool JsonFindDouble(std::string_view json, std::string_view key, double& out_val) {
    size_t pos=0;
    if (!FindJsonKeyValuePos(json,key,pos)) return false;
    double value=0;
    const auto result=std::from_chars(json.data()+pos,json.data()+json.size(),value);
    if(result.ec!=std::errc{} || !std::isfinite(value)) return false;
    const char* end=result.ptr;
    while(end<json.data()+json.size() && (*end==' '||*end=='\t'||*end=='\r'||*end=='\n')) ++end;
    if(end<json.data()+json.size() && *end!=',' && *end!='}') return false;
    out_val=value;return true;
}

bool JsonFindUint64(std::string_view json, std::string_view key, uint64_t& out_val) {
    size_t pos = 0;
    if (!FindJsonKeyValuePos(json, key, pos)) return false;
    size_t end = pos;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9') ++end;
    if (end == pos) return false;
    const auto res = std::from_chars(json.data() + pos, json.data() + end, out_val);
    return res.ec == std::errc{};
}

bool JsonFindBool(std::string_view json, std::string_view key, bool& out_val) {
    size_t pos = 0;
    if (!FindJsonKeyValuePos(json, key, pos)) return false;
    if (json.substr(pos).starts_with("true")) {
        out_val = true;
        return true;
    }
    if (json.substr(pos).starts_with("false")) {
        out_val = false;
        return true;
    }
    return false;
}

std::vector<uint8_t> EncodePreviewPayload(const PreviewPayloadData& data) {
    std::vector<uint8_t> buf;
    buf.reserve(128 + data.thumb_bgra.size());
    AppendPod(buf, data.status);
    AppendPod(buf, data.size_bytes);
    AppendPod(buf, data.modified_unix_ms);
    AppendPod(buf, data.image_width);
    AppendPod(buf, data.image_height);
    AppendPod(buf, data.thumb_width);
    AppendPod(buf, data.thumb_height);
    AppendPod(buf, data.thumb_stride);
    AppendLengthPrefixedWide(buf, data.path);
    AppendLengthPrefixedWide(buf, data.display_type);
    AppendLengthPrefixedWide(buf, data.text_excerpt);
    const uint32_t bgra_len = static_cast<uint32_t>(data.thumb_bgra.size());
    AppendPod(buf, bgra_len);
    if (bgra_len > 0) {
        buf.insert(buf.end(), data.thumb_bgra.begin(), data.thumb_bgra.end());
    }
    return buf;
}

bool DecodePreviewPayload(const std::vector<uint8_t>& bytes, PreviewPayloadData& out_data) {
    out_data = {};
    size_t offset = 0;
    if (!ReadPod(bytes, offset, out_data.status) ||
        !ReadPod(bytes, offset, out_data.size_bytes) ||
        !ReadPod(bytes, offset, out_data.modified_unix_ms) ||
        !ReadPod(bytes, offset, out_data.image_width) ||
        !ReadPod(bytes, offset, out_data.image_height) ||
        !ReadPod(bytes, offset, out_data.thumb_width) ||
        !ReadPod(bytes, offset, out_data.thumb_height) ||
        !ReadPod(bytes, offset, out_data.thumb_stride) ||
        !ReadLengthPrefixedWide(bytes, offset, out_data.path) ||
        !ReadLengthPrefixedWide(bytes, offset, out_data.display_type) ||
        !ReadLengthPrefixedWide(bytes, offset, out_data.text_excerpt)) {
        return false;
    }
    uint32_t bgra_len = 0;
    if (!ReadPod(bytes, offset, bgra_len)) return false;
    if (bgra_len > kMaxPayloadBytes || offset + bgra_len > bytes.size()) return false;
    if (bgra_len > 0) {
        out_data.thumb_bgra.assign(bytes.begin() + static_cast<ptrdiff_t>(offset),
                                   bytes.begin() + static_cast<ptrdiff_t>(offset + bgra_len));
    }
    return true;
}

bool RandomBytes(void* out, size_t size) noexcept {
    if (!out || size == 0) return false;
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(size),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::wstring MakeUniquePipeName() {
    static std::atomic<uint64_t> seq{0};
    uint8_t rnd[16]{};
    RandomBytes(rnd, sizeof(rnd));
    wchar_t hex[33]{};
    for (size_t i = 0; i < sizeof(rnd); ++i) {
        swprintf_s(hex + i * 2, 3, L"%02x", rnd[i]);
    }
    const uint64_t n = ++seq;
    return L"\\\\.\\pipe\\PulseDesktop_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
           std::to_wstring(n) + hex;
}

HANDLE CreateAuthenticatedServerPipe(const std::wstring& pipe_name) {
    const std::wstring sid = CurrentUserSidString();
    if (sid.empty()) return INVALID_HANDLE_VALUE;
    const std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        return INVALID_HANDLE_VALUE;
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;

    HANDLE pipe = CreateNamedPipeW(
        pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        65536,
        65536,
        0,
        &sa);
    LocalFree(sd);
    return pipe;
}

HANDLE ConnectClientPipe(const std::wstring& pipe_name, DWORD wait_timeout_ms) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        HANDLE h = CreateFileW(
            pipe_name.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
            nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;

        const DWORD err = GetLastError();
        if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND) {
            return INVALID_HANDLE_VALUE;
        }
        const ULONGLONG elapsed = GetTickCount64() - start;
        if (elapsed >= wait_timeout_ms) return INVALID_HANDLE_VALUE;
        const DWORD rem = static_cast<DWORD>(std::min<ULONGLONG>(50, wait_timeout_ms - elapsed));
        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(pipe_name.c_str(), rem);
        } else {
            Sleep(15);
        }
    }
}

AsyncPipeEndpoint::~AsyncPipeEndpoint() {
    Stop();
}

bool AsyncPipeEndpoint::StartServer(const std::wstring& pipe_name,
                                    FrameCallback on_frame,
                                    DisconnectCallback on_disconnect,
                                    DWORD accept_timeout_ms) {
    Stop();
    HANDLE pipe = CreateAuthenticatedServerPipe(pipe_name);
    if (pipe == INVALID_HANDLE_VALUE) return false;

    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    write_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!stop_event_ || !write_event_) {
        CloseHandle(pipe);
        if (stop_event_) { CloseHandle(stop_event_); stop_event_ = nullptr; }
        if (write_event_) { CloseHandle(write_event_); write_event_ = nullptr; }
        return false;
    }

    pipe_ = pipe;
    on_frame_ = std::move(on_frame);
    on_disconnect_ = std::move(on_disconnect);
    running_.store(true, std::memory_order_release);
    connected_.store(false, std::memory_order_release);

    writer_thread_ = std::thread([this] { WriterLoop(); });
    io_thread_ = std::thread([this, accept_timeout_ms] { IoLoop(true, accept_timeout_ms); });
    return true;
}

bool AsyncPipeEndpoint::StartClient(const std::wstring& pipe_name,
                                    FrameCallback on_frame,
                                    DisconnectCallback on_disconnect,
                                    DWORD connect_timeout_ms) {
    Stop();
    HANDLE pipe = ConnectClientPipe(pipe_name, connect_timeout_ms);
    if (pipe == INVALID_HANDLE_VALUE) return false;

    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    write_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!stop_event_ || !write_event_) {
        CloseHandle(pipe);
        if (stop_event_) { CloseHandle(stop_event_); stop_event_ = nullptr; }
        if (write_event_) { CloseHandle(write_event_); write_event_ = nullptr; }
        return false;
    }

    pipe_ = pipe;
    on_frame_ = std::move(on_frame);
    on_disconnect_ = std::move(on_disconnect);
    running_.store(true, std::memory_order_release);
    connected_.store(true, std::memory_order_release);

    writer_thread_ = std::thread([this] { WriterLoop(); });
    io_thread_ = std::thread([this] { IoLoop(false, 0); });
    return true;
}

bool AsyncPipeEndpoint::Send(MessageType type, uint64_t request_id, std::string_view utf8_json) {
    std::vector<uint8_t> bytes(reinterpret_cast<const uint8_t*>(utf8_json.data()),
                               reinterpret_cast<const uint8_t*>(utf8_json.data()) + utf8_json.size());
    return SendBinary(type, request_id, std::move(bytes));
}

bool AsyncPipeEndpoint::SendBinary(MessageType type, uint64_t request_id, std::vector<uint8_t> payload) {
    if (!running_.load(std::memory_order_acquire)) return false;
    if (payload.size() > kMaxPayloadBytes) return false;

    FrameHeader hdr{};
    hdr.magic = kFrameMagic;
    hdr.version = kProtocolVersion;
    hdr.type = static_cast<uint16_t>(type);
    hdr.request_id = request_id;
    hdr.timestamp_ms = GetTickCount64();
    hdr.payload_size = static_cast<uint32_t>(payload.size());
    hdr.reserved = 0;

    std::vector<uint8_t> wire(sizeof(FrameHeader) + payload.size());
    std::memcpy(wire.data(), &hdr, sizeof(FrameHeader));
    if (!payload.empty()) {
        std::memcpy(wire.data() + sizeof(FrameHeader), payload.data(), payload.size());
    }

    {
        std::lock_guard lock(out_mutex_);
        if (out_queued_bytes_ + wire.size() > kMaxOutboundQueueBytes) {
            return false;
        }
        out_queued_bytes_ += wire.size();
        out_queue_.push_back(std::move(wire));
    }
    if (write_event_) SetEvent(write_event_);
    return true;
}

void AsyncPipeEndpoint::Stop() noexcept {
    const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    connected_.store(false, std::memory_order_release);
    if (stop_event_) SetEvent(stop_event_);
    if (write_event_) SetEvent(write_event_);
    if (was_running && pipe_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(pipe_, nullptr);
    }
    if (writer_thread_.joinable() && writer_thread_.get_id() != std::this_thread::get_id()) {
        writer_thread_.join();
    }
    if (io_thread_.joinable() && io_thread_.get_id() != std::this_thread::get_id()) {
        io_thread_.join();
    }
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    if (stop_event_) {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
    if (write_event_) {
        CloseHandle(write_event_);
        write_event_ = nullptr;
    }
    {
        std::lock_guard lock(out_mutex_);
        out_queue_.clear();
        out_queued_bytes_ = 0;
    }
}

void AsyncPipeEndpoint::IoLoop(bool is_server, DWORD connect_timeout_ms) {
    if (is_server) {
        ScopedEvent ev;
        if (!ev.handle) {
            connected_.store(false, std::memory_order_release);
            if (running_.load(std::memory_order_acquire) && on_disconnect_) on_disconnect_();
            return;
        }
        OVERLAPPED ov{};
        ov.hEvent = ev.handle;
        bool accepted = false;
        if (ConnectNamedPipe(pipe_, &ov)) {
            accepted = true;
        } else {
            const DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                accepted = true;
            } else if (err == ERROR_IO_PENDING) {
                HANDLE waits[2]{ev.handle, stop_event_};
                const DWORD res = WaitForMultipleObjects(2, waits, FALSE, connect_timeout_ms);
                if (res == WAIT_OBJECT_0) {
                    DWORD ignored = 0;
                    accepted = GetOverlappedResult(pipe_, &ov, &ignored, FALSE) != FALSE;
                } else {
                    CancelIoEx(pipe_, &ov);
                    DWORD ignored = 0;
                    GetOverlappedResult(pipe_, &ov, &ignored, TRUE);
                }
            }
        }
        if (!accepted || !running_.load(std::memory_order_acquire)) {
            connected_.store(false, std::memory_order_release);
            if (write_event_) SetEvent(write_event_);
            if (running_.load(std::memory_order_acquire) && on_disconnect_) on_disconnect_();
            return;
        }
        connected_.store(true, std::memory_order_release);
        if (write_event_) SetEvent(write_event_);
    }

    while (running_.load(std::memory_order_acquire)) {
        FrameHeader hdr{};
        if (!OverlappedReadExact(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr))) {
            break;
        }
        if (hdr.magic != kFrameMagic || hdr.version != kProtocolVersion ||
            hdr.reserved != 0 || hdr.payload_size > kMaxPayloadBytes) {
            break;
        }
        Frame frame{};
        frame.type = static_cast<MessageType>(hdr.type);
        frame.request_id = hdr.request_id;
        frame.timestamp_ms = hdr.timestamp_ms;
        if (hdr.payload_size > 0) {
            frame.payload.resize(hdr.payload_size);
            if (!OverlappedReadExact(frame.payload.data(), frame.payload.size())) {
                break;
            }
        }
        if (on_frame_ && running_.load(std::memory_order_acquire)) {
            try {
                on_frame_(std::move(frame));
            } catch (...) {
            }
        }
    }

    connected_.store(false, std::memory_order_release);
    if (write_event_) SetEvent(write_event_);
    if (running_.load(std::memory_order_acquire) && on_disconnect_) {
        try {
            on_disconnect_();
        } catch (...) {
        }
    }
}

void AsyncPipeEndpoint::WriterLoop() {
    while (running_.load(std::memory_order_acquire)) {
        if (!connected_.load(std::memory_order_acquire)) {
            HANDLE waits[2]{write_event_, stop_event_};
            const DWORD waited = WaitForMultipleObjects(2, waits, FALSE, 50);
            if (waited == WAIT_OBJECT_0 + 1) break;
            continue;
        }
        std::vector<uint8_t> next_wire;
        {
            std::lock_guard lock(out_mutex_);
            if (!out_queue_.empty()) {
                next_wire = std::move(out_queue_.front());
                out_queue_.pop_front();
                out_queued_bytes_ -= next_wire.size();
            }
        }
        if (next_wire.empty()) {
            HANDLE waits[2]{write_event_, stop_event_};
            const DWORD waited = WaitForMultipleObjects(2, waits, FALSE, 250);
            if (waited == WAIT_OBJECT_0 + 1) break;
            continue;
        }
        if (!OverlappedWriteExact(next_wire.data(), next_wire.size())) {
            connected_.store(false, std::memory_order_release);
            break;
        }
    }
}

bool AsyncPipeEndpoint::OverlappedReadExact(uint8_t* dst, size_t count) {
    size_t offset = 0;
    while (offset < count && running_.load(std::memory_order_acquire)) {
        ScopedEvent ev;
        if (!ev.handle) return false;
        OVERLAPPED ov{};
        ov.hEvent = ev.handle;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(count - offset, 1u << 20));
        DWORD got = 0;
        if (!ReadFile(pipe_, dst + offset, chunk, &got, &ov)) {
            const DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) return false;
            HANDLE waits[2]{ev.handle, stop_event_};
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w != WAIT_OBJECT_0) {
                CancelIoEx(pipe_, &ov);
                DWORD ignored = 0;
                GetOverlappedResult(pipe_, &ov, &ignored, TRUE);
                return false;
            }
            if (!GetOverlappedResult(pipe_, &ov, &got, FALSE)) return false;
        }
        if (got == 0) return false;
        offset += got;
    }
    return offset == count;
}

bool AsyncPipeEndpoint::OverlappedWriteExact(const uint8_t* src, size_t count) {
    size_t offset = 0;
    while (offset < count && running_.load(std::memory_order_acquire)) {
        ScopedEvent ev;
        if (!ev.handle) return false;
        OVERLAPPED ov{};
        ov.hEvent = ev.handle;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(count - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(pipe_, src + offset, chunk, &written, &ov)) {
            const DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) return false;
            HANDLE waits[2]{ev.handle, stop_event_};
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 5000);
            if (w != WAIT_OBJECT_0) {
                CancelIoEx(pipe_, &ov);
                DWORD ignored = 0;
                GetOverlappedResult(pipe_, &ov, &ignored, TRUE);
                return false;
            }
            if (!GetOverlappedResult(pipe_, &ov, &written, FALSE)) return false;
        }
        if (written == 0) return false;
        offset += written;
    }
    return offset == count;
}

} // namespace pulse::desktop::ipc

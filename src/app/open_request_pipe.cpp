#include "open_request_pipe.h"
#include "../common/current_user_security.h"
#include <algorithm>
#include <atomic>
#include <iterator>
#include <string_view>
#include <thread>
#include <vector>

namespace pulse::app {
namespace {
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Frame { uint32_t magic = OpenRequestPipe::Magic, version = OpenRequestPipe::Version, size = 0, reserved = 0; };
struct Response { uint32_t magic = OpenRequestPipe::Magic, version = OpenRequestPipe::Version, reply = 0, reserved = 0; };
static_assert(sizeof(Frame) == 16 && sizeof(Response) == 16);
constexpr wchar_t prefix[] = L"\\\\.\\pipe\\Pulse.Open.RawV2.";
bool Nonce(std::wstring_view nonce) {
    if (nonce.size() != 38 || nonce.front() != L'{' || nonce.back() != L'}') return false;
    for (size_t i = 1; i < 37; ++i) {
        if (i == 9 || i == 14 || i == 19 || i == 24) { if (nonce[i] != L'-') return false; }
        else if (!((nonce[i] >= L'0' && nonce[i] <= L'9') || (nonce[i] >= L'a' && nonce[i] <= L'f') ||
            (nonce[i] >= L'A' && nonce[i] <= L'F'))) return false;
    }
    return true;
}
bool LocalName(const std::wstring& name) {
    const auto dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || !Nonce(std::wstring_view(name).substr(dot + 1))) return false;
    return name.starts_with(prefix) && name.size() < 256 && name.find(L'\0') == std::wstring::npos &&
        name.find(L'\\', std::size(prefix) - 1) == std::wstring::npos && name.find(L'/', std::size(prefix) - 1) == std::wstring::npos;
}
bool UserSession(HANDLE process) {
    DWORD session = 0, own = 0;
    return ProcessIdToSessionId(GetProcessId(process), &session) && ProcessIdToSessionId(GetCurrentProcessId(), &own) &&
        session == own && ProcessUserSidString(process) == CurrentUserSidString() && !CurrentUserSidString().empty();
}
DWORD Await(HANDLE pipe, OVERLAPPED& io, HANDLE stop, uint64_t deadline, DWORD& count) {
    HANDLE waits[]{io.hEvent, stop};
    const auto now = GetTickCount64();
    const DWORD timeout = now >= deadline ? 0 : static_cast<DWORD>((std::min)(deadline - now, uint64_t{1000}));
    const DWORD waited = WaitForMultipleObjects(stop ? 2 : 1, waits, FALSE, timeout);
    if (waited == WAIT_OBJECT_0) return GetOverlappedResult(pipe, &io, &count, FALSE) ? ERROR_SUCCESS : GetLastError();
    const DWORD error = waited == WAIT_FAILED ? GetLastError() : waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED;
    CancelIoEx(pipe, &io);
    GetOverlappedResult(pipe, &io, &count, TRUE); // Stack OVERLAPPED and buffer survive cancellation completion.
    return error;
}
DWORD Transfer(HANDLE pipe, void* data, DWORD size, bool write, HANDLE stop, uint64_t deadline) {
    auto* cursor = static_cast<unsigned char*>(data);
    while (size) {
        if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return ERROR_OPERATION_ABORTED;
        if (GetTickCount64() >= deadline) return ERROR_TIMEOUT;
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); if (!event.value) return GetLastError();
        OVERLAPPED io{}; io.hEvent = event.value; DWORD done = 0;
        const BOOL immediate = write ? WriteFile(pipe, cursor, size, &done, &io) : ReadFile(pipe, cursor, size, &done, &io);
        DWORD error = immediate ? ERROR_SUCCESS : GetLastError();
        if (error == ERROR_IO_PENDING) error = Await(pipe, io, stop, deadline, done);
        if (error) return error;
        if (!done || done > size) return ERROR_BROKEN_PIPE;
        cursor += done; size -= done;
    }
    return ERROR_SUCCESS;
}
}
struct OpenRequestPipe::Impl {
    HANDLE pipe = INVALID_HANDLE_VALUE, stop = nullptr, ready = nullptr;
    std::thread worker;
    Handler handler;
    std::atomic<bool> accepting{false};
    void Run() noexcept {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.value) { accepting.store(false, std::memory_order_release); SetEvent(ready); return; }
        SetEvent(ready);
        while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
            try {
                ResetEvent(event.value);
                OVERLAPPED io{}; io.hEvent = event.value; DWORD count = 0;
                const BOOL connected = ConnectNamedPipe(pipe, &io);
                DWORD error = connected ? ERROR_SUCCESS : GetLastError();
                if (error == ERROR_PIPE_CONNECTED) error = ERROR_SUCCESS;
                if (error == ERROR_IO_PENDING) error = Await(pipe, io, stop, GetTickCount64() + 1000, count);
                if (!error && accepting.load(std::memory_order_acquire)) {
                    ULONG pid = 0;
                    if (GetNamedPipeClientProcessId(pipe, &pid)) {
                        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
                        if (process.value && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT && UserSession(process.value)) {
                            const auto deadline = GetTickCount64() + 1000;
                            Frame frame;
                            if (!Transfer(pipe, &frame, sizeof(frame), false, stop, deadline) && frame.magic == Magic &&
                                frame.version == Version && !frame.reserved && frame.size && frame.size <= MaxPayload) {
                                std::vector<unsigned char> payload(frame.size);
                                if (!Transfer(pipe, payload.data(), frame.size, false, stop, deadline) && accepting.load(std::memory_order_acquire)) {
                                    const Reply answer = handler(payload);
                                    if (static_cast<uint32_t>(answer) <= static_cast<uint32_t>(Reply::Retry)) {
                                        Response response; response.reply = static_cast<uint32_t>(answer);
                                        if (!Transfer(pipe, &response, sizeof(response), true, stop, deadline)) {
                                            uint32_t ack = 0;
                                            // Bounded ACK prevents disconnect racing a client's reply read.
                                            Transfer(pipe, &ack, sizeof(ack), false, stop, deadline);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            } catch (...) { /* No exception or handler failure becomes a success reply. */ }
            DisconnectNamedPipe(pipe);
            if (WaitForSingleObject(stop, 1) == WAIT_OBJECT_0) break;
        }
        accepting.store(false, std::memory_order_release);
    }
};
OpenRequestPipe::OpenRequestPipe() : impl_(std::make_unique<Impl>()) {}
OpenRequestPipe::~OpenRequestPipe() { Stop(); }
bool OpenRequestPipe::Identity(HANDLE process, ServerIdentity& result) noexcept {
    result = {}; FILETIME exit{}, kernel{}, user{};
    result.pid = GetProcessId(process);
    if (!result.pid || !GetProcessTimes(process, &result.created, &exit, &kernel, &user)) { result = {}; return false; }
    return true;
}
std::wstring OpenRequestPipe::Name(DWORD pid, const std::wstring& nonce) {
    if (!pid || !Nonce(nonce)) return {};
    const auto sid = CurrentUserSidString(); DWORD session = 0;
    if (sid.empty() || !ProcessIdToSessionId(GetCurrentProcessId(), &session)) return {};
    const auto result = std::wstring(prefix) + sid + L"." + std::to_wstring(session) + L"." + std::to_wstring(pid) + L"." + nonce;
    return LocalName(result) ? result : std::wstring();
}
bool OpenRequestPipe::Start(const std::wstring& name, Handler handler) noexcept {
    if (impl_->worker.joinable() || !handler) return false;
    try {
        if (!LocalName(name) || name != Name(GetCurrentProcessId(), name.substr(name.find_last_of(L'.') + 1))) return false;
        CurrentUserSecurityAttributes security; if (!security) return false;
        impl_->handler = std::move(handler);
        impl_->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        impl_->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!impl_->stop || !impl_->ready) { Stop(); return false; }
        impl_->pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, MaxPayload, MaxPayload, 0, security.get());
        if (impl_->pipe == INVALID_HANDLE_VALUE) { Stop(); return false; }
        impl_->accepting.store(true, std::memory_order_release);
        impl_->worker = std::thread([state = impl_.get()] { state->Run(); });
        if (WaitForSingleObject(impl_->ready, 1000) == WAIT_OBJECT_0 && impl_->accepting.load(std::memory_order_acquire)) return true;
    } catch (...) {}
    Stop(); return false;
}
void OpenRequestPipe::Stop() noexcept {
    impl_->accepting.store(false, std::memory_order_release);
    if (impl_->stop) SetEvent(impl_->stop);
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->pipe != INVALID_HANDLE_VALUE) { CloseHandle(impl_->pipe); impl_->pipe = INVALID_HANDLE_VALUE; }
    if (impl_->stop) { CloseHandle(impl_->stop); impl_->stop = nullptr; }
    if (impl_->ready) { CloseHandle(impl_->ready); impl_->ready = nullptr; }
    impl_->handler = {};
}
OpenRequestPipe::Result OpenRequestPipe::Exchange(const std::wstring& name, const ServerIdentity& expected,
    std::span<const unsigned char> payload, uint64_t deadline) noexcept {
    Result result;
    try {
        if (!LocalName(name) || !expected.pid || payload.empty() || payload.size() > MaxPayload) { result.error = ERROR_INVALID_PARAMETER; return result; }
        if (GetTickCount64() >= deadline) { result.error = ERROR_TIMEOUT; return result; }
        Handle pipe(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        if (pipe.value == INVALID_HANDLE_VALUE) { result.error = GetLastError(); return result; }
        ULONG pid = 0; ServerIdentity actual;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, expected.pid));
        if (!GetNamedPipeServerProcessId(pipe.value, &pid) || pid != expected.pid || !process.value ||
            WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT || !Identity(process.value, actual) ||
            CompareFileTime(&actual.created, &expected.created) != 0 || !UserSession(process.value)) {
            result.error = ERROR_ACCESS_DENIED; return result;
        }
        Frame frame; frame.size = static_cast<uint32_t>(payload.size());
        result.request_may_have_been_sent = true;
        result.stage = Stage::Header;
        result.error = Transfer(pipe.value, &frame, sizeof(frame), true, nullptr, deadline);
        if (!result.error) { result.stage = Stage::Payload; result.error = Transfer(pipe.value, const_cast<unsigned char*>(payload.data()), frame.size, true, nullptr, deadline); }
        Response response;
        if (!result.error) { result.stage = Stage::Reply; result.error = Transfer(pipe.value, &response, sizeof(response), false, nullptr, deadline); }
        if (!result.error) {
            if (response.magic != Magic || response.version != Version || response.reserved || response.reply > static_cast<uint32_t>(Reply::Retry))
                result.error = ERROR_INVALID_DATA;
            else {
                result.reply = static_cast<Reply>(response.reply);
                uint32_t ack = Magic;
                Transfer(pipe.value, &ack, sizeof(ack), true, nullptr, deadline); // Lost ACK cannot undo a received definitive reply.
            }
        }
    } catch (...) { result.error = ERROR_NOT_ENOUGH_MEMORY; }
    return result;
}
}

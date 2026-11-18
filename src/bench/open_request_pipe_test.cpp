#include "../app/open_request_pipe.h"
#include "../common/current_user_security.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <thread>
#include <vector>

namespace {
using Pipe = pulse::app::OpenRequestPipe;
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* label) { ++checks; if (!ok) ++failures; std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); }
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::wstring Name() {
    static unsigned sequence = 0;
    wchar_t nonce[40]{};
    swprintf_s(nonce, L"{%08lX-%04X-0000-0000-%012llX}", GetCurrentProcessId(), ++sequence,
        static_cast<unsigned long long>(GetTickCount64() & 0xffffffffffffULL));
    return Pipe::Name(GetCurrentProcessId(), nonce);
}
Pipe::Result Exchange(const std::wstring& name, Pipe::ServerIdentity identity, std::span<const unsigned char> bytes) {
    const auto deadline = GetTickCount64() + 2000;
    Pipe::Result result;
    do {
        result = Pipe::Exchange(name, identity, bytes, deadline);
        // Only test harness availability retry, before any bytes were sent.
        if (result.reply || result.request_may_have_been_sent || result.error != ERROR_PIPE_BUSY) break;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    return result;
}
HANDLE Connect(const std::wstring& name) {
    const auto deadline = GetTickCount64() + 1000;
    do {
        HANDLE handle = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (handle != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) return handle;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    return INVALID_HANDLE_VALUE;
}
std::vector<unsigned char> RawV2(bool maximum) {
    // Synthetic bytes in the current 32-byte v2 header layout, not a new route
    // enum. The transport must not inspect these fields or normalize the path.
    const std::wstring path = maximum ? std::wstring(32767, L'\x4e2d') : L"pulse:win-e";
    std::vector<unsigned char> bytes(32 + (path.size() + 1) * sizeof(wchar_t));
    const uint32_t version = 2, chars = static_cast<uint32_t>(path.size() + 1);
    const uint64_t deadline = GetTickCount64() + 10000;
    std::memcpy(bytes.data(), &version, 4); std::memcpy(bytes.data() + 4, &chars, 4);
    std::memcpy(bytes.data() + 8, &deadline, 8); bytes[16] = 1;
    std::memcpy(bytes.data() + 32, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
    return bytes;
}
void StopCase(unsigned partial) {
    Pipe server; std::atomic<unsigned> calls{0}; const auto name = Name();
    Check(server.Start(name, [&](auto) { ++calls; return Pipe::Reply::Accepted; }), "Stop fixture starts ready");
    Handle client;
    if (partial) {
        client.value = Connect(name); Check(client.value != INVALID_HANDLE_VALUE, "own partial client connected");
        if (client.value != INVALID_HANDLE_VALUE && partial == 2) {
            const uint32_t magic = Pipe::Magic; DWORD written = 0;
            Check(WriteFile(client.value, &magic, sizeof(magic), &written, nullptr) && written == sizeof(magic), "partial header delivered");
        }
    }
    const auto start = GetTickCount64(); server.Stop();
    Check(GetTickCount64() - start < 1500 && calls == 0, "Stop cancels pending connect/read and joins without dispatch");
    server.Stop();
}
void StopAfterReply() {
    Pipe server; std::atomic<unsigned> calls{0}; const auto name = Name();
    Check(server.Start(name, [&](auto) { ++calls; return Pipe::Reply::Accepted; }), "reply/ACK stop fixture ready");
    Handle client(Connect(name)); const auto payload = RawV2(false);
    const std::array<uint32_t, 4> frame{Pipe::Magic, Pipe::Version, static_cast<uint32_t>(payload.size()), 0};
    DWORD done = 0; std::array<uint32_t, 4> response{};
    const bool replied = client.value != INVALID_HANDLE_VALUE &&
        WriteFile(client.value, frame.data(), sizeof(frame), &done, nullptr) && done == sizeof(frame) &&
        WriteFile(client.value, payload.data(), static_cast<DWORD>(payload.size()), &done, nullptr) && done == payload.size() &&
        ReadFile(client.value, response.data(), sizeof(response), &done, nullptr) && done == sizeof(response);
    Check(replied && response[0] == Pipe::Magic && response[2] == static_cast<uint32_t>(Pipe::Reply::Accepted),
        "client receives real response but deliberately withholds ACK");
    const auto start = GetTickCount64(); server.Stop();
    Check(GetTickCount64() - start < 1500 && calls == 1, "Stop drains reply/ACK boundary without waiting for client or UI");
}
void Malformed(unsigned field, uint32_t value) {
    Pipe server; std::atomic<unsigned> calls{0}; const auto name = Name();
    Check(server.Start(name, [&](auto) { ++calls; return Pipe::Reply::Accepted; }), "malformed-frame fixture starts");
    {
        Handle client(Connect(name)); std::array<uint32_t, 4> frame{Pipe::Magic, Pipe::Version, 1, 0};
        frame[field] = value; DWORD written = 0;
        Check(client.value != INVALID_HANDLE_VALUE && WriteFile(client.value, frame.data(), sizeof(frame), &written, nullptr), "malformed frame sent to own pipe");
        // Wait for server rejection, not an arbitrary sleep. Server disconnect
        // makes this bounded-by-server read return without any Reply envelope.
        unsigned char response[16]{}; DWORD read = 0;
        const BOOL got = client.value != INVALID_HANDLE_VALUE && ReadFile(client.value, response, sizeof(response), &read, nullptr);
        Check(!got || read == 0, "malformed frame produces no success reply");
    }
    server.Stop(); Check(calls == 0, "malformed frame never reaches handler");
}
void StalledPeer(bool drain_payload, const Pipe::ServerIdentity& identity) {
    const auto name = Name(); pulse::CurrentUserSecurityAttributes security;
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr)), stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Check(security && ready.value && stop.value, "owned stalled peer resources");
    if (!security || !ready.value || !stop.value) return;
    std::atomic<bool> prepared{false}, drained{false};
    std::thread peer([&] {
        Handle pipe(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 128, 128, 0, security.get()));
        prepared = pipe.value != INVALID_HANDLE_VALUE; SetEvent(ready.value);
        if (!prepared) return;
        if (!ConnectNamedPipe(pipe.value, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return;
        if (drain_payload) {
            std::array<uint32_t, 4> frame{}; DWORD read = 0;
            if (ReadFile(pipe.value, frame.data(), sizeof(frame), &read, nullptr) && read == sizeof(frame) && frame[2] <= Pipe::MaxPayload) {
                std::vector<unsigned char> payload(frame[2]); size_t offset = 0;
                while (offset < payload.size()) {
                    if (!ReadFile(pipe.value, payload.data() + offset, static_cast<DWORD>(payload.size() - offset), &read, nullptr) || !read) break;
                    offset += read;
                }
                drained = offset == payload.size();
            }
        }
        WaitForSingleObject(stop.value, 2000); DisconnectNamedPipe(pipe.value);
    });
    WaitForSingleObject(ready.value, 1000);
    if (prepared) {
        const auto payload = RawV2(true); const auto start = GetTickCount64();
        const auto result = Pipe::Exchange(name, identity, payload, start + 250);
        Check(!result.reply && result.request_may_have_been_sent && result.error == ERROR_TIMEOUT &&
            result.stage == (drain_payload ? Pipe::Stage::Reply : Pipe::Stage::Payload) && GetTickCount64() - start < 1500,
            drain_payload ? "pending reply read deadline cancels and drains without fallback" : "pending payload write deadline cancels and drains without fallback");
        if (drain_payload) Check(drained, "stalled peer actually consumed full payload before withholding reply");
    } else Check(false, "stalled peer creation");
    SetEvent(stop.value); peer.join();
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Handle done(CreateEventW(nullptr, TRUE, FALSE, nullptr)); if (!done.value) return 2;
    std::thread watchdog([&] { if (WaitForSingleObject(done.value, 15000) != WAIT_OBJECT_0) {
        std::printf("[FAIL] own-process test watchdog\n"); TerminateProcess(GetCurrentProcess(), 99);
    }});
    try {
        Pipe::ServerIdentity identity; Check(Pipe::Identity(GetCurrentProcess(), identity), "exact server PID and creation time captured");
        Check(Pipe::Name(identity.pid, L"../bad").empty(), "invalid nonce rejected");
        Pipe server; const auto name = Name(); const DWORD caller = GetCurrentThreadId();
        std::atomic<unsigned> calls{0}; std::atomic<bool> worker_only{true}, exact{true};
        const auto small = RawV2(false), maximum = RawV2(true);
        std::atomic<Pipe::Reply> answer{Pipe::Reply::Accepted};
        Check(server.Start(name, [&](auto bytes) {
            ++calls; if (GetCurrentThreadId() == caller) worker_only = false;
            const bool matches = (bytes.size() == small.size() && std::memcmp(bytes.data(), small.data(), bytes.size()) == 0) ||
                (bytes.size() == maximum.size() && std::memcmp(bytes.data(), maximum.data(), bytes.size()) == 0);
            if (!matches) exact = false;
            return answer.load();
        }), "ready background pipe starts without a window or UI pump");
        Pipe duplicate; Check(!duplicate.Start(name, [](auto) { return Pipe::Reply::Invalid; }), "first-instance ownership prevents duplicate server");
        for (const auto reply : {Pipe::Reply::Accepted, Pipe::Reply::Duplicate, Pipe::Reply::Retry, Pipe::Reply::Invalid}) {
            answer = reply;
            const auto result = Exchange(name, identity, small);
            Check(result.reply == reply && !result.error, "definitive queue reply preserved without presentation claim");
        }
        answer = Pipe::Reply::Accepted;
        Check(Exchange(name, identity, maximum).reply == Pipe::Reply::Accepted && worker_only && exact,
            "maximum Unicode raw v2 bytes and pulse win-e marker preserved on background worker");
        auto oversized = maximum; oversized.push_back(0);
        Check(Pipe::Exchange(name, identity, oversized, GetTickCount64() + 1000).error == ERROR_INVALID_PARAMETER, "oversized frame rejected before sending");
        auto wrong = identity; ++wrong.created.dwLowDateTime;
        Check(Exchange(name, wrong, small).error == ERROR_ACCESS_DENIED, "server PID reuse creation mismatch rejected");
        wrong = identity; ++wrong.pid;
        Check(Exchange(name, wrong, small).error == ERROR_ACCESS_DENIED, "actual named-pipe server PID must match expected identity");
        wrong = identity; wrong.pid = 0;
        Check(Pipe::Exchange(name, wrong, small, GetTickCount64() + 1000).error == ERROR_INVALID_PARAMETER, "missing expected server identity rejected");
        Check(!Pipe::Exchange(L"\\\\other\\pipe\\foreign", identity, small, GetTickCount64() + 1000).reply, "remote pipe name rejected");
        server.Stop(); Check(calls == 5, "identity and invalid requests never reach handler");
        Check(!Pipe::Exchange(name, identity, small, GetTickCount64() + 100).reply, "stopped endpoint cannot acknowledge requests");
        StopCase(0); StopCase(1); StopCase(2); StopAfterReply();
        Malformed(0, 0x504f504e); Malformed(1, 1); Malformed(2, Pipe::MaxPayload + 1); Malformed(3, 1);
        StalledPeer(false, identity); StalledPeer(true, identity);
    } catch (...) { Check(false, "unexpected test exception"); }
    SetEvent(done.value); watchdog.join();
    std::printf("SUMMARY checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}

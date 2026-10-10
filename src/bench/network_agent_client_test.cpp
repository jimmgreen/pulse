#include "../index/network_agent_client.h"
#include <cstdio>
#include <future>

namespace pulse::index {
struct NetworkAgentClientTestAccess {
    static void Start(NetworkAgentClient& client, const std::wstring& pipe, std::promise<void>& done) {
        client.pipe_name_ = pipe;
        client.running_ = true;
        // This live process handle bypasses helper launching without touching the real singleton.
        client.agent_process_ = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
        client.session_requests_[7] = 77;
        client.search_thread_ = std::thread([&client, &done] {
            Query query; query.session_id = 7; query.needle = L"fixture";
            client.SearchRequest(query, 77);
            done.set_value();
        });
    }
    static bool Search(NetworkAgentClient& client, const std::wstring& pipe, SearchResult& result) {
        client.pipe_name_ = pipe;
        client.running_ = true;
        client.agent_process_ = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
        client.session_requests_[7] = 78;
        Query query; query.session_id = 7; query.needle = L"fixture";
        client.SearchRequest(query, 78);
        return client.TakeResult(78, result);
    }
    static bool Released(NetworkAgentClient& client) {
        std::lock_guard lock(client.pipe_mu_);
        return client.active_pipe_ == INVALID_HANDLE_VALUE;
    }
};
}
namespace {
// Agent whose pipe appears only after `delay_ms` (a freshly launched agent).
bool LateAgentSucceeds(DWORD delay_ms, unsigned long long& elapsed) {
    using namespace pulse;
    const auto name = LR"(\\.\pipe\PulseNetworkLate-)" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64());
    std::thread server([&] {
        Sleep(delay_ms);
        HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return;
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            ipc::MsgHeader header{};
            if (ipc::PipeRead(pipe, reinterpret_cast<uint8_t*>(&header), sizeof(header))) {
                std::vector<uint8_t> payload(header.payload_size);
                if (!payload.empty()) ipc::PipeRead(pipe, payload.data(), static_cast<DWORD>(payload.size()));
                const std::vector<uint8_t> empty(8, 0); // total = 0, count = 0
                const auto reply = index::agent::MakeHeader(index::agent::RSP_SEARCH, header.request_id,
                                                            static_cast<uint32_t>(empty.size()));
                ipc::PipeWrite(pipe, reinterpret_cast<const uint8_t*>(&reply), sizeof(reply));
                ipc::PipeWrite(pipe, empty.data(), static_cast<DWORD>(empty.size()));
                FlushFileBuffers(pipe);
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    });
    index::NetworkAgentClient client;
    index::SearchResult result;
    const auto start = GetTickCount64();
    const bool taken = index::NetworkAgentClientTestAccess::Search(client, name, result);
    elapsed = GetTickCount64() - start;
    server.join();
    client.Stop();
    return taken && result.error == ERROR_SUCCESS;
}

// Agent alive but its pipe never appears: the search still ends, boundedly.
bool MissingAgentFailsBounded(unsigned long long& elapsed) {
    const auto name = LR"(\\.\pipe\PulseNetworkMissing-)" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64());
    pulse::index::NetworkAgentClient client;
    pulse::index::SearchResult result;
    const auto start = GetTickCount64();
    const bool taken = pulse::index::NetworkAgentClientTestAccess::Search(client, name, result);
    elapsed = GetTickCount64() - start;
    client.Stop();
    return taken && result.error == ERROR_CONNECTION_ABORTED;
}
}

int main() {
    using namespace pulse;
    const auto name = LR"(\\.\pipe\PulseNetworkFixture-)" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64());
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (pipe == INVALID_HANDLE_VALUE || !release) return 2;
    std::thread server([&] {
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            ipc::MsgHeader header{};
            if (ipc::PipeRead(pipe, reinterpret_cast<uint8_t*>(&header), sizeof(header))) {
                std::vector<uint8_t> payload(header.payload_size);
                if (!payload.empty()) ipc::PipeRead(pipe, payload.data(), static_cast<DWORD>(payload.size()));
                WaitForSingleObject(release, 15000);
            }
        }
        DisconnectNamedPipe(pipe);
    });
    index::NetworkAgentClient client;
    std::promise<void> done; auto completed = done.get_future();
    const auto start = GetTickCount64();
    index::NetworkAgentClientTestAccess::Start(client, name, done);
    const bool timely = completed.wait_for(std::chrono::seconds(12)) == std::future_status::ready;
    const auto elapsed = GetTickCount64() - start;
    index::SearchResult result;
    const bool terminal = timely && client.TakeResult(77, result) && result.error != ERROR_SUCCESS;
    const bool released = timely && index::NetworkAgentClientTestAccess::Released(client);
    SetEvent(release);
    const auto stop = GetTickCount64(); client.Stop();
    const bool joined = GetTickCount64() - stop < 2000;
    server.join(); CloseHandle(pipe); CloseHandle(release);
    printf("[%s] silent isolated pipe produces a terminal search failure after deadline (%llu ms)\n",
        timely && terminal && elapsed >= 4500 ? "PASS" : "FAIL", elapsed);
    printf("[%s] timed-out request releases pipe and joins client thread\n", released && joined ? "PASS" : "FAIL");
    unsigned long long late_ms = 0, missing_ms = 0;
    const bool late = LateAgentSucceeds(800, late_ms);
    printf("[%s] search waits for a starting agent's pipe instead of failing with 1236 (%llu ms)\n",
        late ? "PASS" : "FAIL", late_ms);
    const bool missing = MissingAgentFailsBounded(missing_ms);
    const bool bounded = missing && missing_ms >= 2500 && missing_ms < 6000;
    printf("[%s] a pipe that never appears still ends the search within the wait bound (%llu ms)\n",
        bounded ? "PASS" : "FAIL", missing_ms);
    return timely && terminal && elapsed >= 4500 && released && joined && late && bounded ? 0 : 1;
}

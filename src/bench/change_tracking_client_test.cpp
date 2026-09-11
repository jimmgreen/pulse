// Standalone transport regression executable; includes the implementation to test
// the wire decoder and bounded exchange without exposing test-only public APIs.
#include "../index/change_tracking_client.cpp"
#include <cstdio>
int main(int argc, char**) {
    using namespace pulse;
    using namespace pulse::index;
    if (argc > 1) {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        bool leased = false;
        const auto started = GetTickCount64();
        ULONGLONG renew = 0;
        std::vector<uint8_t> response;
        auto exchange = [&](uint32_t type, const std::vector<uint8_t>& payload) {
            ExchangeDiagnostics diagnostic;
            const bool ok = Exchange(pipe, type, payload, response, kIndexMagic, &diagnostic, 10000);
            std::printf("t=%llu type=%u ok=%d stage=%s error=%lu elapsed=%llu skipped=%u\n",
                GetTickCount64() - started, type, ok, diagnostic.stage, diagnostic.error, diagnostic.elapsed, diagnostic.skipped);
            std::fflush(stdout); return ok;
        };
        while (GetTickCount64() - started < 35000) {
            if (pipe == INVALID_HANDLE_VALUE) {
                pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
                if (pipe == INVALID_HANDLE_VALUE) {
                    std::printf("connect error=%lu\n", GetLastError()); std::fflush(stdout);
                    std::this_thread::sleep_for(std::chrono::seconds(1)); continue;
                }
                ULONG pid = 0; GetNamedPipeServerProcessId(pipe, &pid);
                std::printf("production pipe server pid=%lu\n", pid);
                exchange(REQ_IDX_STATUS, {});
            }
            if (!leased || GetTickCount64() >= renew) {
                ipc::PayloadWriter lease; lease.PutU32(1);
                leased = exchange(REQ_IDX_CHANGE_LEASE, lease.data()); renew = GetTickCount64() + 10000;
            }
            ipc::PayloadWriter request; request.PutU64(0); request.PutU32(6);
            for (const auto* path : {L"C:\\", L"C:\\Users", L"C:\\Users\\SS", L"C:\\Users\\SS\\Desktop", L"C:\\Users\\SS\\Desktop\\pulse", L"C:\\Program Files"}) request.PutString(path);
            if (leased && exchange(REQ_IDX_CHANGE_SUMMARIES, request.data())) {
                ChangeResponse decoded; const bool ok = Decode(response, false, decoded);
                std::printf("decode=%d global=%u rows=%zu", ok, static_cast<uint32_t>(decoded.state), decoded.summaries.size());
                for (const auto& row : decoded.summaries) std::printf(" state=%u/count=%u", static_cast<uint32_t>(row.state),row.count);
                std::printf("\n"); std::fflush(stdout);
            } else { CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE; leased = false; }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (pipe != INVALID_HANDLE_VALUE) {
            if (leased) { ipc::PayloadWriter lease; lease.PutU32(0); exchange(REQ_IDX_CHANGE_LEASE, lease.data()); }
            CloseHandle(pipe);
        }
        return 0;
    }
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };
    check(IsNetworkPath(L"\\\\server\\share\\dir") && IsNetworkPath(L"\\\\?\\UNC\\server\\share\\dir") &&
          !IsNetworkPath(L"C:\\dir") && !IsNetworkPath(L"\\\\?\\C:\\dir"), "local and UNC endpoint routing");
    check(IsNetworkPath(L"\\\\?\\unc\\server\\share\\dir") &&
          IsNetworkPath(L"\\\\?\\UnC\\server\\share\\dir"), "canonical lowercase UNC endpoint routing");
    ipc::PayloadWriter summary;
    summary.PutU32(1); summary.PutU32(1); summary.PutString(L"C:\\folder");
    summary.PutU64(123); summary.PutU32(3); summary.PutU32(1);
    for (uint32_t i = 0; i < 6; ++i) summary.PutU32(i);
    summary.PutU32(2); summary.PutU32(1); summary.PutU32(1);
    ChangeResponse result;
    check(Decode(summary.data(), false, result) && result.summaries.size() == 1 &&
          result.summaries[0].initial_count == 2 && result.summaries[0].has_deleted &&
          result.summaries[0].counts[5] == 5, "summary fields and baseline count");
    auto truncated = summary.data(); truncated.pop_back(); result = {};
    check(!Decode(truncated, false, result), "truncated summary rejected");
    ipc::PayloadWriter details;
    details.PutU32(0); details.PutU64(42); details.PutU32(1);
    details.PutU64(43); details.PutU64(123); details.PutU32(5); details.PutU32(1);
    details.PutU32(1); details.PutString(L"C:\\new"); details.PutString(L"C:\\old");
    result = {};
    check(Decode(details.data(), true, result) && result.next_cursor == 42 &&
          result.records.size() == 1 && result.records[0].kind == ChangeKind::MovedOut &&
          result.records[0].source == ChangeSource::InitialMtime, "detail cursor and source");
    wchar_t name[128]{};
    swprintf_s(name, L"\\\\.\\pipe\\PulseTrackingTest-%lu", GetCurrentProcessId());
    HANDLE server = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE, 1, 4096, 4096, 0, nullptr);
    HANDLE client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (server != INVALID_HANDLE_VALUE && client != INVALID_HANDLE_VALUE) {
        const auto start = GetTickCount64();
        std::vector<uint8_t> response;
        const bool ok = Exchange(client, REQ_IDX_CHANGE_LEASE, {}, response);
        const auto elapsed = GetTickCount64() - start;
        check(!ok && elapsed < 3500, "old host silent response times out");
    } else check(false, "create isolated test pipe");
    if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
    if (server != INVALID_HANDLE_VALUE) CloseHandle(server);
    server = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE, 1, 65536, 65536, 0, nullptr);
    client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (server != INVALID_HANDLE_VALUE && client != INVALID_HANDLE_VALUE) {
        bool server_ok = true;
        uint32_t ids[2]{};
        std::thread host([&] {
            for (int turn = 0; turn < 2 && server_ok; ++turn) {
                ipc::MsgHeader request{};
                server_ok = Transfer(server, &request, sizeof(request), false);
                ids[turn] = request.request_id;
                for (int i = 0; i < 20 && server_ok; ++i) {
                    auto broadcast = MakeIndexHdr(RSP_IDX_STATUS, 0, 0);
                    server_ok = Transfer(server, &broadcast, sizeof(broadcast), true);
                }
                // A late same-type reply must not satisfy a newer request.
                auto stale = MakeIndexHdr(RSP_IDX_CHANGE_LEASE, request.request_id - 1, 1);
                uint8_t marker = 99;
                server_ok = server_ok && Transfer(server, &stale, sizeof(stale), true) &&
                    Transfer(server, &marker, 1, true);
                auto valid = MakeIndexHdr(RSP_IDX_CHANGE_LEASE, request.request_id, 1);
                marker = static_cast<uint8_t>(turn + 1);
                server_ok = server_ok && Transfer(server, &valid, sizeof(valid), true) &&
                    Transfer(server, &marker, 1, true);
            }
        });
        std::vector<uint8_t> response;
        const bool first = Exchange(client, REQ_IDX_CHANGE_LEASE, {}, response) && response == std::vector<uint8_t>{1};
        const bool second = Exchange(client, REQ_IDX_CHANGE_LEASE, {}, response) && response == std::vector<uint8_t>{2};
        host.join();
        check(first && second && server_ok, "status backlog and stale reply cannot cause false unavailability");
        check(ids[0] != 0 && ids[1] != 0 && ids[0] != ids[1], "wire requests have unique nonzero identifiers");
        CloseHandle(server); server = INVALID_HANDLE_VALUE;
        check(!Exchange(client, REQ_IDX_CHANGE_LEASE, {}, response), "disconnected host remains unavailable");
    } else check(false, "create isolated status backlog pipe");
    if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
    if (server != INVALID_HANDLE_VALUE) CloseHandle(server);
    server = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE, 1, 65536, 65536, 0, nullptr);
    client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (server != INVALID_HANDLE_VALUE && client != INVALID_HANDLE_VALUE) {
        bool served = false;
        std::thread host([&] {
            ipc::MsgHeader request{};
            if (!Transfer(server, &request, sizeof(request), false)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1800));
            auto response = MakeIndexHdr(RSP_IDX_CHANGE_SUMMARIES, request.request_id, 0);
            served = Transfer(server, &response, sizeof(response), true);
        });
        std::vector<uint8_t> response;
        ExchangeDiagnostics diagnostic;
        const bool ok = Exchange(client, REQ_IDX_CHANGE_SUMMARIES, {}, response, kIndexMagic, &diagnostic, 10000);
        host.join();
        check(ok && served && diagnostic.elapsed >= 1700, "slow valid summary does not falsely disconnect");
        HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread cancellation([&] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); SetEvent(cancel); });
        diagnostic = {};
        const bool cancelled = !Exchange(client, REQ_IDX_CHANGE_SUMMARIES, {}, response, kIndexMagic, &diagnostic, 10000, cancel);
        cancellation.join();
        check(cancelled && diagnostic.error == ERROR_OPERATION_ABORTED && diagnostic.elapsed < 1000,
              "stop interrupts silent long data request promptly");
        CloseHandle(cancel);
    } else check(false, "create isolated slow-summary pipe");
    if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
    if (server != INVALID_HANDLE_VALUE) CloseHandle(server);
    return failures ? 1 : 0;
}

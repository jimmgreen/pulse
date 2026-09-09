#include "../index/index_protocol.h"
#include "../ipc/protocol.h"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

using namespace pulse::index;
using pulse::ipc::MsgHeader;
using pulse::ipc::PayloadReader;
using pulse::ipc::PayloadWriter;
using pulse::ipc::PipeRead;
using pulse::ipc::PipeWrite;

namespace {

int failures = 0;

void Check(bool condition, const wchar_t* name) {
    std::wcout << (condition ? L"[PASS] " : L"[FAIL] ") << name << L'\n';
    if (!condition) ++failures;
}

std::wstring SiblingExecutable(const wchar_t* name) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return name;
    return std::wstring(path, slash + 1) + name;
}

HANDLE Connect(const std::wstring& pipe_name, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        HANDLE pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        if (GetTickCount64() >= deadline) return INVALID_HANDLE_VALUE;
        if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(pipe_name.c_str(), 50);
        else Sleep(10);
    }
}

bool SendFrame(HANDLE pipe, uint32_t type, uint32_t id,
               const std::vector<uint8_t>& payload = {}) {
    const MsgHeader header = MakeIndexHdr(type, id, static_cast<uint32_t>(payload.size()));
    return PipeWrite(pipe, reinterpret_cast<const uint8_t*>(&header), sizeof(header)) &&
        (payload.empty() || PipeWrite(pipe, payload.data(),
                                      static_cast<DWORD>(payload.size())));
}

bool ReadFrame(HANDLE pipe, MsgHeader& header, std::vector<uint8_t>& payload,
               DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available >= sizeof(header)) break;
        if (GetTickCount64() >= deadline) return false;
        Sleep(2);
    }
    if (!PipeRead(pipe, reinterpret_cast<uint8_t*>(&header), sizeof(header)) ||
        header.magic != kIndexMagic || header.payload_size > kIndexMaxPayload)
        return false;
    payload.resize(header.payload_size);
    return payload.empty() || PipeRead(pipe, payload.data(), header.payload_size);
}

std::vector<uint8_t> SearchPayload(std::wstring needle) {
    PayloadWriter writer;
    writer.PutU32(1u);
    writer.PutU32(static_cast<uint32_t>(ResultSort::Index));
    writer.PutU32(48);
    writer.PutU32(0);
    writer.PutString(needle);
    writer.PutString(L"");
    return writer.data();
}

bool WaitForSearch(HANDLE pipe, uint32_t wanted_id, DWORD timeout_ms,
                   uint32_t* responses = nullptr, uint32_t* matched = nullptr) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    uint32_t count = 0;
    while (GetTickCount64() < deadline) {
        MsgHeader header{};
        std::vector<uint8_t> payload;
        const DWORD remaining = static_cast<DWORD>((std::min<ULONGLONG>)(
            deadline - GetTickCount64(), 250));
        if (!ReadFrame(pipe, header, payload, remaining)) continue;
        if (header.type != RSP_IDX_SEARCH) continue;
        ++count;
        if (header.request_id == wanted_id) {
            PayloadReader reader(payload.data(), payload.size());
            uint32_t total = 0, hits = 0;
            const bool valid = reader.GetU32(total) && reader.GetU32(hits) && hits <= 48;
            if (responses) *responses = count;
            if (matched) *matched = total;
            return valid;
        }
    }
    if (responses) *responses = count;
    return false;
}

} // namespace

int wmain() {
    const std::wstring token = std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    const std::wstring pipe_name = L"\\\\.\\pipe\\PulseIndex.Test." + token;
    const std::wstring executable = SiblingExecutable(L"Pulse.Index.exe");
    std::wstring command = L"\"" + executable + L"\" --test-host " + token;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                                        FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                        &startup, &process) != FALSE;
    Check(started, L"index stress host starts on an isolated pipe");
    if (!started) return 1;
    CloseHandle(process.hThread);

    std::vector<HANDLE> clients;
    for (int i = 0; i < 16; ++i) {
        HANDLE pipe = Connect(pipe_name, 10000);
        if (pipe == INVALID_HANDLE_VALUE) break;
        clients.push_back(pipe);
        MsgHeader status{};
        std::vector<uint8_t> payload;
        if (!ReadFrame(pipe, status, payload, 2000) || status.type != RSP_IDX_STATUS) break;
    }
    Check(clients.size() == 16, L"index host accepts the configured 16 clients");

    HANDLE overflow = Connect(pipe_name, 150);
    Check(overflow == INVALID_HANDLE_VALUE,
          L"index host rejects a seventeenth concurrent client");
    if (overflow != INVALID_HANDLE_VALUE) CloseHandle(overflow);

    bool sent_all = clients.size() == 16;
    constexpr uint32_t kLastCoalescedId = 299;
    if (sent_all) {
        for (uint32_t id = 100; id <= kLastCoalescedId; ++id)
            sent_all = SendFrame(clients[0], REQ_IDX_SEARCH, id,
                                 SearchPayload(L"stress")) && sent_all;
    }
    uint32_t response_count = 0;
    const bool latest_returned = sent_all &&
        WaitForSearch(clients[0], kLastCoalescedId, 10000, &response_count);
    Check(latest_returned, L"coalesced search returns the newest request");
    Check(latest_returned && response_count < 200,
          L"rapid searches are coalesced instead of all executing");

    uint32_t dot_matches = 0;
    const bool dot_found = !clients.empty() &&
        SendFrame(clients[0], REQ_IDX_SEARCH, 300, SearchPayload(L".codex")) &&
        WaitForSearch(clients[0], 300, 10000, nullptr, &dot_matches);
    Check(dot_found && dot_matches == 1, L"dot folder survives the search pipe roundtrip");
    uint32_t empty_matches = 1;
    const bool empty_found = !clients.empty() &&
        SendFrame(clients[0], REQ_IDX_SEARCH, 301, SearchPayload(L"nonexistent-dot-folder")) &&
        WaitForSearch(clients[0], 301, 10000, nullptr, &empty_matches);
    Check(empty_found && empty_matches == 0, L"zero-result search does not retain dot folder");

    bool concurrent_ok = clients.size() == 16;
    for (size_t i = 1; concurrent_ok && i < clients.size(); ++i) {
        const uint32_t id = 1000 + static_cast<uint32_t>(i);
        concurrent_ok = SendFrame(clients[i], REQ_IDX_SEARCH, id,
                                  SearchPayload(L"stress-item-" + std::to_wstring(i))) &&
            WaitForSearch(clients[i], id, 10000);
    }
    Check(concurrent_ok, L"all connected clients receive serialized search responses");

    bool oversized_disconnected = false;
    if (clients.size() == 16) {
        const MsgHeader oversized = MakeIndexHdr(
            REQ_IDX_SEARCH, 5000, static_cast<uint32_t>(kIndexMaxRequestPayload + 1));
        PipeWrite(clients[15], reinterpret_cast<const uint8_t*>(&oversized), sizeof(oversized));
        for (int i = 0; i < 100; ++i) {
            DWORD available = 0;
            if (!PeekNamedPipe(clients[15], nullptr, 0, nullptr, &available, nullptr)) {
                oversized_disconnected = true;
                break;
            }
            Sleep(10);
        }
    }
    Check(oversized_disconnected, L"oversized request frame disconnects only its client");

    HANDLE malformed = Connect(pipe_name, 2000);
    bool malformed_disconnected = false;
    if (malformed != INVALID_HANDLE_VALUE) {
        MsgHeader status{};
        std::vector<uint8_t> status_payload;
        if (ReadFrame(malformed, status, status_payload, 2000)) {
            PayloadWriter invalid;
            invalid.PutU32(0);
            invalid.PutU32(0xffffffffu); // invalid ResultSort
            invalid.PutU32(48);
            invalid.PutU32(0);
            invalid.PutString(L"stress");
            invalid.PutString(L"");
            SendFrame(malformed, REQ_IDX_SEARCH, 5500, invalid.data());
            for (int i = 0; i < 100; ++i) {
                DWORD available = 0;
                if (!PeekNamedPipe(malformed, nullptr, 0, nullptr, &available, nullptr)) {
                    malformed_disconnected = true;
                    break;
                }
                Sleep(10);
            }
        }
        CloseHandle(malformed);
    }
    Check(malformed_disconnected,
          L"malformed query enum disconnects only its client");

    bool active_sent = !clients.empty();
    for (size_t i = 0; active_sent && i + 1 < clients.size(); ++i) {
        active_sent = SendFrame(clients[i], REQ_IDX_SEARCH,
                                6000 + static_cast<uint32_t>(i),
                                SearchPayload(L"stress"));
    }
    const bool shutdown_sent = !clients.empty() &&
        SendFrame(clients[0], REQ_IDX_TEST_SHUTDOWN, 0);
    const DWORD shutdown_wait = WaitForSingleObject(process.hProcess, 5000);
    Check(active_sent && shutdown_sent && shutdown_wait == WAIT_OBJECT_0,
          L"index host joins active clients and search work during shutdown");

    for (HANDLE pipe : clients) CloseHandle(pipe);
    if (shutdown_wait != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hProcess);
    std::wcout << L"\n== index host stress: " << (failures ? L"FAIL" : L"PASS") << L" ==\n";
    return failures ? 1 : 0;
}

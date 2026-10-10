#include "network_agent_listener.h"
#include "network_agent_protocol.h"
#include "network_agent_security.h"
#include "network_agent_host.h"
#include "network_index.h"
#include "../ipc/protocol.h"
#include "../ipc/deadline_pipe.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <windows.h>
#include <sddl.h>

using namespace pulse::index;
using pulse::ipc::MsgHeader;
using pulse::ipc::PayloadReader;
using pulse::ipc::PayloadWriter;
using pulse::ipc::PipeRead;
using pulse::ipc::PipeWrite;

namespace {

struct Agent {
    NetworkIndex index;
    std::atomic<bool> running{true};
    std::mutex write_mu;
    std::atomic<int> clients{0};
    std::atomic<ULONGLONG> last_activity{0};
} g;

// Every live Pulse window polls the agent once per second, so a long quiet
// period means the Pulse instance(s) that used it are gone (e.g. it was
// replaced by an update). Exit instead of lingering as an orphan forever.
constexpr ULONGLONG kIdleExitMs = 10ull * 60ull * 1000ull;

bool WriteFrame(HANDLE pipe, uint32_t type, uint32_t id,
                const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lock(g.write_mu);
    MsgHeader header = agent::MakeHeader(type, id, static_cast<uint32_t>(payload.size()));
    const auto deadline = GetTickCount64() + 5000;
    const auto stopped = [] { return !g.running.load(); };
    const bool ok = pulse::ipc::DeadlinePipeIo(pipe, reinterpret_cast<uint8_t*>(&header), sizeof(header), true, deadline, stopped) &&
        (payload.empty() || pulse::ipc::DeadlinePipeIo(pipe, const_cast<uint8_t*>(payload.data()),
                           static_cast<DWORD>(payload.size()), true, deadline, stopped));
    if (!ok) DisconnectNamedPipe(pipe);
    return ok;
}

std::vector<uint8_t> SearchPayload(const SearchResult& result) {
    PayloadWriter writer;
    writer.PutU32(static_cast<uint32_t>((std::min)(result.total, static_cast<size_t>(UINT32_MAX))));
    writer.PutU32(static_cast<uint32_t>((std::min)(result.hits.size(), static_cast<size_t>(UINT32_MAX))));
    for (const auto& hit : result.hits) {
        writer.PutString(hit.path);
        writer.PutString(hit.name);
        writer.PutU32(hit.is_dir ? 1u : 0u);
        writer.PutU32(static_cast<uint32_t>(hit.size));
        writer.PutU32(static_cast<uint32_t>(hit.size >> 32));
        writer.PutU32(static_cast<uint32_t>(hit.mtime));
        writer.PutU32(static_cast<uint32_t>(hit.mtime >> 32));
    }
    return writer.data();
}

std::vector<uint8_t> RootsPayload() {
    PayloadWriter writer;
    const auto roots = g.index.Roots();
    writer.PutU32(static_cast<uint32_t>(roots.size()));
    for (const auto& root : roots) {
        writer.PutString(root.path);
        uint32_t flags = root.online ? 1u : 0u;
        if (root.building) flags |= 2u;
        if (root.watching) flags |= 4u;
        writer.PutU32(flags);
        writer.PutU32(root.progress);
        writer.PutU32(static_cast<uint32_t>(root.indexed_items));
        writer.PutU32(static_cast<uint32_t>(root.indexed_items >> 32));
        writer.PutString(root.state);
        writer.PutString(root.error);
    }
    return writer.data();
}

std::vector<uint8_t> ResultPayload(bool ok, const std::wstring& error) {
    PayloadWriter writer;
    writer.PutU32(ok ? 1u : 0u);
    writer.PutString(error);
    return writer.data();
}

void SearchAndReply(HANDLE pipe, uint32_t id, Query query) {
    g.index.SearchAsync(query, id);
    SearchResult result;
    for (int i = 0; i < 600 && g.running; ++i) {
        if (g.index.TakeResult(id, result)) {
            WriteFrame(pipe, agent::RSP_SEARCH, id, SearchPayload(result));
            return;
        }
        Sleep(10);
    }
}

std::wstring TrackingOwner(HANDLE pipe) {
    if (!ImpersonateNamedPipeClient(pipe)) return {};
    HANDLE token = nullptr;
    const BOOL opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token);
    RevertToSelf();
    if (!opened) return {};
    DWORD bytes = 0, session = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> buffer(bytes);
    std::wstring owner;
    if (GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes) &&
        GetTokenInformation(token, TokenSessionId, &session, sizeof(session), &bytes)) {
        LPWSTR sid = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) {
            owner = sid; LocalFree(sid); owner += L"-" + std::to_wstring(session);
        }
    }
    CloseHandle(token); return owner;
}

void SetNetworkTrackingLease(HANDLE pipe, const std::wstring& owner, bool enabled) {
    static std::unordered_map<std::wstring, uint64_t> leases;
    ULONG process = 0;
    if (!GetNamedPipeClientProcessId(pipe, &process)) return;
    const auto now = ChangeTracker::Now();
    const auto prefix = owner + L":";
    const auto key = prefix + std::to_wstring(process);
    leases[key] = enabled ? now + 90 : 0;
    std::erase_if(leases, [now](const auto& entry) { return entry.second < now; });
    bool active = false;
    for (const auto& [id, expiry] : leases) if (id.starts_with(prefix)) active = true;
    g.index.SetChangeLease(owner, active);
}

bool HandleChanges(HANDLE pipe, const MsgHeader& hdr, const std::vector<uint8_t>& payload) {
    const auto owner = TrackingOwner(pipe);
    if (owner.empty()) return false;
    PayloadReader r(payload.data(), payload.size()); PayloadWriter w;
    if (hdr.type == agent::REQ_CHANGE_LEASE) {
        uint32_t enabled = 0;
        if (!r.GetU32(enabled) || enabled > 1 || r.remaining()) return false;
        SetNetworkTrackingLease(pipe, owner, enabled != 0);
        return WriteFrame(pipe, agent::RSP_CHANGE_LEASE, hdr.request_id, w.data());
    }
    ChangeResponse response;
    if (hdr.type == agent::REQ_CHANGE_SUMMARIES) {
        uint64_t since = 0; uint32_t count = 0;
        if (!r.GetU64(since) || !r.GetU32(count) || count > 256) return false;
        std::vector<std::wstring> paths;
        for (uint32_t i = 0; i < count; ++i) {
            std::wstring path;
            if (!r.GetString(path) || path.empty() || path.size() > 32767) return false;
            paths.push_back(std::move(path));
        }
        if (r.remaining()) return false;
        response = g.index.Changes().Summaries(owner, paths, since);
        w.PutU32(static_cast<uint32_t>(response.state)); w.PutU32(static_cast<uint32_t>(response.summaries.size()));
        for (auto& summary : response.summaries) {
            const auto coverage = g.index.ChangeCoverage(summary.path);
            if (coverage != ChangeState::Gap && !(coverage == ChangeState::NotCovered && summary.count)) summary.state = coverage;
            w.PutString(summary.path); w.PutU64(summary.last_change); w.PutU32(summary.count);
            w.PutU32(static_cast<uint32_t>(summary.state));
            for (auto count_kind : summary.counts) w.PutU32(count_kind);
            w.PutU32(summary.initial_count);
            w.PutU32(summary.has_deleted ? 1u : 0u); w.PutU32(summary.incomplete ? 1u : 0u);
        }
        return WriteFrame(pipe, agent::RSP_CHANGE_SUMMARIES, hdr.request_id, w.data());
    }
    std::wstring path; uint64_t since = 0, before = 0; uint32_t limit = 0, filter = 0;
    if (!r.GetString(path) || path.empty() || path.size() > 32767 || !r.GetU64(since) ||
        !r.GetU64(before) || !r.GetU32(limit) || !r.GetU32(filter) || limit == 0 || limit > 200 ||
        (filter != UINT32_MAX && filter > 5) || r.remaining()) return false;
    response = g.index.Changes().Details(owner, path, since, before, limit, filter);
    const auto coverage = g.index.ChangeCoverage(path);
    if (coverage != ChangeState::Gap && !(coverage == ChangeState::NotCovered && !response.records.empty())) response.state = coverage;
    w.PutU32(static_cast<uint32_t>(response.state)); w.PutU64(response.next_cursor);
    w.PutU32(static_cast<uint32_t>(response.records.size()));
    for (const auto& record : response.records) {
        w.PutU64(record.id); w.PutU64(record.time); w.PutU32(static_cast<uint32_t>(record.kind));
        w.PutU32(record.is_dir ? 1u : 0u); w.PutU32(static_cast<uint32_t>(record.source));
        w.PutString(record.path); w.PutString(record.old_path);
    }
    return WriteFrame(pipe, agent::RSP_CHANGE_DETAILS, hdr.request_id, w.data());
}

void ClientLoop(HANDLE pipe, const agent::Identity& owner) {
    while (g.running) {
        MsgHeader header{};
        const auto deadline = GetTickCount64() + 5000;
        const auto stopped = [] { return !g.running.load(); };
        if (!pulse::ipc::DeadlinePipeIo(pipe, reinterpret_cast<uint8_t*>(&header), sizeof(header), false, deadline, stopped) ||
            header.magic != agent::kMagic || header.payload_size > 256 * 1024)
            break;
        std::vector<uint8_t> payload(header.payload_size);
        if (!payload.empty() && !pulse::ipc::DeadlinePipeIo(pipe, payload.data(), header.payload_size, false, deadline, stopped)) break;
        if (!agent::AuthorizeClient(pipe, owner)) break;
        PayloadReader reader(payload.data(), payload.size());
        if (header.type >= agent::REQ_CHANGE_LEASE && header.type <= agent::REQ_CHANGE_DETAILS) {
            if (!HandleChanges(pipe, header, payload)) break;
        } else if (header.type == agent::REQ_ROOTS) {
            WriteFrame(pipe, agent::RSP_ROOTS, header.request_id, RootsPayload());
        } else if (header.type == agent::REQ_STATUS) {
            PayloadWriter writer;
            const auto roots = g.index.Roots();
            writer.PutU32(g.index.ConfigError().empty() ? 1u : 0u);
            uint64_t count = 0;
            for (const auto& root : roots) count += root.indexed_items;
            writer.PutU32(static_cast<uint32_t>((std::min)(count, static_cast<uint64_t>(UINT32_MAX))));
            const auto config_error = g.index.ConfigError();
            writer.PutString(!config_error.empty() ? config_error :
                roots.empty() ? L"网络索引未配置" : L"网络索引代理运行中");
            WriteFrame(pipe, agent::RSP_STATUS, header.request_id, writer.data());
        } else if (header.type == agent::REQ_SEARCH) {
            Query query;
            uint32_t flags = 0, sort = 0, limit = 0, offset = 0;
            if (!reader.GetU32(flags) || !reader.GetU32(sort) || !reader.GetU32(limit) ||
                !reader.GetU32(offset) || !reader.GetString(query.needle) ||
                !reader.GetString(query.path_prefix)) continue;
            query.rank = (flags & 1u) != 0;
            query.folders_only = (flags & 2u) != 0;
            query.sort_desc = (flags & 4u) != 0;
            query.sort = static_cast<ResultSort>(sort);
            query.limit = (std::min)(static_cast<size_t>(limit), kSearchPageCap);
            query.offset = offset;
            SearchAndReply(pipe, header.request_id, std::move(query));
        } else if (header.type == agent::REQ_ADD_ROOT || header.type == agent::REQ_REMOVE_ROOT) {
            std::wstring root, error;
            if (!reader.GetString(root)) continue;
            const bool ok = header.type == agent::REQ_ADD_ROOT
                ? g.index.AddRoot(root, &error) : g.index.RemoveRoot(root, &error);
            WriteFrame(pipe, agent::RSP_RESULT, header.request_id, ResultPayload(ok, error));
        } else if (header.type == agent::REQ_REBUILD) {
            std::wstring root;
            if (!reader.GetString(root)) root.clear();
            g.index.Rebuild(root);
            WriteFrame(pipe, agent::RSP_RESULT, header.request_id, ResultPayload(true, {}));
        }
    }
}

int RunAgent(const std::wstring& test_token = {}) {
    const auto owner = agent::ProcessIdentity();
    agent::EndpointSecurity security(owner);
    const auto suffix = test_token.empty() ? std::wstring{} : L".Test." + test_token;
    const auto pipe_name = agent::PipeName() + suffix;
    if (!owner.valid() || !security || pipe_name.empty()) return ERROR_ACCESS_DENIED;
    HANDLE singleton = CreateMutexW(security.get(), TRUE, (agent::SingletonName() + suffix).c_str());
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (singleton) CloseHandle(singleton);
        return 0;
    }
    g.index.Start(nullptr, 0, 0);
    g.last_activity = GetTickCount64();
    std::thread idle_watch([pipe_name] {
        while (g.running) {
            Sleep(1000);
            if (!g.running || g.clients.load() != 0 ||
                GetTickCount64() - g.last_activity.load() < kIdleExitMs) continue;
            g.running = false;
            // Wake the pending accept so shutdown also works without an active client.
            HANDLE wake = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                      nullptr, OPEN_EXISTING, 0, nullptr);
            if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
        }
    });
    int exit_code = 0;
    agent::PipeListener listener(pipe_name, security, true);
    while (g.running) {
        HANDLE pipe = listener.get();
        if (pipe == INVALID_HANDLE_VALUE) { exit_code = static_cast<int>(GetLastError()); break; }
        OVERLAPPED connect{};
        connect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!connect.hEvent) { exit_code = ERROR_NOT_ENOUGH_MEMORY; break; }
        BOOL connected = ConnectNamedPipe(pipe, &connect);
        if (!connected) {
            const DWORD error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) connected = TRUE;
            else if (error == ERROR_IO_PENDING) {
                DWORD transferred = 0;
                while (g.running && WaitForSingleObject(connect.hEvent, 100) == WAIT_TIMEOUT) {}
                if (!g.running) CancelIoEx(pipe, &connect);
                connected = GetOverlappedResult(pipe, &connect, &transferred, TRUE);
            }
        }
        CloseHandle(connect.hEvent);
        if (!g.running) break;
        if (connected) {
            pipe = listener.Release();
            ++g.clients;
            g.last_activity = GetTickCount64();
            ClientLoop(pipe, owner);
            // Re-arm before closing the served instance: the endpoint stays
            // reserved, and a waiting client sees PIPE_BUSY instead of
            // connecting early and timing out behind a silent peer.
            const bool rearmed = listener.Rearm();
            const DWORD rearm_error = GetLastError();
            CloseHandle(pipe);
            g.last_activity = GetTickCount64();
            --g.clients;
            if (!rearmed) { exit_code = static_cast<int>(rearm_error); break; }
        } else {
            exit_code = static_cast<int>(GetLastError());
            break;
        }
    }
    g.running = false;
    if (idle_watch.joinable()) idle_watch.join();
    listener.Close();
    g.index.Stop();
    ReleaseMutex(singleton);
    CloseHandle(singleton);
    return exit_code;
}

} // namespace

int pulse::index::RunNetworkAgent() {
    return RunAgent();
}

int pulse::index::RunNetworkAgentTest(const std::wstring& token) {
    if (token.empty() || token.size() > 64 || !std::all_of(token.begin(), token.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
               (c >= L'A' && c <= L'Z') || c == L'-' || c == L'_';
    })) return ERROR_INVALID_PARAMETER;
    return RunAgent(token);
}

#include "network_agent_protocol.h"
#include "network_agent_host.h"
#include "network_index.h"
#include "../ipc/protocol.h"
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
} g;

SECURITY_ATTRIBUTES* PipeSecurity() {
    static SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES) };
    static PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!descriptor) {
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)",
            SDDL_REVISION_1, &descriptor, nullptr);
        sa.lpSecurityDescriptor = descriptor;
    }
    return descriptor ? &sa : nullptr;
}

bool WriteFrame(HANDLE pipe, uint32_t type, uint32_t id,
                const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lock(g.write_mu);
    const MsgHeader header = agent::MakeHeader(type, id, static_cast<uint32_t>(payload.size()));
    return PipeWrite(pipe, reinterpret_cast<const uint8_t*>(&header), sizeof(header)) &&
           (payload.empty() || PipeWrite(pipe, payload.data(), static_cast<DWORD>(payload.size())));
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

void ClientLoop(HANDLE pipe) {
    while (g.running) {
        MsgHeader header{};
        if (!PipeRead(pipe, reinterpret_cast<uint8_t*>(&header), sizeof(header)) ||
            header.magic != agent::kMagic || header.payload_size > 256 * 1024)
            break;
        std::vector<uint8_t> payload(header.payload_size);
        if (!payload.empty() && !PipeRead(pipe, payload.data(), header.payload_size)) break;
        PayloadReader reader(payload.data(), payload.size());
        if (header.type >= agent::REQ_CHANGE_LEASE && header.type <= agent::REQ_CHANGE_DETAILS) {
            if (!HandleChanges(pipe, header, payload)) break;
        } else if (header.type == agent::REQ_ROOTS) {
            WriteFrame(pipe, agent::RSP_ROOTS, header.request_id, RootsPayload());
        } else if (header.type == agent::REQ_STATUS) {
            PayloadWriter writer;
            const auto roots = g.index.Roots();
            writer.PutU32(1u);
            uint64_t count = 0;
            for (const auto& root : roots) count += root.indexed_items;
            writer.PutU32(static_cast<uint32_t>((std::min)(count, static_cast<uint64_t>(UINT32_MAX))));
            writer.PutString(roots.empty() ? L"网络索引未配置" : L"网络索引代理运行中");
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
    CloseHandle(pipe);
}

int RunAgent() {
    HANDLE singleton = CreateMutexW(nullptr, TRUE, L"Local\\Pulse.Index.NetworkAgent.Singleton");
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (singleton) CloseHandle(singleton);
        return 0;
    }
    g.index.Start(nullptr, 0, 0);
    while (g.running) {
        HANDLE pipe = CreateNamedPipeW(
            agent::kPipeName, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, PipeSecurity());
        if (pipe == INVALID_HANDLE_VALUE) return static_cast<int>(GetLastError());
        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
            ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);
        if (connected) ClientLoop(pipe);
        else CloseHandle(pipe);
    }
    g.index.Stop();
    ReleaseMutex(singleton);
    CloseHandle(singleton);
    return 0;
}

} // namespace

int pulse::index::RunNetworkAgent() {
    return RunAgent();
}

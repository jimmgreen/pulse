// index_host.cpp — Pulse.Index.exe: index + query server (helper or service).
//
// UI (asInvoker) never hosts Engine::Start. This process owns MFT/USN or the
// user-folder walk, serves named-pipe PulseIndex, and writes pulse-index.bin.
//
//   Pulse.Index.exe              session helper (asInvoker, profile walk)
//   Pulse.Index.exe --service    SCM (SYSTEM → MFT+USN full disk)
//   Pulse.Index.exe --install    UAC once; creates AUTO_START service
//   Pulse.Index.exe --uninstall  removes the service
#include "index_protocol.h"
#include "../ipc/deadline_pipe.h"
#include "index_directory_security.h"
#include "index_engine.h"
#include "search_trace.h"
#include "index_service_start.h"
#include "index_config.h"
#include "../common/crash_reporter.h"
#include "../common/diagnostics_exporter.h"
#include "index_paths.h"
#include "index_path_service.h"
#include "network_agent_host.h"
#include "content_agent.h"
#include "../common/current_user_security.h"
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <winsvc.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace pulse::index;
using pulse::ipc::MsgHeader;
using pulse::ipc::PayloadReader;
using pulse::ipc::PayloadWriter;
using pulse::ipc::PipeRead;
using pulse::ipc::PipeWrite;

namespace {

constexpr UINT WM_ENGINE_NOTIFY = WM_APP + 1;
constexpr UINT WM_QUIT_HOST = WM_APP + 2;
constexpr DWORD kServiceReloadControl = 128;
constexpr UINT kIdleTimer = 1;
constexpr UINT kBroadcastTimer = 2;
constexpr UINT kIdleMs = 15000;
// Engine notifications follow USN batches, often 10+ per second on a busy
// system disk. Every broadcast wakes each client's UI thread and re-runs its
// live subscriptions, so broadcasts are coalesced: at most one per window, and
// the latest state still goes out within kBroadcastMinMs.
constexpr ULONGLONG kBroadcastMinMs = 250;
ULONGLONG g_last_broadcast = 0;
bool g_broadcast_pending = false;
constexpr DWORD kMaxClients = 16;

struct Client {
    std::atomic<HANDLE> pipe{INVALID_HANDLE_VALUE};
    std::mutex write_mu;
    std::condition_variable write_cv;
    struct Frame { MsgHeader header; std::vector<uint8_t> payload; ULONGLONG deadline; };
    std::deque<Frame> outgoing;
    size_t outgoing_bytes = 0;
    bool writing = false;
    std::thread writer;
    std::atomic<bool> alive{true};
    std::atomic<uint32_t> latest_search{0};
    struct Subscription { uint32_t id = 0; Query query; std::shared_ptr<std::atomic<uint32_t>> latest = std::make_shared<std::atomic<uint32_t>>(0); uint32_t sent_id = 0; uint64_t sent_hash = 0; };
    std::mutex subscriptions_mu;
    std::map<uint64_t, Subscription> subscriptions;
    std::atomic<bool> thread_done{false};
    std::wstring tracking_owner;
    bool tracking_lease = false;
    // #66: the caller's own token, captured from its first request, and the
    // folders already checked against it for searches (search thread only).
    HANDLE caller_token = nullptr;
    struct Visibility final : DirVisibility {
        int State(int32_t dir) const override {
            const auto found = dirs.find(dir);
            return found == dirs.end() ? -1 : (found->second ? 1 : 0);
        }
        std::unordered_map<int32_t, bool> dirs;
        uint64_t layout = 0;
        ULONGLONG since = 0;
    } visibility;
    ~Client() { if (caller_token) CloseHandle(caller_token); }
};

struct SearchTask {
    std::shared_ptr<Client> client;
    uint32_t id = 0;
    Query query;
    std::shared_ptr<std::atomic<uint32_t>> latest;
};

struct ClientWorker {
    std::shared_ptr<Client> client;
    std::thread thread;
};

struct Host {
    Engine engine;
    HWND hwnd = nullptr;
    HANDLE stop = nullptr;
    std::mutex stop_mu;
    HANDLE mutex = nullptr;
    std::atomic<bool> running{true};
    bool as_service = false;
    SERVICE_STATUS_HANDLE svc = nullptr;
    SERVICE_STATUS status{};
    std::mutex clients_mu;
    std::vector<std::shared_ptr<Client>> clients;
    std::vector<ClientWorker> client_workers;
    std::thread accept_thread;
    std::mutex search_mu;
    std::condition_variable search_cv;
    std::deque<SearchTask> search_queue;
    std::thread search_thread;
    ULONGLONG idle_since = 0;
    bool ever_client = false;
    bool test_mode = false;
    bool caller_filter_test = false;  // PULSE_INDEX_TEST_CALLER_FILTER=1 with --test-host
    std::wstring pipe_name = kPipeName;
    std::wstring mutex_name = kMutexName;
} g;

void ServiceTrace(const wchar_t* text) {
    if (!g.as_service) return;
    const std::wstring root = MachineDataRoot();
    if (root.empty()) return;
    const std::wstring path = root + L"\\index-service.log";
    HANDLE file = CreateFileW(path.c_str(),
                              FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char line[768]{};
    const int prefix = sprintf_s(line, "%04u-%02u-%02u %02u:%02u:%02u ",
                                 now.wYear, now.wMonth, now.wDay,
                                 now.wHour, now.wMinute, now.wSecond);
    if (prefix < 0) {
        CloseHandle(file);
        return;
    }
    const int body = WideCharToMultiByte(CP_UTF8, 0, text, -1,
                                         line + prefix, static_cast<int>(sizeof(line) - prefix - 3),
                                         nullptr, nullptr);
    if (body <= 1) {
        CloseHandle(file);
        return;
    }
    const int len = prefix + body - 1;
    line[len] = '\r';
    line[len + 1] = '\n';
    DWORD bytes = 0;
    WriteFile(file, line, static_cast<DWORD>(len + 2), &bytes, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

void SetSvc(DWORD state, DWORD win32 = NO_ERROR) {
    if (!g.svc) return;
    g.status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g.status.dwCurrentState = state;
    g.status.dwWin32ExitCode = win32;
    g.status.dwControlsAccepted = (state == SERVICE_RUNNING)
        ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
    g.status.dwWaitHint = (state == SERVICE_START_PENDING) ? 4000 : 0;
    SetServiceStatus(g.svc, &g.status);
}

SECURITY_ATTRIBUTES* PipeSa() {
    // The caller-filter test uses the service DACL so other callers can connect.
    if(!g.as_service && !g.caller_filter_test) {
        static pulse::CurrentUserSecurityAttributes owner;
        return owner.get();
    }
    static SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES) };
    static PSECURITY_DESCRIPTOR sd = nullptr;
    if (!sd) {
        // SYSTEM + Administrators full; Authenticated Users read/write the pipe.
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)",
            SDDL_REVISION_1, &sd, nullptr);
        sa.lpSecurityDescriptor = sd;
        sa.bInheritHandle = FALSE;
    }
    return sd ? &sa : nullptr;
}

bool ClientIo(Client& client, HANDLE pipe, uint8_t* bytes, DWORD size, bool write, ULONGLONG deadline = 0) {
    return pulse::ipc::DeadlinePipeIo(pipe, bytes, size, write, deadline, [&] {
        return !g.running || !client.alive || (g.stop && WaitForSingleObject(g.stop, 0) == WAIT_OBJECT_0);
    });
}

void StopClientLocked(Client& c) {
    c.alive = false;
    ++c.latest_search;
    c.outgoing.clear();
    c.outgoing_bytes = 0;
    const HANDLE pipe = c.pipe.load();
    if (pipe != INVALID_HANDLE_VALUE) CancelIoEx(pipe, nullptr);
    c.write_cv.notify_all();
}

void ClientWriter(const std::shared_ptr<Client>& c) {
    for (;;) {
        Client::Frame frame;
        {
            std::unique_lock lock(c->write_mu);
            c->write_cv.wait(lock, [&] { return !c->alive || !g.running || !c->outgoing.empty(); });
            if (!c->alive || !g.running) return;
            frame = std::move(c->outgoing.front());
            c->outgoing.pop_front();
            c->writing = true;
        }
        const HANDLE pipe = c->pipe.load();
        if (!ClientIo(*c, pipe, reinterpret_cast<uint8_t*>(&frame.header), sizeof(frame.header), true, frame.deadline) ||
            (!frame.payload.empty() && !ClientIo(*c, pipe, frame.payload.data(),
                static_cast<DWORD>(frame.payload.size()), true, frame.deadline))) {
            std::lock_guard lock(c->write_mu);
            StopClientLocked(*c);
            return;
        }
        {
            std::lock_guard lock(c->write_mu);
            if (!c->alive) return;
            c->outgoing_bytes -= sizeof(MsgHeader) + frame.payload.size();
            c->writing = false;
        }
    }
}

bool WriteFrame(Client& c, uint32_t type, uint32_t id, const std::vector<uint8_t>& payload) {
    // Bound queued and in-flight memory and total time, including queue wait.
    constexpr size_t kQueueBytes = kIndexMaxPayload + sizeof(MsgHeader);
    constexpr size_t kQueueFrames = 64;
    constexpr ULONGLONG kWriteDeadlineMs = 3000;
    std::lock_guard lock(c.write_mu);
    if (!g.running || !c.alive) return false;
    const size_t size = sizeof(MsgHeader) + payload.size();
    if (size > kQueueBytes || c.outgoing.size() + (c.writing ? 1u : 0u) >= kQueueFrames || c.outgoing_bytes > kQueueBytes - size) {
        StopClientLocked(c);
        return false;
    }
    c.outgoing.push_back({MakeIndexHdr(type, id, static_cast<uint32_t>(payload.size())),
        payload, GetTickCount64() + kWriteDeadlineMs});
    c.outgoing_bytes += size;
    c.write_cv.notify_one();
    return true;
}

std::vector<uint8_t> StatusPayload() {
    PayloadWriter w;
    w.PutU32(g.engine.Ready() ? 1u : 0u);
    w.PutU32(static_cast<uint32_t>((std::min)(g.engine.Count(), static_cast<size_t>(0xffffffffu))));
    w.PutString(g.engine.Status());
    w.PutU32(g.engine.PinyinReady() ? 1u : 0u);
    w.PutU64(g.engine.Revision()); w.PutU32(1);
    return w.data();
}

void QueueSearch(std::shared_ptr<Client> c, uint32_t id, Query query, bool refresh = false);
void BroadcastStatus() {
    auto payload = StatusPayload();
    std::vector<std::shared_ptr<Client>> clients;
    {
        std::lock_guard<std::mutex> lock(g.clients_mu);
        clients = g.clients;
    }
    for (auto& c : clients) {
        if (c && c->alive) {
            WriteFrame(*c, RSP_IDX_STATUS, 0, payload);
            std::vector<Client::Subscription> subscriptions;
            { std::lock_guard lock(c->subscriptions_mu); for (const auto& [id, sub] : c->subscriptions) if (sub.query.subscribe) subscriptions.push_back(sub); }
            for (const auto& sub : subscriptions) QueueSearch(c, sub.id, sub.query, true);
        }
    }
}

std::vector<uint8_t> SearchPayload(const SearchResult& sr) {
    PayloadWriter w;
    w.PutU32(static_cast<uint32_t>((std::min)(sr.total, static_cast<size_t>(0xffffffffu))));
    w.PutU32(static_cast<uint32_t>(sr.hits.size()));
    for (const auto& h : sr.hits) {
        w.PutString(h.path);
        w.PutString(h.name);
        w.PutU32(h.is_dir ? 1u : 0u);
        w.PutU32(static_cast<uint32_t>(h.size));
        w.PutU32(static_cast<uint32_t>(h.size >> 32));
        w.PutU32(static_cast<uint32_t>(h.mtime));
        w.PutU32(static_cast<uint32_t>(h.mtime >> 32));
    }
    w.PutU64(sr.revision);
    return w.data();
}

std::vector<uint8_t> VolumesPayload() {
    PayloadWriter w;
    IndexConfig config;
    if (g.as_service) LoadMachineConfig(config, nullptr);
    const auto volumes = g.engine.Volumes();
    w.PutU32(g.as_service ? 1u : 0u);
    w.PutString(DataDir());
    w.PutU32(static_cast<uint32_t>(volumes.size()));
    for (const auto& volume : volumes) {
        w.PutString(volume.id);
        w.PutString(volume.label);
        w.PutString(volume.mount_point);
        w.PutString(volume.file_system);
        w.PutString(volume.state);
        w.PutString(volume.error);
        uint32_t flags = volume.online ? 1u : 0u;
        if (volume.supported) flags |= 2u;
        if (volume.enabled) flags |= 4u;
        w.PutU32(flags);
        w.PutU32(static_cast<uint32_t>(volume.kind));
        w.PutU32(volume.progress);
        w.PutU32(static_cast<uint32_t>(volume.indexed_items));
        w.PutU32(static_cast<uint32_t>(volume.indexed_items >> 32));
    }
    if (g.as_service) {
        // Sent after the volume rows so newer clients receive the exclusion list.
        w.PutU32(static_cast<uint32_t>(config.excluded_paths.size()));
        for (const auto& path : config.excluded_paths) w.PutString(path);
        // Appended for clients that show the system folder switch; older
        // clients stop reading after the user list.
        w.PutU32(config.exclude_system ? 1u : 0u);
        w.PutU32(static_cast<uint32_t>(config.system_groups.size()));
        for (const auto& group : config.system_groups) w.PutString(group);
        const auto system_paths = SystemExclusionPaths(config);
        w.PutU32(static_cast<uint32_t>(system_paths.size()));
        for (const auto& path : system_paths) w.PutString(path);
    } else {
        w.PutU32(0);
    }
    return w.data();
}

bool ParseQuery(const uint8_t* p, size_t n, Query& q) {
    PayloadReader r(p, n);
    uint32_t flags = 0, sort = 0, limit = 0, offset = 0;
    if (!r.GetU32(flags) || !r.GetU32(sort) || !r.GetU32(limit) || !r.GetU32(offset) ||
        !r.GetString(q.needle) || !r.GetString(q.path_prefix))
        return false;
    if (sort > static_cast<uint32_t>(ResultSort::Mtime) ||
        q.needle.size() > 4096 || q.path_prefix.size() > 32768)
        return false;
    q.rank = (flags & 1) != 0;
    q.folders_only = (flags & 2) != 0;
    q.sort_desc = (flags & 4) != 0;
    q.subscribe = (flags & 8) != 0;
    if (r.remaining() && !r.GetU64(q.session_id)) return false;
    q.sort = static_cast<ResultSort>(sort);
    q.limit = limit;
    q.offset = offset;
    if (q.limit > kSearchPageCap) q.limit = kSearchPageCap;
    return true;
}

void QueueSearch(std::shared_ptr<Client> c, uint32_t id, Query query, bool refresh) {
    std::shared_ptr<std::atomic<uint32_t>> latest;
    {
        std::lock_guard lock(c->subscriptions_mu);
        auto& subscription = c->subscriptions[query.session_id];
        if (refresh && subscription.id != id) return;
        subscription.id = id; subscription.query = query; latest = subscription.latest; *latest = id;
    }
    std::lock_guard<std::mutex> lock(g.search_mu);
    g.search_queue.erase(
        std::remove_if(g.search_queue.begin(), g.search_queue.end(),
            [&](const SearchTask& task) { return task.client == c && task.query.session_id == query.session_id; }),
        g.search_queue.end());
    g.search_queue.push_back(SearchTask{std::move(c), id, std::move(query), std::move(latest)});
    TraceSearch("filename_query_queued", g.engine.Revision());
    g.search_cv.notify_one();
}

// #66: the service reads the raw MFT as SYSTEM. Without a filter any signed-in
// user could search names inside other profiles. A name is served only when
// the caller could open its folder for listing with the caller's own token.
constexpr ULONGLONG kVisibilityTtlMs = 10 * 60 * 1000;  // picks up ACL edits

bool CallerFilter() { return g.as_service || g.caller_filter_test; }

HANDLE CaptureCallerToken(HANDLE pipe) {
    if (!ImpersonateNamedPipeClient(pipe)) return nullptr;
    HANDLE token = nullptr;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_DUPLICATE,
                         TRUE, &token))
        token = nullptr;
    RevertToSelf();
    return token;
}

bool CallerCanList(HANDLE token, std::wstring dir) {
    if (!token || dir.empty()) return false;
    while (dir.size() > 3 && dir.back() == L'\\') dir.pop_back();
    if (dir.size() == 2 && dir[1] == L':') dir += L'\\';
    if (dir.size() >= MAX_PATH && dir.rfind(L"\\\\", 0) != 0) dir.insert(0, L"\\\\?\\");
    if (!SetThreadToken(nullptr, token)) return false;
    const HANDLE h = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    RevertToSelf();
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

std::wstring ParentDir(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return path;
    std::wstring dir = path.substr(0, slash);
    if (dir.size() == 2 && dir[1] == L':') dir += L'\\';
    return dir;
}

// Path-based checks, memoised for one request (feeds, journals, sizes).
struct ListCheck {
    HANDLE token = nullptr;
    std::unordered_map<std::wstring, bool> memo;
    bool Dir(const std::wstring& dir) {
        if (!CallerFilter()) return true;
        auto [found, added] = memo.try_emplace(dir, false);
        if (added) found->second = CallerCanList(token, dir);
        return found->second;
    }
    bool Record(const ChangeRecord& record) {
        return Dir(ParentDir(record.path)) &&
               (record.old_path.empty() || Dir(ParentDir(record.old_path)));
    }
};

// A broad first query can name tens of thousands of folders; each check is
// one kernel open, so they are spread over a few threads.
void CheckDirs(Client& c, const SearchTask& task,
               const std::vector<std::pair<int32_t, std::wstring>>& dirs) {
    std::vector<int8_t> state(dirs.size(), -1);
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t i; (i = next.fetch_add(1)) < dirs.size();) {
            if (!c.alive || task.latest->load() != task.id) return;
            state[i] = CallerCanList(c.caller_token, dirs[i].second) ? 1 : 0;
        }
    };
    const unsigned threads = dirs.size() < 128 ? 1u
        : (std::min)(8u, (std::max)(2u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < threads; ++i) pool.emplace_back(work);
    work();
    for (auto& thread : pool) thread.join();
    for (size_t i = 0; i < dirs.size(); ++i)
        if (state[i] >= 0) c.visibility.dirs[dirs[i].first] = state[i] != 0;
}

SearchResult SearchForCaller(Client& c, const SearchTask& task) {
    if (!CallerFilter()) return g.engine.Search(task.query, task.latest.get(), task.id);
    auto& vis = c.visibility;
    const ULONGLONG now = GetTickCount64();
    if (!vis.since || now - vis.since > kVisibilityTtlMs) { vis.dirs.clear(); vis.since = now; }
    for (int round = 0; round < 4; ++round) {
        SearchResult sr = g.engine.Search(task.query, task.latest.get(), task.id, &vis);
        if (sr.layout && sr.layout != vis.layout) {
            // Node ids were renumbered, so earlier answers describe other folders.
            const bool stale = !vis.dirs.empty();
            vis.dirs.clear();
            vis.layout = sr.layout;
            if (stale) continue;
        }
        if (sr.unchecked_dirs.empty()) return sr;
        CheckDirs(c, task, sr.unchecked_dirs);
        if (!c.alive || task.latest->load() != task.id) return {};
    }
    return {};
}

void SearchThread() {
    for (;;) {
        SearchTask task;
        {
            std::unique_lock<std::mutex> lock(g.search_mu);
            g.search_cv.wait(lock, [] { return !g.running || !g.search_queue.empty(); });
            if (!g.running) {
                g.search_queue.clear();
                return;
            }
            task = std::move(g.search_queue.front());
            g.search_queue.pop_front();
        }
        auto& c = task.client;
        if (!c || !c->alive || task.latest->load() != task.id) continue;
        TraceSearch("filename_query_begin", g.engine.Revision());
        SearchResult sr = SearchForCaller(*c, task);
        sr.revision = g.engine.Revision();
        TraceSearch("filename_query_done", sr.revision);
        if (!c->alive || task.latest->load() != task.id) continue;
        auto out = SearchPayload(sr);
        if (out.size() > kIndexMaxPayload) {
            sr.hits.clear();
            out = SearchPayload(sr);
        }
        if (task.query.subscribe) {
            // Live refreshes rerun on every engine notification. Rows identical
            // to the last page sent for this request are not pushed again (the
            // trailing revision is excluded from the comparison).
            uint64_t hash = 1469598103934665603ull;
            const size_t hashed = out.size() >= 8 ? out.size() - 8 : out.size();
            for (size_t i = 0; i < hashed; ++i) { hash ^= out[i]; hash *= 1099511628211ull; }
            std::lock_guard lock(c->subscriptions_mu);
            auto found = c->subscriptions.find(task.query.session_id);
            if (found != c->subscriptions.end() && found->second.id == task.id) {
                if (found->second.sent_id == task.id && found->second.sent_hash == hash) continue;
                found->second.sent_id = task.id;
                found->second.sent_hash = hash;
            }
        }
        WriteFrame(*c, RSP_IDX_SEARCH, task.id, out);
    }
}

void SetTrackingLease(Client& c, const std::wstring& owner, bool enabled) {
    std::lock_guard lock(g.clients_mu);
    c.tracking_owner = owner; c.tracking_lease = enabled;
    bool active = enabled;
    for (const auto& other : g.clients)
        if (other.get() != &c && other->tracking_lease && other->tracking_owner == owner) active = true;
    g.engine.SetChangeLease(owner, active);
}

void DropClient(const std::shared_ptr<Client>& c) {
    if (!c) return;
    if (!c->tracking_owner.empty()) SetTrackingLease(*c, c->tracking_owner, false);
    {
        std::lock_guard lock(c->write_mu);
        StopClientLocked(*c);
    }
    {
        std::lock_guard lock(c->subscriptions_mu);
        for (auto& [session, subscription] : c->subscriptions) ++*subscription.latest;
        c->subscriptions.clear();
    }
    if (c->writer.joinable()) c->writer.join();
    {
        std::lock_guard lock(c->write_mu);
        const HANDLE pipe = c->pipe.exchange(INVALID_HANDLE_VALUE);
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    }
    std::lock_guard<std::mutex> lock(g.clients_mu);
    g.clients.erase(std::remove(g.clients.begin(), g.clients.end(), c), g.clients.end());
    if (g.clients.empty()) g.idle_since = GetTickCount64();
}

// Bind journals to the authenticated pipe token, never a caller supplied name.
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

bool HandleChanges(Client& client, const MsgHeader& hdr, const std::vector<uint8_t>& payload) {
    const auto owner = TrackingOwner(client.pipe.load());
    if (owner.empty()) return false;
    PayloadReader r(payload.data(), payload.size()); PayloadWriter w;
    if (hdr.type == REQ_IDX_CHANGE_LEASE) {
        uint32_t enabled = 0;
        if (!r.GetU32(enabled) || enabled > 1 || r.remaining()) return false;
        SetTrackingLease(client, owner, enabled != 0);
        return WriteFrame(client, RSP_IDX_CHANGE_LEASE, hdr.request_id, w.data());
    }
    ChangeResponse response;
    if (hdr.type == REQ_IDX_CHANGE_SUMMARIES) {
        uint64_t since = 0; uint32_t count = 0;
        if (!r.GetU64(since) || !r.GetU32(count) || count > 256) return false;
        std::vector<std::wstring> paths;
        for (uint32_t i = 0; i < count; ++i) {
            std::wstring path;
            if (!r.GetString(path) || path.empty() || path.size() > 32767) return false;
            paths.push_back(std::move(path));
        }
        if (r.remaining()) return false;
        response = g.engine.Changes().Summaries(owner, paths, since);
        w.PutU32(static_cast<uint32_t>(response.state)); w.PutU32(static_cast<uint32_t>(response.summaries.size()));
        ListCheck list{client.caller_token};  // #66
        for (auto& summary : response.summaries) {
            if (!list.Dir(summary.path)) {
                ChangeSummary hidden;
                hidden.path = summary.path;
                summary = std::move(hidden);
            } else {
                const auto coverage = g.engine.ChangeCoverage(summary.path);
                if (coverage != ChangeState::Gap && !(coverage == ChangeState::NotCovered && summary.count)) summary.state = coverage;
            }
            w.PutString(summary.path); w.PutU64(summary.last_change); w.PutU32(summary.count);
            w.PutU32(static_cast<uint32_t>(summary.state));
            for (auto count_kind : summary.counts) w.PutU32(count_kind);
            w.PutU32(summary.initial_count);
            w.PutU32(summary.has_deleted ? 1u : 0u); w.PutU32(summary.incomplete ? 1u : 0u);
        }
        return WriteFrame(client, RSP_IDX_CHANGE_SUMMARIES, hdr.request_id, w.data());
    }
    std::wstring path; uint64_t since = 0, before = 0; uint32_t limit = 0, filter = 0;
    if (!r.GetString(path) || path.empty() || path.size() > 32767 || !r.GetU64(since) ||
        !r.GetU64(before) || !r.GetU32(limit) || !r.GetU32(filter) || limit == 0 || limit > 200 ||
        (filter != UINT32_MAX && filter > 5) || r.remaining()) return false;
    response = g.engine.Changes().Details(owner, path, since, before, limit, filter);
    const auto coverage = g.engine.ChangeCoverage(path);
    if (coverage != ChangeState::Gap && !(coverage == ChangeState::NotCovered && !response.records.empty())) response.state = coverage;
    ListCheck list{client.caller_token};  // #66
    if (!list.Dir(path)) {
        response.records.clear();
        response.state = ChangeState::Unavailable;
    } else {
        std::erase_if(response.records, [&](const ChangeRecord& record) { return !list.Record(record); });
    }
    w.PutU32(static_cast<uint32_t>(response.state)); w.PutU64(response.next_cursor);
    w.PutU32(static_cast<uint32_t>(response.records.size()));
    for (const auto& record : response.records) {
        w.PutU64(record.id); w.PutU64(record.time); w.PutU32(static_cast<uint32_t>(record.kind));
        w.PutU32(record.is_dir ? 1u : 0u); w.PutU32(static_cast<uint32_t>(record.source));
        w.PutString(record.path); w.PutString(record.old_path);
    }
    return WriteFrame(client, RSP_IDX_CHANGE_DETAILS, hdr.request_id, w.data());
}

void ClientThread(std::shared_ptr<Client> c) {
    while (g.running && c->alive) {
        MsgHeader hdr{};
        std::vector<uint8_t> payload;
        const HANDLE pipe = c->pipe.load();
        if (pipe == INVALID_HANDLE_VALUE ||
            !ClientIo(*c, pipe, reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr), false))
            break;
        if (hdr.magic != kIndexMagic || hdr.payload_size > kIndexMaxRequestPayload) break;
        payload.resize(hdr.payload_size);
        if (hdr.payload_size &&
            !ClientIo(*c, pipe, payload.data(), hdr.payload_size, false))
            break;
        // Impersonation needs a completed read; searches are queued only after this.
        if (!c->caller_token && CallerFilter()) c->caller_token = CaptureCallerToken(pipe);
        if(hdr.type==8) {
            PayloadReader reader(payload.data(),payload.size());uint64_t session=0;
            if(!reader.GetU64(session)) break;
            std::lock_guard lock(c->subscriptions_mu);
            if(auto found=c->subscriptions.find(session);found!=c->subscriptions.end()) {++*found->second.latest;c->subscriptions.erase(found);}
        } else if (hdr.type == kFolderSizeRequest) {
            PayloadReader reader(payload.data(), payload.size());
            uint32_t version = 0, count = 0;
            if (!reader.GetU32(version) || version != 1 || !reader.GetU32(count) || !count || count > kFolderSizeBatch) break;
            std::vector<std::wstring> paths;
            bool valid = true;
            for (uint32_t i = 0; i < count; ++i) {
                std::wstring path;
                if (!reader.GetString(path) || path.empty() || path.size() > 32768) { valid = false; break; }
                paths.push_back(std::move(path));
            }
            if (!valid || reader.remaining()) break;
            auto sizes = g.engine.FolderSizes(paths);
            ListCheck list{c->caller_token};  // #66
            for (size_t i = 0; i < sizes.size() && i < paths.size(); ++i)
                if (!list.Dir(paths[i])) sizes[i] = IndexedFolderSize{};
            PayloadWriter writer; PutFolderSizes(writer, sizes);
            if (!WriteFrame(*c, kFolderSizeResponse, hdr.request_id, writer.data())) break;
        } else if (hdr.type == kFeedRequest) {
            PayloadReader reader(payload.data(),payload.size()); uint32_t version=0,changes=0;
            std::wstring root; uint64_t epoch=0,cursor=0;
            if(!reader.GetU32(version)||version!=1||!reader.GetU32(changes)||changes>1||!reader.GetString(root)||root.size()>32768||!reader.GetU64(epoch)||!reader.GetU64(cursor)) break;
            auto page=g.engine.ReadFeed(changes!=0,root,epoch,cursor);
            if(changes && page.ready && !page.gap && page.records.empty() && epoch) {
                for(unsigned i=0;i<10 && g.running && c->alive;++i) {
                    if(WaitForSingleObject(g.stop,10)==WAIT_OBJECT_0) break;
                    page=g.engine.ReadFeed(true,root,epoch,cursor);
                    if(page.gap || !page.records.empty()) break;
                }
            }
            ListCheck list{c->caller_token};  // #66
            if (!root.empty() && !list.Dir(root)) page.records.clear();
            else std::erase_if(page.records, [&](const ChangeRecord& record) { return !list.Record(record); });
            PayloadWriter writer;PutFeedPage(writer,page);
            if(!WriteFrame(*c,kFeedResponse,hdr.request_id,writer.data())) break;
        } else if (hdr.type >= REQ_IDX_CHANGE_LEASE && hdr.type <= REQ_IDX_CHANGE_DETAILS) {
            if (!HandleChanges(*c, hdr, payload)) break;
        } else if (hdr.type == REQ_IDX_STATUS) {
            WriteFrame(*c, RSP_IDX_STATUS, hdr.request_id, StatusPayload());
        } else if (hdr.type == REQ_IDX_VOLUMES) {
            WriteFrame(*c, RSP_IDX_VOLUMES, hdr.request_id, VolumesPayload());
        } else if (hdr.type == REQ_IDX_SEARCH) {
            const uint32_t id = hdr.request_id;
            c->latest_search.store(id);
            Query query;
            if (!ParseQuery(payload.data(), payload.size(), query)) break;
            QueueSearch(c, id, std::move(query));
        } else if (hdr.type == REQ_IDX_TEST_SHUTDOWN && g.test_mode) {
            TraceSearch("filename_shutdown_requested");
            g.running = false;
            if (g.stop) SetEvent(g.stop);
            PostMessageW(g.hwnd, WM_QUIT_HOST, 0, 0);
            break;
        }
    }
    DropClient(c);
    c->thread_done = true;
}

void ReapClientWorkers() {
    for (auto it = g.client_workers.begin(); it != g.client_workers.end();) {
        if (!it->client->thread_done) {
            ++it;
            continue;
        }
        if (it->thread.joinable()) it->thread.join();
        it = g.client_workers.erase(it);
    }
}

void AcceptLoop() {
    bool first = true;
    int first_failures = 0;
    while (g.running && (!g.stop || WaitForSingleObject(g.stop, 0) != WAIT_OBJECT_0)) {
        DWORD flags = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
        if (first) flags |= FILE_FLAG_FIRST_PIPE_INSTANCE;
        HANDLE h = CreateNamedPipeW(
            g.pipe_name.c_str(), flags,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            kMaxClients, 64 * 1024, 64 * 1024, 0, PipeSa());
        if (h == INVALID_HANDLE_VALUE) {
            if (first) {
                // A leftover user-mode helper may still own the pipe after an
                // upgrade. Service hosts retry briefly instead of exiting; the
                // UI helper still yields immediately.
                ++first_failures;
                if (g.as_service && first_failures < 40) {
                    ServiceTrace(L"pipe first-instance busy, retrying");
                    Sleep(250);
                    if (g.stop && WaitForSingleObject(g.stop, 0) == WAIT_OBJECT_0) break;
                    continue;
                }
                ServiceTrace(L"pipe first-instance unavailable, quitting");
                if (g.hwnd) PostMessageW(g.hwnd, WM_QUIT_HOST, 0, 0);
                break;
            }
            Sleep(200);
            if (g.stop && WaitForSingleObject(g.stop, 0) == WAIT_OBJECT_0) break;
            continue;
        }
        first = false;
        first_failures = 0;
        OVERLAPPED ol{};
        ol.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL pending = ConnectNamedPipe(h, &ol) ? FALSE
            : (GetLastError() == ERROR_IO_PENDING);
        if (GetLastError() == ERROR_PIPE_CONNECTED) pending = FALSE;
        if (pending) {
            HANDLE waits[2] = { ol.hEvent, g.stop };
            DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            DWORD dummy = 0;
            if (wr != WAIT_OBJECT_0 || !GetOverlappedResult(h, &ol, &dummy, FALSE)) {
                CancelIoEx(h, &ol);
                GetOverlappedResult(h, &ol, &dummy, TRUE);
                CloseHandle(ol.hEvent);
                CloseHandle(h);
                break;
            }
        }
        CloseHandle(ol.hEvent);
        if (!g.running) {
            CloseHandle(h);
            break;
        }
        ReapClientWorkers();
        auto c = std::make_shared<Client>();
        c->pipe.store(h);
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(g.clients_mu);
            if (g.clients.size() < kMaxClients) {
                g.clients.push_back(c);
                g.idle_since = 0;
                g.ever_client = true;
                accepted = true;
            }
        }
        if (!accepted) {
            CloseHandle(h);
            c->pipe = INVALID_HANDLE_VALUE;
            continue;
        }
        c->writer = std::thread(ClientWriter, c);
        g.client_workers.push_back(ClientWorker{c, std::thread(ClientThread, c)});
        WriteFrame(*c, RSP_IDX_STATUS, 0, StatusPayload());
    }
}

// UI thread of the host window only; the timer carries a deferred broadcast.
void ScheduleBroadcast(HWND hwnd) {
    if (g_broadcast_pending) return;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG since = now - g_last_broadcast;
    if (g_last_broadcast == 0 || since >= kBroadcastMinMs ||
        !SetTimer(hwnd, kBroadcastTimer, static_cast<UINT>(kBroadcastMinMs - since), nullptr)) {
        g_last_broadcast = now;
        BroadcastStatus();
        return;
    }
    g_broadcast_pending = true;
}

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ENGINE_NOTIFY) {
        ScheduleBroadcast(hwnd);
        return 0;
    }
    if (msg == WM_TIMER && wParam == kBroadcastTimer) {
        KillTimer(hwnd, kBroadcastTimer);
        g_broadcast_pending = false;
        g_last_broadcast = GetTickCount64();
        BroadcastStatus();
        return 0;
    }
    if (msg == WM_TIMER && wParam == kIdleTimer) {
        if (!g.as_service && g.running) {
            std::lock_guard<std::mutex> lock(g.clients_mu);
            if (g.ever_client && g.clients.empty() && g.idle_since &&
                GetTickCount64() - g.idle_since >= kIdleMs) {
                PostMessageW(hwnd, WM_QUIT_HOST, 0, 0);
            }
        }
        return 0;
    }
    if (msg == WM_QUIT_HOST) {
        g.running = false;
        if (g.stop) SetEvent(g.stop);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND CreateMsgWindow() {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = HostWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseIndexHost";
    RegisterClassExW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
}

int RunHost(bool as_service, bool test_mode = false,
            std::wstring pipe_name = kPipeName,
            std::wstring mutex_name = kMutexName, std::wstring fixture_root = {}) {
    g.as_service = as_service;
    g.test_mode = test_mode;
    wchar_t caller_filter[4]{};
    g.caller_filter_test = test_mode &&
        GetEnvironmentVariableW(L"PULSE_INDEX_TEST_CALLER_FILTER", caller_filter, 4) == 1 &&
        caller_filter[0] == L'1';
    g.pipe_name = std::move(pipe_name);
    g.mutex_name = std::move(mutex_name);
    ServiceTrace(L"RunHost entered");
    SetMachineIndexScope(as_service);
    PrivateIndexDirectoryLock index_directory_lock;
    if (as_service) {
        (void)MachineDataRoot();
        IndexConfig config;
        if (!LoadMachineConfig(config, nullptr)) {
            ServiceTrace(L"Cannot load index directory configuration");
            SetSvc(SERVICE_STOPPED, ERROR_INVALID_DATA);
            return ERROR_INVALID_DATA;
        }
        if (!CreateDirectoryW(config.index_path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            const DWORD failure = GetLastError();
            SetSvc(SERVICE_STOPPED, failure);
            return static_cast<int>(failure);
        }
        // #66: index files list every name on the volumes. The default folder
        // is repaired on every start; a chosen one only while it is empty.
        if (CompareStringOrdinal(config.index_path.c_str(), -1, (MachineDataRoot() + L"\\Index").c_str(), -1,
                                 TRUE) == CSTR_EQUAL)
            (void)MachineIndexRoot();
        const bool pinned = index_directory_lock.Acquire(std::filesystem::path(config.index_path));
        bool private_directory = pinned && ProtectIndexDirectory(config.index_path);
        if (pinned && !private_directory && AdoptIndexDirectory(std::filesystem::path(config.index_path))) {
            ServiceTrace(L"Index directory left readable by an older release was made private");
            private_directory = ProtectIndexDirectory(config.index_path);
        }
        if (!private_directory) {
            ServiceTrace(L"Index directory is not private; refusing to expose service metadata");
            SetSvc(SERVICE_STOPPED, kIndexDirectoryNotPrivate);
            return static_cast<int>(kIndexDirectoryNotPrivate);
        }
        SetActiveIndexDirectory(config.index_path);
        const std::wstring probe = config.index_path + L"\\.pulse-write-check-" +
            std::to_wstring(GetCurrentProcessId());
        HANDLE check = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (check == INVALID_HANDLE_VALUE) {
            const DWORD failure = GetLastError();
            ServiceTrace(L"Index directory is not writable by the service");
            SetSvc(SERVICE_STOPPED, failure);
            return static_cast<int>(failure);
        }
        CloseHandle(check);
    }
    g.running = true;
    g.idle_since = GetTickCount64();

    if (!as_service) {
        g.mutex = CreateMutexW(nullptr, TRUE, g.mutex_name.c_str());
        if (!g.mutex) return 1;
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(g.mutex);
            g.mutex = nullptr;
            return 0;
        }
    }

    g.hwnd = CreateMsgWindow();
    if (!g.hwnd) {
        ServiceTrace(L"CreateMsgWindow failed");
        if (as_service) SetSvc(SERVICE_STOPPED, GetLastError());
        return 1;
    }
    ServiceTrace(L"message window ready");
    g.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (as_service) SetSvc(SERVICE_RUNNING);
    ServiceTrace(L"service running reported");
    if (!fixture_root.empty()) {
        g.engine.StartFixture(g.hwnd, WM_ENGINE_NOTIFY, std::move(fixture_root));
    } else if (g.test_mode) {
        g.engine.AddForTest(L"C:\\PulseIndexStress\\.codex", L".codex", true);
        for (uint32_t i = 0; i < 50000; ++i) {
            const std::wstring name = L"stress-item-" + std::to_wstring(i) + L".txt";
            g.engine.AddForTest(L"C:\\PulseIndexStress\\" + name, name, false,
                                i * 17ull, i);
        }
    } else {
        g.engine.Start(g.hwnd, WM_ENGINE_NOTIFY);
    }
    ServiceTrace(L"engine thread started");
    SetTimer(g.hwnd, kIdleTimer, 1000, nullptr);
    g.search_thread = std::thread(SearchThread);
    g.accept_thread = std::thread(AcceptLoop);
    ServiceTrace(L"accept thread started");

    MSG msg{};
    while (g.running) {
        const DWORD wait = MsgWaitForMultipleObjects(1, &g.stop, FALSE, INFINITE, QS_ALLINPUT);
        if (wait != WAIT_OBJECT_0 + 1) break;
        while (g.running && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g.running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    ServiceTrace(L"message loop exited");
    TraceSearch("filename_shutdown_message_loop_done");

    g.running = false;
    g.engine.RequestStop();
    if (g.stop) SetEvent(g.stop);
    {
        std::lock_guard<std::mutex> lock(g.clients_mu);
        for (auto& c : g.clients) {
            if (!c) continue;
            std::lock_guard write_lock(c->write_mu);
            StopClientLocked(*c);
        }
    }
    g.search_cv.notify_all();
    ServiceTrace(L"waiting for pipe workers to stop");
    if (g.accept_thread.joinable()) g.accept_thread.join();
    for (auto& worker : g.client_workers)
        if (worker.thread.joinable()) worker.thread.join();
    g.client_workers.clear();
    if (g.search_thread.joinable()) g.search_thread.join();
    TraceSearch("filename_shutdown_clients_done");
    ServiceTrace(L"pipe and search workers stopped; stopping engine");
    g.engine.Stop();
    TraceSearch("filename_shutdown_engine_done");
    ServiceTrace(L"engine stopped");
    if (g.mutex) {
        ReleaseMutex(g.mutex);
        CloseHandle(g.mutex);
        g.mutex = nullptr;
    }
    {
        std::lock_guard lock(g.stop_mu);
        if (g.stop) CloseHandle(g.stop);
        g.stop = nullptr;
    }
    if (as_service) SetSvc(SERVICE_STOPPED);
    return 0;
}

DWORD WINAPI SvcCtrl(DWORD ctrl, DWORD, LPVOID, LPVOID) {
    if (ctrl == kServiceReloadControl) {
        g.engine.RequestRebuild();
        return NO_ERROR;
    }
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        SetSvc(SERVICE_STOP_PENDING);
        g.running = false;
        {
            std::lock_guard lock(g.stop_mu);
            if (g.stop) SetEvent(g.stop);
        }
        g.search_cv.notify_all();
    }
    return NO_ERROR;
}

VOID WINAPI SvcMain(DWORD, LPWSTR*) {
    g.svc = RegisterServiceCtrlHandlerExW(kServiceName, SvcCtrl, nullptr);
    SetSvc(SERVICE_START_PENDING);
    RunHost(true);
}

std::wstring SelfPath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    return path;
}

// A host that dies (crash, hang ended by setup) or quits with an error must
// not leave search waiting until someone reinstalls the service:
//  - SCM restarts it. The first delay is long on purpose: setup ends a host
//    that does not stop and then replaces the files; an early restart would
//    run the old binary, and --install would find it running and keep it.
//  - Interactive users may start it, so Pulse can bring back a stopped host
//    (IndexClient). Everything else matches the default service DACL.
void ConfigureServiceRecovery(SC_HANDLE svc) {
    SC_ACTION actions[3] = {{SC_ACTION_RESTART, 60000}, {SC_ACTION_RESTART, 120000}, {SC_ACTION_RESTART, 300000}};
    SERVICE_FAILURE_ACTIONSW failure{};
    failure.dwResetPeriod = 24 * 60 * 60;
    failure.cActions = ARRAYSIZE(actions);
    failure.lpsaActions = actions;
    if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &failure))
        ServiceTrace(L"failure actions not set");
    SERVICE_FAILURE_ACTIONS_FLAG flag{TRUE};
    if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag))
        ServiceTrace(L"failure actions flag not set");
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)"
            L"(A;;CCLCSWRPLOCRRC;;;IU)(A;;CCLCSWLOCRRC;;;SU)",
            SDDL_REVISION_1, &sd, nullptr)) {
        if (!SetServiceObjectSecurity(svc, DACL_SECURITY_INFORMATION, sd))
            ServiceTrace(L"service DACL not set");
        LocalFree(sd);
    }
}

int InstallService() {
    SetMachineIndexScope(true);
    // Repair ProgramData ACLs on every install so older SY/BA-only trees become
    // readable again for interactive admins and Authenticated Users.
    (void)MachineDataRoot();
    (void)MachineIndexRoot();
    IndexConfig config;
    LoadMachineConfig(config, nullptr);
    SaveMachineConfig(config, nullptr);
    const std::wstring bin = L"\"" + SelfPath() + L"\" --service";

    DWORD last_err = ERROR_GEN_FAILURE;
    for (int attempt = 0; attempt < 40; ++attempt) {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
        if (!scm) return static_cast<int>(GetLastError());

        SC_HANDLE svc = OpenServiceW(scm, kServiceName,
                                     SERVICE_START | SERVICE_QUERY_STATUS |
                                         SERVICE_CHANGE_CONFIG | READ_CONTROL | WRITE_DAC);
        if (!svc && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
            svc = CreateServiceW(scm, kServiceName, L"Pulse Index",
                                 SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                 SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                 bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
        }
        if (!svc) {
            last_err = GetLastError();
            CloseServiceHandle(scm);
            if (last_err != ERROR_SERVICE_MARKED_FOR_DELETE) break;
            Sleep(250);
            continue;
        }

        if (!ChangeServiceConfigW(svc, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                                  SERVICE_ERROR_NORMAL, bin.c_str(), nullptr, nullptr,
                                  nullptr, nullptr, nullptr, L"Pulse Index")) {
            last_err = GetLastError();
        } else {
            SERVICE_DESCRIPTIONW desc{};
            wchar_t text[] = L"Pulse file-name index (MFT + USN). UI talks to this over a named pipe.";
            desc.lpDescription = text;
            ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
            ConfigureServiceRecovery(svc);
            last_err = EnsureServiceRunning(
                [&](SERVICE_STATUS_PROCESS& status) -> DWORD {
                    DWORD needed = 0;
                    return QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                        reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed)
                        ? ERROR_SUCCESS : GetLastError();
                },
                [&]() -> DWORD {
                    return StartServiceW(svc, 0, nullptr) ? ERROR_SUCCESS : GetLastError();
                },
                [](DWORD delay) { Sleep(delay); });
        }
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        if (last_err != ERROR_SERVICE_MARKED_FOR_DELETE) break;
        Sleep(250);
    }
    return static_cast<int>(last_err);
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation,
                                               sizeof(elevation), &bytes) &&
                          elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

bool ReloadService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_USER_DEFINED_CONTROL);
    if (!svc) {
        CloseServiceHandle(scm);
        return false;
    }
    SERVICE_STATUS status{};
    const bool ok = ControlService(svc, kServiceReloadControl, &status) != FALSE;
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

int ConfigureCommand(const std::vector<std::wstring>& args) {
    if (!IsElevated()) return ERROR_ELEVATION_REQUIRED;
    struct ConfigurationLock {
        HANDLE handle = CreateMutexW(nullptr, FALSE, L"Global\\PulseIndexConfiguration");
        bool locked = false;
        ~ConfigurationLock() {
            if (locked) ReleaseMutex(handle);
            if (handle) CloseHandle(handle);
        }
    } configuration_lock;
    if (!configuration_lock.handle) return static_cast<int>(GetLastError());
    const DWORD wait = WaitForSingleObject(configuration_lock.handle, 0);
    configuration_lock.locked = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    if (!configuration_lock.locked) return ERROR_BUSY;
    SetMachineIndexScope(true);
    std::wstring error;
    bool ok = false;
    if (args.size() >= 3 && args[1] == L"--configure-volume") {
        const bool enabled = args.size() >= 4 && args[3] == L"--enable";
        const bool disabled = args.size() >= 4 && args[3] == L"--disable";
        if (!enabled && !disabled) return ERROR_INVALID_PARAMETER;
        ok = ConfigureVolume(args[2], enabled, &error);
    } else if (args.size() >= 3 && args[1] == L"--set-index-path") {
        return ConfigureServiceIndexPath(args[2]);
    } else if (args.size() >= 3 && args[1] == L"--configure-exclude") {
        const bool enabled = args.size() >= 4 && args[3] == L"--enable";
        const bool disabled = args.size() >= 4 && args[3] == L"--disable";
        if (!enabled && !disabled) return ERROR_INVALID_PARAMETER;
        ok = ConfigureExcludePath(args[2], enabled, &error);
    } else if (args.size() >= 3 && args[1] == L"--configure-system-exclusion") {
        const bool enabled = args.size() >= 4 && args[3] == L"--enable";
        const bool disabled = args.size() >= 4 && args[3] == L"--disable";
        if (!enabled && !disabled) return ERROR_INVALID_PARAMETER;
        ok = ConfigureSystemExclusion(args[2], enabled, &error);
    } else if (args.size() >= 2 && args[1] == L"--rebuild-index") {
        ok = true;
    }
    if (!ok) return ERROR_INVALID_DATA;
    ReloadService();
    return 0;
}

int ExportDiagnosticsCommand(const std::vector<std::wstring>& args) {
    if (args.size() != 3 && !(args.size() == 4 && args[3] == L"--include-dumps")) return ERROR_INVALID_PARAMETER;
    if (!IsElevated()) return ERROR_ELEVATION_REQUIRED;
    pulse::diagnostics::ExportOptions options;
    options.source_root = MachineDataRoot();
    options.destination = args[2];
    options.include_dumps = args.size() == 4;
    options.support_report = true;
    options.configuration_file = MachineConfigPath();
    options.require_empty_destination = true;
    std::wstring error;
    return pulse::diagnostics::Export(options, &error) ? 0 : ERROR_WRITE_FAULT;
}

int UninstallService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return static_cast<int>(GetLastError());
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!svc) {
        const DWORD err = GetLastError();
        CloseServiceHandle(scm);
        return (err == ERROR_SERVICE_DOES_NOT_EXIST) ? 0 : static_cast<int>(err);
    }
    SERVICE_STATUS st{};
    ControlService(svc, SERVICE_CONTROL_STOP, &st);
    for (int i = 0; i < 40; ++i) {
        SERVICE_STATUS_PROCESS ssp{};
        DWORD needed = 0;
        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &needed) ||
            ssp.dwCurrentState == SERVICE_STOPPED) break;
        Sleep(250);
    }
    DeleteService(svc);
    CloseServiceHandle(svc);
    // SCM keeps the name until the last handle/process releases it.
    for (int i = 0; i < 40; ++i) {
        SC_HANDLE check = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS);
        if (!check) {
            CloseServiceHandle(scm);
            return 0;
        }
        CloseServiceHandle(check);
        Sleep(250);
    }
    CloseServiceHandle(scm);
    return static_cast<int>(ERROR_SERVICE_MARKED_FOR_DELETE);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    if (argv) {
        args.reserve(static_cast<size_t>(argc));
        for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
    }
    std::wstring a1 = args.size() >= 2 ? args[1] : L"";
    if (argv) LocalFree(argv);

    const bool service_mode = a1 == L"--service";
    const auto role = service_mode ? pulse::crash::ProcessRole::IndexService
        : a1 == L"--network-agent" ? pulse::crash::ProcessRole::NetworkAgent
        : a1 == L"--content-agent" ? pulse::crash::ProcessRole::ContentAgent
        : pulse::crash::ProcessRole::IndexHelper;
    pulse::crash::Initialize({role, service_mode, {}});

    if (a1 == L"--install") return InstallService();
    if (a1 == L"--uninstall") return UninstallService();
    if (a1 == L"--export-diagnostics") return ExportDiagnosticsCommand(args);
    if (a1 == L"--configure-volume" || a1 == L"--set-index-path" ||
        a1 == L"--configure-exclude" || a1 == L"--configure-system-exclusion" ||
        a1 == L"--rebuild-index") return ConfigureCommand(args);
    if (a1 == L"--service") {
        SERVICE_TABLE_ENTRYW table[] = {
            { const_cast<LPWSTR>(kServiceName), SvcMain },
            { nullptr, nullptr }
        };
        if (!StartServiceCtrlDispatcherW(table)) return static_cast<int>(GetLastError());
        return 0;
    }
    if (a1 == L"--network-agent") return RunNetworkAgent();
    if (a1 == L"--test-network-agent" && args.size() == 3) return RunNetworkAgentTest(args[2]);
    if (a1 == L"--content-instant-agent" && args.size() == 4) return RunPersistentContentAgent(args[2], wcstoul(args[3].c_str(), nullptr, 10), ContentAgentMode::Instant);
    if (a1 == L"--content-index-agent" && args.size() == 4) return RunPersistentContentAgent(args[2], wcstoul(args[3].c_str(), nullptr, 10));
    if (a1 == L"--content-index-observer" && args.size() == 4) return RunPersistentContentAgent(args[2], wcstoul(args[3].c_str(), nullptr, 10), true);
    if (a1 == L"--content-agent" && args.size() == 3) return RunContentAgent(args[2]);
    if (a1 == L"--test-host" && args.size() >= 3) {
        const std::wstring& token = args[2];
        const bool valid = !token.empty() && token.size() <= 64 &&
            std::all_of(token.begin(), token.end(), [](wchar_t c) {
                return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
                       (c >= L'A' && c <= L'Z') || c == L'-' || c == L'_';
            });
        if (!valid) return ERROR_INVALID_PARAMETER;
        if (args.size() >= 5) SetActiveIndexDirectory(args[4]);
        return RunHost(false, true,
            L"\\\\.\\pipe\\PulseIndex.Test." + token,
            L"Local\\Pulse.Index.Test." + token, args.size() >= 5 ? args[3] : std::wstring{});
    }
    return RunHost(false);
}

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
#include "index_engine.h"
#include "index_config.h"
#include "../common/crash_reporter.h"
#include "../common/diagnostics_exporter.h"
#include "index_paths.h"
#include "index_path_service.h"
#include "network_agent_host.h"
#include "content_agent.h"
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
constexpr UINT kIdleMs = 15000;
constexpr DWORD kMaxClients = 16;

struct Client {
    std::atomic<HANDLE> pipe{INVALID_HANDLE_VALUE};
    std::mutex write_mu;
    std::atomic<bool> alive{true};
    std::atomic<uint32_t> latest_search{0};
    std::atomic<bool> thread_done{false};
};

struct SearchTask {
    std::shared_ptr<Client> client;
    uint32_t id = 0;
    Query query;
};

struct ClientWorker {
    std::shared_ptr<Client> client;
    std::thread thread;
};

struct Host {
    Engine engine;
    HWND hwnd = nullptr;
    HANDLE listen = INVALID_HANDLE_VALUE;
    HANDLE stop = nullptr;
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

bool WriteFrame(Client& c, uint32_t type, uint32_t id, const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lock(c.write_mu);
    const HANDLE pipe = c.pipe.load();
    if (pipe == INVALID_HANDLE_VALUE) return false;
    auto hdr = MakeIndexHdr(type, id, static_cast<uint32_t>(payload.size()));
    if (!PipeWrite(pipe, reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr)))
        return false;
    if (!payload.empty() &&
        !PipeWrite(pipe, payload.data(), static_cast<DWORD>(payload.size())))
        return false;
    return true;
}

std::vector<uint8_t> StatusPayload() {
    PayloadWriter w;
    w.PutU32(g.engine.Ready() ? 1u : 0u);
    w.PutU32(static_cast<uint32_t>((std::min)(g.engine.Count(), static_cast<size_t>(0xffffffffu))));
    w.PutString(g.engine.Status());
    return w.data();
}

void BroadcastStatus() {
    auto payload = StatusPayload();
    std::vector<std::shared_ptr<Client>> clients;
    {
        std::lock_guard<std::mutex> lock(g.clients_mu);
        clients = g.clients;
    }
    for (auto& c : clients) {
        if (c && c->alive) WriteFrame(*c, RSP_IDX_STATUS, 0, payload);
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
    q.sort = static_cast<ResultSort>(sort);
    q.limit = limit;
    q.offset = offset;
    if (q.limit > kSearchPageCap) q.limit = kSearchPageCap;
    return true;
}

void QueueSearch(std::shared_ptr<Client> c, uint32_t id, Query query) {
    std::lock_guard<std::mutex> lock(g.search_mu);
    g.search_queue.erase(
        std::remove_if(g.search_queue.begin(), g.search_queue.end(),
            [&](const SearchTask& task) { return task.client == c; }),
        g.search_queue.end());
    g.search_queue.push_back(SearchTask{std::move(c), id, std::move(query)});
    g.search_cv.notify_one();
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
        if (!c || !c->alive || c->latest_search.load() != task.id) continue;
        SearchResult sr = g.engine.Search(task.query, &c->latest_search, task.id);
        if (!c->alive || c->latest_search.load() != task.id) continue;
        auto out = SearchPayload(sr);
        if (out.size() > kIndexMaxPayload) {
            sr.hits.clear();
            out = SearchPayload(sr);
        }
        WriteFrame(*c, RSP_IDX_SEARCH, task.id, out);
    }
}

void DropClient(const std::shared_ptr<Client>& c) {
    if (!c) return;
    c->alive = false;
    ++c->latest_search;
    {
        std::lock_guard<std::mutex> lock(c->write_mu);
        const HANDLE pipe = c->pipe.exchange(INVALID_HANDLE_VALUE);
        if (pipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(pipe, nullptr);
            CloseHandle(pipe);
        }
    }
    std::lock_guard<std::mutex> lock(g.clients_mu);
    g.clients.erase(std::remove(g.clients.begin(), g.clients.end(), c), g.clients.end());
    if (g.clients.empty()) g.idle_since = GetTickCount64();
}

void ClientThread(std::shared_ptr<Client> c) {
    while (g.running && c->alive) {
        MsgHeader hdr{};
        std::vector<uint8_t> payload;
        const HANDLE pipe = c->pipe.load();
        if (pipe == INVALID_HANDLE_VALUE ||
            !PipeRead(pipe, reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr)))
            break;
        if (hdr.magic != kIndexMagic || hdr.payload_size > kIndexMaxRequestPayload) break;
        payload.resize(hdr.payload_size);
        if (hdr.payload_size &&
            !PipeRead(pipe, payload.data(), hdr.payload_size))
            break;
        if (hdr.type == REQ_IDX_STATUS) {
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
        g.listen = h;
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
                CloseHandle(ol.hEvent);
                CloseHandle(h);
                g.listen = INVALID_HANDLE_VALUE;
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
            g.listen = INVALID_HANDLE_VALUE;
            continue;
        }
        g.client_workers.push_back(ClientWorker{c, std::thread(ClientThread, c)});
        WriteFrame(*c, RSP_IDX_STATUS, 0, StatusPayload());
        g.listen = INVALID_HANDLE_VALUE;
    }
}

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ENGINE_NOTIFY) {
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
        if (g.listen != INVALID_HANDLE_VALUE) CancelIoEx(g.listen, nullptr);
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
            std::wstring mutex_name = kMutexName) {
    g.as_service = as_service;
    g.test_mode = test_mode;
    g.pipe_name = std::move(pipe_name);
    g.mutex_name = std::move(mutex_name);
    ServiceTrace(L"RunHost entered");
    SetMachineIndexScope(as_service);
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
    if (g.test_mode) {
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
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ServiceTrace(L"message loop exited");

    g.running = false;
    if (g.stop) SetEvent(g.stop);
    if (g.listen != INVALID_HANDLE_VALUE) {
        CancelIoEx(g.listen, nullptr);
        CloseHandle(g.listen);
        g.listen = INVALID_HANDLE_VALUE;
    }
    {
        std::lock_guard<std::mutex> lock(g.clients_mu);
        for (auto& c : g.clients) {
            if (!c) continue;
            c->alive = false;
            ++c->latest_search;
            const HANDLE pipe = c->pipe.load();
            if (pipe != INVALID_HANDLE_VALUE) CancelIoEx(pipe, nullptr);
        }
    }
    g.search_cv.notify_all();
    if (g.accept_thread.joinable()) g.accept_thread.join();
    for (auto& worker : g.client_workers)
        if (worker.thread.joinable()) worker.thread.join();
    g.client_workers.clear();
    if (g.search_thread.joinable()) g.search_thread.join();
    if (!g.test_mode) g.engine.Stop();
    if (g.mutex) {
        ReleaseMutex(g.mutex);
        CloseHandle(g.mutex);
        g.mutex = nullptr;
    }
    if (g.stop) {
        CloseHandle(g.stop);
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
        if (g.hwnd) PostMessageW(g.hwnd, WM_QUIT_HOST, 0, 0);
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
                                     SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS |
                                         SERVICE_CHANGE_CONFIG);
        if (!svc) {
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

        ChangeServiceConfigW(svc, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                             SERVICE_ERROR_NORMAL, bin.c_str(), nullptr, nullptr,
                             nullptr, nullptr, nullptr, L"Pulse Index");
        SERVICE_DESCRIPTIONW desc{};
        wchar_t text[] = L"Pulse file-name index (MFT + USN). UI talks to this over a named pipe.";
        desc.lpDescription = text;
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

        SERVICE_STATUS_PROCESS ssp{};
        DWORD needed = 0;
        if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp),
                                 sizeof(ssp), &needed) &&
            ssp.dwCurrentState != SERVICE_STOPPED &&
            ssp.dwCurrentState != SERVICE_STOP_PENDING) {
            SERVICE_STATUS stop_status{};
            ControlService(svc, SERVICE_CONTROL_STOP, &stop_status);
            for (int i = 0; i < 40; ++i) {
                Sleep(250);
                if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                          reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp),
                                          &needed) ||
                    ssp.dwCurrentState == SERVICE_STOPPED) break;
            }
        }

        const BOOL started = StartServiceW(svc, 0, nullptr);
        last_err = started ? 0 : GetLastError();
        if (last_err == ERROR_SERVICE_ALREADY_RUNNING) last_err = 0;
        if (last_err == 0) {
            // Wait until RUNNING, then confirm it stays up past the early
            // crash window (corrupt delta replay previously died ~2s in).
            bool running = false;
            for (int i = 0; i < 80; ++i) {
                if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                          reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp),
                                          &needed)) break;
                if (ssp.dwCurrentState == SERVICE_RUNNING) {
                    running = true;
                    break;
                }
                if (ssp.dwCurrentState == SERVICE_STOPPED) {
                    last_err = ssp.dwWin32ExitCode ? ssp.dwWin32ExitCode
                                                   : ERROR_SERVICE_NOT_ACTIVE;
                    break;
                }
                Sleep(100);
            }
            if (running) {
                Sleep(3500);
                if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                         reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp),
                                         &needed) &&
                    ssp.dwCurrentState == SERVICE_RUNNING) {
                    CloseServiceHandle(svc);
                    CloseServiceHandle(scm);
                    return 0;
                }
                last_err = (ssp.dwCurrentState == SERVICE_STOPPED && ssp.dwWin32ExitCode)
                    ? ssp.dwWin32ExitCode
                    : ERROR_SERVICE_NOT_ACTIVE;
            } else if (last_err == 0) {
                last_err = ERROR_SERVICE_REQUEST_TIMEOUT;
            }
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
    } else if (args.size() >= 2 && args[1] == L"--rebuild-index") {
        ok = true;
    }
    if (!ok) return ERROR_INVALID_DATA;
    ReloadService();
    return 0;
}

int ExportDiagnosticsCommand(const std::vector<std::wstring>& args) {
    if (args.size() != 3) return ERROR_INVALID_PARAMETER;
    if (!IsElevated()) return ERROR_ELEVATION_REQUIRED;
    pulse::diagnostics::ExportOptions options;
    options.source_root = MachineDataRoot();
    options.destination = args[2];
    options.include_dumps = true;
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
        a1 == L"--configure-exclude" ||
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
    if (a1 == L"--content-agent" && args.size() == 3) return RunContentAgent(args[2]);
    if (a1 == L"--test-host" && args.size() >= 3) {
        const std::wstring& token = args[2];
        const bool valid = !token.empty() && token.size() <= 64 &&
            std::all_of(token.begin(), token.end(), [](wchar_t c) {
                return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
                       (c >= L'A' && c <= L'Z') || c == L'-' || c == L'_';
            });
        if (!valid) return ERROR_INVALID_PARAMETER;
        return RunHost(false, true,
            L"\\\\.\\pipe\\PulseIndex.Test." + token,
            L"Local\\Pulse.Index.Test." + token);
    }
    return RunHost(false);
}

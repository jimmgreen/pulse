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
#include "index_paths.h"
#include "network_agent_host.h"
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <winsvc.h>
#include <algorithm>
#include <atomic>
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

struct Client {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::mutex write_mu;
    std::atomic<bool> alive{true};
    std::atomic<uint32_t> latest_search{0};
};

struct Host {
    Engine engine;
    HWND hwnd = nullptr;
    HANDLE listen = INVALID_HANDLE_VALUE;
    HANDLE stop = nullptr;
    HANDLE mutex = nullptr;
    std::atomic<bool> running{true};
    std::atomic<uint32_t> active_queries{0};
    bool as_service = false;
    SERVICE_STATUS_HANDLE svc = nullptr;
    SERVICE_STATUS status{};
    std::mutex clients_mu;
    std::vector<std::shared_ptr<Client>> clients;
    std::thread accept_thread;
    ULONGLONG idle_since = 0;
    bool ever_client = false;
} g;

void ServiceTrace(const wchar_t* text) {
    if (!g.as_service) return;
    CreateDirectoryW(L"C:\\ProgramData\\Pulse", nullptr);
    HANDLE file = CreateFileW(L"C:\\ProgramData\\Pulse\\index-service.log",
                              FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t line[512]{};
    swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u %s\r\n",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, text);
    DWORD bytes = 0;
    WriteFile(file, line, static_cast<DWORD>(wcslen(line) * sizeof(wchar_t)), &bytes, nullptr);
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
    if (c.pipe == INVALID_HANDLE_VALUE) return false;
    auto hdr = MakeIndexHdr(type, id, static_cast<uint32_t>(payload.size()));
    if (!PipeWrite(c.pipe, reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr)))
        return false;
    if (!payload.empty() &&
        !PipeWrite(c.pipe, payload.data(), static_cast<DWORD>(payload.size())))
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
    std::lock_guard<std::mutex> lock(g.clients_mu);
    for (auto& c : g.clients) {
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
    w.PutString(g.as_service ? config.index_path : DataDir());
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
    return w.data();
}

Query ParseQuery(const uint8_t* p, size_t n) {
    Query q;
    PayloadReader r(p, n);
    uint32_t flags = 0, sort = 0, limit = 0, offset = 0;
    if (!r.GetU32(flags) || !r.GetU32(sort) || !r.GetU32(limit) || !r.GetU32(offset) ||
        !r.GetString(q.needle) || !r.GetString(q.path_prefix))
        return q;
    q.rank = (flags & 1) != 0;
    q.folders_only = (flags & 2) != 0;
    q.sort_desc = (flags & 4) != 0;
    q.sort = static_cast<ResultSort>(sort);
    q.limit = limit;
    q.offset = offset;
    if (q.limit > kSearchPageCap) q.limit = kSearchPageCap;
    return q;
}

void DropClient(const std::shared_ptr<Client>& c) {
    if (!c) return;
    c->alive = false;
    ++c->latest_search;
    {
        std::lock_guard<std::mutex> lock(c->write_mu);
        if (c->pipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(c->pipe, nullptr);
            CloseHandle(c->pipe);
            c->pipe = INVALID_HANDLE_VALUE;
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
        if (!PipeRead(c->pipe, reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr))) break;
        if (hdr.magic != kIndexMagic || hdr.payload_size > kIndexMaxPayload) break;
        payload.resize(hdr.payload_size);
        if (hdr.payload_size &&
            !PipeRead(c->pipe, payload.data(), hdr.payload_size))
            break;
        if (hdr.type == REQ_IDX_STATUS) {
            WriteFrame(*c, RSP_IDX_STATUS, hdr.request_id, StatusPayload());
        } else if (hdr.type == REQ_IDX_VOLUMES) {
            WriteFrame(*c, RSP_IDX_VOLUMES, hdr.request_id, VolumesPayload());
        } else if (hdr.type == REQ_IDX_SEARCH) {
            const uint32_t id = hdr.request_id;
            c->latest_search.store(id);
            Query query = ParseQuery(payload.data(), payload.size());
            ++g.active_queries;
            std::thread([c, id, query = std::move(query)] {
                SearchResult sr = g.engine.Search(query, &c->latest_search, id);
                if (c->alive && c->latest_search.load() == id) {
                    auto out = SearchPayload(sr);
                    if (out.size() > kIndexMaxPayload) {
                        sr.hits.clear();
                        out = SearchPayload(sr);
                    }
                    WriteFrame(*c, RSP_IDX_SEARCH, id, out);
                }
                --g.active_queries;
            }).detach();
        }
    }
    DropClient(c);
}

void AcceptLoop() {
    bool first = true;
    while (g.running && (!g.stop || WaitForSingleObject(g.stop, 0) != WAIT_OBJECT_0)) {
        DWORD flags = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
        if (first) flags |= FILE_FLAG_FIRST_PIPE_INSTANCE;
        HANDLE h = CreateNamedPipeW(
            kPipeName, flags,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, PipeSa());
        if (h == INVALID_HANDLE_VALUE) {
            if (first) {
                if (g.hwnd) PostMessageW(g.hwnd, WM_QUIT_HOST, 0, 0);
                break; // another instance already owns the pipe
            }
            Sleep(200);
            if (g.stop && WaitForSingleObject(g.stop, 0) == WAIT_OBJECT_0) break;
            continue;
        }
        first = false;
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
        auto c = std::make_shared<Client>();
        c->pipe = h;
        {
            std::lock_guard<std::mutex> lock(g.clients_mu);
            g.clients.push_back(c);
            g.idle_since = 0;
            g.ever_client = true;
        }
        std::thread(ClientThread, c).detach();
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

int RunHost(bool as_service) {
    g.as_service = as_service;
    ServiceTrace(L"RunHost entered");
    SetMachineIndexScope(as_service);
    g.running = true;
    g.idle_since = GetTickCount64();

    if (!as_service) {
        g.mutex = CreateMutexW(nullptr, TRUE, kMutexName);
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
    g.engine.Start(g.hwnd, WM_ENGINE_NOTIFY);
    ServiceTrace(L"engine thread started");
    SetTimer(g.hwnd, kIdleTimer, 1000, nullptr);
    g.accept_thread = std::thread(AcceptLoop);
    ServiceTrace(L"accept thread started");

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

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
            if (c->pipe != INVALID_HANDLE_VALUE) CancelIoEx(c->pipe, nullptr);
        }
    }
    if (g.accept_thread.joinable()) g.accept_thread.join();
    for (int i = 0; i < 100 && g.active_queries.load() != 0; ++i) Sleep(50);
    g.engine.Stop();
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
    IndexConfig config;
    LoadMachineConfig(config, nullptr);
    SaveMachineConfig(config, nullptr);
    const std::wstring bin = L"\"" + SelfPath() + L"\" --service";
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) return static_cast<int>(GetLastError());
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_START | SERVICE_CHANGE_CONFIG);
    if (!svc) {
        svc = CreateServiceW(scm, kServiceName, L"Pulse Index",
                             SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                             SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                             bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    }
    if (!svc) {
        const DWORD err = GetLastError();
        CloseServiceHandle(scm);
        return static_cast<int>(err);
    }
    SERVICE_DESCRIPTIONW desc{};
    wchar_t text[] = L"Pulse file-name index (MFT + USN). UI talks to this over a named pipe.";
    desc.lpDescription = text;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
    StartServiceW(svc, 0, nullptr);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
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
    SetMachineIndexScope(true);
    std::wstring error;
    bool ok = false;
    if (args.size() >= 3 && args[1] == L"--configure-volume") {
        const bool enabled = args.size() >= 4 && args[3] == L"--enable";
        const bool disabled = args.size() >= 4 && args[3] == L"--disable";
        if (!enabled && !disabled) return ERROR_INVALID_PARAMETER;
        ok = ConfigureVolume(args[2], enabled, &error);
    } else if (args.size() >= 3 && args[1] == L"--set-index-path") {
        ok = ConfigureIndexPath(args[2], &error);
    } else if (args.size() >= 2 && args[1] == L"--rebuild-index") {
        ok = true;
    }
    if (!ok) return ERROR_INVALID_DATA;
    ReloadService();
    return 0;
}

int UninstallService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return static_cast<int>(GetLastError());
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP | DELETE);
    if (!svc) {
        const DWORD err = GetLastError();
        CloseServiceHandle(scm);
        return (err == ERROR_SERVICE_DOES_NOT_EXIST) ? 0 : static_cast<int>(err);
    }
    SERVICE_STATUS st{};
    ControlService(svc, SERVICE_CONTROL_STOP, &st);
    DeleteService(svc);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
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

    if (a1 == L"--install") return InstallService();
    if (a1 == L"--uninstall") return UninstallService();
    if (a1 == L"--configure-volume" || a1 == L"--set-index-path" ||
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
    return RunHost(false);
}

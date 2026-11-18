#include "isolation.h"
#include "loopback_evidence.h"
#include <aclapi.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <sddl.h>
#include <string>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <netfw.h>

namespace {
using pulse::extension_host::Isolation;
std::wstring Quote(const std::wstring &value) { return L"\"" + value + L"\""; }
DWORD Diagnostic(DWORD error) { return 0x10000000u | (error & 0x00ffffffu); }
std::wstring SidText(PSID sid) {
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(sid, &text))
        return {};
    std::wstring result(text);
    LocalFree(text);
    return result;
}
std::wstring UserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> bytes(size);
    const bool ok = GetTokenInformation(token, TokenUser, bytes.data(), size, &size) != FALSE;
    CloseHandle(token);
    return ok ? SidText(reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid) : L"";
}
bool Acl(const std::wstring &path, const std::wstring &user, PSID container = nullptr) {
    // Protected, non-inheriting ACL: a grant on the fixture directory never grants its secret file.
    std::wstring sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;" + user + L")";
    if (container)
        sddl += L"(A;;FRFX;;;" + SidText(container) + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor,
                                                              nullptr))
        return false;
    const bool ok =
        SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                         descriptor) != FALSE;
    LocalFree(descriptor);
    return ok;
}
bool Put(const std::wstring &path) {
    HANDLE file =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, "probe-marker", 12, &written, nullptr) && written == 12;
    CloseHandle(file);
    return ok;
}
bool ListenerBaseline(SOCKET listener, const sockaddr_in &address) {
    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET)
        return false;
    bool ok = connect(client, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0;
    if (ok) {
        fd_set readable{};
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        timeval timeout{2, 0};
        ok = select(0, &readable, nullptr, nullptr, &timeout) > 0;
        if (ok) {
            SOCKET accepted = accept(listener, nullptr, nullptr);
            ok = accepted != INVALID_SOCKET;
            if (ok)
                closesocket(accepted);
        }
    }
    closesocket(client);
    return ok;
}
DWORD DiagnoseLoopbackTimeout() {
    HMODULE module = LoadLibraryExW(L"Firewallapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
        return 0x30000000u | (GetLastError() & 0x0fffffffu);
    decltype(&NetworkIsolationDiagnoseConnectFailureAndGetInfo) diagnose = nullptr;
    const FARPROC address = GetProcAddress(module, "NetworkIsolationDiagnoseConnectFailureAndGetInfo");
    static_assert(sizeof(diagnose) == sizeof(address));
    std::memcpy(&diagnose, &address, sizeof(diagnose));
    NETISO_ERROR_TYPE category = NETISO_ERROR_TYPE_MAX;
    const DWORD status = diagnose ? diagnose(L"127.0.0.1", &category) : ERROR_PROC_NOT_FOUND;
    FreeLibrary(module);
    // Preserve a nonzero diagnostic result. Capability diagnosis is not a socket-level WFP audit.
    if (status != ERROR_SUCCESS)
        return 0x30000000u | (status & 0x0fffffffu);
    return 0x20000000u | static_cast<DWORD>(category);
}
DWORD ConnectDenied(const char *address, unsigned short port) {
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        const int error = WSAGetLastError();
        return error == WSAEACCES ? 0 : Diagnostic(error);
    }
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking)) {
        const int error = WSAGetLastError();
        closesocket(socket);
        return Diagnostic(error);
    }
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    InetPtonA(AF_INET, address, &peer.sin_addr);
    int result = connect(socket, reinterpret_cast<sockaddr *>(&peer), sizeof(peer));
    int error = result == 0 ? 0 : WSAGetLastError();
    if (error == WSAEWOULDBLOCK) {
        fd_set write{}, except{};
        FD_ZERO(&write);
        FD_ZERO(&except);
        FD_SET(socket, &write);
        FD_SET(socket, &except);
        timeval timeout{2, 0};
        result = select(0, nullptr, &write, &except, &timeout);
        if (result > 0) {
            int size = sizeof(error);
            if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &size))
                error = WSAGetLastError();
        } else
            error = result == 0 ? WSAETIMEDOUT : WSAGetLastError();
    }
    closesocket(socket);
    // Unreachable routes, firewall timeouts and connection refusal are not proof of sandbox denial.
    return error == WSAEACCES ? 0 : Diagnostic(static_cast<DWORD>(error));
}
DWORD Child(int argc, wchar_t **argv) {
    if (argc != 7)
        return Diagnostic(ERROR_INVALID_PARAMETER);
    HANDLE token = nullptr;
    DWORD flag = 0, size = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return Diagnostic(GetLastError());
    bool valid = GetTokenInformation(token, TokenIsAppContainer, &flag, sizeof(flag), &size) && flag == 1;
    GetTokenInformation(token, TokenCapabilities, nullptr, 0, &size);
    std::vector<unsigned char> capabilities(size);
    valid = valid && GetTokenInformation(token, TokenCapabilities, capabilities.data(), size, &size) &&
            reinterpret_cast<TOKEN_GROUPS *>(capabilities.data())->GroupCount == 0;
    CloseHandle(token);
    if (!valid)
        return Diagnostic(ERROR_INVALID_TOKEN);
    const std::wstring mode = argv[2], root = argv[3];
    if (mode == L"read" || mode == L"secret" || mode == L"write" || mode == L"create") {
        const std::wstring path = root + (mode == L"secret"   ? L"\\secret.txt"
                                          : mode == L"create" ? L"\\forbidden.txt"
                                                              : L"\\allowed.txt");
        const bool writing = mode == L"write" || mode == L"create";
        HANDLE file =
            CreateFileW(path.c_str(), writing ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ, nullptr,
                        mode == L"create" ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            return mode != L"read" && error == ERROR_ACCESS_DENIED ? 0 : Diagnostic(error);
        }
        char bytes[12]{};
        DWORD read = 0;
        const bool ok = mode == L"read" && ReadFile(file, bytes, 12, &read, nullptr) && read == 12 &&
                        std::string(bytes, 12) == "probe-marker";
        CloseHandle(file);
        return ok ? 0 : Diagnostic(ERROR_INVALID_ACCESS);
    }
    if (mode == L"ipc") {
        HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[4]);
        if (event) {
            CloseHandle(event);
            return Diagnostic(ERROR_INVALID_ACCESS);
        }
        const DWORD error = GetLastError();
        return error == ERROR_ACCESS_DENIED || error == ERROR_FILE_NOT_FOUND ? 0 : Diagnostic(error);
    }
    if (mode == L"pipe") {
        HANDLE pipe =
            CreateFileW(argv[6], GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
            return Diagnostic(ERROR_INVALID_ACCESS);
        }
        const DWORD error = GetLastError();
        return error == ERROR_ACCESS_DENIED ? 0 : Diagnostic(error);
    }
    if (mode == L"child") {
        PROCESS_MITIGATION_CHILD_PROCESS_POLICY policy{};
        if (!GetProcessMitigationPolicy(GetCurrentProcess(), ProcessChildProcessPolicy, &policy,
                                        sizeof(policy)) ||
            !policy.NoChildProcessCreation)
            return Diagnostic(ERROR_INVALID_STATE);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        std::wstring command = Quote(argv[0]) + L" --no-action";
        if (CreateProcessW(argv[0], command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                           nullptr, &startup, &process)) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return Diagnostic(ERROR_INVALID_ACCESS);
        }
        const DWORD error = GetLastError();
        return error == ERROR_ACCESS_DENIED || error == ERROR_CHILD_PROCESS_BLOCKED ? 0 : Diagnostic(error);
    }
    if (mode == L"network" || mode == L"external") {
        WSADATA data{};
        const int error = WSAStartup(MAKEWORD(2, 2), &data);
        if (error)
            return Diagnostic(error);
        const DWORD result = mode == L"network"
                                 ? ConnectDenied("127.0.0.1", static_cast<unsigned short>(_wtoi(argv[5])))
                                 : ConnectDenied("192.0.2.1", 443);
        const DWORD diagnosed = mode == L"network" && result == Diagnostic(WSAETIMEDOUT)
                                    ? DiagnoseLoopbackTimeout() : result;
        WSACleanup();
        return diagnosed;
    }
    if (mode == L"memory") {
        void *memory = VirtualAlloc(nullptr, 128 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (memory) {
            VirtualFree(memory, 0, MEM_RELEASE);
            return Diagnostic(ERROR_INVALID_ACCESS);
        }
        const DWORD error = GetLastError();
        return error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_COMMITMENT_LIMIT ? 0 : Diagnostic(error);
    }
    if (mode == L"spin") {
        std::atomic<unsigned long long> counter{0};
        for (;;)
            counter.fetch_add(1, std::memory_order_relaxed);
    }
    return Diagnostic(ERROR_INVALID_PARAMETER);
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc > 1 && std::wstring(argv[1]) == L"--child")
        return static_cast<int>(Child(argc, argv));
    DWORD diagnostic_memory_mib = 0;
    bool diagnostic_profile_environment = false;
    if (argc == 2 && std::wstring(argv[1]) == L"--diagnose-loader")
        diagnostic_memory_mib = 64;
    else if (argc == 2 && std::wstring(argv[1]) == L"--diagnose-loader-128")
        diagnostic_memory_mib = 128;
    else if (argc == 2 && std::wstring(argv[1]) == L"--diagnose-loader-profile") {
        diagnostic_memory_mib = 64;
        diagnostic_profile_environment = true;
    }
    else if (argc != 1)
        return 2;
    if (diagnostic_memory_mib)
        std::wprintf(L"[DIAGNOSTIC ONLY] loader trace; memory=%lu MiB; no security-test verdict\n",
                     diagnostic_memory_mib);
    wchar_t temp[32768]{}, executable[32768]{};
    if (!GetTempPathW(32768, temp) || !GetModuleFileNameW(nullptr, executable, 32768))
        return 2;
    const std::wstring root = std::wstring(temp) + L"PulseIsolation-" +
                              std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(GetTickCount64());
    const std::wstring user = UserSid(), copy = root + L"\\probe.exe", allowed = root + L"\\allowed.txt",
                       secret = root + L"\\secret.txt";
    if (user.empty() || !CreateDirectoryW(root.c_str(), nullptr)) {
        std::printf("[UNAVAILABLE] fixture creation %lu\n", GetLastError());
        return 2;
    }
    bool setup = Acl(root, user) && CopyFileW(executable, copy.c_str(), TRUE) && Put(allowed) &&
                 Put(secret) && Acl(secret, user);
    const std::wstring event_name = L"Local\\PulseIsolationPrivate-" + std::to_wstring(GetCurrentProcessId());
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
    HANDLE baseline_event = event ? OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str()) : nullptr;
    setup = setup && event && baseline_event;
    if (baseline_event)
        CloseHandle(baseline_event);
    const std::wstring pipe_name =
        L"\\\\.\\pipe\\PulseIsolationPrivate-" + std::to_wstring(GetCurrentProcessId());
    HANDLE pipe =
        CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 256,
                         256, 0, nullptr);
    HANDLE baseline_pipe = pipe == INVALID_HANDLE_VALUE
                               ? INVALID_HANDLE_VALUE
                               : CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                             OPEN_EXISTING, 0, nullptr);
    setup = setup && baseline_pipe != INVALID_HANDLE_VALUE;
    if (baseline_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(baseline_pipe);
        DisconnectNamedPipe(pipe);
    }
    WSADATA data{};
    const bool winsock = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    SOCKET listener = winsock ? socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) : INVALID_SOCKET;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int address_size = sizeof(address);
    setup = setup && listener != INVALID_SOCKET &&
            bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 &&
            listen(listener, 4) == 0 &&
            getsockname(listener, reinterpret_cast<sockaddr *>(&address), &address_size) == 0;
    if (setup)
        setup = ListenerBaseline(listener, address);
    void *memory_baseline =
        VirtualAlloc(nullptr, 128 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    setup = setup && memory_baseline != nullptr;
    if (memory_baseline)
        VirtualFree(memory_baseline, 0, MEM_RELEASE);
    int failures = 0;
    if (!setup) {
        std::printf("[UNAVAILABLE] controlled fixture/network baseline failed Win32=%lu WSA=%d\n",
                    GetLastError(), WSAGetLastError());
        ++failures;
    }
    if (setup)
        for (const auto *mode : {L"read", L"secret", L"write", L"create", L"ipc", L"pipe", L"child",
                                 L"network", L"external", L"memory", L"spin"}) {
            if (diagnostic_memory_mib && std::wstring(mode) != L"read")
                continue;
            Isolation isolation;
            bool ok = isolation.Create();
            if (ok)
                ok = Acl(root, user, isolation.Sid()) && Acl(copy, user, isolation.Sid()) &&
                     Acl(allowed, user, isolation.Sid());
            if (!ok) {
                std::wprintf(L"[UNAVAILABLE] %ls setup stage=%ls error=0x%08lx last=%lu\n", mode,
                             isolation.Stage().c_str(), isolation.Error(), GetLastError());
                ++failures;
                continue;
            }
            const std::wstring args = L"--child " + std::wstring(mode) + L" " + Quote(root) + L" " +
                                      Quote(event_name) + L" " + std::to_wstring(ntohs(address.sin_port)) +
                                      L" " + Quote(pipe_name);
            pulse::extension_host::LoopbackEvidence network_evidence;
            if (std::wstring(mode) == L"network")
                network_evidence.Start(copy, isolation.Sid(), ntohs(address.sin_port));
            if (std::wstring(mode) == L"network")
                std::wprintf(L"[NETWORK] loopback port=%u container=%ls; parent connect/accept baseline established\n",
                             static_cast<unsigned>(ntohs(address.sin_port)), SidText(isolation.Sid()).c_str());
            ok = isolation.Launch(copy, args, diagnostic_memory_mib, diagnostic_profile_environment);
            DWORD code = STILL_ACTIVE;
            if (ok && std::wstring(mode) == L"spin") {
                // The process must actually remain alive before closing its sole Job handle.
                ok = !isolation.Wait(400, code) && isolation.Error() == ERROR_TIMEOUT &&
                     isolation.CloseJobAndWait() && isolation.Wait(1000, code) && code != STILL_ACTIVE;
            } else if (ok)
                ok = isolation.Wait(10000, code) && code == 0;
            const bool netiso_category = (code & 0xf0000000u) == 0x20000000u;
            const bool netiso_failure = (code & 0xf0000000u) == 0x30000000u;
            bool inconclusive = std::wstring(mode) == L"network" &&
                                      (code == Diagnostic(WSAETIMEDOUT) || netiso_category || netiso_failure);
            if (std::wstring(mode) == L"network") {
                const auto evidence = network_evidence.Finish();
                std::printf("[WFP] subscribed=%d error=%lu capabilityDrop=%d filter=%llu capability=%u; settingsChanged=0\n",
                            evidence.subscribed, evidence.error, evidence.capability_drop,
                            static_cast<unsigned long long>(evidence.filter_id), evidence.capability);
                // A genuine connected socket or launch failure cannot be repaired by a drop event.
                if (inconclusive && evidence.capability_drop && evidence.error == ERROR_SUCCESS) {
                    ok = true;
                    inconclusive = false;
                }
                if (netiso_category)
                    std::wprintf(L"[NETWORK] timeout; NetworkIsolation diagnosis ERROR_SUCCESS category=%lu "
                                 L"(0=none,1=privateNetwork,2=internetClient,3=internetClientServer,4=invalid)\n",
                                 code & 0x0fffffffu);
                else if (netiso_failure)
                    std::wprintf(L"[NETWORK] timeout; NetworkIsolation diagnosis unavailable error=0x%08lx\n",
                                 code & 0x0fffffffu);
                const bool healthy = ListenerBaseline(listener, address);
                std::wprintf(L"[NETWORK] parent connect/accept after child=%ls\n", healthy ? L"healthy" : L"failed");
                ok = ok && healthy;
                if (inconclusive)
                    std::printf("[INCONCLUSIVE] loopback timed out; no explicit WSAEACCES or correlated WFP denial; gate remains closed.\n");
            }
            std::wprintf(L"[%ls] %ls child=0x%08lx stage=%ls error=0x%08lx\n",
                         diagnostic_memory_mib ? L"DIAGNOSTIC" : (inconclusive ? L"INCONCLUSIVE" : (ok ? L"PASS" : L"FAIL")), mode,
                         code, isolation.Stage().c_str(), isolation.Error());
            if (code == 0xc0000142u)
                std::printf("[DIAGNOSTIC] STATUS_DLL_INIT_FAILED: child loader initialization failed; "
                            "no isolation assertion was reached.\n");
            if (!ok)
                ++failures;
            const bool stopped = isolation.CloseJobAndWait();
            const bool removed = stopped && isolation.RemoveContainerProfile();
            if (!removed) {
                ++failures;
                std::wprintf(L"[FAIL] profile/process cleanup %ls error=0x%08lx\n", isolation.Stage().c_str(),
                             isolation.Error());
            }
        }
    if (listener != INVALID_SOCKET)
        closesocket(listener);
    if (winsock)
        WSACleanup();
    if (event)
        CloseHandle(event);
    if (pipe != INVALID_HANDLE_VALUE)
        CloseHandle(pipe);
    // Only known entries within the just-created fixture are removed; never recurse through a link.
    Acl(root, user);
    DeleteFileW(copy.c_str());
    DeleteFileW(allowed.c_str());
    DeleteFileW(secret.c_str());
    DeleteFileW((root + L"\\forbidden.txt").c_str());
    const bool cleaned = RemoveDirectoryW(root.c_str()) != FALSE;
    if (!cleaned) {
        ++failures;
        std::wprintf(L"[FAIL] fixture cleanup %ls error=%lu\n", root.c_str(), GetLastError());
    }
    std::printf("Isolation PoC: failures=%d; third-party scripts remain unavailable.\n", failures);
    return diagnostic_memory_mib ? 2 : (failures ? 1 : 0);
}

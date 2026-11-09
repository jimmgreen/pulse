#include "../ipc/picker_transport.h"

#include <iostream>
#include <thread>

using namespace pulse::ipc::picker;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) ++failures;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << std::endl;
}
std::wstring Executable() {
    std::wstring path(32768, L'\0');
    const auto n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(n);
    return path;
}
struct Child {
    PipeHandle process;
    bool Start(const std::wstring& arguments) {
        const auto exe = Executable();
        std::wstring command = L"\"" + exe + L"\" " + arguments;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) return false;
        CloseHandle(info.hThread);
        process.Reset(info.hProcess);
        return true;
    }
    bool Finish() {
        if (!process || WaitForSingleObject(process.Get(), 60000) != WAIT_OBJECT_0) return false;
        DWORD code = 1;
        const bool queried = GetExitCodeProcess(process.Get(), &code) != FALSE;
        if (!queried || code != 0) std::cout << "[DIAG] child exit=" << code << " queried=" << queried << std::endl;
        return queried && code == 0;
    }
    ~Child() {
        if (process && WaitForSingleObject(process.Get(), 0) == WAIT_TIMEOUT) {
            TerminateProcess(process.Get(), 99);
            WaitForSingleObject(process.Get(), 5000);
        }
    }
};
Message RequestMessage() { Message m; m.request_id[0] = 1; return m; }
int Host(const std::wstring& name, DWORD parent_pid, const std::wstring& mode) {
    PipeHandle parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, parent_pid));
    if (!parent) return 10;
    if (mode == L"raw" || mode == L"cancel" || mode == L"truncated" || mode == L"truncated-response" || mode == L"oversize" || mode == L"disconnect" || mode == L"fragment") {
        PipeHandle pipe(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        if (!pipe) return 11;
        uint8_t ack = 0; DWORD read = 0;
        if (mode != L"raw" && (!ReadFile(pipe.Get(), &ack, 1, &read, nullptr) || read != 1)) return 13;
        if (mode == L"disconnect") return 0;
        std::vector<uint8_t> bytes;
        Encode(RequestMessage(), bytes);
        if (mode == L"truncated-response") {
            Response response; response.status = Status::Accepted; response.paths = {u"C:\\buffered.txt"};
            Encode(Message{RequestMessage().request_id, response}, bytes);
            bytes.resize(29);
        }
        if (mode == L"oversize") { bytes.resize(28); for (size_t i = 8; i < 12; ++i) bytes[i] = 0xff; }
        if (mode == L"truncated") bytes.resize(29);
        if (mode != L"raw" && mode != L"cancel") {
            for (const auto b : bytes) {
                DWORD written = 0;
                if (!WriteFile(pipe.Get(), &b, 1, &written, nullptr) || written != 1) return 12;
            }
        }
        if (mode == L"truncated" || mode == L"truncated-response") return 0;
        while (ReadFile(pipe.Get(), &ack, 1, &read, nullptr) && read != 0) {} // Keep identity alive until the caller closes.
        return 0;
    }
    Channel channel;
    if (!Connect(name, parent.Get(), channel)) return 20;
    Message request;
    if (!channel.Receive(request, {nullptr, 5000})) return 21;
    Response response;
    response.status = Status::Accepted; response.paths = {u"C:\\isolated-fixture.txt"};
    Message answer{request.request_id, response};
    if (!channel.Send(answer, {nullptr, 5000})) return 22;
    if (mode == L"exit-response") return 0;
    Message cancel;
    if (!channel.Receive(cancel, {nullptr, 5000}) || !std::holds_alternative<Cancel>(cancel.body)) return 23;
    if (!channel.Send(cancel, {nullptr, 5000})) return 24;
    Message ack;
    if (!channel.Receive(ack, {nullptr, 5000})) return 25;
    return 0;
}
void Exchange() {
    Server server;
    Check(static_cast<bool>(Server::Create(server)), "create current-user random local listener");
    Check(IsValidPipeName(server.Name()), "generated name valid");
    Child child;
    Check(child.Start(L"--host " + server.Name() + L" " + std::to_wstring(GetCurrentProcessId()) + L" exchange"), "launch controlled host process");
    Channel channel;
    Check(static_cast<bool>(server.Accept(child.process.Get(), channel)), "authenticate actual client PID user session");
    auto request = RequestMessage();
    Check(static_cast<bool>(channel.Send(request, {nullptr, 5000})), "send request across processes");
    Message response;
    Check(static_cast<bool>(channel.Receive(response, {nullptr, 5000})) && ValidateResponse(request, response), "receive matching response");
    Message cancel{request.request_id, Cancel{}};
    Check(static_cast<bool>(channel.Send(cancel, {nullptr, 5000})), "send explicit cancel frame");
    Check(static_cast<bool>(channel.Receive(response, {nullptr, 5000})) && response == cancel, "cancel returned by host");
    Check(static_cast<bool>(channel.Send(cancel, {nullptr, 5000})), "finish exchange");
    Check(child.Finish(), "host exits cleanly");
}
void RawCase(const wchar_t* mode, bool wrong_identity = false) {
    Server server;
    Check(static_cast<bool>(Server::Create(server)), "fault listener created");
    Child child;
    Check(child.Start(L"--host " + server.Name() + L" " + std::to_wstring(GetCurrentProcessId()) + L" " + mode), "fault host launched");
    Channel channel;
    const auto accepted = server.Accept(wrong_identity ? GetCurrentProcess() : child.process.Get(), channel);
    if (accepted && !wrong_identity) Check(static_cast<bool>(channel.Send(RequestMessage(), {nullptr, 5000})), "release authenticated fault host");
    if (wrong_identity) {
        std::cout << "[DIAG] wrong-identity Accept error=" << accepted.error << std::endl;
        Check(accepted.error == ERROR_ACCESS_DENIED, "reject actual peer versus expected PID mismatch");
    }
    else if (std::wstring(mode) == L"disconnect") {
        if (accepted) { Message m; Check(!channel.Receive(m, {nullptr, 5000}), "disconnect detected"); }
        else Check(accepted.error != ERROR_SUCCESS, "disconnect during authentication detected");
    } else {
        Check(static_cast<bool>(accepted), "fault channel authenticated");
        Message m;
        PipeHandle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        std::thread cancel_thread;
        if (std::wstring(mode) == L"cancel") cancel_thread = std::thread([&] { Sleep(20); SetEvent(cancel.Get()); });
        const auto result = channel.Receive(m, {cancel.Get(), std::wstring(mode) == L"raw" ? DWORD{30} :
            std::wstring(mode) == L"cancel" ? INFINITE : DWORD{5000}});
        if (cancel_thread.joinable()) cancel_thread.join();
        if (std::wstring(mode) == L"fragment") Check(result && m == RequestMessage(), "one-byte writes assembled into complete frame");
        else if (std::wstring(mode) == L"oversize") Check(result.error == ERROR_INVALID_DATA, "overlarge declared payload rejected before allocation");
        else if (std::wstring(mode) == L"raw") Check(result.error == ERROR_TIMEOUT, "pending frame read timeout cancels safely");
        else if (std::wstring(mode) == L"cancel") Check(result.error == ERROR_CANCELLED, "infinite user wait remains cancellable");
        else Check(!result && result.error != ERROR_TIMEOUT, "truncated frame fails on disconnect");
    }
    channel = Channel{};
    Check(child.Finish(), "fault child cleaned up");
}
void BufferedAfterExit(const wchar_t* mode) {
    Server server;
    Check(static_cast<bool>(Server::Create(server)), "buffered exit listener");
    Child child;
    Check(child.Start(L"--host " + server.Name() + L" " + std::to_wstring(GetCurrentProcessId()) + L" " + mode), "buffered exit host");
    Channel channel;
    Check(static_cast<bool>(server.Accept(child.process.Get(), channel)), "buffered exit authenticated");
    const auto request = RequestMessage();
    Check(static_cast<bool>(channel.Send(request, {nullptr, 5000})), "buffered exit request");
    Check(child.Finish(), "host exited before response read");
    Message response;
    const auto result = channel.Receive(response, {nullptr, 5000});
    if (std::wstring(mode) == L"exit-response")
        Check(result && ValidateResponse(request, response), "complete buffered response survives peer exit");
    else Check(!result && result.error != ERROR_TIMEOUT, "truncated response fails after peer exit");
}
int Caller() {
    const HANDLE self = GetCurrentProcess();
    std::cout << "[DIAG] self PID=" << GetProcessId(self) << " single=" << WaitForSingleObject(self, 0)
              << " multiple=" << WaitForMultipleObjects(1, &self, FALSE, 0) << " error=" << GetLastError() << std::endl;
    Exchange();
    BufferedAfterExit(L"exit-response"); BufferedAfterExit(L"truncated-response");
    RawCase(L"fragment"); RawCase(L"raw", true); RawCase(L"raw"); RawCase(L"cancel"); RawCase(L"truncated"); RawCase(L"oversize"); RawCase(L"disconnect");
    Channel channel;
    Check(Connect(L"\\\\remote\\pipe\\Pulse.Picker.1.0123456789abcdef0123456789abcdef", GetCurrentProcess(), channel).error == ERROR_INVALID_NAME,
          "remote pipe name rejected");
    Check(!IsValidPipeName(L"\\\\.\\pipe\\Pulse.Picker.01.0123456789abcdef0123456789abcdef"), "noncanonical PID rejected");
    Check(!IsValidPipeName(L"\\\\.\\pipe\\Pulse.Picker.4294967296.0123456789abcdef0123456789abcdef"), "overflow PID rejected");
    Server server;
    Server::Create(server);
    PipeHandle cancel(CreateEventW(nullptr, TRUE, TRUE, nullptr));
    Check(server.Accept(GetCurrentProcess(), channel, {cancel.Get(), 1000}).error == ERROR_CANCELLED, "connection cancellation distinct from timeout");
    Server::Create(server);
    const auto accept_timeout = server.Accept(GetCurrentProcess(), channel, {nullptr, 30});
    std::cout << "[DIAG] Accept timeout error=" << accept_timeout.error << std::endl;
    Check(accept_timeout.error == ERROR_TIMEOUT, "bounded connection timeout");
    const auto connect_timeout = Connect(server.Name(), GetCurrentProcess(), channel, {nullptr, 30});
    std::cout << "[DIAG] Connect timeout error=" << connect_timeout.error << std::endl;
    Check(connect_timeout.error == ERROR_TIMEOUT, "client retry timeout");
    Check(Connect(server.Name(), GetCurrentProcess(), channel, {cancel.Get(), 1000}).error == ERROR_CANCELLED, "client cancellation");
    Check(WaitOptions{}.timeout_ms == INFINITE, "user wait has no default deadline");

    // Standby host pipe (this process plays both sides).
    {
        wchar_t own_image[MAX_PATH]{};
        GetModuleFileNameW(nullptr, own_image, MAX_PATH);
        Channel standby_client;
        DWORD server_pid = 0;
        const auto absent_start = GetTickCount64();
        const auto absent = ConnectStandby(own_image, {}, standby_client, server_pid);
        Check(absent.error == ERROR_FILE_NOT_FOUND && GetTickCount64() - absent_start < 150, "no standby fails fast for the cold path");
        Check(StandbyPipeName().rfind(L"\\\\.\\pipe\\Pulse.PickerStandby.", 0) == 0, "standby pipe name is local and per user/session");
        Check(StandbyIdleMutexName(L"C:\\A\\pulse.exe") == StandbyIdleMutexName(L"c:\\a\\PULSE.EXE") &&
            StandbyIdleMutexName(L"C:\\A\\pulse.exe") != StandbyIdleMutexName(L"C:\\B\\pulse.exe"), "idle mutex is per bundle, case-insensitive");
        // Verified server: the client sees its own image, so the request flows.
        StandbyServer standby;
        Check(static_cast<bool>(StandbyServer::Create(standby)), "create standby listener");
        Channel host_side;
        DWORD client_pid = 0;
        TransportResult accepted;
        std::thread host([&] { accepted = standby.Accept(host_side, client_pid, {nullptr, 5000}); });
        const auto connected = ConnectStandby(own_image, {}, standby_client, server_pid, {nullptr, 2000});
        host.join();
        Check(connected && accepted && server_pid == GetCurrentProcessId() && client_pid == GetCurrentProcessId(), "verified standby connects both ways");
        Message request;
        request.request_id[0] = 7;
        request.body = Request{};
        if (connected && accepted) {
            Message received;
            Check(standby_client.Send(request, {nullptr, 2000}) && host_side.Receive(received, {nullptr, 2000}) && received == request,
                "request crosses the standby pipe");
        }
        // A listener whose image is not the expected pulse.exe is refused.
        StandbyServer impostor;
        Check(static_cast<bool>(StandbyServer::Create(impostor)), "second standby instance can listen");
        Channel impostor_side, refused_client;
        TransportResult impostor_accept;
        std::thread other([&] { impostor_accept = impostor.Accept(impostor_side, client_pid, {nullptr, 3000}); });
        const auto refused = ConnectStandby(L"C:\\Elsewhere\\pulse.exe", {}, refused_client, server_pid, {nullptr, 2000});
        other.join();
        Check(refused.error == ERROR_ACCESS_DENIED && !refused_client, "standby with another image is refused");
    }
    std::cout << "Checks: " << checks << ", failures: " << failures << std::endl;
    return failures ? 1 : 0;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 5 && std::wstring(argv[1]) == L"--host") return Host(argv[2], wcstoul(argv[3], nullptr, 10), argv[4]);
    if (argc == 2 && std::wstring(argv[1]) == L"--caller") return Caller();
    // All pipe traffic is between a controlled caller child and its host child.
    Child caller;
    if (!caller.Start(L"--caller")) return 1;
    return caller.Finish() ? 0 : 1;
}

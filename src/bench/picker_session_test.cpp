#include "../ipc/picker_session.h"

#include <iostream>
#include <string>

using namespace pulse::ipc::picker;
namespace {
constexpr WaitOptions kWait{nullptr, 5000};
int checks = 0;
int failures = 0;
void Check(bool ok, const std::wstring& name) {
    ++checks;
    if (!ok) ++failures;
    std::wcout << (ok ? L"[PASS] " : L"[FAIL] ") << name << std::endl;
}
struct Child {
    PipeHandle process;
    bool Start(const std::wstring& arguments) {
        std::wstring executable(32768, L'\0');
        executable.resize(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())));
        std::wstring command = L"\"" + executable + L"\" " + arguments;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) return false;
        CloseHandle(info.hThread);
        process.Reset(info.hProcess);
        return true;
    }
    bool Finish() {
        if (!process || WaitForSingleObject(process.Get(), 10000) != WAIT_OBJECT_0) return false;
        DWORD code = 1;
        return GetExitCodeProcess(process.Get(), &code) && code == 0;
    }
    ~Child() {
        if (process && WaitForSingleObject(process.Get(), 0) == WAIT_TIMEOUT) {
            TerminateProcess(process.Get(), 99);
            WaitForSingleObject(process.Get(), 5000);
        }
    }
};
Message RequestMessage() {
    Message request;
    request.request_id[0] = 42;
    return request;
}
Message Ack() { return Message{RequestMessage().request_id, Cancel{}}; }
bool AwaitAck(Channel& channel) {
    Message message;
    return channel.Receive(message, kWait) && message == Ack();
}
int Host(const std::wstring& name, DWORD parent_pid, const std::wstring& mode) {
    PipeHandle parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, parent_pid));
    Channel channel;
    if (!parent || !Connect(name, parent.Get(), channel, kWait)) return 10;
    const auto request = RequestMessage();
    if (mode == L"exchange") {
        Message received;
        if (!ReceivePickerSessionRequest(channel, received, kWait) || received != request) return 11;
        Response response;
        response.status = Status::Accepted;
        response.paths = {u"C:\\session-fixture.txt"};
        if (!channel.Send(Message{request.request_id, response}, kWait)) return 12;
        return AwaitAck(channel) ? 0 : 13;
    }
    if (mode.rfind(L"client-", 0) == 0) {
        Message hello;
        if (!channel.Receive(hello, kWait)) return mode == L"client-cancel" ? 0 : 14;
        if (!std::holds_alternative<SessionHello>(hello.body)) return 15;
        if (mode == L"client-disconnect") return 0;
        if (mode == L"client-timeout") {
            Message ignored;
            return !channel.Receive(ignored, kWait) ? 0 : 16;
        }
        SessionWelcome welcome;
        Message answer{hello.request_id, welcome};
        if (mode == L"client-wrong-id") answer.request_id[0] ^= 1;
        if (mode == L"client-wrong-kind") answer.body = Cancel{};
        if (mode == L"client-zero-version") welcome = SessionWelcome{0, 0, 0};
        if (mode == L"client-no-basic") welcome.features = 0;
        if (mode == L"client-no-caps") welcome.capabilities = 0;
        if (mode != L"client-wrong-kind") answer.body = welcome;
        if (!channel.Send(answer, kWait)) return 17;
        return AwaitAck(channel) ? 0 : 18;
    }
    if (mode == L"legacy") {
        if (!channel.Send(request, kWait)) return 19;
        return AwaitAck(channel) ? 0 : 20;
    }
    SessionHello hello;
    if (mode == L"unknown-version") hello.min_version = hello.max_version = 99;
    if (mode == L"unknown-feature") hello.required_features |= 0x80000000u;
    if (mode == L"unknown-capability") hello.required_capabilities |= 0x80000000u;
    if (!channel.Send(Message{request.request_id, hello}, kWait)) return 21;
    Message received;
    if (!channel.Receive(received, kWait) || !std::holds_alternative<SessionWelcome>(received.body)) return 22;
    const auto& welcome = std::get<SessionWelcome>(received.body);
    if (mode.rfind(L"unknown-", 0) == 0) {
        if (welcome.version != 0) return 23;
        return AwaitAck(channel) ? 0 : 24;
    }
    if (welcome.version != 2) return 25;
    Message next = request;
    if (mode == L"wrong-id") next.request_id[0] ^= 1;
    if (mode == L"capability-drift") {
        auto& body = std::get<Request>(next.body);
        body.operation = Operation::PickFolder;
        body.required_capabilities = Folder;
    }
    if (mode == L"repeat-hello") next.body = hello;
    if (mode == L"cancel") next.body = Cancel{};
    if (!channel.Send(next, kWait)) return 26;
    return AwaitAck(channel) ? 0 : 27;
}
bool Launch(const std::wstring& mode, Server& server, Child& child, Channel& channel) {
    const bool ok = Server::Create(server) &&
        child.Start(L"--host " + server.Name() + L" " + std::to_wstring(GetCurrentProcessId()) + L" " + mode) &&
        server.Accept(child.process.Get(), channel, kWait);
    Check(ok, mode + L": authenticated subprocess channel");
    return ok;
}
void ServerCase(const wchar_t* mode, DWORD expected) {
    Server server;
    Child child;
    Channel channel;
    if (!Launch(mode, server, child, channel)) return;
    Message output{RequestMessage().request_id, Cancel{}};
    const auto sentinel = output;
    const auto result = ReceivePickerSessionRequest(channel, output, kWait);
    Check(result.error == expected, std::wstring(mode) + L": server result");
    Check(expected == ERROR_SUCCESS ? output == RequestMessage() : output == sentinel,
        std::wstring(mode) + L": only validated request published");
    Check(static_cast<bool>(channel.Send(Ack(), kWait)), std::wstring(mode) + L": release peer after validation");
    Check(child.Finish(), std::wstring(mode) + L": child exits cleanly");
}
void ClientCase(const wchar_t* mode, DWORD expected) {
    Server server;
    Child child;
    Channel channel;
    if (!Launch(mode, server, child, channel)) return;
    PipeHandle cancel(CreateEventW(nullptr, TRUE, TRUE, nullptr));
    const std::wstring name(mode);
    const bool cancelled = name == L"client-cancel";
    const bool timeout = name == L"client-timeout";
    const auto started = GetTickCount64();
    const auto result = NegotiatePickerSession(channel, RequestMessage(),
        {cancelled ? cancel.Get() : nullptr, timeout ? DWORD{100} : DWORD{5000}});
    Check(expected == ERROR_BROKEN_PIPE ? !result && result.error != ERROR_TIMEOUT : result.error == expected,
        name + L": client result");
    Check(GetTickCount64() - started < 6000, name + L": bounded completion");
    if (!cancelled && !timeout && name != L"client-disconnect")
        Check(static_cast<bool>(channel.Send(Ack(), kWait)), name + L": release peer after validation");
    channel = Channel{};
    Check(child.Finish(), name + L": child exits cleanly");
}
void Exchange() {
    Server server;
    Child child;
    Channel channel;
    if (!Launch(L"exchange", server, child, channel)) return;
    const auto request = RequestMessage();
    Check(static_cast<bool>(NegotiatePickerSession(channel, request, kWait)), L"successful hello/welcome negotiation");
    Check(static_cast<bool>(channel.Send(request, kWait)), L"request sent only after negotiation");
    Message response;
    Check(channel.Receive(response, kWait) && ValidateResponse(request, response), L"negotiated request receives bound response");
    Check(static_cast<bool>(channel.Send(Ack(), kWait)), L"response acknowledged before child exit");
    Check(child.Finish(), L"successful session cleaned up");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 5 && std::wstring(argv[1]) == L"--host")
        return Host(argv[2], wcstoul(argv[3], nullptr, 10), argv[4]);
    Exchange();
    ServerCase(L"legacy", ERROR_SUCCESS);
    ServerCase(L"unknown-version", ERROR_NOT_SUPPORTED);
    ServerCase(L"unknown-feature", ERROR_NOT_SUPPORTED);
    ServerCase(L"unknown-capability", ERROR_NOT_SUPPORTED);
    ServerCase(L"wrong-id", ERROR_INVALID_DATA);
    ServerCase(L"capability-drift", ERROR_INVALID_DATA);
    ServerCase(L"repeat-hello", ERROR_INVALID_DATA);
    ServerCase(L"cancel", ERROR_CANCELLED);
    ClientCase(L"client-wrong-id", ERROR_INVALID_DATA);
    ClientCase(L"client-wrong-kind", ERROR_INVALID_DATA);
    ClientCase(L"client-zero-version", ERROR_NOT_SUPPORTED);
    ClientCase(L"client-no-basic", ERROR_INVALID_DATA);
    ClientCase(L"client-no-caps", ERROR_INVALID_DATA);
    ClientCase(L"client-disconnect", ERROR_BROKEN_PIPE);
    ClientCase(L"client-cancel", ERROR_CANCELLED);
    ClientCase(L"client-timeout", ERROR_TIMEOUT);
    std::cout << "Checks: " << checks << ", failures: " << failures << std::endl;
    return failures ? 1 : 0;
}

#include "picker_client.h"
#include "../common/picker_owner_guard.h"
#include <array>

#include <filesystem>
#include <thread>

namespace pulse::ipc::picker {
namespace {
Message Terminal(const Message& request, Status status, DWORD error = ERROR_SUCCESS) {
    Response response;
    if (const auto* body = std::get_if<Request>(&request.body)) response.operation = body->operation;
    response.status = status;
    response.error_code = error;
    response.error_source = error ? ErrorSource::Win32 : ErrorSource::None;
    return {request.request_id, std::move(response)};
}
struct ClientGuard {
    bool& active;
    explicit ClientGuard(bool& value) : active(value) { active = true; }
    ~ClientGuard() { active = false; }
};

struct Child {
    PipeHandle job;
    PipeHandle process;
    ~Child() {
        // Closing this private job cannot terminate the caller or unrelated apps.
        job.Reset();
        if (process) WaitForSingleObject(process.Get(), 5000);
    }
};
Message Exchange(const std::wstring& executable, const std::wstring& desktop,
                 const Message& request, HANDLE cancel) {
    Server server;
    auto transport = Server::Create(server);
    if (!transport) return Terminal(request, Status::Failed, transport.error);
    Child child;
    child.job.Reset(CreateJobObjectW(nullptr, nullptr));
    if (!child.job) return Terminal(request, Status::Failed, GetLastError());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(child.job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return Terminal(request, Status::Failed, GetLastError());
    std::wstring command = L"\"" + executable + L"\" --picker-host " + server.Name() + L" " +
        std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = const_cast<wchar_t*>(desktop.c_str());
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                        nullptr, nullptr, &startup, &info))
        return Terminal(request, Status::Failed, GetLastError());
    child.process.Reset(info.hProcess);
    PipeHandle thread(info.hThread);
    if (!AssignProcessToJobObject(child.job.Get(), child.process.Get())) {
        const DWORD error = GetLastError();
        TerminateProcess(child.process.Get(), error); // Only our still-suspended child.
        return Terminal(request, Status::Failed, error);
    }
    if (ResumeThread(thread.Get()) == static_cast<DWORD>(-1))
        return Terminal(request, Status::Failed, GetLastError());
    Channel channel;
    transport = server.Accept(child.process.Get(), channel, {cancel, 10000});
    if (transport) transport = channel.Send(request, {cancel, 10000});
    Message response;
    if (transport) transport = channel.Receive(response, {cancel, INFINITE});
    if (!transport) return Terminal(request, transport.error == ERROR_CANCELLED ? Status::Cancelled : Status::Failed,
                                    transport.error == ERROR_CANCELLED ? ERROR_SUCCESS : transport.error);
    if (!ValidateResponse(request, response)) return Terminal(request, Status::Failed, ERROR_INVALID_DATA);
    // No result reaches the caller until the private host is cleaned up.
    WaitForSingleObject(child.process.Get(), 2000);
    return response;
}
}

Message RunPickerClient(std::wstring executable, Message request, HWND owner, HANDLE cancel) {
    static thread_local bool active = false;
    if (active) return Terminal(request, Status::Failed, ERROR_BUSY);
    ClientGuard call(active);
    std::vector<uint8_t> encoded;
    if (!std::holds_alternative<Request>(request.body) || Encode(request, encoded) != CodecError::None)
        return Terminal(request, Status::Failed, ERROR_INVALID_DATA);
    if (owner && !IsWindow(owner))
        return Terminal(request, Status::Failed, ERROR_INVALID_WINDOW_HANDLE);
    if (executable.find(L'"') != std::wstring::npos || !std::filesystem::path(executable).is_absolute())
        return Terminal(request, Status::Failed, ERROR_INVALID_NAME);
    if (cancel) {
        const DWORD state = WaitForSingleObject(cancel, 0);
        if (state == WAIT_OBJECT_0) return Terminal(request, Status::Cancelled);
        if (state != WAIT_TIMEOUT) return Terminal(request, Status::Failed, ERROR_INVALID_HANDLE);
    }
    wchar_t desktop[32768]{};
    DWORD needed = 0;
    if (!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof(desktop), &needed))
        return Terminal(request, Status::Failed, GetLastError());
    PipeHandle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    PipeHandle done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stop || !done) return Terminal(request, Status::Failed, GetLastError());
    Message response = Terminal(request, Status::Failed, ERROR_GEN_FAILURE);
    pulse::PickerOwnerGuard modality(owner);
    if (modality.Error()) return Terminal(request, Status::Failed, modality.Error());
    if (!modality.Alive()) return Terminal(request, Status::Cancelled);
    std::thread worker;
    try {
        worker = std::thread([&] {
            try { response = Exchange(executable, desktop, request, stop.Get()); }
            catch (...) { response = Terminal(request, Status::Failed, ERROR_NOT_ENOUGH_MEMORY); }
            SetEvent(done.Get());
        });
    } catch (const std::system_error&) { return Terminal(request, Status::Failed, ERROR_NOT_ENOUGH_MEMORY); }
    bool quit = false;
    int quit_code = 0;
    DWORD pump_error = ERROR_SUCCESS;
    bool cancelling = false;
    for (;;) {
        if (quit) {
            WaitForSingleObject(done.Get(), INFINITE); // Worker cleanup never requires caller UI dispatch.
            break;
        }
        HANDLE handles[]{done.Get(), cancel};
        const DWORD count = cancel && !cancelling ? 2 : 1;
        const DWORD wait = MsgWaitForMultipleObjectsEx(count, handles, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) { pump_error = GetLastError(); SetEvent(stop.Get()); cancelling = true; }
        if ((count == 2 && wait == WAIT_OBJECT_0 + 1) || !modality.Alive()) {
            SetEvent(stop.Get()); cancelling = true;
        }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                quit = true; quit_code = static_cast<int>(message.wParam);
                SetEvent(stop.Get()); cancelling = true;
                break;
            } else {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (WaitForSingleObject(done.Get(), 0) == WAIT_OBJECT_0) break;
        }
    }
    worker.join();
    // done may race cancellation, owner destruction or an already queued quit.
    MSG pending_quit{};
    if (!quit && PeekMessageW(&pending_quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {
        quit = true;
        quit_code = static_cast<int>(pending_quit.wParam);
    }
    if (quit) PostQuitMessage(quit_code);
    if (pump_error) return Terminal(request, Status::Failed, pump_error);
    if (cancelling || quit || !modality.Alive() ||
        (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0))
        return Terminal(request, Status::Cancelled);
    return response;
}
} // namespace pulse::ipc::picker

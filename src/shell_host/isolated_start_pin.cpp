#include "isolated_start_pin.h"
#include "../common/current_user_security.h"
#include <mutex>
#include <shellapi.h>
#include <string>
#include <utility>
namespace pulse::shell {
namespace {
constexpr DWORD limit = 1024 * 1024;
struct IdleChild {
    std::mutex mutex;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    ~IdleChild() {
        if (pipe != INVALID_HANDLE_VALUE)
            CloseHandle(pipe);
        if (job)
            CloseHandle(job);
        if (process)
            CloseHandle(process);
    }
};
IdleChild &Pool() {
    static IdleChild pool;
    return pool;
}
HRESULT Error() {
    return HRESULT_FROM_WIN32(GetLastError());
}
HRESULT Transfer(HANDLE pipe, void *data, DWORD size, bool write, HANDLE cancel, ULONGLONG deadline,
                 bool pump = false) {
    while (size) {
        if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        if (GetTickCount64() >= deadline)
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent)
            return Error();
        DWORD done = 0;
        BOOL ok = write ? WriteFile(pipe, data, size, &done, &ov) : ReadFile(pipe, data, size, &done, &ov);
        HRESULT hr = S_OK;
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            HANDLE waits[] = {ov.hEvent, cancel};
            DWORD wait = WAIT_TIMEOUT;
            for (;;) {
                const auto current = GetTickCount64();
                DWORD remaining = current < deadline ? static_cast<DWORD>(deadline - current) : 0;
                wait = pump ? MsgWaitForMultipleObjectsEx(cancel ? 2 : 1, waits, remaining, QS_ALLINPUT,
                                                          MWMO_INPUTAVAILABLE)
                            : WaitForMultipleObjects(cancel ? 2 : 1, waits, FALSE, remaining);
                if (!pump || wait != WAIT_OBJECT_0 + (cancel ? 2u : 1u))
                    break;
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            if (wait != WAIT_OBJECT_0) {
                hr = HRESULT_FROM_WIN32(wait == WAIT_OBJECT_0 + 1 ? ERROR_CANCELLED : ERROR_TIMEOUT);
                CancelIoEx(pipe, &ov);
                GetOverlappedResult(pipe, &ov, &done, TRUE);
            } else if (!GetOverlappedResult(pipe, &ov, &done, FALSE))
                hr = Error();
        } else if (!ok)
            hr = Error();
        CloseHandle(ov.hEvent);
        if (FAILED(hr))
            return hr;
        if (!done)
            return HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE);
        data = static_cast<uint8_t *>(data) + done;
        size -= done;
    }
    return S_OK;
}
} // namespace
void IsolatedStartPinSession::Destroy() {
    if (pipe_ != INVALID_HANDLE_VALUE)
        CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
    // EOF lets the STA release its menu and COM objects before the job fallback.
    if (process_ && WaitForSingleObject(process_, 500) != WAIT_OBJECT_0) {
        TerminateProcess(process_, 0);
        WaitForSingleObject(process_, 1000);
    }
    if (job_)
        CloseHandle(job_);
    job_ = nullptr;
    if (process_)
        CloseHandle(process_);
    process_ = nullptr;
}
void IsolatedStartPinSession::Close() {
    if (!process_ || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT) {
        Destroy();
        return;
    }
    ipc::PayloadWriter request;
    request.PutU32(3);
    std::vector<uint8_t> response;
    HRESULT hr = Exchange(request, response, nullptr, 250);
    ipc::PayloadReader reader(response.data(), response.size());
    uint32_t result = 0;
    if (FAILED(hr) || !reader.GetU32(result) || result != S_OK || reader.remaining()) {
        Destroy();
        return;
    }
    ReturnIdle();
}
void IsolatedStartPinSession::ReturnIdle() {
    auto &pool = Pool();
    {
        std::lock_guard lock(pool.mutex);
        if (!pool.process) {
            pool.pipe = std::exchange(pipe_, INVALID_HANDLE_VALUE);
            pool.process = std::exchange(process_, nullptr);
            pool.job = std::exchange(job_, nullptr);
        }
    }
    // A second active session never waits for the pooled one; surplus children exit here.
    Destroy();
}
IsolatedStartPinSession::~IsolatedStartPinSession() {
    Close();
}
HRESULT IsolatedStartPinSession::Exchange(const ipc::PayloadWriter &request, std::vector<uint8_t> &response,
                                          HANDLE cancel, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    DWORD size = static_cast<DWORD>(request.data().size());
    if (size > limit)
        return E_INVALIDARG;
    HRESULT hr = Transfer(pipe_, &size, sizeof(size), true, cancel, deadline);
    if (SUCCEEDED(hr))
        hr = Transfer(pipe_, const_cast<uint8_t *>(request.data().data()), size, true, cancel, deadline);
    if (SUCCEEDED(hr))
        hr = Transfer(pipe_, &size, sizeof(size), false, cancel, deadline);
    if (SUCCEEDED(hr) && size > limit)
        hr = E_INVALIDARG;
    if (SUCCEEDED(hr)) {
        response.resize(size);
        hr = Transfer(pipe_, response.data(), size, false, cancel, deadline);
    }
    if (FAILED(hr))
        Destroy();
    return hr;
}
void IsolatedStartPinSession::Prewarm() {
    IsolatedStartPinSession session;
    if (SUCCEEDED(session.AcquireProcess()))
        session.ReturnIdle();
    else
        session.Destroy();
}
HRESULT IsolatedStartPinSession::AcquireProcess() {
    auto &pool = Pool();
    {
        std::lock_guard lock(pool.mutex);
        pipe_ = std::exchange(pool.pipe, INVALID_HANDLE_VALUE);
        process_ = std::exchange(pool.process, nullptr);
        job_ = std::exchange(pool.job, nullptr);
    }
    if (process_ && WaitForSingleObject(process_, 0) != WAIT_TIMEOUT)
        Destroy();
    HRESULT hr = S_OK;
    if (!process_) {
        GUID guid{};
        CoCreateGuid(&guid);
        wchar_t guid_text[40]{};
        StringFromGUID2(guid, guid_text, 40);
        std::wstring name = L"\\\\.\\pipe\\pulse_start_pin_" + std::wstring(guid_text);
        CurrentUserSecurityAttributes security;
        if (!security)
            return E_ACCESSDENIED;
        pipe_ = CreateNamedPipeW(
            name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, limit, limit, 0, security.get());
        if (pipe_ == INVALID_HANDLE_VALUE)
            return Error();
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE child_pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, &sa, OPEN_EXISTING,
                                        FILE_FLAG_OVERLAPPED, nullptr);
        if (child_pipe == INVALID_HANDLE_VALUE) {
            HRESULT create_error = Error();
            Destroy();
            return create_error;
        }
        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION ji{};
        ji.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &ji, sizeof(ji))) {
            HRESULT create_error = Error();
            CloseHandle(child_pipe);
            Destroy();
            return create_error;
        }
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<uint8_t> attrs(bytes);
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
        BOOL initialized = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &bytes);
        BOOL ready =
            initialized && UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                     &child_pipe, sizeof(child_pipe), nullptr, nullptr);
        wchar_t exe[32768]{};
        GetModuleFileNameW(nullptr, exe, 32768);
        std::wstring path(exe);
        path.resize(path.find_last_of(L"\\/") + 1);
        path += L"pulse_shell.exe";
        std::wstring cmd = L"\"" + path + L"\" --isolated-start-pin " +
                           std::to_wstring(reinterpret_cast<uintptr_t>(child_pipe));
        PROCESS_INFORMATION pi{};
        BOOL created =
            ready && CreateProcessW(path.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                    EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                    nullptr, nullptr, &si.StartupInfo, &pi);
        hr = created ? S_OK : Error();
        if (initialized)
            DeleteProcThreadAttributeList(si.lpAttributeList);
        CloseHandle(child_pipe);
        if (created) {
            process_ = pi.hProcess;
            if (!AssignProcessToJobObject(job_, process_) ||
                ResumeThread(pi.hThread) == static_cast<DWORD>(-1))
                hr = Error();
            CloseHandle(pi.hThread);
        }
        if (FAILED(hr)) {
            Destroy();
            return hr;
        }
    }
    return S_OK;
}
HRESULT IsolatedStartPinSession::Query(const std::vector<std::wstring> &paths, HWND owner, UINT first,
                                       UINT last, UINT flags, HANDLE cancel, std::vector<CtxItemOut> &out) {
    Close();
    out.clear();
    if (paths.empty() || paths.size() > 4096 || first > last || last > 0x7fff)
        return E_INVALIDARG;
    if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0)
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    HRESULT hr = AcquireProcess();
    if (FAILED(hr))
        return hr;
    ipc::PayloadWriter w;
    w.PutU32(1);
    w.PutU64(reinterpret_cast<uintptr_t>(owner));
    w.PutU32(first);
    w.PutU32(last);
    w.PutU32(flags);
    w.PutStringArray(paths);
    std::vector<uint8_t> reply;
    hr = Exchange(w, reply, cancel);
    if (FAILED(hr))
        return hr;
    ipc::PayloadReader r(reply.data(), reply.size());
    uint32_t result = 0, count = 0;
    if (!r.GetU32(result)) {
        Destroy();
        return E_FAIL;
    }
    if (FAILED(static_cast<HRESULT>(result))) {
        Destroy();
        return static_cast<HRESULT>(result);
    }
    if (!r.GetU32(count) || count > 4096) {
        Destroy();
        return E_FAIL;
    }
    std::vector<CtxItemOut> items(count);
    for (auto &item : items) {
        uint32_t bits = 0, mnemonic = 0;
        if (!r.GetU32(item.id) || !r.GetU32(bits) || !r.GetU32(mnemonic) || !r.GetString(item.verb) ||
            !r.GetString(item.text) || !r.GetString(item.clsid) || !r.GetString(item.handler)) {
            Destroy();
            return E_FAIL;
        }
        if (item.id && (item.id < first || item.id > last)) {
            Destroy();
            return E_FAIL;
        }
        item.enabled = (bits & 1) != 0;
        item.separator_after = (bits & 2) != 0;
        item.has_children = (bits & 4) != 0;
        item.child = (bits & 8) != 0;
        item.depth = static_cast<uint8_t>((bits >> 8) & 0xFF);
        item.mnemonic = static_cast<wchar_t>(mnemonic);
    }
    if (!ipc::TryCtxItemIcons(r, items) || r.remaining()) {
        Destroy();
        return E_FAIL;
    }
    out = std::move(items);
    return static_cast<HRESULT>(result);
}
HRESULT IsolatedStartPinSession::Invoke(UINT id) {
    ipc::PayloadWriter w;
    w.PutU32(2);
    w.PutU32(id);
    std::vector<uint8_t> reply;
    HRESULT hr = Exchange(w, reply, nullptr);
    uint32_t result = 0;
    ipc::PayloadReader r(reply.data(), reply.size());
    if (SUCCEEDED(hr))
        hr = r.GetU32(result) && !r.remaining() ? static_cast<HRESULT>(result) : E_FAIL;
    Close();
    return hr;
}
HRESULT InvokeIsolatedStartPinItem(const CtxHandlerSlot &slot, const std::vector<CtxItemOut> &items,
                                   HWND owner, UINT id) {
    if (!slot.menu || id < slot.id_first || id > slot.id_last)
        return E_INVALIDARG;
    for (const auto &item : items) {
        if (item.id != id || !item.enabled || item.has_children)
            continue;
        CMINVOKECOMMANDINFOEX info{};
        info.cbSize = sizeof(info);
        info.fMask = CMIC_MASK_UNICODE;
        info.hwnd = owner;
        info.lpVerb = MAKEINTRESOURCEA(id - slot.id_first);
        info.lpVerbW = MAKEINTRESOURCEW(id - slot.id_first);
        info.nShow = SW_SHOWNORMAL;
        return slot.menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO *>(&info));
    }
    return E_INVALIDARG;
}
int RunIsolatedStartPin(int argc, wchar_t **argv) {
    if (argc != 3)
        return 2;
    wchar_t *end = nullptr;
    auto value = _wcstoui64(argv[2], &end, 10);
    if (!value || !end || *end)
        return 2;
    HANDLE pipe = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
    HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(apartment))
        return 3;
    {
        std::unique_ptr<CtxBind> bind;
        CtxHandlerSlot slot;
        std::vector<CtxItemOut> items;
        HWND owner = nullptr;
        auto reset = [&] {
            ReleaseHandlerSlot(slot);
            slot = {};
            items.clear();
            bind.reset();
            owner = nullptr;
        };
        for (;;) {
            const ULONGLONG deadline = GetTickCount64() + 120000;
            DWORD size = 0;
            if (FAILED(Transfer(pipe, &size, sizeof(size), false, nullptr, deadline, true)) || size > limit)
                break;
            std::vector<uint8_t> payload(size);
            if (FAILED(Transfer(pipe, payload.data(), size, false, nullptr, deadline, true)))
                break;
            ipc::PayloadReader r(payload.data(), payload.size());
            uint32_t op = 0;
            HRESULT hr = E_INVALIDARG;
            ipc::PayloadWriter w;
            if (r.GetU32(op) && op == 1) {
                reset();
                bind = std::make_unique<CtxBind>();
                uint64_t hwnd = 0;
                uint32_t first = 0, last = 0, flags = 0, count = 0;
                std::vector<std::wstring> paths;
                bool valid = r.GetU64(hwnd) && r.GetU32(first) && r.GetU32(last) && r.GetU32(flags) &&
                             r.GetU32(count) && count > 0 && count <= 4096 && first <= last && last <= 0x7fff;
                for (uint32_t i = 0; valid && i < count; ++i) {
                    std::wstring p;
                    valid = r.GetString(p);
                    paths.push_back(std::move(p));
                }
                if (valid && !r.remaining()) {
                    owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(hwnd));
                    if (BindCtxSelection(paths, false, *bind)) {
                        CtxHandlerDesc desc;
                        desc.clsid_text = L"{470c0ebd-5d73-4d58-9ced-e91e22e23282}";
                        CLSIDFromString(desc.clsid_text.c_str(), &desc.clsid);
                        desc.name = L"Start pin";
                        hr = QueryOneHandler(desc, *bind, owner, first, last, flags, slot);
                        if (SUCCEEDED(hr))
                            CollectHandlerItems(slot, false, items);
                    } else
                        hr = E_FAIL;
                }
                w.PutU32(static_cast<uint32_t>(hr));
                if (SUCCEEDED(hr)) {
                    w.PutU32(static_cast<uint32_t>(items.size()));
                    for (const auto &item : items) {
                        w.PutU32(item.id);
                        w.PutU32((item.enabled ? 1u : 0u) | (item.separator_after ? 2u : 0u) |
                                 (item.has_children ? 4u : 0u) | (item.child ? 8u : 0u) | (uint32_t(item.depth) << 8));
                        w.PutU32(item.mnemonic);
                        w.PutString(item.verb);
                        w.PutString(item.text);
                        w.PutString(item.clsid);
                        w.PutString(item.handler);
                    }
                    ipc::PutCtxItemIcons(w, items);
                }
            } else {
                uint32_t id = 0;
                if (op == 2 && r.GetU32(id) && !r.remaining() && slot.menu) {
                    hr = InvokeIsolatedStartPinItem(slot, items, owner, id);
                }
                if (op == 3 && !r.remaining())
                    hr = S_OK;
                reset();
                w.PutU32(static_cast<uint32_t>(hr));
            }
            size = static_cast<DWORD>(w.data().size());
            if (size > limit || FAILED(Transfer(pipe, &size, sizeof(size), true, nullptr, deadline, true)) ||
                FAILED(Transfer(pipe, const_cast<uint8_t *>(w.data().data()), size, true, nullptr, deadline,
                                true)))
                break;
        }
        reset();
    }
    CloseHandle(pipe);
    CoUninitialize();
    return 0;
}
} // namespace pulse::shell

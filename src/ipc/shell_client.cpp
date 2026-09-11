// shell_client.cpp — See shell_client.h for the contract.
#include "shell_client.h"

namespace pulse::ipc {

ShellClient& ShellClient::Instance() {
    static ShellClient inst;
    return inst;
}

ShellClient::~ShellClient() {
    Stop();
}

void ShellClient::Start(Callbacks cb) {
    if (running_.exchange(true)) return;
    cb_ = std::move(cb);
    reader_ = std::thread([this] { ReaderThread(); });
}

void ShellClient::Stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        // Connection publication and read initiation use this same lock. Once
        // cancelled, no reader can issue another operation on this pipe.
        if (pipe_ != INVALID_HANDLE_VALUE) CancelIoEx(pipe_, nullptr);
    }
    if (reader_.joinable()) reader_.join();
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        if (pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
        KillChild();
    }
}

void ShellClient::KillChild() {
    if (child_started_) {
        TerminateProcess(child_.hProcess, 1);
        CloseHandle(child_.hProcess);
        CloseHandle(child_.hThread);
        child_ = {};
        child_started_ = false;
    }
}

bool ShellClient::SpawnChild() {
    KillChild();

    const ULONGLONG now = GetTickCount64();
    if (last_spawn_try_ != 0 && now - last_spawn_try_ < 1500) return false;
    last_spawn_try_ = now;

    wchar_t exe_dir[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_dir, ARRAYSIZE(exe_dir));
    wchar_t* slash = wcsrchr(exe_dir, L'\\');
    if (!slash) return false;
    *(slash + 1) = L'\0';

    std::wstring cmd = L"\"";
    cmd += exe_dir;
    cmd += L"pulse_shell.exe\" ";
    cmd += std::to_wstring(GetCurrentProcessId());

    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        const DWORD gle = GetLastError();
        fprintf(stderr, "[shell_client] CreateProcess failed gle=%lu cmd=%ls\n", gle, cmd.c_str());
        if (gle == ERROR_FILE_NOT_FOUND) {
            last_error_ = L"\u627E\u4E0D\u5230 pulse_shell.exe\uFF08\u9700\u4E0E Pulse \u653E\u5728\u540C\u4E00\u76EE\u5F55\uFF09";
        } else {
            last_error_ = L"\u65E0\u6CD5\u542F\u52A8 pulse_shell.exe\uFF08\u9519\u8BEF " +
                          std::to_wstring(gle) + L"\uFF09";
        }
        return false;
    }
    child_ = pi;
    child_started_ = true;
    last_error_.clear();
    return true;
}

// Caller must hold send_mutex_.
bool ShellClient::EnsureConnected() {
    if (!running_.load()) return false;
    if (pipe_ != INVALID_HANDLE_VALUE) return true;

    std::wstring name = PipeNameFor(GetCurrentProcessId());
    if (!child_started_ && !SpawnChild()) return false;
    HANDLE h = INVALID_HANDLE_VALUE;
    // WaitNamedPipe returns ERROR_FILE_NOT_FOUND before the child creates its
    // pipe, so retry in a bounded loop.
    const auto deadline = GetTickCount64() + 8000;
    while (true) {
        if (!running_.load()) return false;
        h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            ULONG server_pid = 0;
            if (GetNamedPipeServerProcessId(h, &server_pid) &&
                server_pid == child_.dwProcessId)
                break;
            CloseHandle(h);
            last_error_ = L"pulse_shell.exe pipe identity mismatch";
            return false;
        }
        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(name.c_str(), 200);
            if (GetTickCount64() > deadline) {
                last_error_ = L"pulse_shell.exe \u7BA1\u9053\u5FD9\u788C";
                return false;
            }
            continue;
        }
        if (err == ERROR_FILE_NOT_FOUND) {
            if (WaitForSingleObject(child_.hProcess, 0) == WAIT_OBJECT_0) {
                DWORD code = 0;
                GetExitCodeProcess(child_.hProcess, &code);
                fprintf(stderr, "[shell_client] child exited early code=%lu\n", code);
                last_error_ = L"pulse_shell.exe \u542F\u52A8\u540E\u7ACB\u5373\u9000\u51FA";
                return false;
            }
            if (GetTickCount64() > deadline) {
                last_error_ = L"pulse_shell.exe \u672A\u54CD\u5E94\u7BA1\u9053";
                return false;
            }
            Sleep(50);
            continue;
        }
        fprintf(stderr, "[shell_client] pipe CreateFile err=%lu\n", err);
        return false;
    }
    if (!running_.load()) { CloseHandle(h); return false; }
    pipe_ = h;
    return true;
}

// Caller must hold send_mutex_.
bool ShellClient::SendFrame(uint32_t type, uint32_t id, const std::vector<uint8_t>& payload) {
    if (!running_.load()) return false;
    MsgHeader h;
    h.type = type;
    h.request_id = id;
    h.payload_size = (uint32_t)payload.size();

    auto write_all = [this](const void* data, DWORD size) {
        bool ok = PipeWrite(pipe_, static_cast<const uint8_t*>(data), size);
        if (!ok)
            fprintf(stderr, "[client] WriteFile size=%lu failed gle=%lu\n", size, GetLastError());
        return ok;
    };

    if (!write_all(&h, sizeof(h))) return false;
    if (!payload.empty() && !write_all(payload.data(), (DWORD)payload.size())) return false;
    return true;
}

uint32_t ShellClient::Submit(uint32_t type, const std::vector<uint8_t>& payload) {
    if (!running_.load()) return 0;

    Pending p;
    p.type = type;
    p.payload = payload;
    p.id = next_id_.fetch_add(1);

    bool sent = false;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        if (EnsureConnected()) {
            std::lock_guard<std::mutex> pending_lock(pending_mutex_);
            pending_.emplace(p.id, p);
            sent = SendFrame(p.type, p.id, p.payload);
            if (!sent) {
                // Pipe broke mid-send: restart the child and retry once.
                pending_.find(p.id)->second.retried = true;
                CloseHandle(pipe_);
                pipe_ = INVALID_HANDLE_VALUE;
                KillChild();
                if (EnsureConnected())
                    sent = SendFrame(p.type, p.id, p.payload);
            }
            if (!sent) pending_.erase(p.id);
        }
    }
    if (!sent) {
        FireDone(p.id, HRESULT_FROM_WIN32(ERROR_PIPE_NOT_CONNECTED), false,
                 UnreachableMessage());
        return p.id;
    }
    return p.id;
}

uint32_t ShellClient::DeleteRecycle(const std::vector<std::wstring>& paths) {
    PayloadWriter w;
    w.PutStringArray(paths);
    return Submit(REQ_DELETE_RECYCLE, w.data());
}

uint32_t ShellClient::RealDelete(const std::vector<std::wstring>& paths) {
    PayloadWriter w;
    w.PutStringArray(paths);
    return Submit(REQ_REALDELETE, w.data());
}

uint32_t ShellClient::RestoreRecycle(const std::vector<std::wstring>& paths) {
    PayloadWriter w;
    w.PutStringArray(paths);
    return Submit(REQ_RESTORE_RECYCLE, w.data());
}

uint32_t ShellClient::Rename(const std::wstring& path, const std::wstring& new_name) {
    PayloadWriter w;
    w.PutString(path);
    w.PutString(new_name);
    return Submit(REQ_RENAME, w.data());
}

uint32_t ShellClient::CreateFolder(const std::wstring& path) {
    PayloadWriter w;
    w.PutString(path);
    return Submit(REQ_NEW_FOLDER, w.data());
}

uint32_t ShellClient::CreateNewFile(const std::wstring& path) {
    PayloadWriter w;
    w.PutString(path);
    return Submit(REQ_NEW_FILE, w.data());
}

void ShellClient::Cancel(uint32_t id) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (pipe_ == INVALID_HANDLE_VALUE) return;
    SendFrame(REQ_CANCEL, id, {});
}

void ShellClient::Abort(uint32_t id) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        {
            std::lock_guard<std::mutex> pending_lock(pending_mutex_);
            removed = pending_.erase(id) != 0;
        }
        if (pipe_ != INVALID_HANDLE_VALUE) {
            CancelIoEx(pipe_, nullptr);
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
        KillChild();
    }
    if (removed) {
        FireDone(id, HRESULT_FROM_WIN32(ERROR_TIMEOUT), false,
                 L"shell host timed out");
    }
}

uint32_t ShellClient::QueryContextMenu(const std::vector<std::wstring>& paths,
                                       uint32_t owner_hwnd, bool background, bool extended,
                                       const std::vector<std::wstring>& disabled_clsids) {
    PayloadWriter w;
    w.PutU32(owner_hwnd);
    uint32_t flags = 0;
    if (extended) flags |= CTXF_EXTENDED;
    if (background) flags |= CTXF_BACKGROUND;
    w.PutU32(flags);
    w.PutStringArray(paths);
    w.PutStringArray(disabled_clsids);
    return Submit(REQ_CTX_QUERY, w.data());
}

uint32_t ShellClient::InvokeContextMenu(uint32_t session_id, uint32_t item_id,
                                        const std::wstring& verb, const std::wstring& text) {
    PayloadWriter w;
    w.PutU32(session_id);
    w.PutU32(item_id);
    w.PutString(verb);
    w.PutString(text);
    return Submit(REQ_CTX_INVOKE, w.data());
}

// Fire-and-forget like Cancel: no pending entry, no retry-on-restart (a fresh
// host has no such session anyway).
void ShellClient::CloseContextMenu(uint32_t session_id) {
    PayloadWriter w;
    w.PutU32(session_id);
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (pipe_ == INVALID_HANDLE_VALUE) return;
    SendFrame(REQ_CTX_CLOSE, 0, w.data());
}

bool ShellClient::Ping() {
    return Submit(REQ_PING, {}) != 0;
}

void ShellClient::FireDone(uint32_t id, uint32_t hr, bool cancelled, const std::wstring& error) {
    if (cb_.done) cb_.done(id, hr, cancelled, error);
}

const std::wstring& ShellClient::UnreachableMessage() const {
    static const std::wstring kFallback = L"shell host unreachable";
    return last_error_.empty() ? kFallback : last_error_;
}

static bool ReadFull(HANDLE pipe, void* out, DWORD size,
                     std::mutex& send_mutex, const std::atomic<bool>& running) {
    auto* bytes = static_cast<uint8_t*>(out);
    while (size) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;
        DWORD got = 0, error = ERROR_OPERATION_ABORTED;
        BOOL ok = FALSE;
        {
            std::lock_guard<std::mutex> lock(send_mutex);
            if (running.load()) {
                ok = ReadFile(pipe, bytes, size, &got, &overlapped);
                if (!ok) error = GetLastError();
            }
        }
        // Stop cancels while holding send_mutex, but joins outside it. Keep the
        // OVERLAPPED and event alive until cancellation has actually completed.
        if (!ok && error == ERROR_IO_PENDING)
            ok = GetOverlappedResult(pipe, &overlapped, &got, TRUE);
        CloseHandle(overlapped.hEvent);
        if (!ok || !got) return false;
        bytes += got; size -= got;
    }
    return true;
}

void ShellClient::ReaderThread() {
    while (running_.load()) {
        HANDLE pipe;
        {
            std::lock_guard<std::mutex> lock(send_mutex_);
            if (!EnsureConnected()) {
                // Child won't start: back off and retry while running.
                pipe = INVALID_HANDLE_VALUE;
            } else {
                pipe = pipe_;
            }
        }
        if (pipe == INVALID_HANDLE_VALUE) {
            for (int i = 0; i < 30 && running_.load(); ++i) Sleep(50);
            continue;
        }

        while (running_.load()) {
            MsgHeader h{};
            if (!ReadFull(pipe, &h, sizeof(h), send_mutex_, running_)) break;
            if (h.magic != kMagic || h.payload_size > kMaxPayload) break;
            std::vector<uint8_t> payload(h.payload_size);
            if (h.payload_size && !ReadFull(pipe, payload.data(), h.payload_size, send_mutex_, running_)) break;

            PayloadReader r(payload.data(), payload.size());
            if (h.type == RSP_PROGRESS) {
                float pct = 0.0f;
                std::wstring item;
                uint32_t items_done = 0, total_items = 0;
                if (r.GetF32(pct) && r.GetString(item) &&
                    r.GetU32(items_done) && r.GetU32(total_items) && cb_.progress)
                    cb_.progress(h.request_id, pct, item, items_done, total_items);
            } else if (h.type == RSP_DONE) {
                uint32_t hr = 0, cancelled = 0;
                std::wstring err;
                if (r.GetU32(hr) && r.GetU32(cancelled) && r.GetString(err)) {
                    bool pending = false;
                    {
                        std::lock_guard<std::mutex> lock(pending_mutex_);
                        pending = pending_.erase(h.request_id) != 0;
                    }
                    if (pending) FireDone(h.request_id, hr, cancelled != 0, err);
                }
            } else if (h.type == RSP_CTX_ITEMS) {
                uint32_t session = 0, msg_flags = 0, count = 0;
                std::vector<CtxMenuItem> items;
                bool ok = r.GetU32(session) && r.GetU32(msg_flags) && r.GetU32(count) &&
                          count <= 4096;
                const bool partial = (msg_flags & CTX_ITEMS_PARTIAL) != 0;
                for (uint32_t i = 0; ok && i < count; ++i) {
                    CtxMenuItem it;
                    uint32_t flags = 0;
                    ok = r.GetU32(it.id) && r.GetU32(flags) &&
                         r.GetString(it.verb) && r.GetString(it.text);
                    if (ok) {
                        if (!r.GetString(it.clsid)) it.clsid.clear();
                        if (!r.GetString(it.handler)) it.handler.clear();
                    }
                    it.enabled = (flags & CTX_ITEM_ENABLED) != 0;
                    it.separator_after = (flags & CTX_ITEM_SEPARATOR_AFTER) != 0;
                    it.has_children = (flags & CTX_ITEM_HAS_CHILDREN) != 0;
                    it.child = (flags & CTX_ITEM_CHILD) != 0;
                    if (ok) items.push_back(std::move(it));
                }
                std::vector<std::wstring> slow_clsids;
                if (ok) r.TryStringArray(slow_clsids);
                if (ok) {
                    bool pending = false;
                    {
                        std::lock_guard<std::mutex> lock(pending_mutex_);
                        if (partial) {
                            pending = pending_.contains(h.request_id);
                        } else {
                            pending = pending_.erase(h.request_id) != 0;
                        }
                    }
                    if (pending && cb_.ctx_items)
                        cb_.ctx_items(h.request_id, std::move(items), partial,
                                      std::move(slow_clsids));
                }
            } else if (h.type == RSP_PONG) {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                pending_.erase(h.request_id);
            }
        }

        if (!running_.load()) break;
        HandleDisconnect();
    }
}

// Pipe dropped (host died / crashed). Restart the host and resend each
// in-flight request once; requests that already survived one retry fail out.
void ShellClient::HandleDisconnect() {
    std::vector<Pending> retry;
    std::vector<Pending> failed;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        if (pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
        KillChild();
    }
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        for (auto& [id, p] : pending_) {
            if (p.retried) failed.push_back(std::move(p));
            else { p.retried = true; retry.push_back(std::move(p)); }
        }
        pending_.clear();
    }
    for (auto& p : failed)
        FireDone(p.id, HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE), false,
                 L"shell host died twice on one request");
    for (auto& p : retry) {
        const uint32_t id = p.id;
        bool sent = false;
        {
            std::lock_guard<std::mutex> lock(send_mutex_);
            if (EnsureConnected()) {
                std::lock_guard<std::mutex> pending_lock(pending_mutex_);
                pending_.emplace(id, p);
                sent = SendFrame(p.type, id, p.payload);
                if (!sent) pending_.erase(id);
            }
        }
        if (!sent) {
            FireDone(id, HRESULT_FROM_WIN32(ERROR_PIPE_NOT_CONNECTED), false,
                     UnreachableMessage());
        }
    }
}

} // namespace pulse::ipc

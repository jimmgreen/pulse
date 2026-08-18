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
    if (pipe_ != INVALID_HANDLE_VALUE) {
        // Unblock the reader's ReadFile.
        CancelIoEx(pipe_, nullptr);
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    if (reader_.joinable()) reader_.join();
    KillChild();
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
    if (pipe_ != INVALID_HANDLE_VALUE) return true;

    std::wstring name = PipeNameFor(GetCurrentProcessId());
    HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (!SpawnChild()) return false;
        // Poll until the child brings the pipe up (WaitNamedPipe alone returns
        // ERROR_FILE_NOT_FOUND immediately when the server has not created
        // the pipe yet, so retry in a bounded loop).
        auto deadline = GetTickCount64() + 8000;
        while (true) {
            h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (h != INVALID_HANDLE_VALUE) break;
            DWORD err = GetLastError();
            if (err == ERROR_PIPE_BUSY) {
                WaitNamedPipeW(name.c_str(), 200);
                if (GetTickCount64() > deadline) {
                    last_error_ = L"pulse_shell.exe \u7BA1\u9053\u5FD9\u788C";
                    return false;
                }
                continue;
            }
            if (err == ERROR_FILE_NOT_FOUND) {
                if (child_started_ && WaitForSingleObject(child_.hProcess, 0) == WAIT_OBJECT_0) {
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
    }
    pipe_ = h;
    return true;
}

// Caller must hold send_mutex_.
bool ShellClient::SendFrame(uint32_t type, uint32_t id, const std::vector<uint8_t>& payload) {
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
            sent = SendFrame(p.type, p.id, p.payload);
            if (!sent) {
                // Pipe broke mid-send: restart the child and retry once.
                CloseHandle(pipe_);
                pipe_ = INVALID_HANDLE_VALUE;
                KillChild();
                if (EnsureConnected())
                    sent = SendFrame(p.type, p.id, p.payload);
            }
        }
    }
    if (!sent) {
        FireDone(p.id, HRESULT_FROM_WIN32(ERROR_PIPE_NOT_CONNECTED), false,
                 UnreachableMessage());
        return p.id;
    }
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_[p.id] = std::move(p);
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

uint32_t ShellClient::QueryContextMenu(const std::vector<std::wstring>& paths,
                                       uint32_t owner_hwnd, bool background, bool extended) {
    PayloadWriter w;
    w.PutU32(owner_hwnd);
    uint32_t flags = 0;
    if (extended) flags |= CTXF_EXTENDED;
    if (background) flags |= CTXF_BACKGROUND;
    w.PutU32(flags);
    w.PutStringArray(paths);
    return Submit(REQ_CTX_QUERY, w.data());
}

uint32_t ShellClient::InvokeContextMenu(uint32_t session_id, uint32_t item_id) {
    PayloadWriter w;
    w.PutU32(session_id);
    w.PutU32(item_id);
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

static bool ReadFull(HANDLE pipe, void* out, DWORD size) {
    return PipeRead(pipe, static_cast<uint8_t*>(out), size);
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
            Sleep(1500);
            continue;
        }

        for (;;) {
            MsgHeader h{};
            if (!ReadFull(pipe, &h, sizeof(h))) break;
            if (h.magic != kMagic || h.payload_size > kMaxPayload) break;
            std::vector<uint8_t> payload(h.payload_size);
            if (h.payload_size && !ReadFull(pipe, payload.data(), h.payload_size)) break;

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
                    {
                        std::lock_guard<std::mutex> lock(pending_mutex_);
                        pending_.erase(h.request_id);
                    }
                    FireDone(h.request_id, hr, cancelled != 0, err);
                }
            } else if (h.type == RSP_CTX_ITEMS) {
                uint32_t session = 0, count = 0;
                std::vector<CtxMenuItem> items;
                bool ok = r.GetU32(session) && r.GetU32(count) && count <= 4096;
                for (uint32_t i = 0; ok && i < count; ++i) {
                    CtxMenuItem it;
                    uint32_t flags = 0;
                    ok = r.GetU32(it.id) && r.GetU32(flags) &&
                         r.GetString(it.verb) && r.GetString(it.text);
                    it.enabled = (flags & CTX_ITEM_ENABLED) != 0;
                    it.separator_after = (flags & CTX_ITEM_SEPARATOR_AFTER) != 0;
                    it.has_children = (flags & CTX_ITEM_HAS_CHILDREN) != 0;
                    it.child = (flags & CTX_ITEM_CHILD) != 0;
                    if (ok) items.push_back(std::move(it));
                }
                if (ok) {
                    {
                        std::lock_guard<std::mutex> lock(pending_mutex_);
                        pending_.erase(h.request_id);
                    }
                    if (cb_.ctx_items) cb_.ctx_items(h.request_id, std::move(items));
                }
            }
            // RSP_PONG: no state to update.
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
        uint32_t type = p.type, id = p.id;
        auto payload = std::move(p.payload);
        bool sent = false;
        {
            std::lock_guard<std::mutex> lock(send_mutex_);
            if (EnsureConnected()) sent = SendFrame(type, id, payload);
        }
        if (sent) {
            Pending again;
            again.type = type;
            again.id = id;
            again.payload = std::move(payload);
            again.retried = true;
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_[id] = std::move(again);
        } else {
            FireDone(id, HRESULT_FROM_WIN32(ERROR_PIPE_NOT_CONNECTED), false,
                     UnreachableMessage());
        }
    }
}

} // namespace pulse::ipc

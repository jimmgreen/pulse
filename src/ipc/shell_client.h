// shell_client.h — Async client for pulse_shell.exe (UI-process side).
//
// Owns the child process lifetime: pipe connects to \\.\pipe\pulse_shell_<pid>;
// on send/receive failure the child is restarted (CreateProcess) and the
// in-flight request is retried exactly once, then reported as failed.
// All callbacks fire on the client's reader thread — never block the UI.
#pragma once
#include "protocol.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulse::ipc {

// One Explorer verb from a host-side IContextMenu session (already filtered
// by pulse_shell; see ctx_menu_util.h). Software-owned submenus arrive as a
// has_children header followed by its child rows (one level).
struct CtxMenuItem {
    uint32_t id = 0;              // host menu command id; pass back to Invoke
    bool enabled = true;
    bool separator_after = false;
    bool has_children = false;    // submenu header; id is not invokable
    bool child = false;           // row inside the preceding header's flyout
    std::wstring verb;            // canonical verb (may be empty)
    std::wstring text;
    std::wstring clsid;
    std::wstring handler;
};

class ShellClient {
public:
    struct Callbacks {
        std::function<void(uint32_t id, float percent, std::wstring item,
                           uint32_t items_done, uint32_t total_items)> progress;
        std::function<void(uint32_t id, uint32_t hr, bool cancelled, std::wstring error)> done;
        // RSP_CTX_ITEMS for a REQ_CTX_QUERY; id is the query id (== session id).
        std::function<void(uint32_t id, std::vector<CtxMenuItem> items, bool partial,
                           std::vector<std::wstring> slow_clsids)> ctx_items;
    };

    static ShellClient& Instance();

    void Start(Callbacks cb);
    void Stop();

    uint32_t DeleteRecycle(const std::vector<std::wstring>& paths);
    uint32_t RealDelete(const std::vector<std::wstring>& paths);
    uint32_t RestoreRecycle(const std::vector<std::wstring>& paths);
    uint32_t Rename(const std::wstring& path, const std::wstring& new_name);
    uint32_t CreateFolder(const std::wstring& path);
    uint32_t CreateNewFile(const std::wstring& path);
    void Cancel(uint32_t id);
    void Abort(uint32_t id);
    bool Ping();

    // Explorer context-menu session. Query returns the session id; items come
    // back through Callbacks::ctx_items. Invoke completes via Callbacks::done
    // (the host closes the session afterwards). Close is fire-and-forget for
    // sessions dismissed without invoking.
    uint32_t QueryContextMenu(const std::vector<std::wstring>& paths,
                              uint32_t owner_hwnd, bool background, bool extended,
                              const std::vector<std::wstring>& disabled_clsids = {});
    uint32_t InvokeContextMenu(uint32_t session_id, uint32_t item_id,
                               const std::wstring& verb = {},
                               const std::wstring& text = {});
    void CloseContextMenu(uint32_t session_id);

private:
    ShellClient() = default;
    ~ShellClient();

    struct Pending {
        uint32_t type = 0;
        uint32_t id = 0;
        std::vector<uint8_t> payload;
        bool retried = false;
    };

    uint32_t Submit(uint32_t type, const std::vector<uint8_t>& payload);
    bool EnsureConnected();
    bool SpawnChild();
    void KillChild();
    bool SendFrame(uint32_t type, uint32_t id, const std::vector<uint8_t>& payload);
    void ReaderThread();
    void HandleDisconnect();
    void FireDone(uint32_t id, uint32_t hr, bool cancelled, const std::wstring& error);
    const std::wstring& UnreachableMessage() const;

    Callbacks cb_;
    std::mutex send_mutex_;          // serializes pipe writes + reconnect
    std::mutex pending_mutex_;
    std::map<uint32_t, Pending> pending_;

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION child_{};
    bool child_started_ = false;
    std::wstring last_error_;
    ULONGLONG last_spawn_try_ = 0;

    std::thread reader_;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> next_id_{1};
};

} // namespace pulse::ipc

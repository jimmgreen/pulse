// ops_manager.h — Operation queue, progress status, undo stack (UI-process side).
//
// All file operations are serialized on one worker thread and executed through
// pulse_shell.exe (ShellClient). The UI thread never touches COM/shell:
// it calls Submit/Cancel/Undo/Status and gets repaints via the notify callback.
//
// Undo model (stage 1B-1):
//   Move          -> undo = move the copies at dest back to their original parent
//   Rename        -> undo = rename back to the original name
//   Copy          -> undo = recycle-delete the produced copies (Explorer-like)
//   CreateFolder /
//   CreateTextFile-> undo = recycle-delete the created item
//   RecycleDelete -> undo = restore the original paths from $Recycle.Bin
//   RealDelete    -> never recorded, not undoable.
#pragma once
#include <windows.h>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <atomic>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace pulse::ops {

enum class OpType { Copy, Move, RecycleDelete, RealDelete, Rename, CreateFolder, CreateTextFile, RestoreRecycle };
enum class CollisionPolicy { System, Replace, KeepBoth };
enum class OpPhase { Queued, Scanning, WaitingForConflict, Running, Paused,
                     Cancelling, Completed, Failed };
enum class ConflictChoice { Cancel, Replace, Skip, KeepBoth };

struct ConflictItemInfo {
    uint64_t token = 0;
    uint64_t task_id = 0;
    std::wstring source;
    std::wstring destination;
    uint64_t source_size = 0;
    uint64_t destination_size = 0;
    FILETIME source_modified{};
    FILETIME destination_modified{};
    bool source_is_directory = false;
    bool destination_is_directory = false;
    size_t remaining = 0;
};

struct OpRequest {
    OpType type = OpType::Copy;
    std::vector<std::wstring> sources;
    std::wstring dest_dir;    // Copy / Move
    std::wstring new_name;    // Rename
    CollisionPolicy collision_policy = CollisionPolicy::System; // Copy / Move
    bool is_undo = false;     // undo-originated ops do not re-enter the stack
};

struct UndoEntry {
    OpType type = OpType::Copy;
    std::vector<std::wstring> sources;
    std::vector<std::wstring> destinations; // actual names, including keep-both results
    std::wstring dest_dir;
    std::wstring new_name;
    bool supported = true;    // false = recorded for history, cannot be undone
};

struct OpStatus {
    bool active = false;
    OpType type = OpType::Copy;
    OpPhase phase = OpPhase::Completed;
    uint64_t task_id = 0;
    float percent = -1.0f;    // <0 = nothing to show
    std::wstring summary;     // one-line status-bar text
    std::wstring last_error;
    std::wstring source_label;
    std::wstring destination_label;
    std::wstring current_item;
    uint64_t total_bytes = 0;
    uint64_t transferred_bytes = 0;
    uint64_t total_items = 0;
    uint64_t completed_items = 0;
    double bytes_per_second = 0.0;
    double peak_bytes_per_second = 0.0;
    uint64_t eta_seconds = 0;
    uint64_t completed_ops = 0; // bumped on every finished op (UI edge detect)
};

struct CompletedOperation {
    OpType type = OpType::Copy;
    std::vector<std::wstring> sources;
    std::vector<std::wstring> destinations;
};

// One Explorer verb coming back from a pulse_shell context-menu session.
// Software submenus keep one level: header row (has_children) + child rows.
struct ShellMenuItem {
    uint32_t id = 0;             // host menu id; pass to InvokeShellMenu
    bool enabled = true;
    bool separator_after = false;
    bool has_children = false;
    bool child = false;
    std::wstring verb;           // canonical verb (may be empty)
    std::wstring text;
};

class OpsManager {
public:
    OpsManager() = default;
    ~OpsManager();

    // notify is invoked from worker/IPC threads whenever status changes.
    void Start(std::function<void()> notify);
    void Stop();

    uint64_t Submit(OpRequest req);
    void CancelCurrent();
    void PauseCurrent();
    void ResumeCurrent();
    std::optional<ConflictItemInfo> PendingConflict() const;
    void ResolveConflict(uint64_t token, ConflictChoice choice, bool apply_to_all);

    // Double-click open: ShellExecuteEx on the ops worker thread (plan §6.2).
    void OpenWith(const std::wstring& path);

    // Shell "properties" verb on the ops worker thread. Compile-verified only
    // in 1B-2; wired to the context menu but exercised manually.
    void ShowProperties(const std::wstring& path);

    // Any registry shell verb ("print", "edit", "openas", …) via ShellExecuteEx
    // on the ops worker thread.
    void ExecuteVerb(const std::wstring& path, const std::wstring& verb);

    // Open `file` with a specific application (open-with MRU entry).
    void OpenWithApp(const std::wstring& app_exe, const std::wstring& file);

    // `wt.exe -d <dir>` on the ops worker thread. Compile-verified only.
    void OpenTerminal(const std::wstring& dir);

    // --- Explorer context-menu sessions (pulse_shell IContextMenu) ---------
    // Runs on a dedicated forwarding thread so a slow pipe reconnect or an
    // in-flight transfer never delays a right-click. The callback fires on the
    // shell client's reader thread; PostMessage from it, do not paint.
    using ShellMenuCallback =
        std::function<void(uint32_t token, std::vector<ShellMenuItem> items)>;
    void SetShellMenuCallback(ShellMenuCallback cb);
    // Returns a token identifying the session (0 when the manager is stopped).
    uint32_t QueryShellMenu(std::vector<std::wstring> paths, void* owner_hwnd,
                            bool background, bool extended);
    void InvokeShellMenu(uint32_t token, uint32_t item_id); // host auto-closes after
    void CloseShellMenu(uint32_t token);                    // dismissed without invoke
    // True if a context-menu InvokeCommand finished since the last take
    // (UI uses this to refresh the folder the verb may have mutated).
    bool TakeCtxInvokeDone();

    bool CanUndo() const;
    std::wstring UndoLabel() const;   // "撤销移动 xxx" etc; empty if none
    void Undo();

    OpStatus Status() const;
    std::vector<CompletedOperation> DrainCompletions();

    // Session persistence of the undo stack (JSON, same style as StagingTray).
    std::wstring UndoToJson() const;
    bool UndoFromJson(const std::wstring& in);

private:
    struct QueueItem {
        OpRequest req;
        std::wstring open_path;   // non-empty => ShellExecuteEx instead
        std::wstring open_verb;   // "open" (default) / "properties" / ...
        std::wstring open_args;   // e.g. -d "<dir>" for wt.exe
        std::wstring open_file;   // explicit program (empty => open_path is the file)
        uint64_t seq = 0;
    };

    struct MenuJob {
        enum class Kind { Query, Invoke, Close } kind = Kind::Query;
        uint32_t token = 0;
        uint32_t item_id = 0;
        uint32_t owner_hwnd = 0;
        bool background = false;
        bool extended = false;
        std::vector<std::wstring> paths;
    };

    void WorkerThread();
    void MenuThread();
    void RunShellOp(const OpRequest& req, uint64_t task_id);
    void RunTransfer(const OpRequest& req, uint64_t task_id);
    void SetStatus(const std::function<void(OpStatus&)>& fn);
    void PushUndo(const OpRequest& req,
                  const std::vector<std::wstring>* actual_destinations = nullptr);
    bool ConsumeCtxInvokeDone(uint32_t id);   // true = RSP_DONE was a menu invoke
    void OnCtxItems(uint32_t client_id, std::vector<ShellMenuItem> items);

    std::function<void()> notify_;

    mutable std::mutex mutex_;            // guards queue_ + status_ + undo_
    std::condition_variable cv_;
    std::deque<QueueItem> queue_;
    OpStatus status_;
    std::deque<UndoEntry> undo_;
    std::deque<CompletedOperation> completions_;

    std::thread thread_;
    bool running_ = false;
    uint64_t next_seq_ = 1;

    std::atomic<uint32_t> current_req_id_{0};
    std::atomic<bool> transfer_active_{false};
    std::atomic<bool> transfer_cancel_{false};
    std::atomic<bool> transfer_pause_{false};

    mutable std::mutex transfer_control_mutex_;
    std::condition_variable transfer_control_cv_;
    std::optional<ConflictItemInfo> pending_conflict_;
    uint64_t next_conflict_token_ = 1;
    uint64_t resolved_conflict_token_ = 0;
    ConflictChoice resolved_conflict_choice_ = ConflictChoice::Cancel;
    bool resolved_conflict_apply_all_ = false;

    // Completion sync for the op currently in flight.
    std::mutex done_mutex_;
    std::condition_variable done_cv_;
    bool done_ready_ = false;
    uint32_t done_id_ = 0;
    uint32_t done_hr_ = 0;
    bool done_cancelled_ = false;
    std::wstring done_error_;

    // Context-menu forwarding thread + token <-> pipe-request-id bookkeeping.
    std::thread menu_thread_;
    bool menu_running_ = false;
    mutable std::mutex menu_mutex_;
    std::condition_variable menu_cv_;
    std::deque<MenuJob> menu_queue_;
    ShellMenuCallback menu_cb_;
    uint32_t next_menu_token_ = 1;
    std::map<uint32_t, uint32_t> menu_session_by_token_;  // token -> query req id
    std::map<uint32_t, uint32_t> menu_token_by_session_;  // query req id -> token
    std::set<uint32_t> ctx_invoke_ids_;                   // in-flight invoke req ids
    std::atomic<uint32_t> ctx_invoke_done_{0};
};

// wt.exe argument string for "open terminal here" (unit-tested; launching is
// compile-verified only in 1B-2).
std::wstring TerminalCommandLine(const std::wstring& dir);

} // namespace pulse::ops

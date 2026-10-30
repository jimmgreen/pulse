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
#include "file_lock_owner.h"
#include "duplicate_cleanup_guard.h"
#include "op_speed_graph.h"
#include "../common/menu_icon.h"
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

enum class OpType { Copy, Move, RecycleDelete, RealDelete, Rename, CreateFolder, CreateTextFile, RestoreRecycle, EmptyRecycle, BatchRename };
enum class CollisionPolicy { System, Replace, KeepBoth };
enum class OpPhase { Queued, Scanning, WaitingForConflict, Running, Paused,
                     Verifying, Cancelling, Completed, Failed };
enum class ConflictChoice { Cancel, Replace, Skip, KeepBoth };
enum class AuthorizationState { None, Requesting, ActionRequired };
enum class AuthorizationChoice { Cancel, Retry, Skip };
// Every task reports speed in exactly one unit; the transfer dialog labels the history
// graph, the current speed and the peak from it. Empty-recycle counts as bytes because
// its poller tracks recycled bytes.
enum class OpSpeedBasis { None, Bytes, Items };

inline OpSpeedBasis SpeedBasisFor(OpType type) {
    switch (type) {
    case OpType::Copy:
    case OpType::Move:
    case OpType::EmptyRecycle: return OpSpeedBasis::Bytes;
    case OpType::RecycleDelete:
    case OpType::RealDelete:
    case OpType::RestoreRecycle: return OpSpeedBasis::Items;
    default: return OpSpeedBasis::None;
    }
}

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
    size_t remaining = 0;     // conflicts still to come, this one included
    size_t ordinal = 0;       // 1-based position among the conflicts asked so far (0 = unknown)
    OpType type = OpType::Copy;
    std::wstring keep_both_name; // leaf name 保留两者 would give the new copy
};

struct OpRequest {
    OpType type = OpType::Copy;
    std::vector<std::wstring> sources;
    std::wstring dest_dir;    // Copy / Move
    std::wstring new_name;    // Rename, or explicit leaf name for a single-root Move
    std::vector<std::wstring> new_names; // BatchRename, parallel to sources
    // Status text name for the first source when its file name means nothing
    // to the user (还原 from the 回收站 view: sources are $R payloads).
    std::wstring display_name;
    // CreateTextFile from a 新建 (ShellNew) entry: copy this template file, or
    // write these bytes; neither = an empty file.
    std::wstring template_file;
    std::vector<uint8_t> template_data;
    CollisionPolicy collision_policy = CollisionPolicy::System; // Copy / Move
    bool is_undo = false;     // undo-originated ops do not re-enter the stack
    // An undo task: the operation it reverses, so it finishes as 已撤销复制
    // rather than as the inverse (删除完成). Session only.
    std::optional<OpType> undo_of;
    // Several operations making up one user action (合并到文件夹, 解散文件夹)
    // share a nonzero group and undo as one step, under this label.
    uint64_t undo_group = 0;
    std::wstring undo_label;
    // RecycleDelete only: skip any source that is not an empty folder when
    // its turn comes (解散文件夹 queues it behind the move-out).
    bool only_if_empty = false;
    // Lock retry only: end these processes first (never journaled, so a
    // recovered operation cannot terminate anything).
    std::vector<LockOwner> close_first;
    // Retry of a locked-file failure (with or without ending processes): the
    // failed attempt may have handled part of the selection already.
    bool lock_retry = false;
    bool duplicate_cleanup = false;
    std::vector<DuplicateCleanupGroup> duplicate_groups;
    // Phone operations (IsShellTransferRequest): Pulse folders to re-list
    // when the task ends (a pulse:device: folder, a local target). A phone
    // folder has no change notifications Pulse can follow.
    std::vector<std::wstring> refresh_directories;
};

// A phone's items (MTP / PTP) have no path, only a shell parsing name
// ("::{20D04FE0-...}\\\\?\\usb#..."). Requests naming one run through the
// shell host's IFileOperation, Explorer's own engine (REQ_SHELL_TRANSFER):
// Copy / Move with sources or dest_dir a shell id; CreateFolder /
// CreateTextFile with dest_dir the phone folder and new_name the leaf (no
// sources); RealDelete with shell id sources (a phone has no Recycle Bin).
inline bool IsShellItemId(const std::wstring& path) {
    return path.size() > 2 && path[0] == L':' && path[1] == L':';
}
inline bool IsShellTransferRequest(const OpRequest& req) {
    if (IsShellItemId(req.dest_dir)) return true;
    for (const auto& source : req.sources)
        if (IsShellItemId(source)) return true;
    return false;
}

struct UndoEntry {
    OpType type = OpType::Copy;
    std::vector<std::wstring> sources;
    std::vector<std::wstring> destinations; // actual names, including keep-both results
    std::wstring dest_dir;
    std::wstring new_name;
    bool supported = true;    // false = recorded for history, cannot be undone
    uint64_t group = 0;       // OpRequest::undo_group (session only, not persisted)
    std::wstring label;       // OpRequest::undo_label
    uint64_t task_id = 0;     // the task that recorded it (session only)
};

// One item a copy, move or delete could not handle while the rest went on
// (pulse_ops_dialogs.html, partial failure). A delete's has no destination.
struct OpFailure {
    std::wstring source;
    std::wstring destination;
    std::wstring reason;        // in the user's words, without the path
    HRESULT hr = S_OK;
};

struct OpStatus {
    bool active = false;
    bool can_pause = true;
    // Pause was asked for and not yet resumed. The worker may still be inside
    // a write and only reports Paused at its next check; the UI shows paused
    // from this so a second click resumes instead of pausing again.
    bool pause_requested = false;
    AuthorizationState authorization = AuthorizationState::None;
    bool can_skip_authorization = false;
    // ActionRequired only: "Retry" was chosen and the attempt is in flight
    // without a UAC prompt (reused elevated session). The dialog keeps the
    // same view and disables its actions instead of switching views.
    bool authorization_retrying = false;
    OpType type = OpType::Copy;
    OpPhase phase = OpPhase::Completed;
    uint64_t task_id = 0;
    float percent = -1.0f;    // <0 = nothing to show
    std::wstring summary;     // one-line status-bar text
    std::wstring last_error;
    std::wstring source_label;
    std::wstring destination_label;
    std::wstring destination_dir;   // copy / move: the folder items go to
    std::wstring current_item;
    uint64_t total_bytes = 0;
    uint64_t transferred_bytes = 0;
    uint64_t total_items = 0;
    uint64_t completed_items = 0;
    double bytes_per_second = 0.0;
    double peak_bytes_per_second = 0.0;
    double items_per_second = 0.0;
    double peak_items_per_second = 0.0;
    OpSpeedBasis speed_basis = OpSpeedBasis::None;
    uint64_t eta_seconds = 0;
    uint64_t completed_ops = 0; // bumped on every finished op (UI edge detect)
    // Failed because other processes hold the item (Restart Manager result).
    std::wstring locked_path;
    std::vector<LockOwner> lock_owners;
    // Items that failed while the task went on (Failed phase). At most
    // kMaxReportedFailures are listed; failed_items is the full count.
    std::vector<OpFailure> failures;
    uint64_t failed_items = 0;
};

struct CompletedOperation {
    uint64_t task_id = 0;
    OpType type = OpType::Copy;
    std::vector<std::wstring> sources;
    std::vector<std::wstring> destinations;
    bool refresh_only = false;
    std::vector<std::wstring> refresh_directories;
};

// Why a recycle left items untouched: the Shell would have deleted them for
// good. The UI then asks, as Explorer does, whether to delete them anyway.
enum class RecycleRefusal { None, TooLarge, Disabled, Location };

struct FinishedTask {
    uint64_t task_id = 0;
    std::vector<std::wstring> moved_sources; // Confirmed whole request roots, never descendants alone.
    OpType type = OpType::Copy;
    bool success = false;
    bool is_undo = false;   // an undo's own step (never on the undo stack)
    std::vector<std::wstring> sources;
    // Recycle delete: sources the Recycle Bin would not take, untouched, and
    // the reason that covers them all (Location over Disabled over TooLarge).
    std::vector<std::wstring> recycle_refused;
    RecycleRefusal recycle_refusal = RecycleRefusal::None;
};

// Failures about the target, the device or the session rather than one item:
// every later item would fail the same way, so the task stops on them instead
// of going on with the next item.
bool IsStopTransferError(HRESULT hr);

using TaskResult = FinishedTask;

struct RecoveryEntry {
    uint64_t sequence = 0;
    OpRequest request;
    bool was_active = false;
};

struct RecoverySnapshot {
    std::vector<RecoveryEntry> entries;
    bool has_uncertain_destructive = false;
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
    std::wstring clsid;
    std::wstring handler;
    wchar_t mnemonic = 0;        // access key from the Explorer label, 0 = none
    common::MenuIconRef icon;    // the row's app icon, as Explorer shows it
};

// One handler of a context-menu answer, in menu order (ipc::CtxHandlerStatus).
struct ShellMenuHandlerStatus {
    std::wstring clsid;
    bool done = false;       // finished; its rows (maybe none) are in the answer
    bool duplicate = false;  // finished, rows dropped as copies of classic rows
    uint32_t elapsed_ms = 0; // finished: its own query time; else time so far
    uint32_t rows = 0;
};

// What an answer carries besides its rows.
struct ShellMenuAnswer {
    // Complete answers only: the handler provably hung on its own.
    std::vector<std::wstring> slow_clsids;
    // Empty from an older host or the no-handler fallback menu.
    std::vector<ShellMenuHandlerStatus> handlers;
};

class OpsManager {
#ifdef PULSE_ELEVATED_TEST_CLIENT
    friend struct OpsManagerReviewTestAccess;
#endif
public:
    OpsManager() = default;
    ~OpsManager();

    // notify is invoked from worker/IPC threads whenever status changes.
    // A picker uses a private copy/move/rename queue without taking ownership
    // of the main window's singleton Shell client or its callbacks.
    void Start(std::function<void()> notify, bool own_shell_client = true);
    void Stop();

    void SetJournalPath(std::wstring path);
    RecoverySnapshot PendingRecovery() const;
    bool RetryRecovery();
    void DiscardRecovery();
    void SetVerifyCopies(bool enabled) noexcept { verify_copies_.store(enabled); }
    bool VerifyCopies() const noexcept { return verify_copies_.load(); }
    // Owner window for the shell dialogs the open thread raises (打开方式…,
    // 属性). Explorer parents those to the folder window; passing our own HWND
    // keeps them tied to Pulse instead of whatever happens to be foreground
    // when the ops thread gets to the request.
    void SetUiWindow(HWND hwnd) noexcept { ui_hwnd_.store(hwnd); }
    HWND UiWindow() const noexcept { return ui_hwnd_.load(); }

    uint64_t Submit(OpRequest req);
    // Atomically reject shutdown when work is queued/running and stop new submissions.
    bool TryPrepareForUpdate();
    void CancelUpdatePreparation();
    void CancelCurrent();
    void PauseCurrent();
    void ResumeCurrent();
    std::optional<ConflictItemInfo> PendingConflict() const;
    void ResolveConflict(uint64_t token, ConflictChoice choice, bool apply_to_all);
    void ResolveAuthorization(uint64_t task_id, bool retry);
    // Re-queue the operation that failed on a locked item (status.task_id).
    // close_owners = end the closable lock owners on the worker first. Items
    // that are already gone are skipped for delete/move. skip_item = drop the
    // locked item itself and go on with the rest (see CanSkipLockedItem).
    bool RetryLockedOperation(uint64_t task_id, bool close_owners, bool skip_item = false);
    // The locked item is one of several top-level sources, so the rest can go
    // on without it (an item inside a source folder cannot be left out).
    bool CanSkipLockedItem(uint64_t task_id) const;
    // Re-queue failed items of a finished copy, move or delete (indices into
    // status.failures of task_id): each goes back to the folder it was meant
    // for (a delete's go as one request). Empty `indices` = all of them, once. Returns the queued tasks and
    // the items each covers; empty when that task's failures are gone (the
    // last few tasks with failures are kept).
    struct RetrySpawn {
        uint64_t task_id = 0;
        std::vector<size_t> items;
    };
    std::vector<RetrySpawn> RetryFailedItems(uint64_t task_id, const std::vector<size_t>& indices = {});
    // How a recent task ended (the last 16). Status() only shows the newest
    // task, so retries that run back to back are looked up here.
    std::optional<OpStatus> FinalStatus(uint64_t task_id) const;
    // The speed history of the newest task, recorded while it runs whether
    // or not a progress card is visible; empty for any other task.
    OpSpeedGraph SpeedGraph(uint64_t task_id) const;
    static constexpr size_t kMaxReportedFailures = 500;

    // Double-click open: ShellExecuteEx on a dedicated open thread (plan §6.2).
    void OpenWith(const std::wstring& path);

    // Shell "properties" verb on the ops worker thread. Compile-verified only
    // in 1B-2; wired to the context menu but exercised manually.
    void ShowProperties(const std::wstring& path);
    // Several items: one merged Explorer sheet (SHMultiFileProperties) on a
    // short-lived STA thread that pumps messages while the sheet is open.
    void ShowProperties(const std::vector<std::wstring>& paths);

    // Any registry shell verb ("print", "edit", …) via ShellExecuteEx on the
    // ops worker thread. "openas" / "打开方式…" uses SHOpenWithDialog.
    void ExecuteVerb(const std::wstring& path, const std::wstring& verb);

    // Open `file` with a specific application (open-with MRU entry).
    void OpenWithApp(const std::wstring& app_exe, const std::wstring& file);

    // Expand a registry command template (%1/%L) and launch it on the open thread.
    void ExecuteCommand(const std::wstring& command, const std::wstring& path);

    // Launch the configured context-menu executable on the open worker; empty uses wt.exe.
    void OpenTerminal(const std::wstring& dir, const std::wstring& program = {});

    // Start a console program (cmd.exe, powershell.exe, ...) with `dir` as its
    // working directory, on the open thread. wt.exe gets `-d <dir>` before args.
    void OpenProgramIn(const std::wstring& exe, const std::wstring& dir,
                       const std::wstring& args);

    // Items dropped on a program shortcut: `program` (its resolved target)
    // starts with the shortcut's own arguments, then `args`, in the shortcut's
    // start-in folder, elevated when the shortcut asks for it. The .lnk is
    // read on the open thread.
    void OpenShortcutProgram(const std::wstring& lnk, const std::wstring& program,
                             const std::wstring& args);

    // --- Explorer context-menu sessions (pulse_shell IContextMenu) ---------
    // Runs on a dedicated forwarding thread so a slow pipe reconnect or an
    // in-flight transfer never delays a right-click. The callback fires on the
    // shell client's reader thread; PostMessage from it, do not paint.
    using ShellMenuCallback =
        std::function<void(uint32_t token, std::vector<ShellMenuItem> items, bool partial,
                           ShellMenuAnswer answer)>;
    void SetShellMenuCallback(ShellMenuCallback cb);
    // Returns a token identifying the session (0 when the manager is stopped).
    uint32_t QueryShellMenu(std::vector<std::wstring> paths, void* owner_hwnd,
                            bool background, bool extended,
                            std::vector<std::wstring> disabled_clsids = {});
    void InvokeShellMenu(uint32_t token, uint32_t item_id,
                         std::wstring verb = {}, std::wstring text = {}); // host auto-closes after
    void CloseShellMenu(uint32_t token);                    // dismissed without invoke
    // True if a context-menu InvokeCommand finished since the last take
    // (UI uses this to refresh the folder the verb may have mutated).
    bool TakeCtxInvokeDone();
    // Explorer's own verb on shell items (phone files: open, copy, cut,
    // delete, properties; "paste" with `background` into a folder), run by
    // the shell host. Empty verb = the default one. "drop:copy" / "drop:move"
    // with `background`: the folder, then the dropped paths or shell items,
    // which the folder's own drop target transfers. A delete / paste / drop /
    // rename completion counts as a context-menu invoke (TakeCtxInvokeDone: the
    // folder refreshes); a failure leaves its message for TakeShellVerbError.
    // False when it could not be queued.
    bool RunShellVerb(std::vector<std::wstring> shell_ids, std::wstring verb, bool background,
                      void* owner_hwnd);
    // Renames one shell item (a phone file) through the host, same completion.
    bool RenameShellItem(std::wstring shell_id, std::wstring new_name);
    std::wstring TakeShellVerbError();

    bool CanUndo() const;
    // The newest undo step belongs to this task (the pill's 撤销).
    bool CanUndoTask(uint64_t task_id) const;
    std::wstring UndoLabel() const;   // "撤销移动 xxx" etc; empty if none
    void Undo();

    // An item made outside the queue (an AI client's zip, ai_tools.h): one
    // undo step like 新建 (undo recycles it), and a completion so the folder
    // refreshes. Not recorded while an update is being prepared.
    void RecordExternalCreate(const std::wstring& path, uint64_t group, std::wstring label);
    // The group of the newest undo step; 0 when there is none or it has none.
    uint64_t TopUndoGroup() const;
    enum class GroupUndo { Started, NotOnTop, Pending, Unavailable };
    // Undo only when the newest step belongs to `group`, decided under the
    // same lock as the undo itself, so a step the user adds meanwhile is never
    // undone instead. Pending: part of the group is still queued.
    GroupUndo UndoGroup(uint64_t group);

    OpStatus Status() const;
    std::vector<CompletedOperation> DrainCompletions();
    std::vector<FinishedTask> DrainFinishedTasks();
    std::vector<TaskResult> DrainTaskResults() { return DrainFinishedTasks(); }

    // Session persistence of the undo stack (JSON, same style as StagingTray).
    std::wstring UndoToJson() const;
    bool UndoFromJson(const std::wstring& in);

private:
    friend struct OpsManagerAuditAccess;
    void RecordShellDone(uint32_t id, uint32_t hr, bool cancelled, std::wstring error);
    struct QueueItem {
        OpRequest req;
        std::wstring open_path;   // non-empty => ShellExecuteEx instead
        std::wstring open_verb;   // empty = default action; "open" / "properties" / ...
        std::wstring open_args;   // e.g. -d "<dir>" for wt.exe
        std::wstring open_file;   // explicit program (empty => open_path is the file)
        std::vector<std::wstring> open_paths; // multi-item "properties"
        uint64_t seq = 0;
        // Request-owned control survives queue acceptance -> worker startup.
        // Guarded by mutex_; never inherited by a subsequent request.
        bool cancel_requested = false;
        bool transfer_cancelled = false;
        bool transfer_paused = false;
        ULONGLONG enqueued_at = 0; // diagnostics: queue wait vs shell cost
    };

    QueueItem* CurrentTransferLocked(); // mutex_ held; active request or queue front
    // Phone operations (IsShellTransferRequest), on the worker thread: the
    // conflicts are settled with Pulse's own dialog first, then the shell
    // host's IFileOperation does the transfer with progress and cancel.
    void RunShellTransfer(const OpRequest& req, uint64_t task_id);
    void RunShellItemOp(const OpRequest& req, uint64_t task_id);
    // Completion of a phone operation: re-list its folders (refresh only, no
    // undo step: Pulse cannot put a phone's files back).
    void CompleteShellRequest(const OpRequest& req, uint64_t task_id,
                              std::vector<std::wstring> destinations);

    struct MenuJob {
        enum class Kind { Query, Invoke, Close, Verb, Rename } kind = Kind::Query;
        uint32_t token = 0;
        uint32_t item_id = 0;
        uint32_t owner_hwnd = 0;
        bool background = false;
        bool extended = false;
        std::vector<std::wstring> paths;
        std::vector<std::wstring> disabled_clsids;
        std::wstring verb;
        std::wstring text;
    };

    void ConfigureShellCallbacks();
    void DispatchMenuJob(const MenuJob& job);
    void RegisterShellRequest(uint32_t id);
    void WorkerThread();
    void MenuThread();
    void OpenThread();
    // front = interactive dialog request (打开方式…, 属性): it jumps ahead of
    // queued opens so the dialog answers the click. Plain opens keep FIFO order.
    void EnqueueOpen(QueueItem item, bool front = false);
    void RunShellOp(const OpRequest& req, uint64_t task_id);
    bool WaitShellDone(uint32_t id, uint32_t& hr, bool& cancelled, std::wstring& error);
    void RunTransfer(const OpRequest& req, uint64_t task_id);
    void RunAuthorizedTransfer(const OpRequest& req, uint64_t task_id);
    void RunAuthorizedDelete(const OpRequest& req, uint64_t task_id);
    AuthorizationChoice WaitForAuthorization(uint64_t task_id, const std::wstring& error,
                                            const std::wstring& source);
    LockReport ProbeLock(const OpRequest& req, uint64_t task_id, HRESULT hr,
                         const std::wstring& error);
    bool PrepareLockRetry(OpRequest& req, uint64_t task_id);
    void SetStatus(const std::function<void(OpStatus&)>& fn);
    // Per-file progress: the status always changes, but the UI is told at
    // most every kProgressNotifyMs. Phase changes and results use SetStatus.
    void SetProgressStatus(const std::function<void(OpStatus&)>& fn);
    void NotifyStatusChanged();
    void PushUndo(const OpRequest& req, uint64_t task_id,
                  const std::vector<std::wstring>* actual_destinations = nullptr);
    // Undo's body; with `required_group` only when the newest step is in it.
    GroupUndo UndoNewest(const uint64_t* required_group);
    void LoadRecoveryJournal();
    void PersistJournal();
    std::wstring JournalJsonLocked() const;
    bool ConsumeCtxQueryDone(uint32_t id);
    bool ConsumeCtxInvokeDone(uint32_t id);   // true = RSP_DONE was a menu invoke
    void OnCtxItems(uint32_t client_id, std::vector<ShellMenuItem> items, bool partial,
                    ShellMenuAnswer answer);

    std::function<void()> notify_;
    // One posted notification at a time: stamped when notify_ runs, cleared
    // when the UI reads Status(). Without it a 10 000-file copy posts 50 000+
    // messages that the UI thread handles one by one. A consumer that never
    // reads Status() is still told again after kNotifyPendingMs.
    mutable std::atomic<ULONGLONG> notify_pending_since_{0};
    std::atomic<ULONGLONG> last_status_notify_{0};

    mutable std::mutex mutex_;            // guards queue_ + status_ + undo_
    std::condition_variable cv_;
    std::deque<QueueItem> queue_;
    std::deque<std::wstring> recovery_cleanup_roots_;
    std::optional<QueueItem> active_item_;
    // Undo asked for while part of that action is still queued; runs once
    // the group's last operation finishes. Guarded by mutex_.
    uint64_t deferred_undo_group_ = 0;
    bool GroupPendingLocked(uint64_t group) const;
    bool update_preparing_ = false;
    bool recovery_cleanup_active_ = false;
    struct LockRetry {
        uint64_t task_id = 0;
        OpRequest req;
        std::vector<LockOwner> owners;
        std::wstring path;              // the locked item
    };
    std::optional<LockRetry> lock_retry_;   // guarded by mutex_
    struct FailedItemsRetry {
        uint64_t task_id = 0;
        OpRequest req;
        std::vector<OpFailure> failures;
    };
    std::deque<FailedItemsRetry> failed_retries_;   // newest last; guarded by mutex_
    std::deque<OpStatus> final_statuses_;            // newest last; guarded by mutex_
    OpSpeedGraph speed_graph_;                       // status_.task_id's; guarded by mutex_
    uint64_t speed_graph_task_ = 0;
    void SampleSpeedLocked();
    void ApplyUndoSummaryLocked();
    std::vector<RecoveryEntry> pending_recovery_;
    std::wstring journal_path_;
    OpStatus status_;
    std::deque<UndoEntry> undo_;
    std::deque<CompletedOperation> completions_;
    std::deque<FinishedTask> finished_tasks_;
    std::vector<std::wstring> moved_sources_; // Worker-owned until terminal publication.
    std::vector<std::wstring> recycle_refused_;     // same; see FinishedTask
    RecycleRefusal recycle_refusal_ = RecycleRefusal::None;

    std::thread thread_;
    bool running_ = false;
    std::atomic<bool> stopping_{false};
    uint64_t next_seq_ = 1;

    std::atomic<uint32_t> current_req_id_{0};
    std::atomic<bool> shell_cancel_requested_{false};
    std::atomic<ULONGLONG> shell_activity_tick_{0};
    std::atomic<bool> transfer_active_{false};
    std::atomic<bool> transfer_cancel_{false};
    std::atomic<bool> transfer_pause_{false};
    std::atomic<bool> verify_copies_{false};

    mutable std::mutex transfer_control_mutex_;
    std::condition_variable transfer_control_cv_;
    std::optional<ConflictItemInfo> pending_conflict_;
    uint64_t next_conflict_token_ = 1;
    uint64_t resolved_conflict_token_ = 0;
    ConflictChoice resolved_conflict_choice_ = ConflictChoice::Cancel;
    bool resolved_conflict_apply_all_ = false;
    uint64_t authorization_task_ = 0;
    std::optional<AuthorizationChoice> authorization_choice_;

    // Completion sync for the op currently in flight.
    std::mutex done_mutex_;
    std::condition_variable done_cv_;
    struct ShellDoneResult {
        uint32_t hr = 0;
        bool cancelled = false;
        std::wstring error;
    };
    std::set<uint32_t> shell_request_ids_;
    std::set<uint32_t>& file_op_ids_ = shell_request_ids_;
    std::map<uint32_t, ShellDoneResult> done_results_;

    // Context-menu forwarding thread + token <-> pipe-request-id bookkeeping.
    std::thread menu_thread_;
    bool menu_running_ = false;
    bool own_shell_client_ = true;
    mutable std::mutex menu_mutex_;
    std::condition_variable menu_cv_;
    std::deque<MenuJob> menu_queue_;
    bool menu_dispatching_ = false;
    ShellMenuCallback menu_cb_;
    uint32_t next_menu_token_ = 1;
    std::map<uint32_t, uint32_t> menu_session_by_token_;  // token -> query req id
    std::map<uint32_t, uint32_t> menu_token_by_session_;  // query req id -> token
    std::set<uint32_t> ctx_invoke_ids_;                   // in-flight invoke req ids
    std::atomic<uint32_t> ctx_invoke_done_{0};
    std::map<uint32_t, bool> shell_verb_ids_;             // in-flight RunShellVerb / RenameShellItem -> refresh
    std::wstring shell_verb_error_;                       // last failure, until taken (menu_mutex_)

    // ShellExecute / properties: dedicated STA thread so opens never
    // serialize behind transfers (SEE_MASK_NOASYNC on the transfer
    // worker made double-click open wait for in-flight copies).
    std::thread open_thread_;
    // Owner for the shell dialogs above; written by the UI thread only.
    std::atomic<HWND> ui_hwnd_{nullptr};
    bool open_running_ = false;
    mutable std::mutex open_mutex_;
    std::condition_variable open_cv_;
    std::deque<QueueItem> open_queue_;
};

// wt.exe argument string for "open terminal here" (unit-tested; launching is
// compile-verified only in 1B-2).
std::wstring TerminalCommandLine(const std::wstring& dir);

} // namespace pulse::ops

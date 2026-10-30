#pragma once

#include "FluentTokens.h"
#include "confirm_dialog.h"
#include "fluent_components.h"
#include "ui_compositor.h"
#include "../ops/ops_manager.h"

#include <chrono>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pulse::ui {

struct FileOperationCallbacks {
    std::function<void()> cancel;
    std::function<void()> pause;
    std::function<void()> resume;
    std::function<void()> dismiss;
    std::function<void(uint64_t)> retry_authorization;
    std::function<void(uint64_t)> skip_authorization;
    // Partial failure: re-queue failed items (OpsManager::RetryFailedItems)
    // and open the destination folder.
    std::function<std::vector<ops::OpsManager::RetrySpawn>(uint64_t, const std::vector<size_t>&)> retry_failed;
    std::function<void(const std::wstring&)> open_folder;
    // How a finished task ended (OpsManager::FinalStatus): retries that ran
    // back to back are only partly seen through Update.
    std::function<std::optional<ops::OpStatus>(uint64_t)> task_outcome;
    // The engine's background speed history (OpsManager::SpeedGraph): a card
    // first shown late, or shown again after hiding, draws the whole run.
    std::function<ops::OpSpeedGraph(uint64_t)> speed_history;
};

// What the footer offers while failed items are listed.
struct TransferFailureView {
    bool on = false;
    bool can_retry = false;   // a listed item can be retried
    bool can_open = false;    // the destination folder is known
};

class FileOperationWindow {
public:
    FileOperationWindow();
    ~FileOperationWindow();

    bool Create(HWND owner, FileOperationCallbacks callbacks);
    void Destroy();
    void SetTheme(bool dark, D2D1_COLOR_F accent);
    void Update(const ops::OpStatus& status);
    void Show(bool activate = true);
    void Hide();
    bool IsVisible() const;
    // When the window is built after the task started (first pill click), the
    // app passes the start it recorded so 已用时间 counts from the real start.
    void SetTaskStart(uint64_t task_id, std::chrono::steady_clock::time_point started);
    // The close-button countdown after a successful finish runs only for a
    // window the app opened itself or one opened from the status pill while
    // the task ran; a summary opened after the finish stays until closed.
    void SetAutoClose(bool on);

private:
    friend struct FileOperationWindowTestAccess;
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    void ApplyWindowTheme();
    void Render();
    void ResizeForDetails(bool preserve_center);
    int HitTestControl(float x, float y) const;
    void RequestClose();
    void CreateCardFormats();
    bool AutoClosePending() const;
    void TickAutoClose(ULONGLONG now);
    void BeginAutoClose();
    void FinishAutoClose();
    void KeepOpen();
    void SampleGraph(ULONGLONG now);
    TransferFailureView FailureView() const;
public:
    // A retry started from the failed-items list: its outcome is shown in the
    // list (no separate locked-item prompt).
    bool TracksRetry(uint64_t task_id) const;
private:
    size_t VisibleFailureRows() const;
    float FailureBlockDip() const;
    ops::OpStatus BoardStatus() const;
    void RetryFailureRows(const std::vector<size_t>& rows);
    void FinishRetry(const ops::OpStatus& status);

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    FileOperationCallbacks callbacks_;
    ops::OpStatus status_;
    // Speed graph (pulse_ops_dialogs.html §1): x is progress when the task
    // reports a percentage, otherwise sample order.
    ops::OpSpeedGraph graph_;
    void SyncGraph();
    ULONGLONG task_start_tick_ = 0;
    ULONGLONG task_end_tick_ = 0;
    // Auto-close after a successful finish: counts down 3 s from the finish.
    // Hovering does not hold it; a click, a key or a drag on the finished
    // card keeps it open (kept_), and a minimized card waits.
    static constexpr ULONGLONG kAutoCloseMs = 3000;
    static constexpr ULONGLONG kCloseAnimMs = 200;
    ULONGLONG countdown_base_ = 0;
    bool countdown_hover_ = false;
    bool auto_close_ = true;
    bool kept_ = false;
    // Finished while minimized: the countdown starts when it is restored.
    bool finish_unseen_ = false;
    bool closing_ = false;
    ULONGLONG closing_tick_ = 0;
    ComPtr<IDWriteTextFormat> pct_format_;
    ComPtr<IDWriteTextFormat> unit_format_;
    ComPtr<IDWriteTextFormat> title_format_;
    ComPtr<IDWriteTextFormat> speed_format_;
    ComPtr<IDWriteTextFormat> label_format_;
    ComPtr<IDWriteTextFormat> value_format_;
    ComPtr<IDWriteTextFormat> chip_format_;
    ComPtr<IDWriteTextFormat> glyph_format_;
    ComPtr<IDWriteTextFormat> tile_glyph_format_;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool detailed_ = false;
    bool positioned_ = false;
    float scale_ = 1.0f;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    int hover_ = 0;
    int pressed_ = 0;
    // Leaving the authorization view is deferred briefly: a retry that fails
    // again returns to it within milliseconds, and switching immediately
    // flashes the progress/estimate view in between.
    // A retry that fails at once keeps "Retrying..." visible this long so the
    // click is acknowledged rather than looking ignored.
    static constexpr ULONGLONG kAuthorizationExitDelayMs = 400;
    static constexpr ULONGLONG kRetryMinVisibleMs = 350;
    ops::OpStatus held_status_;
    bool has_held_status_ = false;
    ULONGLONG hold_until_ = 0;
    ULONGLONG retry_shown_tick_ = 0;

    // Partial failure (pulse_ops_dialogs.html, 部分失败): the failed items stay
    // listed while their retries run, each retry being a task of its own; the
    // card keeps showing the task that failed until another one starts.
    struct FailureRow {
        ops::OpFailure failure;
        uint64_t origin_task = 0;   // the task whose failures list holds it
        size_t origin_index = 0;
        uint64_t retry_task = 0;    // the retry in flight, 0 = none
        enum class State { Failed, Retrying, Done } state = State::Failed;
    };
    struct FailureBoard {
        uint64_t task_id = 0;
        ops::OpStatus status;       // the finish that opened the list
        std::vector<FailureRow> rows;
        uint64_t unlisted = 0;      // failed beyond the listed rows
        // Every retry started from the list: their statuses keep arriving
        // after they finish and must not read as another operation.
        std::vector<uint64_t> retry_tasks;
    };
    std::optional<FailureBoard> board_;
    size_t board_scroll_ = 0;
    // Row retry buttons as last drawn: visible row, bounds (pixels).
    std::vector<std::pair<size_t, D2D1_RECT_F>> failure_buttons_;
};

struct ConflictDialogResult {
    ops::ConflictChoice choice = ops::ConflictChoice::Cancel;
    bool apply_to_all = false;
};

ConflictDialogResult ShowFileConflictDialog(HWND owner,
                                            const ops::ConflictItemInfo& conflict,
                                            bool dark,
                                            D2D1_COLOR_F accent);

// Permanent delete confirmation (pulse_ops_dialogs.html §6).
struct PermanentDeleteInfo {
    std::vector<std::wstring> paths;
    bool no_recycle_bin = false; // meant for the bin, but these items have none
    // Why there is no bin for them: the location has none, the bin is turned
    // off for the drive, or an item is larger than the bin can hold.
    enum class NoBinReason { Location, Disabled, TooLarge } no_bin_reason = NoBinReason::Location;
    bool offer_recycle = false;  // show 移到回收站 as the alternative
};

enum class PermanentDeleteChoice { Cancel, Delete, Recycle };

PermanentDeleteChoice ShowPermanentDeleteDialog(HWND owner, const PermanentDeleteInfo& info,
                                                bool dark, D2D1_COLOR_F accent);

// File in use (pulse_ops_dialogs.html §5). Watches the owners and the item and
// returns Retry on its own once the item is free.
struct LockedItemInfo {
    ops::OpType type = ops::OpType::RecycleDelete;
    std::wstring path;                 // the locked item
    std::vector<ops::LockOwner> owners;
    bool can_end = false;              // at least one owner may be ended: 结束进程并重试
    bool can_skip = false;             // 跳过此项 (OpsManager::CanSkipLockedItem)
};

enum class LockedItemChoice { Cancel, Retry, EndAndRetry, Skip };

LockedItemChoice ShowLockedItemDialog(HWND owner, const LockedItemInfo& info, bool dark,
                                      D2D1_COLOR_F accent);

} // namespace pulse::ui

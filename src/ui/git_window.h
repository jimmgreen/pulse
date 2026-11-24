#pragma once

// Version Control window (in-app Git, scenes 2, 3, 4, 5):
// - Changes tab: staged/unstaged/conflicts lists, suggested message, split
//   commit+push, inline & side-by-side diff, blame, per-hunk stage/unstage/revert,
//   and interactive image before/after slider comparison.
// - History tab: branch/path/mine filter + search, remote-ahead banner, D2D
//   topology graph, commit details, browse-files-at-commit, compare-with-working,
//   create-branch-here, single-file restore, and soft/hard reset.
// - Conflict tab: merge progress bar, abort/complete merge, 2-way/3-way + base
//   text conflict cards with live result preview & manual edit, and binary/image
//   conflict picker with unchosen copy preservation.
// - Shared Branch & Sync popover (GitBranchPopup).

#include "ui_compositor.h"
#include "fluent_components.h"
#include "git_branch_popup.h"
#include "git_image_compare.h"
#include "../app/git_branches.h"
#include "../app/git_changes.h"
#include "../app/git_conflict.h"
#include "../app/git_history.h"
#include "../app/text_diff.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse::ui {

enum class GitViewTab : uint8_t { Changes = 0, History = 1, Conflict = 2 };

class GitWindow {
public:
    using ChangedFn = std::function<void(const std::wstring& root)>;
    using TermsChangedFn = std::function<void(bool plain_terms)>;
    using RevealFileFn = std::function<void(const std::wstring& full_path)>;

    GitWindow() = default;
    ~GitWindow();
    GitWindow(const GitWindow&) = delete;
    GitWindow& operator=(const GitWindow&) = delete;

    void Show(HWND owner, const std::wstring& root, bool dark, D2D1_COLOR_F accent,
              bool plain_terms = true, GitViewTab initial_tab = GitViewTab::Changes,
              const std::wstring& focus_rel_path = {});
    void Close();
    bool visible() const noexcept;
    HWND hwnd() const noexcept { return hwnd_; }
    const std::wstring& root() const noexcept { return root_; }
    void SetOnChanged(ChangedFn fn) { on_changed_ = std::move(fn); }
    void SetOnTermsChanged(TermsChangedFn fn) { on_terms_changed_ = std::move(fn); }
    void SetOnRevealFile(RevealFileFn fn) { on_reveal_file_ = std::move(fn); }

    // Invokes a remote/stash/branch op from outside (or after popup dismissal).
    void TriggerBranchPopup(POINT screen_pt, bool open_above = false);
    void RefreshAsync();

private:
    static constexpr UINT kMsgOpDone = WM_APP + 42;
    static constexpr UINT kMsgDiffDone = WM_APP + 43;
    static constexpr UINT kMsgHistDone = WM_APP + 44;
    static constexpr UINT kMsgHistDetailDone = WM_APP + 45;
    static constexpr UINT kMsgHistDiffDone = WM_APP + 46;
    static constexpr UINT kMsgCfStateDone = WM_APP + 47;
    static constexpr UINT kMsgCfFileDone = WM_APP + 48;

    static constexpr UINT_PTR kSubEditHistSearch = 2;
    static constexpr UINT_PTR kSubEditHistBranch = 3;
    static constexpr UINT_PTR kSubEditCfCustom = 4;

    enum class Section : uint8_t { Conflicts, Staged, Changes };
    enum class Caption : uint8_t { None, Minimize, Maximize, Close };  // self-drawn title bar
    enum class HitKind : uint8_t {
        None,
        // Top bar
        TabChanges, TabHistory, TabConflict,
        TopStash, TopStashList, TopBranch, TopSync, TopTerms, TopRefresh,
        // Changes tab - left
        Field, SuggestChip, Commit, CommitPush, Amend,
        Row, RowCheck, RowDiscard, RowReveal, SectionAction, ListBar,
        // Changes tab - right
        DiffModeInline, DiffModeSbs, DiffBlameToggle,
        DiffStage, DiffDiscard, DiffRestoreDeleted,
        HunkStage, HunkDiscard, DiffImageHandle, DiffBody, DiffBar,
        // Status bar
        Undo,
        // History tab
        HistScopeAll, HistScopePath, HistScopeMine, HistSearchField,
        HistPullBanner, HistRow, HistListBar,
        HistBtnFiles, HistBtnCompare, HistBtnBranch, HistBtnReset,
        HistFileRow, HistFileDiff, HistFileRestore,
        HistBackToDetail, HistTreeRow, HistTreeRestore,
        HistBranchCreateOk, HistBranchCreateCancel, HistDetailBar,
        // Conflict tab
        CfAbort, CfComplete, CfFileRow,
        CfPickOurs, CfPickTheirs, CfPickBoth, CfToggleCustom, CfToggleBase,
        CfCustomApply, CfApplyFile,
        CfBinPickOurs, CfBinPickTheirs, CfBinSaveCopyToggle, CfBodyBar
    };
    struct Hit {
        HitKind kind = HitKind::None;
        int row = -1;
        int sub = -1;  // hunk index or sub-item index
    };
    struct ListRow {
        Section section = Section::Changes;
        int index = -1;  // -1: section header
    };
    enum class OpKind : uint8_t {
        Refresh, Stage, Unstage, Discard, Commit, CommitAndPush, UndoCommit,
        StageHunk, UnstageHunk, RevertHunk,
        Pull, Push, Sync, Fetch, SwitchBranch, CheckoutRemote, CreateBranch, MergeBranch,
        StashPush, StashApply, StashDrop,
        ResolveTextConflict, ResolveBinaryConflict, CompleteMerge, AbortMerge,
        RestoreCommitFile, ResetToCommit
    };
    struct OpRequest {
        OpKind kind = OpKind::Refresh;
        std::vector<std::wstring> paths;
        std::vector<std::wstring> untracked;
        std::wstring message;
        std::wstring arg1;   // branch / remote / ref / hash
        std::wstring arg2;   // start_point / rel_path / patch target
        std::wstring expect_head;
        std::string patch_bytes;
        bool amend = false;
        bool has_head = true;
        bool flag1 = false;  // auto_stash / hard / pick_ours
        bool flag2 = false;  // save_other_copy
    };
    struct OpResult {
        uint64_t generation = 0;
        OpKind kind = OpKind::Refresh;
        std::wstring arg1;
        app::GitOpResult op;
        app::GitRemoteOpResult remote_op;
        app::GitOpResult status;
        app::GitChangeSet set;
        app::GitIdentity identity;
        int stash_count = 0;
        bool merging = false;
        std::wstring head;
        std::wstring subject;
        std::wstring undone_message;
        std::wstring saved_copy_path;
    };
    enum class URowKind : uint8_t { Hunk, Same, Del, Add };
    struct URow {
        URowKind kind = URowKind::Same;
        int left = -1;
        int right = -1;
        int diff_row = -1;
        int hunk_index = -1;
        std::wstring text;
    };
    struct SbsRow {
        bool is_hunk = false;
        int hunk_index = -1;
        int left = -1;
        int right = -1;
        int diff_row = -1;
        diff::RowKind kind = diff::RowKind::Same;
        std::wstring header;
    };
    struct DiffJob {
        uint64_t generation = 0;
        bool is_image = false;
        std::shared_ptr<const diff::DiffSide> left, right;
        diff::DiffResult result;
        std::vector<URow> urows;
        std::vector<SbsRow> sbs_rows;
        std::vector<app::GitDiffHunkPatch> hunks;
        std::vector<app::GitBlameLine> blame;
        DecodedGitImage old_image;
        DecodedGitImage new_image;
        bool has_old_image = false;
        bool has_new_image = false;
        int max_cols = 0;
    };
    struct CharSpans {
        std::vector<diff::CharSpan> left, right;
    };
    struct RowGeom {
        D2D1_RECT_F row{}, check{}, discard{}, reveal{}, letter{}, action{};
    };
    struct HunkBtnRects {
        int hunk_index = -1;
        D2D1_RECT_F stage_rc{};
        D2D1_RECT_F discard_rc{};
    };

    // History state
    enum class HistDetailMode : uint8_t { Detail = 0, TreeBrowser = 1, InlineDiff = 2 };
    struct HistJob {
        uint64_t generation = 0;
        bool ok = false;
        int behind = 0;
        std::vector<app::GitCommitItem> commits;
    };
    struct HistDetailJob {
        uint64_t generation = 0;
        std::wstring hash;
        bool load_tree = false;
        std::vector<app::GitCommitFile> files;
        std::vector<app::GitTreeEntry> tree;
    };
    struct HistDiffJob {
        uint64_t generation = 0;
        bool is_image = false;
        std::shared_ptr<const diff::DiffSide> left, right;
        diff::DiffResult result;
        std::vector<URow> urows;
        DecodedGitImage old_image, new_image;
        bool has_old_image = false, has_new_image = false;
    };

    // Conflict state
    struct ConflictFileJob {
        uint64_t generation = 0;
        std::wstring path;
        bool is_image = false;
        bool is_binary = false;
        std::vector<app::GitConflictHunk> hunks;
        std::vector<std::string> segments;
        DecodedGitImage ours_image, theirs_image;
        uint64_t ours_bytes = 0, theirs_bytes = 0;
        bool has_ours = false, has_theirs = false;
    };
    struct ConflictStateJob {
        uint64_t generation = 0;
        app::GitConflictState state;
    };
    struct ConflictHunkRects {
        D2D1_RECT_F ours_btn{}, theirs_btn{}, both_btn{}, custom_btn{}, base_btn{};
        D2D1_RECT_F card_top{};
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR ref);
    static LRESULT CALLBACK SubEditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR ref);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    bool EnsureWindow(HWND owner);
    void ApplyTheme();
    Theme CurrentTheme() const;
    void EnsureHatch();

    // Git ops
    bool StartOp(OpRequest request);
    void JoinOp();
    void AdoptOp();
    void StartDiff();
    void StopDiff();
    void AdoptDiff();
    void SetActiveTab(GitViewTab tab);
    void SwitchTerms(bool plain);

    // Model (Changes)
    void RebuildRows();
    const std::vector<app::GitChange>& List(Section section) const;
    const app::GitChange* ChangeAt(int row) const;
    const app::GitChange* Selected() const;
    void Select(int row, bool scroll_into_view = true);
    void StepSelection(int delta);
    void ToggleStage(int row);
    void StageSection(Section section);
    void Discard(const std::vector<const app::GitChange*>& changes);
    void Commit(bool also_push = false);
    void UndoCommit();
    void ApplyHunkAction(int hunk_index, bool is_discard);
    void PromptStashPush();
    void PromptSwitchBranch(const std::wstring& branch, int dirty_count);
    void PromptMergeBranch(const std::wstring& branch);
    void PromptDropStash(const std::wstring& ref, const std::wstring& message);
    std::wstring Message() const;
    std::wstring SuggestedMessage() const;
    int StagedCount() const noexcept { return static_cast<int>(set_.staged.size()); }
    bool CanCommit() const;
    GitBranchPopupCallbacks MakePopupCallbacks();

    // History methods (implemented in git_window_history.cpp)
    void StartHistoryLoad();
    void StopHistoryLoad();
    void AdoptHistoryLoad();
    void StartHistoryDetailLoad(bool load_tree);
    void AdoptHistoryDetailLoad();
    void StartHistoryFileDiff(const std::wstring& hash, const std::wstring& rel_path);
    void AdoptHistoryFileDiff();
    void SelectHistoryCommit(int index);
    void LayoutHistory(float w, float h);
    void RenderHistory(ID2D1DeviceContext* dc, const Theme& theme, bool hc, bool painter_ok);
    Hit HitTestHistory(float x, float y) const;
    bool OnClickHistory(const Hit& hit, float x, float y);
    void PromptRestoreHistoryFile(const std::wstring& hash, const std::wstring& rel_path);
    void PromptResetToCommit(const app::GitCommitItem& commit);
    std::wstring HistQueryText() const;
    std::wstring HistBranchInputText() const;

    // Conflict methods (implemented in git_window_conflict.cpp)
    void StartConflictStateLoad();
    void StopConflictLoad();
    void AdoptConflictStateLoad();
    void StartConflictFileLoad();
    void AdoptConflictFileLoad();
    void LayoutConflict(float w, float h);
    void RenderConflict(ID2D1DeviceContext* dc, const Theme& theme, bool hc, bool painter_ok);
    Hit HitTestConflict(float x, float y) const;
    bool OnClickConflict(const Hit& hit, float x, float y);
    int ConflictTotalCount() const noexcept;
    int ConflictResolvedCount() const noexcept;
    void ApplyCustomEditBuffer();
    void SyncCfCustomEditBox();
    void PromptAbortMerge();

    // View
    void RecreateFormats();
    void CreateEdits();
    void LayoutEdits();
    void SyncChildEditsVisibility();
    void Layout();
    void Render();
    Hit HitTest(float x, float y) const;
    void OnClick(const Hit& hit, float x, float y);
    void ScrollListTo(float y);
    void ScrollDiffTo(float y);
    void ScrollDiffX(float dx);
    // On a viewport's scrollbar: fluent::ScrollbarHitTest only says the point is in a
    // scrollable viewport, so this also keeps to the bar's strip at its right edge.
    bool OnScrollbar(const fluent::ScrollbarSpec& bar, float x, float y) const noexcept {
        return fluent::ScrollbarHitTest(bar, x, y) && x >= bar.viewport.right - 14.0f * scale_;
    }
    float Measure(std::wstring_view text, IDWriteTextFormat* format) const;
    float MeasureWrappedHeight(std::wstring_view text, float width, IDWriteTextFormat* format) const;
    const CharSpans& SpansFor(int diff_row);
    RowGeom GeomFor(int row) const;
    float ListRowHeight() const noexcept { return 30.0f * scale_; }
    float DiffRowHeight() const noexcept { return 20.0f * scale_; }
    float ListContentHeight() const noexcept { return static_cast<float>(rows_.size()) * ListRowHeight(); }
    float DiffContentHeight() const noexcept {
        const size_t count = sbs_mode_ ? sbs_rows_.size() : urows_.size();
        return static_cast<float>(count) * DiffRowHeight();
    }
    void SetNotice(std::wstring text, bool error);
    // Title bar: which caption button is under a client point, and the HT code
    // the system needs for it (HTMAXBUTTON keeps Snap Layouts on maximize).
    Caption CaptionAt(float x, float y) const;
    LRESULT NcHitTest(LPARAM lparam) const;
    void DoCaption(Caption caption);
    ID2D1Bitmap* Logo();
    void UpdateTitle();

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND edit_ = nullptr;
    HWND hist_search_edit_ = nullptr;
    HWND hist_branch_edit_ = nullptr;
    HWND cf_custom_edit_ = nullptr;
    HFONT edit_font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    std::wstring edit_cue_, hist_search_cue_, hist_branch_cue_;
    Compositor compositor_;
    fluent::Painter painter_;
    float scale_ = 1.0f;
    bool dark_ = false;
    bool plain_terms_ = true;
    D2D1_COLOR_F accent_{0.0f, 0.47f, 0.83f, 1.0f};
    ChangedFn on_changed_;
    TermsChangedFn on_terms_changed_;
    RevealFileFn on_reveal_file_;
    GitViewTab active_tab_ = GitViewTab::Changes;
    GitBranchPopup branch_popup_;

    ComPtr<IDWriteTextFormat> mono_;
    ComPtr<IDWriteTextFormat> gutter_;
    ComPtr<IDWriteTextFormat> ui_;
    ComPtr<IDWriteTextFormat> ui_bold_;
    ComPtr<IDWriteTextFormat> small_;
    ComPtr<IDWriteTextFormat> small_bold_;
    ComPtr<IDWriteTextFormat> letter_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1BitmapBrush> hatch_;
    bool hatch_dark_ = false;
    float char_w_ = 8.0f;

    // Repository state
    std::wstring root_;
    std::wstring repo_name_;
    app::GitChangeSet set_;
    app::GitIdentity identity_;
    int stash_count_ = 0;
    bool merging_ = false;
    bool loaded_ = false;
    bool git_missing_ = false;
    std::vector<ListRow> rows_;
    Section sel_section_ = Section::Changes;
    std::wstring sel_path_;
    int sel_row_ = -1;
    bool amend_ = false;
    bool sbs_mode_ = false;
    bool blame_mode_ = false;
    std::wstring notice_;
    bool notice_error_ = false;
    std::wstring undo_head_;
    ULONGLONG last_refresh_ = 0;

    // Op worker
    std::thread op_thread_;
    std::mutex op_mutex_;
    std::unique_ptr<OpResult> op_pending_;
    uint64_t op_generation_ = 0;
    bool op_busy_ = false;
    bool refresh_queued_ = false;
    std::unique_ptr<OpRequest> queued_op_;

    // Diff worker (Changes tab)
    std::thread diff_thread_;
    std::atomic<bool> diff_cancel_{false};
    std::mutex diff_mutex_;
    std::unique_ptr<DiffJob> diff_pending_;
    uint64_t diff_generation_ = 0;
    bool diff_loading_ = false;
    std::wstring diff_key_;
    bool diff_is_image_ = false;
    std::shared_ptr<const diff::DiffSide> left_, right_;
    diff::DiffResult result_;
    std::vector<URow> urows_;
    std::vector<SbsRow> sbs_rows_;
    std::vector<app::GitDiffHunkPatch> hunk_patches_;
    std::vector<app::GitBlameLine> blame_lines_;
    DecodedGitImage old_img_meta_, new_img_meta_;
    ComPtr<ID2D1Bitmap> old_img_bmp_, new_img_bmp_;
    bool has_old_img_ = false, has_new_img_ = false;
    float img_split_ = 0.5f;
    bool dragging_img_split_ = false;
    std::unordered_map<int, CharSpans> spans_;
    std::vector<HunkBtnRects> hunk_btn_rects_;
    int max_cols_ = 0;
    int number_digits_ = 1;

    // History tab state
    std::thread hist_thread_, hist_detail_thread_, hist_diff_thread_;
    std::mutex hist_mutex_;
    std::unique_ptr<HistJob> hist_pending_;
    std::unique_ptr<HistDetailJob> hist_detail_pending_;
    std::unique_ptr<HistDiffJob> hist_diff_pending_;
    uint64_t hist_generation_ = 0;
    uint64_t hist_detail_gen_ = 0;
    uint64_t hist_diff_gen_ = 0;
    bool hist_loading_ = false;
    bool hist_detail_loading_ = false;
    app::GitHistoryFilter::Scope hist_scope_ = app::GitHistoryFilter::Scope::All;
    std::wstring hist_scope_rel_path_;
    int hist_behind_ = 0;
    std::vector<app::GitCommitItem> hist_commits_;
    int hist_sel_ = -1; // -1 = WIP row if changes > 0, else commit index in hist_commits_
    bool hist_wip_sel_ = false;
    std::vector<app::GitCommitFile> hist_files_;
    std::vector<app::GitTreeEntry> hist_tree_;
    HistDetailMode hist_detail_mode_ = HistDetailMode::Detail;
    bool hist_creating_branch_ = false;
    std::wstring hist_diff_path_;
    std::vector<URow> hist_diff_urows_;
    std::shared_ptr<const diff::DiffSide> hist_diff_left_, hist_diff_right_;
    DecodedGitImage hist_old_img_meta_, hist_new_img_meta_;
    ComPtr<ID2D1Bitmap> hist_old_img_bmp_, hist_new_img_bmp_;
    bool hist_has_old_img_ = false, hist_has_new_img_ = false;
    float hist_list_scroll_ = 0.0f;
    float hist_detail_scroll_ = 0.0f;
    std::vector<D2D1_RECT_F> hist_file_rects_;
    std::vector<D2D1_RECT_F> hist_file_diff_rects_;
    std::vector<D2D1_RECT_F> hist_file_restore_rects_;
    std::vector<D2D1_RECT_F> hist_tree_rects_;
    std::vector<D2D1_RECT_F> hist_tree_restore_rects_;

    // Conflict tab state
    std::thread cf_state_thread_, cf_file_thread_;
    std::mutex cf_mutex_;
    std::unique_ptr<ConflictStateJob> cf_state_pending_;
    std::unique_ptr<ConflictFileJob> cf_file_pending_;
    uint64_t cf_state_gen_ = 0;
    uint64_t cf_file_gen_ = 0;
    bool cf_loading_ = false;
    app::GitConflictState cf_state_;
    std::unordered_set<std::wstring> cf_session_resolved_;
    std::vector<app::GitConflictFile> cf_files_; // session-tracked list (resolved stays visible)
    std::wstring cf_sel_path_;
    bool cf_file_is_image_ = false;
    bool cf_file_is_binary_ = false;
    std::vector<app::GitConflictHunk> cf_hunks_;
    std::vector<std::string> cf_segments_;
    std::vector<bool> cf_show_base_;
    int cf_editing_hunk_ = -1;
    bool cf_save_other_copy_ = true;
    DecodedGitImage cf_ours_meta_, cf_theirs_meta_;
    ComPtr<ID2D1Bitmap> cf_ours_bmp_, cf_theirs_bmp_;
    uint64_t cf_ours_bytes_ = 0, cf_theirs_bytes_ = 0;
    bool cf_has_ours_ = false, cf_has_theirs_ = false;
    float cf_body_scroll_ = 0.0f;
    float cf_body_content_h_ = 0.0f;
    std::vector<D2D1_RECT_F> cf_file_rects_;
    std::vector<ConflictHunkRects> cf_hunk_rects_;

    // View state (Changes)
    float list_scroll_ = 0.0f;
    float diff_scroll_ = 0.0f;
    float diff_scroll_x_ = 0.0f;
    Hit hover_{};
    Hit pressed_{};
    int drag_bar_ = 0;  // 1 list, 2 diff, 3 hist list, 4 hist detail, 5 cf body
    float drag_grab_ = 0.0f;
    bool tracking_leave_ = false;
    // Self-drawn title bar (no system frame): its caption buttons are tracked
    // apart from hover_, as the system sends them non-client messages.
    Caption caption_hover_ = Caption::None;
    Caption caption_pressed_ = Caption::None;
    bool tracking_nc_leave_ = false;
    ComPtr<ID2D1Bitmap> logo_;
    float title_text_w_ = 0.0f;   // 0: the title gave way to the views
    float title_repo_w_ = 0.0f;
    float logo_scale_ = 0.0f;

    // Layout rects
    D2D1_RECT_F title_{}, caption_min_{}, caption_max_{}, caption_close_{}, logo_rc_{};
    D2D1_RECT_F toolbar_{}, status_{}, undo_rc_{};
    // Top bar controls
    D2D1_RECT_F tab_changes_rc_{}, tab_history_rc_{}, tab_conflict_rc_{},
        top_stash_rc_{}, top_stash_list_rc_{}, top_branch_rc_{}, top_sync_rc_{},
        top_terms_rc_{}, refresh_rc_{};
    // Changes tab rects
    D2D1_RECT_F left_panel_{}, field_{}, suggest_rc_{}, commit_rc_{}, commit_push_rc_{},
        amend_rc_{}, who_rc_{}, list_{}, diff_card_{}, diff_header_{}, diff_body_{},
        mode_inline_rc_{}, mode_sbs_rc_{}, blame_rc_{}, stage_btn_{}, discard_btn_{},
        restore_deleted_btn_{};
    // History tab rects
    D2D1_RECT_F hist_left_{}, hist_right_{}, hist_bar_{},
        hist_scope_all_{}, hist_scope_path_{}, hist_scope_mine_{}, hist_search_rc_{},
        hist_banner_rc_{}, hist_pull_rc_{}, hist_tl_rc_{},
        hist_meta_rc_{}, hist_files_rc_{}, hist_acts_rc_{},
        hist_btn_files_{}, hist_btn_compare_{}, hist_btn_branch_{}, hist_btn_reset_{},
        hist_branch_box_{}, hist_branch_ok_{}, hist_branch_cancel_{}, hist_back_rc_{};
    // Conflict tab rects
    D2D1_RECT_F cf_bar_{}, cf_prog_rc_{}, cf_abort_rc_{}, cf_complete_rc_{},
        cf_left_{}, cf_right_{}, cf_body_{},
        cf_apply_file_rc_{}, cf_custom_box_{}, cf_custom_apply_{},
        cf_bin_ours_rc_{}, cf_bin_theirs_rc_{}, cf_bin_copy_rc_{};
};

} // namespace pulse::ui

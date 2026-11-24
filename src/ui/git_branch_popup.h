#pragma once
// Shared Branch & Sync popover (mockup scene 4): used from the main window's
// toolbar/status-bar Git chip and the Source Control window's branch button.
// Borderless popup hosted with Compositor + fluent::Painter; all git queries run
// on a worker thread, and write actions close the popup before invoking the
// host callback so confirmation dialogs sit cleanly on the owner window.
#include "fluent_components.h"
#include "ui_compositor.h"
#include "../app/git_branches.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulse::ui {

struct GitBranchPopupCallbacks {
    std::function<void()> on_sync;
    std::function<void()> on_pull;
    std::function<void()> on_push;
    std::function<void()> on_fetch;
    std::function<void(const std::wstring& branch, int dirty_count)> on_switch_branch;
    std::function<void(const std::wstring& remote_ref)> on_checkout_remote;
    std::function<void(const std::wstring& new_name)> on_create_branch;
    std::function<void(const std::wstring& branch)> on_merge_branch;
    std::function<void(const std::wstring& ref)> on_stash_apply;
    std::function<void(const std::wstring& ref, const std::wstring& message)> on_stash_drop;
    std::function<void()> on_open_history;
    std::function<void()> on_closed;
};

class GitBranchPopup {
public:
    GitBranchPopup() = default;
    ~GitBranchPopup();
    GitBranchPopup(const GitBranchPopup&) = delete;
    GitBranchPopup& operator=(const GitBranchPopup&) = delete;

    void Show(HWND owner, const std::wstring& root, POINT anchor_screen_pt, bool open_above,
              bool dark, D2D1_COLOR_F accent, bool plain_terms, GitBranchPopupCallbacks callbacks);
    void Hide();
    bool IsOpen() const noexcept;

private:
    enum class RowKind : uint8_t {
        Header, CreateBranch, LocalBranch, RemoteBranch, Stash, EmptyStash
    };
    struct ListRow {
        RowKind kind = RowKind::Header;
        std::wstring title;
        std::wstring subtitle;
        std::wstring key;       // branch name, remote ref, or stash@{n}
        bool is_current = false;
        float y = 0.0f;
        float h = 0.0f;
        D2D1_RECT_F rect{};
        D2D1_RECT_F action1{};  // merge / apply
        D2D1_RECT_F action2{};  // drop stash
    };
    enum class HitKind : uint8_t {
        None, SearchField, SyncBtn, PullBtn, PushBtn, FetchBtn,
        Row, RowAction1, RowAction2, ListBar,
        FootNewBranch, FootHistory
    };
    struct Hit {
        HitKind kind = HitKind::None;
        int index = -1;
    };
    struct FetchJob {
        uint64_t generation = 0;
        app::GitBranchOverview overview;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                     UINT_PTR, DWORD_PTR ref);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    bool EnsureWindow(HWND owner);
    void PositionPopup();
    void ApplyTheme();
    Theme CurrentTheme() const;
    void RecreateFormats();
    void CreateEdit();
    void LayoutEdit();
    void StartLoad();
    void StopLoad();
    void AdoptLoad();
    void RebuildRows();
    void Layout();
    void Render();
    Hit HitTest(float x, float y) const;
    void OnClick(const Hit& hit, float x, float y);
    void ScrollTo(float y);
    std::wstring QueryText() const;
    float Measure(std::wstring_view text, IDWriteTextFormat* format) const;

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND search_edit_ = nullptr;
    HFONT edit_font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    std::wstring edit_cue_;
    Compositor compositor_;
    fluent::Painter painter_;
    float scale_ = 1.0f;
    bool dark_ = false;
    bool plain_terms_ = true;
    bool open_above_ = false;
    POINT anchor_pt_{};
    D2D1_COLOR_F accent_{0.0f, 0.47f, 0.83f, 1.0f};
    GitBranchPopupCallbacks callbacks_;

    ComPtr<IDWriteTextFormat> ui_;
    ComPtr<IDWriteTextFormat> ui_bold_;
    ComPtr<IDWriteTextFormat> small_;
    ComPtr<IDWriteTextFormat> small_bold_;
    ComPtr<ID2D1SolidColorBrush> brush_;

    std::wstring root_;
    app::GitBranchOverview overview_;
    bool loading_ = false;
    std::vector<ListRow> rows_;
    float list_content_h_ = 0.0f;
    float scroll_y_ = 0.0f;

    std::thread worker_;
    std::mutex job_mutex_;
    std::unique_ptr<FetchJob> pending_job_;
    uint64_t generation_ = 0;

    Hit hover_{};
    Hit pressed_{};
    bool drag_bar_ = false;
    float drag_grab_ = 0.0f;
    bool tracking_leave_ = false;
    bool closing_ = false;

    D2D1_RECT_F search_rc_{}, cur_rc_{}, behind_box_{}, ahead_box_{},
        sync_btn_{}, pull_btn_{}, push_btn_{}, fetch_btn_{},
        list_rc_{}, foot_rc_{}, foot_new_{}, foot_hist_{};
};

}  // namespace pulse::ui

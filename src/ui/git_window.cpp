#include "git_window.h"

#include "../app/git_terms.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "confirm_dialog.h"
#include "edit_host.h"
#include "git_mark.h"
#include "typography.h"
#include "window_helpers.h"
#include "window_material.h"

#include <commctrl.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace pulse::ui {
namespace {

constexpr wchar_t kClassName[] = L"PulseGitWindow";
constexpr UINT_PTR kEditSubclass = 1;
constexpr float kTitleH = 48.0f;    // Pulse's title bar (kTitleBarHeight)
constexpr float kToolbarH = 48.0f;
constexpr float kStatusH = 28.0f;
constexpr float kDiffHeaderH = 50.0f;
constexpr int kContextLines = 3;
constexpr size_t kMaxDrawChars = 4000;
constexpr size_t kMaxConfirmRows = 8;

using Sid = pulse::l10n::StringId;
using app::GitChange;
using app::GitChangeKind;
using app::GitFmt;
using app::GitFmtCount;
using app::GitStr;

std::wstring Str(Sid id) { return std::wstring(pulse::l10n::Get(id)); }

bool GitHighContrast() {
    HIGHCONTRASTW value{sizeof(value)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(value), &value, 0) &&
           (value.dwFlags & HCF_HIGHCONTRASTON);
}

struct Palette {
    D2D1_COLOR_F panel, body, add_bg, add_hi, del_bg, del_hi, add_fg, del_fg, hunk_bg, warn_fg, ok_fg, purple_fg;
};

Palette MakePalette(bool dark) {
    Palette p{};
    if (dark) {
        p.panel = D2D1::ColorF(0x202227);
        p.body = D2D1::ColorF(0x1B1D22);
        p.add_bg = D2D1::ColorF(0x3DD68C, 0.13f);
        p.add_hi = D2D1::ColorF(0x3DD68C, 0.36f);
        p.del_bg = D2D1::ColorF(0xFF6369, 0.13f);
        p.del_hi = D2D1::ColorF(0xFF6369, 0.36f);
        p.add_fg = D2D1::ColorF(0x3DD68C);
        p.del_fg = D2D1::ColorF(0xFF6369);
        p.hunk_bg = D2D1::ColorF(0x3B6CF6, 0.12f);
        p.warn_fg = D2D1::ColorF(0xF5B941);
        p.ok_fg = D2D1::ColorF(0x3DD68C);
        p.purple_fg = D2D1::ColorF(0xA78BFA);
    } else {
        p.panel = D2D1::ColorF(0xF5F6F9);
        p.body = D2D1::ColorF(0xFFFFFF);
        p.add_bg = D2D1::ColorF(0x1F9D55, 0.10f);
        p.add_hi = D2D1::ColorF(0x1F9D55, 0.28f);
        p.del_bg = D2D1::ColorF(0xE5484D, 0.09f);
        p.del_hi = D2D1::ColorF(0xE5484D, 0.25f);
        p.add_fg = D2D1::ColorF(0x1F9D55);
        p.del_fg = D2D1::ColorF(0xE5484D);
        p.hunk_bg = D2D1::ColorF(0x3B6CF6, 0.07f);
        p.warn_fg = D2D1::ColorF(0xC77D02);
        p.ok_fg = D2D1::ColorF(0x1F9D55);
        p.purple_fg = D2D1::ColorF(0x8B5CF6);
    }
    return p;
}

int DisplayColumns(const std::wstring& line) {
    int cols = 0;
    const size_t n = (std::min)(line.size(), kMaxDrawChars);
    for (size_t i = 0; i < n; ++i) cols += line[i] >= 0x1100 ? 2 : 1;
    return cols;
}

bool Inside(const D2D1_RECT_F& rc, float x, float y) {
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}

std::wstring FileName(const std::wstring& rel) {
    const size_t slash = rel.find_last_of(L"\\/");
    return slash == std::wstring::npos ? rel : rel.substr(slash + 1);
}

std::wstring FolderOf(const std::wstring& rel) {
    const size_t slash = rel.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : rel.substr(0, slash);
}

std::wstring FirstLine(const std::wstring& text) {
    const size_t end = text.find_first_of(L"\r\n");
    return end == std::wstring::npos ? text : text.substr(0, end);
}

bool ReadWorktreeBytes(const std::wstring& full_path, std::string& out, uint64_t max_bytes) {
    out.clear();
    const HANDLE f = CreateFileW(full_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart < 0 || static_cast<uint64_t>(sz.QuadPart) > max_bytes) {
        CloseHandle(f);
        return false;
    }
    out.resize(static_cast<size_t>(sz.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(out.size() - done, 1u << 20));
        DWORD got = 0;
        if (!ReadFile(f, out.data() + done, chunk, &got, nullptr) || got == 0) break;
        done += got;
    }
    CloseHandle(f);
    out.resize(done);
    return true;
}

std::wstring PlainKindText(GitChangeKind k, bool plain) {
    switch (k) {
    case GitChangeKind::Modified:
    case GitChangeKind::TypeChanged:
        return GitStr(plain, Sid::GitMarkModifiedPlain, Sid::GitMarkModifiedCode);
    case GitChangeKind::Added:
    case GitChangeKind::Copied:
        return GitStr(plain, Sid::GitMarkAddedPlain, Sid::GitMarkAddedCode);
    case GitChangeKind::Untracked:
        return GitStr(plain, Sid::GitMarkAddedPlain, Sid::GitMarkUntrackedCode);
    case GitChangeKind::Deleted:
        return GitStr(plain, Sid::GitMarkDeletedPlain, Sid::GitMarkDeletedCode);
    case GitChangeKind::Renamed:
        return GitStr(plain, Sid::GitMarkRenamedPlain, Sid::GitMarkRenamedCode);
    case GitChangeKind::Conflict:
        return GitStr(plain, Sid::GitMarkConflictPlain, Sid::GitMarkConflictCode);
    }
    return {};
}

std::wstring FormatHunkText(int old_start, int old_count, int new_start, int new_count, bool plain) {
    if (plain) {
        const int line = new_start > 0 ? new_start : old_start;
        return GitFmt(Str(Sid::GitHunkNearLine), L"{line}", std::to_wstring(line));
    }
    return L"@@ \x2212" + std::to_wstring(old_start) + L"," + std::to_wstring(old_count) + L" +" +
           std::to_wstring(new_start) + L"," + std::to_wstring(new_count) + L" @@";
}

}  // namespace

GitWindow::~GitWindow() {
    branch_popup_.Hide();
    StopDiff();
    StopHistoryLoad();
    StopConflictLoad();
    JoinOp();
    if (hwnd_ && IsWindow(hwnd_)) {
        compositor_.Shutdown();
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    if (edit_font_) DeleteObject(edit_font_);
    if (edit_brush_) DeleteObject(edit_brush_);
}

bool GitWindow::visible() const noexcept {
    return hwnd_ && IsWindowVisible(hwnd_);
}

Theme GitWindow::CurrentTheme() const {
    return GitHighContrast() ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
}

void GitWindow::EnsureHatch() {
    if (hatch_.get() && hatch_dark_ == dark_) return;
    hatch_.reset();
    auto* dc = compositor_.Dc();
    if (!dc) return;
    const float size = std::round(8.0f * scale_);
    ComPtr<ID2D1BitmapRenderTarget> target;
    if (FAILED(dc->CreateCompatibleRenderTarget(D2D1::SizeF(size, size), &target)) || !target.get()) return;
    ComPtr<ID2D1SolidColorBrush> ink;
    target->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0xFFFFFF, 0.08f) : D2D1::ColorF(0x000000, 0.07f), &ink);
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0.0f));
    if (ink.get()) {
        const float w = (std::max)(1.0f, scale_);
        target->DrawLine(D2D1::Point2F(-1.0f, size + 1.0f), D2D1::Point2F(size + 1.0f, -1.0f), ink.get(), w);
        target->DrawLine(D2D1::Point2F(-1.0f, 1.0f), D2D1::Point2F(1.0f, -1.0f), ink.get(), w);
        target->DrawLine(D2D1::Point2F(size - 1.0f, size + 1.0f), D2D1::Point2F(size + 1.0f, size - 1.0f), ink.get(), w);
    }
    if (FAILED(target->EndDraw())) return;
    ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(target->GetBitmap(&bitmap)) || !bitmap.get()) return;
    const D2D1_BITMAP_BRUSH_PROPERTIES props =
        D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    dc->CreateBitmapBrush(bitmap.get(), props, &hatch_);
    hatch_dark_ = dark_;
}

bool GitWindow::EnsureWindow(HWND owner) {
    if (hwnd_) return true;
    static bool registered = false;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &GitWindow::WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        registered = true;
    }
    const UINT dpi = owner ? pulse::compat::WindowDpi(owner) : 96u;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT work = mi.rcWork;
    const int work_w = work.right - work.left;
    const int work_h = work.bottom - work.top;
    const int width = (std::min)(MulDiv(1260, static_cast<int>(dpi), 96), work_w * 9 / 10);
    const int height = (std::min)(MulDiv(800, static_cast<int>(dpi), 96), work_h * 9 / 10);
    const int x = work.left + (work_w - width) / 2;
    const int y = work.top + (work_h - height) / 2;
    // The quick preview window's frame: a borderless popup Pulse paints whole
    // (WM_NCCALCSIZE makes it all client area) that DWM still rounds, shadows
    // and lets the user size; ApplyTheme sets the corners (ApplyWindowEffect).
    constexpr DWORD kStyle = WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN;
    CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kClassName,
                    GitStr(plain_terms_, Sid::GitCenterPlain, Sid::GitCenterGit).c_str(),
                    kStyle, x, y, width, height, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (owner) {
        if (const auto big = reinterpret_cast<HICON>(GetClassLongPtrW(owner, GCLP_HICON)))
            SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
        if (const auto icon_sm = reinterpret_cast<HICON>(GetClassLongPtrW(owner, GCLP_HICONSM)))
            SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_sm));
    }
    return true;
}

void GitWindow::ApplyTheme() {
    if (!hwnd_) return;
    // Rounded corners, dark frame and no classic caption, as the preview window.
    ApplyWindowEffect(hwnd_, WindowEffect::None, dark_);
    if (edit_brush_) DeleteObject(edit_brush_);
    edit_brush_ = CreateSolidBrush(EditBackColor(dark_));
    hatch_.reset();
    for (HWND h : {edit_, hist_search_edit_, hist_branch_edit_, cf_custom_edit_}) {
        if (h) InvalidateRect(h, nullptr, TRUE);
    }
}

void GitWindow::Show(HWND owner, const std::wstring& root, bool dark, D2D1_COLOR_F accent,
                     bool plain_terms, GitViewTab initial_tab, const std::wstring& focus_rel_path) {
    owner_ = owner;
    dark_ = dark;
    accent_ = accent;
    plain_terms_ = plain_terms;
    if (!EnsureWindow(owner)) return;
    ApplyTheme();

    const bool new_repo = _wcsicmp(root.c_str(), root_.c_str()) != 0;
    if (new_repo) {
        StopDiff();
        StopHistoryLoad();
        StopConflictLoad();
        root_ = root;
        std::wstring trimmed = root;
        while (!trimmed.empty() && (trimmed.back() == L'\\' || trimmed.back() == L'/')) trimmed.pop_back();
        const size_t slash = trimmed.find_last_of(L"\\/");
        repo_name_ = slash == std::wstring::npos ? trimmed : trimmed.substr(slash + 1);
        set_ = app::GitChangeSet{};
        identity_ = app::GitIdentity{};
        stash_count_ = 0;
        merging_ = false;
        loaded_ = false;
        rows_.clear();
        sel_row_ = -1;
        sel_path_.clear();
        urows_.clear();
        sbs_rows_.clear();
        hunk_patches_.clear();
        blame_lines_.clear();
        left_.reset();
        right_.reset();
        old_img_bmp_.reset();
        new_img_bmp_.reset();
        has_old_img_ = has_new_img_ = false;
        diff_key_.clear();
        undo_head_.clear();
        notice_.clear();
        amend_ = false;
        hist_commits_.clear();
        hist_files_.clear();
        hist_tree_.clear();
        hist_sel_ = -1;
        hist_detail_mode_ = HistDetailMode::Detail;
        hist_creating_branch_ = false;
        cf_session_resolved_.clear();
        cf_files_.clear();
        cf_sel_path_.clear();
        cf_hunks_.clear();
        cf_segments_.clear();
        cf_editing_hunk_ = -1;
        list_scroll_ = diff_scroll_ = diff_scroll_x_ = hist_list_scroll_ = hist_detail_scroll_ = cf_body_scroll_ = 0.0f;
        if (edit_) SetWindowTextW(edit_, L"");
        if (hist_search_edit_) SetWindowTextW(hist_search_edit_, L"");
        if (hist_branch_edit_) SetWindowTextW(hist_branch_edit_, L"");
        if (cf_custom_edit_) SetWindowTextW(cf_custom_edit_, L"");
    }
    if (!focus_rel_path.empty()) {
        std::wstring norm = focus_rel_path;
        std::replace(norm.begin(), norm.end(), L'\\', L'/');
        sel_path_ = norm;
        hist_scope_rel_path_ = norm;
        if (initial_tab == GitViewTab::History) hist_scope_ = app::GitHistoryFilter::Scope::CurrentPath;
    } else {
        hist_scope_rel_path_.clear();
    }

    active_tab_ = initial_tab;
    UpdateTitle();
    StartOp(OpRequest{});
    if (active_tab_ == GitViewTab::History) StartHistoryLoad();
    else if (active_tab_ == GitViewTab::Conflict) StartConflictStateLoad();

    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    Layout();
    LayoutEdits();
    SyncChildEditsVisibility();
    if (active_tab_ == GitViewTab::Changes && edit_) SetFocus(edit_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void GitWindow::Close() {
    branch_popup_.Hide();
    StopDiff();
    StopHistoryLoad();
    StopConflictLoad();
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    if (owner_ && IsWindow(owner_)) SetForegroundWindow(owner_);
}

void GitWindow::RefreshAsync() {
    if (!root_.empty() && hwnd_) StartOp(OpRequest{});
}

void GitWindow::UpdateTitle() {
    if (!hwnd_) return;
    std::wstring title = GitStr(plain_terms_, Sid::GitCenterPlain, Sid::GitCenterGit) + L" \x2014 " + repo_name_;
    if (!set_.branch.empty()) title += L" (" + set_.branch + L")";
    SetWindowTextW(hwnd_, title.c_str());
}

void GitWindow::SetNotice(std::wstring text, bool error) {
    notice_ = std::move(text);
    notice_error_ = error;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

GitWindow::Caption GitWindow::CaptionAt(float x, float y) const {
    if (Inside(caption_close_, x, y)) return Caption::Close;
    if (Inside(caption_max_, x, y)) return Caption::Maximize;
    if (Inside(caption_min_, x, y)) return Caption::Minimize;
    return Caption::None;
}

LRESULT GitWindow::NcHitTest(LPARAM lparam) const {
    POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    ScreenToClient(hwnd_, &pt);
    if (!IsZoomed(hwnd_)) {
        // As the preview window: an 8-DIP sizing band just inside the edge.
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int border = (std::max)(6, static_cast<int>(8.0f * scale_));
        const bool left = pt.x < border, right = pt.x >= client.right - border;
        const bool top = pt.y < border, bottom = pt.y >= client.bottom - border;
        if (top && right && pt.x >= static_cast<int>(caption_close_.left) + border) return HTCLOSE;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }
    const float x = static_cast<float>(pt.x), y = static_cast<float>(pt.y);
    if (y < 0.0f || y >= title_.bottom) return HTCLIENT;
    switch (CaptionAt(x, y)) {
    case Caption::Close: return HTCLOSE;
    case Caption::Maximize: return HTMAXBUTTON;   // keeps Snap Layouts on hover
    case Caption::Minimize: return HTMINBUTTON;
    default: break;
    }
    for (const D2D1_RECT_F& rc : {tab_changes_rc_, tab_history_rc_, tab_conflict_rc_})
        if (Inside(rc, x, y)) return HTCLIENT;
    return HTCAPTION;
}

void GitWindow::DoCaption(Caption caption) {
    if (!hwnd_) return;
    switch (caption) {
    case Caption::Minimize: ShowWindow(hwnd_, SW_MINIMIZE); break;
    case Caption::Maximize: ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE); break;
    case Caption::Close: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    default: break;
    }
}

// The packaged app icon at 20 DIP, as the main window's title bar shows it.
ID2D1Bitmap* GitWindow::Logo() {
    ID2D1DeviceContext* dc = compositor_.Dc();
    if (!dc) return nullptr;
    if (logo_.get() && std::abs(logo_scale_ - scale_) <= 0.001f) return logo_.get();
    logo_.reset();
    const int px = (std::max)(16, static_cast<int>(20.0f * scale_ + 0.5f));
    HICON icon = nullptr;
    if (FAILED(LoadIconWithScaleDown(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101 /* IDI_PULSE */), px, px, &icon)) ||
        !icon)
        return nullptr;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICFormatConverter> converter;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateBitmapFromHICON(icon, &bitmap)) && SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(bitmap.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                        0.0, WICBitmapPaletteTypeMedianCut))) {
        dc->CreateBitmapFromWicBitmap(converter.get(), nullptr, &logo_);
    }
    DestroyIcon(icon);
    if (logo_.get()) logo_scale_ = scale_;
    return logo_.get();
}

void GitWindow::SwitchTerms(bool plain) {
    if (plain_terms_ == plain) return;
    plain_terms_ = plain;
    UpdateTitle();
    LayoutEdits();
    if (on_terms_changed_) on_terms_changed_(plain_terms_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void GitWindow::SetActiveTab(GitViewTab tab) {
    if (active_tab_ == tab) return;
    active_tab_ = tab;
    if (tab == GitViewTab::History && hist_commits_.empty() && !hist_loading_) StartHistoryLoad();
    if (tab == GitViewTab::Conflict) StartConflictStateLoad();
    Layout();
    LayoutEdits();
    SyncChildEditsVisibility();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Git worker

void GitWindow::JoinOp() {
    if (op_thread_.joinable()) op_thread_.join();
}

bool GitWindow::StartOp(OpRequest request) {
    if (root_.empty() || !hwnd_) return false;
    if (op_busy_) {
        if (request.kind == OpKind::Refresh) refresh_queued_ = true;
        else queued_op_ = std::make_unique<OpRequest>(std::move(request));
        return false;
    }
    JoinOp();
    op_busy_ = true;
    const uint64_t generation = ++op_generation_;
    const HWND target = hwnd_;
    const std::wstring root = root_;
    const bool need_identity = identity_.name.empty();
    op_thread_ = std::thread([this, target, generation, root, need_identity, request = std::move(request)]() {
        auto res = std::make_unique<OpResult>();
        res->generation = generation;
        res->kind = request.kind;
        res->arg1 = request.arg1;
        switch (request.kind) {
        case OpKind::Refresh:
            res->op.ok = true;
            break;
        case OpKind::Stage:
            res->op = app::GitStagePaths(root, request.paths);
            break;
        case OpKind::Unstage:
            res->op = app::GitUnstagePaths(root, request.paths, request.has_head);
            break;
        case OpKind::Discard:
            res->op = app::GitDiscardPaths(root, request.paths);
            if (res->op.ok) res->op = app::GitRecycleUntracked(root, request.untracked);
            break;
        case OpKind::Commit:
        case OpKind::CommitAndPush:
            res->op = app::GitCommit(root, request.message, request.amend);
            if (res->op.ok) {
                res->head = app::GitHeadId(root);
                res->subject = FirstLine(app::GitLastCommitMessage(root));
                if (request.kind == OpKind::CommitAndPush) {
                    res->remote_op = app::GitPush(root, request.flag1, request.arg1);
                }
            }
            break;
        case OpKind::UndoCommit: {
            const std::wstring message = app::GitLastCommitMessage(root);
            res->op = app::GitUndoCommit(root, request.expect_head);
            if (res->op.ok) res->undone_message = message;
            break;
        }
        case OpKind::StageHunk:
            res->op = app::GitApplyHunkPatch(root, request.patch_bytes, true, false);
            break;
        case OpKind::UnstageHunk:
            res->op = app::GitApplyHunkPatch(root, request.patch_bytes, true, true);
            break;
        case OpKind::RevertHunk:
            res->op = app::GitApplyHunkPatch(root, request.patch_bytes, false, true);
            break;
        case OpKind::Pull:
            res->remote_op = app::GitPull(root);
            res->op.ok = res->remote_op.ok;
            res->op.git_missing = res->remote_op.git_missing;
            res->op.message = res->remote_op.message;
            break;
        case OpKind::Push:
            res->remote_op = app::GitPush(root, request.flag1, request.arg1);
            res->op.ok = res->remote_op.ok;
            res->op.git_missing = res->remote_op.git_missing;
            res->op.message = res->remote_op.message;
            break;
        case OpKind::Sync:
            res->remote_op = app::GitSync(root, request.arg1, request.flag1);
            res->op.ok = res->remote_op.ok;
            res->op.git_missing = res->remote_op.git_missing;
            res->op.message = res->remote_op.message;
            break;
        case OpKind::Fetch:
            res->remote_op = app::GitFetch(root);
            res->op.ok = res->remote_op.ok;
            res->op.git_missing = res->remote_op.git_missing;
            res->op.message = res->remote_op.message;
            break;
        case OpKind::SwitchBranch:
            res->op = app::GitSwitchBranch(root, request.arg1, request.flag1);
            break;
        case OpKind::CheckoutRemote:
            res->op = app::GitCheckoutRemoteBranch(root, request.arg1);
            break;
        case OpKind::CreateBranch:
            res->op = app::GitCreateBranch(root, request.arg1, request.arg2);
            break;
        case OpKind::MergeBranch:
            res->remote_op = app::GitMergeBranch(root, request.arg1);
            res->op.ok = res->remote_op.ok;
            res->op.git_missing = res->remote_op.git_missing;
            res->op.message = res->remote_op.message;
            break;
        case OpKind::StashPush:
            res->op = app::GitStashPush(root, request.message, true);
            break;
        case OpKind::StashApply:
            res->remote_op = app::GitStashApply(root, request.arg1);
            res->op.ok = res->remote_op.ok;
            res->op.git_missing = res->remote_op.git_missing;
            res->op.message = res->remote_op.message;
            break;
        case OpKind::StashDrop:
            res->op = app::GitStashDrop(root, request.arg1);
            break;
        case OpKind::ResolveTextConflict:
            res->op = app::GitWriteResolvedTextFile(root, request.arg1, request.patch_bytes);
            break;
        case OpKind::ResolveBinaryConflict:
            res->op = app::GitResolveBinaryConflict(root, request.arg1, request.flag1, request.flag2,
                                                    request.arg2, &res->saved_copy_path);
            break;
        case OpKind::CompleteMerge:
            res->op = app::GitCompleteMerge(root);
            break;
        case OpKind::AbortMerge:
            res->op = app::GitAbortMerge(root);
            break;
        case OpKind::RestoreCommitFile:
            res->op = app::GitRestoreFileFromCommit(root, request.arg1, request.arg2, request.flag1);
            break;
        case OpKind::ResetToCommit:
            res->op = app::GitResetToCommit(root, request.arg1, request.flag1);
            break;
        }

        if (!res->op.git_missing) {
            res->status = app::GitReadChanges(root, res->set);
            res->merging = !res->set.conflicts.empty() || app::GitIsMerging(root);
            if (need_identity) res->identity = app::GitReadIdentity(root);
            std::vector<app::GitStashInfo> stashes;
            const auto sl = app::RunGit(root, L"stash list --format=__PULSE_STASH__%gd%x1f%at%x1f%gs", 5000, false);
            if (sl.started && !sl.timed_out && sl.exit_code == 0) {
                app::ParseStashListOutput(sl.out, stashes);
                res->stash_count = static_cast<int>(stashes.size());
            }
        } else {
            res->status = res->op;
        }
        {
            std::lock_guard<std::mutex> lock(op_mutex_);
            op_pending_ = std::move(res);
        }
        PostMessageW(target, kMsgOpDone, 0, 0);
    });
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void GitWindow::AdoptOp() {
    std::unique_ptr<OpResult> res;
    {
        std::lock_guard<std::mutex> lock(op_mutex_);
        res = std::move(op_pending_);
    }
    if (!res || res->generation != op_generation_) return;
    JoinOp();
    op_busy_ = false;
    last_refresh_ = GetTickCount64();
    git_missing_ = res->op.git_missing || res->status.git_missing;
    if (res->status.ok) {
        set_ = std::move(res->set);
        merging_ = res->merging;
        stash_count_ = res->stash_count;
        if (!res->identity.name.empty()) identity_ = res->identity;
        loaded_ = true;
    }

    const bool wrote = res->kind != OpKind::Refresh;
    if (git_missing_) {
        SetNotice(Str(Sid::GitNotFound), true);
    } else if (res->remote_op.has_conflicts) {
        SetNotice(Str(Sid::GitMergeConflictToast), false);
        active_tab_ = GitViewTab::Conflict;
        StartConflictStateLoad();
    } else if (!res->op.ok) {
        SetNotice(GitFmt(Str(Sid::GitScFailed), L"{msg}", res->op.message), true);
    } else if (!res->status.ok) {
        SetNotice(GitFmt(Str(Sid::GitScFailed), L"{msg}", res->status.message), true);
    } else if (res->kind == OpKind::Commit || res->kind == OpKind::CommitAndPush) {
        if (edit_) SetWindowTextW(edit_, L"");
        const bool was_amend = amend_;
        amend_ = false;
        undo_head_ = was_amend ? std::wstring() : res->head;
        if (res->kind == OpKind::CommitAndPush && !res->remote_op.ok) {
            SetNotice(res->remote_op.push_rejected_non_ff ? Str(Sid::GitSavePushRejected)
                                                           : GitFmt(Str(Sid::GitScFailed), L"{msg}", res->remote_op.message),
                      true);
        } else {
            std::wstring text = GitFmt(Str(Sid::GitScCommitted), L"{hash}", res->head.substr(0, 7));
            text = GitFmt(std::move(text), L"{subject}", res->subject);
            if (res->kind == OpKind::CommitAndPush) {
                text += GitStr(plain_terms_, Sid::GitSavedAndPushedPlain, Sid::GitSavedAndPushedGit);
            }
            SetNotice(std::move(text), false);
        }
    } else if (res->kind == OpKind::UndoCommit) {
        undo_head_.clear();
        if (edit_ && GetWindowTextLengthW(edit_) == 0 && !res->undone_message.empty()) {
            std::wstring crlf;
            for (wchar_t c : res->undone_message) {
                if (c == L'\n') crlf += L"\r\n";
                else if (c != L'\r') crlf += c;
            }
            SetWindowTextW(edit_, crlf.c_str());
        }
        SetNotice(Str(Sid::GitScCommitUndone), false);
    } else if (res->kind == OpKind::CompleteMerge) {
        cf_session_resolved_.clear();
        cf_files_.clear();
        SetNotice(Str(Sid::GitMergeDone), false);
        active_tab_ = GitViewTab::Changes;
    } else if (res->kind == OpKind::AbortMerge) {
        cf_session_resolved_.clear();
        cf_files_.clear();
        notice_.clear();
        active_tab_ = GitViewTab::Changes;
    } else if (res->kind == OpKind::Sync || res->kind == OpKind::Pull || res->kind == OpKind::Push ||
               res->kind == OpKind::Fetch) {
        SetNotice(res->remote_op.summary.empty() ? Str(Sid::GitSyncDone) : res->remote_op.summary, false);
    } else if (res->kind == OpKind::ResolveTextConflict || res->kind == OpKind::ResolveBinaryConflict) {
        cf_session_resolved_.insert(res->arg1);
        for (auto& f : cf_files_) {
            if (f.path == res->arg1) f.resolved = true;
        }
        // Advance to next unresolved conflict file.
        for (const auto& f : cf_files_) {
            if (!f.resolved && !cf_session_resolved_.contains(f.path)) {
                cf_sel_path_ = f.path;
                break;
            }
        }
        StartConflictStateLoad();
        notice_.clear();
    } else if (wrote) {
        notice_.clear();
    }

    if (wrote && on_changed_) on_changed_(root_);
    UpdateTitle();
    RebuildRows();
    if (active_tab_ == GitViewTab::Changes) StartDiff();
    else if (active_tab_ == GitViewTab::History && wrote) StartHistoryLoad();
    else if (active_tab_ == GitViewTab::Conflict && !res->remote_op.has_conflicts &&
             res->kind != OpKind::ResolveTextConflict && res->kind != OpKind::ResolveBinaryConflict) {
        StartConflictStateLoad();
    }
    Layout();
    LayoutEdits();
    SyncChildEditsVisibility();
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (queued_op_) {
        std::unique_ptr<OpRequest> next = std::move(queued_op_);
        refresh_queued_ = false;
        StartOp(std::move(*next));
    } else if (refresh_queued_) {
        refresh_queued_ = false;
        StartOp(OpRequest{});
    }
}

void GitWindow::StopDiff() {
    diff_cancel_.store(true);
    if (diff_thread_.joinable()) diff_thread_.join();
    diff_cancel_.store(false);
    std::lock_guard<std::mutex> lock(diff_mutex_);
    diff_pending_.reset();
}

void GitWindow::StartDiff() {
    const GitChange* change = Selected();
    StopDiff();
    const uint64_t generation = ++diff_generation_;
    if (!change) {
        diff_loading_ = false;
        diff_is_image_ = false;
        urows_.clear();
        sbs_rows_.clear();
        hunk_patches_.clear();
        blame_lines_.clear();
        left_.reset();
        right_.reset();
        old_img_bmp_.reset();
        new_img_bmp_.reset();
        has_old_img_ = has_new_img_ = false;
        result_ = diff::DiffResult{};
        diff_key_.clear();
        spans_.clear();
        return;
    }
    const std::wstring key = std::to_wstring(static_cast<int>(sel_section_)) + L"|" + change->path;
    if (key != diff_key_) {
        urows_.clear();
        sbs_rows_.clear();
        hunk_patches_.clear();
        blame_lines_.clear();
        left_.reset();
        right_.reset();
        old_img_bmp_.reset();
        new_img_bmp_.reset();
        has_old_img_ = has_new_img_ = false;
        result_ = diff::DiffResult{};
        spans_.clear();
        diff_scroll_ = diff_scroll_x_ = 0.0f;
        diff_key_ = key;
    }
    diff_loading_ = true;
    const HWND target = hwnd_;
    const std::wstring root = root_;
    const GitChange c = *change;
    const Section section = sel_section_;
    const bool want_blame = blame_mode_;
    const bool plain = plain_terms_;
    diff_thread_ = std::thread([this, target, generation, root, c, section, want_blame, plain]() {
        auto job = std::make_unique<DiffJob>();
        job->generation = generation;
        job->is_image = app::GitIsImagePath(c.path);
        const std::wstring old_path = c.orig_path.empty() ? c.path : c.orig_path;

        if (job->is_image && c.kind != GitChangeKind::Deleted) {
            constexpr uint64_t kMaxImageBytes = 32ull * 1024 * 1024;
            std::string old_bytes, new_bytes;
            if (section == Section::Staged) {
                if (c.kind != GitChangeKind::Added && c.kind != GitChangeKind::Copied &&
                    app::GitReadBlob(root, app::GitBlobSource::Head, old_path, old_bytes, kMaxImageBytes)) {
                    job->old_image = DecodeGitImageBytes(old_bytes);
                    job->has_old_image = job->old_image.ok();
                }
                if (app::GitReadBlob(root, app::GitBlobSource::Index, c.path, new_bytes, kMaxImageBytes)) {
                    job->new_image = DecodeGitImageBytes(new_bytes);
                    job->has_new_image = job->new_image.ok();
                }
            } else {
                if (c.kind != GitChangeKind::Untracked &&
                    app::GitReadBlob(root, app::GitBlobSource::Index, c.path, old_bytes, kMaxImageBytes)) {
                    job->old_image = DecodeGitImageBytes(old_bytes);
                    job->has_old_image = job->old_image.ok();
                }
                if (ReadWorktreeBytes(app::GitJoinPath(root, c.path), new_bytes, kMaxImageBytes)) {
                    job->new_image = DecodeGitImageBytes(new_bytes);
                    job->has_new_image = job->new_image.ok();
                }
            }
            std::lock_guard<std::mutex> lock(diff_mutex_);
            diff_pending_ = std::move(job);
            PostMessageW(target, kMsgDiffDone, 0, 0);
            return;
        }

        auto empty_side = [](const std::wstring& path) {
            auto side = std::make_shared<diff::DiffSide>();
            side->path = path;
            side->status = diff::LoadStatus::Ok;
            return side;
        };
        auto work_side = [&](const std::wstring& path) {
            auto side = std::make_shared<diff::DiffSide>();
            diff::LoadDiffSide(app::GitJoinPath(root, path), *side);
            side->path = path;
            return side;
        };
        auto blob_side = [&](app::GitBlobSource source, const std::wstring& path) {
            std::string bytes;
            bool too_large = false;
            if (!app::GitReadBlob(root, source, path, bytes, diff::kMaxDiffFileBytes, &too_large)) {
                auto side = empty_side(path);
                if (too_large) side->status = diff::LoadStatus::TooLarge;
                return side;
            }
            auto side = std::make_shared<diff::DiffSide>();
            const std::wstring temp = app::GitWriteTempFile(bytes, FileName(path));
            if (temp.empty()) {
                side->status = diff::LoadStatus::ReadFailed;
            } else {
                diff::LoadDiffSide(temp, *side);
                DeleteFileW(temp.c_str());
            }
            side->path = path;
            return side;
        };

        std::shared_ptr<diff::DiffSide> left, right;
        switch (section) {
        case Section::Staged:
            left = (c.kind == GitChangeKind::Added || c.kind == GitChangeKind::Copied)
                       ? empty_side(c.path)
                       : blob_side(app::GitBlobSource::Head, old_path);
            if (diff_cancel_.load()) return;
            right = c.kind == GitChangeKind::Deleted ? empty_side(c.path)
                                                     : blob_side(app::GitBlobSource::Index, c.path);
            break;
        case Section::Changes:
            left = c.kind == GitChangeKind::Untracked ? empty_side(c.path)
                                                      : blob_side(app::GitBlobSource::Index, c.path);
            if (diff_cancel_.load()) return;
            right = c.kind == GitChangeKind::Deleted ? empty_side(c.path) : work_side(c.path);
            break;
        case Section::Conflicts:
            left = blob_side(app::GitBlobSource::Head, c.path);
            if (diff_cancel_.load()) return;
            right = work_side(c.path);
            break;
        }
        if (diff_cancel_.load()) return;

        if (c.kind != GitChangeKind::Deleted && left->status == diff::LoadStatus::Ok &&
            right->status == diff::LoadStatus::Ok) {
            if (!diff::ComputeLineDiff(left->lines, right->lines, diff::DiffOptions{}, job->result, &diff_cancel_))
                return;

            // Load native hunk patches for tracked modified files.
            if (section != Section::Conflicts && c.kind == GitChangeKind::Modified) {
                app::GitReadFileHunks(root, c.path, section == Section::Staged, job->hunks);
            }
            if (diff_cancel_.load()) return;
            if (want_blame && c.kind != GitChangeKind::Untracked) {
                app::GitReadBlame(root, c.path, job->blame);
            }
            if (diff_cancel_.load()) return;

            const auto& rows = job->result.rows;
            const int n = static_cast<int>(rows.size());
            std::vector<int> left_before(static_cast<size_t>(n) + 1, 0);
            std::vector<int> right_before(static_cast<size_t>(n) + 1, 0);
            for (int i = 0; i < n; ++i) {
                left_before[static_cast<size_t>(i) + 1] =
                    left_before[static_cast<size_t>(i)] + (rows[static_cast<size_t>(i)].left >= 0 ? 1 : 0);
                right_before[static_cast<size_t>(i) + 1] =
                    right_before[static_cast<size_t>(i)] + (rows[static_cast<size_t>(i)].right >= 0 ? 1 : 0);
            }

            size_t h = 0;
            const auto& hunks = job->result.hunks;
            int hunk_idx = 0;
            while (h < hunks.size()) {
                const int a = (std::max)(0, hunks[h].first - kContextLines);
                int b = (std::min)(n - 1, hunks[h].last + kContextLines);
                size_t next = h + 1;
                while (next < hunks.size() && hunks[next].first - kContextLines <= b + 1) {
                    b = (std::min)(n - 1, hunks[next].last + kContextLines);
                    ++next;
                }
                const int old_count = left_before[static_cast<size_t>(b) + 1] - left_before[static_cast<size_t>(a)];
                const int new_count = right_before[static_cast<size_t>(b) + 1] - right_before[static_cast<size_t>(a)];
                const int old_start = left_before[static_cast<size_t>(a)] + (old_count > 0 ? 1 : 0);
                const int new_start = right_before[static_cast<size_t>(a)] + (new_count > 0 ? 1 : 0);
                const std::wstring htext = FormatHunkText(old_start, old_count, new_start, new_count, plain);

                URow u_hdr;
                u_hdr.kind = URowKind::Hunk;
                u_hdr.hunk_index = hunk_idx;
                u_hdr.text = htext;
                job->urows.push_back(std::move(u_hdr));

                SbsRow s_hdr;
                s_hdr.is_hunk = true;
                s_hdr.hunk_index = hunk_idx;
                s_hdr.header = htext;
                job->sbs_rows.push_back(std::move(s_hdr));

                int r = a;
                while (r <= b) {
                    const diff::DiffRow& row = rows[static_cast<size_t>(r)];
                    if (row.kind == diff::RowKind::Same) {
                        job->urows.push_back({URowKind::Same, row.left, row.right, r, -1, {}});
                        job->sbs_rows.push_back({false, -1, row.left, row.right, r, diff::RowKind::Same, {}});
                        ++r;
                        continue;
                    }
                    int end = r;
                    while (end <= b && rows[static_cast<size_t>(end)].kind != diff::RowKind::Same) ++end;
                    for (int q = r; q < end; ++q) {
                        if (rows[static_cast<size_t>(q)].left >= 0)
                            job->urows.push_back({URowKind::Del, rows[static_cast<size_t>(q)].left, -1, q, -1, {}});
                    }
                    for (int q = r; q < end; ++q) {
                        if (rows[static_cast<size_t>(q)].right >= 0)
                            job->urows.push_back({URowKind::Add, -1, rows[static_cast<size_t>(q)].right, q, -1, {}});
                    }
                    for (int q = r; q < end; ++q) {
                        const auto& sr = rows[static_cast<size_t>(q)];
                        job->sbs_rows.push_back({false, -1, sr.left, sr.right, q, sr.kind, {}});
                    }
                    r = end;
                }
                ++hunk_idx;
                h = next;
            }
            for (const URow& row : job->urows) {
                if (row.kind == URowKind::Hunk) continue;
                const std::wstring& line = row.left >= 0 ? left->lines[static_cast<size_t>(row.left)]
                                                         : right->lines[static_cast<size_t>(row.right)];
                job->max_cols = (std::max)(job->max_cols, DisplayColumns(line));
            }
        }
        job->left = std::move(left);
        job->right = std::move(right);
        {
            std::lock_guard<std::mutex> lock(diff_mutex_);
            diff_pending_ = std::move(job);
        }
        PostMessageW(target, kMsgDiffDone, 0, 0);
    });
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void GitWindow::AdoptDiff() {
    std::unique_ptr<DiffJob> job;
    {
        std::lock_guard<std::mutex> lock(diff_mutex_);
        job = std::move(diff_pending_);
    }
    if (!job || job->generation != diff_generation_) return;
    if (diff_thread_.joinable()) diff_thread_.join();
    diff_loading_ = false;
    diff_is_image_ = job->is_image;
    left_ = std::move(job->left);
    right_ = std::move(job->right);
    result_ = std::move(job->result);
    urows_ = std::move(job->urows);
    sbs_rows_ = std::move(job->sbs_rows);
    hunk_patches_ = std::move(job->hunks);
    blame_lines_ = std::move(job->blame);
    old_img_meta_ = std::move(job->old_image);
    new_img_meta_ = std::move(job->new_image);
    has_old_img_ = job->has_old_image;
    has_new_img_ = job->has_new_image;
    old_img_bmp_.reset();
    new_img_bmp_.reset();
    if (auto* dc = compositor_.Dc()) {
        if (has_old_img_) old_img_bmp_ = CreateBitmapFromDecodedImage(dc, old_img_meta_);
        if (has_new_img_) new_img_bmp_ = CreateBitmapFromDecodedImage(dc, new_img_meta_);
    }
    max_cols_ = job->max_cols;
    spans_.clear();
    size_t most = 1;
    for (const auto* side : {left_.get(), right_.get()})
        if (side) most = (std::max)(most, side->lines.size());
    number_digits_ = 1;
    for (size_t v = most; v >= 10; v /= 10) ++number_digits_;
    ScrollDiffTo(diff_scroll_);
    ScrollDiffX(0.0f);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Model (Changes)

const std::vector<GitChange>& GitWindow::List(Section section) const {
    switch (section) {
    case Section::Conflicts: return set_.conflicts;
    case Section::Staged: return set_.staged;
    case Section::Changes: break;
    }
    return set_.unstaged;
}

const GitChange* GitWindow::ChangeAt(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
    const ListRow& r = rows_[static_cast<size_t>(row)];
    if (r.index < 0) return nullptr;
    const auto& list = List(r.section);
    return r.index < static_cast<int>(list.size()) ? &list[static_cast<size_t>(r.index)] : nullptr;
}

const GitChange* GitWindow::Selected() const { return ChangeAt(sel_row_); }

void GitWindow::RebuildRows() {
    rows_.clear();
    for (Section section : {Section::Conflicts, Section::Staged, Section::Changes}) {
        const auto& list = List(section);
        if (list.empty()) continue;
        rows_.push_back({section, -1});
        for (int i = 0; i < static_cast<int>(list.size()); ++i) rows_.push_back({section, i});
    }
    int found = -1;
    if (!sel_path_.empty()) {
        for (int pass = 0; pass < 2 && found < 0; ++pass) {
            for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
                const GitChange* c = ChangeAt(i);
                if (!c || c->path != sel_path_) continue;
                if (pass == 0 && rows_[static_cast<size_t>(i)].section != sel_section_) continue;
                found = i;
                break;
            }
        }
    }
    if (found < 0 && !rows_.empty()) {
        int at = std::clamp(sel_row_, 0, static_cast<int>(rows_.size()) - 1);
        while (at < static_cast<int>(rows_.size()) && !ChangeAt(at)) ++at;
        if (at >= static_cast<int>(rows_.size())) {
            at = static_cast<int>(rows_.size()) - 1;
            while (at >= 0 && !ChangeAt(at)) --at;
        }
        found = at;
    }
    sel_row_ = found;
    if (const GitChange* c = Selected()) {
        sel_section_ = rows_[static_cast<size_t>(sel_row_)].section;
        sel_path_ = c->path;
    } else {
        sel_path_.clear();
    }
    ScrollListTo(list_scroll_);
}

void GitWindow::Select(int row, bool scroll_into_view) {
    if (!ChangeAt(row)) return;
    const bool changed = row != sel_row_;
    sel_row_ = row;
    sel_section_ = rows_[static_cast<size_t>(row)].section;
    sel_path_ = ChangeAt(row)->path;
    if (scroll_into_view) {
        const float top = static_cast<float>(row) * ListRowHeight();
        const float view = list_.bottom - list_.top;
        if (top < list_scroll_) ScrollListTo(top - ListRowHeight());
        else if (top + ListRowHeight() > list_scroll_ + view) ScrollListTo(top + 2.0f * ListRowHeight() - view);
    }
    if (changed) StartDiff();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void GitWindow::StepSelection(int delta) {
    int at = sel_row_ < 0 ? (delta > 0 ? -1 : static_cast<int>(rows_.size())) : sel_row_;
    for (at += delta; at >= 0 && at < static_cast<int>(rows_.size()); at += delta) {
        if (ChangeAt(at)) {
            Select(at);
            return;
        }
    }
}

void GitWindow::ToggleStage(int row) {
    const GitChange* c = ChangeAt(row);
    if (!c || op_busy_) return;
    const Section section = rows_[static_cast<size_t>(row)].section;
    OpRequest request;
    request.paths.push_back(c->path);
    request.has_head = set_.HasHead();
    if (section == Section::Staged) {
        request.kind = OpKind::Unstage;
        if (!c->orig_path.empty()) request.paths.push_back(c->orig_path);
    } else {
        request.kind = OpKind::Stage;
        if (section == Section::Conflicts) {
            ConfirmDialogSpec spec;
            spec.title = GitFmt(Str(Sid::GitScResolveTitle), L"{name}", FileName(c->path));
            spec.message = Str(Sid::GitScResolveMsg);
            spec.confirm_text = Str(Sid::GitScResolve);
            spec.cancel_text = Str(Sid::Cancel);
            spec.tone = ConfirmTone::Question;
            spec.note = L"git add -- " + c->path;
            if (ShowConfirmDialogEx(hwnd_, spec, dark_, accent_) != ConfirmChoice::Confirm) return;
        }
    }
    StartOp(std::move(request));
}

void GitWindow::StageSection(Section section) {
    if (op_busy_) return;
    OpRequest request;
    request.has_head = set_.HasHead();
    request.kind = section == Section::Staged ? OpKind::Unstage : OpKind::Stage;
    for (const GitChange& c : List(section)) {
        request.paths.push_back(c.path);
        if (section == Section::Staged && !c.orig_path.empty()) request.paths.push_back(c.orig_path);
    }
    if (request.paths.empty()) return;
    StartOp(std::move(request));
}

void GitWindow::Discard(const std::vector<const GitChange*>& changes) {
    if (changes.empty() || op_busy_) return;
    OpRequest request;
    request.kind = OpKind::Discard;
    ConfirmDialogSpec spec;
    for (const GitChange* c : changes) {
        if (c->kind == GitChangeKind::Untracked) request.untracked.push_back(c->path);
        else request.paths.push_back(c->path);
        if (spec.items.size() < kMaxConfirmRows) spec.items.push_back(c->path);
    }
    if (changes.size() > kMaxConfirmRows) spec.items_total = changes.size();
    spec.title = changes.size() == 1
                     ? GitFmt(Str(Sid::GitScDiscardTitleOne), L"{name}", FileName(changes[0]->path))
                     : GitFmt(Str(Sid::GitScDiscardTitleMany), L"{n}", std::to_wstring(changes.size()));
    if (!request.paths.empty()) spec.message = Str(Sid::GitScDiscardMsg);
    if (!request.untracked.empty()) {
        if (!spec.message.empty()) spec.message += L" ";
        spec.message += Str(Sid::GitScDiscardUntracked);
    }
    std::wstring command;
    if (!request.paths.empty()) command = L"git restore --worktree -- \x2026";
    if (!request.untracked.empty()) {
        if (!command.empty()) command += L"; ";
        command += Str(Sid::GitScRecycleCmd);
    }
    spec.note = GitFmt(Str(Sid::GitScDiscardCmd), L"{cmd}", command);
    spec.confirm_text = GitStr(plain_terms_, Sid::GitDiscardPlain, Sid::GitScDiscard);
    spec.cancel_text = Str(Sid::Cancel);
    spec.danger = true;
    spec.tone = ConfirmTone::Danger;
    spec.default_choice = ConfirmChoice::Cancel;
    if (ShowConfirmDialogEx(hwnd_, spec, dark_, accent_) != ConfirmChoice::Confirm) return;
    StartOp(std::move(request));
}

void GitWindow::ApplyHunkAction(int hunk_index, bool is_discard) {
    if (op_busy_ || hunk_index < 0 || hunk_index >= static_cast<int>(hunk_patches_.size())) return;
    const std::string& patch = hunk_patches_[static_cast<size_t>(hunk_index)].patch_bytes;
    if (patch.empty()) return;
    if (is_discard) {
        ConfirmDialogSpec spec;
        spec.title = Str(Sid::GitHunkDiscardTitle);
        spec.message = Str(Sid::GitHunkDiscardMsg);
        spec.confirm_text = GitStr(plain_terms_, Sid::GitHunkDiscardPlain, Sid::GitHunkDiscardGit);
        spec.cancel_text = Str(Sid::Cancel);
        spec.danger = true;
        spec.tone = ConfirmTone::Danger;
        spec.default_choice = ConfirmChoice::Cancel;
        spec.note = GitFmt(Str(Sid::GitCmdDetailsSummary), L"{cmd}", L"git apply -R");
        if (ShowConfirmDialogEx(hwnd_, spec, dark_, accent_) != ConfirmChoice::Confirm) return;
        OpRequest req;
        req.kind = OpKind::RevertHunk;
        req.patch_bytes = patch;
        StartOp(std::move(req));
    } else {
        OpRequest req;
        req.kind = sel_section_ == Section::Staged ? OpKind::UnstageHunk : OpKind::StageHunk;
        req.patch_bytes = patch;
        StartOp(std::move(req));
    }
}

std::wstring GitWindow::Message() const {
    if (!edit_) return {};
    const int n = GetWindowTextLengthW(edit_);
    std::wstring text(static_cast<size_t>(n), L'\0');
    if (n > 0) GetWindowTextW(edit_, text.data(), n + 1);
    return text;
}

std::wstring GitWindow::SuggestedMessage() const {
    if (amend_ || set_.staged.empty()) return {};
    return app::GitSuggestCommitMessage(set_.staged, plain_terms_);
}

bool GitWindow::CanCommit() const {
    if (op_busy_ || !loaded_ || !set_.conflicts.empty()) return false;
    if (amend_) return set_.HasHead();
    if (set_.staged.empty()) return false;
    const std::wstring message = Message();
    return std::any_of(message.begin(), message.end(), [](wchar_t c) { return !iswspace(c); });
}

void GitWindow::Commit(bool also_push) {
    if (!CanCommit()) {
        if (!op_busy_ && loaded_ && set_.staged.empty() && !amend_) SetNotice(Str(Sid::GitScNothingStaged), true);
        return;
    }
    OpRequest request;
    request.kind = also_push ? OpKind::CommitAndPush : OpKind::Commit;
    request.message = Message();
    request.amend = amend_;
    request.arg1 = set_.branch;
    request.flag1 = set_.upstream.empty();
    StartOp(std::move(request));
}

void GitWindow::UndoCommit() {
    if (undo_head_.empty() || op_busy_) return;
    OpRequest request;
    request.kind = OpKind::UndoCommit;
    request.expect_head = undo_head_;
    StartOp(std::move(request));
}

void GitWindow::PromptStashPush() {
    if (op_busy_ || set_.Total() == 0) return;
    ConfirmDialogSpec spec;
    spec.title = GitFmtCount(Str(Sid::GitStashTitle), static_cast<int>(set_.Total()));
    spec.message = Str(Sid::GitStashMsg);
    spec.confirm_text = Str(Sid::GitStashConfirm);
    spec.cancel_text = Str(Sid::Cancel);
    spec.tone = ConfirmTone::Info;
    spec.note = GitFmt(Str(Sid::GitCmdDetailsSummary), L"{cmd}", L"git stash push -u");
    if (ShowConfirmDialogEx(hwnd_, spec, dark_, accent_) != ConfirmChoice::Confirm) return;
    OpRequest req;
    req.kind = OpKind::StashPush;
    req.message = L"Pulse stash";
    StartOp(std::move(req));
}

void GitWindow::PromptSwitchBranch(const std::wstring& branch, int dirty_count) {
    if (op_busy_) return;
    bool auto_stash = false;
    if (dirty_count > 0) {
        ConfirmDialogSpec spec;
        spec.title = GitFmt(Str(Sid::GitSwitchDirtyTitle), L"{branch}", branch);
        spec.message = GitFmtCount(Str(Sid::GitSwitchDirtyMsg), dirty_count);
        spec.confirm_text = Str(Sid::GitSwitchStashOpt);
        spec.secondary_text = Str(Sid::GitSwitchCarryOpt);
        spec.cancel_text = Str(Sid::Cancel);
        spec.tone = ConfirmTone::Question;
        spec.default_choice = ConfirmChoice::Confirm;
        spec.note = L"git stash push -u && git switch " + branch;
        const ConfirmChoice choice = ShowConfirmDialogEx(hwnd_, spec, dark_, accent_);
        if (choice == ConfirmChoice::Cancel) return;
        auto_stash = (choice == ConfirmChoice::Confirm);
    }
    OpRequest req;
    req.kind = OpKind::SwitchBranch;
    req.arg1 = branch;
    req.flag1 = auto_stash;
    StartOp(std::move(req));
}

void GitWindow::PromptMergeBranch(const std::wstring& branch) {
    if (op_busy_) return;
    ConfirmDialogSpec spec;
    spec.title = GitFmt(Str(Sid::GitMergeConfirmTitle), L"{branch}", branch);
    spec.message = Str(Sid::GitMergeConfirmMsg);
    spec.confirm_text = Str(Sid::GitMergeBtn);
    spec.cancel_text = Str(Sid::Cancel);
    spec.tone = ConfirmTone::Question;
    spec.note = L"git merge " + branch;
    if (ShowConfirmDialogEx(hwnd_, spec, dark_, accent_) != ConfirmChoice::Confirm) return;
    OpRequest req;
    req.kind = OpKind::MergeBranch;
    req.arg1 = branch;
    StartOp(std::move(req));
}

void GitWindow::PromptDropStash(const std::wstring& ref, const std::wstring& message) {
    if (op_busy_) return;
    ConfirmDialogSpec spec;
    spec.title = GitFmt(Str(Sid::GitStashDropTitle), L"{name}", message.empty() ? ref : message);
    spec.message = Str(Sid::GitStashDropMsg);
    spec.confirm_text = Str(Sid::GitStashDropBtn);
    spec.cancel_text = Str(Sid::Cancel);
    spec.danger = true;
    spec.tone = ConfirmTone::Danger;
    spec.default_choice = ConfirmChoice::Cancel;
    spec.note = L"git stash drop " + ref;
    if (ShowConfirmDialogEx(hwnd_, spec, dark_, accent_) != ConfirmChoice::Confirm) return;
    OpRequest req;
    req.kind = OpKind::StashDrop;
    req.arg1 = ref;
    StartOp(std::move(req));
}

GitBranchPopupCallbacks GitWindow::MakePopupCallbacks() {
    GitBranchPopupCallbacks cb;
    cb.on_sync = [this] {
        OpRequest req;
        req.kind = OpKind::Sync;
        req.arg1 = set_.branch;
        req.flag1 = set_.upstream.empty();
        StartOp(std::move(req));
    };
    cb.on_pull = [this] {
        OpRequest req;
        req.kind = OpKind::Pull;
        StartOp(std::move(req));
    };
    cb.on_push = [this] {
        OpRequest req;
        req.kind = OpKind::Push;
        req.arg1 = set_.branch;
        req.flag1 = set_.upstream.empty();
        StartOp(std::move(req));
    };
    cb.on_fetch = [this] {
        OpRequest req;
        req.kind = OpKind::Fetch;
        StartOp(std::move(req));
    };
    cb.on_switch_branch = [this](const std::wstring& branch, int dirty) { PromptSwitchBranch(branch, dirty); };
    cb.on_checkout_remote = [this](const std::wstring& remote_ref) {
        OpRequest req;
        req.kind = OpKind::CheckoutRemote;
        req.arg1 = remote_ref;
        StartOp(std::move(req));
    };
    cb.on_create_branch = [this](const std::wstring& name) {
        OpRequest req;
        req.kind = OpKind::CreateBranch;
        req.arg1 = name;
        StartOp(std::move(req));
    };
    cb.on_merge_branch = [this](const std::wstring& branch) { PromptMergeBranch(branch); };
    cb.on_stash_apply = [this](const std::wstring& ref) {
        OpRequest req;
        req.kind = OpKind::StashApply;
        req.arg1 = ref;
        StartOp(std::move(req));
    };
    cb.on_stash_drop = [this](const std::wstring& ref, const std::wstring& msg) { PromptDropStash(ref, msg); };
    cb.on_open_history = [this] { SetActiveTab(GitViewTab::History); };
    return cb;
}

void GitWindow::TriggerBranchPopup(POINT screen_pt, bool open_above) {
    if (root_.empty() || !hwnd_) return;
    branch_popup_.Show(hwnd_, root_, screen_pt, open_above, dark_, accent_, plain_terms_, MakePopupCallbacks());
}

// ---------------------------------------------------------------------------
// View / Layout / Formats

void GitWindow::RecreateFormats() {
    auto* factory = compositor_.DwriteFactory();
    if (!factory) return;
    mono_.reset();
    gutter_.reset();
    ui_.reset();
    ui_bold_.reset();
    small_.reset();
    small_bold_.reset();
    letter_.reset();
    const wchar_t* mono_family = L"Consolas";
    {
        ComPtr<IDWriteFontCollection> fonts;
        if (SUCCEEDED(factory->GetSystemFontCollection(&fonts, FALSE)) && fonts.get()) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(fonts->FindFamilyName(L"Cascadia Mono", &index, &exists)) && exists)
                mono_family = L"Cascadia Mono";
        }
    }
    std::wstring ui_family = L"Segoe UI";
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"";
    if (IDWriteTextFormat* base = compositor_.TextFormat()) {
        const UINT32 len = base->GetFontFamilyNameLength();
        std::wstring family(len + 1, L'\0');
        if (SUCCEEDED(base->GetFontFamilyName(family.data(), len + 1))) {
            family.resize(len);
            if (!family.empty()) ui_family = family;
        }
        base->GetLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
    }
    auto make = [&](const wchar_t* family, DWRITE_FONT_WEIGHT weight, float dip, DWRITE_TEXT_ALIGNMENT align,
                    ComPtr<IDWriteTextFormat>& out) {
        if (FAILED(factory->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, dip * scale_ * typography::UiFontScale(),
                                             locale, &out)) ||
            !out.get())
            return;
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        out->SetTextAlignment(align);
    };
    make(mono_family, DWRITE_FONT_WEIGHT_NORMAL, 12.5f, DWRITE_TEXT_ALIGNMENT_LEADING, mono_);
    make(mono_family, DWRITE_FONT_WEIGHT_NORMAL, 11.0f, DWRITE_TEXT_ALIGNMENT_TRAILING, gutter_);
    make(ui_family.c_str(), DWRITE_FONT_WEIGHT_NORMAL, 13.0f, DWRITE_TEXT_ALIGNMENT_LEADING, ui_);
    make(ui_family.c_str(), DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f, DWRITE_TEXT_ALIGNMENT_LEADING, ui_bold_);
    // 12 px: the painter's caption size (Pulse's secondary text everywhere else).
    make(ui_family.c_str(), DWRITE_FONT_WEIGHT_NORMAL, 12.0f, DWRITE_TEXT_ALIGNMENT_LEADING, small_);
    make(ui_family.c_str(), DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.0f, DWRITE_TEXT_ALIGNMENT_LEADING, small_bold_);
    make(mono_family, DWRITE_FONT_WEIGHT_BOLD, 11.0f, DWRITE_TEXT_ALIGNMENT_CENTER, letter_);
    char_w_ = mono_.get() ? Measure(L"0000000000", mono_.get()) / 10.0f : 8.0f * scale_;
    if (char_w_ <= 0.0f) char_w_ = 8.0f * scale_;
}

float GitWindow::Measure(std::wstring_view text, IDWriteTextFormat* format) const {
    auto* factory = compositor_.DwriteFactory();
    if (!factory || !format || text.empty()) return 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, 100000.0f, 1000.0f,
                                         &layout)) ||
        !layout.get())
        return 0.0f;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

float GitWindow::MeasureWrappedHeight(std::wstring_view text, float width, IDWriteTextFormat* format) const {
    auto* factory = compositor_.DwriteFactory();
    if (!factory || !format || text.empty() || width <= 4.0f) return 20.0f * scale_;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, width, 10000.0f,
                                         &layout)) ||
        !layout.get())
        return 20.0f * scale_;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return (std::max)(20.0f * scale_, metrics.height);
}

void GitWindow::CreateEdits() {
    if (!hwnd_) return;
    if (!edit_) {
        edit_ = CreateWindowExW(WS_EX_LAYERED, L"EDIT", L"",
                                WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                                0, 0, 0, 0, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (edit_) {
            SetLayeredWindowAttributes(edit_, 0, 255, LWA_ALPHA);
            SetWindowTheme(edit_, L"", L"");
            SendMessageW(edit_, EM_LIMITTEXT, 20000, 0);
            SetWindowSubclass(edit_, EditProc, kEditSubclass, reinterpret_cast<DWORD_PTR>(this));
        }
    }
    auto make_sub = [&](HWND& target, UINT_PTR sub_id, bool multiline) {
        if (target) return;
        const DWORD style = multiline ? (ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN) : 0;
        target = CreateRedirectedChildEdit(hwnd_, L"", style);
        if (target) {
            SetWindowTheme(target, L"", L"");
            SendMessageW(target, EM_LIMITTEXT, multiline ? 20000 : 256, 0);
            SetWindowSubclass(target, SubEditProc, sub_id, reinterpret_cast<DWORD_PTR>(this));
        }
    };
    make_sub(hist_search_edit_, kSubEditHistSearch, false);
    make_sub(hist_branch_edit_, kSubEditHistBranch, false);
    make_sub(cf_custom_edit_, kSubEditCfCustom, true);
}

void GitWindow::SyncChildEditsVisibility() {
    auto toggle = [&](HWND h, bool show) {
        if (!h) return;
        ShowWindow(h, show ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (show) {
            PresentChildEdit(compositor_, compositor_.TextFormat(), ColorFromRef(EditTextColor(dark_)),
                             ColorFromRef(EditBackColor(dark_)), h);
        }
    };
    toggle(edit_, active_tab_ == GitViewTab::Changes);
    toggle(hist_search_edit_, active_tab_ == GitViewTab::History);
    toggle(hist_branch_edit_, active_tab_ == GitViewTab::History && hist_creating_branch_);
    toggle(cf_custom_edit_, active_tab_ == GitViewTab::Conflict && cf_editing_hunk_ >= 0);
}

void GitWindow::LayoutEdits() {
    if (!hwnd_) return;
    if (!edit_font_) {
        edit_font_ = typography::CreateEditFont(scale_);
        if (edit_font_) {
            for (HWND h : {edit_, hist_search_edit_, hist_branch_edit_, cf_custom_edit_}) {
                if (!h) continue;
                SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_), TRUE);
                SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
            }
            if (edit_) {
                const int line = EditLineHeight(edit_, edit_font_, 1000);
                SetPropW(edit_, L"Pulse.EditMultiline", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(line) * 64u));
            }
            if (cf_custom_edit_) {
                const int line = EditLineHeight(cf_custom_edit_, edit_font_, 1000);
                SetPropW(cf_custom_edit_, L"Pulse.EditMultiline",
                         reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(line) * 64u));
            }
        }
    }

    const Theme theme = CurrentTheme();
    const COLORREF cue_rgb = RGB(static_cast<int>(theme.text_secondary.r * 255.0f),
                                 static_cast<int>(theme.text_secondary.g * 255.0f),
                                 static_cast<int>(theme.text_secondary.b * 255.0f));
    auto set_cue = [&](HWND h, std::wstring& stored, const std::wstring& next) {
        if (!h || next == stored) return;
        stored = next;
        SetPropW(h, L"Pulse.EditCue", const_cast<wchar_t*>(stored.c_str()));
        SetPropW(h, L"Pulse.EditCueColor", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(0x01000000u | cue_rgb)));
        InvalidateRect(h, nullptr, TRUE);
    };

    const std::wstring br = set_.branch.empty() ? Str(Sid::GitScDetached) : set_.branch;
    const std::wstring cue = amend_
        ? GitStr(plain_terms_, Sid::GitAmendCuePlain, Sid::GitScAmendCue)
        : GitFmt(GitStr(plain_terms_, Sid::GitMsgCuePlain, Sid::GitScMessageCue), L"{branch}", br);
    set_cue(edit_, edit_cue_, cue);
    set_cue(hist_search_edit_, hist_search_cue_, GitStr(plain_terms_, Sid::GitHistSearchPlain, Sid::GitHistSearchGit));
    set_cue(hist_branch_edit_, hist_branch_cue_, Str(Sid::GitNewBranch));

    auto place = [&](HWND h, const D2D1_RECT_F& rc, float inset_x, float inset_y, bool visible) {
        if (!h) return;
        if (!visible || rc.right <= rc.left) {
            if (IsWindowVisible(h)) ShowWindow(h, SW_HIDE);
            return;
        }
        const int x = static_cast<int>(std::lround(rc.left + inset_x));
        const int y = static_cast<int>(std::lround(rc.top + inset_y));
        const int w = (std::max)(20, static_cast<int>(std::lround(rc.right - rc.left - 2.0f * inset_x)));
        const int bh = (std::max)(16, static_cast<int>(std::lround(rc.bottom - rc.top - 2.0f * inset_y)));
        SetWindowPos(h, HWND_TOP, x, y, w, bh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        PresentChildEdit(compositor_, compositor_.TextFormat(), ColorFromRef(EditTextColor(dark_)),
                         ColorFromRef(EditBackColor(dark_)), h);
    };

    place(edit_, field_, 10.0f * scale_, 7.0f * scale_, active_tab_ == GitViewTab::Changes);
    place(hist_search_edit_, hist_search_rc_, 28.0f * scale_, 5.0f * scale_, active_tab_ == GitViewTab::History);
    place(hist_branch_edit_, hist_branch_box_, 8.0f * scale_, 5.0f * scale_,
          active_tab_ == GitViewTab::History && hist_creating_branch_);
    place(cf_custom_edit_, cf_custom_box_, 8.0f * scale_, 6.0f * scale_,
          active_tab_ == GitViewTab::Conflict && cf_editing_hunk_ >= 0);
}

void GitWindow::Layout() {
    const float s = scale_;
    const float w = static_cast<float>(compositor_.Width());
    const float h = static_cast<float>(compositor_.Height());
    // Self-drawn title bar: Pulse mark, the window's name and the repository,
    // the views on one segmented track, then the caption buttons. Below it the
    // command bar: stash actions on the left, branch & sync on the right.
    const float title_h = kTitleH * s;
    title_ = D2D1::RectF(0, 0, w, title_h);
    toolbar_ = D2D1::RectF(0, title_h, w, title_h + kToolbarH * s);
    status_ = D2D1::RectF(0, (std::max)(toolbar_.bottom, h - kStatusH * s), w, h);

    const float cap_w = 46.0f * s;
    caption_close_ = D2D1::RectF(w - cap_w, 0, w, title_h);
    caption_max_ = D2D1::RectF(caption_close_.left - cap_w, 0, caption_close_.left, title_h);
    caption_min_ = D2D1::RectF(caption_max_.left - cap_w, 0, caption_max_.left, title_h);

    const float mark = 20.0f * s;
    logo_rc_ = D2D1::RectF(14.0f * s, (title_h - mark) * 0.5f, 14.0f * s + mark, (title_h + mark) * 0.5f);

    // Views: icon 16 + gap 6 + label (13 px semibold when chosen) + insets + room.
    auto seg_w = [&](const std::wstring& text) { return Measure(text, ui_bold_.get()) + 46.0f * s; };
    const std::wstring chg_t = GitStr(plain_terms_, Sid::GitChangesPlain, Sid::GitChangesGit) +
                               (set_.Total() > 0 ? L" (" + std::to_wstring(set_.Total()) + L")" : L"");
    const std::wstring hist_t = GitStr(plain_terms_, Sid::GitHistoryPlain, Sid::GitHistoryGit);
    const bool has_conflict_tab = merging_ || !set_.conflicts.empty();
    const std::wstring cf_t = GitStr(plain_terms_, Sid::GitConflictsPlain, Sid::GitConflictsGit);
    const float tabs_w = seg_w(chg_t) + seg_w(hist_t) + (has_conflict_tab ? seg_w(cf_t) : 0.0f);
    // The title gives way to the views when the window is narrow.
    const std::wstring name_t = GitStr(plain_terms_, Sid::GitCenterPlain, Sid::GitCenterGit);
    const float name_w = Measure(name_t, ui_bold_.get());
    const float repo_w = Measure(repo_name_, ui_.get());
    const float caption_left = caption_min_.left - 12.0f * s;
    float lx = logo_rc_.right + 10.0f * s;
    title_text_w_ = 0.0f;
    title_repo_w_ = 0.0f;
    if (lx + name_w + 10.0f * s + repo_w + 24.0f * s + tabs_w <= caption_left) {
        title_text_w_ = name_w;
        title_repo_w_ = repo_w;
        lx += name_w + 10.0f * s + repo_w + 24.0f * s;
    } else if (lx + name_w + 24.0f * s + tabs_w <= caption_left) {
        title_text_w_ = name_w;
        lx += name_w + 24.0f * s;
    } else {
        lx += 6.0f * s;
    }
    const float seg_top = (title_h - 30.0f * s) * 0.5f, seg_bot = seg_top + 30.0f * s;
    tab_changes_rc_ = D2D1::RectF(lx, seg_top, lx + seg_w(chg_t), seg_bot);
    tab_history_rc_ = D2D1::RectF(tab_changes_rc_.right, seg_top, tab_changes_rc_.right + seg_w(hist_t), seg_bot);
    tab_conflict_rc_ = D2D1::RectF(0, 0, 0, 0);
    if (has_conflict_tab)
        tab_conflict_rc_ = D2D1::RectF(tab_history_rc_.right, seg_top, tab_history_rc_.right + seg_w(cf_t), seg_bot);

    // Command bar.
    const float tab_top = toolbar_.top + 6.0f * s, tab_bot = toolbar_.bottom - 6.0f * s;
    const std::wstring stash_t = GitStr(plain_terms_, Sid::GitStashPlain, Sid::GitStashGit);
    const std::wstring stash_list_t = GitStr(plain_terms_, Sid::GitStashListPlain, Sid::GitStashListGit) +
                                      (stash_count_ > 0 ? L" (" + std::to_wstring(stash_count_) + L")" : L"");
    const float st_w = painter_.MeasureButtonWidth(stash_t, L"\xE7B8");
    const float stl_w = painter_.MeasureButtonWidth(stash_list_t, L"\xE8F1");

    // Right side: branch, sync, terms, refresh. When narrow the terms and sync
    // labels fold to their icons (a 32-DIP square Render draws icon-only), then
    // the branch name shortens, then the stash labels fold too.
    const std::wstring terms_t = Str(plain_terms_ ? Sid::GitTermsPlain : Sid::GitTermsTechnical);
    std::wstring sync_t = Str(Sid::GitSyncLabel);
    if (set_.behind > 0) sync_t += L" \u2193" + std::to_wstring(set_.behind);
    if (set_.ahead > 0) sync_t += L" \u2191" + std::to_wstring(set_.ahead);
    const std::wstring br_t = set_.branch.empty() ? (set_.head.empty() ? L"HEAD" : set_.head) : set_.branch;
    const float icon_btn = 32.0f * s, gap = 4.0f * s;
    const float trm_full = painter_.MeasureButtonWidth(terms_t, L"\xE8C1");
    const float sy_full = painter_.MeasureButtonWidth(sync_t, L"\xE895");
    const float br_full = (std::min)(220.0f * s, painter_.MeasureButtonWidth(br_t, L"\xF003", true));
    const float br_min = (std::min)(br_full, 110.0f * s);
    const float bar_left = 12.0f * s;
    const float stash_min = 2.0f * icon_btn + 4.0f * s + 16.0f * s;
    const float right_room = (w - 12.0f * s) - bar_left - stash_min;
    auto right_need = [&](float trm, float sy, float br) { return icon_btn + trm + sy + br + 3.0f * gap + 9.0f * s; };
    float trm_w = trm_full, sy_w = sy_full, br_w = br_full;
    if (right_need(trm_w, sy_w, br_w) > right_room) trm_w = icon_btn;
    if (right_need(trm_w, sy_w, br_w) > right_room) sy_w = icon_btn;
    if (right_need(trm_w, sy_w, br_w) > right_room)
        br_w = (std::max)(br_min, right_room - right_need(trm_w, sy_w, 0.0f));

    float rx = w - 12.0f * s;
    refresh_rc_ = D2D1::RectF(rx - icon_btn, tab_top, rx, tab_bot);
    rx = refresh_rc_.left - gap;
    top_terms_rc_ = D2D1::RectF(rx - trm_w, tab_top, rx, tab_bot);
    // A short divider between the view options (terms, refresh) and the repository ones.
    rx = top_terms_rc_.left - gap - 9.0f * s;
    top_sync_rc_ = D2D1::RectF(rx - sy_w, tab_top, rx, tab_bot);
    rx = top_sync_rc_.left - gap;
    top_branch_rc_ = D2D1::RectF(rx - br_w, tab_top, rx, tab_bot);

    top_stash_rc_ = top_stash_list_rc_ = D2D1::RectF(0, 0, 0, 0);
    const float stash_room = top_branch_rc_.left - 16.0f * s - bar_left;
    float st_use = 0.0f, stl_use = 0.0f;
    if (st_w + stl_w + 4.0f * s <= stash_room) {
        st_use = st_w;
        stl_use = stl_w;
    } else if (2.0f * icon_btn + 4.0f * s <= stash_room) {
        st_use = stl_use = icon_btn;
    }
    if (st_use > 0.0f) {
        top_stash_rc_ = D2D1::RectF(bar_left, tab_top, bar_left + st_use, tab_bot);
        top_stash_list_rc_ = D2D1::RectF(top_stash_rc_.right + 4.0f * s, tab_top,
                                         top_stash_rc_.right + 4.0f * s + stl_use, tab_bot);
    }

    // Undo commit link in status bar
    undo_rc_ = D2D1::RectF(0, 0, 0, 0);
    if (!undo_head_.empty() && !op_busy_) {
        const std::wstring ut = GitStr(plain_terms_, Sid::GitUndoSavePlain, Sid::GitScUndoCommit);
        const float uw = Measure(ut, small_.get()) + 16.0f * s;
        undo_rc_ = D2D1::RectF(w - 10.0f * s - uw, status_.top + 3.0f * s, w - 10.0f * s, status_.bottom - 3.0f * s);
    }

    if (active_tab_ == GitViewTab::History) {
        LayoutHistory(w, h);
        return;
    }
    if (active_tab_ == GitViewTab::Conflict) {
        LayoutConflict(w, h);
        return;
    }

    // Changes tab layout
    // The list sits on the window like Pulse's sidebar; the diff is a card
    // like the main window's file list.
    const float lw = std::clamp(340.0f * s, 280.0f * s, (std::max)(280.0f * s, w * 0.42f));
    left_panel_ = D2D1::RectF(0, toolbar_.bottom, lw, status_.top);
    const float pad = 14.0f * s;
    field_ = D2D1::RectF(pad, left_panel_.top + 4.0f * s, lw - pad, left_panel_.top + 4.0f * s + 76.0f * s);

    float cy = field_.bottom + 8.0f * s;
    suggest_rc_ = D2D1::RectF(0, 0, 0, 0);
    const std::wstring sg = SuggestedMessage();
    if (!sg.empty() && Message().empty()) {
        const float lbl_w = Measure(Str(Sid::GitSuggestLabel), small_.get()) + 6.0f * s;
        const float chip_w = (std::min)(lw - 2.0f * pad - lbl_w, Measure(sg, small_.get()) + 28.0f * s);
        suggest_rc_ = D2D1::RectF(pad + lbl_w, cy, pad + lbl_w + chip_w, cy + 24.0f * s);
        cy = suggest_rc_.bottom + 8.0f * s;
    }

    // Split commit + push button
    // Save (primary) and save-and-upload (a standard icon button beside it).
    const float push_w = 32.0f * s;
    commit_push_rc_ = D2D1::RectF(lw - pad - push_w, cy, lw - pad, cy + 32.0f * s);
    commit_rc_ = D2D1::RectF(pad, cy, commit_push_rc_.left - 6.0f * s, cy + 32.0f * s);
    cy = commit_rc_.bottom + 10.0f * s;

    // DrawCheckBox: an 18-DIP box, then its label in the 14 px body style.
    const float amend_w = 18.0f * s + 8.0f * s + painter_.MeasureButtonWidth(Str(Sid::GitScAmend)) - 8.0f * s;
    amend_rc_ = D2D1::RectF(pad, cy, (std::min)(lw - pad, pad + amend_w), cy + 24.0f * s);
    who_rc_ = D2D1::RectF(pad, amend_rc_.bottom + 6.0f * s, lw - pad, amend_rc_.bottom + 26.0f * s);
    list_ = D2D1::RectF(0, who_rc_.bottom + 14.0f * s, lw, status_.top);

    diff_card_ = D2D1::RectF(lw, toolbar_.bottom, w - 12.0f * s, status_.top);
    // Inside the card's 1 px edge; the body stops short of the rounded bottom corners.
    diff_header_ = D2D1::RectF(diff_card_.left + 1.0f, diff_card_.top + 1.0f, diff_card_.right - 1.0f,
                               diff_card_.top + 1.0f + kDiffHeaderH * s);
    diff_body_ = D2D1::RectF(diff_card_.left + 1.0f, diff_header_.bottom, diff_card_.right - 1.0f,
                             diff_card_.bottom - 8.0f * s);

    // Diff header controls, right aligned
    stage_btn_ = discard_btn_ = restore_deleted_btn_ = mode_inline_rc_ = mode_sbs_rc_ = blame_rc_ = D2D1::RectF(0, 0, 0, 0);
    const float top = diff_header_.top + 9.0f * s;
    const float bottom = top + 32.0f * s;
    float right = diff_header_.right - 10.0f * s;
    if (const GitChange* c = Selected()) {
        // Room for the file name before the controls; past it the switch goes,
        // then the view segments, then the two buttons keep only their icons.
        const float min_left = diff_header_.left + 16.0f * s + 120.0f * s;
        const Sid disc_id = c->kind == GitChangeKind::Untracked
            ? (plain_terms_ ? Sid::GitDeleteUntrackedPlain : Sid::GitDeleteUntrackedGit)
            : c->kind == GitChangeKind::Deleted
            ? (plain_terms_ ? Sid::GitRestoreDeletedPlain : Sid::GitRestoreDeletedGit)
            : (plain_terms_ ? Sid::GitDiscardPlain : Sid::GitScDiscard);
        const Sid stage_id = sel_section_ == Section::Staged
            ? (plain_terms_ ? Sid::GitStagedOnePlain : Sid::GitScUnstage)
            : sel_section_ == Section::Conflicts ? Sid::GitScResolve
            : (plain_terms_ ? Sid::GitStageOnePlain : Sid::GitScStage);
        const bool has_discard = sel_section_ == Section::Changes;
        const bool is_text = !app::GitIsImagePath(c->path) && c->kind != GitChangeKind::Deleted;
        const std::wstring bl_t = GitStr(plain_terms_, Sid::GitBlamePlain, Sid::GitBlameGit);
        const std::wstring sbs_t = GitStr(plain_terms_, Sid::GitSbsPlain, Sid::GitSbsGit);
        const std::wstring inl_t = GitStr(plain_terms_, Sid::GitInlinePlain, Sid::GitInlineGit);
        float dw = has_discard ? painter_.MeasureButtonWidth(Str(disc_id), L"\xE7A7") : 0.0f;
        float sw = painter_.MeasureButtonWidth(Str(stage_id), L"\xE710");
        // DrawSwitch: a 42-DIP track, a 12 gap, the 14 px body label.
        const float bl_w = is_text ? 42.0f * s + 12.0f * s + painter_.MeasureButtonWidth(bl_t) - 12.0f * s : 0.0f;
        // DrawTrackSegment: label in 13 px semibold, 6 inset each side, plus room.
        const float sbs_w = Measure(sbs_t, ui_bold_.get()) + 24.0f * s;
        const float inl_w = Measure(inl_t, ui_bold_.get()) + 24.0f * s;
        const float seg_total = is_text ? sbs_w + inl_w + 4.0f * s : 0.0f;
        bool show_blame = is_text, show_mode = is_text;
        auto need = [&] {
            return (dw > 0.0f ? dw + 8.0f * s : 0.0f) + sw + 10.0f * s + (show_blame ? bl_w + 12.0f * s : 0.0f) +
                   (show_mode ? seg_total : 0.0f);
        };
        if (right - need() < min_left) show_blame = false;
        if (right - need() < min_left) show_mode = false;
        if (right - need() < min_left) {
            if (dw > 0.0f) dw = 32.0f * s;
            sw = 32.0f * s;
        }
        if (has_discard) {
            discard_btn_ = D2D1::RectF(right - dw, top, right, bottom);
            right = discard_btn_.left - 8.0f * s;
        }
        stage_btn_ = D2D1::RectF(right - sw, top, right, bottom);
        right = stage_btn_.left - 10.0f * s;

        if (show_blame) {
            blame_rc_ = D2D1::RectF(right - bl_w, top, right, bottom);
            right = blame_rc_.left - 12.0f * s;
        }
        if (show_mode) {
            mode_sbs_rc_ = D2D1::RectF(right - 2.0f * s - sbs_w, top + 2.0f * s, right - 2.0f * s, bottom - 2.0f * s);
            mode_inline_rc_ = D2D1::RectF(mode_sbs_rc_.left - inl_w, top + 2.0f * s, mode_sbs_rc_.left, bottom - 2.0f * s);
        }
        if (c->kind == GitChangeKind::Deleted) {
            const float rw = painter_.MeasureButtonWidth(
                GitStr(plain_terms_, Sid::GitRestoreDeletedPlain, Sid::GitRestoreDeletedGit), L"\xE7A7");
            const float cx = (diff_body_.left + diff_body_.right) * 0.5f;
            const float cy_b = (diff_body_.top + diff_body_.bottom) * 0.5f + 36.0f * s;
            restore_deleted_btn_ = D2D1::RectF(cx - rw * 0.5f, cy_b, cx + rw * 0.5f, cy_b + 32.0f * s);
        }
    }
}

GitWindow::RowGeom GitWindow::GeomFor(int row) const {
    RowGeom g;
    const float s = scale_;
    const float rh = ListRowHeight();
    const float y = list_.top + static_cast<float>(row) * rh - list_scroll_;
    g.row = D2D1::RectF(list_.left + 6.0f * s, y, list_.right - 10.0f * s, y + rh);
    const float cy = y + rh / 2.0f;
    g.check = D2D1::RectF(g.row.left + 6.0f * s, cy - 10.0f * s, g.row.left + 26.0f * s, cy + 10.0f * s);

    if (row >= 0 && row < static_cast<int>(rows_.size())) {
        const auto& r = rows_[static_cast<size_t>(row)];
        if (r.index < 0) {
            if (r.section != Section::Conflicts) {
                const std::wstring act = r.section == Section::Staged
                    ? GitStr(plain_terms_, Sid::GitUnstageAllPlain, Sid::GitScUnstageAll)
                    : GitStr(plain_terms_, Sid::GitStageAllPlain, Sid::GitScStageAll);
                const float aw = painter_.MeasureButtonWidth(act);
                g.action = D2D1::RectF(g.row.right - aw, y + 2.0f * s, g.row.right, y + rh - 2.0f * s);
            }
        } else {
            const GitChange* c = ChangeAt(row);
            const std::wstring lbl = c ? PlainKindText(c->kind, plain_terms_) : std::wstring(L"M");
            const float lw = plain_terms_ ? painter_.MeasureTagWidth(lbl) : 20.0f * s;
            g.letter = D2D1::RectF(g.row.right - lw - 4.0f * s, cy - 10.0f * s, g.row.right - 4.0f * s, cy + 10.0f * s);
            g.reveal = D2D1::RectF(g.row.right - 26.0f * s, cy - 12.0f * s, g.row.right - 2.0f * s, cy + 12.0f * s);
            g.discard = D2D1::RectF(g.reveal.left - 26.0f * s, cy - 12.0f * s, g.reveal.left - 2.0f * s, cy + 12.0f * s);
        }
    }
    return g;
}

GitWindow::Hit GitWindow::HitTest(float x, float y) const {
    if (Inside(refresh_rc_, x, y)) return {HitKind::TopRefresh, -1};
    if (Inside(top_terms_rc_, x, y)) return {HitKind::TopTerms, -1};
    if (Inside(top_sync_rc_, x, y)) return {HitKind::TopSync, -1};
    if (Inside(top_branch_rc_, x, y)) return {HitKind::TopBranch, -1};
    if (Inside(top_stash_rc_, x, y)) return {HitKind::TopStash, -1};
    if (Inside(top_stash_list_rc_, x, y)) return {HitKind::TopStashList, -1};
    if (Inside(tab_changes_rc_, x, y)) return {HitKind::TabChanges, -1};
    if (Inside(tab_history_rc_, x, y)) return {HitKind::TabHistory, -1};
    if (Inside(tab_conflict_rc_, x, y)) return {HitKind::TabConflict, -1};
    if (Inside(undo_rc_, x, y)) return {HitKind::Undo, -1};

    if (active_tab_ == GitViewTab::History) return HitTestHistory(x, y);
    if (active_tab_ == GitViewTab::Conflict) return HitTestConflict(x, y);

    if (Inside(commit_push_rc_, x, y)) return {HitKind::CommitPush, -1};
    if (Inside(commit_rc_, x, y)) return {HitKind::Commit, -1};
    if (Inside(suggest_rc_, x, y)) return {HitKind::SuggestChip, -1};
    if (Inside(amend_rc_, x, y)) return {HitKind::Amend, -1};
    if (Inside(field_, x, y)) return {HitKind::Field, -1};
    if (Inside(mode_inline_rc_, x, y)) return {HitKind::DiffModeInline, -1};
    if (Inside(mode_sbs_rc_, x, y)) return {HitKind::DiffModeSbs, -1};
    if (Inside(blame_rc_, x, y)) return {HitKind::DiffBlameToggle, -1};
    if (Inside(stage_btn_, x, y)) return {HitKind::DiffStage, -1};
    if (Inside(discard_btn_, x, y)) return {HitKind::DiffDiscard, -1};
    if (Inside(restore_deleted_btn_, x, y)) return {HitKind::DiffRestoreDeleted, -1};

    if (Inside(list_, x, y)) {
        fluent::ScrollbarSpec bar{list_, list_scroll_, list_.bottom - list_.top, ListContentHeight()};
        if (OnScrollbar(bar, x, y) && ListContentHeight() > list_.bottom - list_.top)
            return {HitKind::ListBar, -1};
        const int row = static_cast<int>((y - list_.top + list_scroll_) / ListRowHeight());
        if (row < 0 || row >= static_cast<int>(rows_.size())) return {};
        const RowGeom g = GeomFor(row);
        if (rows_[static_cast<size_t>(row)].index < 0) {
            if (Inside(g.action, x, y)) return {HitKind::SectionAction, row};
            return {};
        }
        if (Inside(g.check, x, y)) return {HitKind::RowCheck, row};
        if (Inside(g.reveal, x, y)) return {HitKind::RowReveal, row};
        if (rows_[static_cast<size_t>(row)].section == Section::Changes && Inside(g.discard, x, y))
            return {HitKind::RowDiscard, row};
        return {HitKind::Row, row};
    }

    if (Inside(diff_body_, x, y)) {
        if (diff_is_image_ && has_old_img_ && has_new_img_) {
            const DecodedGitImage* ref = new_img_meta_.ok() ? &new_img_meta_ : &old_img_meta_;
            const auto lay = LayoutGitImageCompare(diff_body_, ref->orig_w, ref->orig_h, img_split_, scale_);
            if (Inside(lay.image, x, y)) return {HitKind::DiffImageHandle, -1};
        }
        for (const auto& hb : hunk_btn_rects_) {
            if (Inside(hb.discard_rc, x, y)) return {HitKind::HunkDiscard, -1, hb.hunk_index};
            if (Inside(hb.stage_rc, x, y)) return {HitKind::HunkStage, -1, hb.hunk_index};
        }
        fluent::ScrollbarSpec bar{diff_body_, diff_scroll_, diff_body_.bottom - diff_body_.top, DiffContentHeight()};
        if (OnScrollbar(bar, x, y) && DiffContentHeight() > diff_body_.bottom - diff_body_.top)
            return {HitKind::DiffBar, -1};
        return {HitKind::DiffBody, -1};
    }
    return {};
}

void GitWindow::OnClick(const Hit& hit, float x, float y) {
    switch (hit.kind) {
    case HitKind::TabChanges: SetActiveTab(GitViewTab::Changes); return;
    case HitKind::TabHistory: SetActiveTab(GitViewTab::History); return;
    case HitKind::TabConflict: SetActiveTab(GitViewTab::Conflict); return;
    case HitKind::TopRefresh:
        StartOp(OpRequest{});
        if (active_tab_ == GitViewTab::History) StartHistoryLoad();
        else if (active_tab_ == GitViewTab::Conflict) StartConflictStateLoad();
        return;
    case HitKind::TopTerms:
        SwitchTerms(!plain_terms_);
        return;
    case HitKind::TopStash:
        PromptStashPush();
        return;
    case HitKind::TopBranch:
    case HitKind::TopStashList: {
        const D2D1_RECT_F anchor_rc = hit.kind == HitKind::TopBranch ? top_branch_rc_ : top_stash_list_rc_;
        POINT pt{static_cast<LONG>(anchor_rc.left), static_cast<LONG>(anchor_rc.bottom + 4.0f * scale_)};
        ClientToScreen(hwnd_, &pt);
        TriggerBranchPopup(pt, false);
        return;
    }
    case HitKind::TopSync: {
        OpRequest req;
        req.kind = OpKind::Sync;
        req.arg1 = set_.branch;
        req.flag1 = set_.upstream.empty();
        StartOp(std::move(req));
        return;
    }
    case HitKind::Undo:
        UndoCommit();
        return;
    default:
        break;
    }

    if (active_tab_ == GitViewTab::History && OnClickHistory(hit, x, y)) return;
    if (active_tab_ == GitViewTab::Conflict && OnClickConflict(hit, x, y)) return;

    switch (hit.kind) {
    case HitKind::Commit: Commit(false); break;
    case HitKind::CommitPush: Commit(true); break;
    case HitKind::SuggestChip: {
        const std::wstring sg = SuggestedMessage();
        if (!sg.empty() && edit_) {
            SetWindowTextW(edit_, sg.c_str());
            SetFocus(edit_);
            const int len = GetWindowTextLengthW(edit_);
            SendMessageW(edit_, EM_SETSEL, len, len);
            Layout();
        }
        break;
    }
    case HitKind::Amend:
        if (set_.HasHead()) {
            amend_ = !amend_;
            LayoutEdits();
        }
        break;
    case HitKind::Field:
        if (edit_) SetFocus(edit_);
        break;
    case HitKind::DiffModeInline:
        sbs_mode_ = false;
        ScrollDiffTo(diff_scroll_);
        break;
    case HitKind::DiffModeSbs:
        sbs_mode_ = true;
        ScrollDiffTo(diff_scroll_);
        break;
    case HitKind::DiffBlameToggle:
        blame_mode_ = !blame_mode_;
        StartDiff();
        break;
    case HitKind::RowCheck: ToggleStage(hit.row); break;
    case HitKind::RowDiscard:
        if (const GitChange* c = ChangeAt(hit.row)) Discard({c});
        break;
    case HitKind::RowReveal:
        if (const GitChange* c = ChangeAt(hit.row); c && on_reveal_file_) {
            on_reveal_file_(app::GitJoinPath(root_, c->path));
        }
        break;
    case HitKind::Row: Select(hit.row, false); break;
    case HitKind::SectionAction: StageSection(rows_[static_cast<size_t>(hit.row)].section); break;
    case HitKind::DiffStage: ToggleStage(sel_row_); break;
    case HitKind::DiffDiscard:
    case HitKind::DiffRestoreDeleted:
        if (const GitChange* c = Selected()) Discard({c});
        break;
    case HitKind::HunkStage:
        ApplyHunkAction(hit.sub, false);
        break;
    case HitKind::HunkDiscard:
        ApplyHunkAction(hit.sub, true);
        break;
    case HitKind::DiffImageHandle: {
        const DecodedGitImage* ref = new_img_meta_.ok() ? &new_img_meta_ : &old_img_meta_;
        const auto lay = LayoutGitImageCompare(diff_body_, ref->orig_w, ref->orig_h, img_split_, scale_);
        img_split_ = GitImageSplitFromX(lay, x);
        dragging_img_split_ = true;
        SetCapture(hwnd_);
        break;
    }
    case HitKind::ListBar:
    case HitKind::DiffBar: {
        const bool list = hit.kind == HitKind::ListBar;
        const D2D1_RECT_F view = list ? list_ : diff_body_;
        fluent::ScrollbarSpec bar{view, list ? list_scroll_ : diff_scroll_, view.bottom - view.top,
                                  list ? ListContentHeight() : DiffContentHeight()};
        const D2D1_RECT_F thumb = fluent::ScrollbarThumbRect(bar, scale_, GitHighContrast());
        drag_bar_ = list ? 1 : 2;
        drag_grab_ = fluent::ScrollbarGrabOffset(thumb.top, thumb.bottom - thumb.top, y);
        SetCapture(hwnd_);
        break;
    }
    default: break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void GitWindow::ScrollListTo(float y) {
    const float max_y = (std::max)(0.0f, ListContentHeight() - (list_.bottom - list_.top));
    list_scroll_ = std::clamp(y, 0.0f, max_y);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void GitWindow::ScrollDiffTo(float y) {
    const float max_y = (std::max)(0.0f, DiffContentHeight() - (diff_body_.bottom - diff_body_.top) + DiffRowHeight());
    diff_scroll_ = std::clamp(y, 0.0f, max_y);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void GitWindow::ScrollDiffX(float dx) {
    const float gutter = 2.0f * (static_cast<float>(number_digits_) * char_w_ + 14.0f * scale_) + 22.0f * scale_;
    const float view = (std::max)(0.0f, diff_body_.right - diff_body_.left - gutter - 24.0f * scale_);
    const float max_x = (std::max)(0.0f, static_cast<float>(max_cols_) * char_w_ - view);
    diff_scroll_x_ = std::clamp(diff_scroll_x_ + dx, 0.0f, max_x);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

const GitWindow::CharSpans& GitWindow::SpansFor(int diff_row) {
    auto it = spans_.find(diff_row);
    if (it != spans_.end()) return it->second;
    CharSpans spans;
    if (left_ && right_ && diff_row >= 0 && diff_row < static_cast<int>(result_.rows.size())) {
        const diff::DiffRow& r = result_.rows[static_cast<size_t>(diff_row)];
        if (r.kind == diff::RowKind::Mod && r.left >= 0 && r.right >= 0)
            diff::ComputeCharDiff(left_->lines[static_cast<size_t>(r.left)],
                                  right_->lines[static_cast<size_t>(r.right)],
                                  diff::DiffOptions{}, spans.left, spans.right);
    }
    return spans_.emplace(diff_row, std::move(spans)).first->second;
}

void GitWindow::Render() {
    if (!compositor_.Dc()) return;
    if (compositor_.NeedsRecovery()) {
        if (!compositor_.Recover()) return;
        compositor_.RecreateTextFormats(scale_);
        brush_.reset();
        hatch_.reset();
        logo_.reset();
        old_img_bmp_.reset();
        new_img_bmp_.reset();
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    compositor_.Resize((std::max)(1L, client.right), (std::max)(1L, client.bottom));
    Layout();
    LayoutEdits();
    EnsureHatch();

    auto* dc = compositor_.Dc();
    auto* factory = compositor_.DwriteFactory();
    const bool hc = GitHighContrast();
    const Theme theme = CurrentTheme();
    Palette pal = MakePalette(dark_);
    // Pulse's own surfaces: the window colour, and the white (dark: raised) card.
    pal.panel = theme.bg;
    pal.body = theme.surface_card;
    if (hc) pal.panel = pal.body = theme.bg;
    D2D1_COLOR_F link_ink = theme.accent;
    if (dark_ && !hc) {
        link_ink.r += (1.0f - link_ink.r) * 0.28f;
        link_ink.g += (1.0f - link_ink.g) * 0.28f;
        link_ink.b += (1.0f - link_ink.b) * 0.28f;
    }
    const float s = scale_;
    const float w = static_cast<float>(compositor_.Width());
    const float line_w = (std::max)(1.0f, std::floor(s));

    if (has_old_img_ && !old_img_bmp_.get()) old_img_bmp_ = CreateBitmapFromDecodedImage(dc, old_img_meta_);
    if (has_new_img_ && !new_img_bmp_.get()) new_img_bmp_ = CreateBitmapFromDecodedImage(dc, new_img_meta_);

    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->Clear(pal.panel);
    if (!brush_.get()) dc->CreateSolidColorBrush(theme.text, &brush_);
    painter_.SetCompositor(&compositor_);
    painter_.SetScale(s);
    const bool painter_ok = painter_.BeginFrame(theme, hc);

    auto fill = [&](const D2D1_RECT_F& rc, const D2D1_COLOR_F& color) {
        brush_->SetColor(color);
        dc->FillRectangle(rc, brush_.get());
    };
    auto text_at = [&](std::wstring_view text, const D2D1_RECT_F& rc, IDWriteTextFormat* format,
                       const D2D1_COLOR_F& color) {
        if (!format || text.empty() || rc.right <= rc.left) return;
        brush_->SetColor(color);
        dc->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format, rc, brush_.get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    };
    auto fit = [&](const std::wstring& text, float width, IDWriteTextFormat* format) {
        if (Measure(text, format) <= width) return text;
        return FitTextEnd(text, width, [&](std::wstring_view t) { return Measure(t, format); });
    };
    // Button labels are drawn in the painter's 14 px body style.
    auto fit_body = [&](const std::wstring& text, float width) {
        auto body_w = [&](std::wstring_view t) { return painter_.MeasureButtonWidth(t) - 16.0f * s; };
        if (body_w(text) <= width) return text;
        return FitTextEnd(text, width, body_w);
    };
    auto button = [&](const D2D1_RECT_F& rc, const std::wstring& text, std::wstring_view glyph,
                      fluent::ButtonKind kind, bool enabled, HitKind id, bool icon_only = false, bool drop = false) {
        if (!painter_ok || rc.right <= rc.left) return;
        fluent::ButtonSpec spec{};
        spec.bounds = rc;
        spec.text = text;
        spec.glyph = glyph;
        spec.kind = kind;
        spec.icon_only = icon_only;
        spec.drop_down = drop;
        spec.state.enabled = enabled;
        spec.state.hovered = enabled && hover_.kind == id;
        spec.state.pressed = enabled && pressed_.kind == id && hover_.kind == id;
        painter_.DrawButton(spec);
    };

    // ---- Title bar: mark, name, repository (the views and caption buttons follow) ----
    {
        if (ID2D1Bitmap* logo = Logo())
            dc->DrawBitmap(logo, logo_rc_, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        float tx = logo_rc_.right + 10.0f * s;
        if (title_text_w_ > 0.0f) {
            const std::wstring name_t = GitStr(plain_terms_, Sid::GitCenterPlain, Sid::GitCenterGit);
            text_at(name_t, D2D1::RectF(tx, title_.top, tx + title_text_w_ + 2.0f, title_.bottom), ui_bold_.get(),
                    theme.text);
            tx += title_text_w_ + 10.0f * s;
        }
        if (title_repo_w_ > 0.0f) {
            text_at(repo_name_, D2D1::RectF(tx, title_.top, tx + title_repo_w_ + 2.0f, title_.bottom), ui_.get(),
                    theme.text_secondary);
        }
        if (painter_ok) {
            auto caption = [&](const D2D1_RECT_F& rc, fluent::TitleBarButtonRole role, Caption id) {
                fluent::ControlState st{};
                st.hovered = caption_hover_ == id;
                st.pressed = caption_pressed_ == id && caption_hover_ == id;
                painter_.DrawTitleBarButton(rc, role, {}, st);
            };
            caption(caption_min_, fluent::TitleBarButtonRole::Minimize, Caption::Minimize);
            caption(caption_max_, IsZoomed(hwnd_) ? fluent::TitleBarButtonRole::Restore
                                                  : fluent::TitleBarButtonRole::Maximize, Caption::Maximize);
            caption(caption_close_, fluent::TitleBarButtonRole::Close, Caption::Close);
        }
    }
    // ---- Views & command bar ----
    {
        const std::wstring chg_t = GitStr(plain_terms_, Sid::GitChangesPlain, Sid::GitChangesGit) +
                                   (set_.Total() > 0 ? L" (" + std::to_wstring(set_.Total()) + L")" : L"");
        const std::wstring hist_t = GitStr(plain_terms_, Sid::GitHistoryPlain, Sid::GitHistoryGit);
        // The views: segments on one track, the chosen one a raised plate.
        const D2D1_RECT_F last_tab = tab_conflict_rc_.right > tab_conflict_rc_.left ? tab_conflict_rc_ : tab_history_rc_;
        if (painter_ok) {
            painter_.DrawSegmentedTrack(D2D1::RectF(tab_changes_rc_.left - 2.0f * s, tab_changes_rc_.top - 2.0f * s,
                                                    last_tab.right + 2.0f * s, last_tab.bottom + 2.0f * s));
            auto tab = [&](const D2D1_RECT_F& rc, const std::wstring& txt, std::wstring_view glyph, GitViewTab id,
                           HitKind hit) {
                if (rc.right <= rc.left) return;
                fluent::SegmentedItemSpec sp{};
                sp.bounds = rc;
                sp.text = txt;
                sp.glyph = glyph;
                sp.shared_track = true;
                sp.state.selected = active_tab_ == id;
                sp.state.hovered = hover_.kind == hit;
                sp.state.pressed = pressed_.kind == hit && hover_.kind == hit;
                painter_.DrawTrackSegment(sp);
            };
            tab(tab_changes_rc_, chg_t, L"\xE7A2", GitViewTab::Changes, HitKind::TabChanges);
            tab(tab_history_rc_, hist_t, L"\xE81C", GitViewTab::History, HitKind::TabHistory);
            tab(tab_conflict_rc_, GitStr(plain_terms_, Sid::GitConflictsPlain, Sid::GitConflictsGit), L"\xE7BA",
                GitViewTab::Conflict, HitKind::TabConflict);
        }
        // Layout folds a labelled button to a 32-DIP square when it runs out of room.
        auto icon_only = [&](const D2D1_RECT_F& rc) { return rc.right - rc.left <= 33.0f * s; };
        const bool compact = icon_only(top_stash_rc_);

        if (top_stash_rc_.right > top_stash_rc_.left) {
            button(top_stash_rc_, compact ? std::wstring() : GitStr(plain_terms_, Sid::GitStashPlain, Sid::GitStashGit),
                   L"\xE7B8", fluent::ButtonKind::Transparent, !op_busy_ && set_.Total() > 0, HitKind::TopStash, compact);
            const std::wstring stl = GitStr(plain_terms_, Sid::GitStashListPlain, Sid::GitStashListGit) +
                                     (stash_count_ > 0 ? L" (" + std::to_wstring(stash_count_) + L")" : L"");
            button(top_stash_list_rc_, compact ? std::wstring() : stl, L"\xE8F1", fluent::ButtonKind::Transparent, true,
                   HitKind::TopStashList, compact);
        }

        const std::wstring br_t = set_.branch.empty() ? (set_.head.empty() ? L"HEAD" : set_.head) : set_.branch;
        // Button padding 8+8, icon 20+4, chevron 16+4.
        const float br_text_w = top_branch_rc_.right - top_branch_rc_.left - 60.0f * s;
        button(top_branch_rc_, fit_body(br_t, br_text_w), L"\xF003", fluent::ButtonKind::Standard, true,
               HitKind::TopBranch, false, true);

        std::wstring sync_t = Str(Sid::GitSyncLabel);
        if (set_.behind > 0) sync_t += L" \u2193" + std::to_wstring(set_.behind);
        if (set_.ahead > 0) sync_t += L" \u2191" + std::to_wstring(set_.ahead);
        button(top_sync_rc_, icon_only(top_sync_rc_) ? std::wstring() : sync_t, L"\xE895", fluent::ButtonKind::Standard,
               !op_busy_, HitKind::TopSync, icon_only(top_sync_rc_));

        // Repository | view options: the short divider the main toolbar puts between groups.
        const float dx = std::floor((top_sync_rc_.right + top_terms_rc_.left) * 0.5f);
        fill(D2D1::RectF(dx, toolbar_.top + 15.0f * s, dx + line_w, toolbar_.bottom - 15.0f * s), theme.stroke_divider);
        button(top_terms_rc_,
               icon_only(top_terms_rc_) ? std::wstring() : Str(plain_terms_ ? Sid::GitTermsPlain : Sid::GitTermsTechnical),
               L"\xE8C1", fluent::ButtonKind::Transparent, true, HitKind::TopTerms, icon_only(top_terms_rc_));
        button(refresh_rc_, {}, L"\xE72C", fluent::ButtonKind::Transparent, !op_busy_, HitKind::TopRefresh, true);
    }
    // History and conflicts start their own bars under the command bar.
    if (active_tab_ != GitViewTab::Changes)
        fill(D2D1::RectF(0, toolbar_.bottom - line_w, w, toolbar_.bottom), theme.stroke_divider);

    if (active_tab_ == GitViewTab::History) {
        RenderHistory(dc, theme, hc, painter_ok);
    } else if (active_tab_ == GitViewTab::Conflict) {
        RenderConflict(dc, theme, hc, painter_ok);
    } else {
        // ---- Changes tab: Left panel (on the window, like Pulse's sidebar) ----
        if (painter_ok) {
            fluent::ControlState field{};
            field.focused = edit_ && GetFocus() == edit_;
            field.hovered = hover_.kind == HitKind::Field;
            painter_.DrawTextFieldFrame(field_, field, true);

            if (suggest_rc_.right > suggest_rc_.left) {
                text_at(Str(Sid::GitSuggestLabel),
                        D2D1::RectF(field_.left, suggest_rc_.top, suggest_rc_.left - 4.0f * s, suggest_rc_.bottom),
                        small_.get(), theme.text_secondary);
                const bool hot = hover_.kind == HitKind::SuggestChip;
                painter_.FillRoundedRect(suggest_rc_, 12.0f * s,
                                         D2D1::ColorF(theme.accent.r, theme.accent.g, theme.accent.b, hot ? 0.22f : 0.12f));
                text_at(fit(SuggestedMessage(), suggest_rc_.right - suggest_rc_.left - 18.0f * s, small_.get()),
                        D2D1::RectF(suggest_rc_.left + 9.0f * s, suggest_rc_.top, suggest_rc_.right - 9.0f * s, suggest_rc_.bottom),
                        small_.get(), link_ink);
            }

            const int staged = StagedCount();
            std::wstring commit_text;
            if (amend_) {
                commit_text = Str(Sid::GitScAmendCommit);
            } else if (staged > 0) {
                commit_text = GitFmtCount(GitStr(plain_terms_, Sid::GitSaveVersionNPlain, Sid::GitScCommitN), staged);
            } else {
                commit_text = GitStr(plain_terms_, Sid::GitSaveVersionPlain, Sid::GitScCommit);
            }
            button(commit_rc_, commit_text, L"\xE73E", fluent::ButtonKind::Primary, CanCommit(), HitKind::Commit);
            button(commit_push_rc_, {}, L"\xE898", fluent::ButtonKind::Standard, CanCommit(), HitKind::CommitPush, true);

            fluent::ControlState amend{};
            amend.enabled = set_.HasHead() && !op_busy_;
            amend.checked = amend_;
            amend.hovered = hover_.kind == HitKind::Amend;
            painter_.DrawCheckBox(amend_rc_, Str(Sid::GitScAmend), amend);

            std::wstring who_text;
            if (!identity_.name.empty()) {
                who_text = plain_terms_
                    ? GitFmt(Str(Sid::GitWhoSavePlain), L"{name}", identity_.name)
                    : (identity_.name + (identity_.email.empty() ? L"" : L" <" + identity_.email + L">"));
            } else {
                who_text = Str(Sid::GitWhoNoIdentity);
            }
            // The save shortcut as a keycap, like the hints elsewhere in Pulse.
            const std::wstring_view key = L"Ctrl+Enter";
            const float key_w = painter_.MeasureBadgeWidth(key);
            const float key_cy = (who_rc_.top + who_rc_.bottom) * 0.5f;
            fluent::BadgeSpec keycap{};
            keycap.bounds = D2D1::RectF(who_rc_.right - key_w, key_cy - 9.0f * s, who_rc_.right, key_cy + 9.0f * s);
            keycap.text = key;
            keycap.kind = fluent::BadgeKind::Keycap;
            const float who_right = keycap.bounds.left - 8.0f * s;
            text_at(fit(who_text, who_right - who_rc_.left, small_.get()),
                    D2D1::RectF(who_rc_.left, who_rc_.top, who_right, who_rc_.bottom), small_.get(), theme.text_secondary);
            painter_.DrawBadge(keycap);
        }

        fill(D2D1::RectF(field_.left, list_.top - 7.0f * s, field_.right, list_.top - 7.0f * s + line_w),
             theme.stroke_divider);
        dc->PushAxisAlignedClip(list_, D2D1_ANTIALIAS_MODE_ALIASED);
        if (!loaded_) {
            text_at(Str(git_missing_ ? Sid::GitNotFound : Sid::GitScLoading),
                    D2D1::RectF(list_.left + 18.0f * s, list_.top + 10.0f * s, list_.right - 12.0f * s, list_.top + 40.0f * s),
                    ui_.get(), theme.text_secondary);
        } else if (rows_.empty()) {
            if (painter_ok) {
                fluent::EmptyStateSpec empty{};
                empty.bounds = D2D1::RectF(list_.left + 12.0f * s, list_.top + 30.0f * s, list_.right - 12.0f * s,
                                           (std::min)(list_.bottom, list_.top + 230.0f * s));
                empty.glyph = L"\xE73E";
                const std::wstring title = Str(Sid::GitScClean);
                const std::wstring message = Str(Sid::GitScCleanHint);
                empty.title = title;
                empty.message = message;
                painter_.DrawEmptyState(empty);
            }
        } else {
            const float rh = ListRowHeight();
            const int first = (std::max)(0, static_cast<int>(list_scroll_ / rh));
            const int last = (std::min)(static_cast<int>(rows_.size()),
                                        first + static_cast<int>((list_.bottom - list_.top) / rh) + 2);
            for (int i = first; i < last; ++i) {
                const ListRow& r = rows_[static_cast<size_t>(i)];
                const RowGeom g = GeomFor(i);
                if (r.index < 0) {
                    const std::wstring title = r.section == Section::Conflicts
                        ? GitStr(plain_terms_, Sid::GitConflictsPlain, Sid::GitScConflicts)
                        : r.section == Section::Staged
                        ? GitStr(plain_terms_, Sid::GitStagedPlain, Sid::GitScStaged)
                        : GitStr(plain_terms_, Sid::GitUnstagedPlain, Sid::GitScChanges);
                    const float tw = Measure(title, ui_bold_.get());
                    const float tx = g.row.left + 8.0f * s;
                    text_at(title, D2D1::RectF(tx, g.row.top + 2.0f * s, tx + tw + 2.0f, g.row.bottom), ui_bold_.get(),
                            r.section == Section::Conflicts ? theme.danger : theme.text);
                    if (painter_ok) {
                        const std::wstring count = std::to_wstring(List(r.section).size());
                        const float bw = painter_.MeasureBadgeWidth(count);
                        fluent::BadgeSpec badge{};
                        const float by = (g.row.top + g.row.bottom) * 0.5f + 1.0f * s;
                        badge.bounds = D2D1::RectF(tx + tw + 8.0f * s, by - 9.0f * s, tx + tw + 8.0f * s + bw, by + 9.0f * s);
                        badge.text = count;
                        badge.kind = r.section == Section::Conflicts ? fluent::BadgeKind::Danger : fluent::BadgeKind::Neutral;
                        painter_.DrawBadge(badge);
                    }
                    if (g.action.right > g.action.left) {
                        const std::wstring act = r.section == Section::Staged
                            ? GitStr(plain_terms_, Sid::GitUnstageAllPlain, Sid::GitScUnstageAll)
                            : GitStr(plain_terms_, Sid::GitStageAllPlain, Sid::GitScStageAll);
                        fluent::ButtonSpec spec{};
                        spec.bounds = g.action;
                        spec.text = act;
                        spec.kind = fluent::ButtonKind::Transparent;
                        spec.state.enabled = !op_busy_;
                        spec.state.hovered = !op_busy_ && hover_.kind == HitKind::SectionAction && hover_.row == i;
                        spec.state.pressed = spec.state.hovered && pressed_.kind == HitKind::SectionAction;
                        painter_.DrawButton(spec);
                    }
                    continue;
                }

                const GitChange* c = ChangeAt(i);
                if (!c) continue;
                const bool selected = i == sel_row_;
                const bool hovered = hover_.row == i && (hover_.kind == HitKind::Row || hover_.kind == HitKind::RowCheck ||
                                                         hover_.kind == HitKind::RowDiscard || hover_.kind == HitKind::RowReveal);
                if (painter_ok && (selected || hovered)) {
                    // The file list's own row: selection fill with the accent edge, hover wash.
                    fluent::ListRowSpec row_spec{};
                    row_spec.bounds = D2D1::RectF(g.row.left - 4.0f * s, g.row.top + 1.0f * s, g.row.right + 4.0f * s,
                                                  g.row.bottom - 1.0f * s);
                    row_spec.state.selected = selected;
                    row_spec.state.hovered = hovered && !selected;
                    painter_.DrawListRowBackground(row_spec);
                }
                if (painter_ok) {
                    fluent::ControlState check{};
                    check.enabled = !op_busy_;
                    check.checked = r.section == Section::Staged;
                    check.hovered = hover_.kind == HitKind::RowCheck && hover_.row == i;
                    painter_.DrawCheckBox(g.check, {}, check);
                }
                const bool show_actions = hovered && !op_busy_;
                const float text_right = (show_actions ? (r.section == Section::Changes ? g.discard.left : g.reveal.left)
                                                       : g.letter.left) - 6.0f * s;
                const float nx = g.check.right + 10.0f * s;
                const std::wstring name = FileName(c->path);
                std::wstring folder = FolderOf(c->path);
                if (!c->orig_path.empty() && r.section == Section::Staged) folder = c->orig_path + L" \x2192";
                const float name_w = (std::min)(Measure(name, ui_.get()), (std::max)(0.0f, text_right - nx));
                const D2D1_COLOR_F name_color = c->kind == GitChangeKind::Deleted ? theme.text_secondary : theme.text;
                text_at(fit(name, name_w + 1.0f, ui_.get()), D2D1::RectF(nx, g.row.top, nx + name_w + 2.0f, g.row.bottom),
                        ui_.get(), name_color);
                if (c->kind == GitChangeKind::Deleted) {
                    const float my = (g.row.top + g.row.bottom) * 0.5f + 1.0f * s;
                    fill(D2D1::RectF(nx, my, nx + name_w, my + line_w), theme.text_secondary);
                }
                const float fx = nx + name_w + 8.0f * s;
                if (!folder.empty() && text_right - fx > 24.0f * s) {
                    const std::wstring shown =
                        Measure(folder, small_.get()) <= text_right - fx
                            ? folder
                            : FitPathMiddle(folder, text_right - fx, [&](std::wstring_view t) { return Measure(t, small_.get()); });
                    text_at(shown, D2D1::RectF(fx, g.row.top + 1.0f * s, text_right, g.row.bottom), small_.get(),
                            theme.text_secondary);
                }
                if (show_actions) {
                    auto row_icon = [&](const D2D1_RECT_F& rc, std::wstring_view glyph, HitKind id) {
                        fluent::ButtonSpec spec{};
                        spec.bounds = rc;
                        spec.glyph = glyph;
                        spec.kind = fluent::ButtonKind::Transparent;
                        spec.icon_only = true;
                        spec.state.hovered = hover_.kind == id && hover_.row == i;
                        spec.state.pressed = spec.state.hovered && pressed_.kind == id;
                        // A 24-DIP square: the button's 8-DIP padding would leave the icon
                        // 8 DIP, so the plate comes from the button and the icon is 16 DIP.
                        spec.skip_glyph = true;
                        painter_.DrawButton(spec);
                        const float cx = (rc.left + rc.right) * 0.5f, cy = (rc.top + rc.bottom) * 0.5f;
                        painter_.DrawGlyph(glyph, D2D1::RectF(cx - 8.0f * s, cy - 8.0f * s, cx + 8.0f * s, cy + 8.0f * s),
                                           spec.state.pressed ? theme.text_secondary : theme.text);
                    };
                    if (r.section == Section::Changes) row_icon(g.discard, L"\xE7A7", HitKind::RowDiscard);
                    row_icon(g.reveal, L"\xE8B7", HitKind::RowReveal);
                } else {
                    const ui::GitMark mark = app::GitChangeMark(c->kind);
                    const std::wstring lbl = PlainKindText(c->kind, plain_terms_);
                    if (plain_terms_) {
                        fluent::TagSpec tag{};
                        tag.bounds = g.letter;
                        tag.text = lbl;
                        tag.color = GitMarkColor(mark, dark_);
                        tag.bordered = false;
                        painter_.DrawTag(tag);
                    } else {
                        text_at(lbl, g.letter, letter_.get(), GitMarkColor(mark, dark_));
                    }
                }
            }
            if (painter_ok && ListContentHeight() > list_.bottom - list_.top) {
                fluent::ScrollbarSpec bar{list_, list_scroll_, list_.bottom - list_.top, ListContentHeight()};
                bar.expand_progress = (hover_.kind == HitKind::ListBar || drag_bar_ == 1) ? 1.0f : 0.0f;
                painter_.DrawScrollbar(bar);
            }
        }
        dc->PopAxisAlignedClip();

        // ---- Diff header ----
        const GitChange* sel = Selected();
        // The diff card: Pulse's content surface with its hairline edge, as the
        // main window's file list.
        {
            const float radius = theme.radius_control * s;
            painter_.FillRoundedRect(diff_card_, radius, pal.body);
            painter_.StrokeRoundedRect(diff_card_, radius, theme.stroke_card, line_w);
        }
        fill(D2D1::RectF(diff_header_.left, diff_header_.bottom - line_w, diff_header_.right, diff_header_.bottom),
             theme.stroke_divider);
        if (sel) {
            const float x0 = diff_header_.left + 16.0f * s;
            float buttons_left = diff_header_.right - 10.0f * s;
            for (const auto& r : {mode_inline_rc_, blame_rc_, stage_btn_, discard_btn_}) {
                if (r.right > r.left) buttons_left = (std::min)(buttons_left, r.left);
            }
            std::wstring counts;
            if (!diff_loading_ && !diff_is_image_ && left_ && right_ &&
                left_->status == diff::LoadStatus::Ok && right_->status == diff::LoadStatus::Ok) {
                int added = 0, deleted = 0;
                for (const URow& r : urows_) {
                    if (r.kind == URowKind::Add) ++added;
                    else if (r.kind == URowKind::Del) ++deleted;
                }
                counts = L"+" + std::to_wstring(added) + L"  \x2212" + std::to_wstring(deleted);
            }
            const float counts_w = counts.empty() ? 0.0f : Measure(counts, ui_bold_.get()) + 16.0f * s;
            const float name_max = (std::max)(0.0f, buttons_left - x0 - counts_w - 16.0f * s);
            const std::wstring name = FileName(sel->path);
            const float name_w = (std::min)(Measure(name, ui_bold_.get()), name_max);
            text_at(fit(name, name_w + 1.0f, ui_bold_.get()),
                    D2D1::RectF(x0, diff_header_.top + 6.0f * s, x0 + name_w + 2.0f, diff_header_.top + 27.0f * s),
                    ui_bold_.get(), theme.text);
            if (!counts.empty()) {
                const std::wstring plus = counts.substr(0, counts.find(L' '));
                const float pw = Measure(plus + L"  ", ui_bold_.get());
                const float cx = x0 + name_w + 12.0f * s;
                text_at(plus, D2D1::RectF(cx, diff_header_.top + 6.0f * s, cx + pw, diff_header_.top + 27.0f * s),
                        ui_bold_.get(), pal.add_fg);
                const std::wstring minus = counts.substr(counts.find(L'\x2212'));
                text_at(minus, D2D1::RectF(cx + pw, diff_header_.top + 6.0f * s, cx + counts_w, diff_header_.top + 27.0f * s),
                        ui_bold_.get(), pal.del_fg);
            }
            Sid side_id = Sid::GitScIndexWorktree;
            if (sel_section_ == Section::Staged) side_id = Sid::GitScHeadIndex;
            else if (sel_section_ == Section::Conflicts) side_id = Sid::GitScHeadWorktree;
            else if (sel->kind == GitChangeKind::Untracked) side_id = Sid::GitScUntracked;
            std::wstring sub = Str(side_id);
            const std::wstring folder = FolderOf(sel->path);
            if (!sel->orig_path.empty()) sub = sel->orig_path + L" \x2192 " + sel->path + L"  \x00B7  " + sub;
            else if (!folder.empty()) sub = folder + L"  \x00B7  " + sub;
            const float sub_w = (std::max)(0.0f, buttons_left - x0 - 12.0f * s);
            text_at(fit(sub, sub_w, small_.get()),
                    D2D1::RectF(x0, diff_header_.top + 27.0f * s, x0 + sub_w, diff_header_.top + 45.0f * s),
                    small_.get(), theme.text_secondary);

            if (mode_inline_rc_.right > mode_inline_rc_.left && painter_ok) {
                const D2D1_RECT_F track = D2D1::RectF(mode_inline_rc_.left - 2.0f * s, mode_inline_rc_.top - 2.0f * s,
                                                      mode_sbs_rc_.right + 2.0f * s, mode_sbs_rc_.bottom + 2.0f * s);
                painter_.DrawSegmentedTrack(track);
                auto seg_item = [&](const D2D1_RECT_F& rc, const std::wstring& txt, bool sel_on, HitKind id,
                                    fluent::SegmentPosition pos) {
                    fluent::SegmentedItemSpec sp{};
                    sp.bounds = rc;
                    sp.text = txt;
                    sp.position = pos;
                    sp.state.enabled = true;
                    sp.state.selected = sel_on;
                    sp.state.hovered = hover_.kind == id;
                    painter_.DrawTrackSegment(sp);
                };
                seg_item(mode_inline_rc_, GitStr(plain_terms_, Sid::GitInlinePlain, Sid::GitInlineGit),
                         !sbs_mode_, HitKind::DiffModeInline, fluent::SegmentPosition::First);
                seg_item(mode_sbs_rc_, GitStr(plain_terms_, Sid::GitSbsPlain, Sid::GitSbsGit),
                         sbs_mode_, HitKind::DiffModeSbs, fluent::SegmentPosition::Last);
            }
            if (blame_rc_.right > blame_rc_.left && painter_ok) {
                fluent::ControlState sw{};
                sw.enabled = true;
                sw.checked = blame_mode_;
                sw.hovered = hover_.kind == HitKind::DiffBlameToggle;
                painter_.DrawSwitch(blame_rc_, GitStr(plain_terms_, Sid::GitBlamePlain, Sid::GitBlameGit), sw);
            }
            const Sid stage_id = sel_section_ == Section::Staged
                ? (plain_terms_ ? Sid::GitStagedOnePlain : Sid::GitScUnstage)
                : sel_section_ == Section::Conflicts ? Sid::GitScResolve
                : (plain_terms_ ? Sid::GitStageOnePlain : Sid::GitScStage);
            const wchar_t* stage_glyph = sel_section_ == Section::Staged ? L"\xE738"
                                         : sel_section_ == Section::Conflicts ? L"\xE73E" : L"\xE710";
            const bool stage_icon = stage_btn_.right - stage_btn_.left <= 33.0f * s;
            button(stage_btn_, stage_icon ? std::wstring() : Str(stage_id), stage_glyph, fluent::ButtonKind::Standard,
                   !op_busy_, HitKind::DiffStage, stage_icon);
            if (discard_btn_.right > discard_btn_.left) {
                const Sid disc_id = sel->kind == GitChangeKind::Untracked
                    ? (plain_terms_ ? Sid::GitDeleteUntrackedPlain : Sid::GitDeleteUntrackedGit)
                    : sel->kind == GitChangeKind::Deleted
                    ? (plain_terms_ ? Sid::GitRestoreDeletedPlain : Sid::GitRestoreDeletedGit)
                    : (plain_terms_ ? Sid::GitDiscardPlain : Sid::GitScDiscard);
                // A standard button: what it throws away is confirmed in a dialog first.
                const bool disc_icon = discard_btn_.right - discard_btn_.left <= 33.0f * s;
                button(discard_btn_, disc_icon ? std::wstring() : Str(disc_id), L"\xE7A7", fluent::ButtonKind::Standard,
                       !op_busy_, HitKind::DiffDiscard, disc_icon);
            }
        }

        // ---- Diff body ----
        hunk_btn_rects_.clear();
        auto centered = [&](const std::wstring& message, const D2D1_COLOR_F& color) {
            const float mw = Measure(message, ui_.get());
            const float cx = (diff_body_.left + diff_body_.right) * 0.5f;
            const float cy = (diff_body_.top + diff_body_.bottom) * 0.5f;
            text_at(message, D2D1::RectF(cx - mw * 0.5f - 10.0f, cy - 16.0f * s, cx + mw * 0.5f + 10.0f, cy + 16.0f * s),
                    ui_.get(), color);
        };
        const float row_h = DiffRowHeight();
        const float num_w = static_cast<float>(number_digits_) * char_w_ + 12.0f * s;

        if (!sel) {
            if (loaded_ && !rows_.empty()) centered(Str(Sid::GitScSelectFile), theme.text_secondary);
        } else if (sel->kind == GitChangeKind::Deleted) {
            centered(Str(Sid::GitImgDeletedPlain), theme.text_secondary);
            if (restore_deleted_btn_.right > restore_deleted_btn_.left) {
                button(restore_deleted_btn_,
                       GitStr(plain_terms_, Sid::GitRestoreDeletedPlain, Sid::GitRestoreDeletedGit),
                       L"\xE7A7", fluent::ButtonKind::Primary, !op_busy_, HitKind::DiffRestoreDeleted);
            }
        } else if (diff_loading_ && urows_.empty() && !has_new_img_ && !has_old_img_) {
            centered(Str(Sid::DiffComputing), theme.text_secondary);
        } else if (diff_is_image_ && (has_old_img_ || has_new_img_)) {
            DrawGitImageCompare(dc, painter_, small_.get(), brush_.get(), theme, s, diff_body_,
                                old_img_bmp_.get(), has_old_img_ ? &old_img_meta_ : nullptr,
                                new_img_bmp_.get(), has_new_img_ ? &new_img_meta_ : nullptr,
                                img_split_, plain_terms_, dragging_img_split_);
        } else if (left_ && right_ && (left_->status != diff::LoadStatus::Ok || right_->status != diff::LoadStatus::Ok)) {
            const diff::DiffSide* bad = left_->status != diff::LoadStatus::Ok ? left_.get() : right_.get();
            const Sid id = bad->status == diff::LoadStatus::Binary ? Sid::DiffBinary
                         : bad->status == diff::LoadStatus::TooLarge ? Sid::DiffTooLarge
                         : Sid::DiffReadFailed;
            centered(GitFmt(Str(id), L"{name}", FileName(sel->path)), theme.text_secondary);
        } else if (urows_.empty() && left_ && right_) {
            centered(Str(left_->lines.empty() && right_->lines.empty() ? Sid::GitScEmptyFile : Sid::GitScNoTextChange),
                     theme.text_secondary);
        } else if (!urows_.empty()) {
            dc->PushAxisAlignedClip(diff_body_, D2D1_ANTIALIAS_MODE_ALIASED);
            auto draw_text_line = [&](const std::wstring& line, float left_x, float right_x, float y,
                                      const std::vector<diff::CharSpan>* spans, const D2D1_COLOR_F& hi) {
                if (!factory || !mono_.get() || right_x <= left_x) return;
                const UINT32 len = static_cast<UINT32>((std::min)(line.size(), kMaxDrawChars));
                if (len == 0) return;
                dc->PushAxisAlignedClip(D2D1::RectF(left_x - 2.0f * s, y, right_x, y + row_h),
                                        D2D1_ANTIALIAS_MODE_ALIASED);
                ComPtr<IDWriteTextLayout> layout;
                if (SUCCEEDED(factory->CreateTextLayout(line.data(), len, mono_.get(), 1.0e6f, row_h, &layout)) &&
                    layout.get()) {
                    const float ox = left_x - diff_scroll_x_;
                    if (spans) {
                        for (const auto& span : *spans) {
                            const int b = (std::min)(span.begin, static_cast<int>(len));
                            const int e = (std::min)(span.end, static_cast<int>(len));
                            if (e <= b) continue;
                            UINT32 hits = 0;
                            layout->HitTestTextRange(static_cast<UINT32>(b), static_cast<UINT32>(e - b), ox, y,
                                                     nullptr, 0, &hits);
                            if (hits == 0) continue;
                            std::vector<DWRITE_HIT_TEST_METRICS> metrics(hits);
                            if (FAILED(layout->HitTestTextRange(static_cast<UINT32>(b), static_cast<UINT32>(e - b), ox, y,
                                                                metrics.data(), hits, &hits)))
                                continue;
                            brush_->SetColor(hi);
                            for (UINT32 m = 0; m < hits; ++m) {
                                const auto& hm = metrics[m];
                                const D2D1_RECT_F rc = D2D1::RectF(hm.left, y + 2.0f * s,
                                                                   hm.left + (std::max)(hm.width, 2.0f * s), y + row_h - 2.0f * s);
                                dc->FillRoundedRectangle(D2D1::RoundedRect(rc, 2.0f * s, 2.0f * s), brush_.get());
                            }
                        }
                    }
                    brush_->SetColor(theme.text);
                    dc->DrawTextLayout(D2D1::Point2F(ox, y), layout.get(), brush_.get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                }
                dc->PopAxisAlignedClip();
            };

            auto draw_hunk_bar = [&](const D2D1_RECT_F& row_rc, const std::wstring& hdr_text, int hunk_idx) {
                fill(row_rc, pal.hunk_bg);
                float rx = row_rc.right - 16.0f * s;
                if (hunk_idx >= 0 && hunk_idx < static_cast<int>(hunk_patches_.size()) &&
                    sel_section_ != Section::Conflicts) {
                    HunkBtnRects hb{};
                    hb.hunk_index = hunk_idx;
                    const std::wstring st_t = sel_section_ == Section::Staged
                        ? GitStr(plain_terms_, Sid::GitHunkUnstagePlain, Sid::GitHunkUnstageGit)
                        : GitStr(plain_terms_, Sid::GitHunkStagePlain, Sid::GitHunkStageGit);
                    const float st_w = Measure(st_t, small_.get()) + 16.0f * s;
                    hb.stage_rc = D2D1::RectF(rx - st_w, row_rc.top + 1.0f * s, rx, row_rc.bottom - 1.0f * s);
                    rx = hb.stage_rc.left - 6.0f * s;
                    const bool hot_s = hover_.kind == HitKind::HunkStage && hover_.sub == hunk_idx;
                    if (hot_s) painter_.FillRoundedRect(hb.stage_rc, 4.0f * s, theme.fill_hover);
                    text_at(st_t, D2D1::RectF(hb.stage_rc.left + 8.0f * s, hb.stage_rc.top, hb.stage_rc.right, hb.stage_rc.bottom),
                            small_.get(), link_ink);

                    if (sel_section_ == Section::Changes) {
                        const std::wstring d_t = GitStr(plain_terms_, Sid::GitHunkDiscardPlain, Sid::GitHunkDiscardGit);
                        const float d_w = Measure(d_t, small_.get()) + 16.0f * s;
                        hb.discard_rc = D2D1::RectF(rx - d_w, row_rc.top + 1.0f * s, rx, row_rc.bottom - 1.0f * s);
                        rx = hb.discard_rc.left - 6.0f * s;
                        const bool hot_d = hover_.kind == HitKind::HunkDiscard && hover_.sub == hunk_idx;
                        if (hot_d) painter_.FillRoundedRect(hb.discard_rc, 4.0f * s, theme.fill_hover);
                        text_at(d_t, D2D1::RectF(hb.discard_rc.left + 8.0f * s, hb.discard_rc.top, hb.discard_rc.right, hb.discard_rc.bottom),
                                small_.get(), pal.del_fg);
                    }
                    hunk_btn_rects_.push_back(hb);
                }
                text_at(hdr_text, D2D1::RectF(row_rc.left + 12.0f * s, row_rc.top, rx - 8.0f * s, row_rc.bottom),
                        small_.get(), theme.text_secondary);
            };

            if (!sbs_mode_) {
                const int count = static_cast<int>(urows_.size());
                const int first = (std::max)(0, static_cast<int>(diff_scroll_ / row_h));
                const int last = (std::min)(count, first + static_cast<int>((diff_body_.bottom - diff_body_.top) / row_h) + 2);
                const float blame_w = (blame_mode_ && !blame_lines_.empty()) ? 132.0f * s : 0.0f;
                const float x_old = diff_body_.left + blame_w;
                const float x_new = x_old + num_w;
                const float sign_x = x_new + num_w + 4.0f * s;
                const float text_left = sign_x + 16.0f * s;

                auto number = [&](int line, float x0, float y) {
                    if (line < 0) return;
                    text_at(std::to_wstring(line + 1), D2D1::RectF(x0, y, x0 + num_w - 8.0f * s, y + row_h),
                            gutter_.get(), theme.text_secondary);
                };

                std::wstring prev_blame_hash;
                for (int i = first; i < last; ++i) {
                    const URow& r = urows_[static_cast<size_t>(i)];
                    const float y = diff_body_.top + static_cast<float>(i) * row_h - diff_scroll_;
                    const D2D1_RECT_F row_rc = D2D1::RectF(diff_body_.left, y, diff_body_.right, y + row_h);
                    if (r.kind == URowKind::Hunk) {
                        prev_blame_hash.clear();
                        draw_hunk_bar(row_rc, r.text, r.hunk_index);
                        continue;
                    }
                    if (r.kind == URowKind::Del || r.kind == URowKind::Add) {
                        fill(row_rc, r.kind == URowKind::Del ? pal.del_bg : pal.add_bg);
                    }
                    if (blame_w > 0.0f) {
                        if (r.right >= 0 && static_cast<size_t>(r.right) < blame_lines_.size()) {
                            const auto& bl = blame_lines_[static_cast<size_t>(r.right)];
                            if (bl.hash != prev_blame_hash) {
                                prev_blame_hash = bl.hash;
                                const std::wstring info = bl.uncommitted
                                    ? Str(Sid::GitBlameUncommitted)
                                    : (bl.author + L" \u00B7 " + bl.date_text);
                                text_at(fit(info, blame_w - 12.0f * s, small_.get()),
                                        D2D1::RectF(diff_body_.left + 8.0f * s, y, x_old - 4.0f * s, y + row_h),
                                        small_.get(), bl.uncommitted ? pal.warn_fg : theme.text_secondary);
                            }
                        } else {
                            prev_blame_hash.clear();
                        }
                    }
                    if (r.kind == URowKind::Same) {
                        number(r.left, x_old, y);
                        number(r.right, x_new, y);
                        draw_text_line(left_->lines[static_cast<size_t>(r.left)], text_left,
                                       diff_body_.right - 12.0f * s, y, nullptr, pal.add_hi);
                    } else {
                        const bool del = r.kind == URowKind::Del;
                        number(del ? r.left : -1, x_old, y);
                        number(del ? -1 : r.right, x_new, y);
                        text_at(del ? L"\x2212" : L"+", D2D1::RectF(sign_x, y, sign_x + 14.0f * s, y + row_h),
                                mono_.get(), del ? pal.del_fg : pal.add_fg);
                        const std::vector<diff::CharSpan>* spans = nullptr;
                        if (r.diff_row >= 0 && r.diff_row < static_cast<int>(result_.rows.size()) &&
                            result_.rows[static_cast<size_t>(r.diff_row)].kind == diff::RowKind::Mod) {
                            const CharSpans& cs = SpansFor(r.diff_row);
                            spans = del ? &cs.left : &cs.right;
                        }
                        const std::wstring& line = del ? left_->lines[static_cast<size_t>(r.left)]
                                                       : right_->lines[static_cast<size_t>(r.right)];
                        draw_text_line(line, text_left, diff_body_.right - 12.0f * s, y, spans,
                                       del ? pal.del_hi : pal.add_hi);
                    }
                }
                if (blame_w > 0.0f) {
                    fill(D2D1::RectF(x_old - line_w, diff_body_.top, x_old, diff_body_.bottom), theme.stroke_divider);
                }
                fill(D2D1::RectF(x_new + num_w - line_w, diff_body_.top, x_new + num_w, diff_body_.bottom),
                     theme.stroke_divider);
            } else {
                // Side-by-side mode
                const int count = static_cast<int>(sbs_rows_.size());
                const int first = (std::max)(0, static_cast<int>(diff_scroll_ / row_h));
                const int last = (std::min)(count, first + static_cast<int>((diff_body_.bottom - diff_body_.top) / row_h) + 2);
                const float mid_x = std::floor((diff_body_.left + diff_body_.right) * 0.5f);
                const float l_num_x = diff_body_.left;
                const float l_sign_x = l_num_x + num_w + 2.0f * s;
                const float l_txt_x = l_sign_x + 14.0f * s;
                const float r_num_x = mid_x + line_w;
                const float r_sign_x = r_num_x + num_w + 2.0f * s;
                const float r_txt_x = r_sign_x + 14.0f * s;

                for (int i = first; i < last; ++i) {
                    const SbsRow& sr = sbs_rows_[static_cast<size_t>(i)];
                    const float y = diff_body_.top + static_cast<float>(i) * row_h - diff_scroll_;
                    const D2D1_RECT_F full_rc = D2D1::RectF(diff_body_.left, y, diff_body_.right, y + row_h);
                    if (sr.is_hunk) {
                        draw_hunk_bar(full_rc, sr.header, sr.hunk_index);
                        continue;
                    }
                    const D2D1_RECT_F l_rc = D2D1::RectF(diff_body_.left, y, mid_x, y + row_h);
                    const D2D1_RECT_F r_rc = D2D1::RectF(mid_x + line_w, y, diff_body_.right, y + row_h);
                    const std::vector<diff::CharSpan>* lsp = nullptr;
                    const std::vector<diff::CharSpan>* rsp = nullptr;
                    if (sr.kind == diff::RowKind::Mod && sr.diff_row >= 0) {
                        const CharSpans& cs = SpansFor(sr.diff_row);
                        lsp = &cs.left;
                        rsp = &cs.right;
                    }

                    if (sr.left >= 0) {
                        if (sr.kind != diff::RowKind::Same) fill(l_rc, pal.del_bg);
                        text_at(std::to_wstring(sr.left + 1), D2D1::RectF(l_num_x, y, l_num_x + num_w - 6.0f * s, y + row_h),
                                gutter_.get(), theme.text_secondary);
                        if (sr.kind != diff::RowKind::Same) {
                            text_at(L"\x2212", D2D1::RectF(l_sign_x, y, l_sign_x + 12.0f * s, y + row_h), mono_.get(), pal.del_fg);
                        }
                        draw_text_line(left_->lines[static_cast<size_t>(sr.left)], l_txt_x, mid_x - 4.0f * s, y, lsp, pal.del_hi);
                    } else if (hatch_.get()) {
                        dc->FillRectangle(l_rc, hatch_.get());
                    }

                    if (sr.right >= 0) {
                        if (sr.kind != diff::RowKind::Same) fill(r_rc, pal.add_bg);
                        text_at(std::to_wstring(sr.right + 1), D2D1::RectF(r_num_x, y, r_num_x + num_w - 6.0f * s, y + row_h),
                                gutter_.get(), theme.text_secondary);
                        if (sr.kind != diff::RowKind::Same) {
                            text_at(L"+", D2D1::RectF(r_sign_x, y, r_sign_x + 12.0f * s, y + row_h), mono_.get(), pal.add_fg);
                        }
                        draw_text_line(right_->lines[static_cast<size_t>(sr.right)], r_txt_x, diff_body_.right - 12.0f * s,
                                       y, rsp, pal.add_hi);
                    } else if (hatch_.get()) {
                        dc->FillRectangle(r_rc, hatch_.get());
                    }
                }
                fill(D2D1::RectF(mid_x, diff_body_.top, mid_x + line_w, diff_body_.bottom), theme.stroke_divider);
            }

            if (painter_ok && DiffContentHeight() > diff_body_.bottom - diff_body_.top) {
                fluent::ScrollbarSpec bar{diff_body_, diff_scroll_, diff_body_.bottom - diff_body_.top, DiffContentHeight()};
                bar.expand_progress = (hover_.kind == HitKind::DiffBar || drag_bar_ == 2) ? 1.0f : 0.0f;
                painter_.DrawScrollbar(bar);
            }
            dc->PopAxisAlignedClip();
        }

    }

    // ---- Status bar (on the window, as in the main window) ----
    if (active_tab_ != GitViewTab::Changes)
        fill(D2D1::RectF(0, status_.top, w, status_.top + line_w), theme.stroke_divider);
    {
        std::wstring text;
        D2D1_COLOR_F color = theme.text_secondary;
        if (op_busy_) {
            text = Str(Sid::GitScRunning);
        } else if (!notice_.empty()) {
            text = notice_;
            if (notice_error_) color = theme.danger;
        } else if (loaded_) {
            text = GitFmt(GitFmt(Str(Sid::GitScSummary), L"{n}", std::to_wstring(set_.Total())),
                          L"{s}", std::to_wstring(set_.staged.size()));
        }
        for (wchar_t& c : text)
            if (c == L'\n' || c == L'\r') c = L' ';
        float right = undo_rc_.right > undo_rc_.left ? undo_rc_.left - 10.0f * s : w - 12.0f * s;
        if (active_tab_ == GitViewTab::Changes && undo_rc_.right <= undo_rc_.left) {
            // Changes: the file-switch shortcut on the right, and what 撤销修改
            // does after the summary while there is room for all of it.
            const std::wstring fkey = Str(Sid::GitSwitchShortcutHint);
            const float kw = Measure(fkey, small_.get());
            text_at(fkey, D2D1::RectF(right - kw - 2.0f, status_.top, right, status_.bottom), small_.get(),
                    theme.text_secondary);
            right -= kw + 24.0f * s;
            const std::wstring ftip = GitStr(plain_terms_, Sid::GitFootTipPlain, Sid::GitFootTipGit);
            const float sum_w = Measure(text, small_.get());
            const float tip_w = Measure(ftip, small_.get());
            const float tip_x = 14.0f * s + sum_w + 24.0f * s;
            if (!op_busy_ && notice_.empty() && tip_x + tip_w <= right) {
                text_at(ftip, D2D1::RectF(tip_x, status_.top, tip_x + tip_w + 2.0f, status_.bottom), small_.get(),
                        theme.text_disabled);
            }
        }
        text_at(fit(text, right - 14.0f * s, small_.get()),
                D2D1::RectF(14.0f * s, status_.top, right, status_.bottom), small_.get(), color);
        if (undo_rc_.right > undo_rc_.left) {
            const bool hot = hover_.kind == HitKind::Undo;
            if (hot) painter_.FillRoundedRect(undo_rc_, 4.0f * s, theme.fill_hover);
            text_at(GitStr(plain_terms_, Sid::GitUndoSavePlain, Sid::GitScUndoCommit),
                    D2D1::RectF(undo_rc_.left + 8.0f * s, undo_rc_.top, undo_rc_.right, undo_rc_.bottom),
                    small_.get(), link_ink);
        }
    }

    const HRESULT hr = dc->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        compositor_.NotifyDeviceLost(hr);
    else
        compositor_.Present();
}

// ---------------------------------------------------------------------------
// Messages & Edit subclasses

LRESULT CALLBACK GitWindow::EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<GitWindow*>(ref);
    if (!self) return DefSubclassProc(hwnd, msg, wparam, lparam);
    const D2D1_COLOR_F fore = ColorFromRef(EditTextColor(self->dark_));
    const D2D1_COLOR_F back = ColorFromRef(EditBackColor(self->dark_));
    IDWriteTextFormat* format = self->compositor_.TextFormat();
    switch (msg) {
    case WM_GETDLGCODE:
        return DefSubclassProc(hwnd, msg, wparam, lparam) | DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (wparam == VK_RETURN && GetKeyState(VK_CONTROL) < 0) {
            self->Commit(false);
            return 0;
        }
        if (wparam == L'B' && GetKeyState(VK_CONTROL) < 0) {
            POINT pt{static_cast<LONG>(self->top_branch_rc_.left),
                     static_cast<LONG>(self->top_branch_rc_.bottom + 4.0f * self->scale_)};
            ClientToScreen(self->hwnd_, &pt);
            self->TriggerBranchPopup(pt, false);
            return 0;
        }
        if (wparam == VK_ESCAPE) {
            self->Close();
            return 0;
        }
        if (wparam == VK_TAB) {
            SetFocus(self->hwnd_);
            return 0;
        }
        if (wparam == VK_F5) {
            self->StartOp(OpRequest{});
            return 0;
        }
        break;
    case WM_SYSKEYDOWN:
        if (wparam == VK_UP) {
            self->StepSelection(-1);
            return 0;
        }
        if (wparam == VK_DOWN) {
            self->StepSelection(1);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wparam == L'\n' || wparam == 27 || wparam == L'\t') return 0;
        if (wparam == 1) {
            SendMessageW(hwnd, EM_SETSEL, 0, -1);
            return 0;
        }
        break;
    case WM_NCDESTROY: {
        LRESULT ignored = 0;
        HandleChildEditMessage(self->compositor_, format, fore, back, EditBackBrush(self->edit_brush_),
                               hwnd, msg, wparam, lparam, ignored);
        RemovePropW(hwnd, L"Pulse.EditMultiline");
        RemovePropW(hwnd, L"Pulse.EditCue");
        RemovePropW(hwnd, L"Pulse.EditCueColor");
        RemoveWindowSubclass(hwnd, EditProc, kEditSubclass);
        return DefSubclassProc(hwnd, msg, wparam, lparam);
    }
    default: break;
    }
    const bool focus_ring = msg == WM_SETFOCUS || msg == WM_KILLFOCUS;
    LRESULT result = 0;
    if (!HandleChildEditMessage(self->compositor_, format, fore, back, EditBackBrush(self->edit_brush_),
                                hwnd, msg, wparam, lparam, result))
        result = DefPresentedChildEditProc(self->compositor_, format, fore, back, hwnd, msg, wparam, lparam);
    if (focus_ring) InvalidateRect(self->hwnd_, nullptr, FALSE);
    return result;
}

LRESULT CALLBACK GitWindow::SubEditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                        UINT_PTR sub_id, DWORD_PTR ref) {
    auto* self = reinterpret_cast<GitWindow*>(ref);
    if (!self) return DefSubclassProc(hwnd, msg, wparam, lparam);
    const D2D1_COLOR_F fore = ColorFromRef(EditTextColor(self->dark_));
    const D2D1_COLOR_F back = ColorFromRef(EditBackColor(self->dark_));
    IDWriteTextFormat* format = self->compositor_.TextFormat();
    switch (msg) {
    case WM_GETDLGCODE:
        return DefSubclassProc(hwnd, msg, wparam, lparam) | DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            if (sub_id == kSubEditHistBranch) {
                self->hist_creating_branch_ = false;
                self->SyncChildEditsVisibility();
                InvalidateRect(self->hwnd_, nullptr, FALSE);
                return 0;
            }
            if (sub_id == kSubEditCfCustom) {
                self->cf_editing_hunk_ = -1;
                self->SyncChildEditsVisibility();
                InvalidateRect(self->hwnd_, nullptr, FALSE);
                return 0;
            }
            SetFocus(self->hwnd_);
            return 0;
        }
        if (wparam == VK_RETURN) {
            if (sub_id == kSubEditHistSearch) {
                self->StartHistoryLoad();
                return 0;
            }
            if (sub_id == kSubEditHistBranch) {
                const std::wstring name = self->HistBranchInputText();
                if (!name.empty() && self->hist_sel_ >= 0 &&
                    self->hist_sel_ < static_cast<int>(self->hist_commits_.size())) {
                    OpRequest req;
                    req.kind = OpKind::CreateBranch;
                    req.arg1 = name;
                    req.arg2 = self->hist_commits_[static_cast<size_t>(self->hist_sel_)].full_hash;
                    self->hist_creating_branch_ = false;
                    self->SyncChildEditsVisibility();
                    self->StartOp(std::move(req));
                }
                return 0;
            }
            if (sub_id == kSubEditCfCustom && GetKeyState(VK_CONTROL) < 0) {
                self->ApplyCustomEditBuffer();
                return 0;
            }
        }
        break;
    case WM_CHAR:
        if (wparam == 1) {
            SendMessageW(hwnd, EM_SETSEL, 0, -1);
            return 0;
        }
        if (wparam == 27 || (sub_id != kSubEditCfCustom && (wparam == L'\r' || wparam == L'\n'))) return 0;
        break;
    case WM_NCDESTROY: {
        LRESULT ignored = 0;
        HandleChildEditMessage(self->compositor_, format, fore, back, EditBackBrush(self->edit_brush_),
                               hwnd, msg, wparam, lparam, ignored);
        RemovePropW(hwnd, L"Pulse.EditMultiline");
        RemovePropW(hwnd, L"Pulse.EditCue");
        RemovePropW(hwnd, L"Pulse.EditCueColor");
        RemoveWindowSubclass(hwnd, SubEditProc, sub_id);
        return DefSubclassProc(hwnd, msg, wparam, lparam);
    }
    default: break;
    }
    LRESULT result = 0;
    if (!HandleChildEditMessage(self->compositor_, format, fore, back, EditBackBrush(self->edit_brush_),
                                hwnd, msg, wparam, lparam, result))
        result = DefPresentedChildEditProc(self->compositor_, format, fore, back, hwnd, msg, wparam, lparam);
    return result;
}

LRESULT CALLBACK GitWindow::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    GitWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<GitWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<GitWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT GitWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
        if (!compositor_.Init(hwnd_)) return -1;
        compositor_.RecreateTextFormats(scale_);
        painter_.SetCompositor(&compositor_);
        painter_.SetScale(scale_);
        RecreateFormats();
        edit_brush_ = CreateSolidBrush(EditBackColor(dark_));
        CreateEdits();
        return 0;
    case WM_SIZE:
        Layout();
        LayoutEdits();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_NCCALCSIZE:
        if (wparam && IsZoomed(hwnd_)) {
            // No invisible maximized border to inset: fill the monitor's work area.
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromRect(&params->rgrc[0], MONITOR_DEFAULTTONEAREST), &monitor))
                params->rgrc[0] = monitor.rcWork;
        }
        return 0;
    case WM_NCACTIVATE:
        // -1: update the activation state without painting a classic caption.
        return DefWindowProcW(hwnd_, message, wparam, -1);
    case WM_NCHITTEST:
        return NcHitTest(lparam);
    case WM_NCMOUSEMOVE: {
        const Caption next = wparam == HTCLOSE ? Caption::Close : wparam == HTMAXBUTTON ? Caption::Maximize
                           : wparam == HTMINBUTTON ? Caption::Minimize : Caption::None;
        if (next != caption_hover_ || hover_.kind != HitKind::None) {
            caption_hover_ = next;
            hover_ = {};
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (!tracking_nc_leave_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd_, 0};
            tracking_nc_leave_ = TrackMouseEvent(&tme) != FALSE;
        }
        break;
    }
    case WM_NCMOUSELEAVE:
        tracking_nc_leave_ = false;
        if (caption_hover_ != Caption::None || caption_pressed_ != Caption::None) {
            caption_hover_ = caption_pressed_ = Caption::None;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (wparam == HTCLOSE || wparam == HTMAXBUTTON || wparam == HTMINBUTTON) {
            // Ours to draw and run; the default would track a classic button.
            caption_pressed_ = wparam == HTCLOSE ? Caption::Close : wparam == HTMAXBUTTON ? Caption::Maximize
                             : Caption::Minimize;
            caption_hover_ = caption_pressed_;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_NCLBUTTONUP:
        if (caption_pressed_ != Caption::None) {
            const Caption up = wparam == HTCLOSE ? Caption::Close : wparam == HTMAXBUTTON ? Caption::Maximize
                             : wparam == HTMINBUTTON ? Caption::Minimize : Caption::None;
            const Caption pressed = caption_pressed_;
            caption_pressed_ = Caption::None;
            InvalidateRect(hwnd_, nullptr, FALSE);
            if (up == pressed) DoCaption(pressed);
            return 0;
        }
        break;
    case WM_GETMINMAXINFO: {
        // Below this the 280-DIP list leaves the diff no room for its controls.
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = static_cast<LONG>(std::lround(600.0f * scale_));
        info->ptMinTrackSize.y = static_cast<LONG>(std::lround(420.0f * scale_));
        return 0;
    }
    case WM_DPICHANGED: {
        scale_ = static_cast<float>(HIWORD(wparam)) / 96.0f;
        compositor_.RecreateTextFormats(scale_);
        painter_.SetScale(scale_);
        RecreateFormats();
        hatch_.reset();
        if (edit_font_) {
            DeleteObject(edit_font_);
            edit_font_ = nullptr;
        }
        const RECT* r = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        LayoutEdits();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wparam) != WA_INACTIVE && loaded_) {
            const ULONGLONG now = GetTickCount64();
            if (!op_busy_ && now - last_refresh_ > 1500) StartOp(OpRequest{});
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd_, &paint);
        Render();
        EndPaint(hwnd_, &paint);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        const HDC hdc = reinterpret_cast<HDC>(wparam);
        SetTextColor(hdc, EditTextColor(dark_));
        SetBkColor(hdc, EditBackColor(dark_));
        return reinterpret_cast<LRESULT>(EditBackBrush(edit_brush_));
    }
    case WM_COMMAND:
        if (HIWORD(wparam) == EN_CHANGE) {
            const HWND src = reinterpret_cast<HWND>(lparam);
            if (src == hist_search_edit_) {
                StartHistoryLoad();
            } else if (src == edit_) {
                if (!notice_.empty() && notice_error_) notice_.clear();
                Layout();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return 0;
    case kMsgOpDone:
        AdoptOp();
        return 0;
    case kMsgDiffDone:
        AdoptDiff();
        return 0;
    case kMsgHistDone:
        AdoptHistoryLoad();
        return 0;
    case kMsgHistDetailDone:
        AdoptHistoryDetailLoad();
        return 0;
    case kMsgHistDiffDone:
        AdoptHistoryFileDiff();
        return 0;
    case kMsgCfStateDone:
        AdoptConflictStateLoad();
        return 0;
    case kMsgCfFileDone:
        AdoptConflictFileLoad();
        return 0;
    case WM_MOUSEMOVE: {
        const float x = static_cast<float>(GET_X_LPARAM(lparam));
        const float y = static_cast<float>(GET_Y_LPARAM(lparam));
        if (caption_hover_ != Caption::None) {
            caption_hover_ = Caption::None;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (dragging_img_split_) {
            const DecodedGitImage* ref = new_img_meta_.ok() ? &new_img_meta_ : &old_img_meta_;
            const auto lay = LayoutGitImageCompare(diff_body_, ref->orig_w, ref->orig_h, img_split_, scale_);
            img_split_ = GitImageSplitFromX(lay, x);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (drag_bar_) {
            if (drag_bar_ == 1 || drag_bar_ == 2) {
                const bool list = drag_bar_ == 1;
                const D2D1_RECT_F view = list ? list_ : diff_body_;
                const float content = list ? ListContentHeight() : DiffContentHeight();
                fluent::ScrollbarSpec bar{view, list ? list_scroll_ : diff_scroll_, view.bottom - view.top, content};
                const D2D1_RECT_F thumb = fluent::ScrollbarThumbRect(bar, scale_, GitHighContrast());
                const float max_offset = (std::max)(0.0f, content - (view.bottom - view.top));
                const float offset = fluent::ScrollbarDragOffset(view.top, view.bottom - view.top,
                                                                 thumb.bottom - thumb.top, max_offset, y, drag_grab_);
                if (list) ScrollListTo(offset);
                else ScrollDiffTo(offset);
            } else if (drag_bar_ == 3) {
                const float rh = 54.0f * scale_;
                const float content = static_cast<float>(hist_commits_.size() + (set_.Total() > 0 ? 1 : 0)) * rh;
                fluent::ScrollbarSpec bar{hist_tl_rc_, hist_list_scroll_, hist_tl_rc_.bottom - hist_tl_rc_.top, content};
                const D2D1_RECT_F thumb = fluent::ScrollbarThumbRect(bar, scale_, GitHighContrast());
                const float max_offset = (std::max)(0.0f, content - (hist_tl_rc_.bottom - hist_tl_rc_.top));
                hist_list_scroll_ = std::clamp(fluent::ScrollbarDragOffset(hist_tl_rc_.top, hist_tl_rc_.bottom - hist_tl_rc_.top,
                                                                           thumb.bottom - thumb.top, max_offset, y, drag_grab_),
                                               0.0f, max_offset);
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (drag_bar_ == 5) {
                fluent::ScrollbarSpec bar{cf_body_, cf_body_scroll_, cf_body_.bottom - cf_body_.top, cf_body_content_h_};
                const D2D1_RECT_F thumb = fluent::ScrollbarThumbRect(bar, scale_, GitHighContrast());
                const float max_offset = (std::max)(0.0f, cf_body_content_h_ - (cf_body_.bottom - cf_body_.top));
                cf_body_scroll_ = std::clamp(fluent::ScrollbarDragOffset(cf_body_.top, cf_body_.bottom - cf_body_.top,
                                                                         thumb.bottom - thumb.top, max_offset, y, drag_grab_),
                                             0.0f, max_offset);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        if (!tracking_leave_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            tracking_leave_ = TrackMouseEvent(&tme) != FALSE;
        }
        const Hit hit = HitTest(x, y);
        if (hit.kind != hover_.kind || hit.row != hover_.row || hit.sub != hover_.sub) {
            hover_ = hit;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_leave_ = false;
        if (hover_.kind != HitKind::None) {
            hover_ = Hit{};
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            const Hit hit = HitTest(static_cast<float>(pt.x), static_cast<float>(pt.y));
            const bool hand = hit.kind == HitKind::SectionAction || hit.kind == HitKind::Undo ||
                              hit.kind == HitKind::RowDiscard || hit.kind == HitKind::RowCheck ||
                              hit.kind == HitKind::RowReveal || hit.kind == HitKind::SuggestChip ||
                              hit.kind == HitKind::HunkStage || hit.kind == HitKind::HunkDiscard;
            const wchar_t* cur = hit.kind == HitKind::Field || hit.kind == HitKind::HistSearchField
                ? IDC_IBEAM
                : hit.kind == HitKind::DiffImageHandle ? IDC_SIZEWE
                : hand ? IDC_HAND : IDC_ARROW;
            SetCursor(LoadCursorW(nullptr, cur));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const float x = static_cast<float>(GET_X_LPARAM(lparam));
        const float y = static_cast<float>(GET_Y_LPARAM(lparam));
        const Hit hit = HitTest(x, y);
        if (hit.kind != HitKind::Field && hit.kind != HitKind::HistSearchField) SetFocus(hwnd_);
        if (hit.kind == HitKind::ListBar || hit.kind == HitKind::DiffBar ||
            hit.kind == HitKind::HistListBar || hit.kind == HitKind::CfBodyBar ||
            hit.kind == HitKind::Row || hit.kind == HitKind::Field ||
            hit.kind == HitKind::HistSearchField || hit.kind == HitKind::DiffImageHandle) {
            OnClick(hit, x, y);
            if (message == WM_LBUTTONDBLCLK && hit.kind == HitKind::Row) ToggleStage(hit.row);
            return 0;
        }
        pressed_ = hit;
        SetCapture(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (dragging_img_split_) {
            dragging_img_split_ = false;
            ReleaseCapture();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (drag_bar_) {
            drag_bar_ = 0;
            ReleaseCapture();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        const float x = static_cast<float>(GET_X_LPARAM(lparam));
        const float y = static_cast<float>(GET_Y_LPARAM(lparam));
        const Hit pressed = pressed_;
        pressed_ = Hit{};
        if (GetCapture() == hwnd_) ReleaseCapture();
        const Hit hit = HitTest(x, y);
        if (pressed.kind != HitKind::None && hit.kind == pressed.kind && hit.row == pressed.row && hit.sub == pressed.sub)
            OnClick(hit, x, y);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_CAPTURECHANGED:
        drag_bar_ = 0;
        dragging_img_split_ = false;
        return 0;
    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd_, &pt);
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        const float x = static_cast<float>(pt.x), y = static_cast<float>(pt.y);
        if (active_tab_ == GitViewTab::Changes) {
            if (Inside(list_, x, y)) ScrollListTo(list_scroll_ - steps * 3.0f * ListRowHeight());
            else if (Inside(diff_body_, x, y)) {
                if (GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT) ScrollDiffX(-steps * char_w_ * 8.0f);
                else ScrollDiffTo(diff_scroll_ - steps * 3.0f * DiffRowHeight());
            }
        } else if (active_tab_ == GitViewTab::History) {
            if (Inside(hist_tl_rc_, x, y)) {
                const float rh = 54.0f * scale_;
                const float content = static_cast<float>(hist_commits_.size() + (set_.Total() > 0 ? 1 : 0)) * rh;
                const float max_y = (std::max)(0.0f, content - (hist_tl_rc_.bottom - hist_tl_rc_.top));
                hist_list_scroll_ = std::clamp(hist_list_scroll_ - steps * 3.0f * rh, 0.0f, max_y);
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (Inside(hist_right_, x, y)) {
                hist_detail_scroll_ = (std::max)(0.0f, hist_detail_scroll_ - steps * 48.0f * scale_);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        } else if (active_tab_ == GitViewTab::Conflict) {
            if (Inside(cf_body_, x, y)) {
                const float max_y = (std::max)(0.0f, cf_body_content_h_ - (cf_body_.bottom - cf_body_.top));
                cf_body_scroll_ = std::clamp(cf_body_scroll_ - steps * 48.0f * scale_, 0.0f, max_y);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_MOUSEHWHEEL: {
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        ScrollDiffX(steps * char_w_ * 8.0f);
        return 0;
    }
    case WM_SYSKEYDOWN:
        if (active_tab_ == GitViewTab::Changes) {
            if (wparam == VK_UP) {
                StepSelection(-1);
                return 0;
            }
            if (wparam == VK_DOWN) {
                StepSelection(1);
                return 0;
            }
        }
        break;
    case WM_KEYDOWN: {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (ctrl && wparam == L'B') {
            POINT pt{static_cast<LONG>(top_branch_rc_.left), static_cast<LONG>(top_branch_rc_.bottom + 4.0f * scale_)};
            ClientToScreen(hwnd_, &pt);
            TriggerBranchPopup(pt, false);
            return 0;
        }
        if (wparam == VK_ESCAPE) {
            Close();
            return 0;
        }
        if (wparam == VK_F5) {
            StartOp(OpRequest{});
            return 0;
        }
        if (active_tab_ == GitViewTab::Changes) {
            const float page = (std::max)(DiffRowHeight(), diff_body_.bottom - diff_body_.top - DiffRowHeight() * 2.0f);
            switch (wparam) {
            case VK_UP: StepSelection(-1); return 0;
            case VK_DOWN: StepSelection(1); return 0;
            case VK_SPACE: ToggleStage(sel_row_); return 0;
            case VK_DELETE:
                if (sel_section_ == Section::Changes)
                    if (const GitChange* c = Selected()) Discard({c});
                return 0;
            case VK_PRIOR: ScrollDiffTo(diff_scroll_ - page); return 0;
            case VK_NEXT: ScrollDiffTo(diff_scroll_ + page); return 0;
            case VK_HOME: ScrollDiffTo(0.0f); return 0;
            case VK_END: ScrollDiffTo(DiffContentHeight()); return 0;
            case VK_LEFT: ScrollDiffX(-char_w_ * 4.0f); return 0;
            case VK_RIGHT: ScrollDiffX(char_w_ * 4.0f); return 0;
            case VK_TAB:
                if (edit_) SetFocus(edit_);
                return 0;
            case VK_RETURN:
                if (ctrl) Commit(false);
                else if (edit_) SetFocus(edit_);
                return 0;
            default: break;
            }
        }
        break;
    }
    case WM_SYSCOMMAND:
        if ((wparam & 0xFFF0) == SC_KEYMENU && lparam == 0) return 0;
        break;
    case WM_CLOSE:
        Close();
        return 0;
    case WM_DESTROY:
        for (HWND* p : {&edit_, &hist_search_edit_, &hist_branch_edit_, &cf_custom_edit_}) {
            if (*p) {
                DestroyWindow(*p);
                *p = nullptr;
            }
        }
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

} // namespace pulse::ui

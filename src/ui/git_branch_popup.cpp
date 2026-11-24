#include "git_branch_popup.h"

#include "../app/git_terms.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "edit_host.h"
#include "typography.h"
#include "window_helpers.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace pulse::ui {
namespace {

constexpr wchar_t kBranchPopupClass[] = L"PulseGitBranchPopup";
constexpr UINT kMsgOverviewDone = WM_APP + 51;
constexpr UINT_PTR kPopupEditSubclass = 2;
constexpr float kPopupWidthDip = 388.0f;
constexpr float kPopupHeightDip = 520.0f;

using Sid = pulse::l10n::StringId;

bool InsideRect(const D2D1_RECT_F& rc, float x, float y) {
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}

std::wstring LowerText(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool MatchesQuery(const std::wstring& q, const std::wstring& a, const std::wstring& b = {}) {
    if (q.empty()) return true;
    const std::wstring ql = LowerText(q);
    return LowerText(a).find(ql) != std::wstring::npos ||
           (!b.empty() && LowerText(b).find(ql) != std::wstring::npos);
}

std::wstring TrimSpace(std::wstring s) {
    while (!s.empty() && std::iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::iswspace(s[i])) ++i;
    if (i > 0) s.erase(0, i);
    return s;
}

bool IsValidQuickBranchName(const std::wstring& name) {
    if (name.empty()) return false;
    for (wchar_t c : name) {
        if (std::iswspace(c) || c == L'~' || c == L'^' || c == L':' || c == L'?' ||
            c == L'*' || c == L'[' || c == L'\\')
            return false;
    }
    return name != L"." && name != L".." && !name.starts_with(L'-');
}

bool HighContrastOn() {
    HIGHCONTRASTW hc{sizeof(hc)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) &&
           (hc.dwFlags & HCF_HIGHCONTRASTON);
}

}  // namespace

GitBranchPopup::~GitBranchPopup() {
    StopLoad();
    if (hwnd_ && IsWindow(hwnd_)) {
        compositor_.Shutdown();
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    if (edit_font_) DeleteObject(edit_font_);
    if (edit_brush_) DeleteObject(edit_brush_);
}

bool GitBranchPopup::IsOpen() const noexcept {
    return hwnd_ && IsWindowVisible(hwnd_);
}

Theme GitBranchPopup::CurrentTheme() const {
    return HighContrastOn() ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
}

bool GitBranchPopup::EnsureWindow(HWND owner) {
    if (hwnd_) return true;
    static bool registered = false;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &GitBranchPopup::WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kBranchPopupClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        registered = true;
    }
    CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW, kBranchPopupClass, L"",
                    WS_POPUP | WS_CLIPCHILDREN, 0, 0, 100, 100, owner, nullptr, instance, this);
    return hwnd_ != nullptr;
}

void GitBranchPopup::PositionPopup() {
    if (!hwnd_) return;
    HMONITOR mon = MonitorFromPoint(anchor_pt_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const UINT dpi = pulse::compat::WindowDpi(hwnd_);
    scale_ = static_cast<float>(dpi) / 96.0f;

    const int w = static_cast<int>(std::lround(kPopupWidthDip * scale_));
    const int max_h = (std::max)(260, static_cast<int>(mi.rcWork.bottom - mi.rcWork.top - 24));
    const int h = (std::min)(max_h, static_cast<int>(std::lround(kPopupHeightDip * scale_)));

    int x = anchor_pt_.x;
    if (x + w > mi.rcWork.right - 8) x = mi.rcWork.right - 8 - w;
    if (x < mi.rcWork.left + 8) x = mi.rcWork.left + 8;

    int y = open_above_ ? (anchor_pt_.y - h) : anchor_pt_.y;
    if (y + h > mi.rcWork.bottom - 8) y = anchor_pt_.y - h;
    if (y < mi.rcWork.top + 8) y = mi.rcWork.top + 8;

    SetWindowPos(hwnd_, HWND_TOP, x, y, w, h, SWP_NOACTIVATE);
}

void GitBranchPopup::ApplyTheme() {
    if (!hwnd_) return;
    const BOOL dark = dark_ ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const DWORD corner = 2 /* DWMWCP_ROUND */;
    DwmSetWindowAttribute(hwnd_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
    if (edit_brush_) DeleteObject(edit_brush_);
    edit_brush_ = CreateSolidBrush(EditBackColor(dark_));
    if (search_edit_) InvalidateRect(search_edit_, nullptr, TRUE);
}

void GitBranchPopup::Show(HWND owner, const std::wstring& root, POINT anchor_screen_pt, bool open_above,
                          bool dark, D2D1_COLOR_F accent, bool plain_terms,
                          GitBranchPopupCallbacks callbacks) {
    owner_ = owner;
    root_ = root;
    anchor_pt_ = anchor_screen_pt;
    open_above_ = open_above;
    dark_ = dark;
    accent_ = accent;
    plain_terms_ = plain_terms;
    callbacks_ = std::move(callbacks);
    closing_ = false;
    scroll_y_ = 0.0f;
    hover_ = Hit{};
    pressed_ = Hit{};

    if (!EnsureWindow(owner)) return;
    if (owner && GetParent(hwnd_) != owner) {
        SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    }
    PositionPopup();
    ApplyTheme();
    if (search_edit_) SetWindowTextW(search_edit_, L"");
    overview_ = app::GitBranchOverview{};
    loading_ = true;
    RebuildRows();
    Layout();
    LayoutEdit();
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    if (search_edit_) SetFocus(search_edit_);
    StartLoad();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void GitBranchPopup::Hide() {
    if (closing_) return;
    closing_ = true;
    StopLoad();
    const bool was_visible = IsOpen();
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    if (was_visible && callbacks_.on_closed) {
        auto fn = std::move(callbacks_.on_closed);
        callbacks_.on_closed = nullptr;
        fn();
    }
}

void GitBranchPopup::StopLoad() {
    ++generation_;
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(job_mutex_);
    pending_job_.reset();
}

void GitBranchPopup::StartLoad() {
    StopLoad();
    if (root_.empty() || !hwnd_) return;
    loading_ = true;
    const uint64_t gen = ++generation_;
    const HWND target = hwnd_;
    const std::wstring root = root_;
    worker_ = std::thread([this, target, gen, root]() {
        auto job = std::make_unique<FetchJob>();
        job->generation = gen;
        job->overview = app::GitReadBranchOverview(root);
        {
            std::lock_guard<std::mutex> lock(job_mutex_);
            pending_job_ = std::move(job);
        }
        PostMessageW(target, kMsgOverviewDone, 0, 0);
    });
}

void GitBranchPopup::AdoptLoad() {
    std::unique_ptr<FetchJob> job;
    {
        std::lock_guard<std::mutex> lock(job_mutex_);
        job = std::move(pending_job_);
    }
    if (!job || job->generation != generation_) return;
    if (worker_.joinable()) worker_.join();
    loading_ = false;
    if (job->overview.ok) overview_ = std::move(job->overview);
    RebuildRows();
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::wstring GitBranchPopup::QueryText() const {
    if (!search_edit_) return {};
    const int n = GetWindowTextLengthW(search_edit_);
    std::wstring text(static_cast<size_t>(n), L'\0');
    if (n > 0) GetWindowTextW(search_edit_, text.data(), n + 1);
    return TrimSpace(std::move(text));
}

void GitBranchPopup::RebuildRows() {
    rows_.clear();
    const std::wstring q = QueryText();
    const float s = scale_;
    const float hdr_h = 26.0f * s;
    const float item_h = 44.0f * s;
    const float create_h = 36.0f * s;

    bool exact_local = false;
    for (const auto& b : overview_.local_branches) {
        if (_wcsicmp(b.name.c_str(), q.c_str()) == 0) {
            exact_local = true;
            break;
        }
    }
    if (!q.empty() && !exact_local && IsValidQuickBranchName(q)) {
        ListRow cr;
        cr.kind = RowKind::CreateBranch;
        cr.key = q;
        cr.title = app::GitFmt(l10n::Get(Sid::GitCreateAndSwitch), L"{name}", q);
        cr.h = create_h;
        rows_.push_back(std::move(cr));
    }

    // Local branches
    std::vector<const app::GitBranchInfo*> locals;
    for (const auto& b : overview_.local_branches) {
        if (MatchesQuery(q, b.name, b.subject)) locals.push_back(&b);
    }
    if (!locals.empty()) {
        ListRow hdr;
        hdr.kind = RowKind::Header;
        hdr.title = app::GitStr(plain_terms_, Sid::GitLocalBranchesPlain, Sid::GitLocalBranchesGit);
        hdr.h = hdr_h;
        rows_.push_back(std::move(hdr));
        for (const auto* b : locals) {
            ListRow row;
            row.kind = RowKind::LocalBranch;
            row.title = b->name;
            row.key = b->name;
            row.is_current = b->is_current;
            if (b->is_current) {
                row.subtitle = app::GitStr(plain_terms_, Sid::GitBranchCurrentHintPlain, Sid::GitBranchCurrentHintGit);
            } else {
                row.subtitle = b->last_author;
                if (!b->last_date.empty()) {
                    if (!row.subtitle.empty()) row.subtitle += L" \u00B7 ";
                    row.subtitle += b->last_date;
                }
                if (!b->subject.empty()) {
                    if (!row.subtitle.empty()) row.subtitle += L" \u00B7 ";
                    row.subtitle += b->subject;
                }
            }
            row.h = item_h;
            rows_.push_back(std::move(row));
        }
    }

    // Remote branches
    std::vector<const app::GitBranchInfo*> remotes;
    for (const auto& b : overview_.remote_branches) {
        if (MatchesQuery(q, b.name, b.last_author)) remotes.push_back(&b);
    }
    if (!remotes.empty()) {
        ListRow hdr;
        hdr.kind = RowKind::Header;
        hdr.title = app::GitStr(plain_terms_, Sid::GitRemoteBranchesPlain, Sid::GitRemoteBranchesGit);
        hdr.h = hdr_h;
        rows_.push_back(std::move(hdr));
        for (const auto* b : remotes) {
            ListRow row;
            row.kind = RowKind::RemoteBranch;
            row.title = plain_terms_ ? b->short_name : b->name;
            row.key = b->name;
            if (plain_terms_) {
                row.subtitle = b->last_author;
                if (!b->last_date.empty()) {
                    if (!row.subtitle.empty()) row.subtitle += L" \u00B7 ";
                    row.subtitle += b->last_date;
                }
                if (!row.subtitle.empty()) row.subtitle += L" \u00B7 ";
                row.subtitle += l10n::Get(Sid::GitRemoteBranchHintPlain);
            } else {
                row.subtitle = b->name;
            }
            row.h = item_h;
            rows_.push_back(std::move(row));
        }
    }

    // Stashes (only when not typing a search query, or if matching)
    if (q.empty()) {
        ListRow hdr;
        hdr.kind = RowKind::Header;
        hdr.title = app::GitStr(plain_terms_, Sid::GitStashListPlain, Sid::GitStashListGit);
        hdr.h = hdr_h;
        rows_.push_back(std::move(hdr));
        if (overview_.stashes.empty()) {
            ListRow empty;
            empty.kind = RowKind::EmptyStash;
            empty.title = l10n::Get(Sid::GitStashEmpty);
            empty.h = 30.0f * s;
            rows_.push_back(std::move(empty));
        } else {
            for (const auto& st : overview_.stashes) {
                ListRow row;
                row.kind = RowKind::Stash;
                row.title = st.message.empty() ? st.ref : st.message;
                row.key = st.ref;
                row.subtitle = st.date_text;
                if (st.file_count > 0) {
                    if (!row.subtitle.empty()) row.subtitle += L" \u00B7 ";
                    row.subtitle += app::GitFmtCount(l10n::Get(Sid::GitStashFilesCount), st.file_count);
                }
                row.h = item_h;
                rows_.push_back(std::move(row));
            }
        }
    }

    float y = 0.0f;
    for (auto& row : rows_) {
        row.y = y;
        y += row.h;
    }
    list_content_h_ = y;
    ScrollTo(scroll_y_);
}

void GitBranchPopup::RecreateFormats() {
    auto* factory = compositor_.DwriteFactory();
    if (!factory) return;
    ui_.reset();
    ui_bold_.reset();
    small_.reset();
    small_bold_.reset();
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
    auto make = [&](DWRITE_FONT_WEIGHT weight, float dip, ComPtr<IDWriteTextFormat>& out) {
        if (FAILED(factory->CreateTextFormat(ui_family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, dip * scale_ * typography::UiFontScale(),
                                             locale, &out)) ||
            !out.get())
            return;
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    };
    make(DWRITE_FONT_WEIGHT_NORMAL, 13.0f, ui_);
    make(DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f, ui_bold_);
    make(DWRITE_FONT_WEIGHT_NORMAL, 11.5f, small_);
    make(DWRITE_FONT_WEIGHT_SEMI_BOLD, 11.5f, small_bold_);
}

float GitBranchPopup::Measure(std::wstring_view text, IDWriteTextFormat* format) const {
    auto* factory = compositor_.DwriteFactory();
    if (!factory || !format || text.empty()) return 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, 10000.0f, 100.0f,
                                         &layout)) ||
        !layout.get())
        return 0.0f;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void GitBranchPopup::CreateEdit() {
    if (search_edit_ || !hwnd_) return;
    search_edit_ = CreateRedirectedChildEdit(hwnd_, L"");
    if (!search_edit_) return;
    SetWindowTheme(search_edit_, L"", L"");
    SendMessageW(search_edit_, EM_LIMITTEXT, 200, 0);
    SetWindowSubclass(search_edit_, EditProc, kPopupEditSubclass, reinterpret_cast<DWORD_PTR>(this));
}

void GitBranchPopup::LayoutEdit() {
    if (!search_edit_ || !hwnd_) return;
    if (!edit_font_) {
        edit_font_ = typography::CreateEditFont(scale_);
        if (edit_font_) {
            SendMessageW(search_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_), TRUE);
            SendMessageW(search_edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        }
    }
    const std::wstring cue = app::GitStr(plain_terms_, Sid::GitBranchSearchPhPlain, Sid::GitBranchSearchPhGit);
    if (cue != edit_cue_) {
        edit_cue_ = cue;
        SetPropW(search_edit_, L"Pulse.EditCue", const_cast<wchar_t*>(edit_cue_.c_str()));
        const Theme theme = CurrentTheme();
        const COLORREF cue_rgb = RGB(static_cast<int>(theme.text_secondary.r * 255.0f),
                                     static_cast<int>(theme.text_secondary.g * 255.0f),
                                     static_cast<int>(theme.text_secondary.b * 255.0f));
        SetPropW(search_edit_, L"Pulse.EditCueColor",
                 reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(0x01000000u | cue_rgb)));
    }
    const int x = static_cast<int>(std::lround(search_rc_.left + 30.0f * scale_));
    const int y = static_cast<int>(std::lround(search_rc_.top + 9.0f * scale_));
    const int w = (std::max)(20, static_cast<int>(std::lround(search_rc_.right - search_rc_.left - 40.0f * scale_)));
    const int h = (std::max)(16, static_cast<int>(std::lround(search_rc_.bottom - search_rc_.top - 18.0f * scale_)));
    SetWindowPos(search_edit_, HWND_TOP, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    PresentChildEdit(compositor_, compositor_.TextFormat(), ColorFromRef(EditTextColor(dark_)),
                     ColorFromRef(EditBackColor(dark_)), search_edit_);
}

void GitBranchPopup::Layout() {
    const float s = scale_;
    const float w = static_cast<float>(compositor_.Width());
    const float h = static_cast<float>(compositor_.Height());
    const float pad = 12.0f * s;

    search_rc_ = D2D1::RectF(0, 0, w, 40.0f * s);
    cur_rc_ = D2D1::RectF(0, search_rc_.bottom, w, search_rc_.bottom + 126.0f * s);

    const float box_w = (w - 2.0f * pad - 8.0f * s) * 0.5f;
    const float box_top = cur_rc_.top + 36.0f * s;
    const float box_bot = box_top + 46.0f * s;
    behind_box_ = D2D1::RectF(pad, box_top, pad + box_w, box_bot);
    ahead_box_ = D2D1::RectF(behind_box_.right + 8.0f * s, box_top, w - pad, box_bot);

    const float btn_top = box_bot + 8.0f * s;
    const float btn_bot = btn_top + 28.0f * s;
    const float fetch_w = 28.0f * s;
    fetch_btn_ = D2D1::RectF(w - pad - fetch_w, btn_top, w - pad, btn_bot);
    const std::wstring pull_t = app::GitStr(plain_terms_, Sid::GitFetchUpdatesPlain, Sid::GitPull);
    const std::wstring push_t = app::GitStr(plain_terms_, Sid::GitUploadPlain, Sid::GitPush);
    const float pull_w = painter_.MeasureButtonWidth(pull_t, L"\xE896") - 4.0f * s;
    const float push_w = painter_.MeasureButtonWidth(push_t, L"\xE898") - 4.0f * s;
    push_btn_ = D2D1::RectF(fetch_btn_.left - 6.0f * s - push_w, btn_top, fetch_btn_.left - 6.0f * s, btn_bot);
    pull_btn_ = D2D1::RectF(push_btn_.left - 6.0f * s - pull_w, btn_top, push_btn_.left - 6.0f * s, btn_bot);
    sync_btn_ = D2D1::RectF(pad, btn_top, (std::max)(pad + 60.0f * s, pull_btn_.left - 6.0f * s), btn_bot);

    foot_rc_ = D2D1::RectF(0, (std::max)(cur_rc_.bottom, h - 42.0f * s), w, h);
    list_rc_ = D2D1::RectF(0, cur_rc_.bottom, w, foot_rc_.top);

    const float nw = painter_.MeasureButtonWidth(l10n::Get(Sid::GitNewBranch), L"\xE710");
    foot_new_ = D2D1::RectF(pad, foot_rc_.top + 7.0f * s, pad + nw, foot_rc_.bottom - 7.0f * s);
    const std::wstring hist_t = app::GitStr(plain_terms_, Sid::GitHistoryPlain, Sid::GitHistoryGit);
    const float hw = painter_.MeasureButtonWidth(hist_t, L"\xE81C");
    foot_hist_ = D2D1::RectF(w - pad - hw, foot_rc_.top + 7.0f * s, w - pad, foot_rc_.bottom - 7.0f * s);

    for (size_t i = 0; i < rows_.size(); ++i) {
        auto& row = rows_[i];
        const float top = list_rc_.top + row.y - scroll_y_;
        row.rect = D2D1::RectF(6.0f * s, top, w - 12.0f * s, top + row.h);
        row.action1 = row.action2 = D2D1::RectF(0, 0, 0, 0);
        if (row.kind == RowKind::LocalBranch && !row.is_current) {
            row.action1 = D2D1::RectF(row.rect.right - 30.0f * s, top + 8.0f * s,
                                      row.rect.right - 4.0f * s, top + row.h - 8.0f * s);
        } else if (row.kind == RowKind::Stash) {
            row.action2 = D2D1::RectF(row.rect.right - 28.0f * s, top + 8.0f * s,
                                      row.rect.right - 4.0f * s, top + row.h - 8.0f * s);
            const std::wstring unstash = app::GitStr(plain_terms_, Sid::GitUnstashPlain, Sid::GitUnstashGit);
            const float aw = Measure(unstash, small_.get()) + 16.0f * s;
            row.action1 = D2D1::RectF(row.action2.left - 4.0f * s - aw, top + 8.0f * s,
                                      row.action2.left - 4.0f * s, top + row.h - 8.0f * s);
        }
    }
}

void GitBranchPopup::ScrollTo(float y) {
    const float view_h = list_rc_.bottom - list_rc_.top;
    const float max_y = (std::max)(0.0f, list_content_h_ - view_h);
    scroll_y_ = std::clamp(y, 0.0f, max_y);
}

GitBranchPopup::Hit GitBranchPopup::HitTest(float x, float y) const {
    if (InsideRect(search_rc_, x, y)) return {HitKind::SearchField, -1};
    if (InsideRect(sync_btn_, x, y)) return {HitKind::SyncBtn, -1};
    if (InsideRect(pull_btn_, x, y)) return {HitKind::PullBtn, -1};
    if (InsideRect(push_btn_, x, y)) return {HitKind::PushBtn, -1};
    if (InsideRect(fetch_btn_, x, y)) return {HitKind::FetchBtn, -1};
    if (InsideRect(foot_new_, x, y)) return {HitKind::FootNewBranch, -1};
    if (InsideRect(foot_hist_, x, y)) return {HitKind::FootHistory, -1};
    if (InsideRect(list_rc_, x, y)) {
        fluent::ScrollbarSpec bar{list_rc_, scroll_y_, list_rc_.bottom - list_rc_.top, list_content_h_};
        // ScrollbarHitTest covers the whole viewport; only its right-edge strip is the bar.
        if (fluent::ScrollbarHitTest(bar, x, y) && x >= list_rc_.right - 14.0f * scale_ &&
            list_content_h_ > list_rc_.bottom - list_rc_.top)
            return {HitKind::ListBar, -1};
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
            const auto& row = rows_[static_cast<size_t>(i)];
            if (row.kind == RowKind::Header || row.kind == RowKind::EmptyStash) continue;
            if (InsideRect(row.action2, x, y)) return {HitKind::RowAction2, i};
            if (InsideRect(row.action1, x, y)) return {HitKind::RowAction1, i};
            if (InsideRect(row.rect, x, y)) return {HitKind::Row, i};
        }
    }
    return {};
}

void GitBranchPopup::OnClick(const Hit& hit, float x, float y) {
    (void)x;
    switch (hit.kind) {
    case HitKind::SearchField:
        if (search_edit_) SetFocus(search_edit_);
        break;
    case HitKind::FootNewBranch:
        if (search_edit_) SetFocus(search_edit_);
        break;
    case HitKind::ListBar: {
        fluent::ScrollbarSpec bar{list_rc_, scroll_y_, list_rc_.bottom - list_rc_.top, list_content_h_};
        const D2D1_RECT_F thumb = fluent::ScrollbarThumbRect(bar, scale_, HighContrastOn());
        drag_bar_ = true;
        drag_grab_ = fluent::ScrollbarGrabOffset(thumb.top, thumb.bottom - thumb.top, y);
        SetCapture(hwnd_);
        break;
    }
    case HitKind::SyncBtn: {
        auto fn = callbacks_.on_sync;
        Hide();
        if (fn) fn();
        break;
    }
    case HitKind::PullBtn: {
        auto fn = callbacks_.on_pull;
        Hide();
        if (fn) fn();
        break;
    }
    case HitKind::PushBtn: {
        auto fn = callbacks_.on_push;
        Hide();
        if (fn) fn();
        break;
    }
    case HitKind::FetchBtn: {
        auto fn = callbacks_.on_fetch;
        Hide();
        if (fn) fn();
        break;
    }
    case HitKind::FootHistory: {
        auto fn = callbacks_.on_open_history;
        Hide();
        if (fn) fn();
        break;
    }
    case HitKind::Row:
    case HitKind::RowAction1:
    case HitKind::RowAction2: {
        if (hit.index < 0 || hit.index >= static_cast<int>(rows_.size())) break;
        const ListRow row = rows_[static_cast<size_t>(hit.index)];
        if (row.kind == RowKind::CreateBranch) {
            auto fn = callbacks_.on_create_branch;
            Hide();
            if (fn) fn(row.key);
        } else if (row.kind == RowKind::LocalBranch) {
            if (hit.kind == HitKind::RowAction1) {
                auto fn = callbacks_.on_merge_branch;
                Hide();
                if (fn) fn(row.key);
            } else if (!row.is_current) {
                auto fn = callbacks_.on_switch_branch;
                const int dirty = overview_.changed_count;
                Hide();
                if (fn) fn(row.key, dirty);
            }
        } else if (row.kind == RowKind::RemoteBranch) {
            auto fn = callbacks_.on_checkout_remote;
            Hide();
            if (fn) fn(row.key);
        } else if (row.kind == RowKind::Stash) {
            if (hit.kind == HitKind::RowAction1) {
                auto fn = callbacks_.on_stash_apply;
                Hide();
                if (fn) fn(row.key);
            } else if (hit.kind == HitKind::RowAction2) {
                auto fn = callbacks_.on_stash_drop;
                Hide();
                if (fn) fn(row.key, row.title);
            }
        }
        break;
    }
    default:
        break;
    }
}

void GitBranchPopup::Render() {
    if (!compositor_.Dc()) return;
    if (compositor_.NeedsRecovery()) {
        if (!compositor_.Recover()) return;
        compositor_.RecreateTextFormats(scale_);
        brush_.reset();
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    compositor_.Resize((std::max)(1L, client.right), (std::max)(1L, client.bottom));
    Layout();
    LayoutEdit();

    auto* dc = compositor_.Dc();
    const bool hc = HighContrastOn();
    const Theme theme = CurrentTheme();
    const float s = scale_;
    const float w = static_cast<float>(compositor_.Width());
    const float h = static_cast<float>(compositor_.Height());
    const float line_w = (std::max)(1.0f, std::floor(s));

    const D2D1_COLOR_F bg_fly = hc ? theme.bg : D2D1::ColorF(dark_ ? 0x2A2C33 : 0xFFFFFF);
    const D2D1_COLOR_F bg_sub = hc ? theme.bg : D2D1::ColorF(dark_ ? 0x1D1F24 : 0xF5F6F9);
    const D2D1_COLOR_F ok_fg = D2D1::ColorF(dark_ ? 0x3DD68C : 0x1F9D55);

    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->Clear(bg_fly);
    if (!brush_.get()) dc->CreateSolidColorBrush(theme.text, &brush_);
    painter_.SetCompositor(&compositor_);
    painter_.SetScale(s);
    const bool painter_ok = painter_.BeginFrame(theme, hc);

    auto text_at = [&](std::wstring_view txt, const D2D1_RECT_F& rc, IDWriteTextFormat* fmt, const D2D1_COLOR_F& col) {
        if (!fmt || txt.empty() || rc.right <= rc.left) return;
        brush_->SetColor(col);
        dc->DrawTextW(txt.data(), static_cast<UINT32>(txt.size()), fmt, rc, brush_.get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    };
    auto fit = [&](const std::wstring& txt, float max_w, IDWriteTextFormat* fmt) {
        if (Measure(txt, fmt) <= max_w) return txt;
        return FitTextEnd(txt, max_w, [&](std::wstring_view t) { return Measure(t, fmt); });
    };

    // Search bar
    painter_.DrawGlyph(L"\xE721", D2D1::RectF(12.0f * s, search_rc_.top + 12.0f * s, 28.0f * s, search_rc_.top + 28.0f * s),
                       theme.text_secondary);
    brush_->SetColor(theme.stroke_divider);
    dc->FillRectangle(D2D1::RectF(0, search_rc_.bottom - line_w, w, search_rc_.bottom), brush_.get());

    // Current branch + sync boxes
    const float pad = 12.0f * s;
    painter_.DrawGlyph(L"\xF003", D2D1::RectF(pad, cur_rc_.top + 10.0f * s, pad + 18.0f * s, cur_rc_.top + 28.0f * s),
                       theme.accent);
    const std::wstring cur_name = overview_.current_branch.empty()
        ? (overview_.head.empty() ? l10n::Get(Sid::GitScDetached) : overview_.head)
        : overview_.current_branch;
    const float nw = (std::min)(Measure(cur_name, ui_bold_.get()) + 4.0f, 160.0f * s);
    text_at(fit(cur_name, nw, ui_bold_.get()),
            D2D1::RectF(pad + 22.0f * s, cur_rc_.top + 6.0f * s, pad + 22.0f * s + nw, cur_rc_.top + 30.0f * s),
            ui_bold_.get(), theme.text);

    if (overview_.changed_count > 0) {
        const std::wstring pill = app::GitFmtCount(
            app::GitStr(plain_terms_, Sid::GitBranchChangesPillPlain, Sid::GitBranchChangesPillGit),
            overview_.changed_count);
        const float pw = painter_.MeasureTagWidth(pill);
        fluent::TagSpec tag{};
        tag.bounds = D2D1::RectF(w - pad - pw, cur_rc_.top + 8.0f * s, w - pad, cur_rc_.top + 28.0f * s);
        tag.text = pill;
        tag.color = D2D1::ColorF(dark_ ? 0xF5B941 : 0xC77D02);
        tag.bordered = false;
        painter_.DrawTag(tag);
    }

    auto draw_box = [&](const D2D1_RECT_F& rc, const std::wstring& top_t, const std::wstring& sub_t,
                        const D2D1_COLOR_F& col) {
        painter_.FillRoundedRect(rc, 7.0f * s, bg_sub);
        painter_.StrokeRoundedRect(rc, 7.0f * s, theme.stroke_card, 1.0f);
        text_at(fit(top_t, rc.right - rc.left - 16.0f * s, small_bold_.get()),
                D2D1::RectF(rc.left + 9.0f * s, rc.top + 4.0f * s, rc.right - 8.0f * s, rc.top + 24.0f * s),
                small_bold_.get(), col);
        text_at(fit(sub_t, rc.right - rc.left - 16.0f * s, small_.get()),
                D2D1::RectF(rc.left + 9.0f * s, rc.top + 23.0f * s, rc.right - 8.0f * s, rc.bottom - 4.0f * s),
                small_.get(), theme.text_secondary);
    };
    const std::wstring dn_t = L"\u2193 " + app::GitFmtCount(
        app::GitStr(plain_terms_, Sid::GitBehindBoxPlain, Sid::GitBehindBoxGit), overview_.behind);
    const std::wstring up_t = L"\u2191 " + app::GitFmtCount(
        app::GitStr(plain_terms_, Sid::GitAheadBoxPlain, Sid::GitAheadBoxGit), overview_.ahead);
    const std::wstring dn_sub = plain_terms_
        ? l10n::Get(Sid::GitBehindDescPlain)
        : (overview_.upstream.empty() ? l10n::Get(Sid::AiGitNoUpstream) : overview_.upstream);
    const std::wstring up_sub = app::GitStr(plain_terms_, Sid::GitAheadDescPlain, Sid::GitAheadDescGit);
    draw_box(behind_box_, dn_t, dn_sub, ok_fg);
    draw_box(ahead_box_, up_t, up_sub, theme.accent);

    if (painter_ok) {
        auto btn = [&](const D2D1_RECT_F& rc, const std::wstring& txt, std::wstring_view glyph,
                       fluent::ButtonKind kind, HitKind id, bool icon_only = false) {
            fluent::ButtonSpec spec{};
            spec.bounds = rc;
            spec.text = txt;
            spec.glyph = glyph;
            spec.kind = kind;
            spec.icon_only = icon_only;
            spec.state.enabled = true;
            spec.state.hovered = hover_.kind == id;
            spec.state.pressed = pressed_.kind == id && hover_.kind == id;
            painter_.DrawButton(spec);
        };
        btn(sync_btn_, l10n::Get(Sid::GitSyncLabel), L"\xE895", fluent::ButtonKind::Primary, HitKind::SyncBtn);
        btn(pull_btn_, app::GitStr(plain_terms_, Sid::GitFetchUpdatesPlain, Sid::GitPull),
            L"\xE896", fluent::ButtonKind::Standard, HitKind::PullBtn);
        btn(push_btn_, app::GitStr(plain_terms_, Sid::GitUploadPlain, Sid::GitPush),
            L"\xE898", fluent::ButtonKind::Standard, HitKind::PushBtn);
        btn(fetch_btn_, {}, L"\xE72C", fluent::ButtonKind::Standard, HitKind::FetchBtn, true);
    }
    brush_->SetColor(theme.stroke_divider);
    dc->FillRectangle(D2D1::RectF(0, cur_rc_.bottom - line_w, w, cur_rc_.bottom), brush_.get());

    // Scrollable list
    dc->PushAxisAlignedClip(list_rc_, D2D1_ANTIALIAS_MODE_ALIASED);
    if (loading_ && rows_.empty()) {
        text_at(l10n::Get(Sid::GitScLoading),
                D2D1::RectF(pad, list_rc_.top + 12.0f * s, w - pad, list_rc_.top + 34.0f * s),
                small_.get(), theme.text_secondary);
    } else {
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
            const auto& row = rows_[static_cast<size_t>(i)];
            if (row.rect.bottom < list_rc_.top || row.rect.top > list_rc_.bottom) continue;
            if (row.kind == RowKind::Header) {
                text_at(row.title, D2D1::RectF(pad, row.rect.top + 4.0f * s, w - pad, row.rect.bottom),
                        small_bold_.get(), theme.text_secondary);
                continue;
            }
            if (row.kind == RowKind::EmptyStash) {
                text_at(row.title, D2D1::RectF(pad + 4.0f * s, row.rect.top, w - pad, row.rect.bottom),
                        small_.get(), theme.text_secondary);
                continue;
            }
            const bool hot = hover_.index == i && (hover_.kind == HitKind::Row ||
                                                   hover_.kind == HitKind::RowAction1 ||
                                                   hover_.kind == HitKind::RowAction2);
            if (row.is_current) painter_.FillRoundedRect(row.rect, 6.0f * s, theme.fill_selected);
            else if (hot) painter_.FillRoundedRect(row.rect, 6.0f * s, theme.fill_hover);

            const wchar_t* glyph = L"\xF003";
            D2D1_COLOR_F icon_col = row.is_current ? theme.accent : theme.text_secondary;
            if (row.kind == RowKind::CreateBranch) {
                glyph = L"\xE710";
                icon_col = theme.accent;
            } else if (row.kind == RowKind::LocalBranch && row.is_current) {
                glyph = L"\xE73E";
            } else if (row.kind == RowKind::RemoteBranch) {
                glyph = L"\xE753";
            } else if (row.kind == RowKind::Stash) {
                glyph = L"\xE7B8";
            }
            const float cy = (row.rect.top + row.rect.bottom) * 0.5f;
            painter_.DrawGlyph(glyph, D2D1::RectF(row.rect.left + 8.0f * s, cy - 8.0f * s,
                                                  row.rect.left + 24.0f * s, cy + 8.0f * s), icon_col);

            float text_right = row.rect.right - 8.0f * s;
            if (row.action1.right > row.action1.left) text_right = row.action1.left - 6.0f * s;

            const float tx = row.rect.left + 30.0f * s;
            if (row.kind == RowKind::CreateBranch) {
                text_at(fit(row.title, text_right - tx, ui_bold_.get()),
                        D2D1::RectF(tx, row.rect.top, text_right, row.rect.bottom), ui_bold_.get(), theme.accent);
            } else {
                text_at(fit(row.title, text_right - tx, ui_.get()),
                        D2D1::RectF(tx, row.rect.top + 3.0f * s, text_right, row.rect.top + 23.0f * s),
                        ui_.get(), theme.text);
                text_at(fit(row.subtitle, text_right - tx, small_.get()),
                        D2D1::RectF(tx, row.rect.top + 22.0f * s, text_right, row.rect.bottom - 2.0f * s),
                        small_.get(), theme.text_secondary);
            }

            if (row.kind == RowKind::LocalBranch && !row.is_current && hot) {
                if (hover_.kind == HitKind::RowAction1 && hover_.index == i)
                    painter_.FillRoundedRect(row.action1, 4.0f * s, theme.fill_pressed);
                painter_.DrawGlyph(L"\xEA3C", row.action1, theme.text);
            } else if (row.kind == RowKind::Stash) {
                const bool h1 = hover_.kind == HitKind::RowAction1 && hover_.index == i;
                const bool h2 = hover_.kind == HitKind::RowAction2 && hover_.index == i;
                if (h1) painter_.FillRoundedRect(row.action1, 4.0f * s, theme.fill_pressed);
                const std::wstring unstash = app::GitStr(plain_terms_, Sid::GitUnstashPlain, Sid::GitUnstashGit);
                const auto oa = small_->GetTextAlignment();
                small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                text_at(unstash, row.action1, small_.get(), theme.accent);
                small_->SetTextAlignment(oa);
                if (h2) painter_.FillRoundedRect(row.action2, 4.0f * s, theme.fill_pressed);
                painter_.DrawGlyph(L"\xE74D", row.action2, h2 ? theme.danger : theme.text_secondary);
            }
        }
        if (painter_ok && list_content_h_ > list_rc_.bottom - list_rc_.top) {
            fluent::ScrollbarSpec bar{list_rc_, scroll_y_, list_rc_.bottom - list_rc_.top, list_content_h_};
            bar.expand_progress = (hover_.kind == HitKind::ListBar || drag_bar_) ? 1.0f : 0.0f;
            painter_.DrawScrollbar(bar);
        }
    }
    dc->PopAxisAlignedClip();

    // Footer
    brush_->SetColor(bg_sub);
    dc->FillRectangle(foot_rc_, brush_.get());
    brush_->SetColor(theme.stroke_divider);
    dc->FillRectangle(D2D1::RectF(0, foot_rc_.top, w, foot_rc_.top + line_w), brush_.get());
    if (painter_ok) {
        fluent::ButtonSpec b1{};
        b1.bounds = foot_new_;
        b1.text = l10n::Get(Sid::GitNewBranch);
        b1.glyph = L"\xE710";
        b1.kind = fluent::ButtonKind::Standard;
        b1.state.enabled = true;
        b1.state.hovered = hover_.kind == HitKind::FootNewBranch;
        b1.state.pressed = pressed_.kind == HitKind::FootNewBranch && hover_.kind == HitKind::FootNewBranch;
        painter_.DrawButton(b1);

        fluent::ButtonSpec b2{};
        b2.bounds = foot_hist_;
        b2.text = app::GitStr(plain_terms_, Sid::GitHistoryPlain, Sid::GitHistoryGit);
        b2.glyph = L"\xE81C";
        b2.kind = fluent::ButtonKind::Transparent;
        b2.state.enabled = true;
        b2.state.hovered = hover_.kind == HitKind::FootHistory;
        b2.state.pressed = pressed_.kind == HitKind::FootHistory && hover_.kind == HitKind::FootHistory;
        painter_.DrawButton(b2);
    }

    painter_.StrokeRoundedRect(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), 8.0f * s, theme.stroke_card, 1.0f);
    const HRESULT hr = dc->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        compositor_.NotifyDeviceLost(hr);
    else
        compositor_.Present();
}

LRESULT CALLBACK GitBranchPopup::EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                          UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<GitBranchPopup*>(ref);
    if (!self) return DefSubclassProc(hwnd, msg, wparam, lparam);
    const D2D1_COLOR_F fore = ColorFromRef(EditTextColor(self->dark_));
    const D2D1_COLOR_F back = ColorFromRef(EditBackColor(self->dark_));
    IDWriteTextFormat* fmt = self->compositor_.TextFormat();
    switch (msg) {
    case WM_GETDLGCODE:
        return DefSubclassProc(hwnd, msg, wparam, lparam) | DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            self->Hide();
            return 0;
        }
        if (wparam == VK_RETURN) {
            const std::wstring q = self->QueryText();
            if (!q.empty() && IsValidQuickBranchName(q)) {
                auto fn = self->callbacks_.on_create_branch;
                self->Hide();
                if (fn) fn(q);
            }
            return 0;
        }
        break;
    case WM_CHAR:
        if (wparam == 27 || wparam == L'\r' || wparam == L'\n') return 0;
        break;
    case WM_NCDESTROY: {
        LRESULT ignored = 0;
        HandleChildEditMessage(self->compositor_, fmt, fore, back, EditBackBrush(self->edit_brush_),
                               hwnd, msg, wparam, lparam, ignored);
        RemovePropW(hwnd, L"Pulse.EditCue");
        RemovePropW(hwnd, L"Pulse.EditCueColor");
        RemoveWindowSubclass(hwnd, EditProc, kPopupEditSubclass);
        return DefSubclassProc(hwnd, msg, wparam, lparam);
    }
    default:
        break;
    }
    LRESULT result = 0;
    if (!HandleChildEditMessage(self->compositor_, fmt, fore, back, EditBackBrush(self->edit_brush_),
                                hwnd, msg, wparam, lparam, result))
        result = DefPresentedChildEditProc(self->compositor_, fmt, fore, back, hwnd, msg, wparam, lparam);
    return result;
}

LRESULT CALLBACK GitBranchPopup::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    GitBranchPopup* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<GitBranchPopup*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<GitBranchPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT GitBranchPopup::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
        if (!compositor_.Init(hwnd_)) return -1;
        compositor_.SetSyncInterval(0);
        compositor_.RecreateTextFormats(scale_);
        painter_.SetCompositor(&compositor_);
        painter_.SetScale(scale_);
        RecreateFormats();
        edit_brush_ = CreateSolidBrush(EditBackColor(dark_));
        CreateEdit();
        return 0;
    case WM_SIZE:
        Layout();
        LayoutEdit();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            const HWND activated = reinterpret_cast<HWND>(lparam);
            if (activated != hwnd_ && activated != search_edit_) Hide();
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        Render();
        EndPaint(hwnd_, &ps);
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
            RebuildRows();
            Layout();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case kMsgOverviewDone:
        AdoptLoad();
        return 0;
    case WM_MOUSEMOVE: {
        const float x = static_cast<float>(GET_X_LPARAM(lparam));
        const float y = static_cast<float>(GET_Y_LPARAM(lparam));
        if (drag_bar_) {
            fluent::ScrollbarSpec bar{list_rc_, scroll_y_, list_rc_.bottom - list_rc_.top, list_content_h_};
            const D2D1_RECT_F thumb = fluent::ScrollbarThumbRect(bar, scale_, HighContrastOn());
            const float max_offset = (std::max)(0.0f, list_content_h_ - (list_rc_.bottom - list_rc_.top));
            ScrollTo(fluent::ScrollbarDragOffset(list_rc_.top, list_rc_.bottom - list_rc_.top,
                                                 thumb.bottom - thumb.top, max_offset, y, drag_grab_));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (!tracking_leave_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            tracking_leave_ = TrackMouseEvent(&tme) != FALSE;
        }
        const Hit h = HitTest(x, y);
        if (h.kind != hover_.kind || h.index != hover_.index) {
            hover_ = h;
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
    case WM_LBUTTONDOWN: {
        const float x = static_cast<float>(GET_X_LPARAM(lparam));
        const float y = static_cast<float>(GET_Y_LPARAM(lparam));
        const Hit h = HitTest(x, y);
        if (h.kind == HitKind::ListBar || h.kind == HitKind::SearchField) {
            OnClick(h, x, y);
            return 0;
        }
        pressed_ = h;
        SetCapture(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (drag_bar_) {
            drag_bar_ = false;
            ReleaseCapture();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        const float x = static_cast<float>(GET_X_LPARAM(lparam));
        const float y = static_cast<float>(GET_Y_LPARAM(lparam));
        const Hit pressed = pressed_;
        pressed_ = Hit{};
        if (GetCapture() == hwnd_) ReleaseCapture();
        const Hit h = HitTest(x, y);
        if (pressed.kind != HitKind::None && h.kind == pressed.kind && h.index == pressed.index)
            OnClick(h, x, y);
        if (IsOpen()) InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_CAPTURECHANGED:
        drag_bar_ = false;
        return 0;
    case WM_MOUSEWHEEL: {
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        ScrollTo(scroll_y_ - steps * 36.0f * scale_);
        Layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            Hide();
            return 0;
        }
        break;
    case WM_DESTROY:
        if (search_edit_) {
            DestroyWindow(search_edit_);
            search_edit_ = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

}  // namespace pulse::ui

#include "../common/volume_path.h"
#include "edit_host.h"
#include "FluentTokens.h"
#include "legacy_icons.h"
#include "command_icons.h"
#include "../common/windows_compat.h"
#include "quick_preview_window.h"
#include "text_contrast_scope.h"
#include "../app/folder_alias.h"
#include "preview_notice.h"
#include "../common/file_placeholder.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../ipc/preview_protocol.h"
#include "../ops/clipboard.h"
#include "quick_preview_sizing.h"
#include "fluent_components.h"
#include "lumatext_renderer.h"
#include "syntax_highlight.h"
#include "typography.h"
#include "ui_motion.h"
#include <dwmapi.h>
#include <shellapi.h>

#include <commctrl.h>
#include <d2d1helper.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwctype>

#pragma comment(lib, "uxtheme.lib")

namespace pulse::ui {
namespace {

// 别名: the title names the item the way the list does — the alias, then the
// real name under 别名 + 原名.
template <class Item>
std::wstring PreviewTitle(const Item& item) {
    const std::wstring alias = item.path.empty() ? std::wstring() : app::AliasStore::Instance().Shown(item.path);
    if (alias.empty()) return item.name;
    if (app::AliasStore::Instance().Display() != app::AliasDisplay::Both || item.name.empty()) return alias;
    return alias + L"\u2003" + item.name;
}
// The folder / archive card's own heading: the alias alone.
template <class Item>
std::wstring PreviewCardName(const Item& item) {
    const std::wstring alias = item.path.empty() ? std::wstring() : app::AliasStore::Instance().Shown(item.path);
    return alias.empty() ? item.name : alias;
}

constexpr wchar_t kWindowClass[] = L"Pulse.QuickPreview";
constexpr float kCloseGlyphSize = 16.0f * 0.66f;
constexpr float kFindBarHeight = 44.0f;
// Rollback switch: false shows Markdown as highlighted source only (no host
// rich-text request, no Rendered | Source pill).
constexpr bool kMarkdownRender = true;
// Rollback switch: false shows CSV as text and XLSX through the system preview
// handler instead of the table grid.
constexpr bool kTableView = true;
// Rollback switch: false shows JSON/XML as highlighted source (no tree view).
constexpr bool kTreeView = true;
// Rollback switch: false leaves DOCX to the system preview handler and EPUB
// unpreviewed (no host document conversion).
constexpr bool kDocView = true;
// Rollback switch: false keeps animated WebP / APNG still, .ico/.cur without
// size pills and HEIF / AVIF without the codec card.
constexpr bool kImageExtras = true;
constexpr UINT_PTR kTableTipTimer = 73;
constexpr UINT_PTR kAnimationTimer = 7;
constexpr UINT_PTR kPreviewDeadlineTimer = 31;
constexpr UINT_PTR kFindEditCaretTimer = 71;
constexpr UINT_PTR kZoomCloseTimer = 72;
constexpr UINT_PTR kNavSlideTimer = 74;  // the next item slides in (motion phase 6)
constexpr UINT_PTR kFitTimer = 75;       // 适应内容 once the first frame tells the size
constexpr double kZoomOpenSeconds = 0.22;
constexpr double kZoomCloseSeconds = 0.15;
constexpr float kMaxDecodedZoom = 4.0f;
constexpr size_t kFindQueryLimit = 256;
constexpr wchar_t kSearchGlyph[] = L"\xE721";
constexpr wchar_t kCopyGlyph[] = L"\xE8C8";
constexpr wchar_t kSelectAllGlyph[] = L"\xE8B3";
constexpr wchar_t kPrevGlyph[] = L"\xE72B";
constexpr wchar_t kNextGlyph[] = L"\xE72A";
constexpr wchar_t kMoreGlyph[] = L"\xE712";
constexpr wchar_t kOpenGlyph[] = L"\xE8E5";
constexpr wchar_t kCutGlyph[] = L"\xE8C6";
constexpr wchar_t kLinkGlyph[] = L"\xE71B";
constexpr wchar_t kStarGlyph[] = L"\xE734";
constexpr wchar_t kStarFilledGlyph[] = L"\xE735";
constexpr wchar_t kRenameGlyph[] = L"\xE8AC";
constexpr wchar_t kDeleteGlyph[] = L"\xE74D";
constexpr wchar_t kPropertiesGlyph[] = L"\xE946";

enum {
    kTextCmdCopy = 1,
    kTextCmdSelectAll,
    kTextCmdFind,
    kTextCmdLineNumbers,
    kArchiveCmdCopyPath,
    kViewCmdFirst,       // + 0 / 1: the header's view toggle, folded into the menu
    kViewCmdSecond,
    kSizeCmdFirst = 60,  // + PreviewSizeMode: the header's window size menu
    kSizeCmdRemember = 70,
    kFileCmdBase = 100,  // + static_cast<int>(QuickPreviewAction)
};

std::wstring ExtensionLabel(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    std::wstring ext = path.substr(dot + 1);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towupper(c));
    return ext;
}

// 1: CSV family drawn as a grid; 2: workbook (grid or system preview handler).
int TableFileKind(const std::wstring& path) {
    const std::wstring ext = ExtensionLabel(path);
    if (ext == L"CSV" || ext == L"TSV" || ext == L"TAB" || ext == L"PSV") return 1;
    if (ext == L"XLSX" || ext == L"XLSM") return 2;
    return 0;
}

bool IsIconFile(const std::wstring& path) {
    const std::wstring ext = ExtensionLabel(path);
    return ext == L"ICO" || ext == L"CUR";
}

// Word documents the host converts to the block payload ("Document | System").
bool IsDocxFile(const std::wstring& path) {
    const std::wstring ext = ExtensionLabel(path);
    return ext == L"DOCX" || ext == L"DOCM" || ext == L"DOTX";
}

// 12,480: digits grouped for the status pill.
std::wstring GroupedNumber(uint32_t value) {
    std::wstring digits = std::to_wstring(value);
    for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3) digits.insert(static_cast<size_t>(at), 1, L',');
    return digits;
}

// Themed brush color; high contrast is applied where the colors are used.
COLORREF FindEditBrushColor(bool dark) noexcept {
    return dark ? RGB(30, 30, 30) : RGB(255, 255, 255);
}

D2D1_COLOR_F FindEditBg(bool dark) noexcept { return ColorFromRef(EditBackColor(dark)); }
D2D1_COLOR_F FindEditFg(bool dark) noexcept { return ColorFromRef(EditTextColor(dark)); }

void FillHitRange(ID2D1DeviceContext* dc, IDWriteTextLayout* layout,
                  uint32_t start, uint32_t length, float origin_x, float origin_y,
                  ID2D1SolidColorBrush* brush) {
    if (!dc || !layout || !brush || length == 0) return;
    DWRITE_HIT_TEST_METRICS metrics[96];
    UINT32 actual = 0;
    if (FAILED(layout->HitTestTextRange(start, length, origin_x, origin_y,
                                        metrics, ARRAYSIZE(metrics), &actual))) return;
    const UINT32 count = (std::min)(actual, static_cast<UINT32>(ARRAYSIZE(metrics)));
    for (UINT32 i = 0; i < count; ++i) {
        dc->FillRectangle(D2D1::RectF(metrics[i].left, metrics[i].top,
                                      metrics[i].left + metrics[i].width,
                                      metrics[i].top + metrics[i].height), brush);
    }
}

} // namespace

void QuickPreviewWindow::SetHandlerCovered(bool covered) {
    handler_covered_ = covered;
    handler_.SetCovered(covered || embedded_occluded_);
}

QuickPreviewWindow::~QuickPreviewWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
}

bool QuickPreviewWindow::Initialize(HWND owner, UINT navigate_message, UINT open_message,
                                    UINT command_message) {
    if (hwnd_) return true;
    owner_ = owner;
    navigate_message_ = navigate_message;
    open_message_ = open_message;
    command_message_ = command_message;
    thumbnails_.SetQuickLookContent(kMarkdownRender || kTableView || kTreeView || kDocView);
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.style = CS_DBLCLKS;
    window_class.lpfnWndProc = WndProc;
    window_class.lpszClassName = kWindowClass;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = static_cast<HICON>(LoadImageW(window_class.hInstance,
        MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    RegisterClassExW(&window_class);
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
        kWindowClass, pulse::l10n::Get(pulse::l10n::StringId::QuickPreview).c_str(),
        embedded_ ? (WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS) : (WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_CLIPCHILDREN),
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 680, owner, nullptr, window_class.hInstance, this);
    return hwnd_ != nullptr;
}

bool QuickPreviewWindow::InitializeEmbedded(HWND owner, UINT navigate_message, UINT open_message, UINT command_message) {
    embedded_ = true;
    return Initialize(owner, navigate_message, open_message, command_message);
}

void QuickPreviewWindow::SetEmbeddedBounds(RECT bounds) {
    if (!hwnd_ || !embedded_) return;
    RECT current{};
    GetWindowRect(hwnd_, &current);
    MapWindowPoints(nullptr, owner_, reinterpret_cast<POINT*>(&current), 2);
    if (EqualRect(&current, &bounds)) return;
    SetWindowPos(hwnd_, nullptr, bounds.left, bounds.top,
        (std::max)(1L, bounds.right - bounds.left), (std::max)(1L, bounds.bottom - bounds.top),
        SWP_NOACTIVATE | SWP_NOZORDER);
}

void QuickPreviewWindow::ShowEmbedded(const QuickPreviewItem& item, RECT bounds, bool dark, bool safe_mode) {
    SetEmbeddedBounds(bounds);
    Show(item, dark, WindowEffect::None, safe_mode, nullptr, false);
}

void QuickPreviewWindow::ResetTextState() {
    std::wstring{}.swap(preview_text_);
    preview_text_identity_.Reset();
    plain_highlight_lines_.Reset();
    ResetTextDrawingCache();
    text_layout_.reset();
    text_layout_width_ = 0.0f;
    text_layout_scale_ = 0.0f;
    text_layout_hex_ = false;
    hex_hover_ = hex_selected_ = -1;
    sel_anchor_ = sel_focus_ = 0;
    selecting_ = false;
    CloseFind();
    decltype(find_matches_){}.swap(find_matches_);
}

void QuickPreviewWindow::ResetView() {
    native_preview_.Reset();
    if (hwnd_) KillTimer(hwnd_, kPreviewDeadlineTimer);
    preview_deadline_ = 0;
    thumbnails_.TakeRetryDeadline();
    sheet_request_ = 0;
    preview_notice_.clear();
    preview_notice_height_ = 0;
    ResetPlayback();
    text_scroll_ = 0.0f;
    pan_x_ = 0.0f;
    pan_y_ = 0.0f;
    image_fit_ = true;
    image_zoom_ = 1.0f;
    image_turns_ = 0;
    decoded_w_ = decoded_h_ = 0;
    source_w_ = source_h_ = 0;
    native_kind_ = NativeKind::None;
    archive_.Clear();
    markdown_.Clear();
    table_.Clear();
    tree_.Clear();
    panning_ = false;
    waveform_.Reset();
    audio_wave_rect_ = {};
    ResetPages();
    ResetTextState();
}

void QuickPreviewWindow::ResetAnimation() {
    CancelPlaybackScrub();
    if (hwnd_) KillTimer(hwnd_, kAnimationTimer);
    frame_index_ = 0; requested_frame_ = 0; frame_count_ = 1; frame_delay_ms_ = 0;
    loop_count_ = 0; completed_loops_ = 0;
    animation_active_ = false; waiting_for_frame_ = false; animation_started_ = false;
}

void QuickPreviewWindow::RecreateFormats() {
    close_format_.reset();
    preview_text_format_.reset();
    reading_text_format_.reset();
    plain_highlight_lines_.Reset();
    ResetTextDrawingCache();
    text_layout_.reset();
    if (!compositor_.DwriteFactory()) return;
    typography::CreateTextFormat(compositor_.DwriteFactory(),
        {typography::FontRole::Icon, kCloseGlyphSize * scale_}, &close_format_);
    if (close_format_.get()) {
        close_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        close_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    typography::CreateTextFormat(compositor_.DwriteFactory(),
        {typography::FontRole::Monospace, 13.0f * scale_}, &preview_text_format_);
    if (preview_text_format_.get()) {
        preview_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        preview_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        preview_text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    typography::CreateTextFormat(compositor_.DwriteFactory(),
        {typography::FontRole::Text, 15.0f * scale_}, &reading_text_format_);
    if (reading_text_format_.get()) {
        const float line = 15.0f * 1.75f * scale_;
        reading_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        reading_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        reading_text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        reading_text_format_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line, line * 0.78f);
    }
    badge_format_.reset();
    badge_small_format_.reset();
    open_format_.reset();
    typography::CreateTextFormat(compositor_.DwriteFactory(),
        {typography::FontRole::Text, 9.0f * scale_, DWRITE_FONT_WEIGHT_BOLD}, &badge_format_);
    typography::CreateTextFormat(compositor_.DwriteFactory(),
        {typography::FontRole::Text, 7.5f * scale_, DWRITE_FONT_WEIGHT_BOLD}, &badge_small_format_);
    typography::CreateTextFormat(compositor_.DwriteFactory(),
        {typography::FontRole::Text, 12.5f * scale_, DWRITE_FONT_WEIGHT_SEMI_BOLD}, &open_format_);
    gutter_format_.reset();
    typography::CreateTextFormat(compositor_.DwriteFactory(),
        {typography::FontRole::Monospace, 13.0f * scale_}, &gutter_format_);
    if (gutter_format_.get()) {
        gutter_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        gutter_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        gutter_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    find_painter_.SetCompositor(&compositor_);
    find_painter_.SetScale(scale_);
    if (find_edit_font_) {
        DeleteObject(find_edit_font_);
        find_edit_font_ = nullptr;
    }
    find_edit_font_ = typography::CreateEditFont(scale_);
    if (find_edit_ && find_edit_font_)
        SendMessageW(find_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(find_edit_font_), TRUE);
    if (find_edit_brush_) {
        DeleteObject(find_edit_brush_);
        find_edit_brush_ = nullptr;
    }
}

void QuickPreviewWindow::Show(const QuickPreviewItem& item, bool dark, WindowEffect effect,
                              bool safe_mode, const POINT* zoom_from, bool activate) {
    if (path::OnAnyVolume(item.path, suspended_volumes_)) return;
    if (!hwnd_ || item.path.empty()) return;
    if (closing_) {
        // Re-opened while the close zoom is still running.
        KillTimer(hwnd_, kZoomCloseTimer);
        closing_ = false;
        compositor_.ClearZoom();
    }
    if (embedded_) embedded_open_ = true;
    item_ = item;
    title_ = PreviewTitle(item_);
    open_app_cache_.clear();   // associations may have changed since the last preview
    RefreshItemFacts();
    dark_ = dark;
    commands_.Clear();
    effect_ = effect;
    safe_mode_ = safe_mode;
    handler_immediate_ = true;
    if (!embedded_) ApplyWindowEffect(hwnd_, effect_, dark_);
    if (find_edit_brush_) {
        DeleteObject(find_edit_brush_);
        find_edit_brush_ = nullptr;
    }
    ++generation_;
    ResetView();
    ResetAnimation();
    icon_request_ = 0; icon_sizes_.clear(); icon_selected_ = -1; icon_pill_rects_.clear();
    BeginVideo();
    if (!activate && !embedded_ && video_.active()) video_.SetMuted(true);   // a peek is silent
    if (embedded_) {
        Render();
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    StartKeyHint();
    RECT owner_rect{};
    GetWindowRect(owner_, &owner_rect);
    // Size: Settings > 快速预览 > 预览窗口 (a remembered drag wins), centred
    // on Pulse's window.
    LeaveFullScreen(false);
    user_sized_ = false;
    fit_pending_ = false;
    KillTimer(hwnd_, kFitTimer);
    const POINT owner_centre{(owner_rect.left + owner_rect.right) / 2, (owner_rect.top + owner_rect.bottom) / 2};
    const bool full_screen = options_.size_mode == static_cast<int>(PreviewSizeMode::FullScreen);
    const RECT placed = PlanRect(full_screen ? static_cast<int>(PreviewSizeMode::Fit) : options_.size_mode,
                                 owner_centre, true);
    const int x = placed.left, y = placed.top;
    const int width = (std::max)(1L, placed.right - placed.left);
    const int height = (std::max)(1L, placed.bottom - placed.top);
    SetWindowTextW(hwnd_, item_.name.empty()
        ? pulse::l10n::Get(pulse::l10n::StringId::QuickPreview).c_str() : PreviewTitle(item_).c_str());
    // Zoom out of the item's icon. Only with a plain (None) window effect:
    // a DWM system backdrop would pop in at full size around the zooming
    // content. The DWM show transition is disabled while our zoom runs.
    // The icon zoom read as two layers (DWM frame + zooming content); the
    // user preferred the plain system show/hide animation, so it stays off.
    (void)zoom_from;
    zoom_enabled_ = false;
    const BOOL no_dwm_transition = zoom_enabled_ ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED, &no_dwm_transition,
                          sizeof(no_dwm_transition));
    zoom_pending_ = zoom_enabled_ && compositor_.HideContent();
    if (zoom_pending_) zoom_origin_ = POINT{zoom_from->x - x, zoom_from->y - y};
    else compositor_.ClearZoom();
    // Size and draw the first frame while still hidden so the system show
    // animation starts from real content instead of an empty (black) window.
    const bool was_visible = IsWindowVisible(hwnd_) != FALSE;
    SetWindowPos(hwnd_, HWND_TOP, x, y, width, height,
                 was_visible ? 0u : static_cast<UINT>(SWP_NOACTIVATE));
    if (full_screen) EnterFullScreen();
    if (!was_visible) Render();
    ShowWindow(hwnd_, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    if (activate) SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// Prev / next: remember the direction so the item that answers slides in
// from that side (spring Smooth).
// A peek pinned with Space: it takes the keyboard and its sound back.
void QuickPreviewWindow::Activate() {
    if (!visible()) return;
    if (video_.active()) video_.SetMuted(playback_muted_);
    SetForegroundWindow(hwnd_);
}

void QuickPreviewWindow::PostNavigate(int dir) {
    if (!owner_ || !navigate_message_) return;
    pending_nav_dir_ = dir;
    pending_nav_at_ = motion::NowMs();
    PostMessageW(owner_, navigate_message_, static_cast<WPARAM>(static_cast<INT_PTR>(dir)), 0);
}

void QuickPreviewWindow::Update(const QuickPreviewItem& item) {
    if (path::OnAnyVolume(item.path, suspended_volumes_)) return;
    if (!visible() || item.path.empty()) return;
    const uint64_t nav_now = motion::NowMs();
    nav_dir_ = 0;
    if (pending_nav_dir_ != 0 && item.path != item_.path && nav_now - pending_nav_at_ < 600 &&
        motion::SystemAnimationsEnabled()) {
        nav_dir_ = pending_nav_dir_;
        nav_start_ = nav_now;
        SetTimer(hwnd_, kNavSlideTimer, 15, nullptr);
    }
    pending_nav_dir_ = 0;
    commands_.Clear();
    const bool same = item.path == item_.path;
    const int position = item_.position, count = item_.count;
    item_ = item;
    if (same && item_.count == 0) {   // a refresh of the shown item keeps its place
        item_.position = position;
        item_.count = count;
    }
    title_ = PreviewTitle(item_);
    RefreshItemFacts();
    ++generation_;
    handler_immediate_ = false;
    ResetView();
    ResetAnimation();
    icon_request_ = 0; icon_sizes_.clear(); icon_selected_ = -1; icon_pill_rects_.clear();
    handler_.Reset();
    BeginVideo();
    if (!same) StartKeyHint();
    // 适应内容 follows each item's shape around the window's centre, unless
    // the window was sized by hand, maximized or is full screen.
    fit_pending_ = false;
    KillTimer(hwnd_, kFitTimer);
    const RECT& kept = options_.remembered;
    const bool remembered = options_.remember_size && kept.right > kept.left && kept.bottom > kept.top;
    if (!same && !fullscreen_ && !user_sized_ && !remembered && !IsZoomed(hwnd_) &&
        options_.size_mode == static_cast<int>(PreviewSizeMode::Fit)) {
        const RECT next = PlanRect(options_.size_mode, WindowCentre(), false);
        if (!fit_pending_) ApplyRect(next);   // pending: keep the size until the first frame
    }
    SetWindowTextW(hwnd_, item_.name.empty()
        ? pulse::l10n::Get(pulse::l10n::StringId::QuickPreview).c_str() : PreviewTitle(item_).c_str());
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::SetStarred(bool starred) {
    if (item_.starred == starred) return;
    item_.starred = starred;
}

void QuickPreviewWindow::Close() {
    if (embedded_ && !embedded_open_) return;
    embedded_open_ = false;
    if (!hwnd_ || closing_) return;
    commands_.Clear();
    fit_pending_ = false;
    KillTimer(hwnd_, kFitTimer);
    LeaveFullScreen(false);   // the next Show places the window again
    handler_.Hide();
    ResetAnimation();
    ResetView();
    thumbnails_.Evict();
    preview_pixels_ = 0;
    close_hover_ = false;
    chrome_hover_ = ChromeButton::None;
    zoom_pending_ = false;
    // Shrink back into the item, then hide. Focus returns to the owner right
    // away so keyboard input never waits for the animation.
    if (zoom_enabled_ && IsWindowVisible(hwnd_) &&
        compositor_.PlayZoom(static_cast<float>(zoom_origin_.x),
                             static_cast<float>(zoom_origin_.y), false, kZoomCloseSeconds)) {
        closing_ = true;
        SetTimer(hwnd_, kZoomCloseTimer,
                 static_cast<UINT>(kZoomCloseSeconds * 1000.0) + 20, nullptr);
        if (owner_) SetForegroundWindow(owner_);
        return;
    }
    FinishClose(!embedded_);
}

// restore_focus is false after the close zoom: focus already went back to the
// owner when the close started, and the user may have switched apps since.
void QuickPreviewWindow::FinishClose(bool restore_focus) {
    closing_ = false;
    if (hwnd_) {
        KillTimer(hwnd_, kZoomCloseTimer);
        ShowWindow(hwnd_, SW_HIDE);
    }
    compositor_.ClearZoom();
    if (restore_focus && owner_) SetForegroundWindow(owner_);
}

bool QuickPreviewWindow::visible() const noexcept {
    return hwnd_ && !closing_ && IsWindowVisible(hwnd_) != FALSE;
}

bool QuickPreviewWindow::OfflinePlaceholder() const noexcept {
    return files::PreviewWouldRecall(item_.attrs);   // the preview host's rule
}

void QuickPreviewWindow::Resize() {
    if (!hwnd_ || !compositor_.Dc()) return;
    RECT rect{};
    GetClientRect(hwnd_, &rect);
    const int width = (std::max)(1L, rect.right);
    const int height = (std::max)(1L, rect.bottom);
    // A playback tick redraws the chrome; it must not discard and recreate the
    // presentation buffers when the window has not actually changed size.
    if (width != compositor_.Width() || height != compositor_.Height())
        compositor_.Resize(width, height);
}

float QuickPreviewWindow::FindBarHeight() const noexcept {
    return find_open_ && (native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex ||
                          native_kind_ == NativeKind::Archive || native_kind_ == NativeKind::Markdown ||
                          native_kind_ == NativeKind::Table || native_kind_ == NativeKind::Tree)
        ? kFindBarHeight * scale_ : 0.0f;
}

D2D1_RECT_F QuickPreviewWindow::FindBarRect() const {
    const float width = static_cast<float>(compositor_.Width());
    const float header = HeaderHeight();
    return D2D1::RectF(0, header, width, header + kFindBarHeight * scale_);
}

D2D1_RECT_F QuickPreviewWindow::FindFieldRect() const {
    const D2D1_RECT_F bar = FindBarRect();
    const float pad_x = 12.0f * scale_;
    const float pad_y = 6.0f * scale_;
    return D2D1::RectF(bar.left + pad_x, bar.top + pad_y, bar.right - pad_x, bar.bottom - pad_y);
}

D2D1_RECT_F QuickPreviewWindow::FindEditCell() const {
    const D2D1_RECT_F field = FindFieldRect();
    const float inset = 10.0f * scale_;
    const float icon = 18.0f * scale_;
    const float gap = 6.0f * scale_;
    const float trail = find_query_.empty() ? inset : 72.0f * scale_;
    return D2D1::RectF(field.left + inset + icon + gap, field.top,
                       field.right - trail, field.bottom);
}

D2D1_RECT_F QuickPreviewWindow::ContentRect() const {
    const float width = static_cast<float>(compositor_.Width());
    const float height = static_cast<float>(compositor_.Height());
    const float header = HeaderHeight();
    return D2D1::RectF(0, header + FindBarHeight() + preview_notice_height_, width - InfoDrawerWidth(),
                       (std::max)(header + FindBarHeight() + preview_notice_height_, height - FooterHeight() - PlaybackHeight()));
}

D2D1_RECT_F QuickPreviewWindow::ChromeButtonRect(ChromeButton button) const {
    switch (button) {
    case ChromeButton::Prev: return header_.prev;
    case ChromeButton::Next: return header_.next;
    case ChromeButton::More: return header_.more;
    case ChromeButton::Open: return header_.open;
    case ChromeButton::Size: return header_.size;
    default: return D2D1::RectF(0, 0, 0, 0);
    }
}

// The view toggle in the header's tool area (LayoutHeader places it).
D2D1_RECT_F QuickPreviewWindow::MarkdownToggleRect(int segment) const {
    const D2D1_RECT_F pill = header_.tools;
    if (segment < 0) return pill;
    const float inner = 2.0f * scale_;
    const float w0 = ToggleSegmentWidth(0);
    if (segment == 0)
        return D2D1::RectF(pill.left + inner, pill.top + inner, pill.left + inner + w0, pill.bottom - inner);
    return D2D1::RectF(pill.left + inner + w0, pill.top + inner, pill.right - inner, pill.bottom - inner);
}

bool& QuickPreviewWindow::ToggleSecond() noexcept {
    switch (toggle_kind_) {
    case ToggleKind::TableSource: return table_source_;
    case ToggleKind::TableHandler: return table_handler_;
    case ToggleKind::TreeSource: return tree_source_;
    case ToggleKind::NotebookSource: return notebook_source_;
    case ToggleKind::DocHandler: return doc_handler_;
    case ToggleKind::TextReading:
        return ExtensionLabel(item_.path) == L"TXT" ? text_mono_txt_ : text_mono_other_;
    default: return markdown_source_;
    }
}

int QuickPreviewWindow::HitMarkdownToggle(POINT client) const {
    if (!markdown_shown_ || !header_.tools_visible) return -1;
    const D2D1_RECT_F pill = MarkdownToggleRect(-1);
    const float x = static_cast<float>(client.x), y = static_cast<float>(client.y);
    if (x < pill.left || x >= pill.right || y < pill.top || y >= pill.bottom) return -1;
    return x < MarkdownToggleRect(0).right ? 0 : 1;
}

void QuickPreviewWindow::DrawMarkdownToggle(ID2D1DeviceContext* dc,
                                            ID2D1SolidColorBrush* text_brush) {
    IDWriteTextFormat* format = compositor_.SmallFormat();
    if (!dc || !text_brush || !format) return;
    const wchar_t* labels[2]{};
    ToggleLabels(labels);
    D2D1_COLOR_F color = text_brush->GetColor();
    ComPtr<ID2D1SolidColorBrush> track, thumb, dim;
    color.a = dark_ ? 0.08f : 0.06f;
    dc->CreateSolidColorBrush(color, &track);
    color.a = 0.62f;
    dc->CreateSolidColorBrush(color, &dim);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0xFFFFFF, 0.14f) : D2D1::ColorF(0xFFFFFF, 1.0f),
                              &thumb);
    const D2D1_RECT_F pill = MarkdownToggleRect(-1);
    const float radius = (pill.bottom - pill.top) * 0.5f;
    if (track.get()) dc->FillRoundedRectangle(D2D1::RoundedRect(pill, radius, radius), track.get());
    const int active = ToggleSecond() ? 1 : 0;
    const D2D1_RECT_F knob = MarkdownToggleRect(active);
    const float knob_radius = (knob.bottom - knob.top) * 0.5f;
    if (thumb.get()) dc->FillRoundedRectangle(D2D1::RoundedRect(knob, knob_radius, knob_radius), thumb.get());
    if (!dark_ && track.get())
        dc->DrawRoundedRectangle(D2D1::RoundedRect(knob, knob_radius, knob_radius), track.get(), 1.0f);
    for (int i = 0; i < 2; ++i) {
        ID2D1SolidColorBrush* brush = i == active || !dim.get() ? text_brush : dim.get();
        DrawUiText(dc, labels[i], format, MarkdownToggleRect(i), brush->GetColor(),
                   DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

QuickPreviewWindow::ChromeButton QuickPreviewWindow::HitChromeButton(POINT client) const {
    const float x = static_cast<float>(client.x);
    const float y = static_cast<float>(client.y);
    if (fullscreen_) return ChromeButton::None;
    for (ChromeButton button : {ChromeButton::Prev, ChromeButton::Next, ChromeButton::Open,
                                ChromeButton::Size, ChromeButton::More}) {
        const D2D1_RECT_F rect = ChromeButtonRect(button);
        if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom) return button;
    }
    return ChromeButton::None;
}

void QuickPreviewWindow::ActivateChromeButton(ChromeButton button) {
    switch (button) {
    case ChromeButton::Prev:
        PostNavigate(-1);
        break;
    case ChromeButton::Next:
        PostNavigate(1);
        break;
    case ChromeButton::Open:
        // Folders open in a new tab; the list behind keeps its place.
        if (command_message_)
            PostAction((item_.attrs & FILE_ATTRIBUTE_DIRECTORY) ? QuickPreviewAction::OpenInNewTab
                                                                : QuickPreviewAction::Open, &item_);
        else if (owner_ && open_message_) PostMessageW(owner_, open_message_, 0, 0);
        break;
    case ChromeButton::More: {
        const D2D1_RECT_F rect = ChromeButtonRect(ChromeButton::More);
        POINT screen{static_cast<LONG>(rect.left), static_cast<LONG>(rect.bottom)};
        ClientToScreen(hwnd_, &screen);
        ShowContextMenu(screen);
        break;
    }
    case ChromeButton::Size: {
        const D2D1_RECT_F rect = ChromeButtonRect(ChromeButton::Size);
        POINT screen{static_cast<LONG>(rect.left), static_cast<LONG>(rect.bottom)};
        ClientToScreen(hwnd_, &screen);
        ShowSizeMenu(screen);
        break;
    }
    default:
        break;
    }
}

// ---- Window size presets ---------------------------------------------------

POINT QuickPreviewWindow::WindowCentre() const {
    RECT now{};
    GetWindowRect(hwnd_, &now);
    return POINT{(now.left + now.right) / 2, (now.top + now.bottom) / 2};
}

int QuickPreviewWindow::SizingChromeHeight() const {
    // Videos and animated pictures both carry the transport bar under the picture.
    const bool bar = (video_.active() && !VideoPreview::IsAudio(item_.path)) || frame_count_ > 1;
    const float playback = bar ? PlaybackHeight() : 0.0f;
    return static_cast<int>(std::lround(HeaderHeight() + FooterHeight() + playback));
}

bool QuickPreviewWindow::ProbeContentSize(uint32_t& width, uint32_t& height) const {
    width = height = 0;
    if (!image_item_ || item_.path.empty() || (item_.attrs & FILE_ATTRIBUTE_DIRECTORY) ||
        OfflinePlaceholder()) return false;
    // Network files can stall the UI thread: their first frame sizes them.
    std::wstring path = item_.path;
    if (path.rfind(L"\\\\?\\", 0) == 0) path.erase(0, 4);
    if (path.rfind(L"\\\\", 0) == 0 || path.rfind(L"UNC\\", 0) == 0) return false;
    if (path.size() >= 2 && path[1] == L':') {
        const wchar_t root[4] = {path[0], L':', L'\\', 0};
        if (GetDriveTypeW(root) == DRIVE_REMOTE) return false;
    }
    HANDLE file = CreateFileW(item_.path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::vector<uint8_t> head(128 * 1024);   // JPEG EXIF blocks come before the frame header
    DWORD read = 0;
    const BOOL ok = ReadFile(file, head.data(), static_cast<DWORD>(head.size()), &read, nullptr);
    CloseHandle(file);
    return ok && ProbeImageSize(head.data(), read, width, height);
}

bool QuickPreviewWindow::ContentPixelSize(uint32_t& width, uint32_t& height) const {
    if (image_item_ && (source_w_ || decoded_w_)) {
        width = source_w_ ? source_w_ : decoded_w_;
        height = source_w_ ? source_h_ : decoded_h_;
        if (image_turns_ & 1) std::swap(width, height);
        return width && height;
    }
    if (video_.active()) {
        if (VideoPreview::IsAudio(item_.path)) return false;
        const auto state = video_.Snapshot();
        width = state.width;
        height = state.height;
        return width && height;
    }
    return ProbeContentSize(width, height);
}

RECT QuickPreviewWindow::PlanRect(int mode, POINT centre, bool use_remembered) {
    PreviewSizeInput in;
    in.mode = static_cast<PreviewSizeMode>(std::clamp(mode, 0, 4));
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST), &info);
    in.work = info.rcWork;
    in.centre = centre;
    in.chrome_h = SizingChromeHeight();
    in.scale = scale_;
    const RECT& kept = options_.remembered;
    if (use_remembered && options_.remember_size && kept.right > kept.left && kept.bottom > kept.top) {
        in.remembered_w = kept.right - kept.left;
        in.remembered_h = kept.bottom - kept.top;
    } else if (in.mode == PreviewSizeMode::Fit && !(item_.attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        uint32_t w = 0, h = 0;
        if (ContentPixelSize(w, h)) {
            in.content_w = static_cast<int>(w);
            in.content_h = static_cast<int>(h);
        } else if (video_.active() && !VideoPreview::IsAudio(item_.path)) {
            in.content_w = 1920;   // 16:9 until the video reports its size
            in.content_h = 1080;
            fit_pending_ = true;
        } else if (image_item_) {
            fit_pending_ = true;   // 标准 until the first frame
        }
    }
    return PreviewWindowRect(in);
}

void QuickPreviewWindow::ApplyRect(const RECT& rect) {
    if (embedded_) return;
    if (!hwnd_ || rect.right <= rect.left || rect.bottom <= rect.top) return;
    RECT now{};
    GetWindowRect(hwnd_, &now);
    if (EqualRect(&now, &rect)) return;
    SetWindowPos(hwnd_, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void QuickPreviewWindow::FitToContent(uint32_t width, uint32_t height) {
    if (embedded_) return;
    if (!visible() || !width || !height || fullscreen_ || user_sized_ || IsZoomed(hwnd_) ||
        options_.size_mode != static_cast<int>(PreviewSizeMode::Fit)) return;
    PreviewSizeInput in;
    in.mode = PreviewSizeMode::Fit;
    in.centre = WindowCentre();
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromPoint(in.centre, MONITOR_DEFAULTTONEAREST), &info);
    in.work = info.rcWork;
    in.chrome_h = SizingChromeHeight();
    in.scale = scale_;
    in.content_w = static_cast<int>(width);
    in.content_h = static_cast<int>(height);
    ApplyRect(PreviewWindowRect(in));
}

void QuickPreviewWindow::ChooseSizeMode(int mode) {
    mode = std::clamp(mode, 0, 4);
    options_.size_mode = mode;
    options_.remembered = RECT{};   // a preset replaces the dragged size
    user_sized_ = false;
    fit_pending_ = false;
    if (mode == static_cast<int>(PreviewSizeMode::FullScreen)) {
        EnterFullScreen();
    } else {
        LeaveFullScreen(false);
        if (IsZoomed(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        ApplyRect(PlanRect(mode, WindowCentre(), false));
    }
    ReportOptions();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::EnterFullScreen() {
    if (embedded_) return;
    if (!hwnd_ || fullscreen_) return;
    if (IsZoomed(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    GetWindowRect(hwnd_, &pre_fullscreen_);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &info)) return;
    fullscreen_ = true;
    const DWORD corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    const RECT& m = info.rcMonitor;
    SetWindowPos(hwnd_, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (find_open_) LayoutFindEdit();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::LeaveFullScreen(bool restore_rect) {
    if (!hwnd_ || !fullscreen_) return;
    fullscreen_ = false;
    const DWORD corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    if (restore_rect) ApplyRect(pre_fullscreen_);
    if (find_open_) LayoutFindEdit();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::StartKeyHint() {
    key_hint_until_ = 0;
    if (!video_.active() || VideoPreview::IsAudio(item_.path) || !options_.space_closes ||
        options_.key_hint_left <= 0) return;
    key_hint_until_ = GetTickCount64() + 5000;
    --options_.key_hint_left;
    ReportOptions();
}

void QuickPreviewWindow::ReportOptions() {
    if (item_.path.empty()) return;
    const QuickPreviewItem target = item_;
    PostAction(QuickPreviewAction::SaveOptions, &target);
}

void QuickPreviewWindow::ToggleLoop() {
    // The three choices nest (不循环 < 仅动图 < 动图和视频), so L steps along
    // them: a looping video implies looping animations, and vice versa.
    const bool video = video_.active();
    if (video) options_.loop = VideoLoops() ? 0 : 1;
    else options_.loop = AnimationLoops() ? 2 : 0;
    const bool on = video ? VideoLoops() : AnimationLoops();
    playback_note_ = pulse::l10n::Get(on ? pulse::l10n::StringId::PreviewLoopOn
                                         : pulse::l10n::StringId::PreviewLoopOff);
    playback_note_until_ = GetTickCount64() + 1500;
    // Turning it back on wakes an animation that stopped on its last frame.
    if (!video && on && !animation_active_ && frame_count_ > 1 && frame_index_ + 1 >= frame_count_)
        TogglePlayback();
    ReportOptions();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::ShowSizeMenu(POINT screen) {
    if (!text_menu_.Create(hwnd_, &compositor_, scale_)) return;
    text_menu_.SetTheme(dark_, accent_);
    text_menu_.SetTopmost(native_kind_ == NativeKind::None);
    using pulse::l10n::StringId;
    static constexpr StringId kLabels[5] = {StringId::PreviewSizeFit, StringId::PreviewSizeStandard,
        StringId::PreviewSizeLarge, StringId::PreviewSizeMaximized, StringId::PreviewSizeFullScreen};
    static constexpr const wchar_t* kKeys[5] = {L"", L"Ctrl+1", L"Ctrl+2", L"Ctrl+3", L"F"};
    const int current = fullscreen_ ? 4 : std::clamp(options_.size_mode, 0, 4);
    std::vector<FluentMenuItem> items;
    for (int i = 0; i < 5; ++i) {
        FluentMenuItem item;
        item.command = kSizeCmdFirst + i;
        item.text = pulse::l10n::Get(kLabels[i]);
        item.shortcut = kKeys[i];
        item.radio_group = true;
        item.radio = i == current;
        item.separator_after = i == 4;
        items.push_back(std::move(item));
    }
    FluentMenuItem remember;
    remember.command = kSizeCmdRemember;
    remember.text = pulse::l10n::Get(StringId::PreviewRememberSize);
    remember.glyph = L"\xE81C";
    remember.toggle = true;
    remember.checked = options_.remember_size;
    items.push_back(std::move(remember));
    const uint64_t menu_generation = generation_;
    const int cmd = text_menu_.TrackPopup(screen, std::move(items));
    if (!visible() || menu_generation != generation_) return;
    if (cmd >= kSizeCmdFirst && cmd < kSizeCmdFirst + 5) {
        ChooseSizeMode(cmd - kSizeCmdFirst);
    } else if (cmd == kSizeCmdRemember) {
        options_.remember_size = !options_.remember_size;
        if (!options_.remember_size) options_.remembered = RECT{};
        ReportOptions();
    }
}

QuickPreviewItem QuickPreviewWindow::ActionTarget() const {
    if (native_kind_ == NativeKind::Archive && archive_.IsFolderListing())
        return FolderPreviewTarget(item_, archive_.SelectedPath(), archive_.SelectedIsDirectory());
    return item_;
}

void QuickPreviewWindow::PostAction(QuickPreviewAction action, const QuickPreviewItem* target) {
    if (!owner_ || !command_message_ || action == QuickPreviewAction::None) return;
    // An explicit target (header button, menus) is acted on as given.
    if (!target && native_kind_ == NativeKind::Archive &&
        !CanApplyPreviewFileAction(archive_.IsFolderListing(), archive_.HasSelection())) {
        if (action == QuickPreviewAction::CopyPath || action == QuickPreviewAction::Copy)
            pulse::ops::WriteClipboardText(archive_.SelectedPath());
        return;
    }
    QuickPreviewCommand command;
    command.action = action;
    command.target = target ? *target : ActionTarget();
    if (command.target.path.empty()) return;
    command.folder_child = command.target.path != item_.path;
    command.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const UINT_PTR token = commands_.Push(std::move(command));
    if (token && !PostMessageW(owner_, command_message_, static_cast<WPARAM>(action), static_cast<LPARAM>(token))) {
        QuickPreviewCommand ignored;
        commands_.Take(token, ignored);
    }
}

float QuickPreviewWindow::FitScale(float view_w, float view_h) const noexcept {
    if (decoded_w_ == 0 || decoded_h_ == 0) return 1.0f;
    return (std::min)(view_w / TurnedW(),
                      view_h / TurnedH());
}

bool QuickPreviewWindow::CanPanImage() const {
    if (image_fit_ || native_kind_ != NativeKind::Bitmap || decoded_w_ == 0) return false;
    const D2D1_RECT_F content = ContentRect();
    const float view_w = content.right - content.left;
    const float view_h = content.bottom - content.top;
    return TurnedW() * image_zoom_ > view_w + 0.5f ||
           TurnedH() * image_zoom_ > view_h + 0.5f;
}

void QuickPreviewWindow::SetFitMode() {
    image_fit_ = true;
    image_zoom_ = 1.0f;
    pan_x_ = 0.0f;
    pan_y_ = 0.0f;
}

void QuickPreviewWindow::SetActualPixels() {
    image_fit_ = false;
    image_zoom_ = 1.0f;
    pan_x_ = 0.0f;
    pan_y_ = 0.0f;
    const D2D1_RECT_F content = ContentRect();
    ClampPan(content.right - content.left, content.bottom - content.top);
}

void QuickPreviewWindow::ToggleFitActual() {
    if (image_fit_) SetActualPixels();
    else SetFitMode();
}

void QuickPreviewWindow::ClampPan(float view_w, float view_h) {
    if (decoded_w_ == 0 || decoded_h_ == 0 || image_fit_) {
        pan_x_ = 0.0f;
        pan_y_ = 0.0f;
        return;
    }
    const float draw_w = TurnedW() * image_zoom_;
    const float draw_h = TurnedH() * image_zoom_;
    const float max_x = (std::max)(0.0f, (draw_w - view_w) * 0.5f);
    const float max_y = (std::max)(0.0f, (draw_h - view_h) * 0.5f);
    pan_x_ = std::clamp(pan_x_, -max_x, max_x);
    pan_y_ = std::clamp(pan_y_, -max_y, max_y);
}

void QuickPreviewWindow::ZoomAt(float cursor_x, float cursor_y, float factor) {
    if (decoded_w_ == 0 || decoded_h_ == 0) return;
    const D2D1_RECT_F content = ContentRect();
    const float view_w = content.right - content.left;
    const float view_h = content.bottom - content.top;
    const float fit = FitScale(view_w, view_h);
    const float old_zoom = image_fit_ ? fit : image_zoom_;
    const float max_zoom = (std::max)(fit, kMaxDecodedZoom);
    float next = std::clamp(old_zoom * factor, fit, max_zoom);
    if (next <= fit * 1.02f) {
        SetFitMode();
        return;
    }
    const float draw_w = TurnedW() * old_zoom;
    const float draw_h = TurnedH() * old_zoom;
    const float left = content.left + (view_w - draw_w) * 0.5f + pan_x_;
    const float top = content.top + (view_h - draw_h) * 0.5f + pan_y_;
    const float u = (cursor_x - left) / (std::max)(0.001f, old_zoom);
    const float v = (cursor_y - top) / (std::max)(0.001f, old_zoom);
    image_fit_ = false;
    image_zoom_ = next;
    const float new_w = TurnedW() * next;
    const float new_h = TurnedH() * next;
    pan_x_ = cursor_x - u * next - content.left - (view_w - new_w) * 0.5f;
    pan_y_ = cursor_y - v * next - content.top - (view_h - new_h) * 0.5f;
    ClampPan(view_w, view_h);
}

D2D1_RECT_F QuickPreviewWindow::ImageDest(const D2D1_RECT_F& content) const {
    if (image_fit_ || decoded_w_ == 0 || decoded_h_ == 0) return content;
    const float view_w = content.right - content.left;
    const float view_h = content.bottom - content.top;
    const float draw_w = TurnedW() * image_zoom_;
    const float draw_h = TurnedH() * image_zoom_;
    const float left = content.left + (view_w - draw_w) * 0.5f + pan_x_;
    const float top = content.top + (view_h - draw_h) * 0.5f + pan_y_;
    return D2D1::RectF(left, top, left + draw_w, top + draw_h);
}

uint32_t QuickPreviewWindow::RequestedPixelSize(const D2D1_RECT_F& content) const {
    const float longest = (std::max)(content.right - content.left, content.bottom - content.top);
    return ipc::BucketPreviewPixelSize(static_cast<uint32_t>((std::max)(1.0f, longest)));
}

// PULSEICO payload from the host (preview_host/image_frames.h).
void QuickPreviewWindow::ParseIconSizes(const std::wstring& payload) {
    if (payload.rfind(L"PULSEICO\t1\n", 0) != 0) return;
    std::vector<IconSize> sizes;
    int selected = -1;
    size_t pos = payload.find(L'\n') + 1;
    while (pos < payload.size()) {
        size_t end = payload.find(L'\n', pos);
        if (end == std::wstring::npos) end = payload.size();
        const std::wstring line = payload.substr(pos, end - pos);
        pos = end + 1;
        if (line.rfind(L"S\t", 0) == 0) {
            selected = _wtoi(line.c_str() + 2);
        } else if (line.rfind(L"E\t", 0) == 0 && sizes.size() < 64) {
            IconSize size;
            const wchar_t* p = line.c_str() + 2;
            wchar_t* next = nullptr;
            size.width = static_cast<uint32_t>(wcstoul(p, &next, 10));
            if (next && *next == L'\t') size.height = static_cast<uint32_t>(wcstoul(next + 1, &next, 10));
            if (next && *next == L'\t') size.bits = static_cast<uint32_t>(wcstoul(next + 1, &next, 10));
            if (next && *next == L'\t') size.png = next[1] == L'1';
            sizes.push_back(size);
        }
    }
    icon_sizes_ = std::move(sizes);
    icon_selected_ = selected >= 0 && selected < static_cast<int>(icon_sizes_.size()) ? selected : -1;
}

// Size pills centred above the HUD; the shown size is highlighted and names
// its format ("256 · PNG"). Only clicks pick a size: ← / → still change files.
void QuickPreviewWindow::DrawIconSizes(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                                       ID2D1SolidColorBrush* brush) {
    icon_pill_rects_.clear();
    IDWriteTextFormat* format = compositor_.SmallFormat();
    auto* factory = compositor_.DwriteFactory();
    if (!dc || !brush || !format || !factory || icon_sizes_.size() < 1) return;
    const float s = scale_;
    std::vector<std::wstring> labels;
    float total = 0.0f;
    std::vector<float> widths;
    for (size_t i = 0; i < icon_sizes_.size(); ++i) {
        const IconSize& size = icon_sizes_[i];
        std::wstring label = size.width == size.height ? std::to_wstring(size.width)
            : std::to_wstring(size.width) + L"\x00D7" + std::to_wstring(size.height);
        if (static_cast<int>(i) == icon_selected_) {
            if (size.png) label += L" \x00B7 PNG";
            else if (size.bits) label += L" \x00B7 " + std::to_wstring(size.bits) + L"-bit";
        }
        const float w = typography::MeasureAdvance(factory, format, label) + 20.0f * s;
        widths.push_back(w);
        total += w;
        labels.push_back(std::move(label));
    }
    const float gap = 6.0f * s, h = 22.0f * s;
    total += gap * static_cast<float>(labels.size() - 1);
    if (total > content.right - content.left - 24.0f * s) return;  // too narrow: the HUD still names the size
    const float bottom = content.bottom - 12.0f * s;
    float x = std::round((content.left + content.right - total) * 0.5f);
    const float y = std::round(bottom - h);
    ComPtr<ID2D1SolidColorBrush> pill, accent, on_accent;
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x2B2B2B, 0.92f) : D2D1::ColorF(0xF3F3F3, 0.95f), &pill);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x60CDFF) : D2D1::ColorF(0x005FB8), &accent);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x062030) : D2D1::ColorF(0xFFFFFF), &on_accent);
    const auto text_alignment = format->GetTextAlignment();
    const auto paragraph_alignment = format->GetParagraphAlignment();
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    for (size_t i = 0; i < labels.size(); ++i) {
        const D2D1_RECT_F r = D2D1::RectF(x, y, x + widths[i], y + h);
        icon_pill_rects_.push_back(r);
        const bool on = static_cast<int>(i) == icon_selected_;
        ID2D1SolidColorBrush* fill = on ? accent.get() : pill.get();
        if (fill) dc->FillRoundedRectangle(D2D1::RoundedRect(r, h * 0.5f, h * 0.5f), fill);
        ID2D1SolidColorBrush* ink = on && on_accent.get() ? on_accent.get() : brush;
        dc->DrawTextW(labels[i].data(), static_cast<UINT32>(labels[i].size()), format, r, ink,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        x += widths[i] + gap;
    }
    format->SetTextAlignment(text_alignment);
    format->SetParagraphAlignment(paragraph_alignment);
}

int QuickPreviewWindow::HitIconSize(POINT client) const {
    if (native_kind_ != NativeKind::Bitmap) return -1;
    const float x = static_cast<float>(client.x), y = static_cast<float>(client.y);
    for (size_t i = 0; i < icon_pill_rects_.size(); ++i) {
        const D2D1_RECT_F& r = icon_pill_rects_[i];
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return static_cast<int>(i);
    }
    return -1;
}

// Size · format · bytes on the left; the zoom (click: fit / actual pixels) on the right.
void QuickPreviewWindow::SetImageFooter(const D2D1_RECT_F& content) {
    if (native_kind_ != NativeKind::Bitmap || decoded_w_ == 0) return;
    uint32_t shown_w = source_w_ ? source_w_ : decoded_w_;
    uint32_t shown_h = source_h_ ? source_h_ : decoded_h_;
    if (image_turns_ & 1) std::swap(shown_w, shown_h);
    std::vector<std::wstring> parts;
    wchar_t dims[64];
    swprintf_s(dims, L"%u \x00D7 %u", shown_w, shown_h);
    parts.push_back(dims);
    if (source_w_ && source_h_ && (source_w_ != decoded_w_ || source_h_ != decoded_h_)) {
        wchar_t preview[64];
        swprintf_s(preview, pulse::l10n::Get(pulse::l10n::StringId::PreviewDecodedFormat).c_str(),
                   (std::max)(decoded_w_, decoded_h_));
        parts.push_back(preview);
    }
    if (std::wstring ext = ExtensionLabel(item_.path); !ext.empty()) parts.push_back(std::move(ext));
    if (std::wstring size = pulse::format::ByteSize(item_.size, true); !size.empty())
        parts.push_back(std::move(size));
    SetFooterParts(std::move(parts));
    // Screen pixels per image pixel: fit scales the whole picture into the view.
    float ratio = 1.0f;
    if (image_fit_) {
        ratio = (std::min)((content.right - content.left) / static_cast<float>(shown_w),
                           (content.bottom - content.top) / static_cast<float>(shown_h));
    } else {
        const D2D1_RECT_F dest = ImageDest(content);
        ratio = (dest.right - dest.left) / static_cast<float>(shown_w);
    }
    wchar_t percent[16];
    swprintf_s(percent, L"%.0f%%", ratio * 100.0f);
    std::wstring zoom = percent;
    if (image_fit_) zoom = std::wstring(pulse::l10n::Get(pulse::l10n::StringId::PreviewFit)) + L" \x00B7 " + zoom;
    AddFooterControl(kFooterZoomOut, L"\x2212");
    AddFooterControl(kFooterZoom, std::move(zoom));
    AddFooterControl(kFooterZoomIn, L"+");
    if ((!kImageExtras || !IsIconFile(item_.path)) && frame_count_ <= 1)
        AddFooterControl(kFooterRotate, L"\x21BB");
    if (!kImageExtras || !IsIconFile(item_.path)) {
        using pulse::l10n::StringId;
        static constexpr StringId kBackdrop[] = {StringId::PreviewBgAuto, StringId::PreviewBgLight,
                                                 StringId::PreviewBgDark, StringId::PreviewBgChecker};
        AddFooterControl(kFooterImageBg, pulse::l10n::Get(kBackdrop[image_backdrop_ & 3]));
    }
    if (!embedded_) {
        AddFooterControl(kFooterInfo, pulse::l10n::Get(pulse::l10n::StringId::Information));
        footer_controls_.back().on = info_open_;
    }
}

bool QuickPreviewWindow::ImageBackdropWanted() {
    if (decoded_w_ == 0 || decoded_h_ == 0) return false;
    if (image_backdrop_ != 0) return true;
    ImageStats stats;
    return thumbnails_.Stats(item_.path, preview_pixels_, item_.modified, item_.size, stats) &&
           stats.has_alpha;
}

bool QuickPreviewWindow::DrawImageBackdrop(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                                           const D2D1_RECT_F& draw) {
    if (!dc || !ImageBackdropWanted()) return false;
    if (image_backdrop_ == 1 || image_backdrop_ == 2) {
        ComPtr<ID2D1SolidColorBrush> fill;
        dc->CreateSolidColorBrush(D2D1::ColorF(image_backdrop_ == 1 ? 0xF2F2F2 : 0x1C1C1C), &fill);
        if (!fill.get()) return false;
        dc->FillRectangle(content, fill.get());
        return true;
    }
    // Checkerboard under the picture only: a 2 x 2 bitmap tiled at 8 DIP cells.
    D2D1_RECT_F shown = draw;
    if (image_fit_) {
        const float aspect = TurnedW() / TurnedH();
        const float dw = draw.right - draw.left, dh = draw.bottom - draw.top;
        if (dw <= 0.0f || dh <= 0.0f) return false;
        if (aspect > dw / dh) {
            const float h = dw / aspect, cy = (draw.top + draw.bottom) * 0.5f;
            shown.top = cy - h * 0.5f;
            shown.bottom = cy + h * 0.5f;
        } else {
            const float w = dh * aspect, cx = (draw.left + draw.right) * 0.5f;
            shown.left = cx - w * 0.5f;
            shown.right = cx + w * 0.5f;
        }
    }
    const uint32_t a = dark_ ? 0xFF2E2E2Eu : 0xFFFFFFFFu, b = dark_ ? 0xFF3C3C3Cu : 0xFFE6E6E6u;
    const uint32_t cells[4] = {a, b, b, a};
    const auto props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Device> device;
    dc->GetDevice(&device);
    if (device.get() != backdrop_device_.get() || backdrop_dark_ != dark_) {
        backdrop_brush_.reset();
        backdrop_device_ = std::move(device);
        backdrop_dark_ = dark_;
    }
    if (!backdrop_brush_.get()) {
        ComPtr<ID2D1Bitmap> tile;
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(2, 2), cells, 8, &props, &tile)) || !tile.get() ||
        FAILED(dc->CreateBitmapBrush(tile.get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP,
            D2D1_EXTEND_MODE_WRAP, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR), &backdrop_brush_)) ||
        !backdrop_brush_.get()) return false;
    }
    const float cell = std::round(8.0f * scale_);
    backdrop_brush_->SetTransform(D2D1::Matrix3x2F::Scale(cell, cell) *
                        D2D1::Matrix3x2F::Translation(std::round(shown.left), std::round(shown.top)));
    dc->FillRectangle(shown, backdrop_brush_.get());
    return true;
}

bool QuickPreviewWindow::PlainTextCandidate() const {
    // Prose-like text gets the 阅读 | 等宽 toggle; code keeps its highlighted view.
    const std::wstring_view language = SyntaxLanguageName(L"." + ExtensionLabel(item_.path));
    return language.empty() || language == L"Log";
}

void QuickPreviewWindow::EnsureTextLayout(const std::wstring& text, bool hex, float width,
                                          bool reading, bool same_text) {
    reading = reading && !hex && reading_text_format_.get();
    if (text.empty() || !compositor_.DwriteFactory() || !preview_text_format_.get()) {
        // The source token must not acknowledge text we could not synchronize.
        preview_text_identity_.Reset();
        plain_highlight_lines_.Reset();
        ResetTextDrawingCache();
        text_layout_.reset();
        return;
    }
    const std::wstring extension = L"." + ExtensionLabel(item_.path);
    const bool gutter = !hex && !reading && line_numbers_ && SyntaxWantsLineNumbers(extension);
    if (text_layout_.get() && (same_text || text == preview_text_) && hex == text_layout_hex_ &&
        std::abs(width - text_layout_width_) < 0.5f &&
        std::abs(scale_ - text_layout_scale_) < 0.001f && dark_ == text_layout_dark_ &&
        gutter == (text_gutter_ > 0.0f) && reading == text_layout_reading_) return;
    const bool text_changed = !same_text && text != preview_text_;
    if (!same_text) preview_text_ = text;
    plain_highlight_lines_.Reset();
    ResetTextDrawingCache();
    text_layout_.reset();
    preview_text_format_->SetWordWrapping(
        hex ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_WRAP);
    text_line_count_ = 1;
    for (const wchar_t c : text) if (c == L'\n') ++text_line_count_;
    {
        bool lf = false, crlf = false, cr = false;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == L'\r') {
                if (i + 1 < text.size() && text[i + 1] == L'\n') { crlf = true; ++i; }
                else cr = true;
            } else if (text[i] == L'\n') {
                lf = true;
            }
        }
        const int kinds = static_cast<int>(lf) + static_cast<int>(crlf) + static_cast<int>(cr);
        text_eol_ = kinds > 1 ? 4 : crlf ? 2 : cr ? 3 : lf ? 1 : 0;
    }
    if (!text.empty() && text.back() == L'\n') --text_line_count_;
    line_starts_.clear();
    text_gutter_ = 0.0f;
    if (gutter) {
        line_starts_.reserve(text_line_count_);
        line_starts_.push_back(0);
        for (size_t i = 0; i + 1 < text.size(); ++i)
            if (text[i] == L'\n') line_starts_.push_back(static_cast<uint32_t>(i + 1));
        float digit = 8.0f * scale_;
        ComPtr<IDWriteTextLayout> probe;
        if (SUCCEEDED(compositor_.DwriteFactory()->CreateTextLayout(L"0000000000", 10,
                preview_text_format_.get(), 1000.0f, 100.0f, &probe)) && probe.get()) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(probe->GetMetrics(&metrics))) digit = metrics.width / 10.0f;
        }
        const size_t digits = std::to_wstring(line_starts_.size()).size();
        text_gutter_ = (static_cast<float>((std::max)(digits, size_t{2})) + 2.0f) * digit;
    }
    text_layout_reading_ = reading;
    text_reading_x_ = 0.0f;
    if (reading) {
        // A centred column capped at 680 px reads like a page rather than a terminal.
        static constexpr float kColumns[] = {560.0f, 680.0f, 860.0f};
        const float column = (std::min)(kColumns[std::clamp(reading_width_, 0, 2)] * scale_,
                                        (std::max)(1.0f, width));
        text_reading_x_ = 20.0f * scale_ + (std::max)(0.0f, (width - column) * 0.5f);
        compositor_.DwriteFactory()->CreateTextLayout(
            text.data(), static_cast<UINT32>(text.size()), reading_text_format_.get(),
            column, 100000.0f, &text_layout_);
        // A short first line followed by a blank line is treated as the title.
        const size_t first = text.find(L'\n');
        if (text_layout_.get() && first != std::wstring::npos && first > 0) {
            size_t end = first;
            if (end > 0 && text[end - 1] == L'\r') --end;
            size_t next = first + 1;
            if (next < text.size() && text[next] == L'\r') ++next;
            bool title = end > 0 && end <= 60 && next < text.size() && text[next] == L'\n';
            if (title) {
                bool ink = false;
                for (size_t i = 0; i < end; ++i) if (!std::iswspace(text[i])) { ink = true; break; }
                title = ink;
            }
            if (title) {
                const DWRITE_TEXT_RANGE range{0, static_cast<UINT32>(end)};
                text_layout_->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, range);
                text_layout_->SetFontSize(18.0f * scale_, range);
            }
        }
    } else {
        compositor_.DwriteFactory()->CreateTextLayout(
            text.data(), static_cast<UINT32>(text.size()), preview_text_format_.get(),
            (std::max)(1.0f, width - text_gutter_), 100000.0f, &text_layout_);
    }
    syntax_language_ = hex ? std::wstring() : std::wstring(SyntaxLanguageName(extension));
    if (!hex && !reading && text_layout_.get() && compositor_.Dc()) {
        for (auto& brush : syntax_brushes_) brush.reset();
        const auto spans = HighlightSyntax(extension, text);
        for (const SyntaxSpan& span : spans) {
            const auto index = static_cast<size_t>(span.token);
            if (index == 0 || index >= std::size(syntax_brushes_)) continue;
            if (!syntax_brushes_[index].get())
                compositor_.Dc()->CreateSolidColorBrush(
                    D2D1::ColorF(SyntaxTokenRgb(span.token, dark_)), &syntax_brushes_[index]);
            if (syntax_brushes_[index].get())
                text_layout_->SetDrawingEffect(syntax_brushes_[index].get(),
                                               DWRITE_TEXT_RANGE{span.start, span.length});
        }
    }
    text_layout_dark_ = dark_;
    text_layout_width_ = width;
    text_layout_scale_ = scale_;
    text_layout_hex_ = hex;
    if (text_changed) UpdateFindMatches();
}

bool QuickPreviewWindow::HitTestText(float x, float y, uint32_t& index) {
    if (native_kind_ == NativeKind::Markdown) return markdown_.HitTest(x, y, index);
    if (!text_layout_.get()) return false;
    const D2D1_RECT_F content = ContentRect();
    const float pad = 20.0f * scale_;
    const float origin_x = TextOriginX();
    const float origin_y = content.top + pad + HexRulerHeight() - text_scroll_;
    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(text_layout_->HitTestPoint(x - origin_x, y - origin_y, &trailing, &inside,
                                          &metrics))) return false;
    index = metrics.textPosition + (trailing ? metrics.length : 0u);
    index = (std::min)(index, static_cast<uint32_t>(preview_text_.size()));
    return true;
}

bool QuickPreviewWindow::HasTextSelection() const noexcept {
    if (native_kind_ == NativeKind::Table) return table_.HasSelection();
    if (native_kind_ == NativeKind::Tree) return tree_.HasCurrent();
    return sel_anchor_ != sel_focus_;
}

void QuickPreviewWindow::CopyTextSelection(bool require_selection) const {
    if (native_kind_ == NativeKind::Table) {
        // Cells as TSV (Excel pastes it back as a range).
        if (require_selection && !table_.HasSelection()) return;
        pulse::ops::WriteClipboardText(table_.SelectionText());
        return;
    }
    if (native_kind_ == NativeKind::Tree) {
        // The current node's value (a subtree as JSON/XML), else the source.
        if (require_selection && !tree_.HasCurrent()) return;
        pulse::ops::WriteClipboardText(tree_.CurrentValue());
        return;
    }
    if (preview_text_.empty()) return;
    uint32_t a = (std::min)(sel_anchor_, sel_focus_);
    uint32_t b = (std::max)(sel_anchor_, sel_focus_);
    a = (std::min)(a, static_cast<uint32_t>(preview_text_.size()));
    b = (std::min)(b, static_cast<uint32_t>(preview_text_.size()));
    if (b <= a) {
        if (require_selection) return;
        pulse::ops::WriteClipboardText(preview_text_);
        return;
    }
    pulse::ops::WriteClipboardText(preview_text_.substr(a, b - a));
}

void QuickPreviewWindow::SelectAllText() {
    if (native_kind_ == NativeKind::Table) {
        table_.SelectAll();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (native_kind_ == NativeKind::Tree) return;  // rows are picked one at a time
    sel_anchor_ = 0;
    sel_focus_ = static_cast<uint32_t>(preview_text_.size());
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::SetSearchQuery(const std::wstring& query) {
    if (search_query_ == query) return;
    search_query_ = query;
    if (find_open_) return;
    find_query_ = search_query_;
    UpdateFindMatches();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::OpenFind() {
    find_open_ = true;
    if (HasTextSelection() && find_query_.empty()) {
        uint32_t a = (std::min)(sel_anchor_, sel_focus_);
        uint32_t b = (std::max)(sel_anchor_, sel_focus_);
        a = (std::min)(a, static_cast<uint32_t>(preview_text_.size()));
        b = (std::min)(b, static_cast<uint32_t>(preview_text_.size()));
        if (b > a && b - a <= kFindQueryLimit)
            find_query_ = preview_text_.substr(a, b - a);
    }
    UpdateFindMatches();
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (!EnsureFindEdit()) return;
    SetWindowTextW(find_edit_, find_query_.c_str());
    LayoutFindEdit();
    ShowWindow(find_edit_, SW_SHOW);
    SetForegroundWindow(GetAncestor(find_edit_, GA_ROOT));
    SetFocus(find_edit_);
    SendMessageW(find_edit_, EM_SETSEL, 0, -1);
    if (compositor_.CustomEditEnabled())
        PaintFindEditLuma(find_edit_, nullptr);
}

void QuickPreviewWindow::CloseFind() {
    const bool restore_focus = find_open_ && find_edit_ && GetFocus() == find_edit_;
    plain_highlight_lines_.Reset();
    find_open_ = false;
    find_query_ = search_query_;
    UpdateFindMatches();
    archive_.SetFilter(L"");
    find_index_ = 0;
    if (find_edit_) {
        SetWindowTextW(find_edit_, L"");
        ShowWindow(find_edit_, SW_HIDE);
    }
    if (hwnd_ && IsWindowVisible(hwnd_)) {
        if (restore_focus) SetFocus(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

bool QuickPreviewWindow::EnsureFindEdit() {
    if (find_edit_) return true;
    if (!hwnd_) return false;
    if (!find_edit_font_) RecreateFormats();
    find_edit_ = CreateChildEdit(hwnd_);
    if (!find_edit_) return false;
    SetWindowTheme(find_edit_, L"", L"");
    if (find_edit_font_)
        SendMessageW(find_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(find_edit_font_), TRUE);
    if (!find_edit_brush_)
        find_edit_brush_ = CreateSolidBrush(FindEditBrushColor(dark_));
    SendMessageW(find_edit_, EM_SETLIMITTEXT, static_cast<WPARAM>(kFindQueryLimit), 0);
    SendMessageW(find_edit_, EM_SETCUEBANNER, TRUE,
        reinterpret_cast<LPARAM>(pulse::l10n::Get(pulse::l10n::StringId::Search).c_str()));
    SetWindowSubclass(find_edit_, FindEditProc, 1, reinterpret_cast<DWORD_PTR>(this));
    return true;
}

void QuickPreviewWindow::LayoutFindEdit() {
    if (!find_edit_ || !find_open_ || !hwnd_) return;
    const D2D1_RECT_F cell = FindEditCell();
    POINT pt{ static_cast<int>(std::lround(cell.left)),
              static_cast<int>(std::lround(cell.top)) };

    const int w = (std::max)(40, static_cast<int>(std::lround(cell.right - cell.left)));
    const int cell_h = (std::max)(18, static_cast<int>(std::lround(cell.bottom - cell.top)));
    const int line_h = EditLineHeight(find_edit_, find_edit_font_, cell_h);
    const int y = pt.y + (std::max)(0, (cell_h - line_h) / 2);
    RECT cur{};
    GetWindowRect(find_edit_, &cur);
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&cur), 2);
    const bool moved = cur.left != pt.x || cur.top != y || cur.right != pt.x + w ||
        cur.bottom != y + line_h;
    // Child coordinates stay local when the preview window moves.
    UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW;
    if (!moved) flags |= SWP_NOMOVE | SWP_NOSIZE | SWP_NOREDRAW;
    SetWindowPos(find_edit_, HWND_TOP, pt.x, y, w, line_h, flags);
    if (moved && compositor_.CustomEditEnabled())
        PaintFindEditLuma(find_edit_, nullptr);
}

void QuickPreviewWindow::SyncFindFromEdit() {
    if (!find_edit_ || !find_open_) return;
    wchar_t buf[kFindQueryLimit + 1]{};
    GetWindowTextW(find_edit_, buf, ARRAYSIZE(buf));
    if (find_query_ == buf) return;
    find_query_ = buf;
    if (native_kind_ == NativeKind::Archive) {
        // Archive search filters the tree (matches stay with their folders).
        archive_.SetFilter(find_query_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    UpdateFindMatches();
    if (!find_matches_.empty()) {
        find_index_ = 0;
        ScrollMatchIntoView(find_matches_[0]);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::PaintFindEditLuma(HWND hwnd, HDC hdc) {
    if (!hwnd || !compositor_.CustomEditEnabled()) return;
    HideCaret(hwnd);
    IDWriteTextFormat* format = compositor_.AddressFormat();
    if (!format) format = compositor_.TextFormat();
    if (format && compositor_.PresentLumaEdit(hwnd, format, FindEditFg(dark_),
                                              FindEditBg(dark_)))
        return;
    if (!hdc) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (!find_edit_brush_)
        find_edit_brush_ = CreateSolidBrush(FindEditBrushColor(dark_));
    FillRect(hdc, &rc, EditBackBrush(find_edit_brush_));
}

LRESULT QuickPreviewWindow::ForwardFindEditKeepLuma(HWND hwnd, UINT message,
                                                    WPARAM wparam, LPARAM lparam) {
    const bool mouse = message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK ||
        message == WM_LBUTTONUP || message == WM_MOUSEMOVE || message == WM_CAPTURECHANGED;
    LRESULT result;
    if (mouse) {
        result = compositor_.CallLumaEditMouse(
            hwnd, message, wparam, lparam,
            compositor_.AddressFormat() ? compositor_.AddressFormat()
                                        : compositor_.TextFormat());
        if (message != WM_MOUSEMOVE || GetCapture() == hwnd)
            PaintFindEditLuma(hwnd, nullptr);
    } else {
        SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
        result = DefSubclassProc(hwnd, message, wparam, lparam);
        HideCaret(hwnd);
        PaintFindEditLuma(hwnd, nullptr);
        SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
    }
    return result;
}

void QuickPreviewWindow::DestroyFindEdit() {
    if (find_edit_) {
        if (IsWindow(find_edit_)) {
            RemoveWindowSubclass(find_edit_, FindEditProc, 1);
            DestroyWindow(find_edit_);
        }
        find_edit_ = nullptr;
    }
    if (find_edit_font_) {
        DeleteObject(find_edit_font_);
        find_edit_font_ = nullptr;
    }
    if (find_edit_brush_) {
        DeleteObject(find_edit_brush_);
        find_edit_brush_ = nullptr;
    }
}

LRESULT CALLBACK QuickPreviewWindow::FindEditProc(HWND hwnd, UINT message, WPARAM wparam,
                                                  LPARAM lparam, UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<QuickPreviewWindow*>(data);
    if (!self) return DefSubclassProc(hwnd, message, wparam, lparam);
    const bool luma = SynchronizeChildEditBackend(self->compositor_, hwnd);
    if (luma && (message == WM_PRINT || message == WM_PRINTCLIENT ||
                 message == WM_NCPAINT)) {
        return 0;
    }
    switch (message) {
    case WM_GETDLGCODE:
        return DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            self->CloseFind();
            return 0;
        }
        if (wparam == VK_RETURN || wparam == VK_F3) {
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            self->FindNext(shift ? -1 : 1);
            return 0;
        }
        if (luma) return self->ForwardFindEditKeepLuma(hwnd, message, wparam, lparam);
        break;
    case WM_CHAR:
        if (wparam == VK_RETURN || wparam == VK_ESCAPE) return 0;
        if (luma) {
            const LRESULT result = self->ForwardFindEditKeepLuma(hwnd, message, wparam, lparam);
            self->SyncFindFromEdit();
            return result;
        }
        {
            const LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
            self->SyncFindFromEdit();
            return result;
        }
    case WM_PASTE:
    case WM_CUT: {
        const LRESULT result = luma
            ? self->ForwardFindEditKeepLuma(hwnd, message, wparam, lparam)
            : DefSubclassProc(hwnd, message, wparam, lparam);
        self->SyncFindFromEdit();
        return result;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_MOUSEMOVE:
    case WM_CAPTURECHANGED:
        if (luma) return self->ForwardFindEditKeepLuma(hwnd, message, wparam, lparam);
        break;
    case WM_IME_COMPOSITION:
    case WM_IME_CHAR:
        if (luma) {
            const LRESULT result = self->ForwardFindEditKeepLuma(hwnd, message, wparam, lparam);
            self->SyncFindFromEdit();
            return result;
        }
        break;
    case WM_PAINT: {
        if (!luma) break;
        self->PaintFindEditLuma(hwnd, nullptr);
        return 0;
    }
    case WM_SETFOCUS: {
        const LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
        if (luma) {
            HideCaret(hwnd);
            SetTimer(hwnd, kFindEditCaretTimer, GetCaretBlinkTime(), nullptr);
            self->PaintFindEditLuma(hwnd, nullptr);
        }
        if (self->hwnd_) InvalidateRect(self->hwnd_, nullptr, FALSE);
        return result;
    }
    case WM_KILLFOCUS:
        KillTimer(hwnd, kFindEditCaretTimer);
        if (self->hwnd_) InvalidateRect(self->hwnd_, nullptr, FALSE);
        break;
    case WM_TIMER:
        if (wparam == kFindEditCaretTimer) {
            if (GetCapture() != hwnd)
                self->PaintFindEditLuma(hwnd, nullptr);
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        if (luma) return 1;
        {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            if (!self->find_edit_brush_)
                self->find_edit_brush_ = CreateSolidBrush(FindEditBrushColor(self->dark_));
            FillRect(reinterpret_cast<HDC>(wparam), &rc, EditBackBrush(self->find_edit_brush_));
            return 1;
        }
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

void QuickPreviewWindow::ShowContextMenu(POINT screen) {
    if (!text_menu_.Create(hwnd_, &compositor_, scale_)) return;
    text_menu_.SetTheme(dark_, accent_);
    // System preview handlers (video, PDF, Office) sit in a topmost overlay
    // over the content area; a plain popup would open underneath it.
    text_menu_.SetTopmost(native_kind_ == NativeKind::None);
    using pulse::l10n::StringId;
    const bool text_kind = native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex ||
                           native_kind_ == NativeKind::Markdown || native_kind_ == NativeKind::Table;
    const QuickPreviewItem target = ActionTarget();
    const uint64_t menu_generation = generation_;
    const bool child = target.path != item_.path;
    const bool archive_entry = native_kind_ == NativeKind::Archive &&
        !CanApplyPreviewFileAction(archive_.IsFolderListing(), archive_.HasSelection());
    const auto archive_path = archive_entry ? archive_.SelectedPath() : std::wstring{};
    const bool file_verbs = !archive_entry && owner_ && command_message_ != 0 && !target.path.empty();
    std::vector<FluentMenuItem> items;
    const auto add = [&](int command, StringId label, const wchar_t* glyph,
                         const wchar_t* shortcut, bool enabled = true,
                         bool separator_after = false) {
        FluentMenuItem item;
        item.command = command;
        item.text = pulse::l10n::Get(label);
        item.glyph = glyph;
        item.shortcut = shortcut;
        item.enabled = enabled;
        item.separator_after = separator_after;
        items.push_back(std::move(item));
    };
    const auto file_cmd = [](QuickPreviewAction action) {
        return kFileCmdBase + static_cast<int>(action);
    };
    if (markdown_shown_ && !header_.tools_visible) {
        // The header folded its view toggle into this menu.
        const wchar_t* labels[2]{};
        ToggleLabels(labels);
        const bool second = ToggleSecond();
        for (int i = 0; i < 2; ++i) {
            FluentMenuItem item;
            item.command = kViewCmdFirst + i;
            item.text = labels[i];
            item.radio_group = true;
            item.radio = (i == 1) == second;
            item.separator_after = i == 1;
            items.push_back(std::move(item));
        }
    }
    if (archive_entry) {
        add(kArchiveCmdCopyPath, StringId::CopyPath, kLinkGlyph, L"Ctrl+Shift+C");
        items.back().text = pulse::l10n::Get(pulse::l10n::StringId::CopyPathInsideArchive).c_str();
    }
    if (text_kind) {
        // Text verbs keep their ids and order; file verbs follow below them.
        add(kTextCmdCopy, StringId::Copy, kCopyGlyph, L"Ctrl+C", HasTextSelection());
        add(kTextCmdSelectAll, StringId::SelectAll, kSelectAllGlyph, L"Ctrl+A", true, !file_verbs);
        const bool line_toggle = native_kind_ == NativeKind::Text &&
            SyntaxWantsLineNumbers(L"." + ExtensionLabel(item_.path));
        add(kTextCmdFind, StringId::Search, kSearchGlyph, L"Ctrl+F", true,
            file_verbs && !line_toggle);
        if (line_toggle) {
            FluentMenuItem item;
            item.command = kTextCmdLineNumbers;
            item.text = line_numbers_ ? (pulse::l10n::Get(pulse::l10n::StringId::HideLineNumbers).c_str())
                                      : (pulse::l10n::Get(pulse::l10n::StringId::ShowLineNumbers).c_str());
            item.glyph = L"\xE8FD";
            item.shortcut = L"";
            item.separator_after = file_verbs;
            items.push_back(std::move(item));
        }
    }
    if (file_verbs) {
        const bool writable = !target.read_only;
        add(file_cmd(QuickPreviewAction::Open), StringId::Open, kOpenGlyph, L"Enter");
        if (!text_kind)
            add(file_cmd(QuickPreviewAction::Copy), StringId::Copy, kCopyGlyph, L"Ctrl+C");
        add(file_cmd(QuickPreviewAction::Cut), StringId::Cut, kCutGlyph, L"Ctrl+X", writable);
        add(file_cmd(QuickPreviewAction::CopyPath), StringId::CopyPath, kLinkGlyph,
            L"Ctrl+Shift+C", true, true);
        add(file_cmd(QuickPreviewAction::ToggleStar),
            target.starred ? StringId::Unstar : StringId::Star,
            target.starred ? kStarFilledGlyph : kStarGlyph, L"", true, true);
        if (child) items.back().text = pulse::l10n::Get(pulse::l10n::StringId::ToggleStar).c_str();
        add(file_cmd(QuickPreviewAction::Rename), StringId::Rename, kRenameGlyph, L"F2", writable);
        add(file_cmd(QuickPreviewAction::Delete), StringId::Delete, kDeleteGlyph, L"Delete",
            writable, true);
        add(file_cmd(QuickPreviewAction::Properties), StringId::Properties, kPropertiesGlyph,
            L"Alt+Enter");
    }
    if (items.empty()) return;
    const int cmd = text_menu_.TrackPopup(screen, std::move(items));
    if (cmd == kViewCmdFirst || cmd == kViewCmdFirst + 1) {
        if (menu_generation == generation_ && markdown_shown_) {
            bool& second = ToggleSecond();
            if ((cmd == kViewCmdFirst + 1) != second) {
                second = cmd == kViewCmdFirst + 1;
                selecting_ = false;
                sel_anchor_ = sel_focus_ = 0;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
    }
    else if (cmd == kTextCmdCopy) CopyTextSelection(true);
    else if (cmd == kArchiveCmdCopyPath) pulse::ops::WriteClipboardText(archive_path);
    else if (cmd == kTextCmdSelectAll) SelectAllText();
    else if (cmd == kTextCmdFind) OpenFind();
    else if (cmd == kTextCmdLineNumbers) {
        line_numbers_ = !line_numbers_;
        plain_highlight_lines_.Reset();
        ResetTextDrawingCache();
        text_layout_.reset();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    else if (cmd >= kFileCmdBase && menu_generation == generation_ && visible())
        PostAction(static_cast<QuickPreviewAction>(cmd - kFileCmdBase), &target);
}

void QuickPreviewWindow::UpdateFindMatches() {
    find_matches_.clear();
    find_index_ = 0;
    pending_search_reveal_ = false;
    if (find_query_.empty() || preview_text_.empty()) {
        plain_highlight_lines_.Reset();
        return;
    }
    const size_t n = preview_text_.size();
    const size_t q = find_query_.size();
    if (q > n) return;
    for (size_t i = 0; i + q <= n; ++i) {
        bool match = true;
        for (size_t j = 0; j < q; ++j) {
            if (std::towlower(preview_text_[i + j]) != std::towlower(find_query_[j])) {
                match = false;
                break;
            }
        }
        if (match) find_matches_.push_back(static_cast<uint32_t>(i));
    }
    pending_search_reveal_ = !find_open_ && !search_query_.empty() && !find_matches_.empty();
}

void QuickPreviewWindow::ScrollMatchIntoView(uint32_t start) {
    if (native_kind_ == NativeKind::Markdown) {
        markdown_.Reveal(start);
        return;
    }
    if (native_kind_ == NativeKind::Table) {
        table_.Reveal(start);
        return;
    }
    if (native_kind_ == NativeKind::Tree) {
        tree_.Reveal(start);
        return;
    }
    if (!text_layout_.get()) return;
    FLOAT x = 0, y = 0;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(text_layout_->HitTestTextPosition(start, FALSE, &x, &y, &metrics))) return;
    const D2D1_RECT_F content = ContentRect();
    const float pad = 20.0f * scale_;
    const float view = (std::max)(1.0f, content.bottom - content.top - pad * 2.0f);
    if (y < text_scroll_) text_scroll_ = (std::max)(0.0f, y);
    else if (y + metrics.height > text_scroll_ + view)
        text_scroll_ = y + metrics.height - view;
}

void QuickPreviewWindow::FindNext(int direction) {
    if (find_matches_.empty()) return;
    if (direction >= 0) {
        find_index_ = (find_index_ + 1) % static_cast<uint32_t>(find_matches_.size());
    } else {
        find_index_ = (find_index_ + static_cast<uint32_t>(find_matches_.size()) - 1) %
                      static_cast<uint32_t>(find_matches_.size());
    }
    ScrollMatchIntoView(find_matches_[find_index_]);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

Theme QuickPreviewWindow::CurrentTheme() const {
    HIGHCONTRASTW value{sizeof(value)};
    const bool high_contrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(value), &value, 0) &&
        (value.dwFlags & HCF_HIGHCONTRASTON);
    return high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, HexColor(0x0078D4));
}

void QuickPreviewWindow::DrawFindBar(ID2D1DeviceContext* dc, const D2D1_RECT_F& bar) {
    if (!dc) return;
    const bool high_contrast = [] {
        HIGHCONTRASTW value{sizeof(value)};
        return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(value), &value, 0) &&
            (value.dwFlags & HCF_HIGHCONTRASTON);
    }();
    const Theme theme = CurrentTheme();
    find_painter_.SetCompositor(&compositor_);
    find_painter_.SetScale(scale_);
    if (!find_painter_.BeginFrame(theme, high_contrast)) return;
    ComPtr<ID2D1SolidColorBrush> fill;
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x1F1F1F) : D2D1::ColorF(0xEDEDED), &fill);
    dc->FillRectangle(bar, fill.get());
    fluent::TextFieldSpec field{};
    field.bounds = FindFieldRect();
    field.leading_glyph = kSearchGlyph;
    field.compact_leading_glyph = true;
    field.suppress_text = true;
    field.hosted_edit = true;
    field.state.focused = find_edit_ && GetFocus() == find_edit_;
    field.state.enabled = true;
    std::wstring count_text;
    if (!find_query_.empty()) {
        wchar_t count[32]{};
        if (native_kind_ == NativeKind::Archive) swprintf_s(count, L"%u", archive_.FilterHits());
        else if (find_matches_.empty()) swprintf_s(count, L"0/0");
        else swprintf_s(count, L"%u/%u", find_index_ + 1,
                        static_cast<uint32_t>(find_matches_.size()));
        count_text = count;
        field.trailing_badge = count_text;
    }
    find_painter_.DrawTextField(field);
}

void QuickPreviewWindow::DrawTextPreview(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                                          ID2D1SolidColorBrush* text_brush) {
    if (!text_layout_.get() || !dc) return;
    LARGE_INTEGER text_start{};
    QueryPerformanceCounter(&text_start);
    const float pad = 20.0f * scale_;
    const float origin_x = TextOriginX();
    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(text_layout_->GetMetrics(&metrics))) {
        // A little room below the last line.
        const float view = (std::max)(1.0f, content.bottom - content.top - pad - HexRulerHeight() -
                                                8.0f * scale_);
        const float max_scroll = (std::max)(0.0f, metrics.height - view);
        text_scroll_ = std::clamp(text_scroll_, 0.0f, max_scroll);
    }
    // After the clamp: End and wheel overshoot must not draw one frame past the end.
    const float origin_y = content.top + pad + HexRulerHeight() - text_scroll_;
    if (!find_query_.empty() && !find_matches_.empty()) {
        ComPtr<ID2D1SolidColorBrush> match_brush;
        ComPtr<ID2D1SolidColorBrush> current_brush;
        dc->CreateSolidColorBrush(D2D1::ColorF(0xC19C00, 0.28f), &match_brush);
        dc->CreateSolidColorBrush(D2D1::ColorF(0xCA5010, 0.40f), &current_brush);
        const uint32_t length = static_cast<uint32_t>(find_query_.size());
        auto first = find_matches_.begin();
        auto last = find_matches_.end();
        uint32_t visible_begin = 0, visible_end = 0;
        if (find_matches_.size() > 128 && plain_highlight_lines_.Range(text_layout_.get(),
                content.top - origin_y, content.bottom - origin_y, visible_begin, visible_end)) {
            const uint32_t earliest = visible_begin >= length ? visible_begin - length + 1 : 0;
            first = std::lower_bound(first, last, earliest);
            last = std::lower_bound(first, last, visible_end);
        }
        for (auto match = first; match != last; ++match) {
            const size_t i = static_cast<size_t>(match - find_matches_.begin());
            FillHitRange(dc, text_layout_.get(), find_matches_[i], length, origin_x, origin_y,
                         i == find_index_ ? current_brush.get() : match_brush.get());
        }
    }
    const uint32_t a = (std::min)(sel_anchor_, sel_focus_);
    const uint32_t b = (std::max)(sel_anchor_, sel_focus_);
    if (b > a) {
        ComPtr<ID2D1SolidColorBrush> sel;
        dc->CreateSolidColorBrush(D2D1::ColorF(0x0078D4, 0.35f), &sel);
        FillHitRange(dc, text_layout_.get(), a, b - a, origin_x, origin_y, sel.get());
    }
    if (text_layout_hex_) {
        // The clicked byte and the byte under the pointer, lit in both the hex
        // and the character column.
        const auto light = [&](int byte, float alpha) {
            if (byte < 0) return;
            ComPtr<ID2D1SolidColorBrush> tint_brush;
            D2D1_COLOR_F tint = Palette().accent;
            tint.a = alpha;
            dc->CreateSolidColorBrush(tint, &tint_brush);
            const uint32_t line = static_cast<uint32_t>(byte) / 16u * 76u;
            const uint32_t column = static_cast<uint32_t>(byte) % 16u;
            FillHitRange(dc, text_layout_.get(), line + 10u + column * 3u, 2u, origin_x, origin_y, tint_brush.get());
            FillHitRange(dc, text_layout_.get(), line + 59u + column, 1u, origin_x, origin_y, tint_brush.get());
        };
        light(hex_selected_, dark_ ? 0.45f : 0.32f);
        if (hex_hover_ != hex_selected_) light(hex_hover_, dark_ ? 0.26f : 0.16f);
    }
    DrawLineNumbers(dc, content, origin_y);
    DrawCachedText(dc, text_brush, origin_x, origin_y);
    if (text_layout_hex_) DrawHexRuler(dc, content, origin_x);
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    last_text_ms_ = static_cast<double>(now.QuadPart - text_start.QuadPart) * 1000.0 /
                    static_cast<double>(freq.QuadPart);
}

// Hex dump lines are fixed at 76 characters: an 8-digit offset and two
// spaces, 16 "XX " cells, a space, 16 characters and the line feed.
int QuickPreviewWindow::HexByteAt(uint32_t index) const noexcept {
    if (index >= preview_text_.size()) return -1;
    const uint32_t line = index / 76u, c = index % 76u;
    uint32_t column = 0;
    if (c >= 10u && c < 58u) column = (c - 10u) / 3u;
    else if (c >= 59u && c < 75u) column = c - 59u;
    else return -1;
    const size_t cell = static_cast<size_t>(line) * 76u + 10u + column * 3u;
    if (cell >= preview_text_.size() || preview_text_[cell] == L' ') return -1;
    return static_cast<int>(line * 16u + column);
}

void QuickPreviewWindow::DrawHexRuler(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                                      float origin_x) {
    if (!dc || !preview_text_format_.get() || !compositor_.DwriteFactory()) return;
    const ChromePalette palette = Palette();
    const float s = scale_;
    const float band = content.top + 20.0f * s + HexRulerHeight() - 6.0f * s;
    ComPtr<ID2D1SolidColorBrush> bg, ink, line;
    dc->CreateSolidColorBrush(palette.bg, &bg);
    dc->CreateSolidColorBrush(palette.ink3, &ink);
    dc->CreateSolidColorBrush(palette.line, &line);
    if (!bg.get() || !ink.get()) return;
    dc->FillRectangle(D2D1::RectF(content.left, content.top, content.right, band), bg.get());
    if (line.get())
        dc->FillRectangle(D2D1::RectF(content.left, band - (std::max)(1.0f, std::round(s)), content.right, band),
                          line.get());
    // Column ruler: 00..0F over the cells and 0..F over the characters, with
    // the hovered column drawn in full ink.
    std::wstring ruler(10, L' ');
    static constexpr wchar_t kDigits[] = L"0123456789ABCDEF";
    for (int i = 0; i < 16; ++i) { ruler += L'0'; ruler += kDigits[i]; ruler += L' '; }
    ruler += L' ';
    ruler += kDigits;
    ComPtr<IDWriteTextLayout> layout;
    preview_text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    compositor_.DwriteFactory()->CreateTextLayout(ruler.data(), static_cast<UINT32>(ruler.size()),
        preview_text_format_.get(), 10000.0f, 100.0f, &layout);
    preview_text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    if (!layout.get()) return;
    if (HexFocus() >= 0) {
        ComPtr<ID2D1SolidColorBrush> strong;
        dc->CreateSolidColorBrush(palette.ink, &strong);
        const UINT32 column = static_cast<UINT32>(HexFocus() % 16);
        if (strong.get()) {
            layout->SetDrawingEffect(strong.get(), DWRITE_TEXT_RANGE{10u + column * 3u, 2u});
            layout->SetDrawingEffect(strong.get(), DWRITE_TEXT_RANGE{59u + column, 1u});
        }
    }
    dc->DrawTextLayout(D2D1::Point2F(origin_x, band - 4.0f * s - 18.0f * s), layout.get(), ink.get(),
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// Footer facts of the text view:
//   C++  |  UTF-8  |  245 lines \x00B7 8.4 KB
// CSV: kind, delimiter and encoding, shape. Workbooks: format, sheet, shape, size.
void QuickPreviewWindow::SetTableFooter(uint32_t encoding) {
    std::vector<std::wstring> parts = table_.StatusParts(encoding);
    if (parts.empty()) return;
    if (table_.IsSpreadsheet()) {
        parts.insert(parts.begin(), ExtensionLabel(item_.path));
        if (item_.size) parts.push_back(pulse::format::ByteSize(item_.size, true));
    }
    SetFooterParts(std::move(parts));
}

void QuickPreviewWindow::SetTextFooter(bool hex, uint32_t bytes_read, bool truncated,
                                       uint32_t encoding, bool show_eol) {
    std::vector<std::wstring> parts;
    if (hex) {
        parts.push_back(pulse::l10n::Get(pulse::l10n::StringId::Hex).c_str());
    } else {
        parts.push_back(!syntax_language_.empty() ? syntax_language_
                                                  : std::wstring(pulse::l10n::Get(pulse::l10n::StringId::PlainText).c_str()));
        std::wstring name;
        switch (static_cast<ipc::PreviewTextEncoding>(encoding)) {
        case ipc::PreviewTextEncoding::Utf8: name = L"UTF-8"; break;
        case ipc::PreviewTextEncoding::Utf8Bom: name = L"UTF-8 BOM"; break;
        case ipc::PreviewTextEncoding::Utf16Le: name = L"UTF-16 LE"; break;
        case ipc::PreviewTextEncoding::Utf16Be: name = L"UTF-16 BE"; break;
        case ipc::PreviewTextEncoding::Ansi:
            switch (GetACP()) {
            case 936: name = L"GBK"; break;
            case 950: name = L"Big5"; break;
            case 932: name = L"Shift-JIS"; break;
            case 949: name = L"EUC-KR"; break;
            case 1252: name = L"Windows-1252"; break;
            default: name = L"ANSI " + std::to_wstring(GetACP()); break;
            }
            break;
        default: break;
        }
        if (!name.empty()) parts.push_back(std::move(name));
        switch (show_eol ? text_eol_ : 0) {
        case 1: parts.push_back(L"LF"); break;
        case 2: parts.push_back(L"CRLF"); break;
        case 3: parts.push_back(L"CR"); break;
        case 4: parts.push_back(pulse::l10n::Get(pulse::l10n::StringId::PreviewMixedEol).c_str()); break;
        default: break;
        }
    }
    // The catalog strings carry their own " · " joints; the footer adds its own.
    const auto bare = [](std::wstring text) {
        const auto joint = [](wchar_t c) { return c == L' ' || c == 0x00B7; };
        while (!text.empty() && joint(text.back())) text.pop_back();
        size_t start = 0;
        while (start < text.size() && joint(text[start])) ++start;
        return text.substr(start);
    };
    if (!hex)
        parts.push_back(std::to_wstring(text_line_count_) + L" " +
                        bare(pulse::l10n::Get(pulse::l10n::StringId::Lines).c_str()));
    parts.push_back(pulse::format::ByteSize(bytes_read, true));
    if (truncated) parts.push_back(bare(pulse::l10n::Get(pulse::l10n::StringId::Truncated).c_str()));
    if (const int focus = HexFocus(); hex && focus >= 0) {
        // 0x0000001F = 0x41 'A'
        const size_t cell = static_cast<size_t>(focus) / 16u * 76u + 10u +
                            static_cast<size_t>(focus % 16) * 3u;
        if (cell + 1 < preview_text_.size()) {
            const wchar_t ascii = preview_text_[static_cast<size_t>(focus) / 16u * 76u + 59u +
                                                static_cast<size_t>(focus % 16)];
            wchar_t buffer[64];
            swprintf_s(buffer, L"0x%08X = 0x%c%c", static_cast<unsigned>(focus),
                       preview_text_[cell], preview_text_[cell + 1]);
            std::wstring part = buffer;
            if (ascii != L'.') { part += L" '"; part += ascii; part += L"'"; }
            parts.push_back(std::move(part));
        }
    }
    SetFooterParts(std::move(parts));
}

// Enter / double-click on a row of a folder listing opens that item.
bool QuickPreviewWindow::OpenListingSelection() {
    if (!owner_ || !archive_.IsFolderListing()) return false;
    const std::wstring relative = archive_.SelectedPath();
    if (relative.empty()) return false;
    if (command_message_) {
        const auto target = ActionTarget();
        PostAction(QuickPreviewAction::Open, &target);
        return true;
    }
    if (!open_message_) return false;
    open_path_ = item_.path;
    if (!open_path_.empty() && open_path_.back() != L'\\') open_path_ += L'\\';
    open_path_ += relative;
    PostMessageW(owner_, open_message_, 1, 0);
    return true;
}

float QuickPreviewWindow::TextOriginX() const noexcept {
    if (text_layout_reading_) return text_reading_x_;
    return 20.0f * scale_ + text_gutter_;
}

void QuickPreviewWindow::DrawLineNumbers(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                                         float origin_y) {
    if (text_gutter_ <= 0.0f || line_starts_.empty() || !gutter_format_.get() ||
        !text_layout_.get())
        return;
    const float pad = 20.0f * scale_;
    const float right = pad + text_gutter_ - 12.0f * scale_;
    ComPtr<ID2D1SolidColorBrush> number_brush, rule_brush;
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x858585) : D2D1::ColorF(0x9A9A9A), &number_brush);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0xFFFFFF, 0.08f) : D2D1::ColorF(0x000000, 0.08f),
                              &rule_brush);
    if (!number_brush.get()) return;
    if (rule_brush.get()) {
        const float x = right + 6.0f * scale_;
        dc->FillRectangle(D2D1::RectF(x, content.top, x + 1.0f, content.bottom), rule_brush.get());
    }
    auto line_y = [&](size_t line) {
        FLOAT x = 0, y = 0;
        DWRITE_HIT_TEST_METRICS metrics{};
        text_layout_->HitTestTextPosition(line_starts_[line], FALSE, &x, &y, &metrics);
        return std::pair<float, float>{y, metrics.height};
    };
    // First line whose top is inside the view (layout coordinates grow monotonically).
    const float view_top = content.top - origin_y;
    size_t lo = 0, hi = line_starts_.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const auto [y, h] = line_y(mid);
        if (y + h < view_top) lo = mid + 1;
        else hi = mid;
    }
    for (size_t line = lo; line < line_starts_.size(); ++line) {
        const auto [y, h] = line_y(line);
        const float top = origin_y + y;
        if (top > content.bottom) break;
        const std::wstring label = std::to_wstring(line + 1);
        dc->DrawTextW(label.data(), static_cast<UINT32>(label.size()), gutter_format_.get(),
                      D2D1::RectF(pad * 0.25f, top, right, top + (std::max)(h, 1.0f)),
                      number_brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

bool QuickPreviewWindow::ClientPoint(LPARAM lparam, POINT& out) const {
    out = POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    return true;
}

HCURSOR QuickPreviewWindow::ContentCursor(POINT client) const {
    if (OverCodecButton(client) || HitFooterControl(client) != 0) return LoadCursorW(nullptr, IDC_HAND);
    if (client.y < static_cast<LONG>(HeaderHeight()) || client.y >= static_cast<LONG>(FooterRect().top))
        return LoadCursorW(nullptr, IDC_ARROW);
    if (HasPlayback() && client.y >= PlaybackRect().top)
        return LoadCursorW(nullptr, IDC_HAND);
    if (find_open_ && FindBarHeight() > 0.0f) {
        const D2D1_RECT_F field = FindFieldRect();
        if (client.x >= field.left && client.x < field.right &&
            client.y >= field.top && client.y < field.bottom)
            return LoadCursorW(nullptr, IDC_IBEAM);
    }
    const D2D1_RECT_F content = ContentRect();
    if (client.x < content.left || client.x >= content.right ||
        client.y < content.top || client.y >= content.bottom)
        return LoadCursorW(nullptr, IDC_ARROW);
    if (native_kind_ == NativeKind::Markdown &&
        (markdown_.DraggingScrollbar() || markdown_.OverScrollbar(static_cast<float>(client.x), static_cast<float>(client.y))))
        return LoadCursorW(nullptr, IDC_ARROW);
    if (native_kind_ == NativeKind::Markdown &&
        markdown_.IsClickable(static_cast<float>(client.x), static_cast<float>(client.y)))
        return LoadCursorW(nullptr, IDC_HAND);
    if (native_kind_ == NativeKind::Markdown) {
        // Links open with Ctrl+click; a plain click still selects text.
        const bool link = (GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
            !markdown_.LinkAt(static_cast<float>(client.x), static_cast<float>(client.y)).empty();
        return LoadCursorW(nullptr, link ? IDC_HAND : IDC_IBEAM);
    }
    if (native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex)
        return LoadCursorW(nullptr, IDC_IBEAM);
    if (native_kind_ == NativeKind::Table) return LoadCursorW(nullptr, IDC_ARROW);
    if (native_kind_ == NativeKind::Pages)
        return LoadCursorW(nullptr, panning_ ? IDC_SIZEALL : IDC_ARROW);
    if (panning_ || CanPanImage()) return LoadCursorW(nullptr, IDC_SIZEALL);
    return LoadCursorW(nullptr, IDC_ARROW);
}

void QuickPreviewWindow::SchedulePreviewDeadline() {
    uint64_t deadline = thumbnails_.TakeRetryDeadline();
    if (compositor_.NeedsRecovery()) {
        const auto retry = GetTickCount64() + 250;
        if (!deadline || retry < deadline) deadline = retry;
    }
    const auto handler = handler_.NextDeadline();
    if (handler && (!deadline || handler < deadline)) deadline = handler;
    if (!hwnd_ || closing_ || !visible()) deadline = 0;
    if (deadline == preview_deadline_) return;
    KillTimer(hwnd_, kPreviewDeadlineTimer);
    preview_deadline_ = 0;
    if (deadline) {
        const auto now = GetTickCount64();
        const auto wait = deadline > now ? deadline - now : 1;
        if (SetTimer(hwnd_, kPreviewDeadlineTimer,
            static_cast<UINT>((std::min)(wait, uint64_t{USER_TIMER_MAXIMUM})), nullptr))
            preview_deadline_ = deadline;
    }
}

void QuickPreviewWindow::Render() {
    LARGE_INTEGER render_start{};
    QueryPerformanceCounter(&render_start);
    thumbnails_.BeginFrame();
    if (compositor_.NeedsRecovery()) {
        native_preview_.Reset();
        if (!compositor_.Recover()) {
            SchedulePreviewDeadline();
            return;
        }
        compositor_.RecreateTextFormats(scale_);
        RecreateFormats();
        thumbnails_.SetDeviceContext(compositor_.Dc());
        open_icons_.SetDeviceContext(compositor_.Dc());
    }
    if (!compositor_.Dc()) return;
    Resize();
    auto* dc = compositor_.Dc();
    dc->BeginDraw();
    const ChromePalette palette = Palette();
    const bool high_contrast = palette.high_contrast;
    const TextContrastScope text_contrast_scope(high_contrast, !TextOptimizationBaselineForTest());
    // System colours are COLORREFs (0x00BBGGRR): Palette() converts them.
    const D2D1_COLOR_F background = palette.bg;
    const D2D1_COLOR_F foreground = palette.ink;
    const D2D1_COLOR_F secondary = high_contrast ? foreground
        : dark_ ? D2D1::ColorF(0xB8B8B8) : D2D1::ColorF(0x606060);
    // Full screen: pictures and videos sit on black.
    dc->Clear(fullscreen_ && !high_contrast && (image_item_ || video_.active())
        ? D2D1::ColorF(0x000000) : background);
    ComPtr<ID2D1SolidColorBrush> text_brush;
    ComPtr<ID2D1SolidColorBrush> secondary_brush;
    dc->CreateSolidColorBrush(foreground, &text_brush);
    dc->CreateSolidColorBrush(secondary, &secondary_brush);
    const float width = static_cast<float>(compositor_.Width());
    const float height = static_cast<float>(compositor_.Height());
    const float pad = 20.0f * scale_;
    const float header = HeaderHeight();
    footer_parts_.clear();
    footer_hint_.clear();
    footer_controls_.clear();
    const D2D1_RECT_F notice_rect = D2D1::RectF(0, header + FindBarHeight(), width, height);
    preview_notice_height_ = DrawPreviewNotice(dc, compositor_.DwriteFactory(), compositor_.SmallFormat(),
        preview_notice_, notice_rect, scale_, CurrentTheme().text, CurrentTheme().fill_hover);
    const D2D1_RECT_F full_content = D2D1::RectF(0, header, width, (std::max)(header, height - FooterHeight()));
    // Prev / next: the new item's content slides 36 px in from its side and
    // fades in (160 ms). Preview handler windows stay put.
    float nav_dx = 0.0f, nav_alpha = 1.0f;
    if (nav_dir_ != 0) {
        const float t = static_cast<float>(motion::NowMs() - nav_start_);
        if (t >= static_cast<float>(motion::SpringSettleMs(motion::spring::Smooth))) {
            nav_dir_ = 0;
        } else {
            const float p = motion::SpringProgress(motion::spring::Smooth, t);
            nav_dx = std::round(36.0f * scale_ * static_cast<float>(nav_dir_) * (1.0f - p));
            nav_alpha = (std::clamp)(t / 160.0f, 0.0f, 1.0f);
        }
    }
    const bool nav_layer = nav_dx != 0.0f || nav_alpha < 1.0f;
    D2D1_MATRIX_3X2_F nav_saved{};
    if (nav_layer) {
        dc->GetTransform(&nav_saved);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(nav_dx, 0.0f) *
                         *D2D1::Matrix3x2F::ReinterpretBaseType(&nav_saved));
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr,
                                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                                             nav_alpha, nullptr, D2D1_LAYER_OPTIONS1_NONE), nullptr);
    }
    const bool offline = OfflinePlaceholder();
    const bool plugin_preview = !offline && !safe_mode_ && !(item_.attrs & FILE_ATTRIBUTE_DIRECTORY) &&
        native_preview_.Available(owner_, item_.path);
    const bool use_handler = !plugin_preview && !offline && !safe_mode_ && !video_.active() &&
        !(item_.attrs & FILE_ATTRIBUTE_DIRECTORY) && PreviewHandlerHost::CanHost(item_.path) &&
        !(kMarkdownRender && SyntaxLanguageName(L"." + ExtensionLabel(item_.path)) == L"Markdown") &&
        !(kTableView && TableFileKind(item_.path) == 2 && !table_handler_) &&
        !(kDocView && IsDocxFile(item_.path) && !doc_handler_) &&
        !(kDocView && ExtensionLabel(item_.path) == L"EPUB");
    handler_.Sync(hwnd_, full_content, item_.path, item_.attrs, generation_, item_.modified,
                  item_.size, dark_, background, foreground, use_handler,
                  handler_immediate_);
    handler_immediate_ = false;
    markdown_shown_ = false;
    toggle_kind_ = ToggleKind::Markdown;
    if (use_handler && kTableView && TableFileKind(item_.path) == 2) {
        // "Table | System" stays available over the Office preview handler.
        markdown_shown_ = true;
        toggle_kind_ = ToggleKind::TableHandler;
    } else if (use_handler && kDocView && IsDocxFile(item_.path)) {
        markdown_shown_ = true;
        toggle_kind_ = ToggleKind::DocHandler;
    }
    codec_store_rect_ = codec_open_rect_ = codec_pack_rect_ = cloud_download_rect_ = D2D1_RECT_F{};
    std::wstring status;
    if (offline) {
        native_kind_ = NativeKind::None;
        if (!DrawCloudCard(dc, ContentRect(), text_brush.get()))
            status = pulse::l10n::Get(pulse::l10n::StringId::PreviewCloudOnly);
    } else if (video_.active()) {
        native_kind_ = NativeKind::None;
        const auto video = video_.Snapshot();
        const bool audio = VideoPreview::IsAudio(item_.path);
        const bool codec_card = CodecCardVisible(video);
        const bool picture = !audio && !codec_card && !video.opening && video.ready && SUCCEEDED(video.error);
        if (picture && !high_contrast) {
            // Video sits on black, transport bar included.
            ComPtr<ID2D1SolidColorBrush> black;
            dc->CreateSolidColorBrush(D2D1::ColorF(0x000000), &black);
            if (black.get())
                dc->FillRectangle(D2D1::RectF(0, ContentRect().top, width, PlaybackRect().bottom), black.get());
        }
        LayoutVideo(dc, video, picture && visible());
        if (codec_card) DrawCodecCard(dc, ContentRect(), video, text_brush.get());
        if (audio && SUCCEEDED(video.error))
            DrawAudio(dc, video, text_brush.get(), secondary_brush.get());
        else if (!audio) {
            // Codec · size · length, then the frame rate; the decoder on the right.
            std::vector<PreviewProperty> properties;
            std::vector<std::wstring> facts;
            thumbnails_.Properties(item_.path, item_.attrs, generation_, item_.modified, item_.size,
                                   properties);
            VideoPreview::State shown = video;
            for (const PreviewProperty& property : properties) {
                if (property.value.empty()) continue;
                if (property.label == L"帧率") facts.push_back(property.value);
                else if (property.label == L"编码格式" && shown.codec_name.empty()) shown.codec_name = property.value;
            }
            SetMediaFooter(shown, std::move(facts));
            if (video.ready && SUCCEEDED(video.error) && GetTickCount64() < key_hint_until_)
                footer_hint_ = pulse::l10n::Get(pulse::l10n::StringId::PreviewKeyHint);
            else if (video.ready && SUCCEEDED(video.error))
                footer_hint_ = pulse::l10n::Get(video.ffmpeg ? pulse::l10n::StringId::PreviewDecodeFfmpeg
                                                             : pulse::l10n::StringId::PreviewDecodeSystem);
        }
        if (codec_card) {
            // The card explains the missing picture.
        } else if (video.opening)
            status = pulse::l10n::Get(pulse::l10n::StringId::PreviewLoading);
        else if (FAILED(video.error))
            status = video.unsupported_audio
                ? l10n::HantText(l10n::Get(l10n::StringId::WindowsCannotDecodeThisAudioOpenItInTheDefaultApp).c_str())
                : pulse::l10n::Get(pulse::l10n::StringId::PreviewCannotRender);
        else if (!video.ready && !audio)
            status = pulse::l10n::Get(pulse::l10n::StringId::PreviewLoading);
    } else if (use_handler) {
        native_kind_ = NativeKind::None;
        const auto state = handler_.state();
        if (state == PreviewHandlerHost::State::Loading ||
            state == PreviewHandlerHost::State::Idle)
            status = pulse::l10n::Get(pulse::l10n::StringId::PreviewLoading);
        else if (state == PreviewHandlerHost::State::Failed)
            status = pulse::l10n::Get(pulse::l10n::StringId::PreviewHostUnavailable);
    } else if (pages_count_ > 1) {
        native_kind_ = NativeKind::Pages;
        DrawPages(dc, secondary_brush.get());
    } else {
        std::shared_ptr<const std::wstring> shared_payload;
        std::wstring error;
        preview::Integrity integrity;
        bool truncated = false;
        uint32_t bytes_read = 0;
        uint32_t reported_frame_count = 1, reported_delay_ms = 0, reported_loop_count = 0;
        uint32_t text_encoding = 0;
        const D2D1_RECT_F content = ContentRect();
        const bool folder = (item_.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        // A folder listing does not depend on the window size.
        const uint32_t want = folder ? ipc::kPreviewDefaultPixelSize : RequestedPixelSize(content);
        const bool icon = kImageExtras && IsIconFile(item_.path);
        D2D1_RECT_F draw = ImageDest(content);
        if (icon && image_fit_ && decoded_w_ && decoded_h_) {
            // Icons show their real pixels (shrunk only when they do not fit).
            const float dw = static_cast<float>(decoded_w_), dh = static_cast<float>(decoded_h_);
            const float fit = (std::min)({(content.right - content.left) / dw, (content.bottom - content.top) / dh, 1.0f});
            const float cx = std::round((content.left + content.right - dw * fit) * 0.5f);
            const float cy = std::round((content.top + content.bottom - dh * fit) * 0.5f);
            draw = D2D1::RectF(cx, cy, cx + std::round(dw * fit), cy + std::round(dh * fit));
        }
        const bool icon_unsized = icon && image_fit_ && (!decoded_w_ || !decoded_h_);
        dc->PushAxisAlignedClip(content, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const bool backdrop = !icon && !folder && image_item_ && pages_count_ <= 1 &&
                              DrawImageBackdrop(dc, content, draw);
        if (table_.TakePendingSheetRequest(sheet_request_)) {
            preview_text_.clear();
            preview_text_identity_.Reset();
            find_matches_.clear();
            preview_notice_.clear();
        }
        const uint32_t requested = TableFileKind(item_.path) == 2 ? sheet_request_ :
            icon ? icon_request_ : waiting_for_frame_ ? requested_frame_ : frame_index_;
        if (icon && !icon_sizes_.empty() && draw.right - draw.left < 8192.0f && draw.bottom - draw.top < 8192.0f) {
            // Transparent icon pixels sit on a checkerboard (mockup ⑥).
            ComPtr<ID2D1SolidColorBrush> cell;
            dc->CreateSolidColorBrush(D2D1::ColorF(0x808080, 0.18f), &cell);
            const float step = 8.0f * scale_;
            int row = 0;
            if (cell.get())
                for (float y = draw.top; y < draw.bottom; y += step, ++row)
                    for (float x = draw.left + (row % 2 ? step : 0.0f); x < draw.right; x += step * 2.0f)
                        dc->FillRectangle(D2D1::RectF(x, y, (std::min)(x + step, draw.right),
                                                      (std::min)(y + step, draw.bottom)), cell.get());
        }
        auto draw_at = [&](uint32_t pixels, uint32_t frame) {
            // A turned picture draws upright into the unturned box, under a rotation.
            D2D1_RECT_F box = draw;
            D2D1_MATRIX_3X2_F saved{};
            const bool turned = image_turns_ != 0 && !icon && image_item_;
            if (turned) {
                dc->GetTransform(&saved);
                const D2D1_POINT_2F c = D2D1::Point2F((draw.left + draw.right) * 0.5f, (draw.top + draw.bottom) * 0.5f);
                if (image_turns_ & 1) {
                    const float hw = (draw.bottom - draw.top) * 0.5f, hh = (draw.right - draw.left) * 0.5f;
                    box = D2D1::RectF(c.x - hw, c.y - hh, c.x + hw, c.y + hh);
                }
                dc->SetTransform(D2D1::Matrix3x2F::Rotation(90.0f * static_cast<float>(image_turns_), c) * saved);
            }
            const auto contributed = plugin_preview ? native_preview_.Draw(owner_, hwnd_, dc, box,
                item_.path, item_.modified, item_.size, pixels, shared_payload, truncated, integrity,
                decoded_w_, decoded_h_, dark_, accent_, scale_) : std::optional<PreviewDrawResult>{};
            if (contributed) {
                source_w_ = decoded_w_; source_h_ = decoded_h_;
                if (turned) dc->SetTransform(saved);
                return *contributed;
            }
            const PreviewDrawResult drawn = thumbnails_.Draw(dc, box, item_.path, item_.attrs, pixels,
                generation_, item_.modified, item_.size, 1.0f, nullptr, &truncated,
                &bytes_read, true, &error, nullptr, nullptr, nullptr, nullptr,
                frame, &reported_frame_count, &reported_delay_ms, &reported_loop_count,
                &decoded_w_, &decoded_h_, &source_w_, &source_h_, nullptr, &text_encoding,
                false, nullptr, nullptr, &integrity, {}, &shared_payload);
            if (turned) dc->SetTransform(saved);
            return drawn;
        };
        PreviewDrawResult result = draw_at(want, requested);
        if (!plugin_preview && result == PreviewDrawResult::Pending && preview_pixels_ != 0 &&
            preview_pixels_ != want) {
            result = draw_at(preview_pixels_, requested);
        } else if (result != PreviewDrawResult::Pending) {
            preview_pixels_ = want;
        }
        if (folder && result == PreviewDrawResult::Archive &&
            shared_payload && shared_payload->rfind(L"PULSEARC\t1\tDIR\t-\t2", 0) == 0) {
            // The quick pass ran out of time: the full count replaces it when ready.
            std::shared_ptr<const std::wstring> full_text;
            std::wstring full_error;
            bool full_truncated = false;
            uint32_t full_bytes = 0;
            if (thumbnails_.Draw(dc, draw, item_.path, item_.attrs, want, generation_,
                    item_.modified, item_.size, 1.0f, nullptr, &full_truncated, &full_bytes,
                    true, &full_error, nullptr, nullptr, nullptr, nullptr, 1,
                    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                    nullptr, nullptr, false, nullptr, nullptr, &integrity, {}, &full_text) ==
                PreviewDrawResult::Archive) {
                shared_payload = std::move(full_text);
                error = std::move(full_error);
            }
        }
        const std::wstring empty_text;
        const std::wstring& text = shared_payload ? *shared_payload : empty_text;
        if (result != PreviewDrawResult::Pending) {
            // The picture cards explain these failures themselves.
            const bool codec_card = kImageExtras && result == PreviewDrawResult::Failed &&
                (error.rfind(L"image-codec-missing:", 0) == 0 || error == L"image-pack-missing");
            const auto notice = codec_card ? std::wstring{} : PreviewNotice(error, text, integrity);
            if (notice != preview_notice_) {
                preview_notice_ = notice;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        bool committed_frame = false;
        if (result == PreviewDrawResult::Bitmap && reported_frame_count > 1 &&
            reported_delay_ms == 0) {
            // Paged document (PDF): switch to the page reader from the next frame.
            pages_count_ = reported_frame_count;
            page_aspect_.assign(pages_count_, 0.0f);
            if (source_w_ && source_h_)
                page_aspect_[0] = static_cast<float>(source_h_) / static_cast<float>(source_w_);
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (result == PreviewDrawResult::Bitmap && reported_frame_count > 1 &&
                   (kImageExtras || ExtensionLabel(item_.path) == L"GIF")) {
            if (frame_count_ <= 1) {
                InvalidateRect(hwnd_, nullptr, FALSE);
                // It animates: 适应内容 makes room for the transport bar it now shows.
                fit_w_ = source_w_ ? source_w_ : decoded_w_;
                fit_h_ = source_w_ ? source_h_ : decoded_h_;
                if (fit_w_ && fit_h_) SetTimer(hwnd_, kFitTimer, 1, nullptr);
            }
            frame_count_ = reported_frame_count;
            frame_delay_ms_ = std::clamp(reported_delay_ms, 20u, 2000u);
            loop_count_ = reported_loop_count;
            if (waiting_for_frame_) {
                frame_index_ = requested_frame_;
                waiting_for_frame_ = false;
                committed_frame = true;
            }
            if (!animation_started_) {
                animation_started_ = true;
                animation_active_ = true;
                committed_frame = true;
            }
            if (committed_frame && animation_active_ && !playback_scrub_pending_)
                SetTimer(hwnd_, kAnimationTimer, frame_delay_ms_, nullptr);
        } else if (result == PreviewDrawResult::Pending && animation_started_) {
            std::wstring ignored_text;
            std::wstring ignored_error;
            bool ignored_truncated = false;
            uint32_t ignored_bytes = 0;
            uint32_t ignored_count = 1, ignored_delay = 0, ignored_loop = 0;
            const auto fallback = thumbnails_.Draw(dc, draw, item_.path, item_.attrs,
                preview_pixels_ ? preview_pixels_ : want, generation_,
                item_.modified, item_.size, 1.0f, &ignored_text, &ignored_truncated,
                &ignored_bytes, true, &ignored_error, nullptr, nullptr, nullptr, nullptr,
                frame_index_, &ignored_count, &ignored_delay, &ignored_loop,
                &decoded_w_, &decoded_h_, &source_w_, &source_h_);
            // Keep the displayed frame's HUD while the next frame is decoding.
            if (fallback == PreviewDrawResult::Bitmap) result = fallback;
            status.clear();
        } else if (result != PreviewDrawResult::Pending) {
            ResetAnimation();
        }
        if (result == PreviewDrawResult::Custom) {
            native_kind_ = NativeKind::Model;
        } else if (result == PreviewDrawResult::Bitmap) {
            native_kind_ = NativeKind::Bitmap;
            if (fit_pending_ && image_item_ && !icon) {
                // 适应内容 for a picture whose header could not be read.
                fit_pending_ = false;
                fit_w_ = source_w_ ? source_w_ : decoded_w_;
                fit_h_ = source_w_ ? source_h_ : decoded_h_;
                if (fit_w_ && fit_h_) SetTimer(hwnd_, kFitTimer, 1, nullptr);
            }
            SetImageFooter(content);
            // The first decoded frame tells the backdrop where (and whether) to draw.
            if (!backdrop && !icon && image_item_ && ImageBackdropWanted())
                InvalidateRect(hwnd_, nullptr, FALSE);
            if (icon) {
                ParseIconSizes(text);
                DrawIconSizes(dc, content, text_brush.get());
                if (icon_unsized) InvalidateRect(hwnd_, nullptr, FALSE);  // lay out at the real size
            }
        } else if (result == PreviewDrawResult::Archive &&
                   archive_.SetPayload(shared_payload, PreviewCardName(item_), item_.size)) {
            native_kind_ = NativeKind::Archive;
            archive_.Draw(dc, &compositor_, content, CurrentTheme(), scale_, true);
            SetFooterParts(archive_.StatusParts());
            footer_hint_ = archive_.Hint();
        } else if (result == PreviewDrawResult::Markdown && markdown_.SetPayload(shared_payload, item_.path) &&
                   (!markdown_.DocFormat().empty() ||
                    !(markdown_.IsNotebook() ? notebook_source_ : markdown_source_))) {
            // DOCX / EPUB have no source view; DOCX may switch to the handler.
            const bool document = !markdown_.DocFormat().empty();
            markdown_shown_ = !document ||
                (markdown_.DocFormat() == L"docx" && PreviewHandlerHost::CanHost(item_.path));
            native_kind_ = NativeKind::Markdown;
            if (markdown_.IsNotebook()) toggle_kind_ = ToggleKind::NotebookSource;
            if (document) toggle_kind_ = ToggleKind::DocHandler;
            if (preview_text_identity_.Changed(&markdown_, markdown_.TextRevision(), markdown_.PlainText(), preview_text_)) {
                preview_text_ = markdown_.PlainText();
                plain_highlight_lines_.Reset();
                ResetTextDrawingCache();
                text_layout_.reset();
                sel_anchor_ = sel_focus_ = 0;
                if (find_open_ || !search_query_.empty()) UpdateFindMatches();
            }
            const SearchHighlights marks(find_matches_, static_cast<uint32_t>(find_query_.size()), find_index_);
            markdown_.SetTocHidden(toc_hidden_);
            if (markdown_.HasToc() && content.right - content.left >= 640.0f * scale_)
                AddFooterControl(kFooterToc, pulse::l10n::Get(toc_hidden_
                    ? pulse::l10n::StringId::PreviewShowToc : pulse::l10n::StringId::PreviewHideToc).c_str());
            if (markdown_.Draw(dc, compositor_.DwriteFactory(), content, CurrentTheme(), dark_,
                               scale_, &thumbnails_, generation_,
                               (std::min)(sel_anchor_, sel_focus_),
                               (std::max)(sel_anchor_, sel_focus_), marks))
                InvalidateRect(hwnd_, nullptr, FALSE);
            if (markdown_.IsNotebook()) {
                // Jupyter · Python 3  |  12 个单元格  |  86 KB
                std::vector<std::wstring> parts;
                parts.push_back(markdown_.NotebookKernel().empty()
                                    ? std::wstring(L"Jupyter")
                                    : L"Jupyter \x00B7 " + markdown_.NotebookKernel());
                parts.push_back(std::to_wstring(markdown_.NotebookCells()) +
                                (pulse::l10n::Get(pulse::l10n::StringId::Cells).c_str()));
                std::wstring size = pulse::format::ByteSize(bytes_read, true);
                if (truncated) size += pulse::l10n::Get(pulse::l10n::StringId::Truncated).c_str();
                parts.push_back(std::move(size));
                SetFooterParts(std::move(parts));
            } else if (markdown_.DocFormat() == L"docx") {
                // Word 文档  |  约 1,240 字  |  38 KB
                const bool zh = pulse::l10n::IsChinese();
                std::vector<std::wstring> parts;
                parts.push_back(pulse::l10n::Get(pulse::l10n::StringId::WordDocument).c_str());
                const std::wstring words = GroupedNumber(markdown_.WordCount());
                parts.push_back(zh ? pulse::l10n::Cn(L"\x7EA6 ") + words + pulse::l10n::Cn(L" \x5B57") : L"about " + words + L" words");
                std::wstring size = pulse::format::ByteSize(item_.size ? item_.size : bytes_read, true);
                if (truncated) size += pulse::l10n::Get(pulse::l10n::StringId::Truncated).c_str();
                parts.push_back(std::move(size));
                SetFooterParts(std::move(parts));
            } else if (markdown_.DocFormat() == L"epub") {
                // EPUB  |  第 1 / 27 章  |  author
                const bool zh = pulse::l10n::IsChinese();
                std::vector<std::wstring> parts;
                parts.push_back(L"EPUB");
                const int count = (std::max)(1, markdown_.SectionCount());
                const std::wstring at = std::to_wstring(markdown_.SectionIndex() + 1) + L" / " + std::to_wstring(count);
                parts.push_back(zh ? pulse::l10n::Cn(L"\x7B2C ") + at + pulse::l10n::Cn(L" \x7AE0") : L"Chapter " + at);
                std::wstring who = markdown_.DocAuthor();
                if (who.size() > 40) who = who.substr(0, 39) + L"\x2026";
                if (!who.empty()) parts.push_back(std::move(who));
                if (truncated) parts.push_back(pulse::l10n::Get(pulse::l10n::StringId::Truncated_2).c_str());
                SetFooterParts(std::move(parts));
            } else if (markdown_.DocFormat().empty()) {
                // Markdown  |  12 个标题  |  约 3,400 字  |  14 KB
                std::vector<std::wstring> parts;
                parts.push_back(L"Markdown");
                wchar_t buffer[96];
                if (markdown_.HeadingCount()) {
                    swprintf_s(buffer, pulse::l10n::Get(pulse::l10n::StringId::PreviewHeadings).c_str(),
                               markdown_.HeadingCount());
                    parts.push_back(buffer);
                }
                if (markdown_.WordCount()) {
                    swprintf_s(buffer, pulse::l10n::Get(pulse::l10n::StringId::PreviewAboutWords).c_str(),
                               GroupedNumber(markdown_.WordCount()).c_str());
                    parts.push_back(buffer);
                }
                std::wstring size = pulse::format::ByteSize(item_.size ? item_.size : bytes_read, true);
                if (truncated) size += pulse::l10n::Get(pulse::l10n::StringId::Truncated).c_str();
                parts.push_back(std::move(size));
                SetFooterParts(std::move(parts));
            }
        } else if (result == PreviewDrawResult::Markdown && markdown_.SetPayload(shared_payload, item_.path)) {
            // Source mode: the highlighted text view with its status pill.
            markdown_shown_ = true;
            if (markdown_.IsNotebook()) toggle_kind_ = ToggleKind::NotebookSource;
            native_kind_ = NativeKind::Text;
            const bool changed = preview_text_identity_.Changed(&markdown_, markdown_.TextRevision(),
                markdown_.Source(), preview_text_, true);
            EnsureTextLayout(markdown_.Source(), false, (std::max)(1.0f, width - pad * 2.0f), false, !changed);
            if (changed) {
                sel_anchor_ = sel_focus_ = 0;
                if (find_open_ || !search_query_.empty()) UpdateFindMatches();
            }
            DrawTextPreview(dc, content, text_brush.get());
            SetTextFooter(false, bytes_read, truncated, text_encoding);
        } else if (result == PreviewDrawResult::Table && table_.SetPayload(shared_payload)) {
            const bool sheet = table_.IsSpreadsheet();
            if (kTableView && (!sheet || PreviewHandlerHost::CanHost(item_.path))) {
                markdown_shown_ = true;
                toggle_kind_ = sheet ? ToggleKind::TableHandler : ToggleKind::TableSource;
            }
            if (!sheet && (!kTableView || table_source_)) {
                // Source mode: the text view with its status pill.
                native_kind_ = NativeKind::Text;
                const bool changed = preview_text_identity_.Changed(&table_, table_.TextRevision(),
                    table_.Source(), preview_text_, true);
                EnsureTextLayout(table_.Source(), false, (std::max)(1.0f, width - pad * 2.0f), false, !changed);
                if (changed) {
                    sel_anchor_ = sel_focus_ = 0;
                    if (find_open_ || !search_query_.empty()) UpdateFindMatches();
                }
                DrawTextPreview(dc, content, text_brush.get());
                SetTextFooter(false, bytes_read, truncated, text_encoding);
            } else {
                native_kind_ = NativeKind::Table;
                if (preview_text_identity_.Changed(&table_, table_.TextRevision(), table_.PlainText(), preview_text_)) {
                    preview_text_ = table_.PlainText();
                    plain_highlight_lines_.Reset();
                    ResetTextDrawingCache();
                    text_layout_.reset();
                    sel_anchor_ = sel_focus_ = 0;
                    if (find_open_ || !search_query_.empty()) UpdateFindMatches();
                }
                const SearchHighlights marks(find_matches_, static_cast<uint32_t>(find_query_.size()), find_index_);
                table_.SetFooterTabs(true);
                table_.Draw(dc, compositor_.DwriteFactory(), content, CurrentTheme(), dark_, scale_,
                            background, marks);
                SetTableFooter(text_encoding);
            }
        } else if (result == PreviewDrawResult::Tree && tree_.SetPayload(shared_payload)) {
            // The Tree | Source pill only when there is a tree to go back to.
            markdown_shown_ = kTreeView && !tree_.HasError();
            toggle_kind_ = ToggleKind::TreeSource;
            if (!kTreeView || tree_.HasError() || tree_source_) {
                native_kind_ = NativeKind::Text;
                const bool changed = preview_text_identity_.Changed(&tree_, tree_.TextRevision(),
                    tree_.Source(), preview_text_, true);
                EnsureTextLayout(tree_.Source(), false, (std::max)(1.0f, width - pad * 2.0f), false, !changed);
                if (changed) {
                    sel_anchor_ = sel_focus_ = 0;
                    if (find_open_ || !search_query_.empty()) UpdateFindMatches();
                }
                DrawTextPreview(dc, content, text_brush.get());
                if (tree_.HasError() && kTreeView) {
                    // Malformed: the source, with where it broke in the pill.
                    std::vector<std::wstring> parts;
                    parts.push_back(tree_.IsXml() ? L"XML" : L"JSON");
                    parts.push_back(tree_.ErrorText());
                    parts.push_back(pulse::format::ByteSize(bytes_read, true));
                    SetFooterParts(std::move(parts));
                } else {
                    SetTextFooter(false, bytes_read, truncated, text_encoding);
                }
            } else {
                native_kind_ = NativeKind::Tree;
                if (preview_text_identity_.Changed(&tree_, tree_.TextRevision(), tree_.PlainText(), preview_text_)) {
                    preview_text_ = tree_.PlainText();
                    plain_highlight_lines_.Reset();
                    ResetTextDrawingCache();
                    text_layout_.reset();
                    sel_anchor_ = sel_focus_ = 0;
                    if (find_open_ || !search_query_.empty()) UpdateFindMatches();
                }
                const SearchHighlights marks(find_matches_, static_cast<uint32_t>(find_query_.size()), find_index_);
                tree_.Draw(dc, compositor_.DwriteFactory(), content, CurrentTheme(), dark_, scale_, marks);
                if (tree_.HasCurrent())
                    AddFooterControl(kFooterCopyPath, pulse::l10n::Get(pulse::l10n::StringId::CopyPath).c_str());
                AddFooterControl(kFooterFoldAll, pulse::l10n::Get(tree_.AnyCollapsed()
                    ? pulse::l10n::StringId::PreviewExpandAll : pulse::l10n::StringId::PreviewCollapseAll).c_str());
                auto parts = tree_.StatusParts();
                std::wstring size = pulse::format::ByteSize(bytes_read, true);
                if (truncated) size += pulse::l10n::Get(pulse::l10n::StringId::Truncated).c_str();
                parts.push_back(std::move(size));
                SetFooterParts(std::move(parts));
            }
        } else if (result == PreviewDrawResult::Text || result == PreviewDrawResult::Hex ||
                   result == PreviewDrawResult::Archive) {
            native_kind_ = result == PreviewDrawResult::Hex ? NativeKind::Hex : NativeKind::Text;
            bool reading = false;
            if (result == PreviewDrawResult::Text && PlainTextCandidate()) {
                markdown_shown_ = true;
                toggle_kind_ = ToggleKind::TextReading;
                reading = !ToggleSecond();
            }
            preview_text_identity_.Reset();
            EnsureTextLayout(text, result == PreviewDrawResult::Hex,
                             (std::max)(1.0f, width - pad * 2.0f), reading);
            DrawTextPreview(dc, content, text_brush.get());
            SetTextFooter(result == PreviewDrawResult::Hex, bytes_read, truncated, text_encoding, true);
            if (text_layout_reading_) {
                static constexpr pulse::l10n::StringId kWidths[] = {
                    pulse::l10n::StringId::PreviewWidthNarrow, pulse::l10n::StringId::PreviewWidthMedium,
                    pulse::l10n::StringId::PreviewWidthWide};
                AddFooterControl(kFooterReadWidth,
                                 pulse::l10n::Get(kWidths[std::clamp(reading_width_, 0, 2)]).c_str());
            }
        } else if (TableFileKind(item_.path) == 2 && table_.HasData() &&
                   (result == PreviewDrawResult::Pending || result == PreviewDrawResult::Failed)) {
            native_kind_ = NativeKind::Table;
            if (result == PreviewDrawResult::Failed) table_.FailPendingSheetRequest(sheet_request_);
            table_.SetFooterTabs(true);
            table_.Draw(dc, compositor_.DwriteFactory(), content, CurrentTheme(), dark_, scale_, background, {});
            SetTableFooter(text_encoding);
        } else if (result == PreviewDrawResult::Pending && !animation_started_) {
            native_kind_ = NativeKind::None;
            status = pulse::l10n::Get(pulse::l10n::StringId::PreviewLoading);
        } else if (result == PreviewDrawResult::Failed && kImageExtras &&
                   error.rfind(L"image-codec-missing:", 0) == 0) {
            native_kind_ = NativeKind::None;
            DrawImageCodecCard(dc, content, error.substr(20), text_brush.get());
        } else if (result == PreviewDrawResult::Failed && kImageExtras && error == L"image-pack-missing") {
            native_kind_ = NativeKind::None;
            DrawImageCodecCard(dc, content, L"pack", text_brush.get());
        } else if (result == PreviewDrawResult::Failed) {
            native_kind_ = NativeKind::None;
            status = pulse::l10n::Get(error == L"archive-plugin-required" ? pulse::l10n::StringId::ArchivePluginRequired : error == L"path-unavailable"
                ? pulse::l10n::StringId::PreviewFileUnavailable
                : pulse::l10n::StringId::PreviewCannotRender);
        }
        dc->PopAxisAlignedClip();
        if (FindBarHeight() > 0.0f) {
            DrawFindBar(dc, FindBarRect());
        }
    }
    if (pending_search_reveal_ && !find_matches_.empty()) {
        pending_search_reveal_ = false;
        ScrollMatchIntoView(find_matches_.front());
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    DrawPlayback(dc, text_brush.get());
    if (!status.empty()) {
        compositor_.TextFormat()->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        compositor_.TextFormat()->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawTextW(status.data(), static_cast<UINT32>(status.size()),
            compositor_.TextFormat(), ContentRect(), secondary_brush.get());
        compositor_.TextFormat()->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        compositor_.TextFormat()->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (nav_layer) {
        dc->PopLayer();
        dc->SetTransform(nav_saved);
    }
    // The shell stays put while the content slides.
    LayoutHeader();
    DrawInfoDrawer(dc, palette);
    if (!fullscreen_) {
        DrawHeader(dc, palette);
        DrawFooter(dc, palette);
    }
    SchedulePreviewDeadline();
    {
        // Frame building only; EndDraw's flush and Present wait on the GPU and vsync.
        LARGE_INTEGER now{}, freq{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        last_render_ms_ = static_cast<double>(now.QuadPart - render_start.QuadPart) * 1000.0 /
                          static_cast<double>(freq.QuadPart);
    }
    const HRESULT result = dc->EndDraw();
    if (result == D2DERR_RECREATE_TARGET || result == DXGI_ERROR_DEVICE_REMOVED ||
        result == DXGI_ERROR_DEVICE_RESET) compositor_.NotifyDeviceLost(result);
    else {
#if defined(PULSE_PICKER_INPROCESS_PROOF)
        if (native_kind_ != NativeKind::None) SetPropW(hwnd_, L"Pulse.PreviewTestContentReady", reinterpret_cast<HANDLE>(1));
        else RemovePropW(hwnd_, L"Pulse.PreviewTestContentReady");
        if (const auto snapshot = reinterpret_cast<const wchar_t*>(RemovePropW(hwnd_, L"Pulse.PreviewTestSnapshot")))
            compositor_.SaveSnapshot(snapshot);
#endif
        compositor_.Present();
        if (zoom_pending_) {
            // Start after the first frame of the new item is on screen.
            zoom_pending_ = false;
            if (!compositor_.PlayZoom(static_cast<float>(zoom_origin_.x),
                                      static_cast<float>(zoom_origin_.y), true,
                                      kZoomOpenSeconds))
                compositor_.ClearZoom();
        }
    }
    if (find_open_ && (native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex ||
                       native_kind_ == NativeKind::Archive || native_kind_ == NativeKind::Markdown ||
                       native_kind_ == NativeKind::Table || native_kind_ == NativeKind::Tree))
        LayoutFindEdit();
    else if (find_edit_)
        ShowWindow(find_edit_, SW_HIDE);
}

LRESULT CALLBACK QuickPreviewWindow::WndProc(HWND hwnd, UINT message,
                                              WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<QuickPreviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<QuickPreviewWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    return self ? self->HandleMessage(message, wparam, lparam)
                : DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT QuickPreviewWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_NCCALCSIZE:
        if (wparam) return 0;
        break;
    case WM_NCHITTEST: {
        if (embedded_ || fullscreen_) return HTCLIENT;   // no frame to drag or size
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd_, &point);
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int border = (std::max)(6, static_cast<int>(8.0f * scale_));
        const bool left = point.x < border;
        const bool right = point.x >= client.right - border;
        const bool top = point.y < border;
        const bool bottom = point.y >= client.bottom - border;
        if (!IsZoomed(hwnd_)) {
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        if (point.y >= 0 && point.y < static_cast<int>(HeaderHeight())) {
            // The whole header drags the window except its controls.
            if (header_.close.right > header_.close.left && point.x >= static_cast<int>(header_.close.left))
                return HTCLOSE;
            if (HeaderInteractive(point)) return HTCLIENT;
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lparam) == HTCLIENT) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(hwnd_, &point);
            SetCursor(ContentCursor(point));
            return TRUE;
        }
        break;
    }
    case WM_CREATE:
        scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
        if (!compositor_.Init(hwnd_)) return -1;
        compositor_.RecreateTextFormats(scale_);
        RecreateFormats();
        thumbnails_.SetDeviceContext(compositor_.Dc());
        thumbnails_.SetNotifyWindow(hwnd_);
        open_icons_.SetDeviceContext(compositor_.Dc());
        open_icons_.SetNotifyWindow(hwnd_);
        markdown_.SetNotifyWindow(hwnd_);
        handler_.SetNotifyWindow(hwnd_);
        return 0;
    case WM_SIZE:
        Resize();
        if (find_open_) LayoutFindEdit();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_MOVE:
        compositor_.UpdateTextRenderingParams(
            MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
        handler_.Reposition();
        if (find_open_) LayoutFindEdit();
        return 0;
    case WM_DPICHANGED: {
        scale_ = static_cast<float>(HIWORD(wparam)) / 96.0f;
        compositor_.RecreateTextFormats(scale_);
        RecreateFormats();
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        if (find_open_) LayoutFindEdit();
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd_, &paint);
        Render();
        EndPaint(hwnd_, &paint);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        POINT cursor{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd_, &cursor);
        if (PlaybackWheel(cursor, steps)) {
            return 0;
        }
        if (PagesWheel(cursor, steps, (GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL) != 0,
                       (GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT) != 0)) {
            // handled by the page reader
        } else if (native_kind_ == NativeKind::Bitmap) {
            ZoomAt(static_cast<float>(cursor.x), static_cast<float>(cursor.y),
                   std::pow(1.15f, steps));
        } else if (native_kind_ == NativeKind::Archive) {
            archive_.Scroll(steps);
            archive_.Hover(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
        } else if (native_kind_ == NativeKind::Markdown) {
            if ((GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT) != 0) markdown_.ScrollHorizontal(steps);
            else markdown_.ScrollAt(static_cast<float>(cursor.x), static_cast<float>(cursor.y), steps);
        } else if (native_kind_ == NativeKind::Table) {
            table_.Scroll(steps, (GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT) != 0);
            KillTimer(hwnd_, kTableTipTimer);
        } else if (native_kind_ == NativeKind::Tree) {
            tree_.Scroll(steps);
            tree_.Hover(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
        } else {
            text_scroll_ = (std::max)(0.0f, text_scroll_ - steps * 56.0f * scale_);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEHWHEEL: {
        // Tilt wheel / touchpad sideways: the table grid scrolls horizontally.
        if (native_kind_ != NativeKind::Table && native_kind_ != NativeKind::Markdown) break;
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        const bool changed = native_kind_ == NativeKind::Table ? table_.Scroll(-steps, true) : markdown_.ScrollHorizontal(-steps);
        if (changed) InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT point{};
        ClientPoint(lparam, point);
        if (footer_focus_) {
            footer_focus_ = 0;   // the pointer takes over from the keyboard
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (PlaybackMouseDown(point)) return 0;
        if (const ChromeButton button = HitChromeButton(point); button != ChromeButton::None) {
            if (GetFocus() != hwnd_) SetFocus(hwnd_);  // provider may hold focus
            ActivateChromeButton(button);
            return 0;
        }
        if (const int control = HitFooterControl(point); control != 0) {
            ActivateFooterControl(control);
            return 0;
        }
        if (CodecCardClick(point)) return 0;
        if (const int size = HitIconSize(point); size >= 0) {
            if (static_cast<uint32_t>(size + 1) != icon_request_ && size != icon_selected_) {
                icon_request_ = static_cast<uint32_t>(size + 1);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        if (const int segment = HitMarkdownToggle(point); segment >= 0) {
            bool& second = ToggleSecond();
            if ((segment == 1) != second) {
                second = segment == 1;
                selecting_ = false;
                sel_anchor_ = sel_focus_ = 0;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        if (find_open_ && FindBarHeight() > 0.0f) {
            const D2D1_RECT_F bar = FindBarRect();
            if (point.x >= bar.left && point.x < bar.right &&
                point.y >= bar.top && point.y < bar.bottom) {
                if (find_edit_) SetFocus(find_edit_);
                return 0;
            }
        }
        if (native_kind_ == NativeKind::Table && table_.IsSpreadsheet()) {
            // Sheet tabs live in the footer.
            const D2D1_RECT_F bar = FooterRect();
            if (point.x >= bar.left && point.x < bar.right && point.y >= bar.top && point.y < bar.bottom) {
                if (table_.TabClick(static_cast<float>(point.x), static_cast<float>(point.y)))
                    InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }
        const D2D1_RECT_F content = ContentRect();
        if (point.x < content.left || point.x >= content.right ||
            point.y < content.top || point.y >= content.bottom) return 0;
        if (native_kind_ == NativeKind::Archive) {
            if (archive_.Click(static_cast<float>(point.x), static_cast<float>(point.y)))
                InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (native_kind_ == NativeKind::Table) {
            KillTimer(hwnd_, kTableTipTimer);
            if (table_.MouseDown(static_cast<float>(point.x), static_cast<float>(point.y),
                                 (wparam & MK_SHIFT) != 0)) {
                if (table_.Dragging()) SetCapture(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        if (native_kind_ == NativeKind::Tree) {
            if (tree_.MouseDown(static_cast<float>(point.x), static_cast<float>(point.y))) {
                if (tree_.Dragging()) SetCapture(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        if (native_kind_ == NativeKind::Markdown &&
            markdown_.ScrollbarMouseDown(static_cast<float>(point.x), static_cast<float>(point.y))) {
            selecting_ = false;
            if (markdown_.DraggingScrollbar()) SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (native_kind_ == NativeKind::Markdown &&
            markdown_.Click(static_cast<float>(point.x), static_cast<float>(point.y))) {
            // Contents entry or next-chapter link: the click is spent.
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (AudioMouseDown(point)) return 0;
        if (PagesMouseDown(point)) {
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (video_.active() && !VideoPreview::IsAudio(item_.path)) {
            TogglePlayback();   // a click on the picture pauses / resumes, like K
            return 0;
        }
        SetCapture(hwnd_);
        if (native_kind_ == NativeKind::Bitmap && CanPanImage()) {
            panning_ = true;
            pan_anchor_ = point;
            pan_start_x_ = pan_x_;
            pan_start_y_ = pan_y_;
        } else if (native_kind_ == NativeKind::Markdown && (wparam & MK_CONTROL) &&
                   !markdown_.LinkAt(static_cast<float>(point.x),
                                     static_cast<float>(point.y)).empty()) {
            ReleaseCapture();
            const std::wstring target =
                markdown_.LinkAt(static_cast<float>(point.x), static_cast<float>(point.y));
            // Only web and mail links; local paths and other schemes stay inert.
            if (_wcsnicmp(target.c_str(), L"http://", 7) == 0 ||
                _wcsnicmp(target.c_str(), L"https://", 8) == 0 ||
                _wcsnicmp(target.c_str(), L"mailto:", 7) == 0)
                ShellExecuteW(hwnd_, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex ||
                   native_kind_ == NativeKind::Markdown) {
            uint32_t index = 0;
            if (HitTestText(static_cast<float>(point.x), static_cast<float>(point.y), index)) {
                selecting_ = true;
                sel_anchor_ = sel_focus_ = index;
                if (native_kind_ == NativeKind::Hex) hex_selected_ = HexByteAt(index);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        POINT point{};
        ClientPoint(lparam, point);
        if (PlaybackMouseDown(point)) return 0;
        if (const ChromeButton button = HitChromeButton(point); button != ChromeButton::None) {
            ActivateChromeButton(button);  // rapid clicks keep stepping through files
            return 0;
        }
        const D2D1_RECT_F content = ContentRect();
        if (native_kind_ == NativeKind::Markdown &&
            markdown_.ScrollbarMouseDown(static_cast<float>(point.x), static_cast<float>(point.y))) {
            selecting_ = false;
            if (markdown_.DraggingScrollbar()) SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (native_kind_ == NativeKind::Archive) {
            // Folder listing: double-click opens the item in the main window.
            if (archive_.IsFolderListing() && archive_.HasSelection() &&
                archive_.Contains(static_cast<float>(point.x), static_cast<float>(point.y)) &&
                OpenListingSelection())
                return 0;
            // Second click of a fast pair keeps toggling like a single click.
            if (archive_.Click(static_cast<float>(point.x), static_cast<float>(point.y)))
                InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (native_kind_ == NativeKind::Pages) {
            const PagesLayout pages = ComputePagesLayout();
            if (point.x >= pages.view.left && point.x < pages.view.right &&
                point.y >= pages.view.top && point.y < pages.view.bottom) {
                TogglePagesFit();
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else {
                PagesMouseDown(point);  // fast clicks in the strip keep jumping
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        if (video_.active() && !VideoPreview::IsAudio(item_.path) && !CodecCardClick(point) &&
            point.x >= content.left && point.x < content.right &&
            point.y >= content.top && point.y < content.bottom) {
            TogglePlayback();   // the second click of a fast pair is a click too
            return 0;
        }
        if (native_kind_ == NativeKind::Bitmap &&
            point.x >= content.left && point.x < content.right &&
            point.y >= content.top && point.y < content.bottom) {
            ToggleFitActual();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT point{};
        ClientPoint(lparam, point);
        if (markdown_.DraggingScrollbar()) {
            if (!(wparam & MK_LBUTTON)) {
                markdown_.EndScrollbarDrag();
                if (GetCapture() == hwnd_) ReleaseCapture();
            } else if (markdown_.ScrollbarMouseMove(static_cast<float>(point.x), static_cast<float>(point.y))) {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        if (playback_volume_drag_) {
            DragPlaybackVolume(static_cast<float>(point.x));
            return 0;
        }
        if (playback_drag_) {
            SeekPlayback(static_cast<float>(point.x));
            return 0;
        }
        if (PlaybackHover(point)) InvalidateRect(hwnd_, nullptr, FALSE);
        if (const ChromeButton hover = (panning_ || selecting_) ? ChromeButton::None
                                                                 : HitChromeButton(point);
            hover != chrome_hover_) {
            chrome_hover_ = hover;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (const int hover = (panning_ || selecting_) ? 0 : HitFooterControl(point);
            hover != footer_hover_) {
            footer_hover_ = hover;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (!mouse_tracking_) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd_, 0};
            mouse_tracking_ = TrackMouseEvent(&tracking) != FALSE;
        }
        if (native_kind_ == NativeKind::Archive && !panning_ && !selecting_ &&
            archive_.Hover(static_cast<float>(point.x), static_cast<float>(point.y)))
            InvalidateRect(hwnd_, nullptr, FALSE);
        if (native_kind_ == NativeKind::Table) {
            const float x = static_cast<float>(point.x), y = static_cast<float>(point.y);
            if (table_.Dragging()) {
                if (!(wparam & MK_LBUTTON)) {
                    table_.MouseUp();
                    if (GetCapture() == hwnd_) ReleaseCapture();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                } else if (table_.MouseMove(x, y)) InvalidateRect(hwnd_, nullptr, FALSE);
            } else {
                if (table_.Hover(x, y)) InvalidateRect(hwnd_, nullptr, FALSE);
                // Full text of a clipped cell once the pointer rests on it.
                if (table_.TipArmed()) SetTimer(hwnd_, kTableTipTimer, 500, nullptr);
                else KillTimer(hwnd_, kTableTipTimer);
            }
        }
        if (native_kind_ == NativeKind::Tree) {
            const float x = static_cast<float>(point.x), y = static_cast<float>(point.y);
            if (tree_.Dragging() ? tree_.MouseMove(x, y) : tree_.Hover(x, y))
                InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (native_kind_ == NativeKind::Hex && !selecting_) {
            const D2D1_RECT_F content = ContentRect();
            const float x = static_cast<float>(point.x), y = static_cast<float>(point.y);
            uint32_t index = 0;
            int hover = -1;
            if (x >= content.left && x < content.right &&
                y >= content.top + 20.0f * scale_ + HexRulerHeight() && y < content.bottom &&
                HitTestText(x, y, index))
                hover = HexByteAt(index);
            if (hover != hex_hover_) {
                hex_hover_ = hover;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        if (panning_ && native_kind_ == NativeKind::Pages) {
            pages_pan_x_ = pan_start_x_ - static_cast<float>(point.x - pan_anchor_.x);
            pages_scroll_ = pan_start_y_ - static_cast<float>(point.y - pan_anchor_.y);
            ClampPages(ComputePagesLayout());
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (panning_) {
            pan_x_ = pan_start_x_ + static_cast<float>(point.x - pan_anchor_.x);
            pan_y_ = pan_start_y_ + static_cast<float>(point.y - pan_anchor_.y);
            const D2D1_RECT_F content = ContentRect();
            ClampPan(content.right - content.left, content.bottom - content.top);
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (selecting_) {
            uint32_t index = 0;
            if (HitTestText(static_cast<float>(point.x), static_cast<float>(point.y), index)) {
                sel_focus_ = index;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        mouse_tracking_ = false;
        if (archive_.Leave()) InvalidateRect(hwnd_, nullptr, FALSE);
        KillTimer(hwnd_, kTableTipTimer);
        if (table_.Leave()) InvalidateRect(hwnd_, nullptr, FALSE);
        if (tree_.Leave()) InvalidateRect(hwnd_, nullptr, FALSE);
        if (hex_hover_ >= 0) {
            hex_hover_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (PlaybackHover(POINT{-1, -1})) InvalidateRect(hwnd_, nullptr, FALSE);
        if (chrome_hover_ != ChromeButton::None || footer_hover_ != 0) {
            chrome_hover_ = ChromeButton::None;
            footer_hover_ = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        if (message == WM_LBUTTONUP && playback_drag_)
            SeekPlayback(static_cast<float>(GET_X_LPARAM(lparam)));
        if (message == WM_LBUTTONUP && playback_volume_drag_)
            DragPlaybackVolume(static_cast<float>(GET_X_LPARAM(lparam)));
        playback_volume_drag_ = false;
        EndPlaybackDrag(message == WM_LBUTTONUP);
        panning_ = false;
        selecting_ = false;
        if (markdown_.DraggingScrollbar()) {
            if (message == WM_LBUTTONUP)
                markdown_.ScrollbarMouseMove(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            markdown_.EndScrollbarDrag();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (table_.Dragging()) {
            table_.MouseUp();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (tree_.Dragging()) {
            tree_.MouseUp();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if ((message == WM_LBUTTONUP || message == WM_CANCELMODE) && GetCapture() == hwnd_) ReleaseCapture();
        return 0;
    case WM_RBUTTONUP: {
        POINT point{};
        ClientPoint(lparam, point);
        if (find_open_ && FindBarHeight() > 0.0f) {
            const D2D1_RECT_F bar = FindBarRect();
            if (point.x >= bar.left && point.x < bar.right &&
                point.y >= bar.top && point.y < bar.bottom) {
                if (find_edit_) SetFocus(find_edit_);
                return 0;
            }
        }
        const D2D1_RECT_F content = ContentRect();
        const bool in_content = point.x >= content.left && point.x < content.right &&
            point.y >= content.top && point.y < content.bottom;
        if (native_kind_ == NativeKind::Archive && in_content) {
            archive_.SelectAt(static_cast<float>(point.x), static_cast<float>(point.y));
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if ((native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex ||
             native_kind_ == NativeKind::Markdown) && in_content) {
            uint32_t index = 0;
            if (HitTestText(static_cast<float>(point.x), static_cast<float>(point.y), index)) {
                const uint32_t a = (std::min)(sel_anchor_, sel_focus_);
                const uint32_t b = (std::max)(sel_anchor_, sel_focus_);
                if (!(b > a && index >= a && index < b))
                    sel_anchor_ = sel_focus_ = index;
            }
        }
        POINT screen = point;
        ClientToScreen(hwnd_, &screen);
        ShowContextMenu(screen);
        return 0;
    }
    case WM_CONTEXTMENU: {
        // Also reached from the preview-handler overlay (forwarded by post);
        // never keep a cross-thread sender waiting on the modal menu.
        if (InSendMessage()) ReplyMessage(0);
        POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (screen.x == -1 && screen.y == -1) {
            const D2D1_RECT_F content = ContentRect();
            screen.x = static_cast<LONG>((content.left + content.right) * 0.5f);
            screen.y = static_cast<LONG>((content.top + content.bottom) * 0.5f);
            ClientToScreen(hwnd_, &screen);
        } else {
            POINT client = screen;
            ScreenToClient(hwnd_, &client);
            const D2D1_RECT_F content = ContentRect();
            if (client.x < content.left || client.x >= content.right ||
                client.y < content.top || client.y >= content.bottom)
                break;  // caption right-click keeps the system menu
            if (native_kind_ == NativeKind::Archive) {
                archive_.SelectAt(static_cast<float>(client.x), static_cast<float>(client.y));
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        ShowContextMenu(screen);
        return 0;
    }
    case WM_TIMER:
        if (wparam == kPreviewDeadlineTimer) {
            KillTimer(hwnd_, kPreviewDeadlineTimer);
            if (preview_deadline_) {
                preview_deadline_ = 0;
                if (!closing_ && visible()) InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        if (wparam == kTableTipTimer) {
            KillTimer(hwnd_, kTableTipTimer);
            if (native_kind_ == NativeKind::Table && table_.ShowTip())
                InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (wparam == kZoomCloseTimer) {
            if (closing_) FinishClose(false);
            else KillTimer(hwnd_, kZoomCloseTimer);
            return 0;
        }
        if (wparam == kFitTimer) {
            KillTimer(hwnd_, kFitTimer);
            FitToContent(fit_w_, fit_h_);
            return 0;
        }
        if (wparam == kNavSlideTimer) {
            if (nav_dir_ != 0 && visible()) InvalidateRect(hwnd_, nullptr, FALSE);
            else KillTimer(hwnd_, kNavSlideTimer);
            return 0;
        }
        if (wparam == 9) {
            TickPlaybackSeek();
            return 0;
        }
        if (wparam == 8 && video_.active()) {
            if (fit_pending_ || VideoLoops()) {
                const auto state = video_.Snapshot();
                if (fit_pending_ && state.ready && state.width && state.height) {
                    fit_pending_ = false;
                    FitToContent(state.width, state.height);
                }
                // 循环播放: start again from the beginning once it ends.
                const ULONGLONG now = GetTickCount64();
                if (VideoLoops() && state.ready && state.ended && SUCCEEDED(state.error) &&
                    now - video_loop_at_ > 500) {
                    video_loop_at_ = now;
                    video_.Seek(0.0);
                    video_.Play(true);
                }
            }
            if (visible() && !IsIconic(hwnd_)) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (wparam == kAnimationTimer && animation_active_ && visible()) {
            if (frame_count_ > 1 && !waiting_for_frame_ && !playback_scrub_pending_) {
                if (frame_index_ + 1 >= frame_count_) {
                    // 循环播放 off: finish this pass and rest on the last frame (most
                    // files ask to loop forever, so the file's own count cannot decide).
                    if (!AnimationLoops()) {
                        animation_active_ = false;
                        KillTimer(hwnd_, kAnimationTimer);
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        return 0;
                    }
                    requested_frame_ = 0;
                    ++completed_loops_;
                } else {
                    requested_frame_ = frame_index_ + 1;
                }
                waiting_for_frame_ = true;
                KillTimer(hwnd_, kAnimationTimer);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        break;
    case WM_NCMOUSEMOVE:
        if (const bool hovered = wparam == HTCLOSE; hovered != close_hover_) {
            close_hover_ = hovered;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE | TME_NONCLIENT, hwnd_, 0};
            TrackMouseEvent(&tracking);
        }
        return 0;
    case WM_NCMOUSELEAVE:
        if (close_hover_) {
            close_hover_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_NCLBUTTONDOWN:
        if (wparam == HTCLOSE) {
            Close();
            return 0;
        }
        break;
    case WM_NCLBUTTONDBLCLK:
        if (wparam == HTCAPTION) {
            ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
            return 0;
        }
        break;
    case WM_ENTERSIZEMOVE:
        GetWindowRect(hwnd_, &size_move_start_);
        break;
    case WM_EXITSIZEMOVE: {
        // A size dragged by hand: kept for this preview, and next time while
        // 记住我拖出来的大小 is on. Moving alone changes nothing.
        RECT now{};
        GetWindowRect(hwnd_, &now);
        const bool resized = now.right - now.left != size_move_start_.right - size_move_start_.left ||
                             now.bottom - now.top != size_move_start_.bottom - size_move_start_.top;
        if (resized && !fullscreen_ && !IsZoomed(hwnd_)) {
            user_sized_ = true;
            fit_pending_ = false;
            if (options_.remember_size) {
                options_.remembered = now;
                ReportOptions();
            }
        }
        break;
    }
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMinTrackSize.x = static_cast<LONG>(480.0f * scale_);
        limits->ptMinTrackSize.y = static_cast<LONG>(320.0f * scale_);
        return 0;
    }
    case WM_COMMAND:
        if (find_edit_ && lparam == reinterpret_cast<LPARAM>(find_edit_) &&
            HIWORD(wparam) == EN_CHANGE) {
            SyncFindFromEdit();
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT: {
        if (find_edit_ && reinterpret_cast<HWND>(lparam) == find_edit_) {
            if (!find_edit_brush_) find_edit_brush_ = CreateSolidBrush(FindEditBrushColor(dark_));
            SetTextColor(reinterpret_cast<HDC>(wparam), EditTextColor(dark_));
            SetBkColor(reinterpret_cast<HDC>(wparam), EditBackColor(dark_));
            SetBkMode(reinterpret_cast<HDC>(wparam), OPAQUE);
            return reinterpret_cast<LRESULT>(EditBackBrush(find_edit_brush_));
        }
        break;
    }
    case WM_CHAR:
        if (find_open_ && find_edit_ && wparam >= 32 && wparam != 127) {
            if (GetFocus() != find_edit_) SetFocus(find_edit_);
            SendMessageW(find_edit_, WM_CHAR, wparam, lparam);
        }
        return 0;
    case WM_KEYDOWN: {
        if (embedded_ && !find_open_ && (wparam == VK_ESCAPE || wparam == VK_SPACE)) {
            if (wparam == VK_SPACE && HasPlayback()) {
                if (!(lparam & (1LL << 30))) TogglePlayback();
            } else if (owner_) SetFocus(owner_);
            return 0;
        }
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (wparam == VK_TAB && !find_open_ && !ctrl) {
            if (CycleFooterFocus(shift)) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (footer_focus_ && !find_open_ &&
            std::any_of(footer_controls_.begin(), footer_controls_.end(),
                        [this](const FooterControl& c) { return c.id == footer_focus_; })) {
            if (wparam == VK_RETURN || wparam == VK_SPACE) {
                if (!(lparam & (1LL << 30))) ActivateFooterControl(footer_focus_);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_ESCAPE) {
                footer_focus_ = 0;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }
        if (wparam == VK_ESCAPE) {
            if (find_open_) CloseFind();
            else if (fullscreen_) LeaveFullScreen(true);   // Esc leaves full screen first
            else Close();
            return 0;
        }
        const bool repeat = (lparam & (1LL << 30)) != 0;
        if (!find_open_ && ctrl && !shift &&
            ((wparam >= '1' && wparam <= '3') || (wparam >= VK_NUMPAD1 && wparam <= VK_NUMPAD3))) {
            // 标准 / 大 / 最大化, like the header's size menu.
            const int step = static_cast<int>(wparam >= VK_NUMPAD1 ? wparam - VK_NUMPAD1 : wparam - '1');
            if (!repeat) ChooseSizeMode(static_cast<int>(PreviewSizeMode::Standard) + step);
            return 0;
        }
        if (!find_open_ && !ctrl && !shift && wparam == 'F') {
            if (!repeat) {
                if (fullscreen_) LeaveFullScreen(true);
                else EnterFullScreen();
            }
            return 0;
        }
        if (!find_open_ && HasPlayback() && !ctrl) {
            if (wparam == VK_SPACE) {
                // 空格键 setting: close like on every other file, or play / pause.
                if (options_.space_closes) Close();
                else if (!repeat) TogglePlayback();
                return 0;
            }
            if (wparam == 'K') {
                if (!repeat) TogglePlayback();
                return 0;
            }
            if (wparam == 'L') {
                if (!repeat) ToggleLoop();
                return 0;
            }
            if (wparam == VK_OEM_COMMA || wparam == VK_OEM_PERIOD) {
                StepPlayback(wparam == VK_OEM_COMMA ? -1 : 1);
                return 0;
            }
            if (wparam == VK_HOME || wparam == VK_END) {
                if (video_.active()) video_.Play(false);
                animation_active_ = false;
                KillTimer(hwnd_, kAnimationTimer);
                const auto track = PlaybackTrackRect();
                SeekPlayback(wparam == VK_HOME ? track.left : track.right);
                return 0;
            }
        }
        if (wparam == VK_SPACE && !find_open_) {
            Close();
            return 0;
        }
        const bool text_kind = native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex ||
                               native_kind_ == NativeKind::Markdown ||
                               native_kind_ == NativeKind::Table ||
                               native_kind_ == NativeKind::Tree;
        if (native_kind_ == NativeKind::Tree && !find_open_ && ctrl && shift && wparam == 'C' &&
            tree_.HasCurrent()) {
            // The node's path ($.items[3].name) rather than the file's.
            pulse::ops::WriteClipboardText(tree_.CurrentPath());
            return 0;
        }
        if (!find_open_ && command_message_) {
            // File verbs on the previewed entry (same keys as the main list).
            QuickPreviewAction action = QuickPreviewAction::None;
            if (ctrl && shift && wparam == 'C') action = QuickPreviewAction::CopyPath;
            else if (ctrl && wparam == 'C' && !text_kind) action = QuickPreviewAction::Copy;
            else if (ctrl && wparam == 'X') action = QuickPreviewAction::Cut;
            else if (wparam == VK_DELETE) action = QuickPreviewAction::Delete;
            else if (wparam == VK_F2) action = QuickPreviewAction::Rename;
            if (action != QuickPreviewAction::None) {
                PostAction(action);
                return 0;
            }
        }
        if (native_kind_ == NativeKind::Archive && !find_open_) {
            // With a row selected, Left/Right fold the tree; otherwise they
            // keep stepping through files. Paging keys scroll the tree.
            const bool fold = (wparam == VK_LEFT || wparam == VK_RIGHT) && archive_.HasSelection();
            const bool page = wparam == VK_PRIOR || wparam == VK_NEXT ||
                              ((wparam == VK_HOME || wparam == VK_END) && !HasPlayback());
            if (fold || page) {
                archive_.Key(static_cast<UINT>(wparam));
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }
        if (native_kind_ == NativeKind::Archive && find_open_ && wparam == VK_RETURN)
            return 0;
        if (native_kind_ == NativeKind::Table && !find_open_ &&
            table_.Key(static_cast<UINT>(wparam), shift, ctrl)) {
            KillTimer(hwnd_, kTableTipTimer);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (native_kind_ == NativeKind::Tree && !find_open_ && !ctrl &&
            !(HasPlayback() && (wparam == VK_HOME || wparam == VK_END)) &&
            tree_.Key(static_cast<UINT>(wparam))) {
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (native_kind_ == NativeKind::Markdown && !find_open_ && !ctrl &&
            (wparam == VK_PRIOR || wparam == VK_NEXT || wparam == VK_LEFT || wparam == VK_RIGHT ||
             ((wparam == VK_HOME || wparam == VK_END) && !HasPlayback()))) {
            // Left/Right scroll a wide document sideways; when there is
            // nothing to scroll they keep stepping through files.
            const bool moved = markdown_.Key(static_cast<UINT>(wparam));
            if (moved || (wparam != VK_LEFT && wparam != VK_RIGHT)) {
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }
        if (ctrl && wparam == 'F' &&
            (native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex ||
             native_kind_ == NativeKind::Archive || native_kind_ == NativeKind::Markdown ||
             native_kind_ == NativeKind::Table || native_kind_ == NativeKind::Tree)) {
            OpenFind();
            return 0;
        }
        if (ctrl && wparam == 'C' && text_kind) {
            CopyTextSelection(false);
            return 0;
        }
        if (ctrl && wparam == 'A' && text_kind) {
            SelectAllText();
            return 0;
        }
        if ((wparam == VK_F3 || (find_open_ && wparam == VK_RETURN)) && text_kind) {
            if (!find_open_) OpenFind();
            FindNext(shift ? -1 : 1);
            return 0;
        }
        if (!find_open_ && !ctrl && (native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex) &&
            (wparam == VK_PRIOR || wparam == VK_NEXT ||
             ((wparam == VK_HOME || wparam == VK_END) && !HasPlayback()))) {
            // Keyboard scrolling; the draw clamps to the document's end.
            const D2D1_RECT_F content = ContentRect();
            const float page = (std::max)(40.0f * scale_, (content.bottom - content.top) * 0.9f);
            if (wparam == VK_PRIOR) text_scroll_ = (std::max)(0.0f, text_scroll_ - page);
            else if (wparam == VK_NEXT) text_scroll_ += page;
            else text_scroll_ = wparam == VK_HOME ? 0.0f : 1.0e9f;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (!find_open_ && !ctrl && PagesKey(wparam)) {
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (!find_open_ && native_kind_ == NativeKind::Bitmap) {
            if (wparam == '1' || wparam == VK_NUMPAD1) {
                SetFitMode();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wparam == '2' || wparam == VK_NUMPAD2) {
                SetActualPixels();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (!ctrl && (wparam == 'I' || wparam == 'B' || wparam == 'R' || wparam == VK_OEM_PLUS ||
                          wparam == VK_ADD || wparam == VK_OEM_MINUS || wparam == VK_SUBTRACT)) {
                ActivateFooterControl(wparam == 'I' ? kFooterInfo : wparam == 'B' ? kFooterImageBg :
                    wparam == 'R' ? kFooterRotate :
                    (wparam == VK_OEM_PLUS || wparam == VK_ADD) ? kFooterZoomIn : kFooterZoomOut);
                return 0;
            }
        }
        if (wparam == VK_RETURN && native_kind_ == NativeKind::Archive && !find_open_ &&
            OpenListingSelection())
            return 0;
        if (wparam == VK_RETURN && owner_ && open_message_ && !find_open_)
            PostMessageW(owner_, open_message_, 0, 0);
        else if (wparam == VK_LEFT || wparam == VK_UP)
            PostNavigate(-1);
        else if (wparam == VK_RIGHT || wparam == VK_DOWN)
            PostNavigate(1);
        return 0;
    }
    case WM_SYSKEYDOWN:
        if (wparam == VK_RETURN && (lparam & (1 << 29)) && !find_open_) {
            PostAction(QuickPreviewAction::Properties);  // Alt+Enter → properties
            return 0;
        }
        break;
    case WM_ACTIVATE: {
        const bool active = LOWORD(wparam) != WA_INACTIVE;
        handler_.NotifyAppActivate(active);
        if (!active) {
            HWND other = reinterpret_cast<HWND>(lparam);
            if (find_edit_ && other != find_edit_ && other != hwnd_) {
                const HWND other_owner = other ? GetWindow(other, GW_OWNER) : nullptr;
                if (other_owner != hwnd_)
                    ShowWindow(find_edit_, SW_HIDE);
            }
        } else if (find_open_) {
            LayoutFindEdit();
        }
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        RefreshHighContrast();
        if (!embedded_) ApplyWindowEffect(hwnd_, effect_, dark_);
        if (find_edit_brush_) {
            DeleteObject(find_edit_brush_);
            find_edit_brush_ = nullptr;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_CLOSE:
        Close();
        return 0;
    case WM_DESTROY:
        ResetPlayback();
        ResetAnimation();
        handler_.Reset();
        thumbnails_.Reset();
        DestroyFindEdit();
        close_format_.reset();
        preview_text_format_.reset();
        plain_highlight_lines_.Reset();
        ResetTextDrawingCache();
        text_layout_.reset();
        compositor_.Shutdown();
        hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

} // namespace pulse::ui

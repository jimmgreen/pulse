#include "edit_host.h"
#include "legacy_icons.h"
#include "../common/windows_compat.h"
#include "quick_preview_window.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../ipc/preview_protocol.h"
#include "../ops/clipboard.h"
#include "fluent_components.h"
#include "lumatext_renderer.h"
#include "typography.h"

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

constexpr wchar_t kWindowClass[] = L"Pulse.QuickPreview";
constexpr DWORD kRecallOnOpen = 0x00040000;
constexpr DWORD kRecallOnData = 0x00400000;
constexpr DWORD kPinned = 0x00080000;
constexpr float kCloseButtonWidth = 46.0f;
constexpr float kCloseGlyphSize = 16.0f * 0.66f;
constexpr float kFindBarHeight = 44.0f;
constexpr float kHudHeight = 28.0f;
constexpr UINT_PTR kAnimationTimer = 7;
constexpr UINT_PTR kFindEditCaretTimer = 71;
constexpr float kMaxDecodedZoom = 4.0f;
constexpr size_t kFindQueryLimit = 256;
constexpr wchar_t kSearchGlyph[] = L"\xE721";
constexpr wchar_t kCopyGlyph[] = L"\xE8C8";
constexpr wchar_t kSelectAllGlyph[] = L"\xE8B3";

enum {
    kTextCmdCopy = 1,
    kTextCmdSelectAll,
    kTextCmdFind,
};

std::wstring ExtensionLabel(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    std::wstring ext = path.substr(dot + 1);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towupper(c));
    return ext;
}

COLORREF FindEditBgColor(bool dark) noexcept {
    return dark ? RGB(30, 30, 30) : RGB(255, 255, 255);
}

COLORREF FindEditFgColor(bool dark) noexcept {
    return dark ? RGB(255, 255, 255) : RGB(26, 26, 26);
}

D2D1_COLOR_F FindEditBg(bool dark) noexcept {
    return dark ? D2D1::ColorF(30.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f)
                : D2D1::ColorF(1.0f, 1.0f, 1.0f);
}

D2D1_COLOR_F FindEditFg(bool dark) noexcept {
    return dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f)
                : D2D1::ColorF(26.0f / 255.0f, 26.0f / 255.0f, 26.0f / 255.0f);
}

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

QuickPreviewWindow::~QuickPreviewWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
}

bool QuickPreviewWindow::Initialize(HWND owner, UINT navigate_message, UINT open_message) {
    if (hwnd_) return true;
    owner_ = owner;
    navigate_message_ = navigate_message;
    open_message_ = open_message;
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
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 680, owner, nullptr, window_class.hInstance, this);
    return hwnd_ != nullptr;
}

void QuickPreviewWindow::ResetTextState() {
    preview_text_.clear();
    text_layout_.reset();
    text_layout_width_ = 0.0f;
    text_layout_scale_ = 0.0f;
    text_layout_hex_ = false;
    sel_anchor_ = sel_focus_ = 0;
    selecting_ = false;
    CloseFind();
}

void QuickPreviewWindow::ResetView() {
    text_scroll_ = 0.0f;
    pan_x_ = 0.0f;
    pan_y_ = 0.0f;
    image_fit_ = true;
    image_zoom_ = 1.0f;
    decoded_w_ = decoded_h_ = 0;
    source_w_ = source_h_ = 0;
    native_kind_ = NativeKind::None;
    panning_ = false;
    ResetTextState();
}

void QuickPreviewWindow::ResetAnimation() {
    if (hwnd_) KillTimer(hwnd_, kAnimationTimer);
    frame_index_ = 0; requested_frame_ = 0; frame_count_ = 1; frame_delay_ms_ = 0;
    loop_count_ = 0; completed_loops_ = 0;
    animation_active_ = false; waiting_for_frame_ = false; animation_started_ = false;
}

void QuickPreviewWindow::RecreateFormats() {
    close_format_.reset();
    preview_text_format_.reset();
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
    find_painter_.SetCompositor(&compositor_);
    find_painter_.SetScale(scale_);
    if (find_edit_font_) {
        DeleteObject(find_edit_font_);
        find_edit_font_ = nullptr;
    }
    const int height = -std::max(1, static_cast<int>(std::lround(14.0f * scale_)));
    find_edit_font_ = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, typography::PreferredTextFamily());
    if (find_edit_ && find_edit_font_)
        SendMessageW(find_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(find_edit_font_), TRUE);
    if (find_edit_brush_) {
        DeleteObject(find_edit_brush_);
        find_edit_brush_ = nullptr;
    }
}

void QuickPreviewWindow::Show(const QuickPreviewItem& item, bool dark, WindowEffect effect,
                              bool safe_mode) {
    if (!hwnd_ || item.path.empty()) return;
    item_ = item;
    dark_ = dark;
    effect_ = effect;
    safe_mode_ = safe_mode;
    handler_immediate_ = true;
    ApplyWindowEffect(hwnd_, effect_, dark_);
    if (find_edit_brush_) {
        DeleteObject(find_edit_brush_);
        find_edit_brush_ = nullptr;
    }
    ++generation_;
    ResetView();
    ResetAnimation();
    RECT owner_rect{};
    GetWindowRect(owner_, &owner_rect);
    const int owner_width = owner_rect.right - owner_rect.left;
    const int owner_height = owner_rect.bottom - owner_rect.top;
    const int width = std::clamp(static_cast<int>(owner_width * 0.82), 640, 1280);
    const int height = std::clamp(static_cast<int>(owner_height * 0.82), 480, 900);
    const int x = owner_rect.left + (owner_width - width) / 2;
    const int y = owner_rect.top + (owner_height - height) / 2;
    SetWindowTextW(hwnd_, item_.name.empty()
        ? pulse::l10n::Get(pulse::l10n::StringId::QuickPreview).c_str() : item_.name.c_str());
    SetWindowPos(hwnd_, HWND_TOP, x, y, width, height, SWP_SHOWWINDOW);
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::Update(const QuickPreviewItem& item) {
    if (!visible() || item.path.empty()) return;
    item_ = item;
    ++generation_;
    handler_immediate_ = false;
    ResetView();
    ResetAnimation();
    handler_.Reset();
    SetWindowTextW(hwnd_, item_.name.empty()
        ? pulse::l10n::Get(pulse::l10n::StringId::QuickPreview).c_str() : item_.name.c_str());
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::Close() {
    if (!hwnd_) return;
    handler_.Hide();
    ResetAnimation();
    ResetView();
    thumbnails_.Evict();
    preview_pixels_ = 0;
    close_hover_ = false;
    ShowWindow(hwnd_, SW_HIDE);
    if (owner_) SetForegroundWindow(owner_);
}

bool QuickPreviewWindow::visible() const noexcept {
    return hwnd_ && IsWindowVisible(hwnd_) != FALSE;
}

bool QuickPreviewWindow::OfflinePlaceholder() const noexcept {
    return (item_.attrs & (kRecallOnOpen | kRecallOnData)) && !(item_.attrs & kPinned);
}

void QuickPreviewWindow::Resize() {
    if (!hwnd_ || !compositor_.Dc()) return;
    RECT rect{};
    GetClientRect(hwnd_, &rect);
    compositor_.Resize((std::max)(1L, rect.right), (std::max)(1L, rect.bottom));
}

float QuickPreviewWindow::FindBarHeight() const noexcept {
    return find_open_ && (native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex)
        ? kFindBarHeight * scale_ : 0.0f;
}

D2D1_RECT_F QuickPreviewWindow::FindBarRect() const {
    const float width = static_cast<float>(compositor_.Width());
    const float header = kTitleBarHeight * scale_;
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
    const float header = kTitleBarHeight * scale_;
    return D2D1::RectF(0, header + FindBarHeight(), width, height);
}

float QuickPreviewWindow::FitScale(float view_w, float view_h) const noexcept {
    if (decoded_w_ == 0 || decoded_h_ == 0) return 1.0f;
    return (std::min)(view_w / static_cast<float>(decoded_w_),
                      view_h / static_cast<float>(decoded_h_));
}

bool QuickPreviewWindow::CanPanImage() const {
    if (image_fit_ || native_kind_ != NativeKind::Bitmap || decoded_w_ == 0) return false;
    const D2D1_RECT_F content = ContentRect();
    const float view_w = content.right - content.left;
    const float view_h = content.bottom - content.top;
    return decoded_w_ * image_zoom_ > view_w + 0.5f ||
           decoded_h_ * image_zoom_ > view_h + 0.5f;
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
    const float draw_w = decoded_w_ * image_zoom_;
    const float draw_h = decoded_h_ * image_zoom_;
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
    const float draw_w = decoded_w_ * old_zoom;
    const float draw_h = decoded_h_ * old_zoom;
    const float left = content.left + (view_w - draw_w) * 0.5f + pan_x_;
    const float top = content.top + (view_h - draw_h) * 0.5f + pan_y_;
    const float u = (cursor_x - left) / (std::max)(0.001f, old_zoom);
    const float v = (cursor_y - top) / (std::max)(0.001f, old_zoom);
    image_fit_ = false;
    image_zoom_ = next;
    const float new_w = decoded_w_ * next;
    const float new_h = decoded_h_ * next;
    pan_x_ = cursor_x - u * next - content.left - (view_w - new_w) * 0.5f;
    pan_y_ = cursor_y - v * next - content.top - (view_h - new_h) * 0.5f;
    ClampPan(view_w, view_h);
}

D2D1_RECT_F QuickPreviewWindow::ImageDest(const D2D1_RECT_F& content) const {
    if (image_fit_ || decoded_w_ == 0 || decoded_h_ == 0) return content;
    const float view_w = content.right - content.left;
    const float view_h = content.bottom - content.top;
    const float draw_w = decoded_w_ * image_zoom_;
    const float draw_h = decoded_h_ * image_zoom_;
    const float left = content.left + (view_w - draw_w) * 0.5f + pan_x_;
    const float top = content.top + (view_h - draw_h) * 0.5f + pan_y_;
    return D2D1::RectF(left, top, left + draw_w, top + draw_h);
}

uint32_t QuickPreviewWindow::RequestedPixelSize(const D2D1_RECT_F& content) const {
    const float longest = (std::max)(content.right - content.left, content.bottom - content.top);
    return ipc::BucketPreviewPixelSize(static_cast<uint32_t>((std::max)(1.0f, longest)));
}

void QuickPreviewWindow::DrawHud(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                                 ID2D1SolidColorBrush* text_brush) {
    if (!dc || native_kind_ != NativeKind::Bitmap || decoded_w_ == 0) return;
    const float hud_h = kHudHeight * scale_;
    if (content.bottom - content.top < hud_h + 8.0f * scale_) return;
    const D2D1_RECT_F hud = D2D1::RectF(content.left, content.bottom - hud_h,
                                        content.right, content.bottom);
    ComPtr<ID2D1SolidColorBrush> background;
    dc->CreateSolidColorBrush(D2D1::ColorF(0x000000, dark_ ? 0.45f : 0.28f), &background);
    dc->FillRectangle(hud, background.get());

    const uint32_t shown_w = source_w_ ? source_w_ : decoded_w_;
    const uint32_t shown_h = source_h_ ? source_h_ : decoded_h_;
    wchar_t dims[64];
    swprintf_s(dims, L"%u\x00D7%u", shown_w, shown_h);
    std::wstring line = dims;
    if (source_w_ && source_h_ &&
        (source_w_ != decoded_w_ || source_h_ != decoded_h_)) {
        wchar_t preview[64];
        swprintf_s(preview, pulse::l10n::Get(pulse::l10n::StringId::PreviewDecodedFormat).c_str(),
                   (std::max)(decoded_w_, decoded_h_));
        line += L" \x00B7 ";
        line += preview;
    }
    const std::wstring size = pulse::format::ByteSize(item_.size, true);
    if (!size.empty()) {
        line += L" \x00B7 ";
        line += size;
    }
    if (const std::wstring ext = ExtensionLabel(item_.path); !ext.empty()) {
        line += L" \x00B7 ";
        line += ext;
    }
    line += L" \x00B7 ";
    if (image_fit_) {
        line += pulse::l10n::Get(pulse::l10n::StringId::PreviewFit);
    } else {
        wchar_t percent[16];
        swprintf_s(percent, L"%.0f%%", image_zoom_ * 100.0f);
        line += percent;
    }
    const float pad = 12.0f * scale_;
    IDWriteTextFormat* format = compositor_.SmallFormat();
    if (format) {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawTextW(line.data(), static_cast<UINT32>(line.size()), format,
                      D2D1::RectF(hud.left + pad, hud.top, hud.right - pad, hud.bottom),
                      text_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

void QuickPreviewWindow::EnsureTextLayout(const std::wstring& text, bool hex, float width) {
    if (text.empty() || !compositor_.DwriteFactory() || !preview_text_format_.get()) {
        text_layout_.reset();
        return;
    }
    if (text_layout_.get() && text == preview_text_ && hex == text_layout_hex_ &&
        std::abs(width - text_layout_width_) < 0.5f &&
        std::abs(scale_ - text_layout_scale_) < 0.001f) return;
    preview_text_ = text;
    text_layout_.reset();
    preview_text_format_->SetWordWrapping(
        hex ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_WRAP);
    compositor_.DwriteFactory()->CreateTextLayout(
        text.data(), static_cast<UINT32>(text.size()), preview_text_format_.get(),
        (std::max)(1.0f, width), 100000.0f, &text_layout_);
    text_layout_width_ = width;
    text_layout_scale_ = scale_;
    text_layout_hex_ = hex;
}

bool QuickPreviewWindow::HitTestText(float x, float y, uint32_t& index) {
    if (!text_layout_.get()) return false;
    const D2D1_RECT_F content = ContentRect();
    const float pad = 20.0f * scale_;
    const float origin_x = pad;
    const float origin_y = content.top + pad - text_scroll_;
    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(text_layout_->HitTestPoint(x - origin_x, y - origin_y, &trailing, &inside,
                                          &metrics))) return false;
    index = metrics.textPosition + (trailing ? 1u : 0u);
    index = (std::min)(index, static_cast<uint32_t>(preview_text_.size()));
    return true;
}

bool QuickPreviewWindow::HasTextSelection() const noexcept {
    return sel_anchor_ != sel_focus_;
}

void QuickPreviewWindow::CopyTextSelection(bool require_selection) const {
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
    sel_anchor_ = 0;
    sel_focus_ = static_cast<uint32_t>(preview_text_.size());
    InvalidateRect(hwnd_, nullptr, FALSE);
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
    if (compositor_.LumaTextEnabled())
        PaintFindEditLuma(find_edit_, nullptr);
}

void QuickPreviewWindow::CloseFind() {
    find_open_ = false;
    find_query_.clear();
    find_matches_.clear();
    find_index_ = 0;
    if (find_edit_) {
        SetWindowTextW(find_edit_, L"");
        ShowWindow(find_edit_, SW_HIDE);
    }
    if (hwnd_ && IsWindowVisible(hwnd_)) {
        SetFocus(hwnd_);
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
    if (!compositor_.LumaTextEnabled())
        SetLayeredWindowAttributes(find_edit_, 0, 255, LWA_ALPHA);
    if (find_edit_font_)
        SendMessageW(find_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(find_edit_font_), TRUE);
    if (!find_edit_brush_)
        find_edit_brush_ = CreateSolidBrush(FindEditBgColor(dark_));
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
    int line_h = cell_h;
    if (find_edit_font_) {
        HDC hdc = GetDC(find_edit_);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, find_edit_font_));
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);
        SelectObject(hdc, old);
        ReleaseDC(find_edit_, hdc);
        line_h = (std::max)(1, static_cast<int>(tm.tmHeight));
    }
    line_h = (std::min)(line_h, cell_h);
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
    if (moved && compositor_.LumaTextEnabled())
        PaintFindEditLuma(find_edit_, nullptr);
}

void QuickPreviewWindow::SyncFindFromEdit() {
    if (!find_edit_) return;
    wchar_t buf[kFindQueryLimit + 1]{};
    GetWindowTextW(find_edit_, buf, ARRAYSIZE(buf));
    if (find_query_ == buf) return;
    find_query_ = buf;
    UpdateFindMatches();
    if (!find_matches_.empty()) {
        find_index_ = 0;
        ScrollMatchIntoView(find_matches_[0]);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::PaintFindEditLuma(HWND hwnd, HDC hdc) {
    if (!hwnd || !compositor_.LumaTextEnabled()) return;
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
        find_edit_brush_ = CreateSolidBrush(FindEditBgColor(dark_));
    FillRect(hdc, &rc, find_edit_brush_);
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
    const bool luma = self->compositor_.LumaTextEnabled();
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
                self->find_edit_brush_ = CreateSolidBrush(FindEditBgColor(self->dark_));
            FillRect(reinterpret_cast<HDC>(wparam), &rc, self->find_edit_brush_);
            return 1;
        }
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

void QuickPreviewWindow::ShowTextContextMenu(POINT screen) {
    if (native_kind_ != NativeKind::Text && native_kind_ != NativeKind::Hex) return;
    if (!text_menu_.Create(hwnd_, &compositor_, scale_)) return;
    text_menu_.SetTheme(dark_, HexColor(0x0078D4));
    std::vector<FluentMenuItem> items;
    FluentMenuItem copy;
    copy.command = kTextCmdCopy;
    copy.text = pulse::l10n::Get(pulse::l10n::StringId::Copy);
    copy.glyph = kCopyGlyph;
    copy.shortcut = L"Ctrl+C";
    copy.enabled = HasTextSelection();
    items.push_back(std::move(copy));
    FluentMenuItem select_all;
    select_all.command = kTextCmdSelectAll;
    select_all.text = pulse::l10n::Get(pulse::l10n::StringId::SelectAll);
    select_all.glyph = kSelectAllGlyph;
    select_all.shortcut = L"Ctrl+A";
    select_all.separator_after = true;
    items.push_back(std::move(select_all));
    FluentMenuItem find;
    find.command = kTextCmdFind;
    find.text = pulse::l10n::Get(pulse::l10n::StringId::Search);
    find.glyph = kSearchGlyph;
    find.shortcut = L"Ctrl+F";
    items.push_back(std::move(find));
    const int cmd = text_menu_.TrackPopup(screen, std::move(items));
    if (cmd == kTextCmdCopy) CopyTextSelection(true);
    else if (cmd == kTextCmdSelectAll) SelectAllText();
    else if (cmd == kTextCmdFind) OpenFind();
}

void QuickPreviewWindow::UpdateFindMatches() {
    find_matches_.clear();
    find_index_ = 0;
    if (find_query_.empty() || preview_text_.empty()) return;
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
}

void QuickPreviewWindow::ScrollMatchIntoView(uint32_t start) {
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

void QuickPreviewWindow::DrawFindBar(ID2D1DeviceContext* dc, const D2D1_RECT_F& bar) {
    if (!dc) return;
    const bool high_contrast = [] {
        HIGHCONTRASTW value{sizeof(value)};
        return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(value), &value, 0) &&
            (value.dwFlags & HCF_HIGHCONTRASTON);
    }();
    const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, HexColor(0x0078D4));
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
        if (find_matches_.empty()) swprintf_s(count, L"0/0");
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
    const float pad = 20.0f * scale_;
    const float origin_x = pad;
    const float origin_y = content.top + pad - text_scroll_;
    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(text_layout_->GetMetrics(&metrics))) {
        const float view = (std::max)(1.0f, content.bottom - content.top - pad);
        const float max_scroll = (std::max)(0.0f, metrics.height - view);
        text_scroll_ = std::clamp(text_scroll_, 0.0f, max_scroll);
    }
    if (find_open_ && !find_query_.empty() && !find_matches_.empty()) {
        ComPtr<ID2D1SolidColorBrush> match_brush;
        ComPtr<ID2D1SolidColorBrush> current_brush;
        dc->CreateSolidColorBrush(D2D1::ColorF(0xC19C00, 0.28f), &match_brush);
        dc->CreateSolidColorBrush(D2D1::ColorF(0xCA5010, 0.40f), &current_brush);
        const uint32_t length = static_cast<uint32_t>(find_query_.size());
        for (size_t i = 0; i < find_matches_.size(); ++i) {
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
    dc->DrawTextLayout(D2D1::Point2F(origin_x, origin_y), text_layout_.get(), text_brush,
                       D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

bool QuickPreviewWindow::ClientPoint(LPARAM lparam, POINT& out) const {
    out = POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    return true;
}

HCURSOR QuickPreviewWindow::ContentCursor(POINT client) const {
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
    if (native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex)
        return LoadCursorW(nullptr, IDC_IBEAM);
    if (panning_ || CanPanImage()) return LoadCursorW(nullptr, IDC_SIZEALL);
    return LoadCursorW(nullptr, IDC_ARROW);
}

void QuickPreviewWindow::Render() {
    if (!compositor_.Dc()) return;
    if (compositor_.NeedsRecovery()) {
        if (!compositor_.Recover()) return;
        compositor_.RecreateTextFormats(scale_);
        RecreateFormats();
        thumbnails_.SetDeviceContext(compositor_.Dc());
    }
    Resize();
    auto* dc = compositor_.Dc();
    dc->BeginDraw();
    const bool high_contrast = [] {
        HIGHCONTRASTW value{sizeof(value)};
        return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(value), &value, 0) &&
            (value.dwFlags & HCF_HIGHCONTRASTON);
    }();
    const D2D1_COLOR_F background = high_contrast
        ? D2D1::ColorF(GetSysColor(COLOR_WINDOW))
        : dark_ ? D2D1::ColorF(0x151515) : D2D1::ColorF(0xF7F7F7);
    const D2D1_COLOR_F foreground = high_contrast
        ? D2D1::ColorF(GetSysColor(COLOR_WINDOWTEXT))
        : dark_ ? D2D1::ColorF(0xF4F4F4) : D2D1::ColorF(0x202020);
    const D2D1_COLOR_F secondary = high_contrast ? foreground
        : dark_ ? D2D1::ColorF(0xB8B8B8) : D2D1::ColorF(0x606060);
    dc->Clear(background);
    ComPtr<ID2D1SolidColorBrush> text_brush;
    ComPtr<ID2D1SolidColorBrush> secondary_brush;
    dc->CreateSolidColorBrush(foreground, &text_brush);
    dc->CreateSolidColorBrush(secondary, &secondary_brush);
    const float width = static_cast<float>(compositor_.Width());
    const float height = static_cast<float>(compositor_.Height());
    const float pad = 20.0f * scale_;
    const float header = kTitleBarHeight * scale_;
    const D2D1_RECT_F title_rect = typography::SnapVerticalBounds(D2D1::RectF(
        pad, 8.0f * scale_, width - (kCloseButtonWidth + 8.0f) * scale_, header));
    dc->DrawTextW(item_.name.data(), static_cast<UINT32>(item_.name.size()),
                  compositor_.HeaderFormat(), title_rect, text_brush.get(),
                  D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    const D2D1_RECT_F close_rect = D2D1::RectF(width - kCloseButtonWidth * scale_, 0,
                                               width, header);
    if (close_hover_) {
        ComPtr<ID2D1SolidColorBrush> close_background;
        dc->CreateSolidColorBrush(D2D1::ColorF(0xC42B1C), &close_background);
        dc->FillRectangle(close_rect, close_background.get());
    }
    ComPtr<ID2D1SolidColorBrush> close_brush;
    dc->CreateSolidColorBrush(close_hover_ ? D2D1::ColorF(0xFFFFFF) : foreground,
                              &close_brush);
    static constexpr wchar_t close_glyph[] = L"\xE711";
    if (!DrawLegacyIcon(dc, compositor_.DwriteFactory(), close_glyph, close_rect, close_brush.get()))
        dc->DrawTextW(close_glyph, 1,
                  close_format_.get() ? close_format_.get() : compositor_.IconFormat(), close_rect,
                  close_brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
                  DWRITE_MEASURING_MODE_NATURAL);
    const D2D1_RECT_F full_content = D2D1::RectF(0, header, width, height);
    const bool offline = OfflinePlaceholder();
    const bool use_handler = !offline && !safe_mode_ &&
        PreviewHandlerHost::CanHost(item_.path);
    handler_.Sync(hwnd_, full_content, item_.path, item_.attrs, generation_, item_.modified,
                  item_.size, dark_, background, foreground, use_handler,
                  handler_immediate_);
    handler_immediate_ = false;
    std::wstring status;
    if (offline) {
        native_kind_ = NativeKind::None;
        status = pulse::l10n::Get(pulse::l10n::StringId::PreviewCloudOnly);
    } else if (use_handler) {
        native_kind_ = NativeKind::None;
        const auto state = handler_.state();
        if (state == PreviewHandlerHost::State::Loading ||
            state == PreviewHandlerHost::State::Idle)
            status = pulse::l10n::Get(pulse::l10n::StringId::PreviewLoading);
        else if (state == PreviewHandlerHost::State::Failed)
            status = pulse::l10n::Get(pulse::l10n::StringId::PreviewHostUnavailable);
    } else {
        std::wstring text;
        std::wstring error;
        bool truncated = false;
        uint32_t bytes_read = 0;
        uint32_t reported_frame_count = 1, reported_delay_ms = 0, reported_loop_count = 0;
        const D2D1_RECT_F content = ContentRect();
        const uint32_t want = RequestedPixelSize(content);
        const D2D1_RECT_F draw = ImageDest(content);
        dc->PushAxisAlignedClip(content, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const uint32_t requested = waiting_for_frame_ ? requested_frame_ : frame_index_;
        auto draw_at = [&](uint32_t pixels, uint32_t frame) {
            return thumbnails_.Draw(dc, draw, item_.path, item_.attrs, pixels,
                generation_, item_.modified, item_.size, 1.0f, &text, &truncated,
                &bytes_read, true, &error, nullptr, nullptr, nullptr, nullptr,
                frame, &reported_frame_count, &reported_delay_ms, &reported_loop_count,
                &decoded_w_, &decoded_h_, &source_w_, &source_h_);
        };
        PreviewDrawResult result = draw_at(want, requested);
        if (result == PreviewDrawResult::Pending && preview_pixels_ != 0 &&
            preview_pixels_ != want) {
            result = draw_at(preview_pixels_, requested);
        } else if (result != PreviewDrawResult::Pending) {
            preview_pixels_ = want;
        }
        bool committed_frame = false;
        if (result == PreviewDrawResult::Bitmap && reported_frame_count > 1) {
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
            if (committed_frame && animation_active_)
                SetTimer(hwnd_, kAnimationTimer, frame_delay_ms_, nullptr);
        } else if (result == PreviewDrawResult::Pending && animation_started_) {
            std::wstring ignored_text;
            std::wstring ignored_error;
            bool ignored_truncated = false;
            uint32_t ignored_bytes = 0;
            uint32_t ignored_count = 1, ignored_delay = 0, ignored_loop = 0;
            thumbnails_.Draw(dc, draw, item_.path, item_.attrs,
                preview_pixels_ ? preview_pixels_ : want, generation_,
                item_.modified, item_.size, 1.0f, &ignored_text, &ignored_truncated,
                &ignored_bytes, true, &ignored_error, nullptr, nullptr, nullptr, nullptr,
                frame_index_, &ignored_count, &ignored_delay, &ignored_loop,
                &decoded_w_, &decoded_h_, &source_w_, &source_h_);
            status.clear();
        } else if (result != PreviewDrawResult::Pending) {
            ResetAnimation();
        }
        if (result == PreviewDrawResult::Bitmap) {
            native_kind_ = NativeKind::Bitmap;
            DrawHud(dc, content, text_brush.get());
        } else if (result == PreviewDrawResult::Text || result == PreviewDrawResult::Hex) {
            native_kind_ = result == PreviewDrawResult::Hex ? NativeKind::Hex : NativeKind::Text;
            EnsureTextLayout(text, result == PreviewDrawResult::Hex,
                             (std::max)(1.0f, width - pad * 2.0f));
            DrawTextPreview(dc, content, text_brush.get());
            std::wstring footer = pulse::l10n::Get(result == PreviewDrawResult::Hex
                ? pulse::l10n::StringId::HexPrefix : pulse::l10n::StringId::TextPrefix);
            footer += pulse::format::ByteSize(bytes_read, true);
            if (truncated) footer += pulse::l10n::Get(pulse::l10n::StringId::TruncatedSuffix);
            IDWriteTextFormat* small_format = compositor_.SmallFormat();
            if (small_format) {
                small_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                small_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                dc->DrawTextW(footer.data(), static_cast<UINT32>(footer.size()), small_format,
                    D2D1::RectF(pad, content.bottom - 18.0f * scale_, width - pad,
                                content.bottom - 2.0f * scale_),
                    secondary_brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        } else if (result == PreviewDrawResult::Pending && !animation_started_) {
            native_kind_ = NativeKind::None;
            status = pulse::l10n::Get(pulse::l10n::StringId::PreviewLoading);
        } else if (result == PreviewDrawResult::Failed) {
            native_kind_ = NativeKind::None;
            status = pulse::l10n::Get(error == L"path-unavailable"
                ? pulse::l10n::StringId::PreviewFileUnavailable
                : pulse::l10n::StringId::PreviewCannotRender);
        }
        dc->PopAxisAlignedClip();
        if (FindBarHeight() > 0.0f) {
            DrawFindBar(dc, FindBarRect());
        }
    }
    if (!status.empty()) {
        compositor_.TextFormat()->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        compositor_.TextFormat()->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawTextW(status.data(), static_cast<UINT32>(status.size()),
            compositor_.TextFormat(), full_content, secondary_brush.get());
        compositor_.TextFormat()->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        compositor_.TextFormat()->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    const HRESULT result = dc->EndDraw();
    if (result == D2DERR_RECREATE_TARGET || result == DXGI_ERROR_DEVICE_REMOVED ||
        result == DXGI_ERROR_DEVICE_RESET) compositor_.NotifyDeviceLost(result);
    else compositor_.Present();
    if (find_open_ && (native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex))
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
        const int close_left = client.right - static_cast<int>(kCloseButtonWidth * scale_);
        if (point.y >= 0 && point.y < static_cast<int>(kTitleBarHeight * scale_)) {
            if (point.x >= close_left) return HTCLOSE;
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
        if (native_kind_ == NativeKind::Bitmap) {
            ZoomAt(static_cast<float>(cursor.x), static_cast<float>(cursor.y),
                   std::pow(1.15f, steps));
        } else {
            text_scroll_ = (std::max)(0.0f, text_scroll_ - steps * 56.0f * scale_);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
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
        if (point.x < content.left || point.x >= content.right ||
            point.y < content.top || point.y >= content.bottom) return 0;
        SetCapture(hwnd_);
        if (native_kind_ == NativeKind::Bitmap && CanPanImage()) {
            panning_ = true;
            pan_anchor_ = point;
            pan_start_x_ = pan_x_;
            pan_start_y_ = pan_y_;
        } else if (native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex) {
            uint32_t index = 0;
            if (HitTestText(static_cast<float>(point.x), static_cast<float>(point.y), index)) {
                selecting_ = true;
                sel_anchor_ = sel_focus_ = index;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        POINT point{};
        ClientPoint(lparam, point);
        const D2D1_RECT_F content = ContentRect();
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
        if (panning_) {
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
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        panning_ = false;
        selecting_ = false;
        if (message == WM_LBUTTONUP) ReleaseCapture();
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
        if ((native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex) &&
            point.x >= content.left && point.x < content.right &&
            point.y >= content.top && point.y < content.bottom) {
            uint32_t index = 0;
            if (HitTestText(static_cast<float>(point.x), static_cast<float>(point.y), index)) {
                const uint32_t a = (std::min)(sel_anchor_, sel_focus_);
                const uint32_t b = (std::max)(sel_anchor_, sel_focus_);
                if (!(b > a && index >= a && index < b))
                    sel_anchor_ = sel_focus_ = index;
            }
            POINT screen = point;
            ClientToScreen(hwnd_, &screen);
            ShowTextContextMenu(screen);
        }
        return 0;
    }
    case WM_CONTEXTMENU: {
        if (native_kind_ != NativeKind::Text && native_kind_ != NativeKind::Hex) break;
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
                return 0;
        }
        ShowTextContextMenu(screen);
        return 0;
    }
    case WM_TIMER:
        if (wparam == kAnimationTimer && animation_active_ && visible()) {
            if (frame_count_ > 1 && !waiting_for_frame_) {
                if (frame_index_ + 1 >= frame_count_) {
                    if (loop_count_ != 0 && completed_loops_ >= loop_count_) {
                        animation_active_ = false;
                        KillTimer(hwnd_, kAnimationTimer);
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
            const COLORREF fg = FindEditFgColor(dark_);
            const COLORREF bg = FindEditBgColor(dark_);
            if (!find_edit_brush_) find_edit_brush_ = CreateSolidBrush(bg);
            SetTextColor(reinterpret_cast<HDC>(wparam), fg);
            SetBkColor(reinterpret_cast<HDC>(wparam), bg);
            SetBkMode(reinterpret_cast<HDC>(wparam), OPAQUE);
            return reinterpret_cast<LRESULT>(find_edit_brush_);
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
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (wparam == VK_ESCAPE) {
            if (find_open_) CloseFind();
            else Close();
            return 0;
        }
        if (wparam == VK_SPACE && !find_open_) {
            Close();
            return 0;
        }
        if (ctrl && wparam == 'F' &&
            (native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex)) {
            OpenFind();
            return 0;
        }
        if (ctrl && wparam == 'C' &&
            (native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex)) {
            CopyTextSelection(false);
            return 0;
        }
        if (ctrl && wparam == 'A' &&
            (native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex)) {
            SelectAllText();
            return 0;
        }
        if ((wparam == VK_F3 || (find_open_ && wparam == VK_RETURN)) &&
            (native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex)) {
            if (!find_open_) OpenFind();
            FindNext(shift ? -1 : 1);
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
        }
        if (wparam == VK_RETURN && owner_ && open_message_ && !find_open_)
            PostMessageW(owner_, open_message_, 0, 0);
        else if ((wparam == VK_LEFT || wparam == VK_UP) && owner_ && navigate_message_)
            PostMessageW(owner_, navigate_message_, static_cast<WPARAM>(-1), 0);
        else if ((wparam == VK_RIGHT || wparam == VK_DOWN) && owner_ && navigate_message_)
            PostMessageW(owner_, navigate_message_, 1, 0);
        return 0;
    }
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
        ApplyWindowEffect(hwnd_, effect_, dark_);
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
        ResetAnimation();
        handler_.Reset();
        thumbnails_.Reset();
        DestroyFindEdit();
        close_format_.reset();
        preview_text_format_.reset();
        text_layout_.reset();
        compositor_.Shutdown();
        hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

} // namespace pulse::ui

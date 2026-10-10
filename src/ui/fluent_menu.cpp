#include "edit_host.h"
#include "../common/localization.h"
// fluent_menu.cpp — See fluent_menu.h for the contract.
#include "fluent_menu.h"
#include "FluentTokens.h"
#include "lumatext_renderer.h"
#include "typography.h"

#include <windowsx.h>
#include <commctrl.h>
#include <imm.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <d2d1effects.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <string_view>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "imm32.lib")

namespace pulse::ui {

namespace {

// d2d1.lib does not export the effect CLSIDs; define the shadow one locally.
// (CLSID_D2D1Shadow from d2d1effects.h.)
constexpr GUID kShadowEffectClsid = { 0xC67EA361, 0x1863, 0x4e69,
    { 0x89, 0xDB, 0x69, 0x5D, 0x3E, 0x9D, 0x5B, 0x53 } };

constexpr UINT_PTR kAnimTimer = 1;
constexpr wchar_t kMenuClass[] = L"PulseFluentMenu";

float MeasureWidth(IDWriteFactory2* dwrite, IDWriteTextFormat* format,
                   const std::wstring& text) {
    if (!dwrite || !format || text.empty()) return 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(), format,
                             10000.0f, 100.0f, &layout);
    if (!layout.get()) return 0.0f;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return (std::max)(m.width, m.widthIncludingTrailingWhitespace);
}

// Icon-button strips (glyph swatches) need wider slots and hit targets than
// the tag color dots that share the quick_swatches mechanism.
bool SwatchStripHasGlyphs(const FluentMenuItem& it) {
    for (const auto& s : it.quick_swatches)
        if (!s.glyph.empty()) return true;
    return false;
}

float SwatchSpacingDip(const FluentMenuItem& it) {
    return SwatchStripHasGlyphs(it) ? 44.0f : 28.0f;
}

float SwatchHitRadiusDip(const FluentMenuItem& it) {
    return SwatchStripHasGlyphs(it) ? 20.0f : 13.0f;
}

ComPtr<IDWriteTextFormat> MakeFormat(IDWriteFactory2* dwrite, float size) {
    ComPtr<IDWriteTextFormat> fmt;
    if (!dwrite) return fmt;
    typography::CreateTextFormat(dwrite,
        {typography::FontRole::Text, size, DWRITE_FONT_WEIGHT_NORMAL}, &fmt);
    return fmt;
}

} // namespace

// ---------------------------------------------------------------------------
// FluentMenuModel
// ---------------------------------------------------------------------------
void FluentMenuModel::SetItems(std::vector<FluentMenuItem> items) {
    items_ = std::move(items);
}

namespace {

bool DisplayEqualItem(const FluentMenuItem& a, const FluentMenuItem& b) {
    if (a.text != b.text || a.enabled != b.enabled ||
        a.toggle != b.toggle || a.secondary != b.secondary || a.checked != b.checked ||
        a.shortcut_inline != b.shortcut_inline || a.shortcut != b.shortcut ||
        a.separator_after != b.separator_after || a.tooltip != b.tooltip ||
        (a.trailing_command != 0) != (b.trailing_command != 0) ||
        a.children.size() != b.children.size() ||
        a.quick_swatches.size() != b.quick_swatches.size())
        return false;
    for (size_t i = 0; i < a.children.size(); ++i) {
        if (!DisplayEqualItem(a.children[i], b.children[i])) return false;
    }
    return true;
}

void PatchItemCommands(FluentMenuItem& dest, const FluentMenuItem& src) {
    dest.command = src.command;
    dest.trailing_command = src.trailing_command;
    dest.mnemonic = src.mnemonic;
    const size_t n = (std::min)(dest.children.size(), src.children.size());
    for (size_t i = 0; i < n; ++i)
        PatchItemCommands(dest.children[i], src.children[i]);
    const size_t ns = (std::min)(dest.quick_swatches.size(), src.quick_swatches.size());
    for (size_t i = 0; i < ns; ++i) {
        dest.quick_swatches[i].command = src.quick_swatches[i].command;
        dest.quick_swatches[i].mnemonic = src.quick_swatches[i].mnemonic;
    }
}

wchar_t UpperKey(wchar_t key) {
    return key ? static_cast<wchar_t>(std::towupper(key)) : 0;
}

} // namespace

bool FluentMenuModel::PatchCommands(const std::vector<FluentMenuItem>& src) {
    if (items_.size() != src.size()) return false;
    for (size_t i = 0; i < items_.size(); ++i)
        if (!DisplayEqualItem(items_[i], src[i])) return false;
    for (size_t i = 0; i < items_.size(); ++i)
        PatchItemCommands(items_[i], src[i]);
    return true;
}

void FluentMenuModel::SetRowHeightDip(float dip) {
    row_dip_ = std::clamp(dip, kMinRowDip, kMaxRowDip);
}

void FluentMenuModel::Layout(IDWriteFactory2* dwrite, float scale, float min_width_px) {
    scale_ = std::max(0.25f, scale);
    row_h_ = row_dip_ * scale_;  // theme.row_menu
    pad_v_ = 4.0f * scale_;
    sep_h_ = 5.0f * scale_;

    auto body = MakeFormat(dwrite, 14.0f * scale_);
    auto caption = MakeFormat(dwrite, 12.0f * scale_);

    // Must match DrawMenuItem: inset 6, content pad 10, icon 20, gap 8, pad 10, inset 6.
    const float inset = 6.0f * scale_;
    const float content_pad = 10.0f * scale_;
    const float icon_w = 20.0f * scale_;
    const float icon_gap = 8.0f * scale_;
    const float left_chrome = inset + content_pad + icon_w + icon_gap;
    const float right_chrome = content_pad + inset;
    const float shortcut_gap = 24.0f * scale_;

    inline_label_width_ = 0.0f;
    std::vector<float> row_w;
    row_w.reserve(items_.size());
    float content_w = 160.0f * scale_;
    for (const auto& it : items_) {
        const float radio_col = it.radio_group ? 12.0f * scale_ : 0.0f;
        const float text_w = std::ceil(MeasureWidth(dwrite, body.get(), it.text) + 2.0f * scale_);
        if (it.shortcut_inline && !it.shortcut.empty())
            inline_label_width_ = std::max(inline_label_width_, text_w);
        float w = radio_col + left_chrome + text_w + right_chrome;
        if (it.trailing_command) w += 32.0f * scale_;
        if (it.toggle) w += 52.0f * scale_;
        if (!it.children.empty()) {
            w += 26.0f * scale_; // 22px chevron column + 4px gap
        } else if (!it.badge_text.empty()) {
            w += shortcut_gap + MeasureWidth(dwrite, caption.get(), it.badge_text) + 14.0f * scale_;
        } else if (!it.shortcut.empty()) {
            w += shortcut_gap + MeasureWidth(dwrite, caption.get(), it.shortcut);
        }
        if (!it.quick_swatches.empty())
            w = (std::max)(w, (32.0f + SwatchSpacingDip(it) *
                               static_cast<float>(it.quick_swatches.size())) * scale_);
        row_w.push_back(it.quick_swatches.empty() ? w : 0.0f);
        content_w = (std::max)(content_w, w);
    }
    // Context menus stay compact; a long undo/file name ellipsizes instead of
    // stretching the flyout. Command palette passes a larger min_width_px.
    const float max_w = (std::max)(min_width_px, 320.0f * scale_);
    width_ = (int)std::ceil((std::max)(min_width_px, (std::min)(content_w, max_w)));
    truncated_.assign(items_.size(), 0);
    for (size_t i = 0; i < row_w.size(); ++i)
        truncated_[i] = row_w[i] > static_cast<float>(width_) + 0.5f ? 1 : 0;

    const float inline_space = std::max(0.0f, static_cast<float>(width_) -
        left_chrome - right_chrome - shortcut_gap);
    inline_label_width_ = std::min({inline_label_width_, 240.0f * scale_, inline_space * 0.4f});

    float h = pad_v_ * 2.0f;
    for (const auto& it : items_) {
        h += row_h_;
        if (it.separator_after) h += sep_h_;
    }
    height_ = (int)std::ceil(h);
}

const FluentMenuItem* FluentMenuModel::At(int i) const {
    return (i >= 0 && i < (int)items_.size()) ? &items_[i] : nullptr;
}

bool FluentMenuModel::Truncated(int i) const {
    return i >= 0 && i < (int)truncated_.size() && truncated_[static_cast<size_t>(i)] != 0;
}

float FluentMenuModel::RowTopPx(int i) const {
    float y = pad_v_;
    for (int k = 0; k < i && k < (int)items_.size(); ++k) {
        y += row_h_;
        if (items_[k].separator_after) y += sep_h_;
    }
    return y;
}

int FluentMenuModel::HitTestRow(float y_px) const {
    float y = pad_v_;
    for (int i = 0; i < (int)items_.size(); ++i) {
        if (y_px >= y && y_px < y + row_h_) return i;
        y += row_h_;
        if (items_[i].separator_after) {
            if (y_px >= y && y_px < y + sep_h_) return -1; // dead zone
            y += sep_h_;
        }
    }
    return -1;
}

wchar_t MenuItemMnemonic(const FluentMenuItem& item) {
    if (item.mnemonic) return UpperKey(item.mnemonic);
    // "编辑(E)" / "打开方式(H)...": shell32's CJK "(&X)" after CleanMenuText.
    std::wstring_view text = item.text;
    while (!text.empty() && (text.back() == L'.' || text.back() == L'\x2026' || text.back() == L' '))
        text.remove_suffix(1);
    if (text.size() < 3) return 0;
    const wchar_t open = text[text.size() - 3];
    const wchar_t key = text[text.size() - 2];
    const wchar_t close = text.back();
    if (!((open == L'(' && close == L')') || (open == L'\xFF08' && close == L'\xFF09'))) return 0;
    const bool ascii_key = (key >= L'A' && key <= L'Z') || (key >= L'a' && key <= L'z') ||
                           (key >= L'0' && key <= L'9');
    return ascii_key ? UpperKey(key) : 0;
}

std::vector<MenuMnemonicTarget> FindMenuMnemonic(const std::vector<FluentMenuItem>& items,
                                                 wchar_t key_a, wchar_t key_b) {
    const wchar_t a = UpperKey(key_a);
    const wchar_t b = UpperKey(key_b);
    auto matches = [&](wchar_t key) { return key != 0 && (key == a || key == b); };
    std::vector<MenuMnemonicTarget> out;
    for (size_t i = 0; i < items.size(); ++i) {
        const FluentMenuItem& item = items[i];
        if (!item.enabled) continue;
        if ((item.command != 0 || !item.children.empty()) && matches(MenuItemMnemonic(item)))
            out.push_back({ static_cast<int>(i), -1 });
        for (size_t s = 0; s < item.quick_swatches.size(); ++s) {
            const FluentMenuSwatch& swatch = item.quick_swatches[s];
            if (swatch.command != 0 && matches(UpperKey(swatch.mnemonic)))
                out.push_back({ static_cast<int>(i), static_cast<int>(s) });
        }
    }
    return out;
}

size_t NextMenuMnemonicTarget(const std::vector<MenuMnemonicTarget>& targets, int row,
                              int swatch) {
    for (size_t i = 0; i < targets.size(); ++i) {
        if (targets[i].row > row || (targets[i].row == row && targets[i].swatch > swatch))
            return i;
    }
    return 0;
}

int FluentMenuModel::NextEnabled(int from, int dir) const {
    if (items_.empty()) return -1;
    int n = (int)items_.size();
    int i = from;
    for (int step = 0; step < n; ++step) {
        i += dir > 0 ? 1 : -1;
        if (i < 0) i = n - 1;
        if (i >= n) i = 0;
        if (items_[i].enabled) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// FluentMenu window
// ---------------------------------------------------------------------------
FluentMenu::~FluentMenu() {
    if (tooltip_) DestroyWindow(tooltip_);
    HideFilterEdit();
    if (edit_) {
        DestroyWindow(edit_);
        edit_ = nullptr;
    }
    if (edit_font_) { DeleteObject(edit_font_); edit_font_ = nullptr; }
    if (edit_brush_) { DeleteObject(edit_brush_); edit_brush_ = nullptr; }
    if (sub_hwnd_) DestroyWindow(sub_hwnd_);
    if (hwnd_) DestroyWindow(hwnd_);
    if (surf_.dib) DeleteObject(surf_.dib);
    if (surf_.mem_dc) DeleteDC(surf_.mem_dc);
    if (sub_surf_.dib) DeleteObject(sub_surf_.dib);
    if (sub_surf_.mem_dc) DeleteDC(sub_surf_.mem_dc);
    if (tip_surf_.dib) DeleteObject(tip_surf_.dib);
    if (tip_surf_.mem_dc) DeleteDC(tip_surf_.mem_dc);
}

bool FluentMenu::Create(HWND owner, Compositor* compositor, float scale) {
    owner_ = owner;
    compositor_ = compositor;
    scale_ = scale;
    painter_.SetCompositor(compositor);
    painter_.SetScale(scale);
    return EnsureWindow();
}

void FluentMenu::SetTheme(bool dark, D2D1_COLOR_F accent) {
    dark_ = dark;
    accent_ = accent;
}

void FluentMenu::SetRowHeightDip(float dip) {
    model_.SetRowHeightDip(dip);
    sub_model_.SetRowHeightDip(dip);
}

namespace {

// Native menus take letters as access keys even while a Chinese or Japanese
// IME is on: the IME never sees them, so no candidate window pops up behind
// the menu. A command menu has nothing to type into, so for its lifetime the
// input context is detached from the window keeping keyboard focus and from
// the menu itself (a click focuses it), then handed back unchanged.
class ImeSuspension {
public:
    ImeSuspension(HWND focus, HWND menu) : focus_(focus), menu_(menu) {
        if (focus_ && focus_ != menu_) saved_ = ImmAssociateContext(focus_, nullptr);
        if (menu_) ImmAssociateContextEx(menu_, nullptr, 0);
    }
    ~ImeSuspension() {
        if (menu_ && IsWindow(menu_)) ImmAssociateContextEx(menu_, nullptr, IACE_DEFAULT);
        if (focus_ && focus_ != menu_ && IsWindow(focus_)) ImmAssociateContext(focus_, saved_);
    }
    ImeSuspension(const ImeSuspension&) = delete;
    ImeSuspension& operator=(const ImeSuspension&) = delete;

private:
    HWND focus_ = nullptr;
    HWND menu_ = nullptr;
    HIMC saved_ = nullptr;
};

} // namespace

bool FluentMenu::EnsureWindow() {
    if (hwnd_) return true;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MenuWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kMenuClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!GetClassInfoExW(inst, kMenuClass, &wc)) RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                            kMenuClass, L"PulseMenu", WS_POPUP,
                            0, 0, 10, 10, owner_, nullptr, inst, this);
    return hwnd_ != nullptr;
}

LRESULT CALLBACK FluentMenu::MenuWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<FluentMenu*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<FluentMenu*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    const bool is_sub = hwnd == self->sub_hwnd_ && self->sub_hwnd_ != nullptr;
    switch (msg) {
    case WM_MOUSEWHEEL:
        if (!is_sub && self->Scrollable()) {
            const int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            const int rows = notches != 0 ? -notches * 3
                : (GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -1 : 1); // high-resolution wheels
            if (self->ScrollRows(rows) && self->Render())
                self->Present(255, self->present_offset_);
            return 0;
        }
        break;
    case WM_MOUSEMOVE: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (is_sub) self->OnSubMouse(pt, false);
        else self->OnMouse(pt, false);
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (is_sub) {
            if (self->sub_hover_ != -1) {
                self->sub_hover_ = -1;
                self->sub_hover_swatch_ = -1;
                if (self->RenderSub()) self->PresentSub(255);
            }
        } else {
            // Keep the flyout: the pointer may be crossing onto it.
            if (self->arrow_hover_ != 0) {
                self->arrow_hover_ = 0;
                if (self->hover_row_ < 0 && self->Render()) self->Present(255, self->present_offset_);
            }
            self->UpdateHover(-1);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (!is_sub) SetFocus(self->external_edit_ ? self->external_edit_ : self->edit_ ? self->edit_ : hwnd);
        return 0; // press visual is skipped; invoke happens on release
    case WM_LBUTTONUP: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (is_sub) self->OnSubMouse(pt, true);
        else self->OnMouse(pt, true);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        if (self->external_edit_) return MA_NOACTIVATE;
        if (self->filter_fn_) {
            SetFocus(self->edit_ ? self->edit_ : hwnd);
            return MA_ACTIVATE;
        }
        break;
    case WM_CTLCOLOREDIT: {
        const bool dark = self->dark_;
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, EditTextColor(dark));
        SetBkColor(hdc, EditBackColor(dark));
        if (!self->edit_brush_) {
            self->edit_brush_ = CreateSolidBrush(dark ? RGB(30, 30, 30) : RGB(255, 255, 255));
        }
        return reinterpret_cast<LRESULT>(EditBackBrush(self->edit_brush_));
    }
    case WM_COMMAND:
        if (HIWORD(wParam) == EN_CHANGE && self->edit_ &&
            reinterpret_cast<HWND>(lParam) == self->edit_) {
            self->SyncFilterFromEdit();
            return 0;
        }
        break;
    case WM_CHAR:
        if (self->HandleFilterKey(msg, wParam)) return 0;
        break;
    case WM_GETDLGCODE:
        if (self->filter_fn_) return DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTALLKEYS;
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_APP:
        if (self->open_ && !self->animating_out_) self->RefreshFilter();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

float FluentMenu::BodyHeightPx() const {
    if (overflow_) return body_limit_px_;
    if (max_visible_rows_ <= 0) return static_cast<float>(model_.HeightPx());
    return std::min(body_limit_px_ > 0 ? body_limit_px_ : static_cast<float>(model_.HeightPx()),
        model_.RowTopPx(std::min(max_visible_rows_, model_.Count())) + 4.0f * scale_);
}

float FluentMenu::ArrowPx() const {
    return overflow_ ? std::round(kScrollArrowDip * scale_) : 0.0f;
}

float FluentMenu::ViewportPx() const {
    return std::max(0.0f, BodyHeightPx() - ArrowPx() * 2.0f);
}

float FluentMenu::MaxScrollPx() const {
    return std::max(0.0f, static_cast<float>(model_.HeightPx()) - ViewportPx());
}

void FluentMenu::ApplyHeightLimit(float available_px) {
    overflow_ = false;
    if (max_visible_rows_ > 0) {
        body_limit_px_ = std::max(model_.RowHeightPx(), available_px);
    } else if (static_cast<float>(model_.HeightPx()) > available_px) {
        // Taller than the work area (#67): keep the card on screen and scroll
        // the rows between two arrow strips, like native menus do.
        overflow_ = true;
        body_limit_px_ = std::max(available_px, ArrowPx() * 2.0f + model_.RowHeightPx());
    } else {
        body_limit_px_ = 0.0f;
    }
    scroll_y_ = std::clamp(scroll_y_, 0.0f, MaxScrollPx());
}

bool FluentMenu::ScrollTo(float y) {
    const float clamped = std::clamp(y, 0.0f, MaxScrollPx());
    if (std::abs(clamped - scroll_y_) < 0.01f) return false;
    scroll_y_ = clamped;
    // Rows moved under the pointer: drop the hover, its tooltip and a flyout
    // that was anchored to a row which is somewhere else now.
    if (sub_parent_row_ >= 0) HideSubWindow();
    hover_row_ = -1;
    hover_swatch_ = -1;
    UpdateTooltip(-1);
    return true;
}

bool FluentMenu::ScrollRows(int rows) {
    if (rows == 0 || model_.Count() == 0) return false;
    // First row whose top is at or below the viewport top; a half-hidden row
    // above it is the one a step up reveals.
    // Scrolled rows sit flush under the top arrow; only the very top keeps
    // the menu's leading padding.
    int first = model_.Count() - 1;
    for (int i = 0; i < model_.Count(); ++i) {
        if (model_.RowTopPx(i) >= scroll_y_ - 0.5f) {
            first = i;
            break;
        }
    }
    const int target = std::clamp(first + rows, 0, model_.Count() - 1);
    return ScrollTo(target == 0 ? 0.0f : model_.RowTopPx(target));
}

void FluentMenu::TickArrowScroll() {
    const auto now = std::chrono::steady_clock::now();
    if (arrow_hover_ == 0) {
        arrow_tick_ = {};
        arrow_accum_ms_ = 0.0f;
        return;
    }
    if (arrow_tick_ == std::chrono::steady_clock::time_point{}) {
        arrow_tick_ = now;
        arrow_accum_ms_ = 0.0f;
        return;
    }
    arrow_accum_ms_ += std::min(100.0f,
        std::chrono::duration<float, std::milli>(now - arrow_tick_).count());
    arrow_tick_ = now;
    int steps = 0;
    while (arrow_accum_ms_ >= kScrollArrowMsPerRow) {
        arrow_accum_ms_ -= kScrollArrowMsPerRow;
        ++steps;
    }
    if (steps > 0 && ScrollRows(arrow_hover_ * steps) && Render())
        Present(255, present_offset_);
}

void FluentMenu::LayoutWindow(POINT screen_pt) {
    int w = model_.WidthPx() + kShadowMargin * 2;
    int h = static_cast<int>(std::ceil(BodyHeightPx())) + (int)FilterHeaderPx() + kShadowMargin * 2;

    HMONITOR mon = MonitorFromPoint(screen_pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    ApplyHeightLimit(static_cast<float>(mi.rcWork.bottom - mi.rcWork.top - kShadowMargin * 2) -
                     FilterHeaderPx());
    h = static_cast<int>(std::ceil(BodyHeightPx())) + static_cast<int>(FilterHeaderPx()) + kShadowMargin * 2;
    int x = screen_pt.x;
    int y = screen_pt.y;
    if (dropdown_) {
        const int gap = static_cast<int>(std::lround(4 * scale_));
        x = dropdown_rect_.left - kShadowMargin;
        y = dropdown_rect_.bottom + gap - kShadowMargin;
        if (y + h > mi.rcWork.bottom)
            y = dropdown_rect_.top - gap + kShadowMargin - h;
        x = std::max<int>(mi.rcWork.left, std::min<int>(x, mi.rcWork.right - w));
        y = std::max<int>(mi.rcWork.top, std::min<int>(y, mi.rcWork.bottom - h));
    } else if (anchor_to_rect_) {
        x = anchor_rect_.left - kShadowMargin;
        y = (external_edit_ ? anchor_rect_.bottom + static_cast<int>(4 * scale_) : anchor_rect_.top) - kShadowMargin;
        if (x + w > mi.rcWork.right) x = (std::max)(mi.rcWork.left, mi.rcWork.right - w);
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y + h > mi.rcWork.bottom) y = std::max(mi.rcWork.top, mi.rcWork.bottom - h);
    } else if (top_center_ && owner_) {
        RECT wr{};
        GetWindowRect(owner_, &wr);
        x = wr.left + ((wr.right - wr.left) - w) / 2;
        POINT top{ 0, static_cast<LONG>(std::lround(44.0f * scale_ + 4.0f * scale_)) };
        ClientToScreen(owner_, &top);
        y = top.y - kShadowMargin;
        if (x + w > mi.rcWork.right) x = (std::max)(mi.rcWork.left, mi.rcWork.right - w);
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y + h > mi.rcWork.bottom) y = (std::max)(mi.rcWork.top, mi.rcWork.bottom - h);
        if (y < mi.rcWork.top) y = mi.rcWork.top;
    } else {
        if (x + w > mi.rcWork.right) x = (std::max)(mi.rcWork.left, mi.rcWork.right - w);
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y + h > mi.rcWork.bottom) y = (std::max)(mi.rcWork.top, mi.rcWork.bottom - h);
        if (y < mi.rcWork.top) y = mi.rcWork.top;
    }
    base_x_ = x;
    base_y_ = y;
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (filter_fn_ && !external_edit_)
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex & ~WS_EX_NOACTIVATE);
    else
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE);
    SetWindowPos(hwnd_, topmost_ ? HWND_TOPMOST : HWND_TOP, x, y, w, h,
                 (filter_fn_ && !external_edit_) ? SWP_SHOWWINDOW : (SWP_NOACTIVATE | SWP_SHOWWINDOW));
}

bool FluentMenu::Render() {
    // While the flyout is open its header row stays visually active even if
    // the pointer moved into the flyout window.
    const int hov = hover_row_ >= 0 ? hover_row_ : sub_parent_row_;
    return RenderSurface(model_, hov, hover_swatch_, FilterHeaderPx(),
                         filter_fn_ != nullptr, surf_);
}

bool FluentMenu::RenderSub() {
    return RenderSurface(sub_model_, sub_hover_, sub_hover_swatch_, 0.0f, false, sub_surf_);
}

bool FluentMenu::RenderSurface(const FluentMenuModel& model, int hover_row, int hover_swatch,
                               float header_px, bool draw_filter_field, Surface& s) {
    ID2D1DeviceContext* dc = compositor_ ? compositor_->Dc() : nullptr;
    if (!dc || model.Count() == 0) return false;

    const int cw = model.WidthPx();
    const int header = (int)header_px;
    const bool main = &model == &model_;
    const int body = main ? static_cast<int>(std::ceil(BodyHeightPx())) : model.HeightPx();
    const int ch = body + header;
    const int w = cw + kShadowMargin * 2;
    const int h = ch + kShadowMargin * 2;

    D2D1_SIZE_U size{ (UINT32)w, (UINT32)h };
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> bmp;
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &bmp))) return false;

    ComPtr<ID2D1Image> old_target;
    dc->GetTarget(&old_target);
    dc->SetTarget(bmp.get());
    // Match the main compositor: grayscale text AA. Default ClearType writes
    // subpixel RGB coverage that survives premultiplied readback as ghosted
    // glyphs (worst on CJK) once composited by UpdateLayeredWindow.
    const D2D1_TEXT_ANTIALIAS_MODE old_text_aa = dc->GetTextAntialiasMode();
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    const auto card = D2D1::RectF((float)kShadowMargin, (float)kShadowMargin,
                                  (float)(kShadowMargin + cw), (float)(kShadowMargin + ch));
    const auto drop = (anchor_to_rect_ && header > 0)
        ? D2D1::RectF(card.left, card.top + (float)header, card.right, card.bottom)
        : card;
    Theme theme = IsHighContrast() ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
    const float radius = theme.radius_flyout * scale_;

    // Drop shadow: card silhouette -> CLSID_D2D1Shadow, drawn with an offset.
    ComPtr<ID2D1CommandList> card_list;
    if (drop.right > drop.left && drop.bottom > drop.top &&
        SUCCEEDED(dc->CreateCommandList(&card_list))) {
        dc->SetTarget(card_list.get());
        ComPtr<ID2D1SolidColorBrush> black;
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1.0f), &black);
        dc->FillRoundedRectangle(D2D1::RoundedRect(drop, radius, radius), black.get());
        card_list->Close();
        dc->SetTarget(bmp.get());

        ComPtr<ID2D1Effect> shadow;
        if (SUCCEEDED(dc->CreateEffect(kShadowEffectClsid, &shadow))) {
            shadow->SetInput(0, card_list.get());
            shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 7.0f);
            shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0.0f, 0.0f, 0.0f, 0.36f));
            dc->DrawImage(shadow.get(), D2D1::Point2F(0.0f, 4.0f * scale_));
        }
    }

    if (painter_.BeginFrame(theme, IsHighContrast())) {
        if (drop.right > drop.left && drop.bottom > drop.top)
            painter_.DrawMenuSurface(drop);
        if (anchor_to_rect_ && header > 0) {
            fluent::ControlState bar;
            bar.focused = true;
            painter_.DrawTextFieldFrame(
                D2D1::RectF(card.left, card.top, card.right, card.top + (float)header), bar);
        }
        float y = (float)kShadowMargin;
        if (header > 0 && draw_filter_field && !anchor_to_rect_) {
            fluent::TextFieldSpec field;
            field.bounds = D2D1::RectF(card.left + 8.0f * scale_, card.top + 6.0f * scale_,
                                       card.right - 8.0f * scale_, card.top + (float)header - 4.0f * scale_);
            field.placeholder = filter_placeholder_.empty()
                ? pulse::l10n::Get(pulse::l10n::StringId::TabMenuSearch) : filter_placeholder_;
            field.text = filter_query_;
            field.leading_glyph = L"\xE721";
            field.state.focused = true;
            field.suppress_text = edit_ != nullptr;
            painter_.DrawTextField(field);
        }
        if (header > 0) y += (float)header;
        const float body_top = y;
        const float arrow = main ? ArrowPx() : 0.0f;
        dc->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(kShadowMargin), y + arrow,
            static_cast<float>(kShadowMargin + cw), y + body - arrow), D2D1_ANTIALIAS_MODE_ALIASED);
        y += arrow + model.RowTopPx(0) - (main ? scroll_y_ : 0.0f);
        for (int i = 0; i < model.Count(); ++i) {
            const FluentMenuItem* it = model.At(i);
            fluent::MenuItemSpec spec;
            spec.bounds = D2D1::RectF((float)kShadowMargin, y,
                                      (float)(kShadowMargin + cw), y + model.RowHeightPx());
            spec.text = it->text;
            spec.glyph = it->glyph;
            spec.glyph_scale = it->glyph_scale;
            spec.shortcut = it->shortcut;
            spec.shortcut_inline = it->shortcut_inline;
            spec.inline_label_width = model.InlineLabelWidthPx();
            spec.badge_text = it->badge_text;
            spec.has_submenu = !it->children.empty();
            spec.state.enabled = it->enabled;
            // A quick-swatch strip owns its hover feedback per dot; treating it
            // as a regular menu row produces a misleading full-width highlight.
            spec.state.hovered = (i == hover_row && it->quick_swatches.empty());
            spec.separator_after = it->separator_after;
            spec.has_swatch = it->has_swatch;
            spec.checked = it->checked;
            spec.toggle = it->toggle;
            spec.secondary = it->secondary;
            spec.mixed = it->mixed;
            spec.radio = it->radio;
            spec.radio_group = it->radio_group;
            spec.pictogram = it->pictogram;
            spec.swatch_color = it->swatch_color;
            if (it->trailing_command) spec.bounds.right -= 32.0f * scale_;
            painter_.DrawMenuItem(spec);
            if (it->trailing_command && i == hover_row && it->enabled) {
                const float right = static_cast<float>(kShadowMargin + cw) - 8.0f * scale_;
                const float glyph_top = y + (model.RowHeightPx() - 20.0f * scale_) * 0.5f;
                painter_.DrawGlyph(L"\xE711", D2D1::RectF(right - 20.0f * scale_,
                    glyph_top, right, glyph_top + 20.0f * scale_), theme.text_secondary);
            }
            if (!it->quick_swatches.empty()) {
                const float spacing = SwatchSpacingDip(*it) * scale_;
                const float strip_width = spacing * static_cast<float>(it->quick_swatches.size() - 1);
                const float first = static_cast<float>(kShadowMargin) +
                    (static_cast<float>(cw) - strip_width) * 0.5f;
                const float center_y = (spec.bounds.top + spec.bounds.bottom) * 0.5f;
                for (size_t swatch_index = 0; swatch_index < it->quick_swatches.size(); ++swatch_index) {
                    const auto& swatch = it->quick_swatches[swatch_index];
                    const float center_x = first + spacing * static_cast<float>(swatch_index);
                    const bool hovered = i == hover_row &&
                                         static_cast<int>(swatch_index) == hover_swatch;
                    if (!swatch.glyph.empty()) {
                        // Icon button: hover pill + Fluent glyph in text color.
                        const float half_w = 18.0f * scale_;
                        const float half_h = 13.0f * scale_;
                        const D2D1_RECT_F pill = D2D1::RectF(
                            center_x - half_w, center_y - half_h,
                            center_x + half_w, center_y + half_h);
                        if (hovered && it->enabled)
                            painter_.FillRoundedRect(pill, 4.0f * scale_, theme.fill_hover);
                        const float g = 8.0f * scale_;
                        painter_.DrawGlyph(swatch.glyph,
                            D2D1::RectF(center_x - g, center_y - g, center_x + g, center_y + g),
                            it->enabled ? theme.text : theme.text_disabled);
                    } else {
                        painter_.DrawTagDotState(D2D1::Point2F(center_x, center_y), 6.0f,
                                                 swatch.color, swatch.checked, swatch.mixed,
                                                 hovered);
                    }
                }
            }
            y += model.RowHeightPx();
            if (it->separator_after) y += 5.0f * scale_;
        }
        dc->PopAxisAlignedClip();
        if (arrow > 0.0f) {
            const float max_scroll = MaxScrollPx();
            for (int dir = -1; dir <= 1; dir += 2) {
                const float top = dir < 0 ? body_top : body_top + static_cast<float>(body) - arrow;
                const D2D1_RECT_F strip = D2D1::RectF(
                    static_cast<float>(kShadowMargin) + 4.0f * scale_, top + 2.0f * scale_,
                    static_cast<float>(kShadowMargin + cw) - 4.0f * scale_, top + arrow - 2.0f * scale_);
                const bool can_scroll = dir < 0 ? scroll_y_ > 0.5f : scroll_y_ < max_scroll - 0.5f;
                if (can_scroll && arrow_hover_ == dir)
                    painter_.FillRoundedRect(strip, 4.0f * scale_, theme.fill_hover);
                const float g = 6.0f * scale_;
                const float cx = (strip.left + strip.right) * 0.5f;
                const float cy = (strip.top + strip.bottom) * 0.5f;
                painter_.DrawGlyph(dir < 0 ? L"\xE70E" : L"\xE70D",
                    D2D1::RectF(cx - g, cy - g, cx + g, cy + g),
                    can_scroll ? theme.text_secondary : theme.text_disabled);
            }
        }
    }

    HRESULT hr = dc->EndDraw();
    dc->SetTextAntialiasMode(old_text_aa);
    dc->SetTarget(old_target.get());
    if (FAILED(hr)) return false;

    // GPU -> CPU readback.
    auto cpu_props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> cpu;
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, cpu_props, &cpu))) return false;
    if (FAILED(cpu->CopyFromBitmap(nullptr, bmp.get(), nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;

    // (Re)create the staging DIB.
    if (!s.mem_dc) s.mem_dc = CreateCompatibleDC(nullptr);
    if (s.dib && (s.w != w || s.h != h)) {
        DeleteObject(s.dib);
        s.dib = nullptr;
    }
    if (!s.dib) {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        s.dib = CreateDIBSection(s.mem_dc, &bmi, DIB_RGB_COLORS, &s.bits, nullptr, 0);
        if (!s.dib) {
            cpu->Unmap();
            return false;
        }
        s.w = w;
        s.h = h;
    }
    for (int row = 0; row < h; ++row) {
        std::memcpy(static_cast<uint8_t*>(s.bits) + (size_t)row * w * 4,
                    static_cast<const uint8_t*>(mapped.bits) + (size_t)row * mapped.pitch,
                    (size_t)w * 4);
    }
    cpu->Unmap();
    return true;
}

void FluentMenu::Present(BYTE alpha, int y_offset_px) {
    if (!surf_.dib || !surf_.bits) return;
    HGDIOBJ old = SelectObject(surf_.mem_dc, surf_.dib);
    POINT dst{ base_x_, base_y_ + y_offset_px };
    POINT src{ 0, 0 };
    SIZE sz{ surf_.w, surf_.h };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
    UpdateLayeredWindow(hwnd_, nullptr, &dst, &sz, surf_.mem_dc, &src, 0, &blend, ULW_ALPHA);
    SelectObject(surf_.mem_dc, old);
    present_offset_ = y_offset_px;
    if (filter_fn_ && edit_ && open_ && !animating_out_)
        PlaceFilterEdit(y_offset_px);
}

void FluentMenu::PresentSub(BYTE alpha) {
    if (!sub_hwnd_ || !sub_surf_.dib || !sub_surf_.bits) return;
    HGDIOBJ old = SelectObject(sub_surf_.mem_dc, sub_surf_.dib);
    POINT dst{ sub_x_, sub_y_ };
    POINT src{ 0, 0 };
    SIZE sz{ sub_surf_.w, sub_surf_.h };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
    UpdateLayeredWindow(sub_hwnd_, nullptr, &dst, &sz, sub_surf_.mem_dc, &src, 0,
                        &blend, ULW_ALPHA);
    SelectObject(sub_surf_.mem_dc, old);
}

bool FluentMenu::EnsureSubWindow() {
    if (sub_hwnd_) return true;
    if (!hwnd_) return false;
    // Same class as the main popup (registered in EnsureWindow); the shared
    // wndproc routes by hwnd.
    sub_hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                kMenuClass, L"PulseSubMenu", WS_POPUP,
                                0, 0, 10, 10, hwnd_, nullptr,
                                GetModuleHandleW(nullptr), this);
    return sub_hwnd_ != nullptr;
}

void FluentMenu::HideSubWindow() {
    sub_pending_row_ = -1;
    sub_parent_row_ = -1;
    sub_hover_ = -1;
    sub_hover_swatch_ = -1;
    if (sub_hwnd_) ShowWindow(sub_hwnd_, SW_HIDE);
}

// Opens the one-level flyout for row's children, anchored to the row's right
// edge (flips to the left when the work area runs out).
void FluentMenu::OpenSubmenu(int row) {
    const FluentMenuItem* it = model_.At(row);
    if (!it || !it->enabled || it->children.empty() || !compositor_) return;
    if (row == sub_parent_row_) return; // already showing this row's children
    if (!EnsureSubWindow()) return;
    sub_model_.SetItems(it->children);
    sub_model_.Layout(compositor_->DwriteFactory(), scale_);
    sub_parent_row_ = row;
    sub_hover_ = -1;
    sub_hover_swatch_ = -1;
    sub_pending_row_ = -1;

    const int content_w = sub_model_.WidthPx();
    const int w = content_w + kShadowMargin * 2;
    const int h = sub_model_.HeightPx() + kShadowMargin * 2;
    const int overlap = (int)std::lround(4.0f * scale_);
    // First child row lines up with the header row.
    int y = base_y_ + present_offset_ +
            (int)(FilterHeaderPx() + ArrowPx() + model_.RowTopPx(row) - scroll_y_) -
            (int)sub_model_.RowTopPx(0);
    int x = base_x_ + model_.WidthPx() - overlap; // card edges overlap by 4dip

    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    if (x + kShadowMargin + content_w > mi.rcWork.right)
        x = base_x_ - content_w + overlap; // open to the left instead
    if (y + h - kShadowMargin > mi.rcWork.bottom)
        y = mi.rcWork.bottom - h + kShadowMargin;
    if (y < mi.rcWork.top - kShadowMargin) y = mi.rcWork.top - kShadowMargin;
    sub_x_ = x;
    sub_y_ = y;
    SetWindowPos(sub_hwnd_, topmost_ ? HWND_TOPMOST : HWND_TOP, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (RenderSub()) PresentSub(255);
    if (Render()) Present(255, present_offset_); // header row stays active
}

void FluentMenu::CloseSubmenu() {
    const bool was_open = sub_parent_row_ >= 0;
    HideSubWindow();
    if (was_open && open_ && !animating_out_ && Render()) Present(255, present_offset_);
}

void FluentMenu::OnSubMouse(POINT client_pt, bool button_up) {
    if (!open_ || animating_out_ || sub_parent_row_ < 0) return;
    const float y = (float)client_pt.y - (float)kShadowMargin;
    int row = sub_model_.HitTestRow(y);
    const FluentMenuItem* it = sub_model_.At(row);
    if (it && !it->enabled) {
        row = -1;
        it = nullptr;
    }
    if (button_up) {
        const int command = InvokeAt(sub_model_, row, static_cast<float>(client_pt.x));
        if (command != 0) {
            result_ = command;
            Dismiss();
        }
        return;
    }
    const int swatch = HitTestSwatch(sub_model_, row, static_cast<float>(client_pt.x));
    if (row != sub_hover_ || swatch != sub_hover_swatch_) {
        sub_hover_ = row;
        sub_hover_swatch_ = swatch;
        if (RenderSub()) PresentSub(255);
    }
}

void FluentMenu::OnMouse(POINT client_pt, bool button_up) {
    if (!open_ || animating_out_) return;
    float y = (float)client_pt.y - (float)kShadowMargin - FilterHeaderPx();
    const float body_h = BodyHeightPx();
    const float arrow = ArrowPx();
    int zone = 0; // -1 / +1: over the top / bottom scroll arrow (#67)
    if (arrow > 0.0f && y >= 0 && y < body_h)
        zone = y < arrow ? -1 : (y >= body_h - arrow ? 1 : 0);
    const bool zone_changed = zone != arrow_hover_;
    arrow_hover_ = zone;
    if (zone_changed) arrow_tick_ = {};
    if (zone != 0) {
        sub_pending_row_ = -1;
        if (button_up) {
            // A click pages; resting on the arrow scrolls continuously.
            const int page = std::max(1, static_cast<int>(ViewportPx() / model_.RowHeightPx()) - 1);
            if (ScrollRows(zone * page) && Render())
                Present(255, present_offset_);
            return;
        }
        UpdateTooltip(-1);
        if (hover_row_ >= 0) UpdateHover(-1);
        else if (zone_changed && Render()) Present(255, present_offset_);
        return;
    }
    if (zone_changed && Render()) Present(255, present_offset_);
    int row = y >= 0 && y < body_h ? model_.HitTestRow(y - arrow + scroll_y_) : -1;
    const FluentMenuItem* it = model_.At(row);
    const bool flyout_header = it && it->enabled && !it->children.empty();
    if (button_up) {
        if (flyout_header) {
            OpenSubmenu(row); // a header never returns a command
            return;
        }
        if (row >= 0) {
            int cmd = InvokeAt(row, static_cast<float>(client_pt.x));
            if (cmd != 0) {
                result_ = cmd;
                Dismiss();
            }
        }
        return;
    }
    UpdateHover(row, HitTestSwatch(row, static_cast<float>(client_pt.x)));
    UpdateTooltip(row);
    // Flyout scheduling: dwell on a header opens it; moving to another row
    // closes it. Leaving the window (towards the flyout) keeps it open.
    if (flyout_header) {
        if (row != sub_parent_row_ && sub_pending_row_ != row) {
            sub_pending_row_ = row;
            sub_pending_since_ = std::chrono::steady_clock::now();
        }
    } else if (row >= 0) {
        sub_pending_row_ = -1;
        if (sub_parent_row_ >= 0) CloseSubmenu();
    } else {
        sub_pending_row_ = -1;
    }
}

void FluentMenu::UpdateTooltip(int row) {
    // Drawn like the main window's tooltip (flyout fill, card stroke, 4 dip
    // radius, small text) instead of the system tooltip control.
    const auto* item = model_.At(row);
    // Ellipsized labels (long Explorer verbs) show their full text.
    const std::wstring text = !item ? std::wstring{}
        : !item->tooltip.empty() ? item->tooltip
        : model_.Truncated(row) ? item->text : std::wstring{};
    if (text == tooltip_text_) return;
    tooltip_text_ = text;
    if (text.empty()) {
        if (tooltip_) ShowWindow(tooltip_, SW_HIDE);
        return;
    }
    if (!tooltip_ && hwnd_) {
        // No create param: the shared wndproc treats it as a plain window.
        tooltip_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                                   WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                   kMenuClass, L"PulseMenuTip", WS_POPUP, 0, 0, 1, 1, hwnd_,
                                   nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    if (!tooltip_ || !RenderTip(text)) return;
    POINT pt{};
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    int x = pt.x + static_cast<int>(12.0f * scale_);
    int y = pt.y + static_cast<int>(18.0f * scale_);
    x = (std::min)(x, static_cast<int>(mi.rcWork.right) - tip_surf_.w);
    if (y + tip_surf_.h > mi.rcWork.bottom) y = pt.y - tip_surf_.h - static_cast<int>(8.0f * scale_);
    x = (std::max)(x, static_cast<int>(mi.rcWork.left));
    HGDIOBJ old = SelectObject(tip_surf_.mem_dc, tip_surf_.dib);
    POINT dst{x, y};
    POINT src{0, 0};
    SIZE sz{tip_surf_.w, tip_surf_.h};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(tooltip_, nullptr, &dst, &sz, tip_surf_.mem_dc, &src, 0, &blend, ULW_ALPHA);
    SelectObject(tip_surf_.mem_dc, old);
    SetWindowPos(tooltip_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

bool FluentMenu::RenderTip(const std::wstring& text) {
    ID2D1DeviceContext* dc = compositor_ ? compositor_->Dc() : nullptr;
    IDWriteFactory2* dwrite = compositor_ ? compositor_->DwriteFactory() : nullptr;
    IDWriteTextFormat* fmt = compositor_ ? compositor_->SmallFormat() : nullptr;
    if (!dc || !dwrite || !fmt || text.empty()) return false;
    std::vector<std::wstring> lines;
    for (size_t start = 0;;) {
        const size_t nl = text.find(L'\n', start);
        lines.push_back(text.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start));
        if (nl == std::wstring::npos || lines.size() >= 6) break;
        start = nl + 1;
    }
    float tw = 0.0f;
    for (const auto& line : lines) tw = (std::max)(tw, MeasureWidth(dwrite, fmt, line));
    const float line_h = 18.0f * scale_;
    const bool multi = lines.size() > 1;
    const int w = static_cast<int>(std::ceil((std::min)(tw + 20.0f * scale_, 480.0f * scale_)));
    const int h = static_cast<int>(std::ceil(multi ? 10.0f * scale_ + line_h * lines.size() : 28.0f * scale_));
    if (w <= 0 || h <= 0) return false;

    D2D1_SIZE_U size{static_cast<UINT32>(w), static_cast<UINT32>(h)};
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> bmp;
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &bmp))) return false;
    ComPtr<ID2D1Image> old_target;
    dc->GetTarget(&old_target);
    dc->SetTarget(bmp.get());
    const D2D1_TEXT_ANTIALIAS_MODE old_text_aa = dc->GetTextAntialiasMode();
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    const Theme theme = IsHighContrast() ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
    // The layered window has nothing behind it: flatten the flyout colour onto bg.
    D2D1_COLOR_F fill = theme.surface_flyout;
    const float a = fill.a;
    fill = D2D1::ColorF(fill.r * a + theme.bg.r * (1.0f - a), fill.g * a + theme.bg.g * (1.0f - a),
                        fill.b * a + theme.bg.b * (1.0f - a), 1.0f);
    const float r = 4.0f * scale_;
    const D2D1_RECT_F card = D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f);
    ComPtr<ID2D1SolidColorBrush> brush;
    dc->CreateSolidColorBrush(fill, &brush);
    if (brush.get()) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(card, r, r), brush.get());
        brush->SetColor(theme.stroke_card);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(card, r, r), brush.get(), 1.0f);
        brush->SetColor(theme.text);
        for (size_t i = 0; i < lines.size(); ++i) {
            const float top = multi ? 5.0f * scale_ + line_h * i : 0.0f;
            const float row_h = multi ? line_h : static_cast<float>(h);
            ComPtr<IDWriteTextLayout> layout;
            if (FAILED(dwrite->CreateTextLayout(lines[i].c_str(), static_cast<UINT32>(lines[i].size()), fmt,
                    (std::max)(1.0f, w - 20.0f * scale_), row_h, &layout)) || !layout.get()) continue;
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            dc->DrawTextLayout(D2D1::Point2F(10.0f * scale_, top), layout.get(), brush.get(),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    const HRESULT hr = dc->EndDraw();
    dc->SetTextAntialiasMode(old_text_aa);
    dc->SetTarget(old_target.get());
    if (FAILED(hr)) return false;

    auto cpu_props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> cpu;
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, cpu_props, &cpu))) return false;
    if (FAILED(cpu->CopyFromBitmap(nullptr, bmp.get(), nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    Surface& s = tip_surf_;
    if (!s.mem_dc) s.mem_dc = CreateCompatibleDC(nullptr);
    if (s.dib && (s.w != w || s.h != h)) {
        DeleteObject(s.dib);
        s.dib = nullptr;
    }
    if (!s.dib) {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        s.dib = CreateDIBSection(s.mem_dc, &bmi, DIB_RGB_COLORS, &s.bits, nullptr, 0);
        if (!s.dib) {
            cpu->Unmap();
            return false;
        }
        s.w = w;
        s.h = h;
    }
    for (int y = 0; y < h; ++y) {
        std::memcpy(static_cast<uint8_t*>(s.bits) + static_cast<size_t>(y) * w * 4,
                    static_cast<const uint8_t*>(mapped.bits) + static_cast<size_t>(y) * mapped.pitch,
                    static_cast<size_t>(w) * 4);
    }
    cpu->Unmap();
    return true;
}

void FluentMenu::UpdateHover(int row, int swatch) {
    if (row >= 0) {
        const FluentMenuItem* it = model_.At(row);
        if (it && !it->enabled) {
            row = -1;
            swatch = -1;
        } else if (!it || it->quick_swatches.empty()) {
            swatch = -1;
        }
    }
    if (row == hover_row_ && swatch == hover_swatch_) return;
    UpdateTooltip(-1);
    hover_row_ = row;
    hover_swatch_ = swatch;
    if (row >= 0 && Scrollable()) {
        const float top = model_.RowTopPx(row);
        if (top < scroll_y_) scroll_y_ = top <= model_.RowTopPx(0) + 0.5f ? 0.0f : top;
        if (top + model_.RowHeightPx() > scroll_y_ + ViewportPx())
            scroll_y_ = row == model_.Count() - 1 ? MaxScrollPx()
                                                  : top + model_.RowHeightPx() - ViewportPx();
    }
    if (Render()) Present(255, 0);
}

int FluentMenu::HitTestSwatch(int row, float client_x) const {
    return HitTestSwatch(model_, row, client_x);
}

int FluentMenu::HitTestSwatch(const FluentMenuModel& model, int row, float client_x) const {
    const FluentMenuItem* item = model.At(row);
    if (!item || !item->enabled || item->quick_swatches.empty()) return -1;
    const float spacing = SwatchSpacingDip(*item) * scale_;
    const float radius = SwatchHitRadiusDip(*item) * scale_;
    const float strip_width = spacing * static_cast<float>(item->quick_swatches.size() - 1);
    const float first = static_cast<float>(kShadowMargin) +
        (static_cast<float>(model.WidthPx()) - strip_width) * 0.5f;
    for (size_t i = 0; i < item->quick_swatches.size(); ++i) {
        const float center = first + spacing * static_cast<float>(i);
        if (std::abs(client_x - center) <= radius)
            return static_cast<int>(i);
    }
    return -1;
}

int FluentMenu::InvokeRow(int row) {
    const FluentMenuItem* it = model_.At(row);
    if (!it || !it->enabled) return 0;
    return it->command;
}

int FluentMenu::InvokeAt(int row, float client_x) const {
    return InvokeAt(model_, row, client_x);
}

int FluentMenu::InvokeAt(const FluentMenuModel& model, int row, float client_x) const {
    const FluentMenuItem* item = model.At(row);
    if (item && item->enabled && item->trailing_command &&
        client_x >= kShadowMargin + model.WidthPx() - 36.0f * scale_ &&
        client_x < kShadowMargin + model.WidthPx() - 6.0f * scale_)
        return item->trailing_command;
    if (!item || !item->enabled || item->quick_swatches.empty())
        return item && item->enabled ? item->command : 0;
    const int swatch = HitTestSwatch(model, row, client_x);
    if (swatch >= 0) return item->quick_swatches[static_cast<size_t>(swatch)].command;
    return item->command;
}

bool FluentMenu::HandleMnemonicKey(const MSG& msg) {
    if (filter_fn_ || animating_out_ || !open_) return false;
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) return false;
    if (GetKeyState(VK_CONTROL) & 0x8000) return false; // Ctrl+C etc. stay app shortcuts
    UINT vk = static_cast<UINT>(msg.wParam);
    // An active IME (Chinese/Japanese input in the window behind) reports
    // VK_PROCESSKEY; menus take the physical key, as native menus do.
    if (vk == VK_PROCESSKEY) vk = ImmGetVirtualKey(msg.hwnd);
    wchar_t latin = 0;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        latin = static_cast<wchar_t>(vk);
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        latin = static_cast<wchar_t>(L'0' + (vk - VK_NUMPAD0));
    // The keyboard layout's own character covers non-Latin access keys.
    const UINT mapped = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR);
    wchar_t layout = (mapped & 0x80000000u) ? 0 : static_cast<wchar_t>(mapped & 0xFFFF);
    if (layout <= L' ') layout = 0;
    if (!latin && !layout) return false; // navigation / function keys

    // Keys go to the flyout only once it has a highlighted row, like native
    // menus: a flyout merely opened by hovering leaves the parent current.
    const bool in_sub = sub_parent_row_ >= 0 && sub_hover_ >= 0;
    const FluentMenuModel& model = in_sub ? sub_model_ : model_;
    const auto targets = FindMenuMnemonic(model.Items(), latin, layout);
    if (targets.empty()) return true;
    if (targets.size() == 1) {
        const MenuMnemonicTarget target = targets.front();
        const FluentMenuItem* item = model.At(target.row);
        if (!item) return true;
        if (target.swatch >= 0) {
            result_ = item->quick_swatches[static_cast<size_t>(target.swatch)].command;
            Dismiss();
        } else if (!in_sub && !item->children.empty()) {
            UpdateHover(target.row);
            OpenSubmenu(target.row);
            sub_hover_ = sub_model_.FirstEnabled();
            if (RenderSub()) PresentSub(255);
        } else if (item->command != 0) {
            result_ = item->command;
            Dismiss();
        }
        return true;
    }
    const size_t next = NextMenuMnemonicTarget(targets, in_sub ? sub_hover_ : hover_row_,
                                               in_sub ? sub_hover_swatch_ : hover_swatch_);
    if (in_sub) {
        sub_hover_ = targets[next].row;
        sub_hover_swatch_ = targets[next].swatch;
        if (RenderSub()) PresentSub(255);
    } else {
        CloseSubmenu();
        UpdateHover(targets[next].row, targets[next].swatch);
    }
    return true;
}

float FluentMenu::FilterHeaderPx() const {
    if (!filter_fn_ || external_edit_) return 0.0f;
    if (anchor_to_rect_) {
        const float h = static_cast<float>(anchor_rect_.bottom - anchor_rect_.top);
        return (std::max)(h, 32.0f * scale_);
    }
    return 44.0f * scale_;
}

bool FluentMenu::IsPaletteHwnd(HWND hwnd) const {
    return hwnd && (hwnd == hwnd_ || hwnd == edit_ || hwnd == external_edit_ || hwnd == sub_hwnd_);
}

bool FluentMenu::EnsureFilterEdit() {
    if (!filter_fn_ || !hwnd_ || external_edit_) return false;
    // The menu outlives settings changes: follow the interface font size.
    if (!edit_font_ || edit_font_generation_ != typography::Generation() ||
        std::abs(edit_font_scale_ - scale_) > 0.001f) {
        if (HFONT font = typography::CreateEditFont(scale_)) {
            if (edit_font_) DeleteObject(edit_font_);
            edit_font_ = font;
            edit_font_generation_ = typography::Generation();
            edit_font_scale_ = scale_;
            if (edit_) SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_), FALSE);
        }
    }
    if (edit_brush_) { DeleteObject(edit_brush_); edit_brush_ = nullptr; }
    edit_brush_ = CreateSolidBrush(dark_ ? RGB(30, 30, 30) : RGB(255, 255, 255));
    if (edit_) return true;

    edit_ = CreateChildEdit(hwnd_);
    if (!edit_) return false;
    SetWindowTheme(edit_, L"", L"");
    SendMessageW(edit_, WM_SETFONT, (WPARAM)edit_font_, TRUE);
    SendMessageW(edit_, EM_SETCUEBANNER, TRUE,
        reinterpret_cast<LPARAM>(l10n::Get(l10n::StringId::TabMenuSearch).c_str()));
    SetWindowSubclass(edit_, FilterEditProc, 1, reinterpret_cast<DWORD_PTR>(this));
    return true;
}

void FluentMenu::PlaceFilterEdit(int y_offset_px) {
    if (!edit_ || !filter_fn_ || external_edit_) return;
    const float s = scale_;
    int x = 0, y = 0, w = 40, cell_h = 18;
    if (anchor_to_rect_) {
        const float left = 12.0f * s;
        const float right = 8.0f * s;
        x = anchor_rect_.left + static_cast<int>(std::lround(left));
        w = std::max(40, static_cast<int>(std::lround(
            static_cast<float>(anchor_rect_.right - anchor_rect_.left) - left - right)));
        cell_h = std::max(18, static_cast<int>(anchor_rect_.bottom - anchor_rect_.top));
        y = anchor_rect_.top;
    } else {
        const float header = FilterHeaderPx();
        const float field_l = static_cast<float>(kShadowMargin) + 8.0f * s;
        const float field_t = static_cast<float>(kShadowMargin) + 6.0f * s;
        const float field_r = static_cast<float>(kShadowMargin + model_.WidthPx()) - 8.0f * s;
        const float field_b = static_cast<float>(kShadowMargin) + header - 4.0f * s;
        const float text_l = field_l + 34.0f * s;
        const float text_r = field_r - 8.0f * s;
        x = base_x_ + static_cast<int>(std::lround(text_l));
        y = base_y_ + y_offset_px + static_cast<int>(std::lround(field_t));
        w = std::max(40, static_cast<int>(std::lround(text_r - text_l)));
        cell_h = std::max(18, static_cast<int>(std::lround(field_b - field_t)));
    }
    const int line_h = EditLineHeight(edit_, edit_font_, cell_h);
    y += std::max(0, (cell_h - line_h) / 2);
    POINT client{x, y};
    ScreenToClient(hwnd_, &client);
    SetWindowPos(edit_, HWND_TOP, client.x, client.y, w, line_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void FluentMenu::HideFilterEdit() {
    if (edit_ && IsWindow(edit_)) ShowWindow(edit_, SW_HIDE);
}

void FluentMenu::SyncFilterFromEdit() {
    const HWND edit = external_edit_ ? external_edit_ : edit_;
    if (!edit || !filter_fn_) return;
    const int length = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    text.resize(GetWindowTextW(edit, text.data(), length + 1));
    if (filter_query_ == text) return;
    filter_query_ = std::move(text);
    RefreshFilter();
}

LRESULT CALLBACK FluentMenu::ExternalFilterEditProc(HWND hwnd, UINT msg, WPARAM wParam,
        LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<FluentMenu*>(data);
    if (msg == WM_IME_STARTCOMPOSITION) self->filter_composing_ = true;
    if (msg == WM_IME_ENDCOMPOSITION) self->filter_composing_ = false;
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && self->HandleFilterKey(msg, wParam))
        return 0;
    if (!self->filter_composing_ && msg == WM_CHAR && (wParam == VK_RETURN || wParam == VK_ESCAPE || wParam == VK_TAB))
        return 0;
    const auto result = DefSubclassProc(hwnd, msg, wParam, lParam);
    if (!self->filter_composing_ && (msg == WM_CHAR || msg == WM_SETTEXT || msg == WM_PASTE || msg == WM_CUT ||
        msg == WM_CLEAR || msg == WM_UNDO || msg == WM_KEYDOWN || msg == WM_IME_ENDCOMPOSITION))
        self->SyncFilterFromEdit();
    return result;
}

LRESULT CALLBACK FluentMenu::FilterEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR, DWORD_PTR dwRefData) {
    auto* self = reinterpret_cast<FluentMenu*>(dwRefData);
    if (!self) return DefSubclassProc(hwnd, msg, wParam, lParam);
    // Menu-specific: IME state, navigation keys and live filtering.
    if (msg == WM_IME_STARTCOMPOSITION) self->filter_composing_ = true;
    if (msg == WM_IME_ENDCOMPOSITION) self->filter_composing_ = false;
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !self->filter_composing_ &&
        (wParam == VK_ESCAPE || wParam == VK_UP || wParam == VK_DOWN || wParam == VK_RETURN)) {
        self->HandleFilterKey(msg, wParam);
        return 0;
    }
    if (msg == WM_CHAR && (wParam == VK_RETURN || wParam == VK_ESCAPE)) return 0;
    // Everything visual is the shared hosted-edit handling: LumaText
    // presentation, the caret timer, and the switch back to the native EDIT
    // when a layered present fails (so the filter never turns invisible).
    LRESULT result = 0;
    if (!self->compositor_) {
        result = DefSubclassProc(hwnd, msg, wParam, lParam);
    } else {
        const D2D1_COLOR_F fg = ColorFromRef(EditTextColor(self->dark_));
        const D2D1_COLOR_F bg = ColorFromRef(EditBackColor(self->dark_));
        IDWriteTextFormat* format = self->compositor_->TextFormat();
        if (!HandleChildEditMessage(*self->compositor_, format, fg, bg, EditBackBrush(self->edit_brush_),
                                    hwnd, msg, wParam, lParam, result)) {
            result = DefPresentedChildEditProc(*self->compositor_, format, fg, bg, hwnd, msg, wParam, lParam);
        }
    }
    // Delete has no WM_CHAR; every message that can change the text resyncs
    // (SyncFilterFromEdit returns early when nothing changed).
    if (!self->filter_composing_ &&
        (msg == WM_CHAR || msg == WM_KEYDOWN || msg == WM_PASTE || msg == WM_CUT || msg == WM_CLEAR ||
         msg == WM_UNDO || msg == WM_SETTEXT || msg == WM_IME_ENDCOMPOSITION))
        self->SyncFilterFromEdit();
    return result;
}

bool FluentMenu::AppendFilterChar(wchar_t ch) {
    if (!filter_fn_ || ch < 32 || ch == 127) return false;
    filter_query_.push_back(ch);
    RefreshFilter();
    return true;
}

void FluentMenu::PasteFilterText() {
    if (!filter_fn_ || !OpenClipboard(hwnd_ ? hwnd_ : owner_)) return;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data) {
        if (const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data))) {
            for (const wchar_t* p = text; *p; ++p) {
                if (*p >= 32 && *p != 127) filter_query_.push_back(*p);
            }
            GlobalUnlock(data);
            RefreshFilter();
        }
    }
    CloseClipboard();
}

bool FluentMenu::HandleFilterKey(UINT msg, WPARAM wParam) {
    if (filter_composing_) return false;
    if (!filter_fn_ || animating_out_) return false;
    if (msg == WM_CHAR || msg == WM_SYSCHAR)
        return AppendFilterChar(static_cast<wchar_t>(wParam));
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) return false;

    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (ctrl && wParam == VK_TAB) {
        result_ = 0;
        open_ = false;
        forward_tab_key_ = true;
        UpdateTooltip(-1);
        HideFilterEdit();
        HideSubWindow();
        if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
        return true;
    }
    if (wParam == VK_ESCAPE) {
        UpdateTooltip(-1);
        result_ = 0;
        HideFilterEdit();
        animating_out_ = true;
        anim_start_ = std::chrono::steady_clock::now();
        return true;
    }
    if (wParam == VK_DOWN || wParam == VK_UP) {
        int next = hover_row_ < 0
            ? model_.FirstEnabled()
            : model_.NextEnabled(hover_row_, wParam == VK_DOWN ? 1 : -1);
        UpdateHover(next);
        return true;
    }
    if (wParam == VK_RETURN) {
        if (hover_row_ >= 0) {
            int cmd = InvokeRow(hover_row_);
            if (cmd != 0) { result_ = cmd; Dismiss(); }
        } else {
            filter_committed_ = true;
            result_ = 0;
            Dismiss();
        }
        return true;
    }
    if (wParam == VK_BACK) {
        if (edit_ || external_edit_) return false;
        if (!filter_query_.empty()) {
            filter_query_.pop_back();
            RefreshFilter();
        }
        return true;
    }
    if (ctrl && (wParam == L'V' || wParam == L'v')) {
        if (edit_ || external_edit_) return false;
        PasteFilterText();
        return true;
    }
    return false;
}

int FluentMenu::RunModalLoop() {
    MSG msg{};
    while (open_) {
        // Animation progress (fade + slide for open, fade for dismiss).
        if (hwnd_) {
            double elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - anim_start_).count();
            if (animating_out_) {
                const float kOutMs = 80.0f; // ui.md §6 菜单收起
                float t = (float)(elapsed / kOutMs);
                if (t >= 1.0f) {
                    open_ = false;
                    ShowWindow(hwnd_, SW_HIDE);
                    break;
                }
                Present((BYTE)(255 * (1.0f - t)), 0);
            } else {
                // ui.md §6: 淡入 120ms + 下滑 8px OutQuad.
                fluent::MotionSpec spec{ 120.0f, fluent::EasingCurve::OutQuad };
                float t = fluent::EvaluateMotion(spec, (float)elapsed);
                if (t < 1.0f) {
                    const float dip = (anchor_to_rect_ && filter_fn_)
                        ? 0.0f : fluent::motion::MenuTravelDip * scale_;
                    Present((BYTE)(255 * t), (int)std::lround(-dip * (1.0f - t)));
                } else {
                    Present(255, 0);
                }
            }
        }
        // Hover-to-open flyout delay (like native menus).
        if (open_ && !animating_out_ && sub_pending_row_ >= 0) {
            const double dwell = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - sub_pending_since_).count();
            if (dwell >= 200.0) {
                const int r = sub_pending_row_;
                sub_pending_row_ = -1;
                if (r == hover_row_) OpenSubmenu(r);
            }
        }
        if (open_ && !animating_out_ && overflow_) TickArrowScroll();
        DWORD wait = MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
        (void)wait;
        if (topmost_ && open_ && !animating_out_ &&
            ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000)) {
            // A press over the foreign overlay never reaches this thread's
            // queue; treat it like any other click outside the palette.
            POINT cursor{};
            GetCursorPos(&cursor);
            const HWND under = WindowFromPoint(cursor);
            if (under && !IsPaletteHwnd(under) &&
                GetWindowThreadProcessId(under, nullptr) != GetCurrentThreadId()) {
                result_ = 0;
                open_ = false;
                HideFilterEdit();
                HideSubWindow();
                ShowWindow(hwnd_, SW_HIDE);
            }
        }
        while (open_ && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                open_ = false;
                PostQuitMessage((int)msg.wParam);
                break;
            }
            bool swallow = false;
            if (open_ && !animating_out_) {
                switch (msg.message) {
                case WM_MOUSEWHEEL:
                    if (Scrollable()) {
                        SendMessageW(hwnd_, WM_MOUSEWHEEL, msg.wParam, msg.lParam);
                        swallow = true;
                    }
                    break;
                case WM_KEYDOWN:
                case WM_SYSKEYDOWN:
                    if (HandleFilterKey(msg.message, msg.wParam)) {
                        swallow = true;
                    } else if (!filter_fn_ && msg.wParam == VK_ESCAPE) {
                        if (sub_parent_row_ >= 0) {
                            CloseSubmenu(); // first Esc only closes the flyout
                        } else {
                            result_ = 0;
                            animating_out_ = true;
                            anim_start_ = std::chrono::steady_clock::now();
                        }
                        swallow = true;
                    } else if (!filter_fn_ && (msg.wParam == VK_DOWN || msg.wParam == VK_UP)) {
                        if (sub_parent_row_ >= 0) {
                            sub_hover_ = sub_hover_ < 0
                                ? sub_model_.FirstEnabled()
                                : sub_model_.NextEnabled(sub_hover_, msg.wParam == VK_DOWN ? 1 : -1);
                            if (RenderSub()) PresentSub(255);
                        } else {
                            int next = hover_row_ < 0
                                ? model_.FirstEnabled()
                                : model_.NextEnabled(hover_row_, msg.wParam == VK_DOWN ? 1 : -1);
                            UpdateHover(next);
                        }
                        swallow = true;
                    } else if (!filter_fn_ && msg.wParam == VK_RIGHT && sub_parent_row_ < 0 &&
                               hover_row_ >= 0 && model_.At(hover_row_) &&
                               !model_.At(hover_row_)->children.empty()) {
                        OpenSubmenu(hover_row_);
                        sub_hover_ = sub_model_.FirstEnabled();
                        if (RenderSub()) PresentSub(255);
                        swallow = true;
                    } else if (!filter_fn_ && msg.wParam == VK_LEFT && sub_parent_row_ >= 0) {
                        CloseSubmenu();
                        swallow = true;
                    } else if (!filter_fn_ && msg.wParam == VK_RETURN) {
                        if (sub_parent_row_ >= 0) {
                            const FluentMenuItem* child = sub_model_.At(sub_hover_);
                            if (child && child->enabled && child->command != 0) {
                                result_ = child->command;
                                Dismiss();
                            }
                        } else if (hover_row_ >= 0) {
                            const FluentMenuItem* hov = model_.At(hover_row_);
                            if (hov && hov->enabled && !hov->children.empty()) {
                                OpenSubmenu(hover_row_);
                                sub_hover_ = sub_model_.FirstEnabled();
                                if (RenderSub()) PresentSub(255);
                            } else {
                                int cmd = InvokeRow(hover_row_);
                                if (cmd != 0) { result_ = cmd; Dismiss(); }
                            }
                        }
                        swallow = true;
                    } else if (HandleMnemonicKey(msg)) {
                        swallow = true;
                    } else if (filter_fn_ && !edit_ && !external_edit_) {
                        msg.hwnd = hwnd_;
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                        swallow = true;
                    }
                    break;
                case WM_KEYUP:
                case WM_SYSKEYUP:
                    if (filter_fn_ && !edit_ && !external_edit_) swallow = true;
                    break;
                case WM_CHAR:
                case WM_SYSCHAR:
                    if (!edit_ && !external_edit_ && HandleFilterKey(msg.message, msg.wParam)) swallow = true;
                    break;
                case WM_LBUTTONDOWN:
                case WM_RBUTTONDOWN:
                case WM_MBUTTONDOWN:
                case WM_NCLBUTTONDOWN:
                case WM_NCRBUTTONDOWN:
                    if (!IsPaletteHwnd(msg.hwnd)) {
                        result_ = 0;
                        open_ = false;
                        HideFilterEdit();
                        HideSubWindow();
                        ShowWindow(hwnd_, SW_HIDE);
                    }
                    break;
                case WM_ACTIVATE:
                    if (msg.hwnd == owner_ && LOWORD(msg.wParam) == WA_INACTIVE) {
                        HWND newly = reinterpret_cast<HWND>(msg.lParam);
                        if (!IsPaletteHwnd(newly)) {
                            result_ = 0;
                            open_ = false;
                            HideFilterEdit();
                            HideSubWindow();
                            ShowWindow(hwnd_, SW_HIDE);
                        }
                    }
                    break;
                default:
                    break;
                }
            }
            if (!swallow) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    HideSubWindow();
    if (hwnd_ && IsWindow(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
    HideFilterEdit();
    return result_;
}

int FluentMenu::TrackDropdown(RECT control_rect, std::vector<FluentMenuItem> items) {
    SetDropdownRect(control_rect);
    for (auto& item : items) {
        if (item.checked) item.glyph = L"\xE73E";
    }
    return TrackPopup({control_rect.left, control_rect.bottom}, std::move(items));
}

int FluentMenu::TrackPopup(POINT screen_pt, std::vector<FluentMenuItem> items,
                           FilterFn filter, bool top_center) {
    if (!EnsureWindow()) {
        dropdown_ = false;
        external_edit_ = nullptr;
        max_visible_rows_ = 0;
        return 0;
    }
    HideSubWindow(); // stale flyout state from the previous invocation
    filter_fn_ = std::move(filter);
    filter_query_ = initial_filter_text_;
    filter_committed_ = false;
    filter_composing_ = false;
    forward_tab_key_ = false;
    popup_pt_ = screen_pt;
    top_center_ = top_center;
    scroll_y_ = 0.0f;
    body_limit_px_ = 0.0f;
    overflow_ = false;
    arrow_hover_ = 0;
    arrow_tick_ = {};
    if (items.empty() && !filter_fn_) {
        dropdown_ = false;
        external_edit_ = nullptr;
        max_visible_rows_ = 0;
        return 0;
    }
    const float minW = filter_fn_
        ? (filter_min_width_ > 0.0f ? filter_min_width_ : 440.0f) * scale_
        : dropdown_ ? static_cast<float>(dropdown_rect_.right - dropdown_rect_.left) : 0.0f;
    model_.SetItems(std::move(items));
    model_.Layout(compositor_->DwriteFactory(), scale_, minW);
    hover_row_ = hover_first_on_open_ ? model_.FirstEnabled() : -1;
    hover_swatch_ = -1;
    result_ = 0;
    animating_out_ = false;
    LayoutWindow(screen_pt);
    if (!Render()) {
        external_edit_ = nullptr;
        max_visible_rows_ = 0;
        ShowWindow(hwnd_, SW_HIDE);
        anchor_to_rect_ = false;
        dropdown_ = false;
        hover_first_on_open_ = true;
        select_all_on_open_ = true;
        return 0;
    }
    open_ = true;
    anim_start_ = std::chrono::steady_clock::now();
    const int slide = (anchor_to_rect_ && filter_fn_)
        ? 0 : (int)std::lround(-fluent::motion::MenuTravelDip * scale_);
    Present(0, slide);
    if (external_edit_) {
        HideFilterEdit();
        SetWindowSubclass(external_edit_, ExternalFilterEditProc, 2, reinterpret_cast<DWORD_PTR>(this));
    } else if (filter_fn_ && EnsureFilterEdit()) {
        SetWindowTextW(edit_, filter_query_.c_str());
        PlaceFilterEdit(slide);
        SetForegroundWindow(GetAncestor(edit_, GA_ROOT));
        SetFocus(edit_);
        if (select_all_on_open_) {
            SendMessageW(edit_, EM_SETSEL, 0, -1);
        } else {
            const int len = GetWindowTextLengthW(edit_);
            SendMessageW(edit_, EM_SETSEL, len, len);
        }
    } else {
        HideFilterEdit();
    }
    int popup_result = 0;
    {
        const bool command_menu = !filter_fn_ && !external_edit_;
        ImeSuspension ime(command_menu ? GetFocus() : nullptr, command_menu ? hwnd_ : nullptr);
        popup_result = RunModalLoop();
    }
    if (external_edit_) RemoveWindowSubclass(external_edit_, ExternalFilterEditProc, 2);
    external_edit_ = nullptr;
    UpdateTooltip(-1);
    initial_filter_text_.clear();
    filter_min_width_ = 0.0f;
    max_visible_rows_ = 0;
    scroll_y_ = 0.0f;
    overflow_ = false;
    arrow_hover_ = 0;
    arrow_tick_ = {};
    anchor_to_rect_ = false;
    dropdown_ = false;
    hover_first_on_open_ = true;
    select_all_on_open_ = true;
    // Queue only after the modal loop has unwound, avoiding nested tab changes.
    if (forward_tab_key_ && owner_) PostMessageW(owner_, WM_KEYDOWN, VK_TAB, 0);
    return popup_result;
}

void FluentMenu::RefreshFilter() {
    if (!filter_fn_ || !compositor_) return;
    auto items = filter_fn_(filter_query_);
    scroll_y_ = 0.0f;
    UpdateTooltip(-1);
    if (items.empty()) {
        FluentMenuItem none;
        none.text = filter_query_.empty() ? pulse::l10n::Get(pulse::l10n::StringId::MenuTypeSearch).c_str() : pulse::l10n::Get(pulse::l10n::StringId::MenuNoMatch).c_str();
        none.enabled = false;
        items.push_back(std::move(none));
    }
    model_.SetItems(std::move(items));
    model_.Layout(compositor_->DwriteFactory(), scale_,
                  (filter_min_width_ > 0.0f ? filter_min_width_ : 440.0f) * scale_);
    hover_row_ = model_.FirstEnabled();
    hover_swatch_ = -1;
    LayoutWindow(popup_pt_);
    if (Render()) Present(255, present_offset_);
    // Refresh only the results; a cached internal editor may be hidden while
    // an external search field owns input, selection and IME composition.
}

void FluentMenu::RequestFilterRefresh() {
    if (hwnd_ && open_) PostMessageW(hwnd_, WM_APP, 0, 0);
}

bool FluentMenu::ReplaceItems(std::vector<FluentMenuItem> items) {
    if (!open_ || animating_out_ || filter_fn_ || !compositor_ || items.empty()) return false;
    // A flyout owns a copy of the displayed command IDs. Keep both that copy
    // and the controller's display snapshot until it closes; IDs are session-local.
    if (sub_parent_row_ >= 0) return false;
    if (model_.PatchCommands(items)) return true;
    // The rows changed shape: a context menu opened before the shell host
    // answered (no cached layout for this type yet) only had Pulse's own rows,
    // and the Explorer section (New, archivers, ...) arrives afterwards. Rebuild
    // in place so those rows show up in this menu instead of the next one
    // (#73). Shell rows are appended after the built-in ones, so the rows under
    // the cursor keep their place. A visible flyout would be yanked away by the
    // relayout, so that case keeps the current rows.
    if (dropdown_ || sub_parent_row_ >= 0) return false;
    std::wstring hover_text;
    if (const FluentMenuItem* it = model_.At(hover_row_)) hover_text = it->text;
    model_.SetItems(std::move(items));
    model_.Layout(compositor_->DwriteFactory(), scale_, 0.0f);
    hover_row_ = -1;
    if (!hover_text.empty()) {
        for (int i = 0; i < model_.Count(); ++i) {
            const FluentMenuItem* it = model_.At(i);
            if (it && it->enabled && it->text == hover_text) { hover_row_ = i; break; }
        }
    }
    hover_swatch_ = -1;
    sub_pending_row_ = -1;
    UpdateTooltip(-1);
    LayoutWindow(popup_pt_);
    // The modal loop presents every frame (and owns the open fade).
    Render();
    return true;
}

void FluentMenu::Dismiss() {
    if (!open_) return;
    UpdateTooltip(-1);
    HideFilterEdit();
    HideSubWindow();
    animating_out_ = true;
    anim_start_ = std::chrono::steady_clock::now();
}

bool FluentMenu::SaveDebugSnapshot(const wchar_t* png_path,
                                   std::vector<FluentMenuItem> items, int hover_row) {
    if (!compositor_ || items.empty()) return false;
    model_.SetItems(std::move(items));
    model_.Layout(compositor_->DwriteFactory(), scale_, dropdown_
        ? static_cast<float>(dropdown_rect_.right - dropdown_rect_.left) : filter_min_width_ * scale_);
    hover_row_ = hover_row;
    hover_swatch_ = -1;
    if (!Render()) return false;

    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic))))
        return false;
    ComPtr<IWICStream> stream;
    wic->CreateStream(&stream);
    if (!stream.get() || FAILED(stream->InitializeFromFilename(png_path, GENERIC_WRITE)))
        return false;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr))) return false;
    if (FAILED(frame->Initialize(nullptr))) return false;
    if (FAILED(frame->SetSize((UINT)surf_.w, (UINT)surf_.h))) return false;
    // The layered-window DIB contains premultiplied alpha. Tell WIC that
    // explicitly; treating it as straight BGRA darkens thin CJK/icon strokes
    // in visual-regression PNGs and makes them appear partially clipped.
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    if (FAILED(frame->WritePixels((UINT)surf_.h, (UINT)surf_.w * 4,
                                  (UINT)surf_.w * 4 * (UINT)surf_.h,
                                  static_cast<BYTE*>(surf_.bits))))
        return false;
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(encoder->Commit());
}

} // namespace pulse::ui
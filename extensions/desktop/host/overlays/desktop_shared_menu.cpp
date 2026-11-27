#include "host/overlays/desktop_overlays.h"
#include "host/render/desktop_appearance.h"
#include "host/render/wallpaper_backdrop.h"
#include "host/render/host_backdrop_layer.h"
#include "../../../../src/ui/edit_host.h"
#include "../../../../src/ui/typography.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <imm.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace pulse::desktop::overlays {
namespace {
class MenuHostBackdrop final : public ui::NativeMenuBackdrop {
public:
    MenuHostBackdrop(bool dark, uint8_t opacity, const render::WallpaperBackdrop* source)
        : dark_(dark), profile_(render::CardAcrylicProfile(dark, opacity)), source_(source) {}
    bool Initialize(HWND hwnd) noexcept {
        // The menu paints its own 12-DIP corners and border: DWM's 8 px rounding and 1 px border would add a
        // second, tighter corner outside them (as dialog_jump's surfaces do, opt out of both).
        const DWORD no_round = DWMWCP_DONOTROUND;
        const COLORREF no_border = DWMWA_COLOR_NONE;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &no_round, sizeof(no_round));
        DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &no_border, sizeof(no_border));
        return layer_.Initialize(hwnd) && layer_.SetMaterial(dark_, profile_.tint_opacity,
            profile_.luminosity_opacity, render::ColorToArgb(render::CardMaterialTint(dark_)));
    }
    bool Update(UINT width, UINT height, float radius) noexcept override {
        layer_.BeginFrame();
        const bool added = layer_.AddCard(D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)), radius, 1.0f);
        layer_.EndFrame();
        return added && layer_.Active();
    }
    bool PaintForeground(ID2D1DeviceContext* dc, const D2D1_RECT_F& card, float radius, bool dark) noexcept override {
        (void)dark;
        if (source_) {
            if (auto* noise = source_->MenuNoiseBrush(dc))
                dc->FillRoundedRectangle(D2D1::RoundedRect(card, radius, radius), noise);
        }
        return true;
    }
private:
    render::HostBackdropLayer layer_;
    bool dark_;
    render::CardMaterialProfile profile_;
    const render::WallpaperBackdrop* source_;
};

D2D1_COLOR_F MenuColor(uint32_t argb) {
    return D2D1::ColorF(static_cast<float>((argb >> 16) & 255) / 255.0f,
                       static_cast<float>((argb >> 8) & 255) / 255.0f,
                       static_cast<float>(argb & 255) / 255.0f,
                       static_cast<float>(argb >> 24) / 255.0f);
}

uint32_t HashGlassConfig(const model::BeautifySettings& s) noexcept {
    uint32_t h = 2166136261u;
    const auto mix = [&](uint32_t v) {
        h ^= v;
        h *= 16777619u;
    };
    mix(s.glass_thickness);
    mix(s.glass_ior_x100);
    mix(s.glass_dispersion);
    mix(s.glass_glare);
    mix(static_cast<uint32_t>(static_cast<int32_t>(s.glass_glare_angle) + 360));
    mix(s.glass_frost);
    return h;
}

uint64_t BuildSharedMenuMaterialKey(bool native_acrylic, bool dark, bool liquid_glass,
                                    const model::BeautifySettings& s, float scale) noexcept {
    const uint32_t gh = liquid_glass ? HashGlassConfig(s) : 0u;
    return static_cast<uint64_t>(native_acrylic) |
           (static_cast<uint64_t>(dark) << 1) |
           (static_cast<uint64_t>(liquid_glass) << 2) |
           (static_cast<uint64_t>(s.card_opacity_percent) << 8) |
           (static_cast<uint64_t>(scale * 1024.0f) << 16) |
           (static_cast<uint64_t>(gh) << 32);
}

void AssignSharedMenuGlassPainter(ui::FluentMenu* menu,
                                  const render::WallpaperBackdrop* source,
                                  const model::BeautifySettings& glass_cfg) {
    if (!menu || !source) return;
    const uint8_t opacity = glass_cfg.card_opacity_percent;
    const uint8_t disp_tier = model::MapGlassGainToLegacyTier(glass_cfg.glass_dispersion);
    menu->SetMaterialPainter(
        [menu, source, opacity, disp_tier, glass_cfg](ID2D1DeviceContext* dc, const D2D1_RECT_F& card_px,
                                                 POINT popup_px, float radius_px, bool dark) {
            const bool drawn = source->DrawMenuMaterial(
                dc, card_px, popup_px, radius_px, model::CardMaterial::LiquidGlass,
                dark, opacity, disp_tier, &glass_cfg);
            if (source->MenuUnderlayPending()) menu->RequestMaterialRefresh();
            return drawn;
        });
}
}

void DesktopOverlayController::TrimIdleResources() noexcept {
    if (shared_menu_tracking_ || IsOpen() || IsInlineRenaming()) return;
    DestroyHostedPopupEdit();
    shared_menu_.reset();
    if (backdrop_) backdrop_->ReleaseMenuMaterialCache();
    shared_menu_compositor_.reset();
    ui_compositor_scale_ = 0.0f;
    if (popup_edit_font_14_) {
        DeleteObject(popup_edit_font_14_);
        popup_edit_font_14_ = nullptr;
    }
    if (popup_edit_font_13_) {
        DeleteObject(popup_edit_font_13_);
        popup_edit_font_13_ = nullptr;
    }
    if (popup_edit_brush_) {
        DeleteObject(popup_edit_brush_);
        popup_edit_brush_ = nullptr;
    }
    ReleaseRenderDevices();
}

bool DesktopOverlayController::EnsureUiCompositor() {
    const auto ensure_fonts = [this]() {
        if (shared_menu_compositor_ && std::abs(ui_compositor_scale_ - scale_) > 0.001f) {
            shared_menu_compositor_->RecreateTextFormats(scale_);
            ui_compositor_scale_ = scale_;
            if (popup_edit_font_14_) {
                DeleteObject(popup_edit_font_14_);
                popup_edit_font_14_ = nullptr;
            }
            if (popup_edit_font_13_) {
                DeleteObject(popup_edit_font_13_);
                popup_edit_font_13_ = nullptr;
            }
        }
        if (!popup_edit_font_14_) {
            popup_edit_font_14_ = ui::typography::CreateEditFont(scale_, 14.0f);
        }
        if (!popup_edit_font_13_) {
            popup_edit_font_13_ = ui::typography::CreateEditFont(scale_, 13.0f);
        }
        if (popup_edit_brush_ && popup_edit_brush_dark_ != dark_mode_) {
            DeleteObject(popup_edit_brush_);
            popup_edit_brush_ = nullptr;
        }
        if (!popup_edit_brush_) {
            popup_edit_brush_ = CreateSolidBrush(dark_mode_ ? RGB(28, 32, 42) : RGB(250, 250, 252));
            popup_edit_brush_dark_ = dark_mode_;
        }
    };

    if (shared_menu_compositor_ && ui_device_hwnd_ && IsWindow(ui_device_hwnd_) &&
        shared_menu_compositor_->Hwnd() == ui_device_hwnd_) {
        ensure_fonts();
        return true;
    }
    DestroyHostedPopupEdit();
    shared_menu_.reset();
    shared_menu_compositor_.reset();
    ui_compositor_scale_ = 0.0f;
    if (!ui_device_hwnd_ || !IsWindow(ui_device_hwnd_)) {
        HINSTANCE hinst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = hinst;
        wc.lpszClassName = L"PulseDesktopUiDevice";
        RegisterClassExW(&wc);
        // Hidden and never shown: it only gives the compositor a window to bind to.
        ui_device_hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"",
                                          WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, hinst, nullptr);
        if (!ui_device_hwnd_) return false;
    }
    auto compositor = std::make_unique<ui::Compositor>();
    if (!compositor->Init(ui_device_hwnd_)) return false;
    compositor->Resize(1, 1);
    shared_menu_compositor_ = std::move(compositor);
    ensure_fonts();
    return true;
}

D2D1_COLOR_F DesktopOverlayController::HostedPopupEditForeground() const noexcept {
    if (active_kind_ == OverlayKind::StickyNotePopover &&
        sticky_tile_id_ != "@todos" && sticky_tile_id_ != "@weather_city" &&
        !sticky_tile_id_.starts_with("@title:")) {
        return MenuColor(dark_mode_ ? 0xF8FFF6D6u : 0xF83A2A0Du);
    }
    return MenuColor(dark_mode_ ? 0xF8FFFFFFu : 0xF8181E28u);
}

IDWriteTextFormat* DesktopOverlayController::HostedPopupEditFormat() const noexcept {
    if (!shared_menu_compositor_) return nullptr;
    return popup_edit_use_13px_ ? shared_menu_compositor_->FileNameFormat()
                                : shared_menu_compositor_->TextFormat();
}

std::wstring DesktopOverlayController::ReadHostedPopupEditText() const {
    if (!popup_edit_hwnd_ || !IsWindow(popup_edit_hwnd_)) return {};
    const int len = GetWindowTextLengthW(popup_edit_hwnd_);
    if (len <= 0) return {};
    std::wstring raw(static_cast<size_t>(len) + 1, L'\0');
    const int copied = GetWindowTextW(popup_edit_hwnd_, raw.data(), len + 1);
    raw.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    raw.erase(std::remove(raw.begin(), raw.end(), L'\r'), raw.end());
    return raw;
}

void DesktopOverlayController::PlaceHostedPopupEdit() {
    if (!popup_edit_hwnd_ || !IsWindow(popup_edit_hwnd_) || !hwnd_) return;
    const float pad = using_dcomp_ ? static_cast<float>(kShadowMargin) : 0.0f;
    const float left_dip = pad + popup_edit_card_dip_.left;
    const float top_dip = pad + popup_edit_card_dip_.top;
    const float right_dip = pad + popup_edit_card_dip_.right;
    const float bottom_dip = pad + popup_edit_card_dip_.bottom;

    const int px_x = static_cast<int>(std::lround(left_dip * scale_));
    const int px_w = std::max(24, static_cast<int>(std::lround((right_dip - left_dip) * scale_)));
    const int box_px_y = static_cast<int>(std::lround(top_dip * scale_));
    const int box_px_h = std::max(16, static_cast<int>(std::lround((bottom_dip - top_dip) * scale_)));

    int px_y = box_px_y;
    int px_h = box_px_h;
    HFONT font = popup_edit_use_13px_ ? popup_edit_font_13_ : popup_edit_font_14_;
    if (!popup_edit_multiline_) {
        const int line_h = ui::EditLineHeight(popup_edit_hwnd_, font, box_px_h);
        px_y = box_px_y + std::max(0, (box_px_h - line_h) / 2);
        px_h = std::max(1, line_h);
    } else {
        SetPropW(popup_edit_hwnd_, L"Pulse.EditWrapW",
                 reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(px_w) * 64u));
    }

    RECT cur{};
    GetWindowRect(popup_edit_hwnd_, &cur);
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&cur), 2);
    const bool placed = (cur.left == px_x && cur.top == px_y &&
                         (cur.right - cur.left) == px_w && (cur.bottom - cur.top) == px_h);
    const bool visible = (GetWindowLongPtrW(popup_edit_hwnd_, GWL_STYLE) & WS_VISIBLE) != 0;
    if (!placed || !visible) {
        SetWindowPos(popup_edit_hwnd_, HWND_TOP, px_x, px_y, px_w, px_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

bool DesktopOverlayController::AttachHostedPopupEdit(
    const D2D1_RECT_F& card_dip_rect, const std::wstring& initial_text,
    const std::wstring& cue_text, bool multiline, bool use_13px,
    int select_start, int select_end, uint32_t max_chars) {
    DestroyHostedPopupEdit();
    if (!EnsureWindowCreated() || !EnsureUiCompositor()) return false;

    popup_edit_card_dip_ = card_dip_rect;
    popup_edit_cue_ = cue_text;
    popup_edit_multiline_ = multiline;
    popup_edit_use_13px_ = use_13px;
    popup_edit_composing_ = false;
    popup_edit_ignore_killfocus_ = false;

    std::wstring crlf_text;
    crlf_text.reserve(initial_text.size() + 8);
    for (wchar_t ch : initial_text) {
        if (ch == L'\n') crlf_text.push_back(L'\r');
        crlf_text.push_back(ch);
    }

    const DWORD style = multiline ? (ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN) : 0u;
    popup_edit_hwnd_ = ui::CreatePulsePaintedChildEdit(hwnd_, crlf_text.c_str(), style, true);
    if (!popup_edit_hwnd_) return false;

    SetWindowTheme(popup_edit_hwnd_, L"", L"");
    HFONT font = use_13px ? popup_edit_font_13_ : popup_edit_font_14_;
    if (font) {
        SendMessageW(popup_edit_hwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    }
    SendMessageW(popup_edit_hwnd_, EM_SETLIMITTEXT, max_chars, 0);

    if (multiline) {
        int line_px = std::max(18, static_cast<int>(std::lround(21.0f * scale_)));
        if (font) {
            HDC hdc = GetDC(popup_edit_hwnd_);
            if (hdc) {
                HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
                TEXTMETRICW tm{};
                if (GetTextMetricsW(hdc, &tm) && tm.tmHeight > 0) {
                    line_px = std::max<int>(tm.tmHeight + static_cast<int>(std::lround(3.0f * scale_)), tm.tmHeight);
                }
                SelectObject(hdc, old);
                ReleaseDC(popup_edit_hwnd_, hdc);
            }
        }
        SetPropW(popup_edit_hwnd_, L"Pulse.EditMultiline",
                 reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(line_px) * 64u));
        if (!popup_edit_cue_.empty()) {
            SetPropW(popup_edit_hwnd_, L"Pulse.EditCue",
                     reinterpret_cast<HANDLE>(const_cast<wchar_t*>(popup_edit_cue_.c_str())));
            const bool warm = (active_kind_ == OverlayKind::StickyNotePopover &&
                               sticky_tile_id_ != "@todos");
            const COLORREF cue_rgb = warm
                                         ? (dark_mode_ ? RGB(178, 166, 128) : RGB(126, 98, 46))
                                         : (dark_mode_ ? RGB(152, 160, 174) : RGB(112, 120, 134));
            SetPropW(popup_edit_hwnd_, L"Pulse.EditCueColor",
                     reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(0x01000000u | cue_rgb)));
        }
    } else if (!popup_edit_cue_.empty()) {
        SendMessageW(popup_edit_hwnd_, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(popup_edit_cue_.c_str()));
    }

    SetWindowSubclass(popup_edit_hwnd_, &DesktopOverlayController::PopupEditSubclassProc, 1,
                      reinterpret_cast<DWORD_PTR>(this));
    PlaceHostedPopupEdit();

    const int total_len = static_cast<int>(crlf_text.size());
    const int s0 = (select_start < 0) ? total_len : std::clamp(select_start, 0, total_len);
    const int s1 = (select_end < 0) ? total_len : std::clamp(select_end, 0, total_len);
    SendMessageW(popup_edit_hwnd_, EM_SETSEL, static_cast<WPARAM>(s0), static_cast<LPARAM>(s1));
    return true;
}

void DesktopOverlayController::DestroyHostedPopupEdit() noexcept {
    if (!popup_edit_hwnd_) return;
    HWND target = popup_edit_hwnd_;
    popup_edit_hwnd_ = nullptr;
    popup_edit_composing_ = false;
    popup_edit_ignore_killfocus_ = true;
    if (IsWindow(target)) {
        RemovePropW(target, L"Pulse.EditMultiline");
        RemovePropW(target, L"Pulse.EditWrapW");
        RemovePropW(target, L"Pulse.EditCue");
        RemovePropW(target, L"Pulse.EditCueColor");
        RemoveWindowSubclass(target, &DesktopOverlayController::PopupEditSubclassProc, 1);
        DestroyWindow(target);
    }
    popup_edit_ignore_killfocus_ = false;
}

void DesktopOverlayController::SyncHostedPopupEditChanged() {
    if (!popup_edit_hwnd_ || !IsWindow(popup_edit_hwnd_)) return;
    const std::wstring current = ReadHostedPopupEditText();

    if (active_kind_ == OverlayKind::StickyNotePopover) {
        if (current != sticky_note_text_) {
            sticky_note_text_ = current;
            if (callbacks_.on_sticky_note_changed &&
                sticky_tile_id_ != "@weather_city" && !sticky_tile_id_.starts_with("@title:")) {
                callbacks_.on_sticky_note_changed(sticky_tile_id_, sticky_note_text_);
            }
        }
    } else if (active_kind_ == OverlayKind::SearchPopup) {
        if (current != search_query_) {
            const bool empty_flipped = (search_query_.empty() != current.empty());
            search_query_ = current;
            search_selected_idx_ = 0;
            if (callbacks_.on_search_query_changed) {
                callbacks_.on_search_query_changed(search_query_);
            }
            if (empty_flipped) {
                const bool prev_ignore = popup_edit_ignore_killfocus_;
                popup_edit_ignore_killfocus_ = true;
                RepositionAndShow(card_screen_x_, card_screen_y_, 540, search_query_.empty() ? 188 : 424, false);
                popup_edit_ignore_killfocus_ = prev_ignore;
            } else {
                Render();
            }
        }
    }
}

void DesktopOverlayController::ShowPopupEditContextMenu(HWND edit_hwnd, POINT screen_pt) {
    if (!edit_hwnd || !IsWindow(edit_hwnd)) return;

    DWORD sel_start = 0, sel_end = 0;
    SendMessageW(edit_hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_start), reinterpret_cast<LPARAM>(&sel_end));
    const DWORD lo = std::min(sel_start, sel_end);
    const DWORD hi = std::max(sel_start, sel_end);
    const int text_len = GetWindowTextLengthW(edit_hwnd);
    const bool has_sel = (lo != hi);
    const bool can_undo = SendMessageW(edit_hwnd, EM_CANUNDO, 0, 0) != 0;
    const bool can_paste = IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
    const bool can_select_all = text_len > 0 && !(lo == 0 && hi == static_cast<DWORD>(text_len));

    enum EditMenuCmd : int {
        kEditUndo = 1,
        kEditCut = 2,
        kEditCopy = 3,
        kEditPaste = 4,
        kEditDelete = 5,
        kEditSelectAll = 6,
    };
    const auto mk = [](int cmd, const wchar_t* label, const wchar_t* glyph,
                       const wchar_t* shortcut, bool enabled) {
        ui::FluentMenuItem it;
        it.command = cmd;
        it.text = label;
        it.glyph = glyph;
        it.shortcut = shortcut;
        it.enabled = enabled;
        return it;
    };

    std::vector<ui::FluentMenuItem> items;
    items.reserve(6);
    items.push_back(mk(kEditUndo, L"撤销", L"\xE7A7", L"Ctrl+Z", can_undo));
    items.back().separator_after = true;
    items.push_back(mk(kEditCut, L"剪切", L"\xE8C6", L"Ctrl+X", has_sel));
    items.push_back(mk(kEditCopy, L"复制", L"\xE8C8", L"Ctrl+C", has_sel));
    items.push_back(mk(kEditPaste, L"粘贴", L"\xE77F", L"Ctrl+V", can_paste));
    items.push_back(mk(kEditDelete, L"删除", L"\xE74D", L"Del", has_sel));
    items.back().separator_after = true;
    items.push_back(mk(kEditSelectAll, L"全选", L"\xE8B3", L"Ctrl+A", can_select_all));

    const auto appearance = render::ReadSystemAppearance();
    const auto material = beautify_settings_.default_material == model::CardMaterial::FollowHost
                              ? host_material_
                              : beautify_settings_.default_material;
    const bool liquid_glass =
        appearance.transparency && material == model::CardMaterial::LiquidGlass && backdrop_ != nullptr;
    const bool native_acrylic =
        appearance.transparency && material != model::CardMaterial::Solid && !liquid_glass;
    const uint64_t key = BuildSharedMenuMaterialKey(
        native_acrylic, dark_mode_, liquid_glass, beautify_settings_, scale_);
    if (shared_menu_ && (shared_menu_material_key_ != key || shared_menu_source_ != backdrop_ ||
                         shared_menu_source_dc_ != backdrop_owner_dc_)) {
        shared_menu_.reset();
    }
    if (!EnsureSharedMenu()) return;
    shared_menu_material_key_ = key;
    shared_menu_source_ = backdrop_;
    shared_menu_source_dc_ = backdrop_owner_dc_;
    shared_menu_->SetTheme(dark_mode_, MenuColor(accent_argb_));
    shared_menu_->SetGlassSurface(liquid_glass || native_acrylic);
    shared_menu_->SetNativeAcrylic(false);
    shared_menu_->SetMaterialPainter({});
    if (liquid_glass) {
        if (callbacks_.on_prepare_menu_underlay) callbacks_.on_prepare_menu_underlay(screen_pt);
        AssignSharedMenuGlassPainter(shared_menu_.get(), backdrop_, beautify_settings_);
    } else if (native_acrylic) {
        const bool dark = dark_mode_;
        const uint8_t opacity = beautify_settings_.card_opacity_percent;
        const auto* source = backdrop_;
        shared_menu_->SetNativeAcrylic(true, [dark, opacity, source](HWND hwnd) -> std::unique_ptr<ui::NativeMenuBackdrop> {
            auto layer = std::make_unique<MenuHostBackdrop>(dark, opacity, source);
            return layer->Initialize(hwnd) ? std::move(layer) : nullptr;
        });
    }
    shared_menu_->SetTopmost(true);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    const bool prev_ignore = popup_edit_ignore_killfocus_;
    const bool prev_tracking = shared_menu_tracking_;
    popup_edit_ignore_killfocus_ = true;
    shared_menu_tracking_ = true;
    const int cmd = shared_menu_->TrackPopup(screen_pt, std::move(items));
    shared_menu_tracking_ = prev_tracking;
    popup_edit_ignore_killfocus_ = prev_ignore;
    shared_menu_->SetTopmost(false);

    if (!popup_edit_hwnd_ || popup_edit_hwnd_ != edit_hwnd || !IsWindow(edit_hwnd)) return;
    if (hwnd_ && IsWindowVisible(hwnd_)) {
        SetForegroundWindow(hwnd_);
        SetFocus(edit_hwnd);
        SendMessageW(edit_hwnd, EM_SETSEL, sel_start, sel_end);
    }
    switch (cmd) {
    case kEditUndo:      SendMessageW(edit_hwnd, WM_UNDO, 0, 0); break;
    case kEditCut:       SendMessageW(edit_hwnd, WM_CUT, 0, 0); break;
    case kEditCopy:      SendMessageW(edit_hwnd, WM_COPY, 0, 0); break;
    case kEditPaste:     SendMessageW(edit_hwnd, WM_PASTE, 0, 0); break;
    case kEditDelete:    SendMessageW(edit_hwnd, WM_CLEAR, 0, 0); break;
    case kEditSelectAll: SendMessageW(edit_hwnd, EM_SETSEL, 0, -1); break;
    default: break;
    }
    SyncHostedPopupEditChanged();
    Render();
}

LRESULT CALLBACK DesktopOverlayController::PopupEditSubclassProc(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
    UINT_PTR /*subclass_id*/, DWORD_PTR ref_data) noexcept {
    auto* self = reinterpret_cast<DesktopOverlayController*>(ref_data);
    if (!self) return DefSubclassProc(hwnd, msg, wparam, lparam);

    if (msg == WM_IME_STARTCOMPOSITION) self->popup_edit_composing_ = true;
    if (msg == WM_IME_ENDCOMPOSITION) self->popup_edit_composing_ = false;

    if (msg == WM_GETDLGCODE) {
        return DefSubclassProc(hwnd, msg, wparam, lparam) | DLGC_WANTALLKEYS;
    }

    if (msg == WM_CONTEXTMENU) {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (pt.x == -1 && pt.y == -1) {
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            pt.x = rc.left + static_cast<LONG>(std::lround(8.0f * self->scale_));
            pt.y = rc.bottom + static_cast<LONG>(std::lround(4.0f * self->scale_));
        }
        self->ShowPopupEditContextMenu(hwnd, pt);
        return 0;
    }

    if (msg == WM_MOUSEWHEEL && !self->popup_edit_multiline_ && self->hwnd_) {
        return SendMessageW(self->hwnd_, msg, wparam, lparam);
    }

    if (!self->popup_edit_composing_ && wparam != VK_PROCESSKEY) {
        if (msg == WM_KEYDOWN) {
            if (wparam == VK_ESCAPE) {
                if (self->active_kind_ == OverlayKind::AiOrganizeDrawer ||
                    self->active_kind_ == OverlayKind::InlineRename) {
                    PostMessageW(self->hwnd_, WM_APP + 72, 0, 0);
                    return 0;
                }
                if (self->active_kind_ == OverlayKind::StickyNotePopover) {
                    const std::string tid = self->sticky_tile_id_;
                    const std::wstring txt = self->ReadHostedPopupEditText();
                    self->sticky_note_text_ = txt;
                    PostMessageW(self->hwnd_, WM_APP + 73, 0, 0);
                    if (self->callbacks_.on_sticky_note_changed) {
                        self->callbacks_.on_sticky_note_changed(tid, txt);
                    }
                    return 0;
                }
                if (self->active_kind_ == OverlayKind::SearchPopup) {
                    PostMessageW(self->hwnd_, WM_APP + 73, 0, 0);
                    return 0;
                }
            }
            if (wparam == VK_F2 && self->active_kind_ == OverlayKind::InlineRename) {
                const std::wstring txt = self->ReadHostedPopupEditText();
                const int len = static_cast<int>(txt.size());
                const int stem = (self->rename_stem_len_ > 0 && self->rename_stem_len_ < len)
                                     ? self->rename_stem_len_
                                     : len;
                DWORD a = 0, b = 0;
                SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&a), reinterpret_cast<LPARAM>(&b));
                int from = 0, to = stem;
                if (stem >= len) to = len;
                else if (a == 0 && static_cast<int>(b) == stem) to = len;
                else if (a == 0 && static_cast<int>(b) == len) { from = stem + 1; to = len; }
                SendMessageW(hwnd, EM_SETSEL, static_cast<WPARAM>(from), static_cast<LPARAM>(to));
                if (self->hwnd_) InvalidateRect(self->hwnd_, nullptr, FALSE);
                return 0;
            }
            if ((wparam == VK_UP || wparam == VK_DOWN) && self->active_kind_ == OverlayKind::SearchPopup) {
                self->HandleKeyDown(wparam);
                return 0;
            }
            if (wparam == VK_RETURN) {
                if (self->active_kind_ == OverlayKind::SearchPopup) {
                    self->HandleKeyDown(VK_RETURN);
                    return 0;
                }
                if (self->active_kind_ == OverlayKind::InlineRename ||
                    (self->active_kind_ == OverlayKind::AiOrganizeDrawer && !self->popup_edit_multiline_)) {
                    PostMessageW(self->hwnd_, WM_APP + 72, 1, 0);
                    return 0;
                }
                if (self->active_kind_ == OverlayKind::StickyNotePopover) {
                    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                    if (!self->popup_edit_multiline_ || ctrl) {
                        const std::string tid = self->sticky_tile_id_;
                        const std::wstring txt = self->ReadHostedPopupEditText();
                        self->sticky_note_text_ = txt;
                        if (self->callbacks_.on_sticky_note_changed) {
                            self->callbacks_.on_sticky_note_changed(tid, txt);
                        }
                        PostMessageW(self->hwnd_, WM_APP + 73, 0, 0);
                        return 0;
                    }
                }
            }
        } else if (msg == WM_CHAR) {
            if (!self->popup_edit_multiline_ &&
                (wparam == VK_RETURN || wparam == VK_ESCAPE || wparam == VK_TAB)) {
                return 0;
            }
            if (self->popup_edit_multiline_) {
                if (wparam == VK_ESCAPE) return 0;
                if ((wparam == L'\n' || wparam == L'\r') && (GetKeyState(VK_CONTROL) & 0x8000) != 0) return 0;
                if (wparam == 1) {
                    SendMessageW(hwnd, EM_SETSEL, 0, -1);
                    if (self->hwnd_) InvalidateRect(self->hwnd_, nullptr, FALSE);
                    return 0;
                }
            }
        }
    }

    if (msg == WM_KILLFOCUS && !self->popup_edit_ignore_killfocus_ &&
        self->active_kind_ == OverlayKind::AiOrganizeDrawer &&
        reinterpret_cast<HWND>(wparam) == self->hwnd_) {
        PostMessageW(self->hwnd_, WM_APP + 72, 1, 0);
    }

    LRESULT result = 0;
    if (self->shared_menu_compositor_) {
        IDWriteTextFormat* fmt = self->HostedPopupEditFormat();
        const D2D1_COLOR_F fg = self->HostedPopupEditForeground();
        const D2D1_COLOR_F bg = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
        if (!ui::HandleChildEditMessage(*self->shared_menu_compositor_, fmt, fg, bg,
                                        self->popup_edit_brush_, hwnd, msg, wparam, lparam, result)) {
            result = ui::DefPresentedChildEditProc(*self->shared_menu_compositor_, fmt, fg, bg,
                                                   hwnd, msg, wparam, lparam);
        }
    } else {
        result = DefSubclassProc(hwnd, msg, wparam, lparam);
    }

    // Keep TSF caret synced even when caret sits at end-of-line (where EM_POSFROMCHAR returns -1).
    if (!self->popup_edit_multiline_ && GetFocus() == hwnd &&
        (msg == WM_SETFOCUS || msg == WM_KEYDOWN || msg == WM_CHAR || msg == WM_SETTEXT ||
         msg == EM_SETSEL || msg == WM_PASTE || msg == WM_CUT || msg == WM_UNDO ||
         msg == WM_IME_COMPOSITION || msg == WM_IME_ENDCOMPOSITION)) {
        DWORD s0 = 0, s1 = 0;
        SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&s0), reinterpret_cast<LPARAM>(&s1));
        if (SendMessageW(hwnd, EM_POSFROMCHAR, s1, 0) == -1 && self->shared_menu_compositor_) {
            const std::wstring txt = self->ReadHostedPopupEditText();
            const size_t idx = std::min<size_t>(s1, txt.size());
            float px_w = 0.0f;
            self->shared_menu_compositor_->MeasureLumaText(
                std::wstring_view(txt.data(), idx), self->HostedPopupEditFormat(), px_w);
            RECT crc{};
            GetClientRect(hwnd, &crc);
            const int cx = std::clamp(static_cast<int>(std::lround(2.0f + px_w)), 2, std::max(2, static_cast<int>(crc.right) - 2));
            SetCaretPos(cx, 2);
            NotifyWinEvent(EVENT_OBJECT_LOCATIONCHANGE, hwnd, OBJID_CARET, CHILDID_SELF);
        }
    }

    if (!self->popup_edit_composing_ &&
        (msg == WM_CHAR || msg == WM_KEYDOWN || msg == WM_PASTE || msg == WM_CUT ||
         msg == WM_CLEAR || msg == WM_UNDO || msg == EM_UNDO || msg == WM_SETTEXT ||
         msg == EM_REPLACESEL || msg == WM_IME_ENDCOMPOSITION)) {
        self->SyncHostedPopupEditChanged();
    }

    if (self->hwnd_ && (msg == WM_SETFOCUS || msg == WM_KILLFOCUS ||
                        msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_LBUTTONUP)) {
        InvalidateRect(self->hwnd_, nullptr, FALSE);
    }

    // Win32 ES_MULTILINE EDIT overwrites IMM32 COMPOSITIONFORM with CFS_RECT (4,0)
    // during WM_SETFOCUS / WM_IME_SETCONTEXT / WM_IME_STARTCOMPOSITION / WM_SIZE.
    // Re-run Render() synchronously so LumaTextRenderer restores exact CFS_POINT
    // and CFS_EXCLUDE coordinates at the painted DirectWrite caret.
    if (GetFocus() == hwnd && self->target_bitmap_ &&
        (msg == WM_SETFOCUS || msg == WM_SIZE || msg == WM_IME_SETCONTEXT ||
         msg == WM_IME_STARTCOMPOSITION || msg == WM_IME_COMPOSITION)) {
        thread_local bool s_in_ime_sync = false;
        if (!s_in_ime_sync) {
            s_in_ime_sync = true;
            self->Render();
            NotifyWinEvent(EVENT_OBJECT_LOCATIONCHANGE, hwnd, OBJID_CARET, CHILDID_SELF);
            s_in_ime_sync = false;
        }
    }
    return result;
}

void DesktopOverlayController::PulseButton(ID2D1DeviceContext* dc, float ox, float oy, const D2D1_RECT_F& r,
                                           const std::wstring& text, const std::string& action, bool primary, bool enabled) {
    const D2D1_RECT_F b = D2D1::RectF(ox + r.left, oy + r.top, ox + r.right, oy + r.bottom);
    const bool hot = enabled && hovered_action_id_ == action;
    if (enabled)
        click_regions_.push_back({action, RECT{static_cast<LONG>(r.left), static_cast<LONG>(r.top), static_cast<LONG>(r.right),
                                               static_cast<LONG>(r.bottom)}});
    // The widgets' and the dock's glass popups use capsule buttons (macOS); other panels keep Pulse's own.
    const bool capsule = active_kind_ == OverlayKind::WidgetDetail || active_kind_ == OverlayKind::WidgetGalleryDrawer ||
                         active_kind_ == OverlayKind::DockSystemPanel;
    if (!capsule && dc == d2d_dc_.Get() && EnsureUiCompositor()) {
        pulse_buttons_.push_back({b, text, primary, hot, enabled});
        return;
    }
    const float dim = enabled ? 1.0f : 0.45f;
    // Snapshot look-alike of the fluent button (4 DIP corners), or a capsule on the glass popups.
    const float rad = capsule ? (b.bottom - b.top) * 0.5f : 4.0f;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    auto fill = [&](const D2D1_RECT_F& rc, D2D1_COLOR_F c) {
        brush.Reset();
        if (SUCCEEDED(dc->CreateSolidColorBrush(c, &brush))) dc->FillRoundedRectangle(D2D1::RoundedRect(rc, rad, rad), brush.Get());
    };
    auto stroke = [&](const D2D1_RECT_F& rc, D2D1_COLOR_F c) {
        brush.Reset();
        if (SUCCEEDED(dc->CreateSolidColorBrush(c, &brush)))
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rc.left + 0.5f, rc.top + 0.5f, rc.right - 0.5f, rc.bottom - 0.5f), rad, rad), brush.Get(), 1.0f);
    };
    const D2D1_COLOR_F accent = MenuColor(accent_argb_);
    if (primary) {
        D2D1_COLOR_F c = accent;
        if (hot) { c.r = c.r * 0.9f + 0.1f; c.g = c.g * 0.9f + 0.1f; c.b = c.b * 0.9f + 0.1f; }
        c.a *= dim;
        fill(b, c);
        stroke(b, D2D1::ColorF(1, 1, 1, 0.08f));
    } else if (capsule) {
        fill(b, dark_mode_ ? D2D1::ColorF(1, 1, 1, (hot ? 0.24f : 0.16f) * dim) : D2D1::ColorF(0, 0, 0, (hot ? 0.11f : 0.07f) * dim));
    } else {
        fill(b, dark_mode_ ? D2D1::ColorF(1, 1, 1, hot ? 0.083f : 0.061f) : D2D1::ColorF(1, 1, 1, hot ? 0.5f : 0.7f));
        stroke(b, dark_mode_ ? D2D1::ColorF(1, 1, 1, 0.09f) : D2D1::ColorF(0, 0, 0, 0.073f));
    }
    const float lum = 0.2126f * accent.r + 0.7152f * accent.g + 0.0722f * accent.b;
    const D2D1_COLOR_F ink = primary ? (lum > 0.55f ? D2D1::ColorF(0, 0, 0, 0.9f * dim) : D2D1::ColorF(1, 1, 1, dim))
                                     : (dark_mode_ ? D2D1::ColorF(1, 1, 1, dim) : D2D1::ColorF(0, 0, 0, 0.9f * dim));
    brush.Reset();
    if (SUCCEEDED(dc->CreateSolidColorBrush(ink, &brush)))
        dc->DrawText(text.c_str(), static_cast<UINT32>(text.size()), fmt_small_center_.Get(), b, brush.Get());
}

void DesktopOverlayController::PaintPulseControls() {
    const bool has_segments = (active_kind_ == OverlayKind::WidgetGalleryDrawer && !gallery_segments_.empty());
    const bool has_buttons = !pulse_buttons_.empty();
    const bool has_edit = (popup_edit_hwnd_ != nullptr && IsWindow(popup_edit_hwnd_) &&
                           (GetWindowLongPtrW(popup_edit_hwnd_, GWL_STYLE) & WS_VISIBLE) != 0);
    if ((!has_segments && !has_edit && !has_buttons) || !EnsureUiCompositor() || !shared_menu_compositor_ || !target_bitmap_)
        return;
    ID2D1DeviceContext* cdc = shared_menu_compositor_->Dc();
    if (!cdc) return;
    // Same thread D2D device as the overlay, so it can draw into the overlay's back buffer, in pixels.
    Microsoft::WRL::ComPtr<ID2D1Image> old_target;
    cdc->GetTarget(&old_target);
    float old_dpi_x = 96.0f, old_dpi_y = 96.0f;
    cdc->GetDpi(&old_dpi_x, &old_dpi_y);
    D2D1_MATRIX_3X2_F old_transform{};
    cdc->GetTransform(&old_transform);
    cdc->SetTarget(target_bitmap_.Get());
    cdc->SetDpi(96.0f, 96.0f);
    cdc->SetTransform(D2D1::Matrix3x2F::Identity());
    cdc->BeginDraw();

    if (has_segments) {
        ui_painter_.SetCompositor(shared_menu_compositor_.get());
        ui_painter_.SetScale(scale_);
        const auto px = [&](const D2D1_RECT_F& r) {
            return D2D1::RectF(std::round(r.left * scale_), std::round(r.top * scale_), std::round(r.right * scale_),
                               std::round(r.bottom * scale_));
        };
        const ui::Theme theme = ui::MakeTheme(dark_mode_, MenuColor(accent_argb_));
        if (ui_painter_.BeginFrame(theme)) {
            ui_painter_.DrawSegmentedTrack(px(gallery_segment_track_));
            const size_t count = gallery_segments_.size();
            for (size_t i = 0; i < count; ++i) {
                const auto& seg = gallery_segments_[i];
                ui::fluent::SegmentedItemSpec spec;
                spec.bounds = px(seg.rect);
                spec.text = seg.text;
                spec.state.selected = seg.selected;
                spec.state.hovered = seg.hovered;
                spec.shared_track = true;
                spec.position = count == 1 ? ui::fluent::SegmentPosition::Single
                              : i == 0 ? ui::fluent::SegmentPosition::First
                              : i + 1 == count ? ui::fluent::SegmentPosition::Last
                                               : ui::fluent::SegmentPosition::Middle;
                ui_painter_.DrawSegmentedItem(spec);
            }
        }
    }

    if (has_buttons) {
        ui_painter_.SetCompositor(shared_menu_compositor_.get());
        ui_painter_.SetScale(scale_);
        const ui::Theme theme = ui::MakeTheme(dark_mode_, MenuColor(accent_argb_));
        if (ui_painter_.BeginFrame(theme)) {
            for (const auto& b : pulse_buttons_) {
                ui::fluent::ButtonSpec spec;
                spec.bounds = D2D1::RectF(std::round(b.rect.left * scale_), std::round(b.rect.top * scale_),
                                          std::round(b.rect.right * scale_), std::round(b.rect.bottom * scale_));
                spec.text = b.text;
                spec.kind = b.primary ? ui::fluent::ButtonKind::Primary : ui::fluent::ButtonKind::Standard;
                spec.state.hovered = b.hovered;
                spec.state.enabled = b.enabled;
                ui_painter_.DrawButton(spec);
            }
        }
    }

    if (has_edit) {
        RECT rect{};
        GetClientRect(popup_edit_hwnd_, &rect);
        MapWindowPoints(popup_edit_hwnd_, hwnd_, reinterpret_cast<POINT*>(&rect), 2);
        const D2D1_RECT_F bounds = D2D1::RectF(
            static_cast<float>(rect.left), static_cast<float>(rect.top),
            static_cast<float>(rect.right), static_cast<float>(rect.bottom));
        ui::DrawPulsePaintedChildEdit(
            *shared_menu_compositor_, HostedPopupEditFormat(), HostedPopupEditForeground(),
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), popup_edit_hwnd_, bounds);
    }

    cdc->EndDraw();
    cdc->SetTarget(old_target.Get());
    cdc->SetDpi(old_dpi_x, old_dpi_y);
    cdc->SetTransform(old_transform);
}

bool DesktopOverlayController::EnsureSharedMenu() {
    if (shared_menu_) return true;
    if (!EnsureWindowCreated()) return false;
    // Owned popups inherit their owner's topmost band, even without TOPMOST themselves.
    SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!EnsureUiCompositor()) return false;
    auto menu = std::make_unique<ui::FluentMenu>();
    if (!menu->Create(hwnd_, shared_menu_compositor_.get(), scale_)) return false;

    menu->SetTopmost(false);
    menu->SetActivateOnOpen(true);
    // macOS menu geometry: 14 DIP corners, a concentric highlight inset 6 DIP, compact rows.
    menu->SetCornerRadiusDip(14.0f);
    menu->SetHoverPlate(8.0f, 6.0f);
    menu->SetRowHeightDip(28.0f);
    shared_menu_ = std::move(menu);
    return true;
}

std::vector<ui::FluentMenuItem> DesktopOverlayController::BuildSharedMenuItems() {
    // Stable command identities also survive an async patch rejected while a
    // flyout is visible. Never reinterpret an already displayed command index.
    auto command = [this](const MenuRow& row) {
        for (size_t i = 0; i < shared_menu_commands_.size(); ++i) {
            if (shared_menu_commands_[i].command_id == row.command_id) return static_cast<int>(i + 1);
        }
        shared_menu_commands_.push_back(row);
        return static_cast<int>(shared_menu_commands_.size());
    };
    // Pulse's menu shows state by kind: an on/off row is a switch, a one-of-several row a filled dot. A bare
    // `checked` draws nothing, so each stateful desktop row is mapped to its kind here.
    const auto is_toggle = [](const std::string& id) {
        return id == "toggle-tile-live" || id == "photo-shuffle" || id == "tile-show-title" ||
               id == "tile-stack-rotate" || id == "toggle-focus" || id == "dock-mag-toggle";
    };
    const auto is_choice = [](const std::string& id) {
        for (const char* prefix : {"photo-interval:", "weather-city:", "tile-stack-sel:", "move-to-group:", "tile-mat:",
                                   "tile-size:", "note-color:", "dock-size:", "dock-scale:", "dock-hide:", "dock-minfx:", "dock-pos:",
                                   "dock-focus-win:"})
            if (id.starts_with(prefix)) return true;
        return id == "move-to-loose" || id == "tile-bg:material" || id == "sub:tile-color" || id == "sub:tile-image";
    };
    std::function<ui::FluentMenuItem(const MenuRow&)> convert = [&](const MenuRow& row) {
        ui::FluentMenuItem item;
        item.text = row.label;
        item.glyph = row.icon_glyph ? std::wstring(1, row.icon_glyph) : std::wstring{};
        item.enabled = row.enabled;
        item.checked = row.checked;
        if (is_toggle(row.command_id)) {
            item.toggle = true;
        } else if (is_choice(row.command_id)) {
            item.radio = row.checked;
            item.radio_group = true;
            item.checked = false;
        }
        item.separator_after = row.separator_after;
        item.accent_glyph = row.is_accent_primary;
        if (row.is_shell_verb) item.badge_text = row.right_badge;
        else item.shortcut = row.right_badge;
        if (row.is_icon_bar) {
            static constexpr const char* commands[] = {
                "cut-item", "copy-item", "rename-item", "delete-item", "share-item"};
            static constexpr wchar_t glyphs[] = {L'\xE8C6', L'\xE8C8', L'\xE8AC', L'\xE74D', L'\xE724'};
            for (int i = 0; i < 5; ++i) {
                MenuRow action;
                action.command_id = commands[i];
                ui::FluentMenuSwatch swatch;
                swatch.command = command(action);
                swatch.glyph.assign(1, glyphs[i]);
                item.quick_swatches.push_back(std::move(swatch));
            }
        } else if (row.is_tag_row) {
            item.text.clear();
            const size_t n_swatches = std::min<size_t>(7, menu_spec_.tag_palette.size());
            for (size_t ti = 0; ti < n_swatches; ++ti) {
                const auto& tag = menu_spec_.tag_palette[ti];
                MenuRow action;
                action.command_id = tag.id == menu_spec_.current_tag_id ? "tag:clear" : "tag:" + ipc::WideToUtf8(tag.id);
                ui::FluentMenuSwatch swatch;
                swatch.command = command(action);
                swatch.color = MenuColor(tag.argb);
                swatch.checked = tag.id == menu_spec_.current_tag_id;
                item.quick_swatches.push_back(std::move(swatch));
            }
        } else if (!row.color_swatches.empty()) {
            item.text.clear();
            for (const auto& c : row.color_swatches) {
                MenuRow action;
                action.command_id = c.command_id;
                ui::FluentMenuSwatch swatch;
                swatch.command = command(action);
                swatch.color = MenuColor(c.argb);
                swatch.checked = c.checked;
                item.quick_swatches.push_back(std::move(swatch));
            }
        } else if (!row.submenu_rows.empty()) {
            for (const auto& child : row.submenu_rows) item.children.push_back(convert(child));
        } else {
            item.command = command(row);
        }
        return item;
    };
    std::vector<ui::FluentMenuItem> items;
    for (const auto& row : menu_rows_) items.push_back(convert(row));
    return items;
}

void DesktopOverlayController::TrackSharedMenu() {
    if (shared_menu_tracking_) return;
    const auto appearance = render::ReadSystemAppearance();
    const auto material = beautify_settings_.default_material == model::CardMaterial::FollowHost
        ? host_material_ : beautify_settings_.default_material;
    const bool liquid_glass =
        appearance.transparency && material == model::CardMaterial::LiquidGlass && backdrop_ != nullptr;
    // Acrylic, Mica, and MicaAlt transient menus use real Windows 11 HostBackdrop Acrylic;
    // Solid and the system transparency preference remain explicit opt-outs.
    const bool native_acrylic =
        appearance.transparency && material != model::CardMaterial::Solid && !liquid_glass;
    // Both Liquid Glass menus and Win11 Acrylic menus now clamp backdrop luminance into the active
    // theme's readable band (matching WacOS and Fluent 2), so menu theme follows dark_mode_ stably.
    const bool menu_dark = dark_mode_;
    const uint64_t key = BuildSharedMenuMaterialKey(
        native_acrylic, menu_dark, liquid_glass, beautify_settings_, scale_);
    if (shared_menu_ && (shared_menu_material_key_ != key || shared_menu_source_ != backdrop_ ||
                        shared_menu_source_dc_ != backdrop_owner_dc_)) shared_menu_.reset();
    if (!EnsureSharedMenu()) { Dismiss(); return; }
    shared_menu_material_key_ = key;
    shared_menu_source_ = backdrop_;
    shared_menu_source_dc_ = backdrop_owner_dc_;
    shared_menu_->SetTheme(menu_dark, MenuColor(accent_argb_));
    shared_menu_->SetGlassSurface(liquid_glass || native_acrylic);
    shared_menu_->SetNativeAcrylic(false);
    shared_menu_->SetMaterialPainter({});
    const POINT anchor_px{ToPx(menu_spec_.screen_anchor.x), ToPx(menu_spec_.screen_anchor.y)};
    if (liquid_glass) {
        if (callbacks_.on_prepare_menu_underlay) callbacks_.on_prepare_menu_underlay(anchor_px);
        AssignSharedMenuGlassPainter(shared_menu_.get(), backdrop_, beautify_settings_);
    } else if (native_acrylic) {
        const auto opacity = beautify_settings_.card_opacity_percent;
        const bool dark = menu_dark;
        const auto* source = backdrop_;
        shared_menu_->SetNativeAcrylic(true, [dark, opacity, source](HWND hwnd) -> std::unique_ptr<ui::NativeMenuBackdrop> {
            auto layer = std::make_unique<MenuHostBackdrop>(dark, opacity, source);
            return layer->Initialize(hwnd) ? std::move(layer) : nullptr;
        });
    }
    shared_menu_commands_.clear();
    const auto generation = menu_generation_;
    shared_menu_tracking_ = true;
    const int selected = shared_menu_->TrackPopup(anchor_px, BuildSharedMenuItems());
    shared_menu_tracking_ = false;
    // Another overlay may have replaced this menu from inside the modal loop.
    const bool current = generation == menu_generation_ && active_kind_ == OverlayKind::ContextMenu;
    MenuRow action;
    if (current && selected > 0 && static_cast<size_t>(selected) <= shared_menu_commands_.size())
        action = shared_menu_commands_[static_cast<size_t>(selected - 1)];
    // Keep one hidden menu/device pair warm. The material key above invalidates
    // theme/opacity/DPI changes; TrimIdleResources and Shutdown release it.
    if (backdrop_) backdrop_->ReleaseMenuMaterialCache();
    if (shared_menu_compositor_) shared_menu_compositor_->Resize(1, 1);
    if (!current) return;
    const auto spec = menu_spec_;
    const auto session = shell_session_id_;
    const bool shell_command = action.is_shell_verb && action.shell_item_id != 0;
    if (shell_command && session == 0) {
        // Shown from the cache and clicked before the live reply: run it when the reply lands.
        pending_shell_label_ = action.label;
        pending_shell_depth_ = action.shell_depth;
        Dismiss();
        return;
    }
    if (shell_command) shell_session_id_ = 0;
    Dismiss();
    if (shell_command && callbacks_.on_invoke_shell_verb)
        callbacks_.on_invoke_shell_verb(session, action.shell_item_id, action.shell_verb);
    else if (!action.command_id.empty() && callbacks_.on_menu_command)
        callbacks_.on_menu_command(action.command_id, spec);
}

bool DesktopOverlayController::SaveContextMenuSnapshot(const wchar_t* path, const ContextMenuSpec& spec,
    const model::BeautifySettings& settings, bool dark) {
    if (shared_menu_tracking_) return false;
    Dismiss();
    menu_spec_ = spec;
    BuildMenuRows(settings);
    shared_menu_commands_.clear();
    if (!EnsureSharedMenu()) return false;
    shared_menu_->SetTheme(dark, MenuColor(accent_argb_));
    const auto material = settings.default_material == model::CardMaterial::FollowHost
        ? host_material_ : settings.default_material;
    const bool use_glass = (material == model::CardMaterial::LiquidGlass) && backdrop_ != nullptr;
    shared_menu_->SetGlassSurface(use_glass || material != model::CardMaterial::Solid);
    if (use_glass) {
        const POINT anchor_px{ToPx(spec.screen_anchor.x), ToPx(spec.screen_anchor.y)};
        if (callbacks_.on_prepare_menu_underlay) callbacks_.on_prepare_menu_underlay(anchor_px);
        AssignSharedMenuGlassPainter(shared_menu_.get(), backdrop_, settings);
    }
    const bool saved = shared_menu_->SaveDebugSnapshot(path, BuildSharedMenuItems(), 0);
    shared_menu_.reset();
    if (backdrop_) backdrop_->ReleaseMenuMaterialCache();
    if (shared_menu_compositor_) shared_menu_compositor_->Resize(1, 1);
    return saved;
}
} // namespace pulse::desktop::overlays

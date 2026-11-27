// dock_system_panel.cpp - Frosted glass Dock .dk-sys panels & floating Volume Wheel OSD
// following docs/design/desktop_widgets_redesign.html (#v-dock .dk-panel & .osd).
#include "host/overlays/desktop_overlays.h"

#include "host/overlays/lunar_calendar.h"

#include <commoncontrols.h>
#include <shellapi.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>

namespace pulse::desktop::overlays {
namespace {
using Microsoft::WRL::ComPtr;

constexpr UINT_PTR kTimerDockOsd = 8804;

std::wstring ToLowerW(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

std::wstring FirstVisiblePanelChar(const std::wstring& s) {
    for (wchar_t ch : s) {
        if (ch <= 0x0020 || ch == 0x00A0 || ch == 0xFEFF || ch == 0x3000) continue;
        if ((ch >= 0x200B && ch <= 0x200F) ||
            (ch >= 0x202A && ch <= 0x202E) ||
            (ch >= 0x2060 && ch <= 0x206F)) continue;
        return std::wstring(1, ch);
    }
    return L"•";
}

bool OverlayFileExists(const std::wstring& path) noexcept {
    if (path.empty()) return false;
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

HICON ExtractTrayAppHicon(const std::wstring& exe_path, HWND hwnd) noexcept {
    if (!exe_path.empty() && OverlayFileExists(exe_path)) {
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(exe_path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX) != 0) {
            ComPtr<IImageList> il;
            if (SUCCEEDED(SHGetImageList(SHIL_EXTRALARGE, IID_PPV_ARGS(&il))) && il) {
                HICON hi = nullptr;
                if (SUCCEEDED(il->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &hi)) && hi) {
                    return hi;
                }
            }
        }
        SHFILEINFOW sfi2{};
        if (SHGetFileInfoW(exe_path.c_str(), 0, &sfi2, sizeof(sfi2), SHGFI_ICON | SHGFI_LARGEICON) != 0 && sfi2.hIcon) {
            return sfi2.hIcon;
        }
        HICON hi_large = nullptr;
        if (ExtractIconExW(exe_path.c_str(), 0, &hi_large, nullptr, 1) > 0 && hi_large) {
            return hi_large;
        }
    }
    if (hwnd && IsWindow(hwnd)) {
        DWORD_PTR res = 0;
        HICON raw = nullptr;
        if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 40, &res) && res) {
            raw = reinterpret_cast<HICON>(res);
        }
        if (!raw) raw = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON));
        if (!raw && SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 40, &res) && res) {
            raw = reinterpret_cast<HICON>(res);
        }
        if (!raw) raw = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
        if (raw) return CopyIcon(raw);
    }
    return nullptr;
}

D2D1_COLOR_F ArgbColor(uint32_t argb, float mul_a = 1.0f) noexcept {
    const float a = static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;
    const float r = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
    const float g = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
    const float b = static_cast<float>(argb & 0xFFu) / 255.0f;
    return D2D1::ColorF(r, g, b, std::clamp(a * mul_a, 0.0f, 1.0f));
}

void DrawSmallEthernetGlyph(ID2D1Factory* fac, ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br,
                            ID2D1StrokeStyle* round, float cx, float cy, bool connected,
                            uint32_t pri, uint32_t dim) noexcept {
    br->SetColor(ArgbColor(connected ? pri : dim));
    const float ml = cx - 8.0f, mt = cy - 5.4f, mr = cx + 2.2f, mb = cy + 3.0f;
    const float pl = cx + 3.0f, pr = cx + 7.6f, pt = cy - 5.6f, pb = cy - 0.5f;
    const float pcx = (pl + pr) * 0.5f;
    if (fac) {
        ComPtr<ID2D1PathGeometry> geo;
        ComPtr<ID2D1GeometrySink> sk;
        if (SUCCEEDED(fac->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sk))) {
            sk->BeginFigure(D2D1::Point2F(mr - 0.5f, mt), D2D1_FIGURE_BEGIN_HOLLOW);
            sk->AddLine(D2D1::Point2F(ml + 1.4f, mt));
            sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(ml, mt + 1.4f), D2D1::SizeF(1.4f, 1.4f), 0.0f,
                                        D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sk->AddLine(D2D1::Point2F(ml, mb - 1.4f));
            sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(ml + 1.4f, mb), D2D1::SizeF(1.4f, 1.4f), 0.0f,
                                        D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sk->AddLine(D2D1::Point2F(mr - 0.5f, mb));
            sk->EndFigure(D2D1_FIGURE_END_OPEN);
            sk->Close();
            dc->DrawGeometry(geo.Get(), br, 1.5f, round);
        }
    }
    const float mcx = (ml + mr) * 0.5f - 0.2f;
    dc->DrawLine(D2D1::Point2F(mcx, mb + 0.2f), D2D1::Point2F(mcx, cy + 5.6f), br, 1.5f, round);
    dc->DrawLine(D2D1::Point2F(mcx - 3.2f, cy + 5.6f), D2D1::Point2F(mcx + 3.2f, cy + 5.6f), br, 1.5f, round);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(pl, pt, pr, pb), 1.3f, 1.3f), br, 1.35f);
    dc->DrawLine(D2D1::Point2F(pl + 1.0f, pt + 2.0f), D2D1::Point2F(pr - 1.0f, pt + 2.0f), br, 1.15f, round);
    dc->DrawLine(D2D1::Point2F(pcx, pb + 0.2f), D2D1::Point2F(pcx, cy + 5.6f), br, 1.5f, round);
}

int DaysInMonth(int year, int month) noexcept {
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    return (month == 2 && leap) ? 29 : kDays[std::clamp(month, 1, 12) - 1];
}

int DayOfWeek(int y, int m, int d) noexcept { // 0 = Sunday
    static constexpr int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

void DrawSmallWifiBars(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br,
                       float x, float cy, int bars, uint32_t pri, uint32_t dim) noexcept {
    for (int i = 0; i < 3; ++i) {
        const float bh = 4.0f + static_cast<float>(i) * 3.0f;
        const float bx = x + static_cast<float>(i) * 4.5f;
        br->SetColor(ArgbColor(i < bars ? pri : dim));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, cy + 5.5f - bh, bx + 2.8f, cy + 5.5f), 1.2f, 1.2f), br);
    }
}

void DrawSmallSpeaker(ID2D1Factory* fac, ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br,
                      ID2D1StrokeStyle* round, float cx, float cy, int vol_pct, bool muted,
                      uint32_t pri, uint32_t dim) noexcept {
    const float sx = cx - 2.0f;
    if (fac) {
        ComPtr<ID2D1PathGeometry> geo;
        ComPtr<ID2D1GeometrySink> sk;
        if (SUCCEEDED(fac->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sk))) {
            sk->BeginFigure(D2D1::Point2F(sx - 5.0f, cy - 2.5f), D2D1_FIGURE_BEGIN_FILLED);
            sk->AddLine(D2D1::Point2F(sx - 2.2f, cy - 2.5f));
            sk->AddLine(D2D1::Point2F(sx + 1.5f, cy - 5.5f));
            sk->AddLine(D2D1::Point2F(sx + 1.5f, cy + 5.5f));
            sk->AddLine(D2D1::Point2F(sx - 2.2f, cy + 2.5f));
            sk->AddLine(D2D1::Point2F(sx - 5.0f, cy + 2.5f));
            sk->EndFigure(D2D1_FIGURE_END_CLOSED);
            sk->Close();
            br->SetColor(ArgbColor(muted ? dim : pri));
            dc->FillGeometry(geo.Get(), br);
        }
    }
    if (muted || vol_pct <= 0) {
        br->SetColor(ArgbColor(0xFFFF453Au));
        dc->DrawLine(D2D1::Point2F(sx + 4.0f, cy - 3.0f), D2D1::Point2F(sx + 9.0f, cy + 3.0f), br, 1.6f, round);
        dc->DrawLine(D2D1::Point2F(sx + 9.0f, cy - 3.0f), D2D1::Point2F(sx + 4.0f, cy + 3.0f), br, 1.6f, round);
    } else if (fac) {
        const int waves = (vol_pct > 66) ? 2 : 1;
        constexpr float kWR[2] = {3.6f, 6.6f};
        for (int i = 0; i < 2; ++i) {
            const float r = kWR[i];
            const float x0 = sx + 2.0f + r * 0.62f;
            ComPtr<ID2D1PathGeometry> geo;
            ComPtr<ID2D1GeometrySink> sk;
            if (SUCCEEDED(fac->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sk))) {
                sk->BeginFigure(D2D1::Point2F(x0, cy - r * 0.72f), D2D1_FIGURE_BEGIN_HOLLOW);
                sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(x0, cy + r * 0.72f), D2D1::SizeF(r, r), 0.0f,
                                            D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sk->EndFigure(D2D1_FIGURE_END_OPEN);
                sk->Close();
                br->SetColor(ArgbColor(i < waves ? pri : dim));
                dc->DrawGeometry(geo.Get(), br, 1.5f, round);
            }
        }
    }
}

void DrawSmallCheck(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, ID2D1StrokeStyle* round,
                    float cx, float cy, uint32_t col) noexcept {
    br->SetColor(ArgbColor(col));
    dc->DrawLine(D2D1::Point2F(cx - 4.2f, cy + 0.2f), D2D1::Point2F(cx - 1.2f, cy + 3.4f), br, 1.8f, round);
    dc->DrawLine(D2D1::Point2F(cx - 1.2f, cy + 3.4f), D2D1::Point2F(cx + 4.6f, cy - 3.2f), br, 1.8f, round);
}

void ComputeDockPanelDimensions(DockPanelKind kind, const live::DockSystemSnapshot& snap,
                                int& out_w, int& out_h) noexcept {
    out_w = 312;
    switch (kind) {
    case DockPanelKind::TrayOverflow: {
        const int count = static_cast<int>(std::min<size_t>(snap.tray_apps.size(), 12));
        const int rows = std::max(1, (count + 3) / 4);
        out_h = 52 + rows * 70 + 46;
        break;
    }
    case DockPanelKind::TrayAppMenu:
        out_w = 236;
        out_h = 166;
        break;
    case DockPanelKind::Ime: {
        const int n = std::max(1, static_cast<int>(std::min<size_t>(snap.ime_layouts.size(), 6)));
        out_h = 54 + n * 44 + 46;
        break;
    }
    case DockPanelKind::Wifi: {
        if (snap.ethernet_connected && !snap.wifi_available) {
            out_h = 204;
        } else if (snap.ethernet_connected && snap.wifi_available) {
            const int n = std::max(1, static_cast<int>(std::min<size_t>(snap.wifi_networks.size(), 5)));
            out_h = 132 + n * 40 + 46;
        } else {
            const int n = std::max(1, static_cast<int>(std::min<size_t>(snap.wifi_networks.size(), 6)));
            out_h = 56 + n * 40 + 46;
        }
        break;
    }
    case DockPanelKind::Volume: {
        const int n = std::max(1, static_cast<int>(std::min<size_t>(snap.audio_devices.size(), 5)));
        out_h = 128 + n * 38 + 46;
        break;
    }
    case DockPanelKind::Bluetooth: {
        const int n = std::max(1, static_cast<int>(std::min<size_t>(snap.bt_devices.size(), 6)));
        out_h = 56 + n * 42 + 46;
        break;
    }
    case DockPanelKind::Battery:
        out_h = 224;
        break;
    case DockPanelKind::Clock:
        out_w = 324;
        out_h = 376;
        break;
    default:
        out_w = 280;
        out_h = 180;
        break;
    }
}

void AnchorDockPopup(const RECT& work_area, const RECT& anchor, uint8_t dock_pos,
                     int w, int h, int& out_x, int& out_y) noexcept {
    const int wa_l = static_cast<int>(work_area.left) + 12;
    const int wa_t = static_cast<int>(work_area.top) + 12;
    const int wa_r = static_cast<int>(work_area.right) - 12;
    const int wa_b = static_cast<int>(work_area.bottom) - 12;
    const int acx = (static_cast<int>(anchor.left) + static_cast<int>(anchor.right)) / 2;
    const int acy = (static_cast<int>(anchor.top) + static_cast<int>(anchor.bottom)) / 2;
    if (dock_pos == 1) {
        out_x = static_cast<int>(anchor.right) + 14;
        out_y = acy - h / 2;
    } else if (dock_pos == 2) {
        out_x = static_cast<int>(anchor.left) - w - 14;
        out_y = acy - h / 2;
    } else {
        out_x = acx - w / 2;
        out_y = static_cast<int>(anchor.top) - h - 14;
    }
    out_x = std::clamp(out_x, wa_l, std::max(wa_l, wa_r - w));
    out_y = std::clamp(out_y, wa_t, std::max(wa_t, wa_b - h));
}

} // namespace

ID2D1Bitmap* DesktopOverlayController::GetOrLoadDockTrayIcon(ID2D1DeviceContext* dc,
                                                             const std::wstring& exe_path,
                                                             HWND hwnd) noexcept {
    if (!dc || (exe_path.empty() && !hwnd)) return nullptr;
    if (dock_tray_icon_dc_ != dc) {
        dock_tray_icon_cache_.clear();
        dock_tray_icon_dc_ = dc;
    }
    const std::wstring key = !exe_path.empty()
        ? (L"exe:" + ToLowerW(exe_path))
        : (L"hwnd:" + std::to_wstring(reinterpret_cast<uintptr_t>(hwnd)));
    if (auto it = dock_tray_icon_cache_.find(key); it != dock_tray_icon_cache_.end()) {
        return it->second.Get();
    }
    if (!dock_tray_wic_) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dock_tray_wic_));
    }
    if (!dock_tray_wic_) return nullptr;

    HICON hi = ExtractTrayAppHicon(exe_path, hwnd);
    if (!hi) return nullptr;

    ComPtr<ID2D1Bitmap> bmp;
    ComPtr<IWICBitmap> wic_bmp;
    if (SUCCEEDED(dock_tray_wic_->CreateBitmapFromHICON(hi, &wic_bmp)) && wic_bmp) {
        ComPtr<IWICFormatConverter> conv;
        if (SUCCEEDED(dock_tray_wic_->CreateFormatConverter(&conv)) && conv &&
            SUCCEEDED(conv->Initialize(wic_bmp.Get(), GUID_WICPixelFormat32bppPBGRA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
            dc->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp);
        }
    }
    DestroyIcon(hi);
    ID2D1Bitmap* raw = bmp.Get();
    if (bmp) {
        dock_tray_icon_cache_[key] = std::move(bmp);
    }
    return raw;
}

bool DesktopOverlayController::OpenDockSystemPanel(const RECT& work_area,
                                                   const RECT& anchor_screen_rc,
                                                   uint8_t dock_position,
                                                   DockPanelKind kind,
                                                   const live::DockSystemSnapshot& snap,
                                                   int tray_app_index,
                                                   uint8_t week_start,
                                                   uint8_t battery_low_pct,
                                                   const std::vector<model::TodoItem>& todos) {
    if (kind == DockPanelKind::None) {
        Dismiss();
        return false;
    }
    Dismiss();
    work_area_ = work_area;
    active_kind_ = OverlayKind::DockSystemPanel;
    dock_panel_kind_ = kind;
    dock_panel_snap_ = snap;
    dock_panel_tray_index_ = tray_app_index;
    dock_panel_week_start_ = week_start ? 1u : 0u;
    dock_panel_battery_low_pct_ = battery_low_pct;
    dock_panel_todos_ = todos;
    dock_panel_month_offset_ = 0;
    dock_vol_dragging_ = false;
    dock_vol_track_ = {};

    int w = 312, h = 220;
    ComputeDockPanelDimensions(kind, snap, w, h);
    int x = 0, y = 0;
    AnchorDockPopup(work_area, anchor_screen_rc, dock_position, w, h, x, y);
    const model::CardMaterial eff_mat = (beautify_settings_.default_material == model::CardMaterial::FollowHost)
                                            ? host_material_
                                            : beautify_settings_.default_material;
    if (eff_mat == model::CardMaterial::LiquidGlass && callbacks_.on_prepare_menu_underlay) {
        const POINT center_px{
            static_cast<LONG>(std::lround((static_cast<float>(x) + static_cast<float>(w) * 0.5f) * scale_)),
            static_cast<LONG>(std::lround((static_cast<float>(y) + static_cast<float>(h) * 0.5f) * scale_)),
        };
        callbacks_.on_prepare_menu_underlay(center_px);
    }
    RepositionAndShow(x, y, w, h, true);
    return true;
}

void DesktopOverlayController::UpdateDockSystemPanel(const live::DockSystemSnapshot& snap) {
    if (active_kind_ != OverlayKind::DockSystemPanel) return;
    dock_panel_snap_ = snap;
    Render();
}

bool DesktopOverlayController::ShowDockVolumeOsd(const RECT& work_area,
                                                 const RECT& anchor_screen_rc,
                                                 uint8_t dock_position,
                                                 int volume_pct,
                                                 bool muted,
                                                 uint32_t duration_ms) {
    const bool already_osd = (active_kind_ == OverlayKind::DockVolumeOsd && hwnd_ != nullptr);
    if (!already_osd) {
        Dismiss();
        active_kind_ = OverlayKind::DockVolumeOsd;
    }
    work_area_ = work_area;
    dock_osd_volume_pct_ = std::clamp(volume_pct, 0, 100);
    dock_osd_muted_ = muted || (dock_osd_volume_pct_ <= 0);
    const int w = 204, h = 40;
    int x = 0, y = 0;
    AnchorDockPopup(work_area, anchor_screen_rc, dock_position, w, h, x, y);
    if (already_osd) {
        Render();
    } else {
        const model::CardMaterial eff_mat = (beautify_settings_.default_material == model::CardMaterial::FollowHost)
                                                ? host_material_
                                                : beautify_settings_.default_material;
        if (eff_mat == model::CardMaterial::LiquidGlass && callbacks_.on_prepare_menu_underlay) {
            const POINT center_px{
                static_cast<LONG>(std::lround((static_cast<float>(x) + static_cast<float>(w) * 0.5f) * scale_)),
                static_cast<LONG>(std::lround((static_cast<float>(y) + static_cast<float>(h) * 0.5f) * scale_)),
            };
            callbacks_.on_prepare_menu_underlay(center_px);
        }
        RepositionAndShow(x, y, w, h, /*take_focus=*/false);
    }
    if (hwnd_) {
        KillTimer(hwnd_, kTimerDockOsd);
        SetTimer(hwnd_, kTimerDockOsd, std::clamp(duration_ms, 400u, 5000u), nullptr);
    }
    return true;
}

bool DesktopOverlayController::ApplyDockVolumeTrackAtX(int card_x, bool dragging) noexcept {
    if (dock_vol_track_.right <= dock_vol_track_.left) return false;
    const float t = std::clamp(
        static_cast<float>(card_x - dock_vol_track_.left) /
            static_cast<float>(std::max<LONG>(1, dock_vol_track_.right - dock_vol_track_.left)),
        0.0f, 1.0f);
    const int pct = std::clamp(static_cast<int>(std::lround(t * 100.0f)), 0, 100);
    dock_panel_snap_.volume_pct = pct;
    dock_panel_snap_.muted = (pct == 0);
    if (callbacks_.on_dock_volume_changed) {
        callbacks_.on_dock_volume_changed(pct, dragging);
    }
    Render();
    return true;
}

bool DesktopOverlayController::HandleDockPanelAction(const std::string& act) {
    if (!act.starts_with("dk:")) return false;
    const std::string cmd = act.substr(3);
    if (cmd == "cal-prev" || cmd == "cal-next" || cmd == "cal-today") {
        dock_panel_month_offset_ = (cmd == "cal-today") ? 0 : (dock_panel_month_offset_ + (cmd == "cal-next" ? 1 : -1));
        Render();
        return true;
    }
    if (cmd == "vol-mute") {
        dock_panel_snap_.muted = !dock_panel_snap_.muted;
        if (callbacks_.on_dock_panel_action) callbacks_.on_dock_panel_action(cmd);
        Render();
        return true;
    }
    // Pin toggle, power mode switch, audio device switch, IME toggle keep panel responsive or dismiss appropriately
    const bool dismiss_after = cmd.starts_with("uri:") || cmd.starts_with("tray-open:") ||
                               cmd == "notif-center" || cmd.starts_with("tray-unpin:") ||
                               cmd.starts_with("ime-hkl:");
    if (cmd.starts_with("tray-pin:")) {
        const size_t idx = static_cast<size_t>(std::atoi(cmd.c_str() + 9));
        if (idx < dock_panel_snap_.tray_apps.size()) {
            dock_panel_snap_.tray_apps[idx].pinned = !dock_panel_snap_.tray_apps[idx].pinned;
        }
    } else if (cmd.starts_with("pwr:")) {
        dock_panel_snap_.power_mode = static_cast<uint8_t>(std::clamp(std::atoi(cmd.c_str() + 4), 0, 2));
        dock_panel_snap_.power_plan_name = (dock_panel_snap_.power_mode == 0) ? L"节能"
                                         : (dock_panel_snap_.power_mode == 2) ? L"性能" : L"平衡";
    } else if (cmd.starts_with("audio-dev:")) {
        const size_t idx = static_cast<size_t>(std::atoi(cmd.c_str() + 10));
        for (size_t i = 0; i < dock_panel_snap_.audio_devices.size(); ++i) {
            dock_panel_snap_.audio_devices[i].is_default = (i == idx);
        }
    } else if (cmd == "ime-toggle") {
        dock_panel_snap_.ime_chinese = !dock_panel_snap_.ime_chinese;
        dock_panel_snap_.ime_badge = dock_panel_snap_.ime_chinese ? L"中" : L"英";
    }
    if (dismiss_after) Dismiss();
    if (callbacks_.on_dock_panel_action) callbacks_.on_dock_panel_action(cmd);
    if (!dismiss_after && active_kind_ == OverlayKind::DockSystemPanel) Render();
    return true;
}

void DesktopOverlayController::DrawDockVolumeOsd(ID2D1DeviceContext* dc, float ox, float oy) {
    const float w = static_cast<float>(card_w_);
    const float h = static_cast<float>(card_h_);
    const bool light_ink = dock_panel_light_ink_;
    const uint32_t pri = light_ink ? 0xF5FFFFFFu : 0xF01D1D1Fu;
    const uint32_t dim = light_ink ? 0x4CFFFFFFu : 0x38000000u;
    const uint32_t track = light_ink ? 0x2AFFFFFFu : 0x1E000000u;

    ComPtr<ID2D1Factory> fac;
    dc->GetFactory(&fac);
    ComPtr<ID2D1StrokeStyle> round;
    if (fac) {
        fac->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
            nullptr, 0, &round);
    }

    const float cy = oy + h * 0.5f;
    DrawSmallSpeaker(fac.Get(), dc, Brush(dc, pri), round.Get(), ox + 22.0f, cy,
                     dock_osd_volume_pct_, dock_osd_muted_, pri, dim);

    const float bx0 = ox + 40.0f;
    const float bx1 = ox + w - 48.0f;
    const float bh = 6.0f;
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, cy - bh * 0.5f, bx1, cy + bh * 0.5f), 3.0f, 3.0f),
                             Brush(dc, track));
    const float frac = dock_osd_muted_ ? 0.0f : std::clamp(static_cast<float>(dock_osd_volume_pct_) / 100.0f, 0.0f, 1.0f);
    if (frac > 0.0f) {
        const float fw = std::max(bh, (bx1 - bx0) * frac);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, cy - bh * 0.5f, bx0 + fw, cy + bh * 0.5f), 3.0f, 3.0f),
                                 Brush(dc, light_ink ? 0xF5FFFFFFu : accent_argb_));
    }

    const std::wstring val = dock_osd_muted_ ? L"静音" : (std::to_wstring(dock_osd_volume_pct_) + L"%");
    detail_text_.DrawText(dwrite_.Get(), dc, val.c_str(), static_cast<UINT32>(val.size()),
                          fmt_small_right_.Get(),
                          D2D1::RectF(ox + w - 46.0f, oy + 10.0f, ox + w - 12.0f, oy + h - 8.0f),
                          Brush(dc, dock_osd_muted_ ? 0xFFFF453Au : pri));
}

void DesktopOverlayController::DrawDockSystemPanel(ID2D1DeviceContext* dc, float ox, float oy) {
    const float w = static_cast<float>(card_w_);
    const float h = static_cast<float>(card_h_);
    const bool light_ink = dock_panel_light_ink_;
    const uint32_t pri = light_ink ? 0xF8FFFFFFu : 0xF21D1D1Fu;
    const uint32_t sec = light_ink ? 0xB4FFFFFFu : 0x9E26303Eu;
    const uint32_t dim = light_ink ? 0x52FFFFFFu : 0x42000000u;
    const uint32_t line = light_ink ? 0x26FFFFFFu : 0x1A000000u;
    const uint32_t hover_bg = light_ink ? 0x24FFFFFFu : 0x14000000u;
    const uint32_t chip_bg = light_ink ? 0x1AFFFFFFu : 0x10000000u;

    ComPtr<ID2D1Factory> fac;
    dc->GetFactory(&fac);
    ComPtr<ID2D1StrokeStyle> round;
    if (fac) {
        fac->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
            nullptr, 0, &round);
    }

    auto text = [&](const std::wstring& s, IDWriteTextFormat* f, float l, float t, float r, float b, uint32_t col) {
        if (!s.empty() && f && r > l) {
            detail_text_.DrawText(dwrite_.Get(), dc, s.c_str(), static_cast<UINT32>(s.size()), f,
                                  D2D1::RectF(ox + l, oy + t, ox + r, oy + b), Brush(dc, col));
        }
    };
    auto centered = [&](const std::wstring& s, IDWriteTextFormat* f, float l, float t, float r, float b, uint32_t col) {
        if (!s.empty() && f && r > l) {
            const auto prev = f->GetTextAlignment();
            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            text(s, f, l, t, r, b, col);
            f->SetTextAlignment(prev);
        }
    };
    auto right_aligned = [&](const std::wstring& s, IDWriteTextFormat* f, float l, float t, float r, float b, uint32_t col) {
        if (!s.empty() && f && r > l) {
            const auto prev = f->GetTextAlignment();
            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            text(s, f, l, t, r, b, col);
            f->SetTextAlignment(prev);
        }
    };
    auto hline = [&](float y) {
        dc->FillRectangle(D2D1::RectF(ox + 14.0f, oy + y, ox + w - 14.0f, oy + y + 1.0f), Brush(dc, line));
    };
    auto footer_link = [&](const std::wstring& label, const std::string& act) {
        const float fy = h - 42.0f;
        hline(fy - 4.0f);
        const D2D1_RECT_F rc = D2D1::RectF(ox + 8.0f, oy + fy, ox + w - 8.0f, oy + h - 8.0f);
        if (hovered_action_id_ == act) {
            dc->FillRoundedRectangle(D2D1::RoundedRect(rc, 9.0f, 9.0f), Brush(dc, hover_bg));
        }
        text(label, fmt_small_.Get(), 16.0f, fy + 8.0f, w - 34.0f, h - 10.0f, sec);
        // Subtle right chevron ›
        const float cx = ox + w - 20.0f, cy = oy + fy + 16.0f;
        dc->DrawLine(D2D1::Point2F(cx - 2.0f, cy - 4.0f), D2D1::Point2F(cx + 2.2f, cy), Brush(dc, sec), 1.4f, round.Get());
        dc->DrawLine(D2D1::Point2F(cx + 2.2f, cy), D2D1::Point2F(cx - 2.0f, cy + 4.0f), Brush(dc, sec), 1.4f, round.Get());
        click_regions_.push_back({act, RECT{8, static_cast<LONG>(fy), static_cast<LONG>(w - 8.0f), static_cast<LONG>(h - 8.0f)}});
    };

    const auto& snap = dock_panel_snap_;

    switch (dock_panel_kind_) {
    case DockPanelKind::TrayOverflow: {
        text(L"托盘图标", fmt_body_semi_.Get(), 16.0f, 14.0f, 140.0f, 36.0f, pri);
        text(L"点右上角圆点固定到程序坞", fmt_small_right_.Get(), 130.0f, 16.0f, w - 16.0f, 34.0f, sec);
        hline(42.0f);

        const size_t n = std::min<size_t>(snap.tray_apps.size(), 12);
        if (n == 0) {
            centered(L"当前没有常驻托盘应用", fmt_small_.Get(), 16.0f, 68.0f, w - 16.0f, 92.0f, sec);
        } else {
            const float gx = 12.0f;
            const float gy = 50.0f;
            const float cw = (w - 24.0f) / 4.0f;
            const float ch = 68.0f;
            for (size_t i = 0; i < n; ++i) {
                const auto& app = snap.tray_apps[i];
                const int col = static_cast<int>(i % 4);
                const int row = static_cast<int>(i / 4);
                const float cl = gx + cw * static_cast<float>(col) + 2.0f;
                const float ct = gy + ch * static_cast<float>(row) + 2.0f;
                const float cr = cl + cw - 4.0f;
                const float cb = ct + ch - 4.0f;

                const std::string open_act = "dk:tray-open:" + std::to_string(i);
                const std::string pin_act = "dk:tray-pin:" + std::to_string(i);
                const bool hot = (hovered_action_id_ == open_act || hovered_action_id_ == pin_act);
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ox + cl, oy + ct, ox + cr, oy + cb), 11.0f, 11.0f),
                                         Brush(dc, hot ? hover_bg : chip_bg));

                const float icx = (cl + cr) * 0.5f;
                const float icy = ct + 23.0f;
                const D2D1_RECT_F ir{ox + icx - 14.0f, oy + icy - 14.0f, ox + icx + 14.0f, oy + icy + 14.0f};
                if (ID2D1Bitmap* bmp = GetOrLoadDockTrayIcon(dc, app.exe_path, app.hwnd)) {
                    dc->DrawBitmap(bmp, &ir, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
                } else {
                    const D2D1_RECT_F sr{ox + icx - 13.0f, oy + icy - 13.0f, ox + icx + 13.0f, oy + icy + 13.0f};
                    const uint32_t bg = app.accent_argb ? app.accent_argb : 0xFF0A84FFu;
                    dc->FillRoundedRectangle(D2D1::RoundedRect(sr, 7.0f, 7.0f), Brush(dc, bg));
                    centered(FirstVisiblePanelChar(app.title), fmt_body_semi_.Get(),
                             icx - 13.0f, icy - 9.0f, icx + 13.0f, icy + 11.0f, 0xFFFFFFFFu);
                }

                if (app.badge_count > 0) {
                    const D2D1_RECT_F br_rc{ir.right - 6.0f, ir.top - 4.0f, ir.right + 6.0f, ir.top + 8.0f};
                    dc->FillRoundedRectangle(D2D1::RoundedRect(br_rc, 6.0f, 6.0f), Brush(dc, 0xFFFF3B30u));
                }

                centered(app.title, fmt_small_.Get(), cl + 2.0f, ct + 42.0f, cr - 2.0f, cb - 2.0f, pri);

                // Interactive pin dot at top-right of cell (accent blue when pinned)
                const float px = cr - 9.0f;
                const float py = ct + 9.0f;
                if (app.pinned) {
                    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ox + px, oy + py), 5.5f, 5.5f), Brush(dc, accent_argb_));
                    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ox + px, oy + py), 2.0f, 2.0f), Brush(dc, 0xFFFFFFFFu));
                } else {
                    dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(ox + px, oy + py), 4.8f, 4.8f), Brush(dc, dim), 1.3f);
                }

                // Register pin dot region BEFORE cell body so clicking pin takes priority
                click_regions_.push_back({pin_act, RECT{static_cast<LONG>(cr - 18.0f), static_cast<LONG>(ct),
                                                        static_cast<LONG>(cr), static_cast<LONG>(ct + 18.0f)}});
                click_regions_.push_back({open_act, RECT{static_cast<LONG>(cl), static_cast<LONG>(ct),
                                                         static_cast<LONG>(cr), static_cast<LONG>(cb)}});
            }
        }
        footer_link(L"任务栏和托盘设置…", "dk:uri:ms-settings:taskbar");
        break;
    }

    case DockPanelKind::TrayAppMenu: {
        const bool valid_idx = (dock_panel_tray_index_ >= 0 &&
                                static_cast<size_t>(dock_panel_tray_index_) < snap.tray_apps.size());
        const live::DockTrayApp* app_ptr = valid_idx ? &snap.tray_apps[static_cast<size_t>(dock_panel_tray_index_)] : nullptr;
        const std::wstring title = app_ptr ? app_ptr->title : L"托盘应用";
        const uint32_t accent_bg = (app_ptr && app_ptr->accent_argb) ? app_ptr->accent_argb : accent_argb_;
        const D2D1_RECT_F hdr_ir{ox + 14.0f, oy + 12.0f, ox + 36.0f, oy + 34.0f};
        if (ID2D1Bitmap* bmp = app_ptr ? GetOrLoadDockTrayIcon(dc, app_ptr->exe_path, app_ptr->hwnd) : nullptr) {
            dc->DrawBitmap(bmp, &hdr_ir, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
        } else {
            dc->FillRoundedRectangle(D2D1::RoundedRect(hdr_ir, 6.0f, 6.0f), Brush(dc, accent_bg));
            centered(FirstVisiblePanelChar(title), fmt_small_.Get(), 14.0f, 15.0f, 36.0f, 33.0f, 0xFFFFFFFFu);
        }
        text(title, fmt_body_semi_.Get(), 44.0f, 13.0f, w - 14.0f, 35.0f, pri);
        hline(44.0f);

        auto menu_item = [&](float y, const std::wstring& lbl, const std::string& act) {
            const D2D1_RECT_F r{ox + 8.0f, oy + y, ox + w - 8.0f, oy + y + 34.0f};
            if (hovered_action_id_ == act) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, 8.0f, 8.0f), Brush(dc, hover_bg));
            }
            text(lbl, fmt_body_.Get(), 16.0f, y + 7.0f, w - 16.0f, y + 29.0f, pri);
            click_regions_.push_back({act, RECT{8, static_cast<LONG>(y), static_cast<LONG>(w - 8.0f), static_cast<LONG>(y + 34.0f)}});
        };
        const std::string idx_s = std::to_string(std::max(0, dock_panel_tray_index_));
        menu_item(50.0f, L"打开或激活窗口", "dk:tray-open:" + idx_s);
        menu_item(86.0f, L"不在程序坞上显示", "dk:tray-unpin:" + idx_s);
        footer_link(L"任务栏和托盘设置…", "dk:uri:ms-settings:taskbar");
        break;
    }

    case DockPanelKind::Ime: {
        text(L"输入法", fmt_body_semi_.Get(), 16.0f, 14.0f, 140.0f, 36.0f, pri);
        const std::string tog = "dk:ime-toggle";
        const D2D1_RECT_F pill{ox + w - 104.0f, oy + 12.0f, ox + w - 14.0f, oy + 36.0f};
        dc->FillRoundedRectangle(D2D1::RoundedRect(pill, 12.0f, 12.0f),
                                 Brush(dc, hovered_action_id_ == tog ? accent_argb_ : chip_bg,
                                       hovered_action_id_ == tog ? 0.32f : 1.0f));
        centered(snap.ime_chinese ? L"当前：中" : L"当前：英", fmt_small_.Get(), w - 104.0f, 16.0f, w - 14.0f, 34.0f, pri);
        click_regions_.push_back({tog, RECT{static_cast<LONG>(w - 104.0f), 12, static_cast<LONG>(w - 14.0f), 36}});
        hline(44.0f);

        float y = 52.0f;
        const size_t n = std::min<size_t>(snap.ime_layouts.size(), 6);
        for (size_t i = 0; i < n; ++i) {
            const auto& ly = snap.ime_layouts[i];
            const std::string act = "dk:ime-hkl:" + std::to_string(ly.hkl_raw);
            const D2D1_RECT_F r{ox + 8.0f, oy + y, ox + w - 8.0f, oy + y + 40.0f};
            if (hovered_action_id_ == act || ly.active) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, 9.0f, 9.0f),
                                         Brush(dc, ly.active ? accent_argb_ : hover_bg, ly.active ? 0.22f : 1.0f));
            }
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ox + 16.0f, oy + y + 7.0f, ox + 42.0f, oy + y + 33.0f), 7.0f, 7.0f),
                                     Brush(dc, ly.active ? accent_argb_ : chip_bg));
            centered(ly.short_code, fmt_small_.Get(), 16.0f, y + 11.0f, 42.0f, y + 30.0f, ly.active ? 0xFFFFFFFFu : pri);
            text(ly.name, fmt_body_.Get(), 50.0f, y + 4.0f, w - 38.0f, y + 22.0f, pri);
            text(ly.sub, fmt_small_.Get(), 50.0f, y + 22.0f, w - 38.0f, y + 38.0f, sec);
            if (ly.active) DrawSmallCheck(dc, Brush(dc, accent_argb_), round.Get(), ox + w - 24.0f, oy + y + 20.0f, accent_argb_);
            click_regions_.push_back({act, RECT{8, static_cast<LONG>(y), static_cast<LONG>(w - 8.0f), static_cast<LONG>(y + 40.0f)}});
            y += 44.0f;
        }
        footer_link(L"Win+空格 切换输入法 · 语言设置…", "dk:uri:ms-settings:regionlanguage");
        break;
    }

    case DockPanelKind::Wifi: {
        const bool wired_only = snap.ethernet_connected && !snap.wifi_available;
        const wchar_t* hdr_title = wired_only ? L"以太网" : (snap.ethernet_connected ? L"网络" : L"Wi-Fi");
        text(hdr_title, fmt_body_semi_.Get(), 16.0f, 14.0f, 130.0f, 36.0f, pri);
        if (snap.wifi_connected) {
            const std::string dis = "dk:wifi-disconnect";
            const D2D1_RECT_F pill{ox + w - 76.0f, oy + 12.0f, ox + w - 14.0f, oy + 36.0f};
            dc->FillRoundedRectangle(D2D1::RoundedRect(pill, 12.0f, 12.0f),
                                     Brush(dc, hovered_action_id_ == dis ? hover_bg : chip_bg));
            centered(L"断开", fmt_small_.Get(), w - 76.0f, 16.0f, w - 14.0f, 34.0f, pri);
            click_regions_.push_back({dis, RECT{static_cast<LONG>(w - 76.0f), 12, static_cast<LONG>(w - 14.0f), 36}});
        } else if (snap.ethernet_connected) {
            text(L"已连接", fmt_small_right_.Get(), 130.0f, 16.0f, w - 16.0f, 34.0f, 0xFF30D158u);
        } else {
            text(snap.wifi_available ? (snap.wifi_enabled ? L"未连接" : L"已关闭") : L"未连接",
                 fmt_small_right_.Get(), 120.0f, 16.0f, w - 16.0f, 34.0f, sec);
        }
        hline(44.0f);

        float y = 52.0f;
        if (snap.ethernet_connected) {
            // Crisp wired Ethernet status card
            const D2D1_RECT_F card{ox + 12.0f, oy + y, ox + w - 12.0f, oy + y + 54.0f};
            dc->FillRoundedRectangle(D2D1::RoundedRect(card, 11.0f, 11.0f), Brush(dc, chip_bg));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(card, 11.0f, 11.0f), Brush(dc, line), 0.9f);
            DrawSmallEthernetGlyph(fac.Get(), dc, Brush(dc, pri), round.Get(),
                                   ox + 34.0f, oy + y + 27.0f, true, pri, dim);
            const std::wstring ename = snap.ethernet_name.empty() ? L"以太网" : snap.ethernet_name;
            text(ename, fmt_body_semi_.Get(), 56.0f, y + 8.0f, w - 82.0f, y + 28.0f, pri);
            const uint32_t badge_col = light_ink ? 0xFF7CE0FFu : 0xFF0067C0u;
            text(L"有线连接", fmt_small_right_.Get(), w - 88.0f, y + 10.0f, w - 20.0f, y + 26.0f, badge_col);
            std::wstring sub = snap.ethernet_desc;
            if (!wired_only && !snap.ethernet_speed.empty()) {
                sub = sub.empty() ? snap.ethernet_speed : (sub + L" · " + snap.ethernet_speed);
            }
            if (sub.empty()) sub = L"已接入局域网与互联网";
            text(sub, fmt_small_.Get(), 56.0f, y + 30.0f, w - 20.0f, y + 48.0f, sec);
            y += 62.0f;

            if (wired_only) {
                auto kv_row = [&](const wchar_t* k, const std::wstring& v) {
                    if (v.empty()) return;
                    text(k, fmt_small_.Get(), 16.0f, y + 2.0f, 110.0f, y + 20.0f, sec);
                    text(v, fmt_small_right_.Get(), 110.0f, y + 2.0f, w - 16.0f, y + 20.0f, pri);
                    y += 22.0f;
                };
                kv_row(L"链路速率", snap.ethernet_speed.empty() ? L"自动协商" : snap.ethernet_speed);
                kv_row(L"IPv4 地址", snap.ethernet_ipv4.empty() ? L"—" : snap.ethernet_ipv4);
            } else {
                hline(y + 2.0f);
                text(L"Wi-Fi 网络", fmt_small_.Get(), 16.0f, y + 8.0f, w - 16.0f, y + 24.0f, sec);
                y += 28.0f;
            }
        }

        if (!wired_only) {
            const size_t max_nets = snap.ethernet_connected ? 5u : 6u;
            const size_t n = std::min<size_t>(snap.wifi_networks.size(), max_nets);
            if (n == 0) {
                centered(snap.wifi_available ? L"未扫描到附近的 Wi-Fi 网络" : L"当前未连接到有线或无线网络",
                         fmt_small_.Get(), 16.0f, y + 6.0f, w - 16.0f, y + 32.0f, sec);
            } else {
                for (size_t i = 0; i < n; ++i) {
                    const auto& net = snap.wifi_networks[i];
                    const std::string act = "dk:wifi-conn:" + std::to_string(i);
                    const D2D1_RECT_F r{ox + 8.0f, oy + y, ox + w - 8.0f, oy + y + 36.0f};
                    if (hovered_action_id_ == act || net.connected) {
                        dc->FillRoundedRectangle(D2D1::RoundedRect(r, 9.0f, 9.0f),
                                                 Brush(dc, net.connected ? accent_argb_ : hover_bg, net.connected ? 0.22f : 1.0f));
                    }
                    DrawSmallWifiBars(dc, Brush(dc, pri), ox + 18.0f, oy + y + 18.0f, net.bars,
                                      net.connected ? accent_argb_ : pri, dim);
                    text(net.ssid, net.connected ? fmt_body_semi_.Get() : fmt_body_.Get(), 40.0f, y + 8.0f, w - 86.0f, y + 29.0f, pri);
                    const std::wstring right = net.connected ? L"已连接" : (net.secured ? L"加密" : L"开放");
                    text(right, fmt_small_right_.Get(), w - 90.0f, y + 10.0f, w - 16.0f, y + 28.0f,
                         net.connected ? accent_argb_ : sec);
                    click_regions_.push_back({act, RECT{8, static_cast<LONG>(y), static_cast<LONG>(w - 8.0f), static_cast<LONG>(y + 36.0f)}});
                    y += 40.0f;
                }
            }
        }
        footer_link(L"网络和 Internet 设置…", wired_only ? "dk:uri:ms-settings:network-ethernet" : "dk:uri:ms-settings:network-wifi");
        break;
    }

    case DockPanelKind::Volume: {
        text(L"声音", fmt_body_semi_.Get(), 16.0f, 14.0f, 120.0f, 36.0f, pri);
        const std::wstring pct_s = snap.muted ? L"静音" : (std::to_wstring(snap.volume_pct) + L"%");
        right_aligned(pct_s, fmt_body_semi_.Get(), w - 96.0f, 14.0f, w - 16.0f, 36.0f, snap.muted ? 0xFFFF453Au : pri);

        // Interactive draggable glass volume pill (desktop_widgets_redesign.html .vol-bar)
        const float bar_l = 16.0f;
        const float bar_t = 46.0f;
        const float bar_r = w - 16.0f;
        const float bar_b = 78.0f;
        const float bar_h = bar_b - bar_t;
        const D2D1_RECT_F pill{ox + bar_l, oy + bar_t, ox + bar_r, oy + bar_b};
        dc->FillRoundedRectangle(D2D1::RoundedRect(pill, bar_h * 0.5f, bar_h * 0.5f), Brush(dc, chip_bg));

        const float track_l = bar_l + 36.0f;
        const float track_r = bar_r - 6.0f;
        dock_vol_track_ = RECT{static_cast<LONG>(track_l), static_cast<LONG>(bar_t),
                               static_cast<LONG>(track_r), static_cast<LONG>(bar_b)};

        const float frac = snap.muted ? 0.0f : std::clamp(static_cast<float>(snap.volume_pct) / 100.0f, 0.0f, 1.0f);
        if (frac > 0.005f) {
            const float fill_r = bar_l + 32.0f + (bar_r - bar_l - 32.0f) * frac;
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ox + bar_l, oy + bar_t, ox + fill_r, oy + bar_b),
                                                       bar_h * 0.5f, bar_h * 0.5f),
                                     Brush(dc, light_ink ? 0xE6FFFFFFu : accent_argb_, light_ink ? 0.90f : 0.88f));
        }
        dc->DrawRoundedRectangle(D2D1::RoundedRect(pill, bar_h * 0.5f, bar_h * 0.5f), Brush(dc, line), 1.0f);

        // Mute button embedded at left of pill
        const uint32_t spk_ink = (!snap.muted && frac > 0.01f && light_ink) ? 0xFF1A1E28u
                               : (!snap.muted && frac > 0.01f && !light_ink) ? 0xFFFFFFFFu : pri;
        DrawSmallSpeaker(fac.Get(), dc, Brush(dc, spk_ink), round.Get(), ox + bar_l + 18.0f, oy + bar_t + bar_h * 0.5f,
                         snap.volume_pct, snap.muted, spk_ink, dim);
        click_regions_.push_back({"dk:vol-mute", RECT{static_cast<LONG>(bar_l), static_cast<LONG>(bar_t),
                                                      static_cast<LONG>(bar_l + 34.0f), static_cast<LONG>(bar_b)}});

        hline(92.0f);
        text(L"输出设备", fmt_small_.Get(), 16.0f, 100.0f, w - 16.0f, 118.0f, sec);

        float y = 122.0f;
        const size_t n = std::min<size_t>(snap.audio_devices.size(), 5);
        for (size_t i = 0; i < n; ++i) {
            const auto& dev = snap.audio_devices[i];
            const std::string act = "dk:audio-dev:" + std::to_string(i);
            const D2D1_RECT_F r{ox + 8.0f, oy + y, ox + w - 8.0f, oy + y + 34.0f};
            if (hovered_action_id_ == act || dev.is_default) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, 9.0f, 9.0f),
                                         Brush(dc, dev.is_default ? accent_argb_ : hover_bg, dev.is_default ? 0.16f : 1.0f));
            }
            text(dev.name, dev.is_default ? fmt_body_semi_.Get() : fmt_body_.Get(), 16.0f, y + 7.0f, w - 38.0f, y + 28.0f, pri);
            if (dev.is_default) DrawSmallCheck(dc, Brush(dc, accent_argb_), round.Get(), ox + w - 24.0f, oy + y + 17.0f, accent_argb_);
            click_regions_.push_back({act, RECT{8, static_cast<LONG>(y), static_cast<LONG>(w - 8.0f), static_cast<LONG>(y + 34.0f)}});
            y += 38.0f;
        }
        footer_link(L"音量合成器…", "dk:uri:ms-settings:apps-volume");
        break;
    }

    case DockPanelKind::Bluetooth: {
        text(L"蓝牙", fmt_body_semi_.Get(), 16.0f, 14.0f, 140.0f, 36.0f, pri);
        text(snap.bt_enabled ? L"已开启" : L"未开启", fmt_small_right_.Get(), 140.0f, 16.0f, w - 16.0f, 34.0f,
             snap.bt_enabled ? accent_argb_ : sec);
        hline(44.0f);

        float y = 52.0f;
        const size_t n = std::min<size_t>(snap.bt_devices.size(), 6);
        if (n == 0) {
            centered(L"暂无已配对的蓝牙设备", fmt_small_.Get(), 16.0f, y + 8.0f, w - 16.0f, y + 34.0f, sec);
        } else {
            for (size_t i = 0; i < n; ++i) {
                const auto& d = snap.bt_devices[i];
                const D2D1_RECT_F r{ox + 8.0f, oy + y, ox + w - 8.0f, oy + y + 38.0f};
                if (d.connected) {
                    dc->FillRoundedRectangle(D2D1::RoundedRect(r, 9.0f, 9.0f), Brush(dc, chip_bg));
                }
                const wchar_t* badge = (d.kind == 1) ? L"耳" : (d.kind == 2) ? L"鼠" : (d.kind == 3) ? L"键" : L"蓝";
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ox + 16.0f, oy + y + 7.0f, ox + 40.0f, oy + y + 31.0f), 7.0f, 7.0f),
                                         Brush(dc, d.connected ? accent_argb_ : chip_bg, d.connected ? 0.24f : 1.0f));
                centered(badge, fmt_small_.Get(), 16.0f, y + 10.0f, 40.0f, y + 29.0f, d.connected ? accent_argb_ : sec);
                text(d.name, fmt_body_.Get(), 48.0f, y + 4.0f, w - 68.0f, y + 22.0f, pri);
                text(d.connected ? L"已连接" : L"已配对", fmt_small_.Get(), 48.0f, y + 21.0f, w - 68.0f, y + 37.0f, sec);
                if (d.battery_pct >= 0) {
                    const uint32_t bc = (d.battery_pct <= static_cast<int>(dock_panel_battery_low_pct_)) ? 0xFFFF453Au : pri;
                    text(std::to_wstring(d.battery_pct) + L"%", fmt_small_right_.Get(), w - 64.0f, y + 11.0f, w - 16.0f, y + 29.0f, bc);
                }
                y += 42.0f;
            }
        }
        footer_link(L"添加蓝牙或其他设备…", "dk:uri:ms-settings:bluetooth");
        break;
    }

    case DockPanelKind::Battery: {
        const uint32_t bat_col = snap.battery_charging ? 0xFF30D158u
                               : (snap.battery_pct <= static_cast<int>(dock_panel_battery_low_pct_) ? 0xFFFF453Au : pri);
        text(L"电池", fmt_body_semi_.Get(), 16.0f, 14.0f, 120.0f, 36.0f, pri);
        right_aligned(std::to_wstring(snap.battery_pct) + L"%", fmt_title_.Get(), w - 108.0f, 10.0f, w - 16.0f, 40.0f, bat_col);
        std::wstring sub = snap.battery_charging ? L"正在充电 · 已接通电源" : L"使用电池供电";
        if (!snap.battery_charging && snap.battery_remaining_min > 0) {
            wchar_t rb[48]{};
            std::swprintf(rb, ARRAYSIZE(rb), L"预计剩余 %d 小时 %d 分钟",
                          snap.battery_remaining_min / 60, snap.battery_remaining_min % 60);
            sub = rb;
        }
        text(sub, fmt_small_.Get(), 16.0f, 42.0f, w - 16.0f, 60.0f, sec);

        // Battery level bar
        const D2D1_RECT_F trk{ox + 16.0f, oy + 68.0f, ox + w - 16.0f, oy + 76.0f};
        dc->FillRoundedRectangle(D2D1::RoundedRect(trk, 4.0f, 4.0f), Brush(dc, chip_bg));
        const float frac = std::clamp(static_cast<float>(snap.battery_pct) / 100.0f, 0.0f, 1.0f);
        if (frac > 0.0f) {
            dc->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(trk.left, trk.top, trk.left + (trk.right - trk.left) * frac, trk.bottom), 4.0f, 4.0f),
                Brush(dc, bat_col));
        }

        hline(92.0f);
        text(L"电源模式", fmt_small_.Get(), 16.0f, 102.0f, w - 16.0f, 120.0f, sec);
        static constexpr const wchar_t* kModes[3] = {L"节能", L"平衡", L"性能"};
        const float sw = (w - 32.0f - 12.0f) / 3.0f;
        for (int m = 0; m < 3; ++m) {
            const float mx = 16.0f + static_cast<float>(m) * (sw + 6.0f);
            const std::string act = "dk:pwr:" + std::to_string(m);
            const bool sel = (snap.power_mode == static_cast<uint8_t>(m));
            const bool hot = (hovered_action_id_ == act);
            const D2D1_RECT_F btn{ox + mx, oy + 126.0f, ox + mx + sw, oy + 160.0f};
            dc->FillRoundedRectangle(D2D1::RoundedRect(btn, 10.0f, 10.0f),
                                     Brush(dc, sel ? accent_argb_ : (hot ? hover_bg : chip_bg), sel ? 0.90f : 1.0f));
            centered(kModes[m], fmt_body_semi_.Get(), mx, 133.0f, mx + sw, 155.0f, sel ? 0xFFFFFFFFu : pri);
            click_regions_.push_back({act, RECT{static_cast<LONG>(mx), 126, static_cast<LONG>(mx + sw), 160}});
        }
        footer_link(L"电源和电池设置…", "dk:uri:ms-settings:powersleep");
        break;
    }

    case DockPanelKind::Clock: {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        int year = now.wYear;
        int month = now.wMonth + dock_panel_month_offset_;
        while (month < 1) { month += 12; --year; }
        while (month > 12) { month -= 12; ++year; }
        const bool this_month = (dock_panel_month_offset_ == 0);
        const uint32_t red = 0xFFFF3B30u;

        wchar_t hdr[32]{};
        std::swprintf(hdr, ARRAYSIZE(hdr), L"%d年%d月", year, month);
        text(hdr, fmt_title_.Get(), 16.0f, 12.0f, 160.0f, 38.0f, pri);
        click_regions_.push_back({"dk:cal-today", RECT{12, 10, 150, 38}});

        const std::wstring lunar = LunarFullText(now.wYear, now.wMonth, now.wDay);
        if (!lunar.empty()) {
            text(lunar, fmt_small_.Get(), 16.0f, 38.0f, 220.0f, 54.0f, sec);
        }

        for (const bool next : {false, true}) {
            const float cx = w - (next ? 26.0f : 56.0f);
            const float cy = 24.0f;
            const std::string act = next ? "dk:cal-next" : "dk:cal-prev";
            if (hovered_action_id_ == act) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ox + cx - 13.0f, oy + cy - 12.0f, ox + cx + 13.0f, oy + cy + 12.0f), 6.0f, 6.0f),
                                         Brush(dc, hover_bg));
            }
            const float dx = next ? 2.2f : -2.2f;
            dc->DrawLine(D2D1::Point2F(ox + cx - dx, oy + cy - 4.5f), D2D1::Point2F(ox + cx + dx, oy + cy), Brush(dc, pri), 1.5f, round.Get());
            dc->DrawLine(D2D1::Point2F(ox + cx + dx, oy + cy), D2D1::Point2F(ox + cx - dx, oy + cy + 4.5f), Brush(dc, pri), 1.5f, round.Get());
            click_regions_.push_back({act, RECT{static_cast<LONG>(cx - 13.0f), 12, static_cast<LONG>(cx + 13.0f), 36}});
        }

        // 7-column month grid respecting WidgetPrefs::week_start (1=Mon, 0=Sun)
        static constexpr const wchar_t* kHeadSun[7] = {L"日", L"一", L"二", L"三", L"四", L"五", L"六"};
        const int ws = dock_panel_week_start_ ? 1 : 0;
        const float gx = 12.0f;
        const float cw = (w - 24.0f) / 7.0f;
        const float ch = 28.0f;
        const float gy = 80.0f;
        for (int c = 0; c < 7; ++c) {
            const int dow = (c + ws) % 7;
            const bool wknd = (dow == 0 || dow == 6);
            centered(kHeadSun[dow], fmt_small_.Get(), gx + cw * static_cast<float>(c), 58.0f,
                     gx + cw * static_cast<float>(c + 1), 75.0f, wknd ? red : sec);
        }
        const int first_dow = DayOfWeek(year, month, 1);
        const int offset = (first_dow - ws + 7) % 7;
        const int days = DaysInMonth(year, month);
        for (int d = 1; d <= days; ++d) {
            const int slot = offset + d - 1;
            const int col = slot % 7;
            const int row = slot / 7;
            const float cx = gx + cw * (static_cast<float>(col) + 0.5f);
            const float cy = gy + ch * (static_cast<float>(row) + 0.5f);
            const bool today = this_month && (d == now.wDay);
            if (today) {
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ox + cx, oy + cy), 11.5f, 11.5f), Brush(dc, red));
            }
            centered(std::to_wstring(d), today ? fmt_body_semi_.Get() : fmt_small_.Get(),
                     cx - cw * 0.5f, cy - 8.5f, cx + cw * 0.5f, cy + 9.5f, today ? 0xFFFFFFFFu : pri);
        }

        const float ny = gy + ch * 6.0f + 6.0f;
        hline(ny);
        text(L"今日待办", fmt_small_.Get(), 16.0f, ny + 8.0f, w - 16.0f, ny + 24.0f, sec);
        float ty = ny + 28.0f;
        int shown = 0;
        for (const auto& td : dock_panel_todos_) {
            if (td.done || td.text.empty()) continue;
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ox + 20.0f, oy + ty + 8.0f), 2.8f, 2.8f), Brush(dc, red));
            text(td.text, fmt_small_.Get(), 30.0f, ty, w - 16.0f, ty + 18.0f, pri);
            ty += 20.0f;
            if (++shown >= 2) break;
        }
        if (shown == 0) {
            text(L"今天没有未完成的待办", fmt_small_.Get(), 16.0f, ty, w - 16.0f, ty + 18.0f, sec);
        }
        footer_link(L"打开系统通知中心…", "dk:notif-center");
        break;
    }

    default:
        break;
    }
}

bool DesktopOverlayController::SaveDockPanelSnapshot(const wchar_t* path,
                                                     DockPanelKind kind,
                                                     const live::DockSystemSnapshot& snap,
                                                     bool dark,
                                                     uint8_t week_start,
                                                     uint8_t battery_low_pct) {
    if (!path || !*path || kind == DockPanelKind::None) return false;
    if (!EnsureRenderDevices() || !d2d_factory_) return false;
    dark_mode_ = dark;
    dock_panel_light_ink_ = dark;
    active_kind_ = OverlayKind::DockSystemPanel;
    dock_panel_kind_ = kind;
    dock_panel_snap_ = snap;
    dock_panel_tray_index_ = 0;
    dock_panel_week_start_ = week_start ? 1u : 0u;
    dock_panel_battery_low_pct_ = battery_low_pct;
    dock_panel_month_offset_ = 0;
    dock_panel_todos_ = {{L"15:00 项目评审", false}, {L"整理桌面小组件", false}};
    ComputeDockPanelDimensions(kind, snap, card_w_, card_h_);

    const UINT pad = 20;
    const UINT img_w = static_cast<UINT>(card_w_) + pad * 2;
    const UINT img_h = static_cast<UINT>(card_h_) + pad * 2;
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) || !wic) {
        return false;
    }
    ComPtr<IWICBitmap> wic_bmp;
    if (FAILED(wic->CreateBitmap(img_w, img_h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &wic_bmp)) || !wic_bmp) {
        return false;
    }
    const D2D1_RENDER_TARGET_PROPERTIES rt_props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(d2d_factory_->CreateWicBitmapRenderTarget(wic_bmp.Get(), rt_props, &rt)) || !rt) return false;
    ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(rt.As(&dc)) || !dc) return false;

    dc->BeginDraw();
    // Backdrop wall so frosted glass panel contrast is accurately visible in snapshots
    dc->Clear(dark ? D2D1::ColorF(0.08f, 0.11f, 0.18f, 1.0f) : D2D1::ColorF(0.84f, 0.89f, 0.95f, 1.0f));
    click_regions_.clear();
    pulse_buttons_.clear();
    DrawMainContent(dc.Get(), static_cast<float>(pad), static_cast<float>(pad), true);
    if (FAILED(dc->EndDraw())) return false;

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(img_w, img_h))) {
        return false;
    }
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&pf);
    const bool ok = SUCCEEDED(frame->WriteSource(wic_bmp.Get(), nullptr)) &&
                    SUCCEEDED(frame->Commit()) &&
                    SUCCEEDED(enc->Commit());
    active_kind_ = OverlayKind::None;
    return ok;
}

} // namespace pulse::desktop::overlays

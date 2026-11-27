// dock_tile.cpp - macOS 26 Tahoe Liquid Glass Dock layout, fisheye magnification,
// 3-hop launch bounce, floating frosted tooltip pill, and live taskbar running detection.
#include "host/render/dock_tile.h"

#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace pulse::desktop::render {
namespace {

using Microsoft::WRL::ComPtr;

constexpr float kPi = 3.14159265358979323846f;
constexpr double kBounceHopDurSec = 0.36;
constexpr int kBounceHops = 3;

D2D1_COLOR_F ArgbToColor(uint32_t argb, float opacity = 1.0f) noexcept {
    const float a = static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;
    const float r = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
    const float g = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
    const float b = static_cast<float>(argb & 0xFFu) / 255.0f;
    return D2D1::ColorF(r, g, b, std::clamp(a * opacity, 0.0f, 1.0f));
}

std::wstring LowerBasename(const std::wstring& full_path) {
    if (full_path.empty()) return {};
    const size_t slash = full_path.find_last_of(L"\\/");
    std::wstring name = (slash == std::wstring::npos) ? full_path : full_path.substr(slash + 1);
    for (wchar_t& ch : name) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return name;
}

bool EndsWithNoCase(const std::wstring& s, const wchar_t* suffix) noexcept {
    const size_t slen = std::wcslen(suffix);
    if (s.size() < slen) return false;
    return _wcsicmp(s.c_str() + (s.size() - slen), suffix) == 0;
}

bool IsTaskbarAppWindow(HWND hwnd) noexcept {
    if (!hwnd || !IsWindowVisible(hwnd)) return false;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return false;
    if (GetWindow(hwnd, GW_OWNER) != nullptr && !(ex & WS_EX_APPWINDOW)) return false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return false;
    }
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
        wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0) {
        return false;
    }
    return GetWindowTextLengthW(hwnd) > 0;
}

std::wstring WindowExeBasename(HWND hwnd) noexcept {
    if (!hwnd) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return {};
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};
    wchar_t buf[MAX_PATH * 2]{};
    DWORD len = ARRAYSIZE(buf);
    std::wstring out;
    if (QueryFullProcessImageNameW(proc, 0, buf, &len) && len > 0) {
        out = LowerBasename(std::wstring(buf, len));
    }
    CloseHandle(proc);
    return out;
}

} // namespace

float TileCornerRadiusDip(const model::DesktopTile& tile, const D2D1_RECT_F& rect) noexcept {
    const auto active = model::ActiveTileView(tile);
    const float rw = std::max(1.0f, rect.right - rect.left);
    const float rh = std::max(1.0f, rect.bottom - rect.top);
    if (active.kind == model::TileKind::Apps && tile.size.cols >= 2) {
        if (rh < 145.0f || tile.size.rows == 1) {
            return std::min({rw * 0.5f, rh * 0.34f, 26.0f});
        }
        return std::min({rw * 0.5f, rh * 0.20f, 22.0f});
    }
    // macOS widgets share one corner radius at every size (22 pt; app-icon-like on the 1×1 squares), and the
    // parts inside follow it concentrically (radius minus the 14 DIP inset).
    return std::min({22.0f, rw * 0.5f, rh * 0.5f});
}

bool IsDockBounceActive(double bounce_start_ms, double now_ms) noexcept {
    if (bounce_start_ms <= 0.0 || now_ms < bounce_start_ms) return false;
    const double elapsed_sec = (now_ms - bounce_start_ms) / 1000.0;
    return elapsed_sec < (kBounceHopDurSec * kBounceHops);
}

float EvaluateDockBounceDip(double bounce_start_ms, double now_ms) noexcept {
    if (!IsDockBounceActive(bounce_start_ms, now_ms)) return 0.0f;
    const double elapsed_sec = (now_ms - bounce_start_ms) / 1000.0;
    const int hop = std::min(kBounceHops - 1, static_cast<int>(elapsed_sec / kBounceHopDurSec));
    const float u = static_cast<float>((elapsed_sec - static_cast<double>(hop) * kBounceHopDurSec) / kBounceHopDurSec);
    const float amp = 18.0f * std::pow(0.58f, static_cast<float>(hop));
    return amp * 4.0f * u * (1.0f - u);
}

DockTileLayout ComputeDockTileLayout(const model::DesktopTile& tile,
                                     const D2D1_RECT_F& rect,
                                     size_t app_count,
                                     const DockLayoutInput& input) noexcept {
    DockTileLayout out{};
    const float rw = std::max(1.0f, rect.right - rect.left);
    const float rh = std::max(1.0f, rect.bottom - rect.top);
    out.corner_radius = TileCornerRadiusDip(tile, rect);
    out.single_row = (rh < 145.0f) || (tile.size.rows == 1);

    const float presence = std::clamp(input.hover_presence, 0.0f, 1.0f);

    if (out.single_row) {
        // Pure macOS 26 Tahoe horizontal Dock capsule: large crisp icons, dedicated bottom running-dot
        // channel, raised-cosine fisheye hover wave, and sleek side chevrons if overflowing.
        float base = std::clamp(rh * 0.54f, 32.0f, 52.0f);
        float gap = std::clamp(base * 0.24f, 8.0f, 14.0f);
        auto compute_cols = [&](float pad_x) -> int {
            const float avail_w = std::max(base, rw - pad_x * 2.0f);
            return std::max(1, static_cast<int>(std::floor((avail_w + gap) / (base + gap))));
        };
        float pad_x = std::clamp(rw * 0.035f, 12.0f, 20.0f);
        out.cols = compute_cols(pad_x);
        if (app_count > static_cast<size_t>(out.cols) && rw >= 160.0f) {
            // Reserve left/right margin for sleek side pager chevrons so bottom stays 100% pure for running dots.
            pad_x = 30.0f;
            out.cols = compute_cols(pad_x);
        }
        out.rows = 1;
        out.room = static_cast<size_t>(out.cols);
        out.pages = std::max(1, static_cast<int>((app_count + out.room - 1) / out.room));
        out.active_page = std::clamp(input.apps_page, 0, out.pages - 1);
        out.first_index = static_cast<size_t>(out.active_page) * out.room;
        out.base_icon = base;

        const size_t shown = (app_count > out.first_index)
            ? std::min(out.room, app_count - out.first_index)
            : 0;
        if (shown == 0) return out;

        const float dot_y = rect.bottom - 8.5f;
        const float icon_bottom_rest = dot_y - 7.0f;
        const float rest_cy = std::min(rect.top + rh * 0.45f, icon_bottom_rest - base * 0.5f);

        // Compute evenly centered resting centers.
        const float rest_total_w = static_cast<float>(shown) * base +
                                   static_cast<float>(shown > 1 ? shown - 1 : 0) * gap;
        const float rest_start_x = (rect.left + rect.right - rest_total_w) * 0.5f;

        out.slots.resize(shown);
        constexpr float kMaxScale = 1.44f;
        const float influence = base * 2.45f;

        for (size_t k = 0; k < shown; ++k) {
            auto& s = out.slots[k];
            s.app_index = out.first_index + k;
            s.row = 0;
            s.col = static_cast<int>(k);
            s.base_size = base;
            s.rest_cx = rest_start_x + static_cast<float>(k) * (base + gap) + base * 0.5f;
            s.rest_cy = rest_cy;
            s.dot_y = dot_y;

            float sc = 1.0f;
            if (presence > 0.001f) {
                const float u = std::fabs(input.smooth_cursor_x - s.rest_cx) / influence;
                if (u < 1.0f) {
                    const float wave = 0.5f * (1.0f + std::cos(kPi * u));
                    sc = 1.0f + presence * (kMaxScale - 1.0f) * std::pow(wave, 1.35f);
                }
            }
            if (input.pressed_app_index == static_cast<int>(s.app_index)) {
                sc *= 0.90f;
            }
            s.scale = sc;
            s.draw_size = base * sc;
            const float bounce = (input.bouncing_app_index == static_cast<int>(s.app_index))
                ? input.bounce_offset_dip : 0.0f;
            const float mag_lift = std::max(0.0f, sc - 1.0f) * base * 0.36f;
            s.lift_y = mag_lift + bounce;
            s.draw_cy = rest_cy - s.lift_y;
        }

        // Neighbor parting layout (centered horizontally, softly clamped inside pad_x).
        float total_w = 0.0f;
        float sum_sizes = 0.0f;
        for (size_t k = 0; k < shown; ++k) {
            sum_sizes += out.slots[k].draw_size;
            total_w += out.slots[k].draw_size;
            if (k + 1 < shown) {
                const float avg_s = 0.5f * (out.slots[k].scale + out.slots[k + 1].scale);
                total_w += gap * (0.72f + 0.28f * avg_s);
            }
        }
        const float max_span = std::max(sum_sizes, rw - std::max(10.0f, pad_x * 1.4f));
        const float gap_scale = (total_w > max_span && total_w > sum_sizes + 1e-3f)
            ? std::clamp((max_span - sum_sizes) / (total_w - sum_sizes), 0.15f, 1.0f)
            : 1.0f;
        const float final_total_w = sum_sizes + (total_w - sum_sizes) * gap_scale;
        float x = (rect.left + rect.right - final_total_w) * 0.5f;
        for (size_t k = 0; k < shown; ++k) {
            out.slots[k].draw_cx = x + out.slots[k].draw_size * 0.5f;
            x += out.slots[k].draw_size;
            if (k + 1 < shown) {
                const float avg_s = 0.5f * (out.slots[k].scale + out.slots[k + 1].scale);
                x += gap * (0.72f + 0.28f * avg_s) * gap_scale;
            }
        }
        return out;
    }

    // Multi-row Liquid Glass Dock shelf (e.g. 4x2, 2x2, 4x3, 4x4):
    // Clean headerless glass capsule with crisp icon rows, live per-row fisheye wave,
    // running indicator dots, and subtle labels when unhovered (floating tooltip on hover).
    const float pad_x = 14.0f;
    const float top_pad = 12.0f;
    const float avail_w = std::max(44.0f, rw - pad_x * 2.0f);
    out.cols = std::clamp(static_cast<int>(std::floor((avail_w + 10.0f) / 66.0f)), 2, 12);

    auto compute_rows = [&](float bottom_pad) -> int {
        const float ah = std::max(64.0f, rh - top_pad - bottom_pad);
        return std::max(1, static_cast<int>(std::floor(ah / 74.0f)));
    };
    out.rows = compute_rows(10.0f);
    out.room = static_cast<size_t>(out.cols * out.rows);
    float bottom_pad = 10.0f;
    if (app_count > out.room) {
        bottom_pad = 22.0f;
        out.rows = compute_rows(bottom_pad);
        out.room = static_cast<size_t>(out.cols * out.rows);
        out.pager_dot_y = rect.bottom - 10.0f;
    }
    out.pages = std::max(1, static_cast<int>((app_count + out.room - 1) / out.room));
    out.active_page = std::clamp(input.apps_page, 0, out.pages - 1);
    out.first_index = static_cast<size_t>(out.active_page) * out.room;

    const size_t shown = (app_count > out.first_index)
        ? std::min(out.room, app_count - out.first_index)
        : 0;
    if (shown == 0) return out;

    const int used_rows = std::min(out.rows, std::max(1, static_cast<int>((shown + static_cast<size_t>(out.cols) - 1) / static_cast<size_t>(out.cols))));
    const float avail_h = std::max(64.0f, rh - top_pad - bottom_pad);
    const float row_h = std::clamp(avail_h / static_cast<float>(used_rows), 70.0f, 94.0f);
    const float block_top = rect.top + top_pad + std::max(0.0f, (avail_h - row_h * static_cast<float>(used_rows)) * 0.5f);
    const float cell_w = avail_w / static_cast<float>(out.cols);
    const float base = std::clamp(std::min(cell_w * 0.62f, row_h * 0.50f), 34.0f, 44.0f);
    out.base_icon = base;

    int active_row = -1;
    if (presence > 0.001f && input.cursor_y >= block_top - 12.0f &&
        input.cursor_y <= block_top + row_h * static_cast<float>(used_rows) + 10.0f) {
        active_row = std::clamp(static_cast<int>((input.cursor_y - block_top) / row_h), 0, used_rows - 1);
    }

    out.slots.resize(shown);
    constexpr float kRowMaxScale = 1.32f;
    const float influence = std::max(base * 2.35f, cell_w * 1.65f);

    for (int r = 0; r < used_rows; ++r) {
        const size_t row_start = static_cast<size_t>(r * out.cols);
        const size_t row_count = std::min(static_cast<size_t>(out.cols), shown - row_start);
        if (row_count == 0) break;

        const float row_top = block_top + static_cast<float>(r) * row_h;
        const float rest_cy = row_top + 6.0f + base * 0.5f;
        const float dot_y = rest_cy + base * 0.5f + 5.0f;
        const float label_top = dot_y + 3.5f;
        const float label_bottom = std::min(row_top + row_h - 1.0f, label_top + 17.0f);

        // Center sparse rows cleanly when a single row has fewer icons than full columns,
        // while keeping multi-row pages neatly left-aligned on the grid columns.
        const float row_left = (used_rows == 1 && row_count < static_cast<size_t>(out.cols))
            ? (rect.left + rect.right - static_cast<float>(row_count) * cell_w) * 0.5f
            : (rect.left + pad_x);

        const float row_presence = (r == active_row) ? presence : 0.0f;
        const float gap = std::max(6.0f, cell_w - base);

        for (size_t c = 0; c < row_count; ++c) {
            const size_t k = row_start + c;
            auto& s = out.slots[k];
            s.app_index = out.first_index + k;
            s.row = r;
            s.col = static_cast<int>(c);
            s.base_size = base;
            s.rest_cx = row_left + (static_cast<float>(c) + 0.5f) * cell_w;
            s.rest_cy = rest_cy;
            s.dot_y = dot_y;
            s.label_rect = D2D1::RectF(s.rest_cx - cell_w * 0.48f, label_top,
                                       s.rest_cx + cell_w * 0.48f, label_bottom);

            float sc = 1.0f;
            if (row_presence > 0.001f) {
                const float u = std::fabs(input.smooth_cursor_x - s.rest_cx) / influence;
                if (u < 1.0f) {
                    const float wave = 0.5f * (1.0f + std::cos(kPi * u));
                    sc = 1.0f + row_presence * (kRowMaxScale - 1.0f) * std::pow(wave, 1.35f);
                }
            }
            if (input.pressed_app_index == static_cast<int>(s.app_index)) {
                sc *= 0.90f;
            }
            s.scale = sc;
            s.draw_size = base * sc;
            const float bounce = (input.bouncing_app_index == static_cast<int>(s.app_index))
                ? input.bounce_offset_dip : 0.0f;
            const float mag_lift = std::max(0.0f, sc - 1.0f) * base * 0.28f;
            s.lift_y = mag_lift + bounce;
            s.draw_cy = rest_cy - s.lift_y;
        }

        // Apply subtle horizontal parting on the active magnified row while preserving alignment.
        if (row_presence > 0.001f && row_count > 1) {
            float total_w = 0.0f;
            float sum_sizes = 0.0f;
            for (size_t c = 0; c < row_count; ++c) {
                const auto& s = out.slots[row_start + c];
                sum_sizes += s.draw_size;
                total_w += s.draw_size;
                if (c + 1 < row_count) {
                    const float avg_s = 0.5f * (s.scale + out.slots[row_start + c + 1].scale);
                    total_w += gap * (0.78f + 0.22f * avg_s);
                }
            }
            const float row_span = static_cast<float>(row_count) * cell_w - gap * 0.25f;
            const float max_span = std::max(sum_sizes, std::min(rw - 12.0f, row_span + 18.0f));
            const float gap_scale = (total_w > max_span && total_w > sum_sizes + 1e-3f)
                ? std::clamp((max_span - sum_sizes) / (total_w - sum_sizes), 0.20f, 1.0f)
                : 1.0f;
            const float final_w = sum_sizes + (total_w - sum_sizes) * gap_scale;
            const float row_center = row_left + static_cast<float>(row_count) * cell_w * 0.5f;
            float x = std::clamp(row_center - final_w * 0.5f, rect.left + 6.0f,
                                 std::max(rect.left + 6.0f, rect.right - 6.0f - final_w));
            for (size_t c = 0; c < row_count; ++c) {
                auto& s = out.slots[row_start + c];
                s.draw_cx = x + s.draw_size * 0.5f;
                x += s.draw_size;
                if (c + 1 < row_count) {
                    const float avg_s = 0.5f * (s.scale + out.slots[row_start + c + 1].scale);
                    x += gap * (0.78f + 0.22f * avg_s) * gap_scale;
                }
            }
        } else {
            for (size_t c = 0; c < row_count; ++c) {
                out.slots[row_start + c].draw_cx = out.slots[row_start + c].rest_cx;
            }
        }
    }
    return out;
}

int HitTestDockTileApp(const model::DesktopTile& tile,
                       const D2D1_RECT_F& rect,
                       size_t app_count,
                       const DockLayoutInput& input,
                       float px, float py) noexcept {
    if (app_count == 0) return -1;
    const auto active = model::ActiveTileView(tile);
    if (active.kind != model::TileKind::Apps || tile.size.cols < 2) return -1;

    const DockTileLayout layout = ComputeDockTileLayout(tile, rect, app_count, input);
    if (layout.slots.empty()) return -1;

    int best = -1;
    float best_d2 = 1e30f;
    for (const auto& s : layout.slots) {
        const float half_w = s.draw_size * 0.58f + 3.0f;
        const float top = s.draw_cy - s.draw_size * 0.56f - 4.0f;
        const float bottom = layout.single_row
            ? (rect.bottom - 2.0f)
            : std::max(s.dot_y + 18.0f, s.label_rect.bottom + 2.0f);
        if (px < s.draw_cx - half_w || px > s.draw_cx + half_w || py < top || py > bottom) {
            continue;
        }
        const float dx = px - s.draw_cx;
        const float dy = py - s.draw_cy;
        const float d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = static_cast<int>(s.app_index);
        }
    }
    return best;
}

RunningAppSnapshot QueryRunningAppSnapshot() noexcept {
    RunningAppSnapshot snap{};
    if (const HWND fg = GetForegroundWindow(); fg && IsTaskbarAppWindow(fg)) {
        snap.active_exe = WindowExeBasename(fg);
        if (!snap.active_exe.empty()) {
            snap.running_exes.insert(snap.active_exe);
        }
    }

    auto enum_cb = [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* out = reinterpret_cast<RunningAppSnapshot*>(lp);
        if (!out || !IsTaskbarAppWindow(hwnd)) return TRUE;
        std::wstring exe = WindowExeBasename(hwnd);
        if (!exe.empty()) {
            out->running_exes.insert(std::move(exe));
        }
        return TRUE;
    };
    EnumWindows(enum_cb, reinterpret_cast<LPARAM>(&snap));
    return snap;
}

std::wstring ResolveShortcutExeBasename(const std::wstring& path) noexcept {
    if (path.empty()) return {};
    if (EndsWithNoCase(path, L".exe")) {
        return LowerBasename(path);
    }
    if (!EndsWithNoCase(path, L".lnk")) {
        return {};
    }
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) || !link) {
        return {};
    }
    ComPtr<IPersistFile> pf;
    if (FAILED(link.As(&pf)) || !pf) return {};
    if (FAILED(pf->Load(path.c_str(), STGM_READ))) return {};
    wchar_t target[MAX_PATH * 2]{};
    if (FAILED(link->GetPath(target, ARRAYSIZE(target), nullptr, SLGP_RAWPATH)) || target[0] == L'\0') {
        return {};
    }
    std::wstring resolved(target);
    if (!EndsWithNoCase(resolved, L".exe")) return {};
    return LowerBasename(resolved);
}

D2D1_RECT_F DockPagerArrowRect(const D2D1_RECT_F& rect, bool right) noexcept {
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float cx = right ? (rect.right - 14.0f) : (rect.left + 14.0f);
    return D2D1::RectF(cx - 10.0f, cy - 14.0f, cx + 10.0f, cy + 14.0f);
}

void DrawDockTopSheen(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect,
                      float corner_radius, bool dark_ink) noexcept {
    if (!dc) return;
    const float inset = std::min(corner_radius, (rect.right - rect.left) * 0.42f);
    if (rect.right - rect.left <= inset * 2.0f + 4.0f) return;

    const float peak_a = dark_ink ? 0.22f : 0.34f;
    const D2D1_GRADIENT_STOP stops[3] = {
        {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
        {0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, peak_a)},
        {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
    };
    ComPtr<ID2D1GradientStopCollection> sc;
    if (FAILED(dc->CreateGradientStopCollection(stops, 3, &sc)) || !sc) return;
    ComPtr<ID2D1LinearGradientBrush> br;
    const D2D1_RECT_F strip = D2D1::RectF(rect.left + inset, rect.top + 1.0f,
                                          rect.right - inset, rect.top + 2.2f);
    if (SUCCEEDED(dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(strip.left, strip.top),
                                                D2D1::Point2F(strip.right, strip.top)),
            sc.Get(), &br)) && br) {
        dc->FillRectangle(strip, br.Get());
    }
    // Subtle bottom capsule sheen highlight matching liquid_dock.cpp.
    const D2D1_GRADIENT_STOP bstops[3] = {
        {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
        {0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, dark_ink ? 0.11f : 0.18f)},
        {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
    };
    ComPtr<ID2D1GradientStopCollection> bsc;
    ComPtr<ID2D1LinearGradientBrush> bbr;
    if (SUCCEEDED(dc->CreateGradientStopCollection(bstops, 3, &bsc)) && bsc) {
        const D2D1_RECT_F bstrip = D2D1::RectF(rect.left + inset, rect.bottom - 2.0f,
                                               rect.right - inset, rect.bottom - 1.0f);
        if (SUCCEEDED(dc->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(bstrip.left, bstrip.top),
                                                    D2D1::Point2F(bstrip.right, bstrip.top)),
                bsc.Get(), &bbr)) && bbr) {
            dc->FillRectangle(bstrip, bbr.Get());
        }
    }
}

void DrawDockPagerChevron(ID2D1DeviceContext* dc, const D2D1_RECT_F& btn_rect,
                          bool right, bool enabled, bool dark_ink) noexcept {
    if (!dc) return;
    const float cx = (btn_rect.left + btn_rect.right) * 0.5f;
    const float cy = (btn_rect.top + btn_rect.bottom) * 0.5f;
    const float op = enabled ? 0.86f : 0.24f;
    ComPtr<ID2D1SolidColorBrush> br;
    if (enabled && SUCCEEDED(dc->CreateSolidColorBrush(
            ArgbToColor(dark_ink ? 0x26FFFFFFu : 0x1A000000u), &br)) && br) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 8.5f, cy - 11.5f, cx + 8.5f, cy + 11.5f),
                                                   8.5f, 8.5f), br.Get());
    }
    br.Reset();
    if (SUCCEEDED(dc->CreateSolidColorBrush(
            ArgbToColor(dark_ink ? 0xFFFFFFFFu : 0xFF1B2028u, op), &br)) && br) {
        const float dir = right ? 1.0f : -1.0f;
        dc->DrawLine(D2D1::Point2F(cx - 2.2f * dir, cy - 4.2f),
                     D2D1::Point2F(cx + 2.2f * dir, cy), br.Get(), 1.6f);
        dc->DrawLine(D2D1::Point2F(cx + 2.2f * dir, cy),
                     D2D1::Point2F(cx - 2.2f * dir, cy + 4.2f), br.Get(), 1.6f);
    }
}

void DrawDockContactShadow(ID2D1DeviceContext* dc, float cx, float shadow_y,
                           float icon_size, float elevation_dip) noexcept {
    if (!dc) return;
    const float alpha = std::clamp(0.20f - elevation_dip * 0.0042f, 0.05f, 0.20f);
    ComPtr<ID2D1SolidColorBrush> br;
    if (SUCCEEDED(dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, alpha), &br)) && br) {
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, shadow_y),
                                      icon_size * 0.36f, 2.7f),
                        br.Get());
    }
}

void DrawDockRunningDot(ID2D1DeviceContext* dc, float cx, float dot_y,
                        bool running, bool active, bool dark_ink) noexcept {
    if (!dc || (!running && !active)) return;
    ComPtr<ID2D1SolidColorBrush> br;
    if (active) {
        if (SUCCEEDED(dc->CreateSolidColorBrush(ArgbToColor(0x736BD1FFu), &br)) && br) {
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, dot_y), 5.0f, 3.4f), br.Get());
        }
        br.Reset();
        if (SUCCEEDED(dc->CreateSolidColorBrush(ArgbToColor(0xF5FFFFFFu), &br)) && br) {
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, dot_y), 2.2f, 2.2f), br.Get());
        }
        return;
    }

    const uint32_t halo_col = dark_ink ? 0x38FFFFFFu : 0x26000000u;
    const uint32_t core_col = dark_ink ? 0xE6FFFFFFu : 0xCC1B2432u;
    if (SUCCEEDED(dc->CreateSolidColorBrush(ArgbToColor(halo_col), &br)) && br) {
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, dot_y), 4.1f, 2.9f), br.Get());
    }
    br.Reset();
    if (SUCCEEDED(dc->CreateSolidColorBrush(ArgbToColor(core_col), &br)) && br) {
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, dot_y), 2.0f, 2.0f), br.Get());
    }
}

void DrawDockTooltipPill(ID2D1Factory* factory, ID2D1DeviceContext* dc,
                         const std::wstring& label,
                         float anchor_cx, float icon_top_y,
                         float clamp_left, float clamp_right,
                         float text_width_dip, float alpha,
                         const DockTooltipTextFn& draw_text) noexcept {
    if (!dc || label.empty() || alpha <= 0.02f) return;
    const float a = std::clamp(alpha, 0.0f, 1.0f);
    const float tip_w = std::clamp(text_width_dip + 22.0f, 44.0f, 220.0f);
    const float tip_h = 24.0f;
    const float tip_rad = 11.5f;
    const float notch_h = 5.0f;

    float tip_left = anchor_cx - tip_w * 0.5f;
    if (clamp_right - clamp_left > tip_w + 8.0f) {
        tip_left = std::clamp(tip_left, clamp_left + 4.0f, clamp_right - tip_w - 4.0f);
    }
    const float tip_bottom = icon_top_y - 9.0f;
    const float tip_top = tip_bottom - tip_h;
    const D2D1_RECT_F tip_rc = D2D1::RectF(tip_left, tip_top, tip_left + tip_w, tip_bottom);
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(tip_rc, tip_rad, tip_rad);

    // Subtle elevation shadow beneath tooltip pill.
    ComPtr<ID2D1SolidColorBrush> br;
    if (SUCCEEDED(dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.28f * a), &br)) && br) {
        const D2D1_RECT_F sh_rc = D2D1::RectF(tip_rc.left, tip_rc.top + 2.5f, tip_rc.right, tip_rc.bottom + 2.5f);
        dc->FillRoundedRectangle(D2D1::RoundedRect(sh_rc, tip_rad, tip_rad), br.Get());
    }

    // Dark frosted macOS HUD capsule fill.
    br.Reset();
    if (SUCCEEDED(dc->CreateSolidColorBrush(ArgbToColor(0xDC161A24u, a), &br)) && br) {
        dc->FillRoundedRectangle(rr, br.Get());
    }

    // Downward triangular pointer notch pointing straight at the hovered icon.
    if (factory) {
        const float notch_cx = std::clamp(anchor_cx, tip_rc.left + 12.0f, tip_rc.right - 12.0f);
        ComPtr<ID2D1PathGeometry> tri;
        if (SUCCEEDED(factory->CreatePathGeometry(&tri)) && tri) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(tri->Open(&sink)) && sink) {
                sink->BeginFigure(D2D1::Point2F(notch_cx - 5.2f, tip_bottom - 0.5f), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(notch_cx + 5.2f, tip_bottom - 0.5f));
                sink->AddLine(D2D1::Point2F(notch_cx, tip_bottom + notch_h));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                if (br) dc->FillGeometry(tri.Get(), br.Get());
            }
        }
    }

    // Crisp specular white hairline border.
    br.Reset();
    if (SUCCEEDED(dc->CreateSolidColorBrush(ArgbToColor(0x52FFFFFFu, a), &br)) && br) {
        const D2D1_RECT_F inset = D2D1::RectF(tip_rc.left + 0.5f, tip_rc.top + 0.5f,
                                              tip_rc.right - 0.5f, tip_rc.bottom - 0.5f);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(inset, tip_rad - 0.5f, tip_rad - 0.5f), br.Get(), 1.0f);
    }

    if (draw_text) {
        const D2D1_RECT_F text_rc = D2D1::RectF(tip_rc.left + 7.0f, tip_rc.top,
                                                tip_rc.right - 7.0f, tip_rc.bottom);
        draw_text(label, text_rc, 0xF5FFFFFFu, a);
    }
}

} // namespace pulse::desktop::render

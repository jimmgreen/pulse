// dock_genie.cpp - Direct3D 11 + DirectComposition Genie & Scale minimize/restore animator
// ported cleanly from reference demo/genie_animator.cpp + WacOS genio.cpp.
#include "host/render/dock_genie.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pulse::desktop::render {
namespace {

using Microsoft::WRL::ComPtr;

constexpr float kPi = 3.14159265358979323846f;
constexpr int kMeshCols = 32;
constexpr int kMeshRows = 96; // dense rows: the funnel walls stay smooth curves, not facets

struct GenieVertex {
    float x, y;
    float u, v;
    float alpha;
    float sheen;
};

constexpr const char* kGenieShaderSrc = R"HLSL(
struct VSIn {
    float2 pos   : POSITION;
    float2 uv    : TEXCOORD0;
    float  alpha : TEXCOORD1;
    float  sheen : TEXCOORD2;
};
struct PSIn {
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float  alpha : TEXCOORD1;
    float  sheen : TEXCOORD2;
};

PSIn VSMain(VSIn i) {
    PSIn o;
    o.pos = float4(i.pos, 0.0f, 1.0f);
    o.uv = i.uv;
    o.alpha = i.alpha;
    o.sheen = i.sheen;
    return o;
}

Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);

// `sheen` carries the window's corner radius as a fraction of its width, so the corners stay
// round (the Windows 11 radius) at any aspect ratio instead of stretching with the uv square.
float4 PSMain(PSIn i) : SV_TARGET {
    float4 c = tex0.Sample(samp0, i.uv);
    float tw, th;
    tex0.GetDimensions(tw, th);
    const float2 size = float2(tw, th);
    const float rad = max(0.5f, i.sheen * tw);
    const float2 d = abs(i.uv - 0.5f) * size;
    const float2 q = max(d - (size * 0.5f - rad), 0.0f);
    const float dist = length(q) - rad;
    const float aa = max(fwidth(dist), 0.75f);
    const float corner = saturate(0.5f - dist / aa);
    const float a = c.a * i.alpha * corner;
    return float4(c.rgb * i.alpha * corner, a);
}
)HLSL";

inline float Clamp01(float v) noexcept { return std::clamp(v, 0.0f, 1.0f); }

inline float SmoothStep(float a, float b, float x) noexcept {
    const float t = Clamp01((x - a) / std::max(1e-5f, b - a));
    return t * t * (3.0f - 2.0f * t);
}

inline float EaseInOutCubic(float t) noexcept {
    t = Clamp01(t);
    return (t < 0.5f) ? (4.0f * t * t * t) : (1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f);
}

inline float EaseOutCubic(float t) noexcept {
    t = Clamp01(t);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

inline float Lerp(float a, float b, float t) noexcept { return a + (b - a) * t; }

// Cubic in, quadratic out (value and speed continuous at 0.5): the window gathers speed down the
// funnel and lands in the icon without the long cubic tail that parked it on the icon at the end.
inline float EaseInCubicOutQuad(float t) noexcept {
    t = Clamp01(t);
    if (t < 0.5f) return 3.2f * t * t * t;
    const float u = 1.0f - t;
    return 1.0f - 2.4f * u * u;
}

// The last stretch: once the window is icon sized it is drawn in, a slight shrink about the
// icon's centre while it fades, so it reads as absorbed rather than switched off.
void AbsorbIntoIcon(std::vector<GenieVertex>& verts, float cx_ndc, float cy_ndc, float p) noexcept {
    const float s = SmoothStep(0.84f, 1.0f, p);
    if (s <= 0.0f) return;
    const float k = 1.0f - 0.32f * s;
    for (auto& v : verts) {
        v.x = cx_ndc + (v.x - cx_ndc) * k;
        v.y = cy_ndc + (v.y - cy_ndc) * k;
    }
}

void BuildDeformedMesh(const RECT& canvas_rect,
                       const RECT& win_rect,
                       const RECT& dock_rect,
                       float progress,
                       DockMinimizeEffect style,
                       std::vector<GenieVertex>& out_verts,
                       uint8_t dock_edge = 0) {
    const float cw = static_cast<float>(std::max(1L, canvas_rect.right - canvas_rect.left));
    const float ch = static_cast<float>(std::max(1L, canvas_rect.bottom - canvas_rect.top));

    const float x0 = static_cast<float>(win_rect.left - canvas_rect.left);
    const float y0 = static_cast<float>(win_rect.top - canvas_rect.top);
    const float x1 = static_cast<float>(win_rect.right - canvas_rect.left);
    const float y1 = static_cast<float>(win_rect.bottom - canvas_rect.top);

    const float dx0 = static_cast<float>(dock_rect.left - canvas_rect.left);
    const float dy0 = static_cast<float>(dock_rect.top - canvas_rect.top);
    const float dx1 = static_cast<float>(dock_rect.right - canvas_rect.left);
    const float dy1 = static_cast<float>(dock_rect.bottom - canvas_rect.top);

    const float p = Clamp01(progress);
    const float fade_out = 1.0f - SmoothStep(0.88f, 1.0f, p);
    // Windows 11 window corners (8 px at 100%), as a fraction of the window width for the shader.
    const float corner_px = 8.0f * static_cast<float>(std::max(96u, GetDpiForSystem())) / 96.0f;
    const float corner_k = corner_px / std::max(16.0f, x1 - x0);
    const float icon_cx_ndc = (((dx0 + dx1) * 0.5f) / cw) * 2.0f - 1.0f;
    const float icon_cy_ndc = 1.0f - (((dy0 + dy1) * 0.5f) / ch) * 2.0f;

    out_verts.resize(static_cast<size_t>((kMeshRows + 1) * (kMeshCols + 1)));

    if (style == DockMinimizeEffect::Scale) {
        const float e = EaseInCubicOutQuad(SmoothStep(0.0f, 0.94f, p));
        const float l = Lerp(x0, dx0, e);
        const float r = Lerp(x1, dx1, e);
        const float t = Lerp(y0, dy0, e);
        const float b = Lerp(y1, dy1, e);

        for (int row = 0; row <= kMeshRows; ++row) {
            const float v = static_cast<float>(row) / static_cast<float>(kMeshRows);
            const float py = Lerp(t, b, v);
            const float ndc_y = 1.0f - (py / ch) * 2.0f;
            for (int col = 0; col <= kMeshCols; ++col) {
                const float u = static_cast<float>(col) / static_cast<float>(kMeshCols);
                const float px = Lerp(l, r, u);
                const float ndc_x = (px / cw) * 2.0f - 1.0f;
                out_verts[static_cast<size_t>(row * (kMeshCols + 1) + col)] = {
                    ndc_x, ndc_y, u, v, fade_out, corner_k
                };
            }
        }
        AbsorbIntoIcon(out_verts, icon_cx_ndc, icon_cy_ndc, p);
        return;
    }

    // Authentic macOS Genie curved funnel deformation along Bottom (0), Left (1), or Right (2) edge.
    // Phase 1 (to ~0.42) pulls the near edge into the funnel; the far edge then slides through it
    // and reaches the icon at ~0.94, leaving only the short absorb + fade, never a parked window.
    const float pinch = SmoothStep(0.0f, 0.42f, p);
    const float near_slide = EaseOutCubic(SmoothStep(0.0f, 0.62f, p));
    const float far_slide = EaseInCubicOutQuad(Clamp01((p - 0.16f) / 0.78f));

    if (dock_edge == 1 || dock_edge == 2) {
        const bool to_left = (dock_edge == 1);
        const float cur_left_x = Lerp(x0, dx0, to_left ? near_slide : far_slide);
        const float cur_right_x = Lerp(x1, dx1, to_left ? far_slide : near_slide);
        const float funnel_far_x = to_left ? x1 : x0;
        const float funnel_near_x = to_left ? std::min(funnel_far_x - 16.0f, dx0)
                                            : std::max(funnel_far_x + 16.0f, dx1);
        const float inv_span = 1.0f / std::max(16.0f, std::fabs(funnel_near_x - funnel_far_x));

        for (int col = 0; col <= kMeshCols; ++col) {
            const float u = static_cast<float>(col) / static_cast<float>(kMeshCols);
            const float px = Lerp(cur_left_x, cur_right_x, u);
            const float ndc_x = (px / cw) * 2.0f - 1.0f;

            const float h = Clamp01((to_left ? (funnel_far_x - px) : (px - funnel_far_x)) * inv_span);
            const float s_raw = 0.5f * (1.0f - std::cos(kPi * h));
            const float s_curve = std::pow(s_raw, 1.15f);
            const float wall_t = Clamp01(pinch * s_curve + far_slide * 0.12f * (1.0f - s_curve));

            const float top_y = Lerp(y0, dy0, wall_t);
            const float bot_y = Lerp(y1, dy1, wall_t);
            for (int row = 0; row <= kMeshRows; ++row) {
                const float v = static_cast<float>(row) / static_cast<float>(kMeshRows);
                const float py = Lerp(top_y, bot_y, v);
                const float ndc_y = 1.0f - (py / ch) * 2.0f;
                out_verts[static_cast<size_t>(row * (kMeshCols + 1) + col)] = {
                    ndc_x, ndc_y, u, v, fade_out, corner_k
                };
            }
        }
        AbsorbIntoIcon(out_verts, icon_cx_ndc, icon_cy_ndc, p);
        return;
    }

    const float cur_top_y = Lerp(y0, dy0, far_slide);
    const float cur_bot_y = Lerp(y1, dy1, near_slide);

    const float funnel_start_y = y0;
    const float funnel_end_y = std::max(funnel_start_y + 16.0f, dy1);
    const float inv_funnel_span = 1.0f / (funnel_end_y - funnel_start_y);

    for (int row = 0; row <= kMeshRows; ++row) {
        const float v = static_cast<float>(row) / static_cast<float>(kMeshRows);
        const float py = Lerp(cur_top_y, cur_bot_y, v);
        const float ndc_y = 1.0f - (py / ch) * 2.0f;

        const float h = Clamp01((py - funnel_start_y) * inv_funnel_span);
        const float s_raw = 0.5f * (1.0f - std::cos(kPi * h));
        const float s_curve = std::pow(s_raw, 1.15f);
        const float wall_t = Clamp01(pinch * s_curve + far_slide * 0.12f * (1.0f - s_curve));

        const float left_x = Lerp(x0, dx0, wall_t);
        const float right_x = Lerp(x1, dx1, wall_t);

        for (int col = 0; col <= kMeshCols; ++col) {
            const float u = static_cast<float>(col) / static_cast<float>(kMeshCols);
            const float px = Lerp(left_x, right_x, u);
            const float ndc_x = (px / cw) * 2.0f - 1.0f;
            out_verts[static_cast<size_t>(row * (kMeshCols + 1) + col)] = {
                ndc_x, ndc_y, u, v, fade_out, corner_k
            };
        }
    }
    AbsorbIntoIcon(out_verts, icon_cx_ndc, icon_cy_ndc, p);
}

bool HasNonBlackPixels(const std::vector<uint32_t>& px, int w, int h) noexcept {
    if (px.empty() || w < 4 || h < 4) return false;
    int non_zero = 0;
    for (int gy = 1; gy <= 5; ++gy) {
        for (int gx = 1; gx <= 5; ++gx) {
            const int x = (w * gx) / 6;
            const int y = (h * gy) / 6;
            const uint32_t c = px[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
            if ((c & 0x00FFFFFFu) != 0) ++non_zero;
        }
    }
    return non_zero >= 2;
}

void DownscaleSnapshotIfNeeded(DockWindowSnapshot& snap, int max_long_edge) {
    const int long_edge = std::max(snap.width, snap.height);
    if (long_edge <= max_long_edge || max_long_edge < 32) return;

    const double scale = static_cast<double>(max_long_edge) / static_cast<double>(long_edge);
    const int nw = std::max(4, static_cast<int>(std::lround(snap.width * scale)));
    const int nh = std::max(4, static_cast<int>(std::lround(snap.height * scale)));
    std::vector<uint32_t> dst(static_cast<size_t>(nw) * static_cast<size_t>(nh));

    for (int y = 0; y < nh; ++y) {
        const float sy = (static_cast<float>(y) + 0.5f) * static_cast<float>(snap.height) / static_cast<float>(nh) - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, snap.height - 1);
        const int y1 = std::min(snap.height - 1, y0 + 1);
        const float fy = Clamp01(sy - static_cast<float>(y0));
        for (int x = 0; x < nw; ++x) {
            const float sx = (static_cast<float>(x) + 0.5f) * static_cast<float>(snap.width) / static_cast<float>(nw) - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, snap.width - 1);
            const int x1 = std::min(snap.width - 1, x0 + 1);
            const float fx = Clamp01(sx - static_cast<float>(x0));

            auto sample = [&](int xx, int yy) noexcept {
                return snap.pixels[static_cast<size_t>(yy) * static_cast<size_t>(snap.width) + static_cast<size_t>(xx)];
            };
            const uint32_t c00 = sample(x0, y0), c10 = sample(x1, y0);
            const uint32_t c01 = sample(x0, y1), c11 = sample(x1, y1);
            uint32_t out = 0;
            for (int shift = 0; shift < 32; shift += 8) {
                const float v00 = static_cast<float>((c00 >> shift) & 0xFFu);
                const float v10 = static_cast<float>((c10 >> shift) & 0xFFu);
                const float v01 = static_cast<float>((c01 >> shift) & 0xFFu);
                const float v11 = static_cast<float>((c11 >> shift) & 0xFFu);
                const float top = v00 + (v10 - v00) * fx;
                const float bot = v01 + (v11 - v01) * fx;
                const uint32_t v = static_cast<uint32_t>(std::clamp<int>(
                    static_cast<int>(std::lround(top + (bot - top) * fy)), 0, 255));
                out |= (v << shift);
            }
            dst[static_cast<size_t>(y) * static_cast<size_t>(nw) + static_cast<size_t>(x)] = out;
        }
    }
    snap.width = nw;
    snap.height = nh;
    snap.pixels = std::move(dst);
}

double NowGenieMs() noexcept {
    static const LARGE_INTEGER freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return freq.QuadPart > 0
        ? (static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart))
        : static_cast<double>(GetTickCount64());
}

bool IsWindowUnobstructedOnScreen(HWND target, const RECT& ext, HWND ignore_above_hwnd) noexcept {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (ext.left < vx || ext.top < vy || ext.right > vx + vw || ext.bottom > vy + vh) {
        return false;
    }

    const DWORD self_pid = GetCurrentProcessId();
    for (HWND w = GetWindow(target, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
        if (w == ignore_above_hwnd || !IsWindowVisible(w) || IsIconic(w)) continue;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) continue;

        DWORD wpid = 0;
        GetWindowThreadProcessId(w, &wpid);
        if (wpid == self_pid) continue;

        const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
        if (ex & WS_EX_LAYERED) {
            BYTE alpha = 255;
            DWORD flags = 0;
            if (GetLayeredWindowAttributes(w, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) && alpha == 0) {
                continue;
            }
        }

        RECT wr{};
        if (!GetWindowRect(w, &wr)) continue;
        RECT wext = wr;
        DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &wext, sizeof(wext));
        if (wext.right <= wext.left || wext.bottom <= wext.top) continue;

        RECT inter{};
        if (IntersectRect(&inter, &ext, &wext)) {
            if ((inter.right - inter.left) > 12 && (inter.bottom - inter.top) > 12) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

bool CaptureWindowSnapshotForDock(HWND hwnd,
                                  DockWindowSnapshot& out_snap,
                                  int max_long_edge,
                                  WindowCaptureMode mode,
                                  HWND ignore_above_hwnd) {
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd) || !IsWindowVisible(hwnd)) return false;

    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) return false;
    RECT ext = wr;
    DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &ext, sizeof(ext));

    const int full_w = static_cast<int>(wr.right - wr.left);
    const int full_h = static_cast<int>(wr.bottom - wr.top);
    const int vis_w = static_cast<int>(ext.right - ext.left);
    const int vis_h = static_cast<int>(ext.bottom - ext.top);
    if (full_w < 8 || full_h < 8 || vis_w < 8 || vis_h < 8) return false;
    if (full_w > 8192 || full_h > 8192) return false;

    const bool unobstructed = IsWindowUnobstructedOnScreen(hwnd, ext, ignore_above_hwnd);
    if (mode == WindowCaptureMode::PassiveScreenOnly && !unobstructed) {
        return false;
    }

    auto capture_passive_screen = [&]() -> bool {
        HDC scr_dc = GetDC(nullptr);
        if (!scr_dc) return false;
        HDC mem_dc = CreateCompatibleDC(scr_dc);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), vis_w, -vis_h, 1, 32, BI_RGB};
        void* bits = nullptr;
        HBITMAP dib = mem_dc ? CreateDIBSection(mem_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (!dib || !bits) {
            if (dib) DeleteObject(dib);
            if (mem_dc) DeleteDC(mem_dc);
            ReleaseDC(nullptr, scr_dc);
            return false;
        }
        HGDIOBJ old_obj = SelectObject(mem_dc, dib);
        const BOOL ok = BitBlt(mem_dc, 0, 0, vis_w, vis_h, scr_dc, ext.left, ext.top, SRCCOPY);
        bool valid = false;
        if (ok) {
            out_snap.width = vis_w;
            out_snap.height = vis_h;
            out_snap.screen_rect = ext;
            out_snap.pixels.resize(static_cast<size_t>(vis_w) * static_cast<size_t>(vis_h));
            const auto* src = static_cast<const uint32_t*>(bits);
            for (size_t i = 0; i < out_snap.pixels.size(); ++i) {
                out_snap.pixels[i] = src[i] | 0xFF000000u;
            }
            valid = HasNonBlackPixels(out_snap.pixels, vis_w, vis_h);
            if (!valid) out_snap = {};
        }
        SelectObject(mem_dc, old_obj);
        DeleteObject(dib);
        DeleteDC(mem_dc);
        ReleaseDC(nullptr, scr_dc);
        if (valid) DownscaleSnapshotIfNeeded(out_snap, max_long_edge);
        return valid;
    };

    if (unobstructed && capture_passive_screen()) {
        return true;
    }
    if (mode == WindowCaptureMode::PassiveScreenOnly) {
        return false;
    }

    // Never send synchronous WM_PRINT / PrintWindow to an unresponsive window (prevents UI deadlock).
    if (IsHungAppWindow(hwnd)) {
        return false;
    }

    // Fallback when target is occluded behind another window (only during explicit minimize requests).
    HDC scr_dc = GetDC(nullptr);
    HDC mem_dc = CreateCompatibleDC(scr_dc);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), full_w, -full_h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        if (mem_dc) DeleteDC(mem_dc);
        if (scr_dc) ReleaseDC(nullptr, scr_dc);
        return false;
    }

    HGDIOBJ old_obj = SelectObject(mem_dc, dib);
    std::memset(bits, 0, static_cast<size_t>(full_w) * static_cast<size_t>(full_h) * 4);

    constexpr UINT kPwRenderFullContent = 0x00000002u;
    const BOOL ok = PrintWindow(hwnd, mem_dc, kPwRenderFullContent);
    std::vector<uint32_t> raw(static_cast<size_t>(full_w) * static_cast<size_t>(full_h));
    std::memcpy(raw.data(), bits, raw.size() * 4);

    SelectObject(mem_dc, old_obj);
    DeleteObject(dib);
    DeleteDC(mem_dc);
    ReleaseDC(nullptr, scr_dc);

    if (!ok || !HasNonBlackPixels(raw, full_w, full_h)) {
        return false;
    }

    const int off_x = std::clamp(static_cast<int>(ext.left - wr.left), 0, full_w - 1);
    const int off_y = std::clamp(static_cast<int>(ext.top - wr.top), 0, full_h - 1);
    const int cw = std::min(vis_w, full_w - off_x);
    const int ch = std::min(vis_h, full_h - off_y);
    if (cw < 8 || ch < 8) return false;

    out_snap.width = cw;
    out_snap.height = ch;
    out_snap.screen_rect = ext;
    out_snap.pixels.resize(static_cast<size_t>(cw) * static_cast<size_t>(ch));

    for (int y = 0; y < ch; ++y) {
        const uint32_t* src_row = raw.data() + static_cast<size_t>(off_y + y) * static_cast<size_t>(full_w) + off_x;
        uint32_t* dst_row = out_snap.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(cw);
        for (int x = 0; x < cw; ++x) {
            dst_row[x] = src_row[x] | 0xFF000000u;
        }
    }

    if (!HasNonBlackPixels(out_snap.pixels, out_snap.width, out_snap.height)) {
        out_snap = {};
        return false;
    }

    DownscaleSnapshotIfNeeded(out_snap, max_long_edge);
    return true;
}

struct DockGenieAnimator::Impl {
    HINSTANCE hinst = nullptr;
    HWND overlay_hwnd = nullptr;

    ComPtr<ID3D11Device> d3d_dev;
    ComPtr<ID3D11DeviceContext> d3d_ctx;
    ComPtr<IDXGIFactory2> dxgi_fac;
    ComPtr<IDCompositionDevice> dcomp_dev;
    ComPtr<IDCompositionTarget> dcomp_target;
    ComPtr<IDCompositionVisual> dcomp_visual;

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> rast;
    ComPtr<ID3D11Buffer> vb;
    ComPtr<ID3D11Buffer> ib;
    UINT index_count = 0;

    // Transient per-animation GPU buffers (freed immediately on finish -> 0 MB idle VRAM)
    ComPtr<IDXGISwapChain1> swap_chain;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> snap_srv;
    int swap_w = 0;
    int swap_h = 0;

    bool active = false;
    bool reverse = false;
    bool settling_restore = false;
    bool watchdog_armed = false;
    double settle_start_ms = 0.0;
    bool os_anim_suppressed = false;
    BOOL saved_os_min_anim = FALSE;
    DockMinimizeEffect style = DockMinimizeEffect::Genie;
    uint8_t dock_edge = 0;
    RECT canvas_rect{};
    RECT win_rect{};
    RECT dock_rect{};
    double start_ms = 0.0;
    double duration_ms = 420.0;
    std::function<void()> on_complete;

    static constexpr double kRestoreSettleMs = 120.0;
    static constexpr UINT_PTR kTimerGenieWatchdog = 0x5D77;

    static LRESULT CALLBACK OverlayWndProc(HWND h, UINT m, WPARAM w, LPARAM l) noexcept {
        if (m == WM_NCCREATE) {
            auto* cs = reinterpret_cast<const CREATESTRUCTW*>(l);
            if (cs && cs->lpCreateParams) {
                SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            }
            return DefWindowProcW(h, m, w, l);
        }
        if (m == WM_TIMER && w == kTimerGenieWatchdog) {
            auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
            if (self) {
                self->TickAnim(NowGenieMs());
            } else {
                KillTimer(h, kTimerGenieWatchdog);
                ShowWindow(h, SW_HIDE);
            }
            return 0;
        }
        if (m == WM_NCHITTEST) return HTTRANSPARENT;
        if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        return DefWindowProcW(h, m, w, l);
    }

    // Temporarily suppresses Windows' built-in DWM minimize/restore fly-in transition ONLY during
    // our Genie restore handoff (in-memory only, never persisted to registry), and always
    // restores the user's original ANIMATIONINFO setting immediately after handoff/cancel/shutdown.
    void SuppressOsMinAnimate() noexcept {
        if (os_anim_suppressed) return;
        ANIMATIONINFO ai{sizeof(ai)};
        if (SystemParametersInfoW(SPI_GETANIMATION, sizeof(ai), &ai, 0) && ai.iMinAnimate) {
            saved_os_min_anim = ai.iMinAnimate;
            ai.iMinAnimate = FALSE;
            if (SystemParametersInfoW(SPI_SETANIMATION, sizeof(ai), &ai, 0)) {
                os_anim_suppressed = true;
            }
        }
    }

    void RestoreOsMinAnimate() noexcept {
        if (!os_anim_suppressed) return;
        ANIMATIONINFO ai{sizeof(ai)};
        ai.iMinAnimate = saved_os_min_anim;
        SystemParametersInfoW(SPI_SETANIMATION, sizeof(ai), &ai, 0);
        os_anim_suppressed = false;
    }

    void DisarmWatchdog() noexcept {
        if (watchdog_armed && overlay_hwnd && IsWindow(overlay_hwnd)) {
            KillTimer(overlay_hwnd, kTimerGenieWatchdog);
        }
        watchdog_armed = false;
    }

    ~Impl() { Shutdown(); }

    void ReleaseTransient() noexcept {
        DisarmWatchdog();
        if (dcomp_visual) {
            dcomp_visual->SetContent(nullptr);
            D2D_MATRIX_3X2_F ident{};
            ident._11 = 1.0f;
            ident._22 = 1.0f;
            dcomp_visual->SetTransform(ident);
            if (dcomp_dev) dcomp_dev->Commit();
        }
        rtv.Reset();
        swap_chain.Reset();
        snap_srv.Reset();
        swap_w = 0;
        swap_h = 0;
        if (overlay_hwnd && IsWindow(overlay_hwnd)) {
            ShowWindow(overlay_hwnd, SW_HIDE);
        }
    }

    void ForceCompleteAnim() noexcept {
        if (!active) {
            ReleaseTransient();
            RestoreOsMinAnimate();
            return;
        }
        auto cb = std::move(on_complete);
        active = false;
        settling_restore = false;
        ReleaseTransient();
        RestoreOsMinAnimate();
        if (cb) cb();
    }

    bool InitD3D() {
        if (d3d_dev) return true;

        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                       nullptr, 0, D3D11_SDK_VERSION, &d3d_dev, &fl, &d3d_ctx);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                   nullptr, 0, D3D11_SDK_VERSION, &d3d_dev, &fl, &d3d_ctx);
        }
        if (FAILED(hr) || !d3d_dev || !d3d_ctx) return false;

        ComPtr<IDXGIDevice> dxgi_dev;
        if (SUCCEEDED(d3d_dev.As(&dxgi_dev)) && dxgi_dev) {
            ComPtr<IDXGIAdapter> ad;
            if (SUCCEEDED(dxgi_dev->GetAdapter(&ad)) && ad) {
                ad->GetParent(IID_PPV_ARGS(&dxgi_fac));
            }
            DCompositionCreateDevice(dxgi_dev.Get(), IID_PPV_ARGS(&dcomp_dev));
        }
        if (!dxgi_fac) return false;

        ComPtr<ID3DBlob> vs_blob;
        ComPtr<ID3DBlob> ps_blob;
        if (FAILED(D3DCompile(kGenieShaderSrc, std::strlen(kGenieShaderSrc), nullptr, nullptr, nullptr,
                              "VSMain", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs_blob, nullptr))) {
            return false;
        }
        if (FAILED(D3DCompile(kGenieShaderSrc, std::strlen(kGenieShaderSrc), nullptr, nullptr, nullptr,
                              "PSMain", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps_blob, nullptr))) {
            return false;
        }

        d3d_dev->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs);
        d3d_dev->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &ps);

        constexpr D3D11_INPUT_ELEMENT_DESC kElems[4] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        d3d_dev->CreateInputLayout(kElems, 4, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &layout);
        if (!vs || !ps || !layout) return false;

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = 8;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MinLOD = 0.0f;
        sd.MaxLOD = D3D11_FLOAT32_MAX; // a zeroed MaxLOD pinned sampling to mip 0 (shimmer when small)
        d3d_dev->CreateSamplerState(&sd, &sampler);

        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        d3d_dev->CreateBlendState(&bd, &blend);

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE;
        d3d_dev->CreateRasterizerState(&rd, &rast);

        const UINT vert_count = static_cast<UINT>((kMeshRows + 1) * (kMeshCols + 1));
        D3D11_BUFFER_DESC vbd{};
        vbd.ByteWidth = vert_count * sizeof(GenieVertex);
        vbd.Usage = D3D11_USAGE_DYNAMIC;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        d3d_dev->CreateBuffer(&vbd, nullptr, &vb);

        std::vector<uint16_t> indices;
        indices.reserve(static_cast<size_t>(kMeshRows * kMeshCols * 6));
        for (int r = 0; r < kMeshRows; ++r) {
            for (int c = 0; c < kMeshCols; ++c) {
                const uint16_t i0 = static_cast<uint16_t>(r * (kMeshCols + 1) + c);
                const uint16_t i1 = static_cast<uint16_t>(i0 + 1);
                const uint16_t i2 = static_cast<uint16_t>(i0 + (kMeshCols + 1));
                const uint16_t i3 = static_cast<uint16_t>(i2 + 1);
                indices.push_back(i0); indices.push_back(i1); indices.push_back(i2);
                indices.push_back(i1); indices.push_back(i3); indices.push_back(i2);
            }
        }
        index_count = static_cast<UINT>(indices.size());
        D3D11_BUFFER_DESC ibd{};
        ibd.ByteWidth = index_count * sizeof(uint16_t);
        ibd.Usage = D3D11_USAGE_IMMUTABLE;
        ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        const D3D11_SUBRESOURCE_DATA isd{indices.data(), 0, 0};
        d3d_dev->CreateBuffer(&ibd, &isd, &ib);

        return vb && ib;
    }

    bool InitOverlayWindow(HINSTANCE hi) {
        hinst = hi;
        if (!InitD3D() || !dcomp_dev) return false;
        if (overlay_hwnd && IsWindow(overlay_hwnd)) return true;

        constexpr const wchar_t* kCls = L"PulseDockGenieOverlay";
        WNDCLASSEXW wc{sizeof(wc)};
        if (!GetClassInfoExW(hinst, kCls, &wc)) {
            wc = {sizeof(wc)};
            wc.lpfnWndProc = &Impl::OverlayWndProc;
            wc.hInstance = hinst;
            wc.lpszClassName = kCls;
            RegisterClassExW(&wc);
        }

        overlay_hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
            kCls, L"", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, hinst, this);
        if (!overlay_hwnd) return false;

        if (FAILED(dcomp_dev->CreateTargetForHwnd(overlay_hwnd, TRUE, &dcomp_target)) ||
            FAILED(dcomp_dev->CreateVisual(&dcomp_visual))) {
            return false;
        }
        dcomp_target->SetRoot(dcomp_visual.Get());
        dcomp_dev->Commit();
        return true;
    }

    ComPtr<ID3D11ShaderResourceView> UploadSnapshotTexture(const DockWindowSnapshot& snap) {
        if (!d3d_dev || !snap.Valid()) return {};
        // A full mip chain: the window shrinks to a ~48 px icon, and sampling a 1400 px snapshot at
        // that size without mips aliases into sparkle and crawling text on the way into the Dock.
        if (d3d_ctx) {
            D3D11_TEXTURE2D_DESC md{};
            md.Width = static_cast<UINT>(snap.width);
            md.Height = static_cast<UINT>(snap.height);
            md.MipLevels = 0;
            md.ArraySize = 1;
            md.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            md.SampleDesc.Count = 1;
            md.Usage = D3D11_USAGE_DEFAULT;
            md.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            md.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            ComPtr<ID3D11Texture2D> mtex;
            ComPtr<ID3D11ShaderResourceView> msrv;
            if (SUCCEEDED(d3d_dev->CreateTexture2D(&md, nullptr, &mtex)) && mtex &&
                SUCCEEDED(d3d_dev->CreateShaderResourceView(mtex.Get(), nullptr, &msrv)) && msrv) {
                d3d_ctx->UpdateSubresource(mtex.Get(), 0, nullptr, snap.pixels.data(),
                                           static_cast<UINT>(snap.width * 4), 0);
                d3d_ctx->GenerateMips(msrv.Get());
                return msrv;
            }
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(snap.width);
        td.Height = static_cast<UINT>(snap.height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA sd{snap.pixels.data(), static_cast<UINT>(snap.width * 4), 0};
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(d3d_dev->CreateTexture2D(&td, &sd, &tex)) || !tex) return {};
        ComPtr<ID3D11ShaderResourceView> srv;
        d3d_dev->CreateShaderResourceView(tex.Get(), nullptr, &srv);
        return srv;
    }

    void DrawMeshToRtv(ID3D11RenderTargetView* target_rtv,
                       int w,
                       int h,
                       const RECT& c_rect,
                       const RECT& w_rect,
                       const RECT& d_rect,
                       ID3D11ShaderResourceView* srv,
                       float progress,
                       DockMinimizeEffect st,
                       uint8_t edge = 0) {
        if (!d3d_ctx || !target_rtv || !srv) return;

        std::vector<GenieVertex> verts;
        BuildDeformedMesh(c_rect, w_rect, d_rect, progress, st, verts, edge);

        D3D11_MAPPED_SUBRESOURCE ms{};
        if (SUCCEEDED(d3d_ctx->Map(vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            std::memcpy(ms.pData, verts.data(), verts.size() * sizeof(GenieVertex));
            d3d_ctx->Unmap(vb.Get(), 0);
        }

        constexpr float kClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        d3d_ctx->OMSetRenderTargets(1, &target_rtv, nullptr);
        d3d_ctx->ClearRenderTargetView(target_rtv, kClear);

        const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        d3d_ctx->RSSetViewports(1, &vp);
        d3d_ctx->RSSetState(rast.Get());
        d3d_ctx->OMSetBlendState(blend.Get(), nullptr, 0xFFFFFFFFu);

        d3d_ctx->IASetInputLayout(layout.Get());
        const UINT stride = sizeof(GenieVertex);
        const UINT offset = 0;
        ID3D11Buffer* vb_raw = vb.Get();
        d3d_ctx->IASetVertexBuffers(0, 1, &vb_raw, &stride, &offset);
        d3d_ctx->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
        d3d_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        d3d_ctx->VSSetShader(vs.Get(), nullptr, 0);
        d3d_ctx->PSSetShader(ps.Get(), nullptr, 0);
        d3d_ctx->PSSetShaderResources(0, 1, &srv);
        ID3D11SamplerState* samp_raw = sampler.Get();
        d3d_ctx->PSSetSamplers(0, 1, &samp_raw);

        d3d_ctx->DrawIndexed(index_count, 0, 0);

        ID3D11ShaderResourceView* null_srv = nullptr;
        d3d_ctx->PSSetShaderResources(0, 1, &null_srv);
    }

    bool StartAnim(const DockWindowSnapshot& snap,
                   RECT w_rect,
                   RECT d_rect,
                   bool rev,
                   DockMinimizeEffect st,
                   std::function<void()> cb_done,
                   uint8_t edge = 0) {
        if (st == DockMinimizeEffect::Off || !overlay_hwnd || !dcomp_dev || !dcomp_visual || !dxgi_fac || !snap.Valid()) {
            return false;
        }

        if (active) {
            ForceCompleteAnim();
        }

        UnionRect(&canvas_rect, &w_rect, &d_rect);
        InflateRect(&canvas_rect, 6, 6);
        const int cw = std::clamp<int>(canvas_rect.right - canvas_rect.left, 16, 4096);
        const int ch = std::clamp<int>(canvas_rect.bottom - canvas_rect.top, 16, 2400);
        canvas_rect.right = canvas_rect.left + cw;
        canvas_rect.bottom = canvas_rect.top + ch;

        // Cap transient swapchain long edge to 1920 px (snapshots are already <= 1280 px) and scale
        // via DComp visual transform: cuts 4K transient VRAM from ~64 MB down to <= 16 MB.
        constexpr int kMaxSwapEdge = 1920;
        int sw = cw;
        int sh = ch;
        const int long_edge = std::max(cw, ch);
        if (long_edge > kMaxSwapEdge) {
            const double sc = static_cast<double>(kMaxSwapEdge) / static_cast<double>(long_edge);
            sw = std::max(16, static_cast<int>(std::lround(cw * sc)));
            sh = std::max(16, static_cast<int>(std::lround(ch * sc)));
        }

        snap_srv = UploadSnapshotTexture(snap);
        if (!snap_srv) return false;

        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = static_cast<UINT>(sw);
        scd.Height = static_cast<UINT>(sh);
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        scd.Scaling = DXGI_SCALING_STRETCH;

        if (FAILED(dxgi_fac->CreateSwapChainForComposition(d3d_dev.Get(), &scd, nullptr, &swap_chain)) || !swap_chain) {
            ReleaseTransient();
            return false;
        }

        ComPtr<ID3D11Texture2D> back_buf;
        if (SUCCEEDED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buf))) && back_buf) {
            d3d_dev->CreateRenderTargetView(back_buf.Get(), nullptr, &rtv);
        }
        if (!rtv) {
            ReleaseTransient();
            return false;
        }

        swap_w = sw;
        swap_h = sh;
        win_rect = w_rect;
        dock_rect = d_rect;
        reverse = rev;
        settling_restore = false;
        settle_start_ms = 0.0;
        style = st;
        dock_edge = edge;
        duration_ms = (st == DockMinimizeEffect::Genie) ? 470.0 : 340.0;
        start_ms = NowGenieMs();
        on_complete = std::move(cb_done);
        active = true;

        SetWindowPos(overlay_hwnd, HWND_TOPMOST, canvas_rect.left, canvas_rect.top, cw, ch,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        dcomp_visual->SetContent(swap_chain.Get());
        D2D_MATRIX_3X2_F scale_mat{};
        scale_mat._11 = static_cast<float>(cw) / static_cast<float>(sw);
        scale_mat._22 = static_cast<float>(ch) / static_cast<float>(sh);
        dcomp_visual->SetTransform(scale_mat);
        dcomp_visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
        dcomp_dev->Commit();

        // Arm an independent Win32 fallback timer directly on overlay_hwnd so the animation and its
        // completion callback ALWAYS finish cleanly even if the host VSync clock pauses or suspends.
        watchdog_armed = (SetTimer(overlay_hwnd, kTimerGenieWatchdog, 20, nullptr) != 0);

        const float p0 = reverse ? 0.96f : 0.0f;
        DrawMeshToRtv(rtv.Get(), swap_w, swap_h, canvas_rect, win_rect, dock_rect, snap_srv.Get(), p0, style, dock_edge);
        swap_chain->Present(0, 0);
        return true;
    }

    bool TickAnim(double now_ms) {
        if (!active) return false;
        const double mono_now = NowGenieMs();
        if (now_ms < start_ms || std::fabs(now_ms - mono_now) > 5000.0) {
            now_ms = mono_now;
        }
        if (settling_restore) {
            if ((now_ms - settle_start_ms) >= kRestoreSettleMs) {
                active = false;
                settling_restore = false;
                ReleaseTransient();
                RestoreOsMinAnimate();
                return false;
            }
            return true;
        }

        const float raw_t = Clamp01(static_cast<float>((now_ms - start_ms) / duration_ms));
        const float p = reverse ? (1.0f - raw_t) : raw_t;

        DrawMeshToRtv(rtv.Get(), swap_w, swap_h, canvas_rect, win_rect, dock_rect, snap_srv.Get(), p, style, dock_edge);
        if (swap_chain) swap_chain->Present(0, 0);

        if (raw_t >= 1.0f) {
            auto done = std::move(on_complete);
            if (reverse) {
                // Suppress OS restore fly-in strictly during the restore invocation and immediately
                // restore SPI_SETANIMATION afterward so the global OS setting can never leak.
                SuppressOsMinAnimate();
                if (done) done();
                RestoreOsMinAnimate();
                if (overlay_hwnd && IsWindow(overlay_hwnd)) {
                    SetWindowPos(overlay_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
                settling_restore = true;
                settle_start_ms = now_ms;
                return true;
            }
            active = false;
            ReleaseTransient();
            RestoreOsMinAnimate();
            if (done) done();
            return false;
        }
        return true;
    }

    void Shutdown() noexcept {
        active = false;
        settling_restore = false;
        on_complete = nullptr;
        ReleaseTransient();
        RestoreOsMinAnimate();
        dcomp_visual.Reset();
        dcomp_target.Reset();
        dcomp_dev.Reset();
        if (overlay_hwnd && IsWindow(overlay_hwnd)) {
            SetWindowLongPtrW(overlay_hwnd, GWLP_USERDATA, 0);
            DestroyWindow(overlay_hwnd);
            overlay_hwnd = nullptr;
        }
        ib.Reset();
        vb.Reset();
        rast.Reset();
        blend.Reset();
        sampler.Reset();
        layout.Reset();
        ps.Reset();
        vs.Reset();
        dxgi_fac.Reset();
        d3d_ctx.Reset();
        d3d_dev.Reset();
    }
};

DockGenieAnimator::DockGenieAnimator() : impl_(std::make_unique<Impl>()) {}
DockGenieAnimator::~DockGenieAnimator() = default;

bool DockGenieAnimator::Initialize(HINSTANCE hinst) { return impl_->InitOverlayWindow(hinst); }
void DockGenieAnimator::Shutdown() { impl_->Shutdown(); }

bool DockGenieAnimator::Start(const DockWindowSnapshot& snap,
                              RECT window_screen_rect,
                              RECT dock_slot_screen_rect,
                              bool reverse,
                              DockMinimizeEffect style,
                              std::function<void()> on_complete,
                              uint8_t dock_edge) {
    return impl_->StartAnim(snap, window_screen_rect, dock_slot_screen_rect, reverse, style, std::move(on_complete), dock_edge);
}

bool DockGenieAnimator::Tick(double now_ms) { return impl_->TickAnim(now_ms); }
void DockGenieAnimator::ForceComplete() noexcept { impl_->ForceCompleteAnim(); }
bool DockGenieAnimator::IsActive() const noexcept { return impl_->active; }
HWND DockGenieAnimator::OverlayHandle() const noexcept { return impl_->overlay_hwnd; }

bool DockGenieAnimator::RenderPreviewFrame(const DockWindowSnapshot& snap,
                                           int canvas_w,
                                           int canvas_h,
                                           RECT win_rect,
                                           RECT dock_rect,
                                           float progress,
                                           DockMinimizeEffect style,
                                           std::vector<uint32_t>& out_bgra,
                                           uint8_t dock_edge) {
    if (!impl_->InitD3D() || !snap.Valid() || canvas_w < 4 || canvas_h < 4) return false;
    auto srv = impl_->UploadSnapshotTexture(snap);
    if (!srv) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(canvas_w);
    td.Height = static_cast<UINT>(canvas_h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> rt_tex;
    ComPtr<ID3D11RenderTargetView> rt_view;
    if (FAILED(impl_->d3d_dev->CreateTexture2D(&td, nullptr, &rt_tex)) || !rt_tex ||
        FAILED(impl_->d3d_dev->CreateRenderTargetView(rt_tex.Get(), nullptr, &rt_view)) || !rt_view) {
        return false;
    }

    const RECT c_rect{0, 0, canvas_w, canvas_h};
    impl_->DrawMeshToRtv(rt_view.Get(), canvas_w, canvas_h, c_rect, win_rect, dock_rect, srv.Get(), progress, style, dock_edge);

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(impl_->d3d_dev->CreateTexture2D(&td, nullptr, &staging)) || !staging) return false;

    impl_->d3d_ctx->CopyResource(staging.Get(), rt_tex.Get());
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(impl_->d3d_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &ms))) return false;

    out_bgra.resize(static_cast<size_t>(canvas_w) * static_cast<size_t>(canvas_h));
    for (int y = 0; y < canvas_h; ++y) {
        std::memcpy(out_bgra.data() + static_cast<size_t>(y) * static_cast<size_t>(canvas_w),
                    static_cast<const uint8_t*>(ms.pData) + static_cast<size_t>(y) * ms.RowPitch,
                    static_cast<size_t>(canvas_w) * 4);
    }
    impl_->d3d_ctx->Unmap(staging.Get(), 0);
    return true;
}

} // namespace pulse::desktop::render

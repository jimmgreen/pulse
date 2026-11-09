// drag_stack.cpp - The cards that follow the pointer during a file drag.
//
// Up to three cards (one per dragged item, front to back) ride just below
// and right of the pointer. Each card follows the one in front of it on a
// progressively softer spring, so a quick swing fans the stack out into a
// short trail and a pause gathers it again; the cards tilt with their own
// horizontal speed. A badge counts the items and a small glyph shows what
// the current target will do (copy / link / refused; move needs none).
// A cancelled or refused drag flies the stack back to where it was picked
// up; a drop gulps it into the pointer - or, when it went into a folder a
// Pulse list shows, waits there and lands each card on its new row.
//
// The visual is a click-through, layered top-level window, so OLE hit
// testing and every drop target (Pulse or Explorer) behave exactly as
// before. It is driven from IDropSource callbacks inside the modal
// DoDragDrop loop plus a timer while springs settle, and stops redrawing
// once everything is at rest. With system animations off the cards snap
// to the pointer and vanish on release.
#include "drag_drop.h"
#include "drag_layout.h"
#include "drag_feedback.h"
#include "../common/localization.h"
#include "ui_motion.h"
#include "command_icons.h"

#include <windows.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellapi.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowscodecs.lib")

#ifdef PULSE_DRAGSTACK_TEST
// Offline render harness hooks (not compiled into Pulse.exe).
void DragStackTestFrame(HBITMAP dib, int w, int h, POINT pos, float alpha);
#endif

namespace pulse::ui {
namespace {

template <class T>
class Com {
public:
    Com() = default;
    ~Com() { Reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Com& operator=(Com&& o) noexcept {
        if (this != &o) {
            Reset();
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }
    T* operator->() const { return p_; }
    T* Get() const { return p_; }
    T** Put() {
        Reset();
        return &p_;
    }
    void Reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }

private:
    T* p_ = nullptr;
};

constexpr wchar_t kClassName[] = L"PulseDragStack";
// Steps come from a pool timer posting kStepMessage rather than WM_TIMER:
// WM_TIMER is only generated when the queue is empty, and after the drop the
// main window's frame pump keeps it busy (rows fading in, the list shifting),
// so the cards would freeze until the list settled.
constexpr UINT kStepMessage = WM_APP + 0x4D;
constexpr int kMaxCards = 3;

// Geometry in DIPs.
constexpr float kCardH = 30.0f;
constexpr float kCardMinW = 116.0f;
constexpr float kCardMaxW = 200.0f;
constexpr float kRadius = 6.0f;
constexpr float kPadX = 10.0f;
constexpr float kIcon = 16.0f;
constexpr float kGap = 8.0f;
constexpr float kOffX = 14.0f; // lead card's top-left relative to the pointer
constexpr float kOffY = 12.0f;
constexpr float kWinW = 1024.0f; // layered window; the lead card sits at the anchor
constexpr float kWinH = 196.0f;
constexpr float kAnchorX = 96.0f;
constexpr float kAnchorY = 60.0f;

// Resting fan of the cards behind the lead: offset (DIP) and tilt (deg).
constexpr float kFanDx[kMaxCards] = {0.0f, 5.0f, 10.0f};
constexpr float kFanDy[kMaxCards] = {0.0f, 5.0f, 10.0f};
constexpr float kFanRot[kMaxCards] = {0.0f, 0.0f, 0.0f};

// Follow springs, lead first. The lead is stiff so it feels attached to the
// pointer; the ones behind lag more and more.
constexpr motion::SpringSpec kFollow[kMaxCards] = {
    {1100.0f, 62.0f}, {520.0f, 34.0f}, {340.0f, 28.0f}};

double NowMs() {
    static const double freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / freq;
}

float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

D2D1_COLOR_F Rgba(UINT32 rgb, float a) {
    return D2D1::ColorF(rgb, a);
}

float Luminance(COLORREF c) {
    return (0.2126f * GetRValue(c) + 0.7152f * GetGValue(c) + 0.0722f * GetBValue(c)) / 255.0f;
}

std::wstring LeafName(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash + 1 >= p.size()) return p;
    return p.substr(slash + 1);
}

bool FontFamilyExists(IDWriteFactory* dw, const wchar_t* family) {
    Com<IDWriteFontCollection> fonts;
    if (FAILED(dw->GetSystemFontCollection(fonts.Put(), FALSE)) || !fonts.Get()) return false;
    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists;
}

enum class Phase { Live, FlyBack, Gulp, Await, Land };
constexpr float kAwaitMs = 700.0f;    // no rows by then: gulp after all
constexpr float kLandStaggerMs = 30.0f;
// The cards fade out once they rest on their rows (capped, for a far trip).
constexpr float kLandFadeAtMs = 420.0f, kLandFadeMs = 110.0f;
// Pop's stiffness, damped close to critical: a 400 px trip with Pop's own
// damping overshoots by ~40 px and would fade out mid-bounce.
constexpr motion::SpringSpec kLandSpring{620.0f, 46.0f};

struct Card {
    std::wstring name;
    Com<IDWriteTextLayout> layout;
    Com<ID2D1Bitmap> icon;
    float w = kCardMinW; // px
    float x = 0, y = 0;   // top-left, screen px
    float vx = 0, vy = 0; // px/s
    float rot = 0;        // deg
};

class DragStack final : public DragVisual {
public:
    static DragStack* Create(const std::vector<std::wstring>& paths, bool dark);
#ifdef PULSE_WITH_TEST_HOOKS
    static bool SaveReference(const std::vector<std::wstring>& paths, bool dark, DWORD effect,
                              const std::wstring& text, const wchar_t* png, float dpi_scale);
#endif

    void Track() override { Step(false); }

    void Feedback(const std::wstring& text) override {
        if (feedback_ == text) return;
        feedback_ = text;
        Step(true);
    }

    void Effect(DWORD effect) override {
        if (effect == effect_) return;
        effect_ = effect;
        Step(true);
    }

    void Finish(DWORD effect, bool cancelled) override {
        effect_ = effect;
        const bool land = TakeDragLandingExpectation() && !cancelled && effect != DROPEFFECT_NONE;
        if (!hwnd_ || !animate_) {
            Close();
            return;
        }
        phase_ = cancelled ? Phase::FlyBack : land ? Phase::Await : Phase::Gulp;
        if (phase_ == Phase::Await) SetDragLandingWindow(hwnd_);
        finish_ms_ = NowMs();
        GetCursorPos(&drop_pt_);
        Arm();
        Step(true);
    }

    // The new rows are on screen (kDragLandMessage): fly the cards there.
    void BeginLand(std::vector<RECT> rects) {
        if (phase_ != Phase::Await || rects.empty()) return;
        land_rects_ = std::move(rects);
        phase_ = Phase::Land;
        land_ms_ = NowMs();
        rest_ms_ = 0.0;
        Arm();
        Step(true);
    }

private:
    DragStack() = default;
    ~DragStack() { ReleaseSurface(); }

    bool Init(const std::vector<std::wstring>& paths, bool dark);
    bool InitSurface();
    void ReleaseSurface();
    void LoadCard(Card& card, const std::wstring& path, float forced_w);
    void Step(bool force);
    void Integrate(float dt, float tx, float ty);
    void Render(float scale, float alpha);
    float FeedbackWidth();
    void DrawFeedback(float scale, D2D1_POINT_2F pivot);
    void DrawCard(const Card& card, bool lead, float dx, float dy, float scale,
                  D2D1_POINT_2F pivot);
    void Arm() {
        if (hwnd_ && !timer_) {
            tick_hwnd_ = hwnd_;
            tick_pending_ = false;
            timer_ = CreateTimerQueueTimer(&tick_, nullptr, &DragStack::OnTick, this, 8, 8,
                                           WT_EXECUTEDEFAULT) != FALSE;
        }
    }
    void Disarm() {
        if (timer_) {
            // Waits for a callback in flight, so `this` outlives every tick.
            DeleteTimerQueueTimer(nullptr, tick_, INVALID_HANDLE_VALUE);
            tick_ = nullptr;
            timer_ = false;
        }
    }
    static VOID CALLBACK OnTick(PVOID param, BOOLEAN) {
        auto* self = static_cast<DragStack*>(param);
        // One step message in the queue at a time; a busy thread just skips.
        if (!self->tick_pending_.exchange(true)) PostMessageW(self->tick_hwnd_, kStepMessage, 0, 0);
    }
    void Close() {
        if (hwnd_) {
            DestroyWindow(hwnd_); // WM_NCDESTROY deletes this
        } else {
            delete this;
        }
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    bool snapshot_ = false;
    float snapshot_scale_ = 1.0f;
    bool timer_ = false;
    HANDLE tick_ = nullptr;
    HWND tick_hwnd_ = nullptr;
    std::atomic<bool> tick_pending_{false};
    bool shown_ = false;
    bool animate_ = true;
    bool dark_ = false;
    float s_ = 1.0f; // DPI scale
    int win_w_ = 0, win_h_ = 0;
    size_t count_ = 0;
    std::vector<Card> cards_;
    DWORD effect_ = DROPEFFECT_MOVE;
    std::wstring feedback_;
    Phase phase_ = Phase::Live;
    POINT origin_{};  // pointer when the drag began
    POINT drop_pt_{}; // pointer when it ended
    std::vector<RECT> land_rects_;  // screen px, one per new row
    double land_ms_ = 0;
    double rest_ms_ = 0;  // when the lead card settled on its row
    POINT last_pt_{};
    double start_ms_ = 0, last_ms_ = 0, finish_ms_ = 0;
    float alpha_ = 1.0f;  // smoothed refused-drop dimming
    D2D1_COLOR_F accent_{};
    bool accent_light_ = false;

    HDC mem_dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ old_bmp_ = nullptr;
    Com<ID2D1Factory> d2d_;
    Com<ID2D1DCRenderTarget> rt_;
    Com<IDWriteFactory> dw_;
    Com<IDWriteTextFormat> name_fmt_;
    Com<IDWriteTextFormat> badge_fmt_;
    Com<IDWriteTextFormat> feedback_fmt_;
    Com<IDWriteTextFormat> glyph_fmt_;
    Com<IDWriteInlineObject> ellipsis_;
    Com<ID2D1SolidColorBrush> brush_;
    Com<IWICImagingFactory> wic_;
};

LRESULT CALLBACK DragStack::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<DragStack*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        break;
    }
    case kStepMessage:
        if (self) {
            self->tick_pending_ = false;
            self->Step(false);
        }
        return 0;
    case kDragLandMessage:
        if (self) self->BeginLand(TakeDragLandingRects());
        return 0;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCDESTROY:
        if (self) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->Disarm();
            if (self->phase_ == Phase::Await) SetDragLandingWindow(nullptr);
            self->hwnd_ = nullptr;
            delete self;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

DragStack* DragStack::Create(const std::vector<std::wstring>& paths, bool dark) {
    if (paths.empty()) return nullptr;
    auto* stack = new DragStack();
    if (!stack->Init(paths, dark)) {
        stack->Close();
        return nullptr;
    }
    return stack;
}

bool DragStack::Init(const std::vector<std::wstring>& paths, bool dark) {
    animate_ = motion::SystemAnimationsEnabled();
    count_ = paths.size();
    GetCursorPos(&origin_);
    last_pt_ = origin_;

    // GetDpiForWindow is looked up at run time: the build targets an SDK
    // baseline that predates it.
    UINT dpi = 0;
    using DpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto dpi_for_window = reinterpret_cast<DpiForWindowFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    if (HWND fg = GetForegroundWindow(); fg && dpi_for_window) dpi = dpi_for_window(fg);
    if (dpi == 0) {
        if (HDC screen = GetDC(nullptr)) {
            dpi = static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSY));
            ReleaseDC(nullptr, screen);
        }
    }
    if (dpi == 0) dpi = 96;
    s_ = snapshot_ ? snapshot_scale_ : static_cast<float>(dpi) / 96.0f;

    dark_ = dark;
    DWORD color = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque))) {
        const COLORREF rgb = RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
        accent_ = Rgba(((color & 0xFFFFFF)), 1.0f);
        accent_light_ = Luminance(rgb) > 0.62f;
    } else {
        accent_ = Rgba(0x0067C0, 1.0f);
    }

    static const bool registered = [] {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &DragStack::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    if (!registered) return false;

    win_w_ = static_cast<int>(std::ceil(kWinW * s_));
    win_h_ = static_cast<int>(std::ceil(kWinH * s_));
    if (!InitSurface()) return false;

    const size_t n = std::min<size_t>(paths.size(), kMaxCards);
    cards_.resize(n);
    const float x0 = origin_.x + kOffX * s_;
    const float y0 = origin_.y + kOffY * s_;
    for (size_t i = 0; i < n; ++i) {
        LoadCard(cards_[i], paths[i], i == 0 ? 0.0f : cards_[0].w);
        cards_[i].x = x0; // all start under the pointer and fan out
        cards_[i].y = y0;
    }

    if (snapshot_) return true;
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                                WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                            kClassName, L"", WS_POPUP, 0, 0, win_w_, win_h_, nullptr, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;
    start_ms_ = last_ms_ = NowMs();
    Step(true);
    if (animate_) Arm();
    return true;
}

bool DragStack::InitSurface() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.Put()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dw_.Put()))))
        return false;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(wic_.Put())); // optional: icons only

    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f,
        96.0f);
    if (FAILED(d2d_->CreateDCRenderTarget(&props, rt_.Put()))) return false;
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (FAILED(rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0.0f), brush_.Put()))) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = win_w_;
    bi.bmiHeader.biHeight = -win_h_;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    mem_dc_ = CreateCompatibleDC(nullptr);
    if (!mem_dc_) return false;
    dib_ = CreateDIBSection(mem_dc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib_) return false;
    old_bmp_ = SelectObject(mem_dc_, dib_);
    const RECT rc{0, 0, win_w_, win_h_};
    if (FAILED(rt_->BindDC(mem_dc_, &rc))) return false;

    const wchar_t* ui_font = L"Segoe UI";
    if (FAILED(dw_->CreateTextFormat(ui_font, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                     12.5f * s_, L"", name_fmt_.Put())))
        return false;
    name_fmt_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    name_fmt_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (FAILED(dw_->CreateTextFormat(ui_font, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    12.0f * s_, L"", feedback_fmt_.Put())))
        return false;
    feedback_fmt_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    feedback_fmt_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (SUCCEEDED(dw_->CreateEllipsisTrimmingSign(name_fmt_.Get(), ellipsis_.Put()))) {
        const DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        name_fmt_->SetTrimming(&trim, ellipsis_.Get());
        feedback_fmt_->SetTrimming(&trim, ellipsis_.Get());
    }
    if (SUCCEEDED(dw_->CreateTextFormat(ui_font, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        11.0f * s_, L"", badge_fmt_.Put()))) {
        badge_fmt_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        badge_fmt_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        badge_fmt_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    const wchar_t* glyph_font = FontFamilyExists(dw_.Get(), L"Segoe Fluent Icons")
                                    ? L"Segoe Fluent Icons"
                                    : L"Segoe MDL2 Assets";
    if (SUCCEEDED(dw_->CreateTextFormat(glyph_font, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        9.0f * s_, L"", glyph_fmt_.Put()))) {
        glyph_fmt_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        glyph_fmt_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    return true;
}

void DragStack::ReleaseSurface() {
    for (auto& c : cards_) {
        c.icon.Reset();
        c.layout.Reset();
    }
    brush_.Reset();
    rt_.Reset();
    if (mem_dc_) {
        if (old_bmp_) SelectObject(mem_dc_, old_bmp_);
        DeleteDC(mem_dc_);
        mem_dc_ = nullptr;
    }
    if (dib_) {
        DeleteObject(dib_);
        dib_ = nullptr;
    }
}

void DragStack::LoadCard(Card& card, const std::wstring& path, float forced_w) {
    card.name = LeafName(path);
    // Cards behind the lead take its width so the resting stack reads as one
    // pile with edges peeking out, not a row of different-length labels.
    const float text_max = forced_w > 0.0f ? forced_w - (kPadX * 2 + kIcon + kGap + 2.0f) * s_
                                           : (kCardMaxW - kPadX * 2 - kIcon - kGap) * s_;
    float text_w = 0.0f;
    if (name_fmt_.Get() &&
        SUCCEEDED(dw_->CreateTextLayout(card.name.c_str(), static_cast<UINT32>(card.name.size()),
                                        name_fmt_.Get(), text_max, kCardH * s_,
                                        card.layout.Put()))) {
        DWRITE_TEXT_METRICS m{};
        if (SUCCEEDED(card.layout->GetMetrics(&m))) text_w = std::min(m.width, text_max);
    }
    card.w = forced_w > 0.0f ? forced_w
                             : std::clamp(kPadX * 2 * s_ + kIcon * s_ + kGap * s_ + text_w +
                                              2.0f * s_,
                                          kCardMinW * s_, kCardMaxW * s_);

    // Icon by attributes only: no disk or network access beyond one
    // attribute read, so a drag never waits on a slow shell extension.
    if (!wic_.Get()) return;
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) attrs = FILE_ATTRIBUTE_NORMAL;
    SHFILEINFOW sfi{};
    const UINT size_flag = s_ > 1.25f ? SHGFI_LARGEICON : SHGFI_SMALLICON;
    if (!SHGetFileInfoW(path.c_str(), attrs & FILE_ATTRIBUTE_DIRECTORY ? FILE_ATTRIBUTE_DIRECTORY
                                                                        : FILE_ATTRIBUTE_NORMAL,
                        &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_USEFILEATTRIBUTES | size_flag) ||
        !sfi.hIcon)
        return;
    Com<IWICBitmap> bmp;
    Com<IWICFormatConverter> conv;
    if (SUCCEEDED(wic_->CreateBitmapFromHICON(sfi.hIcon, bmp.Put())) &&
        SUCCEEDED(wic_->CreateFormatConverter(conv.Put())) &&
        SUCCEEDED(conv->Initialize(bmp.Get(), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeMedianCut))) {
        rt_->CreateBitmapFromWicBitmap(conv.Get(), nullptr, card.icon.Put());
    }
    DestroyIcon(sfi.hIcon);
}

void DragStack::Integrate(float dt, float tx, float ty) {
    const bool live = phase_ == Phase::Live;
    for (size_t i = 0; i < cards_.size(); ++i) {
        Card& c = cards_[i];
        float gx = tx, gy = ty;
        motion::SpringSpec spec = kFollow[i];
        if (live) {
            if (i > 0) {
                gx = cards_[i - 1].x + (kFanDx[i] - kFanDx[i - 1]) * s_;
                gy = cards_[i - 1].y + (kFanDy[i] - kFanDy[i - 1]) * s_;
            }
        } else if (phase_ == Phase::FlyBack) {
            // Everyone heads home on the same soft spring, slightly staggered
            // by their current lag, so the stack re-gathers in flight.
            spec = motion::spring::Soft;
            gx = tx + kFanDx[i] * s_ * 0.5f;
            gy = ty + kFanDy[i] * s_ * 0.5f;
        } else if (phase_ == Phase::Await ||
                   (phase_ == Phase::Land && NowMs() - land_ms_ < i * kLandStaggerMs)) {
            // Held where it was let go, gathered a little.
            spec = {900.0f, 60.0f};
            gx = tx + kFanDx[i] * s_ * 0.5f;
            gy = ty + kFanDy[i] * s_ * 0.5f;
        } else if (phase_ == Phase::Land) {
            // Each card onto its own new row (the last row takes the rest),
            // the card's icon over the row's icon.
            const RECT& r = land_rects_[std::min(i, land_rects_.size() - 1)];
            const size_t extra = i >= land_rects_.size() ? i - land_rects_.size() + 1 : 0;
            spec = kLandSpring;
            gx = r.left - kPadX * s_ + extra * 3.0f * s_;
            gy = (r.top + r.bottom) * 0.5f - kCardH * s_ * 0.5f;
        } else {
            spec = {900.0f, 60.0f};
        }
        if (!animate_) {
            c.x = gx;
            c.y = gy;
            c.vx = c.vy = 0;
        } else {
            const float ax = spec.k * (gx - c.x) - spec.c * c.vx;
            const float ay = spec.k * (gy - c.y) - spec.c * c.vy;
            c.vx += ax * dt;
            c.vy += ay * dt;
            c.x += c.vx * dt;
            c.y += c.vy * dt;
        }
        // Tilt with horizontal speed (px/s -> DIP/s), eased so jitter
        // does not wobble the cards.
        const float tilt = std::clamp(c.vx / s_ * 0.0075f, -9.0f, 9.0f);
        const float rest = live ? kFanRot[i] : phase_ == Phase::Land ? 0.0f : kFanRot[i] * 0.5f;
        const float want = rest + (animate_ ? tilt : 0.0f);
        c.rot += (want - c.rot) * std::min(1.0f, dt * 22.0f);
    }
}

void DragStack::Step(bool force) {
    if (!hwnd_ || cards_.empty()) return;
    const double now = NowMs();
    POINT pt{};
    GetCursorPos(&pt);
    const bool moved = pt.x != last_pt_.x || pt.y != last_pt_.y;
    // The modal loop can call in at the mouse report rate; 4 ms is plenty.
    if (!force && !moved && now - last_ms_ < 4.0) return;
    float dt = static_cast<float>(std::min(now - last_ms_, 34.0)) / 1000.0f;
    const float frame_dt = dt;
    last_ms_ = now;
    last_pt_ = pt;

    float tx = 0, ty = 0;
    if (phase_ == Phase::Live) {
        tx = pt.x + kOffX * s_;
        ty = pt.y + kOffY * s_;
    } else if (phase_ == Phase::Await || phase_ == Phase::Land) {
        tx = drop_pt_.x + kOffX * s_;
        ty = drop_pt_.y + kOffY * s_;
    } else if (phase_ == Phase::FlyBack) {
        tx = origin_.x + kOffX * s_;
        ty = origin_.y + kOffY * s_;
    } else {
        tx = drop_pt_.x - cards_[0].w * 0.25f;
        ty = drop_pt_.y - kCardH * s_ * 0.25f;
    }
    // Fixed 4 ms sub-steps keep the stiff lead spring stable at any frame rate.
    while (dt > 0.0f) {
        const float h = std::min(dt, 0.004f);
        Integrate(h, tx, ty);
        dt -= h;
    }

    const float since_start = static_cast<float>(now - start_ms_);
    float scale = 1.0f;
    float alpha = 1.0f;
    if (animate_) {
        // Lift: pops from 0.92 while fading in over 90 ms.
        scale = 0.92f + 0.08f * motion::SpringProgress(motion::spring::Pop, since_start);
        alpha = Clamp01(since_start / 90.0f);
    }
    const bool refused = phase_ == Phase::Live && effect_ == DROPEFFECT_NONE;
    // After release keep whatever dimming the last target gave.
    const float dim_target = phase_ != Phase::Live ? alpha_ : (refused ? 0.55f : 1.0f);
    alpha_ = animate_ ? alpha_ + (dim_target - alpha_) * std::min(1.0f, frame_dt * 14.0f)
                      : dim_target;
    alpha *= alpha_;

    bool done = false;
    if (phase_ == Phase::FlyBack) {
        const float t = static_cast<float>(now - finish_ms_);
        alpha *= 1.0f - Clamp01((t - 170.0f) / 190.0f);
        scale *= 1.0f - 0.06f * Clamp01(t / 360.0f);
        done = t >= 380.0f;
    } else if (phase_ == Phase::Await) {
        const float t = static_cast<float>(now - finish_ms_);
        scale *= 1.0f - 0.03f * Clamp01(t / 120.0f);
        if (t >= kAwaitMs) {
            // No rows showed up in time (a slow copy, a conflict prompt).
            SetDragLandingWindow(nullptr);
            phase_ = Phase::Gulp;
            finish_ms_ = now;
            GetCursorPos(&drop_pt_);
        }
    } else if (phase_ == Phase::Land) {
        const float t = static_cast<float>(now - land_ms_);
        const float stagger = kLandStaggerMs * static_cast<float>(
            std::min(cards_.size(), land_rects_.size()) - 1);
        if (rest_ms_ == 0.0) {
            const Card& lead = cards_[0];
            const RECT& r = land_rects_[0];
            const float dx = lead.x - (r.left - kPadX * s_);
            const float dy = lead.y - ((r.top + r.bottom) * 0.5f - kCardH * s_ * 0.5f);
            if (std::fabs(dx) < 2.0f * s_ && std::fabs(dy) < 2.0f * s_ &&
                std::fabs(lead.vx) + std::fabs(lead.vy) < 120.0f * s_)
                rest_ms_ = now;
        }
        const float fade_at = std::min(kLandFadeAtMs,
                                       rest_ms_ != 0.0 ? static_cast<float>(rest_ms_ - land_ms_) : kLandFadeAtMs) +
                              stagger;
        scale *= 1.0f - 0.03f * (1.0f - Clamp01(t / 120.0f));  // un-gather from Await
        alpha *= 1.0f - Clamp01((t - fade_at) / kLandFadeMs);
        done = t >= fade_at + kLandFadeMs;
    } else if (phase_ == Phase::Gulp) {
        const float t = static_cast<float>(now - finish_ms_);
        const float k = Clamp01(t / 150.0f);
        alpha *= 1.0f - k;
        scale *= 1.0f - 0.55f * k * k;
        done = t >= 160.0f;
    }
    if (done) {
        Close();
        return;
    }

    Render(scale, alpha);

    if (phase_ == Phase::Live && animate_) {
        bool settled = since_start > 400.0f && std::fabs(alpha_ - dim_target) < 0.01f;
        for (size_t i = 0; settled && i < cards_.size(); ++i) {
            const Card& c = cards_[i];
            settled = std::fabs(c.vx) < 2.0f && std::fabs(c.vy) < 2.0f &&
                      std::fabs(c.rot - kFanRot[i]) < 0.05f;
        }
        if (settled) {
            Disarm();
        } else {
            Arm();
        }
    }
}

void DragStack::DrawCard(const Card& c, bool lead, float dx, float dy, float scale,
                         D2D1_POINT_2F pivot) {
    const float title_h = kCardH * s_;
    const float h = title_h;
    const float w = c.w;
    const float r = kRadius * s_;
    const float ax = kAnchorX * s_ + dx;
    const float ay = kAnchorY * s_ + dy;
    const D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Rotation(c.rot, D2D1::Point2F(w * 0.5f, title_h * 0.5f)) *
                                D2D1::Matrix3x2F::Translation(ax, ay) *
                                D2D1::Matrix3x2F::Scale(scale, scale, pivot);
    rt_->SetTransform(m);

    // Soft shadow from two inflated, offset plates (no blur effects: the DC
    // target is software-friendly and this stays well under a millisecond).
    const float sa = dark_ ? 0.30f : 0.10f;
    brush_->SetColor(D2D1::ColorF(0, sa * 0.45f));
    rt_->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(-3 * s_, 1 * s_, w + 3 * s_, h + 6 * s_), r + 3 * s_,
                          r + 3 * s_),
        brush_.Get());
    brush_->SetColor(D2D1::ColorF(0, sa));
    rt_->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(-1 * s_, 1 * s_, w + 1 * s_, h + 2.5f * s_), r + 1 * s_,
                          r + 1 * s_),
        brush_.Get());

    const D2D1_ROUNDED_RECT body = D2D1::RoundedRect(D2D1::RectF(0, 0, w, h), r, r);
    brush_->SetColor(dark_ ? Rgba(lead ? 0x2F2F2F : 0x292929, 1.0f)
                           : Rgba(lead ? 0xFFFFFF : 0xF7F7F7, 1.0f));
    rt_->FillRoundedRectangle(body, brush_.Get());
    brush_->SetColor(dark_ ? D2D1::ColorF(0xFFFFFF, 0.10f) : D2D1::ColorF(0, 0.09f));
    rt_->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), r, r), brush_.Get(),
        1.0f);

    const float icon = kIcon * s_;
    const float ix = kPadX * s_;
    const float iy = (title_h - icon) * 0.5f;
    if (c.icon.Get()) {
        rt_->DrawBitmap(c.icon.Get(), D2D1::RectF(ix, iy, ix + icon, iy + icon), 1.0f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    if (c.layout.Get()) {
        brush_->SetColor(dark_ ? D2D1::ColorF(0xFFFFFF, lead ? 0.92f : 0.70f)
                               : D2D1::ColorF(0x1A1A1A, lead ? 1.0f : 0.72f));
        rt_->DrawTextLayout(D2D1::Point2F(ix + icon + kGap * s_, 0), c.layout.Get(),
                            brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    if (!lead) return;

    // Count badge on the upper-left corner, above the stack.
    if (count_ > 1 && badge_fmt_.Get()) {
        const std::wstring text = count_ > 99 ? L"99+" : std::to_wstring(count_);
        const float bh = 20.0f * s_;
        const float bw = std::max(bh, (8.0f + 6.5f * static_cast<float>(text.size())) * s_);
        const float bx = bw * 0.5f - 8 * s_;
        const float by = -8 * s_;
        const D2D1_ROUNDED_RECT pill =
            D2D1::RoundedRect(D2D1::RectF(bx - bw * 0.5f, by, bx + bw * 0.5f, by + bh), bh * 0.5f,
                              bh * 0.5f);
        brush_->SetColor(dark_ ? Rgba(0x2F2F2F, 1.0f) : Rgba(0xFFFFFF, 1.0f));
        rt_->DrawRoundedRectangle(pill, brush_.Get(), 3.0f * s_);
        brush_->SetColor(accent_);
        rt_->FillRoundedRectangle(pill, brush_.Get());
        brush_->SetColor(accent_light_ ? D2D1::ColorF(0x111111) : D2D1::ColorF(0xFFFFFF));
        rt_->DrawText(text.c_str(), static_cast<UINT32>(text.size()), badge_fmt_.Get(),
                       D2D1::RectF(bx - bw * 0.5f, by, bx + bw * 0.5f, by + bh - 0.5f * s_),
                       brush_.Get());
    }


}

float DragStack::FeedbackWidth() {
    return LayoutDragFeedback(dw_.Get(), feedback_.empty() ? l10n::Get(l10n::StringId::DragHintReleaseCancel) : feedback_, s_, effect_ == DROPEFFECT_NONE).width;
}

void DragStack::DrawFeedback(float scale, D2D1_POINT_2F pivot) {
    if (phase_ != Phase::Live) return;
    const auto text = feedback_.empty() ? l10n::Get(l10n::StringId::DragHintReleaseCancel) : feedback_;
    const auto layout = LayoutDragFeedback(dw_.Get(), text, s_, effect_ == DROPEFFECT_NONE);
    // HTML .gcards has a fixed 36px height and .gbadge a 6px top margin.
    rt_->SetTransform(D2D1::Matrix3x2F::Translation(kAnchorX * s_, (kAnchorY + 42) * s_) *
        D2D1::Matrix3x2F::Scale(scale, scale, pivot));
    DrawDragFeedback(rt_.Get(), brush_.Get(), layout, 0, 0, s_, dark_, effect_ == DROPEFFECT_NONE,
        (effect_ & DROPEFFECT_COPY) != 0, feedback_.empty() || text == l10n::Get(l10n::StringId::DragHintReleaseCancel));
}

void DragStack::Render(float scale, float alpha) {
    if (!rt_.Get() || !mem_dc_) return;
    const Card& lead = cards_[0];
    const float fx = lead.x - std::floor(lead.x);
    const float fy = lead.y - std::floor(lead.y);
    POINT win_pos{static_cast<LONG>(std::floor(lead.x) - kAnchorX * s_),
                  static_cast<LONG>(std::floor(lead.y) - kAnchorY * s_)};
    if (phase_ == Phase::Live) {
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromPoint(last_pt_, MONITOR_DEFAULTTONEAREST), &monitor)) {
            // Include the stack, shadow and separate feedback capsule so it
            // remains readable at the right/bottom screen edges.
            const float inset = 32.0f * s_;
            const float x = std::clamp(lead.x, monitor.rcWork.left + inset,
                std::max(monitor.rcWork.left + inset, monitor.rcWork.right - std::max(kCardMaxW * s_, FeedbackWidth()) - inset));
            const float y = std::clamp(lead.y, monitor.rcWork.top + inset,
                std::max(monitor.rcWork.top + inset, monitor.rcWork.bottom - (kCardH + 10.0f + 6.0f + 24.0f) * s_ - inset));
            win_pos.x += static_cast<LONG>(std::floor(x) - std::floor(lead.x));
            win_pos.y += static_cast<LONG>(std::floor(y) - std::floor(lead.y));
        }
    }

    const RECT rc{0, 0, win_w_, win_h_};
    rt_->BindDC(mem_dc_, &rc);
    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(0, 0.0f));
    // Scale about the pointer side of the lead card so lifting and
    // gulping read as "toward the pointer".
    const D2D1_POINT_2F pivot = D2D1::Point2F(kAnchorX * s_ - kOffX * s_ * 0.5f,
                                              kAnchorY * s_ - kOffY * s_ * 0.5f);
    const float max_dx = kWinW * s_ - kAnchorX * s_ - lead.w - 8 * s_;
    for (size_t i = cards_.size(); i-- > 0;) {
        const Card& c = cards_[i];
        float dx = c.x - std::floor(lead.x);
        float dy = c.y - std::floor(lead.y);
        if (i > 0) {
            // Keep a lagging card inside the surface (the trail is short).
            dx = std::clamp(dx, -kAnchorX * s_ + 8 * s_, std::max(0.0f, max_dx));
            dy = std::clamp(dy, -kAnchorY * s_ + 10 * s_, kWinH * s_ - kAnchorY * s_ - kCardH * s_ - 12 * s_);
        } else {
            dx = fx;
            dy = fy;
        }
        DrawCard(c, i == 0, dx, dy, scale, pivot);
    }
    DrawFeedback(scale, pivot);
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) {
        // Lost device on a DC target is rare; dropping the frame is fine.
    }

#ifdef PULSE_DRAGSTACK_TEST
    GdiFlush();
    DragStackTestFrame(dib_, win_w_, win_h_, win_pos, Clamp01(alpha));
#endif
    if (snapshot_) return;
    POINT src{0, 0};
    SIZE size{win_w_, win_h_};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(Clamp01(alpha) * 255.0f)),
                        AC_SRC_ALPHA};
    POINT pos = win_pos;
    UpdateLayeredWindow(hwnd_, nullptr, &pos, &size, mem_dc_, &src, 0, &blend, ULW_ALPHA);
    if (!shown_) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        shown_ = true;
    }
}

#ifdef PULSE_WITH_TEST_HOOKS
bool DragStack::SaveReference(const std::vector<std::wstring>& paths,bool dark,DWORD effect,
                             const std::wstring& text,const wchar_t* png,float dpi_scale) {
    if(paths.empty() || !png)return false;
    DragStack stack;
    stack.snapshot_=true;
    stack.snapshot_scale_=std::clamp(dpi_scale,1.0f,2.5f);
    if(!stack.Init(paths,dark) || !stack.wic_.Get())return false;
    stack.effect_=effect;stack.feedback_=text;
    for(size_t i=0;i<stack.cards_.size();++i) {
        stack.cards_[i].x=stack.cards_[0].x+static_cast<float>(i)*5*stack.s_;
        stack.cards_[i].y=stack.cards_[0].y+static_cast<float>(i)*5*stack.s_;
        stack.cards_[i].rot=0;
    }
    stack.Render(1,1);
    GdiFlush();
    DIBSECTION dib{};
    if(GetObjectW(stack.dib_,sizeof(dib),&dib)!=sizeof(dib) || !dib.dsBm.bmBits)return false;
    Com<IWICBitmap> bitmap;
    if(FAILED(stack.wic_->CreateBitmapFromMemory(static_cast<UINT>(stack.win_w_),static_cast<UINT>(stack.win_h_),
        GUID_WICPixelFormat32bppPBGRA,static_cast<UINT>(dib.dsBm.bmWidthBytes),
        static_cast<UINT>(dib.dsBm.bmWidthBytes*stack.win_h_),static_cast<BYTE*>(dib.dsBm.bmBits),bitmap.Put())))return false;
    Com<IWICStream> stream;Com<IWICBitmapEncoder> encoder;Com<IWICBitmapFrameEncode> frame;
    if(FAILED(stack.wic_->CreateStream(stream.Put())) || FAILED(stream->InitializeFromFilename(png,GENERIC_WRITE)) ||
       FAILED(stack.wic_->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.Put())) ||
       FAILED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache)) ||
       FAILED(encoder->CreateNewFrame(frame.Put(),nullptr)) || FAILED(frame->Initialize(nullptr)))return false;
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    if(FAILED(frame->SetSize(static_cast<UINT>(stack.win_w_),static_cast<UINT>(stack.win_h_))) ||
       FAILED(frame->SetPixelFormat(&format)) || FAILED(frame->WriteSource(bitmap.Get(),nullptr)))return false;
    return SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}
#endif

DragVisual* CreateDragStack(const std::vector<std::wstring>& paths, bool dark) {
    return DragStack::Create(paths, dark);
}

// Pulse.exe compiles this file; the console drag test does not, and runs
// without a visual.
const bool g_drag_stack_registered = (SetDragVisualFactory(&CreateDragStack), true);

} // namespace
#ifdef PULSE_WITH_TEST_HOOKS
bool SaveDragStackReference(const std::vector<std::wstring>& paths,bool dark,DWORD effect,
                            const std::wstring& text,const wchar_t* png,float dpi_scale) {
    return DragStack::SaveReference(paths,dark,effect,text,png,dpi_scale);
}
#endif
} // namespace pulse::ui

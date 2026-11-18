#include "setup_ui.h"

#include "setup_common.h"
#include "setup_log.h"
#include "setup_uninstall.h"

#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace pulse::setup {
namespace {

constexpr float kW = 720.f, kH = 468.f, kTitle = 40.f;
// Welcome hero block offset: centres the logo..path group between titlebar and footer.
constexpr float kWelcomeDy = 30.f;
constexpr UINT WM_ENGINE_PHASE = WM_APP + 1, WM_ENGINE_BUSY = WM_APP + 2, WM_ENGINE_DONE = WM_APP + 3, WM_ENGINE_LOCKED = WM_APP + 4;
constexpr float kPi = 3.14159265f;
float EaseOut(float t) { const float u = 1 - t; return 1 - u * u * u; }  // cubic, no powf

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.f) {
    return {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a};
}
D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) { c.a *= a; return c; }

struct Theme {
    bool dark;
    D2D1_COLOR_F accent, accent_hover, accent_press, accent_text, mica1, mica2, win, card, card_strong, stroke,
        divider, text, text2, text3, input, input_hover, input_line, hover, press, danger, success, warn, scrim;
    COLORREF edit_bg, edit_text;
};

// Values from the prototype's CSS custom properties.
Theme MakeTheme(bool dark) {
    Theme t{};
    t.dark = dark;
    if (dark) {
        t.accent = Hex(0x7B83F0); t.accent_hover = Hex(0x8C93F4); t.accent_press = Hex(0x6A72E0);
        t.accent_text = Hex(0x0E1030); t.mica1 = Hex(0x20212A); t.mica2 = Hex(0x1A1B1F); t.win = Hex(0x1C1C1F);
        t.card = Hex(0xFFFFFF, .045f); t.card_strong = Hex(0xFFFFFF, .07f); t.stroke = Hex(0xFFFFFF, .075f);
        t.divider = Hex(0xFFFFFF, .06f); t.text = Hex(0xFFFFFF); t.text2 = Hex(0xFFFFFF, .62f);
        t.text3 = Hex(0xFFFFFF, .40f); t.input = Hex(0xFFFFFF, .06f); t.input_hover = Hex(0xFFFFFF, .085f);
        t.input_line = Hex(0xFFFFFF, .54f); t.hover = Hex(0xFFFFFF, .07f); t.press = Hex(0xFFFFFF, .045f);
        t.danger = Hex(0xFF99A4); t.success = Hex(0x6CCB5F); t.warn = Hex(0xFCE100); t.scrim = Hex(0x000000, .35f);
        t.edit_bg = RGB(0x2B, 0x2C, 0x33); t.edit_text = RGB(255, 255, 255);
    } else {
        t.accent = Hex(0x5B63D9); t.accent_hover = Hex(0x6B72DE); t.accent_press = Hex(0x4C53C4);
        t.accent_text = Hex(0xFFFFFF); t.mica1 = Hex(0xEEF0F8); t.mica2 = Hex(0xF5F6F2); t.win = Hex(0xF3F5F1);
        t.card = Hex(0xFFFFFF, .72f); t.card_strong = Hex(0xFFFFFF); t.stroke = Hex(0x000000, .07f);
        t.divider = Hex(0x000000, .06f); t.text = Hex(0x1F2635); t.text2 = Hex(0x5D6A80); t.text3 = Hex(0x929CAE);
        t.input = Hex(0xFFFFFF, .85f); t.input_hover = Hex(0xFFFFFF); t.input_line = Hex(0x000000, .45f);
        t.hover = Hex(0x000000, .04f); t.press = Hex(0x000000, .025f); t.danger = Hex(0xC42B1C);
        t.success = Hex(0x0F7B0F); t.warn = Hex(0x9D5D00); t.scrim = Hex(0xFFFFFF, .35f);
        t.edit_bg = RGB(0xFC, 0xFC, 0xFD); t.edit_text = RGB(0x1F, 0x26, 0x35);
    }
    return t;
}

bool SystemUsesDarkTheme() {
    // Screenshot tests of the other theme (only honoured with /PULSETEST, see Run()).
    wchar_t forced[8]{};
    if (GetEnvironmentVariableW(L"PULSE_SETUP_TEST_THEME", forced, 8)) return !lstrcmpiW(forced, L"dark");
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value == 0;
}

std::wstring Format(const wchar_t* pattern, const std::wstring& a1, const std::wstring& a2 = {}) {
    std::wstring s = pattern;
    for (auto [key, value] : {std::pair{L"%1", &a1}, std::pair{L"%2", &a2}}) {
        const auto pos = s.find(key);
        if (pos != std::wstring::npos) s.replace(pos, 2, *value);
    }
    return s;
}

std::wstring FormatSize(uint64_t bytes) {
    wchar_t buf[32];
    const double gb = bytes / 1073741824.0;
    if (gb >= 1) swprintf_s(buf, gb >= 100 ? L"%.0f GB" : L"%.1f GB", gb);
    else swprintf_s(buf, L"%.0f MB", bytes / 1048576.0);
    return buf;
}

std::wstring RootOf(const std::wstring& path) { return path.size() >= 2 && path[1] == L':' ? path.substr(0, 2) : L""; }

bool ValidInstallPath(const std::wstring& p) {
    const wchar_t d = p.empty() ? 0 : static_cast<wchar_t>(p[0] | 0x20);
    if (p.size() < 4 || d < L'a' || d > L'z' || p[1] != L':' || p[2] != L'\\') return false;
    if (p.find_first_of(L"<>\"|?*", 2) != std::wstring::npos || p.find(L':', 2) != std::wstring::npos) return false;
    const UINT type = GetDriveTypeW((p.substr(0, 3)).c_str());
    return type == DRIVE_FIXED || type == DRIVE_REMOVABLE;
}

enum Id : int {
    kNone, kMin, kCloseX, kInstall, kAgree, kLicense, kToOptions, kBack, kAppEdit, kBrowseApp, kTglIndex,
    kIndexEdit, kBrowseIndex, kTglStartup, kTglDesktop, kTglGlobal, kTglAi, kOptBack, kOptInstall, kCancelInstall, kLaunch,
    kCloseDone, kOpenLog, kDlgPrimary, kDlgSecondary, kDlgEndRetry, kDot0, kDot1, kDot2, kDot3, kDot4, kDot5, kTglCleanup, kUnStart, kUnClose,
};

enum class Page { Welcome, Options, Install, Done, Error, Uninstall };
enum class UnState { Idle, Running, Done, Failed };
enum class Dialog { None, Busy, Cancel, Locked, Cleanup };

class Wizard {
public:
    // Uninstall mode when `payload` is null and `un` is given.
    Wizard(Payload* payload, const std::wstring& self, InstallRequest request, Lang lang,
           const UninstallRequest* un = nullptr)
        : payload_(payload), self_(self), req_(std::move(request)), lang_(lang),
          theme_(MakeTheme(SystemUsesDarkTheme())) {
        if (!req_.options.test) {
            SetEnvironmentVariableW(L"PULSE_SETUP_TEST_THEME", nullptr);
            theme_ = MakeTheme(SystemUsesDarkTheme());
        } else {
            slow_ = GetEnvironmentVariableW(L"PULSE_SETUP_TEST_SLOW", nullptr, 0) != 0;
        }
        if (payload_) for (const auto& f : payload_->Info().files) total_bytes_ += f.size;
        if (un) {
            uninstall_ = true;
            ureq_ = *un;
            cleanup_ = un->cleanup_data;
            page_ = prev_page_ = Page::Uninstall;
            service_installed_ = !req_.options.test && IndexServiceInstalled();
        }
        busy_answer_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ~Wizard() {
        if (worker_.joinable()) worker_.join();
        CloseHandle(busy_answer_event_);
        ReleaseDevice();
        for (auto& [k, f] : formats_) SafeRelease(f);
        SafeRelease(ellipsis_fmt_);
        SafeRelease(dwrite_);
        SafeRelease(d2d_);
        SafeRelease(wic_);
        if (edit_font_) DeleteObject(edit_font_);
        if (edit_brush_) DeleteObject(edit_brush_);
    }

    WizardOutcome Run();

private:
    // ---- device ----
    bool CreateFactories();
    bool EnsureDevice();
    void ReleaseDevice() { SafeRelease(brush_); SafeRelease(logo_); SafeRelease(logo_small_); SafeRelease(rt_); SafeRelease(window_rt_); }
#ifdef PULSE_SETUP_UI_CAPTURE
    bool CaptureProbe(const wchar_t* path);
#endif
    void LoadLogo();
    float Dpi() const { return static_cast<float>(dpi_); }
    float Scale() const { return dpi_ / 96.f; }

    // ---- drawing helpers ----
    void Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius = 0);
    void Stroke(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius = 0, float width = 1);
    void Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float w = 1.3f);
    IDWriteTextFormat* Fmt(float size, DWRITE_FONT_WEIGHT weight);
    IDWriteTextLayout* Layout(const std::wstring& s, float size, DWRITE_FONT_WEIGHT w, float max_w, float max_h,
                              bool wrap, DWRITE_TEXT_ALIGNMENT align);
    void Text(const std::wstring& s, D2D1_RECT_F r, float size, D2D1_COLOR_F c,
              DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
              bool wrap = false);
    float TextWidth(const std::wstring& s, float size, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL);
    float TextHeight(const std::wstring& s, float size, float max_w, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL);
    void Arc(float cx, float cy, float r, float start_deg, float sweep_deg, D2D1_COLOR_F c, float width,
             ID2D1Brush* brush = nullptr);
    void Polyline(std::initializer_list<D2D1_POINT_2F> pts, D2D1_COLOR_F c, float w, bool closed = false);

    // ---- widgets (immediate mode; each records a hit rect) ----
    void Hit(int id, const D2D1_RECT_F& r, bool focusable = true);
    enum class Btn { Normal, Primary, Ghost, Danger };
    float Button(int id, const std::wstring& label, float x, float y, Btn style, float min_w = 0, float h = 32,
                 bool align_right = false, float font = 13.f, float radius = 5.f, int icon = 0);
    void Toggle(int id, bool on, float right, float cy, bool enabled = true);
    void Checkbox(int id, bool on, float x, float y);
    void FocusRing(int id, const D2D1_RECT_F& r, float radius);
    void Icon(int kind, float x, float y, D2D1_COLOR_F c);

    // ---- pages ----
    void Paint();
    void PaintTitleBar();
    void PaintPage(Page p, float dx, float alpha);
    void PaintWelcome();
    void PaintOptions();
    void PaintInstall();
    void PaintDone();
    void PaintError();
    void PaintDialog();
    void PaintLockDialog();
    void PaintUninstall();
    float CardTop(float y) const { return y - scroll_; }

    // ---- behaviour ----
    LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK StaticProc(HWND h, UINT m, WPARAM w, LPARAM l);
    void Activate(int id);
    void Show(Page p);
    void StartInstall();
    void EngineThread(InstallRequest request);
    void StartUninstall();
    void UninstallThread(UninstallRequest request);
    void Browse(bool app);
    void LayoutEdits();
    void CreateEdits();
    void SyncFromEdits();
    int HitTest(float x, float y) const;
    void MoveFocus(bool back);
    std::vector<int> StepsList() const;
    bool Animating() const;
    bool GlowAnimating() const;
    void Invalidate() { InvalidateRect(hwnd_, nullptr, FALSE); }
    void CloseWith(int code) { outcome_.exit_code = code; DestroyWindow(hwnd_); }
    const wchar_t* T(Str s) const { return pulse::setup::Text(lang_, s); }

    Payload* payload_;
    // uninstall mode
    bool uninstall_ = false, cleanup_ = true, service_installed_ = false;
    UninstallRequest ureq_;
    UnState un_state_ = UnState::Idle;
    std::atomic<int> un_percent_{0};
    std::wstring un_item_;  // guarded by file_mutex_
    UninstallResult ures_;
    std::wstring self_;
    InstallRequest req_;
    Lang lang_;
    Theme theme_;
    uint64_t total_bytes_ = 0;
    WizardOutcome outcome_;

    HWND hwnd_ = nullptr, app_edit_ = nullptr, index_edit_ = nullptr;
    HFONT edit_font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    UINT dpi_ = 96;
    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    IWICImagingFactory* wic_ = nullptr;
    ID2D1RenderTarget* rt_ = nullptr;
    ID2D1HwndRenderTarget* window_rt_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    ID2D1Bitmap* logo_ = nullptr;
    ID2D1Bitmap* logo_small_ = nullptr;
    IDWriteTextFormat* ellipsis_fmt_ = nullptr;
    std::map<int, IDWriteTextFormat*> formats_;
    std::wstring font_family_;

    struct HitRect { int id; D2D1_RECT_F r; bool focusable; };
    std::vector<HitRect> hits_;
    int hover_ = kNone, pressed_ = kNone, focus_ = kNone;
    bool keyboard_focus_ = false;
    D2D1_RECT_F app_edit_rect_{}, index_edit_rect_{};
    bool app_edit_visible_ = false, index_edit_visible_ = false;

    Page page_ = Page::Welcome, prev_page_ = Page::Welcome;
    ULONGLONG page_time_ = 0;
    Dialog dialog_ = Dialog::None;
    ULONGLONG dialog_time_ = 0;
    float scroll_ = 0, max_scroll_ = 0;
    bool agree_ = true;
    ULONGLONG agree_flash_ = 0;
    std::wstring path_error_;

    // engine state
    std::thread worker_;
    std::atomic<uint64_t> done_bytes_{0};
    std::atomic<bool> paused_{false}, cancel_{false}, committing_{false};
    std::mutex file_mutex_;
    std::wstring current_file_;
    Phase phase_ = Phase::Preparing;
    float shown_progress_ = 0;
    InstallResult result_;
    bool engine_done_ = false, busy_given_up_ = false;
    ULONGLONG done_time_ = 0;
    ULONGLONG glow_frame_ = 0;  // last welcome-glow repaint (throttled to ~30 fps)
    FileLockReport locked_report_;
    FileLockReport displayed_locks_;
    size_t locked_cleanup_versions_ = 0, displayed_cleanup_versions_ = 0;
    float lock_scroll_ = 0, lock_scroll_max_ = 0;
    HANDLE busy_answer_event_ = nullptr;
    std::atomic<bool> busy_retry_{false};
    std::atomic<bool> end_locks_{false}, end_locks_failed_{false};
    bool slow_ = false;
    bool probe_ = false;
    int slide_ = 0;
    ULONGLONG slide_time_ = 0;
};

// ======================================================================
// Device

bool Wizard::CreateFactories() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&dwrite_))))
        return false;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    // Keep both Chinese translations in the prototype's sans-serif family.
    // Older Windows installations may expose YaHei without the UI suffix.
    IDWriteFontCollection* fonts = nullptr;
    dwrite_->GetSystemFontCollection(&fonts);
    auto has = [&](const wchar_t* name) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        return fonts && SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists;
    };
    font_family_ = L"Segoe UI";
    if (lang_ != Lang::En) {
        for (const wchar_t* family : {L"Microsoft YaHei UI", L"Microsoft YaHei", L"Microsoft JhengHei UI", L"Microsoft JhengHei"}) {
            if (has(family)) {
                font_family_ = family;
                break;
            }
        }
    }
    SafeRelease(fonts);
    return true;
}

bool Wizard::EnsureDevice() {
    if (rt_) return true;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                                                    Dpi(), Dpi());
    if (FAILED(d2d_->CreateHwndRenderTarget(
            props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)), &window_rt_)))
        return false;
    rt_ = window_rt_;
    rt_->AddRef();
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt_->CreateSolidColorBrush(theme_.text, &brush_);
    LoadLogo();
    return true;
}

void Wizard::LoadLogo() {
    if (!wic_) return;
    // Scaled down by the shell from the 256 px image at the exact pixel size,
    // which looks far better than bilinear sampling in Direct2D.
    auto load = [&](float dips) -> ID2D1Bitmap* {
        const int px = static_cast<int>(std::lround(dips * Scale()));
        HICON icon = nullptr;
        if (FAILED(LoadIconWithScaleDown(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), px, px, &icon))) return nullptr;
        ID2D1Bitmap* out = nullptr;
        IWICBitmap* bitmap = nullptr;
        IWICFormatConverter* conv = nullptr;
        if (SUCCEEDED(wic_->CreateBitmapFromHICON(icon, &bitmap)) && SUCCEEDED(wic_->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(bitmap, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom)))
            rt_->CreateBitmapFromWicBitmap(conv, nullptr, &out);
        SafeRelease(conv);
        SafeRelease(bitmap);
        DestroyIcon(icon);
        return out;
    };
    logo_ = load(96);
    logo_small_ = load(16);
}

// ======================================================================
// Drawing helpers

void Wizard::Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius) {
    brush_->SetColor(c);
    if (radius > 0) rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_);
    else rt_->FillRectangle(r, brush_);
}

void Wizard::Stroke(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius, float width) {
    brush_->SetColor(c);
    const float h = width / 2;
    const D2D1_RECT_F in{r.left + h, r.top + h, r.right - h, r.bottom - h};
    if (radius > 0) rt_->DrawRoundedRectangle(D2D1::RoundedRect(in, radius, radius), brush_, width);
    else rt_->DrawRectangle(in, brush_, width);
}

void Wizard::Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float w) {
    brush_->SetColor(c);
    rt_->DrawLine({x1, y1}, {x2, y2}, brush_, w);
}

IDWriteTextFormat* Wizard::Fmt(float size, DWRITE_FONT_WEIGHT weight) {
    const int key = static_cast<int>(size * 10) * 1000 + weight;
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second;
    IDWriteTextFormat* f = nullptr;
    const wchar_t* locale = lang_ == Lang::ZhHans ? L"zh-CN" : lang_ == Lang::ZhHant ? L"zh-TW" : L"en-US";
    dwrite_->CreateTextFormat(font_family_.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL, size, locale, &f);
    formats_[key] = f;
    return f;
}

IDWriteTextLayout* Wizard::Layout(const std::wstring& s, float size, DWRITE_FONT_WEIGHT w, float max_w, float max_h,
                                  bool wrap, DWRITE_TEXT_ALIGNMENT align) {
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), Fmt(size, w), max_w, max_h,
                                         &layout)))
        return nullptr;
    layout->SetTextAlignment(align);
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!wrap) {
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        IDWriteInlineObject* sign = nullptr;
        dwrite_->CreateEllipsisTrimmingSign(Fmt(size, w), &sign);
        layout->SetTrimming(&trim, sign);
        SafeRelease(sign);
    }
    return layout;
}

void Wizard::Text(const std::wstring& s, D2D1_RECT_F r, float size, D2D1_COLOR_F c, DWRITE_FONT_WEIGHT w,
                  DWRITE_TEXT_ALIGNMENT align, bool wrap) {
    if (s.empty() || r.right <= r.left) return;
    IDWriteTextLayout* layout = Layout(s, size, w, r.right - r.left, r.bottom - r.top, wrap, align);
    if (!layout) return;
    // Vertically centre single lines in their rectangle.
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    float y = r.top;
    if (!wrap) y = r.top + std::max(0.f, (r.bottom - r.top - m.height) / 2);
    brush_->SetColor(c);
    rt_->DrawTextLayout({r.left, y}, layout, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    layout->Release();
}

float Wizard::TextWidth(const std::wstring& s, float size, DWRITE_FONT_WEIGHT w) {
    IDWriteTextLayout* layout = Layout(s, size, w, 4000, 200, false, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!layout) return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    layout->Release();
    return m.widthIncludingTrailingWhitespace;
}

float Wizard::TextHeight(const std::wstring& s, float size, float max_w, DWRITE_FONT_WEIGHT w) {
    IDWriteTextLayout* layout = Layout(s, size, w, max_w, 2000, true, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!layout) return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    layout->Release();
    return m.height;
}

void Wizard::Arc(float cx, float cy, float r, float start_deg, float sweep_deg, D2D1_COLOR_F c, float width,
                 ID2D1Brush* brush) {
    if (sweep_deg <= 0.01f) return;
    sweep_deg = std::min(sweep_deg, 359.99f);
    ID2D1PathGeometry* geo = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(d2d_->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) { SafeRelease(geo); return; }
    auto at = [&](float deg) {
        const float a = deg * kPi / 180.f;
        return D2D1::Point2F(cx + r * std::cos(a), cy + r * std::sin(a));
    };
    sink->BeginFigure(at(start_deg), D2D1_FIGURE_BEGIN_HOLLOW);
    float done = 0;
    while (done < sweep_deg) {  // arcs of at most 180° keep ArcSize unambiguous
        const float part = std::min(180.f, sweep_deg - done);
        done += part;
        sink->AddArc(D2D1::ArcSegment(at(start_deg + done), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    ID2D1StrokeStyle* style = nullptr;
    d2d_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &style);
    brush_->SetColor(c);
    rt_->DrawGeometry(geo, brush ? brush : brush_, width, style);
    SafeRelease(style);
    SafeRelease(sink);
    SafeRelease(geo);
}

void Wizard::Polyline(std::initializer_list<D2D1_POINT_2F> pts, D2D1_COLOR_F c, float w, bool closed) {
    ID2D1PathGeometry* geo = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(d2d_->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) { SafeRelease(geo); return; }
    auto it = pts.begin();
    sink->BeginFigure(*it, D2D1_FIGURE_BEGIN_HOLLOW);
    for (++it; it != pts.end(); ++it) sink->AddLine(*it);
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    sink->Close();
    ID2D1StrokeStyle* style = nullptr;
    d2d_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                            nullptr, 0, &style);
    brush_->SetColor(c);
    rt_->DrawGeometry(geo, brush_, w, style);
    SafeRelease(style);
    SafeRelease(sink);
    SafeRelease(geo);
}

// 20×20 line icons following the prototype's SVGs.
void Wizard::Icon(int kind, float x, float y, D2D1_COLOR_F c) {
    brush_->SetColor(c);
    const float w = 1.3f;
    switch (kind) {
        case 1:  // folder
            Polyline({{x + 2.5f, y + 14.5f}, {x + 2.5f, y + 5.5f}, {x + 4, y + 4}, {x + 7.6f, y + 4}, {x + 9.2f, y + 5.8f},
                      {x + 16, y + 5.8f}, {x + 17.5f, y + 7.3f}, {x + 17.5f, y + 14.5f}, {x + 16, y + 16}, {x + 4, y + 16}},
                     c, w, true);
            break;
        case 2:  // search
            rt_->DrawEllipse(D2D1::Ellipse({x + 8.5f, y + 8.5f}, 5.5f, 5.5f), brush_, w);
            Line(x + 12.6f, y + 12.6f, x + 17, y + 17, c, w);
            break;
        case 3:  // database
            rt_->DrawEllipse(D2D1::Ellipse({x + 10, y + 5}, 6, 2.2f), brush_, w);
            Line(x + 4, y + 5, x + 4, y + 15, c, w);
            Line(x + 16, y + 5, x + 16, y + 15, c, w);
            Arc(x + 10, y + 10, 6, 0, 180, c, w);  // approximations of the lower curves
            Arc(x + 10, y + 15, 6, 0, 180, c, w);
            break;
        case 4:  // power
            Line(x + 10, y + 2.5f, x + 10, y + 9.5f, c, w);
            Arc(x + 10, y + 10.5f, 6, -50, 280, c, w);
            break;
        case 5:  // monitor
            rt_->DrawRoundedRectangle(D2D1::RoundedRect({x + 2.5f, y + 3.5f, x + 17.5f, y + 14}, 1.5f, 1.5f), brush_, w);
            Line(x + 7, y + 17, x + 13, y + 17, c, w);
            Line(x + 10, y + 14, x + 10, y + 17, c, w);
            break;
        case 6:  // gear (14 px)
            rt_->DrawEllipse(D2D1::Ellipse({x + 7, y + 7}, 2, 2), brush_, w);
            for (int i = 0; i < 8; ++i) {
                const float a = i * kPi / 4;
                Line(x + 7 + 4.2f * std::cos(a), y + 7 + 4.2f * std::sin(a), x + 7 + 6 * std::cos(a),
                     y + 7 + 6 * std::sin(a), c, w);
            }
            break;
        case 7:  // back chevron (14 px)
            Polyline({{x + 9, y + 1.5f}, {x + 3.5f, y + 7}, {x + 9, y + 12.5f}}, c, 1.5f);
            break;
        case 9:  // dual sparkle, matching the setup prototype's 20 px icon
            Polyline({{x + 9, y + 2.5f}, {x + 10.7f, y + 7.1f}, {x + 15.3f, y + 8.8f},
                      {x + 10.7f, y + 10.5f}, {x + 9, y + 15.1f}, {x + 7.3f, y + 10.5f},
                      {x + 2.7f, y + 8.8f}, {x + 7.3f, y + 7.1f}}, c, w, true);
            Polyline({{x + 15.5f, y + 13}, {x + 16.1f, y + 14.6f}, {x + 17.7f, y + 15.2f},
                      {x + 16.1f, y + 15.8f}, {x + 15.5f, y + 17.4f}, {x + 14.9f, y + 15.8f},
                      {x + 13.3f, y + 15.2f}, {x + 14.9f, y + 14.6f}}, c, w, true);
            break;
        case 8:  // warning triangle (20 px)
            Polyline({{x + 10, y + 2}, {x + 18.5f, y + 17}, {x + 1.5f, y + 17}}, c, 1.5f, true);
            Line(x + 10, y + 7.5f, x + 10, y + 11.5f, c, 1.6f);
            Line(x + 10, y + 14.2f, x + 10, y + 14.4f, c, 1.8f);
            break;
    }
}

// ======================================================================
// Widgets

void Wizard::Hit(int id, const D2D1_RECT_F& r, bool focusable) { hits_.push_back({id, r, focusable}); }

void Wizard::FocusRing(int id, const D2D1_RECT_F& r, float radius) {
    if (!keyboard_focus_ || focus_ != id) return;
    Stroke({r.left - 3, r.top - 3, r.right + 3, r.bottom + 3}, theme_.text, radius + 3, 2);
}

float Wizard::Button(int id, const std::wstring& label, float x, float y, Btn style, float min_w, float h,
                     bool align_right, float font, float radius, int icon) {
    const DWRITE_FONT_WEIGHT weight =
        style == Btn::Primary || style == Btn::Danger ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
    const float icon_w = icon ? 22.f : 0.f;
    const float w = std::max(min_w, TextWidth(label, font, weight) + 32 + icon_w);
    if (align_right) x -= w;
    const D2D1_RECT_F r{x, y, x + w, y + h};
    const bool hot = hover_ == id, down = pressed_ == id && hot;
    D2D1_COLOR_F bg{}, fg = theme_.text;
    switch (style) {
        case Btn::Primary:
            bg = down ? theme_.accent_press : hot ? theme_.accent_hover : theme_.accent;
            fg = theme_.accent_text;
            break;
        case Btn::Danger:
            bg = hot ? Hex(0xD23B2C) : Hex(0xC42B1C);
            fg = Hex(0xFFFFFF);
            break;
        case Btn::Ghost:
            bg = hot ? theme_.hover : D2D1_COLOR_F{0, 0, 0, 0};
            fg = hot ? theme_.text : theme_.text2;
            break;
        case Btn::Normal:
            bg = down ? theme_.press : hot ? theme_.hover : theme_.card_strong;
            break;
    }
    if (bg.a > 0) Fill(r, bg, radius);
    if (style == Btn::Normal) Stroke(r, theme_.stroke, radius);
    if (style == Btn::Primary) Line(r.left + radius, r.bottom - .5f, r.right - radius, r.bottom - .5f, Hex(0, .18f), 1);
    const float tw = TextWidth(label, font, weight);
    float tx = x + (w - tw - icon_w) / 2;
    if (icon) {
        float center_y = y + h / 2;
        if (auto* layout = Layout(label, font, weight, tw + 1, h, false, DWRITE_TEXT_ALIGNMENT_LEADING)) {
            DWRITE_TEXT_METRICS metrics{};
            DWRITE_OVERHANG_METRICS ink{};
            if (SUCCEEDED(layout->GetMetrics(&metrics)) && SUCCEEDED(layout->GetOverhangMetrics(&ink))) {
                // Match the visible glyphs, not the font's asymmetric line box.
                center_y = y + std::max(0.f, (h - metrics.height) / 2) + (h - ink.top + ink.bottom) / 2;
            }
            layout->Release();
        }
        Icon(icon, tx, center_y - 7, fg);
        tx += icon_w;
    }
    Text(label, {tx, y, tx + tw + 1, y + h}, font, fg, weight);
    FocusRing(id, r, radius);
    Hit(id, r);
    return w;
}

void Wizard::Toggle(int id, bool on, float right, float cy, bool enabled) {
    const D2D1_RECT_F r{right - 40, cy - 10, right, cy + 10};
    const bool hot = enabled && hover_ == id;
    const float a = enabled ? 1.f : .45f;
    if (on) Fill(r, WithAlpha(theme_.accent, a), 10);
    else { Fill(r, WithAlpha(theme_.input, a), 10); Stroke(r, WithAlpha(theme_.input_line, a), 10); }
    const float knob = hot ? 6.f : 5.f;
    const float kx = on ? r.right - 10 : r.left + 10;
    brush_->SetColor(WithAlpha(on ? theme_.accent_text : theme_.text2, a));
    rt_->FillEllipse(D2D1::Ellipse({kx, cy}, knob, knob), brush_);
    Text(on ? T(Str::On) : T(Str::Off), {right - 110, cy - 10, right - 50, cy + 10}, 12, WithAlpha(theme_.text2, a),
         DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    FocusRing(id, r, 10);
    if (enabled) Hit(id, {r.left - 4, r.top - 6, r.right + 4, r.bottom + 6});
}

void Wizard::Checkbox(int id, bool on, float x, float y) {
    const D2D1_RECT_F r{x, y, x + 18, y + 18};
    const bool flash = agree_flash_ && GetTickCount64() - agree_flash_ < 900;
    if (on) Fill(r, theme_.accent, 4);
    else { Fill(r, theme_.input, 4); Stroke(r, flash ? theme_.danger : theme_.input_line, 4, flash ? 2.f : 1.f); }
    if (on) Polyline({{x + 4.5f, y + 9.5f}, {x + 7.5f, y + 12.5f}, {x + 13.5f, y + 6}}, theme_.accent_text, 1.8f);
    FocusRing(id, r, 4);
}

// ======================================================================
// Pages

void Wizard::Paint() {
    if (!EnsureDevice()) return;
    hits_.clear();
    app_edit_visible_ = index_edit_visible_ = false;
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    // Window background: the prototype's diagonal "mica" gradient.
    {
        D2D1_GRADIENT_STOP stops[] = {{0, theme_.mica1}, {.55f, theme_.mica2}, {1, theme_.mica2}};
        ID2D1GradientStopCollection* coll = nullptr;
        ID2D1LinearGradientBrush* grad = nullptr;
        rt_->CreateGradientStopCollection(stops, 3, &coll);
        if (coll && SUCCEEDED(rt_->CreateLinearGradientBrush({{0, 0}, {kW, kH}}, coll, &grad)))
            rt_->FillRectangle({0, 0, kW, kH}, grad);
        SafeRelease(grad);
        SafeRelease(coll);
    }
    PaintTitleBar();

    // Page transition: the old page slides left and fades, the new one slides in.
    const float t = std::min(1.f, (GetTickCount64() - page_time_) / 420.f);
    const float ease = EaseOut(t);
    rt_->PushAxisAlignedClip({0, kTitle, kW, kH}, D2D1_ANTIALIAS_MODE_ALIASED);
    if (t < 1 && prev_page_ != page_) PaintPage(prev_page_, -24 * ease, 1 - std::min(1.f, t * 1.6f));
    const size_t first_hit = hits_.size();
    PaintPage(page_, 24 * (1 - ease), std::min(1.f, t * 1.3f));
    if (t < 1) hits_.resize(first_hit);  // no clicks while sliding
    rt_->PopAxisAlignedClip();
    if (dialog_ != Dialog::None) PaintDialog();

    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) ReleaseDevice();
    LayoutEdits();
}

void Wizard::PaintTitleBar() {
    if (logo_small_) rt_->DrawBitmap(logo_small_, {14, 12, 30, 28}, 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    Text(T(uninstall_ ? Str::UnWindowTitle : Str::WindowTitle), {40, 0, 400, kTitle}, 12, theme_.text2);
    const D2D1_RECT_F min_r{kW - 92, 0, kW - 46, kTitle}, close_r{kW - 46, 0, kW, kTitle};
    if (hover_ == kMin) Fill(min_r, theme_.hover);
    if (hover_ == kCloseX) Fill(close_r, Hex(0xC42B1C));
    Line(min_r.left + 18, 20.5f, min_r.left + 28, 20.5f, theme_.text, 1);
    const D2D1_COLOR_F xc = hover_ == kCloseX ? Hex(0xFFFFFF) : theme_.text;
    Line(close_r.left + 18, 15.5f, close_r.left + 28, 25.5f, xc, 1);
    Line(close_r.left + 28, 15.5f, close_r.left + 18, 25.5f, xc, 1);
    Hit(kMin, min_r, false);
    const bool can_close = !(page_ == Page::Install && committing_) && un_state_ != UnState::Running;
    if (can_close) Hit(kCloseX, close_r, false);
}

#ifdef PULSE_SETUP_UI_CAPTURE
// Test executable only: exercise the same painter without requiring an unlocked
// desktop. The shipping installer has no capture entry point.
bool Wizard::CaptureProbe(const wchar_t* path) {
    ReleaseDevice();
    IWICBitmap* bitmap = nullptr;
    HRESULT hr = wic_->CreateBitmap(static_cast<UINT>(kW * Scale()), static_cast<UINT>(kH * Scale()),
        GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap);
    if (SUCCEEDED(hr)) hr = d2d_->CreateWicBitmapRenderTarget(bitmap,
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), Dpi(), Dpi()), &rt_);
    if (SUCCEEDED(hr)) {
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        hr = rt_->CreateSolidColorBrush(theme_.text, &brush_);
    }
    if (SUCCEEDED(hr)) {
        LoadLogo();
        Paint();
        wchar_t scroll[16]{};
        if (GetEnvironmentVariableW(L"PULSE_SETUP_TEST_LOCK_SCROLL", scroll, 16)) {
            lock_scroll_ = static_cast<float>(_wtoi(scroll));
            Paint();
        }
    }
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    if (SUCCEEDED(hr)) hr = wic_->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->WriteSource(bitmap, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    SafeRelease(frame); SafeRelease(encoder); SafeRelease(stream); SafeRelease(bitmap);
    return SUCCEEDED(hr);
}
#endif

void Wizard::PaintPage(Page p, float dx, float alpha) {
    if (alpha <= 0.01f) return;
    rt_->SetTransform(D2D1::Matrix3x2F::Translation(dx, 0));
    ID2D1Layer* layer = nullptr;
    const bool fade = alpha < .999f;
    if (fade && SUCCEEDED(rt_->CreateLayer(&layer)))
        rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), alpha),
                       layer);
    switch (p) {
        case Page::Welcome: PaintWelcome(); break;
        case Page::Options: PaintOptions(); break;
        case Page::Install: PaintInstall(); break;
        case Page::Done: PaintDone(); break;
        case Page::Error: PaintError(); break;
        case Page::Uninstall: PaintUninstall(); break;
    }
    if (layer) { rt_->PopLayer(); layer->Release(); }
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Wizard::PaintWelcome() {
    // Soft violet light follows the folded-page product mark.
    {
        const float lx = kW / 2 - 48, ly = 54 + kWelcomeDy, cx = kW / 2, cy = 102 + kWelcomeDy;
        const D2D1_POINT_2F dot{lx + 64.2f, ly + 42.6f};
        const D2D1_COLOR_F gold = Hex(0x8079F4), hot = Hex(0xAAA3FF), core = Hex(0xE5E2FF);
        const float k = theme_.dark ? 1.15f : 1.f;
        // Seconds within a 36 s loop: every period below divides it.
        const float t = GlowAnimating() ? static_cast<float>(GetTickCount64() % 36000) / 1000.f : 0.f;
        const float breath = GlowAnimating() ? .5f - .5f * std::cos(2 * kPi * t / 3.6f) : .7f;
        const float drift = 2 * kPi * t / 9.f, orbit = 2 * kPi * t / 12.f, counter = -2 * kPi * t / 18.f;
        auto glow = [&](D2D1_POINT_2F c, float rx, float ry, std::initializer_list<D2D1_GRADIENT_STOP> list) {
            ID2D1GradientStopCollection* coll = nullptr;
            ID2D1RadialGradientBrush* brush = nullptr;
            rt_->CreateGradientStopCollection(list.begin(), static_cast<UINT32>(list.size()), &coll);
            if (coll && SUCCEEDED(rt_->CreateRadialGradientBrush(
                            D2D1::RadialGradientBrushProperties(c, {0, 0}, rx, ry), coll, &brush)))
                rt_->FillEllipse(D2D1::Ellipse(c, rx, ry), brush);
            SafeRelease(brush);
            SafeRelease(coll);
        };
        const D2D1_COLOR_F wash = theme_.dark ? Hex(0x8C84F8) : Hex(0xC3C0FF);
        const float wa = theme_.dark ? .06f : .11f;  // low enough to stay free of 8-bit banding rings
        glow({cx, cy - 20}, 380, 380, {{0, WithAlpha(wash, wa)}, {.45f, WithAlpha(wash, wa * .45f)}, {1, WithAlpha(wash, 0)}});
        glow({cx + 14 * std::cos(drift), cy + 9 * std::sin(drift)}, 175, 175,
             {{0, WithAlpha(gold, .20f * k)}, {.38f, WithAlpha(gold, .13f * k)}, {.62f, WithAlpha(hot, .05f * k)},
              {1, WithAlpha(hot, 0)}});
        // Two soft lights circle the mark in opposite directions, so the glow
        // visibly moves around it rather than only pulsing in place.
        if (GlowAnimating()) {
            glow({cx + 72 * std::cos(orbit), cy + 40 * std::sin(orbit)}, 110, 110,
                 {{0, WithAlpha(hot, .24f * k)}, {.45f, WithAlpha(gold, .10f * k)}, {1, WithAlpha(gold, 0)}});
            glow({cx + 62 * std::cos(counter + kPi), cy + 34 * std::sin(counter + kPi)}, 96, 96,
                 {{0, WithAlpha(core, .20f * k)}, {.5f, WithAlpha(hot, .07f * k)}, {1, WithAlpha(hot, 0)}});
        }
        const float dk = k * (.55f + .45f * breath), dr = 128 * (.88f + .20f * breath);
        glow(dot, dr, dr, {{0, WithAlpha(core, .70f * dk)}, {.10f, WithAlpha(hot, .55f * dk)}, {.30f, WithAlpha(gold, .30f * dk)},
                           {.55f, WithAlpha(gold, .10f * dk)}, {.80f, WithAlpha(gold, .025f * dk)}, {1, WithAlpha(gold, 0)}});
    }
    if (logo_) rt_->DrawBitmap(logo_, {kW / 2 - 48, 54 + kWelcomeDy, kW / 2 + 48, 150 + kWelcomeDy}, 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);

    const std::wstring version(payload_->Info().version.begin(), payload_->Info().version.end());
    const float name_w = TextWidth(L"Pulse", 28, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float badge_w = TextWidth(version, 12, DWRITE_FONT_WEIGHT_MEDIUM) + 14;
    const float x0 = (kW - name_w - 8 - badge_w) / 2;
    Text(L"Pulse", {x0, 164 + kWelcomeDy, x0 + name_w + 2, 200 + kWelcomeDy}, 28, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const D2D1_RECT_F badge{x0 + name_w + 8, 173 + kWelcomeDy, x0 + name_w + 8 + badge_w, 193 + kWelcomeDy};
    Fill(badge, theme_.card_strong, 10);
    Stroke(badge, theme_.stroke, 10);
    Text(version, badge, 12, theme_.text2, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
    Text(T(Str::Tagline), {48, 204 + kWelcomeDy, kW - 48, 226 + kWelcomeDy}, 13.5f, theme_.text2, DWRITE_FONT_WEIGHT_NORMAL,
         DWRITE_TEXT_ALIGNMENT_CENTER);

    const std::wstring label = T(req_.upgrade ? Str::Upgrade : Str::Install);
    const float bw = std::max(220.f, TextWidth(label, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 64);
    Button(kInstall, label, (kW - bw) / 2, 252 + kWelcomeDy, Btn::Primary, bw, 44, false, 15, 8);

    // "Install to <path> · 28 MB required"
    const std::wstring to = std::wstring(T(Str::InstallTo)) + L" ";
    const std::wstring need = std::wstring(L" · ") + Format(T(Str::Needs), std::to_wstring((total_bytes_ + 1048575) / 1048576));
    const float to_w = TextWidth(to, 12), need_w = TextWidth(need, 12);
    const float path_w = std::min(300.f, TextWidth(req_.app_dir, 12) + 1);
    float x = (kW - to_w - path_w - need_w) / 2;
    Text(to, {x, 306 + kWelcomeDy, x + to_w + 1, 326 + kWelcomeDy}, 12, theme_.text3);
    x += to_w;
    Text(req_.app_dir, {x, 306 + kWelcomeDy, x + path_w, 326 + kWelcomeDy}, 12, theme_.text2);
    x += path_w;
    Text(need, {x, 306 + kWelcomeDy, x + need_w + 1, 326 + kWelcomeDy}, 12, theme_.text3);

    // Footer: agreement + custom install.
    const float fy = kH - 16 - 32;
    Checkbox(kAgree, agree_, 20, fy + 7);
    const std::wstring agree = std::wstring(T(Str::Agree)) + L" ";
    const float agree_w = TextWidth(agree, 12);
    Text(agree, {46, fy, 46 + agree_w + 1, fy + 32}, 12, theme_.text2);
    Hit(kAgree, {18, fy + 4, 46 + agree_w, fy + 28});
    const float lic_w = TextWidth(T(Str::License), 12);
    const D2D1_RECT_F lic{46 + agree_w, fy + 6, 46 + agree_w + lic_w + 1, fy + 26};
    Text(T(Str::License), {lic.left, fy, lic.right, fy + 32}, 12, theme_.accent);
    if (hover_ == kLicense) Line(lic.left, fy + 23, lic.right, fy + 23, theme_.accent, 1);
    FocusRing(kLicense, lic, 2);
    Hit(kLicense, lic);
    Button(kToOptions, T(Str::Custom), kW - 20, fy, Btn::Ghost, 0, 32, true, 13, 5, 6);
}

void Wizard::PaintOptions() {
    // Header
    const D2D1_RECT_F back{28, kTitle + 4, 60, kTitle + 36};
    if (hover_ == kBack) Fill(back, theme_.hover, 5);
    Icon(7, 37, kTitle + 13, theme_.text);
    FocusRing(kBack, back, 5);
    Hit(kBack, back);
    Text(T(Str::Custom), {72, kTitle + 4, 600, kTitle + 36}, 20, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);

    const float view_top = kTitle + 50, view_bottom = kH - 61;
    rt_->PushAxisAlignedClip({0, view_top, kW, view_bottom}, D2D1_ANTIALIAS_MODE_ALIASED);
    float y = view_top + 4 - scroll_;
    const float L = 28, R = kW - 28;
    auto section = [&](Str s) {
        Text(T(s), {L + 2, y, R, y + 18}, 12, theme_.text2, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        y += 26;
    };
    auto card = [&](int icon, Str title, Str sub, float alpha, const wchar_t* badge = nullptr,
                    bool wide_text = false) {
        const float sub_right = wide_text ? R - 80 : R - 330;
        const float sub_height = wide_text ? TextHeight(T(sub), 12, sub_right - L - 50) : 18.f;
        const D2D1_RECT_F r{L, y, R, y + std::max(62.f, sub_height + 44.f)};
        Fill(r, theme_.card, 7);
        Stroke(r, theme_.stroke, 7);
        Icon(icon, L + 16, y + 21, WithAlpha(theme_.text, .9f * alpha));
        const std::wstring t1 = T(title);
        Text(t1, {L + 50, y + 12, wide_text ? R - 80 : R - 300, y + 32}, 13.5f, WithAlpha(theme_.text, alpha));
        if (badge) {
            const float bx = L + 50 + TextWidth(t1, 13.5f) + 6, bw = TextWidth(badge, 11) + 14;
            const D2D1_RECT_F b{bx, y + 14, bx + bw, y + 30};
            Fill(b, WithAlpha(theme_.accent, .22f * alpha), 8);
            Text(badge, b, 11, WithAlpha(theme_.accent, alpha), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        Text(T(sub), {L + 50, y + 32, sub_right, y + 32 + sub_height}, 12,
             WithAlpha(theme_.text2, alpha), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, wide_text);
        return r;
    };
    auto field = [&](int edit_id, int browse_id, const D2D1_RECT_F& card_r, float width, D2D1_RECT_F& edit_rect,
                     bool& visible, bool enabled) {
        const float bw = TextWidth(T(Str::Browse), 13) + 32;
        const float right = card_r.right - 16;
        const float bx = right - bw;
        const D2D1_RECT_F in{bx - 6 - width, card_r.top + 15, bx - 6, card_r.top + 47};
        const float a = enabled ? 1.f : .45f;
        const bool focused = GetFocus() == (edit_id == kAppEdit ? app_edit_ : index_edit_);
        Fill(in, WithAlpha(focused ? theme_.input_hover : theme_.input, a), 5);
        Stroke(in, WithAlpha(theme_.stroke, a), 5);
        Line(in.left + 3, in.bottom - (focused ? 1.f : .5f), in.right - 3, in.bottom - (focused ? 1.f : .5f),
             focused ? theme_.accent : WithAlpha(theme_.input_line, a), focused ? 2.f : 1.f);
        edit_rect = {in.left + 9, in.top + 8, in.right - 8, in.bottom - 7};
        visible = enabled && in.top >= view_top && in.bottom <= view_bottom;
        // While the native edit is hidden (disabled, scrolled, sliding) draw its text.
        const bool edit_shown = visible && dialog_ == Dialog::None && GetTickCount64() - page_time_ >= 420;
        if (!edit_shown)
            Text(edit_id == kAppEdit ? req_.app_dir : req_.index_path, edit_rect, 12.5f,
                 WithAlpha(theme_.text, enabled ? 1.f : .45f));
        if (enabled) Hit(edit_id, in);
        if (enabled) Button(browse_id, T(Str::Browse), bx, card_r.top + 15, Btn::Normal, bw);
        else Text(T(Str::Browse), {bx, card_r.top + 15, right, card_r.top + 47}, 13, WithAlpha(theme_.text3, a),
                  DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    };

    section(Str::SecLocation);
    const auto app_card = card(1, Str::AppDir, Str::AppDirSub, 1);
    field(kAppEdit, kBrowseApp, app_card, 260, app_edit_rect_, app_edit_visible_, true);
    y += 62 + 6;
    {
        std::wstring dir = req_.app_dir;
        const std::wstring root = RootOf(dir);
        ULARGE_INTEGER free_bytes{}, total{};
        const bool known = !root.empty() && GetDiskFreeSpaceExW((root + L"\\").c_str(), &free_bytes, &total, nullptr);
        const std::wstring need = Format(T(Str::Needs), std::to_wstring((total_bytes_ + 1048575) / 1048576));
        Text(path_error_.empty() ? need : path_error_, {L + 2, y, R - 200, y + 16}, 11.5f,
             path_error_.empty() ? theme_.text3 : theme_.danger);
        if (known) {
            Text(Format(T(Str::SpaceFree), root, FormatSize(free_bytes.QuadPart)), {R - 300, y, R - 2, y + 16}, 11.5f,
                 theme_.text3, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
            const float used = total.QuadPart ? 1.f - float(free_bytes.QuadPart) / float(total.QuadPart) : 0;
            Fill({L + 2, y + 21, R - 2, y + 24}, theme_.divider, 1.5f);
            Fill({L + 2, y + 21, L + 2 + (R - L - 4) * used, y + 24}, theme_.accent, 1.5f);
        }
        y += 30;
    }
    y += 10;
    section(Str::SecSearch);
    const auto idx_card = card(2, Str::OptIndex, Str::OptIndexSub, 1, T(Str::Recommended), true);
    Toggle(kTglIndex, req_.tasks.index_service, idx_card.right - 16, idx_card.top + 31);
    y = idx_card.bottom + 3;
    const float ia = req_.tasks.index_service ? 1.f : .45f;
    const auto dir_card = card(3, Str::IndexDir, Str::IndexDirSub, ia);
    field(kIndexEdit, kBrowseIndex, dir_card, 220, index_edit_rect_, index_edit_visible_, req_.tasks.index_service);
    y += 62 + 14;
    const auto global_card = card(2, Str::OptGlobalSearch, Str::OptGlobalSearchSub, 1, nullptr, true);
    Toggle(kTglGlobal, req_.tasks.global_search, global_card.right - 16, global_card.top + 31);
    y = global_card.bottom + 14;
    section(Str::SecAi);
    const auto ai_card = card(9, Str::OptAiAssistant, Str::OptAiAssistantSub, 1, T(Str::New), true);
    Toggle(kTglAi, req_.tasks.ai_assistant, ai_card.right - 16, ai_card.top + 31);
    y = ai_card.bottom + 14;
    section(Str::SecOther);
    const auto st_card = card(4, Str::OptStartup, Str::OptStartupSub, 1, nullptr, true);
    Toggle(kTglStartup, req_.tasks.startup, st_card.right - 16, st_card.top + 31);
    y = st_card.bottom + 3;
    const auto dk_card = card(5, Str::OptDesktop, Str::OptDesktopSub, 1, nullptr, true);
    Toggle(kTglDesktop, req_.tasks.desktop_icon, dk_card.right - 16, dk_card.top + 31);
    y = dk_card.bottom + 8;
    rt_->PopAxisAlignedClip();
    max_scroll_ = std::max(0.f, (y + scroll_) - view_bottom);

    if (max_scroll_ > 0) {  // thin scroll indicator
        const float vh = view_bottom - view_top, th = vh * vh / (vh + max_scroll_);
        const float ty = view_top + (vh - th) * (scroll_ / max_scroll_);
        Fill({kW - 10, ty, kW - 7, ty + th}, theme_.text3, 1.5f);
    }

    // Footer
    Line(0, view_bottom + .5f, kW, view_bottom + .5f, theme_.divider, 1);
    const std::wstring label = T(req_.upgrade ? Str::Upgrade : Str::Install);
    const float iw = Button(kOptInstall, label, kW - 28, view_bottom + 12, Btn::Primary, 120, 32, true);
    Button(kOptBack, T(Str::Back), kW - 28 - iw - 8, view_bottom + 12, Btn::Normal, 0, 32, true);
}

std::vector<int> Wizard::StepsList() const {
    std::vector<int> s{0, 1};  // extract, shortcuts
    if (req_.tasks.index_service && !req_.options.test) s.push_back(2);
    s.push_back(3);  // verbs
    s.push_back(4);  // finish
    return s;
}

void Wizard::PaintInstall() {
    // Progress target from the engine phase; the ring eases towards it.
    const auto steps = StepsList();
    int current = 0;  // index into steps
    float target = 0;
    const float extract = total_bytes_ ? float(done_bytes_.load()) / float(total_bytes_) : 0;
    auto index_of = [&](int step) {
        for (size_t i = 0; i < steps.size(); ++i) if (steps[i] == step) return static_cast<int>(i);
        return 0;
    };
    switch (phase_) {
        case Phase::Preparing: case Phase::Extracting: target = .8f * extract; current = 0; break;
        case Phase::Closing: target = .8f; current = 0; break;
        case Phase::Replacing: target = .84f; current = 0; break;
        case Phase::Registering: target = .88f; current = index_of(1); break;
        case Phase::IndexService: target = .92f; current = index_of(2); break;
        case Phase::SeedVerbs: target = .96f; current = index_of(3); break;
        case Phase::Done: target = 1; current = static_cast<int>(steps.size()); break;
    }
    shown_progress_ += (target - shown_progress_) * .18f;
    if (target - shown_progress_ < .002f) shown_progress_ = target;

    const float cx = 44 + 66, cy = 96 + 66;
    Arc(cx, cy, 58, 0, 360, theme_.divider, 6);
    {
        D2D1_GRADIENT_STOP stops[] = {{0, theme_.accent}, {1, Hex(0x38BDF8)}};
        ID2D1GradientStopCollection* coll = nullptr;
        ID2D1LinearGradientBrush* grad = nullptr;
        rt_->CreateGradientStopCollection(stops, 2, &coll);
        if (coll) rt_->CreateLinearGradientBrush({{cx - 66, cy - 66}, {cx + 66, cy + 66}}, coll, &grad);
        Arc(cx, cy, 58, -90, 360 * shown_progress_, theme_.accent, 6, grad);
        SafeRelease(grad);
        SafeRelease(coll);
    }
    const std::wstring pct = std::to_wstring(static_cast<int>(shown_progress_ * 100 + .001f));
    const float pw = TextWidth(pct, 30, DWRITE_FONT_WEIGHT_SEMI_BOLD), sw = TextWidth(L"%", 14, DWRITE_FONT_WEIGHT_MEDIUM);
    Text(pct, {cx - (pw + sw) / 2, cy - 22, cx - (pw + sw) / 2 + pw + 1, cy + 20}, 30, theme_.text,
         DWRITE_FONT_WEIGHT_SEMI_BOLD);
    Text(L"%", {cx - (pw + sw) / 2 + pw + 1, cy - 8, cx + (pw + sw) / 2 + 2, cy + 14}, 14, theme_.text2,
         DWRITE_FONT_WEIGHT_MEDIUM);

    const Str names[] = {Str::StepExtract, Str::StepShortcuts, Str::StepIndex, Str::StepVerbs, Str::StepFinish};
    float y = 250;
    const float spin = (GetTickCount64() % 800) / 800.f * 360;
    for (size_t i = 0; i < steps.size(); ++i, y += 26) {
        const bool ok = static_cast<int>(i) < current, run = static_cast<int>(i) == current;
        const D2D1_COLOR_F c = ok ? theme_.text2 : run ? theme_.text : theme_.text3;
        const float dx = 44 + 8, dy = y + 13;
        if (ok) {
            brush_->SetColor(theme_.success);
            rt_->FillEllipse(D2D1::Ellipse({dx, dy}, 8, 8), brush_);
            Polyline({{dx - 3.5f, dy + .2f}, {dx - 1, dy + 2.6f}, {dx + 3.6f, dy - 2.4f}}, Hex(0xFFFFFF), 1.6f);
        } else if (run) {
            Arc(dx, dy, 7.25f, spin, 270, theme_.accent, 1.5f);
        } else {
            brush_->SetColor(c);
            rt_->DrawEllipse(D2D1::Ellipse({dx, dy}, 7.25f, 7.25f), brush_, 1.5f);
        }
        Text(T(names[steps[i]]), {70, y, 300, y + 26}, 12.5f, c);
        if (run && steps[i] == 0) {
            wchar_t sub[48];
            swprintf_s(sub, L"%.1f / %.0f MB", done_bytes_.load() / 1048576.0, total_bytes_ / 1048576.0);
            Text(sub, {250, y, 340, y + 26}, 11, theme_.text3, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
    }

    // Feature slides.
    const D2D1_RECT_F panel{360, 58, kW - 24, kH - 50};
    Fill(panel, theme_.card, 10);
    Stroke(panel, theme_.stroke, 10);
    if (!probe_ && GetTickCount64() - slide_time_ > 3200) { slide_ = (slide_ + 1) % 6; slide_time_ = GetTickCount64(); }
    const float st = std::min(1.f, (GetTickCount64() - slide_time_) / 500.f);
    const Str titles[] = {Str::S4Title, Str::S1Title, Str::S2Title, Str::S3Title, Str::S5Title, Str::S6Title};
    const Str texts[] = {Str::S4Text, Str::S1Text, Str::S2Text, Str::S3Text, Str::S5Text, Str::S6Text};
    const float text_h = TextHeight(T(texts[slide_]), 12.5f, panel.right - panel.left - 44);
    const D2D1_RECT_F art{panel.left + 22, panel.top + 22 + 10 * (1 - st), panel.right - 22,
                          panel.bottom - 30 - text_h - 38 + 10 * (1 - st)};
    ID2D1Layer* layer = nullptr;
    if (st < 1 && SUCCEEDED(rt_->CreateLayer(&layer)))
        rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), st), layer);
    {
        D2D1_GRADIENT_STOP stops[] = {{0, WithAlpha(theme_.accent, .20f)}, {.7f, WithAlpha(theme_.accent, 0)}};
        ID2D1GradientStopCollection* coll = nullptr;
        ID2D1LinearGradientBrush* grad = nullptr;
        rt_->CreateGradientStopCollection(stops, 2, &coll);
        if (coll && SUCCEEDED(rt_->CreateLinearGradientBrush({{art.left, art.top}, {art.right, art.bottom}}, coll, &grad)))
            rt_->FillRoundedRectangle(D2D1::RoundedRect(art, 8, 8), grad);
        SafeRelease(grad);
        SafeRelease(coll);
        Stroke(art, theme_.stroke, 8);
        // Mock content.
        const float mx = art.left + 12, mw = art.right - art.left - 24;
        float my = art.top + 12;
        auto row = [&](float frac, bool hl) {
            Fill({mx, my, mx + mw * frac, my + 12}, hl ? WithAlpha(theme_.accent, .45f) : theme_.card_strong, 3);
            my += 18;
        };
        if (slide_ == 0) {
            const float prompt_height = TextHeight(T(Str::AiDemoPrompt), 11.5f, mw - 44);
            Fill({mx + 24, my, mx + mw, my + prompt_height + 16}, WithAlpha(theme_.accent, .20f), 8);
            Text(T(Str::AiDemoPrompt), {mx + 34, my + 8, mx + mw - 10, my + 8 + prompt_height},
                 11.5f, theme_.text, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
            my += prompt_height + 26;
            const float reply_height = TextHeight(T(Str::AiDemoReply), 11.5f, mw - 36);
            const float reply_bottom = my + reply_height + 52;
            Fill({mx, my, mx + mw - 12, reply_bottom}, theme_.input, 8);
            Stroke({mx, my, mx + mw - 12, reply_bottom}, theme_.stroke, 8);
            Text(T(Str::AiDemoReply), {mx + 12, my + 10, mx + mw - 24, my + 10 + reply_height},
                 11.5f, theme_.text2, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
            const float action_width = TextWidth(T(Str::AiDemoAction), 10.5f) + 20;
            const D2D1_RECT_F action{mx + 12, my + reply_height + 19, mx + 12 + action_width, reply_bottom - 9};
            Fill(action, WithAlpha(theme_.accent, .14f), 4);
            Text(T(Str::AiDemoAction), action, 10.5f, theme_.accent, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
            my = reply_bottom + 12;
            if (my + 30 < art.bottom) { row(.86f, false); row(.64f, false); }
        } else if (slide_ == 1) {
            const D2D1_RECT_F s{mx, my, mx + mw, my + 22};
            Fill(s, theme_.input, 5);
            Stroke(s, theme_.stroke, 5);
            Icon(2, mx + 4, my + 3, theme_.text2);
            const std::wstring q = T(Str::SearchDemo);
            Text(q, {mx + 26, my, mx + mw, my + 22}, 10, theme_.text2);
            if ((GetTickCount64() / 500) % 2) {
                const float qx = mx + 26 + TextWidth(q, 10) + 2;
                Line(qx, my + 5.5f, qx, my + 16.5f, theme_.text, 1);
            }
            my += 28;
            row(1, true); row(1, false); row(.8f, false); row(.9f, false); row(.6f, false);
        } else if (slide_ == 2) {
            const float half = (mw - 6) / 2;
            float ry = my;
            for (float f : {1.f, .9f, 1.f, .7f, .85f}) { Fill({mx, ry, mx + half * f, ry + 12}, f == .9f ? WithAlpha(theme_.accent, .45f) : theme_.card_strong, 3); ry += 18; }
            D2D1_GRADIENT_STOP ps[] = {{0, Hex(0xF5D23A, .35f)}, {1, Hex(0x7B83F0, .35f)}};
            ID2D1GradientStopCollection* pc = nullptr;
            ID2D1LinearGradientBrush* pg = nullptr;
            rt_->CreateGradientStopCollection(ps, 2, &pc);
            const D2D1_RECT_F pv{mx + half + 6, my, mx + mw, art.bottom - 12};
            if (pc && SUCCEEDED(rt_->CreateLinearGradientBrush({{pv.left, pv.top}, {pv.right, pv.bottom}}, pc, &pg)))
                rt_->FillRoundedRectangle(D2D1::RoundedRect(pv, 5, 5), pg);
            SafeRelease(pg);
            SafeRelease(pc);
        } else if (slide_ == 4) {
            const D2D1_RECT_F tray{mx, my, mx + mw, my + 110};
            Fill(tray, WithAlpha(theme_.accent, .06f), 6);
            Stroke(tray, WithAlpha(theme_.accent, .35f), 6);
            for (int i = 0; i < 3; ++i) {
                const float left = mx + mw * (i == 1 ? .30f : i == 2 ? .18f : .10f);
                const float top = my + 12 + i * 28.f;
                Fill({left, top, left + mw * .55f, top + 22}, theme_.input, 4);
                Stroke({left, top, left + mw * .55f, top + 22}, theme_.stroke, 4);
                Fill({left + 8, top + 7, left + mw * .42f, top + 15}, theme_.card_strong, 3);
            }
            my += 122;
            row(.58f, true); row(.8f, false);
        } else if (slide_ == 5) {
            const D2D1_COLOR_F colors[] = {Hex(0xEF4444), Hex(0xF59E0B), Hex(0x22C55E),
                                          Hex(0x3B82F6), Hex(0xA855F7), Hex(0xEF4444)};
            const float widths[] = {.9f, .7f, .85f, .6f, .75f, .5f};
            for (int r = 0; r < 6; ++r) {
                Fill({mx, my + 3, mx + 9, my + 12}, colors[r], 4.5f);
                Fill({mx + 18, my + 1, mx + 18 + (mw - 18) * widths[r], my + 13}, theme_.card_strong, 3);
                my += 24;
            }
        } else {
            const float half = (mw - 6) / 2;
            for (int c = 0; c < 2; ++c) {
                float ry = my;
                const float x = mx + c * (half + 6);
                Fill({x, ry, x + half, ry + 14}, theme_.card_strong, 3);
                ry += 22;
                for (int r = 0; r < 5; ++r) {
                    Fill({x, ry, x + half * (r % 2 ? .75f : 1.f), ry + 12},
                         (c == 0 && r == 1) || (c == 1 && r == 3) ? WithAlpha(theme_.accent, .45f) : theme_.card_strong, 3);
                    ry += 18;
                }
            }
        }
    }
    if (slide_ == 0) {
        const float badge_width = TextWidth(T(Str::New), 10.5f) + 14;
        const float badge_left = art.left + TextWidth(T(titles[slide_]), 15, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 8;
        const D2D1_RECT_F badge{badge_left, art.bottom + 16, badge_left + badge_width, art.bottom + 32};
        Fill(badge, WithAlpha(theme_.accent, .16f), 8);
        Text(T(Str::New), badge, 10.5f, theme_.accent, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    Text(T(titles[slide_]), {art.left, art.bottom + 14, art.right, art.bottom + 34}, 15, theme_.text,
         DWRITE_FONT_WEIGHT_SEMI_BOLD);
    Text(T(texts[slide_]), {art.left, art.bottom + 38, art.right, art.bottom + 40 + text_h}, 12.5f, theme_.text2,
         DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    if (layer) { rt_->PopLayer(); layer->Release(); }
    {
        float x = panel.right - 16 - (14 + 5 * 5 + 5 * 5);
        for (int i = 0; i < 6; ++i) {
            const float w = i == slide_ ? 14.f : 5.f;
            const D2D1_RECT_F d{x, panel.bottom - 17, x + w, panel.bottom - 12};
            Fill(d, i == slide_ ? theme_.accent : theme_.text3, 2.5f);
            Hit(kDot0 + i, {d.left - 2, d.top - 5, d.right + 2, d.bottom + 5}, false);
            x += w + 5;
        }
    }

    // Footer: current file + cancel.
    std::wstring file;
    {
        std::lock_guard lock(file_mutex_);
        file = current_file_;
    }
    if (phase_ == Phase::Extracting && !file.empty())
        Text(req_.app_dir + L"\\" + file, {44, kH - 40, 44 + 420, kH - 20}, 12, theme_.text3);
    if (!committing_ && phase_ != Phase::Done)
        Button(kCancelInstall, T(Str::Cancel), kW - 20, kH - 44, Btn::Ghost, 0, 30, true, 12);
}

void Wizard::PaintDone() {
    const float t = std::min(1.f, (GetTickCount64() - page_time_) / 900.f);
    std::vector<std::pair<std::wstring, D2D1_COLOR_F>> pills;
    if (req_.tasks.index_service && !req_.options.test && (result_.index_running || !result_.index_configured))
        pills.push_back(result_.index_running ? std::pair{std::wstring(T(Str::SumIndex)), theme_.success}
                                              : std::pair{std::wstring(T(Str::SumIndexFailed)), theme_.warn});
    if (result_.defaults_written && req_.tasks.global_search) pills.push_back({T(Str::SumGlobalSearch), theme_.success});
    if (result_.defaults_written && req_.tasks.ai_assistant) pills.push_back({T(Str::SumAiAssistant), theme_.success});
    if (result_.verbs_seeded) pills.push_back({T(Str::SumMenu), theme_.success});
    if (result_.startup) pills.push_back({T(Str::SumStart), theme_.success});
    if (result_.dialog_cleanup_pending) pills.push_back({T(Str::CleanupPending), theme_.warn});
    std::vector<float> row_widths;
    std::vector<size_t> row_starts;
    for (size_t i = 0; i < pills.size(); ++i) {
        const float w = TextWidth(pills[i].first, 11.5f) + 32;
        if (row_widths.empty() || row_widths.back() + 6 + w > kW - 96) {
            row_starts.push_back(i);
            row_widths.push_back(w);
        } else row_widths.back() += 6 + w;
    }
    const float pills_height = row_widths.empty() ? 0.f : 30.f * static_cast<float>(row_widths.size()) + 18.f;
    const std::wstring sub_measure = req_.upgrade ? T(Str::DoneSubUp)
                                     : result_.index_running ? T(Str::DoneSub) : T(Str::DoneSubNoIndex);
    // icon 84 + 18 + title 44 + text + 16 + pills 48 + buttons 32, centred in the stage
    const float content = 84 + 18 + 44 + TextHeight(sub_measure, 13.5f, 420) + 16 + pills_height + 32;
    const float cx = kW / 2, top = kTitle + (kH - kTitle - content) / 2 - 6;
    // Check mark that draws itself.
    const float circle = std::min(1.f, t / .65f);
    Arc(cx, top + 42, 39, -90, 360 * EaseOut(circle), theme_.success, 3);
    if (t > .55f) {
        const float k = std::min(1.f, (t - .55f) / .45f);
        const D2D1_POINT_2F a{cx - 16, top + 43}, b{cx - 5, top + 54}, c{cx + 17, top + 31};
        const float l1 = 15.6f, l2 = 31.8f, len = (l1 + l2) * k;
        if (len <= l1) Polyline({a, {a.x + (b.x - a.x) * len / l1, a.y + (b.y - a.y) * len / l1}}, theme_.success, 4);
        else Polyline({a, b, {b.x + (c.x - b.x) * (len - l1) / l2, b.y + (c.y - b.y) * (len - l1) / l2}}, theme_.success, 4);
    }
    float y = top + 84 + 18;
    const std::wstring version(payload_->Info().version.begin(), payload_->Info().version.end());
    const std::wstring title = req_.upgrade ? Format(T(Str::DoneTitleUp), version) : T(Str::DoneTitle);
    Text(title, {48, y, kW - 48, y + 36}, 28, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 44;
    const std::wstring sub = req_.upgrade ? T(Str::DoneSubUp)
                             : result_.index_running ? T(Str::DoneSub) : T(Str::DoneSubNoIndex);
    const float sh = TextHeight(sub, 13.5f, 420);
    Text(sub, {(kW - 420) / 2, y, (kW + 420) / 2, y + sh + 2}, 13.5f, theme_.text2, DWRITE_FONT_WEIGHT_NORMAL,
         DWRITE_TEXT_ALIGNMENT_CENTER, true);
    y += sh + 16;
    // Summary pills.
    size_t row = 0;
    float x = pills.empty() ? 0.f : (kW - row_widths[0]) / 2;
    for (size_t i = 0; i < pills.size(); ++i) {
        if (row + 1 < row_starts.size() && i == row_starts[row + 1]) {
            ++row;
            y += 30;
            x = (kW - row_widths[row]) / 2;
        }
        const auto& [label, color] = pills[i];
        const float w = TextWidth(label, 11.5f) + 32;
        const D2D1_RECT_F r{x, y, x + w, y + 24};
        Fill(r, theme_.card, 12);
        Stroke(r, theme_.stroke, 12);
        brush_->SetColor(color);
        rt_->FillEllipse(D2D1::Ellipse({x + 13, y + 12}, 3, 3), brush_);
        Text(label, {x + 22, y, x + w, y + 24}, 11.5f, theme_.text2);
        x += w + 6;
    }
    y += pills.empty() ? 0 : 24 + 24;
    // Buttons
    const float lw = std::max(120.f, TextWidth(T(Str::Launch), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 32);
    const float cw = TextWidth(T(Str::Close), 13) + 32;
    const float bx = (kW - lw - cw - 8) / 2;
    Button(kCloseDone, T(Str::Close), bx, y, Btn::Normal, cw);
    Button(kLaunch, T(Str::Launch), bx + cw + 8, y, Btn::Primary, lw);
}

void Wizard::PaintError() {
    const float cx = kW / 2, top = 120;
    brush_->SetColor(WithAlpha(theme_.danger, .15f));
    rt_->FillEllipse(D2D1::Ellipse({cx, top + 32}, 32, 32), brush_);
    Line(cx, top + 18, cx, top + 37, theme_.danger, 4);
    Line(cx, top + 45, cx, top + 46, theme_.danger, 4.5f);
    float y = top + 84;
    Text(T(Str::ErrorTitle), {48, y, kW - 48, y + 32}, 22, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD,
         DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 42;
    const std::wstring msg = result_.error + L"\n" + T(Str::ErrorRolledBack);
    const float h = TextHeight(msg, 13, 480);
    Text(msg, {(kW - 480) / 2, y, (kW + 480) / 2, y + h + 2}, 13, theme_.text2, DWRITE_FONT_WEIGHT_NORMAL,
         DWRITE_TEXT_ALIGNMENT_CENTER, true);
    y += h + 24;
    const float lw = TextWidth(T(Str::OpenLog), 13) + 32, cw = std::max(96.f, TextWidth(T(Str::Close), 13) + 32);
    const float bx = (kW - lw - cw - 8) / 2;
    Button(kOpenLog, T(Str::OpenLog), bx, y, Btn::Normal, lw);
    Button(kCloseDone, T(Str::Close), bx + lw + 8, y, Btn::Primary, cw);
}

void Wizard::PaintUninstall() {
    const float L = 40, R = kW - 40;
    float y = kTitle + 26;
    if (logo_) rt_->DrawBitmap(logo_, {L, y, L + 56, y + 56}, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    Text(T(Str::UnTitle), {L + 72, y + 4, R, y + 34}, 22, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    Text(T(Str::UnSub), {L + 72, y + 35, R, y + 55}, 12.5f, theme_.text2);
    y += 56 + 22;
    const bool idle = un_state_ == UnState::Idle;
    auto card = [&](float top, Str title, const std::wstring& sub) {
        const D2D1_RECT_F r{L, top, R, top + 64};
        Fill(r, theme_.card, 6);
        Stroke(r, theme_.stroke, 6);
        Text(T(title), {L + 50, top + 13, R - 130, top + 33}, 14, theme_.text);
        Text(sub, {L + 50, top + 34, R - 130, top + 52}, 12, theme_.text2);
        return r;
    };
    const auto clean = card(y, Str::UnClean, T(Str::UnCleanSub));
    {  // trash can
        const float x = L + 16, iy = y + 22;
        const D2D1_COLOR_F c = theme_.text2;
        Line(x + 4, iy + 6, x + 16, iy + 6, c);
        Polyline({{x + 8, iy + 6}, {x + 8, iy + 4}, {x + 12, iy + 4}, {x + 12, iy + 6}}, c, 1.3f);
        Polyline({{x + 5.5f, iy + 6}, {x + 6.3f, iy + 16.5f}, {x + 13.7f, iy + 16.5f}, {x + 14.5f, iy + 6}}, c, 1.3f);
    }
    Toggle(kTglCleanup, cleanup_, R - 20, clean.top + 32, idle);
    y += 64 + 8;
    card(y, Str::UnService, T(service_installed_ ? Str::UnServiceOn : Str::UnServiceNone));
    Icon(2, L + 16, y + 22, theme_.text2);
    y += 64 + 20;

    if (!idle) {
        const float target = un_state_ == UnState::Running ? un_percent_ / 100.f : 1.f;
        shown_progress_ += (target - shown_progress_) * .25f;
        if (std::fabs(target - shown_progress_) < .002f) shown_progress_ = target;
        const D2D1_COLOR_F bar = un_state_ == UnState::Done     ? theme_.success
                                 : un_state_ == UnState::Failed ? theme_.danger
                                                                : theme_.accent;
        Fill({L, y, R, y + 4}, theme_.divider, 2);
        if (shown_progress_ > 0) Fill({L, y, L + (R - L) * shown_progress_, y + 4}, bar, 2);
        std::wstring line;
        D2D1_COLOR_F color = theme_.text3;
        if (un_state_ == UnState::Running) {
            std::lock_guard lock(file_mutex_);
            line = un_item_;
        } else if (un_state_ == UnState::Done) {
            line = T(ures_.dialog_cleanup_pending ? Str::UnDoneDialogRetained : ures_.reboot_needed ? Str::UnDoneReboot : Str::UnDone);
            color = theme_.text2;
        } else {
            line = std::wstring(T(Str::UnFailed)) + L" " + ures_.error;
            color = theme_.danger;
        }
        const float lh = TextHeight(line, 12, R - L);
        Text(line, {L, y + 12, R, y + 14 + lh}, 12, color, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING,
             true);
        if (un_state_ == UnState::Done && ures_.restore_result > 0) {
            const std::wstring warn =
                T(ures_.restore_result == 1 ? Str::IntegrationIncomplete : Str::IntegrationFailed);
            const float wy = y + 20 + lh;
            Icon(8, L, wy - 1, ures_.restore_result == 1 ? theme_.text2 : theme_.danger);
            Text(warn, {L + 28, wy, R, wy + TextHeight(warn, 12, R - L - 28) + 2}, 12, theme_.text2,
                 DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        }
    }

    const float fy = kH - 16 - 32;
    if (idle) {
        const float w = Button(kUnStart, T(Str::UnBtn), kW - 28, fy, Btn::Danger, 120, 32, true);
        Button(kUnClose, T(Str::Cancel), kW - 28 - w - 8, fy, Btn::Normal, 0, 32, true);
    } else if (un_state_ == UnState::Done) {
        Button(kUnClose, T(Str::Close), kW - 28, fy, Btn::Primary, 96, 32, true);
    } else if (un_state_ == UnState::Failed) {
        const float w = Button(kUnClose, T(Str::Close), kW - 28, fy, Btn::Primary, 96, 32, true);
        Button(kOpenLog, T(Str::OpenLog), kW - 28 - w - 8, fy, Btn::Normal, 0, 32, true);
    }
}

void Wizard::PaintDialog() {
    if (dialog_ == Dialog::Locked || dialog_ == Dialog::Cleanup) { PaintLockDialog(); return; }
    hits_.clear();  // modal
    Hit(kMin, {kW - 92, 0, kW - 46, kTitle}, false);
    const float t = std::min(1.f, (GetTickCount64() - dialog_time_) / 220.f);
    Fill({0, kTitle, kW, kH}, WithAlpha(theme_.scrim, t));
    const bool busy = dialog_ == Dialog::Busy;
    const std::wstring title = T(busy ? Str::BusyTitle : Str::CancelTitle);
    const std::wstring text = T(busy ? (uninstall_ ? Str::UnBusyText : Str::BusyText) : Str::CancelText);
    const float w = 420, text_h = TextHeight(text, 13, w - 48);
    const float h = 22 + 24 + 10 + text_h + 22 + 64;
    const float s = 1.04f - .04f * EaseOut(t);
    const float cx = kW / 2, cy = kTitle + (kH - kTitle) / 2;
    rt_->SetTransform(D2D1::Matrix3x2F::Scale(s, s, {cx, cy}));
    const D2D1_RECT_F r{cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2};
    for (int i = 6; i > 0; --i)  // soft shadow
        Fill({r.left - i * 2.f, r.top - i * 1.5f + 8, r.right + i * 2.f, r.bottom + i * 2.5f + 8}, Hex(0, .035f * t), 8.f + i * 2);
    Fill(r, theme_.win, 8);
    Stroke(r, theme_.stroke, 8);
    Icon(8, r.left + 24, r.top + 22 + 1, busy ? theme_.warn : theme_.text2);
    Text(title, {r.left + 54, r.top + 20, r.right - 24, r.top + 46}, 17, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    Text(text, {r.left + 24, r.top + 56, r.right - 24, r.top + 56 + text_h + 2}, 13, theme_.text2,
         DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    const D2D1_RECT_F act{r.left, r.bottom - 64, r.right, r.bottom};
    rt_->PushAxisAlignedClip(act, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    Fill({act.left, act.top, act.right, act.bottom + 8}, theme_.card, 8);
    rt_->PopAxisAlignedClip();
    Line(act.left, act.top + .5f, act.right, act.top + .5f, theme_.divider, 1);
    const float pw = Button(kDlgPrimary, T(busy ? Str::Retry : Str::KeepInstalling), r.right - 24, act.top + 16,
                            Btn::Primary, 96, 32, true);
    Button(kDlgSecondary, T(busy ? Str::Cancel : Str::CancelYes), r.right - 24 - pw - 8, act.top + 16,
           busy ? Btn::Normal : Btn::Danger, 96, 32, true);
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    if (t < 1) hits_.resize(1);
}

void Wizard::PaintLockDialog() {
    hits_.clear();
    Hit(kMin, {kW - 92, 0, kW - 46, kTitle}, false);
    const bool cleanup = dialog_ == Dialog::Cleanup;
    Fill({0, kTitle, kW, kH}, theme_.scrim);
    const D2D1_RECT_F card{60, 58, kW - 60, kH - 18};
    for (int i = 5; i > 0; --i)
        Fill({card.left - i * 2.f, card.top + 5 - i, card.right + i * 2.f, card.bottom + 5 + i}, Hex(0, .035f), 10.f + i);
    Fill(card, theme_.win, 10);
    Stroke(card, theme_.stroke, 10);
    Icon(8, card.left + 22, card.top + 23, theme_.warn);
    Text(T(cleanup ? Str::CleanupTitle : Str::LockedTitle),
         {card.left + 52, card.top + 18, card.right - 22, card.top + 46}, 18, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const bool owners = !displayed_locks_.programs.empty();
    const auto description = T(cleanup ? (owners ? Str::CleanupText : Str::CleanupUnknown) : Str::LockedText);
    const float text_h = TextHeight(description, 12.5f, card.right - card.left - 44);
    Text(description, {card.left + 22, card.top + 55, card.right - 22, card.top + 55 + text_h},
         12.5f, theme_.text2, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    const float section_y = card.top + 66 + text_h;
    Text(T(owners ? Str::CleanupApps : Str::LockedTitle),
         {card.left + 22, section_y, card.right - 22, section_y + 20}, 12, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (cleanup) Text(std::wstring(T(Str::CleanupVersions)) + L": " + std::to_wstring(displayed_cleanup_versions_),
         {card.left + 230, section_y, card.right - 22, section_y + 20}, 11.5f, theme_.text3,
         DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    const float footer_y = card.bottom - 62;
    const auto hint = T(end_locks_failed_ ? Str::EndFailed : cleanup ? Str::CleanupHint : !owners ? Str::LockedUnknown : Str::LockedLog);
    const float hint_h = TextHeight(hint, 11.5f, card.right - card.left - 44);
    const D2D1_RECT_F list{card.left + 22, section_y + 26, card.right - 22, footer_y - hint_h - 20};
    Fill(list, theme_.card, 6);
    Stroke(list, theme_.stroke, 6);
    const size_t rows = owners ? displayed_locks_.programs.size() : displayed_locks_.files.size();
    const float available = std::max(1.f, list.bottom - list.top - 8);
    lock_scroll_max_ = std::max(0.f, static_cast<float>(rows) * 42.f - available);
    lock_scroll_ = std::clamp(lock_scroll_, 0.f, lock_scroll_max_);
    rt_->PushAxisAlignedClip({list.left + 1, list.top + 1, list.right - 1, list.bottom - 1}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (size_t i = 0; i < rows; ++i) {
        const float y = list.top + 4 + static_cast<float>(i) * 42 - lock_scroll_;
        if (y + 42 < list.top || y > list.bottom) continue;
        std::wstring title, detail;
        if (owners) {
            const auto& program = displayed_locks_.programs[i];
            title = program.name.empty() ? L"PID " + std::to_wstring(program.pid) : program.name;
            detail = L"PID " + std::to_wstring(program.pid);
        } else {
            const auto& path = displayed_locks_.files[i];
            const auto slash = path.find_last_of(L"\\/");
            title = slash == std::wstring::npos ? path : path.substr(slash + 1);
            detail = path;
        }
        Text(title, {list.left + 12, y + 2, list.right - 15, y + 22}, 12.5f, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        Text(detail, {list.left + 12, y + 23, list.right - 15, y + 39}, 10.5f, theme_.text3);
        if (i + 1 < rows) Line(list.left + 12, y + 41, list.right - 12, y + 41, theme_.divider, .7f);
    }
    rt_->PopAxisAlignedClip();
    if (lock_scroll_max_ > 0) {
        const float thumb = std::max(20.f, available * available / (available + lock_scroll_max_));
        const float y = list.top + 4 + (available - thumb) * lock_scroll_ / lock_scroll_max_;
        Fill({list.right - 6, y, list.right - 3, y + thumb}, theme_.text3, 1.5f);
    }
    Text(hint, {card.left + 22, footer_y - hint_h - 10, card.right - 22, footer_y - 10},
         11.5f, theme_.text2, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    Line(card.left, footer_y, card.right, footer_y, theme_.divider, 1);
    Button(kOpenLog, T(Str::OpenLog), card.left + 18, footer_y + 15, Btn::Ghost);
    const bool can_end = std::any_of(displayed_locks_.programs.begin(), displayed_locks_.programs.end(),
        [](const LockedProgram& program) { return program.CanEnd(); });
    float right = card.right - 22;
    if (can_end) right -= Button(kDlgEndRetry, T(Str::EndRetry), right, footer_y + 15,
        Btn::Primary, 128, 32, true) + 8;
    right -= Button(kDlgPrimary, T(Str::Retry), right, footer_y + 15, can_end ? Btn::Normal : Btn::Primary, 64, 32, true) + 8;
    Button(kDlgSecondary, T(cleanup ? Str::CleanupKeep : Str::Cancel), right,
           footer_y + 15, Btn::Normal, 80, 32, true);
}

// ======================================================================
// Behaviour

// The welcome glow flows only while it can be seen, and not when the user
// turned off animations in Windows.
bool Wizard::GlowAnimating() const {
    if (page_ != Page::Welcome || dialog_ != Dialog::None || !hwnd_ || IsIconic(hwnd_) || !IsWindowVisible(hwnd_))
        return false;
    BOOL animate = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
    return animate != FALSE;
}

bool Wizard::Animating() const {
    const ULONGLONG now = GetTickCount64();
    return now - page_time_ < 1000 || now - dialog_time_ < 300 || page_ == Page::Install ||
           un_state_ == UnState::Running || (un_state_ != UnState::Idle && shown_progress_ < .999f) ||
           (agree_flash_ && now - agree_flash_ < 1000);
}

void Wizard::Show(Page p) {
    if (p == page_) return;
    SyncFromEdits();
    prev_page_ = page_;
    page_ = p;
    page_time_ = GetTickCount64();
    focus_ = kNone;
    hover_ = kNone;
    if (p == Page::Install) slide_time_ = page_time_;
    Invalidate();
}

void Wizard::SyncFromEdits() {
    auto get = [](HWND e) {
        const int n = GetWindowTextLengthW(e);
        std::wstring s(static_cast<size_t>(n) + 1, L'\0');
        GetWindowTextW(e, s.data(), n + 1);
        s.resize(static_cast<size_t>(n));
        return NormalizeDir(s);
    };
    if (app_edit_) req_.app_dir = get(app_edit_);
    if (index_edit_) req_.index_path = get(index_edit_);
}

void Wizard::Activate(int id) {
    if (probe_ && (id == kInstall || id == kOptInstall || id == kLaunch || id == kUnStart || id == kOpenLog)) return;
    if (probe_ && (id == kCloseX || id == kCloseDone)) { DestroyWindow(hwnd_); return; }
    if ((id == kCloseX || id == kCancelInstall) && (dialog_ == Dialog::Locked || dialog_ == Dialog::Cleanup)) {
        Activate(kDlgSecondary);
        return;
    }
    switch (id) {
        case kMin: ShowWindow(hwnd_, SW_MINIMIZE); break;
        case kUnClose:
        case kCloseX:
            if (page_ == Page::Uninstall) {
                if (un_state_ == UnState::Done) CloseWith(kExitOk);
                else if (un_state_ == UnState::Failed) CloseWith(outcome_.exit_code);
                else if (un_state_ == UnState::Idle) CloseWith(kExitCancelledBefore);
                break;
            }
            if (page_ == Page::Install) { if (!committing_) { paused_ = true; dialog_ = Dialog::Cancel; dialog_time_ = GetTickCount64(); } }
            else if (page_ == Page::Done) CloseWith(kExitOk);
            else if (page_ == Page::Error) CloseWith(outcome_.exit_code);
            else CloseWith(kExitCancelledBefore);
            break;
        case kAgree: agree_ = !agree_; break;
        case kTglCleanup: if (un_state_ == UnState::Idle) cleanup_ = !cleanup_; break;
        case kUnStart: StartUninstall(); break;
        case kLicense:
            ShellExecuteW(hwnd_, L"open", L"https://www.apache.org/licenses/LICENSE-2.0", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case kToOptions: Show(Page::Options); break;
        case kBack: case kOptBack: Show(Page::Welcome); break;
        case kAppEdit: SetFocus(app_edit_); break;
        case kIndexEdit: if (req_.tasks.index_service) SetFocus(index_edit_); break;
        case kBrowseApp: Browse(true); break;
        case kBrowseIndex: Browse(false); break;
        case kTglIndex: req_.tasks.index_service = !req_.tasks.index_service; break;
        case kTglGlobal: req_.tasks.global_search = !req_.tasks.global_search; break;
        case kTglAi: req_.tasks.ai_assistant = !req_.tasks.ai_assistant; break;
        case kTglStartup: req_.tasks.startup = !req_.tasks.startup; break;
        case kTglDesktop: req_.tasks.desktop_icon = !req_.tasks.desktop_icon; break;
        case kInstall: case kOptInstall: StartInstall(); break;
        case kCancelInstall:
            if (!committing_) { paused_ = true; dialog_ = Dialog::Cancel; dialog_time_ = GetTickCount64(); }
            break;
        case kLaunch: outcome_.launch = true; CloseWith(kExitOk); break;
        case kCloseDone: CloseWith(page_ == Page::Done ? kExitOk : outcome_.exit_code); break;
        case kOpenLog: ShellExecuteW(hwnd_, L"open", LogPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case kDlgEndRetry:
            if (dialog_ != Dialog::Locked && dialog_ != Dialog::Cleanup) break;
            if (std::none_of(displayed_locks_.programs.begin(), displayed_locks_.programs.end(),
                [](const LockedProgram& program) { return program.CanEnd(); })) break;
            if (MessageBoxW(hwnd_, T(Str::EndConfirm), T(Str::EndRetry),
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) break;
            Log(L"Lock dialog user action: end verified processes and retry");
            end_locks_ = true;
            busy_retry_ = true;
            SetEvent(busy_answer_event_);
            dialog_ = Dialog::None;
            break;
        case kDlgPrimary:
            if (dialog_ == Dialog::Cleanup) Log(L"Dialog cleanup user action: retry");
            if (dialog_ == Dialog::Busy || dialog_ == Dialog::Locked || dialog_ == Dialog::Cleanup) { busy_retry_ = true; SetEvent(busy_answer_event_); }
            else paused_ = false;
            dialog_ = Dialog::None;
            break;
        case kDlgSecondary:
            if (dialog_ == Dialog::Cleanup) Log(L"Dialog cleanup user action: keep for now");
            if (dialog_ == Dialog::Busy || dialog_ == Dialog::Locked || dialog_ == Dialog::Cleanup) {
                busy_retry_ = false;
                if (dialog_ != Dialog::Cleanup) busy_given_up_ = true;
                SetEvent(busy_answer_event_);
            }
            else { cancel_ = true; paused_ = false; }
            dialog_ = Dialog::None;
            break;
        case kDot0: case kDot1: case kDot2: case kDot3: case kDot4: case kDot5: slide_ = id - kDot0; slide_time_ = GetTickCount64(); break;
    }
    Invalidate();
}

void Wizard::StartInstall() {
    SyncFromEdits();
    if (!agree_) { agree_flash_ = GetTickCount64(); return; }
    path_error_.clear();
    if (!ValidInstallPath(req_.app_dir)) path_error_ = T(Str::BadDir);
    else {
        ULARGE_INTEGER free_bytes{};
        if (GetDiskFreeSpaceExW((RootOf(req_.app_dir) + L"\\").c_str(), &free_bytes, nullptr, nullptr) &&
            free_bytes.QuadPart < total_bytes_ * 2)  // staging + backup
            path_error_ = T(Str::NoSpace);
    }
    if (req_.tasks.index_service && !ValidInstallPath(req_.index_path) && path_error_.empty())
        path_error_ = T(Str::BadDir);
    if (!path_error_.empty()) { Show(Page::Options); return; }

    done_bytes_ = 0;
    cancel_ = paused_ = committing_ = false;
    engine_done_ = busy_given_up_ = false;
    phase_ = Phase::Preparing;
    shown_progress_ = 0;
    Show(Page::Install);
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread(&Wizard::EngineThread, this, req_);
}

void Wizard::EngineThread(InstallRequest request) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    EngineCallbacks cb;
    cb.phase = [this](Phase p, const std::wstring&) {
        if (p == Phase::Replacing) committing_ = true;
        PostMessageW(hwnd_, WM_ENGINE_PHASE, static_cast<WPARAM>(p), 0);
    };
    cb.progress = [this](const ExtractProgress& p) {
        done_bytes_ = p.done_bytes;
        if (p.current) {
            std::lock_guard lock(file_mutex_);
            current_file_ = p.current->path;
        }
        while (paused_ && !cancel_) Sleep(50);
        if (slow_) Sleep(40);  // test installs only: make the progress page observable
        return !cancel_.load();
    };
    const auto finish_lock_answer = [this](const FileLockReport& report) {
        if (end_locks_.exchange(false)) {
            bool failed = false;
            for (const auto& program : report.programs) {
                const DWORD error = EndLockedProgram(program);
                failed = failed || error != ERROR_SUCCESS;
                Log(L"End lock owner PID " + std::to_wstring(program.pid) + L": Win32=" + std::to_wstring(error));
            }
            end_locks_failed_ = failed;
        }
        return busy_retry_.load();
    };
    cb.locked = [this, &finish_lock_answer](const FileLockReport& report) {
        { std::lock_guard lock(file_mutex_); locked_report_ = report; }
        PostMessageW(hwnd_, WM_ENGINE_LOCKED, 0, 0);
        WaitForSingleObject(busy_answer_event_, INFINITE);
        return finish_lock_answer(report);
    };
    cb.cleanup_locked = [this, &finish_lock_answer](const DialogCleanupReport& report) {
        { std::lock_guard lock(file_mutex_); locked_report_ = report.locks;
          if (locked_report_.files.empty()) for (const auto& entry : report.blocked_directories)
              locked_report_.files.push_back(entry.directory);
          locked_cleanup_versions_ = report.blocked_directories.size(); }
        PostMessageW(hwnd_, WM_ENGINE_LOCKED, 1, 0);
        WaitForSingleObject(busy_answer_event_, INFINITE);
        return finish_lock_answer(report.locks);
    };
    cb.busy = [this]() {
        PostMessageW(hwnd_, WM_ENGINE_BUSY, 0, 0);
        WaitForSingleObject(busy_answer_event_, INFINITE);
        return busy_retry_.load();
    };
    InstallResult r = RunInstall(*payload_, self_, request, cb);
    CoUninitialize();
    result_ = std::move(r);
    PostMessageW(hwnd_, WM_ENGINE_DONE, 0, 0);
}

void Wizard::StartUninstall() {
    if (un_state_ != UnState::Idle) return;
    un_state_ = UnState::Running;
    un_percent_ = 0;
    shown_progress_ = 0;
    busy_given_up_ = false;
    focus_ = kNone;
    if (worker_.joinable()) worker_.join();
    UninstallRequest request = ureq_;
    request.cleanup_data = cleanup_;
    worker_ = std::thread(&Wizard::UninstallThread, this, request);
}

void Wizard::UninstallThread(UninstallRequest request) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    UninstallCallbacks cb;
    cb.progress = [this](int percent, const std::wstring& item) {
        un_percent_ = percent;
        {
            std::lock_guard lock(file_mutex_);
            un_item_ = item;
        }
        if (slow_) Sleep(60);  // test runs only: make the progress observable
    };
    cb.busy = [this]() {
        PostMessageW(hwnd_, WM_ENGINE_BUSY, 0, 0);
        WaitForSingleObject(busy_answer_event_, INFINITE);
        return busy_retry_.load();
    };
    UninstallResult r = RunUninstall(request, cb);
    CoUninitialize();
    ures_ = std::move(r);
    PostMessageW(hwnd_, WM_ENGINE_DONE, 0, 0);
}

void Wizard::Browse(bool app) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    SyncFromEdits();
    if (dlg->Show(hwnd_) == S_OK) {
        IShellItem* item = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            std::wstring p = NormalizeDir(path);
            // Like Inno's AppendDefaultDirName: picking "D:\Apps" installs to "D:\Apps\Pulse".
            if (app) {
                const auto slash = p.find_last_of(L'\\');
                const std::wstring last = slash == std::wstring::npos ? p : p.substr(slash + 1);
                if (!EqualsNoCase(last, L"Pulse")) p += (p.back() == L'\\' ? L"" : L"\\") + std::wstring(L"Pulse");
            }
            SetWindowTextW(app ? app_edit_ : index_edit_, p.c_str());
            SyncFromEdits();
            path_error_.clear();
        }
        CoTaskMemFree(path);
        SafeRelease(item);
    }
    dlg->Release();
    Invalidate();
}

void Wizard::CreateEdits() {
    auto make = [&](const std::wstring& text) {
        HWND e = CreateWindowExW(0, L"EDIT", text.c_str(), WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd_, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
        SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        return e;
    };
    app_edit_ = make(req_.app_dir);
    index_edit_ = make(req_.index_path);
}

void Wizard::LayoutEdits() {
    if (!app_edit_) return;
    const bool interactive = page_ == Page::Options && prev_page_ != page_ ? GetTickCount64() - page_time_ >= 420 : page_ == Page::Options;
    const float s = Scale();
    auto place = [&](HWND e, const D2D1_RECT_F& r, bool visible) {
        const bool show = visible && interactive && dialog_ == Dialog::None;
        if (show) {
            SetWindowPos(e, nullptr, static_cast<int>(r.left * s), static_cast<int>(r.top * s),
                         static_cast<int>((r.right - r.left) * s), static_cast<int>((r.bottom - r.top) * s),
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else if (IsWindowVisible(e)) {
            if (GetFocus() == e) SetFocus(hwnd_);
            ShowWindow(e, SW_HIDE);
        }
    };
    place(app_edit_, app_edit_rect_, app_edit_visible_);
    place(index_edit_, index_edit_rect_, index_edit_visible_);
}

int Wizard::HitTest(float x, float y) const {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
        if (x >= it->r.left && x < it->r.right && y >= it->r.top && y < it->r.bottom) {
            if (page_ == Page::Options && it->id != kBack && it->id != kMin && it->id != kCloseX &&
                it->id != kOptBack && it->id != kOptInstall && (y < kTitle + 50 || y >= kH - 61)) continue;
            return it->id;
        }
    return kNone;
}

void Wizard::MoveFocus(bool back) {
    std::vector<int> order;
    for (const auto& h : hits_) if (h.focusable && std::find(order.begin(), order.end(), h.id) == order.end()) order.push_back(h.id);
    if (order.empty()) return;
    auto it = std::find(order.begin(), order.end(), focus_);
    int i = it == order.end() ? (back ? 0 : -1) : static_cast<int>(it - order.begin());
    i = (i + (back ? -1 : 1) + static_cast<int>(order.size())) % static_cast<int>(order.size());
    focus_ = order[static_cast<size_t>(i)];
    keyboard_focus_ = true;
    if (focus_ == kAppEdit && IsWindowVisible(app_edit_)) { SetFocus(app_edit_); SendMessageW(app_edit_, EM_SETSEL, 0, -1); }
    else if (focus_ == kIndexEdit && IsWindowVisible(index_edit_)) { SetFocus(index_edit_); SendMessageW(index_edit_, EM_SETSEL, 0, -1); }
    else SetFocus(hwnd_);
    // Keep the focused card visible on the options page.
    if (page_ == Page::Options) {
        for (const auto& h : hits_) {
            if (h.id != focus_) continue;
            const float top = kTitle + 50, bottom = kH - 61;
            if (h.r.top < top) scroll_ = std::max(0.f, scroll_ - (top - h.r.top) - 20);
            else if (h.r.bottom > bottom) scroll_ = std::min(max_scroll_, scroll_ + (h.r.bottom - bottom) + 20);
        }
    }
    Invalidate();
}

LRESULT CALLBACK Wizard::StaticProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* self = static_cast<Wizard*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<Wizard*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Proc(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT Wizard::Proc(UINT msg, WPARAM wp, LPARAM lp) {
    const float s = Scale();
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wp) return 0;  // the whole window is client area; DWM keeps the shadow
            break;
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            const float x = pt.x / s, y = pt.y / s;
            if (y < kTitle && x < kW - 92) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_NCACTIVATE:
            return DefWindowProcW(hwnd_, msg, wp, -1);  // no default frame repaint
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            ReleaseDevice();
            if (edit_font_) DeleteObject(edit_font_);
            edit_font_ = CreateFontW(-static_cast<int>(std::lround(12.5 * Scale())), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                     DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, font_family_.c_str());
            SendMessageW(app_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_), TRUE);
            SendMessageW(index_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_), TRUE);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Invalidate();
            return 0;
        }
        case WM_SIZE:
            if (window_rt_) window_rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            Invalidate();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            Paint();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_TIMER:
            if (Animating()) {
                Invalidate();
            } else if (GlowAnimating() && GetTickCount64() - glow_frame_ >= 33) {
                glow_frame_ = GetTickCount64();
                Invalidate();
            }
            if (page_ == Page::Install && engine_done_ && shown_progress_ >= .999f && GetTickCount64() - done_time_ > 550)
                Show(result_.exit_code == kExitOk ? Page::Done : Page::Error);
            return 0;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            const int h = HitTest(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
            if (h != hover_) { hover_ = h; Invalidate(); }
            static HCURSOR arrow = LoadCursorW(nullptr, IDC_ARROW), hand = LoadCursorW(nullptr, IDC_HAND),
                           ibeam = LoadCursorW(nullptr, IDC_IBEAM);
            SetCursor(h == kLicense ? hand : (h == kAppEdit || h == kIndexEdit) ? ibeam : arrow);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && reinterpret_cast<HWND>(wp) == hwnd_) return TRUE;
            break;
        case WM_MOUSELEAVE:
            hover_ = kNone;
            Invalidate();
            return 0;
        case WM_LBUTTONDOWN:
            pressed_ = HitTest(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
            keyboard_focus_ = false;
            if (pressed_ != kAppEdit && pressed_ != kIndexEdit) SetFocus(hwnd_);
            SetCapture(hwnd_);
            Invalidate();
            return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            const int h = HitTest(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
            const int p = pressed_;
            pressed_ = kNone;
            if (h != kNone && h == p) { focus_ = h; Activate(h); }
            Invalidate();
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (dialog_ == Dialog::Locked || dialog_ == Dialog::Cleanup) {
                lock_scroll_ = std::clamp(lock_scroll_ - GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 42.f, 0.f, lock_scroll_max_);
                Invalidate();
                return 0;
            }
            if (page_ == Page::Options && dialog_ == Dialog::None) {
                scroll_ = std::clamp(scroll_ - GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 48.f, 0.f, max_scroll_);
                Invalidate();
            }
            return 0;
        case WM_KEYDOWN:
            if ((dialog_ == Dialog::Locked || dialog_ == Dialog::Cleanup) &&
                (wp == VK_NEXT || wp == VK_PRIOR || wp == VK_DOWN || wp == VK_UP)) {
                lock_scroll_ = std::clamp(lock_scroll_ + (wp == VK_NEXT || wp == VK_DOWN ? 84.f : -84.f), 0.f, lock_scroll_max_);
                Invalidate(); return 0;
            }
            if (wp == VK_TAB) { MoveFocus(GetKeyState(VK_SHIFT) < 0); return 0; }
            if ((wp == VK_RETURN || wp == VK_SPACE) && focus_ != kNone) {
                for (const auto& h : hits_) if (h.id == focus_) { Activate(focus_); return 0; }
            }
            if (wp == VK_RETURN && dialog_ == Dialog::None) {
                if (page_ == Page::Welcome || page_ == Page::Options) Activate(kInstall);
                else if (page_ == Page::Done) Activate(kLaunch);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                if (dialog_ != Dialog::None) Activate(dialog_ == Dialog::Cancel ? kDlgPrimary : kDlgSecondary);
                else if (page_ == Page::Options) Show(Page::Welcome);
                else Activate(kCloseX);
                return 0;
            }
            break;
        case WM_COMMAND:
            if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) Invalidate();
            if (HIWORD(wp) == EN_CHANGE) { path_error_.clear(); SyncFromEdits(); Invalidate(); }
            return 0;
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, theme_.edit_text);
            SetBkColor(dc, theme_.edit_bg);
            return reinterpret_cast<LRESULT>(edit_brush_);
        }
        case WM_ENGINE_PHASE:
            phase_ = static_cast<Phase>(wp);
            Invalidate();
            return 0;
        case WM_ENGINE_LOCKED: {
            { std::lock_guard lock(file_mutex_); displayed_locks_ = locked_report_;
              displayed_cleanup_versions_ = locked_cleanup_versions_; }
            dialog_ = wp ? Dialog::Cleanup : Dialog::Locked;
            lock_scroll_ = 0;
            focus_ = kDlgPrimary;
            keyboard_focus_ = true;
            dialog_time_ = GetTickCount64();
            FlashWindow(hwnd_, TRUE);
            Invalidate();
            return 0;
        }
        case WM_ENGINE_BUSY:
            dialog_ = Dialog::Busy;
            dialog_time_ = GetTickCount64();
            FlashWindow(hwnd_, TRUE);
            Invalidate();
            return 0;
        case WM_ENGINE_DONE:
            if (uninstall_) {
                if (worker_.joinable()) worker_.join();
                outcome_.exit_code = ures_.exit_code;
                outcome_.self_delete = ures_.schedule_self_delete;
                if (ures_.nothing_changed && busy_given_up_) {
                    un_state_ = UnState::Idle;  // gave up at the busy prompt: nothing removed
                    outcome_.exit_code = kExitCancelledBefore;
                } else {
                    un_state_ = ures_.exit_code == kExitOk ? UnState::Done : UnState::Failed;
                }
                Invalidate();
                return 0;
            }
            engine_done_ = true;
            done_time_ = GetTickCount64();
            outcome_.app_dir = result_.app_dir;
            outcome_.exit_code = result_.exit_code;
            if (result_.exit_code == kExitCancelledDuring || busy_given_up_) {
                // Nothing was changed: back to the start page.
                engine_done_ = false;
                outcome_.exit_code = kExitCancelledBefore;
                Show(Page::Welcome);
            } else if (result_.exit_code == kExitOk) {
                phase_ = Phase::Done;
            } else {
                shown_progress_ = 1;
                Show(Page::Error);
                engine_done_ = false;
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && !lstrcmpW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet")) {
                theme_ = MakeTheme(SystemUsesDarkTheme());
                if (edit_brush_) DeleteObject(edit_brush_);
                edit_brush_ = CreateSolidBrush(theme_.edit_bg);
                InvalidateRect(app_edit_, nullptr, TRUE);
                InvalidateRect(index_edit_, nullptr, TRUE);
                Invalidate();
            }
            return 0;
        case WM_CLOSE:
            Activate(kCloseX);  // Alt+F4 follows the close button's rules
            return 0;
        case WM_DESTROY:
            busy_retry_ = false;
            cancel_ = true;
            paused_ = false;
            SetEvent(busy_answer_event_);
            KillTimer(hwnd_, 1);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

WizardOutcome Wizard::Run() {
    wchar_t probe_value[16]{};
    probe_ = req_.options.test && GetEnvironmentVariableW(L"PULSE_SETUP_UI_PROBE", probe_value, 16) != 0;
    if (!CreateFactories()) {
        Log(L"Direct2D is not available");
        outcome_.exit_code = kExitPrepareFailed;
        return outcome_;
    }
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = StaticProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"PulseSetupWindow";
    RegisterClassExW(&wc);

    // Size for the monitor under the cursor.
    POINT cursor;
    GetCursorPos(&cursor);
    HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
        if (auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"))) fn(mon, 0, &dx, &dy);
    }
    if (probe_) {
        wchar_t value[16]{};
        if (GetEnvironmentVariableW(L"PULSE_SETUP_TEST_DPI", value, 16)) {
            const int requested = _wtoi(value);
            if (requested == 96 || requested == 144 || requested == 192) dx = static_cast<UINT>(requested);
        }
    }
    dpi_ = dx;
    const int w = static_cast<int>(kW * Scale()), h = static_cast<int>(kH * Scale());
    const RECT& wa = mi.rcWork;
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, T(uninstall_ ? Str::UnWindowTitle : Str::WindowTitle),
                            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                            wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h,
                            nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) { outcome_.exit_code = kExitPrepareFailed; return outcome_; }
    const MARGINS margins{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hwnd_, &margins);
    const int round = 2;  // DWMWCP_ROUND (Windows 11; ignored elsewhere)
    DwmSetWindowAttribute(hwnd_, 33, &round, sizeof(round));
    const BOOL dark = theme_.dark;
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));  // DWMWA_USE_IMMERSIVE_DARK_MODE
    edit_brush_ = CreateSolidBrush(theme_.edit_bg);
    edit_font_ = CreateFontW(-static_cast<int>(std::lround(12.5 * Scale())), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                             DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, font_family_.c_str());
    CreateEdits();
    SendMessageW(app_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_), FALSE);
    SendMessageW(index_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_), FALSE);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    page_time_ = GetTickCount64() - 1000;
    if (probe_) {
        wchar_t value[16]{};
        if (GetEnvironmentVariableW(L"PULSE_SETUP_TEST_PAGE", value, 16)) {
            const int page = _wtoi(value);
            if (page >= 0 && page <= 4) page_ = static_cast<Page>(page);
        }
        if (GetEnvironmentVariableW(L"PULSE_SETUP_TEST_SLIDE", value, 16)) slide_ = std::clamp(_wtoi(value), 0, 5);
        if (GetEnvironmentVariableW(L"PULSE_SETUP_TEST_SCROLL", value, 16)) scroll_ = static_cast<float>(std::max(0, _wtoi(value)));
        if (GetEnvironmentVariableW(L"PULSE_SETUP_TEST_LOCKS", value, 16)) {
            page_ = prev_page_ = Page::Install;
            dialog_ = _wtoi(value) == 2 ? Dialog::Locked : Dialog::Cleanup;
            displayed_cleanup_versions_ = 4;
            displayed_locks_.programs = {{6888, L"QQ"}, {18832, L"QQ"}, {36772, L"Weixin"},
                {4096, L"Adobe Photoshop — document editing session"}, {8192, L"Browser"}};
            for (auto& program : displayed_locks_.programs) program.started.dwLowDateTime = 1;
            if (_wtoi(value) == 3) {
                displayed_locks_.programs.clear();
                displayed_locks_.files = {L"C:\\Program Files\\Pulse\\DialogBridge\\9b3c1fcadc5b7e985b8a859cba86d8069e3163dfb447265f93bfe1392fc2a893\\PulseDialogAuto64.dll"};
            }
            dialog_time_ = GetTickCount64() - 1000;
            focus_ = kDlgPrimary;
            keyboard_focus_ = true;
        }
        result_.defaults_written = true;
        result_.verbs_seeded = true;
        result_.startup = true;
        phase_ = Phase::Extracting;
        shown_progress_ = .42f;
        total_bytes_ = 100 * 1048576;
        done_bytes_ = 42 * 1048576;
        slide_time_ = GetTickCount64() - 1000;
    }
#ifdef PULSE_SETUP_UI_CAPTURE
    if (probe_) {
        wchar_t output[32768]{};
        if (GetEnvironmentVariableW(L"PULSE_SETUP_TEST_CAPTURE", output, 32768)) {
            const bool captured = CaptureProbe(output);
            DestroyWindow(hwnd_);
            outcome_.exit_code = captured ? 0 : 1;
            return outcome_;
        }
    }
#endif
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    SetTimer(hwnd_, 1, 16, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        // Tab/Enter/Esc inside an edit go to the wizard's keyboard handling.
        if (m.message == WM_KEYDOWN && m.hwnd != hwnd_ &&
            (m.wParam == VK_TAB || m.wParam == VK_RETURN || m.wParam == VK_ESCAPE)) {
            focus_ = m.hwnd == app_edit_ ? kAppEdit : kIndexEdit;
            if (m.wParam == VK_RETURN) focus_ = kNone;
            SendMessageW(hwnd_, WM_KEYDOWN, m.wParam, m.lParam);
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (worker_.joinable()) {
        // Closing during extraction cancels; the commit itself is never interrupted.
        cancel_ = true;
        paused_ = false;
        busy_retry_ = false;
        SetEvent(busy_answer_event_);
        worker_.join();
    }
    return outcome_;
}

}  // namespace

WizardOutcome RunWizard(Payload& payload, const std::wstring& self_path, InstallRequest request, Lang lang) {
    Wizard wizard(&payload, self_path, std::move(request), lang);
    return wizard.Run();
}

WizardOutcome RunUninstallWizard(const std::wstring& self_path, const UninstallRequest& request, Lang lang) {
    InstallRequest options_only;
    options_only.options = request.options;
    Wizard wizard(nullptr, self_path, std::move(options_only), lang, &request);
    return wizard.Run();
}

}  // namespace pulse::setup

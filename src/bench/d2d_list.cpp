// d2d_list.cpp — Pulse Stage 0: Direct2D virtual list visual/perf sample.
// Fluent details view: 100k fake rows, Mica, rounded corners, Snap Layouts.
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <vector>
#include <string>
#include <string_view>
#include <chrono>
#include <random>
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

// ---------------------------------------------------------------------------
// COM helpers
// ---------------------------------------------------------------------------
template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept { p = o.p; o.p = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept { if (p) p->Release(); p = o.p; o.p = nullptr; return *this; }
    T* operator->() const { return p; }
    T** operator&() { return &p; }
    T* get() const { return p; }
    void reset() { if (p) p->Release(); p = nullptr; }
};

// ---------------------------------------------------------------------------
// App constants / types
// ---------------------------------------------------------------------------
constexpr int kItemCount = 100'000;
constexpr float kBaseRowHeight = 28.0f;
constexpr float kBaseHeaderHeight = 36.0f;
constexpr float kBaseMinScrollbarWidth = 6.0f;
constexpr float kBaseMaxScrollbarWidth = 12.0f;
constexpr float kBaseSelectionBarWidth = 3.0f;
constexpr float kBaseMargin = 8.0f;

enum class ThemeMode { Auto, Light, Dark };

struct Item {
    wchar_t name[80];
    uint64_t size;
    FILETIME mtime;
    bool is_dir;
};

struct Theme {
    D2D1_COLOR_F bg;
    D2D1_COLOR_F text;
    D2D1_COLOR_F text_secondary;
    D2D1_COLOR_F header_bg;
    D2D1_COLOR_F header_sep;
    D2D1_COLOR_F row_bg[2];
    D2D1_COLOR_F hover_bg;
    D2D1_COLOR_F selection_bg;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F scrollbar_bg;
    D2D1_COLOR_F scrollbar_thumb;
    D2D1_COLOR_F icon_folder;
    D2D1_COLOR_F icon_file;
    D2D1_COLOR_F fps_bg;
    D2D1_COLOR_F fps_text;
};

struct AppState {
    HWND hwnd = nullptr;

    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<IDXGIDevice1> dxgiDevice;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID2D1Factory3> d2dFactory;
    ComPtr<ID2D1Device2> d2dDevice;
    ComPtr<ID2D1DeviceContext2> dc;
    ComPtr<ID2D1Bitmap1> targetBitmap;
    ComPtr<IDWriteFactory3> dwriteFactory;
    ComPtr<IDWriteTextFormat> textFormat;
    ComPtr<IDWriteTextFormat> smallFormat;
    ComPtr<IDWriteTextFormat> headerFormat;

    UINT dpiX = 96, dpiY = 96;
    float scale = 1.0f;
    int clientW = 0, clientH = 0;

    bool darkMode = false;
    ThemeMode themeOverride = ThemeMode::Auto;
    D2D1_COLOR_F accentColor;

    std::vector<Item> items;
    float scrollY = 0.0f;
    int selectedIndex = 0;
    int hoverIndex = -1;
    bool scrollbarHovered = false;
    bool scrollbarDragging = false;
    int scrollbarDragStartY = 0;
    float scrollbarDragStartScroll = 0.0f;
    float scrollbarHoverWidth = 0.0f;

    bool showFps = true;
    double lastFrameMs = 0.0;
    double lastFps = 0.0;
    std::chrono::steady_clock::time_point lastFrameTime;

    ComPtr<ID2D1SolidColorBrush> brBg;
    ComPtr<ID2D1SolidColorBrush> brText;
    ComPtr<ID2D1SolidColorBrush> brTextSecondary;
    ComPtr<ID2D1SolidColorBrush> brHeaderBg;
    ComPtr<ID2D1SolidColorBrush> brHeaderSep;
    ComPtr<ID2D1SolidColorBrush> brHover;
    ComPtr<ID2D1SolidColorBrush> brSelection;
    ComPtr<ID2D1SolidColorBrush> brAccent;
    ComPtr<ID2D1SolidColorBrush> brScrollbar;
    ComPtr<ID2D1SolidColorBrush> brScrollbarBg;
    ComPtr<ID2D1SolidColorBrush> brIconFolder;
    ComPtr<ID2D1SolidColorBrush> brIconFile;
    ComPtr<ID2D1SolidColorBrush> brFpsBg;
    ComPtr<ID2D1SolidColorBrush> brFpsText;
};

// ---------------------------------------------------------------------------
// Light/dark tokens
// ---------------------------------------------------------------------------
static D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(
        ((rgb >> 16) & 0xFF) / 255.0f,
        ((rgb >> 8) & 0xFF) / 255.0f,
        (rgb & 0xFF) / 255.0f,
        a);
}

static Theme MakeTheme(bool dark, D2D1_COLOR_F accent) {
    Theme t;
    t.accent = accent;
    if (dark) {
        t.bg              = Hex(0x1E1E1E, 1.00f);
        t.text            = Hex(0xFFFFFF, 1.00f);
        t.text_secondary  = Hex(0xA0A0A0, 1.00f);
        t.header_bg       = Hex(0x2C2C2C, 1.00f);
        t.header_sep      = Hex(0x3C3C3C, 0.80f);
        t.row_bg[0]       = Hex(0x000000, 0.00f);
        t.row_bg[1]       = Hex(0xFFFFFF, 0.015f);
        t.hover_bg        = Hex(0xFFFFFF, 0.08f);
        t.selection_bg    = D2D1::ColorF(accent.r, accent.g, accent.b, 0.15f);
        t.scrollbar_bg    = Hex(0xFFFFFF, 0.00f);
        t.scrollbar_thumb = Hex(0xFFFFFF, 0.30f);
        t.icon_folder     = Hex(0xFFCD70, 1.00f);
        t.icon_file       = Hex(0x78B0E8, 1.00f);
        t.fps_bg          = Hex(0x000000, 0.60f);
        t.fps_text        = Hex(0xFFFFFF, 1.00f);
    } else {
        t.bg              = Hex(0xF5F5F5, 1.00f);
        t.text            = Hex(0x1A1A1A, 1.00f);
        t.text_secondary  = Hex(0x5C5C5C, 1.00f);
        t.header_bg       = Hex(0xFAFAFA, 1.00f);
        t.header_sep      = Hex(0xE5E5E5, 1.00f);
        t.row_bg[0]       = Hex(0x000000, 0.00f);
        t.row_bg[1]       = Hex(0x000000, 0.015f);
        t.hover_bg        = Hex(0x000000, 0.06f);
        t.selection_bg    = D2D1::ColorF(accent.r, accent.g, accent.b, 0.12f);
        t.scrollbar_bg    = Hex(0x000000, 0.00f);
        t.scrollbar_thumb = Hex(0x000000, 0.30f);
        t.icon_folder     = Hex(0xF5B041, 1.00f);
        t.icon_file       = Hex(0x2E86DE, 1.00f);
        t.fps_bg          = Hex(0x000000, 0.55f);
        t.fps_text        = Hex(0xFFFFFF, 1.00f);
    }
    return t;
}

static bool ShouldUseDarkMode(ThemeMode override) {
    if (override == ThemeMode::Dark) return true;
    if (override == ThemeMode::Light) return false;
    // Prefer the undocumented but reliable UXTheme API on Win10 1903+.
    using Fn = bool (WINAPI*)();
    HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
    bool dark = false;
    if (uxtheme) {
        Fn should = reinterpret_cast<Fn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(132)));
        if (should) dark = should();
        FreeLibrary(uxtheme);
        if (should) return dark;
    }
    // Fallback to the registry value (0 = dark, 1 = light).
    DWORD value = 1;
    DWORD size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}

static D2D1_COLOR_F GetAccentColor() {
    DWORD color = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque))) {
        return D2D1::ColorF(
            ((color >> 16) & 0xFF) / 255.0f,
            ((color >> 8) & 0xFF) / 255.0f,
            (color & 0xFF) / 255.0f,
            1.0f);
    }
    return Hex(0x0078D4);
}

static void UpdateWindowTheme(HWND hwnd, bool dark) {
    BOOL darkValue = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkValue, sizeof(darkValue));
}

// ---------------------------------------------------------------------------
// Fake data
// ---------------------------------------------------------------------------
static void GenerateItems(std::vector<Item>& items) {
    items.clear();
    items.reserve(kItemCount);
    const wchar_t* exts[] = { L"dwg", L"txt", L"pdf", L"docx", L"xlsx", L"png", L"jpg", L"zip" };
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> extDist(0, 7);
    std::uniform_int_distribution<int> dirDist(0, 19);
    std::uniform_int_distribution<uint64_t> sizeDist(1, 1024ULL * 1024 * 500);

    SYSTEMTIME st;
    GetSystemTime(&st);
    FILETIME now;
    SystemTimeToFileTime(&st, &now);
    int64_t nowTicks = (int64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;

    for (int i = 0; i < kItemCount; ++i) {
        Item it{};
        bool isDir = dirDist(rng) == 0;
        it.is_dir = isDir;
        if (isDir) {
            swprintf_s(it.name, L"Folder_%05d", i);
            it.size = 0;
        } else {
            swprintf_s(it.name, L"report_%05d.%s", i, exts[extDist(rng)]);
            it.size = sizeDist(rng);
        }
        int64_t t = nowTicks - int64_t(i) * 3600LL * 10000000LL;
        it.mtime.dwLowDateTime = (DWORD)(t & 0xFFFFFFFF);
        it.mtime.dwHighDateTime = (DWORD)(t >> 32);
        items.push_back(it);
    }
}

static std::wstring FormatSize(uint64_t size) {
    if (size == 0) return L"";
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    int unit = 0;
    double s = (double)size;
    while (s >= 1024.0 && unit < 4) { s /= 1024.0; ++unit; }
    wchar_t buf[64];
    if (unit == 0) swprintf_s(buf, L"%llu %s", size, units[unit]);
    else swprintf_s(buf, L"%.1f %s", s, units[unit]);
    return buf;
}

static std::wstring FormatDate(const FILETIME& ft) {
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    wchar_t buf[64];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

// ---------------------------------------------------------------------------
// D2D/D3D/DWrite init
// ---------------------------------------------------------------------------
static bool InitD3D(AppState& s) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &s.d3dDevice, nullptr, nullptr);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            &s.d3dDevice, nullptr, nullptr);
    }
    if (FAILED(hr)) return false;

    s.d3dDevice->QueryInterface(&s.dxgiDevice);
    if (!s.dxgiDevice.get()) return false;

    D2D1_FACTORY_OPTIONS opts{};
#ifdef _DEBUG
    opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory3),
        &opts, reinterpret_cast<void**>(&s.d2dFactory));
    if (FAILED(hr)) return false;

    hr = s.d2dFactory->CreateDevice(s.dxgiDevice.get(), &s.d2dDevice);
    if (FAILED(hr)) return false;

    hr = s.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &s.dc);
    if (FAILED(hr)) return false;

    s.dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    s.dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3),
        reinterpret_cast<IUnknown**>(&s.dwriteFactory));
    if (FAILED(hr)) return false;

    auto createFormat = [&](float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& fmt) {
        s.dwriteFactory->CreateTextFormat(L"Segoe UI Variable", nullptr, weight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &fmt);
        if (!fmt.get()) {
            s.dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, weight,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &fmt);
        }
        if (fmt.get()) {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            fmt->SetTrimming(&trimming, nullptr);
        }
    };
    createFormat(12.0f * s.scale, DWRITE_FONT_WEIGHT_NORMAL, s.textFormat);
    createFormat(10.0f * s.scale, DWRITE_FONT_WEIGHT_NORMAL, s.smallFormat);
    createFormat(11.0f * s.scale, DWRITE_FONT_WEIGHT_SEMI_BOLD, s.headerFormat);

    return true;
}

static bool CreateSwapChain(AppState& s) {
    ComPtr<IDXGIAdapter> adapter;
    s.dxgiDevice->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = (UINT)s.clientW;
    desc.Height = (UINT)s.clientH;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    HRESULT hr = factory->CreateSwapChainForHwnd(s.d3dDevice.get(), s.hwnd, &desc,
        nullptr, nullptr, &s.swapChain);
    if (FAILED(hr)) return false;

    s.dxgiDevice->SetMaximumFrameLatency(1);
    return true;
}

static void ResizeSwapChain(AppState& s) {
    if (!s.swapChain.get() || s.clientW <= 0 || s.clientH <= 0) return;
    s.targetBitmap.reset();
    s.dc->SetTarget(nullptr);
    HRESULT hr = s.swapChain->ResizeBuffers(0, (UINT)s.clientW, (UINT)s.clientH,
        DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) return;

    ComPtr<IDXGISurface> surface;
    s.swapChain->GetBuffer(0, IID_PPV_ARGS(&surface));
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        (float)s.dpiX, (float)s.dpiY);
    s.dc->CreateBitmapFromDxgiSurface(surface.get(), &props, &s.targetBitmap);
    s.dc->SetTarget(s.targetBitmap.get());
}

static void RecreateTextFormats(AppState& s) {
    s.textFormat.reset();
    s.smallFormat.reset();
    s.headerFormat.reset();
    auto createFormat = [&](float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& fmt) {
        s.dwriteFactory->CreateTextFormat(L"Segoe UI Variable", nullptr, weight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &fmt);
        if (!fmt.get()) {
            s.dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, weight,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &fmt);
        }
        if (fmt.get()) {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            fmt->SetTrimming(&trimming, nullptr);
        }
    };
    createFormat(12.0f * s.scale, DWRITE_FONT_WEIGHT_NORMAL, s.textFormat);
    createFormat(10.0f * s.scale, DWRITE_FONT_WEIGHT_NORMAL, s.smallFormat);
    createFormat(11.0f * s.scale, DWRITE_FONT_WEIGHT_SEMI_BOLD, s.headerFormat);
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
static void FillRect(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, float x, float y, float w, float h) {
    dc->FillRectangle(D2D1::RectF(x, y, x + w, y + h), br);
}

static void DrawText(ID2D1DeviceContext* dc, IDWriteTextFormat* fmt, ID2D1SolidColorBrush* br,
    std::wstring_view text, float x, float y, float w, float h) {
    D2D1_RECT_F rc = D2D1::RectF(x, y, x + w, y + h);
    dc->DrawText(text.data(), (UINT32)text.size(), fmt, &rc, br,
        D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

static void RoundedRect(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br,
    float x, float y, float w, float h, float r) {
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r);
    dc->FillRoundedRectangle(&rr, br);
}

// ---------------------------------------------------------------------------
// Layout / scroll helpers
// ---------------------------------------------------------------------------
static float RowHeight(AppState& s) { return kBaseRowHeight * s.scale; }
static float HeaderHeight(AppState& s) { return kBaseHeaderHeight * s.scale; }
static float Margin(AppState& s) { return kBaseMargin * s.scale; }

static float TotalContentHeight(AppState& s) {
    return (float)s.items.size() * RowHeight(s);
}

static float MaxScroll(AppState& s) {
    float view = (float)s.clientH - HeaderHeight(s);
    float total = TotalContentHeight(s);
    return std::max(0.0f, total - view);
}

static void ClampScroll(AppState& s) {
    s.scrollY = std::clamp(s.scrollY, 0.0f, MaxScroll(s));
}

static void ScrollBy(AppState& s, float dy) {
    s.scrollY += dy;
    ClampScroll(s);
}

static int HitRow(AppState& s, int y) {
    if (y < (int)HeaderHeight(s)) return -1;
    float rel = (float)y - HeaderHeight(s) + s.scrollY;
    int idx = (int)(rel / RowHeight(s));
    if (idx < 0 || idx >= (int)s.items.size()) return -1;
    return idx;
}

struct ScrollbarMetrics {
    float x, w, trackH, thumbY, thumbH;
    bool valid;
};

static ScrollbarMetrics ComputeScrollbar(AppState& s) {
    ScrollbarMetrics m{};
    m.valid = false;
    float viewH = (float)s.clientH - HeaderHeight(s);
    float totalH = TotalContentHeight(s);
    if (totalH <= viewH || viewH <= 0) return m;
    m.valid = true;
    float targetW = kBaseMinScrollbarWidth * s.scale + s.scrollbarHoverWidth;
    m.w = targetW;
    m.x = (float)s.clientW - Margin(s) - m.w;
    m.trackH = viewH;
    m.thumbH = std::max(RowHeight(s), viewH * (viewH / totalH));
    m.thumbY = HeaderHeight(s) + (s.scrollY / (totalH - viewH)) * (viewH - m.thumbH);
    return m;
}

static bool HitScrollbar(AppState& s, int x, int y, ScrollbarMetrics* out = nullptr) {
    auto m = ComputeScrollbar(s);
    if (!m.valid) return false;
    if (out) *out = m;
    return x >= m.x - 4 && x < (float)s.clientW && y >= HeaderHeight(s) && y < (float)s.clientH;
}

// ---------------------------------------------------------------------------
// Main render
// ---------------------------------------------------------------------------
static void Render(AppState& s) {
    auto t0 = std::chrono::steady_clock::now();

    // Sync client size right before drawing (handles DPI / resize without racing WM_SIZE).
    if (s.hwnd) {
        RECT rc;
        GetClientRect(s.hwnd, &rc);
        s.clientW = rc.right;
        s.clientH = rc.bottom;
    }

    s.dc->BeginDraw();
    s.dc->Clear(D2D1::ColorF(0, 0));

    Theme theme = MakeTheme(s.darkMode, s.accentColor);

    auto makeBr = [&](const D2D1_COLOR_F& c, ComPtr<ID2D1SolidColorBrush>& br) {
        if (!br.get()) s.dc->CreateSolidColorBrush(c, &br);
        else br->SetColor(c);
    };
    makeBr(theme.bg, s.brBg);
    makeBr(theme.text, s.brText);

    // Solid content background (stage 0 fallback; Mica is still requested from DWM).
    FillRect(s.dc.get(), s.brBg.get(), 0, 0, (float)s.clientW, (float)s.clientH);

    makeBr(theme.text_secondary, s.brTextSecondary);
    makeBr(theme.header_bg, s.brHeaderBg);
    makeBr(theme.header_sep, s.brHeaderSep);
    makeBr(theme.hover_bg, s.brHover);
    makeBr(theme.selection_bg, s.brSelection);
    makeBr(theme.accent, s.brAccent);
    makeBr(theme.scrollbar_thumb, s.brScrollbar);
    makeBr(theme.scrollbar_bg, s.brScrollbarBg);
    makeBr(theme.icon_folder, s.brIconFolder);
    makeBr(theme.icon_file, s.brIconFile);
    makeBr(theme.fps_bg, s.brFpsBg);
    makeBr(theme.fps_text, s.brFpsText);

    float headerH = HeaderHeight(s);
    float rowH = RowHeight(s);
    float margin = Margin(s);

    FillRect(s.dc.get(), s.brHeaderBg.get(), 0, 0, (float)s.clientW, headerH);
    FillRect(s.dc.get(), s.brHeaderSep.get(), 0, headerH - 1, (float)s.clientW, 1);

    float colX[4];
    colX[0] = margin;
    colX[1] = margin + 28 * s.scale;
    colX[2] = (float)s.clientW * 0.40f;
    colX[3] = (float)s.clientW * 0.58f;

    DrawText(s.dc.get(), s.headerFormat.get(), s.brText.get(), L"Name", colX[1], 0, colX[2] - colX[1] - margin, headerH - 1);
    DrawText(s.dc.get(), s.headerFormat.get(), s.brText.get(), L"Size", colX[2], 0, colX[3] - colX[2] - margin, headerH - 1);
    DrawText(s.dc.get(), s.headerFormat.get(), s.brText.get(), L"Modified", colX[3], 0, 200, headerH - 1);

    float viewH = (float)s.clientH - headerH;
    int startIdx = (int)std::floor(s.scrollY / rowH);
    int endIdx = (int)std::ceil((s.scrollY + viewH) / rowH) + 1;
    startIdx = std::max(0, startIdx - 3);
    endIdx = std::min((int)s.items.size() - 1, endIdx + 3);

    for (int i = startIdx; i <= endIdx; ++i) {
        float y = headerH + i * rowH - s.scrollY;
        float x = 0;
        float w = (float)s.clientW;

        D2D1_COLOR_F zebra = theme.row_bg[i & 1];
        if (zebra.a > 0) {
            ComPtr<ID2D1SolidColorBrush> brZebra;
            s.dc->CreateSolidColorBrush(zebra, &brZebra);
            FillRect(s.dc.get(), brZebra.get(), x, y, w, rowH);
        }

        if (i == s.hoverIndex) {
            FillRect(s.dc.get(), s.brHover.get(), x, y, w, rowH);
        }
        if (i == s.selectedIndex) {
            FillRect(s.dc.get(), s.brSelection.get(), x, y, w, rowH);
            float barW = kBaseSelectionBarWidth * s.scale;
            FillRect(s.dc.get(), s.brAccent.get(), x, y, barW, rowH);
        }

        const Item& it = s.items[i];
        float iconSize = 16 * s.scale;
        float iconY = y + (rowH - iconSize) * 0.5f;
        RoundedRect(s.dc.get(), it.is_dir ? s.brIconFolder.get() : s.brIconFile.get(),
            colX[0], iconY, iconSize, iconSize, 3 * s.scale);

        DrawText(s.dc.get(), s.textFormat.get(), s.brText.get(), it.name,
            colX[1], y + 1, colX[2] - colX[1] - margin, rowH - 1);
        std::wstring sizeStr = FormatSize(it.size);
        DrawText(s.dc.get(), s.textFormat.get(), s.brTextSecondary.get(), sizeStr,
            colX[2], y + 1, colX[3] - colX[2] - margin, rowH - 1);
        std::wstring dateStr = FormatDate(it.mtime);
        DrawText(s.dc.get(), s.textFormat.get(), s.brTextSecondary.get(), dateStr,
            colX[3], y + 1, (float)s.clientW - colX[3] - margin, rowH - 1);
    }

    // Scrollbar
    auto sb = ComputeScrollbar(s);
    if (sb.valid) {
        RoundedRect(s.dc.get(), s.brScrollbar.get(), sb.x, sb.thumbY, sb.w, sb.thumbH,
            sb.w * 0.5f);
    }

    // FPS overlay (top-left to stay inside screenshot bounds)
    if (s.showFps) {
        wchar_t buf[128];
        swprintf_s(buf, L"%.2f ms  %.1f FPS", s.lastFrameMs, s.lastFps);
        std::wstring txt = buf;
        ComPtr<IDWriteTextLayout> layout;
        s.dwriteFactory->CreateTextLayout(txt.data(), (UINT32)txt.size(), s.smallFormat.get(),
            1000, 100, &layout);
        DWRITE_TEXT_METRICS m{};
        if (layout.get() && SUCCEEDED(layout->GetMetrics(&m))) {
            float pad = 4 * s.scale;
            float fw = m.width + pad * 2;
            float fh = m.height + pad * 2;
            float fx = margin;
            float fy = headerH + margin;
            RoundedRect(s.dc.get(), s.brFpsBg.get(), fx, fy, fw, fh, 4 * s.scale);
            DrawText(s.dc.get(), s.smallFormat.get(), s.brFpsText.get(), txt,
                fx + pad, fy + 1, m.width, fh);
        }
    }

    s.dc->EndDraw();
    s.swapChain->Present(1, 0);

    auto t1 = std::chrono::steady_clock::now();
    s.lastFrameMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double dt = std::chrono::duration<double>(t1 - s.lastFrameTime).count();
    if (dt > 0.0) s.lastFps = 1.0 / dt;
    s.lastFrameTime = t1;
}

// ---------------------------------------------------------------------------
// Snapshot to PNG (WIC)
// ---------------------------------------------------------------------------
static bool SaveSnapshot(AppState& s, const wchar_t* path) {
    if (!s.swapChain.get() || !s.d3dDevice.get()) return false;

    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = s.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;

    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);

    ComPtr<ID3D11DeviceContext> ctx;
    s.d3dDevice->GetImmediateContext(&ctx);

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    hr = s.d3dDevice->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(hr)) return false;

    ctx->CopyResource(staging.get(), backBuffer.get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    ComPtr<IWICImagingFactory> wic;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic));
    if (FAILED(hr)) { ctx->Unmap(staging.get(), 0); return false; }

    ComPtr<IWICStream> stream;
    hr = wic->CreateStream(&stream);
    if (FAILED(hr)) { ctx->Unmap(staging.get(), 0); return false; }
    hr = stream->InitializeFromFilename(path, GENERIC_WRITE);
    if (FAILED(hr)) { ctx->Unmap(staging.get(), 0); return false; }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) { ctx->Unmap(staging.get(), 0); return false; }
    hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) { ctx->Unmap(staging.get(), 0); return false; }

    ComPtr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(hr)) { ctx->Unmap(staging.get(), 0); return false; }
    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) { ctx->Unmap(staging.get(), 0); return false; }

    hr = frame->SetSize(desc.Width, desc.Height);
    if (FAILED(hr)) { ctx->Unmap(staging.get(), 0); return false; }

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr)) { ctx->Unmap(staging.get(), 0); return false; }

    hr = frame->WritePixels(desc.Height, mapped.RowPitch, mapped.RowPitch * desc.Height,
        static_cast<BYTE*>(mapped.pData));
    ctx->Unmap(staging.get(), 0);
    if (FAILED(hr)) return false;

    hr = frame->Commit();
    if (FAILED(hr)) return false;
    hr = encoder->Commit();
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        s = reinterpret_cast<AppState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        s->hwnd = hwnd;

        s->dpiX = s->dpiY = GetDpiForWindow(hwnd);
        s->scale = (float)s->dpiX / 96.0f;

        s->accentColor = GetAccentColor();
        s->darkMode = ShouldUseDarkMode(s->themeOverride);
        UpdateWindowTheme(hwnd, s->darkMode);

        BOOL round = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &round, sizeof(round));
        DWORD backdrop = DWMSBT_MAINWINDOW;
        DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));

        MARGINS margins = { 0, 0, 1, 0 };
        DwmExtendFrameIntoClientArea(hwnd, &margins);

        RECT rc;
        GetClientRect(hwnd, &rc);
        s->clientW = rc.right;
        s->clientH = rc.bottom;

        if (!InitD3D(*s) || !CreateSwapChain(*s)) {
            MessageBoxW(hwnd, L"Failed to initialize D3D/D2D/DWrite", L"Pulse", MB_OK);
            return -1;
        }
        ResizeSwapChain(*s);
        GenerateItems(s->items);
        s->selectedIndex = 0;
        s->lastFrameTime = std::chrono::steady_clock::now();
        SetTimer(hwnd, 1, 16, nullptr);
        return 0;
    }

    case WM_DPICHANGED: {
        s->dpiX = s->dpiY = HIWORD(wParam);
        s->scale = (float)s->dpiX / 96.0f;
        RECT* const rc = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, rc->left, rc->top,
            rc->right - rc->left, rc->bottom - rc->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        RecreateTextFormats(*s);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_SIZE: {
        if (s) {
            s->clientW = LOWORD(lParam);
            s->clientH = HIWORD(lParam);
            if (s->swapChain.get()) ResizeSwapChain(*s);
            ClampScroll(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (s) Render(*s);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_TIMER: {
        if (s && wParam == 1) {
            float target = s->scrollbarHovered ? (kBaseMaxScrollbarWidth - kBaseMinScrollbarWidth) * s->scale : 0.0f;
            float step = (target - s->scrollbarHoverWidth) * 0.25f;
            if (std::abs(step) > 0.1f) {
                s->scrollbarHoverWidth += step;
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (s->scrollbarHoverWidth != target) {
                s->scrollbarHoverWidth = target;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!s) break;
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);

        bool sbHit = HitScrollbar(*s, x, y);
        if (sbHit != s->scrollbarHovered) {
            s->scrollbarHovered = sbHit;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (s->scrollbarDragging) {
            auto m = ComputeScrollbar(*s);
            if (m.valid) {
                float viewH = (float)s->clientH - HeaderHeight(*s);
                float totalH = TotalContentHeight(*s);
                float trackLen = m.trackH - m.thumbH;
                float dy = (float)(y - s->scrollbarDragStartY);
                float frac = dy / trackLen;
                s->scrollY = s->scrollbarDragStartScroll + frac * (totalH - viewH);
                ClampScroll(*s);
                s->scrollbarDragStartY = y;
                s->scrollbarDragStartScroll = s->scrollY;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else {
            int idx = HitRow(*s, y);
            if (idx != s->hoverIndex) {
                s->hoverIndex = idx;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        if (s) {
            s->scrollbarHovered = false;
            s->hoverIndex = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (!s) break;
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        if (HitScrollbar(*s, x, y)) {
            s->scrollbarDragging = true;
            s->scrollbarDragStartY = y;
            s->scrollbarDragStartScroll = s->scrollY;
            SetCapture(hwnd);
        } else {
            int idx = HitRow(*s, y);
            if (idx >= 0) {
                s->selectedIndex = idx;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (s && s->scrollbarDragging) {
            s->scrollbarDragging = false;
            ReleaseCapture();
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (!s) break;
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        float lines = (float)delta / (float)WHEEL_DELTA;
        ScrollBy(*s, -lines * RowHeight(*s) * 3.0f);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN: {
        if (!s) break;
        switch (wParam) {
        case VK_DOWN:
            if (s->selectedIndex < (int)s->items.size() - 1) {
                s->selectedIndex++;
                ScrollBy(*s, RowHeight(*s));
            }
            break;
        case VK_UP:
            if (s->selectedIndex > 0) {
                s->selectedIndex--;
                ScrollBy(*s, -RowHeight(*s));
            }
            break;
        case VK_NEXT:
            ScrollBy(*s, ((float)s->clientH - HeaderHeight(*s)) * 0.8f);
            break;
        case VK_PRIOR:
            ScrollBy(*s, -((float)s->clientH - HeaderHeight(*s)) * 0.8f);
            break;
        case VK_HOME:
            s->scrollY = 0;
            s->selectedIndex = 0;
            break;
        case VK_END:
            s->scrollY = MaxScroll(*s);
            s->selectedIndex = (int)s->items.size() - 1;
            break;
        case VK_SPACE:
            if (s->themeOverride == ThemeMode::Auto) s->themeOverride = ThemeMode::Dark;
            else if (s->themeOverride == ThemeMode::Dark) s->themeOverride = ThemeMode::Light;
            else s->themeOverride = ThemeMode::Auto;
            s->darkMode = ShouldUseDarkMode(s->themeOverride);
            UpdateWindowTheme(hwnd, s->darkMode);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case VK_F1:
            s->showFps = !s->showFps;
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
        ClampScroll(*s);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_DESTROY:
        if (s) s->hwnd = nullptr;
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Parse command line.
    const wchar_t* shotPath = nullptr;
    bool forceDark = false;
    for (int i = 1; i < __argc; ++i) {
        if (wcscmp(__wargv[i], L"--shot") == 0 && i + 1 < __argc) {
            shotPath = __wargv[++i];
        } else if (wcscmp(__wargv[i], L"--dark") == 0) {
            forceDark = true;
        }
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"PulseD2DList";
    RegisterClassExW(&wc);

    AppState state;
    if (forceDark) state.themeOverride = ThemeMode::Dark;

    HWND hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName,
        L"Pulse — D2D Virtual List",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        (int)(1600 * state.scale), (int)(960 * state.scale),
        nullptr, nullptr, hInstance, &state);

    if (!hwnd) return 1;

    ShowWindow(hwnd, shotPath ? SW_SHOWNORMAL : nCmdShow);
    UpdateWindow(hwnd);

    if (shotPath) {
        // Let DWM compose the frame (Mica needs a couple of compositor passes).
        MSG msg{};
        for (int i = 0; i < 20; ++i) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(40);
        }
        Render(state);
        bool ok = SaveSnapshot(state, shotPath);
        DestroyWindow(hwnd);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return ok ? 0 : 1;
    }

    // Interactive mode: render only when invalidated.
    MSG msg{};
    BOOL ret;
    while ((ret = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (ret == -1) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

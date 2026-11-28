// Renders every self-drawn Dock icon on light and dark backdrops and writes PNG contact sheets,
// so icon artwork can be reviewed (and regressions spotted) without launching the desktop host.
// Also sanity-checks that each icon paints its slot and nothing outside it.
//
// Deliberately COM-free (D2D DC render target on a GDI DIB + a tiny stored-deflate PNG writer),
// so it runs in locked-down environments where CoInitialize/WIC are unavailable.
#include "host/render/liquid_dock_bar.h"

#include <d2d1_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace render = pulse::desktop::render;

namespace {

struct IconCase {
    const wchar_t* name;
    std::function<void(ID2D1DeviceContext*, const D2D1_RECT_F&)> draw;
};

uint32_t Crc32(const uint8_t* data, size_t n, uint32_t crc = 0) {
    static uint32_t table[256] = {};
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

void PutBE(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& body) {
    PutBE(out, static_cast<uint32_t>(body.size()));
    std::vector<uint8_t> tb(type, type + 4);
    tb.insert(tb.end(), body.begin(), body.end());
    out.insert(out.end(), tb.begin(), tb.end());
    PutBE(out, Crc32(tb.data(), tb.size()));
}

// Opaque RGB PNG from a top-down BGRA buffer (zlib stream with stored deflate blocks).
bool WritePng(const std::filesystem::path& path, int w, int h, const uint8_t* bgra, int stride) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (static_cast<size_t>(w) * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        const uint8_t* row = bgra + static_cast<size_t>(y) * static_cast<size_t>(stride);
        for (int x = 0; x < w; ++x) {
            raw.push_back(row[x * 4 + 2]);
            raw.push_back(row[x * 4 + 1]);
            raw.push_back(row[x * 4 + 0]);
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t pos = 0;
    while (pos < raw.size() || raw.empty()) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n & 0xFF));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n & 0xFF));
        z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521u;
        b = (b + a) % 65521u;
    }
    PutBE(z, (b << 16) | a);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    PutBE(ihdr, static_cast<uint32_t>(w));
    PutBE(ihdr, static_cast<uint32_t>(h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    Chunk(png, "IHDR", ihdr);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", {});
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return f.good();
}

struct Placement {
    size_t icon;
    float size;
    float x;
    float y;
};

struct Sheet {
    const wchar_t* file;
    bool dark;
    int width;
    int height;
    std::vector<Placement> cells;
};

// Draws one sheet into a DIB through a D2D DC render target, checks coverage, writes the PNG.
int RenderSheet(ID2D1Factory1* d2d, const std::vector<IconCase>& icons, const Sheet& sheet,
                const std::filesystem::path& out_dir) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = sheet.width;
    bi.bmiHeader.biHeight = -sheet.height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(nullptr);
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!mem || !dib || !bits) {
        std::fwprintf(stderr, L"DIB creation failed\n");
        return 1;
    }
    HGDIOBJ old = SelectObject(mem, dib);

    int failures = 0;
    {
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        ComPtr<ID2D1DCRenderTarget> rt;
        ComPtr<ID2D1DeviceContext> dc;
        RECT rc{0, 0, sheet.width, sheet.height};
        if (FAILED(d2d->CreateDCRenderTarget(&props, &rt)) || !rt || FAILED(rt->BindDC(mem, &rc)) ||
            FAILED(rt.As(&dc)) || !dc) {
            std::fwprintf(stderr, L"DC render target creation failed\n");
            return 1;
        }
        dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->BeginDraw();
        dc->Clear(sheet.dark ? D2D1::ColorF(0.11f, 0.12f, 0.15f, 1.0f) : D2D1::ColorF(0.93f, 0.94f, 0.96f, 1.0f));
        for (const Placement& p : sheet.cells) {
            icons[p.icon].draw(dc.Get(), D2D1::RectF(p.x, p.y, p.x + p.size, p.y + p.size));
        }
        if (FAILED(dc->EndDraw())) {
            std::fwprintf(stderr, L"EndDraw failed\n");
            return 1;
        }
    }
    GdiFlush();

    const auto* px = static_cast<const uint8_t*>(bits);
    const int stride = sheet.width * 4;
    const uint8_t* bg = px + 2 * stride + 2 * 4;
    for (const Placement& p : sheet.cells) {
        if (p.size < 128.0f) continue;
        const int l = static_cast<int>(p.x), t = static_cast<int>(p.y), s = static_cast<int>(p.size);
        int changed = 0, outside = 0;
        for (int yy = t - 8; yy < t + s + 8; ++yy) {
            for (int xx = l - 8; xx < l + s + 8; ++xx) {
                if (yy < 0 || xx < 0 || yy >= sheet.height || xx >= sheet.width) continue;
                const uint8_t* q = px + static_cast<size_t>(yy) * static_cast<size_t>(stride) + static_cast<size_t>(xx) * 4;
                if (std::abs(q[0] - bg[0]) + std::abs(q[1] - bg[1]) + std::abs(q[2] - bg[2]) <= 6) continue;
                if (yy >= t - 1 && yy <= t + s && xx >= l - 1 && xx <= l + s) ++changed; else ++outside;
            }
        }
        const double coverage = static_cast<double>(changed) / (static_cast<double>(s) * s);
        const bool ok = coverage > 0.25 && outside < 32;
        std::wprintf(L"%-11ls %-5ls %3dpx coverage=%.2f outside=%d %ls\n", icons[p.icon].name,
                     sheet.dark ? L"dark" : L"light", s, coverage, outside, ok ? L"ok" : L"FAIL");
        if (!ok) ++failures;
    }

    const std::filesystem::path file = out_dir / sheet.file;
    if (!WritePng(file, sheet.width, sheet.height, px, stride)) {
        std::fwprintf(stderr, L"failed to write %ls\n", file.c_str());
        ++failures;
    } else {
        std::wprintf(L"wrote %ls (%dx%d)\n", file.c_str(), sheet.width, sheet.height);
    }
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    return failures;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const std::filesystem::path out_dir = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(L"dock_icon_gallery");
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    ComPtr<ID2D1Factory1> d2d;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                 reinterpret_cast<void**>(d2d.GetAddressOf())))) {
        std::fwprintf(stderr, L"D2D1CreateFactory failed\n");
        return 3;
    }

    const render::DockBarTextFn no_text; // monogram needs DirectWrite; the tile itself is still drawn
    const std::vector<IconCase> icons = {
        {L"widgets", [](ID2D1DeviceContext* dc, const D2D1_RECT_F& r) { render::DrawDockWidgetsVectorIcon(dc, r); }},
        {L"apps", [](ID2D1DeviceContext* dc, const D2D1_RECT_F& r) { render::DrawDockAppsVectorIcon(dc, r); }},
        {L"settings", [](ID2D1DeviceContext* dc, const D2D1_RECT_F& r) { render::DrawDockSettingsVectorIcon(dc, r); }},
        {L"terminal", [](ID2D1DeviceContext* dc, const D2D1_RECT_F& r) { render::DrawDockTerminalVectorIcon(dc, r); }},
        {L"calculator", [](ID2D1DeviceContext* dc, const D2D1_RECT_F& r) { render::DrawDockCalculatorVectorIcon(dc, r); }},
        {L"explorer", [](ID2D1DeviceContext* dc, const D2D1_RECT_F& r) { render::DrawDockExplorerVectorIcon(dc, r); }},
        {L"generic", [&](ID2D1DeviceContext* dc, const D2D1_RECT_F& r) { render::DrawDockGenericAppVectorIcon(dc, r, L"Notes", no_text); }},
        {L"downloads", [](ID2D1DeviceContext* dc, const D2D1_RECT_F& r) { render::DrawDockFolderVectorIcon(dc, r, true); }},
        {L"folder", [](ID2D1DeviceContext* dc, const D2D1_RECT_F& r) { render::DrawDockFolderVectorIcon(dc, r, false); }},
        {L"trash", [](ID2D1DeviceContext* dc, const D2D1_RECT_F& r) { render::DrawDockTrashVectorIcon(dc, r, false); }},
        {L"trash_full", [](ID2D1DeviceContext* dc, const D2D1_RECT_F& r) { render::DrawDockTrashVectorIcon(dc, r, true); }},
    };
    const size_t n = icons.size();

    // Overview sheets: a 64px Dock row, a 48px Dock row, then every icon at 160px (6 per row).
    const auto overview = [&](const wchar_t* file, bool dark) {
        Sheet s{file, dark, 0, 0, {}};
        const float pad = 20.0f, big = 160.0f, gap_big = 20.0f;
        float y = pad;
        for (float sz : {64.0f, 48.0f}) {
            float x = pad;
            for (size_t i = 0; i < n; ++i) {
                s.cells.push_back({i, sz, x, y});
                x += sz + 14.0f;
            }
            y += sz + pad;
        }
        for (size_t i = 0; i < n; ++i) {
            const float x = pad + static_cast<float>(i % 6) * (big + gap_big);
            const float yy = y + static_cast<float>(i / 6) * (big + gap_big);
            s.cells.push_back({i, big, x, yy});
        }
        s.width = static_cast<int>(pad + 6 * (big + gap_big));
        s.height = static_cast<int>(y + 2 * (big + gap_big) + pad - gap_big);
        return s;
    };
    // Detail sheet: the four always-visible forced icons + Settings reference at 220px.
    Sheet detail{L"dock_icons_detail.png", false, 0, 0, {}};
    {
        const size_t pick[5] = {0, 1, 2, 3, 4};
        float x = 20.0f;
        for (size_t i : pick) {
            detail.cells.push_back({i, 220.0f, x, 20.0f});
            x += 240.0f;
        }
        detail.width = static_cast<int>(x);
        detail.height = 260;
    }

    int failures = 0;
    failures += RenderSheet(d2d.Get(), icons, overview(L"dock_icons_light.png", false), out_dir);
    failures += RenderSheet(d2d.Get(), icons, overview(L"dock_icons_dark.png", true), out_dir);
    failures += RenderSheet(d2d.Get(), icons, detail, out_dir);

    std::wprintf(failures == 0 ? L"dock_icon_gallery_render_test: PASS\n" : L"dock_icon_gallery_render_test: FAIL\n");
    return failures == 0 ? 0 : 1;
}

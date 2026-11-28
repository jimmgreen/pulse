#include "../ui/window_material.h"
#include "../ui/background_image_layout.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using pulse::ui::Compositor;
using pulse::ui::ComPtr;
using pulse::ui::ImageDrawRect;
using pulse::ui::ImageFit;
using pulse::ui::ImagePlacementRect;
using pulse::ui::SourceBakeKey;
using pulse::ui::WindowMaterial;

bool Report(const char* name, bool passed) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
    return passed;
}

bool WriteAll(HANDLE file, const void* data, DWORD bytes) {
    DWORD written = 0;
    return WriteFile(file, data, bytes, &written, nullptr) && written == bytes;
}

bool WriteBmp(const std::wstring& path, LONG width, LONG height, BYTE red, BYTE green, BYTE blue) {
    const DWORD row = (static_cast<DWORD>(width) * 3u + 3u) & ~3u;
    const DWORD image_bytes = row * static_cast<DWORD>(height);
    BITMAPFILEHEADER file_header{};
    file_header.bfType = 0x4D42;
    file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file_header.bfSize = file_header.bfOffBits + image_bytes;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = height;
    info.biPlanes = 1;
    info.biBitCount = 24;
    info.biCompression = BI_RGB;
    info.biSizeImage = image_bytes;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::vector<BYTE> pixels(image_bytes, 0);
    for (LONG y = 0; y < height; ++y) {
        BYTE* scan = pixels.data() + static_cast<size_t>(y) * row;
        for (LONG x = 0; x < width; ++x) {
            scan[x * 3 + 0] = blue;
            scan[x * 3 + 1] = green;
            scan[x * 3 + 2] = red;
        }
    }
    const bool ok = WriteAll(file, &file_header, sizeof(file_header)) &&
                    WriteAll(file, &info, sizeof(info)) &&
                    WriteAll(file, pixels.data(), image_bytes);
    CloseHandle(file);
    return ok;
}

// Fine stripes over a gradient: the content that would show a soft upscale.
bool WritePatternBmp(const std::wstring& path, LONG width, LONG height) {
    const DWORD row = (static_cast<DWORD>(width) * 3u + 3u) & ~3u;
    const DWORD image_bytes = row * static_cast<DWORD>(height);
    BITMAPFILEHEADER file_header{};
    file_header.bfType = 0x4D42;
    file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file_header.bfSize = file_header.bfOffBits + image_bytes;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = height;
    info.biPlanes = 1;
    info.biBitCount = 24;
    info.biCompression = BI_RGB;
    info.biSizeImage = image_bytes;
    std::vector<BYTE> pixels(image_bytes, 0);
    for (LONG y = 0; y < height; ++y) {
        BYTE* scan = pixels.data() + static_cast<size_t>(y) * row;
        for (LONG x = 0; x < width; ++x) {
            const bool stripe = ((x / 6) + (y / 6)) % 2 == 0;
            scan[x * 3 + 0] = static_cast<BYTE>(stripe ? 240 : 30);
            scan[x * 3 + 1] = static_cast<BYTE>(x * 255 / width);
            scan[x * 3 + 2] = static_cast<BYTE>(y * 255 / height);
        }
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool ok = WriteAll(file, &file_header, sizeof(file_header)) &&
                    WriteAll(file, &info, sizeof(info)) &&
                    WriteAll(file, pixels.data(), image_bytes);
    CloseHandle(file);
    return ok;
}

bool ReadPixels(ID2D1DeviceContext* dc, ID2D1Bitmap* source, std::vector<BYTE>& out) {
    const D2D1_SIZE_U size = source->GetPixelSize();
    D2D1_BITMAP_PROPERTIES1 props{};
    props.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;
    ComPtr<ID2D1Bitmap1> cpu;
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &cpu)) ||
        FAILED(cpu->CopyFromBitmap(nullptr, source, nullptr))) {
        return false;
    }
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    out.resize(static_cast<size_t>(size.width) * size.height * 4u);
    for (UINT y = 0; y < size.height; ++y)
        memcpy(out.data() + static_cast<size_t>(y) * size.width * 4u,
               mapped.bits + static_cast<size_t>(y) * mapped.pitch, size.width * 4u);
    cpu->Unmap();
    return true;
}

bool WriteCorrupt(const std::wstring& path) {
    static constexpr char bytes[] = "not an image";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool ok = WriteAll(file, bytes, static_cast<DWORD>(sizeof(bytes) - 1));
    CloseHandle(file);
    return ok;
}

ID2D1Bitmap* WaitBitmap(WindowMaterial& material, const std::wstring& path, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        if (ID2D1Bitmap* bitmap = material.SourceBitmap(path)) return bitmap;
        Sleep(10);
    }
    return nullptr;
}

bool WaitFailure(WindowMaterial& material, const std::wstring& path, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        material.SourceBitmap(path);
        if (material.DecodeFailedForTesting(path)) return true;
        Sleep(10);
    }
    return false;
}

bool WaitForInvalidation(HWND hwnd, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        if (GetUpdateRect(hwnd, nullptr, FALSE)) return true;
        Sleep(5);
    }
    return false;
}

bool HasVisiblePixels(ID2D1DeviceContext* dc, ID2D1Bitmap* source) {
    if (!dc || !source) return false;
    const D2D1_SIZE_U size = source->GetPixelSize();
    D2D1_BITMAP_PROPERTIES1 props{};
    props.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;
    ComPtr<ID2D1Bitmap1> cpu;
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &cpu)) ||
        FAILED(cpu->CopyFromBitmap(nullptr, source, nullptr))) {
        return false;
    }
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    bool visible = false;
    for (UINT y = 0; y < size.height && !visible; ++y) {
        const BYTE* row = mapped.bits + static_cast<size_t>(y) * mapped.pitch;
        for (UINT x = 0; x < size.width; ++x) {
            const BYTE* pixel = row + static_cast<size_t>(x) * 4u;
            if (pixel[0] || pixel[1] || pixel[2]) {
                visible = true;
                break;
            }
        }
    }
    cpu->Unmap();
    return visible;
}

bool Near(float a, float b, float tolerance = 0.001f) {
    return a > b ? a - b <= tolerance : b - a <= tolerance;
}

bool RectNear(const D2D1_RECT_F& rect, float left, float top, float right, float bottom) {
    return Near(rect.left, left) && Near(rect.top, top) &&
           Near(rect.right, right) && Near(rect.bottom, bottom);
}

bool RectFinite(const D2D1_RECT_F& rect) {
    return std::isfinite(rect.left) && std::isfinite(rect.top) &&
           std::isfinite(rect.right) && std::isfinite(rect.bottom);
}

// The cover formula exactly as window_material.cpp computed it before the
// layout module existed: the reference the shared code must match bit for bit.
D2D1_RECT_F LegacyCoverRect(float img_w, float img_h, const D2D1_RECT_F& dest) {
    const float dw = dest.right - dest.left;
    const float dh = dest.bottom - dest.top;
    if (img_w <= 0.0f || img_h <= 0.0f || dw <= 0.0f || dh <= 0.0f) return dest;
    const float scale = (std::max)(dw / img_w, dh / img_h);
    const float w = img_w * scale;
    const float h = img_h * scale;
    const float x = dest.left + (dw - w) * 0.5f;
    const float y = dest.top + (dh - h) * 0.5f;
    return D2D1::RectF(x, y, x + w, y + h);
}

// AC-01: the default wallpaper (position 4, cover) must land on the same
// pixels as before, so this compares against the retired formula exactly.
bool FullAreaCoverMatchesLegacy() {
    const D2D1_RECT_F areas[] = {
        D2D1::RectF(0.0f, 0.0f, 320.0f, 240.0f),
        D2D1::RectF(10.5f, 20.25f, 1300.75f, 900.5f),
        D2D1::RectF(-5.0f, -5.0f, 100.0f, 50.0f),
        D2D1::RectF(0.0f, 0.0f, 1.0f, 1.0f),
        D2D1::RectF(0.0f, 0.0f, 3840.0f, 2160.0f),
    };
    const D2D1_SIZE_F images[] = {
        D2D1::SizeF(96.0f, 64.0f),    D2D1::SizeF(37.0f, 83.0f),
        D2D1::SizeF(1600.0f, 900.0f), D2D1::SizeF(1.0f, 1.0f),
        D2D1::SizeF(3000.0f, 7.0f),   D2D1::SizeF(1920.0f, 1080.0f),
    };
    for (const D2D1_RECT_F& area : areas) {
        for (const D2D1_SIZE_F& image : images) {
            const D2D1_RECT_F legacy = LegacyCoverRect(image.width, image.height, area);
            const D2D1_RECT_F laid =
                ImageDrawRect(image.width, image.height, area, 4, 50, ImageFit::Cover);
            if (legacy.left != laid.left || legacy.top != laid.top ||
                legacy.right != laid.right || legacy.bottom != laid.bottom) {
                std::printf("[INFO] cover drifted for %gx%g in (%g,%g,%g,%g)\n",
                            image.width, image.height, area.left, area.top, area.right,
                            area.bottom);
                return false;
            }
        }
    }
    // A non-default ratio used to be ignored on the full area; it still must be.
    const D2D1_RECT_F dest = D2D1::RectF(0.0f, 0.0f, 320.0f, 240.0f);
    const D2D1_RECT_F ignored =
        ImageDrawRect(96.0f, 64.0f, dest, 4, 20, ImageFit::Cover);
    const D2D1_RECT_F legacy = LegacyCoverRect(96.0f, 64.0f, dest);
    return ignored.left == legacy.left && ignored.top == legacy.top &&
           ignored.right == legacy.right && ignored.bottom == legacy.bottom;
}

bool FitAspectAndCentering() {
    const D2D1_RECT_F area = D2D1::RectF(0.0f, 0.0f, 400.0f, 200.0f);
    // 100x100: contain fits the height (200x200), cover fills the width (400x400).
    if (!RectNear(ImageDrawRect(100.0f, 100.0f, area, 4, 50, ImageFit::Contain),
                  100.0f, 0.0f, 300.0f, 200.0f)) return false;
    if (!RectNear(ImageDrawRect(100.0f, 100.0f, area, 4, 50, ImageFit::Cover),
                  0.0f, -100.0f, 400.0f, 300.0f)) return false;
    if (!RectNear(ImageDrawRect(100.0f, 100.0f, area, 4, 50, ImageFit::Stretch),
                  0.0f, 0.0f, 400.0f, 200.0f)) return false;
    if (!RectNear(ImageDrawRect(100.0f, 100.0f, area, 4, 50, ImageFit::Original),
                  150.0f, 50.0f, 250.0f, 150.0f)) return false;
    // A wide image keeps its ratio and centres vertically.
    if (!RectNear(ImageDrawRect(400.0f, 100.0f, area, 4, 50, ImageFit::Contain),
                  0.0f, 50.0f, 400.0f, 150.0f)) return false;
    // 16:9 inside a square contain box.
    if (!RectNear(ImageDrawRect(16.0f, 9.0f, D2D1::RectF(0.0f, 0.0f, 200.0f, 200.0f),
                                4, 50, ImageFit::Contain),
                  0.0f, 43.75f, 200.0f, 156.25f)) return false;
    // Original never scales: 300x100 stays 300x100, centred.
    if (!RectNear(ImageDrawRect(300.0f, 100.0f, area, 4, 50, ImageFit::Original),
                  -50.0f, 50.0f, 250.0f, 150.0f)) return false;
    return true;
}

bool GridPlacement() {
    const D2D1_RECT_F area = D2D1::RectF(0.0f, 0.0f, 400.0f, 300.0f);
    struct Cell {
        int position;
        float left, top, right, bottom;
    };
    const Cell cells[] = {
        {0, 0.0f, 0.0f, 200.0f, 150.0f},     {1, 100.0f, 0.0f, 300.0f, 150.0f},
        {2, 200.0f, 0.0f, 400.0f, 150.0f},   {3, 0.0f, 75.0f, 200.0f, 225.0f},
        {5, 200.0f, 75.0f, 400.0f, 225.0f},  {6, 0.0f, 150.0f, 200.0f, 300.0f},
        {7, 100.0f, 150.0f, 300.0f, 300.0f}, {8, 200.0f, 150.0f, 400.0f, 300.0f},
    };
    for (const Cell& cell : cells) {
        const D2D1_RECT_F placement = ImagePlacementRect(area, cell.position, 50);
        if (!RectNear(placement, cell.left, cell.top, cell.right, cell.bottom)) {
            std::printf("[INFO] cell %d misplaced\n", cell.position);
            return false;
        }
        // The cell takes 50% of both axes, whatever the fit.
        for (const ImageFit fit : {ImageFit::Contain, ImageFit::Cover, ImageFit::Stretch,
                                   ImageFit::Original}) {
            const D2D1_RECT_F draw = ImageDrawRect(100.0f, 100.0f, area, cell.position, 50, fit);
            if (!RectFinite(draw)) return false;
            // Cover fills the cell; the excess is clipped by the caller.
            if (fit == ImageFit::Cover) {
                if (draw.left > placement.left + 0.001f || draw.top > placement.top + 0.001f ||
                    draw.right < placement.right - 0.001f ||
                    draw.bottom < placement.bottom - 0.001f) {
                    return false;
                }
            }
        }
    }
    // The full area ignores the ratio, and so does an out-of-range position.
    return RectNear(ImagePlacementRect(area, 4, 20), 0.0f, 0.0f, 400.0f, 300.0f) &&
           RectNear(ImagePlacementRect(area, -1, 20), 0.0f, 0.0f, 400.0f, 300.0f) &&
           RectNear(ImagePlacementRect(area, 9, 20), 0.0f, 0.0f, 400.0f, 300.0f);
}

bool DegenerateInputs() {
    const D2D1_RECT_F area = D2D1::RectF(0.0f, 0.0f, 400.0f, 300.0f);
    const D2D1_RECT_F empty = D2D1::RectF(50.0f, 50.0f, 50.0f, 80.0f);
    for (const ImageFit fit : {ImageFit::Contain, ImageFit::Cover, ImageFit::Stretch,
                               ImageFit::Original}) {
        // No image pixels: the placement is the answer, never NaN.
        const D2D1_RECT_F zero = ImageDrawRect(0.0f, 0.0f, area, 4, 50, fit);
        if (!RectFinite(zero) || !RectNear(zero, 0.0f, 0.0f, 400.0f, 300.0f)) return false;
        const D2D1_RECT_F negative = ImageDrawRect(-10.0f, -10.0f, area, 4, 50, fit);
        if (!RectFinite(negative) || !RectNear(negative, 0.0f, 0.0f, 400.0f, 300.0f)) return false;
        // Empty area: the placement and the draw rect stay at the area.
        const D2D1_RECT_F none = ImageDrawRect(100.0f, 100.0f, empty, 4, 50, fit);
        if (!RectFinite(none) || !RectNear(none, 50.0f, 50.0f, 50.0f, 80.0f)) return false;
    }
    // Ratio bounds from the preferences, plus out-of-range values a caller may
    // still hand over.
    for (const int ratio : {20, 100, -50, 0, 500}) {
        const D2D1_RECT_F placement = ImagePlacementRect(area, 0, ratio);
        const D2D1_RECT_F draw = ImageDrawRect(50.0f, 50.0f, area, 0, ratio, ImageFit::Cover);
        if (!RectFinite(placement) || !RectFinite(draw)) return false;
        if (placement.right < placement.left || placement.bottom < placement.top) return false;
        if (draw.right < draw.left || draw.bottom < draw.top) return false;
    }
    if (!RectNear(ImagePlacementRect(area, 0, 20), 0.0f, 0.0f, 80.0f, 60.0f)) return false;
    return RectNear(ImagePlacementRect(area, 0, 100), 0.0f, 0.0f, 400.0f, 300.0f);
}

// The GPU bake cannot be inspected here, so the key that decides "reuse or
// rebake" is a pure value: a new version or placement must never match.
bool BakeCacheKeys() {
    SourceBakeKey base;
    base.path = L"C:\\wallpaper\\a.png";
    base.version = 7;
    base.position = 4;
    base.ratio_percent = 50;
    base.fit = 1;
    base.blur_px = 14.0f;
    base.width = 400;
    base.height = 300;

    SourceBakeKey other = base;
    bool ok = base.SameContent(other) && base.SizeWithin(other, 0, 0);

    other.version = 8;  // a newly published image always rebakes
    ok = ok && !base.SameContent(other);
    other = base;
    other.position = 0;
    ok = ok && !base.SameContent(other);
    other = base;
    other.ratio_percent = 20;
    ok = ok && !base.SameContent(other);
    other = base;
    other.fit = 0;
    ok = ok && !base.SameContent(other);
    other = base;
    other.path = L"C:\\wallpaper\\b.png";
    ok = ok && !base.SameContent(other);

    other = base;
    other.blur_px = 14.4f;  // sub-pixel radius drift keeps the bake
    ok = ok && base.SameContent(other);
    other.blur_px = 20.0f;
    ok = ok && !base.SameContent(other);

    other = base;
    other.width = 420;
    other.height = 310;
    ok = ok && base.SameContent(other) && base.SizeWithin(other, 32, 32) &&
         !base.SizeWithin(other, 8, 8);
    other.width = 900;
    ok = ok && !base.SizeWithin(other, 100, 100);
    return ok;
}

bool PureLayoutTests() {
    bool ok = Report("position 4 cover matches the pre-change formula", FullAreaCoverMatchesLegacy());
    ok &= Report("fit modes keep their aspect ratio and centring", FitAspectAndCentering());
    ok &= Report("nine-grid cells stick to their edge or corner", GridPlacement());
    ok &= Report("degenerate areas and sizes stay finite", DegenerateInputs());
    ok &= Report("bake cache key tracks version and placement", BakeCacheKeys());
    return ok;
}

} // namespace

int wmain() {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(ARRAYSIZE(temp), temp)) return 2;
    wchar_t dir[MAX_PATH]{};
    swprintf_s(dir, L"%sPulseMaterialTest_%lu_%llu", temp, GetCurrentProcessId(),
               GetTickCount64());
    if (!CreateDirectoryW(dir, nullptr)) return 2;
    const std::wstring root = dir;
    const std::wstring image_a = root + L"\\a.bmp";
    const std::wstring image_b = root + L"\\b.bmp";
    const std::wstring corrupt = root + L"\\corrupt.bmp";
    const std::wstring large = root + L"\\large.bmp";
    const std::wstring pattern = root + L"\\pattern.bmp";

    bool passed = PureLayoutTests();
    passed &= Report("create bitmap fixtures",
                     WriteBmp(image_a, 96, 64, 220, 40, 30) &&
                     WriteBmp(image_b, 37, 83, 20, 180, 70) &&
                     WriteCorrupt(corrupt) &&
                     WriteBmp(large, 1600, 900, 60, 90, 200) &&
                     WritePatternBmp(pattern, 1600, 900));

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
                                L"Pulse material test", WS_POPUP | WS_VISIBLE,
                                -32000, -32000, 320, 240, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    Compositor compositor;
    const bool compositor_ready = hwnd && compositor.Init(hwnd);
    passed &= Report("initialize off-screen compositor", compositor_ready);

    if (compositor_ready) {
        WindowMaterial material;
        material.SetCompositor(&compositor);
        material.SetDecodeDelayForTesting(200);
        ValidateRect(hwnd, nullptr);
        const auto start = std::chrono::steady_clock::now();
        ID2D1Bitmap* first = material.SourceBitmap(image_a);
        const double first_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        std::printf("[INFO] first SourceBitmap call %.3f ms\n", first_ms);
        passed &= Report("first request is asynchronous", !first && first_ms < 50.0);

        passed &= Report("decode completion invalidates the render window",
                         WaitForInvalidation(hwnd, 5000));

        ID2D1Bitmap* ready = WaitBitmap(material, image_a, 5000);
        const bool decoded = ready && ready->GetPixelSize().width == 96 &&
                             ready->GetPixelSize().height == 64 &&
                             HasVisiblePixels(compositor.Dc(), ready);
        passed &= Report("decoded pixels upload on render thread", decoded);

        material.SetDecodeDelayForTesting(180);
        material.SourceBitmap(image_b);
        Sleep(30);
        material.SourceBitmap(image_a);
        ready = WaitBitmap(material, image_a, 5000);
        const bool latest_wins = ready && ready->GetPixelSize().width == 96 &&
                                 ready->GetPixelSize().height == 64;
        passed &= Report("rapid switches discard stale decode", latest_wins);

        const uint64_t before_recreate = material.DecodeAttemptsForTesting();
        material.SetCompositor(nullptr);
        compositor.Shutdown();
        const bool device_recreated = compositor.Init(hwnd);
        material.SetCompositor(&compositor);
        // The CPU pixels are released after the upload, so a new device
        // decodes the wallpaper once more (and only once).
        ready = WaitBitmap(material, image_a, 5000);
        passed &= Report("device reset decodes the released pixels once",
                         device_recreated && ready && ready->GetPixelSize().width == 96 &&
                         material.DecodeAttemptsForTesting() == before_recreate + 1);
        for (int i = 0; i < 10; ++i) material.SourceBitmap(image_a);
        passed &= Report("an uploaded wallpaper is not decoded again",
                         material.DecodeAttemptsForTesting() == before_recreate + 1);

        material.Invalidate();
        material.SetDecodeDelayForTesting(0);
        const uint64_t failure_before = material.DecodeAttemptsForTesting();
        material.SourceBitmap(corrupt);
        const bool failed = WaitFailure(material, corrupt, 5000);
        const uint64_t failure_after = material.DecodeAttemptsForTesting();
        for (int i = 0; i < 20; ++i) material.SourceBitmap(corrupt);
        Sleep(50);
        passed &= Report("decode failures are cached",
                         failed && failure_after == failure_before + 1 &&
                         material.DecodeAttemptsForTesting() == failure_after);
    }

    if (compositor_ready) {
        // A blurred cover only needs the sample size; turning the blur off
        // decodes the full image while the smaller one keeps drawing.
        WindowMaterial material;
        material.SetCompositor(&compositor);
        ID2D1DeviceContext* dc = compositor.Dc();
        D2D1_BITMAP_PROPERTIES1 props{};
        props.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
        props.dpiX = 96.0f;
        props.dpiY = 96.0f;
        ComPtr<ID2D1Bitmap1> target;
        const bool target_ready = SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(320, 240), nullptr, 0,
                                                             props, &target));
        const auto draw = [&](float blur_px) {
            dc->SetTarget(target.get());
            dc->BeginDraw();
            const bool drawn = material.DrawSourceCover(dc, D2D1::RectF(0.0f, 0.0f, 320.0f, 240.0f),
                                                        large, blur_px);
            const bool ended = SUCCEEDED(dc->EndDraw());
            dc->SetTarget(nullptr);
            return drawn && ended;
        };
        const uint64_t before = material.DecodeAttemptsForTesting();
        bool soft_drawn = false;
        for (ULONGLONG deadline = GetTickCount64() + 5000;
             target_ready && !soft_drawn && GetTickCount64() < deadline; Sleep(10)) {
            soft_drawn = draw(14.0f);
        }
        const D2D1_SIZE_U soft = material.SourceSizeForTesting();
        passed &= Report("a blurred cover decodes at the sample size",
                         soft_drawn && soft.width == 1024 && soft.height == 576 &&
                         material.DecodeAttemptsForTesting() == before + 1);

        bool every_frame = true;
        D2D1_SIZE_U sharp{};
        for (ULONGLONG deadline = GetTickCount64() + 5000;
             soft_drawn && sharp.width != 1600 && GetTickCount64() < deadline; Sleep(10)) {
            every_frame &= draw(0.0f);
            sharp = material.SourceSizeForTesting();
        }
        passed &= Report("turning the blur off decodes the full image without a blank frame",
                         every_frame && sharp.width == 1600 && sharp.height == 900);

        const uint64_t upgraded = material.DecodeAttemptsForTesting();
        bool blurred_again = true;
        for (int i = 0; i < 10; ++i) blurred_again &= draw(14.0f);
        passed &= Report("the full image serves the blur again without a decode",
                         blurred_again && material.DecodeAttemptsForTesting() == upgraded &&
                         material.SourceSizeForTesting().width == 1600);
    }

    if (compositor_ready) {
        // The blurred cover from the sample-size source against the one from
        // the full source, for a 1400x900 window (bake 1024x658, sigma ~7 px).
        ID2D1DeviceContext* dc = compositor.Dc();
        D2D1_BITMAP_PROPERTIES1 props{};
        props.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
        props.dpiX = 96.0f;
        props.dpiY = 96.0f;
        const auto render = [&](bool full_source, std::vector<BYTE>& pixels) {
            WindowMaterial material;
            material.SetCompositor(&compositor);
            if (full_source && !WaitBitmap(material, pattern, 5000)) return false;
            ComPtr<ID2D1Bitmap1> target;
            if (FAILED(dc->CreateBitmap(D2D1::SizeU(1400, 900), nullptr, 0, props, &target)))
                return false;
            bool drawn = false;
            for (ULONGLONG deadline = GetTickCount64() + 5000; !drawn && GetTickCount64() < deadline;) {
                dc->SetTarget(target.get());
                dc->BeginDraw();
                drawn = material.DrawSourceCover(dc, D2D1::RectF(0.0f, 0.0f, 1400.0f, 900.0f),
                                                 pattern, 14.0f);
                drawn &= SUCCEEDED(dc->EndDraw());
                dc->SetTarget(nullptr);
                if (!drawn) Sleep(10);
            }
            const UINT edge = material.SourceSizeForTesting().width;
            return drawn && edge == (full_source ? 1600u : 1024u) && ReadPixels(dc, target.get(), pixels);
        };
        std::vector<BYTE> soft_pixels;
        std::vector<BYTE> full_pixels;
        const bool rendered = render(false, soft_pixels) && render(true, full_pixels) &&
                              soft_pixels.size() == full_pixels.size();
        double total = 0.0;
        int worst = 0;
        if (rendered) {
            for (size_t i = 0; i < soft_pixels.size(); ++i) {
                if (i % 4 == 3) continue;
                const int d = std::abs(static_cast<int>(soft_pixels[i]) - static_cast<int>(full_pixels[i]));
                total += d;
                if (d > worst) worst = d;
            }
        }
        const double mean = rendered ? total / (soft_pixels.size() / 4 * 3) : 0.0;
        std::printf("[INFO] blurred cover, sample-size vs full source: mean %.3f, max %d (of 255)\n",
                    mean, worst);
        passed &= Report("the sample-size source blurs like the full source",
                         rendered && mean < 1.0 && worst <= 8);
    }

    if (compositor_ready) {
        auto* material = new WindowMaterial();
        material->SetCompositor(&compositor);
        material->SetDecodeDelayForTesting(1500);
        const uint64_t before = material->DecodeAttemptsForTesting();
        material->SourceBitmap(image_b);
        const ULONGLONG deadline = GetTickCount64() + 1000;
        while (material->DecodeAttemptsForTesting() == before && GetTickCount64() < deadline)
            Sleep(1);
        const auto start = std::chrono::steady_clock::now();
        delete material;
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        std::printf("[INFO] destructor during 1500 ms stall %.3f ms\n", elapsed);
        passed &= Report("destructor is bounded during stalled decode", elapsed < 300.0);
        Sleep(1550);
    }

    compositor.Shutdown();
    if (hwnd) DestroyWindow(hwnd);
    DeleteFileW(image_a.c_str());
    DeleteFileW(image_b.c_str());
    DeleteFileW(corrupt.c_str());
    DeleteFileW(large.c_str());
    DeleteFileW(pattern.c_str());
    RemoveDirectoryW(root.c_str());
    if (SUCCEEDED(com_result)) CoUninitialize();
    std::printf("%s\n", passed ? "All material tests passed." : "Material tests failed.");
    return passed ? 0 : 1;
}

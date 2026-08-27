#include "../ui/window_material.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using pulse::ui::Compositor;
using pulse::ui::ComPtr;
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

    bool passed = WriteBmp(image_a, 96, 64, 220, 40, 30) &&
                  WriteBmp(image_b, 37, 83, 20, 180, 70) &&
                  WriteCorrupt(corrupt);
    passed &= Report("create bitmap fixtures", passed);

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
        const auto recreate_start = std::chrono::steady_clock::now();
        ready = material.SourceBitmap(image_a);
        const auto recreate_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - recreate_start).count();
        passed &= Report("device reset reuses decoded CPU pixels",
                         device_recreated && ready && recreate_ms < 50 &&
                         material.DecodeAttemptsForTesting() == before_recreate);

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
    RemoveDirectoryW(root.c_str());
    if (SUCCEEDED(com_result)) CoUninitialize();
    std::printf("%s\n", passed ? "All material tests passed." : "Material tests failed.");
    return passed ? 0 : 1;
}

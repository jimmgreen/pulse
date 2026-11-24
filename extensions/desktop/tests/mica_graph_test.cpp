#include "host/render/desktop_renderer.h"
#include "../../../src/ui/window_material.h"
#include <array>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;
int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    {
        pulse::desktop::render::DesktopRenderer renderer;
        if (!renderer.InitializeOffscreen(128, 128)) return 2;
        auto* dc = renderer.DeviceContext();
        std::vector<uint32_t> pixels(64 * 64);
        for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x)
            pixels[y * 64 + x] = x < 32 ? 0xFFFF3030u : 0xFF3060FFu;
        ComPtr<ID2D1Bitmap1> source, target, readback;
        auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(64, 64), pixels.data(), 64 * 4, props, &source))) return 3;
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(64, 64), nullptr, 0, props, &target))) return 3;
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(64, 64), nullptr, 0, props, &readback))) return 3;
        ComPtr<ID2D1Image> old_target;
        dc->GetTarget(&old_target);
        std::array<std::array<unsigned, 3>, 4> sums{};
        for (unsigned kind = 0; kind < 4; ++kind) {
            ComPtr<ID2D1Image> image;
            const HRESULT graph = pulse::ui::WindowMaterial::BuildMicaImage(dc, source.Get(),
                (kind & 2) != 0, (kind & 1) != 0, 8.0f, &image);
            check(SUCCEEDED(graph) && image, "shared Mica graph creation");
            if (!image) continue;
            dc->SetTarget(target.Get());
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(0, 0));
            const D2D1_RECT_F rect = D2D1::RectF(0, 0, 64, 64);
            dc->DrawImage(image.Get(), D2D1::Point2F(0, 0), rect);
            const HRESULT draw = dc->EndDraw();
            dc->SetTarget(old_target.Get());
            check(SUCCEEDED(draw), "shared graph draw has no deferred D2D error");
            if (FAILED(draw) || FAILED(readback->CopyFromBitmap(nullptr, target.Get(), nullptr))) continue;
            D2D1_MAPPED_RECT map{};
            if (FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &map))) { check(false, "map output"); continue; }
            bool opaque = true;
            for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x) {
                const auto* p = map.bits + y * map.pitch + x * 4;
                opaque &= p[3] == 255;
                for (unsigned c = 0; c < 3; ++c) sums[kind][c] += p[c];
            }
            const auto* left = map.bits + 32 * map.pitch + 8 * 4;
            const auto* right = map.bits + 32 * map.pitch + 56 * 4;
            check(left[0] != right[0] || left[1] != right[1] || left[2] != right[2],
                "wallpaper chroma survives; material is not a solid tint");
            check(opaque, "sampled Mica is opaque without white alpha overlay");
            std::printf("RESULT variant=%u BGR=%u,%u,%u\n", kind, sums[kind][0], sums[kind][1], sums[kind][2]);
            readback->Unmap();
        }
        check(sums[0] != sums[2] && sums[1] != sums[3], "Mica and Mica Alt differ in both themes");
        check(sums[0] != sums[1] && sums[2] != sums[3], "light and dark recipes differ");
        auto& backdrop = renderer.Backdrop();
        check(backdrop.LoadForScreen(dc, 128, 128), "system wallpaper load");
        std::wprintf(L"SYSTEM_WALLPAPER=%ls\n", backdrop.WallpaperPath().c_str());
        for (float dpi : {1.0f, 1.5f}) {
            backdrop.SetDipMapping(dpi, 12, 8);
            auto* brush = backdrop.SampledMicaBrush(dc, true, false);
            check(brush != nullptr, "system wallpaper Mica Alt image brush at DPI");
            if (brush) {
                dc->SetTarget(target.Get()); dc->BeginDraw();
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, 64, 64), 8, 8), brush);
                const HRESULT draw = dc->EndDraw(); dc->SetTarget(old_target.Get());
                check(SUCCEEDED(draw), "system wallpaper image brush draw");
            }
        }
        for (unsigned variant = 0; variant < 4; ++variant)
            check(backdrop.SampledMicaBrush(dc, (variant & 2) != 0, (variant & 1) != 0, 78) != nullptr,
                "mixed material bounded cache warmup");
        const auto cached = backdrop.MaterialStats();
        for (unsigned repeat = 0; repeat < 100; ++repeat)
            for (unsigned variant = 0; variant < 4; ++variant)
                backdrop.SampledMicaBrush(dc, (variant & 2) != 0, (variant & 1) != 0, repeat % 2 == 0 ? 30 : 100);
        const auto reused = backdrop.MaterialStats();
        check(reused.card_builds == cached.card_builds && reused.card_bitmap_bytes == cached.card_bitmap_bytes,
            "400 mixed-material/opacity lookups reuse official opaque recipe without rebuilds");
        check(reused.card_bitmap_bytes <= 16u * 1024u * 1024u,
            "four baked material entries stay within 16MiB explicit bitmap budget");
    }
    CoUninitialize();
    return failures ? 1 : 0;
}

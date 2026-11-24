#include "host/render/desktop_renderer.h"
#include <cstdio>
#include <filesystem>
#include <vector>
#include <thread>
#include <chrono>

using namespace pulse::desktop;
using Microsoft::WRL::ComPtr;
namespace tc = render::text_contrast;

namespace pulse::desktop::render {
struct DesktopContrastTestPeer {
    static void PublicFixture(DesktopRenderer& r, const std::wstring& wallpaper,
                              const model::DesktopModelState& state) {
        r.ReloadWallpaper(model::WallpaperPreset::System, state.settings.dark_mode, wallpaper);
        DesktopInteractionState interaction;
        interaction.force_opaque_wallpaper = true;
        r.DrawScene(state, {}, {}, {}, interaction);
    }
#ifndef PULSE_CONTRAST_BASELINE
    static tc::Samples Draw(DesktopRenderer& r, const std::wstring& wallpaper,
                           model::CardMaterial material, uint8_t opacity, bool dark,
                           const model::DesktopTile* tile, bool label, bool sampled_acrylic = false) {
        struct RestoreFlag {
            bool& flag;
            bool saved;
            ~RestoreFlag() { flag = saved; }
        } restore{r.using_dcomp_, r.using_dcomp_};
        if (sampled_acrylic) {
            if (r.hwnd_ || r.vsurf_) return {};
            r.using_dcomp_ = false;
        }
        auto* dc = r.DeviceContext();
        r.Backdrop().LoadForScreen(dc, 320, 180, model::WallpaperPreset::System, dark, wallpaper);
        r.Backdrop().SetDipMapping(1, 0, 0);
        dc->Clear(D2D1::ColorF(0, 0, 0, 1));
        dc->DrawBitmap(r.Backdrop().QuarterWallpaperBitmap(), D2D1::RectF(0, 0, 320, 180));
        const auto rect = D2D1::RectF(20, 20, 300, 160);
        const auto text = tile ? D2D1::RectF(240, 115, 280, 140) : D2D1::RectF(40, 65, 280, 110);
        tc::Samples samples;
        if (tile) {
            model::DesktopModelState state;
            state.settings.dark_mode = dark;
            state.settings.card_opacity_percent = opacity;
            state.settings.default_material = material;
            r.DrawLiveTile(*tile, rect, state, {}, {});
            samples = r.TileBackgroundSamples(*tile, rect, text, material, state.settings);
        } else {
            r.DrawCardMaterial(rect, 0, material, opacity, dark, 0xFF0078D4, false);
            samples = r.CardBackgroundSamples(text, material, opacity, dark);
        }
        if (label) {
            r.DrawIconLabel(L"MMMMMMMM", D2D1::RectF(40, 65, 280, 110), r.fmt_item_label_.Get(),
                model::IconLabelStyle::Shadow, dark, true, true, 1, &samples);
        }
        return samples;
    }
#endif
};
}

struct Image {
    UINT width = 0, height = 0;
    std::vector<uint8_t> pixels;
};
Image ReadPng(const std::filesystem::path& path) {
    Image out;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&out.width, &out.height)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return {};
    out.pixels.resize(static_cast<size_t>(out.width) * out.height * 4);
    if (FAILED(converter->CopyPixels(nullptr, out.width * 4, static_cast<UINT>(out.pixels.size()), out.pixels.data()))) return {};
    return out;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 3;
    const std::filesystem::path output(argv[1]);
    std::filesystem::create_directories(output);
    int failures = 0, cases = 0;
    auto check = [&](bool ok, const char* name) {
        ++cases;
        failures += !ok;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    };
    {
        render::DesktopRenderer renderer;
        if (!renderer.InitializeOffscreen(320, 180)) return 4;
        model::DesktopModelState state;
        state.settings.wallpaper_preset = model::WallpaperPreset::System;
        render::LiveTileRuntimeData live;
        render::DesktopInteractionState interaction;
        if (argc > 2 && std::wstring_view(argv[2]) == L"--public-only") {
            for (const char* background : {"black", "white", "gray", "green", "blue"}) for (bool dark : {false, true}) {
                const auto wall = (output / (std::string("wall-") + background + ".png")).wstring();
                state.settings.dark_mode = dark;
                state.settings.label_style = model::IconLabelStyle::Shadow;
                model::DesktopItem item;
                item.name = L"MMMMMMMM";
                item.path = L"C:\\pulse-contrast-fixture\\missing.lnk";
                item.loose_pos = {70, 40};
                item.force_loose = true;
                state.items = {item};
                const auto file = output / (std::string("public-") + background + (dark ? "-dark.png" : "-light.png"));
                check(renderer.RenderCompositePng(file.wstring(), state, {}, {}, live, interaction,
                    [&](auto*) { render::DesktopContrastTestPeer::PublicFixture(renderer, wall, state); }),
                    "public fixed-wallpaper loose label fixture");
            }
            std::printf("Public contrast render: %d cases, %d failures\n", cases, failures);
            return failures ? 1 : 0;
        }
#ifndef PULSE_CONTRAST_BASELINE
        const bool sampled_acrylic = argc > 2 && std::wstring_view(argv[2]) == L"--sampled-acrylic";
        const struct { const char* name; model::CardMaterial kind; } materials[] = {
            {"solid", model::CardMaterial::Solid}, {"acrylic", model::CardMaterial::Acrylic},
            {"mica", model::CardMaterial::Mica}, {"micaalt", model::CardMaterial::MicaAlt}};
        auto run = [&](const std::string& name, const std::wstring& wall, model::CardMaterial material,
                       uint8_t opacity, bool dark, const model::DesktopTile* tile) {
            tc::Samples predicted;
            const auto file = output / (name + ".png");
            const bool rendered = renderer.RenderCompositePng(file.wstring(), state, {}, {}, live, interaction,
                [&](auto*) { predicted = render::DesktopContrastTestPeer::Draw(renderer, wall, material, opacity, dark, tile, false, sampled_acrylic); });
            check(rendered, (name + " real D2D draw").c_str());
            const auto bg = ReadPng(file);
            const auto actual = tile ? tc::SamplePbgra(bg.pixels, bg.width, bg.height, 240, 115, 280, 140) :
                                       tc::SamplePbgra(bg.pixels, bg.width, bg.height, 40, 65, 280, 110);
            const auto p = tc::ChooseInk(predicted);
            const auto a = tc::ChooseInk(actual);
            std::printf("METRIC %s predicted=%s %.4f/%.4f measured=%s %.4f/%.4f\n", name.c_str(),
                p.light ? "white" : "dark", p.light_contrast, p.dark_contrast,
                a.light ? "white" : "dark", a.light_contrast, a.dark_contrast);
            check(p.valid && a.valid && p.light == a.light, (name + " predicted ink matches actual background").c_str());
            if (tile) return;
            const auto label_file = output / (name + "-label.png");
            check(renderer.RenderCompositePng(label_file.wstring(), state, {}, {}, live, interaction,
                [&](auto*) { render::DesktopContrastTestPeer::Draw(renderer, wall, material, opacity, dark, nullptr, true, sampled_acrylic); }),
                (name + " real DrawIconLabel").c_str());
            const auto label_image = ReadPng(label_file);
            int white = 0, ink = 0;
            if (label_image.width == bg.width && label_image.height == bg.height) {
                const auto dark_rgb = tc::FromArgb(tc::kDarkInk);
                for (UINT y = 65; y < 110; ++y) for (UINT x = 40; x < 280; ++x) {
                    const size_t offset = (static_cast<size_t>(y) * bg.width + x) * 4;
                    const auto* pixel = label_image.pixels.data() + offset;
                    const auto* prior = bg.pixels.data() + offset;
                    const int delta = std::abs(int(pixel[0]) - prior[0]) + std::abs(int(pixel[1]) - prior[1]) + std::abs(int(pixel[2]) - prior[2]);
                    if (delta < 35) continue;
                    if (pixel[0] > 245 && pixel[1] > 245 && pixel[2] > 245) ++white;
                    if (std::abs(pixel[0] / 255.0f - dark_rgb.b) < 0.025f &&
                        std::abs(pixel[1] / 255.0f - dark_rgb.g) < 0.025f &&
                        std::abs(pixel[2] / 255.0f - dark_rgb.r) < 0.025f) ++ink;
                }
            }
            std::printf("INK %s white_pixels=%d dark_pixels=%d\n", name.c_str(), white, ink);
            check(p.light ? white > ink && white > 10 : ink > white && ink > 10,
                (name + " rasterized glyph uses selected ink").c_str());
        };
        for (const char* background : {"black", "white", "gray", "green", "blue"}) {
            const auto wall = (output / (std::string("wall-") + background + ".png")).wstring();
            const std::vector<uint8_t> opacities = sampled_acrylic ? std::vector<uint8_t>{0, 15, 30, 60, 100} :
                                                                    std::vector<uint8_t>{0, 30, 100};
            for (bool dark : {false, true}) for (const auto& material : materials) for (uint8_t opacity : opacities) {
                if (sampled_acrylic && material.kind != model::CardMaterial::Acrylic) continue;
                const auto name = std::string(background) + "-" + (dark ? "dark-" : "light-") + material.name + "-" + std::to_string(opacity);
                run(name, wall, material.kind, opacity, dark, nullptr);
            }
        }
        for (bool dark : {false, true}) for (const char* background : {"black", "white", "gray"}) {
            model::DesktopTile tile;
            tile.kind = model::TileKind::Apps;
            tile.id = "contrast-apps";
            tile.title = L"Apps";
            tile.size = {2, 2};
            tile.material = model::CardMaterial::Acrylic;
            tile.look.background = model::TileBackground::Color;
            tile.look.color_argb = std::string_view(background) == "white" ? 0xFFFFFFFF :
                                   std::string_view(background) == "gray" ? 0xFF808080 : 0xFF000000;
            const auto wall = (output / "wall-gray.png").wstring();
            run(std::string("apps-color-") + background + (dark ? "-dark" : "-light"), wall,
                tile.material, 60, dark, &tile);
            tile.look.background = model::TileBackground::Image;
            tile.look.image_path = (output / (std::string("wall-") + background + ".png")).wstring();
            bool loaded = false;
            for (int i = 0; i < 100 && !loaded; ++i) {
                loaded = renderer.Backdrop().GetPhotoTileBitmap(renderer.DeviceContext(), tile.look.image_path) != nullptr;
                if (!loaded) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            check(loaded, "async image fixture loaded");
            run(std::string("apps-image-") + background + (dark ? "-dark" : "-light"), wall,
                tile.material, 60, dark, &tile);
        }
#endif
    }
    CoUninitialize();
    std::printf("Contrast render: %d cases, %d failures\n", cases, failures);
    return failures ? 1 : 0;
}

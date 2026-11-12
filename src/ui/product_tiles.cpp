// product_tiles.cpp - store product tiles from archive_ux_mockup.html: a gradient tile
// with a top highlight and an inner hairline, carrying white two-tone artwork drawn
// in a 48 x 48 grid. Details are cut in the tile's dark colour; no font glyphs.
#include "store_icons.h"
#include "command_icons.h"
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace pulse::ui {
namespace {
using Microsoft::WRL::ComPtr;

std::string Circle(float cx, float cy, float r) {
    char t[128];
    std::snprintf(t, sizeof(t), "M%g %ga%g %g 0 1 0 %g 0a%g %g 0 1 0 %g 0z", cx - r, cy, r, r, 2 * r, r, r, -2 * r);
    return t;
}
std::string Rect(float x, float y, float w, float h, float rx) {
    char t[256];
    if (rx <= 0) { std::snprintf(t, sizeof(t), "M%g %gh%gv%gh%gz", x, y, w, h, -w); return t; }
    std::snprintf(t, sizeof(t), "M%g %gh%ga%g %g 0 0 1 %g %gv%ga%g %g 0 0 1 %g %gh%ga%g %g 0 0 1 %g %gv%ga%g %g 0 0 1 %g %gz",
                  x + rx, y, w - 2 * rx, rx, rx, rx, rx, h - 2 * rx, rx, rx, -rx, rx, -(w - 2 * rx), rx, rx, -rx, -rx,
                  -(h - 2 * rx), rx, rx, rx, -rx);
    return t;
}
// One layer of artwork: filled (stroke == 0) or stroked, in white or the tile's dark colour.
struct Prim {
    std::string d;
    bool dark = false;
    float alpha = 1.0f;
    float stroke = 0.0f; // grid units
};
struct Art {
    uint32_t light = 0, dark = 0;
    std::vector<Prim> prims;
};
Art Make(ProductArt art) {
    Art a;
    auto add = [&](std::string d, bool dark, float alpha, float stroke = 0) { a.prims.push_back({std::move(d), dark, alpha, stroke}); };
    switch (art) {
    case ProductArt::SevenZip:
        a.light = 0x6E9F90; a.dark = 0x355A4F;
        add(Rect(9, 19, 30, 21, 3.5f), false, .96f);
        add(Rect(7, 11, 34, 9.5f, 2.75f), false, 1);
        add(Rect(9, 20.5f, 30, 2.2f, 0), true, .22f);
        for (int k = 0; k < 5; ++k) add(Rect(k % 2 ? 24.0f : 21.4f, 22.5f + k * 2.6f, 2.6f, 1.6f, .5f), true, .75f);
        add(Rect(20.6f, 35, 6.8f, 7.2f, 2.2f), true, .95f);
        add(Circle(24, 38.6f, 1.25f), false, 1);
        add(Rect(12, 14.4f, 7, 2.4f, 1.2f), true, .35f);
        break;
    case ProductArt::Images:
        a.light = 0x9A86E0; a.dark = 0x5C47B0;
        add(Rect(7.5f, 10, 33, 27, 4.5f), false, 1);
        add(Rect(11, 13.5f, 26, 20, 2.2f), true, .22f);
        add(Circle(30.5f, 19.5f, 3), true, .55f);
        add("M11 33.5v-4.2l7.2-7.2a1.6 1.6 0 0 1 2.3 0l5.6 5.6 2.6-2.6a1.6 1.6 0 0 1 2.3 0L37 31.1v2.4z", true, .9f);
        add("M38.5 5.5l1.1 2.6 2.6 1.1-2.6 1.1-1.1 2.6-1.1-2.6-2.6-1.1 2.6-1.1z", false, .9f);
        break;
    case ProductArt::Raw:
        a.light = 0xD29455; a.dark = 0x9A5E25;
        add("M17 13.5l2.2-3.5h9.6l2.2 3.5z", false, .7f);
        add(Rect(6.5f, 13, 35, 25, 5), false, 1);
        add(Circle(24, 25.5f, 9), true, .9f);
        add(Circle(24, 25.5f, 5.6f), true, .55f);
        add(Circle(24, 25.5f, 5.6f), false, .45f, 1.2f);
        add(Circle(21.8f, 23.3f, 1.6f), false, .85f);
        add(Rect(33.5f, 16.2f, 4.6f, 2.6f, 1.3f), true, .45f);
        break;
    case ProductArt::Media:
        a.light = 0x4D9DBD; a.dark = 0x245F79;
        add(Rect(8, 9, 32, 30, 4.5f), false, 1);
        for (int k = 0; k < 4; ++k) {
            add(Rect(10.4f, 12.4f + k * 6.4f, 3.2f, 3.6f, 1), true, .45f);
            add(Rect(34.4f, 12.4f + k * 6.4f, 3.2f, 3.6f, 1), true, .45f);
        }
        add("M21.2 18.2v11.6a1.2 1.2 0 0 0 1.8 1l9-5.8a1.2 1.2 0 0 0 0-2l-9-5.8a1.2 1.2 0 0 0-1.8 1z", true, .95f);
        break;
    case ProductArt::Content:
        a.light = 0x6E8FCB; a.dark = 0x3B5B94;
        add("M10 7.5h16.5L34 15v24.5a2 2 0 0 1-2 2H10a2 2 0 0 1-2-2v-30a2 2 0 0 1 2-2z", false, 1);
        add("M26.5 7.5V15H34z", true, .35f);
        add(Rect(12, 19, 12, 2.2f, 1.1f), true, .45f);
        add(Rect(12, 24, 9, 2.2f, 1.1f), true, .45f);
        add(Rect(12, 29, 7, 2.2f, 1.1f), true, .45f);
        add(Circle(30, 30, 7.2f), false, 1);
        add(Circle(30, 30, 7.2f), true, 1, 3);
        add(Rect(28, 28.6f, 4, 2.8f, 1), true, .5f);
        add("M35.3 35.3l5 5", true, 1, 3.6f);
        break;
    case ProductArt::Ai:
        a.light = 0xB08AE2; a.dark = 0x6B48B5;
        add("M21 9.5c.9 6.4 4.1 9.6 10.5 10.5-6.4.9-9.6 4.1-10.5 10.5-.9-6.4-4.1-9.6-10.5-10.5 6.4-.9 9.6-4.1 10.5-10.5z", false, 1);
        add("M33.5 27c.5 3.4 2.2 5.1 5.6 5.6-3.4.5-5.1 2.2-5.6 5.6-.5-3.4-2.2-5.1-5.6-5.6 3.4-.5 5.1-2.2 5.6-5.6z", false, .75f);
        add(Circle(35, 12.5f, 2), false, .6f);
        add(Circle(21, 20, 2.4f), true, .35f);
        break;
    default:
        break;
    }
    return a;
}
D2D1_COLOR_F Hex(uint32_t rgb, float alpha = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha);
}
uint32_t Mix(uint32_t rgb, uint32_t target, float t) {
    auto c = [&](int shift) {
        const float a = static_cast<float>((rgb >> shift) & 0xFF), b = static_cast<float>((target >> shift) & 0xFF);
        return static_cast<uint32_t>(a + (b - a) * t + .5f) << shift;
    };
    return c(16) | c(8) | c(0);
}
uint32_t Rgb(D2D1_COLOR_F c) {
    auto b = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + .5f); };
    return b(c.r) << 16 | b(c.g) << 8 | b(c.b);
}
// Geometries are factory resources: built once per art and factory.
struct Built { std::vector<ComPtr<ID2D1PathGeometry>> geometry; bool done = false; };
struct Cache {
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1StrokeStyle> round;
    std::array<Built, 7> arts;
};
}

ProductArt ProductArtFor(StoreIcon icon) {
    switch (icon) {
    case StoreIcon::Box: return ProductArt::SevenZip;
    case StoreIcon::Image: return ProductArt::Images;
    case StoreIcon::Camera: return ProductArt::Raw;
    case StoreIcon::Film: return ProductArt::Media;
    case StoreIcon::TextSearch: return ProductArt::Content;
    case StoreIcon::Sparkle: return ProductArt::Ai;
    default: return ProductArt::Generic;
    }
}

void DrawProductTile(ID2D1DeviceContext* dc, D2D1_RECT_F bounds, ProductArt art,
                     D2D1_COLOR_F generic_color, StoreIcon generic_icon) {
    if (!dc) return;
    const float size = (std::min)(bounds.right - bounds.left, bounds.bottom - bounds.top);
    if (size < 4) return;
    thread_local Cache cache;
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(factory.GetAddressOf());
    if (cache.factory.Get() != factory.Get()) { cache = Cache{}; cache.factory = factory; }
    if (!cache.round) {
        auto props = D2D1::StrokeStyleProperties();
        props.startCap = props.endCap = D2D1_CAP_STYLE_ROUND;
        props.lineJoin = D2D1_LINE_JOIN_ROUND;
        factory->CreateStrokeStyle(props, nullptr, 0, cache.round.GetAddressOf());
    }
    const Art model = Make(art);
    auto& built = cache.arts[static_cast<size_t>(art)];
    if (!built.done) {
        for (const auto& prim : model.prims) {
            ComPtr<ID2D1PathGeometry> path;
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(factory->CreatePathGeometry(path.GetAddressOf())) || FAILED(path->Open(sink.GetAddressOf()))) return;
            command_icons::AppendSvgPath(sink.Get(), prim.d.c_str(), prim.stroke <= 0);
            if (FAILED(sink->Close())) return;
            built.geometry.push_back(path);
        }
        built.done = true;
    }
    const uint32_t light = art == ProductArt::Generic ? Mix(Rgb(generic_color), 0xFFFFFF, .17f) : model.light;
    const uint32_t dark = art == ProductArt::Generic ? Mix(Rgb(generic_color), 0x000000, .16f) : model.dark;
    // Square tile centred in bounds; corner radius from the mockup (13 / 10.5 / 8.5 of 48).
    const float left = (bounds.left + bounds.right - size) * .5f, top = (bounds.top + bounds.bottom - size) * .5f;
    const auto tile = D2D1::RectF(left, top, left + size, top + size);
    const float radius = size * (size >= 56 ? 13.0f : size >= 40 ? 10.5f : 8.5f) / 48.0f;
    const auto rounded = D2D1::RoundedRect(tile, radius, radius);
    ComPtr<ID2D1SolidColorBrush> solid;
    if (FAILED(dc->CreateSolidColorBrush(Hex(0, 0), solid.GetAddressOf()))) return;
    // Soft contact shadow.
    const float drop = (std::max)(1.0f, size / 40.0f);
    solid->SetColor(Hex(0x000000, .16f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tile.left + drop * .5f, tile.top + drop, tile.right - drop * .5f, tile.bottom + drop), radius, radius), solid.Get());
    // Diagonal gradient, then the top highlight.
    auto gradient = [&](D2D1_POINT_2F from, D2D1_POINT_2F to, const D2D1_GRADIENT_STOP* stops, UINT32 count) {
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> brush;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, count, collection.GetAddressOf())) &&
            SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(from, to), collection.Get(), brush.GetAddressOf())))
            dc->FillRoundedRectangle(rounded, brush.Get());
    };
    const D2D1_GRADIENT_STOP base[] = {{0, Hex(light)}, {1, Hex(dark)}};
    gradient({tile.left, tile.top}, {tile.right, tile.bottom}, base, 2);
    const D2D1_GRADIENT_STOP shine[] = {{0, Hex(0xFFFFFF, .22f)}, {.55f, Hex(0xFFFFFF, 0)}};
    gradient({tile.left, tile.top}, {tile.left, tile.bottom}, shine, 2);
    // Artwork, clipped to the tile.
    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    factory->CreateRoundedRectangleGeometry(rounded, clip.GetAddressOf());
    if (clip) dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip.Get()), nullptr);
    if (art == ProductArt::Generic) {
        const float pad = size * .26f;
        DrawStoreIcon(dc, generic_icon, D2D1::RectF(tile.left + pad, tile.top + pad, tile.right - pad, tile.bottom - pad),
                      Hex(0xFFFFFF), (std::max)(1.35f, size / 22.0f));
    } else {
        D2D1_MATRIX_3X2_F old;
        dc->GetTransform(&old);
        const float scale = size / 48.0f;
        dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(tile.left, tile.top) * old);
        for (size_t i = 0; i < model.prims.size() && i < built.geometry.size(); ++i) {
            const auto& prim = model.prims[i];
            solid->SetColor(Hex(prim.dark ? model.dark : 0xFFFFFF, prim.alpha));
            if (prim.stroke > 0) dc->DrawGeometry(built.geometry[i].Get(), solid.Get(), prim.stroke, cache.round.Get());
            else dc->FillGeometry(built.geometry[i].Get(), solid.Get());
        }
        dc->SetTransform(old);
    }
    if (clip) dc->PopLayer();
    // Inner hairline.
    const float hair = (std::max)(1.0f, size / 72.0f);
    solid->SetColor(Hex(0xFFFFFF, .16f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tile.left + hair * .5f, tile.top + hair * .5f, tile.right - hair * .5f, tile.bottom - hair * .5f),
                                               radius - hair * .5f, radius - hair * .5f), solid.Get(), hair);
}
}

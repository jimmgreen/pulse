#include "store_icons.h"
#include "FluentTokens.h"
#include "command_icons.h"
#include "../app/extensions/icon_registry.h"
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::ui {
namespace {
using Microsoft::WRL::ComPtr;
// A point on a circle around (8, 8); 0 degrees points up, angles run clockwise.
std::string Polar(float radius, float degrees) {
    const double a = (degrees - 90.0) * 3.14159265358979 / 180.0;
    char text[48];
    std::snprintf(text, sizeof(text), "%.2f %.2f", 8.0 + radius * std::cos(a), 8.0 + radius * std::sin(a));
    return text;
}
// Gear and seal are generated rather than hand-typed, so teeth and scallops are even
// (archive_ux_mockup.html). Gear: 8 flat-topped teeth (+-9 deg on r 6.3, flanks to r 4.85 at +-19 deg).
std::string GearPath() {
    std::string d;
    for (int i = 0; i < 8; ++i) {
        const float a = i * 45.0f;
        d += (i ? "L" : "M") + Polar(4.85f, a - 19) + "L" + Polar(6.3f, a - 9) + "L" + Polar(6.3f, a + 9) + "L" + Polar(4.85f, a + 19);
    }
    return d + "z";
}
// Seal: 8 equal outward arcs between points on r 5.35.
std::string SealPath() {
    std::string d = "M" + Polar(5.35f, -22.5f);
    for (int i = 0; i < 8; ++i) d += "A2.15 2.15 0 0 1 " + Polar(5.35f, i * 45.0f + 22.5f);
    return d + "z";
}
// Line icons from archive_ux_mockup.html (16 x 16 grid, drawn at 1.5 dip stroke).
static void Build(StoreIcon icon, ID2D1GeometrySink* stroke, ID2D1GeometrySink* fill) {
    switch (icon) {
    case StoreIcon::Search:
        command_icons::AppendSvgPath(stroke, "M2.75 7a4.25 4.25 0 1 0 8.5 0a4.25 4.25 0 1 0 -8.5 0z", true);
        command_icons::AppendSvgPath(stroke, "m10.25 10.25 3.5 3.5", true);
        break;
    case StoreIcon::Box:
        command_icons::AppendSvgPath(stroke, "M2.25 5.75h11.5v6.75a1 1 0 0 1-1 1h-9.5a1 1 0 0 1-1-1z", true);
        command_icons::AppendSvgPath(stroke, "M1.75 2.75h12.5v3H1.75z", true);
        command_icons::AppendSvgPath(stroke, "M6.5 8.25h3", true);
        break;
    case StoreIcon::Key:
        command_icons::AppendSvgPath(stroke, "M2.25 10.75a3 3 0 1 0 6 0a3 3 0 1 0 -6 0z", true);
        command_icons::AppendSvgPath(stroke, "m7.4 8.6 6-6", true);
        command_icons::AppendSvgPath(stroke, "m11.2 4.8 1.8 1.8M9.6 6.4l1.3 1.3", true);
        command_icons::AppendSvgPath(fill, "M4.65 10.75a0.6 0.6 0 1 0 1.2 0a0.6 0.6 0 1 0 -1.2 0z", true);
        break;
    case StoreIcon::Extract:
        command_icons::AppendSvgPath(stroke, "M2.5 8.5v4a1 1 0 0 0 1 1h9a1 1 0 0 0 1-1v-4", true);
        command_icons::AppendSvgPath(stroke, "M8 10.5V3", true);
        command_icons::AppendSvgPath(stroke, "m5.25 5.75 2.75-2.75 2.75 2.75", true);
        command_icons::AppendSvgPath(stroke, "M12.6 1.6v1.8M11.7 2.5h1.8", true);
        break;
    case StoreIcon::ExtractTo:
        command_icons::AppendSvgPath(stroke, "M1.75 4.25a1 1 0 0 1 1-1h3.1l1.4 1.5h6a1 1 0 0 1 1 1v6.75a1 1 0 0 1-1 1H2.75a1 1 0 0 1-1-1z", true);
        command_icons::AppendSvgPath(stroke, "M8 7v4.25", true);
        command_icons::AppendSvgPath(stroke, "m6 9.5 2 2 2-2", true);
        break;
    case StoreIcon::Compress:
        command_icons::AppendSvgPath(stroke, "M2.5 8.5v4a1 1 0 0 0 1 1h9a1 1 0 0 0 1-1v-4", true);
        command_icons::AppendSvgPath(stroke, "M8 2.5v7.5", true);
        command_icons::AppendSvgPath(stroke, "m5.25 7.25 2.75 2.75 2.75-2.75", true);
        command_icons::AppendSvgPath(stroke, "M2.5 5h2M11.5 5h2", true);
        break;
    case StoreIcon::AddArchive:
        command_icons::AppendSvgPath(stroke, "M2.25 4.75h11.5v8a1 1 0 0 1-1 1h-9.5a1 1 0 0 1-1-1z", true);
        command_icons::AppendSvgPath(stroke, "M1.75 2.25h12.5v2.5H1.75z", true);
        command_icons::AppendSvgPath(stroke, "M8 7v4.5M5.75 9.25h4.5", true);
        break;
    case StoreIcon::Test:
        command_icons::AppendSvgPath(stroke, "M3.25 2.5h6.5l3 3v8h-9.5z", true);
        command_icons::AppendSvgPath(stroke, "M9.5 2.5v3h3", true);
        command_icons::AppendSvgPath(stroke, "m5.75 9.5 1.5 1.5 3-3", true);
        break;
    case StoreIcon::Hash:
        command_icons::AppendSvgPath(stroke, "M6.6 2.25 5.4 13.75M10.6 2.25 9.4 13.75M2.75 5.75h10.75M2.5 10.25h10.75", true);
        break;
    case StoreIcon::Gear:
        command_icons::AppendSvgPath(stroke, GearPath().c_str(), true);
        command_icons::AppendSvgPath(stroke, "M5.95 8a2.05 2.05 0 1 0 4.1 0a2.05 2.05 0 1 0 -4.1 0z", true);
        break;
    case StoreIcon::Folder:
        command_icons::AppendSvgPath(stroke, "M1.75 4.25a1 1 0 0 1 1-1h3.1l1.4 1.5h6a1 1 0 0 1 1 1v6.75a1 1 0 0 1-1 1H2.75a1 1 0 0 1-1-1z", true);
        break;
    case StoreIcon::File:
        command_icons::AppendSvgPath(stroke, "M3.25 1.75h6l3.5 3.5v9h-9.5z", true);
        command_icons::AppendSvgPath(stroke, "M9.25 1.75v3.5h3.5", true);
        break;
    case StoreIcon::ChevronRight:
        command_icons::AppendSvgPath(stroke, "m6 3.5 4.5 4.5L6 12.5", true);
        break;
    case StoreIcon::ChevronDown:
        command_icons::AppendSvgPath(stroke, "m3.5 6 4.5 4.5L12.5 6", true);
        break;
    case StoreIcon::ChevronUp:
        command_icons::AppendSvgPath(stroke, "m3.5 10 4.5-4.5 4.5 4.5", true);
        break;
    case StoreIcon::More:
        command_icons::AppendSvgPath(fill, "M2.6 8a0.9 0.9 0 1 0 1.8 0a0.9 0.9 0 1 0 -1.8 0z", true);
        command_icons::AppendSvgPath(fill, "M7.1 8a0.9 0.9 0 1 0 1.8 0a0.9 0.9 0 1 0 -1.8 0z", true);
        command_icons::AppendSvgPath(fill, "M11.6 8a0.9 0.9 0 1 0 1.8 0a0.9 0.9 0 1 0 -1.8 0z", true);
        break;
    case StoreIcon::Check:
        command_icons::AppendSvgPath(stroke, "m3 8.5 3.25 3.25L13 5", true);
        break;
    case StoreIcon::Shield:
        command_icons::AppendSvgPath(stroke, "M8 1.75 13.25 3.5v4c0 3.1-2.25 5.35-5.25 6.75-3-1.4-5.25-3.65-5.25-6.75v-4z", true);
        command_icons::AppendSvgPath(stroke, "m5.75 8 1.5 1.5 3-3", true);
        break;
    case StoreIcon::Badge:
        command_icons::AppendSvgPath(stroke, SealPath().c_str(), true);
        command_icons::AppendSvgPath(stroke, "m5.6 8.1 1.65 1.65 3.15-3.3", true);
        break;
    case StoreIcon::Lock:
        command_icons::AppendSvgPath(stroke, "M4.5 7h7a1.5 1.5 0 0 1 1.5 1.5v4a1.5 1.5 0 0 1 -1.5 1.5h-7a1.5 1.5 0 0 1 -1.5 -1.5v-4a1.5 1.5 0 0 1 1.5 -1.5z", true);
        command_icons::AppendSvgPath(stroke, "M5.25 7V5a2.75 2.75 0 0 1 5.5 0v2", true);
        command_icons::AppendSvgPath(stroke, "M8 9.75v1.5", true);
        break;
    case StoreIcon::Eye:
        command_icons::AppendSvgPath(stroke, "M1.75 8S4 3.75 8 3.75 14.25 8 14.25 8 12 12.25 8 12.25 1.75 8 1.75 8z", true);
        command_icons::AppendSvgPath(stroke, "M6.1 8a1.9 1.9 0 1 0 3.8 0a1.9 1.9 0 1 0 -3.8 0z", true);
        break;
    case StoreIcon::Copy:
        command_icons::AppendSvgPath(stroke, "M7 5.5h5a1.5 1.5 0 0 1 1.5 1.5v5.5a1.5 1.5 0 0 1 -1.5 1.5h-5a1.5 1.5 0 0 1 -1.5 -1.5v-5.5a1.5 1.5 0 0 1 1.5 -1.5z", true);
        command_icons::AppendSvgPath(stroke, "M3.5 10.5h-.25a1 1 0 0 1-1-1V3a1 1 0 0 1 1-1h6.5a1 1 0 0 1 1 1v.25", true);
        break;
    case StoreIcon::Cut:
        command_icons::AppendSvgPath(stroke, "M2.5 12.0 a2.0 2.0 0 1 0 4.0 0 a2.0 2.0 0 1 0 -4.0 0z", true);
        command_icons::AppendSvgPath(stroke, "M9.5 12.0 a2.0 2.0 0 1 0 4.0 0 a2.0 2.0 0 1 0 -4.0 0z", true);
        command_icons::AppendSvgPath(stroke, "M5.9 10.6 12 1.5M10.1 10.6 4 1.5", true);
        break;
    case StoreIcon::Rename:
        command_icons::AppendSvgPath(stroke, "M2 5.5h8M2 10.5h5M11 3v10M9.5 3h3M9.5 13h3", true);
        break;
    case StoreIcon::Trash:
        command_icons::AppendSvgPath(stroke, "M2.5 4h11M6 4V2.5h4V4M4 4l.7 10h6.6L12 4", true);
        break;
    case StoreIcon::Properties:
        command_icons::AppendSvgPath(stroke, "M4.0 2.0h8.0a2.0 2.0 0 0 1 2.0 2.0v8.0a2.0 2.0 0 0 1 -2.0 2.0h-8.0a2.0 2.0 0 0 1 -2.0 -2.0v-8.0a2.0 2.0 0 0 1 2.0 -2.0z", true);
        command_icons::AppendSvgPath(stroke, "M5 5.5h6M5 8h6M5 10.5h4", true);
        break;
    case StoreIcon::Open:
        command_icons::AppendSvgPath(stroke, "M9.5 2.25h4.25V6.5", true);
        command_icons::AppendSvgPath(stroke, "M13.5 2.5 7.5 8.5", true);
        command_icons::AppendSvgPath(stroke, "M11.75 9.5v3.25a1 1 0 0 1-1 1h-7.5a1 1 0 0 1-1-1v-7.5a1 1 0 0 1 1-1H6.5", true);
        break;
    case StoreIcon::OpenWith:
        command_icons::AppendSvgPath(stroke, "M3.0 2.0h3.0a1.0 1.0 0 0 1 1.0 1.0v3.0a1.0 1.0 0 0 1 -1.0 1.0h-3.0a1.0 1.0 0 0 1 -1.0 -1.0v-3.0a1.0 1.0 0 0 1 1.0 -1.0z", true);
        command_icons::AppendSvgPath(stroke, "M10.0 2.0h3.0a1.0 1.0 0 0 1 1.0 1.0v3.0a1.0 1.0 0 0 1 -1.0 1.0h-3.0a1.0 1.0 0 0 1 -1.0 -1.0v-3.0a1.0 1.0 0 0 1 1.0 -1.0z", true);
        command_icons::AppendSvgPath(stroke, "M3.0 9.0h3.0a1.0 1.0 0 0 1 1.0 1.0v3.0a1.0 1.0 0 0 1 -1.0 1.0h-3.0a1.0 1.0 0 0 1 -1.0 -1.0v-3.0a1.0 1.0 0 0 1 1.0 -1.0z", true);
        command_icons::AppendSvgPath(stroke, "M11.5 9v5M9 11.5h5", true);
        break;
    case StoreIcon::Path:
        command_icons::AppendSvgPath(stroke, "M6.5 9.5 9.5 6.5M7 4.5l1-1a3 3 0 0 1 4.3 4.3l-1 1M9 11.5l-1 1a3 3 0 0 1-4.3-4.3l1-1", true);
        break;
    case StoreIcon::Undo:
        command_icons::AppendSvgPath(stroke, "M5 3.75 2.25 6.5 5 9.25", true);
        command_icons::AppendSvgPath(stroke, "M2.5 6.5h7.25a3.5 3.5 0 0 1 0 7H6", true);
        break;
    case StoreIcon::Close:
        command_icons::AppendSvgPath(stroke, "m4 4 8 8M12 4l-8 8", true);
        break;
    case StoreIcon::Plus:
        command_icons::AppendSvgPath(stroke, "M8 2.5v11M2.5 8h11", true);
        break;
    case StoreIcon::Minus:
        command_icons::AppendSvgPath(stroke, "M3 8h10", true);
        break;
    case StoreIcon::Square:
        command_icons::AppendSvgPath(stroke, "M4.5 3.5h7.0a1.0 1.0 0 0 1 1.0 1.0v7.0a1.0 1.0 0 0 1 -1.0 1.0h-7.0a1.0 1.0 0 0 1 -1.0 -1.0v-7.0a1.0 1.0 0 0 1 1.0 -1.0z", true);
        break;
    case StoreIcon::Back:
        command_icons::AppendSvgPath(stroke, "M13 8H3M7 4 3 8l4 4", true);
        break;
    case StoreIcon::Forward:
        command_icons::AppendSvgPath(stroke, "M3 8h10M9 4l4 4-4 4", true);
        break;
    case StoreIcon::Up:
        command_icons::AppendSvgPath(stroke, "M8 13V3M4 7l4-4 4 4", true);
        break;
    case StoreIcon::Refresh:
        command_icons::AppendSvgPath(stroke, "M13.25 8A5.25 5.25 0 1 1 11.6 4.2", true);
        command_icons::AppendSvgPath(stroke, "M12.25 1.75v3h-3", true);
        break;
    case StoreIcon::Image:
        command_icons::AppendSvgPath(stroke, "M3.0 2.5h10.0a1.5 1.5 0 0 1 1.5 1.5v8.0a1.5 1.5 0 0 1 -1.5 1.5h-10.0a1.5 1.5 0 0 1 -1.5 -1.5v-8.0a1.5 1.5 0 0 1 1.5 -1.5z", true);
        command_icons::AppendSvgPath(stroke, "M4.3 6.0 a1.2 1.2 0 1 0 2.4 0 a1.2 1.2 0 1 0 -2.4 0z", true);
        command_icons::AppendSvgPath(stroke, "m1.5 11.5 4-3.5 3 2.5 2-1.5 4 3", true);
        break;
    case StoreIcon::Camera:
        command_icons::AppendSvgPath(stroke, "M1.5 5a1 1 0 0 1 1-1h2l1.2-1.5h4.6L11.5 4h2a1 1 0 0 1 1 1v7.5a1 1 0 0 1-1 1h-11a1 1 0 0 1-1-1z", true);
        command_icons::AppendSvgPath(stroke, "M5.5 8.5 a2.5 2.5 0 1 0 5.0 0 a2.5 2.5 0 1 0 -5.0 0z", true);
        break;
    case StoreIcon::Film:
        command_icons::AppendSvgPath(stroke, "M3.0 2.5h10.0a1.5 1.5 0 0 1 1.5 1.5v8.0a1.5 1.5 0 0 1 -1.5 1.5h-10.0a1.5 1.5 0 0 1 -1.5 -1.5v-8.0a1.5 1.5 0 0 1 1.5 -1.5z", true);
        command_icons::AppendSvgPath(stroke, "m6.5 5.5 4 2.5-4 2.5z", true);
        break;
    case StoreIcon::TextSearch:
        command_icons::AppendSvgPath(stroke, "M2 3.5h12M2 6.5h6M2 9.5h4M2 12.5h4", true);
        command_icons::AppendSvgPath(stroke, "M8.5 10.0 a2.5 2.5 0 1 0 5.0 0 a2.5 2.5 0 1 0 -5.0 0z", true);
        command_icons::AppendSvgPath(stroke, "m13 12 1.5 1.5", true);
        break;
    case StoreIcon::Sparkle:
        command_icons::AppendSvgPath(stroke, "M8 1.5 9.2 6.8 14.5 8 9.2 9.2 8 14.5 6.8 9.2 1.5 8 6.8 6.8z", true);
        break;
    case StoreIcon::Puzzle:
        command_icons::AppendSvgPath(stroke, "M3 4.5h2.5a1.5 1.5 0 1 1 3 0H11v2.5a1.5 1.5 0 1 1 0 3V13H8.5a1.5 1.5 0 1 0-3 0H3V10a1.5 1.5 0 1 0 0-3z", true);
        break;
    case StoreIcon::Palette:
        command_icons::AppendSvgPath(stroke, "M8 1.5a6.5 6.5 0 0 0 0 13c1 0 1.5-.6 1.5-1.4 0-1.2-1-1.3-1-2.3 0-.8.6-1.3 1.4-1.3H12a2.5 2.5 0 0 0 2.5-2.5C14.5 4 11.6 1.5 8 1.5z", true);
        command_icons::AppendSvgPath(stroke, "M4.4 7.0 a0.6 0.6 0 1 0 1.2 0 a0.6 0.6 0 1 0 -1.2 0z", true);
        command_icons::AppendSvgPath(stroke, "M6.4 4.5 a0.6 0.6 0 1 0 1.2 0 a0.6 0.6 0 1 0 -1.2 0z", true);
        command_icons::AppendSvgPath(stroke, "M9.6 4.8 a0.6 0.6 0 1 0 1.2 0 a0.6 0.6 0 1 0 -1.2 0z", true);
        break;
    case StoreIcon::ListView:
        command_icons::AppendSvgPath(stroke, "M3.0 3.0h10.0a1.5 1.5 0 0 1 1.5 1.5v7.0a1.5 1.5 0 0 1 -1.5 1.5h-10.0a1.5 1.5 0 0 1 -1.5 -1.5v-7.0a1.5 1.5 0 0 1 1.5 -1.5z", true);
        command_icons::AppendSvgPath(stroke, "m6 6.5 1.5 1.5L6 9.5M9 9.5h2", true);
        break;
    case StoreIcon::Tabs:
        command_icons::AppendSvgPath(stroke, "M3.0 3.5h10.0a1.5 1.5 0 0 1 1.5 1.5v7.0a1.5 1.5 0 0 1 -1.5 1.5h-10.0a1.5 1.5 0 0 1 -1.5 -1.5v-7.0a1.5 1.5 0 0 1 1.5 -1.5z", true);
        command_icons::AppendSvgPath(stroke, "M1.5 6.5h13M5 3.5v3", true);
        break;
    case StoreIcon::Power:
        command_icons::AppendSvgPath(stroke, "M8 1.5v6M4.5 3.8a5.5 5.5 0 1 0 7 0", true);
        break;
    case StoreIcon::Duplicate:
        command_icons::AppendSvgPath(stroke, "M3.0 4.5h6.0a1.5 1.5 0 0 1 1.5 1.5v6.0a1.5 1.5 0 0 1 -1.5 1.5h-6.0a1.5 1.5 0 0 1 -1.5 -1.5v-6.0a1.5 1.5 0 0 1 1.5 -1.5z", true);
        command_icons::AppendSvgPath(stroke, "M5.5 4.5v-1a1 1 0 0 1 1-1h7a1 1 0 0 1 1 1v7a1 1 0 0 1-1 1h-1", true);
        break;
    case StoreIcon::Cube:
        command_icons::AppendSvgPath(stroke, "M8 1.5 14 4.5v7L8 14.5 2 11.5v-7z", true);
        command_icons::AppendSvgPath(stroke, "M2 4.5 8 7.5l6-3M8 7.5v7", true);
        break;
    case StoreIcon::Lines:
        command_icons::AppendSvgPath(stroke, "M5.5 4h8M5.5 8h8M5.5 12h8M2.5 4h.5M2.5 8h.5M2.5 12h.5", true);
        break;
    case StoreIcon::Keyboard:
        command_icons::AppendSvgPath(stroke, "M3.0 3.5h10.0a1.5 1.5 0 0 1 1.5 1.5v6.0a1.5 1.5 0 0 1 -1.5 1.5h-10.0a1.5 1.5 0 0 1 -1.5 -1.5v-6.0a1.5 1.5 0 0 1 1.5 -1.5z", true);
        command_icons::AppendSvgPath(stroke, "M4 6.5h.5M6.5 6.5H7M9 6.5h.5M11.5 6.5h.5M4.5 9.5h7", true);
        break;
    case StoreIcon::Info:
        command_icons::AppendSvgPath(stroke, "M2 8a6 6 0 1 0 12 0a6 6 0 1 0 -12 0z", true);
        command_icons::AppendSvgPath(stroke, "M8 7.25v4", true);
        command_icons::AppendSvgPath(fill, "M7.4 5a0.6 0.6 0 1 0 1.2 0a0.6 0.6 0 1 0 -1.2 0z", true);
        break;
    case StoreIcon::Warn:
        command_icons::AppendSvgPath(stroke, "M8 2.25 14 13H2z", true);
        command_icons::AppendSvgPath(stroke, "M8 6.5v3", true);
        command_icons::AppendSvgPath(fill, "M7.4 11.3a0.6 0.6 0 1 0 1.2 0a0.6 0.6 0 1 0 -1.2 0z", true);
        break;
    case StoreIcon::Sidebar:
        command_icons::AppendSvgPath(stroke, "M3.5 2.5h9.0a2.0 2.0 0 0 1 2.0 2.0v7.0a2.0 2.0 0 0 1 -2.0 2.0h-9.0a2.0 2.0 0 0 1 -2.0 -2.0v-7.0a2.0 2.0 0 0 1 2.0 -2.0z", true);
        command_icons::AppendSvgPath(stroke, "M6 2.5v11", true);
        break;
    case StoreIcon::Panes:
        command_icons::AppendSvgPath(stroke, "M3.0 4.0h7.0a1.5 1.5 0 0 1 1.5 1.5v7.0a1.5 1.5 0 0 1 -1.5 1.5h-7.0a1.5 1.5 0 0 1 -1.5 -1.5v-7.0a1.5 1.5 0 0 1 1.5 -1.5z", true);
        command_icons::AppendSvgPath(stroke, "M4.5 4V3a1 1 0 0 1 1-1h8a1 1 0 0 1 1 1v8a1 1 0 0 1-1 1h-2", true);
        break;
    case StoreIcon::Contrast:
        command_icons::AppendSvgPath(stroke, "M1.5 8.0 a6.5 6.5 0 1 0 13.0 0 a6.5 6.5 0 1 0 -13.0 0z", true);
        command_icons::AppendSvgPath(fill, "M8 1.5v13A6.5 6.5 0 0 0 8 1.5z", true);
        break;
    case StoreIcon::Download:
        command_icons::AppendSvgPath(stroke, "M8 2v8", true);
        command_icons::AppendSvgPath(stroke, "M4.75 6.75 8 10l3.25-3.25", true);
        command_icons::AppendSvgPath(stroke, "M2.75 13.5h10.5", true);
        break;
    case StoreIcon::CloudOff:
        command_icons::AppendSvgPath(stroke, "M4.25 12.75h7.25a2.75 2.75 0 0 0 .6-5.45A4.25 4.25 0 0 0 4 6.1a3.35 3.35 0 0 0 .25 6.65z", true);
        command_icons::AppendSvgPath(stroke, "m2.25 2.25 11.5 11.5", true);
        break;
    case StoreIcon::Drag:
        command_icons::AppendSvgPath(stroke, "M5.4 4.0 a0.6 0.6 0 1 0 1.2 0 a0.6 0.6 0 1 0 -1.2 0z", true);
        command_icons::AppendSvgPath(stroke, "M9.4 4.0 a0.6 0.6 0 1 0 1.2 0 a0.6 0.6 0 1 0 -1.2 0z", true);
        command_icons::AppendSvgPath(stroke, "M5.4 8.0 a0.6 0.6 0 1 0 1.2 0 a0.6 0.6 0 1 0 -1.2 0z", true);
        command_icons::AppendSvgPath(stroke, "M9.4 8.0 a0.6 0.6 0 1 0 1.2 0 a0.6 0.6 0 1 0 -1.2 0z", true);
        command_icons::AppendSvgPath(stroke, "M5.4 12.0 a0.6 0.6 0 1 0 1.2 0 a0.6 0.6 0 1 0 -1.2 0z", true);
        command_icons::AppendSvgPath(stroke, "M9.4 12.0 a0.6 0.6 0 1 0 1.2 0 a0.6 0.6 0 1 0 -1.2 0z", true);
        break;
    case StoreIcon::Sort:
        command_icons::AppendSvgPath(stroke, "M4.5 2.5v11M2 11l2.5 2.5L7 11M9 4h5M9 7.5h4M9 11h3", true);
        break;
    case StoreIcon::Filter:
        command_icons::AppendSvgPath(stroke, "M2 3.5h12M4.5 8h7M7 12.5h2", true);
        break;
    case StoreIcon::NewFolder:
        command_icons::AppendSvgPath(stroke, "M1.5 4a1 1 0 0 1 1-1h3.4l1.6 1.6h6a1 1 0 0 1 1 1V12a1 1 0 0 1-1 1h-11a1 1 0 0 1-1-1z", true);
        command_icons::AppendSvgPath(stroke, "M8 7v4M6 9h4", true);
        break;
    case StoreIcon::Star:
        command_icons::AppendSvgPath(stroke, "m8 1.8 1.9 3.9 4.3.6-3.1 3 .7 4.3L8 11.6l-3.8 2 .7-4.3-3.1-3 4.3-.6z", true);
        break;
    case StoreIcon::Pc:
        command_icons::AppendSvgPath(stroke, "M2.7 2.5h10.6a1.2 1.2 0 0 1 1.2 1.2v6.1a1.2 1.2 0 0 1 -1.2 1.2h-10.6a1.2 1.2 0 0 1 -1.2 -1.2v-6.1a1.2 1.2 0 0 1 1.2 -1.2z", true);
        command_icons::AppendSvgPath(stroke, "M5.5 13.5h5M8 11v2.5", true);
        break;
    case StoreIcon::Disk:
        command_icons::AppendSvgPath(stroke, "M3.0 4.5h10.0a1.5 1.5 0 0 1 1.5 1.5v4.0a1.5 1.5 0 0 1 -1.5 1.5h-10.0a1.5 1.5 0 0 1 -1.5 -1.5v-4.0a1.5 1.5 0 0 1 1.5 -1.5z", true);
        command_icons::AppendSvgPath(stroke, "M11.5 8h.5", true);
        break;
    case StoreIcon::ChevronLeft:
        command_icons::AppendSvgPath(stroke, "M10 3.5 5.5 8l4.5 4.5", true);
        break;
    default: break;
    }
}

struct GeometryPair {
    ComPtr<ID2D1PathGeometry> stroke, fill;
};
struct CachedCustomLayer {
    ComPtr<ID2D1PathGeometry> geometry;
    uint32_t draw_flags = 0;
    uint32_t color_role = 0;
    uint32_t argb = 0;
    uint32_t stroke_width_milli = 0;
    uint32_t opacity_milli = 1000u;
};
struct CachedCustomIcon {
    uint64_t revision = 0;
    uint32_t view_box_milli = 16000u;
    std::vector<CachedCustomLayer> layers;
};
struct IconCache {
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1StrokeStyle> style;
    std::array<GeometryPair, static_cast<size_t>(StoreIcon::Count)> paths;
    std::unordered_map<uint32_t, CachedCustomIcon> custom_icons;
};

IconCache& ThreadIconCache(ID2D1DeviceContext* dc, ComPtr<ID2D1Factory>& out_factory) {
    thread_local IconCache cache;
    dc->GetFactory(out_factory.GetAddressOf());
    if (cache.factory.Get() != out_factory.Get()) {
        cache = IconCache{};
        cache.factory = out_factory;
    }
    if (!cache.style && out_factory) {
        auto props = D2D1::StrokeStyleProperties();
        props.startCap = props.endCap = props.dashCap = D2D1_CAP_STYLE_ROUND;
        props.lineJoin = D2D1_LINE_JOIN_ROUND;
        out_factory->CreateStrokeStyle(props, nullptr, 0, cache.style.GetAddressOf());
    }
    return cache;
}

D2D1_COLOR_F ResolveLayerColor(uint32_t role, uint32_t argb, uint32_t opacity_milli,
                               D2D1_COLOR_F foreground, const Theme* theme) noexcept {
    const bool dark = theme ? ((theme->bg.r + theme->bg.g + theme->bg.b) < 1.5f) : true;
    D2D1_COLOR_F base = foreground;
    switch (role) {
    case PULSE_ICON_COLOR_ACCENT:
        base = theme ? theme->accent : HexColor(0x0078D4);
        base.a *= foreground.a;
        break;
    case PULSE_ICON_COLOR_SUCCESS:
        base = dark ? HexColor(0x5FD4A0, foreground.a) : HexColor(0x0F7B4F, foreground.a);
        break;
    case PULSE_ICON_COLOR_WARNING:
        base = dark ? HexColor(0xF2C14E, foreground.a) : HexColor(0xC98A00, foreground.a);
        break;
    case PULSE_ICON_COLOR_DANGER:
        base = theme ? theme->danger : (dark ? HexColor(0xFF99A4) : HexColor(0xC42B1C));
        base.a *= foreground.a;
        break;
    case PULSE_ICON_COLOR_RGB: {
        const uint32_t a8 = (argb >> 24) & 0xFFu;
        const float alpha = (a8 == 0 ? 1.0f : static_cast<float>(a8) / 255.0f) * foreground.a;
        base = HexColor(argb & 0x00FFFFFFu, alpha);
        break;
    }
    default:
        break;
    }
    const float op = std::clamp(static_cast<float>(opacity_milli == 0 ? 1000u : opacity_milli) / 1000.0f, 0.0f, 1.0f);
    base.a *= op;
    return base;
}
}
void DrawStoreIcon(ID2D1DeviceContext* dc, ID2D1Brush* brush, D2D1_RECT_F bounds,
                   StoreIcon icon, float stroke_dip) {
    if (!dc || !brush || icon <= StoreIcon::None || icon >= StoreIcon::Count) return;
    const float side = (std::min)(bounds.right - bounds.left, bounds.bottom - bounds.top);
    if (side <= 0) return;
    // Geometries and stroke styles are factory resources, not device resources.
    // Retaining the factory prevents pointer reuse; a factory change drops every entry.
    ComPtr<ID2D1Factory> factory;
    auto& cache = ThreadIconCache(dc, factory);
    if (!factory || !cache.style) return;
    auto& paths = cache.paths[static_cast<size_t>(icon)];
    if (!paths.stroke) {
        GeometryPair built;
        ComPtr<ID2D1GeometrySink> stroke, fill;
        if (FAILED(factory->CreatePathGeometry(built.stroke.GetAddressOf())) ||
            FAILED(factory->CreatePathGeometry(built.fill.GetAddressOf())) ||
            FAILED(built.stroke->Open(stroke.GetAddressOf())) ||
            FAILED(built.fill->Open(fill.GetAddressOf()))) return;
        Build(icon, stroke.Get(), fill.Get());
        const HRESULT stroke_result = stroke->Close(), fill_result = fill->Close();
        if (FAILED(stroke_result) || FAILED(fill_result)) return;
        paths = std::move(built);
    }
    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);
    const float scale = side / 16.0f;
    dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
        D2D1::Matrix3x2F::Translation((bounds.left + bounds.right - side) * .5f,
                                      (bounds.top + bounds.bottom - side) * .5f) * old);
    dc->DrawGeometry(paths.stroke.Get(), brush, stroke_dip / scale, cache.style.Get());
    dc->FillGeometry(paths.fill.Get(), brush);
    dc->DrawGeometry(paths.fill.Get(), brush, stroke_dip / scale, cache.style.Get());
    dc->SetTransform(old);
}
void DrawStoreIcon(ID2D1DeviceContext* dc, StoreIcon icon, D2D1_RECT_F bounds,
                   D2D1_COLOR_F color, float stroke_dip) {
    if (!dc) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(dc->CreateSolidColorBrush(color, brush.GetAddressOf())))
        DrawStoreIcon(dc, brush.Get(), bounds, icon, stroke_dip);
}

bool DrawExtensionIcon(ID2D1DeviceContext* dc, uint32_t icon_id, D2D1_RECT_F bounds,
                       D2D1_COLOR_F foreground, const Theme* theme, float stroke_dip) {
    if (!dc) return false;
    if (extensions::icons::IsBuiltinIconId(icon_id)) {
        DrawStoreIcon(dc, StoreIconFromId(icon_id), bounds, foreground, stroke_dip);
        return true;
    }
    if (!extensions::icons::IsCustomIconId(icon_id)) return false;
    const float side = (std::min)(bounds.right - bounds.left, bounds.bottom - bounds.top);
    if (side <= 0.0f) return false;

    const auto snapshot = extensions::icons::GlobalRegistry().Snapshot(icon_id);
    if (!snapshot || snapshot->layers.empty()) return false;

    ComPtr<ID2D1Factory> factory;
    auto& cache = ThreadIconCache(dc, factory);
    if (!factory || !cache.style) return false;

    auto found = cache.custom_icons.find(icon_id);
    if (found == cache.custom_icons.end() || found->second.revision != snapshot->revision) {
        CachedCustomIcon built;
        built.revision = snapshot->revision;
        built.view_box_milli = snapshot->view_box_milli ? snapshot->view_box_milli : 16000u;
        built.layers.reserve(snapshot->layers.size());
        for (const auto& layer : snapshot->layers) {
            CachedCustomLayer cached_layer;
            cached_layer.draw_flags = layer.draw_flags;
            cached_layer.color_role = layer.color_role;
            cached_layer.argb = layer.argb;
            cached_layer.stroke_width_milli = layer.stroke_width_milli;
            cached_layer.opacity_milli = layer.opacity_milli;
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(factory->CreatePathGeometry(cached_layer.geometry.GetAddressOf())) ||
                FAILED(cached_layer.geometry->Open(sink.GetAddressOf()))) {
                return false;
            }
            std::string path_str = layer.path_data;
            if ((layer.draw_flags & PULSE_ICON_DRAW_CLOSED) != 0) {
                while (!path_str.empty() && (path_str.back() == ' ' || path_str.back() == '\t' ||
                       path_str.back() == '\r' || path_str.back() == '\n')) {
                    path_str.pop_back();
                }
                if (!path_str.empty() && path_str.back() != 'z' && path_str.back() != 'Z') {
                    path_str.push_back('z');
                }
            }
            const bool filled = (layer.draw_flags & PULSE_ICON_DRAW_FILL) != 0;
            command_icons::AppendSvgPath(sink.Get(), path_str.c_str(), filled);
            if (FAILED(sink->Close())) return false;
            built.layers.push_back(std::move(cached_layer));
        }
        if (cache.custom_icons.size() >= 256) cache.custom_icons.clear();
        found = cache.custom_icons.insert_or_assign(icon_id, std::move(built)).first;
    }

    const CachedCustomIcon& icon = found->second;
    const float view_box = (std::max)(4.0f, static_cast<float>(icon.view_box_milli) / 1000.0f);
    const float scale = side / view_box;
    const float dip_to_grid = side / 16.0f;
    if (scale <= 0.0f || dip_to_grid <= 0.0f) return false;

    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
        D2D1::Matrix3x2F::Translation((bounds.left + bounds.right - side) * 0.5f,
                                      (bounds.top + bounds.bottom - side) * 0.5f) * old);

    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(dc->CreateSolidColorBrush(foreground, brush.GetAddressOf()))) {
        for (const auto& layer : icon.layers) {
            const D2D1_COLOR_F color = ResolveLayerColor(
                layer.color_role, layer.argb, layer.opacity_milli, foreground, theme);
            if (color.a <= 0.001f) continue;
            brush->SetColor(color);
            if ((layer.draw_flags & PULSE_ICON_DRAW_FILL) != 0) {
                dc->FillGeometry(layer.geometry.Get(), brush.Get());
            }
            if ((layer.draw_flags & PULSE_ICON_DRAW_STROKE) != 0) {
                const float layer_dip = layer.stroke_width_milli
                    ? (static_cast<float>(layer.stroke_width_milli) / 1000.0f) * (dip_to_grid > 0.0f ? (stroke_dip / 1.35f) : 1.0f)
                    : stroke_dip;
                dc->DrawGeometry(layer.geometry.Get(), brush.Get(), layer_dip / scale, cache.style.Get());
            }
        }
    }
    dc->SetTransform(old);
    return true;
}

bool DrawExtensionIconGlyph(ID2D1DeviceContext* dc, std::wstring_view glyph, D2D1_RECT_F bounds,
                            D2D1_COLOR_F foreground, const Theme* theme, float stroke_dip) {
    const auto resolved = extensions::icons::ResolveWideIconGlyph(glyph);
    if (!resolved) return false;
    return DrawExtensionIcon(dc, *resolved, bounds, foreground, theme, stroke_dip);
}
}

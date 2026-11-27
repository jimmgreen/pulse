#include "host/render/vector_glyphs.h"

#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pulse::desktop::render {
namespace {

using Microsoft::WRL::ComPtr;

// 16 x 16 outline icons in the Fluent manner: 1.3 strokes, round caps and joins. Paths are SVG path data.
struct GlyphIcon {
    wchar_t glyph;
    const char* path;
};

constexpr const char* kFolder =
    "M1.75 4.25a1.5 1.5 0 0 1 1.5-1.5h2.9l1.5 1.5h5.1a1.5 1.5 0 0 1 1.5 1.5v6.5a1.5 1.5 0 0 1-1.5 1.5H3.25a1.5 1.5 0 0 1-1.5-1.5z";
constexpr const char* kDocument =
    "M4 1.75h5.25L12.75 5.25v8a1 1 0 0 1-1 1h-7.75a1 1 0 0 1-1-1V2.75a1 1 0 0 1 1-1zM9 1.75v3.5h3.75";
constexpr const char* kEye =
    "M1.75 8C1.75 8 4 3.75 8 3.75S14.25 8 14.25 8 12 12.25 8 12.25 1.75 8 1.75 8zM6.25 8a1.75 1.75 0 1 0 3.5 0a1.75 1.75 0 1 0-3.5 0";

constexpr GlyphIcon kIcons[] = {
    {L'\xE8B1', "M1.75 4.75h2.1c1.35 0 2.2.65 2.95 1.8l2.4 3.65c.75 1.15 1.6 1.8 2.95 1.8h2.1M12.5 10.25l1.75 1.75-1.75 1.75M1.75 11.25h2.1c.95 0 1.65-.3 2.25-.9M9.9 5.65c.6-.6 1.3-.9 2.25-.9h2.1M12.5 3l1.75 1.75-1.75 1.75"},
    {L'\xE775', "M8 1.75a6.25 6.25 0 1 0 0 12.5a6.25 6.25 0 1 0 0-12.5M1.75 8h12.5M8 1.75c1.8 1.7 2.6 3.8 2.6 6.25S9.8 12.55 8 14.25M8 1.75C6.2 3.45 5.4 5.55 5.4 8s.8 4.55 2.6 6.25"},
    {L'\xE977', "M2.25 3.25h11.5a.75.75 0 0 1 .75.75v6.5a.75.75 0 0 1-.75.75H2.25a.75.75 0 0 1-.75-.75V4a.75.75 0 0 1 .75-.75zM6 13.75h4M8 11.25v2.5"},
    {L'\xE707', "M8 14.25s4.5-4.1 4.5-7.75a4.5 4.5 0 0 0-9 0C3.5 10.15 8 14.25 8 14.25zM8 4.75a1.75 1.75 0 1 0 0 3.5a1.75 1.75 0 1 0 0-3.5"},
    {L'\xE7EF', "M8 1.75l5.25 2v4c0 3.2-2.3 5.6-5.25 6.5-2.95-.9-5.25-3.3-5.25-6.5v-4z"},
    {L'\xE8EC', "M2.75 2.75h5l5.5 5.5-5 5-5.5-5.5zM5.5 5.5h.01"},
    {L'\xE916', "M8 3.75a5 5 0 1 0 0 10a5 5 0 1 0 0-10M8 6.25v2.5l1.75 1M6.5 1.75h3"},
    {L'\xE9D2', "M2.25 13.25h11.5M3.75 13.25V9.5M6.75 13.25V5.75M9.75 13.25V8M12.75 13.25V3.25"},
    {L'\xEC4E', "M2.25 3.25h11.5a.75.75 0 0 1 .75.75v6.5a.75.75 0 0 1-.75.75H2.25a.75.75 0 0 1-.75-.75V4a.75.75 0 0 1 .75-.75zM6 13.75h4M8 11.25v2.5"},
    {L'\xE71D', "M2.75 2.75h4v4h-4zM9.25 2.75h4v4h-4zM2.75 9.25h4v4h-4zM9.25 9.25h4v4h-4z"},
    {L'\xE787', "M3 3.25h10a1 1 0 0 1 1 1v8.5a1 1 0 0 1-1 1H3a1 1 0 0 1-1-1v-8.5a1 1 0 0 1 1-1zM2 6.25h12M5.25 1.75v3M10.75 1.75v3"},
    {L'\xE73A', "M8 1.75a6.25 6.25 0 1 0 0 12.5a6.25 6.25 0 1 0 0-12.5M5.25 8.25l1.9 1.9 3.6-3.9"},
    {L'\xE9D9', "M2.25 11.5a5.75 5.75 0 1 1 11.5 0M8 11.5l3-3.5M4.25 11.5h.01M11.75 11.5h.01"},
    {L'\xE706', "M8 5.25a2.75 2.75 0 1 0 0 5.5a2.75 2.75 0 1 0 0-5.5M8 1.5v1.5M8 13v1.5M1.5 8H3M13 8h1.5M3.4 3.4l1.05 1.05M11.55 11.55l1.05 1.05M3.4 12.6l1.05-1.05M11.55 4.45l1.05-1.05"},
    {L'\xE738', "M3.25 8h9.5"},
    {L'\xE73E', "M3.25 8.5l3 3 6.5-7"},
    {L'\xE8FD', "M2.75 4h10.5M2.75 8h10.5M2.75 12h10.5"},
    {L'\xE771', "M8 1.75a6.25 6.25 0 0 0 0 12.5c.9 0 1.25-.6 1.25-1.2 0-.8-.6-1.05-.6-1.8 0-.7.55-1.25 1.25-1.25h1.6a2.75 2.75 0 0 0 2.75-2.75c0-3-2.8-5.5-6.25-5.5zM4.75 7.25h.01M6.5 4.75h.01M9.75 4.75h.01M11.5 7.25h.01"},
    {L'\xE70B', "M3 2.25h10a.75.75 0 0 1 .75.75v7.25l-3.5 3.5H3a.75.75 0 0 1-.75-.75V3A.75.75 0 0 1 3 2.25zM13.75 10.25h-3.5v3.5M5 5.5h6M5 8h4"},
    {L'\xE70D', "M3.5 6 8 10.5 12.5 6"},
    {L'\xE70E', "M3.5 10 8 5.5 12.5 10"},
    {L'\xE70F', "M10.75 2.75l2.5 2.5-7.75 7.75-3.25.75.75-3.25zM9.25 4.25l2.5 2.5"},
    {L'\xE710', "M8 2.75v10.5M2.75 8h10.5"},
    {L'\xE711', "M3.75 3.75l8.5 8.5M12.25 3.75l-8.5 8.5"},
    {L'\xE712', "M3.5 8h.01M8 8h.01M12.5 8h.01"},
    {L'\xE713', "M8 5.75a2.25 2.25 0 1 0 0 4.5a2.25 2.25 0 1 0 0-4.5M8 1.75v1.75M8 12.5v1.75M1.75 8H3.5M12.5 8h1.75M3.6 3.6l1.2 1.2M11.2 11.2l1.2 1.2M3.6 12.4l1.2-1.2M11.2 4.8l1.2-1.2"},
    {L'\xE718', "M9.75 1.75l4.5 4.5L12 7l-2.5 2.5.5 3-1.25 1.25-6.5-6.5L3.5 6l3 .5L9 4zM5.25 10.75 1.75 14.25"},
    {L'\xE71B', "M6.75 9.25a3 3 0 0 0 4.25 0l2-2a3 3 0 0 0-4.25-4.25l-.75.75M9.25 6.75a3 3 0 0 0-4.25 0l-2 2a3 3 0 0 0 4.25 4.25l.75-.75"},
    {L'\xE71C', "M2.25 3h11.5L9.25 8.5v4.25l-2.5 1.25V8.5z"},
    {L'\xE721', "M6.75 2.25a4.5 4.5 0 1 0 0 9a4.5 4.5 0 1 0 0-9M10 10l4.25 4.25"},
    {L'\xE724', "M1.75 8 14.25 2 11 14.25 8 9.5zM8 9.5l6.25-7.5"},
    {L'\xE72C', "M13.25 8a5.25 5.25 0 1 1-1.55-3.7M13.25 2.25v3h-3"},
    {L'\xE72D', "M11.5 2.25a1.75 1.75 0 1 0 0 3.5a1.75 1.75 0 1 0 0-3.5M4.5 6.25a1.75 1.75 0 1 0 0 3.5a1.75 1.75 0 1 0 0-3.5M11.5 10.25a1.75 1.75 0 1 0 0 3.5a1.75 1.75 0 1 0 0-3.5M6 7.1l4-2.2M6 8.9l4 2.2"},
    {L'\xE73E', "M2.75 8.5 6.25 12l7-8"},
    {L'\xE740', "M2.25 6V2.25H6M10 2.25h3.75V6M13.75 10v3.75H10M6 13.75H2.25V10"},
    {L'\xE74B', "M8 2.5v11M3.75 9.25 8 13.5l4.25-4.25"},
    {L'\xE74D', "M2.75 4.25h10.5M6 4.25v-1.5a.75.75 0 0 1 .75-.75h2.5a.75.75 0 0 1 .75.75v1.5M4 4.25l.6 8.6a1 1 0 0 0 1 .9h4.8a1 1 0 0 0 1-.9l.6-8.6M6.75 7v4M9.25 7v4"},
    {L'\xE756', "M2.25 2.75h11.5a1 1 0 0 1 1 1v8.5a1 1 0 0 1-1 1H2.25a1 1 0 0 1-1-1v-8.5a1 1 0 0 1 1-1zM4.25 6l2 2-2 2M7.75 10.5h3.5"},
    {L'\xE76C', "M6 3.5 10.5 8 6 12.5"},
    {L'\xE771', "M9.5 2.25l4.25 4.25-5 5-4.25-4.25zM4.5 7.25 2.25 13.75 8.75 11.5"},
    {L'\xE77A', "M9.75 1.75l4.5 4.5L12 7l-2.5 2.5.5 3-1.25 1.25-6.5-6.5L3.5 6l3 .5L9 4zM5.25 10.75 1.75 14.25M2 2l12 12"},
    {L'\xE790', "M8 1.75a6.25 6.25 0 1 0 0 12.5c.9 0 1.25-.55 1.25-1.1 0-.75-.6-1-.6-1.75s.55-1.15 1.35-1.15H12a2.25 2.25 0 0 0 2.25-2.25C14.25 4.5 11.5 1.75 8 1.75zM5 6.75h.01M7.5 4.5h.01M10.5 5h.01M4.75 9.75h.01"},
    {L'\xE7A7', "M4.5 6.25H10a3.5 3.5 0 0 1 0 7H6.5M6.75 3.75 4.25 6.25l2.5 2.5"},
    {L'\xE7AC', "M6.75 3.25h-4.5v10.5h10.5v-4.5M8.75 2.25h5v5M13.75 2.25 7 9"},
    {L'\xE7B8', "M2.25 3.25h11.5v2.75H2.25zM3.25 6v6.5a1 1 0 0 0 1 1h7.5a1 1 0 0 0 1-1V6M6.25 8.75h3.5"},
    {L'\xE7BA', "M8 2 14.5 13.5h-13zM8 6.25v3.5M8 11.75h.01"},
    {L'\xE7C4', "M2.25 3.25h4.5v4.5h-4.5zM9.25 3.25h4.5v4.5h-4.5zM2.25 10.25h4.5v2.5h-4.5zM9.25 10.25h4.5v2.5h-4.5z"},
    {L'\xE7F4', "M2.25 2.75h11.5a1 1 0 0 1 1 1v6.5a1 1 0 0 1-1 1H2.25a1 1 0 0 1-1-1v-6.5a1 1 0 0 1 1-1zM6 14.25h4M8 11.25v3"},
    {L'\xE7F8', "M3.25 3.25h9.5a.75.75 0 0 1 .75.75v6.25h-11V4a.75.75 0 0 1 .75-.75zM1.25 10.25h13.5V11a1.5 1.5 0 0 1-1.5 1.5H2.75a1.5 1.5 0 0 1-1.5-1.5z"},
    {L'\xE81C', "M2.5 8a5.5 5.5 0 1 0 1.6-3.9M2.25 2.5V5h2.5M8 5v3l2 1.25"},
    {L'\xE823', "M2 8a6 6 0 1 0 12 0a6 6 0 1 0-12 0M8 4.75V8l2.25 1.5"},
    {L'\xE890', kEye},
    {L'\xE895', "M13 6.25A5.25 5.25 0 0 0 3.5 4.5M3 9.75a5.25 5.25 0 0 0 9.5 1.75M13.25 2.75v3.5h-3.5M2.75 13.25v-3.5h3.5"},
    {L'\xE896', "M8 2v8.5M4.5 7.25 8 10.75l3.5-3.5M2.75 13.75h10.5"},
    {L'\xE8A5', kDocument},
    {L'\xE8AC', "M2.25 4.75h7.5M2.25 11.25h7.5M12.25 2.25v11.5M10.75 2.25h3M10.75 13.75h3"},
    {L'\xE8B7', kFolder},
    {L'\xE8C6', "M4.5 10.75a1.75 1.75 0 1 0 0 3.5a1.75 1.75 0 1 0 0-3.5M11.5 10.75a1.75 1.75 0 1 0 0 3.5a1.75 1.75 0 1 0 0-3.5M5.75 11.25 11.75 1.75M10.25 11.25 4.25 1.75"},
    {L'\xE8C8', "M5.25 5.25h7.5a1 1 0 0 1 1 1v7.5a1 1 0 0 1-1 1h-7.5a1 1 0 0 1-1-1v-7.5a1 1 0 0 1 1-1zM2.25 10.25v-7a1 1 0 0 1 1-1h7"},
    {L'\xE8CB', "M4.5 2.5v11M2.25 11.25 4.5 13.5l2.25-2.25M11.5 13.5v-11M9.25 4.75 11.5 2.5l2.25 2.25"},
    {L'\xE8DE', "M1.75 4.25a1.5 1.5 0 0 1 1.5-1.5h2.9l1.5 1.5h5.1a1.5 1.5 0 0 1 1.5 1.5v6.5a1.5 1.5 0 0 1-1.5 1.5H3.25a1.5 1.5 0 0 1-1.5-1.5zM5.75 9h4.5M8.75 7.25 10.5 9l-1.75 1.75"},
    {L'\xE8E5', "M4 1.75h5.25L12.75 5.25v8a1 1 0 0 1-1 1h-7.75a1 1 0 0 1-1-1V2.75a1 1 0 0 1 1-1zM5.5 9.75h5M8.75 8l1.75 1.75-1.75 1.75"},
    {L'\xE8F4', "M1.75 4.25a1.5 1.5 0 0 1 1.5-1.5h2.9l1.5 1.5h5.1a1.5 1.5 0 0 1 1.5 1.5v6.5a1.5 1.5 0 0 1-1.5 1.5H3.25a1.5 1.5 0 0 1-1.5-1.5zM8 6.75v4.5M5.75 9h4.5"},
    {L'\xE943', "M5.5 4 1.75 8 5.5 12M10.5 4l3.75 4-3.75 4"},
    {L'\xE945', "M9 1.75 3.75 9h4l-1 5.25L12.25 7h-4z"},
    {L'\xE946', "M2 8a6 6 0 1 0 12 0a6 6 0 1 0-12 0M8 7.25v4M8 4.75h.01"},
    {L'\xE95E', "M8 13.75S2.25 10.35 2.25 6.35A3.1 3.1 0 0 1 8 4.4a3.1 3.1 0 0 1 5.75 1.95c0 4-5.75 7.4-5.75 7.4z"},
    {L'\xEB9F', "M2.75 2.75h10.5a1 1 0 0 1 1 1v8.5a1 1 0 0 1-1 1H2.75a1 1 0 0 1-1-1v-8.5a1 1 0 0 1 1-1zM1.75 11l3.5-3.5 3 3 2-2 4 4M10.25 5.75a1 1 0 1 0 2 0a1 1 0 1 0-2 0"},
    {L'\xEC50', "M1.75 4.25a1.5 1.5 0 0 1 1.5-1.5h2.9l1.5 1.5h5.1a1.5 1.5 0 0 1 1.5 1.5v6.5a1.5 1.5 0 0 1-1.5 1.5H3.25a1.5 1.5 0 0 1-1.5-1.5zM1.75 7h12.5"},
    {L'\xED1A', "M1.75 8C1.75 8 4 3.75 8 3.75S14.25 8 14.25 8 12 12.25 8 12.25 1.75 8 1.75 8zM6.25 8a1.75 1.75 0 1 0 3.5 0a1.75 1.75 0 1 0-3.5 0M2.5 2.5l11 11"},
    {L'\xEDA2', "M2.25 9.25h11.5v3a1 1 0 0 1-1 1H3.25a1 1 0 0 1-1-1zM2.25 9.25 4 3.5a1 1 0 0 1 .95-.75h6.1a1 1 0 0 1 .95.75l1.75 5.75M11 11.25h.01"},
};

const char* FindPath(wchar_t glyph) {
    for (const auto& icon : kIcons) {
        if (icon.glyph == glyph) return icon.path;
    }
    return nullptr;
}

// ---- SVG path data -> Direct2D geometry (M L H V C S Q A Z, absolute and relative) ----
class PathParser {
public:
    explicit PathParser(const char* s, bool filled = false) : p_(s), filled_(filled) {}

    bool Build(ID2D1GeometrySink* sink) {
        char cmd = 0;
        D2D1_POINT_2F cur{0, 0}, start{0, 0}, last_ctrl{0, 0};
        bool open = false, have_ctrl = false;
        auto close_open = [&](D2D1_FIGURE_END end) {
            if (open) sink->EndFigure(end);
            open = false;
        };
        for (;;) {
            SkipSeparators();
            if (!*p_) break;
            if (std::isalpha(static_cast<unsigned char>(*p_))) cmd = *p_++;
            else if (!cmd) return false;
            const bool rel = std::islower(static_cast<unsigned char>(cmd)) != 0;
            const float ox = rel ? cur.x : 0.0f, oy = rel ? cur.y : 0.0f;
            switch (std::toupper(static_cast<unsigned char>(cmd))) {
            case 'M': {
                float x, y;
                if (!Num(x) || !Num(y)) return false;
                close_open(D2D1_FIGURE_END_OPEN);
                cur = start = {ox + x, oy + y};
                sink->BeginFigure(cur, filled_ ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
                open = true;
                cmd = rel ? 'l' : 'L';  // further pairs are lines
                have_ctrl = false;
                break;
            }
            case 'L': {
                float x, y;
                if (!Num(x) || !Num(y)) return false;
                cur = {ox + x, oy + y};
                sink->AddLine(cur);
                have_ctrl = false;
                break;
            }
            case 'H': {
                float x;
                if (!Num(x)) return false;
                cur.x = ox + x;
                sink->AddLine(cur);
                have_ctrl = false;
                break;
            }
            case 'V': {
                float y;
                if (!Num(y)) return false;
                cur.y = oy + y;
                sink->AddLine(cur);
                have_ctrl = false;
                break;
            }
            case 'C': {
                float x1, y1, x2, y2, x, y;
                if (!Num(x1) || !Num(y1) || !Num(x2) || !Num(y2) || !Num(x) || !Num(y)) return false;
                const D2D1_POINT_2F c2{ox + x2, oy + y2};
                sink->AddBezier(D2D1::BezierSegment({ox + x1, oy + y1}, c2, {ox + x, oy + y}));
                last_ctrl = c2;
                have_ctrl = true;
                cur = {ox + x, oy + y};
                break;
            }
            case 'S': {
                float x2, y2, x, y;
                if (!Num(x2) || !Num(y2) || !Num(x) || !Num(y)) return false;
                const D2D1_POINT_2F c1 = have_ctrl ? D2D1_POINT_2F{2 * cur.x - last_ctrl.x, 2 * cur.y - last_ctrl.y} : cur;
                const D2D1_POINT_2F c2{ox + x2, oy + y2};
                sink->AddBezier(D2D1::BezierSegment(c1, c2, {ox + x, oy + y}));
                last_ctrl = c2;
                have_ctrl = true;
                cur = {ox + x, oy + y};
                break;
            }
            case 'Q': {
                float x1, y1, x, y;
                if (!Num(x1) || !Num(y1) || !Num(x) || !Num(y)) return false;
                sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment({ox + x1, oy + y1}, {ox + x, oy + y}));
                have_ctrl = false;
                cur = {ox + x, oy + y};
                break;
            }
            case 'A': {
                float rx, ry, rot, large, sweep, x, y;
                if (!Num(rx) || !Num(ry) || !Num(rot) || !Flag(large) || !Flag(sweep) || !Num(x) || !Num(y)) return false;
                const D2D1_POINT_2F end{ox + x, oy + y};
                if (end.x != cur.x || end.y != cur.y) {
                    sink->AddArc(D2D1::ArcSegment(end, D2D1::SizeF(rx, ry), rot,
                                                  sweep != 0.0f ? D2D1_SWEEP_DIRECTION_CLOCKWISE
                                                                : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                                  large != 0.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
                }
                cur = end;
                have_ctrl = false;
                break;
            }
            case 'Z':
                close_open(D2D1_FIGURE_END_CLOSED);
                cur = start;
                have_ctrl = false;
                cmd = 0;
                break;
            default:
                return false;
            }
        }
        close_open(D2D1_FIGURE_END_OPEN);
        return true;
    }

private:
    void SkipSeparators() {
        while (*p_ == ' ' || *p_ == ',' || *p_ == '\n' || *p_ == '\t') ++p_;
    }
    bool Num(float& out) {
        SkipSeparators();
        char* end = nullptr;
        out = std::strtof(p_, &end);
        if (end == p_) return false;
        p_ = end;
        return true;
    }
    bool Flag(float& out) {
        SkipSeparators();
        if (*p_ != '0' && *p_ != '1') return false;
        out = (*p_ == '1') ? 1.0f : 0.0f;
        ++p_;
        return true;
    }
    const char* p_;
    bool filled_ = false;
};

struct FactoryCache {
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1StrokeStyle> stroke;
    std::unordered_map<wchar_t, ComPtr<ID2D1PathGeometry>> geometries;
    std::unordered_map<const char*, ComPtr<ID2D1PathGeometry>> svg_hollow, svg_filled;
};

std::mutex g_cache_mutex;
std::vector<FactoryCache> g_caches;

FactoryCache* CacheFor(ID2D1Factory* factory) {
    for (auto& c : g_caches) {
        if (c.factory.Get() == factory) return &c;
    }
    FactoryCache c;
    c.factory = factory;
    const D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    factory->CreateStrokeStyle(props, nullptr, 0, &c.stroke);
    g_caches.push_back(std::move(c));
    return &g_caches.back();
}

ID2D1PathGeometry* GeometryFor(FactoryCache& cache, wchar_t glyph) {
    auto it = cache.geometries.find(glyph);
    if (it != cache.geometries.end()) return it->second.Get();
    const char* path = FindPath(glyph);
    if (!path) return nullptr;
    ComPtr<ID2D1PathGeometry> geo;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(cache.factory->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) return nullptr;
    PathParser parser(path);
    const bool ok = parser.Build(sink.Get());
    if (FAILED(sink->Close()) || !ok) return nullptr;
    cache.geometries.emplace(glyph, geo);
    return geo.Get();
}

ID2D1PathGeometry* SvgGeometryFor(FactoryCache& cache, const char* path, bool filled) {
    auto& map = filled ? cache.svg_filled : cache.svg_hollow;
    auto it = map.find(path);
    if (it != map.end()) return it->second.Get();
    ComPtr<ID2D1PathGeometry> geo;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(cache.factory->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) return nullptr;
    if (filled) sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    PathParser parser(path, filled);
    const bool ok = parser.Build(sink.Get());
    if (FAILED(sink->Close()) || !ok) return nullptr;
    map.emplace(path, geo);
    return geo.Get();
}

} // namespace

void DrawSvgPath(ID2D1DeviceContext* dc, const char* path, const D2D1_RECT_F& box, float view, ID2D1Brush* brush,
                 bool fill, float stroke_view) {
    if (!dc || !path || !brush || view <= 0.0f || box.right <= box.left) return;
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    if (!factory) return;
    ID2D1PathGeometry* geo = nullptr;
    ID2D1StrokeStyle* stroke = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        FactoryCache* cache = CacheFor(factory.Get());
        geo = SvgGeometryFor(*cache, path, fill);
        stroke = cache->stroke.Get();
    }
    if (!geo) return;
    const float k = std::min(box.right - box.left, box.bottom - box.top) / view;
    const float ox = (box.left + box.right) * 0.5f - view * k * 0.5f;
    const float oy = (box.top + box.bottom) * 0.5f - view * k * 0.5f;
    D2D1_MATRIX_3X2_F outer{};
    dc->GetTransform(&outer);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(ox, oy) * outer);
    if (fill) dc->FillGeometry(geo, brush);
    else dc->DrawGeometry(geo, brush, stroke_view, stroke);
    dc->SetTransform(outer);
}

bool DrawVectorGlyph(ID2D1DeviceContext* dc, wchar_t glyph, const D2D1_RECT_F& box, float size, ID2D1Brush* brush) {
    if (!dc || !brush || size <= 0.0f) return false;
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    if (!factory) return false;
    ID2D1PathGeometry* geo = nullptr;
    ID2D1StrokeStyle* stroke = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        FactoryCache* cache = CacheFor(factory.Get());
        geo = GeometryFor(*cache, glyph);
        stroke = cache->stroke.Get();
    }
    if (!geo) return false;
    const float cx = (box.left + box.right) * 0.5f, cy = (box.top + box.bottom) * 0.5f;
    const float k = size / 16.0f;
    D2D1_MATRIX_3X2_F outer{};
    dc->GetTransform(&outer);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(-8.0f, -8.0f) * D2D1::Matrix3x2F::Scale(k, k) *
                     D2D1::Matrix3x2F::Translation(cx, cy) * outer);
    dc->DrawGeometry(geo, brush, 1.3f, stroke);
    dc->SetTransform(outer);
    return true;
}

void DrawGlyph(ID2D1DeviceContext* dc, const wchar_t* text, UINT32 length, IDWriteTextFormat* format,
               const D2D1_RECT_F& rect, ID2D1Brush* brush, D2D1_DRAW_TEXT_OPTIONS options) {
    if (!dc || !text || length == 0) return;
    if (format && length >= 1) {
        // The icon fills the font's em box, placed where the format would have put the glyph.
        const float size = format->GetFontSize();
        float cx = (rect.left + rect.right) * 0.5f, cy = (rect.top + rect.bottom) * 0.5f;
        switch (format->GetTextAlignment()) {
        case DWRITE_TEXT_ALIGNMENT_LEADING: cx = rect.left + size * 0.5f; break;
        case DWRITE_TEXT_ALIGNMENT_TRAILING: cx = rect.right - size * 0.5f; break;
        default: break;
        }
        switch (format->GetParagraphAlignment()) {
        case DWRITE_PARAGRAPH_ALIGNMENT_NEAR: cy = rect.top + size * 0.6f; break;
        case DWRITE_PARAGRAPH_ALIGNMENT_FAR: cy = rect.bottom - size * 0.6f; break;
        default: break;
        }
        const D2D1_RECT_F box = D2D1::RectF(cx - size * 0.5f, cy - size * 0.5f, cx + size * 0.5f, cy + size * 0.5f);
        if (DrawVectorGlyph(dc, text[0], box, size, brush)) return;
    }
    dc->DrawText(text, length, format, rect, brush, options);
}

} // namespace pulse::desktop::render

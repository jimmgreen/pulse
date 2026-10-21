#pragma once

#include <d2d1_1.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <string_view>

namespace pulse::ui::command_icons {

// One optical grid for application commands. Shell file icons and artwork keep
// their own rendering paths. Geometry also works without the D2D SVG interface.
inline constexpr float kCanvas = 24.0f;
inline constexpr float kStroke = 1.75f;

enum class Icon {
    None, Add, Close, Minimize, Maximize, Restore, Back, Forward, Up,
    ChevronRight, ChevronLeft, ChevronDown, ChevronUp, Refresh, Search,
    Folder, FolderOpen, Desktop, Computer, Star, History, Download,
    File, Image, Drive, Network, Delete, Recycle, Tag, Settings, Sun,
    Cut, Copy, Paste, Rename, Sort, Filter, List, Grid, Split, Columns,
    Panel, PanelClose, Check, Eye, Info, Home, Tray, Pin, Link,
    Palette, Sliders, Warning, Lock, OpenExternal, StarFilled, More,
    SplitSingle, SplitStacked, SplitThree, SplitFour, Contrast, SwapHorizontal, SwapVertical,
    Music, Video, Count
};

inline Icon FromGlyph(std::wstring_view glyph) noexcept {
    if (glyph.size() != 1) return Icon::None;
    switch (glyph.front()) {
    case L'\xE710': return Icon::Add;
    case L'\xE712': return Icon::More;
    case L'\xE711': case L'\xE8BB': return Icon::Close;
    case L'\xE921': return Icon::Minimize;
    case L'\xE922': return Icon::Maximize;
    case L'\xE923': return Icon::Restore;
    case L'\xE72B': return Icon::Back;
    case L'\xE72A': return Icon::Forward;
    case L'\xE898': return Icon::Up;
    case L'\xE76C': return Icon::ChevronRight;
    case L'\xE76B': return Icon::ChevronLeft;
    case L'\xE70D': return Icon::ChevronDown;
    case L'\xE70E': return Icon::ChevronUp;
    case L'\xE72C': return Icon::Refresh;
    case L'\xE721': return Icon::Search;
    case L'\xE8B7': return Icon::Folder;
    case L'\xE838': return Icon::FolderOpen;
    case L'\xE7F4': return Icon::Desktop;
    case L'\xE977': return Icon::Computer;
    case L'\xE734': return Icon::Star;
    case L'\xE735': return Icon::StarFilled;
    case L'\xE823': case L'\xE787': return Icon::History;
    case L'\xE896': return Icon::Download;
    case L'\xE8A5': return Icon::File;
    case L'\xE8A7': return Icon::OpenExternal;
    case L'\xEB9F': case L'\xE91B': return Icon::Image;
    case L'\xE7F1': return Icon::Drive;
    case L'\xE968': return Icon::Network;
    case L'\xE74D': return Icon::Delete;
    case L'\xE75C': return Icon::Recycle;
    case L'\xE8EC': return Icon::Tag;
    case L'\xE713': return Icon::Settings;
    case L'\xE706': return Icon::Sun;
    case L'\xE793': return Icon::Contrast;
    case L'\xE8C6': return Icon::Cut;
    case L'\xE8C8': case L'\xE8EF': return Icon::Copy;
    case L'\xE77F': return Icon::Paste;
    case L'\xE8AC': return Icon::Rename;
    case L'\xE8CB': return Icon::Sort;
    case L'\xE71C': return Icon::Filter;
    case L'\xE700': case L'\xE8FD': return Icon::List;
    case L'\xE80A': return Icon::Grid;
    case L'\xE8A9': return Icon::Grid;
    case L'\xE8A0': return Icon::Panel;
    case L'\xE89F': return Icon::PanelClose;
    case L'\xE73E': return Icon::Check;
    case L'\xE890': return Icon::Eye;
    case L'\xE946': return Icon::Info;
    case L'\xE80F': return Icon::Home;
    case L'\xE8A1': return Icon::Tray;
    case L'\xE718': case L'\xE840': case L'\xE841': return Icon::Pin;
    case L'\xE71B': return Icon::Link;
    case L'\xE790': return Icon::Palette;
    case L'\xE9E9': case L'\xE8A4': return Icon::Sliders;
    case L'\xE7BA': return Icon::Warning;
    case L'\xE72E': return Icon::Lock;
    case L'\xE8D6': case L'\xEC4F': return Icon::Music;
    case L'\xE714': case L'\xE8B2': return Icon::Video;
    default: return Icon::None;
    }
}

inline HRESULT CreateStrokeStyle(ID2D1RenderTarget* target,
                                  ID2D1StrokeStyle** output) noexcept {
    if (!target || !output) return E_POINTER;
    ID2D1Factory* factory = nullptr;
    target->GetFactory(&factory);
    if (!factory) return E_FAIL;
    auto properties = D2D1::StrokeStyleProperties();
    properties.startCap = properties.endCap = properties.dashCap = D2D1_CAP_STYLE_ROUND;
    properties.lineJoin = D2D1_LINE_JOIN_ROUND;
    const HRESULT result = factory->CreateStrokeStyle(properties, nullptr, 0, output);
    factory->Release();
    return result;
}

inline D2D1_RECT_F CenteredBounds(D2D1_RECT_F bounds, float preferred_size) noexcept {
    const float side = (std::max)(0.0f, (std::min)({preferred_size,
        bounds.right - bounds.left, bounds.bottom - bounds.top}));
    const float cx = (bounds.left + bounds.right) * 0.5f;
    const float cy = (bounds.top + bounds.bottom) * 0.5f;
    return D2D1::RectF(cx - side * 0.5f, cy - side * 0.5f,
                      cx + side * 0.5f, cy + side * 0.5f);
}

inline ID2D1PathGeometry* BuildGeometry(ID2D1Factory* factory, Icon icon) noexcept {
    ID2D1PathGeometry* geometry = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(factory->CreatePathGeometry(&geometry))) return nullptr;
    if (FAILED(geometry->Open(&sink))) {
        geometry->Release();
        return nullptr;
    }
    const auto begin = [&](float x, float y) {
        sink->BeginFigure({x,y}, (icon == Icon::StarFilled || icon == Icon::More) ?
            D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    };
    const auto to = [&](float x, float y) { sink->AddLine({x,y}); };
    const auto curve = [&](float x, float y, float a, float b, float c, float d) {
        sink->AddBezier(D2D1::BezierSegment({x,y},{a,b},{c,d}));
    };
    const auto end = [&](bool closed = false) {
        sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    };
    const auto line = [&](float x, float y, float a, float b) {
        begin(x,y); to(a,b); end();
    };
    const auto path = [&](std::initializer_list<D2D1_POINT_2F> points) {
        if (points.size() < 2) return;
        const auto first = *points.begin(), last = *(points.end()-1);
        const bool closed = first.x == last.x && first.y == last.y;
        begin(first.x,first.y);
        sink->AddLines(points.begin()+1,static_cast<UINT32>(points.size()-1-(closed ? 1 : 0)));
        end(closed);
    };
    const auto box = [&](float x, float y, float a, float b, float r = 2.0f) {
        const float c = r * 0.55228475f;
        begin(x+r,y); to(a-r,y); curve(a-r+c,y,a,y+r-c,a,y+r);
        to(a,b-r); curve(a,b-r+c,a-r+c,b,a-r,b);
        to(x+r,b); curve(x+r-c,b,x,b-r+c,x,b-r);
        to(x,y+r); curve(x,y+r-c,x+r-c,y,x+r,y); end(true);
    };
    const auto circle = [&](float x, float y, float r) {
        begin(x+r,y);
        sink->AddArc(D2D1::ArcSegment({x-r,y},{r,r},0,D2D1_SWEEP_DIRECTION_CLOCKWISE,D2D1_ARC_SIZE_SMALL));
        sink->AddArc(D2D1::ArcSegment({x+r,y},{r,r},0,D2D1_SWEEP_DIRECTION_CLOCKWISE,D2D1_ARC_SIZE_SMALL));
        end(true);
    };
    const auto dot = [&](float x, float y, float r = 0.85f) {
        // A short round-capped stroke stays solid in the shared outline pass.
        line(x,y,x+(std::max)(0.01f,2*r-kStroke),y);
    };
    const auto arc = [&](float cx, float cy, float radius, float from, float finish) {
        auto point = [&](float a) {
            const float angle = a * 0.01745329252f;
            return D2D1::Point2F(cx + radius * std::cos(angle),
                                 cy + radius * std::sin(angle));
        };
        const auto first = point(from);
        begin(first.x,first.y);
        sink->AddArc(D2D1::ArcSegment(point(finish),{radius,radius},0,
            D2D1_SWEEP_DIRECTION_CLOCKWISE,finish-from > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        end();
    };
    const auto folder = [&]() {
        begin(3,8); to(3,6); curve(3,4.9f,3.9f,4,5,4);
        to(9,4); curve(9.6f,4,9.9f,4.3f,10.3f,4.7f); to(12,6.5f);
        to(19,6.5f); curve(20.1f,6.5f,21,7.4f,21,8.5f);
        to(21,18); curve(21,19.1f,20.1f,20,19,20);
        to(5,20); curve(3.9f,20,3,19.1f,3,18); end(true);
        line(3,9,21,9);
    };
    const auto document = [&]() {
        begin(13,3.5f); to(6.5f,3.5f); curve(5.4f,3.5f,4.5f,4.4f,4.5f,5.5f);
        to(4.5f,18.5f); curve(4.5f,19.6f,5.4f,20.5f,6.5f,20.5f);
        to(17.5f,20.5f); curve(18.6f,20.5f,19.5f,19.6f,19.5f,18.5f);
        to(19.5f,10); end(true);
        begin(13,3.5f); to(13,8.5f); curve(13,9.3f,13.7f,10,14.5f,10); to(19.5f,10); end();
    };
    switch (icon) {
    case Icon::More: circle(5.5f,12,1.6f); circle(12,12,1.6f); circle(18.5f,12,1.6f); break;
    case Icon::Add: line(12,5,12,19); line(5,12,19,12); break;
    case Icon::Close: line(6,6,18,18); line(18,6,6,18); break;
    case Icon::Minimize: line(5,12,19,12); break;
    case Icon::Maximize: box(5,5,19,19,1.5f); break;
    case Icon::Restore: box(3,8,16,21,1.5f); path({{8,5},{8,3},{21,3},{21,16},{18,16}}); break;
    case Icon::Back: line(5,12,19,12); path({{10.5f,6.5f},{5,12},{10.5f,17.5f}}); break;
    case Icon::Forward: line(5,12,19,12); path({{13.5f,6.5f},{19,12},{13.5f,17.5f}}); break;
    case Icon::Up: line(12,5,12,19); path({{6.5f,10.5f},{12,5},{17.5f,10.5f}}); break;
    case Icon::ChevronRight: path({{9,6},{15,12},{9,18}}); break;
    case Icon::ChevronLeft: path({{15,6},{9,12},{15,18}}); break;
    case Icon::ChevronDown: path({{6,9},{12,15},{18,9}}); break;
    case Icon::ChevronUp: path({{6,15},{12,9},{18,15}}); break;
    case Icon::Refresh:
        begin(4.5f,8.5f); curve(5.8f,5.7f,8.3f,4,11.5f,4);
        curve(15.3f,4,18,6,20,9); end();
        path({{20,4.5f},{20,9},{15.5f,9}});
        begin(19.5f,15.5f); curve(18.2f,18.3f,15.7f,20,12.5f,20);
        curve(8.7f,20,6,18,4,15); end();
        path({{4,19.5f},{4,15},{8.5f,15}}); break;
    case Icon::Search: circle(10.5f,10.5f,6.5f); line(16,16,21,21); break;
    case Icon::Folder: folder(); break;
    case Icon::FolderOpen:
        begin(3,17); to(3,6); curve(3,4.9f,3.9f,4,5,4);
        to(9,4); to(11.5f,6.5f); to(18,6.5f); curve(19.1f,6.5f,20,7.4f,20,8.5f); end();
        begin(3,18); to(5.5f,10); to(22,10); to(19.5f,18.5f);
        curve(19.2f,19.5f,18.6f,20,17.5f,20); to(5,20); curve(3.5f,20,2.6f,19.3f,3,18); end(true); break;
    case Icon::Desktop:
        box(3,4,21,17); line(12,17,12,21); line(8,21,16,21); break;
    case Icon::Computer:
        box(2.5f,5,15.5f,16); line(9,16,9,20); line(5,20,13,20);
        box(18,3,22,21,1.25f); dot(20,17.5f,0.6f); break;
    case Icon::Star: case Icon::StarFilled:
        path({{12,3},{14.75f,8.56f},{20.9f,9.46f},{16.45f,13.8f},{17.5f,19.9f},
              {12,17.03f},{6.5f,19.9f},{7.55f,13.8f},{3.1f,9.46f},{9.25f,8.56f},{12,3}}); break;
    case Icon::History: circle(12,12,8.5f); path({{12,7},{12,12},{15,14}}); break;
    case Icon::Download:
        line(12,3,12,14); path({{8,10},{12,14},{16,10}});
        begin(4,15); to(4,18.5f); curve(4,19.6f,4.9f,20.5f,6,20.5f);
        to(18,20.5f); curve(19.1f,20.5f,20,19.6f,20,18.5f); to(20,15); end(); break;
    case Icon::File: document(); line(8,14,16,14); line(8,17,13,17); break;
    case Icon::OpenExternal:
        begin(10,4.5f); to(5.5f,4.5f); curve(4.4f,4.5f,3.5f,5.4f,3.5f,6.5f);
        to(3.5f,18.5f); curve(3.5f,19.6f,4.4f,20.5f,5.5f,20.5f);
        to(17.5f,20.5f); curve(18.6f,20.5f,19.5f,19.6f,19.5f,18.5f); to(19.5f,14); end();
        path({{14,3.5f},{20.5f,3.5f},{20.5f,10}}); line(20.5f,3.5f,10,14); break;
    case Icon::Image:
        box(3,3,21,21,3); circle(8.2f,8.2f,1.7f);
        path({{4,17},{9,12},{13,16},{16,13},{21,18}}); break;
    case Icon::Drive: box(3,6,21,19,3); line(3,14,21,14); dot(7,16.5f); dot(11,16.5f); break;
    case Icon::Network:
        box(8,3,16,9,1.5f); box(2,16,10,21,1.3f); box(14,16,22,21,1.3f);
        line(12,9,12,13); path({{6,16},{6,13},{18,13},{18,16}}); break;
    case Icon::Delete: case Icon::Recycle:
        line(4,6.5f,20,6.5f);
        begin(9,6.5f); to(9,4.5f); curve(9,3.7f,9.7f,3,10.5f,3);
        to(13.5f,3); curve(14.3f,3,15,3.7f,15,4.5f); to(15,6.5f); end();
        begin(5.5f,6.5f); to(6.3f,18.6f); curve(6.4f,19.7f,7.1f,20.5f,8.2f,20.5f);
        to(15.8f,20.5f); curve(16.9f,20.5f,17.6f,19.7f,17.7f,18.6f); to(18.5f,6.5f); end();
        line(10,10,10.3f,17); line(14,10,13.7f,17); break;
    case Icon::Tag:
        path({{3,4},{3,11},{12,21},{21,12},{11,3},{4,3},{3,4}}); circle(7.5f,7.5f,1.2f); break;
    case Icon::Settings:
        path({{9.5f,3},{8.9f,5.3f},{6.9f,6.2f},{4.8f,5.5f},{2.8f,9},{4.4f,10.6f},
              {4.4f,13.4f},{2.8f,15},{4.8f,18.5f},{6.9f,17.8f},{8.9f,18.7f},{9.5f,21},
              {14.5f,21},{15.1f,18.7f},{17.1f,17.8f},{19.2f,18.5f},{21.2f,15},
              {19.6f,13.4f},{19.6f,10.6f},{21.2f,9},{19.2f,5.5f},{17.1f,6.2f},
              {15.1f,5.3f},{14.5f,3},{9.5f,3}}); circle(12,12,3); break;
    case Icon::Sun:
        circle(12,12,4); line(12,2,12,4); line(12,20,12,22); line(2,12,4,12); line(20,12,22,12);
        line(5,5,6.5f,6.5f); line(17.5f,17.5f,19,19); line(5,19,6.5f,17.5f); line(17.5f,6.5f,19,5); break;
    case Icon::Contrast:
        // Theme: an outlined disc with its right half solid. The solid half
        // keeps it apart from the outlined gear beside it at 16 px.
        circle(12,12,8.5f);
        sink->BeginFigure({12,3.5f}, D2D1_FIGURE_BEGIN_FILLED);
        sink->AddArc(D2D1::ArcSegment({20.5f,12},{8.5f,8.5f},0,D2D1_SWEEP_DIRECTION_CLOCKWISE,D2D1_ARC_SIZE_SMALL));
        sink->AddArc(D2D1::ArcSegment({12,20.5f},{8.5f,8.5f},0,D2D1_SWEEP_DIRECTION_CLOCKWISE,D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        break;
    case Icon::Cut:
        circle(6.5f,17.5f,3); circle(17.5f,17.5f,3);
        line(8.6f,15.4f,19,3.5f); line(5,3.5f,11,10.4f);
        line(14,13.8f,15.4f,15.4f); break;
    case Icon::Copy:
        box(8,8,20.5f,20.5f,2.25f);
        begin(15.5f,5.5f); curve(15.5f,4.4f,14.6f,3.5f,13.5f,3.5f);
        to(5.5f,3.5f); curve(4.4f,3.5f,3.5f,4.4f,3.5f,5.5f);
        to(3.5f,13.5f); curve(3.5f,14.6f,4.4f,15.5f,5.5f,15.5f); end(); break;
    case Icon::Paste:
        box(8.5f,3,15.5f,7,1.5f);
        begin(8.5f,5); to(6.5f,5); curve(5.4f,5,4.5f,5.9f,4.5f,7);
        to(4.5f,18.5f); curve(4.5f,19.6f,5.4f,20.5f,6.5f,20.5f);
        to(17.5f,20.5f); curve(18.6f,20.5f,19.5f,19.6f,19.5f,18.5f);
        to(19.5f,7); curve(19.5f,5.9f,18.6f,5,17.5f,5); to(15.5f,5); end();
        line(8.5f,11.5f,15.5f,11.5f); line(8.5f,15.5f,13.5f,15.5f); break;
    case Icon::Rename:
        begin(4,15); to(15.2f,3.8f); curve(16,3,17.2f,3,18,3.8f);
        to(20.2f,6); curve(21,6.8f,21,8,20.2f,8.8f);
        to(9,20); to(3,21); end(true);
        line(13.5f,5.5f,18.5f,10.5f); line(4,15,9,20); line(13,20.5f,21,20.5f); break;
    case Icon::Sort: line(5,4,5,20); path({{2,17},{5,20},{8,17}}); line(11,5,21,5); line(11,11,18,11); line(11,17,15,17); break;
    case Icon::Filter: path({{4,4.5f},{20,4.5f},{14,12},{14,18.5f},{10,20.5f},{10,12},{4,4.5f}}); break;
    case Icon::List: line(9,5,21,5); line(9,12,21,12); line(9,19,21,19); dot(4,5); dot(4,12); dot(4,19); break;
    case Icon::Grid: box(3,3,10,10,1.5f); box(14,3,21,10,1.5f); box(3,14,10,21,1.5f); box(14,14,21,21,1.5f); break;
    case Icon::Split: box(3,4,21,20); line(12,4,12,20); break;
    case Icon::SplitSingle: box(3,4,21,20); break;
    case Icon::SplitStacked: box(3,4,21,20); line(3,12,21,12); break;
    case Icon::SplitThree: box(3,4,21,20); line(12,4,12,20); line(12,12,21,12); break;
    case Icon::SplitFour: box(3,4,21,20); line(12,4,12,20); line(3,12,21,12); break;
    case Icon::Columns: box(3,4,21,20); line(9,4,9,20); line(15,4,15,20); break;
    case Icon::SwapHorizontal:
        line(3,8,20,8); path({{16,4},{20,8},{16,12}});
        line(21,16,4,16); path({{8,12},{4,16},{8,20}}); break;
    case Icon::SwapVertical:
        line(8,20,8,3); path({{4,7},{8,3},{12,7}});
        line(16,4,16,21); path({{12,17},{16,21},{20,17}}); break;
    case Icon::Panel: case Icon::PanelClose:
        box(3,4,21,20); line(15,4,15,20);
        if (icon == Icon::PanelClose) path({{7,9},{10,12},{7,15}});
        else path({{10,9},{7,12},{10,15}});
        break;
    case Icon::Check: path({{5,12},{10,17},{20,7}}); break;
    case Icon::Eye:
        begin(2.5f,12); curve(5,8,8,6,12,6); curve(16,6,19,8,21.5f,12);
        curve(19,16,16,18,12,18); curve(8,18,5,16,2.5f,12); end(true);
        circle(12,12,2.75f); break;
    case Icon::Info: circle(12,12,9); dot(12,7.5f,1); line(12,11,12,17); break;
    case Icon::Home: path({{3,10},{12,3},{21,10}}); path({{5,9},{5,21},{10,21},{10,14},{14,14},{14,21},{19,21},{19,9}}); break;
    case Icon::Tray: path({{3,14},{6,5},{18,5},{21,14},{21,20},{3,20},{3,14},{8,14},{10,17},{14,17},{16,14},{21,14}}); break;
    case Icon::Pin: path({{8,3},{16,3},{15,9},{19,13},{19,15},{5,15},{5,13},{9,9},{8,3}}); line(12,15,12,22); break;
    case Icon::Link:
        begin(9.5f,14.5f); curve(7.7f,12.7f,7.7f,10,9.5f,8.2f); to(13,4.7f);
        curve(14.8f,2.9f,17.7f,2.9f,19.5f,4.7f); curve(21.3f,6.5f,21.3f,9.2f,19.5f,11);
        to(17.5f,13); end();
        begin(14.5f,9.5f); curve(16.3f,11.3f,16.3f,14,14.5f,15.8f); to(11,19.3f);
        curve(9.2f,21.1f,6.3f,21.1f,4.5f,19.3f); curve(2.7f,17.5f,2.7f,14.8f,4.5f,13);
        to(6.5f,11); end(); break;
    case Icon::Palette:
        begin(12,3); curve(6.8f,3,3,6.8f,3,12); curve(3,17,6.8f,21,11.5f,21);
        curve(13.2f,21,14,20,13.3f,18.5f); curve(12.5f,16.9f,13.5f,15.5f,15.2f,15.5f);
        to(17,15.5f); curve(19.5f,15.5f,21,14,21,11.5f); curve(21,6.7f,17,3,12,3); end(true);
        circle(7.5f,9,0.8f); circle(12,6.8f,0.8f); circle(16.5f,9,0.8f); circle(6.8f,14,0.8f); break;
    case Icon::Sliders:
        line(3,6,8,6); line(13,6,21,6); circle(10.5f,6,2.5f);
        line(3,18,14,18); line(19,18,21,18); circle(16.5f,18,2.5f); break;
    case Icon::Warning: path({{12,3},{22,21},{2,21},{12,3}}); line(12,9,12,14); dot(12,17.5f); break;
    case Icon::Lock: box(4,10,20,21); arc(12,8,5,180,360); line(7,8,7,10); line(17,8,17,10); line(12,14,12,17); break;
    case Icon::Music:
        circle(7.5f,16.5f,2.5f); circle(16.5f,13.5f,2.5f);
        line(10,16.5f,10,5); line(19,13.5f,19,5);
        line(10,5,19,5); line(10,7.5f,19,7.5f); break;
    case Icon::Video:
        box(3,5,16,19,2); path({{16,9.5f},{21,6.5f},{21,17.5f},{16,14.5f}}); break;
    default: break;
    }
    const HRESULT result = sink->Close();
    sink->Release();
    if (FAILED(result)) { geometry->Release(); return nullptr; }
    return geometry;
}

inline ID2D1PathGeometry* GeometryFor(ID2D1RenderTarget* target, Icon icon) noexcept {
    // Cache complete contours, so translucent strokes are composited once and
    // curves are never tessellated into overlapping, round-capped short lines.
    struct Cache {
        ID2D1Factory* factory = nullptr;
        std::array<ID2D1PathGeometry*,static_cast<size_t>(Icon::Count)> geometries{};
        void Reset() noexcept {
            for (auto*& geometry : geometries) {
                if (geometry) geometry->Release();
                geometry = nullptr;
            }
            if (factory) factory->Release();
            factory = nullptr;
        }
        ~Cache() { Reset(); }
    };
    static thread_local Cache cache;
    ID2D1Factory* factory = nullptr;
    target->GetFactory(&factory);
    if (!factory) return nullptr;
    if (cache.factory != factory) {
        cache.Reset();
        cache.factory = factory;
    } else {
        factory->Release();
    }
    auto*& geometry = cache.geometries[static_cast<size_t>(icon)];
    if (!geometry) geometry = BuildGeometry(cache.factory,icon);
    return geometry;
}

inline bool Draw(ID2D1RenderTarget* target, ID2D1Brush* brush,
                  ID2D1StrokeStyle* stroke, Icon icon, D2D1_RECT_F bounds,
                  float opacity = 1.0f) noexcept {
    if (!target || !brush || icon <= Icon::None || icon >= Icon::Count) return false;
    const float side = (std::min)(bounds.right - bounds.left, bounds.bottom - bounds.top);
    if (!std::isfinite(side) || side <= 0.0f || !std::isfinite(opacity) ||
        !std::isfinite(bounds.left) || !std::isfinite(bounds.top) ||
        !std::isfinite(bounds.right) || !std::isfinite(bounds.bottom)) return true;
    auto* geometry = GeometryFor(target,icon);
    if (!geometry) return false;
    D2D1_MATRIX_3X2_F previous{};
    target->GetTransform(&previous);
    const float previous_opacity = brush->GetOpacity();
    brush->SetOpacity(previous_opacity * std::clamp(opacity,0.0f,1.0f));
    target->SetTransform(D2D1::Matrix3x2F::Scale(side/kCanvas,side/kCanvas) *
        D2D1::Matrix3x2F::Translation((bounds.left+bounds.right-side)*0.5f,
                                     (bounds.top+bounds.bottom-side)*0.5f) * previous);
    if (icon == Icon::StarFilled || icon == Icon::More || icon == Icon::Contrast)
        target->FillGeometry(geometry,brush);
    if (icon != Icon::More) target->DrawGeometry(geometry,brush,kStroke,stroke);
    target->SetTransform(previous);
    brush->SetOpacity(previous_opacity);
    return true;
}

} // namespace pulse::ui::command_icons

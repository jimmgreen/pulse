#pragma once

#include <d2d1_1.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <string_view>
#include <system_error>

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
    // One shape per command (pulse_toolbar_redesign §7): the commands that had
    // to share a Segoe glyph, and the ones only the font used to draw.
    CopyPath, BatchRename, SelectWild, Compare, ApplyAll, AdvSearch, CopyToPane, MoveToPane,
    FolderGo, FolderAdd, FolderGroup, FolderDissolve, FolderSize, Workspace, Toolbar, Sidebar,
    Undo, NewMarkdown, Zip, SelectAll, Invert, EyeHidden, Shield, Compact, Zebra, SizeBar,
    SmartDate, Outline, Terminal, SearchAll, MapDrive, Keyboard, PinBadge,
    // "Pin to X" = X's shape with a pin badge in the lower right.
    PinFavorites, PinQuickAccess, PinWorkspace, PinNetwork, PinToolbar,
    // 分组 (Segoe F168) and a program without an icon (Segoe ECAA).
    GroupBy, App, FolderList,
    // The other Segoe glyphs Pulse shows (menus, settings, dialogs), drawn on
    // the same grid and stroke so no icon comes from the font.
    Reset, TextName, Package, Unpin, Stack, Video, Cloud, Error, Merge, Pause, Play,
    Send, VBar, Move, Power, Bulb, Pointer, Import, Export, Bolt, Camera, FavBar,
    FullScreen, Save, Unlock, Eject, Skip, Disconnect, Music, Wrench, Timer, Help, Puzzle,
    // The Pulse assistant (command palette hand-off), filled in the accent.
    Sparkle,
    // Settings rows, assistant setup, cloud states and actions that still fell
    // back to the font: Segoe Wifi, Microphone, Checkbox, Highlight, Permissions,
    // Bold, MapLayers, Globe, Flag, CellPhone, Completed, CompletedSolid, Blocked
    // and the down arrow (moving a list item down).
    Wifi, Mic, Checkbox, Highlight, Key, Bold, Layers, Globe, Flag, Phone,
    CheckCircle, CheckCircleFilled, Blocked, Down,
    Dialog, User, Exit, PaneExpand, PaneRestore,
    // Added with the two-tone set: New file (document + plus badge) and Redo.
    FileAdd, Redo,
    Count
};

// Private-use glyph ids for the shapes above that no Segoe glyph stands for
// (same scheme as ui::kToolbarSplitGlyph); FromGlyph maps them.
inline constexpr const wchar_t* kGlyphCopyPath = L"\xF5B2";
inline constexpr const wchar_t* kGlyphBatchRename = L"\xF5B3";
inline constexpr const wchar_t* kGlyphSelectWild = L"\xF5B4";
inline constexpr const wchar_t* kGlyphCompare = L"\xF5B5";
inline constexpr const wchar_t* kGlyphApplyAll = L"\xF5B6";
inline constexpr const wchar_t* kGlyphAdvSearch = L"\xF5B7";
inline constexpr const wchar_t* kGlyphCopyToPane = L"\xF5B8";
inline constexpr const wchar_t* kGlyphMoveToPane = L"\xF5B9";
inline constexpr const wchar_t* kGlyphOpenPath = L"\xF5BA";
inline constexpr const wchar_t* kGlyphGroupFolder = L"\xF5BB";
inline constexpr const wchar_t* kGlyphPinFavorites = L"\xF5BC";
inline constexpr const wchar_t* kGlyphPinQuickAccess = L"\xF5BD";
inline constexpr const wchar_t* kGlyphPinWorkspace = L"\xF5BE";
inline constexpr const wchar_t* kGlyphPinNetwork = L"\xF5BF";
inline constexpr const wchar_t* kGlyphPinToolbar = L"\xF5C0";
inline constexpr const wchar_t* kGlyphFolderList = L"\xF5C1";   // a 收藏栏 group
inline constexpr const wchar_t* kGlyphSparkle = L"\xF5C2";
inline constexpr const wchar_t* kGlyphTopBar = L"\xF5C3";   // a window with a top bar (收藏栏)
inline constexpr const wchar_t* kGlyphFileAdd = L"\xF5C4";  // 新建文件

inline constexpr bool IsPinned(Icon icon) noexcept {
    return icon >= Icon::PinFavorites && icon <= Icon::PinToolbar;
}
inline constexpr Icon PinnedBase(Icon icon) noexcept {
    switch (icon) {
    case Icon::PinFavorites: return Icon::Star;
    case Icon::PinQuickAccess: return Icon::Sidebar;
    case Icon::PinWorkspace: return Icon::Workspace;
    case Icon::PinNetwork: return Icon::Network;
    case Icon::PinToolbar: return Icon::Toolbar;
    default: return Icon::None;
    }
}
// Fill opacity of an icon's filled figures (0 = outline only).
inline constexpr float FillOpacity(Icon icon) noexcept {
    switch (icon) {
    case Icon::StarFilled: case Icon::More: case Icon::Contrast: case Icon::SizeBar:
    case Icon::Pause: case Icon::Play: case Icon::Sparkle: return 1.0f;
    case Icon::Invert: case Icon::CheckCircleFilled: return 0.35f;
    case Icon::Zebra: return 0.28f;
    default: return 0.0f;
    }
}

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
    case L'\xF101': return Icon::Dialog;
    case L'\xF102': return Icon::User;
    case L'\xF103': return Icon::Exit;
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
    case L'\xF5B0': return Icon::Split;     // ui::kToolbarSplitGlyph
    case L'\xF5B1': return Icon::Columns;   // ui::kToolbarColumnsGlyph
    case L'\xF5B2': return Icon::CopyPath;
    case L'\xF5B3': return Icon::BatchRename;
    case L'\xF5B4': return Icon::SelectWild;
    case L'\xF5B5': return Icon::Compare;
    case L'\xF5B6': return Icon::ApplyAll;
    case L'\xF5B7': return Icon::AdvSearch;
    case L'\xF5B8': return Icon::CopyToPane;
    case L'\xF5B9': return Icon::MoveToPane;
    case L'\xF5BA': return Icon::FolderGo;
    case L'\xF5BB': return Icon::FolderGroup;
    case L'\xF5BC': return Icon::PinFavorites;
    case L'\xF5BD': return Icon::PinQuickAccess;
    case L'\xF5BE': return Icon::PinWorkspace;
    case L'\xF5BF': return Icon::PinNetwork;
    case L'\xF5C0': return Icon::PinToolbar;
    case L'\xF5C1': return Icon::FolderList;
    case L'\xF5C2': return Icon::Sparkle;
    case L'\xF5C3': return Icon::Toolbar;
    case L'\xF5C4': return Icon::FileAdd;
    case L'\xF168': return Icon::GroupBy;
    case L'\xE8F1': return Icon::FolderList;
    case L'\xE777': return Icon::Reset;
    case L'\xE8D2': case L'\xE8C1': return Icon::TextName;
    case L'\xE7B8': return Icon::Package;
    case L'\xE77A': return Icon::Unpin;
    case L'\xE7C4': return Icon::Stack;
    case L'\xE714': return Icon::Video;
    case L'\xE753': return Icon::Cloud;
    case L'\xE783': case L'\xEA39': return Icon::Error;
    case L'\xE8B8': return Icon::Merge;
    case L'\xF8AE': return Icon::Pause;
    case L'\xEDDA': return Icon::Play;
    case L'\xE724': return Icon::Send;
    case L'\xE76F': return Icon::VBar;
    case L'\xE7C2': return Icon::Move;
    case L'\xE7E8': return Icon::Power;
    case L'\xE82F': case L'\xEA80': return Icon::Bulb;
    case L'\xE8B0': case L'\xE962': return Icon::Pointer;
    case L'\xE8B5': return Icon::Import;
    case L'\xEDE1': return Icon::Export;
    case L'\xE945': return Icon::Bolt;
    case L'\xE722': return Icon::Camera;
    case L'\xE728': return Icon::FavBar;
    case L'\xE740': return Icon::FullScreen;
    case L'\xE74E': return Icon::Save;
    case L'\xE785': return Icon::Unlock;
    case L'\xF847': return Icon::Eject;
    case L'\xE893': return Icon::Skip;
    case L'\xE8CD': return Icon::Disconnect;
    case L'\xE8D6': return Icon::Music;
    case L'\xE90F': return Icon::Wrench;
    case L'\xE916': return Icon::Timer;
    case L'\xE9CE': return Icon::Help;
    case L'\xEA86': return Icon::Puzzle;
    // Same meaning as a shape above.
    case L'\xE8E5': case L'\xE8DA': return Icon::FolderOpen;
    case L'\xE7AC': return Icon::OpenExternal;
    case L'\xE70F': return Icon::Rename;
    case L'\xE71E': case L'\xE7C8': return Icon::Search;
    case L'\xE7C3': return Icon::File;
    case L'\xE895': return Icon::Refresh;
    case L'\xE8B9': return Icon::Image;
    case L'\xECA5': return Icon::Grid;
    case L'\xEA37': case L'\xE71D': return Icon::List;
    case L'\xE737': return Icon::Tray;
    case L'\xE738': return Icon::Minimize;
    case L'\xE7EF': return Icon::Shield;
    case L'\xE88E': return Icon::Drive;
    case L'\xE8EE': return Icon::SwapHorizontal;
    case L'\xEC50': return Icon::Folder;
    case L'\xED1A': return Icon::EyeHidden;
    case L'\xECAA': return Icon::App;
    // Segoe glyphs the toolbar and menus already use, now on the vector grid.
    case L'\xE7A7': return Icon::Undo;
    case L'\xE7A6': return Icon::Redo;
    case L'\xE7B3': return Icon::EyeHidden;
    case L'\xEA18': return Icon::Shield;
    case L'\xE73F': return Icon::Compact;
    case L'\xE8BF': return Icon::Zebra;
    case L'\xE9D2': return Icon::SizeBar;
    case L'\xEC92': return Icon::SmartDate;
    case L'\xE739': return Icon::Outline;
    case L'\xE90C': return Icon::Sidebar;
    case L'\xE8B3': return Icon::SelectAll;
    case L'\xE8AB': case L'\xE7A1': return Icon::Invert;
    case L'\xE943': return Icon::NewMarkdown;
    case L'\xE81C': return Icon::History;
    case L'\xE774': return Icon::SearchAll;
    case L'\xE8CE': return Icon::MapDrive;
    case L'\xE765': return Icon::Keyboard;
    case L'\xF012': return Icon::Zip;
    case L'\xEDA2': return Icon::FolderSize;
    case L'\xE756': return Icon::Terminal;
    case L'\xE8F4': return Icon::FolderAdd;
    case L'\xED25': return Icon::FolderDissolve;
    // Settings, assistant, cloud-state and action glyphs that had no shape yet.
    case L'\xE701': return Icon::Wifi;          // 局域网发现
    case L'\xE720': return Icon::Mic;           // 语音输入
    case L'\xE73A': return Icon::Checkbox;      // 项目复选框
    case L'\xE7E6': return Icon::Highlight;     // 名称高亮
    case L'\xE8D7': return Icon::Key;           // API Key
    case L'\xE8DD': return Icon::Bold;          // 修改时间加粗
    case L'\xE81E': return Icon::Layers;        // 主题、窗口效果
    case L'\xE775': return Icon::Globe;         // 显示语言
    case L'\xE7C1': return Icon::Flag;          // 设为目标栏
    case L'\xE8EA': return Icon::Phone;         // 手机
    case L'\xE930': return Icon::CheckCircle;   // 已在本机
    case L'\xEC61': return Icon::CheckCircleFilled;  // 始终保留在本机
    case L'\xE733': return Icon::Blocked;       // 同步被阻止
    case L'\xE74B': return Icon::Down;
    // ...and the ones whose meaning an existing shape already carries.
    case L'\xE74A': return Icon::Up;
    case L'\xE703': return Icon::Link;          // 连接
    case L'\xE771': case L'\xE9CA': case L'\xEB3C': return Icon::Palette;  // 通用、配色、自定义主题色
    case L'\xE7BC': return Icon::Zebra;
    case L'\xE815': return Icon::Pointer;       // 悬停选中
    case L'\xE897': return Icon::Delete;        // 删除前确认
    case L'\xE8AD': case L'\xE78B': return Icon::OpenExternal;  // 外部打开、新建窗口
    case L'\xE8DE': return Icon::Copy;          // 文件操作
    case L'\xE8E9': return Icon::TextName;      // 界面文字大小
    case L'\xE8F6': return Icon::EyeHidden;     // 暂存区为空时隐藏
    case L'\xE99A': return Icon::Sparkle;       // AI 与 MCP
    case L'\xE9D5': return Icon::History;       // 变更记录保留天数
    case L'\xE9F5': case L'\xED1E': return Icon::Tray;  // 关闭后继续运行、暂存区图标
    case L'\xEB42': return Icon::Image;         // 壁纸模糊
    case L'\xEC57': return Icon::Power;         // 开机自启
    case L'\xECCD': return Icon::Add;           // 新标签页打开
    case L'\xECE7': return Icon::Compact;       // 行高
    case L'\xE768': return Icon::Play;          // 运行右键命令
    case L'\xE8E6': return Icon::Close;         // 取消选择
    case L'\xE8FF': return Icon::Eye;           // 快速预览
    case L'\xE9D9': return Icon::SearchAll;     // 索引状态
    case L'\xEB90': return Icon::Error;         // 同步出错
    case L'\xE7F8': return Icon::Computer;      // 网络设备
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
        sink->BeginFigure({x,y}, (icon == Icon::StarFilled || icon == Icon::More || icon == Icon::SizeBar ||
                                  icon == Icon::Pause || icon == Icon::Play || icon == Icon::Sparkle) ?
            D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    };
    // A figure that is filled (at FillOpacity) as well as outlined.
    const auto fill_begin = [&](float x, float y) { sink->BeginFigure({x,y}, D2D1_FIGURE_BEGIN_FILLED); };
    const auto arcto = [&](float x, float y, float radius, bool large, bool clockwise) {
        sink->AddArc(D2D1::ArcSegment({x,y},{radius,radius},0,
            clockwise ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
            large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
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
    case Icon::PaneExpand:
        path({{4,10},{4,4},{10,4}}); line(4,4,10,10);
        path({{14,20},{20,20},{20,14}}); line(14,14,20,20); break;
    case Icon::PaneRestore:
        path({{4,10},{10,10},{10,4}}); line(4,4,10,10);
        path({{14,20},{14,14},{20,14}}); line(14,14,20,20); break;
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
    case Icon::CopyPath:   // 复制路径 keeps the chain link: two pages would read like 复制
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
    case Icon::BatchRename: begin(3.5f,5.5f); to(10.5f,5.5f); end(); begin(3.5f,9.5f); to(8.5f,9.5f); end();
        begin(3.5f,13.5f); to(6.5f,13.5f); end(); begin(9.5f,18.5f); to(17,11); to(19.5f,13.5f); to(12,21);
        to(9.5f,21); end(true); begin(15.2f,12.8f); to(17.2f,14.8f); end(); break;
    case Icon::SelectWild: path({{3.5f,8},{3.5f,3.5f},{8,3.5f}}); path({{16,3.5f},{20.5f,3.5f},{20.5f,8}});
        path({{20.5f,16},{20.5f,20.5f},{16,20.5f}}); path({{8,20.5f},{3.5f,20.5f},{3.5f,16}});
        line(10.8f,3.5f,13.2f,3.5f); line(10.8f,20.5f,13.2f,20.5f); line(3.5f,10.8f,3.5f,13.2f);
        line(20.5f,10.8f,20.5f,13.2f); line(12,8,12,16); line(8.6f,10,15.4f,14); line(15.4f,10,8.6f,14); break;
    case Icon::Compare: box(3.5f,5,11,19,1.6f); box(13,5,20.5f,19,1.6f); begin(6,9); to(8.5f,9); end();
        begin(6,12); to(8.5f,12); end(); begin(15.5f,9); to(18,9); end(); begin(15.5f,12); to(18,12); end();
        begin(15.5f,15); to(18,15); end(); break;
    case Icon::ApplyAll: box(4,4,10.5f,10.5f,1.4f); box(13.5f,4,20,10.5f,1.4f); box(4,13.5f,10.5f,20,1.4f);
        begin(13.6f,17.2f); to(15.7f,19.3f); to(20,14.7f); end(); break;
    case Icon::AdvSearch: circle(10,10,6); begin(14.4f,14.4f); to(16.5f,16.5f); end(); begin(14.5f,20.5f);
        to(21.5f,20.5f); end(); begin(18.5f,18.5f); to(18.5f,22.5f); end(); break;
    case Icon::CopyToPane: box(3.5f,4.5f,20.5f,19.5f,2); begin(12,4.5f); to(12,8); end(); begin(12,16);
        to(12,19.5f); end(); begin(7,12); to(16.5f,12); end(); begin(14,9.5f); to(16.5f,12); to(14,14.5f);
        end(); begin(6.5f,6.8f); to(6.5f,9.4f); end(); begin(5.2f,8.1f); to(7.8f,8.1f); end(); break;
    case Icon::MoveToPane: box(3.5f,4.5f,20.5f,19.5f,2); begin(12,4.5f); to(12,8); end(); begin(12,16);
        to(12,19.5f); end(); begin(7,12); to(16.5f,12); end(); begin(14,9.5f); to(16.5f,12); to(14,14.5f);
        end(); break;
    case Icon::FolderGo: folder(); begin(9,13.5f); to(15.5f,13.5f); end(); begin(13.2f,11.2f); to(15.5f,13.5f);
        to(13.2f,15.8f); end(); break;
    case Icon::FolderAdd: folder(); begin(12,11); to(12,16.2f); end(); begin(9.4f,13.6f); to(14.6f,13.6f);
        end(); break;
    case Icon::FolderGroup: folder(); line(12,11,12,16.5f); path({{9.6f,14.1f},{12,16.5f},{14.4f,14.1f}}); break;
    case Icon::FolderDissolve: begin(3.5f,9.5f); to(3.5f,7.5f); arcto(5.5f,5.5f,2,false,true); to(9.1f,5.5f);
        to(10.7f,7.3f); to(18.5f,7.3f); arcto(20.5f,9.3f,2,false,true); to(20.5f,10.3f); end();
        begin(20.5f,14.5f); to(20.5f,17.2f); arcto(18.5f,19.2f,2,false,true); to(17,19.2f); end();
        begin(7,19.2f); to(5.5f,19.2f); arcto(3.5f,17.2f,2,false,true); to(3.5f,14.5f); end(); begin(12,17.5f);
        to(12,11.5f); end(); begin(9.6f,13.6f); to(12,11.2f); to(14.4f,13.6f); end(); break;
    case Icon::FolderSize: folder(); begin(8.5f,16); to(8.5f,14.5f); end(); begin(12,16); to(12,12); end();
        begin(15.5f,16); to(15.5f,13.5f); end(); break;
    case Icon::Workspace: box(3.5f,4.5f,20.5f,19.5f,2); begin(3.5f,9); to(20.5f,9); end(); begin(9.5f,9);
        to(9.5f,19.5f); end(); break;
    case Icon::Toolbar: box(3.5f,5,20.5f,19,2); begin(3.5f,9.5f); to(20.5f,9.5f); end(); dot(6.4f,7.25f,0.9f);
        dot(9.2f,7.25f,0.9f); dot(12,7.25f,0.9f); break;
    case Icon::Sidebar: box(3.5f,4.5f,20.5f,19.5f,2); begin(9.5f,4.5f); to(9.5f,19.5f); end(); begin(5.8f,8.5f);
        to(7.4f,8.5f); end(); begin(5.8f,11.5f); to(7.4f,11.5f); end(); break;
    case Icon::Undo: begin(9,6.5f); to(4.5f,11); to(9,15.5f); end(); begin(4.5f,11); to(14.5f,11);
        arcto(14.5f,21,5,false,true); to(11,21); end(); break;
    case Icon::NewMarkdown: begin(6,3.5f); to(13.5f,3.5f); to(18.5f,8.5f); to(18.5f,19.5f);
        arcto(17.5f,20.5f,1,false,true); to(6,20.5f); arcto(5,19.5f,1,false,true); to(5,4.5f);
        arcto(6,3.5f,1,false,true); end(true); begin(13.5f,3.5f); to(13.5f,8.5f); to(18.5f,8.5f); end();
        begin(7.8f,17.5f); to(7.8f,13); to(9.7f,15.3f); to(11.6f,13); to(11.6f,17.5f); end(); begin(15,13);
        to(15,17.5f); end(); begin(13.6f,16.2f); to(15,17.6f); to(16.4f,16.2f); end(); break;
    case Icon::Zip: box(4.5f,3.5f,19.5f,20.5f,2); begin(12,3.5f); to(12,5.5f); end(); begin(12,7.5f);
        to(12,8.7f); end(); begin(12,10.5f); to(12,11.7f); end(); box(10.3f,13.6f,13.7f,17.2f,0.8f); break;
    case Icon::SelectAll: box(4,4,20,20,3); begin(8,12.4f); to(11,15.4f); to(16,9.4f); end(); break;
    case Icon::Invert: box(4,4,20,20,3); line(4.8f,19.2f,19.2f,4.8f); fill_begin(20,7); to(20,17);
        arcto(17,20,3,false,true); to(7,20); end(true); break;
    case Icon::EyeHidden: begin(2.5f,12); curve(2.5f,12,6,5.5f,12,5.5f); curve(18,5.5f,21.5f,12,21.5f,12);
        curve(21.5f,12,18,18.5f,12,18.5f); curve(6,18.5f,2.5f,12,2.5f,12); end(true); circle(12,12,3);
        line(4,4,20,20); break;
    case Icon::Shield: begin(12,3.2f); to(19.5f,6.2f); to(19.5f,12.1f); curve(19.5f,16.5f,16.3f,19.8f,12,21);
        curve(7.7f,19.8f,4.5f,16.5f,4.5f,12.1f); to(4.5f,6.2f); end(true); begin(8.8f,12.2f); to(11,14.4f);
        to(15.2f,10); end(); break;
    case Icon::Compact: begin(4,5.5f); to(20,5.5f); end(); begin(4,9.8f); to(20,9.8f); end(); begin(4,14.2f);
        to(20,14.2f); end(); begin(4,18.5f); to(20,18.5f); end(); break;
    case Icon::Zebra: box(3.5f,4,20.5f,20,2); line(3.5f,9.4f,20.5f,9.4f); line(3.5f,14.6f,20.5f,14.6f);
        fill_begin(3.5f,9.4f); to(20.5f,9.4f); to(20.5f,14.6f); to(3.5f,14.6f); end(true); break;
    case Icon::SizeBar: box(4,5.9f,20,7.1f,0.6f); box(4,11.4f,14,12.6f,0.6f); box(4,16.9f,10,18.1f,0.6f); break;
    case Icon::SmartDate: box(3.5f,5,20.5f,20,2); begin(3.5f,9.5f); to(20.5f,9.5f); end(); begin(8,3); to(8,7);
        end(); begin(16,3); to(16,7); end(); begin(12,12.2f); to(12.9f,14.1f); to(14.8f,15); to(12.9f,15.9f);
        to(12,17.8f); to(11.1f,15.9f); to(9.2f,15); to(11.1f,14.1f); end(true); break;
    case Icon::Outline: arc(6.5f,10,3,180,270); arc(17.5f,10,3,270,360); arc(17.5f,14,3,0,90);
        arc(6.5f,14,3,90,180); line(8.8f,7,10.6f,7); line(13.4f,7,15.2f,7); line(8.8f,17,10.6f,17);
        line(13.4f,17,15.2f,17); line(7.5f,12,16.5f,12); break;
    case Icon::Terminal: box(3,4.5f,21,19.5f,2); begin(7,9.5f); to(10,12); to(7,14.5f); end(); begin(12.5f,15);
        to(17,15); end(); break;
    case Icon::SearchAll: circle(10.5f,10.5f,6.5f); begin(4,10.5f); to(17,10.5f); end(); begin(10.5f,4);
        curve(12.2f,5.8f,13,8,13,10.5f); curve(13,13,12.2f,15.2f,10.5f,17); curve(8.8f,15.2f,8,13,8,10.5f);
        curve(8,8,8.8f,5.8f,10.5f,4); end(true); begin(15.3f,15.3f); to(20,20); end(); break;
    case Icon::MapDrive: box(3.5f,13,20.5f,20,2); dot(7.2f,16.5f,1.15f); begin(12,13); to(12,9.5f); end();
        begin(8.5f,3.5f); to(15.5f,3.5f); to(15.5f,9.5f); to(8.5f,9.5f); end(true); break;
    case Icon::Keyboard: box(2.5f,6,21.5f,18,2); dot(6.5f,10,0.95f); dot(9.5f,10,0.95f); dot(12.5f,10,0.95f);
        dot(15.5f,10,0.95f); dot(18,10,0.95f); begin(8,14.2f); to(16,14.2f); end(); break;
    case Icon::GroupBy: path({{6.5f,3.5f},{4,3.5f},{4,10.5f},{6.5f,10.5f}}); line(9.5f,5,20.5f,5);
        line(9.5f,9,16.5f,9); path({{6.5f,13.5f},{4,13.5f},{4,20.5f},{6.5f,20.5f}}); line(9.5f,15,20.5f,15);
        line(9.5f,19,16.5f,19); break;
    case Icon::App: box(4,4,20,20,4.5f); box(9,9,15,15,1.5f); break;
    case Icon::FolderList: folder(); line(8,13,16,13); line(8,16.5f,13,16.5f); break;
    case Icon::Reset: begin(3.5f,12); arcto(12,3.5f,8.5f,true,false); arcto(6,6,8.5f,false,false); to(3.5f,8.5f);
        end(); path({{3.5f,4},{3.5f,8.5f},{8,8.5f}}); break;
    case Icon::TextName: path({{5,7},{5,4.5f},{19,4.5f},{19,7}}); line(12,4.5f,12,19.5f); line(9,19.5f,15,19.5f); break;
    case Icon::Package: path({{12,3},{20.5f,7.5f},{20.5f,16.5f},{12,21},{3.5f,16.5f},{3.5f,7.5f},{12,3}});
        path({{3.5f,7.5f},{12,12},{20.5f,7.5f}}); line(12,12,12,21); line(7.8f,5.3f,16.2f,9.7f); break;
    case Icon::Unpin: path({{8,3},{16,3},{15,9},{19,13},{19,15},{5,15},{5,13},{9,9},{8,3}}); line(12,15,12,22);
        line(3.5f,3.5f,20.5f,20.5f); break;
    case Icon::Stack: box(3,9,21,20.5f,2); line(5.5f,6,18.5f,6); line(8,3,16,3); break;
    case Icon::Video: box(2.5f,6,15.5f,18,2.5f); path({{15.5f,10.5f},{21.5f,7},{21.5f,17},{15.5f,13.5f}}); break;
    case Icon::Cloud: begin(7,18.5f); to(17.5f,18.5f); curve(19.9f,18.5f,21.5f,16.6f,21.5f,14.5f);
        curve(21.5f,12.3f,19.8f,10.5f,17.6f,10.5f); curve(17,7.3f,14.6f,5,11.5f,5); curve(8.2f,5,5.6f,7.6f,5.5f,10.8f);
        curve(3.8f,11.4f,2.5f,13,2.5f,14.8f); curve(2.5f,16.9f,4.6f,18.5f,7,18.5f); end(true); break;
    case Icon::Error: circle(12,12,9); line(12,7.5f,12,13); dot(12,16.5f,1); break;
    case Icon::Merge: path({{6,3.5f},{6,8},{12,14},{12,20.5f}}); path({{18,3.5f},{18,8},{12,14}});
        path({{9,17.5f},{12,20.5f},{15,17.5f}}); break;
    case Icon::Pause: box(6.5f,5,10,19,1); box(14,5,17.5f,19,1); break;
    case Icon::Play: path({{7.5f,4.5f},{19,12},{7.5f,19.5f},{7.5f,4.5f}}); break;
    case Icon::Send: path({{21,3},{3,10.5f},{10.5f,13.5f},{13.5f,21},{21,3}}); line(21,3,10.5f,13.5f); break;
    case Icon::VBar: line(12,4,12,20); break;
    case Icon::Move: line(12,3,12,21); line(3,12,21,12); path({{9.5f,5.5f},{12,3},{14.5f,5.5f}});
        path({{9.5f,18.5f},{12,21},{14.5f,18.5f}}); path({{5.5f,9.5f},{3,12},{5.5f,14.5f}});
        path({{18.5f,9.5f},{21,12},{18.5f,14.5f}}); break;
    case Icon::Dialog:
        box(2.7f,3.75f,21.3f,20.25f,2.25f); line(2.7f,7.8f,21.3f,7.8f);
        line(6,16.5f,12.75f,16.5f); line(15.3f,16.5f,18,16.5f); break;
    case Icon::User:
        circle(12,8.25f,3.9f); begin(4.5f,20.25f);
        curve(4.5f,10.25f,19.5f,10.25f,19.5f,20.25f); end(); break;
    case Icon::Exit:
        line(3.75f,12,14.25f,12); path({{10.5f,7.5f},{15,12},{10.5f,16.5f}});
        begin(15.75f,4.5f); to(18.75f,4.5f); curve(19.6f,4.5f,20.25f,5.2f,20.25f,6);
        to(20.25f,18); curve(20.25f,18.8f,19.6f,19.5f,18.75f,19.5f); to(15.75f,19.5f); end(); break;
    case Icon::Power: line(12,3,12,11); arc(12,13,8,300,600); break;
    case Icon::Bulb: arc(12,9.5f,6,135,405); path({{7.76f,13.74f},{9.2f,16},{14.8f,16},{16.24f,13.74f}});
        line(9.5f,18.8f,14.5f,18.8f); line(10.5f,21,13.5f,21); break;
    case Icon::Pointer: path({{5,3},{5,18.5f},{9,14.8f},{11.8f,21},{14.6f,19.8f},{11.9f,13.7f},{17.5f,13.7f},{5,3}}); break;
    case Icon::Import: begin(13,3.5f); to(18.5f,3.5f); curve(19.6f,3.5f,20.5f,4.4f,20.5f,5.5f); to(20.5f,18.5f);
        curve(20.5f,19.6f,19.6f,20.5f,18.5f,20.5f); to(13,20.5f); end(); line(3.5f,12,14.5f,12);
        path({{11,8.5f},{14.5f,12},{11,15.5f}}); break;
    case Icon::Export: begin(11,3.5f); to(5.5f,3.5f); curve(4.4f,3.5f,3.5f,4.4f,3.5f,5.5f); to(3.5f,18.5f);
        curve(3.5f,19.6f,4.4f,20.5f,5.5f,20.5f); to(11,20.5f); end(); line(9.5f,12,20.5f,12);
        path({{17,8.5f},{20.5f,12},{17,15.5f}}); break;
    // The assistant mark of the AI panel (ai_panel_icons.cpp k_spark), same path.
    case Icon::Sparkle: begin(11,3.2f); curve(11.7f,8.1f,14.1f,10.5f,19,11.2f);
        curve(14.1f,11.9f,11.7f,14.3f,11,19.2f); curve(10.3f,14.3f,7.9f,11.9f,3,11.2f);
        curve(7.9f,10.5f,10.3f,8.1f,11,3.2f); end(true);
        begin(19,2.6f); curve(19.25f,4.3f,20.05f,5.1f,21.75f,5.35f);
        curve(20.05f,5.6f,19.25f,6.4f,19,8.1f); curve(18.75f,6.4f,17.95f,5.6f,16.25f,5.35f);
        curve(17.95f,5.1f,18.75f,4.3f,19,2.6f); end(true); break;
    case Icon::Bolt: path({{13,2.5f},{4.5f,13.5f},{12,13.5f},{11,21.5f},{19.5f,10.5f},{12,10.5f},{13,2.5f}}); break;
    case Icon::Camera: box(2.5f,7,21.5f,19.5f,2.5f); path({{8,7},{9.5f,4.5f},{14.5f,4.5f},{16,7}});
        circle(12,13,3.5f); break;
    case Icon::FavBar: path({{8.9f,3.97f},{10.5f,7.19f},{14.06f,7.72f},{11.48f,10.23f},{12.09f,13.77f},{8.9f,12.11f},
        {5.71f,13.77f},{6.32f,10.23f},{3.74f,7.72f},{7.31f,7.19f},{8.9f,3.97f}}); line(16.5f,7,20.5f,7);
        line(16.5f,11,20.5f,11); line(3.5f,17.5f,20.5f,17.5f); break;
    case Icon::FullScreen: path({{3.5f,8.5f},{3.5f,3.5f},{8.5f,3.5f}}); path({{15.5f,3.5f},{20.5f,3.5f},{20.5f,8.5f}});
        path({{20.5f,15.5f},{20.5f,20.5f},{15.5f,20.5f}}); path({{8.5f,20.5f},{3.5f,20.5f},{3.5f,15.5f}}); break;
    case Icon::Save: path({{3.5f,3.5f},{16.5f,3.5f},{20.5f,7.5f},{20.5f,20.5f},{3.5f,20.5f},{3.5f,3.5f}});
        path({{7.5f,3.5f},{7.5f,8},{15,8},{15,3.5f}}); path({{7,20.5f},{7,13.5f},{17,13.5f},{17,20.5f}}); break;
    case Icon::Unlock: box(4,10,20,21); begin(7,10); to(7,7); arcto(16.9f,6,5,false,true); end();
        line(12,14,12,17); break;
    case Icon::Eject: path({{12,4},{20,13},{4,13},{12,4}}); line(4,18.5f,20,18.5f); break;
    case Icon::Skip: path({{5,6},{11,12},{5,18}}); path({{12,6},{18,12},{12,18}}); break;
    case Icon::Disconnect: box(3.5f,13,20.5f,20,2); dot(7.2f,16.5f,1.15f); line(9,3.5f,15,9.5f); line(15,3.5f,9,9.5f); break;
    case Icon::Music: path({{9,17.5f},{9,5},{19.5f,3},{19.5f,15.5f}}); circle(6.5f,17.5f,2.5f); circle(17,15.5f,2.5f); break;
    case Icon::Wrench: begin(14.7f,6.3f); arcto(14.7f,7.7f,1,false,false); to(16.3f,9.3f); arcto(17.7f,9.3f,1,false,false);
        to(21.47f,5.53f); arcto(13.53f,13.47f,6,false,true); to(6.62f,20.38f); arcto(3.62f,17.38f,2.12f,false,true);
        to(10.53f,10.47f); arcto(18.47f,2.53f,6,false,true); end(true); break;
    case Icon::Timer: circle(12,13.5f,7.5f); path({{12,10},{12,13.5f},{14.5f,15}}); line(10,2.5f,14,2.5f);
        line(12,2.5f,12,6); break;
    case Icon::Help: circle(12,12,9); begin(9.3f,9.5f); curve(9.5f,8,10.6f,7,12.1f,7); curve(13.7f,7,14.9f,8.1f,14.9f,9.5f);
        curve(14.9f,11.6f,12,11.9f,12,13.8f); end(); dot(12,16.8f,1); break;
    case Icon::Puzzle: begin(4,8); to(8.5f,8); arcto(12.5f,8,2.4f,true,true); to(17,8); to(17,11.5f);
        arcto(17,15.5f,2.4f,true,true); to(17,20.5f); to(4,20.5f); end(true); break;
    case Icon::PinBadge: begin(8.5f,3); to(15.5f,3); to(14.5f,9); to(18,12.5f); to(6,12.5f); to(9.5f,9);
        end(true); begin(12,12.5f); to(12,21); end(); break;
    case Icon::Wifi: arc(12,20,13.5f,227,313); arc(12,20,9,229,311); arc(12,20,4.6f,232,308);
        dot(12,19.4f,1.05f); break;
    case Icon::Mic: box(9,3,15,14,3); arc(12,10.5f,6.5f,0,180); line(12,17,12,20.5f); line(8.5f,20.5f,15.5f,20.5f); break;
    case Icon::Checkbox: box(4,4,20,20,3); path({{8,12.2f},{11,15.2f},{16.5f,9}}); break;
    case Icon::Highlight: path({{15,4},{20,9},{11.5f,17.5f},{6.5f,12.5f},{15,4}});
        path({{6.5f,12.5f},{4,20},{11.5f,17.5f}}); line(14,20.5f,20.5f,20.5f); break;
    case Icon::Key: circle(8,15.5f,4.5f); line(11.2f,12.3f,19.5f,4); line(16.5f,7,19,9.5f); line(14.2f,9.3f,16.2f,11.3f); break;
    case Icon::Bold: begin(7.5f,19.5f); to(7.5f,4.5f); to(12.5f,4.5f); arcto(12.5f,11.5f,3.5f,false,true); to(7.5f,11.5f); end();
        begin(7.5f,11.5f); to(13.5f,11.5f); arcto(13.5f,19.5f,4,false,true); to(7.5f,19.5f); end(); break;
    case Icon::Layers: path({{12,5},{21,10},{12,15},{3,10},{12,5}}); path({{3,14.5f},{12,19.5f},{21,14.5f}}); break;
    case Icon::Globe: circle(12,12,9); begin(12,3); curve(7.5f,6,7.5f,18,12,21); end();
        begin(12,3); curve(16.5f,6,16.5f,18,12,21); end(); line(3,12,21,12); line(4.6f,7.5f,19.4f,7.5f);
        line(4.6f,16.5f,19.4f,16.5f); break;
    case Icon::Flag: line(5.5f,3,5.5f,21); path({{5.5f,4},{18.5f,4},{15.5f,8.25f},{18.5f,12.5f},{5.5f,12.5f}}); break;
    case Icon::Phone: box(7,2.5f,17,21.5f,2.5f); line(10.5f,5.5f,13.5f,5.5f); dot(12,18.2f,1); break;
    case Icon::CheckCircle: circle(12,12,9); path({{8,12.3f},{10.8f,15.1f},{16.2f,9.3f}}); break;
    case Icon::CheckCircleFilled:
        fill_begin(21,12);
        sink->AddArc(D2D1::ArcSegment({3,12},{9,9},0,D2D1_SWEEP_DIRECTION_CLOCKWISE,D2D1_ARC_SIZE_SMALL));
        sink->AddArc(D2D1::ArcSegment({21,12},{9,9},0,D2D1_SWEEP_DIRECTION_CLOCKWISE,D2D1_ARC_SIZE_SMALL));
        end(true); path({{8,12.3f},{10.8f,15.1f},{16.2f,9.3f}}); break;
    case Icon::Blocked: circle(12,12,9); line(5.6f,5.6f,18.4f,18.4f); break;
    case Icon::Down: line(12,5,12,19); path({{6.5f,13.5f},{12,19},{17.5f,13.5f}}); break;
    default: break;
    }
    const HRESULT result = sink->Close();
    sink->Release();
    if (FAILED(result)) { geometry->Release(); return nullptr; }
    return geometry;
}

// "Pin to X": X's outline, widened to a filled shape, with a round hole in the
// lower right that holds a small pin. Drawn with one fill (see Draw).
inline ID2D1PathGeometry* BuildPinnedGeometry(ID2D1Factory* factory, Icon icon) noexcept {
    constexpr float kTolerance = 0.05f;
    ID2D1PathGeometry* base = BuildGeometry(factory, PinnedBase(icon));
    ID2D1PathGeometry* badge = BuildGeometry(factory, Icon::PinBadge);
    ID2D1StrokeStyle* stroke = nullptr;
    auto properties = D2D1::StrokeStyleProperties();
    properties.startCap = properties.endCap = properties.dashCap = D2D1_CAP_STYLE_ROUND;
    properties.lineJoin = D2D1_LINE_JOIN_ROUND;
    factory->CreateStrokeStyle(properties, nullptr, 0, &stroke);
    ID2D1EllipseGeometry* hole = nullptr;
    factory->CreateEllipseGeometry(D2D1::Ellipse({18.0f, 18.0f}, 6.4f, 6.4f), &hole);
    ID2D1PathGeometry* result = nullptr;
    const auto widen = [&](ID2D1Geometry* source, const D2D1_MATRIX_3X2_F* transform, float width) {
        ID2D1PathGeometry* out = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        if (FAILED(factory->CreatePathGeometry(&out))) return static_cast<ID2D1PathGeometry*>(nullptr);
        if (SUCCEEDED(out->Open(&sink))) {
            sink->SetFillMode(D2D1_FILL_MODE_WINDING);
            const HRESULT hr = source->Widen(width, stroke, transform, kTolerance, sink);
            const HRESULT closed = sink->Close();
            sink->Release();
            if (SUCCEEDED(hr) && SUCCEEDED(closed)) return out;
        }
        out->Release();
        return static_cast<ID2D1PathGeometry*>(nullptr);
    };
    const auto combine = [&](ID2D1Geometry* a, ID2D1Geometry* b, D2D1_COMBINE_MODE mode) {
        ID2D1PathGeometry* out = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        if (FAILED(factory->CreatePathGeometry(&out))) return static_cast<ID2D1PathGeometry*>(nullptr);
        if (SUCCEEDED(out->Open(&sink))) {
            const HRESULT hr = a->CombineWithGeometry(b, mode, nullptr, kTolerance, sink);
            const HRESULT closed = sink->Close();
            sink->Release();
            if (SUCCEEDED(hr) && SUCCEEDED(closed)) return out;
        }
        out->Release();
        return static_cast<ID2D1PathGeometry*>(nullptr);
    };
    if (base && badge && stroke && hole) {
        const D2D1_MATRIX_3X2_F place = D2D1::Matrix3x2F::Scale(0.5f, 0.5f) * D2D1::Matrix3x2F::Translation(12.0f, 12.0f);
        ID2D1PathGeometry* outline = widen(base, nullptr, kStroke);
        ID2D1PathGeometry* pin = widen(badge, &place, kStroke * 0.85f);
        ID2D1PathGeometry* cut = outline ? combine(outline, hole, D2D1_COMBINE_MODE_EXCLUDE) : nullptr;
        if (cut && pin) result = combine(cut, pin, D2D1_COMBINE_MODE_UNION);
        for (ID2D1PathGeometry* g : {outline, pin, cut}) if (g) g->Release();
    }
    for (IUnknown* object : std::initializer_list<IUnknown*>{base, badge, stroke, hole})
        if (object) object->Release();
    return result;
}

// ---------------------------------------------------------------------------
// Two-tone designs (Pulse 工具栏图标 mockup, https://claude.ai/artifact/7FnZXLUEEbUfVwsCR4tFmU).
// The icons listed in kDesigns are drawn from this SVG path data instead of
// BuildGeometry: a body in the ink plus at most one accent part (the
// clipboard's clip, the rename caret, the bin's lid, the plus of New). Without
// an accent brush the whole icon takes the ink, so menus and dialogs keep one
// colour.
// ---------------------------------------------------------------------------

// The command category an accent part is coloured by (MainRenderer picks the
// colours; Nav and Clip follow the user's accent). None never takes an accent.
enum class Tone : uint8_t { None, Nav, Clip, New, Edit, Danger, View, Place, Count };

inline constexpr uint8_t kAccent = 1;  // drawn with the accent brush
inline constexpr uint8_t kFill = 2;    // filled instead of stroked
inline constexpr uint8_t kTint = 4;    // filled translucent: 35%, 50% in an accent colour

struct DesignPart {
    const char* d;   // SVG path data on the 24-unit canvas
    uint8_t flags;
};
struct Design {
    Icon icon;
    Tone tone;
    uint16_t body, body_count;    // range in kDesignParts
    uint16_t badge, badge_count;  // drawn over a cut-out in the lower right
};

// Badged icons: the body is cut away under this circle, the badge sits in it.
inline constexpr float kBadgeX = 17.5f, kBadgeY = 17.5f, kBadgeCut = 5.4f;

// Generated from the mockup's icon list (GROUPS in its script); circles and
// rects are written out as paths.
inline constexpr DesignPart kDesignParts[] = {
    {"M19.25 12H5", 0},
    {"M11 5.5 4.75 12 11 18.5", 0},
    {"M4.75 12H19", 0},
    {"M13 5.5 19.25 12 13 18.5", 0},
    {"M12 19.25V5", 0},
    {"M5.5 11 12 4.75 18.5 11", 0},
    {"M19.25 12a7.25 7.25 0 1 1-2.12-5.13L19.25 9", 0},
    {"M19.25 4.5V9h-4.5", 0},
    {"M4.25 10 12 3.75 19.75 10v8.25a2 2 0 0 1-2 2H6.25a2 2 0 0 1-2-2Z", 0},
    {"M9.75 20.25v-4.5a2.25 2.25 0 0 1 4.5 0v4.5", kAccent},
    {"M17.25 10.75A6.5 6.5 0 1 1 4.25 10.75A6.5 6.5 0 1 1 17.25 10.75Z", 0},
    {"m15.5 15.5 4.25 4.25", 0},
    {"M16.5 10.25A6.25 6.25 0 1 1 4 10.25A6.25 6.25 0 1 1 16.5 10.25Z", 0},
    {"m14.75 14.75 1.5 1.5", 0},
    {"M14.25 15.25h6.5", kAccent},
    {"M15.75 17.75h3.5", kAccent},
    {"M16.75 20.25h1.5", kAccent},
    {"M4.75 12a7.25 7.25 0 1 0 2.12-5.13L4.75 9", 0},
    {"M4.75 4.5V9h4.5", 0},
    {"M12 8v4.25l2.75 1.75", kAccent},
    {"M7.15 12A1.4 1.4 0 1 1 4.35 12A1.4 1.4 0 1 1 7.15 12Z", kFill},
    {"M13.4 12A1.4 1.4 0 1 1 10.6 12A1.4 1.4 0 1 1 13.4 12Z", kFill},
    {"M19.65 12A1.4 1.4 0 1 1 16.85 12A1.4 1.4 0 1 1 19.65 12Z", kFill},
    {"m6.5 9.5 5.5 5.5 5.5-5.5", 0},
    {"M12 4.75v14.5", kAccent},
    {"M4.75 12h14.5", kAccent},
    {"M3.25 7.25A2.25 2.25 0 0 1 5.5 5h3.4c.53 0 1.04.21 1.41.59l1.16 1.16h7.03a2.25 2.25 0 0 1 2.25 2.25v8.5a2.25 2.25 0 0 1-2.25 2.25h-13a2.25 2.25 0 0 1-2.25-2.25Z", 0},
    {"M3.25 9.5h6.6c.53 0 1.04-.21 1.41-.59l2.16-2.16", 0},
    {"M17.5 14.5v6", kAccent},
    {"M14.5 17.5h6", kAccent},
    {"M7 2.75h5.88c.6 0 1.17.24 1.59.66l4.12 4.12c.42.42.66.99.66 1.59V19a2.25 2.25 0 0 1-2.25 2.25H7A2.25 2.25 0 0 1 4.75 19V5A2.25 2.25 0 0 1 7 2.75Z", 0},
    {"M13 3v4.25c0 .69.56 1.25 1.25 1.25H19", 0},
    {"M17.5 14.5v6", kAccent},
    {"M14.5 17.5h6", kAccent},
    {"M7 2.75h5.88c.6 0 1.17.24 1.59.66l4.12 4.12c.42.42.66.99.66 1.59V19a2.25 2.25 0 0 1-2.25 2.25H7A2.25 2.25 0 0 1 4.75 19V5A2.25 2.25 0 0 1 7 2.75Z", 0},
    {"M13 3v4.25c0 .69.56 1.25 1.25 1.25H19", 0},
    {"M7.75 17.5v-4.5l1.75 2 1.75-2v4.5", 0},
    {"M14.5 13v4.25", kAccent},
    {"m13.25 16.25 1.25 1.25 1.25-1.25", kAccent},
    {"M9.75 17.25A2.75 2.75 0 1 1 4.25 17.25A2.75 2.75 0 1 1 9.75 17.25Z", kAccent},
    {"M19.75 17.25A2.75 2.75 0 1 1 14.25 17.25A2.75 2.75 0 1 1 19.75 17.25Z", kAccent},
    {"M8.95 15.3 18 4.75", 0},
    {"M15.05 15.3 6 4.75", 0},
    {"M8.25 8V5.75A2 2 0 0 1 10.25 3.75h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H16", 0},
    {"M6 8h7.75A2.25 2.25 0 0 1 16 10.25V18a2.25 2.25 0 0 1-2.25 2.25H6A2.25 2.25 0 0 1 3.75 18v-7.75A2.25 2.25 0 0 1 6 8Z", kAccent},
    {"M8.5 5H7.25A2.25 2.25 0 0 0 5 7.25v11.5A2.25 2.25 0 0 0 7.25 21h9.5A2.25 2.25 0 0 0 19 18.75V7.25A2.25 2.25 0 0 0 16.75 5H15.5", 0},
    {"M9.75 3H14.25A1.25 1.25 0 0 1 15.5 4.25V5.75A1.25 1.25 0 0 1 14.25 7H9.75A1.25 1.25 0 0 1 8.5 5.75V4.25A1.25 1.25 0 0 1 9.75 3Z", kAccent},
    {"M10.5 7.25H5.5A2.25 2.25 0 0 0 3.25 9.5v5a2.25 2.25 0 0 0 2.25 2.25h5", 0},
    {"M17.75 7.25h.75a2.25 2.25 0 0 1 2.25 2.25v5a2.25 2.25 0 0 1-2.25 2.25h-.75", 0},
    {"M14.25 4.25v15.5", kAccent},
    {"M12 4.25h4.5", kAccent},
    {"M12 19.75h4.5", kAccent},
    {"M3.75 7h7.5", 0},
    {"M3.75 12h7.5", 0},
    {"M3.75 17h7.5", 0},
    {"M16.5 4.25v15.5", kAccent},
    {"M14.25 4.25h4.5", kAccent},
    {"M14.25 19.75h4.5", kAccent},
    {"M8.25 8V5.75A2 2 0 0 1 10.25 3.75h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H16", 0},
    {"M6 8h7.75A2.25 2.25 0 0 1 16 10.25V18a2.25 2.25 0 0 1-2.25 2.25H6A2.25 2.25 0 0 1 3.75 18v-7.75A2.25 2.25 0 0 1 6 8Z", 0},
    {"m11.5 11.25-3.25 6", kAccent},
    {"M12 3.75V14", kAccent},
    {"m7.75 8 4.25-4.25L16.25 8", kAccent},
    {"M8 10.75H6.75a2 2 0 0 0-2 2v5.5a2 2 0 0 0 2 2h10.5a2 2 0 0 0 2-2v-5.5a2 2 0 0 0-2-2H16", 0},
    {"M4.5 6.5h15", kAccent},
    {"M9.5 6.5V5a1.5 1.5 0 0 1 1.5-1.5h2A1.5 1.5 0 0 1 14.5 5v1.5", kAccent},
    {"M6.25 6.5l.86 12.3a2 2 0 0 0 2 1.86h5.78a2 2 0 0 0 2-1.86l.86-12.3", 0},
    {"M10.25 10.5v6", 0},
    {"M13.75 10.5v6", 0},
    {"M20.25 12A8.25 8.25 0 1 1 3.75 12A8.25 8.25 0 1 1 20.25 12Z", 0},
    {"M12 11v5.25", kAccent},
    {"M13.1 7.9A1.1 1.1 0 1 1 10.9 7.9A1.1 1.1 0 1 1 13.1 7.9Z", kAccent|kFill},
    {"M7.25 4.75v14.5", kAccent},
    {"m4 15.75 3.25 3.5 3.25-3.5", kAccent},
    {"M13.25 6.5h7", 0},
    {"M13.25 12h5", 0},
    {"M13.25 17.5h3", 0},
    {"M4 7h16", 0},
    {"M7 12h10", 0},
    {"M10 17h4", kAccent},
    {"M5.5 4.25H9A1.75 1.75 0 0 1 10.75 6V9A1.75 1.75 0 0 1 9 10.75H5.5A1.75 1.75 0 0 1 3.75 9V6A1.75 1.75 0 0 1 5.5 4.25Z", kAccent},
    {"M5.5 13.25H9A1.75 1.75 0 0 1 10.75 15V18A1.75 1.75 0 0 1 9 19.75H5.5A1.75 1.75 0 0 1 3.75 18V15A1.75 1.75 0 0 1 5.5 13.25Z", kAccent},
    {"M14 6h6.25", 0},
    {"M14 9h4", 0},
    {"M14 15h6.25", 0},
    {"M14 18h4", 0},
    {"M6.25 6.5A1.25 1.25 0 1 1 3.75 6.5A1.25 1.25 0 1 1 6.25 6.5Z", kAccent|kFill},
    {"M6.25 12A1.25 1.25 0 1 1 3.75 12A1.25 1.25 0 1 1 6.25 12Z", kAccent|kFill},
    {"M6.25 17.5A1.25 1.25 0 1 1 3.75 17.5A1.25 1.25 0 1 1 6.25 17.5Z", kAccent|kFill},
    {"M9 6.5h11", 0},
    {"M9 12h11", 0},
    {"M9 17.5h11", 0},
    {"M5.5 3.75H9A1.75 1.75 0 0 1 10.75 5.5V9A1.75 1.75 0 0 1 9 10.75H5.5A1.75 1.75 0 0 1 3.75 9V5.5A1.75 1.75 0 0 1 5.5 3.75Z", kAccent},
    {"M15 3.75H18.5A1.75 1.75 0 0 1 20.25 5.5V9A1.75 1.75 0 0 1 18.5 10.75H15A1.75 1.75 0 0 1 13.25 9V5.5A1.75 1.75 0 0 1 15 3.75Z", 0},
    {"M5.5 13.25H9A1.75 1.75 0 0 1 10.75 15V18.5A1.75 1.75 0 0 1 9 20.25H5.5A1.75 1.75 0 0 1 3.75 18.5V15A1.75 1.75 0 0 1 5.5 13.25Z", 0},
    {"M15 13.25H18.5A1.75 1.75 0 0 1 20.25 15V18.5A1.75 1.75 0 0 1 18.5 20.25H15A1.75 1.75 0 0 1 13.25 18.5V15A1.75 1.75 0 0 1 15 13.25Z", 0},
    {"M5.5 4.75H18.5A2.25 2.25 0 0 1 20.75 7V17A2.25 2.25 0 0 1 18.5 19.25H5.5A2.25 2.25 0 0 1 3.25 17V7A2.25 2.25 0 0 1 5.5 4.75Z", 0},
    {"M9.25 4.75v14.5", kAccent},
    {"M14.75 4.75v14.5", kAccent},
    {"M4 5.5h16", kAccent},
    {"M4 9.83h16", 0},
    {"M4 14.17h16", 0},
    {"M4 18.5h16", 0},
    {"M5.5 4.75H18.5A2.25 2.25 0 0 1 20.75 7V17A2.25 2.25 0 0 1 18.5 19.25H5.5A2.25 2.25 0 0 1 3.25 17V7A2.25 2.25 0 0 1 5.5 4.75Z", 0},
    {"M14.5 4.75v14.5", 0},
    {"M16.75 8.5h1.75", kAccent},
    {"M16.75 11.5h1.75", kAccent},
    {"M5.5 4.75H18.5A2.25 2.25 0 0 1 20.75 7V17A2.25 2.25 0 0 1 18.5 19.25H5.5A2.25 2.25 0 0 1 3.25 17V7A2.25 2.25 0 0 1 5.5 4.75Z", 0},
    {"M9.5 4.75v14.5", 0},
    {"M5.75 8.5h1.5", kAccent},
    {"M5.75 11.5h1.5", kAccent},
    {"M5.5 4.75H18.5A2.25 2.25 0 0 1 20.75 7V17A2.25 2.25 0 0 1 18.5 19.25H5.5A2.25 2.25 0 0 1 3.25 17V7A2.25 2.25 0 0 1 5.5 4.75Z", 0},
    {"M12 4.75v14.5", kAccent},
    {"M5.5 4.75H18.5A2.25 2.25 0 0 1 20.75 7V17A2.25 2.25 0 0 1 18.5 19.25H5.5A2.25 2.25 0 0 1 3.25 17V7A2.25 2.25 0 0 1 5.5 4.75Z", 0},
    {"M3.25 12h17.5", kAccent},
    {"M5.5 4.75H18.5A2.25 2.25 0 0 1 20.75 7V17A2.25 2.25 0 0 1 18.5 19.25H5.5A2.25 2.25 0 0 1 3.25 17V7A2.25 2.25 0 0 1 5.5 4.75Z", 0},
    {"M12 4.75v14.5", kAccent},
    {"M12 12h8.75", kAccent},
    {"M5.5 4.75H18.5A2.25 2.25 0 0 1 20.75 7V17A2.25 2.25 0 0 1 18.5 19.25H5.5A2.25 2.25 0 0 1 3.25 17V7A2.25 2.25 0 0 1 5.5 4.75Z", 0},
    {"M12 4.75v14.5", kAccent},
    {"M3.25 12h17.5", kAccent},
    {"M10 4.5H6.5a2.25 2.25 0 0 0-2.25 2.25v10.5A2.25 2.25 0 0 0 6.5 19.5H10", 0},
    {"M14 4.5h3.5a2.25 2.25 0 0 1 2.25 2.25v10.5a2.25 2.25 0 0 1-2.25 2.25H14", 0},
    {"M12 2.75v18.5", kAccent},
    {"M14 4.75h5.25V10", kAccent},
    {"M19 5l-5.25 5.25", kAccent},
    {"M10 19.25H4.75V14", 0},
    {"M5 19l5.25-5.25", 0},
    {"M19.25 4.75 14.5 9.5", kAccent},
    {"M14.25 5.5v4.25h4.25", kAccent},
    {"M4.75 19.25 9.5 14.5", 0},
    {"M9.75 18.5v-4.25H5.5", 0},
    {"M2.75 12C4.6 8.25 8 5.75 12 5.75s7.4 2.5 9.25 6.25C19.4 15.75 16 18.25 12 18.25S4.6 15.75 2.75 12Z", 0},
    {"M15 12A3 3 0 1 1 9 12A3 3 0 1 1 15 12Z", kAccent},
    {"M2.75 12C4.6 8.25 8 5.75 12 5.75s7.4 2.5 9.25 6.25C19.4 15.75 16 18.25 12 18.25S4.6 15.75 2.75 12Z", 0},
    {"M15 12A3 3 0 1 1 9 12A3 3 0 1 1 15 12Z", 0},
    {"m4 4 16 16", kAccent},
    {"M4 8.5V6.25A2.25 2.25 0 0 1 6.25 4H8.5", 0},
    {"M15.5 4h2.25A2.25 2.25 0 0 1 20 6.25V8.5", 0},
    {"M20 15.5v2.25A2.25 2.25 0 0 1 17.75 20H15.5", 0},
    {"M8.5 20H6.25A2.25 2.25 0 0 1 4 17.75V15.5", 0},
    {"m8.25 12.25 2.5 2.5 5-5", kAccent},
    {"M6.5 4.25H17.5A2.25 2.25 0 0 1 19.75 6.5V17.5A2.25 2.25 0 0 1 17.5 19.75H6.5A2.25 2.25 0 0 1 4.25 17.5V6.5A2.25 2.25 0 0 1 6.5 4.25Z", 0},
    {"M19.75 6.5v11a2.25 2.25 0 0 1-2.25 2.25h-11Z", kAccent|kTint},
    {"M4 8.5V6.25A2.25 2.25 0 0 1 6.25 4H8.5", 0},
    {"M15.5 4h2.25A2.25 2.25 0 0 1 20 6.25V8.5", 0},
    {"M20 15.5v2.25A2.25 2.25 0 0 1 17.75 20H15.5", 0},
    {"M8.5 20H6.25A2.25 2.25 0 0 1 4 17.75V15.5", 0},
    {"M12 8.25v7.5", kAccent},
    {"m8.75 10.1 6.5 3.8", kAccent},
    {"m15.25 10.1-6.5 3.8", kAccent},
    {"M8.25 8V5.75A2 2 0 0 1 10.25 3.75h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H16", 0},
    {"M6 8h7.75A2.25 2.25 0 0 1 16 10.25V18a2.25 2.25 0 0 1-2.25 2.25H6A2.25 2.25 0 0 1 3.75 18v-7.75A2.25 2.25 0 0 1 6 8Z", 0},
    {"M14.5 17.5h6", kAccent},
    {"m18 15 2.5 2.5L18 20", kAccent},
    {"M11.5 3.75H18A2.25 2.25 0 0 1 20.25 6V13.75A2.25 2.25 0 0 1 18 16H11.5A2.25 2.25 0 0 1 9.25 13.75V6A2.25 2.25 0 0 1 11.5 3.75Z", 0},
    {"M3.75 7.5h3", 0},
    {"M2.75 10.75h4", 0},
    {"M3.75 14h3", 0},
    {"M14.5 17.5h6", kAccent},
    {"m18 15 2.5 2.5L18 20", kAccent},
    {"M3.25 7.25A2.25 2.25 0 0 1 5.5 5h3.4c.53 0 1.04.21 1.41.59l1.16 1.16h7.03a2.25 2.25 0 0 1 2.25 2.25v8.5a2.25 2.25 0 0 1-2.25 2.25h-13a2.25 2.25 0 0 1-2.25-2.25Z", 0},
    {"M3.25 9.5h6.6c.53 0 1.04-.21 1.41-.59l2.16-2.16", 0},
    {"M14.5 17.5h6", kAccent},
    {"m18 15 2.5 2.5L18 20", kAccent},
    {"M10.25 5H7a2.25 2.25 0 0 0-2.25 2.25V17A2.25 2.25 0 0 0 7 19.25h9.75A2.25 2.25 0 0 0 19 17v-3.25", 0},
    {"M13.75 4.75h5.5v5.5", kAccent},
    {"M19 5l-7.5 7.5", kAccent},
    {"M7.25 3.25H16.75A2.25 2.25 0 0 1 19 5.5V18.5A2.25 2.25 0 0 1 16.75 20.75H7.25A2.25 2.25 0 0 1 5 18.5V5.5A2.25 2.25 0 0 1 7.25 3.25Z", 0},
    {"M12 3.25V5", 0},
    {"M10.75 6.5H12", 0},
    {"M12 8h1.25", 0},
    {"M10.75 9.5H12", 0},
    {"M11.25 11.5H12.75A1 1 0 0 1 13.75 12.5V15A1 1 0 0 1 12.75 16H11.25A1 1 0 0 1 10.25 15V12.5A1 1 0 0 1 11.25 11.5Z", kAccent},
    {"M8.75 14.5 4.5 10.25 8.75 6", kAccent},
    {"M4.75 10.25h9.5a5 5 0 0 1 0 10H11", 0},
    {"M15.25 14.5 19.5 10.25 15.25 6", kAccent},
    {"M19.25 10.25h-9.5a5 5 0 0 0 0 10H13", 0},
    {"M5.5 4.75H18.5A2.25 2.25 0 0 1 20.75 7V17A2.25 2.25 0 0 1 18.5 19.25H5.5A2.25 2.25 0 0 1 3.25 17V7A2.25 2.25 0 0 1 5.5 4.75Z", 0},
    {"m7.25 9.5 2.75 2.5-2.75 2.5", 0},
    {"M12.5 15h4.25", kAccent},
    {"M9.25 3.75h5.5", kAccent},
    {"M10.25 3.75v5.5L7.5 12.5h9l-2.75-3.25v-5.5", kAccent},
    {"M12 12.5v7.75", 0},
    {"M12 3.7 14.56 9.08 20.46 9.85 16.14 13.94 17.23 19.8 12 16.95 6.77 19.8 7.86 13.94 3.54 9.85 9.44 9.08Z", kAccent},
    {"M3.75 5.75v5.1c0 .53.21 1.04.59 1.41l7.4 7.4a2 2 0 0 0 2.82 0l5.1-5.1a2 2 0 0 0 0-2.82l-7.4-7.4a2 2 0 0 0-1.41-.59h-5.1a2 2 0 0 0-2 2Z", 0},
    {"M9.3 8A1.3 1.3 0 1 1 6.7 8A1.3 1.3 0 1 1 9.3 8Z", kAccent|kFill},
    {"M10.5 13.5a3.75 3.75 0 0 0 5.3 0l2.95-2.95a3.75 3.75 0 1 0-5.3-5.3l-1.2 1.2", 0},
    {"M13.5 10.5a3.75 3.75 0 0 0-5.3 0l-2.95 2.95a3.75 3.75 0 1 0 5.3 5.3l1.2-1.2", kAccent},
    {"M3.25 7.25A2.25 2.25 0 0 1 5.5 5h3.4c.53 0 1.04.21 1.41.59l1.16 1.16h7.03a2.25 2.25 0 0 1 2.25 2.25v8.5a2.25 2.25 0 0 1-2.25 2.25h-13a2.25 2.25 0 0 1-2.25-2.25Z", 0},
    {"M3.25 9.5h6.6c.53 0 1.04-.21 1.41-.59l2.16-2.16", kAccent},
    {"M3.25 18.5V7.25A2.25 2.25 0 0 1 5.5 5h3.4c.53 0 1.04.21 1.41.59l1.16 1.16h6.03a2.25 2.25 0 0 1 2.25 2.25v2", 0},
    {"M3.6 19.1 6.1 12.4a1.75 1.75 0 0 1 1.64-1.15h12.43a.9.9 0 0 1 .84 1.22l-2.4 6.12a1.75 1.75 0 0 1-1.63 1.11H4.45a.85.85 0 0 1-.85-.6Z", kAccent},
    {"M7 2.75h5.88c.6 0 1.17.24 1.59.66l4.12 4.12c.42.42.66.99.66 1.59V19a2.25 2.25 0 0 1-2.25 2.25H7A2.25 2.25 0 0 1 4.75 19V5A2.25 2.25 0 0 1 7 2.75Z", 0},
    {"M13 3v4.25c0 .69.56 1.25 1.25 1.25H19", kAccent},
    {"M5.5 4.75H18.5A2.25 2.25 0 0 1 20.75 7V17A2.25 2.25 0 0 1 18.5 19.25H5.5A2.25 2.25 0 0 1 3.25 17V7A2.25 2.25 0 0 1 5.5 4.75Z", 0},
    {"M10.25 9.75A1.5 1.5 0 1 1 7.25 9.75A1.5 1.5 0 1 1 10.25 9.75Z", kAccent},
    {"m3.75 17.75 4.6-4.6a1.25 1.25 0 0 1 1.77 0l4.38 4.38", kAccent},
    {"m13 15.75 1.85-1.85a1.25 1.25 0 0 1 1.77 0l3.63 3.63", kAccent},
    {"M5.5 4.75H18.5A2.25 2.25 0 0 1 20.75 7V17A2.25 2.25 0 0 1 18.5 19.25H5.5A2.25 2.25 0 0 1 3.25 17V7A2.25 2.25 0 0 1 5.5 4.75Z", 0},
    {"M6.75 7.5H8A0.75 0.75 0 0 1 8.75 8.25V9.5A0.75 0.75 0 0 1 8 10.25H6.75A0.75 0.75 0 0 1 6 9.5V8.25A0.75 0.75 0 0 1 6.75 7.5Z", kAccent},
    {"M6.75 12H8A0.75 0.75 0 0 1 8.75 12.75V14A0.75 0.75 0 0 1 8 14.75H6.75A0.75 0.75 0 0 1 6 14V12.75A0.75 0.75 0 0 1 6.75 12Z", kAccent},
    {"M3.25 16.25h17.5", 0},
    {"M4.75 4H19.25A2 2 0 0 1 21.25 6V14.5A2 2 0 0 1 19.25 16.5H4.75A2 2 0 0 1 2.75 14.5V6A2 2 0 0 1 4.75 4Z", 0},
    {"M12 16.5v3.75", kAccent},
    {"M8.25 20.25h7.5", kAccent},
    {"M5 7.25H19A2.25 2.25 0 0 1 21.25 9.5V14.5A2.25 2.25 0 0 1 19 16.75H5A2.25 2.25 0 0 1 2.75 14.5V9.5A2.25 2.25 0 0 1 5 7.25Z", 0},
    {"M6.5 12h5", 0},
    {"M18.15 12A1.15 1.15 0 1 1 15.85 12A1.15 1.15 0 1 1 18.15 12Z", kAccent|kFill},
    {"M9.75 3.5H14.25A1.5 1.5 0 0 1 15.75 5V7.75A1.5 1.5 0 0 1 14.25 9.25H9.75A1.5 1.5 0 0 1 8.25 7.75V5A1.5 1.5 0 0 1 9.75 3.5Z", kAccent},
    {"M12 9.25v6.75", 0},
    {"M5 16v-2a1.75 1.75 0 0 1 1.75-1.75h10.5A1.75 1.75 0 0 1 19 14v2", 0},
    {"M4 16H6A1.25 1.25 0 0 1 7.25 17.25V19A1.25 1.25 0 0 1 6 20.25H4A1.25 1.25 0 0 1 2.75 19V17.25A1.25 1.25 0 0 1 4 16Z", 0},
    {"M11 16H13A1.25 1.25 0 0 1 14.25 17.25V19A1.25 1.25 0 0 1 13 20.25H11A1.25 1.25 0 0 1 9.75 19V17.25A1.25 1.25 0 0 1 11 16Z", 0},
    {"M18 16H20A1.25 1.25 0 0 1 21.25 17.25V19A1.25 1.25 0 0 1 20 20.25H18A1.25 1.25 0 0 1 16.75 19V17.25A1.25 1.25 0 0 1 18 16Z", 0},
    {"M12 3.75V15", kAccent},
    {"m7.25 10.5 4.75 4.75 4.75-4.75", kAccent},
    {"M5 20h14", 0},
    {"M7.25 19h10a4.25 4.25 0 0 0 .7-8.44 6 6 0 0 0-11.5 1.2A3.65 3.65 0 0 0 7.25 19Z", 0},
    {"M9.75 14.75 11.5 16.5l3-3", kAccent},
    {"M4.5 6.5h15", 0},
    {"M9.5 6.5V5a1.5 1.5 0 0 1 1.5-1.5h2A1.5 1.5 0 0 1 14.5 5v1.5", 0},
    {"M6.25 6.5l.86 12.3a2 2 0 0 0 2 1.86h5.78a2 2 0 0 0 2-1.86l.86-12.3", 0},
    {"M13.13 11.55A2.25 2.25 0 1 1 10.87 11.55", kAccent},
    {"m10.55 9.95.32 1.6 1.6-.32", kAccent},
    {"M5.25 4.75H10.75A2 2 0 0 1 12.75 6.75V17.25A2 2 0 0 1 10.75 19.25H5.25A2 2 0 0 1 3.25 17.25V6.75A2 2 0 0 1 5.25 4.75Z", 0},
    {"M16.5 4.75H19A1.75 1.75 0 0 1 20.75 6.5V9.25A1.75 1.75 0 0 1 19 11H16.5A1.75 1.75 0 0 1 14.75 9.25V6.5A1.75 1.75 0 0 1 16.5 4.75Z", kAccent},
    {"M16.5 13H19A1.75 1.75 0 0 1 20.75 14.75V17.5A1.75 1.75 0 0 1 19 19.25H16.5A1.75 1.75 0 0 1 14.75 17.5V14.75A1.75 1.75 0 0 1 16.5 13Z", kAccent},
    {"M3.75 13.5h4.6l1.4 2.25h4.5l1.4-2.25h4.6", kAccent},
    {"M5.65 6.5A2 2 0 0 1 7.5 5.25h9a2 2 0 0 1 1.85 1.25l1.9 7v4.75a2 2 0 0 1-2 2H5.75a2 2 0 0 1-2-2V13.5Z", 0},
    {"M10.17 5.4 10.59 3.11A9 9 0 0 1 13.41 3.11L13.83 5.4A6.85 6.85 0 0 1 15.37 6.04L17.29 4.72A9 9 0 0 1 19.28 6.71L17.96 8.63A6.85 6.85 0 0 1 18.6 10.17L20.89 10.59A9 9 0 0 1 20.89 13.41L18.6 13.83A6.85 6.85 0 0 1 17.96 15.37L19.28 17.29A9 9 0 0 1 17.29 19.28L15.37 17.96A6.85 6.85 0 0 1 13.83 18.6L13.41 20.89A9 9 0 0 1 10.59 20.89L10.17 18.6A6.85 6.85 0 0 1 8.63 17.96L6.71 19.28A9 9 0 0 1 4.72 17.29L6.04 15.37A6.85 6.85 0 0 1 5.4 13.83L3.11 13.41A9 9 0 0 1 3.11 10.59L5.4 10.17A6.85 6.85 0 0 1 6.04 8.63L4.72 6.71A9 9 0 0 1 6.71 4.72L8.63 6.04A6.85 6.85 0 0 1 10.17 5.4Z", 0},
    {"M15 12A3 3 0 1 1 9 12A3 3 0 1 1 15 12Z", kAccent},
    {"M4 7.5h8.5", 0},
    {"M17.5 7.5H20", 0},
    {"M17.25 7.5A2.25 2.25 0 1 1 12.75 7.5A2.25 2.25 0 1 1 17.25 7.5Z", kAccent},
    {"M4 16.5h2.5", 0},
    {"M11.5 16.5H20", 0},
    {"M11.25 16.5A2.25 2.25 0 1 1 6.75 16.5A2.25 2.25 0 1 1 11.25 16.5Z", kAccent},
    {"M7.5 10.25H16.5A2.25 2.25 0 0 1 18.75 12.5V18A2.25 2.25 0 0 1 16.5 20.25H7.5A2.25 2.25 0 0 1 5.25 18V12.5A2.25 2.25 0 0 1 7.5 10.25Z", 0},
    {"M8.5 10.25V7.75a3.5 3.5 0 0 1 7 0v2.5", 0},
    {"M12 14.25v2", kAccent},
    {"m5 12.75 4.5 4.5L19 7.75", 0},
    {"m6.25 6.25 11.5 11.5", 0},
    {"m17.75 6.25-11.5 11.5", 0},
    {"M11 3.5c.5 3.9 2.1 5.5 6 6-3.9.5-5.5 2.1-6 6-.5-3.9-2.1-5.5-6-6 3.9-.5 5.5-2.1 6-6Z", kAccent|kFill},
    {"M18 14.5c.25 1.9 1 2.65 2.9 2.9-1.9.25-2.65 1-2.9 2.9-.25-1.9-1-2.65-2.9-2.9 1.9-.25 2.65-1 2.9-2.9Z", kAccent|kFill},
};
inline constexpr Design kDesigns[] = {
    {Icon::Back, Tone::None, 0, 2, 2, 0},  // 后退
    {Icon::Forward, Tone::None, 2, 2, 4, 0},  // 前进
    {Icon::Up, Tone::None, 4, 2, 6, 0},  // 上一级
    {Icon::Refresh, Tone::None, 6, 2, 8, 0},  // 刷新
    {Icon::Home, Tone::Nav, 8, 2, 10, 0},  // 主页
    {Icon::Search, Tone::None, 10, 2, 12, 0},  // 搜索
    {Icon::AdvSearch, Tone::View, 12, 2, 14, 3},  // 高级搜索
    {Icon::History, Tone::Nav, 17, 3, 20, 0},  // 最近使用
    {Icon::More, Tone::None, 20, 3, 23, 0},  // 更多
    {Icon::ChevronDown, Tone::None, 23, 1, 24, 0},  // 展开
    {Icon::Add, Tone::New, 24, 2, 26, 0},  // 新建
    {Icon::FolderAdd, Tone::New, 26, 2, 28, 2},  // 新建文件夹
    {Icon::FileAdd, Tone::New, 30, 2, 32, 2},  // 新建文件
    {Icon::NewMarkdown, Tone::New, 34, 5, 39, 0},  // 新建 Markdown
    {Icon::Cut, Tone::Clip, 39, 4, 43, 0},  // 剪切
    {Icon::Copy, Tone::Clip, 43, 2, 45, 0},  // 复制
    {Icon::Paste, Tone::Clip, 45, 2, 47, 0},  // 粘贴
    {Icon::Rename, Tone::Edit, 47, 5, 52, 0},  // 重命名
    {Icon::BatchRename, Tone::Edit, 52, 6, 58, 0},  // 批量重命名
    {Icon::CopyPath, Tone::Clip, 187, 2, 189, 0},  // 复制路径: the chain (Link's parts), not two pages
    {Icon::Send, Tone::Clip, 61, 3, 64, 0},  // 分享
    {Icon::Delete, Tone::Danger, 64, 5, 69, 0},  // 删除
    {Icon::Info, Tone::Nav, 69, 3, 72, 0},  // 属性
    {Icon::Sort, Tone::View, 72, 5, 77, 0},  // 排序
    {Icon::Filter, Tone::View, 77, 3, 80, 0},  // 筛选
    {Icon::GroupBy, Tone::View, 80, 6, 86, 0},  // 分组
    {Icon::List, Tone::View, 86, 6, 92, 0},  // 详细信息
    {Icon::Grid, Tone::View, 92, 4, 96, 0},  // 图标
    {Icon::Columns, Tone::View, 96, 3, 99, 0},  // 列视图
    {Icon::Compact, Tone::View, 99, 4, 103, 0},  // 紧凑
    {Icon::Panel, Tone::View, 103, 4, 107, 0},  // 预览窗格
    {Icon::Sidebar, Tone::View, 107, 4, 111, 0},  // 侧栏
    {Icon::Split, Tone::View, 111, 2, 113, 0},  // 左右分屏
    {Icon::SplitStacked, Tone::View, 113, 2, 115, 0},  // 上下分屏
    {Icon::SplitThree, Tone::View, 115, 3, 118, 0},  // 三栏
    {Icon::SplitFour, Tone::View, 118, 3, 121, 0},  // 四宫格
    {Icon::Compare, Tone::View, 121, 3, 124, 0},  // 对比
    {Icon::PaneExpand, Tone::View, 124, 4, 128, 0},  // 临时单栏
    {Icon::PaneRestore, Tone::View, 128, 4, 132, 0},  // 还原布局
    {Icon::Eye, Tone::View, 132, 2, 134, 0},  // 显示
    {Icon::EyeHidden, Tone::View, 134, 3, 137, 0},  // 隐藏的项目
    {Icon::SelectAll, Tone::Clip, 137, 5, 142, 0},  // 全选
    {Icon::Invert, Tone::Clip, 142, 2, 144, 0},  // 反向选择
    {Icon::SelectWild, Tone::Clip, 144, 7, 151, 0},  // 按规则选择
    {Icon::CopyToPane, Tone::Clip, 151, 2, 153, 2},  // 复制到另一栏
    {Icon::MoveToPane, Tone::Clip, 155, 4, 159, 2},  // 移动到另一栏
    {Icon::FolderGo, Tone::Clip, 161, 2, 163, 2},  // 打开所在位置
    {Icon::OpenExternal, Tone::Clip, 165, 3, 168, 0},  // 用其他程序打开
    {Icon::Zip, Tone::Place, 168, 6, 174, 0},  // 压缩
    {Icon::Undo, Tone::Nav, 174, 2, 176, 0},  // 撤销
    {Icon::Redo, Tone::Nav, 176, 2, 178, 0},  // 重做
    {Icon::Terminal, Tone::Nav, 178, 3, 181, 0},  // 在终端中打开
    {Icon::Pin, Tone::Nav, 181, 3, 184, 0},  // 固定
    {Icon::Star, Tone::Place, 184, 1, 185, 0},  // 收藏
    {Icon::Tag, Tone::Place, 185, 2, 187, 0},  // 标签
    {Icon::Link, Tone::Nav, 187, 2, 189, 0},  // 链接
    {Icon::Folder, Tone::Place, 189, 2, 191, 0},  // 文件夹
    {Icon::FolderOpen, Tone::Place, 191, 2, 193, 0},  // 打开的文件夹
    {Icon::File, Tone::Nav, 193, 2, 195, 0},  // 文件
    {Icon::Image, Tone::New, 195, 4, 199, 0},  // 图片
    {Icon::Desktop, Tone::Nav, 199, 4, 203, 0},  // 桌面
    {Icon::Computer, Tone::Nav, 203, 3, 206, 0},  // 此电脑
    {Icon::Drive, Tone::New, 206, 3, 209, 0},  // 磁盘
    {Icon::Network, Tone::Nav, 209, 6, 215, 0},  // 网络
    {Icon::Download, Tone::Nav, 215, 3, 218, 0},  // 下载
    {Icon::Cloud, Tone::Nav, 218, 2, 220, 0},  // 云盘
    {Icon::Recycle, Tone::New, 220, 5, 225, 0},  // 回收站
    {Icon::Workspace, Tone::View, 225, 3, 228, 0},  // 工作区
    {Icon::Tray, Tone::Nav, 228, 2, 230, 0},  // 暂存区
    {Icon::Settings, Tone::Nav, 230, 2, 232, 0},  // 设置
    {Icon::Sliders, Tone::Nav, 232, 6, 238, 0},  // 选项
    {Icon::Lock, Tone::Place, 238, 3, 241, 0},  // 锁定
    {Icon::Check, Tone::None, 241, 1, 242, 0},  // 完成
    {Icon::Close, Tone::None, 242, 2, 244, 0},  // 关闭
    {Icon::Sparkle, Tone::Nav, 244, 2, 246, 0},  // Pulse 助手
};

inline const Design* DesignFor(Icon icon) noexcept {
    static constexpr auto kIndex = [] {
        std::array<int16_t, static_cast<size_t>(Icon::Count)> index{};
        for (auto& slot : index) slot = -1;
        for (size_t i = 0; i < std::size(kDesigns); ++i)
            index[static_cast<size_t>(kDesigns[i].icon)] = static_cast<int16_t>(i);
        return index;
    }();
    if (icon <= Icon::None || icon >= Icon::Count) return nullptr;
    const int16_t at = kIndex[static_cast<size_t>(icon)];
    return at < 0 ? nullptr : &kDesigns[at];
}

// The category colour an icon's accent part takes; None = draw it in the ink.
inline Tone ToneOf(Icon icon) noexcept {
    const Design* design = DesignFor(icon);
    if (!design || design->tone == Tone::None) return Tone::None;
    for (uint16_t i = 0; i < design->body_count + design->badge_count; ++i) {
        const uint16_t at = i < design->body_count ? design->body + i : design->badge + (i - design->body_count);
        if (kDesignParts[at].flags & kAccent) return design->tone;
    }
    return Tone::None;
}

// Appends SVG path data to `sink` as figures: M L H V C S A Z in both cases,
// with implicit repeats (the subset the designs use). Stops at anything else,
// always leaving the sink with no open figure.
inline void AppendSvgPath(ID2D1GeometrySink* sink, const char* data, bool filled) noexcept {
    const char* s = data;
    const auto skip = [&] { while (*s == ' ' || *s == ',' || *s == '\t' || *s == '\n' || *s == '\r') ++s; };
    const auto number = [&](float& out) noexcept {
        skip();
        const char* start = s;
        if (*s == '-' || *s == '+') ++s;
        bool digits = false, point = false;
        while ((*s >= '0' && *s <= '9') || (*s == '.' && !point)) {
            if (*s == '.') point = true; else digits = true;
            ++s;
        }
        if (!digits) { s = start; return false; }
        if (*s == 'e' || *s == 'E') {
            const char* exponent = s + 1;
            if (*exponent == '-' || *exponent == '+') ++exponent;
            if (*exponent >= '0' && *exponent <= '9') { s = exponent; while (*s >= '0' && *s <= '9') ++s; }
        }
        const auto parsed = std::from_chars(*start == '+' ? start + 1 : start, s, out);
        return parsed.ec == std::errc{};
    };
    const auto flag = [&](bool& out) noexcept {
        skip();
        if (*s != '0' && *s != '1') return false;
        out = *s++ == '1';
        return true;
    };
    D2D1_POINT_2F current{}, figure_start{}, control{};
    bool open = false, after_curve = false;
    char command = 0;
    const auto begin_at = [&](D2D1_POINT_2F point) {
        if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->BeginFigure(point, filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        open = true;
        figure_start = current = point;
    };
    const auto ensure_open = [&] { if (!open) begin_at(current); };
    for (;;) {
        skip();
        if (!*s) break;
        if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) command = *s++;
        else if (!command) break;   // a number with no command before it
        const bool relative = command >= 'a';
        const char kind = relative ? static_cast<char>(command - 32) : command;
        const float ox = relative ? current.x : 0.0f, oy = relative ? current.y : 0.0f;
        float a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
        bool large = false, sweep = false, curve = false;
        if (kind == 'M') {
            if (!number(a) || !number(b)) break;
            begin_at({ox + a, oy + b});
            command = relative ? 'l' : 'L';   // further pairs are lines
        } else if (kind == 'L') {
            if (!number(a) || !number(b)) break;
            ensure_open();
            current = {ox + a, oy + b};
            sink->AddLine(current);
        } else if (kind == 'H') {
            if (!number(a)) break;
            ensure_open();
            current.x = ox + a;
            sink->AddLine(current);
        } else if (kind == 'V') {
            if (!number(a)) break;
            ensure_open();
            current.y = oy + a;
            sink->AddLine(current);
        } else if (kind == 'C' || kind == 'S') {
            D2D1_POINT_2F first{};
            if (kind == 'C') {
                if (!number(a) || !number(b)) break;
                first = {ox + a, oy + b};
            } else {
                first = after_curve ? D2D1::Point2F(2 * current.x - control.x, 2 * current.y - control.y) : current;
            }
            if (!number(c) || !number(d) || !number(e) || !number(f)) break;
            ensure_open();
            control = {ox + c, oy + d};
            current = {ox + e, oy + f};
            sink->AddBezier(D2D1::BezierSegment(first, control, current));
            curve = true;
        } else if (kind == 'A') {
            float rotation = 0;
            if (!number(a) || !number(b) || !number(rotation) || !flag(large) || !flag(sweep) ||
                !number(e) || !number(f)) break;
            ensure_open();
            current = {ox + e, oy + f};
            sink->AddArc(D2D1::ArcSegment(current, {std::fabs(a), std::fabs(b)}, rotation,
                sweep ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        } else if (kind == 'Z') {
            if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            open = false;
            current = figure_start;
            command = 0;   // a command letter must follow
        } else {
            break;
        }
        after_curve = curve;
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
}

// A design in two colours is drawn in up to six passes (ink, then accent), in
// this order; in one colour, in the three kMono passes, so a translucent icon
// is composited once where its accent part meets the body. Each pass is one
// cached geometry (nullptr when the design has nothing in it).
enum DesignLayer : uint8_t { kInkTint, kInkFill, kInkStroke, kAccentTint, kAccentFill, kAccentStroke,
                             kMonoTint, kMonoFill, kMonoStroke, kDesignLayers };

inline ID2D1PathGeometry* BuildDesignLayer(ID2D1Factory* factory, const DesignPart* parts, uint16_t count,
                                           uint8_t layer) noexcept {
    const bool mono = layer >= kMonoTint;
    const bool accent = layer >= kAccentTint && !mono;
    const uint8_t pass = static_cast<uint8_t>(layer % kAccentTint);   // 0 tint, 1 fill, 2 stroke
    const auto wanted = [&](const DesignPart& part) {
        if (!mono && ((part.flags & kAccent) != 0) != accent) return false;
        const uint8_t kind = (part.flags & kTint) ? 0 : (part.flags & kFill) ? 1 : 2;
        return kind == pass;
    };
    bool any = false;
    for (uint16_t i = 0; i < count; ++i) any |= wanted(parts[i]);
    if (!any) return nullptr;
    ID2D1PathGeometry* geometry = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(factory->CreatePathGeometry(&geometry))) return nullptr;
    if (FAILED(geometry->Open(&sink))) { geometry->Release(); return nullptr; }
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    for (uint16_t i = 0; i < count; ++i)
        if (wanted(parts[i])) AppendSvgPath(sink, parts[i].d, pass != 2);
    const HRESULT result = sink->Close();
    sink->Release();
    if (FAILED(result)) { geometry->Release(); return nullptr; }
    return geometry;
}

// Everything outside the badge circle: the layer mask a badged body is drawn through.
inline ID2D1PathGeometry* BuildBadgeMask(ID2D1Factory* factory) noexcept {
    ID2D1RectangleGeometry* area = nullptr;
    ID2D1EllipseGeometry* hole = nullptr;
    ID2D1PathGeometry* mask = nullptr;
    factory->CreateRectangleGeometry(D2D1::RectF(-2.0f, -2.0f, kCanvas + 2.0f, kCanvas + 2.0f), &area);
    factory->CreateEllipseGeometry(D2D1::Ellipse({kBadgeX, kBadgeY}, kBadgeCut, kBadgeCut), &hole);
    if (area && hole && SUCCEEDED(factory->CreatePathGeometry(&mask))) {
        ID2D1GeometrySink* sink = nullptr;
        HRESULT result = mask->Open(&sink);
        if (SUCCEEDED(result)) {
            result = area->CombineWithGeometry(hole, D2D1_COMBINE_MODE_EXCLUDE, nullptr, 0.05f, sink);
            const HRESULT closed = sink->Close();
            sink->Release();
            if (SUCCEEDED(result)) result = closed;
        }
        if (FAILED(result)) { mask->Release(); mask = nullptr; }
    }
    if (area) area->Release();
    if (hole) hole->Release();
    return mask;
}

// Geometry for one factory, built on first use. Complete contours, so
// translucent strokes are composited once and curves are never tessellated
// into overlapping, round-capped short lines.
struct GeometryCache {
    ID2D1Factory* factory = nullptr;
    std::array<ID2D1PathGeometry*,static_cast<size_t>(Icon::Count)> geometries{};
    struct Layers {
        bool built = false;
        std::array<ID2D1PathGeometry*,kDesignLayers> body{}, badge{};
    };
    std::array<Layers,static_cast<size_t>(Icon::Count)> designs{};
    ID2D1PathGeometry* badge_mask = nullptr;
    bool badge_mask_built = false;
    void Reset() noexcept {
        const auto release = [](ID2D1PathGeometry*& geometry) {
            if (geometry) geometry->Release();
            geometry = nullptr;
        };
        for (auto*& geometry : geometries) release(geometry);
        for (auto& layers : designs) {
            for (auto*& geometry : layers.body) release(geometry);
            for (auto*& geometry : layers.badge) release(geometry);
            layers.built = false;
        }
        release(badge_mask);
        badge_mask_built = false;
        if (factory) factory->Release();
        factory = nullptr;
    }
    ~GeometryCache() { Reset(); }
};

inline GeometryCache* CacheFor(ID2D1RenderTarget* target) noexcept {
    static thread_local GeometryCache cache;
    ID2D1Factory* factory = nullptr;
    target->GetFactory(&factory);
    if (!factory) return nullptr;
    if (cache.factory != factory) {
        cache.Reset();
        cache.factory = factory;
    } else {
        factory->Release();
    }
    return &cache;
}

inline ID2D1PathGeometry* GeometryFor(ID2D1RenderTarget* target, Icon icon) noexcept {
    GeometryCache* cache = CacheFor(target);
    if (!cache) return nullptr;
    auto*& geometry = cache->geometries[static_cast<size_t>(icon)];
    if (!geometry) geometry = IsPinned(icon) ? BuildPinnedGeometry(cache->factory,icon)
                                             : BuildGeometry(cache->factory,icon);
    return geometry;
}

inline const GeometryCache::Layers* DesignLayersFor(ID2D1RenderTarget* target, const Design& design) noexcept {
    GeometryCache* cache = CacheFor(target);
    if (!cache) return nullptr;
    auto& layers = cache->designs[static_cast<size_t>(design.icon)];
    if (!layers.built) {
        layers.built = true;
        for (uint8_t layer = 0; layer < kDesignLayers; ++layer) {
            layers.body[layer] = BuildDesignLayer(cache->factory, kDesignParts + design.body, design.body_count, layer);
            if (design.badge_count)
                layers.badge[layer] = BuildDesignLayer(cache->factory, kDesignParts + design.badge, design.badge_count, layer);
        }
    }
    if (design.badge_count && !cache->badge_mask_built) {
        cache->badge_mask_built = true;
        cache->badge_mask = BuildBadgeMask(cache->factory);
    }
    return &layers;
}

inline bool Draw(ID2D1RenderTarget* target, ID2D1Brush* brush,
                  ID2D1StrokeStyle* stroke, Icon icon, D2D1_RECT_F bounds,
                  float opacity = 1.0f, float width = kStroke) noexcept;

// As Draw, with the icon's accent part (see ToneOf) in `accent`. A null
// accent, or an icon without a design, draws in `brush` alone.
inline bool DrawTwoTone(ID2D1RenderTarget* target, ID2D1Brush* brush, ID2D1Brush* accent,
                        ID2D1StrokeStyle* stroke, Icon icon, D2D1_RECT_F bounds,
                        float opacity = 1.0f, float width = kStroke) noexcept;

// For callers that draw a Segoe glyph as text: its command icon at about the
// size `font_size` draws the glyph, centred in bounds. False = no command
// icon, draw the glyph with the font.
inline bool DrawGlyph(ID2D1RenderTarget* target, ID2D1Brush* brush, std::wstring_view glyph,
                      D2D1_RECT_F bounds, float font_size) noexcept {
    const Icon icon = FromGlyph(glyph);
    if (icon == Icon::None || !target || !brush) return false;
    ID2D1StrokeStyle* stroke = nullptr;
    CreateStrokeStyle(target, &stroke);
    const bool drawn = Draw(target, brush, stroke, icon, CenteredBounds(bounds, font_size * 1.2f));
    if (stroke) stroke->Release();
    return drawn;
}

inline bool DrawTwoTone(ID2D1RenderTarget* target, ID2D1Brush* brush, ID2D1Brush* accent,
                        ID2D1StrokeStyle* stroke, Icon icon, D2D1_RECT_F bounds,
                        float opacity, float width) noexcept {
    if (!target || !brush || icon <= Icon::None || icon >= Icon::Count) return false;
    const float side = (std::min)(bounds.right - bounds.left, bounds.bottom - bounds.top);
    if (!std::isfinite(side) || side <= 0.0f || !std::isfinite(opacity) ||
        !std::isfinite(bounds.left) || !std::isfinite(bounds.top) ||
        !std::isfinite(bounds.right) || !std::isfinite(bounds.bottom)) return true;
    const Design* design = DesignFor(icon);
    const GeometryCache::Layers* layers = design ? DesignLayersFor(target, *design) : nullptr;
    auto* geometry = layers ? nullptr : GeometryFor(target,icon);
    if (!layers && !geometry) return false;
    if (!accent) accent = brush;
    const float alpha = std::clamp(opacity,0.0f,1.0f);
    D2D1_MATRIX_3X2_F previous{};
    target->GetTransform(&previous);
    const float previous_opacity = brush->GetOpacity();
    const float previous_accent = accent->GetOpacity();
    target->SetTransform(D2D1::Matrix3x2F::Scale(side/kCanvas,side/kCanvas) *
        D2D1::Matrix3x2F::Translation((bounds.left+bounds.right-side)*0.5f,
                                     (bounds.top+bounds.bottom-side)*0.5f) * previous);
    const float line = width > 0.0f ? width : kStroke;
    if (layers) {
        const bool two_tone = accent != brush;
        const auto pass = [&](const std::array<ID2D1PathGeometry*,kDesignLayers>& set) {
            for (uint8_t layer = two_tone ? kInkTint : kMonoTint;
                 layer < (two_tone ? kMonoTint : kDesignLayers); ++layer) {
                ID2D1PathGeometry* part = set[layer];
                if (!part) continue;
                const bool in_accent = two_tone && layer >= kAccentTint;
                ID2D1Brush* paint = in_accent ? accent : brush;
                const float base = in_accent ? previous_accent : previous_opacity;
                const uint8_t kind = static_cast<uint8_t>(layer % kAccentTint);
                const float tint = in_accent ? 0.5f : 0.35f;
                paint->SetOpacity(base * alpha * (kind == 0 ? tint : 1.0f));
                if (kind == 2) target->DrawGeometry(part,paint,line,stroke);
                else target->FillGeometry(part,paint);
            }
        };
        if (design->badge_count) {
            GeometryCache* cache = CacheFor(target);
            ID2D1PathGeometry* mask = cache ? cache->badge_mask : nullptr;
            ID2D1Layer* clip = nullptr;
            ID2D1DeviceContext* context = nullptr;
            // Direct2D 1.1 targets take a null layer; plain render targets need one.
            if (mask && FAILED(target->QueryInterface(__uuidof(ID2D1DeviceContext), reinterpret_cast<void**>(&context))))
                target->CreateLayer(nullptr, &clip);
            const bool masked = mask && (context || clip);
            if (masked) target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), mask,
                                                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), clip);
            pass(layers->body);
            if (masked) target->PopLayer();
            if (clip) clip->Release();
            if (context) context->Release();
            pass(layers->badge);
        } else {
            pass(layers->body);
        }
    } else if (IsPinned(icon)) {
        brush->SetOpacity(previous_opacity * alpha);
        target->FillGeometry(geometry,brush);   // already the stroked outline
    } else {
        if (const float fill = FillOpacity(icon); fill > 0.0f) {
            brush->SetOpacity(previous_opacity * alpha * fill);
            target->FillGeometry(geometry,brush);
        }
        brush->SetOpacity(previous_opacity * alpha);
        if (icon != Icon::More) target->DrawGeometry(geometry,brush,line,stroke);
    }
    target->SetTransform(previous);
    brush->SetOpacity(previous_opacity);
    accent->SetOpacity(previous_accent);
    return true;
}

inline bool Draw(ID2D1RenderTarget* target, ID2D1Brush* brush,
                  ID2D1StrokeStyle* stroke, Icon icon, D2D1_RECT_F bounds,
                  float opacity, float width) noexcept {
    return DrawTwoTone(target, brush, nullptr, stroke, icon, bounds, opacity, width);
}

} // namespace pulse::ui::command_icons

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
    Dialog, User, Exit,
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
    if (!geometry) geometry = IsPinned(icon) ? BuildPinnedGeometry(cache.factory,icon)
                                             : BuildGeometry(cache.factory,icon);
    return geometry;
}

inline bool Draw(ID2D1RenderTarget* target, ID2D1Brush* brush,
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

inline bool Draw(ID2D1RenderTarget* target, ID2D1Brush* brush,
                  ID2D1StrokeStyle* stroke, Icon icon, D2D1_RECT_F bounds,
                  float opacity, float width) noexcept {
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
    if (IsPinned(icon)) {
        target->FillGeometry(geometry,brush);   // already the stroked outline
    } else {
        if (const float fill = FillOpacity(icon); fill > 0.0f) {
            brush->SetOpacity(previous_opacity * std::clamp(opacity,0.0f,1.0f) * fill);
            target->FillGeometry(geometry,brush);
            brush->SetOpacity(previous_opacity * std::clamp(opacity,0.0f,1.0f));
        }
        if (icon != Icon::More) target->DrawGeometry(geometry,brush,width > 0.0f ? width : kStroke,stroke);
    }
    target->SetTransform(previous);
    brush->SetOpacity(previous_opacity);
    return true;
}

} // namespace pulse::ui::command_icons

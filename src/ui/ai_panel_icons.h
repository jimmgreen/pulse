// ai_panel_icons.h — Line icons of the Pulse 助手 panel.
//
// The design's 24x24 path data, drawn with D2D
// path geometries (1.6 stroke, round caps and joins) so the panel matches the
// approved design. Geometries are built once per D2D factory.
#pragma once
#include <d2d1_1.h>
#include <cstdint>

namespace pulse::ui {

enum class AiIcon : uint8_t {
    Spark,
    Folder,
    FolderFilled,
    File,
    Close,
    Plus,
    Dock,
    Float,
    ChevronDown,
    ChevronRight,
    Send,
    Stop,
    Retry,
    Check,
    Undo,
    Open,
    Clock,
    At,
    Shield,
    Thumb,
    Copy,
    Eye,
    Window,
    Rows,
    Move,
    Image,
    Video,
    Document,
    Download,
    Size,
    Duplicate,
    Camera,
    Arrow,
    Tag,
    FolderPlus,
    Search,
    ChevronLeft,
    CheckCircle,
    AlertCircle,
    EyeOff,
    // Error cards.
    Key,
    Wallet,
    Cloud,
    Ban,
    WifiOff,
    Plug,
    Gear,
    Swap,
    Warn,
    // First batch of extensions.
    Bolt,
    Package,
    Rename,
    Tasks,
    Lock,
    Skip,
    Sliders,
    // Second batch.
    Hdd,
    Install,
    Temp,
    Trash,
    // Third batch.
    Split,
    NewWindow,
    Desktop,
    Tray,
    Star,
    Keyboard,
    Tiles,
    Mic,
    Monitor,
    Rocket,
    WinSettings,
    // P4.
    Pin,
    Palette,
    Git,
    // P4b: Git push / pull, eject.
    Push,
    Pull,
    Eject,
    // 55: the selection toolbar's 引用.
    Quote,
    // 55 P2: 没帮助, 编辑, the edit box's note, 重新生成's menu.
    ThumbDown,
    Edit,
    Info,
    Detail,
    Short,
    Model,
    // 55 P3: 历史对话 and its rows.
    History,
    Chat,
    Count
};

struct AiIconPose {
    float opacity = 1.0f;
    float rotate_deg = 0.0f;   // about the box centre
    float scale = 1.0f;        // about the box centre
};

// Draws the icon fitted to the square box (its left/top and width) in color.
// Filled parts with a fixed color in the design (the yellow folder) keep it.
void DrawAiIcon(ID2D1DeviceContext* dc, AiIcon icon, const D2D1_RECT_F& box, D2D1_COLOR_F color,
                const AiIconPose& pose = {});

// Drops the cached geometries (device or factory change).
void ReleaseAiIcons();

} // namespace pulse::ui

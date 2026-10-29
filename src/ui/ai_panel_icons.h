// ai_panel_icons.h — Line icons of the Pulse 助手 panel.
//
// The 24x24 path data of plans/ai/pulse_ai_assistant_v2.html, drawn with D2D
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
    // Error cards (plans/ai/Pulse_AI助手_错误与快捷键.html).
    Key,
    Wallet,
    Cloud,
    Ban,
    WifiOff,
    Plug,
    Gear,
    Swap,
    Warn,
    // First batch of extensions (plans/ai/Pulse_AI助手_第一批扩展.html).
    Bolt,
    Package,
    Rename,
    Tasks,
    Lock,
    Skip,
    Sliders,
    // Second batch (plans/ai/Pulse_AI助手_第二批扩展.html).
    Hdd,
    Install,
    Temp,
    Trash,
    // Third batch (plans/ai/Pulse_AI助手_第三批_操控Pulse与语音.html).
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
    // P4 (plans/ai/P4_全部领域接入.html).
    Pin,
    Palette,
    Git,
    // P4b: Git push / pull, eject.
    Push,
    Pull,
    Eject,
    // 55: the selection toolbar's 引用.
    Quote,
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

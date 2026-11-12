#pragma once
#include <d2d1_1.h>
#include <cstdint>
#include <string_view>
namespace pulse::ui {
struct Theme;
enum class StoreIcon { None, Search, Box, Extract, ExtractTo, Compress, AddArchive, Test, Hash, Gear, Folder, File, ChevronRight, ChevronDown, ChevronUp, More, Check, Shield, Badge, Lock, Eye, Copy, Cut, Rename, Trash, Properties, Open, OpenWith, Path, Undo, Close, Plus, Minus, Square, Back, Forward, Up, Refresh, Image, Camera, Film, TextSearch, Sparkle, Puzzle, Palette, ListView, Tabs, Power, Duplicate, Cube, Lines, Keyboard, Info, Warn, Sidebar, Panes, Contrast, Download, CloudOff, Drag, Sort, Filter, NewFolder, Star, Pc, Disk, ChevronLeft, Key, Count };
static_assert(static_cast<uint32_t>(StoreIcon::AddArchive) == 6 &&
              static_cast<uint32_t>(StoreIcon::Key) == 67 &&
              static_cast<uint32_t>(StoreIcon::Count) == 68);
inline constexpr uint32_t kCustomExtensionIconMin = 0x10000u;
inline constexpr bool IsCustomExtensionIcon(uint32_t icon) noexcept {
    return icon >= kCustomExtensionIconMin;
}
inline constexpr StoreIcon StoreIconFromId(uint32_t icon) noexcept {
    switch (icon) {
    case 0: return StoreIcon::File;
    case 1: return StoreIcon::Compress;
    case 2: return StoreIcon::Extract;
    case 3: return StoreIcon::Search;
    case 4: return StoreIcon::Box;
    case 5: return StoreIcon::ExtractTo;
    default:
        return icon >= 6 && icon < static_cast<uint32_t>(StoreIcon::Count)
            ? static_cast<StoreIcon>(icon)
            : StoreIcon::File;
    }
}
inline constexpr uint32_t StoreIconToId(StoreIcon icon) noexcept {
    switch (icon) {
    case StoreIcon::None: return 0;
    case StoreIcon::Compress: return 1;
    case StoreIcon::Extract: return 2;
    case StoreIcon::Search: return 3;
    case StoreIcon::Box: return 4;
    case StoreIcon::ExtractTo: return 5;
    default: {
        const auto v = static_cast<uint32_t>(icon);
        return v >= 6 && v < static_cast<uint32_t>(StoreIcon::Count) ? v : 11u;
    }
    }
}
inline StoreIcon StoreCommandIcon(uint32_t icon) {
    return StoreIconFromId(icon);
}
// A plugin command's icon: legacy 7-Zip commands (icon <= 2) keep their dedicated
// built-in drawings; all built-in PulseIconId values (0..67) map directly via StoreIconFromId.
inline StoreIcon StoreCommandIconFor(uint32_t icon, std::string_view identity) {
    if (icon <= 2 && identity.starts_with("native:pulse.7zip:")) {
        const auto id = identity.substr(18);
        if (id == "extract-to") return StoreIcon::ExtractTo;
        if (id == "test-archive") return StoreIcon::Test;
        if (id == "add-to-archive") return StoreIcon::AddArchive;
        if (id == "checksums") return StoreIcon::Hash;
    }
    return StoreIconFromId(icon);
}
// Product artwork for store tiles (archive_ux_mockup.html): a coloured gradient tile with
// white two-tone artwork. Generic draws `icon` in white on the tile colour instead.
enum class ProductArt { Generic, SevenZip, Images, Raw, Media, Content, Ai };
ProductArt ProductArtFor(StoreIcon icon);
void DrawProductTile(ID2D1DeviceContext* dc, D2D1_RECT_F bounds, ProductArt art,
                     D2D1_COLOR_F generic_color, StoreIcon generic_icon);
void DrawStoreIcon(ID2D1DeviceContext* dc, ID2D1Brush* brush, D2D1_RECT_F bounds,
                   StoreIcon icon, float stroke_dip = 1.35f);
void DrawStoreIcon(ID2D1DeviceContext* dc, StoreIcon icon, D2D1_RECT_F bounds,
                   D2D1_COLOR_F color, float stroke_dip = 1.35f);
bool DrawExtensionIcon(ID2D1DeviceContext* dc, uint32_t icon_id, D2D1_RECT_F bounds,
                       D2D1_COLOR_F foreground, const Theme* theme = nullptr,
                       float stroke_dip = 1.35f);
bool DrawExtensionIconGlyph(ID2D1DeviceContext* dc, std::wstring_view glyph, D2D1_RECT_F bounds,
                            D2D1_COLOR_F foreground, const Theme* theme = nullptr,
                            float stroke_dip = 1.35f);
}

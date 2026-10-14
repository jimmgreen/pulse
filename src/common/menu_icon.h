#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse {

// Owned, top-down premultiplied BGRA pixels; safe to cache across Shell sessions.
struct MenuIcon {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bgra;

    bool Valid() const {
        return width > 0 && height > 0 && width <= 64 && height <= 64 &&
            bgra.size() == static_cast<size_t>(width) * height * 4;
    }
    bool operator==(const MenuIcon&) const = default;
};

MenuIcon LoadMenuIcon(const std::wstring& location);
MenuIcon ReadMenuBitmap(HBITMAP bitmap);
HBITMAP MenuIconBitmap(const MenuIcon& icon);

// Reconstruct alpha from GDI drawing over black and white backgrounds.
MenuIcon MenuIconFromBackgrounds(uint32_t width, uint32_t height,
                                 const uint8_t* black, const uint8_t* white);

} // namespace pulse

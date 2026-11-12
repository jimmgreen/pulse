// menu_icon.h — bitmap icon of one context-menu row, shared by pulse_shell's
// answer (Explorer handler bitmaps), the ops/ipc layers and the menu renderer.
//
// Pixels are 32bpp premultiplied BGRA, top-down, width * 4 bytes per row:
// what WIC's 32bppPBGRA converter and a D2D B8G8R8A8 premultiplied bitmap
// both use, so no layer converts twice. Rows share one buffer through
// MenuIconRef; the renderer keys its device bitmaps by that pointer.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct HICON__;  // <windows.h> HICON, without including it here

namespace pulse::common {

constexpr uint32_t kMaxMenuIconEdge = 64;  // 16 DIP at up to 400% scale

struct MenuIconPixels {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bgra;

    bool Valid() const noexcept {
        return width > 0 && height > 0 && width <= kMaxMenuIconEdge &&
               height <= kMaxMenuIconEdge &&
               bgra.size() == static_cast<size_t>(width) * height * 4u;
    }
};

using MenuIconRef = std::shared_ptr<const MenuIconPixels>;

// menu_icon.cpp (link it where used). HICON -> premultiplied BGRA at the icon's
// own size: alpha icons keep their alpha, classic ones take the AND mask.
MenuIconRef IconPixels(HICON__* icon);
// "file[,index]" (environment strings expanded) -> file and icon index.
bool ParseIconReference(const std::wstring& reference, std::wstring& file, int& index);
// The icon of "file[,index]" extracted at exactly `edge` pixels (1..64), so it is
// drawn without resampling a small icon. Reads the file: worker threads only.
MenuIconRef LoadIconPixels(const std::wstring& reference, uint32_t edge);

} // namespace pulse::common

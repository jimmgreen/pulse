// dock_window_preview.h - 窗口预览: resting the pointer on a running app in the Dock shows a live DWM
// thumbnail of each of its windows above the icon (beside it for a side Dock), as the Windows taskbar
// does. Clicking a thumbnail brings that window forward (restoring it when minimised); its × button or
// a middle click closes it. The thumbnails are DWM's own live copies, so video and typing keep moving.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::desktop::overlays {

// ---------------------------------------------------------------------------------------------
// Pure layout (unit tested). Everything is screen px; `scale` is px per DIP.
// ---------------------------------------------------------------------------------------------
struct DockPreviewLayout {
    RECT panel{};                 // the whole preview window
    std::vector<RECT> cards;      // one per shown window (hover / click area)
    std::vector<RECT> thumbs;     // where the live thumbnail goes, aspect kept, centred in its box
    std::vector<RECT> titles;     // the title line (after a small app icon)
    std::vector<RECT> closes;     // the × button
    size_t shown = 0;             // windows that fit (the first `shown` of the list)
};

constexpr float kDockPreviewThumbW = 208.0f;   // thumbnail box, DIP
constexpr float kDockPreviewThumbH = 128.0f;
constexpr float kDockPreviewMinFactor = 0.62f; // boxes shrink to this before windows are left out

// `source_sizes`: each window's size (only the aspect is used; 0×0 = unknown, laid out as 16:10).
// `dock_edge`: 0 bottom (a row above the icon), 1 left / 2 right (a column beside it).
DockPreviewLayout LayoutDockWindowPreview(const std::vector<SIZE>& source_sizes, const RECT& icon_screen,
                                          const RECT& work_screen, float scale, uint8_t dock_edge);

// When the preview opens and closes (unit tested). The host feeds it every pointer move and a 50 ms
// tick: `target` is the key of the running app under the pointer ("" for none), `over_preview` whether
// the pointer is on the open preview.
class DockPreviewHover {
public:
    enum class Action : uint8_t { None, Show, Close };
    static constexpr uint64_t kEnterMs = 450;   // rest this long on an icon before the preview opens
    static constexpr uint64_t kLeaveMs = 320;   // grace to travel between the icon and the preview

    Action Update(std::wstring_view target, bool over_preview, uint64_t now);
    // The host closed it (a click, a drag, a menu): it stays shut until the pointer leaves `key`.
    void Dismissed(std::wstring_view key);
    const std::wstring& Shown() const noexcept { return shown_; }
    bool NeedsTick() const noexcept { return !candidate_.empty() || !shown_.empty(); }
    void Reset() noexcept { shown_.clear(); candidate_.clear(); suppressed_.clear(); enter_at_ = leave_at_ = 0; }

private:
    std::wstring shown_, candidate_, suppressed_;
    uint64_t enter_at_ = 0, leave_at_ = 0;
};

struct DockWindowPreviewRequest {
    std::wstring key;              // the Dock item's key
    std::vector<HWND> windows;     // its live top-level windows, in Dock order
    RECT icon_screen{};            // the Dock icon, screen px
    RECT work_screen{};            // the monitor work area, screen px
    float scale = 1.0f;            // px per DIP
    uint8_t dock_edge = 0;         // 0 bottom, 1 left, 2 right
    bool dark = true;
    uint32_t accent_argb = 0xFF3B82F6u;
    bool headless = false;         // lay out only; never shown (tests)
};

class DockWindowPreview {
public:
    using WindowFn = std::function<void(HWND window)>;

    DockWindowPreview();
    ~DockWindowPreview();
    DockWindowPreview(const DockWindowPreview&) = delete;
    DockWindowPreview& operator=(const DockWindowPreview&) = delete;

    void SetActivateHandler(WindowFn fn);   // a thumbnail was clicked
    void SetCloseHandler(WindowFn fn);      // its × (or a middle click) asks the window to close
    bool Show(const DockWindowPreviewRequest& request);
    void Close() noexcept;
    bool IsOpen() const noexcept;
    bool IsOpenFor(std::wstring_view key) const noexcept;
    bool ContainsScreenPoint(POINT pt) const noexcept;
    HWND Hwnd() const noexcept;
    const DockPreviewLayout& Layout() const noexcept;
    const std::vector<HWND>& Windows() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulse::desktop::overlays

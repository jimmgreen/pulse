// Mixed-scale placement: a 100% and a 150% monitor side by side (the user
// report: screen 2 at 150%, screen 1 at 100% - "只要两个缩放不一样，就出现窗口
// 显示不全"). No second monitor is needed: monitors, the DPI a window gets
// from the screen it sits on, and the WM_DPICHANGED suggested rect Windows
// sends (current pixels rescaled by new/old DPI) are simulated, while the
// placement math is the production code in src/ui/dialog_dpi_placement.h.
#include "../ui/dialog_dpi_placement.h"

#include <cstdio>
#include <string>
#include <vector>

namespace pl = pulse::ui::placement;

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const std::string& name) {
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL %s\n", name.c_str()); }
}

struct Monitor {
    RECT bounds;
    RECT work;
    UINT dpi;
};

struct Desktop {
    std::vector<Monitor> monitors;  // [0] is primary: its bounds start at (0,0)

    const Monitor& FromRect(const RECT& r) const {
        // MonitorFromRect(MONITOR_DEFAULTTONEAREST): largest intersection,
        // else the nearest one.
        const Monitor* best = &monitors[0];
        long best_area = -1;
        for (const auto& m : monitors) {
            const long w = (std::min)(r.right, m.bounds.right) - (std::max)(r.left, m.bounds.left);
            const long h = (std::min)(r.bottom, m.bounds.bottom) - (std::max)(r.top, m.bounds.top);
            const long area = w > 0 && h > 0 ? w * h : 0;
            if (area > best_area) { best_area = area; best = &m; }
        }
        if (best_area > 0) return *best;
        long best_distance = 0x7fffffff;
        for (const auto& m : monitors) {
            const long dx = r.right < m.bounds.left ? m.bounds.left - r.right
                          : r.left > m.bounds.right ? r.left - m.bounds.right : 0;
            const long dy = r.bottom < m.bounds.top ? m.bounds.top - r.bottom
                          : r.top > m.bounds.bottom ? r.top - m.bounds.bottom : 0;
            if (dx + dy < best_distance) { best_distance = dx + dy; best = &m; }
        }
        return *best;
    }
};

// Screen 2: 2560x1440 at 150%, left and taller; screen 1: 1920x1080 at 100%,
// right and lower (the user's layout). `primary` picks which one sits at (0,0).
Desktop UserDesktop(int primary) {
    const LONG taskbar = 48;
    Monitor screen2{ {0, 0, 2560, 1440}, {0, 0, 2560, 1440 - taskbar * 3 / 2}, 144 };
    Monitor screen1{ {2560, 360, 2560 + 1920, 360 + 1080}, {2560, 360, 2560 + 1920, 360 + 1080 - taskbar}, 96 };
    if (primary == 1) {
        auto shift = [](RECT& r, LONG dx, LONG dy) { r.left -= dx; r.right -= dx; r.top -= dy; r.bottom -= dy; };
        for (RECT* r : { &screen2.bounds, &screen2.work, &screen1.bounds, &screen1.work }) shift(*r, 2560, 360);
        return Desktop{ { screen1, screen2 } };
    }
    return Desktop{ { screen2, screen1 } };
}

enum class Handler { SuggestedRect, DipLayout };

// A per-monitor-v2 dialog: pixels + the scale its WM_DPICHANGED handler holds.
struct SimWindow {
    const Desktop* desktop = nullptr;
    RECT rect{};
    UINT dpi = 96;        // the DPI Windows assigns (GetDpiForWindow)
    float scale = 1.0f;   // the dialog's scale_
    float width_dip = 0, height_dip = 0;
    Handler handler = Handler::DipLayout;
    bool delivers_dpi_changed = true;  // false: hidden window, no message

    void OnDpiChanged(UINT new_dpi, const RECT& suggested) {
        scale = static_cast<float>(new_dpi) / 96.0f;
        rect = handler == Handler::SuggestedRect
            ? suggested : pl::DpiChangedRect(suggested, width_dip, height_dip, new_dpi);
        dpi = desktop->FromRect(rect).dpi;  // the handler's move can hop again
    }
    void SetWindowPos(const RECT& next) {
        rect = next;
        const UINT new_dpi = desktop->FromRect(rect).dpi;
        if (new_dpi == dpi) return;
        const UINT old_dpi = dpi;
        dpi = new_dpi;
        if (!delivers_dpi_changed) return;
        const LONG w = MulDiv(rect.right - rect.left, new_dpi, old_dpi);
        const LONG h = MulDiv(rect.bottom - rect.top, new_dpi, old_dpi);
        OnDpiChanged(new_dpi, RECT{ rect.left, rect.top, rect.left + w, rect.top + h });
    }
    int Width() const { return rect.right - rect.left; }
    int Height() const { return rect.bottom - rect.top; }
};

RECT OwnedRect(const Desktop& d, const RECT& owner, int w, int h) {
    const Monitor& m = d.FromRect(owner);
    return pl::CenteredRect(owner, &m.work, w, h);
}

void CenterOwned(SimWindow& win, const RECT& owner, int w, int h) {
    win.SetWindowPos(OwnedRect(*win.desktop, owner, w, h));
}

// Pre-fix Create(): size from the owner's DPI, born at CW_USEDEFAULT - (0,0)
// on the primary monitor - then CenterOwnedWindow at that pixel size.
SimWindow OldFlow(const Desktop& d, const RECT& owner, float wdip, float hdip) {
    SimWindow win;
    win.desktop = &d;
    win.width_dip = wdip; win.height_dip = hdip;
    win.handler = Handler::SuggestedRect;
    const UINT owner_dpi = d.FromRect(owner).dpi;
    const int w = pl::DipToPx(wdip, owner_dpi), h = pl::DipToPx(hdip, owner_dpi);
    win.rect = RECT{ 0, 0, w, h };
    win.dpi = d.FromRect(win.rect).dpi;          // WM_CREATE
    win.scale = static_cast<float>(win.dpi) / 96.0f;
    CenterOwned(win, owner, w, h);
    return win;
}

// Fixed Create(): born at OwnedWindowOrigin, CenterOwnedWindowDip, then the
// SettleWindowDpi fallback for a window that saw no WM_DPICHANGED.
void Place(SimWindow& win, const RECT& owner, float wdip, float hdip);

SimWindow NewFlow(const Desktop& d, const RECT& owner, float wdip, float hdip, bool delivers) {
    SimWindow win;
    win.desktop = &d;
    win.width_dip = wdip; win.height_dip = hdip;
    win.handler = Handler::DipLayout;
    win.delivers_dpi_changed = delivers;
    const UINT owner_dpi = d.FromRect(owner).dpi;
    const int w = pl::DipToPx(wdip, owner_dpi), h = pl::DipToPx(hdip, owner_dpi);
    const RECT born = OwnedRect(d, owner, w, h);
    win.rect = born;
    win.dpi = d.FromRect(win.rect).dpi;          // WM_CREATE
    win.scale = static_cast<float>(win.dpi) / 96.0f;
    Place(win, owner, wdip, hdip);
    return win;
}

// CenterOwnedWindowDip + SettleWindowDpi, as Create() and the reused
// transfer window's Show() run them.
void Place(SimWindow& win, const RECT& owner, float wdip, float hdip) {
    const UINT dpi = pl::SettleDipSize(win.dpi, wdip, hdip,
        [&](int pw, int ph) { CenterOwned(win, owner, pw, ph); },
        [&] { return pl::Placed{ win.dpi, win.Width(), win.Height() }; });
    if (pl::NeedsDpiSync(win.scale, dpi)) {
        win.OnDpiChanged(dpi, win.rect);
        CenterOwned(win, owner, win.Width(), win.Height());
    }
}

bool Inside(const RECT& r, const RECT& work) {
    return r.left >= work.left && r.top >= work.top && r.right <= work.right && r.bottom <= work.bottom;
}

RECT OwnerOn(const Monitor& m) {
    // A file-manager window in the middle of the monitor's work area.
    const LONG w = (m.work.right - m.work.left) * 3 / 4, h = (m.work.bottom - m.work.top) * 3 / 4;
    const LONG x = m.work.left + (m.work.right - m.work.left - w) / 2;
    const LONG y = m.work.top + (m.work.bottom - m.work.top - h) / 2;
    return RECT{ x, y, x + w, y + h };
}

struct Dialog { const char* name; float w, h; };

}  // namespace

int main() {
    // The four file-operation dialogs (conflict 500x498, delete/locked 440-ish,
    // transfer 440 wide) plus a large one (batch rename) and a small confirm.
    const Dialog dialogs[] = {
        { "conflict", 500.0f, 498.0f }, { "delete", 460.0f, 360.0f }, { "locked", 460.0f, 300.0f },
        { "transfer", 440.0f, 228.0f }, { "batch_rename", 980.0f, 640.0f }, { "confirm", 420.0f, 200.0f },
    };

    // 1. The user's screenshot, reproduced: primary = screen 2 (150%), Pulse on
    //    screen 1 (100%). The old flow ends at 333x332 px drawn at 100%: the
    //    500x498 layout is cut off on the right and at the bottom.
    {
        const Desktop d = UserDesktop(2);
        const SimWindow win = OldFlow(d, OwnerOn(d.monitors[1]), 500.0f, 498.0f);
        Check(win.dpi == 96 && win.Width() == 333 && win.Height() == 332,
              "old flow reproduces the clipped conflict dialog (333x332 at 100%)");
        Check(win.Width() < pl::DipToPx(500.0f, win.dpi), "old flow: narrower than its layout");
    }
    // 2. The mirror case: primary = screen 1 (100%), Pulse on screen 2 (150%).
    //    Old flow: 1125x1120 px at 150%, taller than the screen's work area.
    {
        const Desktop d = UserDesktop(1);
        const SimWindow win = OldFlow(d, OwnerOn(d.monitors[1]), 500.0f, 498.0f);
        Check(win.dpi == 144 && win.Width() == 1125 && win.Height() == MulDiv(747, 144, 96),
              "old flow oversizes the dialog on the 150% screen (1125 px wide, layout needs 750)");
        Check(win.Width() > pl::DipToPx(500.0f, win.dpi), "old flow: wider than its layout");
    }
    // 3. The fix, every combination: either screen primary, Pulse on either
    //    screen, with and without WM_DPICHANGED reaching the window.
    for (int primary : { 1, 2 }) {
        const Desktop d = UserDesktop(primary);
        for (size_t owner_index = 0; owner_index < d.monitors.size(); ++owner_index) {
            const Monitor& target = d.monitors[owner_index];
            const RECT owner = OwnerOn(target);
            for (const Dialog& dialog : dialogs) {
                for (bool delivers : { true, false }) {
                    const SimWindow win = NewFlow(d, owner, dialog.w, dialog.h, delivers);
                    const std::string tag = std::string(dialog.name) + " primary=" + std::to_string(primary) +
                        " owner@" + std::to_string(target.dpi) + (delivers ? "" : " no-dpichanged");
                    Check(win.dpi == target.dpi, tag + ": lands on the owner's screen");
                    Check(win.scale * 96.0f == static_cast<float>(win.dpi), tag + ": scale matches the screen");
                    Check(win.Width() == pl::DipToPx(dialog.w, win.dpi) &&
                          win.Height() == pl::DipToPx(dialog.h, win.dpi), tag + ": size fits its layout");
                    Check(Inside(win.rect, target.work), tag + ": inside the work area");
                }
            }
        }
    }
    // 4. Dragging a placed dialog across the seam either way keeps the layout.
    for (int primary : { 1, 2 }) {
        const Desktop d = UserDesktop(primary);
        SimWindow win = NewFlow(d, OwnerOn(d.monitors[0]), 500.0f, 498.0f, true);
        const Monitor& other = d.monitors[1];
        const LONG x = other.work.left + 100, y = other.work.top + 100;
        win.SetWindowPos(RECT{ x, y, x + win.Width(), y + win.Height() });
        Check(win.dpi == other.dpi && win.Width() == pl::DipToPx(500.0f, other.dpi) &&
              win.Height() == pl::DipToPx(498.0f, other.dpi), "drag to the other screen keeps 500x498 DIP");
        const Monitor& back = d.monitors[0];
        win.SetWindowPos(RECT{ back.work.left + 50, back.work.top + 50,
                               back.work.left + 50 + win.Width(), back.work.top + 50 + win.Height() });
        Check(win.dpi == back.dpi && win.Width() == pl::DipToPx(500.0f, back.dpi), "drag back keeps 500 DIP");
    }
    // 5. The transfer window is created once and reused: Pulse moves to the
    //    other screen before the next copy, the hidden window is re-placed.
    for (int primary : { 1, 2 }) {
        for (bool delivers : { true, false }) {
            const Desktop d = UserDesktop(primary);
            SimWindow win = NewFlow(d, OwnerOn(d.monitors[0]), 440.0f, 228.0f, delivers);
            const Monitor& moved = d.monitors[1];
            Place(win, OwnerOn(moved), 440.0f, static_cast<float>(win.Height()) / win.scale);
            const std::string tag = std::string("reused transfer primary=") + std::to_string(primary) +
                (delivers ? "" : " no-dpichanged");
            Check(win.dpi == moved.dpi && win.scale * 96.0f == static_cast<float>(moved.dpi), tag + ": scale follows");
            Check(win.Width() == pl::DipToPx(440.0f, moved.dpi) && win.Height() == pl::DipToPx(228.0f, moved.dpi),
                  tag + ": size follows");
            Check(Inside(win.rect, moved.work), tag + ": inside the work area");
        }
    }
    // 6. A window whose pixels were sized for the wrong DPI (the pre-fix state)
    //    heals on its next DPI change instead of carrying the mismatch.
    {
        const RECT suggested{ 10, 20, 10 + 333, 20 + 332 };
        const RECT next = pl::DpiChangedRect(suggested, 500.0f, 498.0f, 144);
        Check(next.left == 10 && next.top == 20 && next.right - next.left == 750 && next.bottom - next.top == 747,
              "DpiChangedRect sizes from DIP, keeps the suggested origin");
    }
    // 7. Pure helpers.
    Check(pl::DipToPx(500.0f, 96) == 500 && pl::DipToPx(500.0f, 144) == 750 && pl::DipToPx(498.0f, 120) == 622,
          "DipToPx");
    Check(pl::DipToPx(500.0f, 0) == 500, "DipToPx treats DPI 0 as 96");
    Check(!pl::NeedsDpiSync(1.5f, 144) && pl::NeedsDpiSync(1.0f, 144) && !pl::NeedsDpiSync(1.25f, 0),
          "NeedsDpiSync");
    {
        const RECT anchor{ 0, 0, 1000, 800 }, work{ 0, 0, 1000, 800 };
        const RECT r = pl::CenteredRect(anchor, &work, 400, 300);
        Check(r.left == 300 && r.top == 250 && r.right == 700 && r.bottom == 550, "CenteredRect centres");
        const RECT big = pl::CenteredRect(anchor, &work, 1200, 300);
        Check(big.left == 0, "CenteredRect keeps an oversized window at the work-area edge");
        const RECT loose = pl::CenteredRect(RECT{ -50, 0, 50, 100 }, nullptr, 400, 100);
        Check(loose.left == -200, "CenteredRect without clamping");
    }

    std::printf("dialog dpi placement: %d PASS / %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

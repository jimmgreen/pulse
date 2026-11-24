#pragma once

#include <optional>
#include <windows.h>

namespace pulse::ui {

// Some standard scrollbar owners handle the final position but ignore live
// tracking. Only bridge a real thumb drag whose committed position is stalled.
class PreviewThumbTracking {
public:
    struct Lag {
        int first_track = 0;
        int last_position = 0;
        unsigned pending_samples = 0;
        bool moved = false;
        std::optional<int> awaiting;

        std::optional<int> Observe(int position, int track) {
            if (track != first_track) moved = true;
            if (position != last_position || position == track) pending_samples = 0;
            last_position = position;
            if (awaiting && position == *awaiting) awaiting.reset();
            if (!moved || position == track || awaiting) return {};
            if (++pending_samples < 2) return {};
            pending_samples = 0;
            awaiting = track; // Do not flood a provider that cannot acknowledge it.
            return track;
        }
    };

    bool Begin(HWND host, HWND target, POINT point) {
        *this = {};
        if (!host || !target || (target != host && !IsChild(host, target))) return false;
        wchar_t class_name[32]{};
        RealGetWindowClassW(target, class_name, ARRAYSIZE(class_name));
        if (lstrcmpiW(class_name, L"ScrollBar") == 0) {
            const bool vertical = (GetWindowLongPtrW(target, GWL_STYLE) & SBS_VERT) != 0;
            return Open(host, target, GetParent(target), SB_CTL, OBJID_CLIENT, vertical, point);
        }
        const auto style = GetWindowLongPtrW(target, GWL_STYLE);
        return ((style & WS_VSCROLL) && Open(host, target, target, SB_VERT, OBJID_VSCROLL, true, point)) ||
               ((style & WS_HSCROLL) && Open(host, target, target, SB_HORZ, OBJID_HSCROLL, false, point));
    }

    bool Tick() {
        if (!host_ || (rail_ != host_ && !IsChild(host_, rail_)) ||
            (receiver_ != host_ && !IsChild(host_, receiver_))) return false;
        GUITHREADINFO gui{sizeof(gui)};
        if (GetWindowThreadProcessId(rail_, nullptr) != thread_ ||
            !GetGUIThreadInfo(thread_, &gui)) return false;
        if (gui.hwndCapture != rail_) return !captured_ && ++waiting_ < 4;
        captured_ = true;

        SCROLLINFO sample{sizeof(sample), SIF_POS | SIF_TRACKPOS | SIF_RANGE | SIF_PAGE};
        if (!Read(rail_, bar_, sample)) return false;
        if (const auto request = lag_.Observe(sample.nPos, sample.nTrackPos)) {
            return PostMessageW(receiver_, vertical_ ? WM_VSCROLL : WM_HSCROLL,
                MAKEWPARAM(SB_THUMBPOSITION, *request),
                bar_ == SB_CTL ? reinterpret_cast<LPARAM>(rail_) : 0) != FALSE;
        }
        return true;
    }

private:
    static bool Read(HWND rail, int bar, SCROLLINFO& info) {
        if (!GetScrollInfo(rail, bar, &info) || info.nMin < 0 || info.nMax > 65535 ||
            info.nMax <= info.nMin || info.nPage > static_cast<UINT>(info.nMax - info.nMin)) return false;
        const int max_pos = info.nMax - (info.nPage > 0 ? static_cast<int>(info.nPage) - 1 : 0);
        return info.nTrackPos >= info.nMin && info.nTrackPos <= max_pos;
    }

    bool Open(HWND host, HWND rail, HWND receiver, int bar, LONG object, bool vertical, POINT point) {
        if (receiver != host && !IsChild(host, receiver)) return false;
        SCROLLBARINFO geometry{sizeof(geometry)};
        if (!GetScrollBarInfo(rail, object, &geometry) || !PtInRect(&geometry.rcScrollBar, point) ||
            (geometry.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_UNAVAILABLE)) ||
            geometry.xyThumbTop >= geometry.xyThumbBottom) return false;
        const LONG offset = vertical ? point.y - geometry.rcScrollBar.top : point.x - geometry.rcScrollBar.left;
        if (offset < geometry.xyThumbTop || offset >= geometry.xyThumbBottom) return false;
        SCROLLINFO initial{sizeof(initial), SIF_POS | SIF_TRACKPOS | SIF_RANGE | SIF_PAGE};
        if (!Read(rail, bar, initial)) return false;
        const DWORD thread = GetWindowThreadProcessId(rail, nullptr);
        if (!thread) return false;
        host_ = host;
        rail_ = rail;
        receiver_ = receiver;
        bar_ = bar;
        thread_ = thread;
        vertical_ = vertical;
        lag_.first_track = initial.nTrackPos;
        lag_.last_position = initial.nPos;
        return true;
    }

    HWND host_ = nullptr;
    HWND rail_ = nullptr;
    HWND receiver_ = nullptr;
    DWORD thread_ = 0;
    int bar_ = SB_VERT;
    bool vertical_ = true;
    bool captured_ = false;
    unsigned waiting_ = 0;
    Lag lag_;
};

} // namespace pulse::ui

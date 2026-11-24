#pragma once
#include "ipc/desktop_ipc.h"
#include <algorithm>

namespace pulse::desktop {
// Pulse owns preference gating and dwell. The desktop only publishes changes.
class HoverPreviewSession {
public:
    std::string Update(std::wstring_view path, int x, int y, int w, int h) {
        const bool drive = path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
        const bool unc = path.size() >= 5 && path[0] == L'\\' && path[1] == L'\\';
        if ((!drive && !unc) || std::any_of(path.begin(), path.end(), [](wchar_t c) { return c < 32 || c == L'"'; }))
            return Cancel();
        if (path == path_) return {};
        path_ = path;
        return "{\"mode\":\"hover_preview\",\"path\":" + ipc::JsonQuoteWide(path_) +
            ",\"rect_x\":" + std::to_string(x) + ",\"rect_y\":" + std::to_string(y) +
            ",\"rect_w\":" + std::to_string(w) + ",\"rect_h\":" + std::to_string(h) + "}";
    }
    std::string Cancel() {
        if (path_.empty()) return {};
        auto request = "{\"mode\":\"dismiss_hover_preview\",\"path\":" + ipc::JsonQuoteWide(path_) + "}";
        path_.clear();
        return request;
    }
private:
    std::wstring path_;
};
}

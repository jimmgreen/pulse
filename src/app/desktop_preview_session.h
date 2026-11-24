#pragma once
#include <cstdint>
#include <string>

namespace pulse {
// UI-thread session identity; asynchronous metadata is accepted only for its generation.
struct DesktopPreviewSession {
    std::wstring path;
    uint64_t generation = 0, due = 0, leave_due = 0;
    bool hover = false, pending = false;
    bool Request(std::wstring next, bool passive, bool enabled, uint64_t now, unsigned delay) {
        if (passive && !enabled) { Cancel(path); return false; }
        if (passive && !hover && !path.empty()) return false;
        if (passive && hover && next == path) return false;
        ++generation; leave_due = 0; path = std::move(next); hover = passive; pending = true;
        due = now + (passive ? delay : 0); return true;
    }
    bool Cancel(const std::wstring& expected) {
        if (!hover || (!expected.empty() && expected != path)) return false;
        ++generation; path.clear(); pending = false; hover = false; return true;
    }
    bool Outside(bool outside, bool visible, uint64_t now) {
        if (!outside) { leave_due = 0; return false; }
        if (!visible) return true;
        if (!leave_due) leave_due = now + 150;
        return now >= leave_due;
    }
    bool Ready(uint64_t now) const { return pending && now >= due; }
    bool Accept(uint64_t token) const { return !path.empty() && generation == token; }
    void Reset() { leave_due = 0; ++generation; path.clear(); pending = hover = false; }
};
}

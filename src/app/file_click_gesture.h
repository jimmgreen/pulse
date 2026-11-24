#pragma once
#include <cstdint>
#include <string>
#include <utility>

namespace pulse::app {
struct FileClickIdentity {
    const void* pane = nullptr;
    const void* tab = nullptr;
    const void* snapshot = nullptr;
    uint64_t generation = 0;
    int index = -1;
    std::wstring path;
    bool Valid() const { return pane && tab && index >= 0 && !path.empty(); }
    bool operator==(const FileClickIdentity&) const = default;
};

enum class FileClickAction { None, Open, Rename };

// A file click's identity survives release, never a navigation or a changed view.
class FileClickGesture {
public:
    void Cancel() { *this = {}; }
    // A new down cancels the old action, but not the recent completed click.
    // Custom intervals recognize both DOWN and DBLCLK against the completed press.
    void InterruptForPress() {
        item_ = {};
        pressed_ = single_open_ = opened_ = rename_ = releasing_capture_ = false;
        first_time_ = 0;
        rename_due_ = 0;
    }
    void CancelRename() { rename_ = false; rename_due_ = 0; }
    void Begin(FileClickIdentity item, uint32_t message_time, bool single_open, bool rename,
               uint32_t interval = 500, int x = 0, int y = 0) {
        (void)interval;
        InterruptForPress();
        if (!item.Valid()) return;
        item_ = std::move(item);
        pressed_ = true;
        first_time_ = message_time;
        single_open_ = single_open;
        rename_ = rename && !single_open;
        press_x_ = x; press_y_ = y;
    }
    FileClickAction Release(const FileClickIdentity& item, uint64_t now, uint32_t interval,
                            bool click, bool name, bool selected_only) {
        return Release(item, now, interval, click, name, selected_only, static_cast<uint32_t>(now));
    }
    FileClickAction Release(const FileClickIdentity& item, uint64_t now, uint32_t interval,
                            bool click, bool name, bool selected_only, uint32_t message_time) {
        if (!pressed_) return FileClickAction::None;
        if (!click || item != item_) { Cancel(); return FileClickAction::None; }
        pressed_ = false;
        recent_item_ = item_;
        recent_press_time_ = first_time_;
        recent_x_ = press_x_; recent_y_ = press_y_;
        if (single_open_) {
            opened_ = true;
            return FileClickAction::Open;
        }
        if (rename_ && name && selected_only) {
            const auto elapsed = static_cast<uint32_t>(message_time - first_time_);
            rename_due_ = now + (elapsed < interval ? interval - elapsed : 0);
        }
        return FileClickAction::None;
    }
    FileClickAction DoubleClick(const FileClickIdentity& item, uint32_t message_time, uint32_t interval) {
        (void)message_time; (void)interval;
        const bool open = !opened_ && item.Valid();
        Cancel();
        return open ? FileClickAction::Open : FileClickAction::None;
    }
    bool MatchesDoublePress(const FileClickIdentity& item, uint32_t time, uint32_t interval,
                            int x, int y, int slop_x, int slop_y) const {
        return recent_item_.Valid() && recent_item_.pane == item.pane && recent_item_.tab == item.tab &&
            recent_item_.path == item.path && static_cast<uint32_t>(time - recent_press_time_) <= interval &&
            x - recent_x_ >= -slop_x && x - recent_x_ <= slop_x &&
            y - recent_y_ >= -slop_y && y - recent_y_ <= slop_y;
    }
    FileClickAction Tick(const FileClickIdentity& item, uint64_t now, bool eligible, bool quiet) {
        if (!item_.Valid()) return FileClickAction::None;
        if (item != item_ || !eligible) { Cancel(); return FileClickAction::None; }
        if (!rename_due_ || now < rename_due_ || !quiet) return FileClickAction::None;
        Cancel();
        return FileClickAction::Rename;
    }
    void ExpectCaptureRelease() { releasing_capture_ = true; }
    void SuppressDoubleClick(uint32_t message_time) {
        Cancel(); opened_ = true; first_time_ = message_time;
    }
    bool SuppressesDoubleClick(uint32_t time, uint32_t interval) const {
        return opened_ && static_cast<uint32_t>(time - first_time_) <= interval;
    }
    void CaptureChanged() {
        if (releasing_capture_) releasing_capture_ = false;
        else Cancel();
    }
    bool Pressed() const { return pressed_; }
    bool SingleOpen() const { return pressed_ && single_open_; }
    bool RenamePending() const { return rename_due_ != 0; }
    uint64_t RenameDue() const { return rename_due_; }
    const FileClickIdentity& Item() const { return item_; }
private:
    FileClickIdentity item_;
    FileClickIdentity recent_item_;
    uint32_t recent_press_time_ = 0;
    int press_x_ = 0, press_y_ = 0, recent_x_ = 0, recent_y_ = 0;
    uint32_t first_time_ = 0;
    uint64_t rename_due_ = 0;
    bool pressed_ = false;
    bool single_open_ = false;
    bool rename_ = false;
    bool opened_ = false;
    bool releasing_capture_ = false;
};
}
